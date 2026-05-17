//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef SOCCER_TCPLISTENER_H
#define SOCCER_TCPLISTENER_H

#include <cstdint>
#include <string>
#include <utility>

#include "Coroutines.h"
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
         *
         * @throws SocketException on resolution / socket / setsockopt /
         *         bind / listen failures.
         */
        static TcpListener bind(const std::string &host, std::uint16_t port);

        /**
         * @brief Bind a Unix-domain stream listener at @p socketPath.
         *        Accepts and reads/writes are identical to AF_INET
         *        TCP — same coroutine, same @c TcpStream type.
         *
         * If @p socketPath already exists it is @c unlink(2)-ed first;
         * a stale socket file from a previous run is a routine
         * occurrence (process crash) and we choose ease-of-use over
         * strict semantics. The caller is responsible for choosing a
         * path under an appropriate directory (e.g. @c /tmp,
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

        TcpListener(TcpListener &&other) noexcept : sock(std::exchange(other.sock, -1)) {
        }

        TcpListener &operator=(TcpListener &&other) noexcept {
            if (this != &other) {
                this->close();
                this->sock = std::exchange(other.sock, -1);
            }
            return *this;
        }

        ~TcpListener() { this->close(); }

        /**
         * @brief Accept the next connection. Suspends on readability of
         *        the listening fd; on resumption, issues @c ::accept and
         *        wraps the result in a non-blocking @c TcpStream.
         *
         * @throws SocketException on the accept syscall itself failing
         *         after readiness fired.
         */
        YarnBall::Task<TcpStream> accept();

        /**
         * @return The local address (useful when @c port was @c 0 at bind).
         */
        SocketAddress localAddress() const;

        /**
         * @return Underlying fd, or @c -1 if closed.
         */
        int fd() const noexcept { return this->sock; }

        /**
         * @brief Close the listener. Idempotent. Pending @c accept()
         *        suspensions will see the fd close on the next syscall.
         */
        void close() noexcept;

    private:
        explicit TcpListener(int fd) noexcept : sock(fd) {
        }

        int sock = -1;
    };

}

#endif // SOCCER_TCPLISTENER_H
