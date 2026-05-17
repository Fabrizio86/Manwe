//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "SocketAddress.h"
#include "SocketException.h"

#include <cstring>

#include "PlatformNet.h"

namespace Soccer {

    SocketAddress::SocketAddress() noexcept {
        std::memset(&this->store, 0, sizeof(this->store));
    }

    SocketAddress::SocketAddress(const sockaddr *src, socklen_t len) noexcept {
        std::memset(&this->store, 0, sizeof(this->store));
        if (src && len > 0 && static_cast<size_t>(len) <= sizeof(this->store)) {
            std::memcpy(&this->store, src, static_cast<size_t>(len));
            this->len = len;
        }
    }

    YarnBall::Task<SocketAddress> SocketAddress::resolveAsync(std::string host,
                                                                std::uint16_t port) {
        co_await YarnBall::scheduleOn(YarnBall::Yarn::instance());
        co_return SocketAddress::resolve(host, port);
    }

    SocketAddress SocketAddress::resolve(const std::string &host, std::uint16_t port) {
        // getaddrinfo on Windows requires WSAStartup; idempotent + cheap.
        YarnBall::ensureWsaStarted();

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = 0; // caller picks the protocol
        hints.ai_flags = AI_ADDRCONFIG;

        const std::string port_str = std::to_string(port);
        addrinfo *results = nullptr;
        const int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &results);
        if (rc != 0 || results == nullptr) {
            // gai_strerror exists on both POSIX and Windows (via ws2tcpip.h);
            // on Windows it is the ANSI variant gai_strerrorA under the hood.
            throw SocketException(std::string("getaddrinfo: ") + ::gai_strerror(rc), rc);
        }

        SocketAddress addr(results->ai_addr, static_cast<socklen_t>(results->ai_addrlen));
        ::freeaddrinfo(results);
        return addr;
    }

    std::uint16_t SocketAddress::port() const noexcept {
        const auto *sa = reinterpret_cast<const sockaddr *>(&this->store);
        if (sa->sa_family == AF_INET) {
            return ntohs(reinterpret_cast<const sockaddr_in *>(sa)->sin_port);
        }
        if (sa->sa_family == AF_INET6) {
            return ntohs(reinterpret_cast<const sockaddr_in6 *>(sa)->sin6_port);
        }
        return 0;
    }

    std::string SocketAddress::to_string() const {
        char buf[INET6_ADDRSTRLEN] = {};
        const auto *sa = reinterpret_cast<const sockaddr *>(&this->store);

        if (sa->sa_family == AF_INET) {
            const auto *in = reinterpret_cast<const sockaddr_in *>(sa);
            ::inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf));
            return std::string(buf) + ":" + std::to_string(this->port());
        }
        if (sa->sa_family == AF_INET6) {
            const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(sa);
            ::inet_ntop(AF_INET6, &in6->sin6_addr, buf, sizeof(buf));
            return std::string("[") + buf + "]:" + std::to_string(this->port());
        }
        return "<unknown>";
    }

}
