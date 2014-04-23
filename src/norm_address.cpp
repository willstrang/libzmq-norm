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

#if defined ZMQ_HAVE_NORM

zmq::norm_address_t::norm_address_t ()
    : localId(NORM_NODE_ANY)
{
    memset (&address, 0, sizeof (address));
    ifaceName[0] = '\0';
    hostName[0] = '\0';
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

int zmq::norm_address_t::to_string_raw (std::string &addr_)
{
    std::string id, iface, host, port;
    host = std::string (hostName);

    std::stringstream s1;
    s1 << ":" << (uint32_t)getPortNumber ();
    port = s1.str ();

    if (localId != NORM_NODE_NONE && localId != NORM_NODE_ANY) {
        std::stringstream s2;
        s2 << (uint32_t)localId << ",";
        id = s2.str ();
    }

    if (ifaceName[0] != '\0') {
        iface = std::string (ifaceName) + ";";
    }

    addr_ = std::string ("norm://") + id + iface + host + port;
    // There should at least be a non-empty host name - compose address anyway
    return (hostName[0] == '\0' ? -1 : 0);
}

// returns true if valid address was set, else false
bool zmq::norm_address_t::setTCPAddress(const void *src,
                                        socklen_t src_len,
                                        sa_family_t family)
{
    memset (&address, 0, sizeof (address));
    if (!src)
        return false;

    if (family == AF_INET &&
        src_len >= (socklen_t) sizeof (address.ipv4.sin_addr)) {
        address.ipv4.sin_family = AF_INET;
        memcpy(&address.ipv4.sin_addr, src, sizeof (address.ipv4.sin_addr));
    }
    else if (family == AF_INET6 &&
             src_len >= (socklen_t) sizeof (address.ipv6.sin6_addr)) {
        address.ipv6.sin6_family = AF_INET6;
        memcpy(&address.ipv6.sin6_addr, src, sizeof (address.ipv6.sin6_addr));
    }
    else
        return false;

    return true;
}

// returns true if valid address was returned, else false
bool zmq::norm_address_t::getTCPAddress(void *dst,
                                        socklen_t &dst_len,
                                        sa_family_t &family)
{
    if (!dst)
        return false;

    if (address.generic.sa_family == AF_INET6) {
        family = address.ipv6.sin6_family;
        dst_len = (socklen_t) sizeof (address.ipv6.sin6_addr);
        memcpy(dst, &address.ipv6.sin6_addr, sizeof (address.ipv6.sin6_addr));
    }
    else if (address.generic.sa_family == AF_INET) {
        family = address.ipv4.sin_family;
        dst_len = (socklen_t) sizeof (address.ipv4.sin_addr);
        memcpy(dst, &address.ipv4.sin_addr, sizeof (address.ipv4.sin_addr));
    }
    else
        return false;

    return true;
}

uint16_t zmq::norm_address_t::getPortNumber () const
{
    if (address.generic.sa_family == AF_INET6)
        return ntohs (address.ipv6.sin6_port);

    return ntohs (address.ipv4.sin_port);
}

void zmq::norm_address_t::setPortNumber (uint16_t port_)
{
    if (address.generic.sa_family == AF_INET6)
        address.ipv6.sin6_port = htons(port_);
    else
        address.ipv4.sin_port = htons(port_);
}

void zmq::norm_address_t::setNormNodeId (NormNodeId normNodeId_)
{
    localId = normNodeId_;
}


bool zmq::norm_address_t::isNormNodeId () const
{
    return (localId != NORM_NODE_NONE && localId != NORM_NODE_ANY);
}

int zmq::norm_address_t::setIfaceName (const char *ifaceName_)
{
    size_t ifaceLen = 0;
    if (ifaceName_) ifaceLen = strlen(ifaceName_);
    if (ifaceLen >= sizeof (ifaceName)) {   // error if interface too long
        errno = EINVAL;
        return -1;
    }
    if (ifaceLen > 0)
        memcpy (ifaceName, ifaceName_, ifaceLen);
    ifaceName[ifaceLen] = '\0';
    return 0;
}

int zmq::norm_address_t::setRawHostName (const char *hostName_)
{
    size_t hostLen = 0;
    if (hostName_) hostLen = strlen(hostName_);
    if (hostLen >= sizeof (hostName)) {   // error if hostname too long
        errno = EINVAL;
        return -1;
    }
    if (hostLen > 0)
        memcpy (hostName, hostName_, hostLen);
    hostName[hostLen] = '\0';
    return 0;
}

int zmq::norm_address_t::getEventAddr(NormEvent &event,
                                       char *pAddrStr,
                                       UINT16 &port)
{
    char IPaddr[16]; // big enough for IPv6
    unsigned int addrLen = sizeof (IPaddr);
    NormNodeGetAddress(event.sender, IPaddr, &addrLen, &port);
    int addrFamily;
    if (4 == addrLen)
        addrFamily = AF_INET;
    else
        addrFamily = AF_INET6;
    pAddrStr[63] = '\0';
    inet_ntop(addrFamily, IPaddr, pAddrStr, 63);

    return addrFamily;
}

#endif  //  ZMQ_HAVE_NORM
