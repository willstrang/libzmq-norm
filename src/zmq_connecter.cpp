/*
    Copyright (c) 2007-2009 FastMQ Inc.

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the Lesser GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Lesser GNU General Public License for more details.

    You should have received a copy of the Lesser GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "zmq_connecter.hpp"
#include "zmq_init.hpp"
#include "io_thread.hpp"
#include "err.hpp"

zmq::zmq_connecter_t::zmq_connecter_t (io_thread_t *parent_, object_t *owner_,
      const options_t &options_) :
    owned_t (parent_, owner_),
    io_object_t (parent_),
    handle_valid (false),
    options (options_)
{
}

zmq::zmq_connecter_t::~zmq_connecter_t ()
{
}

int zmq::zmq_connecter_t::set_address (const char *addr_)
{
     return tcp_connecter.set_address (addr_);
}

void zmq::zmq_connecter_t::process_plug ()
{
    start_connecting ();
    owned_t::process_plug ();
}

void zmq::zmq_connecter_t::process_unplug ()
{
    if (handle_valid)
        rm_fd (handle);
}

void zmq::zmq_connecter_t::in_event ()
{
    //  We are not polling for incomming data, so we are actually called
    //  because of error here. However, we can get error on out event as well
    //  on some platforms, so we'll simply handle both events in the same way.
    out_event ();
}

void zmq::zmq_connecter_t::out_event ()
{
    fd_t fd = tcp_connecter.connect ();
    rm_fd (handle);
    handle_valid = false;

    //  If there was error during the connecting, close the socket and wait
    //  for a while before trying to reconnect.
    if (fd == retired_fd) {
        tcp_connecter.close ();
        add_timer ();
        return;
    }

    //  Create an init object. 
    io_thread_t *io_thread = choose_io_thread (options.affinity);
    zmq_init_t *init = new zmq_init_t (io_thread, owner, fd, true, options);
    zmq_assert (init);
    send_plug (init);
    send_own (owner, init);

    //  Ask owner socket to shut the connecter down.
    term ();
}

void zmq::zmq_connecter_t::timer_event ()
{
    //  Reconnect period have elapsed.
    start_connecting ();
}

void zmq::zmq_connecter_t::start_connecting ()
{
    //  Open the connecting socket.
    int rc = tcp_connecter.open ();

    //  Connect may succeed in synchronous manner.
    if (rc == 0) {
        out_event ();
        return;
    }

    //  Connection establishment may be dealyed. Poll for its completion.
    else if (rc == -1 && errno == EINPROGRESS) {
        handle = add_fd (tcp_connecter.get_fd ());
        handle_valid = true;
        set_pollout (handle);
        return;
    }

    //  If none of the above is true, synchronous error occured.
    //  Wait for a while and retry.
    add_timer ();
}
