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

#ifndef __ZMQ_STREAM_ENGINE_HPP_INCLUDED__
#define __ZMQ_STREAM_ENGINE_HPP_INCLUDED__

#include <stddef.h>

#include "fd.hpp"
#include "stream_object.hpp"
#include "options.hpp"
#include "socket_base.hpp"
#include "ip.hpp"
#include "devpoll.hpp"
#include "../include/zmq.h"

namespace zmq
{

    //  This engine handles any socket with SOCK_STREAM semantics,
    //  e.g. TCP socket or an UNIX domain socket.

    class stream_engine_t : public stream_object_t
    {
    public:

        stream_engine_t (fd_t fd_, const options_t &options_, 
                         const std::string &endpoint);
        ~stream_engine_t ();

    private:

        //  Writes data to the socket. Returns the number of bytes actually
        //  written (even zero is to be considered to be a success). In case
        //  of error or orderly shutdown by the other peer -1 is returned.
        int write (const void *data_, size_t size_);

        //  Reads data from the socket (up to 'size' bytes).
        //  Returns the number of bytes actually read or -1 on error.
        //  Zero indicates the peer has closed the connection.
        int read (void *data_, size_t size_);

        void engine_add_fd ();
        void engine_rm_fd ();
        void engine_set_pollin ();
        void engine_reset_pollin ();
        void engine_set_pollout ();
        void engine_reset_pollout ();
        fd_t engine_get_fd ();
        int engine_in_event ();
        int engine_out_event ();

        //  Underlying socket.
        fd_t s;

        handle_t handle;

        // String representation of endpoint
        /// std::string endpoint;

        stream_engine_t (const stream_engine_t&);
        const stream_engine_t &operator = (const stream_engine_t&);
    };

}

#endif
