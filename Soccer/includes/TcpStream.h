//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef SOCCER_TCPSTREAM_H
#define SOCCER_TCPSTREAM_H

#include <cstddef>
#include <span>
#include <string>
#include <utility>

#include "Coroutines.h"
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
     */
    class TcpStream final {
    public:
        TcpStream() = default;

        /**
         * @brief Take ownership of an already-connected, non-blocking fd.
         *        Used by @c TcpListener::accept and @c tcpConnect.
         */
        explicit TcpStream(int fd) noexcept : sock(fd) {
        }

        TcpStream(const TcpStream &) = delete;
        TcpStream &operator=(const TcpStream &) = delete;

        TcpStream(TcpStream &&other) noexcept : sock(std::exchange(other.sock, -1)) {
        }

        TcpStream &operator=(TcpStream &&other) noexcept {
            if (this != &other) {
                this->close();
                this->sock = std::exchange(other.sock, -1);
            }
            return *this;
        }

        ~TcpStream() { this->close(); }

        /**
         * @return The underlying fd, or @c -1 if closed.
         */
        int fd() const noexcept { return this->sock; }

        /**
         * @brief Relinquish ownership of the fd. The stream is left empty
         *        (fd == -1) and will not close on destruction. The caller
         *        becomes responsible for the fd.
         *
         * Used when handing a connected socket to a higher-level wrapper
         * (e.g. @c TlsStream after the server-side handshake).
         */
        int release() noexcept {
            int f = this->sock;
            this->sock = -1;
            return f;
        }

        /**
         * @brief Read up to @p buf.size() bytes. Suspends until the kernel
         *        reports readability; then issues one @c ::recv.
         * @return Number of bytes read; @c 0 indicates the peer closed the
         *         half. Throws @c SocketException on error.
         */
        YarnBall::Task<std::size_t> read(std::span<std::byte> buf);

        /**
         * @brief Write the entire @p buf. Loops with @c EAGAIN-driven
         *        @c co_await waitWritable until every byte is flushed.
         * @return The number of bytes written (== buf.size() on success).
         *         Throws @c SocketException on error.
         */
        YarnBall::Task<std::size_t> write(std::span<const std::byte> buf);

        /**
         * @brief Close the socket. Idempotent.
         */
        void close() noexcept;

    private:
        int sock = -1;
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
     * @throws SocketException on resolution / socket / connect failures.
     */
    YarnBall::Task<TcpStream> tcpConnect(std::string host, std::uint16_t port);

    /**
     * @brief Connect to a Unix-domain stream listener at @p socketPath.
     *        Returns a @c TcpStream; read/write semantics are identical
     *        to AF_INET TCP, including non-blocking + reactor-driven
     *        suspend.
     *
     * Available on POSIX and on Windows 10 1803+. The Unix family on
     * Windows supports stream sockets only.
     *
     * @throws SocketException on socket / connect failure.
     */
    YarnBall::Task<TcpStream> tcpConnectUnix(std::string socketPath);

}

#endif // SOCCER_TCPSTREAM_H
