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

#include "stream_engine.hpp"

zmq::stream_engine_t::stream_engine_t (zmq::fd_t fd_,
                                       const options_t &options_, 
                                       const std::string &endpoint_) :
    stream_object_t(options_, endpoint_),
    s (fd_),
    handle(0)
{
    //  Put the socket into non-blocking mode.
    unblock_socket (s);

    int family = get_peer_ip_address (s, peer_address);
    if (family == 0)
        peer_address = "";
#if defined ZMQ_HAVE_SO_PEERCRED
    else
    if (family == PF_UNIX) {
        struct ucred cred;
        socklen_t size = sizeof (cred);
        if (!getsockopt (s, SOL_SOCKET, SO_PEERCRED, &cred, &size)) {
            std::ostringstream buf;
            buf << ":" << cred.uid << ":" << cred.gid << ":" << cred.pid;
            peer_address += buf.str ();
        }
    }
#elif defined ZMQ_HAVE_LOCAL_PEERCRED
    else
    if (family == PF_UNIX) {
        struct xucred cred;
        socklen_t size = sizeof (cred);
        if (!getsockopt (s, 0, LOCAL_PEERCRED, &cred, &size)
                && cred.cr_version == XUCRED_VERSION) {
            std::ostringstream buf;
            buf << ":" << cred.cr_uid << ":";
            if (cred.cr_ngroups > 0)
                buf << cred.cr_groups[0];
            buf << ":";
            peer_address += buf.str ();
        }
    }
#endif

#ifdef SO_NOSIGPIPE
    //  Make sure that SIGPIPE signal is not generated when writing to a
    //  connection that was already closed by the peer.
    int set = 1;
    rc = setsockopt (s, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof (int));
    errno_assert (rc == 0);
#endif
}

zmq::stream_engine_t::~stream_engine_t ()
{
    zmq_assert (!plugged);

    if (s != retired_fd) {
#ifdef ZMQ_HAVE_WINDOWS
        int rc = closesocket (s);
        wsa_assert (rc != SOCKET_ERROR);
#else
        int rc = close (s);
        errno_assert (rc == 0);
#endif
        s = retired_fd;
    }
}

void zmq::stream_engine_t::engine_add_fd ()
{
    zmq_assert (!handle);
    handle = add_fd (s);
}

void zmq::stream_engine_t::engine_rm_fd ()
{
    zmq_assert (handle);
    rm_fd (handle);
}

void zmq::stream_engine_t::engine_set_pollin ()
{
    zmq_assert (handle);
    set_pollin (handle);
}

void zmq::stream_engine_t::engine_reset_pollin ()
{
    zmq_assert (handle);
    reset_pollin (handle);
}

void zmq::stream_engine_t::engine_set_pollout ()
{
    zmq_assert (handle);
    set_pollout (handle);
}

void zmq::stream_engine_t::engine_reset_pollout ()
{
    zmq_assert (handle);
    reset_pollout (handle);
}

zmq::fd_t zmq::stream_engine_t::engine_get_fd ()
{
    return s;
}

int zmq::stream_engine_t::engine_in_event()
{
    return 0;
}

int zmq::stream_engine_t::engine_out_event()
{
    return 0;
}

int zmq::stream_engine_t::write (const void *data_, size_t size_)
{
#ifdef ZMQ_HAVE_WINDOWS

    int nbytes = send (s, (char*) data_, (int) size_, 0);

    //  If not a single byte can be written to the socket in non-blocking mode
    //  we'll get an error (this may happen during the speculative write).
    if (nbytes == SOCKET_ERROR && WSAGetLastError () == WSAEWOULDBLOCK)
        return 0;
        
    //  Signalise peer failure.
    if (nbytes == SOCKET_ERROR && (
          WSAGetLastError () == WSAENETDOWN ||
          WSAGetLastError () == WSAENETRESET ||
          WSAGetLastError () == WSAEHOSTUNREACH ||
          WSAGetLastError () == WSAECONNABORTED ||
          WSAGetLastError () == WSAETIMEDOUT ||
          WSAGetLastError () == WSAECONNRESET))
        return -1;

    wsa_assert (nbytes != SOCKET_ERROR);
    return nbytes;

#else

    ssize_t nbytes = send (s, data_, size_, 0);

    //  Several errors are OK. When speculative write is being done we may not
    //  be able to write a single byte from the socket. Also, SIGSTOP issued
    //  by a debugging tool can result in EINTR error.
    if (nbytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK ||
          errno == EINTR))
        return 0;

    //  Signalise peer failure.
    if (nbytes == -1) {
        errno_assert (errno != EACCES
                   && errno != EBADF
                   && errno != EDESTADDRREQ
                   && errno != EFAULT
                   && errno != EINVAL
                   && errno != EISCONN
                   && errno != EMSGSIZE
                   && errno != ENOMEM
                   && errno != ENOTSOCK
                   && errno != EOPNOTSUPP);
        return -1;
    }

    return static_cast <int> (nbytes);

#endif
}

int zmq::stream_engine_t::read (void *data_, size_t size_)
{
#ifdef ZMQ_HAVE_WINDOWS

    const int rc = recv (s, (char*) data_, (int) size_, 0);

    //  If not a single byte can be read from the socket in non-blocking mode
    //  we'll get an error (this may happen during the speculative read).
    if (rc == SOCKET_ERROR) {
        if (WSAGetLastError () == WSAEWOULDBLOCK)
            errno = EAGAIN;
        else {
            wsa_assert (WSAGetLastError () == WSAENETDOWN
                     || WSAGetLastError () == WSAENETRESET
                     || WSAGetLastError () == WSAECONNABORTED
                     || WSAGetLastError () == WSAETIMEDOUT
                     || WSAGetLastError () == WSAECONNRESET
                     || WSAGetLastError () == WSAECONNREFUSED
                     || WSAGetLastError () == WSAENOTCONN);
            errno = wsa_error_to_errno (WSAGetLastError ());
        }
    }

    return rc == SOCKET_ERROR? -1: rc;

#else

    const ssize_t rc = recv (s, data_, size_, 0);

    //  Several errors are OK. When speculative read is being done we may not
    //  be able to read a single byte from the socket. Also, SIGSTOP issued
    //  by a debugging tool can result in EINTR error.
    if (rc == -1) {
        errno_assert (errno != EBADF
                   && errno != EFAULT
                   && errno != EINVAL
                   && errno != ENOMEM
                   && errno != ENOTSOCK);
        if (errno == EWOULDBLOCK || errno == EINTR)
            errno = EAGAIN;
    }

    return static_cast <int> (rc);

#endif
}
