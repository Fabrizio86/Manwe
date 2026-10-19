//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "TcpListener.h"

#include <cstring>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#else
#include <afunix.h>
#endif

#include "IoAwaiters.h"
#include "Reactor.h"
#include "NetUtil.h"
#include "PlatformNet.h"
#include "SocketException.h"

namespace Soccer {

    namespace {
        /**
         * @brief Populate a @c sockaddr_un from a filesystem path.
         *        Throws if the path is too long for the platform's
         *        fixed @c sun_path buffer (108 bytes on Linux, 104 on
         *        macOS / BSD, 108 on Windows). Truncating would silently
         *        bind to the wrong socket, so we hard-fail instead.
         */
        socklen_t fillUnixAddr(sockaddr_un *addr, const std::string &path) {
            std::memset(addr, 0, sizeof(*addr));
            addr->sun_family = AF_UNIX;
            if (path.size() >= sizeof(addr->sun_path)) {
                throw SocketException("AF_UNIX path too long for sun_path: " + path);
            }
            std::memcpy(addr->sun_path, path.c_str(), path.size());
            return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
        }
    }

    namespace {
        /**
         * @brief Put @p fd into non-blocking mode. Throws on failure.
         */
        void set_nonblocking(int fd) {
            if (YarnBall::setSocketNonblocking(fd) != 0) {
                throw SocketException("set_nonblocking", YarnBall::lastSocketError());
            }
        }
    }

    TcpListener TcpListener::bind(const std::string &host, std::uint16_t port) {
        YarnBall::ensureWsaStarted();
        const auto addr = SocketAddress::resolve(host, port);

        const int fd = detail::openSocket(addr.family(), SOCK_STREAM, 0);
        if (fd < 0) throw SocketException("socket", YarnBall::lastSocketError());

        // SO_REUSEADDR has different semantics on Windows than on POSIX
        // (BSD-style "rebind same port" rather than "ignore TIME_WAIT"),
        // but for a transient bind during testing this is what we want
        // either way. The cast to char* is required by the WinSock prototype.
#ifdef _WIN32
        // SO_REUSEADDR on Windows lets another process bind the same port.
        const int yes = 1;
        const int reuseOpt = SO_EXCLUSIVEADDRUSE;
#else
        const int yes = 1;
        const int reuseOpt = SO_REUSEADDR;
#endif
        if (::setsockopt(fd, SOL_SOCKET, reuseOpt,
                         reinterpret_cast<const char *>(&yes), sizeof(yes)) < 0) {
            const int e = YarnBall::lastSocketError();
            (void) YarnBall::closeSocket(fd);
            throw SocketException("setsockopt(SO_REUSEADDR)", e);
        }

        if (::bind(fd, addr.data(), addr.length()) < 0) {
            const int e = YarnBall::lastSocketError();
            (void) YarnBall::closeSocket(fd);
            throw SocketException("bind", e);
        }

        if (::listen(fd, SOMAXCONN) < 0) {
            const int e = YarnBall::lastSocketError();
            (void) YarnBall::closeSocket(fd);
            throw SocketException("listen", e);
        }

        try {
            set_nonblocking(fd);
        } catch (...) {
            (void) YarnBall::closeSocket(fd);
            throw;
        }

        return TcpListener(fd);
    }

    TcpListener TcpListener::bindUnix(const std::string &socketPath) {
        YarnBall::ensureWsaStarted();

        // Build (and length-check) the address before creating the socket
        // so an over-long path cannot leak an fd.
        sockaddr_un addr{};
        const socklen_t addrLen = fillUnixAddr(&addr, socketPath);

        const int fd = detail::openSocket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) throw SocketException("socket(AF_UNIX)", YarnBall::lastSocketError());

        // Unlink any stale socket file from a previous run. ENOENT is fine
        // (no prior socket); anything else means the path is held by some
        // other resource (regular file, directory) and we should not blow
        // it away — let bind() fail with the canonical EADDRINUSE.
#ifndef _WIN32
        // Only remove an existing socket node; never delete a regular file.
        struct stat st{};
        if (::lstat(socketPath.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
            (void) ::unlink(socketPath.c_str());
        }
#endif

        if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), addrLen) < 0) {
            const int e = YarnBall::lastSocketError();
            (void) YarnBall::closeSocket(fd);
            throw SocketException("bind(AF_UNIX)", e);
        }

        if (::listen(fd, SOMAXCONN) < 0) {
            const int e = YarnBall::lastSocketError();
            (void) YarnBall::closeSocket(fd);
            throw SocketException("listen(AF_UNIX)", e);
        }

        try {
            set_nonblocking(fd);
        } catch (...) {
            (void) YarnBall::closeSocket(fd);
            throw;
        }

        return TcpListener(fd);
    }

    YarnBall::Task<TcpStream> TcpListener::accept() {
        while (true) {
            // Re-checked on every pass: close() wakes a parked accept, which must then fail.
            if (this->sock < 0) {
                throw SocketException("accept on closed TcpListener");
            }
            SocketAddress peer;
            peer.lengthMut() = sizeof(sockaddr_storage);

            int client;
            do {
#if defined(__linux__)
                client = ::accept4(this->sock, peer.dataMut(), &peer.lengthMut(), SOCK_CLOEXEC);
#else
                client = static_cast<int>(::accept(this->sock,
                                                    peer.dataMut(),
                                                    &peer.lengthMut()));
#endif
            } while (client < 0 && YarnBall::isEintr(YarnBall::lastSocketError()));

            if (client >= 0) {
#if !defined(__linux__)
                detail::setCloexec(client);
#endif
                // A blocking client socket would stall a Yarn worker in
                // recv/send, so drop the connection if this fails.
                if (YarnBall::setSocketNonblocking(client) != 0) {
                    (void) YarnBall::closeSocket(client);
                    continue;
                }
                co_return TcpStream(client);
            }

            const int err = YarnBall::lastSocketError();
            if (YarnBall::isWouldBlock(err)) {
                co_await YarnBall::io::waitReadable(this->sock);
                continue;
            }
            throw SocketException("accept", err);
        }
    }

    SocketAddress TcpListener::localAddress() const {
        SocketAddress addr;
        addr.lengthMut() = sizeof(sockaddr_storage);
        if (::getsockname(this->sock, addr.dataMut(), &addr.lengthMut()) < 0) {
            throw SocketException("getsockname", YarnBall::lastSocketError());
        }
        return addr;
    }

    void TcpListener::close() noexcept {
        const int fd = this->sock;
        if (fd >= 0) {
            // Closed first, so an accept() woken below re-checks `sock` and fails
            // instead of calling accept on a descriptor number that may be reused.
            this->sock = -1;
#if defined(__linux__)
            // Closing alone leaves an accept() parked on this fd asleep. On Linux a
            // shutdown makes the fd readable, so the pending accept wakes and fails.
            (void) ::shutdown(fd, SHUT_RD);
#endif
#ifndef _WIN32
            // Wake any coroutine still parked on this fd before it is closed (and the number reused).
            (void) YarnBall::Reactor::instance()->cancelFd(fd);
#endif
            (void) YarnBall::closeSocket(fd);
        }
    }

}
