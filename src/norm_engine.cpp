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

#include "platform.hpp"

#if defined ZMQ_HAVE_NORM

#include "norm_engine.hpp"
#include "session_base.hpp"
#include "socket_base.hpp"
#include "v2_decoder.hpp"
#include "norm_decoder.hpp"
#include "v2_protocol.hpp"

#ifdef ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#else
#include <arpa/inet.h>
#endif

#if defined ZMQ_DEBUG_NORM
#include <iostream>
#endif

zmq::norm_engine_t::norm_engine_t (zmq::socket_base_t *socket_,
                                   const options_t& options_)
    : stream_object_t (options_, std::string()),
      socket (socket_),
      options (options_),  
      norm_instance (NORM_INSTANCE_INVALID),
      norm_descriptor (retired_fd),
      norm_descriptor_handle (0),
      norm_session (NORM_SESSION_INVALID), 
      norm_plugged (false),
      is_unicast (false),
      is_sender (false),
      is_receiver (false),
      is_stream (false),
      from_listener (false),
      zmq_encoder (0),
      norm_tx_stream (NORM_OBJECT_INVALID), 
      tx_first_msg (true),
      tx_more_bit (false), 
      zmq_output_ready (false),
      norm_tx_ready (false), 
      tx_index (0),
      tx_len (0),
      norm_acking (false),
      norm_watermark_pending (false),
      norm_segment_size (0),
      norm_stream_buffer_max (0),
      norm_stream_buffer_count (0),
      norm_stream_bytes_remain (0),
      norm_ack_retry_max (1),
      norm_ack_retry_count (0),
      zmq_input_ready (false)
{
    int rc = tx_msg.init ();
    errno_assert (0 == rc);
    //  Tell stream_object to use its special behaviors for a norm stream. For
    //  example, require V3 proto, and send greeting all at once, not in parts.
    set_using_norm (true);
}

zmq::norm_engine_t::norm_engine_t (zmq::socket_base_t *socket_,
                                   const options_t& options_,
                                   NormInstanceHandle norm_instance_,
                                   NormSessionHandle norm_session_,
                                   NormNodeHandle normNodeHandle_,
                                   norm_address_t &client_norm_address_,
                                   norm_address_t &listen_norm_address_)
    : stream_object_t (options_, std::string()),
      socket (socket_),
      options (options_),  
      norm_instance (NORM_INSTANCE_INVALID),
      norm_descriptor (retired_fd),
      norm_descriptor_handle (0),
      norm_session (NORM_SESSION_INVALID), 
      norm_plugged (false),
      is_unicast (false),
      is_sender (false),
      is_receiver (false),
      is_stream (false),
      from_listener (false),
      zmq_encoder (0),
      norm_tx_stream (NORM_OBJECT_INVALID), 
      tx_first_msg (true),
      tx_more_bit (false), 
      zmq_output_ready (false),
      norm_tx_ready (false), 
      tx_index (0),
      tx_len (0),
      norm_acking (false),
      norm_watermark_pending (false),
      norm_segment_size (0),
      norm_stream_buffer_max (0),
      norm_stream_buffer_count (0),
      norm_stream_bytes_remain (0),
      norm_ack_retry_max (1),
      norm_ack_retry_count (0),
      zmq_input_ready (false)
{
    int rc = tx_msg.init ();
    errno_assert (0 == rc);
    //  Tell stream_object to use its special behaviors for a norm stream. For
    //  example, require V3 proto, and send greeting all at once, not in parts.
    set_using_norm (true);

    rc = init (norm_instance_, norm_session_, normNodeHandle_,
               client_norm_address_, listen_norm_address_);
    zmq_assert (rc >= 0);
    fd_t fd = NormGetDescriptor (norm_instance_);
    std::string listen_endpoint;
    listen_norm_address_.to_string (listen_endpoint);
    socket->event_accepted (listen_endpoint, fd);
}

zmq::norm_engine_t::norm_engine_t(zmq::socket_base_t *socket_,
                                  const options_t& options_,
                                  const char* network_,
                                  bool send,
                                  bool recv)
    : stream_object_t(options_, std::string()),
      socket (socket_),
      options (options_),  
      norm_instance (NORM_INSTANCE_INVALID),
      norm_descriptor (retired_fd),
      norm_descriptor_handle (0),
      norm_session (NORM_SESSION_INVALID), 
      norm_plugged (false),
      is_unicast (false),
      is_sender (false),
      is_receiver (false),
      is_stream (false),
      from_listener (false),
      zmq_encoder (0),
      norm_tx_stream (NORM_OBJECT_INVALID), 
      tx_first_msg (true),
      tx_more_bit (false), 
      zmq_output_ready (false),
      norm_tx_ready (false), 
      tx_index (0),
      tx_len (0),
      norm_acking (false),
      norm_watermark_pending (false),
      norm_segment_size (0),
      norm_stream_buffer_max (0),
      norm_stream_buffer_count (0),
      norm_stream_bytes_remain (0),
      norm_ack_retry_max (1),
      norm_ack_retry_count (0),
      zmq_input_ready (false)
{
    int rc = tx_msg.init ();
    errno_assert (0 == rc);
    //  Tell stream_object to use its special behaviors for a norm stream. For
    //  example, require V3 proto, and send greeting all at once, not in parts.
    set_using_norm (true);

    rc = init (network_, send, recv);
    zmq_assert (rc >= 0);
}

zmq::norm_engine_t::~norm_engine_t ()
{
    shutdown();  // in case it was not already called
}

// this API used by norm_listener to start engine for incoming unicast session
int zmq::norm_engine_t::init (NormInstanceHandle norm_instance_,
                              NormSessionHandle norm_session_,
                              NormNodeHandle normNodeHandle_,
                              norm_address_t &client_address_,
                              norm_address_t &listen_address_)
{
    zmq_assert (norm_instance_ != NORM_INSTANCE_INVALID);
    zmq_assert (norm_session_ != NORM_SESSION_INVALID);

    // listener calls this API meaning a 2-way messaging model vs 1-way pub/sub
    from_listener = true;
    is_stream = true;
    is_unicast = true;
    is_receiver = true;
    is_sender = true;

    norm_address = client_address_;
    norm_address.to_string (endpoint);
    // our_norm_address = listen_norm_address_;

    norm_instance = norm_instance_;
    norm_descriptor = NormGetDescriptor(norm_instance);
    norm_session = norm_session_;

    /// NormSetDefaultSyncPolicy(norm_session, NORM_SYNC_CURRENT);
    NormSetDefaultSyncPolicy(norm_session, NORM_SYNC_STREAM);

    NormSetCongestionControl(norm_session, true);  // ??

    UINT16 normSegmentSize = 1400;  // NORM_DATA payload size
    UINT16 normNumData = 16;   // FEC block size (user data packets per block)
    // Number of FEC parity packets _computed_ per block
    UINT32 normStreamBufferSize = (is_stream ? 2*1024*1024 : 256*1024);
    if (options.sndbuf != 0)
        normStreamBufferSize = (is_stream ? 2 * options.sndbuf : options.sndbuf);

    norm_tx_ready = true;
    norm_tx_stream = NormStreamOpen(norm_session, normStreamBufferSize);

    // Init variables related to ack-based flow control
    // (norm_acking MUST be set "true" for this to be used)
    norm_acking = true;
    norm_segment_size = normSegmentSize;
    norm_stream_buffer_max = NormGetStreamBufferSegmentCount
        (normStreamBufferSize, normSegmentSize, normNumData);
    // a little safety margin (perhaps not necessary)
    norm_stream_buffer_max -= normNumData;
    norm_stream_buffer_count = 0;
    norm_stream_bytes_remain = 0;
    norm_watermark_pending = false;


#if defined ZMQ_DEBUG_NORM
    char logfilename[48];
    sprintf(logfilename, "normLog_%u_%u.txt",
            (unsigned) listen_address_.getNormNodeId (),
            (unsigned) client_address_.getNormNodeId ());
    bool worked = NormOpenDebugLog(norm_instance, logfilename);
    NormSetMessageTrace(norm_session, true);
    NormSetDebugLevel(3);
    std::string clientStr, serverStr;
    client_address_.to_string_raw (clientStr);
    listen_address_.to_string_raw (serverStr);
    std::cout << "debug log " << logfilename << " for connection from client "
              << clientStr << " to server " << serverStr
              << (worked ? "" : " FAILED") << std::endl << std::flush;
#endif

    return 0;
}


int zmq::norm_engine_t::init(const char* network_, bool send, bool recv)
{
    // both send & recv set means the 2-way messaging model, vs 1-way pub/sub
    is_stream = send && recv;

    // Parse the "network_" address into id, iface, IP/host addr, and port
    // norm endpoint format: [id,][<iface>;]<addr>:<port>

    // TBD - How to determine local_ and ipv6_ to pass into resolve ()?
    if (norm_address.resolve (network_, false, false) < 0) {
        // errno set by whatever caused resolve () to fail
        return -1;
    }
    norm_address.to_string (endpoint);
    
    if (is_stream) {
        // Require unicast address for 2-way connection
        if (!NormIsUnicastAddress (norm_address.getRawHostName ())) {
            errno = EINVAL;
            return -1;
        }
    }
    
    if (NORM_INSTANCE_INVALID == norm_instance)
    {
        if (NORM_INSTANCE_INVALID == (norm_instance = NormCreateInstance()))
        {
            // errno set by whatever caused NormCreateInstance() to fail
            return -1;
        }
    }
    norm_descriptor = NormGetDescriptor(norm_instance);
    // remember our fd for ZMQ_SRCFD in messages
    socket->set_fd(norm_descriptor);

    // TBD - What do we use for our local NormNodeId?
    //  (for now we use automatic, IP addr based assignment or passed in 'id')
    //       a) Use ZMQ Identity somehow?
    //       b) Add function to use iface addr
    //       c) Randomize and implement a NORM session layer
    //          conflict detection/resolution protocol

    const char *hostname = (is_stream ? "127.0.0.1" :
                            norm_address.getRawHostName ());
    UINT16 port = (is_stream ? 0 : norm_address.getPortNumber ());
    norm_session = NormCreateSession(norm_instance, hostname, port,
                                     norm_address.getNormNodeId ());
    if (NORM_SESSION_INVALID == norm_session)
    {
        int savedErrno = errno;
        shutdown();
        errno = savedErrno;
        return -1;
    }

    // There's many other useful NORM options that could be applied here
    if (NormIsUnicastAddress(norm_address.getRawHostName ()))
    {
        is_unicast = true;
        NormSetDefaultUnicastNack(norm_session, true);
    }
    else
    {
        // These NORM options only apply for multicast sessions
        // ZMQ default hops is 1
        //NormSetTTL(norm_session, options.multicast_hops);
        // since the ZMQ_MULTICAST_HOPS socket option isn't well-supported
        NormSetTTL(norm_session, 255);
        // port reuse doesn't work for non-connected unicast
        NormSetRxPortReuse(norm_session, true);
        // needed when multicast users on same machine
        NormSetLoopback(norm_session, true);

        if (norm_address.isIfaceName ())
        {
            // A bad interface may not be caught until sender or receiver start
            // norm_address.resolve () uses tcp_address_t::resolve_interface ()
            // to validate the IfaceName, but that doesn't check that it is
            // multicast capable.
            // (Since sender/receiver is not yet started, always succeeds here)
            NormSetMulticastInterface(norm_session,
                                      norm_address.getIfaceName ());
        }
    }

    UINT16 normSegmentSize = 1400;  // NORM_DATA payload size
    UINT16 normNumData = 16;   // FEC block size (user data packets per block)
    // Number of FEC parity packets _computed_ per block
    UINT16 normNumParity = 4;
    UINT32 normRxBufferSize = (is_stream ? 2*1024*1024 : 128*1024);
    if (options.rcvbuf != 0) normRxBufferSize = options.rcvbuf;
    UINT32 normTxBufferSize = (is_stream ? 2*1024*1024 : 128*1024);
    if (options.sndbuf != 0) normTxBufferSize = options.sndbuf;
    UINT32 normStreamBufferSize = (is_stream ? 2*1024*1024 : 256*1024);
    if (options.sndbuf != 0)
        normStreamBufferSize = (is_stream ? 2 * options.sndbuf : options.sndbuf);

    if (is_stream) {
        // To get here, this unicast socket must be connecting, not binding

        // Here is how we connect with an ephemeral port, rather than needing
        // to use the well-known port connecte to as is norm's default behavior
        // 1) create a session that will be assigned an ephemeral port when
        //    NormStartReceiver() (or sender) is called
        // 2) start receiver and a single tx/rx socket, with the ephemeral
        //    (system assigned) local port binding being opened
        // 3) after socket binding is made, change the session destination to
        //    the server port/addr, while keeping ephemeral local binding
        // 4) start sending NORM_CMD(CC) to server, and server will detect a
        //    "new remote sender" with our address and ephemeral local port

        // NORM_SYNC_CURRENT provides "instant" receiver sync to the senders
        // _current_ message transmission, which might be the most appropriate
        // behavior for 2-way unicast messaging patterns

        /// NormSetDefaultSyncPolicy(norm_session, NORM_SYNC_CURRENT);
        NormSetDefaultSyncPolicy(norm_session, NORM_SYNC_STREAM);
    }
    else {
        // For one-way possibly-multicast pub/sub pattern, NORM_SYNC_STREAM
        // tries for everything the sender has cached/buffered
        NormSetDefaultSyncPolicy(norm_session, NORM_SYNC_STREAM);
    }
    
    if (recv)
    {
        if (!NormStartReceiver(norm_session, normRxBufferSize))
        {
            int savedErrno = errno;   // errno was set by whatever failed
            shutdown();
            errno = savedErrno;
            return -1;
        }
        is_receiver = true;
    }

    if (send)
    {
        if (is_stream) {
            // change the session destination to the server address/port
            bool worked = NormChangeDestination(norm_session,
                                                norm_address.getRawHostName (),
                                                norm_address.getPortNumber ());
            if (!worked) {
                shutdown();
                errno = EREMOTE;  // dunno - pick some distinctive errno?
                return -1;
            }
        }

        // Pick a random sender instance id (aka norm sender session id)
        NormSessionId session_id = NormGetRandomSessionId();
        // TBD - provide "options" for some NORM sender parameters
        if (!NormStartSender(norm_session, session_id, normTxBufferSize, 
                             normSegmentSize, normNumData, normNumParity))
        {
            int savedErrno = errno;   // errno was set by whatever failed
            shutdown();
            errno = savedErrno;
            return -1;
        }    
        NormSetCongestionControl(norm_session, true);
        norm_tx_ready = true;
        is_sender = true;
        norm_tx_stream = NormStreamOpen(norm_session, normStreamBufferSize);
        if (NORM_OBJECT_INVALID == norm_tx_stream)
        {
            int savedErrno = errno;   // errno was set by whatever failed
            shutdown();
            errno = savedErrno;
            return -1;
        }

        if (is_stream) {
            // Init variables related to ack-based flow control
            // (norm_acking MUST be set "true" for this to be used)
            norm_acking = true;
            norm_segment_size = normSegmentSize;
            norm_stream_buffer_max = NormGetStreamBufferSegmentCount
                (normStreamBufferSize, normSegmentSize, normNumData);
            // a little safety margin (perhaps not necessary)
            norm_stream_buffer_max -= normNumData;
            norm_stream_buffer_count = 0;
            norm_stream_bytes_remain = 0;
            norm_watermark_pending = false;
        }
    }
    
#if defined ZMQ_DEBUG_NORM
    char logfilename[32];
    sprintf(logfilename, "normLog_%u.txt",
            (unsigned) norm_address.getNormNodeId ());
    bool worked = NormOpenDebugLog(norm_instance, logfilename);
    NormSetMessageTrace(norm_session, true);
    NormSetDebugLevel(3);
    std::string clientStr;
    norm_address.to_string_raw (clientStr);
    std::cout << "debug log " << logfilename << " to connect on " << clientStr
              << (worked ? "" : " FAILED") << std::endl << std::flush;
#endif
    
    return 0;  // no error
}  // end zmq::norm_engine_t::init()

void zmq::norm_engine_t::shutdown()
{
    // TBD - implement a more graceful shutdown option
    if (is_receiver)
    {
        NormStopReceiver (norm_session);
        
        // delete any active NormRxStreamState
        rx_pending_list.Destroy ();
        rx_ready_list.Destroy ();
        msg_ready_list.Destroy ();
        
        is_receiver = false;
    }
    if (is_sender)
    {
        NormStopSender (norm_session);
        is_sender = false;
    }
    if (NORM_SESSION_INVALID != norm_session)
    {
        NormDestroySession (norm_session);
        norm_session = NORM_SESSION_INVALID;
    }
    if (NORM_INSTANCE_INVALID != norm_instance)
    {
        NormStopInstance (norm_instance);
        NormDestroyInstance (norm_instance);
        norm_instance = NORM_INSTANCE_INVALID;
    }
}  // end zmq::norm_engine_t::shutdown()

void zmq::norm_engine_t::plug (io_thread_t* io_thread_,
                               session_base_t *session_)
{
    zmq_assert (norm_instance != NORM_INSTANCE_INVALID);
    zmq_assert (!norm_plugged);
    norm_plugged = true;

    if (is_sender) zmq_output_ready = true;
    if (is_receiver) zmq_input_ready = true;
    
    // TBD - we may assign the NORM engine to an io_thread in the future???

    //  Connect to session object.
    zmq_assert (!session);
    zmq_assert (session_);

    if (is_stream) {
        stream_object_t::plug (io_thread_, session_);
    } else {
        session = session_;
        io_object_t::plug (io_thread_);

        norm_descriptor = NormGetDescriptor(norm_instance);
        norm_descriptor_handle = add_fd(norm_descriptor);
        // Set POLLIN for notification of pending NormEvents
        set_pollin(norm_descriptor_handle); 
    }

    if (is_sender) send_data();
    
}  // end zmq::norm_engine_t::plug()

void zmq::norm_engine_t::unplug()
{
    zmq_assert (norm_plugged);
    norm_plugged = false;

    if (is_stream) {
        stream_object_t::unplug ();
    } else {
        rm_fd(norm_descriptor_handle);
        session = NULL;
    }

}  // end zmq::norm_engine_t::unplug()

void zmq::norm_engine_t::terminate()
{
    unplug();
    shutdown();
    delete this;
}

void zmq::norm_engine_t::restart_output()
{
    // There's new message data available from the session
    zmq_output_ready = true;
    if (norm_tx_ready) send_data();
    
}  // end zmq::norm_engine_t::restart_output()

void zmq::norm_engine_t::send_data()
{
    // Here we write as much as is available or we can
    while (zmq_output_ready && norm_tx_ready)
    {
        if (is_stream) {
            stream_object_t::out_event();
            continue;
        }

        zmq_assert (!is_stream);
        if (0 == tx_len)
        {
            // Our tx_buffer needs data to send
            // Get more data from encoder
            size_t space = BUFFER_SIZE;
            unsigned char* bufPtr = (unsigned char*)tx_buffer;
            tx_len = zmq_encoder.encode(&bufPtr, space);
            if (0 == tx_len)
            {
                if (tx_first_msg)
                {
                    // We don't need to mark eom/flush until a message is sent
                    tx_first_msg = false;
                }
                else
                {
                    // A prior message was completely written to stream, so
                    // mark end-of-message and possibly flush (to force packet
                    // transmission, even if it's not a full segment so message
                    // gets delivered quickly)
                    // NormStreamMarkEom(norm_tx_stream);
                    // the flush below marks eom
                    // Note NORM_FLUSH_ACTIVE makes NORM fairly chatty for low
                    // duty cycle messaging, but makes sure messages are
                    // delivered quickly.  Positive acknowledgements with flush
                    // override would make NORM more succinct here
                    if (norm_acking)
                        stream_flush(true, NORM_FLUSH_ACTIVE);
                    else
                        NormStreamFlush(norm_tx_stream, true, NORM_FLUSH_ACTIVE);
                }
                // Need to pull and load a new message to send
                if (-1 == session->pull_msg(&tx_msg))
                {
                    // We need to wait for a "restart_output()" call by ZMQ 
                    zmq_output_ready = false;
                    break;
                }
                zmq_encoder.load_msg(&tx_msg);
                // Should we write message size header for NORM to use? Or
                // expect NORM receiver to decode ZMQ message framing formats?
                // OK - we need to use a byte to denote when the ZMQ frame is
                //      the _first_ frame of a message so it can be decoded
                //      properly when a receiver 'syncs' mid-stream.  We key
                //      this norm_sync flag off the state of the 'more_flag'.
                //      I.e., If more_flag _was_ false previously, then this
                //      is the first frame of a ZMQ message.
                if (tx_more_bit)
                    tx_buffer[0] = (char)0xff;  // this is not a first frame
                else
                    tx_buffer[0] = 0x00;  // this is first frame of message
                tx_more_bit = (0 != (tx_msg.flags() & msg_t::more));
                // Go ahead an get a first chunk of the message
                bufPtr++;
                space--;
                tx_len = 1 + zmq_encoder.encode(&bufPtr, space);
                tx_index = 0;
            }
        }
        // Do we have data in our tx_buffer pending
        if (tx_index < tx_len)
        {
            // We have data in our tx_buffer to send, so write it to the stream
            if (norm_acking)
                tx_index += stream_write(tx_buffer + tx_index, tx_len - tx_index);
            else
                tx_index += NormStreamWrite(norm_tx_stream, tx_buffer + tx_index, tx_len - tx_index);
            if (tx_index < tx_len)
            {
                // NORM stream buffer full, wait for NORM_TX_QUEUE_VACANCY
                // (or NORM_TX_WATERMARK_COMPLETED if norm_acking)
                norm_tx_ready = false;
                break;
            }
            tx_len = 0;  // all buffered data was written
        }
    }  // end while (zmq_output_ready && norm_tx_ready)
}  // end zmq::norm_engine_t::send_data()


// This writes to the tx norm_stream and does the accounting needed for
// ack-based flow control.  The number of bytes written is returned.
// (If too much unacknowledged content is outstanding, the amount
//  of data written limited.)

unsigned int zmq::norm_engine_t::stream_write(const char* buffer, unsigned int numBytes)
{
    // This method uses NormStreamWrite(), but limits writes by explicit ACK-based flow control status
    if (norm_stream_buffer_count < norm_stream_buffer_max)
    {
        // 1) How many buffer bytes are available?
        unsigned int bytesAvailable = norm_segment_size * (norm_stream_buffer_max - norm_stream_buffer_count);
        bytesAvailable -= norm_stream_bytes_remain;  // unflushed segment portiomn
        if (numBytes <= bytesAvailable) 
        {
            unsigned int totalBytes = numBytes + norm_stream_bytes_remain;
            unsigned int numSegments = totalBytes / norm_segment_size;
            norm_stream_bytes_remain = totalBytes % norm_segment_size;
            norm_stream_buffer_count += numSegments;
        }
        else
        {
            numBytes = bytesAvailable;
            norm_stream_buffer_count = norm_stream_buffer_max;        
        }
        // 2) Write to the stream
        unsigned int bytesWritten = NormStreamWrite(norm_tx_stream, buffer, numBytes);
        //ASSERT(bytesWritten == numBytes);  // this could happen if timer-based flow control is left enabled
        // 3) Check if we need to issue a watermark ACK request?
        if (!norm_watermark_pending && (norm_stream_buffer_count >= (norm_stream_buffer_max / 2)))
        {
            //TRACE("norm_engine_t::WriteToNormStream() initiating watermark ACK request (buffer count:%lu max:%lu usage:%u)...\n",
            //            norm_stream_buffer_count, norm_stream_buffer_max, NormStreamGetBufferUsage(norm_tx_stream));
            NormSetWatermark(norm_session, norm_tx_stream);
            norm_ack_retry_count = norm_ack_retry_max;
            norm_watermark_pending = true;
        }
        return bytesWritten;
    }
    else
    {
        return 0;
    }
}  // end zmq::norm_engine_t::stream_write()


void zmq::norm_engine_t::stream_flush(bool eom, NormFlushMode flushMode)
{
    // NormStreamFlush always will transmit pending runt segments, if applicable
    // (thus we need to manage our buffer counting accordingly if pending bytes remain)
    if (norm_watermark_pending)
    {
        NormStreamFlush(norm_tx_stream, eom, flushMode);
    }
    else if (NORM_FLUSH_ACTIVE == flushMode)
    {
        // we flush passive, because watermark forces active ack request
        NormStreamFlush(norm_tx_stream, eom, NORM_FLUSH_PASSIVE);
        NormSetWatermark(norm_session, norm_tx_stream, true);
    }
    else
    {
        NormStreamFlush(norm_tx_stream, eom, flushMode);
    }
   
    if (0 != norm_stream_bytes_remain)
    {
        // The flush forces the runt segment out, so we increment our buffer usage count
        norm_stream_buffer_count++;
        norm_stream_bytes_remain = 0;
        if (!norm_watermark_pending && (norm_stream_buffer_count >= (norm_stream_buffer_max >> 1)))
        {
            //TRACE("norm_engine_t::stream_flush() initiating watermark ACK request (buffer count:%lu max:%lu usage:%u)...\n",
            //       norm_stream_buffer_count, norm_stream_buffer_max);
            NormSetWatermark(norm_session, norm_tx_stream, true);
            norm_ack_retry_count = norm_ack_retry_max;
            norm_watermark_pending = true;
        }
    } 
}  // end zmq::norm_engine_t::stream_flush()

int zmq::norm_engine_t::engine_out_event()
{
    return 0;
}

int zmq::norm_engine_t::engine_in_event()
{
    return 0;
}

void zmq::norm_engine_t::in_event()
{
    // This means a NormEvent is probably pending, 
    // so call NormGetNextEvent() and handle
    int events_processed = 0;
    NormEvent event;
    while (NormGetNextEvent(norm_instance, &event, false))
    {
#ifdef ZMQ_DEBUG_NORM_2
        if (event.type < NORM_GRTT_UPDATED &&
            event.type != NORM_TX_RATE_CHANGED) {

            if (event.type > NORM_TX_RATE_CHANGED) {
                char clientAddr[64];
                UINT16 clientPort;
                norm_address_t::getEventAddr(event, clientAddr, clientPort);
                std::cout << "engine event: " << event.type
                          << " from " << clientAddr << ":" << clientPort
                          << " ID " << NormNodeGetId (event.sender)
                          << std::endl << std::flush;
            }
            else
                //  NORM_TX_* events don't have address, port, or normID set
                std::cout << "engine event: " << event.type
                          << std::endl << std::flush;
        }
#endif

        switch(event.type)
        {
            case NORM_TX_QUEUE_VACANCY:
            case NORM_TX_QUEUE_EMPTY:
                events_processed++;
                if (!norm_tx_ready)
                {
                    norm_tx_ready = true;
                    send_data();
                }
                break;
                
            case NORM_TX_WATERMARK_COMPLETED:
                events_processed++;
                if (NORM_ACK_SUCCESS == NormGetAckingStatus(norm_session))
                {
                    if (norm_watermark_pending)
                    {
                        // We were being flow controlled.  Everyone
                        // acknowledged so we can move forward.
                        norm_watermark_pending = false;
                        norm_stream_buffer_count -= (norm_stream_buffer_max / 2);
                        if (!norm_tx_ready)
                        {
                            norm_tx_ready = true;
                            send_data();
                        }
                    }
                }
                else if (norm_ack_retry_count > 0)
                {
                    // Someone didn't acknowledge but we're configured to try
                    // again. Reset the watermark acknowledgement request
                    NormResetWatermark(norm_session);
                    norm_ack_retry_count--;
                }
                else
                {
                    // Find out who didn't acknowledge and kick them out
                    if (is_unicast)
                    {
                        // If it's a unicast session, then reset indefinitely?
                        NormResetWatermark(norm_session);
                    }
                    else
                    {
                        // Assume the non-acking receiver(s) quit and remove from acking list
                        NormAckingStatus ackingStatus;
                        NormNodeId prevNodeId = NORM_NODE_NONE;
                        NormNodeId nodeId = NORM_NODE_NONE; // init iteration
                        while (NormGetNextAckingNode(norm_session, &nodeId, &ackingStatus))
                        {
                            if (NORM_ACK_SUCCESS != ackingStatus)
                            {
                                NormRemoveAckingNode(norm_session, nodeId);
                                // prevent underlying iteration reset
                                nodeId = prevNodeId;
                            }
                            else
                            {
                                // This keeps the underlying NORM iterator from
                                // resetting when an acking node is removed.
                                NormNodeId tempId = nodeId;
                                nodeId = prevNodeId;
                                prevNodeId = tempId;
                            }
                        }
                        if (norm_watermark_pending)
                        {
                            // We were being flow controlled. Move
                            // forward for remaining receivers.
                            norm_watermark_pending = false;
                            norm_stream_buffer_count -= (norm_stream_buffer_max / 2);
                            if (!norm_tx_ready)
                            {
                                norm_tx_ready = true;
                                send_data();
                            }
                        }
                    }
                }
                break;

            case NORM_RX_OBJECT_NEW:
            case NORM_RX_OBJECT_UPDATED:
                events_processed++;
                recv_data(event.object);
                break;

            case NORM_RX_OBJECT_ABORTED:
            {
                events_processed++;
                NormRxStreamState* rxState =
                    (NormRxStreamState*)NormObjectGetUserData(event.object);
                if (NULL != rxState)
                {
                    // Remove the state from the list it's in
                    // This is now unnecessary since deletion takes care of
                    // list removal, but in the interest of being clear ...
                    NormRxStreamState::List* list = rxState->AccessList();
                    if (NULL != list) list->Remove(*rxState);
                }
                delete rxState;
                break;
            }

            case NORM_REMOTE_SENDER_NEW:
                if (is_stream) {
                    events_processed++;
                    NormNodeId remoteId = NormNodeGetId(event.sender);
#ifdef ZMQ_DEBUG_NORM
                    bool worked =
#endif
                        NormAddAckingNode(norm_session, remoteId);
#ifdef ZMQ_DEBUG_NORM
                    std::cout << "NormAddAckingNode(" << (unsigned)remoteId
                              << ")" << (worked ? " WORKED" : " FAILED")
                              << std::endl << std::flush;
#endif
                    norm_address_t sender_address;
                    char IPaddr[16]; // big enough for IPv6
                    unsigned int addrLen = sizeof (IPaddr);
                    UINT16 senderPort;
                    NormNodeGetAddress(event.sender, IPaddr, &addrLen,
                                       &senderPort);
                    int addrFamily= (4 == addrLen ? AF_INET : AF_INET6);
                    sender_address.setTCPAddress(IPaddr, addrLen, addrFamily);
                    sender_address.setPortNumber (senderPort);
                    sender_address.setNormNodeId (NormNodeGetId (event.sender));
                    std::string sender_endpoint;
                    sender_address.to_string (sender_endpoint);
                    //  from_listener engine has already called event_accepted
                    if (!from_listener)
                        socket->event_connected (sender_endpoint,
                                                 engine_get_fd ());
                }
                break;

            case NORM_REMOTE_SENDER_INACTIVE:
                // Here we free resources used for this formerly active sender.
                // Note w/ NORM_SYNC_STREAM, if sender reactivates, we may
                //  get some messages delivered twice.  NORM_SYNC_CURRENT would
                // mitigate that but might miss data at startup. Always tradeoffs.
                // Instead of immediately deleting, we could instead initiate a
                // user configurable timeout here to wait some amount of time
                // after this event to declare the remote sender truly dead
                // and delete its state???
                NormNodeDelete(event.sender);  
                break;
            
            default:
                // We ignore other NORM events 
#if defined ZMQ_DEBUG_NORM && not defined ZMQ_DEBUG_NORM_2
                if (event.type < NORM_GRTT_UPDATED &&
                    event.type != NORM_TX_RATE_CHANGED) {

                    char clientAddr[64];
                    UINT16 clientPort;
                    norm_address_t::getEventAddr(event, clientAddr,clientPort);

                    std::cout << "engine unhandled event: " << event.type
                              << " from " << clientAddr << ":" << clientPort
                              << " ID " << NormNodeGetId (event.sender)
                              << std::endl << std::flush;
                }
#endif
                break;
        }  // end switch(event.type)
    }

    /// I don't think this call is needed. The calls in recv_data () are enough
    // stream_object_t::in_event ();

}  // zmq::norm_engine_t::engine_in_event()

void zmq::norm_engine_t::restart_input()
{
    // TBD - should we check/assert that zmq_input_ready was false???
    zmq_input_ready = true;
    // Process any pending received messages
    if (!msg_ready_list.IsEmpty())
        recv_data(NORM_OBJECT_INVALID);
    
}  // end zmq::norm_engine_t::restart_input()

void zmq::norm_engine_t::recv_data(NormObjectHandle object)
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
            // This is a new stream, so create rxState with zmq decoder, etc
            rxState = new NormRxStreamState(object, options.maxmsgsize);
            if (!rxState->Init(is_stream))
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
    while (!rx_ready_list.IsEmpty() ||
           (zmq_input_ready && !msg_ready_list.IsEmpty()))
    {
        // Iterate through our rx_ready streams, reading data into the decoder
        // (This services incoming "rx ready" streams in a round-robin fashion)
        NormRxStreamState::List::Iterator iterator(rx_ready_list);
        NormRxStreamState* rxState;
        while (NULL != (rxState = iterator.GetNextItem()))
        {
            switch(rxState->Decode (is_stream))
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
                // read our leading 'flag' byte to see if this is a first frame
                char norm_sync;
                unsigned int numBytes = 1;
                if (!NormStreamRead(stream, &norm_sync, &numBytes))
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
                if (0 == norm_sync) rxState->SetSync(true); // is a first frame
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
                rxState->Init(is_stream);  // TBD - check result
                // This will retry syncing, and getting data from this stream
                // since we don't increment the "it" iterator
                continue;
            }
#if defined ZMQ_DEBUG_NORM
            std::cout << "IncrementBufferCount by " << numBytes << " "
                      << std::endl << std::flush;
#endif
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
                if (is_stream) {
                    if (!zmq_input_ready) break;
                    if (!decoder) {
                        if (is_stream)
                            decoder = new (std::nothrow) norm_decoder_t
                                (in_batch_size, options.maxmsgsize);
                        else
                            decoder = new (std::nothrow) v2_decoder_t
                                (in_batch_size, options.maxmsgsize);
                    }
#if defined ZMQ_DEBUG_NORM
                    std::cout << "stream_object_t::in_event with "
                              << rxState->AccessMsg()->size () << " "
                              << std::endl << std::flush;
#endif
                    // Allow stream_object_t to process the received msg
                    stream_object_t::in_event ();
                    if (msg_ready_list.IsEmpty ()) break;
                    continue;
                }

                msg_t* msg = rxState->AccessMsg();
                if (-1 == session->push_msg(msg))
                {
                    if (EAGAIN == errno)
                    {
                        // need to wait until session calls "restart_input()"
                        zmq_input_ready = false;
                        break;
                    }
                    else
                    {
                        // session rejected message?
                        // TBD - handle this better
                        zmq_assert (false); 
                    }
                }
                // else message was accepted.
                msg_ready_list.Remove (*rxState);
                if (rxState->IsRxReady ())
                    // Move back to "rx_ready" list to read more data
                    rx_ready_list.Append (*rxState);
                else
                    // Move back to "rx_pending" until NORM_RX_OBJECT_UPDATED
                    msg_ready_list.Append (*rxState);
            }  // end while(NULL != (rxState = iterator.GetNextItem()))
        }  // end if (zmq_input_ready)
    }  // end while ((!rx_ready_list.empty() || (zmq_input_ready && !msg_ready_list.empty()))
    
    // Alert zmq of the messages we have pushed up
    if (!is_stream)
        session->flush ();
    
}  // end zmq::norm_engine_t::recv_data()

zmq::norm_engine_t::NormRxStreamState::NormRxStreamState(NormObjectHandle normStream, 
                                                         int64_t          maxMsgSize)
    : norm_stream(normStream),
      max_msg_size(maxMsgSize), 
      in_sync(false),
      rx_ready(false),
      zmq_decoder(NULL),
      skip_norm_sync(false),
      buffer_ptr(NULL),
      buffer_size(0),
      buffer_count(0),
      prev(NULL),
      next(NULL),
      list(NULL)
{
}

zmq::norm_engine_t::NormRxStreamState::~NormRxStreamState()
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

bool zmq::norm_engine_t::NormRxStreamState::Init(bool is_stream_)
{
    in_sync = false;
    skip_norm_sync = false;
    if (NULL != zmq_decoder) delete zmq_decoder;
    // Note "in_batch_size" comes from config.h
    if (is_stream_)
        zmq_decoder =
            new (std::nothrow) norm_decoder_t (in_batch_size, max_msg_size);
    else
        zmq_decoder =
            new (std::nothrow) v2_decoder_t (in_batch_size, max_msg_size);
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
}  // end zmq::norm_engine_t::NormRxStreamState::Init()

// This decodes any pending data sitting in our stream decoder buffer
// It returns 1 upon message completion, -1 on error, 1 on msg completion
int zmq::norm_engine_t::NormRxStreamState::Decode(bool is_stream_)
{
    // If we have pending bytes to decode, process those first
    while (buffer_count > 0)
    {
        // There's pending data for the decoder to decode
        size_t processed = 0;
        
        // This a bit of a kludgy approach used to weed
        // out the NORM ZMQ message transport "norm_sync" byte
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
                Init(is_stream_);
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
    
}  // end zmq::norm_engine_t::NormRxStreamState::Decode()

zmq::norm_engine_t::NormRxStreamState::List::List()
 : head(NULL), tail(NULL)
{
}

zmq::norm_engine_t::NormRxStreamState::List::~List()
{
    Destroy();
}

void zmq::norm_engine_t::NormRxStreamState::List::Destroy()
{
    NormRxStreamState* item = head;
    while (NULL != item)
    {
        Remove(*item);
        delete item;
        item = head;
    }
}  // end zmq::norm_engine_t::NormRxStreamState::List::Destroy()

void zmq::norm_engine_t::NormRxStreamState::List::Append(NormRxStreamState& item)
{
    item.prev = tail;
    if (NULL != tail)
        tail->next = &item;
    else
        head = &item;
    item.next = NULL;
    tail = &item;
    item.list = this;
}  // end zmq::norm_engine_t::NormRxStreamState::List::Append()

void zmq::norm_engine_t::NormRxStreamState::List::Remove(NormRxStreamState& item)
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
}  // end zmq::norm_engine_t::NormRxStreamState::List::Remove()

zmq::norm_engine_t::NormRxStreamState::List::Iterator::Iterator(const List& list)
 : next_item(list.head)
{
}

zmq::norm_engine_t::NormRxStreamState* zmq::norm_engine_t::NormRxStreamState::List::Iterator::GetNextItem()
{
    NormRxStreamState* nextItem = next_item;
    if (NULL != nextItem) next_item = nextItem->next;
    return nextItem;
}  // end zmq::norm_engine_t::NormRxStreamState::List::Iterator::GetNextItem()


void zmq::norm_engine_t::engine_add_fd ()
{
    zmq_assert (!norm_descriptor_handle);

    norm_descriptor = NormGetDescriptor(norm_instance);
    norm_descriptor_handle = add_fd(norm_descriptor);

    // Set POLLIN for notification of pending NormEvents
    set_pollin(norm_descriptor_handle); 
}

void zmq::norm_engine_t::engine_rm_fd ()
{
    rm_fd(norm_descriptor_handle);
    norm_descriptor_handle = 0;
}

void zmq::norm_engine_t::engine_set_pollin ()
{
    zmq_input_ready = true;
    if (!msg_ready_list.IsEmpty ())
        stream_object_t::in_event ();
    // restart_input(); /// ??
}

void zmq::norm_engine_t::engine_reset_pollin ()
{
    zmq_input_ready = false;
}

void zmq::norm_engine_t::engine_set_pollout ()
{
    restart_output();
}

void zmq::norm_engine_t::engine_reset_pollout ()
{
#ifdef ZMQ_DEBUG_NORM
    std::cout << "NORM TX flush " << (norm_acking ? "acking " : "")
              << "tx_index: " << tx_index << " tx_len: " << tx_len
              << std::endl << std::flush;
#endif
    if (norm_acking /*&& tx_index < tx_len*/)
        stream_flush(true, NORM_FLUSH_ACTIVE);
    else
        NormStreamFlush(norm_tx_stream, true, NORM_FLUSH_ACTIVE);

    zmq_output_ready = false;
}

zmq::fd_t zmq::norm_engine_t::engine_get_fd ()
{
    return norm_descriptor;
}

int zmq::norm_engine_t::write (const void *data_, size_t size_)
{
    // We have data to send, so write as much as is accepted to the stream
    unsigned int accepted = 0;
    if (norm_tx_ready) {
#if defined ZMQ_DEBUG_NORM
        std::cout << "sending " << size_ << " "
                  << std::endl << std::flush;
#endif
        if (norm_acking)
            accepted = stream_write(static_cast <const char *> (data_),
                                    static_cast <unsigned int> (size_));
        else
            accepted = NormStreamWrite(norm_tx_stream,
                                       static_cast <const char *> (data_),
                                       static_cast <unsigned int> (size_));

        // tx_index += accepted;
        if (accepted < size_) {
            // NORM stream buffer full, wait for NORM_TX_QUEUE_VACANCY
            // (or NORM_TX_WATERMARK_COMPLETED if norm_acking)
            norm_tx_ready = false;
        }
    }
    return accepted;
}

int zmq::norm_engine_t::read (void *data_, size_t size_)
{
    size_t size = 0;

    NormRxStreamState::List::Iterator iterator(msg_ready_list);
    NormRxStreamState* rxState;
    if (NULL != (rxState = iterator.GetNextItem())) {
        msg_t* msg = rxState->AccessMsg();
        size = msg->size ();
        zmq_assert (size <= size_);
        ::memcpy (data_, msg->data (), size);
        // message was accepted.
        msg_ready_list.Remove (*rxState);
        if (rxState->IsRxReady ())
            // Move back to "rx_ready" list to read more data
            rx_ready_list.Append (*rxState);
        else
            // Move back to "rx_pending" until NORM_RX_OBJECT_UPDATED
            msg_ready_list.Append (*rxState);
    }

    return size;
}

#endif // ZMQ_HAVE_NORM
