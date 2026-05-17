//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef SOCCER_RAWSOCKET_H
#define SOCCER_RAWSOCKET_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "Coroutines.h"
#include "SocketAddress.h"

namespace Soccer {

    /**
     * @class RawSocket
     * @brief Raw IP socket with coroutine send / recv.
     *
     * Constructed with an IPv4 family and a kernel protocol number
     * (e.g. @c IPPROTO_ICMP, @c IPPROTO_TCP for sniffer-style usage, etc.).
     * The header inclusion flag (@c IP_HDRINCL) is left at its kernel
     * default; callers writing whole packets need to set it explicitly.
     *
     * Privilege requirements (raw sockets are gated by the kernel):
     *  - Linux: @c CAP_NET_RAW (or root).
     *  - macOS: typically root; some protocols are restricted further.
     *  - Windows: not currently supported on this build.
     *
     * Move-only.
     */
    class RawSocket final {
    public:
        RawSocket() = default;

        /**
         * @brief Open a raw socket for the given protocol number.
         *
         * @param family       @c AF_INET or @c AF_INET6.
         * @param ip_protocol  e.g. @c IPPROTO_ICMP, @c IPPROTO_RAW.
         *
         * @throws SocketException if the socket cannot be created
         *         (commonly EPERM without CAP_NET_RAW).
         */
        static RawSocket open(int family, int ip_protocol);

        /**
         * @brief Convenience: open an ICMP socket (@c IPPROTO_ICMP).
         */
        static RawSocket icmp(int family = AF_INET) {
            return open(family, IPPROTO_ICMP);
        }

        RawSocket(const RawSocket &) = delete;
        RawSocket &operator=(const RawSocket &) = delete;

        RawSocket(RawSocket &&other) noexcept : sock(std::exchange(other.sock, -1)) {
        }

        RawSocket &operator=(RawSocket &&other) noexcept {
            if (this != &other) {
                this->close();
                this->sock = std::exchange(other.sock, -1);
            }
            return *this;
        }

        ~RawSocket() { this->close(); }

        /**
         * @brief Receive one packet (whatever the kernel hands us — the
         *        IP header may or may not be present depending on family
         *        and @c IP_HDRINCL state).
         */
        YarnBall::Task<std::size_t> recv(std::span<std::byte> buf, SocketAddress *sender);

        /**
         * @brief Send one packet to @p destination.
         */
        YarnBall::Task<std::size_t> sendTo(std::span<const std::byte> buf,
                                            const SocketAddress &destination);

        int fd() const noexcept { return this->sock; }

        void close() noexcept;

    private:
        explicit RawSocket(int fd) noexcept : sock(fd) {
        }

        int sock = -1;
    };

}

#endif // SOCCER_RAWSOCKET_H
