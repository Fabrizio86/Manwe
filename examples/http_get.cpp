//
// examples/http_get.cpp -- HTTP/1.1 GET client built on Soccer::HttpClient.
//
// Demonstrates: Soccer::HttpClient::get + BufferedReader-backed response
// parsing. Run as:
//     ./http_get example.com 80 /
// Prints the status line, every response header, and the body.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "Coroutines.h"
#include "Soccer.h"
#include "Yarn.hpp"

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
    if (argc < 4) {
        std::cerr << "usage: " << argv[0] << " <host> <port> <path>\n";
        return 1;
    }
    try {
        auto resp = YarnBall::syncWait(Soccer::HttpClient::get(
            argv[1],
            parsePort(argv[2]),
            argv[3]));
        std::cout << "HTTP " << resp.status << ' ' << resp.reason << "\n";
        for (const auto &h : resp.headers) {
            std::cout << h.name << ": " << h.value << "\n";
        }
        std::cout << "\n" << resp.body;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
