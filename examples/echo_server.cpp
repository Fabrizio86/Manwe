//
// examples/echo_server.cpp -- Minimal TCP echo server.
//
// Demonstrates: Soccer::TcpListener + Soccer::TcpStream + coSpawn for
// per-connection handling. Run with no args to listen on 0.0.0.0:8080, or
// pass a port number. Connect from another terminal with `nc 127.0.0.1 8080`.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>

#include "Coroutines.h"
#include "Soccer.h"
#include "Yarn.hpp"

constexpr std::size_t kEchoBufSize = 4096;
constexpr std::uint16_t kDefaultPort = 8080;
// Limits, as in ws_echo_server: bound the sockets and descriptors a client can pin.
constexpr std::size_t kMaxConnections = 1000;
constexpr auto kIdleTimeout = std::chrono::seconds{60};

/**
 * @brief Echo coroutine: read up to @c kBufSize bytes, write them back,
 *        repeat until the peer half-closes or stays silent for
 *        @c kIdleTimeout (each read and each write gets that long).
 *
 * The timeout is a Soccer::IoDeadline, not a withTimeout around the read: it
 * shuts the socket down when it expires, so the parked read or write fails
 * and this coroutine unwinds on its own. (withTimeout returns while a cancelled
 * operation may still be finishing, which would leave it using @c client and
 * @c buf after this frame is gone.)
 */
YarnBall::Task<> echo_one(Soccer::TcpStream client) {
    std::array<std::byte, kEchoBufSize> buf{};
    Soccer::IoDeadline idle; // declared after client, so it is disarmed before the socket is closed
    try {
        while (true) {
            idle.arm(client.fd(), kIdleTimeout);
            std::size_t n = co_await client.read(buf);
            if (n == 0) break;
            idle.arm(client.fd(), kIdleTimeout);
            co_await client.write(std::span<const std::byte>(buf.data(), n));
        }
    } catch (const Soccer::SocketException &e) {
        if (!idle.expired()) std::cerr << "client error: " << e.what() << "\n";
    }
    if (idle.expired()) std::cerr << "client idle, closing\n";
}

/**
 * @brief Listener loop: tcpServe accepts, spawns an echo coroutine per
 *        connection so concurrent clients are independent, refuses
 *        connections beyond @c kMaxConnections, and rides out transient
 *        accept errors (fd exhaustion, a peer that reset first).
 */
YarnBall::Task<> echo_server(std::uint16_t port) {
    auto listener = Soccer::TcpListener::bind("0.0.0.0", port);
    std::cout << "echo_server listening on " << listener.localAddress().to_string() << "\n";
    co_await Soccer::tcpServe(std::move(listener), echo_one, {}, kMaxConnections);
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
    YarnBall::syncWait(echo_server(port));
    return 0;
}
