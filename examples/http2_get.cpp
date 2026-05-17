//
// examples/http2_get.cpp -- Minimal HTTP/2 GET client.
//
// Demonstrates: Soccer::Http2Connection over h2c (plain HTTP/2,
// useful for cluster-internal traffic) or h2 (TLS-wrapped, for
// public endpoints).
//
// Usage:
//     ./http2_get                              # GET https://nghttp2.org/
//     ./http2_get <host> <port>                # h2c GET <host>:<port>/
//     ./http2_get <host> <port> <path>         # h2c GET path
//     ./http2_get tls <host> <port> <path>     # h2 over TLS
//
// Requires Soccer built with nghttp2 (SOCCER_HAS_HTTP2). Run with
// no nghttp2 and you get a clean Http2NotImplemented at startup.
//

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "Coroutines.h"
#include "Http2.h"
#include "Soccer.h"
#include "Yarn.hpp"

namespace {

#ifdef SOCCER_HAS_HTTP2
    YarnBall::Task<int> doH2c(std::string host, std::uint16_t port, std::string path) {
        try {
            auto conn = co_await Soccer::Http2Connection::connectPlain(host, port);
            auto resp = co_await conn.request("GET", path);
            std::cout << "status " << resp.status << "\n";
            for (const auto &h : resp.headers) {
                std::cout << h.name << ": " << h.value << "\n";
            }
            std::cout << "\n" << resp.body << "\n";
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
            // Use the system trust store path on macOS / Linux. Adjust if
            // your distro keeps the CA bundle elsewhere.
            opts.caBundleFile = "/etc/ssl/cert.pem";

            auto conn = co_await Soccer::Http2Connection::connect(host, port, opts);
            auto resp = co_await conn.request("GET", path);
            std::cout << "status " << resp.status << "\n";
            for (const auto &h : resp.headers) {
                std::cout << h.name << ": " << h.value << "\n";
            }
            std::cout << "\n" << resp.body.substr(0, 1024) << "\n";
            co_return 0;
        } catch (const std::exception &e) {
            std::cerr << "http2_get error: " << e.what() << "\n";
            co_return 1;
        }
    }
#endif
#endif // SOCCER_HAS_HTTP2
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
            ? static_cast<std::uint16_t>(std::atoi(argv[3]))
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
        ? static_cast<std::uint16_t>(std::atoi(argv[2]))
        : 8080;
    const std::string path = (argc > 3) ? argv[3] : "/";
    return YarnBall::syncWait(doH2c(host, port, path));
#endif
}
