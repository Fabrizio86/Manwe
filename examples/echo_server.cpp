//
// examples/echo_server.cpp -- Minimal TCP echo server.
//
// Demonstrates: Soccer::TcpListener + Soccer::TcpStream + coSpawn for
// per-connection handling. Run with no args to listen on 0.0.0.0:8080, or
// pass a port number. Connect from another terminal with `nc 127.0.0.1 8080`.

#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>

#include "Coroutines.h"
#include "Soccer.h"
#include "Yarn.hpp"

/**
 * @brief Echo coroutine: read up to @c kBufSize bytes, write them back,
 *        repeat until the peer half-closes.
 */
constexpr std::size_t kEchoBufSize = 4096;
constexpr std::uint16_t kDefaultPort = 8080;

YarnBall::Task<void> echo_one(Soccer::TcpStream client) {
    std::array<std::byte, kEchoBufSize> buf{};
    try {
        while (true) {
            std::size_t n = co_await client.read(buf);
            if (n == 0) co_return;
            co_await client.write(std::span<const std::byte>(buf.data(), n));
        }
    } catch (const Soccer::SocketException &e) {
        std::cerr << "client error: " << e.what() << "\n";
    }
}

/**
 * @brief Listener loop: accept forever, spawn an echo coroutine per
 *        connection so concurrent clients are independent.
 */
YarnBall::Task<void> echo_server(std::uint16_t port) {
    auto listener = Soccer::TcpListener::bind("0.0.0.0", port);
    std::cout << "echo_server listening on " << listener.localAddress().to_string() << "\n";
    while (true) {
        auto client = co_await listener.accept();
        YarnBall::coSpawn(echo_one(std::move(client)));
    }
}

int main(int argc, char **argv) {
    const std::uint16_t port = (argc > 1)
        ? static_cast<std::uint16_t>(std::atoi(argv[1]))
        : kDefaultPort;
    YarnBall::syncWait(echo_server(port));
    return 0;
}
