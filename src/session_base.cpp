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

#include "session_base.hpp"
#include "i_engine.hpp"
#include "err.hpp"
#include "pipe.hpp"
#include "likely.hpp"
#include "tcp_connecter.hpp"
#include "ipc_connecter.hpp"
#include "tipc_connecter.hpp"
#include "pgm_sender.hpp"
#include "pgm_receiver.hpp"
#include "address.hpp"
#include "norm_engine.hpp"

#include "ctx.hpp"
#include "req.hpp"

zmq::session_base_t *zmq::session_base_t::create (class io_thread_t *io_thread_,
    bool active_, class socket_base_t *socket_, const options_t &options_,
    address_t *addr_)
{
	
    session_base_t *s = NULL;
    switch (options_.type) {
    case ZMQ_REQ:
        s = new (std::nothrow) req_session_t (io_thread_, active_,
            socket_, options_, addr_);
        break;
    case ZMQ_DEALER:
    case ZMQ_REP:
    case ZMQ_ROUTER:
    case ZMQ_PUB:
    case ZMQ_XPUB:
    case ZMQ_SUB:
    case ZMQ_XSUB:
    case ZMQ_PUSH:
    case ZMQ_PULL:
    case ZMQ_PAIR:
    case ZMQ_STREAM:
        s = new (std::nothrow) session_base_t (io_thread_, active_,
            socket_, options_, addr_);
        break;
    default:
        errno = EINVAL;
        return NULL;
    }
    alloc_assert (s);
    return s;
}

zmq::session_base_t::session_base_t (class io_thread_t *io_thread_,
      bool active_, class socket_base_t *socket_, const options_t &options_,
      address_t *addr_) :
    own_t (io_thread_, options_),
    io_object_t (io_thread_),
    active (active_),
    pipe (NULL),
    zap_pipe (NULL),
    incomplete_in (false),
    pending (false),
    engine (NULL),
    socket (socket_),
    io_thread (io_thread_),
    has_linger_timer (false),
    addr (addr_)
{
}

zmq::session_base_t::~session_base_t ()
{
    zmq_assert (!pipe);
    zmq_assert (!zap_pipe);

    //  If there's still a pending linger timer, remove it.
    if (has_linger_timer) {
        cancel_timer (linger_timer_id);
        has_linger_timer = false;
    }

    //  Close the engine.
    if (engine)
        engine->terminate ();

    delete addr;
}

void zmq::session_base_t::attach_pipe (pipe_t *pipe_)
{
    zmq_assert (!is_terminating ());
    zmq_assert (!pipe);
    zmq_assert (pipe_);
    pipe = pipe_;
    pipe->set_event_sink (this);
}

int zmq::session_base_t::pull_msg (msg_t *msg_)
{
    if (!pipe || !pipe->read (msg_)) {
        errno = EAGAIN;
        return -1;
    }

    incomplete_in = msg_->flags () & msg_t::more ? true : false;

    return 0;
}

int zmq::session_base_t::push_msg (msg_t *msg_)
{
    if (pipe && pipe->write (msg_)) {
        int rc = msg_->init ();
        errno_assert (rc == 0);
        return 0;
    }

    errno = EAGAIN;
    return -1;
}

int zmq::session_base_t::read_zap_msg (msg_t *msg_)
{
    if (zap_pipe == NULL) {
        errno = ENOTCONN;
        return -1;
    }

    if (!zap_pipe->read (msg_)) {
        errno = EAGAIN;
        return -1;
    }

    return 0;
}

int zmq::session_base_t::write_zap_msg (msg_t *msg_)
{
    if (zap_pipe == NULL) {
        errno = ENOTCONN;
        return -1;
    }

    const bool ok = zap_pipe->write (msg_);
    zmq_assert (ok);

    if ((msg_->flags () & msg_t::more) == 0)
        zap_pipe->flush ();

    const int rc = msg_->init ();
    errno_assert (rc == 0);
    return 0;
}

void zmq::session_base_t::reset ()
{
}

void zmq::session_base_t::flush ()
{
    if (pipe)
        pipe->flush ();
}

void zmq::session_base_t::clean_pipes ()
{
    zmq_assert (pipe != NULL);

    //  Get rid of half-processed messages in the out pipe. Flush any
    //  unflushed messages upstream.
    pipe->rollback ();
    pipe->flush ();

    //  Remove any half-read message from the in pipe.
    while (incomplete_in) {
        msg_t msg;
        int rc = msg.init ();
        errno_assert (rc == 0);
        rc = pull_msg (&msg);
        errno_assert (rc == 0);
        rc = msg.close ();
        errno_assert (rc == 0);
    }
}

void zmq::session_base_t::pipe_terminated (pipe_t *pipe_)
{
    // Drop the reference to the deallocated pipe if required.
    zmq_assert (pipe_ == pipe
             || pipe_ == zap_pipe
             || terminating_pipes.count (pipe_) == 1);

    if (pipe_ == pipe)
        // If this is our current pipe, remove it
        pipe = NULL;
    else
    if (pipe_ == zap_pipe)
        zap_pipe = NULL;
    else
        // Remove the pipe from the detached pipes set
        terminating_pipes.erase (pipe_);

    if (!is_terminating () &&
        (options.raw_sock || socket->is_terminate_unpiped_session ())) {
        if (engine) {
            engine->terminate ();
            engine = NULL;
        }
        terminate ();
    }

    //  If we are waiting for pending messages to be sent, at this point
    //  we are sure that there will be no more messages and we can proceed
    //  with termination safely.
    if (pending && !pipe && !zap_pipe && terminating_pipes.empty ())
        proceed_with_term ();
}

void zmq::session_base_t::read_activated (pipe_t *pipe_)
{
    // Skip activating if we're detaching this pipe
    if (unlikely(pipe_ != pipe && pipe_ != zap_pipe)) {
        zmq_assert (terminating_pipes.count (pipe_) == 1);
        return;
    }

    if (unlikely (engine == NULL)) {
        pipe->check_read ();
        return;
    }

    if (likely (pipe_ == pipe))
        engine->restart_output ();
    else
        engine->zap_msg_available ();
}

void zmq::session_base_t::write_activated (pipe_t *pipe_)
{
    // Skip activating if we're detaching this pipe
    if (pipe != pipe_) {
        zmq_assert (terminating_pipes.count (pipe_) == 1);
        return;
    }

    if (engine)
        engine->restart_input ();
}

void zmq::session_base_t::hiccuped (pipe_t *)
{
    //  Hiccups are always sent from session to socket, not the other
    //  way round.
    zmq_assert (false);
}

zmq::socket_base_t *zmq::session_base_t::get_socket ()
{
    return socket;
}

void zmq::session_base_t::process_plug ()
{
    if (active)
        start_connecting (false);
    //poller->set_pollin (socket->mailbox_handle); // TODO get sockets handle??
    // socket->get_mailbox->signaler ??
}

int zmq::session_base_t::zap_connect ()
{
    zmq_assert (zap_pipe == NULL);

    endpoint_t peer = find_endpoint ("inproc://zeromq.zap.01");
    if (peer.socket == NULL) {
        errno = ECONNREFUSED;
        return -1;
    }
    if (peer.options.type != ZMQ_REP
    &&  peer.options.type != ZMQ_ROUTER) {
        errno = ECONNREFUSED;
        return -1;
    }

    //  Create a bi-directional pipe that will connect
    //  session with zap socket.
    object_t *parents [2] = {this, peer.socket};
    pipe_t *new_pipes [2] = {NULL, NULL};
    int hwms [2] = {0, 0};
    bool conflates [2] = {false, false};
    int rc = pipepair (parents, new_pipes, hwms, conflates);
    errno_assert (rc == 0);

    //  Attach local end of the pipe to this socket object.
    zap_pipe = new_pipes [0];
    zap_pipe->set_nodelay ();
    zap_pipe->set_event_sink (this);

    new_pipes [1]->set_nodelay ();
    send_bind (peer.socket, new_pipes [1], false);

    //  Send empty identity if required by the peer.
    if (peer.options.recv_identity) {
        msg_t id;
        rc = id.init ();
        errno_assert (rc == 0);
        id.set_flags (msg_t::identity);
        bool ok = zap_pipe->write (&id);
        zmq_assert (ok);
        zap_pipe->flush ();
    }

    return 0;
}

void zmq::session_base_t::process_attach (i_engine *engine_)
{
    zmq_assert (engine_ != NULL);

    //  Create the pipe if it does not exist yet.
    if (!pipe && !is_terminating ()) {
        object_t *parents [2] = {this, socket};
        pipe_t *pipes [2] = {NULL, NULL};

        bool conflate = options.conflate &&
            (options.type == ZMQ_DEALER ||
             options.type == ZMQ_PULL ||
             options.type == ZMQ_PUSH ||
             options.type == ZMQ_PUB ||
             options.type == ZMQ_SUB);

        int hwms [2] = {conflate? -1 : options.rcvhwm,
            conflate? -1 : options.sndhwm};
        bool conflates [2] = {conflate, conflate};
        int rc = pipepair (parents, pipes, hwms, conflates);
        errno_assert (rc == 0);

        //  Plug the local end of the pipe.
        pipes [0]->set_event_sink (this);

        //  Remember the local end of the pipe.
        zmq_assert (!pipe);
        pipe = pipes [0];

        //  Ask socket to plug into the remote end of the pipe.
        send_bind (socket, pipes [1]);
    }

    //  The norm engine logic doesn't cause this, so we must do it here
    if (addr && addr->protocol == "norm")
        pipe->check_read ();

    //  Plug in the engine.
    zmq_assert (!engine);
    engine = engine_;
    engine->plug (io_thread, this);
}

void zmq::session_base_t::engine_error ()
{
    //  Engine is dead. Let's forget about it.
    engine = NULL;

    //  Remove any half-done messages from the pipes.
    if (pipe)
        clean_pipes ();

    if (active)
        reconnect ();
    else
        terminate ();

    //  Just in case there's only a delimiter in the pipe.
    if (pipe)
        pipe->check_read ();

    if (zap_pipe)
        zap_pipe->check_read ();
}

void zmq::session_base_t::process_term (int linger_)
{
    zmq_assert (!pending);

    //  If the termination of the pipe happens before the term command is
    //  delivered there's nothing much to do. We can proceed with the
    //  standard termination immediately.
    if (!pipe && !zap_pipe) {
        proceed_with_term ();
        return;
    }

    pending = true;

    if (pipe != NULL) {
        //  If there's finite linger value, delay the termination.
        //  If linger is infinite (negative) we don't even have to set
        //  the timer.
        if (linger_ > 0) {
            zmq_assert (!has_linger_timer);
            add_timer (linger_, linger_timer_id);
            has_linger_timer = true;
        }

        //  Start pipe termination process. Delay the termination till all messages
        //  are processed in case the linger time is non-zero.
        pipe->terminate (linger_ != 0);

        //  TODO: Should this go into pipe_t::terminate ?
        //  In case there's no engine and there's only delimiter in the
        //  pipe it wouldn't be ever read. Thus we check for it explicitly.
        pipe->check_read ();
    }

    if (zap_pipe != NULL)
        zap_pipe->terminate (false);
}

void zmq::session_base_t::proceed_with_term ()
{
    //  The pending phase has just ended.
    pending = false;

    //  Continue with standard termination.
    own_t::process_term (0);
}

void zmq::session_base_t::timer_event (int id_)
{
    //  Linger period expired. We can proceed with termination even though
    //  there are still pending messages to be sent.
    zmq_assert (id_ == linger_timer_id);
    has_linger_timer = false;

    //  Ask pipe to terminate even though there may be pending messages in it.
    zmq_assert (pipe);
    pipe->terminate (false);
}

void zmq::session_base_t::reconnect ()
{
    //  For delayed connect situations, terminate the pipe
    //  and reestablish later on
    if (pipe && options.immediate == 1 
        && addr->protocol != "pgm" && addr->protocol != "epgm" 
        && addr->protocol != "norm") {
        pipe->hiccup ();
        pipe->terminate (false);
        terminating_pipes.insert (pipe);
        pipe = NULL;
    }

    reset ();

    //  Reconnect.
    if (options.reconnect_ivl != -1)
        start_connecting (true);

    //  For subscriber sockets we hiccup the inbound pipe, which will cause
    //  the socket object to resend all the subscriptions.
    if (pipe && (options.type == ZMQ_SUB || options.type == ZMQ_XSUB))
        pipe->hiccup ();
}

void zmq::session_base_t::start_connecting (bool wait_)
{
    zmq_assert (active);

    //  Choose I/O thread to run connecter in. Given that we are already
    //  running in an I/O thread, there must be at least one available.
    io_thread_t *connect_io_thread = choose_io_thread (options.affinity);
    zmq_assert (connect_io_thread);

    //  Create the connecter object.

    if (addr->protocol == "tcp") {
        tcp_connecter_t *connecter = new (std::nothrow) tcp_connecter_t (
            connect_io_thread, this, options, addr, wait_);
        alloc_assert (connecter);
        launch_child (connecter);
        return;
    }

#if !defined ZMQ_HAVE_WINDOWS && !defined ZMQ_HAVE_OPENVMS
    if (addr->protocol == "ipc") {
        ipc_connecter_t *connecter = new (std::nothrow) ipc_connecter_t (
            connect_io_thread, this, options, addr, wait_);
        alloc_assert (connecter);
        launch_child (connecter);
        return;
    }
#endif
#if defined ZMQ_HAVE_TIPC
    if (addr->protocol == "tipc") {
        tipc_connecter_t *connecter = new (std::nothrow) tipc_connecter_t (
            connect_io_thread, this, options, addr, wait_);
        alloc_assert (connecter);
        launch_child (connecter);
        return;
    }
#endif

#ifdef ZMQ_HAVE_OPENPGM

    //  Both PGM and EPGM transports are using the same infrastructure.
    if (addr->protocol == "pgm" || addr->protocol == "epgm") {

        zmq_assert (options.type == ZMQ_PUB || options.type == ZMQ_XPUB
                 || options.type == ZMQ_SUB || options.type == ZMQ_XSUB);

        //  For EPGM transport with UDP encapsulation of PGM is used.
        bool const udp_encapsulation = addr->protocol == "epgm";

        //  At this point we'll create message pipes to the session straight
        //  away. There's no point in delaying it as no concept of 'connect'
        //  exists with PGM anyway.
        if (options.type == ZMQ_PUB || options.type == ZMQ_XPUB) {

            //  PGM sender.
            pgm_sender_t *pgm_sender = new (std::nothrow) pgm_sender_t (
                connect_io_thread, options);
            alloc_assert (pgm_sender);

            int rc = pgm_sender->init (udp_encapsulation, addr->address.c_str ());
            errno_assert (rc == 0);

            send_attach (this, pgm_sender);
        }
        else {

            //  PGM receiver.
            pgm_receiver_t *pgm_receiver = new (std::nothrow) pgm_receiver_t (
                connect_io_thread, options);
            alloc_assert (pgm_receiver);

            int rc = pgm_receiver->init (udp_encapsulation, addr->address.c_str ());
            errno_assert (rc == 0);

            send_attach (this, pgm_receiver);
        }

        return;
    }
#endif
    
#ifdef ZMQ_HAVE_NORM
    if (addr->protocol == "norm")
    {
        //  For norm, we'll create message pipes to the session straight away.
        //  There's no point in delaying it, as no concept of 'connect' exists
        //  with NORM in pub/sub, and for other models, we need to talk through
        //  those pipes to the norm engine & instance to open connections.
        if (options.type == ZMQ_PUB || options.type == ZMQ_XPUB) {

            //  NORM sender.
            norm_engine_t* norm_sender =
                new (std::nothrow) norm_engine_t(socket, options);
            alloc_assert (norm_sender);

            int rc = norm_sender->init (addr->address.c_str (), true, false);
            errno_assert (rc == 0);

            send_attach (this, norm_sender);
        }
        else if (options.type == ZMQ_SUB || options.type == ZMQ_XSUB) { 

            //  NORM receiver.
            norm_engine_t* norm_receiver =
                new (std::nothrow) norm_engine_t (socket, options);
            alloc_assert (norm_receiver);

            int rc = norm_receiver->init (addr->address.c_str (), false, true);
            errno_assert (rc == 0);

            send_attach (this, norm_receiver);
        }
        else {

            //  NORM bi-directional.
            norm_engine_t* norm_tranceiver =
                new (std::nothrow) norm_engine_t (socket, options,
                                                  addr->address.c_str (),
                                                  true, true);
            alloc_assert (norm_tranceiver);

            send_attach (this, norm_tranceiver);
        }
        return;
    }
#endif // ZMQ_HAVE_NORM

    zmq_assert (false);
}

