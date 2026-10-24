//
// Created by Fabrizio Paino on 2026-05-15.
//
// tcpServe: accept-loop + per-connection coSpawn helper, the canonical
// "TCP server" idiom in coroutine form. Caller supplies a handler that
// returns Task<void>(TcpStream); tcpServe loops accept, spawns the
// handler for each, and never blocks on a slow handler.
//
// Cancellation: pass a std::stop_token. tcpServe registers a
// std::stop_callback that closes the listener fd on stop request,
// which cancels any in-flight accept (kernel returns EBADF). The
// resulting SocketException is swallowed if stop_requested() is
// true, so the loop exits cleanly even when blocked in accept.
// In-flight handlers keep running until they complete.
//

#ifndef SOCCER_TCP_SERVER_H
#define SOCCER_TCP_SERVER_H

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <functional>
#include <optional>
#include <stop_token>
#include <utility>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "ConnectionLimiter.h"
#include "Coroutines.h"
#include "PlatformNet.h"
#include "SocketException.h"
#include "TcpListener.h"
#include "NetUtil.h"
#include "TcpStream.h"
#include "Timers.h"
#include "YarnBall.hpp"

namespace Soccer {

    /// Descriptor exhaustion: the listener stays readable, so retrying accept alone would spin.
    inline bool isFdExhaustionError(int err) noexcept {
#ifndef _WIN32
        return err == EMFILE || err == ENFILE;
#else
        return err == WSAEMFILE;
#endif
    }

    inline bool isTransientAcceptError(int err) noexcept {
#ifndef _WIN32
        return err == EMFILE || err == ENFILE || err == ENOBUFS || err == ENOMEM ||
               err == ECONNABORTED || err == EPROTO || err == EINTR ||
               // Per accept(2), errors of the NEW connection (or the network under it)
               // that must be treated like EAGAIN: retry.
               err == ENETDOWN || err == ENETUNREACH || err == EHOSTUNREACH ||
               err == EHOSTDOWN || err == EOPNOTSUPP || err == ENOPROTOOPT
#ifdef ENONET
               || err == ENONET
#endif
#if defined(__linux__)
               // A firewall rule (netfilter) can reject the new connection after the kernel queued it.
               || err == EPERM
#endif
            ;
#else
        return err == WSAEMFILE || err == WSAECONNRESET || err == WSAENOBUFS ||
               err == WSAECONNABORTED || err == WSAENETDOWN || err == WSAENETUNREACH ||
               err == WSAEHOSTUNREACH || err == WSAEHOSTDOWN || err == WSAEOPNOTSUPP ||
               err == WSAENOPROTOOPT;
#endif
    }

    /**
     * @brief Accept errors that call for a pause before the next accept. Every other
     *        transient error (see @ref isTransientAcceptError) belongs to one connection
     *        that was already taken off the queue, so the next accept can run at once;
     *        these two mean the machine is short of memory, where retrying immediately
     *        would only spin. Descriptor exhaustion is handled separately, by shedding.
     */
    inline bool isAcceptBackoffError(int err) noexcept {
#ifndef _WIN32
        return err == ENOMEM || err == ENOBUFS;
#else
        return err == WSAENOBUFS;
#endif
    }

    namespace detail {
        /// One descriptor held back so an accept can still be made when the process is out of them.
        class SpareFd final {
        public:
            SpareFd() noexcept { this->acquire(); }
            SpareFd(const SpareFd &) = delete;
            SpareFd &operator=(const SpareFd &) = delete;
            ~SpareFd() { this->release(); }
            void release() noexcept {
#ifndef _WIN32
                if (this->fd >= 0) {
                    (void) ::close(this->fd);
                    this->fd = -1;
                }
#endif
            }
            void acquire() noexcept {
#ifndef _WIN32
                if (this->fd < 0) this->fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
#endif
            }

        private:
            int fd = -1;
        };

        /// Wake a listener's pending accept by connecting to its own address, without ever
        /// blocking: the connect is non-blocking and waited for at most a short while.
        inline void knockOnListener(const SocketAddress &local) noexcept {
            sockaddr_storage dest{};
            const socklen_t destLen = local.length();
            std::memcpy(&dest, local.data(), local.length());

            // Rewrite ANY -> loopback so we hit our own listener.
            if (local.family() == AF_INET) {
                auto *in = reinterpret_cast<sockaddr_in *>(&dest);
                if (in->sin_addr.s_addr == htonl(INADDR_ANY)) {
                    in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                }
            } else if (local.family() == AF_INET6) {
                auto *in6 = reinterpret_cast<sockaddr_in6 *>(&dest);
                static const in6_addr loopback6 = IN6ADDR_LOOPBACK_INIT;
                static const in6_addr any6 = IN6ADDR_ANY_INIT;
                if (std::memcmp(&in6->sin6_addr, &any6, sizeof(any6)) == 0) {
                    in6->sin6_addr = loopback6;
                }
            }

            const int probe = openSocket(local.family(), SOCK_STREAM, 0);
            if (probe < 0) return;
            if (YarnBall::setSocketNonblocking(probe) == 0) {
                const int rc = ::connect(probe, reinterpret_cast<sockaddr *>(&dest), destLen);
                if (rc < 0 && YarnBall::isInProgress(YarnBall::lastSocketError())) {
                    // The handshake is under way; give it a moment to be queued on the
                    // listener, but never wait on a full backlog the way a blocking
                    // connect would (minutes).
#ifndef _WIN32
                    pollfd pfd{};
                    pfd.fd = probe;
                    pfd.events = POLLOUT;
                    (void) ::poll(&pfd, 1, 250);
#else
                    WSAPOLLFD pfd{};
                    pfd.fd = static_cast<SOCKET>(probe);
                    pfd.events = POLLWRNORM;
                    (void) ::WSAPoll(&pfd, 1, 250);
#endif
                }
            }
            (void) YarnBall::closeSocket(probe);
        }

        /**
         * @brief Make a parked accept on @p listener return so its loop can see a stop request.
         *        Where the Reactor can cancel the wait (everywhere but Windows) nothing is queued,
         *        so no handler ever sees a stray connection and the stop is immediate. On Windows
         *        it knocks on the listener's own address, see @ref knockOnListener.
         */
        inline void wakeAccept(TcpListener &listener) noexcept {
            if (listener.interruptAccept()) return;
            SocketAddress local;
            try {
                local = listener.localAddress();
            } catch (...) {
                return;
            }
            knockOnListener(local);
        }

        /// Runs @p task and keeps @p slot (a live-connection count) until it ends.
        inline YarnBall::Task<void> heldConnection(YarnBall::Task<void> task,
                                                   ConnectionLimiter::Slot slot) {
            (void) slot;
            co_await std::move(task);
            co_return;
        }
    }

    /**
     * @brief Accept-loop + per-connection spawn. For each accepted
     *        connection, invokes @p handler(std::move(stream)) and
     *        @c coSpawn s the returned task on the Yarn pool.
     *
     * @param listener Bound @c TcpListener. Moved into the coroutine
     *                  frame so its lifetime tracks the loop.
     * @param handler  Per-connection coroutine factory. Called once per
     *                  accepted stream. Returning a @c Task<void> is
     *                  what makes the spawn ergonomic; the handler can
     *                  internally co_await other tasks freely.
     * @param stop     Cooperative stop token. When stopped, a
     *                  @c stop_callback wakes any pending @c accept: on
     *                  Linux by shutting the listening socket down (no
     *                  connection is queued), elsewhere by making a brief
     *                  loopback connection to the listener's own bound
     *                  address (closing the fd alone does not reliably
     *                  wake a kqueue-suspended accept on macOS). The loop
     *                  then observes @c stop_requested() and returns. The
     *                  loopback connect is non-blocking but may hold the
     *                  thread calling @c request_stop for up to 250 ms
     *                  when the backlog is full, and is skipped if the
     *                  process has no descriptor left for it, in which
     *                  case the loop only notices the stop when its next
     *                  connection arrives.
     *
     * @param maxConnections Cap on concurrently running handlers; zero means
     *                  unlimited (the default, kept for compatibility -- a
     *                  public-facing server should set a finite cap). A
     *                  connection accepted while the cap is reached is closed
     *                  immediately without invoking @p handler.
     *
     * @note When the process runs out of file descriptors (EMFILE / ENFILE) the
     *       loop sheds load instead of spinning on a listener that stays readable:
     *       it keeps one spare descriptor (@c /dev/null) in reserve, releases it to
     *       accept the waiting connection (one non-blocking try, never a wait), closes
     *       that connection at once, and re-acquires the reserve. Errors that belong to one
     *       connection (reset before accept, firewall reject, ...) retry at once; only
     *       memory exhaustion (ENOMEM / ENOBUFS) backs off briefly.
     *       An exception thrown by @p handler itself (not by the task it returns)
     *       drops that one connection and does not end the loop.
     *
     * @note In-flight handlers are NOT cancelled when @p stop fires --
     *       C++ coroutines have no forced cancellation. If you want
     *       handlers to shorten, plumb @p stop through to them and
     *       have them check it cooperatively.
     *
     * @note Where the stop is delivered by a loopback connection (not Linux), that connection
     *       is itself accepted and dispatched through
     *       @p handler exactly like a real client -- it just closes
     *       immediately without sending anything. Handlers should
     *       already tolerate a connection that yields immediate EOF
     *       (a routine occurrence with real clients too: health-check
     *       probes, port scanners, failed TLS negotiations), but if
     *       yours does something observable (log a connection event,
     *       bump a counter) before its first read, expect exactly one
     *       such event per graceful shutdown.
     */
    inline YarnBall::Task<void>
    tcpServe(TcpListener listener,
              std::function<YarnBall::Task<void>(TcpStream)> handler,
              std::stop_token stop = {},
              std::size_t maxConnections = 0) {
        ConnectionLimiter limiter(maxConnections);
        // Unblock a pending accept on stop: closing or shutting down
        // the listener fd does NOT reliably wake a kqueue-suspended
        // accept on macOS (the kernel drops the filter silently
        // rather than synthesising an event). The portable way is
        // to "knock on the door" -- briefly open a loopback
        // connection to the listener's own bound address, which
        // forces accept to return one final stream. The loop body
        // then sees stop_requested() and returns; the probe stream
        // is discarded by the handler when it sees an immediate EOF.
        std::stop_callback unblock(stop, [&listener]() noexcept {
            detail::wakeAccept(listener);
        });
        detail::SpareFd spare;
        while (!stop.stop_requested()) {
            bool backoff = false;
            bool shed = false;
            TcpStream client;
            try {
                client = co_await listener.accept();
            } catch (const SocketException &e) {
                if (stop.stop_requested()) co_return;
                // Transient accept failures (fd exhaustion, a peer that
                // reset before accept) must not end the accept loop. A per-connection
                // error just retries; only memory pressure pauses first.
                if (!isTransientAcceptError(e.errorCode())) throw;
                if (isFdExhaustionError(e.errorCode())) shed = true;
                else backoff = isAcceptBackoffError(e.errorCode());
            }
            if (shed) {
                // Out of descriptors with a connection waiting: free the spare one, take that
                // connection and drop it, then re-arm the spare. Without this the listener
                // stays readable and the loop would spin. The take is a single non-blocking
                // try: the waiting connection may be gone by now (its client gave up), and
                // parking here would swallow the NEXT, legitimate, arrival.
                spare.release();
                try {
                    std::optional<TcpStream> dropped = listener.tryAccept();
                    if (!dropped) backoff = true; // nothing was waiting: let the descriptors recover
                } catch (const SocketException &e) {
                    if (stop.stop_requested()) co_return;
                    if (!isTransientAcceptError(e.errorCode())) throw;
                    backoff = true;
                }
                spare.acquire();
            } else if (client.fd() >= 0) {
                try {
                    if (maxConnections == 0) {
                        YarnBall::coSpawn(handler(std::move(client)));
                    } else if (auto slot = limiter.tryAcquire()) {
                        YarnBall::coSpawn(detail::heldConnection(handler(std::move(client)),
                                                                 std::move(slot)));
                    } // else: over the cap -- `client` is closed on scope exit
                } catch (...) {
                    // The handler factory (or the spawn) threw: this connection is lost
                    // (`client` is closed on scope exit), the server is not.
                }
            }
            if (backoff) co_await YarnBall::sleepFor(std::chrono::milliseconds{10});
        }
        co_return;
    }

}

#endif // SOCCER_TCP_SERVER_H
