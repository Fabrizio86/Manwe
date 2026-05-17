//
// examples/http_get.cpp -- HTTP/1.1 GET client built on Soccer::HttpClient.
//
// Demonstrates: Soccer::HttpClient::get + BufferedReader-backed response
// parsing. Run as:
//     ./http_get example.com 80 /
// Prints the status line, every response header, and the body.

#include <cstdlib>
#include <iostream>
#include <string>

#include "Coroutines.h"
#include "Soccer.h"
#include "Yarn.hpp"

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "usage: " << argv[0] << " <host> <port> <path>\n";
        return 1;
    }
    try {
        auto resp = YarnBall::syncWait(Soccer::HttpClient::get(
            argv[1],
            static_cast<std::uint16_t>(std::atoi(argv[2])),
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
