#ifndef RFCLIENT_HH
#define RFCLIENT_HH

#include <net/if.h>
#include <map>
#include <vector>
#include <string>

#include "ipc/IPC.h"
#include "ipc/RFProtocolFactory.h"
#include "FlowTable.hh"
#include "PortMapper.hh"

class RFClient : private RFProtocolFactory, private IPCMessageProcessor,
                 public InterfaceMap {
    public:
        RFClient(uint64_t id, const string &address, RouteSource);
        bool findInterface(const char *ifName, Interface *dst);

    private:
        FlowTable* flowTable;
        PortMapper* portMapper;
        IPCMessageService* ipc;
        uint64_t id;

        boost::mutex ifMutex; /* This guards both of the maps below. */
        map<string, Interface> ifacesMap;
        map<int, Interface*> interfaces;

        uint8_t hwaddress[IFHWADDRLEN];

        void startFlowTable(RouteSource source);
        void startPortMapper();
        bool process(const string &from, const string &to,
                     const string &channel, IPCMessage& msg);

        int set_hwaddr_byname(const char * ifname, uint8_t hwaddr[], int16_t flags);
        uint32_t get_port_number(string ifName);
        map<string, Interface> load_interfaces();
};
#endif /* RFCLIENT_HH */
