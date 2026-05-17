//
// examples/graceful_http.cpp -- HTTP/1.1 server that drains cleanly
// on SIGINT / SIGTERM.
//
// Demonstrates:
//   - YarnBall::SignalSet capturing POSIX signals as coroutine events
//   - Bridging the signal into a std::stop_source whose token is
//     passed to HttpServer::serve(), so the accept loop exits after
//     its next iteration without aborting in-flight handlers
//
// Run with no args to listen on 0.0.0.0:8080, or pass a port:
//
//     ./graceful_http
//     curl http://127.0.0.1:8080/         # -> "hello manwe"
//     curl http://127.0.0.1:8080/health   # -> "ok"
//     Ctrl-C                              # exits cleanly
//

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stop_token>
#include <utility>

#include "Coroutines.h"
#include "Soccer.h"
#include "Yarn.hpp"

#if !defined(_WIN32)
#include <csignal>
#include "SignalSet.h"
#endif

constexpr std::uint16_t kDefaultPort = 8080;

#if !defined(_WIN32)
/**
 * @brief Bridge coroutine: await the next captured signal, log it,
 *        then flip the supplied stop_source. The HttpServer's accept
 *        loop checks the matching stop_token and returns once it
 *        observes the request.
 */
YarnBall::Task<void> watchSignals(YarnBall::SignalSet *sigs,
                                    std::stop_source src) {
    int sig = co_await sigs->next();
    std::cerr << "graceful_http: caught signal " << sig
              << ", draining\n";
    src.request_stop();
    co_return;
}
#endif

YarnBall::Task<void> runServer(std::uint16_t port) {
    Soccer::HttpServer server("0.0.0.0", port);

    server.route("GET", "/", [](Soccer::HttpRequest)
            -> YarnBall::Task<Soccer::HttpResponse> {
        Soccer::HttpResponse r;
        r.status = 200;
        r.body = "hello manwe";
        co_return r;
    });

    server.route("GET", "/health", [](Soccer::HttpRequest)
            -> YarnBall::Task<Soccer::HttpResponse> {
        Soccer::HttpResponse r;
        r.status = 200;
        r.body = "ok";
        co_return r;
    });

    std::cout << "graceful_http listening on 0.0.0.0:" << port << "\n";

#if !defined(_WIN32)
    YarnBall::SignalSet sigs({SIGINT, SIGTERM});
    std::stop_source src;
    YarnBall::coSpawn(watchSignals(&sigs, src));
    co_await server.serve(src.get_token());
#else
    // Windows: SignalSet not implemented yet. Server runs until the
    // process is killed; replace with SetConsoleCtrlHandler when the
    // SignalSet Windows path lands.
    co_await server.serve();
#endif

    std::cerr << "graceful_http: drained, exiting\n";
    co_return;
}

int main(int argc, char **argv) {
    const std::uint16_t port = (argc > 1)
        ? static_cast<std::uint16_t>(std::atoi(argv[1]))
        : kDefaultPort;
    YarnBall::syncWait(runServer(port));
    return 0;
}
