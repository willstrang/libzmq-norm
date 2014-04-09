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

#include <new>

#include <string>
#include <stdio.h>

#include "platform.hpp"

#if defined ZMQ_HAVE_NORM

#include "norm_engine.hpp"
#include "norm_listener.hpp"
#include "io_thread.hpp"
#include "session_base.hpp"
#include "config.hpp"
#include "err.hpp"
#include "ip.hpp"
#include "tcp.hpp"
#include "socket_base.hpp"

#ifdef ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#else
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#endif

#ifdef ZMQ_HAVE_OPENVMS
#include <ioctl.h>
#endif

#if defined ZMQ_DEBUG_NORM
#include <iostream>
#endif

zmq::norm_listener_t::norm_listener_t (io_thread_t *io_thread_,
      socket_base_t *socket_, const options_t &options_) :
    own_t (io_thread_, options_),
    io_object_t (io_thread_),
    s (retired_fd),
    socket (socket_),  
    listen_instance(NORM_INSTANCE_INVALID),
    listen_session(NORM_SESSION_INVALID)
{
#ifdef ZMQ_DEBUG_NORM
    is_twoway = true;
#endif
}

zmq::norm_listener_t::~norm_listener_t ()
{
    zmq_assert (s == retired_fd);
    shutdown();  // in case it was not already called
}

void zmq::norm_listener_t::shutdown()
{
    if (NORM_SESSION_INVALID != listen_session)
    {
        NormStopReceiver(listen_session);
        NormDestroySession(listen_session);
        listen_session = NORM_SESSION_INVALID;
    }
    if (NORM_INSTANCE_INVALID != listen_instance)
    {
        NormStopInstance(listen_instance);
        NormDestroyInstance(listen_instance);
        listen_instance = NORM_INSTANCE_INVALID;
    }
}  // end zmq::norm_listener_t::shutdown()

void zmq::norm_listener_t::process_plug ()
{
    //  Start waiting for incoming connections.
    handle = add_fd(s);
    // Set POLLIN for notification of pending NormEvents
    set_pollin (handle);
}

void zmq::norm_listener_t::process_term (int linger_)
{
    rm_fd (handle);
    close ();
    own_t::process_term (linger_);
}

void zmq::norm_listener_t::in_event ()
{
#ifdef UNUSED
    //  If connection was reset by the peer in the meantime, just ignore it.
    //  TODO: Handle specific errors like ENFILE/EMFILE etc.
    if (fd == retired_fd) {
        socket->event_accept_failed (endpoint, zmq_errno());
        return;
    }
#endif

    // A NormEvent is probably pending, so call NormGetNextEvent () and handle
    NormEvent event;
    while (NormGetNextEvent(listen_instance, &event, false)) {

        switch(event.type) {

#ifdef NORM_REMOTE_SENDER_RESET
        case NORM_REMOTE_SENDER_RESET:
#endif
        case NORM_REMOTE_SENDER_NEW:
        {
            norm_address_t client_address;
            char IPaddr[16]; // big enough for IPv6
            unsigned int addrLen = sizeof (IPaddr);
            UINT16 clientPort;
            bool worked;
            NormNodeGetAddress(event.sender, IPaddr, &addrLen, &clientPort);
            int addrFamily;
            if (4 == addrLen)
                addrFamily = AF_INET;
            else
                addrFamily = AF_INET6;
            char clientAddr[64];
            clientAddr[63] = '\0';
            inet_ntop(addrFamily, IPaddr, clientAddr, 63);
            worked = client_address.setTCPAddress(IPaddr, addrLen, addrFamily);
            if (!worked) {
                socket->event_accept_failed (endpoint, errno);
                return;
            }
            client_address.setPortNumber (clientPort);
             // client_address.setIfaceName ();  // TODO
            client_address.setNormNodeId (NormNodeGetId (event.sender));
            client_address.setRawHostName (clientAddr);

            if (listen_session == event.session) {
                // Only unicast addresses are allowed to "connect" via listener
                if (!NormIsUnicastAddress (client_address.getRawHostName ())) {
                    socket->event_accept_failed (endpoint, errno);
                    return;
                }

                NormInstanceHandle client_instance = NormCreateInstance();
                if (NORM_INSTANCE_INVALID == client_instance) {
                    // errno set by whatever caused NormCreateInstance to fail
                    socket->event_accept_failed (endpoint, errno);
                    return;
                }

                // TBD - should confirm we don't already have session for
                // given clientAddr/clientPort (could happen in case of a
                // race condition as mentioned above)
                NormSessionHandle client_session =
                    NormCreateSession (client_instance, "127.0.0.1",
                                       listen_address.getPortNumber (),
                                       listen_address.getNormNodeId ());
                if (NORM_SESSION_INVALID == client_session) {
                    // errno set by whatever caused NormCreateInstance to fail
                    socket->event_accept_failed (endpoint, errno);
                    NormDestroyInstance (client_instance);
                    return;
                }

                // TODO - set rxBindAddress in NormSetRxPortReuse call if need
                // to force use of a specific NIC (specified by IP address)
                NormSetRxPortReuse (client_session, true, 0,
                                    clientAddr, clientPort);
                NormSetDefaultUnicastNack (client_session, true);
                UINT32 normRxBufferSize = 128*1024;
                if (options.rcvbuf != 0) normRxBufferSize = options.rcvbuf;
                UINT32 normTxBufferSize = 128*1024;
                if (options.sndbuf != 0) normTxBufferSize = options.sndbuf;
                NormStartReceiver (client_session, normRxBufferSize);
                // Any new packets will come to our new connected session
                // instead. However, note that even though we've "connected"
                // this sender, there is a chance that additional packets in
                // the "listen_session" rx socket buffer may look like a new
                // sender if deleted now, so we wait for
                // NORM_REMOTE_SENDER_INACTIVE to delete
                NormChangeDestination (client_session, clientAddr, clientPort);
                NormStartSender (client_session, session_id++,
                                 normTxBufferSize, 1400, 16, 4);
#ifdef ZMQ_DEBUG_NORM
                std::cout << "server connection from client " << clientAddr
                          << ":" << clientPort << std::endl << std::flush;
#endif

                fd_t fd = NormGetDescriptor(client_instance);
                // remember our fd for ZMQ_SRCFD in messages
                socket->set_fd(fd);

                //  Choose I/O thread to run engine in.
                io_thread_t *io_thread = choose_io_thread (options.affinity);
                zmq_assert (io_thread);

                //  Create the engine object for this connection.
                norm_engine_t *engine = new (std::nothrow)
                    norm_engine_t (io_thread, options);
                alloc_assert (engine);
                int rc = engine->init (client_instance, client_session,
                                       event.sender, client_address,
                                       listen_address);
                if (rc < 0) {
                    // errno set by whatever caused init to fail
                    socket->event_accept_failed (endpoint, errno);
                    NormDestroySession (client_session);
                    NormDestroyInstance (client_instance);
                    return;
                }

                //  Create and launch a session object.
                session_base_t *session =
                    session_base_t::create (io_thread, false, socket,
                                            options, NULL);
                errno_assert (session);
                session->inc_seqnum ();
                launch_child (session);
                send_attach (session, engine, false);
                socket->event_accepted (endpoint, fd);
            }
#ifdef NORM_REMOTE_SENDER_RESET
            else if (NORM_REMOTE_SENDER_RESET == event.type) {
                std::cout << "server reconnected to client " << clientAddr
                          << ":" << clientPort << std::endl << std::flush;
            }
#endif
            break;
        }

        case NORM_REMOTE_SENDER_INACTIVE:
            // Here we free resources used for this formerly active sender.
            // Note that w/ NORM_SYNC_STREAM, if sender reactivates, we may
            // get some messages delivered twice.  NORM_SYNC_CURRENT would
            // mitigate that but might miss data at startup. Always tradeoffs.
            // Instead of immediately deleting, we could instead initiate a
            // user configurable timeout here to wait some amount of time
            // after this event to declare the remote sender truly dead
            // and delete its state???
            if (listen_session == event.session) {
                NormNodeDelete(event.sender);
            }
            break;
            
        case NORM_RX_OBJECT_NEW:
        case NORM_RX_OBJECT_UPDATED:
#ifdef ZMQ_DEBUG_NORM
            recv_data(event.object);
            if (listen_session == event.session) {
                std::cout
                    << (event.type == NORM_RX_OBJECT_NEW ? "new" : "updated")
                    << " data on listen_session?!?!\n"
                    << std::endl << std::flush;
            }
            else {
                std::cout << "recv'd data from client ...\n"
                          << std::endl << std::flush;
            }
#endif
            break;

        default:
            // We ignore all other NORM events 
#ifdef ZMQ_DEBUG_NORM
            std::cout << "listen unhandled event: " << event.type << " "
                      << std::endl << std::flush;
#endif
            break;
        }  // end switch(event.type)
    }  // while...
}  // zmq::norm_listener_t::in_event()

#ifdef UNUSED
void zmq::norm_listener_t::start_connection (...)
{
    // remember our fd for ZMQ_SRCFD in messages
    // socket->set_fd(fd);

    //  Create the engine object for this connection.
    stream_engine_t *engine = new (std::nothrow)
        norm_engine_t (fd, options, endpoint);
    alloc_assert (engine);

    //  Choose I/O thread to run connecter in. Given that we are already
    //  running in an I/O thread, there must be at least one available.
    io_thread_t *io_thread = choose_io_thread (options.affinity);
    zmq_assert (io_thread);

    //  Create and launch a session object.
    session_base_t *session = session_base_t::create (io_thread, false,
                                                      socket, options, NULL);
    errno_assert (session);
    session->inc_seqnum ();
    launch_child (session);
    send_attach (session, engine, false);
    socket->event_accepted (endpoint, fd);
}
#endif

void zmq::norm_listener_t::close ()
{
    zmq_assert (s != retired_fd);
    shutdown ();
    socket->event_closed (endpoint, s);
    s = retired_fd;
}

int zmq::norm_listener_t::get_address (std::string &addr_)
{
    // Get the details of the TCP socket
    struct sockaddr_storage ss;
#ifdef ZMQ_HAVE_HPUX
    int sl = sizeof (ss);
#else
    socklen_t sl = sizeof (ss);
#endif
    int rc = getsockname (s, (struct sockaddr *) &ss, &sl);

    if (rc != 0) {
        addr_.clear ();
        return rc;
    }

    norm_address_t addr ((struct sockaddr *) &ss, sl);
    // assume the NormNodeId and IfaceName in listen_address still hold
    addr.setNormNodeId (listen_address.getNormNodeId ());
    addr.setIfaceName (listen_address.getIfaceName ());
    return addr.to_string (addr_);
}

int zmq::norm_listener_t::set_address (const char *addr_)
{
    //  Convert the textual address into norm address structure.
    int rc = listen_address.resolve (addr_, true, options.ipv6);
    if (rc != 0)
        return -1;

    if (NORM_INSTANCE_INVALID == listen_instance) {
        listen_instance = NormCreateInstance ();
        if (NORM_INSTANCE_INVALID == listen_instance) {
            // errno set by whatever caused NormCreateInstance() to fail
            return -1;
        }
    }

    listen_session = NormCreateSession (listen_instance, "127.0.0.1",
                                        listen_address.getPortNumber (),
                                        listen_address.getNormNodeId ());
    if (NORM_SESSION_INVALID == listen_session) {
        int savedErrno = errno;
        NormDestroyInstance(listen_instance);
        listen_instance = NORM_INSTANCE_INVALID;
        errno = savedErrno;
        return -1;
    }

    // If addr_ didn't specify a nodeId, then get what NormCreateSession chose
    listen_address.setNormNodeId (NormGetLocalNodeId (listen_session));

    //  Allow reusing the port.
    NormSetRxPortReuse(listen_session, true);  

    UINT32 normListenBufferSize = 2*1024;

    //  Listen for incomming connections.
    if (!NormStartReceiver(listen_session, normListenBufferSize)) {
        int savedErrno = errno;   // errno was set by whatever failed
        NormDestroyInstance(listen_instance);  // will also destroy the session
        listen_instance = NORM_INSTANCE_INVALID;
        errno = savedErrno;
        return -1;
    }

    s = NormGetDescriptor(listen_instance);

#if defined ZMQ_DEBUG_NORM
    NormSetMessageTrace(listen_session, true);
    NormSetDebugLevel(3);
    char logfilename[32];
    sprintf(logfilename, "normLog_%u.txt",
            (unsigned) listen_address.getNormNodeId ());
   bool worked =  NormOpenDebugLog(listen_instance, logfilename);
    std::string serverStr;
    listen_address.to_string_raw (serverStr);
    std::cout << "debug log " << logfilename << " to listen on "
              << serverStr << " fd: " << s 
              << (worked ? "" : " FAILED") << std::endl << std::flush;
#endif

#ifdef UNUSED
    // Set the IP Type-Of-Service for the underlying socket
    if (options.tos != 0)
        set_ip_type_of_service (s, options.tos);
#endif

    // start our ascending sessionId range with a random one
    session_id = NormGetRandomSessionId();

    listen_address.to_string (endpoint);

    socket->event_listening (endpoint, s);
    return 0;
}

#ifdef UNUSED
zmq::fd_t zmq::norm_listener_t::accept ()
{
    //  The situation where connection cannot be accepted due to insufficient
    //  resources is considered valid and treated by ignoring the connection.
    //  Accept one connection and deal with different failure modes.
    zmq_assert (s != retired_fd);

    struct sockaddr_storage ss;
    memset (&ss, 0, sizeof (ss));
#ifdef ZMQ_HAVE_HPUX
    int ss_len = sizeof (ss);
#else
    socklen_t ss_len = sizeof (ss);
#endif
    fd_t sock = ::accept (s, (struct sockaddr *) &ss, &ss_len);

#ifdef ZMQ_HAVE_WINDOWS
    if (sock == INVALID_SOCKET) {
        wsa_assert (WSAGetLastError () == WSAEWOULDBLOCK ||
            WSAGetLastError () == WSAECONNRESET ||
            WSAGetLastError () == WSAEMFILE ||
            WSAGetLastError () == WSAENOBUFS);
        return retired_fd;
    }
#if !defined _WIN32_WCE
    //  On Windows, preventing sockets from being inherited by child processes.
    BOOL brc = SetHandleInformation ((HANDLE) sock, HANDLE_FLAG_INHERIT, 0);
    win_assert (brc);
#endif
#else
    if (sock == -1) {
        errno_assert (errno == EAGAIN || errno == EWOULDBLOCK ||
            errno == EINTR || errno == ECONNABORTED || errno == EPROTO ||
            errno == ENOBUFS || errno == ENOMEM || errno == EMFILE ||
            errno == ENFILE);
        return retired_fd;
    }
#endif

    if (!options.tcp_accept_filters.empty ()) {
        bool matched = false;
        for (options_t::tcp_accept_filters_t::size_type i = 0; i != options.tcp_accept_filters.size (); ++i) {
            if (options.tcp_accept_filters[i].match_address ((struct sockaddr *) &ss, ss_len)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
#ifdef ZMQ_HAVE_WINDOWS
            int rc = closesocket (sock);
            wsa_assert (rc != SOCKET_ERROR);
#else
            int rc = ::close (sock);
            errno_assert (rc == 0 || errno == EINTR);
#endif
            return retired_fd;
        }
    }

    // Set the IP Type-Of-Service priority for this client socket
    if (options.tos != 0)
        set_ip_type_of_service (sock, options.tos);

    return sock;
}
#endif

#ifdef ZMQ_DEBUG_NORM
void zmq::norm_listener_t::recv_data(NormObjectHandle object)
{
    if (NORM_OBJECT_INVALID != object)
    {
        // Call result of NORM_RX_OBJECT_UPDATED notification
        // This is a rx_ready indication for a new or existing rx stream
        // First, determine if this is a stream we already know
        zmq_assert(NORM_OBJECT_STREAM == NormObjectGetType(object));
        // Since there can be multiple senders (publishers), we keep
        // state for each separate rx stream.
        NormRxStreamState* rxState =
            (NormRxStreamState*)NormObjectGetUserData(object);
        if (NULL == rxState)
        {
            if (is_twoway) {
                
            }
            // This is a new stream, so create rxState with zmq decoder, etc
            rxState = new NormRxStreamState(object, options.maxmsgsize);
            if (!rxState->Init())
            {
                errno_assert(false);
                delete rxState;
                return;
            }
            NormObjectSetUserData(object, rxState);
        }
        else if (!rxState->IsRxReady())
        {
            // Existing non-ready stream, so remove from pending
            // list to be promoted to rx_ready_list ...
            rx_pending_list.Remove(*rxState);
        }
        if (!rxState->IsRxReady())
        {
            // TBD - prepend up front for immediate service?
            rxState->SetRxReady(true);
            rx_ready_list.Append(*rxState);
        }
    }
    // This loop repeats until we've read all data available from "rx ready"
    // inbound streams and pushed any accumulated messages we can up to the
    // zmq session.
    while (!rx_ready_list.IsEmpty() |
           (zmq_input_ready && !msg_ready_list.IsEmpty()))
    {
        // Iterate through our rx_ready streams, reading data into the decoder
        // (This services incoming "rx ready" streams in a round-robin fashion)
        NormRxStreamState::List::Iterator iterator(rx_ready_list);
        NormRxStreamState* rxState;
        while (NULL != (rxState = iterator.GetNextItem()))
        {
            switch(rxState->Decode())
            {
                case 1:  // msg completed   
                    // Complete message decoded, move this stream to
                    // msg_ready_list to push the message up to the session
                    // below.  Note the stream will be returned to the
                    // "rx_ready_list" after that's done
                    rx_ready_list.Remove(*rxState);
                    msg_ready_list.Append(*rxState);
                    continue;
                    
                case -1: // decoding error (shouldn't happen w/ NORM, but ...)
                    // We need to re-sync this stream (decoder buffer was reset)
                    rxState->SetSync(false);
                    break;
                    
                default:  // 0 - need more data
                    break;
            }
            // Get more data from this stream
            NormObjectHandle stream = rxState->GetStreamHandle();
            // First, make sure we're in sync ...
            while (!rxState->InSync())
            {
                // seek NORM message start
                if (!NormStreamSeekMsgStart(stream))
                {
                    // Need to wait for more data
                    break;
                }
                // read message 'flag' byte to see if this it's a 'final' frame
                char syncFlag;
                unsigned int numBytes = 1;
                if (!NormStreamRead(stream, &syncFlag, &numBytes))
                {
                    // broken stream (shouldn't happen after seek msg start?)
                    zmq_assert(false);
                    continue;
                }
                if (0 == numBytes)
                {
                    // This probably shouldn't happen either since we found
                    // msg start. Need to wait for more data
                    break;
                }
                if (0 == syncFlag) rxState->SetSync(true);
                // else keep seeking ...
            }  // end while(!rxState->InSync())
            if (!rxState->InSync())
            {
                // Need more data for this stream, so remove from "rx ready"
                // list and iterate to next "rx ready" stream
                rxState->SetRxReady(false);
                // Move from rx_ready_list to rx_pending_list
                rx_ready_list.Remove(*rxState);
                rx_pending_list.Append(*rxState);
                continue;
            }
            // Now we're actually ready to read data from the NORM stream to
            // the zmq_decoder. The underlying zmq_decoder->get_buffer() call
            // sets how much is needed.
            unsigned int numBytes = rxState->GetBytesNeeded();
            if (!NormStreamRead(stream, rxState->AccessBuffer(), &numBytes))
            {
                // broken NORM stream, so re-sync
                rxState->Init();  // TBD - check result
                // This will retry syncing, and getting data from this stream
                // since we don't increment the "it" iterator
                continue;
            }
            rxState->IncrementBufferCount(numBytes);
            if (0 == numBytes)
            {
                // All the data available has been read
                // Need to wait for NORM_RX_OBJECT_UPDATED for this stream
                rxState->SetRxReady(false);
                // Move from rx_ready_list to rx_pending_list
                rx_ready_list.Remove(*rxState);
                rx_pending_list.Append(*rxState);
            }
        }  // end while(NULL != (rxState = iterator.GetNextItem()))
        
        if (zmq_input_ready)
        {
            // At this point, we've made a pass through the "rx_ready" stream
            // list. Now make a pass through the "msg_pending" list (if the
            // zmq session is ready for more input).  This may possibly return
            // streams back to the "rx ready" stream list after their pending
            // message is handled
            NormRxStreamState::List::Iterator iterator(msg_ready_list);
            NormRxStreamState* rxState;
            while (NULL != (rxState = iterator.GetNextItem()))
            {
                msg_t* msg = rxState->AccessMsg();
                std::string msgdata ((const char *)msg->data (), msg->size ());
                std::cout << "received client data: " << msgdata
                          << std::endl << std::flush;
                delete msg;
                // act like message was accepted.
                msg_ready_list.Remove(*rxState);
                if (rxState->IsRxReady())  // Move back to "rx_ready" list to read more data
                    rx_ready_list.Append(*rxState);
                else  // Move back to "rx_pending" list until NORM_RX_OBJECT_UPDATED
                    msg_ready_list.Append(*rxState);
            }  // end while(NULL != (rxState = iterator.GetNextItem()))
        }  // end if (zmq_input_ready)
    }  // end while ((!rx_ready_list.empty() || (zmq_input_ready && !msg_ready_list.empty()))
    
    // Alert zmq of the messages we have pushed up
    // zmq_session->flush();
    
}  // end zmq::norm_listener_t::recv_data()

zmq::norm_listener_t::NormRxStreamState::NormRxStreamState(NormObjectHandle normStream, 
                                                         int64_t          maxMsgSize)
 : norm_stream(normStream), max_msg_size(maxMsgSize), 
   in_sync(false), rx_ready(false), zmq_decoder(NULL), skip_norm_sync(false),
   buffer_ptr(NULL), buffer_size(0), buffer_count(0),
   prev(NULL), next(NULL), list(NULL)
{
}

zmq::norm_listener_t::NormRxStreamState::~NormRxStreamState()
{
    if (NULL != zmq_decoder)
    {
        delete zmq_decoder;
        zmq_decoder = NULL;
    }
    if (NULL != list)
    {
        list->Remove(*this);
        list = NULL;
    }
}

bool zmq::norm_listener_t::NormRxStreamState::Init()
{
    in_sync = false;
    skip_norm_sync = false;
    if (NULL != zmq_decoder) delete zmq_decoder;
    // Note "in_batch_size" comes from config.h
    zmq_decoder = new (std::nothrow) v2_decoder_t (in_batch_size, max_msg_size);
    alloc_assert (zmq_decoder);
    if (NULL != zmq_decoder)
    {
        buffer_count = 0;
        buffer_size = 0;
        zmq_decoder->get_buffer(&buffer_ptr, &buffer_size);
        return true;
    }
    else
    {
        return false;
    }
}  // end zmq::norm_listener_t::NormRxStreamState::Init()

// This decodes any pending data sitting in our stream decoder buffer
// It returns 1 upon message completion, -1 on error, 1 on msg completion
int zmq::norm_listener_t::NormRxStreamState::Decode()
{
    // If we have pending bytes to decode, process those first
    while (buffer_count > 0)
    {
        // There's pending data for the decoder to decode
        size_t processed = 0;
        
        // This a bit of a kludgy approach used to weed
        // out the NORM ZMQ message transport "syncFlag" byte
        // from the ZMQ message stream being decoded (but it works!)
        if (skip_norm_sync) 
        {
            buffer_ptr++;
            buffer_count--;
            skip_norm_sync = false;
        }
        
        int rc = zmq_decoder->decode(buffer_ptr, buffer_count, processed);
        buffer_ptr += processed;
        buffer_count -= processed;
        switch (rc)
        {
            case 1:
                // msg completed
                if (0 == buffer_count)
                {
                    buffer_size = 0;
                    zmq_decoder->get_buffer(&buffer_ptr, &buffer_size);
                }
                skip_norm_sync = true;
                return 1;
            case -1:
                // decoder error (reset decoder and state variables)
                in_sync = false;
                skip_norm_sync = false;  // will get consumed by norm sync check
                Init();
                break;
                
            case 0:
                // need more data, keep decoding until buffer exhausted
                break;
        }
    }
    // Reset buffer pointer/count for next read
    buffer_count = 0;
    buffer_size = 0;
    zmq_decoder->get_buffer(&buffer_ptr, &buffer_size);
    return 0;  //  need more data
    
}  // end zmq::norm_listener_t::NormRxStreamState::Decode()

zmq::norm_listener_t::NormRxStreamState::List::List()
 : head(NULL), tail(NULL)
{
}

zmq::norm_listener_t::NormRxStreamState::List::~List()
{
    Destroy();
}

void zmq::norm_listener_t::NormRxStreamState::List::Destroy()
{
    NormRxStreamState* item = head;
    while (NULL != item)
    {
        Remove(*item);
        delete item;
        item = head;
    }
}  // end zmq::norm_listener_t::NormRxStreamState::List::Destroy()

void zmq::norm_listener_t::NormRxStreamState::List::Append(NormRxStreamState& item)
{
    item.prev = tail;
    if (NULL != tail)
        tail->next = &item;
    else
        head = &item;
    item.next = NULL;
    tail = &item;
    item.list = this;
}  // end zmq::norm_listener_t::NormRxStreamState::List::Append()

void zmq::norm_listener_t::NormRxStreamState::List::Remove(NormRxStreamState& item)
{
    if (NULL != item.prev)
        item.prev->next = item.next;
    else
        head = item.next;
    if (NULL != item.next)
        item.next ->prev = item.prev;
    else
        tail = item.prev;
    item.prev = item.next = NULL;
    item.list = NULL;
}  // end zmq::norm_listener_t::NormRxStreamState::List::Remove()

zmq::norm_listener_t::NormRxStreamState::List::Iterator::Iterator(const List& list)
 : next_item(list.head)
{
}

zmq::norm_listener_t::NormRxStreamState* zmq::norm_listener_t::NormRxStreamState::List::Iterator::GetNextItem()
{
    NormRxStreamState* nextItem = next_item;
    if (NULL != nextItem) next_item = nextItem->next;
    return nextItem;
}  // end zmq::norm_listener_t::NormRxStreamState::List::Iterator::GetNextItem()
#endif // ZMQ_DEBUG_NORM

#endif // ZMQ_HAVE_NORM
