#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/ether.h>
#include <sys/socket.h>
#include <time.h>
#include <syslog.h>

#include <string>
#include <vector>
#include <cstring>
#include <iostream>

#include "converter.h"
#include "FlowTable.hh"
#include "FPMServer.hh"

using namespace std;

#define EMPTY_MAC_ADDRESS "00:00:00:00:00:00"
#define ROUTE_COOLDOWN 5000 /* milliseconds */

static const MACAddress MAC_ADDR_NONE(EMPTY_MAC_ADDRESS);

// TODO: implement a way to pause the flow table updates when the VM is not
//       associated with a valid datapath

static int HTPollingCb(const struct sockaddr_nl*, struct nlmsghdr *n,
                       void *arg) {
    FlowTable *ft = reinterpret_cast<FlowTable *>(arg);
    return ft->updateHostTable(n);
}

static int RTPollingCb(const struct sockaddr_nl*, struct nlmsghdr *n,
                       void *arg) {
    FlowTable *ft = reinterpret_cast<FlowTable *>(arg);
    return ft->updateRouteTable(n);
}

FlowTable::FlowTable(uint64_t id, InterfaceMap *ifMap, SyncQueue<RouteMod> *rm_q, RouteSource source) {
    this->vm_id = id;
    this->ifMap = ifMap;
    this->rm_q = rm_q;
    this->source = source;
}

FlowTable::FlowTable(const FlowTable& other) {
    this->vm_id = other.vm_id;
    this->ifMap = other.ifMap;
    this->rm_q = other.rm_q;
    this->source = other.source;
}

void FlowTable::operator()() {
    rtnl_open(&rthNeigh, RTMGRP_NEIGH);
    int rs = setsockopt(rthNeigh.fd, SOL_SOCKET, SO_RCVBUFFORCE, &nl_buffersize, sizeof(nl_buffersize));
    if (rs != 0) {
        syslog(LOG_CRIT, "cannot set socket size for neighbors: %d", errno);
        exit(rs);
    }
    HTPolling = boost::thread(&rtnl_listen, &rthNeigh, &HTPollingCb, this);

    switch (this->source) {
        case RS_NETLINK: {
            syslog(LOG_NOTICE, "Netlink interface enabled");
            rtnl_open(&rth, RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE |
                            RTMGRP_IPV4_MROUTE | RTMGRP_IPV6_MROUTE);
            int rs = setsockopt(rth.fd, SOL_SOCKET, SO_RCVBUFFORCE, &nl_buffersize, sizeof(nl_buffersize));
            if (rs != 0) {
                syslog(LOG_CRIT, "cannot set socket size for routes: %d", errno);
                exit(rs);
            }
            RTPolling = boost::thread(&rtnl_listen, &rth, &RTPollingCb, this);
            break;
        }
        case RS_FPM: {
            FPMServer *fpm = new FPMServer(this);
            RTPolling = boost::thread(*fpm);
            syslog(LOG_NOTICE, "FPM interface enabled");
            break;
        }
        default: {
            syslog(LOG_CRIT, "Invalid route source specified. Disabling route updates.");
            break;
        }
    }

    GWResolver = boost::thread(&FlowTable::GWResolverCb, this);
    GWResolver.join();
}

uint64_t FlowTable::get_vm_id() {
    return this->vm_id;
}

void FlowTable::clear() {
    this->routeTable.clear();
    boost::lock_guard<boost::mutex> lock(hostTableMutex);
    this->hostTable.clear();
}

void FlowTable::interrupt() {
    HTPolling.interrupt();
    GWResolver.interrupt();
    RTPolling.interrupt();
}

void FlowTable::sendRm(RouteMod rm) {
    this->rm_q->push(rm);
}

void FlowTable::GWResolverCb(FlowTable *ft) {
    while (true) {
        PendingRoute pr;
        while (ft->pendingRoutes.front(pr)) {
            ft->pendingRoutes.pop();

            const RouteEntry& re = pr.rentry;
            const string re_key = re.toString();
            const string addr_str = re.address.toString();
            const string mask_str = re.netmask.toString();
            const string gw_str = re.gateway.toString();
            bool existingEntry = ft->routeTable.count(re_key) > 0;

            switch (pr.type) {
                case RMT_ADD:
                    if (existingEntry) {
                        syslog(LOG_ERR,
                              "Received duplicate route add for %s",
                              addr_str.c_str());
                    } else {
                        ft->routeTable.insert(make_pair(re_key, re));
                        if (ft->findHost(re.gateway) == MAC_ADDR_NONE) {
                            syslog(LOG_ERR,
                                   "Cannot resolve gateway %s, will retry route %s/%s",
                                    gw_str.c_str(), addr_str.c_str(), mask_str.c_str());
                            ft->unresolvedRoutes.insert(re_key);
                        } else {
                            syslog(LOG_DEBUG,
                                  "Adding route %s/%s via %s",
                                  addr_str.c_str(), mask_str.c_str(), gw_str.c_str());
                            ft->sendToHw(pr.type, pr.rentry);
                        }
                    }
                    break;

                case RMT_DELETE:
                    if (existingEntry) {
                        ft->routeTable.erase(re_key);
                        ft->unresolvedRoutes.erase(re_key);
                        syslog(LOG_DEBUG,
                               "Deleting route %s/%s via %s",
                               addr_str.c_str(), mask_str.c_str(), gw_str.c_str());
                        ft->sendToHw(pr.type, pr.rentry);
                          
                    } else {
                        syslog(LOG_ERR,
                               "Received route removal for %s but not in routing table",
                               addr_str.c_str());
                    }
                    break;

                default:
                    syslog(LOG_ERR,
                           "Received unexpected RouteModType (%d)",
                           pr.type);
                    break;
            }
        }
        if (ft->unresolvedRoutes.size() > 0) {
            set<string> resolvedRoutes;
            set<string>::iterator it;
            uint64_t gatewaysResolved = 0;
            for (it = ft->unresolvedRoutes.begin(); it != ft->unresolvedRoutes.end(); ++it) {
                const RouteEntry& re = ft->routeTable[*it];
                const string addr_str = re.address.toString();
                const string mask_str = re.netmask.toString();
                const string gw_str = re.gateway.toString();
                if (ft->findHost(re.gateway) == MAC_ADDR_NONE) {
                    syslog(LOG_DEBUG,
                          "Still cannot resolve gateway %s, will retry route %s/%s",
                          gw_str.c_str(), addr_str.c_str(), mask_str.c_str());
                    ft->resolveGateway(re.gateway, re.interface);
                } else {
                    syslog(LOG_DEBUG,
                           "Adding previously unresolved route %s/%s via %s",
                           addr_str.c_str(), mask_str.c_str(), gw_str.c_str());
                    ft->sendToHw(RMT_ADD, re);
                    resolvedRoutes.insert(*it);
                    ++gatewaysResolved;
                }
            }
            if (gatewaysResolved) {
                for (it = resolvedRoutes.begin(); it != resolvedRoutes.end(); ++it) {
                    ft->unresolvedRoutes.erase(*it);    
                }
            }
        } else {
            usleep(1000);
        } 
    }
}

/**
 * Get the local interface corresponding to the given interface number.
 *
 * On success, overwrites given interface pointer with the active interface
 * and returns 0;
 * On error, logs it and returns -1.
 */
int FlowTable::getInterface(const char *intf, const char *type,
                            Interface *iface) {
    Interface temp;
    if (!ifMap->findInterface(intf, &temp)) {
        syslog(LOG_ERR, "Interface %s not found, dropping %s entry\n",
               intf, type);
        return -1;
    }

    *iface = temp;
    return 0;
}

int rta_to_ip(unsigned char family, const void *ip, IPAddress& result) {
    if (family == AF_INET) {
        result = IPAddress(reinterpret_cast<const struct in_addr *>(ip));
    } else if (family == AF_INET6) {
        result = IPAddress(reinterpret_cast<const struct in6_addr *>(ip));
    } else {
        syslog(LOG_ERR, "Unrecognised nlmsg family");
        return -1;
    }

    if (result.toString() == "") {
        syslog(LOG_WARNING, "Blank IP address. Dropping Route\n");
        return -1;
    }

    return 0;
}

int FlowTable::updateHostTable(struct nlmsghdr *n) {
    char error[BUFSIZ];
    struct ndmsg *ndmsg_ptr = (struct ndmsg *) NLMSG_DATA(n);
    struct rtattr *rtattr_ptr;

    char intf[IF_NAMESIZE + 1];
    memset(intf, 0, IF_NAMESIZE + 1);

    boost::this_thread::interruption_point();

    if (if_indextoname((unsigned int) ndmsg_ptr->ndm_ifindex, (char *) intf) == NULL) {
        strerror_r(errno, error, BUFSIZ);
        syslog(LOG_ERR, "HostTable: %s", error);
        return 0;
    }

    if (strcmp(intf, DEFAULT_RFCLIENT_INTERFACE) == 0) {
        return 0;
    }

    boost::scoped_ptr<HostEntry> hentry(new HostEntry());

    char mac[2 * IFHWADDRLEN + 5 + 1];
    memset(mac, 0, 2 * IFHWADDRLEN + 5 + 1);

    rtattr_ptr = (struct rtattr *) RTM_RTA(ndmsg_ptr);
    int rtmsg_len = RTM_PAYLOAD(n);

    for (; RTA_OK(rtattr_ptr, rtmsg_len); rtattr_ptr = RTA_NEXT(rtattr_ptr, rtmsg_len)) {
        switch (rtattr_ptr->rta_type) {
        case RTA_DST: {
            if (rta_to_ip(ndmsg_ptr->ndm_family, RTA_DATA(rtattr_ptr),
                          hentry->address) < 0) {
                return 0;
            }
            break;
        }
        case NDA_LLADDR:
            if (strncpy(mac, ether_ntoa(((ether_addr *) RTA_DATA(rtattr_ptr))), sizeof(mac)) == NULL) {
                strerror_r(errno, error, BUFSIZ);
                syslog(LOG_ERR, "HostTable: %s", error);
                return 0;
            }
            break;
        default:
            break;
        }
    }

    if (getInterface(intf, "host", &hentry->interface) != 0) {
        return 0;
    }

    const string host = hentry->address.toString();
    hentry->hwaddress = MACAddress(mac);

    if (strlen(mac) == 0) {
        syslog(LOG_DEBUG, "Received host entry ip=%s with blank mac. Ignoring", host.c_str());
        return 0;
    }

    switch (n->nlmsg_type) {
        case RTM_NEWNEIGH: {
            syslog(LOG_DEBUG, "netlink->RTM_NEWNEIGH: ip=%s, mac=%s",
                   host.c_str(), mac);
            this->sendToHw(RMT_ADD, *hentry);
            {
                // Add to host table
                boost::lock_guard<boost::mutex> lock(hostTableMutex);
                this->hostTable[host] = *hentry;
            }
            // If we have been attempting neighbour discovery for this
            // host, then we can close the associated socket.
            stopND(host);
            break;
        }
    }

    return 0;
}

int FlowTable::updateRouteTable(struct nlmsghdr *n) {
    struct rtmsg *rtmsg_ptr = (struct rtmsg *) NLMSG_DATA(n);

    boost::this_thread::interruption_point();

    if (!((n->nlmsg_type == RTM_NEWROUTE || n->nlmsg_type == RTM_DELROUTE) &&
          rtmsg_ptr->rtm_table == RT_TABLE_MAIN)) {
        return 0;
    }

    boost::scoped_ptr<RouteEntry> rentry(new RouteEntry());

    char intf[IF_NAMESIZE + 1];
    memset(intf, 0, IF_NAMESIZE + 1);

    struct rtattr *rtattr_ptr;
    rtattr_ptr = (struct rtattr *) RTM_RTA(rtmsg_ptr);
    int rtmsg_len = RTM_PAYLOAD(n);

    for (; RTA_OK(rtattr_ptr, rtmsg_len); rtattr_ptr = RTA_NEXT(rtattr_ptr, rtmsg_len)) {
        switch (rtattr_ptr->rta_type) {
        case RTA_DST:
            if (rta_to_ip(rtmsg_ptr->rtm_family, RTA_DATA(rtattr_ptr),
                          rentry->address) < 0) {
                return 0;
            }
            break;
        case RTA_GATEWAY:
            if (rta_to_ip(rtmsg_ptr->rtm_family, RTA_DATA(rtattr_ptr),
                          rentry->gateway) < 0) {
                return 0;
            }
            break;
        case RTA_OIF:
            if_indextoname(*((int *) RTA_DATA(rtattr_ptr)), (char *) intf);
            break;
        case RTA_MULTIPATH: {
            struct rtnexthop *rtnhp_ptr = (struct rtnexthop *) RTA_DATA(
                    rtattr_ptr);
            int rtnhp_len = RTA_PAYLOAD(rtattr_ptr);

            if (rtnhp_len < (int) sizeof(*rtnhp_ptr)) {
                break;
            }

            if (rtnhp_ptr->rtnh_len > rtnhp_len) {
                break;
            }

            if_indextoname(rtnhp_ptr->rtnh_ifindex, (char *) intf);

            int attrlen = rtnhp_len - sizeof(struct rtnexthop);

            if (attrlen) {
                struct rtattr *attr = RTNH_DATA(rtnhp_ptr);

                for (; RTA_OK(attr, attrlen); attr = RTA_NEXT(attr, attrlen))
                    if (attr->rta_type == RTA_GATEWAY) {
                        if (rta_to_ip(rtmsg_ptr->rtm_family, RTA_DATA(attr),
                                      rentry->gateway) < 0) {
                            return 0;
                        }
                        break;
                    }
            }
        }
            break;
        default:
            break;
        }
    }

    if (strcmp(intf, DEFAULT_RFCLIENT_INTERFACE) == 0) {
        return 0;
    }

    if (getInterface(intf, "route", &rentry->interface) != 0) {
        return 0;
    }

    switch (rtmsg_ptr->rtm_family) {
    case AF_INET6:
        rentry->netmask = IPAddress(IPV6, rtmsg_ptr->rtm_dst_len);
        break;
    default:
        rentry->netmask = IPAddress(IPV4, rtmsg_ptr->rtm_dst_len);
        break;
    }

    if (rtmsg_ptr->rtm_dst_len == 0) {
        /* Default route. Zero the address. */
        rentry->address = rentry->netmask;
    }

    string net = rentry->address.toString();
    string mask = rentry->netmask.toString();
    string gw = rentry->gateway.toString();

    switch (n->nlmsg_type) {
        case RTM_NEWROUTE:
            syslog(LOG_DEBUG, "netlink->RTM_NEWROUTE: net=%s, mask=%s, gw=%s",
                   net.c_str(), mask.c_str(), gw.c_str());
            this->pendingRoutes.push(PendingRoute(RMT_ADD, *rentry));
            break;
        case RTM_DELROUTE:
            syslog(LOG_DEBUG, "netlink->RTM_DELROUTE: net=%s, mask=%s, gw=%s",
                   net.c_str(), mask.c_str(), gw.c_str());
            this->pendingRoutes.push(PendingRoute(RMT_DELETE, *rentry));
            break;
    }

    return 0;
}

/**
 * Begins the neighbour discovery process to the specified host.
 *
 * Returns an open socket on success, or -1 on error.
 */
int FlowTable::initiateND(const char *hostAddr) {
    char error[BUFSIZ];
    int s, flags;
    struct sockaddr_storage store;
    struct sockaddr_in *sin = (struct sockaddr_in*)&store;
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6*)&store;

    memset(&store, 0, sizeof(store));

    if (inet_pton(AF_INET, hostAddr, &sin->sin_addr) == 1) {
        store.ss_family = AF_INET;
    } else if (inet_pton(AF_INET6, hostAddr, &sin6->sin6_addr) == 1) {
        store.ss_family = AF_INET6;
        syslog(LOG_ERR, "Refusing to initiateND() for IPv6: %s\n", hostAddr);
        return -1;
    } else {
        syslog(LOG_ERR, "Invalid address family for IP \"%s\". Refusing to "
               "initiateND().\n", hostAddr);
        return -1;
    }

    if ((s = socket(store.ss_family, SOCK_STREAM, 0)) < 0) {
        strerror_r(errno, error, BUFSIZ);
        syslog(LOG_ERR, "socket(): %s", error);
        return -1;
    }

    // Prevent the connect() call from blocking
    flags = fcntl(s, F_GETFL, 0);
    if (fcntl(s, F_SETFL, flags | O_NONBLOCK) == -1) {
        strerror_r(errno, error, BUFSIZ);
        syslog(LOG_ERR, "fcntl(): %s", error);
        close(s);
        return -1;
    }

    connect(s, (struct sockaddr *)&store, sizeof(store));
    return s;
}

void FlowTable::stopND(const string &host) {
    char error[BUFSIZ];
    boost::lock_guard<boost::mutex> lock(ndMutex);
    map<string, int>::iterator iter = pendingNeighbours.find(host);
    if (iter != pendingNeighbours.end()) {
        if (close(iter->second) == -1) {
            strerror_r(errno, error, BUFSIZ);
            syslog(LOG_ERR, "pendingNeighbours: %s", error);
        }
        pendingNeighbours.erase(host);
    }
}

/**
 * Initiates the gateway resolution process for the given host.
 *
 * Returns 0 on success, -1 on error (usually an issue with the socket)
 */
int FlowTable::resolveGateway(const IPAddress& gateway,
                              const Interface& iface) {
    if (!iface.active) {
        return -1;
    }

    string gateway_str = gateway.toString();

    // If we already initiated neighbour discovery for this gateway, return.
    boost::lock_guard<boost::mutex> lock(ndMutex);
    if (pendingNeighbours.find(gateway_str) != pendingNeighbours.end()) {
        syslog(LOG_DEBUG, "already doing neighbour discovery for %s", gateway_str.c_str());
        return 0;
    }

    // Otherwise, we should go ahead and begin the process.
    syslog(LOG_DEBUG, "starting neighbour discovery for %s", gateway_str.c_str());
    int sock = initiateND(gateway_str.c_str());
    if (sock == -1) {
        return -1;
    }
    this->pendingNeighbours[gateway_str] = sock;

    return 0;
}

/**
 * Find the MAC Address for the given host in a thread-safe manner.
 *
 * This searches the internal hostTable structure for the given host, and
 * returns its MAC Address. If the host is unresolved, this will return
 * MAC_ADDR_NONE. Neighbour Discovery is not performed by this function.
 */
const MACAddress& FlowTable::findHost(const IPAddress& host) {
    boost::lock_guard<boost::mutex> lock(hostTableMutex);
    map<string, HostEntry>::iterator iter;
    iter = FlowTable::hostTable.find(host.toString());
    if (iter != FlowTable::hostTable.end()) {
        return iter->second.hwaddress;
    }

    return MAC_ADDR_NONE;
}

int FlowTable::setEthernet(RouteMod& rm, const Interface& local_iface,
                           const MACAddress& gateway) {
    /* RFServer adds the Ethernet match to the flow, so we don't need to. */
    rm.add_action(Action(RFAT_SET_ETH_SRC, local_iface.hwaddress));
    rm.add_action(Action(RFAT_SET_ETH_DST, gateway));

    return 0;
}

int FlowTable::setIP(RouteMod& rm, const IPAddress& addr,
                     const IPAddress& mask) {
     if (addr.getVersion() == IPV4) {
        rm.add_match(Match(RFMT_IPV4, addr, mask));
    } else if (addr.getVersion() == IPV6) {
        rm.add_match(Match(RFMT_IPV6, addr, mask));
    } else {
        syslog(LOG_ERR, "Invalid address family for IP %s\n",
               addr.toString().c_str());
        return -1;
    }

    uint16_t priority = PRIORITY_LOW;
    priority += (mask.toPrefixLen() * PRIORITY_BAND);
    rm.add_option(Option(RFOT_PRIORITY, priority));

    return 0;
}

int FlowTable::sendToHw(RouteModType mod, const RouteEntry& re) {
    const string gateway_str = re.gateway.toString();
    const MACAddress& remoteMac = findHost(re.gateway);
    if (remoteMac == MAC_ADDR_NONE) {
        syslog(LOG_ERR, "Cannot resolve %s", gateway_str.c_str());
        return -1;
    }

    return sendToHw(mod, re.address, re.netmask, re.interface, remoteMac);
}

int FlowTable::sendToHw(RouteModType mod, const HostEntry& he) {
    boost::scoped_ptr<IPAddress> mask;

    if (he.address.getVersion() == IPV6) {
        mask.reset(new IPAddress(IPV6, FULL_IPV6_PREFIX));
    } else if (he.address.getVersion() == IPV4) {
        mask.reset(new IPAddress(IPV4, FULL_IPV4_PREFIX));
    } else {
        syslog(LOG_ERR, "Received HostEntry with invalid address family\n");
        return -1;
    }

    return sendToHw(mod, he.address, *mask.get(), he.interface, he.hwaddress);
}

int FlowTable::sendToHw(RouteModType mod, const IPAddress& addr,
                         const IPAddress& mask, const Interface& local_iface,
                         const MACAddress& gateway) {
    if (!local_iface.active) {
        syslog(LOG_ERR, "Cannot send RouteMod for down port %s\n", local_iface.name.c_str());
        return -1;
    }

    RouteMod rm;

    rm.set_mod(mod);
    rm.set_id(FlowTable::vm_id);

    if (local_iface.vlan) {
        // rm.add_match(Match(RFMT_VLAN, local_iface.vlan));
        rm.add_action(Action(RFAT_SWAP_VLAN_ID, local_iface.vlan));
    }

    const string gw_str = gateway.toString();

    if (setEthernet(rm, local_iface, gateway) != 0) {
        syslog(LOG_ERR, "cannot setEthernet for %s", gw_str.c_str());
        return -1;
    }
    if (setIP(rm, addr, mask) != 0) {
        syslog(LOG_ERR, "cannot setIP for %s", gw_str.c_str());
        return -1;
    }

    /* Add the output port. Even if we're removing the route, RFServer requires
     * the port to determine which datapath to send to. */
    // rm.add_action(Action(RFAT_OUTPUT, local_iface.port));
    rm.set_vm_port(local_iface.port);

    syslog(LOG_DEBUG, "sending rfserver IPC for %s/%s via %s on port %u",
                      addr.toString().c_str(), mask.toString().c_str(),
                      gw_str.c_str(), local_iface.port);
    this->sendRm(rm);
    return 0;
}

/*
 * Add or remove a Push, Pop or Swap operation matching on a label only
 * For matching on IP, update FTN (not yet implemented) is needed
 *
 * TODO: If an error occurs here, the NHLFE is silently dropped. Fix this.
 */
void FlowTable::updateNHLFE(nhlfe_msg_t *nhlfe_msg) {
    RouteMod msg;

    if (nhlfe_msg->table_operation == ADD_LSP) {
        msg.set_mod(RMT_ADD);
    } else if (nhlfe_msg->table_operation == REMOVE_LSP) {
        msg.set_mod(RMT_DELETE);
    } else {
        syslog(LOG_WARNING, "Unrecognised NHLFE table operation %d",
               nhlfe_msg->table_operation);
        return;
    }
    msg.set_id(FlowTable::vm_id);

    // We need the next-hop IP to determine which interface to use.
    int version = nhlfe_msg->ip_version;
    uint8_t* ip_data = reinterpret_cast<uint8_t*>(&nhlfe_msg->next_hop_ip);
    IPAddress gwIP(version, ip_data);

    // Get our interface for packet egress.
    Interface iface;
    map<string, HostEntry>::iterator iter;
    iter = FlowTable::hostTable.find(gwIP.toString());
    if (iter == FlowTable::hostTable.end()) {
        syslog(LOG_WARNING, "Failed to locate interface for LSP");
        return;
    } else {
        iface = iter->second.interface;
    }

    if (!iface.active) {
        syslog(LOG_WARNING, 
               "Cannot send route via inactive interface %s",
               iface.name.c_str());
        return;
    }

    // Get the MAC address corresponding to our gateway.
    const MACAddress& gwMAC = findHost(gwIP);
    if (gwMAC == MAC_ADDR_NONE) {
        syslog(LOG_ERR, "Failed to resolve gateway MAC for NHLFE");
        return;
    }

    if (setEthernet(msg, iface, gwMAC) != 0) {
        return;
    }

    // Match on in_label only - matching on IP is the domain of FTN not NHLFE
    msg.add_match(Match(RFMT_MPLS, nhlfe_msg->in_label));

    if (nhlfe_msg->nhlfe_operation == PUSH) {
        msg.add_action(Action(RFAT_PUSH_MPLS, ntohl(nhlfe_msg->out_label)));
    } else if (nhlfe_msg->nhlfe_operation == POP) {
        msg.add_action(Action(RFAT_POP_MPLS, (uint32_t)0));
    } else if (nhlfe_msg->nhlfe_operation == SWAP) {
        msg.add_action(Action(RFAT_SWAP_MPLS, ntohl(nhlfe_msg->out_label)));
    } else {
        syslog(LOG_ERR, "Unknown lsp_operation");
        return;
    }

    // msg.add_action(Action(RFAT_OUTPUT, iface.port));
    msg.set_vm_port(iface.port);

    this->sendRm(msg);
    return;
}
