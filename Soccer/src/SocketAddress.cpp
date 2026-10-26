//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "SocketAddress.h"
#include "SocketException.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "PlatformNet.h"

#ifndef _WIN32
#include <sys/un.h>
#else
#include <afunix.h>
#endif

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

    YarnBall::Task<std::vector<SocketAddress>> SocketAddress::resolveAllAsync(
        std::string host, std::uint16_t port) {
        co_await YarnBall::scheduleOn(YarnBall::Yarn::instance());
        co_return SocketAddress::resolveAll(host, port);
    }

    namespace {
        /// Whether a failed AI_ADDRCONFIG lookup of @p host (error @p rc) is worth repeating without it.
        bool addrConfigMayHaveHiddenIt(const std::string &host, int rc) {
            if (rc != EAI_NONAME && rc != EAI_FAMILY
#ifdef EAI_ADDRFAMILY
                && rc != EAI_ADDRFAMILY
#endif
#ifdef EAI_NODATA
                && rc != EAI_NODATA
#endif
            ) {
                return false;
            }
            if (host.empty()) return true; // null node: wildcard / loopback
            std::string h;
            for (char c : host) h.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (!h.empty() && h.back() == '.') h.pop_back();
            if (h == "localhost" || h == "ip6-localhost" || h == "ip6-loopback") return true;
            if (h.size() > 10 && h.compare(h.size() - 10, 10, ".localhost") == 0) return true;
            in6_addr a6{};
            in_addr a4{};
            return ::inet_pton(AF_INET, h.c_str(), &a4) == 1 || ::inet_pton(AF_INET6, h.c_str(), &a6) == 1;
        }
    }

    std::vector<SocketAddress> SocketAddress::resolveAll(const std::string &host,
                                                           std::uint16_t port) {
        // getaddrinfo on Windows requires WSAStartup; idempotent + cheap.
        YarnBall::ensureWsaStarted();

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = 0; // caller picks the protocol
        hints.ai_flags = AI_ADDRCONFIG;
        // An empty host is the null node: the wildcard address (what bind expects), the same on every
        // libc, instead of whatever "" happens to mean to this one.
        const char *node = host.empty() ? nullptr : host.c_str();
        if (host.empty()) hints.ai_flags |= AI_PASSIVE;

        const std::string port_str = std::to_string(port);
        addrinfo *results = nullptr;
        int rc = ::getaddrinfo(node, port_str.c_str(), &hints, &results);
        if ((rc != 0 || results == nullptr) && addrConfigMayHaveHiddenIt(host, rc)) {
            // AI_ADDRCONFIG hides every family the host has no non-loopback
            // address for, which makes even "localhost" unresolvable on a machine
            // (or container) whose only interface is the loopback. Retry without it --
            // but only for the names it can hide (a loopback name or an address
            // literal) and only for the errors it causes: any other failure, a name
            // that simply does not exist, would just pay for a second full lookup.
            if (results) { ::freeaddrinfo(results); results = nullptr; }
            hints.ai_flags = host.empty() ? AI_PASSIVE : 0;
            rc = ::getaddrinfo(node, port_str.c_str(), &hints, &results);
        }
        if (rc != 0 || results == nullptr) {
            // gai_strerror exists on both POSIX and Windows (via ws2tcpip.h);
            // on Windows it is the ANSI variant gai_strerrorA under the hood.
            throw SocketException(std::string("getaddrinfo: ") + ::gai_strerror(rc), rc);
        }

        // Collect every candidate, not just the first: a host with
        // multiple A/AAAA records (load-balanced services, CDNs, any
        // multi-homed backend) may have working addresses beyond the
        // head of the list, and a caller doing a real connection
        // attempt (tcpConnect) wants to fall back to the next one
        // instead of failing outright when the first is unreachable.
        std::vector<SocketAddress> out;
        try {
            for (addrinfo *p = results; p != nullptr; p = p->ai_next) {
                // ai_socktype is 0 in the hints, so each address comes back once
                // per socket type; keep a single entry per address.
                if (p->ai_socktype != results->ai_socktype) continue;
                out.emplace_back(p->ai_addr, static_cast<socklen_t>(p->ai_addrlen));
            }
        } catch (...) {
            ::freeaddrinfo(results);
            throw;
        }
        ::freeaddrinfo(results);
        return out;
    }

    SocketAddress SocketAddress::resolve(const std::string &host, std::uint16_t port) {
        // resolveAll() throws on failure and never returns an empty
        // vector on success (the list it walks always starts from a
        // non-null `results`), so .front() is safe here.
        return SocketAddress::resolveAll(host, port).front();
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
            std::string host = buf;
            // A link-local address is ambiguous without its interface; the numeric
            // scope id is accepted back by getaddrinfo.
            if (in6->sin6_scope_id != 0) host += "%" + std::to_string(in6->sin6_scope_id);
            return "[" + host + "]:" + std::to_string(this->port());
        }
        if (sa->sa_family == AF_UNIX) {
            const auto *un = reinterpret_cast<const sockaddr_un *>(sa);
            const std::size_t off = offsetof(sockaddr_un, sun_path);
            const std::size_t have = static_cast<std::size_t>(this->len) > off
                                         ? static_cast<std::size_t>(this->len) - off
                                         : 0;
            const std::size_t room = std::min<std::size_t>(have, sizeof(un->sun_path));
            if (room == 0) return "";                              // unnamed (e.g. an unbound client)
#if defined(__linux__)
            if (un->sun_path[0] == '\0') {                         // Linux abstract namespace
                return "@" + std::string(un->sun_path + 1, room - 1);
            }
#else
            // No abstract namespace here: a leading NUL is an empty path, i.e. an unnamed peer (macOS
            // reports an unbound client as a full-size sockaddr_un with a zeroed sun_path).
            if (un->sun_path[0] == '\0') return "";
#endif
            return std::string(un->sun_path, ::strnlen(un->sun_path, room));
        }
        return "<unknown>";
    }

}
