//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "TcpStream.h"

#include <algorithm>
#include <cstring>

#ifndef _WIN32
#include <sys/un.h>
#else
#include <afunix.h>
#endif

#include "IoAwaiters.h"
#include "Timeout.h"
#include "Reactor.h"
#include "NetUtil.h"
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
        const int fd = this->sock;
        if (fd >= 0) {
            // Mark the stream closed BEFORE waking anyone: a woken read / write re-checks
            // `sock`, so it fails cleanly instead of touching a descriptor number that is
            // about to be closed (and may be handed to another connection).
            this->sock = -1;
#ifndef _WIN32
            // Wake any coroutine still parked on this fd before it is closed (and the number reused).
            (void) YarnBall::Reactor::instance()->cancelFd(fd);
#endif
            (void) YarnBall::closeSocket(fd);
        }
    }

    YarnBall::Task<std::size_t> TcpStream::read(std::span<std::byte> buf) {
        while (true) {
            // Re-checked on every pass: close() wakes a parked reader, which must then fail.
            if (this->sock < 0) {
                throw SocketException("read on closed TcpStream");
            }
            // recv returns ssize_t on POSIX and int on WinSock; either fits
            // in the YarnBall::ssize_t alias.
            ::ssize_t n;
            do {
                n = ::recv(this->sock,
                           reinterpret_cast<char *>(buf.data()),
                           static_cast<int>(std::min<std::size_t>(buf.size(), 0x7fffffffu)), 0);
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
        std::size_t total = 0;
        const auto *p = buf.data();
        if (this->sock < 0) {
            throw SocketException("write on closed TcpStream");
        }
        while (total < buf.size()) {
            if (this->sock < 0) {
                throw SocketException("write on closed TcpStream");
            }
            ::ssize_t n;
            do {
                n = ::send(this->sock,
                           reinterpret_cast<const char *>(p + total),
                           static_cast<int>(std::min<std::size_t>(buf.size() - total, 0x7fffffffu)), 0);
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

        sockaddr_un addr{};
        const socklen_t addrLen = fillUnixAddrConnect(&addr, socketPath);

        const int fd = detail::openSocket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) throw SocketException("socket(AF_UNIX)", YarnBall::lastSocketError());

        try {
            set_nonblocking(fd);
        } catch (...) {
            (void) YarnBall::closeSocket(fd);
            throw;
        }

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

    namespace {
        /// Closes a half-set-up socket on every exit path, including the coroutine
        /// being cancelled (a connect that timed out) or destroyed while parked.
        struct ConnectFd {
            int fd;
            explicit ConnectFd(int f) noexcept : fd(f) {}
            ConnectFd(const ConnectFd &) = delete;
            ConnectFd &operator=(const ConnectFd &) = delete;
            ~ConnectFd() {
                if (fd >= 0) (void) YarnBall::closeSocket(fd);
            }
            int release() noexcept {
                const int f = fd;
                fd = -1;
                return f;
            }
        };

        /**
         * @brief Attempt one connect to @p addr. Throws SocketException
         *        on any failure; the fd is always closed before
         *        throwing, so a failed attempt never leaks. The wait for the
         *        handshake is cancellable (CancelledException under a stopped
         *        scope), which is how tcpConnect's timeout abandons it.
         */
        YarnBall::Task<TcpStream> tryConnect(const SocketAddress &addr) {
            const int rawFd = detail::openSocket(addr.family(), SOCK_STREAM, 0);
            if (rawFd < 0) throw SocketException("socket", YarnBall::lastSocketError());
            ConnectFd guard(rawFd);
            const int fd = rawFd;

            set_nonblocking(fd);

            int rc = ::connect(fd, addr.data(), addr.length());
            if (rc < 0) {
                const int err = YarnBall::lastSocketError();
                if (!YarnBall::isInProgress(err)) {
                    throw SocketException("connect", err);
                }
                // EINPROGRESS / WSAEWOULDBLOCK: wait for writability, then
                // check SO_ERROR to see whether the handshake completed.
                co_await YarnBall::io::waitWritable(fd);
                int sockerr = 0;
                socklen_t errlen = sizeof(sockerr);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                 reinterpret_cast<char *>(&sockerr), &errlen) < 0) {
                    throw SocketException("getsockopt(SO_ERROR)", YarnBall::lastSocketError());
                }
                if (sockerr != 0) {
                    throw SocketException("connect (async)", sockerr);
                }
            }

            co_return TcpStream(guard.release());
        }

        /// Resolve @p host and try each address in turn (no time limit of its own).
        YarnBall::Task<TcpStream> connectAny(std::string host, std::uint16_t port) {
            // Try every candidate getaddrinfo returned, not just the
            // first: a host with multiple A/AAAA records (load-balanced
            // services, CDNs, multi-homed backends) may have working
            // addresses beyond the head of the list. Only the LAST
            // candidate's failure is surfaced to the caller; earlier
            // failures just move on to the next address. resolveAllAsync
            // hops to a worker before the blocking getaddrinfo call, same
            // as the single-address resolveAsync this replaces.
            auto all = co_await SocketAddress::resolveAllAsync(host, port);

            for (std::size_t i = 0; i < all.size(); ++i) {
                try {
                    co_return co_await tryConnect(all[i]);
                } catch (const SocketException &) {
                    if (i + 1 == all.size()) throw;
                    // else: this candidate failed, try the next one.
                }
            }
            // Unreachable: resolveAll() never returns an empty vector on
            // success, so the loop above always either returns or
            // rethrows on its last iteration.
            throw SocketException("tcpConnect: no addresses to try");
        }
    }

    YarnBall::Task<TcpStream> tcpConnect(std::string host, std::uint16_t port,
                                         std::chrono::milliseconds timeout) {
        YarnBall::ensureWsaStarted();
        if (timeout.count() <= 0) {
            co_return co_await connectAny(std::move(host), port);
        }
        // Race the connect against the deadline. The loser runs in a cancellation
        // scope that is stopped when the race is decided, so a connect still waiting
        // for its SYN-ACK wakes with CancelledException and closes its socket.
        try {
            co_return co_await YarnBall::withTimeout(connectAny(std::move(host), port), timeout);
        } catch (const YarnBall::TimeoutException &) {
            // fall through: reported below with a stable message
        }
        throw SocketException("connect timed out");
    }

}
