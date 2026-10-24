//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "TcpStream.h"

#include <algorithm>
#include <cstring>

#ifndef _WIN32
#include <poll.h>
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
        // Taking the fd out of the slot BEFORE waking anyone makes a woken read / write
        // fail cleanly on its re-check instead of touching a descriptor number that is
        // about to be closed (and may be handed to another connection).
        const int fd = this->sock.take();
        if (fd >= 0) {
#ifndef _WIN32
            // Wake any coroutine still parked on this fd before it is closed (and the number reused).
            (void) YarnBall::Reactor::instance()->cancelFd(fd);
#endif
            // The real close waits for any syscall still running on the fd (see SocketSlot).
            this->sock.retire(fd);
        }
    }

    YarnBall::Task<std::size_t> TcpStream::read(std::span<std::byte> buf) {
        while (true) {
            // Re-checked on every pass: close() wakes a parked reader, which must then fail.
            // The Use stays alive across the wait, so the descriptor cannot be closed (and
            // its number reused) between the EAGAIN and the registration.
            auto use = this->sock.begin();
            const int fd = use.fd();
            if (fd < 0) {
                throw SocketException("read on closed TcpStream");
            }
            if (buf.empty()) co_return 0; // nothing to fill; not an EOF indication
            // recv returns ssize_t on POSIX and int on WinSock; either fits
            // in the YarnBall::ssize_t alias.
            ::ssize_t n;
            do {
                n = ::recv(fd,
                           reinterpret_cast<char *>(buf.data()),
                           static_cast<int>(std::min<std::size_t>(buf.size(), 0x7fffffffu)), 0);
            } while (n < 0 && YarnBall::isEintr(YarnBall::lastSocketError()));

            if (n >= 0) co_return static_cast<std::size_t>(n);
            const int err = YarnBall::lastSocketError();
            if (YarnBall::isWouldBlock(err)) {
                co_await detail::waitReadable(use);
                continue;
            }
            throw SocketException("recv", err);
        }
    }

    YarnBall::Task<std::size_t> TcpStream::write(std::span<const std::byte> buf) {
        std::size_t total = 0;
        const auto *p = buf.data();
        if (this->sock.get() < 0) {
            throw SocketException("write on closed TcpStream");
        }
        while (total < buf.size()) {
            auto use = this->sock.begin(); // held across the wait, see read()
            const int fd = use.fd();
            if (fd < 0) {
                throw SocketException("write on closed TcpStream");
            }
            ::ssize_t n;
            do {
                n = ::send(fd,
                           reinterpret_cast<const char *>(p + total),
                           static_cast<int>(std::min<std::size_t>(buf.size() - total, 0x7fffffffu)),
                           detail::kSendFlags);
            } while (n < 0 && YarnBall::isEintr(YarnBall::lastSocketError()));
            const int err = (n < 0) ? YarnBall::lastSocketError() : 0;

            if (n > 0) {
                total += static_cast<std::size_t>(n);
                continue;
            }
            if (n == 0) {
                // A stream socket never legitimately accepts zero bytes of a non-empty
                // buffer; returning a short count would break the "writes it all" contract.
                throw SocketException("send accepted no bytes");
            }
            if (YarnBall::isWouldBlock(err)) {
                co_await detail::waitWritable(use);
                continue;
            }
            throw SocketException("send", err);
        }
        co_return total;
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
    }

    YarnBall::Task<TcpStream> tcpConnectUnix(std::string socketPath,
                                             std::chrono::milliseconds busyRetry) {
        YarnBall::ensureWsaStarted();

        sockaddr_un addr{};
        const socklen_t addrLen = detail::makeUnixAddr(&addr, socketPath);

        const int fd = detail::openSocket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) throw SocketException("socket(AF_UNIX)", YarnBall::lastSocketError());
        // Closed on every exit path, including a cancellation while parked in waitWritable.
        ConnectFd guard(fd);

        set_nonblocking(fd);

        // A Linux server whose accept backlog is full answers a non-blocking connect with
        // EAGAIN, and nothing signals when room appears: poll again with a short back-off
        // until the allowance runs out (or the caller's scope is cancelled).
        const auto giveUp = std::chrono::steady_clock::now() + busyRetry;
        auto pause = std::chrono::milliseconds{1};
        int rc;
        while (true) {
            rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), addrLen);
            if (rc == 0) break;
            const int err = YarnBall::lastSocketError();
            if (YarnBall::isEintr(err)) continue;
            if (YarnBall::isInProgress(err)) break;
            if (YarnBall::isWouldBlock(err) && std::chrono::steady_clock::now() + pause < giveUp) {
                co_await YarnBall::sleepFor(pause);
                pause = std::min(pause * 2, std::chrono::milliseconds{50});
                continue;
            }
            throw SocketException("connect(AF_UNIX)", err);
        }
        if (rc < 0) {
            co_await YarnBall::io::waitWritable(fd);
            int sockerr = 0;
            socklen_t errlen = sizeof(sockerr);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<char *>(&sockerr), &errlen) < 0) {
                throw SocketException("getsockopt(SO_ERROR)", YarnBall::lastSocketError());
            }
            if (sockerr != 0) {
                throw SocketException("connect(AF_UNIX, async)", sockerr);
            }
        }

        co_return TcpStream(guard.release());
    }

    namespace {
        /// Wait for @p fd to become writable; a task of its own so a timer can race it.
        YarnBall::Task<> waitConnectWritable(int fd) {
            co_await YarnBall::io::waitWritable(fd);
        }

        /// Whether a non-blocking connect on @p fd has finished (successfully or not) within a
        /// moment, so there is nothing to park for. A loopback or same-rack handshake completes in
        /// tens of microseconds -- on macOS just after connect() returns EINPROGRESS -- and skipping
        /// the wait spares a Reactor round trip and, with a time limit, the timer racing it. The
        /// look is bounded by one millisecond, and only a handshake still in flight pays it.
        bool connectSettled(int fd) noexcept {
#ifndef _WIN32
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            return ::poll(&pfd, 1, 0) > 0 || ::poll(&pfd, 1, 1) > 0;
#else
            (void) fd;
            return false;
#endif
        }

        /// Whether @p host is an IP address literal, which needs no name lookup (and no worker hop).
        bool isAddressLiteral(const std::string &host) {
            in_addr a4{};
            in6_addr a6{};
            return ::inet_pton(AF_INET, host.c_str(), &a4) == 1 ||
                   ::inet_pton(AF_INET6, host.c_str(), &a6) == 1;
        }

        /**
         * @brief Attempt one connect to @p addr. Throws SocketException
         *        on any failure; the fd is always closed before
         *        throwing, so a failed attempt never leaks. The wait for the
         *        handshake is cancellable (CancelledException under a stopped
         *        scope). With a positive @p limit, a handshake still pending
         *        after it throws YarnBall::TimeoutException; the timer is only
         *        started when the handshake is really pending, so the common
         *        fast connect pays nothing for the limit.
         */
        YarnBall::Task<TcpStream> tryConnect(const SocketAddress &addr, std::chrono::milliseconds limit) {
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
                if (!connectSettled(fd)) {
                    if (limit.count() > 0) {
                        co_await YarnBall::withTimeout(waitConnectWritable(fd), limit);
                    } else {
                        co_await YarnBall::io::waitWritable(fd);
                    }
                }
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

        /// Resolve @p host and try each address in turn. With a @p budget each candidate gets
        /// a share of it (detail::connectSlice) so a blackholed one does not starve the rest.
        YarnBall::Task<TcpStream> connectAny(std::string host, std::uint16_t port,
                                             std::chrono::milliseconds budget) {
            const auto start = std::chrono::steady_clock::now();
            // Try every candidate getaddrinfo returned, not just the
            // first: a host with multiple A/AAAA records (load-balanced
            // services, CDNs, multi-homed backends) may have working
            // addresses beyond the head of the list. Only the LAST
            // candidate's failure is surfaced to the caller; earlier
            // failures just move on to the next address. resolveAllAsync
            // hops to a worker before the blocking getaddrinfo call, same
            // as the single-address resolveAsync this replaces. An address
            // literal needs no lookup, so it is parsed in place.
            std::vector<SocketAddress> all;
            if (isAddressLiteral(host)) {
                all = SocketAddress::resolveAll(host, port);
            } else if (budget.count() > 0) {
                // The budget covers the lookup too.
                try {
                    all = co_await YarnBall::withTimeout(SocketAddress::resolveAllAsync(host, port), budget);
                } catch (const YarnBall::TimeoutException &) {
                    throw SocketException("connect timed out");
                }
            } else {
                all = co_await SocketAddress::resolveAllAsync(host, port);
            }

            for (std::size_t i = 0; i < all.size(); ++i) {
                try {
                    if (budget.count() <= 0) {
                        co_return co_await tryConnect(all[i], std::chrono::milliseconds{0});
                    }
                    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start);
                    const auto left = budget - spent;
                    if (left.count() <= 0) break;
                    co_return co_await tryConnect(all[i], detail::connectSlice(left, all.size() - i));
                } catch (const YarnBall::TimeoutException &) {
                    // this candidate used up its share: fall through to the next one
                    if (i + 1 == all.size()) throw SocketException("connect timed out");
                } catch (const SocketException &) {
                    if (i + 1 == all.size()) throw;
                    // else: this candidate failed, try the next one.
                }
            }
            if (budget.count() > 0) throw SocketException("connect timed out");
            // Unreachable: resolveAll() never returns an empty vector on
            // success, so the loop above always either returns or
            // rethrows on its last iteration.
            throw SocketException("tcpConnect: no addresses to try");
        }
    }

    YarnBall::Task<TcpStream> tcpConnect(std::string host, std::uint16_t port,
                                         std::chrono::milliseconds timeout) {
        YarnBall::ensureWsaStarted();
        // The limit is applied inside connectAny, only around a handshake that is really
        // pending (see tryConnect), instead of racing every connect against a timer.
        co_return co_await connectAny(std::move(host), port, timeout);
    }

}
