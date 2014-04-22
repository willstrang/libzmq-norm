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
#if defined ZMQ_HAVE_WINDOWS
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

#include <string.h>
#include <new>
#include <sstream>

#include "stream_object.hpp"
#include "io_thread.hpp"
#include "session_base.hpp"
#include "v1_encoder.hpp"
#include "v1_decoder.hpp"
#include "v2_encoder.hpp"
#include "v2_decoder.hpp"
#include "null_mechanism.hpp"
#include "plain_mechanism.hpp"
#include "curve_client.hpp"
#include "curve_server.hpp"
#include "raw_decoder.hpp"
#include "raw_encoder.hpp"
#include "config.hpp"
#include "err.hpp"
#include "ip.hpp"
#include "likely.hpp"
#include "wire.hpp"

zmq::stream_object_t::stream_object_t (const options_t &options_, 
                                       const std::string &endpoint_) :
    inpos (NULL),
    insize (0),
    decoder (NULL),
    outpos (NULL),
    outsize (0),
    encoder (NULL),
    handshaking (true),
    greeting_size (v2_greeting_size),
    greeting_bytes_read (0),
    session (NULL),
    //  VeriSign Custom Code
    peer_keepalive_ivl (0),
    keepalive_ivl_time (0),
    keepalive_ticks_in (0),
    keepalive_limit_in (0),
    keepalive_ticks_out (0),
    keepalive_limit_out (0),

    protocol_version (-1),
    options (options_),
    endpoint (endpoint_),
    has_keepalive_timer (false),
    has_handshake_timer(false),
    using_norm (false),
    last_msg_flags (0),
    plugged (false),
    read_msg (&stream_object_t::read_identity),
    write_msg (&stream_object_t::write_identity),
    io_error (false),
    subscription_required (false),
    mechanism (NULL),
    input_stopped (false),
    output_stopped (false),
    socket (NULL)
{
    int rc = tx_msg.init ();
    errno_assert (rc == 0);
    
    //  Do transport-specific start-up stuff in child engine class
}

zmq::stream_object_t::~stream_object_t ()
{
    zmq_assert (!plugged);

    //  child engine class must free transport resources

    int rc = tx_msg.close ();
    errno_assert (rc == 0);

    delete encoder;
    delete decoder;
    delete mechanism;
}

void zmq::stream_object_t::plug (io_thread_t *io_thread_,
                                 session_base_t *session_)
{
    zmq_assert (!plugged);
    plugged = true;

    //  Connect to session object.
    zmq_assert (!session);
    zmq_assert (session_);
    session = session_;
    socket = session-> get_socket ();

    //  Connect to I/O threads poller object.
    io_object_t::plug (io_thread_);
    //  engine class must do any add_fd or equivalent
    engine_add_fd ();
    io_error = false;

#ifdef UNUSED
    if (get_io_thread ()) {
        char buf[120];
        sprintf(buf, "INIT: plug in stream object for endpoint %s",
                endpoint.c_str ());
        get_io_thread ()->log (buf);
        //get_io_thread ()->log ("INIT: plug in stream object");
    }
#endif

    if (options.raw_sock) {
        // no handshaking for raw sock, instantiate raw encoder and decoders
        encoder = new (std::nothrow) raw_encoder_t (out_batch_size);
        alloc_assert (encoder);

        decoder = new (std::nothrow) raw_decoder_t (in_batch_size);
        alloc_assert (decoder);

        // disable handshaking for raw socket
        handshaking = false;

        read_msg = &stream_object_t::pull_msg_from_session;
        write_msg = &stream_object_t::push_msg_to_session;

        //  For raw sockets, send an initial 0-length message to the
        // application so that it knows a peer has connected.
        msg_t connector;
        connector.init();
        (this->*write_msg) (&connector);
        connector.close();
        session->flush ();
    }
    else {
        //  VeriSign Custom Code
        // start init timer, to prevent handshake hanging forever on no input
        set_handshake_timer ();

        //  Send the 'length' and 'flags' fields of the identity message.
        //  The 'length' field is encoded in the long format.
        outpos = greeting_send;
        if (using_norm) {
            // add a norm sync byte saying that sync can start with this msg
            outpos [outsize++] = '\0';
            last_msg_flags = 0;
        }
        outpos [outsize++] = 0xff;
        put_uint64 (&outpos [outsize], options.identity_size + 1);
        outsize += 8;
        //  VeriSign Custom Code - unsets KEEPALIVE (64) bit, so 0x3F
        outpos [outsize++] = msg_t::mask_v1 | msg_t::more;
        // outpos [outsize++] = 0x7f;
 
        if (using_norm) {
            // send the rest of the greeting immediately, instead of waiting
            outpos [outsize++] = 3;     //  Major version number
            outpos [outsize++] = 0; //  Minor version number
            memset (outpos + outsize, 0, 20);

            zmq_assert (options.mechanism == ZMQ_NULL
                        ||  options.mechanism == ZMQ_PLAIN
                        ||  options.mechanism == ZMQ_CURVE);

            if (options.mechanism == ZMQ_NULL)
                memcpy (outpos + outsize, "NULL", 4);
            else
            if (options.mechanism == ZMQ_PLAIN)
                memcpy (outpos + outsize, "PLAIN", 5);
            else
                memcpy (outpos + outsize, "CURVE", 5);
            outsize += 20;
            memset (outpos + outsize, 0, 32);
            outsize += 32;
            greeting_size = v3_greeting_size;
        }
    }

    engine_set_pollin ();
    engine_set_pollout ();

    //  VeriSign Custom Code
    set_keepalive_timer ();

    //  Flush all the data that may have been already received downstream,
    //  or start handshaking.
    this->in_event ();
}

void zmq::stream_object_t::unplug ()
{
    zmq_assert (plugged);
    plugged = false;

    //  VeriSign Custom Code
    if (has_handshake_timer) {
        cancel_timer (handshake_timer_id);
        has_handshake_timer = false;
    }

    //  VeriSign Custom Code
    if (has_keepalive_timer) {
        cancel_timer (keepalive_timer_id);
        has_keepalive_timer = false;
    }

    //  engine class must Cancel all fd subscriptions.
    if (!io_error)
        engine_rm_fd ();

    //  Disconnect from I/O threads poller object.
    io_object_t::unplug ();

    session = NULL;
}

void zmq::stream_object_t::terminate ()
{
    unplug ();
    delete this;
}

void zmq::stream_object_t::in_event ()
{
    zmq_assert (!io_error);

    engine_in_event();

    //  If still handshaking, receive and process the greeting message.
    if (unlikely (handshaking))
        if (!handshake ())
            return;

    zmq_assert (decoder);

    //  If there has been an I/O error, stop polling.
    if (input_stopped) {
        engine_rm_fd ();
        io_error = true;
        return;
    }

    //  If there's no data to process in the buffer...
    if (!insize) {

        //  Retrieve the buffer and read as much data as possible.
        //  Note that buffer can be arbitrarily large. However, we assume
        //  the underlying TCP layer has a fixed buffer size and thus the
        //  number of bytes read will be always limited.
        size_t bufsize = 0;
        decoder->get_buffer (&inpos, &bufsize);

        int const rc = read (inpos, bufsize);
        // NORM transport can reasonably read 0 bytes at this point
        if (rc == 0 && !using_norm) {
            error ();
            return;
        }
        if (rc == -1) {
            if (errno != EAGAIN)
                error ();
            return;
        }

        //  Adjust input size
        insize = static_cast <size_t> (rc);

        //  VeriSign Custom Code
        keepalive_ticks_in = 0;
    }

    int rc = 0;
    size_t processed = 0;

    while (insize > 0) {
        rc = decoder->decode (inpos, insize, processed);
        zmq_assert (processed <= insize);
        inpos += processed;
        insize -= processed;
        if (rc == 0 || rc == -1)
            break;
        //  VeriSign Custom Code
        if (rc == 2)
            rc = recv_keepalive_v1 (decoder->msg ());
        //  VeriSign Custom Code
        else if ((decoder->msg ()->flags () & msg_t::command) &&
                 decoder->msg ()->size () >= 5 &&
                 memcmp(decoder->msg ()->data (),
                        "\4PONG", 5) == 0) {
            // Don't pass PONG command thru to the application. It has already
            // done it's work by making us set keepalive_ticks_in to 0 above.
        }
        else
            rc = (this->*write_msg) (decoder->msg ());
        if (rc == -1)
            break;
    }

    //  Tear down the connection if we have failed to decode input data
    //  or the session has rejected the message.
    if (rc == -1) {
        if (errno != EAGAIN) {
            error ();
            return;
        }

        //  If session has closed pipe to this connection, disconnect socket
        if (!session || !session->has_pipe ()) {
            error ();
            return;
        }

        input_stopped = true;
        engine_reset_pollin ();

        //  VeriSign Custom Code
        if (has_keepalive_timer) {
            cancel_timer (keepalive_timer_id);
            has_keepalive_timer = false;
        }
    }

    session->flush ();
}

void zmq::stream_object_t::out_event ()
{
    zmq_assert (!io_error);

    engine_out_event ();

    //  If write buffer is empty, try to read new data from the encoder.
    if (!outsize) {

        //  Even when we stop polling as soon as there is no
        //  data to send, the poller may invoke out_event one
        //  more time due to 'speculative write' optimisation.
        if (unlikely (encoder == NULL)) {
            zmq_assert (handshaking);
            return;
        }

        outpos = NULL;
        outsize = encoder->encode (&outpos, 0);

        while (outsize < out_batch_size) {
            if ((this->*read_msg) (&tx_msg) == -1)
                break;
            encoder->load_msg (&tx_msg);
            unsigned char *bufptr = outpos + outsize;
            if (using_norm) {
                //  add a norm sync byte saying if sync can start with this msg
                *bufptr = (last_msg_flags & msg_t::more ? (char)0xff : '\0');
                last_msg_flags = tx_msg.flags();
                bufptr++;
            }
            size_t n = encoder->encode (&bufptr, out_batch_size - outsize);
            zmq_assert (n > 0);
            if (outpos == NULL)
                outpos = bufptr;
            outsize += n;
            if (using_norm) {
                outsize++;
                break;
            }
        }

        //  If there is no data to send, stop polling for output.
        if (outsize == 0) {
            output_stopped = true;
            engine_reset_pollout ();
            return;
        }
    }

    //  If there are any data to write in write buffer, write as much as
    //  possible to the socket. Note that amount of data to write can be
    //  arbitrarily large. However, we assume that underlying TCP layer has
    //  limited transmission buffer and thus the actual number of bytes
    //  written should be reasonably modest.
    int nbytes = write (outpos, outsize);

    //  IO error has occurred. We stop waiting for output events.
    //  The engine is not terminated until we detect input error;
    //  this is necessary to prevent losing incoming messages.
    if (nbytes == -1) {
        engine_reset_pollout ();
        return;
    }

    //  VeriSign Custom Code
    keepalive_ticks_out = 0;

    outpos += nbytes;
    outsize -= nbytes;

    //  If we are still handshaking and there are no data
    //  to send, stop polling for output.
    if (unlikely (handshaking))
        if (outsize == 0)
            engine_reset_pollout ();
}

void zmq::stream_object_t::restart_output ()
{
    if (unlikely (io_error))
        return;

    if (likely (output_stopped)) {
        engine_set_pollout ();
        output_stopped = false;
    }

    //  Speculative write: The assumption is that at the moment new message
    //  was sent by the user the socket is probably available for writing.
    //  Thus we try to write the data to socket avoiding polling for POLLOUT.
    //  Consequently, the latency should be better in request/reply scenarios.
    out_event ();
}

void zmq::stream_object_t::restart_input ()
{
    zmq_assert (input_stopped);
    zmq_assert (session != NULL);
    zmq_assert (decoder != NULL);

    int rc = (this->*write_msg) (decoder->msg ());
    if (rc == -1) {
        if (errno == EAGAIN)
            session->flush ();
        else
            error ();
        return;
    }

    while (insize > 0) {
        size_t processed = 0;
        rc = decoder->decode (inpos, insize, processed);
        zmq_assert (processed <= insize);
        inpos += processed;
        insize -= processed;
        if (rc == 0 || rc == -1)
            break;
        rc = (this->*write_msg) (decoder->msg ());
        if (rc == -1)
            break;
    }

    if (rc == -1 && errno == EAGAIN)
        session->flush ();
    else
    if (rc == -1 || io_error)
        error ();
    else {
        input_stopped = false;
        engine_set_pollin ();
        session->flush ();

        //  Speculative read.
        this->in_event ();
    }
}

bool zmq::stream_object_t::handshake ()
{
    zmq_assert (handshaking);
    //  When using_norm, the whole greeting gets sent together, not in 2 parts
    zmq_assert (greeting_bytes_read < greeting_size ||
                (using_norm && greeting_bytes_read <= v3_greeting_size));

    //  Receive the greeting.
    while (greeting_bytes_read < greeting_size) {
        const int n = read (greeting_recv + greeting_bytes_read,
                            greeting_size - greeting_bytes_read);
        if (n == 0) {
            /// hack to wait for receive
            if (!using_norm)
                error ();
            return false;
        }
        if (n == -1) {
            if (errno != EAGAIN)
                error ();
            return false;
        }

#ifdef UNUSED
        if (has_handshake_timer) {
            cancel_timer (handshake_timer_id);
            has_handshake_timer = false;
        }
#endif

        greeting_bytes_read += n;

        //  We have received at least one byte from the peer.
        //  If the first byte is not 0xff, we know that the
        //  peer is using unversioned protocol.
        if (greeting_recv [0] != 0xff)
            break;

        if (greeting_bytes_read < signature_size)
            continue;

        //  Inspect the right-most bit of the 10th byte (which coincides
        //  with the 'flags' field if a regular message was sent).
        //  Zero indicates this is a header of identity message
        //  (i.e. the peer is using the unversioned protocol).
        if (!(greeting_recv [9] & 0x01))
            break;

        //  The peer is using versioned protocol.
        //  Send the major version number.
        if (outpos + outsize == greeting_send + signature_size) {
            if (outsize == 0)
                engine_set_pollout ();
            outpos [outsize++] = 3;     //  Major version number
        }

        // when using_norm, the whole greeting has already been sent
        if (greeting_bytes_read > signature_size && !using_norm) {
            if (outpos + outsize == greeting_send + signature_size + 1) {
                if (outsize == 0)
                    engine_set_pollout ();

                //  Use ZMTP/2.0 to talk to older peers.
                protocol_version = greeting_recv [10];
                if (greeting_recv [10] == ZMTP_1_0
                    ||  greeting_recv [10] == ZMTP_2_0) {
                    outpos [outsize++] = options.type;
                }
                else {
                    outpos [outsize++] = 0; //  Minor version number
                    memset (outpos + outsize, 0, 20);

                    zmq_assert (options.mechanism == ZMQ_NULL
                            ||  options.mechanism == ZMQ_PLAIN
                            ||  options.mechanism == ZMQ_CURVE);

                    if (options.mechanism == ZMQ_NULL)
                        memcpy (outpos + outsize, "NULL", 4);
                    else
                    if (options.mechanism == ZMQ_PLAIN)
                        memcpy (outpos + outsize, "PLAIN", 5);
                    else
                        memcpy (outpos + outsize, "CURVE", 5);
                    outsize += 20;
                    memset (outpos + outsize, 0, 32);
                    outsize += 32;
                    greeting_size = v3_greeting_size;
                }
            }
        }
    }

    //  Position of the revision field in the greeting.
    const size_t revision_pos = 10;

    //  Is the peer using ZMTP/1.0 with no revision number?
    //  If so, we send and receive rest of identity message
    if (greeting_recv [0] != 0xff || !(greeting_recv [9] & 0x01)) {
        if (using_norm) {
            //  NORM is not supported in zmq versions to require V1 protocols
            /// TBD - close or ignore the connection instead?
            zmq_assert (false);
        }
        encoder = new (std::nothrow) v1_encoder_t (out_batch_size);
        alloc_assert (encoder);

        decoder = new (std::nothrow) v1_decoder_t (in_batch_size, options.maxmsgsize);
        alloc_assert (decoder);

        //  We have already sent the message header.
        //  Since there is no way to tell the encoder to
        //  skip the message header, we simply throw that
        //  header data away.
        const size_t header_size = options.identity_size + 1 >= 255 ? 10 : 2;
        unsigned char tmp [10], *bufferp = tmp;

        //  Prepare the identity message and load it into encoder.
        //  Then consume bytes we have already sent to the peer.
        const int rc = tx_msg.init_size (options.identity_size);
        zmq_assert (rc == 0);
        memcpy (tx_msg.data (), options.identity, options.identity_size);
        encoder->load_msg (&tx_msg);
        size_t buffer_size = encoder->encode (&bufferp, header_size);
        zmq_assert (buffer_size == header_size);

        //  Make sure the decoder sees the data we have already received.
        inpos = greeting_recv;
        insize = greeting_bytes_read;

        //  To allow for interoperability with peers that do not forward
        //  their subscriptions, we inject a phantom subscription message
        //  message into the incoming message stream.
        if (options.type == ZMQ_PUB || options.type == ZMQ_XPUB)
            subscription_required = true;

        //  VeriSign Custom Code
        //  We are sending our identity now, so what's the next message?
        if (options.keepalive_ivl > 0)
            // In v1, if we want keepalives, send keepalive message to the peer
            read_msg = &stream_object_t::send_keepalive_msg;
        else
            //  The next message will come from the socket.
            read_msg = &stream_object_t::pull_msg_from_session;

        //  We are expecting identity message.
        write_msg = &stream_object_t::write_identity;
    }
    else
    if (greeting_recv [revision_pos] == ZMTP_1_0) {
        encoder = new (std::nothrow) v1_encoder_t (
            out_batch_size);
        alloc_assert (encoder);

        decoder = new (std::nothrow) v1_decoder_t (
            in_batch_size, options.maxmsgsize);
        alloc_assert (decoder);
    }
    else
    if (greeting_recv [revision_pos] == ZMTP_2_0) {
        encoder = new (std::nothrow) v2_encoder_t (out_batch_size);
        alloc_assert (encoder);

        decoder = new (std::nothrow) v2_decoder_t (
            in_batch_size, options.maxmsgsize);
        alloc_assert (decoder);
    }
    else {
        encoder = new (std::nothrow) v2_encoder_t (out_batch_size);
        alloc_assert (encoder);

        decoder = new (std::nothrow) v2_decoder_t (
            in_batch_size, options.maxmsgsize);
        alloc_assert (decoder);

        if (memcmp (greeting_recv + 12, "NULL\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) == 0) {
            mechanism = new (std::nothrow)
                null_mechanism_t (session, peer_address, options);
            alloc_assert (mechanism);
        }
        else
        if (memcmp (greeting_recv + 12, "PLAIN\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) == 0) {
            mechanism = new (std::nothrow)
                plain_mechanism_t (session, peer_address, options);
            alloc_assert (mechanism);
        }
#ifdef HAVE_LIBSODIUM
        else
        if (memcmp (greeting_recv + 12, "CURVE\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) == 0) {
            if (options.as_server)
                mechanism = new (std::nothrow)
                    curve_server_t (session, peer_address, options);
            else
                mechanism = new (std::nothrow) curve_client_t (options);
            alloc_assert (mechanism);
        }
#endif
        else {
            error ();
            return false;
        }
        read_msg = &stream_object_t::next_handshake_command;
        write_msg = &stream_object_t::process_handshake_command;
    }

    // Start polling for output if necessary.
    if (outsize == 0)
        engine_set_pollout ();

    //  Handshaking was successful.
    //  Switch into the normal message flow.
    handshaking = false;
    alloc_assert (encoder);
    alloc_assert (decoder);

    if (has_handshake_timer) {
        cancel_timer (handshake_timer_id);
        has_handshake_timer = false;
    }

    return true;
}

int zmq::stream_object_t::read_identity (msg_t *msg_)
{
    int rc = msg_->init_size (options.identity_size);
    errno_assert (rc == 0);
    if (options.identity_size > 0)
        memcpy (msg_->data (), options.identity, options.identity_size);
    read_msg = &stream_object_t::pull_msg_from_session;
    return 0;
}

int zmq::stream_object_t::write_identity (msg_t *msg_)
{
    if (options.recv_identity) {
        msg_->set_flags (msg_t::identity);
        int rc = session->push_msg (msg_);
        errno_assert (rc == 0);
    }
    else {
        int rc = msg_->close ();
        errno_assert (rc == 0);
        rc = msg_->init ();
        errno_assert (rc == 0);
    }

    if (subscription_required)
        write_msg = &stream_object_t::write_subscription_msg;
    else
        write_msg = &stream_object_t::push_msg_to_session;

    return 0;
}

int zmq::stream_object_t::next_handshake_command (msg_t *msg_)
{
    zmq_assert (mechanism != NULL);

    const int rc = mechanism->next_handshake_command (msg_);
    if (rc == 0) {
        msg_->set_flags (msg_t::command);
        if (mechanism->is_handshake_complete ())
            mechanism_ready ();
    }

    return rc;
}

int zmq::stream_object_t::process_handshake_command (msg_t *msg_)
{
    zmq_assert (mechanism != NULL);
    const int rc = mechanism->process_handshake_command (msg_);
    if (rc == 0) {
        if (mechanism->is_handshake_complete ())
            mechanism_ready ();
        if (output_stopped)
            restart_output ();
    }

    return rc;
}

void zmq::stream_object_t::zap_msg_available ()
{
    zmq_assert (mechanism != NULL);

    const int rc = mechanism->zap_msg_available ();
    if (rc == -1) {
        error ();
        return;
    }
    if (input_stopped)
        restart_input ();
    if (output_stopped)
        restart_output ();
}

void zmq::stream_object_t::mechanism_ready ()
{
    if (options.recv_identity) {
        msg_t identity;
        mechanism->peer_identity (&identity);
        const int rc = session->push_msg (&identity);
        if (rc == -1 && errno == EAGAIN) {
            // If the write is failing at this stage with
            // an EAGAIN the pipe must be being shut down,
            // so we can just bail out of the identity set.
            return;
        }
        errno_assert (rc == 0);
        session->flush ();
    }

    //  VeriSign Custom Code
    if (mechanism->get_keepalive_found ()) {
        //  Both ends support keepalives

        if (mechanism->get_peer_keepalive_ivl () < 0) {
            if (get_io_thread ())
                get_io_thread ()->log ("KEEP: peer Keepalive parameter has bad value");
        } else
            peer_keepalive_ivl = mechanism->get_peer_keepalive_ivl ();

        //  If either end has keepalives enabled, start our timer
        if (options.keepalive_ivl > 0 || peer_keepalive_ivl > 0)
            set_keepalive_timer ();
    }

    read_msg = &stream_object_t::pull_and_encode;
    write_msg = &stream_object_t::write_credential;
}

int zmq::stream_object_t::send_keepalive_msg (msg_t *msg_)
{
    if (!send_keepalive (true, options.keepalive_ivl)) return -1;

    //  The next message will come from the socket.
    read_msg = &stream_object_t::pull_msg_from_session;
    return 0;
}

int zmq::stream_object_t::pull_msg_from_session (msg_t *msg_)
{
    return session->pull_msg (msg_);
}

int zmq::stream_object_t::push_msg_to_session (msg_t *msg_)
{
    return session->push_msg (msg_);
}

int zmq::stream_object_t::write_credential (msg_t *msg_)
{
    zmq_assert (mechanism != NULL);
    zmq_assert (session != NULL);

    const blob_t credential = mechanism->get_user_id ();
    if (credential.size () > 0) {
        msg_t msg;
        int rc = msg.init_size (credential.size ());
        zmq_assert (rc == 0);
        memcpy (msg.data (), credential.data (), credential.size ());
        msg.set_flags (msg_t::credential);
        rc = session->push_msg (&msg);
        if (rc == -1) {
            rc = msg.close ();
            errno_assert (rc == 0);
            return -1;
        }
    }
    write_msg = &stream_object_t::decode_and_push;
    return decode_and_push (msg_);
}

int zmq::stream_object_t::pull_and_encode (msg_t *msg_)
{
    zmq_assert (mechanism != NULL);

    if (session->pull_msg (msg_) == -1)
        return -1;
    if (mechanism->encode (msg_) == -1)
        return -1;
    return 0;
}

int zmq::stream_object_t::decode_and_push (msg_t *msg_)
{
    zmq_assert (mechanism != NULL);

    if (mechanism->decode (msg_) == -1)
        return -1;
    if (session->push_msg (msg_) == -1) {
        if (errno == EAGAIN)
            write_msg = &stream_object_t::push_one_then_decode_and_push;
        return -1;
    }
    return 0;
}

int zmq::stream_object_t::push_one_then_decode_and_push (msg_t *msg_)
{
    const int rc = session->push_msg (msg_);
    if (rc == 0)
        write_msg = &stream_object_t::decode_and_push;
    return rc;
}

int zmq::stream_object_t::write_subscription_msg (msg_t *msg_)
{
    msg_t subscription;

    //  Inject the subscription message, so that also
    //  ZMQ 2.x peers receive published messages.
    int rc = subscription.init_size (1);
    errno_assert (rc == 0);
    *(unsigned char*) subscription.data () = 1;
    rc = session->push_msg (&subscription);
    if (rc == -1)
       return -1;

    write_msg = &stream_object_t::push_msg_to_session;
    return push_msg_to_session (msg_);
}

void zmq::stream_object_t::error ()
{
    if (options.raw_sock) {
        //  For raw sockets, send a final 0-length message to the application
        //  so that it knows the peer has been disconnected.
        msg_t terminator;
        terminator.init();
        (this->*write_msg) (&terminator);
        terminator.close();
    }
    zmq_assert (session);
    socket->event_disconnected (endpoint, engine_get_fd ());
    session->flush ();
    session->engine_error ();
    unplug ();
    delete this;
}

//  VeriSign Custom Code
int zmq::stream_object_t::recv_keepalive_v1 (msg_t *msg_)
{
    if (msg_->size () == keepalive_v1_pkt_size) {
        unsigned keepalive_type =
            get_uint8((unsigned char *)msg_->data ());
        if (keepalive_type == keepalive_v1_response) {
            int keepalive_ivl_old = peer_keepalive_ivl;
            peer_keepalive_ivl =
                get_uint32 (((unsigned char *)msg_->data ()) + 1);
            if (plugged && peer_keepalive_ivl != keepalive_ivl_old) {
                set_keepalive_timer ();
            }
        } else if (get_io_thread ()) {
            get_io_thread ()->log ("KEEP: keepalive message has bad type");
            errno = EPROTO;
            return -1;
        }
    } else if (get_io_thread ()) {
        get_io_thread ()->log ("KEEP: keepalive message has bad length");
        errno = EPROTO;
        return -1;
    }
    return 0;
}

//  VeriSign Custom Code
int zmq::stream_object_t::format_keepalive_v1 (unsigned char *msg_data,
                                               int keepalive_ivl)
{
    msg_data[0] = keepalive_v1_pkt_size + 1;        // one byte message size
    msg_data[1] = msg_t::keepalive_v1;              // flags
    put_uint8(msg_data + 2, keepalive_v1_response); // keepalive type
    put_uint32(msg_data + 3, keepalive_ivl);        // keepalive interval in ms

    return keepalive_v1_pkt_size + 2;
}

//  VeriSign Custom Code
bool zmq::stream_object_t::send_keepalive (bool force,
                                           int keepalive_ivl)
{
    // cannot send keepalive if engine is not plugged
    if (unlikely (!plugged))
        return false;

    if (force && outsize) {
        zmq_assert (protocol_version < ZMTP_2_0);
        // This can happen only when called from handshake () for v1 protocol,
        // so we can safely assume that the output buffer in use has enough
        // unused space for us to append a keepalive message without checking.
        outsize += format_keepalive_v1 (outpos + outsize, keepalive_ivl);

    } else if (!outsize) {
        // We normally only send a keepalive if there's nothing buffered

        int msg_size = 0;
        // restart_output () calls out_event () calls write (), which should
        // accept all of outsize since we haven't sent anything in long enough
        // to need a keepalive, but use a buffer in our heap instead of on the
        // stack for the keepalive, to handle the worst case of output stalled.
        unsigned char *msg_ptr = keepalive_msg_data;

        if (protocol_version < ZMTP_2_0) {
            msg_size = format_keepalive_v1(msg_ptr, keepalive_ivl);

        } else {
            //  Prepare PONG command msg and encode it.
            const int rc = tx_msg.init_size (5);
            zmq_assert (rc == 0);
            tx_msg.set_flags (msg_t::command);
            memcpy (tx_msg.data (), "\4PONG", 5);
            if (encoder->get_in_progress () == 0) encoder->load_msg (&tx_msg);
            msg_size = encoder->encode (&msg_ptr, keepalive_msg_data_max);
        }

        // nudge output to get the keepalive sent
        outpos = msg_ptr;
        outsize = msg_size;
        restart_output ();
    }
    return true;
}

//  VeriSign Custom Code
void zmq::stream_object_t::set_keepalive_timer ()
{
    int keepalive_ivl_old = keepalive_ivl_time;

    // derive keepalive_ivl_time from options.keepalive_ivl, peer_keepalive_ivl
    if (options.keepalive_ivl > 0 || peer_keepalive_ivl > 0) {
        // Shouldn't be possible, but prevent keepalive intervals below .1 sec
        if (options.keepalive_ivl < 0) options.keepalive_ivl = 0;
        if (options.keepalive_ivl > 0 && options.keepalive_ivl < 100)
            options.keepalive_ivl = 100;
        if (peer_keepalive_ivl < 0) peer_keepalive_ivl = 0;
        if (peer_keepalive_ivl > 0 &&
            peer_keepalive_ivl < 100)
            peer_keepalive_ivl = 100;

        int time_in = options.keepalive_ivl / 4;
        int time_out = peer_keepalive_ivl / 4;
        // Pick the lower of the non-zero in and out times
        int time = (!time_in ? time_out :
                    (!time_out || time_out > time_in ? time_in : time_out));
        // round inbound limit up, outbound limit down (and half as long)
        keepalive_limit_in = (options.keepalive_ivl + (time - 1)) / time;
        keepalive_limit_out = (peer_keepalive_ivl + 1) / (time * 2);
        keepalive_ticks_in = 0;
        keepalive_ticks_out = 0;
        keepalive_ivl_time = time;
        if (get_io_thread ()) {
            char buf[120];
            sprintf(buf,
#ifdef __64BIT__
                    "KEEP: set timer=%d local=%d(%lu) peer=%d(%lu)",
#else
                    "KEEP: set timer=%d local=%d(%llu) peer=%d(%llu)",
#endif
                    time, options.keepalive_ivl, keepalive_limit_in,
                    peer_keepalive_ivl, keepalive_limit_out);
            get_io_thread ()->log (buf);
        }
    } else {
        keepalive_ivl_time = 0;
    }

    if (keepalive_ivl_old != keepalive_ivl_time ||
        has_keepalive_timer != (keepalive_ivl_time > 0)) {
#ifdef UNUSED
        // optional internal log message for debugging
        if (get_io_thread ()) {
            char buf[80];
            sprintf(buf, "KEEP: adjust timer %d != %d || %d != %d",
                    keepalive_ivl_old, keepalive_ivl_time,
                    (int)has_keepalive_timer, (int)(keepalive_ivl_time > 0));
            get_io_thread ()->log (buf);
        }
#endif
        if (has_keepalive_timer) {
            cancel_timer (keepalive_timer_id);
            has_keepalive_timer = false;
        }
        if (keepalive_ivl_time > 0) {
            add_timer (keepalive_ivl_time, keepalive_timer_id);
            has_keepalive_timer = true;
        }
    }
}

//  VeriSign Custom Code
void zmq::stream_object_t::set_handshake_timer ()
{
    zmq_assert (!has_handshake_timer);

    if (!options.raw_sock && options.handshake_ivl > 0) {
        add_timer (options.handshake_ivl, handshake_timer_id);
        has_handshake_timer = true;
    }

}

//  VeriSign Custom Code
void zmq::stream_object_t::timer_event (int id_)
{
    // init timer runs at first, keepalive timer after that, never both at once
    if (unlikely (id_ == handshake_timer_id)) {
        zmq_assert (has_handshake_timer);
        has_handshake_timer = false;
        if (get_io_thread ())
            get_io_thread ()->log ("INIT: handshake timeout, detach socket");
        error ();
        return;
    }

    zmq_assert (has_keepalive_timer);
    has_keepalive_timer = false;
 
    //  keepalive timer expired.
    zmq_assert (id_ == keepalive_timer_id);

    if (keepalive_limit_in && ++keepalive_ticks_in > keepalive_limit_in) {
        if (get_io_thread ())
            get_io_thread ()->log ("KEEP: keepalive timeout, detach socket");
        error ();
        return;
    }

    if (unlikely (session && !session->has_pipe ())) {
        if (get_io_thread ())
            get_io_thread ()->log ("KEEP: session pipe closed, detach socket");
        error ();
        return;
    }

    if (keepalive_limit_out && ++keepalive_ticks_out > keepalive_limit_out) {
        send_keepalive (false, options.keepalive_ivl);
    }

    if (likely (keepalive_ivl_time > 0)) {
        add_timer (keepalive_ivl_time, keepalive_timer_id);
        has_keepalive_timer = true;
    }
}
