//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef SOCCER_SOCKETADDRESS_H
#define SOCCER_SOCKETADDRESS_H

#include <cstdint>
#include <string>

#include "Coroutines.h"
#include "PlatformNet.h"

namespace Soccer {

    /**
     * @class SocketAddress
     * @brief Family-agnostic socket address. Wraps @c sockaddr_storage so
     *        IPv4 and IPv6 endpoints can flow through the same API.
     *
     * The address is constructed either by resolving a host:port pair
     * (via @c resolve, which calls @c getaddrinfo and prefers the first
     * successful result), or by wrapping a kernel-supplied @c sockaddr
     * after @c accept / @c recvfrom.
     */
    class SocketAddress {
    public:
        SocketAddress() noexcept;

        /**
         * @brief Resolve @p host as either an IPv4 / IPv6 literal or a
         *        DNS name, paired with @p port. Returns the first AI_*
         *        result. Throws @c SocketException on resolution failure.
         *
         * @note Synchronous: runs @c getaddrinfo on the calling thread.
         *       For non-blocking flows use @c resolveAsync, which hops
         *       to a Yarn worker first.
         */
        static SocketAddress resolve(const std::string &host, std::uint16_t port);

        /**
         * @brief Same as @ref resolve but hops onto a Yarn worker before
         *        running @c getaddrinfo, so the calling coroutine's
         *        thread never blocks on DNS.
         *
         * @note Resolution itself is still a blocking syscall; this just
         *       moves the block onto a worker. A proper non-blocking
         *       resolver (c-ares or similar) is future work.
         */
        static YarnBall::Task<SocketAddress> resolveAsync(std::string host,
                                                            std::uint16_t port);

        /**
         * @brief Construct from a raw kernel address (e.g. one returned by
         *        @c accept). @p len must be the address length the kernel
         *        provided.
         */
        SocketAddress(const sockaddr *src, socklen_t len) noexcept;

        /**
         * @return Pointer to the underlying address suitable for syscalls.
         */
        const sockaddr *data() const noexcept {
            return reinterpret_cast<const sockaddr *>(&this->store);
        }

        sockaddr *dataMut() noexcept {
            return reinterpret_cast<sockaddr *>(&this->store);
        }

        /**
         * @return Current length in bytes (sized for AF_INET vs AF_INET6).
         */
        socklen_t length() const noexcept { return this->len; }

        /**
         * @return The address family (@c AF_INET / @c AF_INET6 / @c AF_UNSPEC).
         */
        int family() const noexcept {
            return reinterpret_cast<const sockaddr *>(&this->store)->sa_family;
        }

        /**
         * @return Port in host byte order, or 0 if the family does not
         *         carry a port.
         */
        std::uint16_t port() const noexcept;

        /**
         * @brief Human-readable representation, e.g. "127.0.0.1:8080" or
         *        "[::1]:8080".
         */
        std::string to_string() const;

        /**
         * @brief Mutable access to the length, used by callers that hand
         *        @c dataMut() to syscalls like @c accept and need to read
         *        the kernel-updated length back.
         */
        socklen_t &lengthMut() noexcept { return this->len; }

    private:
        /**
         * @brief Storage large enough for any supported family.
         */
        sockaddr_storage store{};

        /**
         * @brief Address length the kernel uses for this address.
         */
        socklen_t len{0};
    };

}

#endif // SOCCER_SOCKETADDRESS_H
