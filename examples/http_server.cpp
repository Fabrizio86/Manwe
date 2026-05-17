//
// examples/http_server.cpp -- HTTP/1.1 server built on Soccer::HttpServer.
//
// Demonstrates the recommended path for HTTP services: register
// routes against an HttpServer, hand it to syncWait. Shared state
// across handlers is plumbed via an AsyncMutex behind a shared_ptr.
//
// Routes:
//   GET  /         -> 200 plain text greeting
//   GET  /count    -> 200 with a request counter (AsyncMutex-protected)
//   POST /echo     -> 200 with the request body echoed back
//   any  *         -> 404 (HttpServer's built-in default)
//
// Run as:
//     ./http_server 8080
// then:
//     curl -v http://127.0.0.1:8080/
//     curl -v http://127.0.0.1:8080/count
//     curl -v http://127.0.0.1:8080/count
//     curl -v -X POST -d 'hello' http://127.0.0.1:8080/echo

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "AsyncSync.h"
#include "Coroutines.h"
#include "Soccer.h"
#include "Yarn.hpp"

namespace {

    /**
     * @brief Shared state across the server's lifetime. Held by
     *        shared_ptr so the route handlers (which copy the
     *        shared_ptr by value) keep it alive for as long as any
     *        in-flight handler runs.
     */
    struct Counter {
        YarnBall::AsyncMutex mu;
        int n = 0;
    };

}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <port>\n";
        return 1;
    }
    const std::uint16_t port = static_cast<std::uint16_t>(std::atoi(argv[1]));

    auto counter = std::make_shared<Counter>();
    Soccer::HttpServer server("0.0.0.0", port);

    server.route("GET", "/",
        [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.status = 200;
            r.reason = "OK";
            r.body = "hello from manwe-http\n";
            r.headers.push_back({"Content-Type", "text/plain"});
            co_return r;
        });

    server.route("GET", "/count",
        [counter](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            int now;
            {
                auto guard = co_await counter->mu.lock();
                now = ++counter->n;
            }
            Soccer::HttpResponse r;
            r.status = 200;
            r.reason = "OK";
            r.body = "request #" + std::to_string(now) + "\n";
            r.headers.push_back({"Content-Type", "text/plain"});
            co_return r;
        });

    server.route("POST", "/echo",
        [](Soccer::HttpRequest req) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.status = 200;
            r.reason = "OK";
            r.body = std::move(req.body);
            r.headers.push_back({"Content-Type", "text/plain"});
            co_return r;
        });

    std::cout << "listening on 0.0.0.0:" << server.localAddress().port() << "\n";
    YarnBall::syncWait(server.serve());
    return 0;
}
