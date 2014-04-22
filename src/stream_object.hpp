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

#ifndef __ZMQ_STREAM_OBJECT_HPP_INCLUDED__
#define __ZMQ_STREAM_OBJECT_HPP_INCLUDED__

#include <stddef.h>

#include "fd.hpp"
#include "i_engine.hpp"
#include "io_object.hpp"
#include "i_encoder.hpp"
#include "i_decoder.hpp"
#include "options.hpp"
#include "socket_base.hpp"
#include "../include/zmq.h"

namespace zmq
{
    //  Protocol revisions
    enum
    {
        ZMTP_1_0 = 0,
        ZMTP_2_0 = 1
    };

    class io_thread_t;
    class msg_t;
    class session_base_t;
    class mechanism_t;

    //  This object is a base class for transports with stream-like semantics,
    //  without requiring that the transport be file descriptor-based.
    //  It encapsulates the functionality that isn't engine-specific.

    class stream_object_t : public io_object_t, public i_engine
    {
    public:

        stream_object_t (const options_t &options_, 
                         const std::string &endpoint);
        ~stream_object_t ();

        //  i_engine interface implementation.
        virtual void plug (zmq::io_thread_t *io_thread_,
                           zmq::session_base_t *session_);
        virtual void terminate ();
        virtual void restart_input ();
        virtual void restart_output ();
        virtual void zap_msg_available ();

        //  i_poll_events interface implementation.
        void in_event ();
        void out_event ();
        void timer_event (int token);

        //  Writes data to the transport. Returns the number of bytes actually
        //  written (even zero is to be considered to be a success). In case
        //  of error or orderly shutdown by the other peer -1 is returned.
        virtual int write (const void *data_, size_t size_) = 0;

        //  Reads data from the transport (up to 'size' bytes).
        //  Returns the number of bytes actually read or -1 on error.
        //  Zero indicates the peer has closed the connection.
        virtual int read (void *data_, size_t size_) = 0;

        virtual void engine_add_fd () = 0;
        virtual void engine_rm_fd () = 0;
        virtual void engine_set_pollin () = 0;
        virtual void engine_reset_pollin () = 0;
        virtual void engine_set_pollout () = 0;
        virtual void engine_reset_pollout () = 0;
        virtual fd_t engine_get_fd () = 0;
        virtual int engine_in_event () = 0;
        virtual int engine_out_event () = 0;

        void set_using_norm (bool using_norm_) { using_norm = using_norm_; }

    protected:  /// private:

        //  Unplug the engine from the session.
        void unplug ();

        //  Function to handle transport connection disconnections.
        void error ();

        //  Receives the greeting message from the peer.
        int receive_greeting ();

        //  Detects the protocol used by the peer.
        bool handshake ();

        int read_identity (msg_t *msg_);
        int write_identity (msg_t *msg_);

        int next_handshake_command (msg_t *msg);
        int process_handshake_command (msg_t *msg);

        int pull_msg_from_session (msg_t *msg_);
        int push_msg_to_session (msg_t *msg);

        int write_credential (msg_t *msg_);
        int pull_and_encode (msg_t *msg_);
        int decode_and_push (msg_t *msg_);
        int push_one_then_decode_and_push (msg_t *msg_);

        void mechanism_ready ();

        int write_subscription_msg (msg_t *msg_);

        size_t add_property (unsigned char *ptr,
            const char *name, const void *value, size_t value_len);

        //  VeriSign Custom Code
        //  Functions to start/stop/adjust keepalive timer
        int recv_keepalive_v1 (msg_t *msg_);

        int send_keepalive_msg (msg_t *msg_);

        void set_keepalive_timer ();

        bool send_keepalive (bool force, int keepalive_ivl);

        int  format_keepalive_v1 (unsigned char *msg_data,
                                  int keepalive_ivl);

        //  Function to start handshake timer
        void set_handshake_timer ();

        //  True iff this is server's engine.
        bool as_server;

        msg_t tx_msg;

        /// handle_t handle;

        unsigned char *inpos;
        size_t insize;
        i_decoder *decoder;

        unsigned char *outpos;
        size_t outsize;
        i_encoder *encoder;

        //  When true, we are still trying to determine whether
        //  the peer is using versioned protocol, and if so, which
        //  version.  When false, normal message flow has started.
        bool handshaking;

        static const size_t signature_size = 10;

        //  Size of ZMTP/1.0 and ZMTP/2.0 greeting message
        static const size_t v2_greeting_size = 12;

        //  Size of ZMTP/3.0 greeting message
        static const size_t v3_greeting_size = 64;

        //  Expected greeting size.
        size_t greeting_size;

        //  Max size of keepalive messages
        static const size_t keepalive_msg_data_size = 128;
        static const size_t keepalive_msg_data_max =
            (keepalive_msg_data_size + (2 * v3_greeting_size));
        // max(keepalive_v1_pkt_size + 2 for v1, guess 256 max for v2) = 256,
        // but greeting_recv/send are unused after handshake, so allocate 128
        // and let msg buffering slop into greeting_recv/send if it needs to.
        unsigned char keepalive_msg_data[keepalive_msg_data_size];

        //  Greeting received from, and sent to peer
        //  Add a few slop bytes: norm definately needs 1 byte in greeting_send
        unsigned char greeting_recv [v3_greeting_size + 2];
        unsigned char greeting_send [v3_greeting_size + 2];


        //  Size of greeting received so far
        unsigned int greeting_bytes_read;

        //  The session this engine is attached to.
        zmq::session_base_t *session;

        //  VeriSign Custom Code
        //  The other ends ZMQ_KEEPALIVE_IVL options time, in milliseconds.
        //  Other end may disconnect if nothing received from us for this long.
        //  Default is 0 (keepalives not enabled/required by other end)
        int peer_keepalive_ivl;

        //  keepalive timer interval, in milliseconds
        //  Default 0 (keepalives not enabled by either end, so no timer)
        int keepalive_ivl_time;

        //  ID of the keepalive timer
        enum {keepalive_timer_id = 0x40,
              keepalive_v1_pkt_size = sizeof(uint8_t) + sizeof(uint32_t),
        //    keepalive_request = 1, //  not used
              keepalive_v1_response = 2};

        //  How many many keepalive_ivl_times, since somthing was last received
        uint64_t keepalive_ticks_in;
        //  If nothing rcvd for this many keepalive_ivl_times, close connection
        uint64_t keepalive_limit_in;

        //  How many many keepalive_ivl_times, since somthing was last sent
        uint64_t keepalive_ticks_out;
        //  If nothing sent for this many keepalive_ivl_times, send a keepalive
        uint64_t keepalive_limit_out;

        // which protocol version does this peer use: -1 == unversioned
        char protocol_version;
        //  end of VeriSign Custom Code

        options_t options;

        // String representation of endpoint
        std::string endpoint;

        //  VeriSign Custom Code
        //  Timer is running.
        bool has_keepalive_timer;

        //  ID of the handshake timer
        enum {handshake_timer_id = 0x41};

        //  init timer is running.
        bool has_handshake_timer;

        bool using_norm;  // do message framing for norm, requires v3 only

        unsigned char last_msg_flags;  // used only when using_norm

        bool plugged;

        int (stream_object_t::*read_msg) (msg_t *msg_);

        int (stream_object_t::*write_msg) (msg_t *msg_);

        bool io_error;

        //  Indicates whether the engine is to inject a phantom
        //  subscription message into the incoming stream.
        //  Needed to support old peers.
        bool subscription_required;

        mechanism_t *mechanism;

        //  True iff the engine couldn't consume the last decoded message.
        bool input_stopped;

        //  True iff the engine doesn't have any message to encode.
        bool output_stopped;

        // Socket
        zmq::socket_base_t *socket;

        std::string peer_address;

        stream_object_t (const stream_object_t&);
        const stream_object_t &operator = (const stream_object_t&);
    };

}

#endif
