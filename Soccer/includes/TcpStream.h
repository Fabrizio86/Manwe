//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef SOCCER_TCPSTREAM_H
#define SOCCER_TCPSTREAM_H

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <utility>

#include "Coroutines.h"
#include "NetUtil.h"
#include "SocketAddress.h"

namespace Soccer {

    /**
     * @class TcpStream
     * @brief Owns one connected TCP socket and exposes coroutine read/write.
     *
     * The fd is set non-blocking at construction; read/write either succeed
     * immediately or @c co_await @c io::waitReadable / @c io::waitWritable
     * until the kernel reports readiness.
     *
     * Move-only. @c close() (or destruction) releases the fd.
     *
     * @c close() may be called from any thread while a @c read / @c write is
     * parked or running: the parked operation wakes and throws, and the fd is
     * not released until no syscall on it is in flight. Moving a stream that is
     * being read or written is not supported.
     *
     * Writes do not raise SIGPIPE (@c MSG_NOSIGNAL, or @c SO_NOSIGPIPE set on the
     * fd by the constructor), so a peer that went away yields a SocketException.
     *
     * The constructor also sets @c TCP_NODELAY on a TCP socket (a no-op on AF_UNIX),
     * so every connection made or accepted through Soccer has Nagle's algorithm off:
     * a reply written as several small sends is not held back for the peer's delayed
     * ACK. Batch writes yourself if packet count matters.
     */
    class TcpStream final {
    public:
        TcpStream() = default;

        /**
         * @brief Take ownership of an already-connected, non-blocking fd.
         *        Used by @c TcpListener::accept and @c tcpConnect.
         */
        explicit TcpStream(int fd) noexcept : sock(fd) {
            if (fd >= 0) {
                detail::suppressSigpipe(fd);
                detail::setNoDelay(fd);
            }
        }

        TcpStream(const TcpStream &) = delete;
        TcpStream &operator=(const TcpStream &) = delete;

        TcpStream(TcpStream &&other) noexcept : sock(std::move(other.sock)) {
        }

        TcpStream &operator=(TcpStream &&other) noexcept {
            if (this != &other) {
                this->close();
                this->sock.reset(other.sock.take());
            }
            return *this;
        }

        ~TcpStream() { this->close(); }

        /**
         * @return The underlying fd, or @c -1 if closed.
         */
        int fd() const noexcept { return this->sock.get(); }

        /**
         * @brief Relinquish ownership of the fd. The stream is left empty
         *        (fd == -1) and will not close on destruction. The caller
         *        becomes responsible for the fd.
         *
         * Used when handing a connected socket to a higher-level wrapper
         * (e.g. @c TlsStream after the server-side handshake).
         */
        int release() noexcept { return this->sock.take(); }

        /**
         * @brief Read up to @p buf.size() bytes. Suspends until the kernel
         *        reports readability; then issues one @c ::recv.
         * @return Number of bytes read; @c 0 indicates the peer closed the
         *         half. Throws @c SocketException on error. An empty @p buf
         *         returns @c 0 at once without touching the socket, so a
         *         caller must not use an empty buffer to probe for EOF.
         */
        YarnBall::Task<std::size_t> read(std::span<std::byte> buf);

        /**
         * @brief Write the entire @p buf. Loops with @c EAGAIN-driven
         *        @c co_await waitWritable until every byte is flushed.
         * @return The number of bytes written (== buf.size() on success).
         *         Throws @c SocketException on error, including a kernel that
         *         accepts zero bytes of a non-empty buffer.
         */
        YarnBall::Task<std::size_t> write(std::span<const std::byte> buf);

        /**
         * @brief Close the socket. Idempotent.
         */
        void close() noexcept;

        /// The socket slot (internal: lets IoDeadline watch the descriptor safely).
        const detail::SocketSlot &slot() const noexcept { return this->sock; }

    private:
        detail::SocketSlot sock;
    };

    /**
     * @brief Asynchronously establish a TCP connection to @p host:@p port.
     *
     * Resolves @p host via @c SocketAddress::resolve (synchronous; wrap in
     * @c co_await scheduleOn if you want the resolution off the calling
     * thread), creates a non-blocking socket, issues @c ::connect, and
     * suspends on writability until the kernel reports the handshake done
     * (or surfaces an error via @c SO_ERROR).
     *
     * @param timeout Longest the whole attempt (resolution plus every candidate
     *        address) may take; zero or negative means no limit beyond the
     *        kernel's own connect timeout. With several candidate addresses each
     *        gets a share of the budget (see @c detail::connectSlice), so an
     *        unreachable first address does not use it all up. The connect runs inside a
     *        cancellation scope (see Cancellation.h) that the deadline stops, so
     *        a socket still in the middle of its SYN is abandoned and closed --
     *        shutting it down would not wake it on every platform.
     *
     * @throws SocketException on resolution / socket / connect failures, and
     *         with a message containing "timed out" when @p timeout elapses.
     */
    YarnBall::Task<TcpStream> tcpConnect(std::string host, std::uint16_t port,
                                         std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    /**
     * @brief Connect to a Unix-domain stream listener at @p socketPath.
     *        Returns a @c TcpStream; read/write semantics are identical
     *        to AF_INET TCP, including non-blocking + reactor-driven
     *        suspend.
     *
     * Available on POSIX and on Windows 10 1803+. The Unix family on
     * Windows supports stream sockets only.
     *
     * @param busyRetry How long a connect that finds the server's accept backlog
     *        full (@c EAGAIN on Linux) is retried, with a short back-off, before
     *        it fails; zero or negative fails at once. The wait is cancellable.
     *
     * @throws SocketException on socket / connect failure.
     */
    YarnBall::Task<TcpStream> tcpConnectUnix(std::string socketPath,
                                             std::chrono::milliseconds busyRetry = std::chrono::milliseconds{5000});

}

#endif // SOCCER_TCPSTREAM_H
