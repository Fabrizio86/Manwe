//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef SOCCER_TCPLISTENER_H
#define SOCCER_TCPLISTENER_H

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "Coroutines.h"
#include "NetUtil.h"
#include "TcpStream.h"

namespace Soccer {

    /**
     * @class TcpListener
     * @brief Listening TCP socket. @c bind synchronously creates the socket
     *        and starts listening; @c accept is a coroutine that suspends
     *        on @c io::waitReadable until a connection is pending, then
     *        issues @c ::accept and returns a non-blocking @c TcpStream.
     *
     * Listening sockets typically live for the lifetime of the program;
     * @c bind is synchronous because it's a one-shot setup syscall and
     * doing it on a coroutine adds nothing.
     *
     * Move-only.
     *
     * Every accepted TCP connection has @c TCP_NODELAY set (see @c TcpStream).
     *
     * @note @c close() shuts the listening socket down on Linux (to wake a parked
     *       accept), and a shutdown acts on the socket, not on one descriptor. A
     *       process that @c fork()s without @c exec shares the socket with its child,
     *       so a @c close() in the child would also stop the parent's listener.
     *       Forking without exec is not supported with Yarn running threads anyway;
     *       exec the child (descriptors are close-on-exec) instead.
     */
    class TcpListener final {
    public:
        TcpListener() = default;

        /**
         * @brief Bind to @p host:@p port and start listening with the
         *        kernel's default backlog (@c SOMAXCONN).
         *
         * @param host  e.g. "0.0.0.0", "::", or a specific local IP.
         * @param port  TCP port. Pass @c 0 to let the kernel assign one;
         *              read it back via @c localAddress().
         * @param dualStack Only for an IPv6 listener (e.g. @p host @c "::"):
         *              @c true accepts IPv4 clients too (via v4-mapped
         *              addresses), @c false makes it IPv6-only. Set explicitly
         *              with @c IPV6_V6ONLY so the behaviour is the same on every
         *              OS; the OS defaults differ (Linux and macOS dual-stack,
         *              BSDs and Windows v6-only). Default @c true, the Linux
         *              and macOS behaviour.
         *
         * @throws SocketException on resolution / socket / setsockopt /
         *         bind / listen failures.
         */
        static TcpListener bind(const std::string &host, std::uint16_t port, bool dualStack = true);

        /**
         * @brief Bind a Unix-domain stream listener at @p socketPath.
         *        Accepts and reads/writes are identical to AF_INET
         *        TCP — same coroutine, same @c TcpStream type.
         *
         * A stale socket file from a previous run (process crash) is
         * removed first, but only after it is shown that nothing is listening
         * on it: a path held by a LIVE server makes this throw instead of
         * silently taking the path over, and a path that is not a socket
         * (regular file, directory) is never removed. Liveness is decided by a
         * probe @c connect (refused or missing means stale; a full backlog,
         * or a permission error such as another user's socket, means live) and
         * by an advisory @c flock on a sidecar file "<path>.lock" that every
         * listener bound here keeps for its lifetime. The lock is what
         * protects a saturated server on macOS and the BSDs, where a full
         * backlog looks like a refusal to the probe; a saturated server that
         * was NOT started through @c bindUnix is only protected on Linux. The
         * ".lock" file is left in place when the listener closes. (A server that
         * starts between the probe and the unlink can still lose its path; the
         * window is a few microseconds.) Empty paths and paths containing
         * a NUL byte are rejected.
         *
         * The socket node is made owner-only (mode 0600) on POSIX by a @c chmod
         * right after the bind, without touching the process-wide umask. Until
         * @c listen() the socket refuses every connect, so the default mode the
         * node briefly has is never usable. Loosen the mode afterwards with
         * @c chmod if other users must connect. The caller is responsible for
         * choosing a path under an appropriate directory (e.g.
         * @c /run/user/<uid>).
         *
         * Available on POSIX and on Windows 10 1803+. On Windows the
         * Unix family supports stream sockets only (no SOCK_DGRAM,
         * no socketpair); within those bounds the public API is
         * identical to POSIX.
         *
         * @throws SocketException on socket / unlink-already-failed /
         *         bind / listen failure.
         */
        static TcpListener bindUnix(const std::string &socketPath);

        TcpListener(const TcpListener &) = delete;
        TcpListener &operator=(const TcpListener &) = delete;

        TcpListener(TcpListener &&other) noexcept
            : sock(std::move(other.sock)), lockFd(std::exchange(other.lockFd, -1)) {
        }

        TcpListener &operator=(TcpListener &&other) noexcept {
            if (this != &other) {
                this->close();
                this->sock.reset(other.sock.take());
                this->lockFd = std::exchange(other.lockFd, -1);
            }
            return *this;
        }

        ~TcpListener() { this->close(); }

        /**
         * @brief Accept the next connection. Suspends on readability of
         *        the listening fd; on resumption, issues @c ::accept and
         *        wraps the result in a non-blocking @c TcpStream.
         *
         * @param peer Optional. When non-null, receives the client's address on
         *        success; it must stay valid until the returned task completes.
         *
         * @throws SocketException on the accept syscall itself failing
         *         after readiness fired.
         */
        YarnBall::Task<TcpStream> accept(SocketAddress *peer = nullptr);

        /**
         * @brief Take one pending connection if there is one, without waiting.
         * @return The stream, or @c std::nullopt when the backlog is empty.
         * @throws SocketException on any other accept failure, or if closed.
         */
        std::optional<TcpStream> tryAccept(SocketAddress *peer = nullptr);

        /**
         * @brief Make an @c accept() parked on this listener return (it throws) without
         *        closing the listener or queueing a connection. Sticky: every later
         *        @c accept() throws too (@c tryAccept is unaffected).
         * @return @c true if the wake was issued; @c false on Windows, or if the listener is
         *         closed. Callers then have to knock on the listener's own address instead
         *         (see @c tcpServe).
         */
        bool interruptAccept() noexcept;

        /**
         * @return The local address (useful when @c port was @c 0 at bind).
         */
        SocketAddress localAddress() const;

        /**
         * @return Underlying fd, or @c -1 if closed.
         */
        int fd() const noexcept { return this->sock.get(); }

        /**
         * @brief Close the listener. Idempotent. Pending @c accept()
         *        suspensions will see the fd close on the next syscall.
         */
        void close() noexcept;

    private:
        explicit TcpListener(int fd) noexcept : sock(fd) {
        }

        detail::SocketSlot sock;
        int lockFd = -1; ///< bindUnix path lock (POSIX), held while the listener lives
    };

}

#endif // SOCCER_TCPLISTENER_H
