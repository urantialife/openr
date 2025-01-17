/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/nl/NetlinkRoute.h"
#include "openr/nl/NetlinkMessage.h"

namespace openr {
namespace Netlink {

NetlinkRouteMessage::NetlinkRouteMessage() {
  // get pointer to NLMSG header
  msghdr_ = getMessagePtr();
}

void
NetlinkRouteMessage::init(
    int type, uint32_t rtFlags, const openr::fbnl::Route& route) {
  // initialize netlink header
  msghdr_->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
  msghdr_->nlmsg_type = type;
  msghdr_->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

  if (type != RTM_DELROUTE) {
    msghdr_->nlmsg_flags |= NLM_F_CREATE;
  }

  if (route.getType() != RTN_MULTICAST) {
    msghdr_->nlmsg_flags |= NLM_F_REPLACE;
  }

  // intialize the route meesage header
  auto nlmsgAlen = NLMSG_ALIGN(sizeof(struct nlmsghdr));
  rtmsg_ = (struct rtmsg*)((char*)msghdr_ + nlmsgAlen);

  rtmsg_->rtm_table = RT_TABLE_MAIN;
  rtmsg_->rtm_protocol = route.getProtocolId();
  rtmsg_->rtm_scope = RT_SCOPE_UNIVERSE;
  rtmsg_->rtm_type = route.getType();
  rtmsg_->rtm_src_len = 0;
  rtmsg_->rtm_tos = 0;
  rtmsg_->rtm_flags = rtFlags;

  auto rtFlag = route.getFlags();
  if (rtFlag.hasValue()) {
    rtmsg_->rtm_flags |= rtFlag.value();
  }
}

void
NetlinkRouteMessage::showRtmMsg(const struct rtmsg* const hdr) const {
  LOG(INFO) << "Route message data"
            << "\nrtm_family:   " << +hdr->rtm_family
            << "\nrtm_dst_len:  " << +hdr->rtm_dst_len
            << "\nrtm_src_len:  " << +hdr->rtm_src_len
            << "\nrtm_tos:      " << +hdr->rtm_tos
            << "\nrtm_table:    " << +hdr->rtm_table
            << "\nrtm_protocol: " << +hdr->rtm_protocol
            << "\nrtm_scope:    " << +hdr->rtm_scope
            << "\nrtm_type:     " << +hdr->rtm_type
            << "\nrtm_flags:    " << std::hex << hdr->rtm_flags;
}

void
NetlinkRouteMessage::showRouteAttribute(const struct rtattr* const hdr) const {
  LOG(INFO) << "Route attributes"
            << "\nrta_len       " << hdr->rta_len << "\nrta_type      "
            << hdr->rta_type;
}

uint32_t
NetlinkRouteMessage::encodeLabel(uint32_t label, bool bos) const {
  if (label > 0xFFFFF) {
    LOG(ERROR) << "Invalid label 0x" << std::hex << label;
    label = 0;
  }
  uint32_t encodeLabel = htonl(label << kLabelShift);
  if (bos) {
    encodeLabel |= htonl(1 << kLabelBosShift);
  }
  return encodeLabel;
}

ResultCode
NetlinkRouteMessage::addIpNexthop(
    struct rtattr* rta,
    struct rtnexthop* rtnh,
    const openr::fbnl::NextHop& path,
    const openr::fbnl::Route& route) const {
  rtnh->rtnh_len = sizeof(*rtnh);
  rtnh->rtnh_ifindex = path.getIfIndex().value();
  rtnh->rtnh_flags = 0;
  rtnh->rtnh_hops = 0;

  // RTA_GATEWAY
  auto const via = path.getGateway();
  if (!via.hasValue()) {
    if (route.getType() == RTN_MULTICAST || route.getScope() == RT_SCOPE_LINK) {
      return ResultCode::SUCCESS;
    }
    LOG(ERROR) << "Nexthop IP not provided";
    return ResultCode::NO_NEXTHOP_IP;
  }
  if (addSubAttributes(
          rta, RTA_GATEWAY, via.value().bytes(), via.value().byteCount()) ==
      nullptr) {
    return ResultCode::NO_MESSAGE_BUFFER;
  };

  // update length in rtnexthop
  rtnh->rtnh_len += via.value().byteCount() + sizeof(struct rtattr);
  return ResultCode::SUCCESS;
}

ResultCode
NetlinkRouteMessage::addSwapOrPHPNexthop(
    struct rtattr* rta,
    struct rtnexthop* rtnh,
    const openr::fbnl::NextHop& path) const {
  rtnh->rtnh_len = sizeof(*rtnh);
  rtnh->rtnh_ifindex = path.getIfIndex().value();
  rtnh->rtnh_flags = 0;
  rtnh->rtnh_hops = 0;

  // add the following subattributes within RTA_MULTIPATH
  size_t prevLen = rta->rta_len;

  // labels.size() = 0 implies PHP
  auto maybeLabel = path.getSwapLabel();
  if (maybeLabel.hasValue()) {
    struct mpls_label swapLabel;
    swapLabel.entry = encodeLabel(maybeLabel.value(), true);

    if (addSubAttributes(
            rta,
            RTA_NEWDST,
            reinterpret_cast<const char*>(&swapLabel),
            sizeof(swapLabel)) == nullptr) {
      return ResultCode::NO_MESSAGE_BUFFER;
    }
  }
  // update rtnh len
  rtnh->rtnh_len += rta->rta_len - prevLen;

  // RTA_VIA
  struct nextHop via;
  via.addrFamily = path.getFamily();
  auto gw = path.getGateway().value();
  int viaLen{sizeof(nextHop)};
  if (via.addrFamily == AF_INET) {
    viaLen = sizeof(nextHopV4);
  }
  memcpy(via.ip, reinterpret_cast<const char*>(gw.bytes()), gw.byteCount());
  if (addSubAttributes(
          rta, RTA_VIA, reinterpret_cast<const char*>(&via), viaLen) ==
      nullptr) {
    return ResultCode::NO_MESSAGE_BUFFER;
  }

  // update length in rtnexthop
  rtnh->rtnh_len += viaLen + sizeof(struct rtattr);
  return ResultCode::SUCCESS;
}

ResultCode
NetlinkRouteMessage::addPopNexthop(
    struct rtattr* rta,
    struct rtnexthop* rtnh,
    const openr::fbnl::NextHop& path) const {
  // for each next hop add the ENCAP
  rtnh->rtnh_len = sizeof(*rtnh);
  if (!path.getIfIndex().hasValue()) {
    LOG(ERROR) << "Loopback interface index not provided for POP";
    return ResultCode::NO_LOOPBACK_INDEX;
  }
  rtnh->rtnh_ifindex = path.getIfIndex().value();
  rtnh->rtnh_flags = 0;
  rtnh->rtnh_hops = 0;

  int oif = rtnh->rtnh_ifindex;
  if (addSubAttributes(
          rta, RTA_OIF, reinterpret_cast<const char*>(&oif), sizeof(oif)) ==
      nullptr) {
    return ResultCode::NO_MESSAGE_BUFFER;
  }

  // update length in rtnexthop
  rtnh->rtnh_len += sizeof(oif) + sizeof(struct rtattr);
  return ResultCode::SUCCESS;
}

ResultCode
NetlinkRouteMessage::addLabelNexthop(
    struct rtattr* rta,
    struct rtnexthop* rtnh,
    const openr::fbnl::NextHop& path) const {
  // fill the OIF
  rtnh->rtnh_len = sizeof(*rtnh);
  rtnh->rtnh_ifindex = path.getIfIndex().value(); /* interface index */
  rtnh->rtnh_flags = 0;
  rtnh->rtnh_hops = 0;

  // add the following subattributes within RTA_MULTIPATH
  size_t prevLen = rta->rta_len;

  // RTA_ENCAP sub attribute
  struct rtattr* rtaEncap = addSubAttributes(rta, RTA_ENCAP, nullptr, 0);

  if (rtaEncap == nullptr) {
    return ResultCode::NO_MESSAGE_BUFFER;
  }

  // MPLS_IP_TUNNEL_DST sub attribute
  std::array<struct mpls_label, kMaxLabels> mplsLabel;
  size_t i = 0;
  auto labels = path.getPushLabels();
  if (!labels.hasValue()) {
    LOG(ERROR) << "Labels not provided for PUSH action";
    return ResultCode::NO_LABEL;
  }
  for (auto label : labels.value()) {
    VLOG(2) << "Pushing label: " << label;
    bool bos = i == labels.value().size() - 1 ? true : false;
    mplsLabel[i++].entry = encodeLabel(label, bos);
  }
  size_t totalSize = labels.value().size() * sizeof(struct mpls_label);
  if (addSubAttributes(rta, MPLS_IPTUNNEL_DST, &mplsLabel, totalSize) ==
      nullptr) {
    return ResultCode::NO_MESSAGE_BUFFER;
  };

  // update RTA ENCAP sub attribute length
  rtaEncap->rta_len = RTA_ALIGN(rta->rta_len) - prevLen;

  // RTA_ENCAP_TYPE sub attribute
  uint16_t encapType = LWTUNNEL_ENCAP_MPLS;
  if (addSubAttributes(rta, RTA_ENCAP_TYPE, &encapType, sizeof(encapType)) ==
      nullptr) {
    return ResultCode::NO_MESSAGE_BUFFER;
  };

  // update rtnh len
  rtnh->rtnh_len += rta->rta_len - prevLen;

  // RTA_GATEWAY
  auto const via = path.getGateway();
  if (!via.hasValue()) {
    LOG(ERROR) << "Nexthop IP not provided";
    return ResultCode::NO_NEXTHOP_IP;
  }
  if (addSubAttributes(
          rta, RTA_GATEWAY, via.value().bytes(), via.value().byteCount()) ==
      nullptr) {
    return ResultCode::NO_MESSAGE_BUFFER;
  };

  // update length in rtnexthop
  rtnh->rtnh_len += via.value().byteCount() + sizeof(struct rtattr);
  return ResultCode::SUCCESS;
}

ResultCode
NetlinkRouteMessage::addNextHops(const openr::fbnl::Route& route) {
  ResultCode status{ResultCode::SUCCESS};
  std::array<char, kMaxNlPayloadSize> nhop = {};
  if (route.getNextHops().size()) {
    if ((status = addMultiPathNexthop(nhop, route)) != ResultCode::SUCCESS) {
      return status;
    };

    // copy the encap info into NLMSG payload
    const char* const data = reinterpret_cast<const char*>(
        RTA_DATA(reinterpret_cast<struct rtattr*>(nhop.data())));
    int payloadLen = RTA_PAYLOAD(reinterpret_cast<struct rtattr*>(nhop.data()));
    if ((status = addAttributes(RTA_MULTIPATH, data, payloadLen, msghdr_)) !=
        ResultCode::SUCCESS) {
      return status;
    };
  }
  return status;
}

ResultCode
NetlinkRouteMessage::addMultiPathNexthop(
    std::array<char, kMaxNlPayloadSize>& nhop,
    const openr::fbnl::Route& route) const {
  // Add [RTA_MULTIPATH - label, via, dev][RTA_ENCAP][RTA_ENCAP_TYPE]
  struct rtattr* rta = reinterpret_cast<struct rtattr*>(nhop.data());

  // MULTIPATH
  rta->rta_type = RTA_MULTIPATH;
  rta->rta_len = RTA_LENGTH(0);
  struct rtnexthop* rtnh = reinterpret_cast<struct rtnexthop*>(RTA_DATA(rta));
  ResultCode result{ResultCode::SUCCESS};

  const auto& paths = route.getNextHops();
  for (const auto& path : paths) {
    VLOG(3) << path.str();
    rtnh->rtnh_len = sizeof(*rtnh);
    rta->rta_len += rtnh->rtnh_len;
    auto action = path.getLabelAction();

    if (action.hasValue()) {
      switch (action.value()) {
      case thrift::MplsActionCode::PUSH:
        result = addLabelNexthop(rta, rtnh, path);
        break;

      case thrift::MplsActionCode::SWAP:
      case thrift::MplsActionCode::PHP:
        result = addSwapOrPHPNexthop(rta, rtnh, path);
        break;

      case thrift::MplsActionCode::POP_AND_LOOKUP:
        result = addPopNexthop(rta, rtnh, path);
        break;

      default:
        LOG(ERROR) << "Unknown action ";
        return ResultCode::UNKNOWN_LABEL_ACTION;
      }
    } else {
      result = addIpNexthop(rta, rtnh, path, route);
    }

    if (result != ResultCode::SUCCESS) {
      return result;
    }
    rtnh = RTNH_NEXT(rtnh);
  }
  return result;
}

void
NetlinkRouteMessage::showMultiPathAttribues(
    const struct rtattr* const rta) const {
  const struct rtnexthop* const rtnh =
      reinterpret_cast<struct rtnexthop*>(RTA_DATA(rta));
  LOG(INFO) << "len: " << rtnh->rtnh_len << " flags: " << rtnh->rtnh_flags;
  LOG(INFO) << "hop: " << rtnh->rtnh_hops << " ifindex: " << rtnh->rtnh_ifindex;

  const struct rtattr* subrta = RTNH_DATA(rtnh);
  int len = rtnh->rtnh_len;

  do {
    if (!RTA_OK(subrta, len)) {
      break;
    }
    showRouteAttribute(subrta);
  } while ((subrta = RTA_NEXT(subrta, len)));
}

void
NetlinkRouteMessage::parseMessage() const {
  LOG(INFO) << "process route message: " << *this;
  const struct rtmsg* const route_entry = (struct rtmsg*)NLMSG_DATA(msghdr_);
  showRtmMsg(route_entry);

  if (route_entry->rtm_table != RT_TABLE_MAIN) {
    return;
  }
  // first route attribute
  const struct rtattr* routeAttr = (const struct rtattr*)RTM_RTA(route_entry);
  auto routeAttrLen = RTM_PAYLOAD(msghdr_);

  // process all route attributes
  do {
    if (!RTA_OK(routeAttr, routeAttrLen)) {
      break;
    }
    showRouteAttribute(routeAttr);
    if (routeAttr->rta_type == RTA_MULTIPATH) {
      showMultiPathAttribues(routeAttr);
    }
  } while ((routeAttr = RTA_NEXT(routeAttr, routeAttrLen)));
}

ResultCode
NetlinkRouteMessage::addRoute(const openr::fbnl::Route& route) {
  auto const& pfix = route.getDestination();
  auto ip = std::get<0>(pfix);
  auto plen = std::get<1>(pfix);
  auto addressFamily = route.getFamily();

  VLOG(1) << "Adding route: " << route.str();

  if (addressFamily != AF_INET && addressFamily != AF_INET6) {
    LOG(ERROR) << "Address family is not AF_INET or AF_INET6";
    return ResultCode::INVALID_ADDRESS_FAMILY;
  }

  init(RTM_NEWROUTE, RTM_F_NOTIFY, route);

  rtmsg_->rtm_family = addressFamily;
  rtmsg_->rtm_dst_len = plen; /* netmask */
  const char* const ipptr = reinterpret_cast<const char*>(ip.bytes());
  ResultCode status{ResultCode::SUCCESS};
  if ((status = addAttributes(RTA_DST, ipptr, ip.byteCount(), msghdr_)) !=
      ResultCode::SUCCESS) {
    return status;
  };

  return addNextHops(route);
}

ResultCode
NetlinkRouteMessage::deleteRoute(const openr::fbnl::Route& route) {
  auto const& pfix = route.getDestination();
  auto addressFamily = route.getFamily();
  VLOG(1) << "Deleting route: " << route.str();

  if (addressFamily != AF_INET && addressFamily != AF_INET6) {
    return ResultCode::INVALID_ADDRESS_FAMILY;
  }
  init(RTM_DELROUTE, RTM_F_NOTIFY, route);

  auto plen = std::get<1>(pfix);
  auto ip = std::get<0>(pfix);
  rtmsg_->rtm_family = addressFamily;
  rtmsg_->rtm_dst_len = plen; /* netmask */
  const char* const ipptr = reinterpret_cast<const char*>(ip.bytes());
  ResultCode status{ResultCode::SUCCESS};
  if ((status = addAttributes(RTA_DST, ipptr, ip.byteCount(), msghdr_)) !=
      ResultCode::SUCCESS) {
    return status;
  };
  return status;
}

ResultCode
NetlinkRouteMessage::addLabelRoute(const openr::fbnl::Route& route) {
  init(RTM_NEWROUTE, 0, route);
  rtmsg_->rtm_family = AF_MPLS;
  rtmsg_->rtm_dst_len = kLabelSizeBits;
  rtmsg_->rtm_flags = 0;
  struct mpls_label mlabel;

  VLOG(1) << "Adding MPLS route " << route.str();
  if (route.getFamily() != AF_MPLS) {
    return ResultCode::INVALID_ADDRESS_FAMILY;
  }

  auto label = route.getMplsLabel();
  if (!label.hasValue()) {
    return ResultCode::NO_LABEL;
  }

  mlabel.entry = encodeLabel(label.value(), true);
  ResultCode status{ResultCode::SUCCESS};
  if ((status = addAttributes(
           RTA_DST,
           reinterpret_cast<const char*>(&mlabel),
           sizeof(mpls_label),
           msghdr_)) != ResultCode::SUCCESS) {
    return status;
  };

  return addNextHops(route);
}

ResultCode
NetlinkRouteMessage::deleteLabelRoute(const openr::fbnl::Route& route) {
  init(RTM_DELROUTE, 0, route);
  rtmsg_->rtm_family = AF_MPLS;
  rtmsg_->rtm_dst_len = kLabelSizeBits;
  rtmsg_->rtm_flags = 0;
  struct mpls_label mlabel;
  auto label = route.getMplsLabel();
  if (!label.hasValue()) {
    LOG(ERROR) << "Label not provided";
    return ResultCode::NO_LABEL;
  }
  VLOG(1) << "Deleting label: " << route.str();
  mlabel.entry = encodeLabel(label.value(), true);
  ResultCode status{ResultCode::SUCCESS};
  if ((status = addAttributes(
           RTA_DST,
           reinterpret_cast<const char*>(&mlabel),
           sizeof(mpls_label),
           msghdr_)) != ResultCode::SUCCESS) {
    return status;
  };

  return status;
}
} // namespace Netlink
} // namespace openr
