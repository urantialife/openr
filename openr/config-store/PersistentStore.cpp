/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PersistentStore.h"

#include <chrono>

#include <folly/FileUtil.h>
#include <folly/io/IOBuf.h>

#include <openr/common/Util.h>

namespace openr {

using namespace std::chrono_literals;

PersistentStore::PersistentStore(
    const std::string& nodeName,
    const std::string& storageFilePath,
    const PersistentStoreUrl& socketUrl,
    fbzmq::Context& context,
    std::chrono::milliseconds saveInitialBackoff,
    std::chrono::milliseconds saveMaxBackoff,
    bool dryrun)
    : OpenrEventLoop(
          nodeName,
          thrift::OpenrModuleType::PERSISTENT_STORE,
          context,
          folly::none,
          std::string(socketUrl)),
      storageFilePath_(storageFilePath),
      dryrun_(dryrun) {
  if (saveInitialBackoff != 0ms or saveMaxBackoff != 0ms) {
    // Create timer and backoff mechanism only if backoff is requested
    saveDbTimerBackoff_ =
        std::make_unique<ExponentialBackoff<std::chrono::milliseconds>>(
            saveInitialBackoff, saveMaxBackoff);

    saveDbTimer_ = fbzmq::ZmqTimeout::make(this, [this]() noexcept {
      if (saveDatabaseToDisk()) {
        saveDbTimerBackoff_->reportSuccess();
      } else {
        // Report error and schedule next-try
        saveDbTimerBackoff_->reportError();
        saveDbTimer_->scheduleTimeout(
            saveDbTimerBackoff_->getTimeRemainingUntilRetry());
      }
    });
  }

  // Load initial database. On failure we will just report error and continue
  // with empty database
  if (not loadDatabaseFromDisk()) {
    LOG(ERROR) << "Failed to load config-database from file: "
               << storageFilePath_;
  }
}

PersistentStore::~PersistentStore() {
  saveDatabaseToDisk();
}

folly::Expected<fbzmq::Message, fbzmq::Error>
PersistentStore::processRequestMsg(fbzmq::Message&& requestMsg) {
  thrift::StoreResponse response;
  auto request = requestMsg.readThriftObj<thrift::StoreRequest>(serializer_);
  if (request.hasError()) {
    LOG(ERROR) << "Error while reading request " << request.error();
    response.success = false;
    return fbzmq::Message::fromThriftObj(response, serializer_);
  }

  // Generate response
  response.key = request->key;
  switch (request->requestType) {
  case thrift::StoreRequestType::STORE: {
    // Override previous value if any
    database_.keyVals[request->key] = request->data;
    response.success = true;
    break;
  }
  case thrift::StoreRequestType::LOAD: {
    auto it = database_.keyVals.find(request->key);
    const bool success = it != database_.keyVals.end();
    response.success = success;
    response.data = success ? it->second : "";
    break;
  }
  case thrift::StoreRequestType::ERASE: {
    response.success = database_.keyVals.erase(request->key) > 0;
    break;
  }
  default: {
    LOG(ERROR) << "Got unknown request.";
    response.success = false;
    break;
  }
  }

  // Schedule database save
  if (response.success and
      (request->requestType != thrift::StoreRequestType::LOAD)) {
    if (not saveDbTimerBackoff_) {
      // This is primarily used for unit testing to save DB immediately
      // Block the response till file is saved
      saveDatabaseToDisk();
    } else if (not saveDbTimer_->isScheduled()) {
      saveDbTimer_->scheduleTimeout(
          saveDbTimerBackoff_->getTimeRemainingUntilRetry());
    }
  }

  // Send response
  return fbzmq::Message::fromThriftObj(response, serializer_);
}

bool
PersistentStore::saveDatabaseToDisk() noexcept {
  // Write database_ to ioBuf
  auto queue = folly::IOBufQueue();
  serializer_.serialize(database_, &queue);
  auto ioBuf = queue.move();
  ioBuf->coalesce();

  try {
    if (not dryrun_) {
      LOG(INFO) << "Updating database on disk";
      const auto startTs = std::chrono::steady_clock::now();
      // Write ioBuf to disk atomically
      auto fileData = ioBuf->moveToFbString().toStdString();
      folly::writeFileAtomic(storageFilePath_, fileData, 0666);
      LOG(INFO) << "Updated database on disk. Took "
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - startTs)
                       .count()
                << "ms";
    } else {
      VLOG(1) << "Skipping writing to disk in dryrun mode";
    }
    numOfWritesToDisk_++;
  } catch (std::exception const& err) {
    LOG(ERROR) << "Failed to write data to file '" << storageFilePath_ << "'. "
               << folly::exceptionStr(err);
    return false;
  }

  return true;
}

bool
PersistentStore::loadDatabaseFromDisk() noexcept {
  // Check if file exists
  if (not fileExists(storageFilePath_)) {
    LOG(INFO) << "Storage file " << storageFilePath_ << " doesn't exists. "
              << "Starting with empty database";
    return true;
  }

  // Read data from file
  std::string fileData{""};
  if (not folly::readFile(storageFilePath_.c_str(), fileData)) {
    LOG(ERROR) << "Failed to read file contents from '" << storageFilePath_
               << "'. Error (" << errno << "): " << folly::errnoStr(errno);
    return false;
  }

  // Parse data into `database_`
  try {
    auto ioBuf = folly::IOBuf::wrapBuffer(fileData.c_str(), fileData.size());
    thrift::StoreDatabase newDatabase;
    serializer_.deserialize(ioBuf.get(), newDatabase);
    database_ = std::move(newDatabase);
    return true;
  } catch (std::exception const& e) {
    LOG(ERROR) << "Failed to decode file content into StoreDatabase."
               << ". Error: " << folly::exceptionStr(e);
    return false;
  }
}

} // namespace openr
