//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "TcpStream.h"

#include <cstring>

#ifndef _WIN32
#include <sys/un.h>
#else
#include <afunix.h>
#endif

#include "IoAwaiters.h"
#include "PlatformNet.h"
#include "SocketException.h"

namespace Soccer {

    namespace {
        /**
         * @brief Put @p fd into non-blocking mode. Throws @c SocketException
         *        on the underlying syscall failure (fcntl on POSIX,
         *        ioctlsocket on Windows).
         */
        void set_nonblocking(int fd) {
            if (YarnBall::setSocketNonblocking(fd) != 0) {
                throw SocketException("set_nonblocking", YarnBall::lastSocketError());
            }
        }
    }

    void TcpStream::close() noexcept {
        if (this->sock >= 0) {
            (void) YarnBall::closeSocket(this->sock);
            this->sock = -1;
        }
    }

    YarnBall::Task<std::size_t> TcpStream::read(std::span<std::byte> buf) {
        if (this->sock < 0) {
            throw SocketException("read on closed TcpStream");
        }
        while (true) {
            // recv returns ssize_t on POSIX and int on WinSock; either fits
            // in the YarnBall::ssize_t alias.
            ::ssize_t n;
            do {
                n = ::recv(this->sock,
                           reinterpret_cast<char *>(buf.data()),
                           static_cast<int>(buf.size()), 0);
            } while (n < 0 && YarnBall::isEintr(YarnBall::lastSocketError()));

            if (n >= 0) co_return static_cast<std::size_t>(n);
            const int err = YarnBall::lastSocketError();
            if (YarnBall::isWouldBlock(err)) {
                co_await YarnBall::io::waitReadable(this->sock);
                continue;
            }
            throw SocketException("recv", err);
        }
    }

    YarnBall::Task<std::size_t> TcpStream::write(std::span<const std::byte> buf) {
        if (this->sock < 0) {
            throw SocketException("write on closed TcpStream");
        }
        std::size_t total = 0;
        const auto *p = buf.data();
        while (total < buf.size()) {
            ::ssize_t n;
            do {
                n = ::send(this->sock,
                           reinterpret_cast<const char *>(p + total),
                           static_cast<int>(buf.size() - total), 0);
            } while (n < 0 && YarnBall::isEintr(YarnBall::lastSocketError()));

            if (n >= 0) {
                if (n == 0) break;
                total += static_cast<std::size_t>(n);
                continue;
            }
            const int err = YarnBall::lastSocketError();
            if (YarnBall::isWouldBlock(err)) {
                co_await YarnBall::io::waitWritable(this->sock);
                continue;
            }
            throw SocketException("send", err);
        }
        co_return total;
    }

    namespace {
        /**
         * @brief Same as @c fillUnixAddr in TcpListener.cpp; duplicated
         *        here rather than exposed because @c sockaddr_un should
         *        stay an internal detail of the .cpp files.
         */
        socklen_t fillUnixAddrConnect(sockaddr_un *addr, const std::string &path) {
            std::memset(addr, 0, sizeof(*addr));
            addr->sun_family = AF_UNIX;
            if (path.size() >= sizeof(addr->sun_path)) {
                throw SocketException("AF_UNIX path too long for sun_path: " + path);
            }
            std::memcpy(addr->sun_path, path.c_str(), path.size());
            return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
        }
    }

    YarnBall::Task<TcpStream> tcpConnectUnix(std::string socketPath) {
        YarnBall::ensureWsaStarted();

        const int fd = static_cast<int>(::socket(AF_UNIX, SOCK_STREAM, 0));
        if (fd < 0) throw SocketException("socket(AF_UNIX)", YarnBall::lastSocketError());

        try {
            set_nonblocking(fd);
        } catch (...) {
            (void) YarnBall::closeSocket(fd);
            throw;
        }

        sockaddr_un addr{};
        const socklen_t addrLen = fillUnixAddrConnect(&addr, socketPath);

        int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), addrLen);
        if (rc < 0) {
            const int err = YarnBall::lastSocketError();
            if (!YarnBall::isInProgress(err)) {
                (void) YarnBall::closeSocket(fd);
                throw SocketException("connect(AF_UNIX)", err);
            }
            co_await YarnBall::io::waitWritable(fd);
            int sockerr = 0;
            socklen_t errlen = sizeof(sockerr);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<char *>(&sockerr), &errlen) < 0) {
                const int e = YarnBall::lastSocketError();
                (void) YarnBall::closeSocket(fd);
                throw SocketException("getsockopt(SO_ERROR)", e);
            }
            if (sockerr != 0) {
                (void) YarnBall::closeSocket(fd);
                throw SocketException("connect(AF_UNIX, async)", sockerr);
            }
        }

        co_return TcpStream(fd);
    }

    YarnBall::Task<TcpStream> tcpConnect(std::string host, std::uint16_t port) {
        YarnBall::ensureWsaStarted();
        auto addr = SocketAddress::resolve(host, port);

        const int fd = static_cast<int>(::socket(addr.family(), SOCK_STREAM, 0));
        if (fd < 0) throw SocketException("socket", YarnBall::lastSocketError());

        try {
            set_nonblocking(fd);
        } catch (...) {
            (void) YarnBall::closeSocket(fd);
            throw;
        }

        int rc = ::connect(fd, addr.data(), addr.length());
        if (rc < 0) {
            const int err = YarnBall::lastSocketError();
            if (!YarnBall::isInProgress(err)) {
                (void) YarnBall::closeSocket(fd);
                throw SocketException("connect", err);
            }
            // EINPROGRESS / WSAEWOULDBLOCK: wait for writability, then
            // check SO_ERROR to see whether the handshake completed.
            co_await YarnBall::io::waitWritable(fd);
            int sockerr = 0;
            socklen_t errlen = sizeof(sockerr);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<char *>(&sockerr), &errlen) < 0) {
                const int e = YarnBall::lastSocketError();
                (void) YarnBall::closeSocket(fd);
                throw SocketException("getsockopt(SO_ERROR)", e);
            }
            if (sockerr != 0) {
                (void) YarnBall::closeSocket(fd);
                throw SocketException("connect (async)", sockerr);
            }
        }

        co_return TcpStream(fd);
    }

}
