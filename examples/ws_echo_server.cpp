//
// examples/ws_echo_server.cpp -- Minimal WebSocket echo server.
//
// Demonstrates: Soccer::WsConnection::serverHandshake on top of an
// existing TcpListener, plus the receive/send/close coroutine API.
// Each accepted client gets its own coroutine via coSpawn, so the
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
 * @brief Echo coroutine: complete the WebSocket handshake on the
 *        accepted stream, then loop receive -> send back until the
 *        peer closes or a protocol error fires.
 */
YarnBall::Task<void> ws_echo_one(Soccer::TcpStream client) {
    try {
        auto ws = co_await Soccer::WsConnection::serverHandshake(std::move(client));
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
    while (true) {
        auto client = co_await listener.accept();
        YarnBall::coSpawn(ws_echo_one(std::move(client)));
    }
}

int main(int argc, char **argv) {
    const std::uint16_t port = (argc > 1)
        ? static_cast<std::uint16_t>(std::atoi(argv[1]))
        : kDefaultPort;
    YarnBall::syncWait(ws_echo_server(port));
    return 0;
}
