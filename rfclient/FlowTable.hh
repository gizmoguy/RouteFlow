#ifndef FLOWTABLE_HH_
#define FLOWTABLE_HH_

#include <list>
#include <map>
#include <stdint.h>
#include <boost/thread.hpp>
#include "libnetlink.hh"
#include "SyncQueue.hh"

#include "fpm.h"
#include "fpm_lsp.h"

#include "ipc/IPC.h"
#include "ipc/RFProtocol.h"
#include "types/IPAddress.h"
#include "types/MACAddress.h"
#include "defs.h"

#include "Interface.hh"
#include "RouteEntry.hh"
#include "HostEntry.hh"
#include "PendingRoute.hh"

using namespace std;

typedef enum route_source {
    RS_NETLINK,
    RS_FPM,
} RouteSource;

class FlowTable {
    public:
        FlowTable(uint64_t vm_id, InterfaceMap*, IPCMessageService*,
                  RouteSource src);
        FlowTable(const FlowTable&);

        static void GWResolverCb(FlowTable *ft);
        void operator()();

        void clear();
        void interrupt();
        void print_test();

        int updateHostTable(struct nlmsghdr*);
        int updateRouteTable(struct nlmsghdr*);
        void updateNHLFE(nhlfe_msg_t *nhlfe_msg);

    private:
        RouteSource source;
        InterfaceMap* ifMap;
        IPCMessageService* ipc;
        uint64_t vm_id;

        boost::thread GWResolver;
        boost::thread HTPolling;
        boost::thread RTPolling;

        /* NetLink handlers */
        struct rtnl_handle rthNeigh;
        struct rtnl_handle rth;

        /* Known hosts */
        map<string, HostEntry> hostTable;
        boost::mutex hostTableMutex;

        /* Known routes */
        SyncQueue<PendingRoute> pendingRoutes;
        list<RouteEntry> routeTable;

        /* Recently deleted routes */
        map<string, RouteEntry> deletedRoutes;
        boost::mutex drMutex;

        /* Cached neighbour discovery sockets */
        map<string, int> pendingNeighbours;
        boost::mutex ndMutex;

        bool is_port_down(uint32_t port);
        int getInterface( const char *iface, const char *type, Interface*);

        int initiateND(const char *hostAddr);
        void stopND(const string &hostAddr);
        int resolveGateway(const IPAddress&, const Interface&);
        const MACAddress& findHost(const IPAddress& host);

        int setEthernet(RouteMod& rm, const Interface& local_iface,
                        const MACAddress& gateway);
        int setIP(RouteMod& rm, const IPAddress& addr, const IPAddress& mask);
        int sendToHw(RouteModType, const RouteEntry&);
        int sendToHw(RouteModType, const HostEntry&);
        int sendToHw(RouteModType, const IPAddress& addr,
                     const IPAddress& mask, const Interface&,
                     const MACAddress& gateway);
};

#endif /* FLOWTABLE_HH_ */
