#ifndef INTERFACE_HH
#define INTERFACE_HH

#include "types/IPAddress.h"
#include "types/MACAddress.h"

class Interface {
    public:
        uint32_t port;
        string name;
        std::vector<IPAddress> addresses;
        MACAddress hwaddress;
        bool active;

        Interface() {
            this->active = false;
        }

        Interface& operator=(const Interface& other) {
            if (this != &other) {
                this->port = other.port;
                this->name = other.name;
                this->addresses = other.addresses;
                this->hwaddress = other.hwaddress;
                this->active = other.active;
            }
            return *this;
        }

        bool operator==(const Interface& other) const {
            return
                (this->port == other.port) and
                (this->name == other.name) and
                (this->addresses == other.addresses) and
                (this->hwaddress == other.hwaddress) and
                (this->active == other.active);
        }
};

/**
 * Abstract class InterfaceMap. Subclasses can implement this functionality
 * to provide access to Interfaces by name.
 */
class InterfaceMap {
    public:
        /**
         * Searches for an interface matching the given name. If it exists,
         * the interface is copied into *dst.
         *
         * Returns true on success, or false on failure.
         */
        virtual bool findInterface(const char *ifName, Interface *dst) = 0;
};

#endif /* INTERFACE_HH */
