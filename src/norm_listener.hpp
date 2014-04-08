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

#ifndef __ZMQ_NORM_LISTENER_HPP_INCLUDED__
#define __ZMQ_NORM_LISTENER_HPP_INCLUDED__

#if defined ZMQ_HAVE_NORM

#include "fd.hpp"
#include "own.hpp"
#include "stdint.hpp"
#include "io_object.hpp"
#include "norm_address.hpp"
#include "../include/zmq.h"

#ifdef ZMQ_DEBUG_NORM
#include "v2_decoder.hpp"
#endif

#include <normApi.h>

namespace zmq
{

    class io_thread_t;
    class socket_base_t;

    class norm_listener_t : public own_t, public io_object_t
    {
    public:

        norm_listener_t (zmq::io_thread_t *io_thread_,
            zmq::socket_base_t *socket_, const options_t &options_);
        ~norm_listener_t ();

        //  Set address to listen on.
        int set_address (const char *addr_);

        // Get the bound address for use with wildcard
        int get_address (std::string &addr_);

    private:

        //  Handlers for incoming commands.
        void process_plug ();
        void process_term (int linger_);

        //  Handlers for I/O events.
        void in_event ();

        void recv_data (NormObjectHandle stream);    

        //  Close the listening socket.
        void close ();

        void shutdown();

        //  Discover a new node "connection". Returns the NormNodeHandle of the
        //  newly discovered node. The function may return NORM_NODE_INVALID
        //  if the node has become unreachable in the mean time (address or
        //  NormNodeId unresolvable?) or was denied because of accept filters.
        NormNodeHandle accept ();

        //  Underlying socket.
        fd_t s;

        //  Handle corresponding to the listening socket.
        handle_t handle;

        //  Socket the listerner belongs to.
        zmq::socket_base_t *socket;

        //  Address to listen on.
        norm_address_t listen_address;

        NormInstanceHandle listen_instance;

        //  NormSessionHandle to look for new nodes on.
        NormSessionHandle listen_session;

        // ascending session_id's to assign to connecting client sessions
        NormSessionId session_id;

        // String representation of endpoint to bind to
        std::string endpoint;

#ifdef ZMQ_DEBUG_NORM
        bool is_twoway;

        // Used to keep track of streams from multiple senders     
        class NormRxStreamState
        {
        public:
            NormRxStreamState (NormObjectHandle normStream,
                               int64_t          maxMsgSize);
            ~NormRxStreamState ();
                    
            NormObjectHandle GetStreamHandle() const { return norm_stream; }
                    
            bool Init();
                    
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
            int Decode ();
                    
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
                                
            };  // end class zmq::norm_listener_t::NormRxStreamState::List
                    
            friend class List;
                    
            List* AccessList() { return list; }
                    
                    
        private:
            NormObjectHandle            norm_stream;
            int64_t                     max_msg_size;
            bool                        in_sync; 
            bool                        rx_ready;
            v2_decoder_t*               zmq_decoder;
            bool                        skip_norm_sync;
            unsigned char*              buffer_ptr;
            size_t                      buffer_size;
            size_t                      buffer_count;
                    
            NormRxStreamState*          prev;
            NormRxStreamState*          next;
            NormRxStreamState::List*    list;
                
        };  // end class zmq::norm_listener_t::NormRxStreamState

        // Receiver state
        bool                    zmq_input_ready; // zmq ready to receive msg(s)
        // Lists of norm rx streams from remote senders
        // rx streams waiting for data reception
        NormRxStreamState::List rx_pending_list;
        // rx streams ready for NormStreamRead()
        NormRxStreamState::List rx_ready_list;
        // rx streams w/ msg ready for push to zmq
        NormRxStreamState::List msg_ready_list;
#endif

        norm_listener_t (const norm_listener_t&);
        const norm_listener_t &operator = (const norm_listener_t&);
    };

}

#endif

#endif
