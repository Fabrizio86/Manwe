//
// examples/ws_echo_server.cpp -- Minimal WebSocket echo server.
//
// Demonstrates: Soccer::wsServe (accept loop, handshake, connection cap,
// Origin policy) on top of a TcpListener, plus the receive/send/close
// coroutine API. Each accepted client gets its own coroutine, so the
// listener stays responsive while clients chat.
//
// Run with no args to listen on 0.0.0.0:8080, or pass a port number.
// Test from a browser DevTools console:
//
//     const ws = new WebSocket("ws://127.0.0.1:8080/");
//     ws.onmessage = e => console.log("server said:", e.data);
//     ws.onopen    = () => ws.send("hello manwe");
//
// or from `wscat -c ws://127.0.0.1:8080`.
//

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <utility>

#include "Coroutines.h"
#include "Soccer.h"
#include "Yarn.hpp"
#include "WebSocket.h"

constexpr std::uint16_t kDefaultPort = 8080;

/**
 * @brief Echo coroutine: runs once wsServe has completed the WebSocket
 *        handshake, then loops receive -> send back until the peer
 *        closes or a protocol error fires.
 */
YarnBall::Task<void> ws_echo_one(Soccer::WsConnection ws) {
    try {
        while (ws.isOpen()) {
            auto msg = co_await ws.receive();
            if (msg.type == Soccer::WsMessageType::Text) {
                co_await ws.sendText(msg.text());
            } else {
                co_await ws.sendBinary(msg.payload);
            }
        }
    } catch (const Soccer::WsException &e) {
        // Includes peer-initiated close, which surfaces as an exception
        // by design -- the connection is over, drop it.
        std::cerr << "ws client gone: " << e.what() << "\n";
    } catch (const Soccer::SocketException &e) {
        std::cerr << "ws socket error: " << e.what() << "\n";
    }
    co_return;
}

YarnBall::Task<void> ws_echo_server(std::uint16_t port) {
    auto listener = Soccer::TcpListener::bind("0.0.0.0", port);
    std::cout << "ws_echo_server listening on "
              << listener.localAddress().to_string() << "\n";
    // wsServe runs the accept loop and the opening handshake for us and applies the
    // limits below: at most 1000 clients at once, a 10 s handshake, a 60 s idle timeout, and (uncomment
    // allowedOrigins) an Origin allow-list against cross-site WebSocket hijacking.
    Soccer::WsOptions options;
    options.maxConnections = 1000;
    options.handshakeTimeout = std::chrono::seconds{10};
    // A client that stays connected but silent for a minute is dropped (the default is five), so idle sockets
    // cannot hold the 1000 connection slots; clients that want to stay should ping.
    options.idleTimeout = std::chrono::seconds{60};
    // options.allowedOrigins = {"https://app.example.com"};
    co_await Soccer::wsServe(std::move(listener), ws_echo_one, options);
}

/// Parse a TCP port (0-65535); exits with a message on bad input instead
/// of silently truncating through a narrowing cast.
static std::uint16_t parsePort(const char *text) {
    char *end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || v < 0 || v > 65535) {
        std::cerr << "invalid port: " << text << "\n";
        std::exit(1);
    }
    return static_cast<std::uint16_t>(v);
}

int main(int argc, char **argv) {
    const std::uint16_t port = (argc > 1)
        ? parsePort(argv[1])
        : kDefaultPort;
    YarnBall::syncWait(ws_echo_server(port));
    return 0;
}
