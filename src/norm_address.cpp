/*
    Copyright (c) 2007-2014 Contributors as noted in the AUTHORS file

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string>
#include <sstream>
#include <ctype.h>

#include "norm_address.hpp"
#include "platform.hpp"
#include "stdint.hpp"
#include "err.hpp"
#include "ip.hpp"

#ifdef ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#else
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netdb.h>
#endif


zmq::norm_address_t::norm_address_t ()
{
    memset (&address, 0, sizeof (address));
}

zmq::norm_address_t::norm_address_t (const sockaddr *sa, socklen_t sa_len)
{
    zmq_assert(sa && sa_len > 0);

    memset (&address, 0, sizeof (address));
    if (sa->sa_family == AF_INET && sa_len >= (socklen_t) sizeof (address.ipv4))
        memcpy(&address.ipv4, sa, sizeof (address.ipv4));
    else 
    if (sa->sa_family == AF_INET6 && sa_len >= (socklen_t) sizeof (address.ipv6))
        memcpy(&address.ipv6, sa, sizeof (address.ipv6));
}

zmq::norm_address_t::~norm_address_t ()
{
}

int zmq::norm_address_t::resolve (const char *name_, bool local_, bool ipv6_)
{
    // Parse the "name_" address into "id", "iface", "addr", and "port"
    // norm endpoint format: [id,][<iface>;]<addr>:<port>

    // If there is a "norm://" prefix, skip past it
    if (strlen (name_) >= 7 && strncasecmp (name_, "norm://", 7) == 0) {
        name_ += 7;
    }

    // First, look for optional local NormNodeId
    // Default of NORM_NODE_ANY tells NORM to use host IPv4 addr for NormNodeId
    localId = NORM_NODE_ANY;
    const char* ifacePtr = strchr (name_, ',');
    if (ifacePtr)
    {
        char idText[11];  // 10 chars will hold any 32 bit number in decimal.
        size_t idLen = ifacePtr - name_;
        if (idLen >= sizeof (idText)) {  // error if id number too long
            errno = EINVAL;
            return -1;
        }
        memcpy (idText, name_, idLen);
        idText[idLen] = '\0';
        for (size_t i = 0; i < idLen; i++) {
            if (!isdigit (idText[i]) ) {
                errno = EINVAL;
                return -1;
            }
        }
        localId = (NormNodeId)strtoul (idText, (char **) NULL, 10);
        if (localId == NORM_NODE_NONE) {  // NORM_NODE_NONE (0) is not allowed
            errno = EINVAL;
            return -1;
        }
        ifacePtr++;
    }
    else
    {
        localId = NORM_NODE_ANY;
        ifacePtr = name_;
    }
    
    // Second, look for optional multicast ifaceName, and if found validate it
    const char* addrPtr = strchr (ifacePtr, ';');
    if (addrPtr)
    {
        size_t ifaceLen = addrPtr - ifacePtr;
        if (ifaceLen >= sizeof (ifaceName)) {   // error if interface too long
            errno = EINVAL;
            return -1;
        }
        memcpy (ifaceName, ifacePtr, ifaceLen);
        ifaceName[ifaceLen] = '\0';
        ifacePtr = ifaceName;
        addrPtr++;

        if (tcp_address_t::resolve_interface (ifaceName, ipv6_) < 0) {
            return -1;
        }
    }
    else
    {
        ifaceName[0] = '\0';
        addrPtr = ifacePtr;
        ifacePtr = NULL;
    }

    // Save the IP address or hostname for later use, without any port number
    const char* portPtr = strrchr (addrPtr, ':');
    size_t addrLen = (portPtr ? portPtr - addrPtr : strlen (addrPtr) );
    if (addrLen >= sizeof (hostName))
    {
        errno = EINVAL;
        return -1;
    }
    strncpy(hostName, addrPtr, addrLen);
    hostName[addrLen] = '\0';
    
    // Finally, parse/resolve/validate the IP address/hostname and port number
    return tcp_address_t::resolve (addrPtr, local_, ipv6_);
}

int zmq::norm_address_t::to_string (std::string &addr_)
{
    std::string id, iface, addr;
    int rc = tcp_address_t::to_string (addr);
    if (rc < 0) {
        addr_.clear ();
        return -1;
    }
    addr.erase (0, 6);  // remove "tcp://" prefix

    if (localId != NORM_NODE_NONE && localId != NORM_NODE_ANY) {
        std::stringstream s;
        s << (uint32_t)localId << ",";
        id = s.str ();
    }

    if (ifaceName[0] != '\0') {
        iface = std::string (ifaceName) + ";";
    }

    addr_ = std::string ("norm://") + id + iface + addr;
    return 0;
}

uint16_t zmq::norm_address_t::getPortNumber() const
{
    if (address.generic.sa_family == AF_INET6)
        return ntohs (address.ipv6.sin6_port);

    return ntohs (address.ipv4.sin_port);
}

bool zmq::norm_address_t::isNormNodeId () const
{
    return (localId != NORM_NODE_NONE && localId != NORM_NODE_ANY);
}
