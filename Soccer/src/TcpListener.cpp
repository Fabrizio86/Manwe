//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "TcpListener.h"

#include <cstring>

#ifndef _WIN32
#include <sys/un.h>
#include <unistd.h>
#else
#include <afunix.h>
#endif

#include "IoAwaiters.h"
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

        const int fd = static_cast<int>(::socket(addr.family(), SOCK_STREAM, 0));
        if (fd < 0) throw SocketException("socket", YarnBall::lastSocketError());

        // SO_REUSEADDR has different semantics on Windows than on POSIX
        // (BSD-style "rebind same port" rather than "ignore TIME_WAIT"),
        // but for a transient bind during testing this is what we want
        // either way. The cast to char* is required by the WinSock prototype.
        int yes = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
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

        const int fd = static_cast<int>(::socket(AF_UNIX, SOCK_STREAM, 0));
        if (fd < 0) throw SocketException("socket(AF_UNIX)", YarnBall::lastSocketError());

        // Unlink any stale socket file from a previous run. ENOENT is fine
        // (no prior socket); anything else means the path is held by some
        // other resource (regular file, directory) and we should not blow
        // it away — let bind() fail with the canonical EADDRINUSE.
#ifndef _WIN32
        if (::unlink(socketPath.c_str()) < 0) {
            const int err = errno;
            if (err != ENOENT) {
                // Non-stale-socket case: continue and let bind surface
                // the real error. Do not delete arbitrary files.
            }
        }
#endif

        sockaddr_un addr{};
        const socklen_t addrLen = fillUnixAddr(&addr, socketPath);

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
        if (this->sock < 0) {
            throw SocketException("accept on closed TcpListener");
        }

        while (true) {
            SocketAddress peer;
            peer.lengthMut() = sizeof(sockaddr_storage);

            int client;
            do {
                client = static_cast<int>(::accept(this->sock,
                                                    peer.dataMut(),
                                                    &peer.lengthMut()));
            } while (client < 0 && YarnBall::isEintr(YarnBall::lastSocketError()));

            if (client >= 0) {
                // Best-effort non-blocking on the new socket; failure here
                // is non-fatal because the caller may explicitly want
                // blocking semantics on the accepted stream.
                (void) YarnBall::setSocketNonblocking(client);
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
        if (this->sock >= 0) {
            (void) YarnBall::closeSocket(this->sock);
            this->sock = -1;
        }
    }

}
