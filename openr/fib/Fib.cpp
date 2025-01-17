/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Fib.h"

#include <fbzmq/service/if/gen-cpp2/Monitor_types.h>
#include <fbzmq/service/logging/LogSample.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/MapUtil.h>
#include <thrift/lib/cpp/protocol/TProtocolTypes.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>

namespace openr {

Fib::Fib(
    std::string myNodeName,
    int32_t thriftPort,
    bool dryrun,
    bool enableFibSync,
    bool enableSegmentRouting,
    bool enableOrderedFib,
    std::chrono::seconds coldStartDuration,
    const DecisionPubUrl& decisionPubUrl,
    const folly::Optional<std::string>& fibRepUrl,
    const LinkMonitorGlobalPubUrl& linkMonPubUrl,
    const MonitorSubmitUrl& monitorSubmitUrl,
    const KvStoreLocalCmdUrl& storeCmdUrl,
    const KvStoreLocalPubUrl& storePubUrl,
    fbzmq::Context& zmqContext)
    : OpenrEventLoop(
          myNodeName, thrift::OpenrModuleType::FIB, zmqContext, fibRepUrl),
      myNodeName_(std::move(myNodeName)),
      thriftPort_(thriftPort),
      dryrun_(dryrun),
      enableFibSync_(enableFibSync),
      enableSegmentRouting_(enableSegmentRouting),
      enableOrderedFib_(enableOrderedFib),
      coldStartDuration_(coldStartDuration),
      decisionSub_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      linkMonSub_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      decisionPubUrl_(std::move(decisionPubUrl)),
      linkMonPubUrl_(std::move(linkMonPubUrl)),
      expBackoff_(
          std::chrono::milliseconds(8), std::chrono::milliseconds(4096)) {
  routeDb_.thisNodeName = myNodeName_;

  syncRoutesTimer_ = fbzmq::ZmqTimeout::make(this, [this]() noexcept {
    auto success = syncRouteDb();
    if (success) {
      expBackoff_.reportSuccess();
    } else {
      // Apply exponential backoff and schedule next run
      expBackoff_.reportError();
      syncRoutesTimer_->scheduleTimeout(
          expBackoff_.getTimeRemainingUntilRetry());
    }
  });

  if (enableOrderedFib_) {
    kvStoreClient_ = std::make_unique<KvStoreClient>(
        zmqContext, this, myNodeName_, storeCmdUrl, storePubUrl);
  }

  syncRoutesTimer_->scheduleTimeout(coldStartDuration_);

  healthChecker_ = fbzmq::ZmqTimeout::make(this, [this]() noexcept {
    // Make thrift calls to do real programming
    try {
      keepAliveCheck();
    } catch (const std::exception& e) {
      tData_.addStatValue("fib.thrift.failure.keepalive", 1, fbzmq::COUNT);
      client_.reset();
      LOG(ERROR) << "Failed to make thrift call to Switch Agent. Error: "
                 << folly::exceptionStr(e);
    }
  });

  // Only schedule health checker in non dry run mode
  if (not dryrun_) {
    healthChecker_->scheduleTimeout(
        Constants::kHealthCheckInterval, true /* schedule periodically */);
  }

  syncFibTimer_ = fbzmq::ZmqTimeout::make(this, [this]() noexcept {
    if (!syncRoutesTimer_->isScheduled()) {
      // Trigger immediate run
      syncRouteDb();
    }
  });

  // Only schedule sync Fib in non dry run and enable sync mode
  if (not dryrun_ and enableFibSync_) {
    syncFibTimer_->scheduleTimeout(
        Constants::kPlatformSyncInterval, true /* schedule periodically */);
  }

  prepare();

  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);
}

void
Fib::prepare() noexcept {
  VLOG(2) << "Fib: Subscribing to decision module '" << decisionPubUrl_ << "'";
  const auto decisionSubConnect =
      decisionSub_.connect(fbzmq::SocketUrl{decisionPubUrl_});
  if (decisionSubConnect.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << decisionPubUrl_ << "' "
               << decisionSubConnect.error();
  }
  const auto decisionSubOpt = decisionSub_.setSockOpt(ZMQ_SUBSCRIBE, "", 0);
  if (decisionSubOpt.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to "
               << ""
               << " " << decisionSubOpt.error();
  }

  VLOG(2) << "Fib: Subscribing to Link Monitor module pub url '"
          << linkMonPubUrl_ << "'";
  const auto lmSubConnect =
      linkMonSub_.connect(fbzmq::SocketUrl{linkMonPubUrl_});
  if (lmSubConnect.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << linkMonPubUrl_ << "' "
               << lmSubConnect.error();
  }
  const auto linkSubOpt = linkMonSub_.setSockOpt(ZMQ_SUBSCRIBE, "", 0);
  if (linkSubOpt.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to "
               << ""
               << " " << linkSubOpt.error();
  }

  LOG(INFO) << "Fib thread attaching socket/timeout callbacks...";

  // Schedule periodic timer for submission to monitor
  const bool isPeriodic = true;
  monitorTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { submitCounters(); });
  monitorTimer_->scheduleTimeout(Constants::kMonitorSubmitInterval, isPeriodic);

  // Received publication from Decision module
  addSocket(
      fbzmq::RawZmqSocketPtr{*decisionSub_}, ZMQ_POLLIN, [this](int) noexcept {
        VLOG(1) << "Fib: publication received ...";
        auto maybeThriftObj = decisionSub_.recvThriftObj<thrift::RouteDatabase>(
            serializer_, Constants::kReadTimeout);
        if (maybeThriftObj.hasError()) {
          LOG(ERROR) << "Error processing decision publication: "
                     << maybeThriftObj.error();
          return;
        }
        auto& thriftRouteDb = maybeThriftObj.value();

        if (thriftRouteDb.thisNodeName != myNodeName_) {
          LOG(ERROR) << "Received publication from unknown node "
                     << thriftRouteDb.thisNodeName;
        } else {
          processRouteDb(std::move(thriftRouteDb));
        }
      });

  // We have received Interface status publication from LinkMonitor
  addSocket(
      fbzmq::RawZmqSocketPtr{*linkMonSub_}, ZMQ_POLLIN, [this](int) noexcept {
        VLOG(1) << "Fib: interface status publication received ...";
        auto maybeThriftObj =
            linkMonSub_.recvThriftObj<thrift::InterfaceDatabase>(
                serializer_, Constants::kReadTimeout);
        if (maybeThriftObj.hasError()) {
          LOG(ERROR) << "Error processing link monitor publication"
                     << maybeThriftObj.error();
          return;
        }

        auto& thriftInterfaceDb = maybeThriftObj.value();
        if (thriftInterfaceDb.thisNodeName != myNodeName_) {
          LOG(ERROR) << "Received interface updates from unknown node "
                     << thriftInterfaceDb.thisNodeName;
        } else {
          processInterfaceDb(std::move(thriftInterfaceDb));
        }
      });
}

// Received FibRequest
folly::Expected<fbzmq::Message, fbzmq::Error>
Fib::processRequestMsg(fbzmq::Message&& request) {
  auto maybeThriftObj = request.readThriftObj<thrift::FibRequest>(serializer_);
  if (maybeThriftObj.hasError()) {
    LOG(ERROR) << "Error processing Fib Request: " << maybeThriftObj.error();
    return folly::makeUnexpected(fbzmq::Error());
  }

  auto& thriftReq = maybeThriftObj.value();
  VLOG(1) << "Fib: Request command: `"
          << apache::thrift::TEnumTraits<thrift::FibCommand>::findName(
                 thriftReq.cmd)
          << "` received";
  switch (thriftReq.cmd) {
  case thrift::FibCommand::ROUTE_DB_GET:
    VLOG(2) << "Fib: RouteDb requested";
    // send the thrift::RouteDatabase
    return fbzmq::Message::fromThriftObj(routeDb_, serializer_);
    break;
  case thrift::FibCommand::PERF_DB_GET:
    VLOG(2) << "Fib: PerfDb requested";
    // send the thrift::PerfDatabase
    return fbzmq::Message::fromThriftObj(dumpPerfDb(), serializer_);
    break;
  case thrift::FibCommand::ROUTE_DB_UNINSTALLABLE_GET:
    VLOG(2) << "Fib: Do not install RouteDb requested";
    // send the thrift::RouteDatabase
    return fbzmq::Message::fromThriftObj(doNotInstallRouteDb_, serializer_);
    break;
  default:
    LOG(ERROR) << "Unknown command received";
    return folly::makeUnexpected(fbzmq::Error());
  }
}

void
Fib::processRouteDb(thrift::RouteDatabase&& newRouteDb) {
  thrift::RouteDatabase doNotInstallRouteDb;

  VLOG(2) << "Processing new routes from Decision. "
          << newRouteDb.unicastRoutes.size() << " unicast routes and "
          << newRouteDb.mplsRoutes.size() << " mpls routes";

  // Update perfEvents_ .. We replace existing perf events with new one as
  // convergence is going to be based on new data, not the old.
  if (newRouteDb.perfEvents) {
    maybePerfEvents_ = newRouteDb.perfEvents;
    addPerfEvent(*maybePerfEvents_, myNodeName_, "FIB_ROUTE_DB_RECVD");
  }

  // remove routes that should not be programmed
  auto rIter = newRouteDb.unicastRoutes.begin();
  while (rIter != newRouteDb.unicastRoutes.end()) {
    if (rIter->doNotInstall) {
      doNotInstallRouteDb.unicastRoutes.emplace_back(
          *std::make_move_iterator(rIter));
      rIter = newRouteDb.unicastRoutes.erase(rIter);
    } else {
      ++rIter;
    }
  }

  // Find out delta to be programmed
  auto const routeDelta = findDeltaRoutes(newRouteDb, routeDb_);
  // update new routeDb_
  routeDb_ = std::move(newRouteDb);
  doNotInstallRouteDb_ = std::move(doNotInstallRouteDb);
  // Add some counters
  tData_.addStatValue("fib.process_route_db", 1, fbzmq::COUNT);
  // Send request to agent
  updateRoutes(routeDelta);
}

void
Fib::processInterfaceDb(thrift::InterfaceDatabase&& interfaceDb) {
  tData_.addStatValue("fib.process_interface_db", 1, fbzmq::COUNT);

  if (interfaceDb.perfEvents) {
    maybePerfEvents_.assign(std::move(interfaceDb.perfEvents));
    addPerfEvent(*maybePerfEvents_, myNodeName_, "FIB_INTF_DB_RECEIVED");
  }

  // Find interfaces which were up before and we detected them down
  std::unordered_set<std::string> affectedInterfaces;
  for (auto const& kv : interfaceDb.interfaces) {
    const auto& ifName = kv.first;
    const auto isUp = kv.second.isUp;

    const auto wasUp = folly::get_default(interfaceStatusDb_, ifName, false);
    interfaceStatusDb_[ifName] = isUp; // Add new status to the map

    if (wasUp and not isUp) {
      affectedInterfaces.insert(ifName);
      LOG(INFO) << "Interface " << ifName << " went DOWN from UP state.";
    }
  }

  thrift::RouteDatabaseDelta routeDbDelta;

  for (auto it = routeDb_.unicastRoutes.begin();
       it != routeDb_.unicastRoutes.end();) {
    // Find valid paths
    std::vector<thrift::NextHopThrift> validNextHops;
    for (auto const& nextHop : it->nextHops) {
      CHECK(nextHop.address.ifName.hasValue());
      if (affectedInterfaces.count(nextHop.address.ifName.value()) == 0) {
        validNextHops.emplace_back(nextHop);
      }
    } // end for ... kv.second

    // Add to affected routes only if best path has changed and also reflect
    // changes in routeDb_
    auto prevBestNextHops = getBestNextHopsUnicast(it->nextHops);

    // Find best paths
    auto validBestNextHops = getBestNextHopsUnicast(validNextHops);

    // Update valid nexthops
    it->nextHops = std::move(validNextHops);

    if (validBestNextHops.size() && validBestNextHops != prevBestNextHops) {
      VLOG(1) << "bestPaths group resize for prefix: " << toString(it->dest)
              << ", old: " << prevBestNextHops.size()
              << ", new: " << validBestNextHops.size();
      thrift::UnicastRoute route;
      route.dest = it->dest;
      route.nextHops = std::move(validBestNextHops);
      routeDbDelta.unicastRoutesToUpdate.emplace_back(std::move(route));
    }

    // Remove route if no valid paths
    if (it->nextHops.size() == 0) {
      VLOG(1) << "Removing prefix " << toString(it->dest)
              << " because of no valid nextHops.";
      routeDbDelta.unicastRoutesToDelete.emplace_back(it->dest);
      it = routeDb_.unicastRoutes.erase(it);
    } else {
      ++it;
    }
  } // end for ... routeDb_.unicastRoutes

  for (auto it = routeDb_.mplsRoutes.begin();
       it != routeDb_.mplsRoutes.end();) {
    // Find valid paths
    std::vector<thrift::NextHopThrift> validNextHops;
    for (auto const& nextHop : it->nextHops) {
      // We don't have ifName for `POP_AND_LOOKUP` mpls action
      if (not nextHop.address.ifName.hasValue() or
          affectedInterfaces.count(nextHop.address.ifName.value()) == 0) {
        validNextHops.emplace_back(nextHop);
      }
    } // end for ... it->nextHops

    // Add to affected routes only if best path has changed and also reflect
    // changes in routeDb_
    auto prevBestNextHops = getBestNextHopsMpls(it->nextHops);

    // Find best paths
    auto validBestNextHops = getBestNextHopsMpls(validNextHops);

    // Update valid nexthops
    it->nextHops = std::move(validNextHops);

    if (validBestNextHops.size() && validBestNextHops != prevBestNextHops) {
      VLOG(1) << "bestPaths group resize for label: "
              << std::to_string(it->topLabel)
              << ", old: " << prevBestNextHops.size()
              << ", new: " << validBestNextHops.size();
      thrift::MplsRoute route;
      route.topLabel = it->topLabel;
      route.nextHops = std::move(validBestNextHops);
      routeDbDelta.mplsRoutesToUpdate.emplace_back(std::move(route));
    }

    // Remove route if no valid paths
    if (it->nextHops.size() == 0) {
      VLOG(1) << "Removing prefix " << std::to_string(it->topLabel)
              << " because of no valid nextHops.";
      routeDbDelta.mplsRoutesToDelete.emplace_back(it->topLabel);
      it = routeDb_.mplsRoutes.erase(it);
    } else {
      ++it;
    }
  } // end for ... routeDb_.mplsRoutes

  updateRoutes(routeDbDelta);
}

thrift::PerfDatabase
Fib::dumpPerfDb() const {
  thrift::PerfDatabase perfDb;
  perfDb.thisNodeName = myNodeName_;
  for (auto const& perf : perfDb_) {
    perfDb.eventInfo.emplace_back(perf);
  }
  return perfDb;
}

void
Fib::updateRoutes(const thrift::RouteDatabaseDelta& routeDbDelta) {
  LOG(INFO) << "Processing route add/update for "
            << routeDbDelta.unicastRoutesToUpdate.size() << " unicast, "
            << routeDbDelta.mplsRoutesToUpdate.size() << " mpls, "
            << "and route delete for "
            << routeDbDelta.unicastRoutesToDelete.size() << "-unicast, "
            << routeDbDelta.mplsRoutesToDelete.size() << "-mpls, ";

  // Only for backward compatibility
  auto const& patchedUnicastRoutesToUpdate =
      createUnicastRoutesWithBestNexthops(routeDbDelta.unicastRoutesToUpdate);

  auto const& mplsRoutesToUpdate =
      createMplsRoutesWithBestNextHops(routeDbDelta.mplsRoutesToUpdate);

  // Do not program routes in case of dryrun
  LOG(INFO) << "Skipping programing of routes in dryrun ... ";
  VLOG(2) << "Unicast routes to add/update";
  for (auto const& route : patchedUnicastRoutesToUpdate) {
    VLOG(2) << "> " << toString(route.dest) << ", " << route.nextHops.size();
    for (auto const& nh : route.nextHops) {
      VLOG(2) << "  " << toString(nh);
    }
  }

  VLOG(2) << "";
  VLOG(2) << "Unicast routes to delete";
  for (auto const& prefix : routeDbDelta.unicastRoutesToDelete) {
    VLOG(2) << "> " << toString(prefix);
  }

  VLOG(2) << "";
  VLOG(2) << "Mpls routes to add/update";
  for (auto const& route : mplsRoutesToUpdate) {
    VLOG(2) << "> " << std::to_string(route.topLabel) << ", "
            << route.nextHops.size();
    for (auto const& nh : route.nextHops) {
      VLOG(2) << "  " << toString(nh);
    }
  }

  VLOG(2) << "";
  VLOG(2) << "MPLS routes to delete";
  for (auto const& topLabel : routeDbDelta.mplsRoutesToDelete) {
    VLOG(2) << "> " << std::to_string(topLabel);
  }

  if (dryrun_) {
    logPerfEvents();
    return;
  }

  if (syncRoutesTimer_->isScheduled()) {
    // Check if there's any full sync scheduled,
    // if so, skip partial sync
    LOG(INFO) << "Pending full sync is scheduled, skip delta sync for now...";
    return;
  } else if (dirtyRouteDb_) {
    // If previous route programming attempt failed, enforce full sync
    LOG(INFO) << "Previous route programming failed, skip delta sync to enforce"
              << " full fib sync...";
    syncRouteDbDebounced();
    return;
  }

  // Make thrift calls to do real programming
  try {
    if (maybePerfEvents_) {
      addPerfEvent(*maybePerfEvents_, myNodeName_, "FIB_DEBOUNCE");
    }
    createFibClient(evb_, socket_, client_, thriftPort_);
    if (routeDbDelta.unicastRoutesToDelete.size()) {
      client_->sync_deleteUnicastRoutes(
          kFibId_, routeDbDelta.unicastRoutesToDelete);
    }
    if (patchedUnicastRoutesToUpdate.size()) {
      client_->sync_addUnicastRoutes(kFibId_, patchedUnicastRoutesToUpdate);
    }
    if (enableSegmentRouting_ && routeDbDelta.mplsRoutesToDelete.size()) {
      client_->sync_deleteMplsRoutes(kFibId_, routeDbDelta.mplsRoutesToDelete);
    }
    if (enableSegmentRouting_ && mplsRoutesToUpdate.size()) {
      client_->sync_addMplsRoutes(kFibId_, mplsRoutesToUpdate);
    }
    dirtyRouteDb_ = false;
    logPerfEvents();
    LOG(INFO) << "Done processing route add/update";
  } catch (const std::exception& e) {
    tData_.addStatValue("fib.thrift.failure.add_del_route", 1, fbzmq::COUNT);
    client_.reset();
    dirtyRouteDb_ = true;
    syncRouteDbDebounced(); // Schedule future full sync of route DB
    LOG(ERROR) << "Failed to make thrift call to FibAgent. Error: "
               << folly::exceptionStr(e);
  }
}

bool
Fib::syncRouteDb() {
  LOG(INFO) << "Syncing latest routeDb with fib-agent with "
            << routeDb_.unicastRoutes.size() << " routes";

  const auto& unicastRoutes =
      createUnicastRoutesWithBestNexthops(routeDb_.unicastRoutes);
  const auto& mplsRoutes =
      createMplsRoutesWithBestNextHops(routeDb_.mplsRoutes);

  // In dry run we just print the routes. No real action
  if (dryrun_) {
    LOG(INFO) << "Skipping programing of routes in dryrun ... ";
    VLOG(2) << "Unicast routes to add/update";
    for (auto const& route : unicastRoutes) {
      VLOG(2) << "> " << toString(route.dest) << ", " << route.nextHops.size();
      for (auto const& nh : route.nextHops) {
        VLOG(2) << "  " << toString(nh);
      }
    }

    VLOG(2) << "";
    VLOG(2) << "Mpls routes to add/update";
    for (auto const& route : mplsRoutes) {
      VLOG(2) << "> " << std::to_string(route.topLabel) << ", "
              << route.nextHops.size();
      for (auto const& nh : route.nextHops) {
        VLOG(2) << "  " << toString(nh);
      }
    }

    logPerfEvents();
    return true;
  }

  try {
    if (maybePerfEvents_) {
      addPerfEvent(*maybePerfEvents_, myNodeName_, "FIB_DEBOUNCE");
    }
    createFibClient(evb_, socket_, client_, thriftPort_);
    tData_.addStatValue("fib.sync_fib_calls", 1, fbzmq::COUNT);

    // Sync unicast routes
    client_->sync_syncFib(kFibId_, unicastRoutes);

    // Sync mpls routes
    if (enableSegmentRouting_) {
      client_->sync_syncMplsFib(kFibId_, mplsRoutes);
    }

    dirtyRouteDb_ = false;
    logPerfEvents();
    LOG(INFO) << "Done syncing latest routeDb with fib-agent";
    return true;
  } catch (std::exception const& e) {
    tData_.addStatValue("fib.thrift.failure.sync_fib", 1, fbzmq::COUNT);
    LOG(ERROR) << "Failed to sync routeDb with switch FIB agent. Error: "
               << folly::exceptionStr(e);
    dirtyRouteDb_ = true;
    client_.reset();
    return false;
  }
}

void
Fib::syncRouteDbDebounced() {
  if (!syncRoutesTimer_->isScheduled()) {
    // Schedule an immediate run if previous one is not scheduled
    syncRoutesTimer_->scheduleTimeout(std::chrono::milliseconds(0));
  }
}

void
Fib::keepAliveCheck() {
  createFibClient(evb_, socket_, client_, thriftPort_);
  int64_t aliveSince = client_->sync_aliveSince();
  // Check if FIB has restarted or not
  if (aliveSince != latestAliveSince_) {
    LOG(WARNING) << "FibAgent seems to have restarted. "
                 << "Performing full route DB sync ...";
    // set dirty flag
    dirtyRouteDb_ = true;
    expBackoff_.reportSuccess();
    syncRouteDbDebounced();
  }
  latestAliveSince_ = aliveSince;
}

void
Fib::createFibClient(
    folly::EventBase& evb,
    std::shared_ptr<apache::thrift::async::TAsyncSocket>& socket,
    std::unique_ptr<thrift::FibServiceAsyncClient>& client,
    int32_t port) {
  // Reset client if channel is not good
  if (socket && (!socket->good() || socket->hangup())) {
    client.reset();
    socket.reset();
  }

  // Do not create new client if one exists already
  if (client) {
    return;
  }

  // Create socket to thrift server and set some connection parameters
  socket = apache::thrift::async::TAsyncSocket::newSocket(
      &evb,
      Constants::kPlatformHost.toString(),
      port,
      Constants::kPlatformConnTimeout.count());

  // Create channel and set timeout
  auto channel = apache::thrift::HeaderClientChannel::newChannel(socket);
  channel->setTimeout(Constants::kPlatformProcTimeout.count());

  // Set BinaryProtocol and Framed client type for talkiing with thrift1 server
  channel->setProtocolId(apache::thrift::protocol::T_BINARY_PROTOCOL);
  channel->setClientType(THRIFT_FRAMED_DEPRECATED);

  // Reset client_
  client = std::make_unique<thrift::FibServiceAsyncClient>(std::move(channel));
}

void
Fib::submitCounters() {
  VLOG(3) << "Submitting counters ... ";

  // Extract/build counters from thread-data
  auto counters = tData_.getCounters();

  // Add some more flat counters
  counters["fib.num_routes"] = routeDb_.unicastRoutes.size();
  counters["fib.require_routedb_sync"] = syncRoutesTimer_->isScheduled();
  counters["fib.zmq_event_queue_size"] = getEventQueueSize();

  zmqMonitorClient_->setCounters(prepareSubmitCounters(std::move(counters)));
}

void
Fib::logPerfEvents() {
  if (!maybePerfEvents_ or !maybePerfEvents_->events.size()) {
    return;
  }

  // Ignore bad perf event sample if creation time of first event is
  // less than creation time of our recently logged perf events.
  if (recentPerfEventCreateTs_ >= maybePerfEvents_->events[0].unixTs) {
    LOG(WARNING) << "Ignoring perf event with old create timestamp "
                 << maybePerfEvents_->events[0].unixTs << ", expected > "
                 << recentPerfEventCreateTs_;
    return;
  } else {
    recentPerfEventCreateTs_ = maybePerfEvents_->events[0].unixTs;
  }

  // Add latest event information (this function is meant to be called after
  // routeDb has synced)
  addPerfEvent(*maybePerfEvents_, myNodeName_, "OPENR_FIB_ROUTES_PROGRAMMED");

  if (enableOrderedFib_) {
    // Export convergence duration counter
    // this is the local time it takes to program a route after an event
    // we are using this for ordered fib programing
    auto localDuration = getDurationBetweenPerfEvents(
        *maybePerfEvents_, "DECISION_RECEIVED", "OPENR_FIB_ROUTES_PROGRAMMED");
    if (localDuration.hasError()) {
      LOG(WARNING) << "Ignoring perf event with bad local duration "
                   << localDuration.error();
    } else if (*localDuration <= Constants::kConvergenceMaxDuration) {
      tData_.addStatValue(
          "fib.local_route_program_time_ms",
          localDuration->count(),
          fbzmq::AVG);
      kvStoreClient_->persistKey(
          Constants::kFibTimeMarker.toString() + myNodeName_,
          std::to_string(tData_.getCounters().at(
              "fib.local_route_program_time_ms.avg.60")));
    }
  }

  // Ignore perf events with very off total duration
  auto totalDuration = getTotalPerfEventsDuration(*maybePerfEvents_);
  if (totalDuration.count() < 0 or
      totalDuration > Constants::kConvergenceMaxDuration) {
    LOG(WARNING) << "Ignoring perf event with bad total duration "
                 << totalDuration.count() << "ms.";
    return;
  }

  // Add new entry to perf DB and purge extra entries
  perfDb_.push_back(*maybePerfEvents_);
  while (perfDb_.size() >= Constants::kPerfBufferSize) {
    perfDb_.pop_front();
  }

  // Log event
  auto eventStrs = sprintPerfEvents(*maybePerfEvents_);
  maybePerfEvents_ = folly::none;
  LOG(INFO) << "OpenR convergence performance. "
            << "Duration=" << totalDuration.count();
  for (auto& str : eventStrs) {
    VLOG(2) << "  " << str;
  }

  // Export convergence duration counter
  tData_.addStatValue(
      "fib.convergence_time_ms", totalDuration.count(), fbzmq::AVG);

  // Log via zmq monitor
  fbzmq::LogSample sample{};
  sample.addString("event", "ROUTE_CONVERGENCE");
  sample.addString("entity", "Fib");
  sample.addString("node_name", myNodeName_);
  sample.addStringVector("perf_events", eventStrs);
  sample.addInt("duration_ms", totalDuration.count());
  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));
}

} // namespace openr
