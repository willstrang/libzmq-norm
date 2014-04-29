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

#ifndef __ZMQ_NORM_ADDRESS_HPP_INCLUDED__
#define __ZMQ_NORM_ADDRESS_HPP_INCLUDED__

#include "platform.hpp"

#if defined ZMQ_HAVE_NORM

// #define ZMQ_DEBUG_NORM
// #define ZMQ_DEBUG_NORM_2

#include <string> // tcp_address.hpp needs this
#include "tcp_address.hpp"

#include <normApi.h>

#if defined ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

namespace zmq
{

    class norm_address_t : public tcp_address_t
    {
    public:

        norm_address_t ();
        norm_address_t (const sockaddr *sa, socklen_t sa_len);
        virtual ~norm_address_t ();

        //  This function translates a textual NORM-extended TCP address or
        //  local endpoint into an address structure.
        //  The norm endpoint/address format is: [id,][<iface>;]<addr>:<port>
        //  If 'local' is true, names are resolved as local interface names.
        //  If it is false, names are resolved as remote hostnames.
        //  If 'ipv6' is true, the name may resolve to IPv6 address.
        int resolve (const char *name_, bool local_, bool ipv6_);

        //  The opposite to resolve ()
        virtual int to_string (std::string &addr_);

        //  Does like to_string (), but uses the raw hostname/IP address,
        //  so this should return exactly the values passed into resolve ().
        //  Using the 4 set* APIs here, you can even compose norm addresses
        //  and get them out with to_string_raw (), without resolving them.
        int to_string_raw (std::string &addr_);

        NormNodeId getNormNodeId () const { return localId; }
        void setNormNodeId (NormNodeId normNodeId_);
        bool isNormNodeId () const;

        const char *getIfaceName () const { return ifaceName; }
        int setIfaceName (const char *ifaceName_);
        bool isIfaceName () const { return (ifaceName[0] != '\0'); }

        // returns true if valid address was returned, else false
        bool getTCPAddress(void *dst,
                           socklen_t &dst_len,
                           sa_family_t &family);
        // returns true if valid address was set, else false
        bool setTCPAddress(const void *src,
                           socklen_t src_len,
                           sa_family_t family);

        uint16_t getPortNumber () const;
        void setPortNumber (uint16_t port_);

        // Get raw hostname/IP address last passed into resolve ()
        const char *getRawHostName () const { return hostName; }
        int setRawHostName (const char *hostName_);
        bool isRawHostName () const { return (hostName[0] != '\0'); }

        // return  value is the address family
        static int getEventAddr(NormEvent &event, char *pAddrStr, UINT16 &port);

    protected:

        // tcp_address  address; // we inherit address from tcp_address

        // 2 Fields added for NORM
        NormNodeId   localId;
        char         ifaceName[256];
        char         hostName[256];
    };

}

#endif

#endif
