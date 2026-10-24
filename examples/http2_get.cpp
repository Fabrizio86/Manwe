//
// examples/http2_get.cpp -- Minimal HTTP/2 GET client.
//
// Demonstrates: Soccer::Http2Connection over h2c (plain HTTP/2,
// useful for cluster-internal traffic) or h2 (TLS-wrapped, for
// public endpoints).
//
// Usage:
//     ./http2_get                              # h2c GET 127.0.0.1:8080/
//     ./http2_get <host> <port>                # h2c GET <host>:<port>/
//     ./http2_get <host> <port> <path>         # h2c GET path
//     ./http2_get tls <host> <port> <path>     # h2 over TLS
//
// Requires Soccer built with nghttp2 (SOCCER_HAS_HTTP2); without it the
// program prints a message and exits with status 1. Both modes print the
// status, the headers and the first 1 KiB of the body.
//

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "Coroutines.h"
#include "Http2.h"
#include "Soccer.h"
#include "Yarn.hpp"

namespace {

#ifdef SOCCER_HAS_HTTP2
    /// Print at most the first 1 KiB of @p body, noting how much was left out.
    void printBody(const std::string &body) {
        constexpr std::size_t limit = 1024;
        std::cout << "\n" << body.substr(0, limit) << "\n";
        if (body.size() > limit) std::cout << "... (" << (body.size() - limit) << " more bytes not shown)\n";
    }

    YarnBall::Task<int> doH2c(std::string host, std::uint16_t port, std::string path) {
        try {
            auto conn = co_await Soccer::Http2Connection::connectPlain(host, port);
            auto resp = co_await conn.request("GET", path);
            std::cout << "status " << resp.status << "\n";
            for (const auto &h : resp.headers) {
                std::cout << h.name << ": " << h.value << "\n";
            }
            printBody(resp.body);
            co_return 0;
        } catch (const std::exception &e) {
            std::cerr << "http2_get error: " << e.what() << "\n";
            co_return 1;
        }
    }

#ifdef SOCCER_HAS_TLS
    YarnBall::Task<int> doH2(std::string host, std::uint16_t port, std::string path) {
        try {
            Soccer::TlsClientOptions opts;
            opts.alpnProtocols = "h2";
            // Pick the first CA bundle that exists (macOS/BSD vs Debian
            // vs RHEL layouts).
            for (const char *cand : {"/etc/ssl/cert.pem",
                                     "/etc/ssl/certs/ca-certificates.crt",
                                     "/etc/pki/tls/certs/ca-bundle.crt"}) {
                if (std::filesystem::exists(cand)) {
                    opts.caBundleFile = cand;
                    break;
                }
            }

            auto conn = co_await Soccer::Http2Connection::connect(host, port, opts);
            auto resp = co_await conn.request("GET", path);
            std::cout << "status " << resp.status << "\n";
            for (const auto &h : resp.headers) {
                std::cout << h.name << ": " << h.value << "\n";
            }
            printBody(resp.body);
            co_return 0;
        } catch (const std::exception &e) {
            std::cerr << "http2_get error: " << e.what() << "\n";
            co_return 1;
        }
    }
#endif
#endif // SOCCER_HAS_HTTP2
}

/// Parse a TCP port (0-65535); exits with a message on bad input instead
/// of silently truncating through a narrowing cast.
[[maybe_unused]] static std::uint16_t parsePort(const char *text) {
    char *end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || v < 0 || v > 65535) {
        std::cerr << "invalid port: " << text << "\n";
        std::exit(1);
    }
    return static_cast<std::uint16_t>(v);
}

int main(int argc, char **argv) {
#ifndef SOCCER_HAS_HTTP2
    (void) argc; (void) argv;
    std::cerr << "http2_get: built without nghttp2 (SOCCER_HAS_HTTP2 is not "
                  "defined); install nghttp2 and rebuild\n";
    return 1;
#else
    if (argc >= 2 && std::string(argv[1]) == "tls") {
#ifdef SOCCER_HAS_TLS
        const std::string host = (argc > 2) ? argv[2] : "nghttp2.org";
        const std::uint16_t port = (argc > 3)
            ? parsePort(argv[3])
            : 443;
        const std::string path = (argc > 4) ? argv[4] : "/";
        return YarnBall::syncWait(doH2(host, port, path));
#else
        std::cerr << "http2_get: built without TLS support\n";
        return 1;
#endif
    }
    const std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
    const std::uint16_t port = (argc > 2)
        ? parsePort(argv[2])
        : 8080;
    const std::string path = (argc > 3) ? argv[3] : "/";
    return YarnBall::syncWait(doH2c(host, port, path));
#endif
}
