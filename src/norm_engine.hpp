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

#ifndef __ZMQ_NORM_ENGINE_HPP_INCLUDED__
#define __ZMQ_NORM_ENGINE_HPP_INCLUDED__

#include "platform.hpp"

#if defined ZMQ_HAVE_NORM

#include "norm_address.hpp"
#include "stream_object.hpp"
#include "options.hpp"
#include "i_decoder.hpp"
#include "v2_encoder.hpp"

#include <normApi.h>

namespace zmq
{
    class io_thread_t;
    class session_base_t;
    class socket_base_t;
    
    class norm_engine_t : public stream_object_t
    {
    public:
        norm_engine_t (zmq::socket_base_t *socket_, const options_t &options_);
        norm_engine_t (zmq::socket_base_t *socket_, const options_t &options_,
                       const char* network_, bool send, bool recv);
        norm_engine_t (zmq::socket_base_t *socket_, const options_t &options_,
                       NormInstanceHandle norm_instance_,
                       NormSessionHandle norm_session_,
                       NormNodeHandle normNodeHandle_,
                       norm_address_t &client_norm_address_,
                       norm_address_t &listen_norm_address_);
        ~norm_engine_t ();
            
        // create NORM instance, session, etc for multicast session: pub/sub
        int init (const char* network_, bool send, bool recv);

        // used by norm_listener to start engine for incoming unicast session
        int init (NormInstanceHandle norm_instance_,
                  NormSessionHandle norm_session_,
                  NormNodeHandle normNodeHandle_,
                  norm_address_t &client_norm_address_,
                  norm_address_t &listen_norm_address_);

        void shutdown ();
            
        //  i_engine interface implementation.
        //  Plug the engine to the session.
        virtual void plug (zmq::io_thread_t *io_thread_,
                           class session_base_t *session_);

        //  Terminate and deallocate the engine. Note that 'detached'
        //  events are not fired on termination.
        virtual void terminate ();

        //  This method is called by the session to signal that more
        //  messages can be written to the pipe.
        virtual void restart_input ();

        //  This method is called by the session to signal that there
        //  are messages to send available.
        virtual void restart_output ();

        virtual void zap_msg_available () {};
            
        // i_poll_events interface implementation.
        // (we only need in_event() for NormEvent notification)
        // (i.e., don't have any output events or timers (yet))
        // void in_event ();
            
    private:
        void unplug ();
        void send_data ();
        unsigned int stream_write (const char* buffer, unsigned int numBytes);
        void stream_flush (bool eom, NormFlushMode flushMode);
        void recv_data (NormObjectHandle stream);    

        // write & read are unused in norm_engine
        int write (const void *data_, size_t size_);
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

        enum {BUFFER_SIZE = 2048};
                   
        // Used to keep track of streams from multiple senders     
        class NormRxStreamState
        {
        public:
            NormRxStreamState (NormObjectHandle normStream,
                               int64_t          maxMsgSize);
            ~NormRxStreamState ();
                    
            NormObjectHandle GetStreamHandle() const { return norm_stream; }
                    
            bool Init(bool is_twoway_);
                    
            void SetRxReady(bool state) { rx_ready = state; }
            bool IsRxReady () const { return rx_ready; }
                    
            void SetSync (bool state) { in_sync = state; }
            bool InSync () const { return in_sync; }
                    
            // These are used to feed data to decoder
            // and its underlying "msg" buffer
            char* AccessBuffer () {
                return (char*)(buffer_ptr + buffer_count); }
            size_t GetBytesNeeded () const {
                return (buffer_size - buffer_count); }
            void IncrementBufferCount (size_t count) { buffer_count += count; }
            msg_t* AccessMsg () { return zmq_decoder->msg(); }

            // This invokes the decoder "decode" method
            // returning 0 if more data is needed,
            // 1 if the message is complete, If an error
            // occurs the 'sync' is dropped and the
            // decoder re-initialized
            int Decode (bool is_twoway_);
                    
            class List
            {
            public:
                List();
                ~List();
                            
                void Append (NormRxStreamState& item);
                void Remove (NormRxStreamState& item);
                            
                bool IsEmpty () const { return (NULL == head); }
                            
                void Destroy ();
                            
                class Iterator
                {
                public:
                    Iterator (const List& list);
                    NormRxStreamState* GetNextItem ();
                private:
                    NormRxStreamState* next_item;
                };
                friend class Iterator;
                            
            private:
                NormRxStreamState*  head;
                NormRxStreamState*  tail;        
                                
            };  // end class zmq::norm_engine_t::NormRxStreamState::List
                    
            friend class List;
                    
            List* AccessList() { return list; }
                    
                    
        private:
            NormObjectHandle            norm_stream;
            int64_t                     max_msg_size;
            bool                        in_sync; 
            bool                        rx_ready;
            i_decoder*                  zmq_decoder;
            bool                        skip_norm_sync;
            unsigned char*              buffer_ptr;
            size_t                      buffer_size;
            size_t                      buffer_count;
                    
            NormRxStreamState*          prev;
            NormRxStreamState*          next;
            NormRxStreamState::List*    list;
                
        };  // end class zmq::norm_engine_t::NormRxStreamState
            
        /// session_base_t*         zmq_session;
        /// #define zmq_session session
        socket_base_t*          socket;
        options_t               options;
        NormInstanceHandle      norm_instance;
        fd_t                    norm_descriptor;
        handle_t                norm_descriptor_handle;
        NormSessionHandle       norm_session;
        norm_address_t          norm_address;
        bool                    norm_plugged;
        bool                    is_unicast;
        bool                    is_sender;
        bool                    is_receiver;
        bool                    is_twoway;
        bool                    is_accept;
            
        // Sender state
        msg_t                   tx_msg;
        v2_encoder_t            zmq_encoder;    // for tx messages (we use v2 for now)  
        NormObjectHandle        norm_tx_stream;
        bool                    tx_first_msg;
        bool                    tx_more_bit;
        bool                    zmq_output_ready; // zmq has msg(s) to send 
        bool                    norm_tx_ready;    // norm has tx queue vacancy
        // tbd - may not need buffer if can access zmq message buffer directly?
        char                    tx_buffer[BUFFER_SIZE];
        unsigned int            tx_index;
        unsigned int            tx_len;

        // Sender variables below used to manage ack-based stream flow control.
        // (norm_acking needs to be set "true" and at least one node id added
        //  via NormAddAckingNode() for this stuff to kick into action.)
        // This makes the NORM sender flow controlled similar to a TCP socket.
        bool                    norm_acking;
        bool                    norm_watermark_pending;
        UINT16                  norm_segment_size;
        unsigned int            norm_stream_buffer_max;
        unsigned int            norm_stream_buffer_count;
        unsigned int            norm_stream_bytes_remain;
        unsigned int            norm_ack_retry_max;
        unsigned int            norm_ack_retry_count;
            
        // Receiver state
        bool                    zmq_input_ready; // zmq ready to receive msg(s)
        // Lists of norm rx streams from remote senders
        // rx streams waiting for data reception
        NormRxStreamState::List rx_pending_list;
        // rx streams ready for NormStreamRead()
        NormRxStreamState::List rx_ready_list;
        // rx streams w/ msg ready for push to zmq
        NormRxStreamState::List msg_ready_list;

    };  // end class norm_engine_t
}

#endif // ZMQ_HAVE_NORM

#endif // !__ZMQ_NORM_ENGINE_HPP_INCLUDED__
