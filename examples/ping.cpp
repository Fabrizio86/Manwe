//
// examples/ping.cpp -- Minimal ICMP ping using a Soccer RawSocket.
//
// Demonstrates: Soccer::RawSocket + Soccer::IcmpEcho helpers + the
// Reactor / Yarn coroutine path on a raw IP socket.
//
// Run as:   sudo ./bin/ping <host> [count]
// Requires CAP_NET_RAW or root because of the underlying SOCK_RAW
// socket.

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <unistd.h>

#include "Coroutines.h"
#include "Soccer.h"
#include "Yarn.hpp"

using clk = std::chrono::steady_clock;

/**
 * @brief Send one Echo Request, wait for the matching Echo Reply, return
 *        round-trip time in microseconds (or -1 on a parse / sequence
 *        mismatch).
 */
YarnBall::Task<long long> ping_one(Soccer::RawSocket &sock,
                                   const Soccer::SocketAddress &dest,
                                   std::uint16_t identifier,
                                   std::uint16_t sequence) {
    const std::byte payload[] = {
        std::byte{'m'}, std::byte{'a'}, std::byte{'n'}, std::byte{'w'}, std::byte{'e'},
    };
    auto packet = Soccer::IcmpEcho::buildRequest(identifier, sequence,
                                                  std::span<const std::byte>(payload));
    const auto t0 = clk::now();
    co_await sock.sendTo(std::span<const std::byte>(packet.data(), packet.size()), dest);

    constexpr std::size_t kRecvBufBytes = 2048;
    std::array<std::byte, kRecvBufBytes> buf{};
    while (true) {
        Soccer::SocketAddress from;
        std::size_t n = co_await sock.recv(buf, &from);
        auto parsed = Soccer::IcmpEcho::parse(
            std::span<const std::byte>(buf.data(), n), /*skip_ip=*/true);
        if (!parsed) continue;
        if (parsed->type != Soccer::IcmpType::EchoReply) continue;
        if (parsed->identifier != identifier || parsed->sequence != sequence) continue;
        const auto t1 = clk::now();
        co_return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    }
}

YarnBall::Task<void> ping_loop(std::string target, int count) {
    auto sock = Soccer::RawSocket::icmp(AF_INET);
    const auto dest = Soccer::SocketAddress::resolve(target, 0);

    std::cout << "PING " << target << " (" << dest.to_string() << ")\n";
    const auto identifier = static_cast<std::uint16_t>(::getpid() & 0xFFFFu);
    for (int i = 1; i <= count; ++i) {
        try {
            const long long us =
                co_await ping_one(sock, dest, identifier, static_cast<std::uint16_t>(i));
            std::printf("seq=%d time=%.3f ms\n", i, static_cast<double>(us) / 1000.0);
        } catch (const Soccer::SocketException &e) {
            std::cerr << "seq=" << i << " error: " << e.what() << "\n";
        }
    }
    co_return;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <host> [count]\n"
                  << "(run with sudo or CAP_NET_RAW)\n";
        return 1;
    }
    const int count = (argc > 2) ? std::atoi(argv[2]) : 4;
    try {
        YarnBall::syncWait(ping_loop(argv[1], count));
    } catch (const Soccer::SocketException &e) {
        std::cerr << "fatal: " << e.what() << "\n";
        if (e.errorCode() == 1 /* EPERM */) {
            std::cerr << "(raw ICMP needs root / CAP_NET_RAW)\n";
        }
        return 1;
    }
    return 0;
}
