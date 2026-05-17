//
// examples/chat_signals.cpp -- Multi-subscriber notification via Wire.
//
// Demonstrates: Telegraph::Signal<...> with synchronous + asynchronous
// emit, plus an awaitable Signal::next() for coroutine consumers.

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "Coroutines.h"
#include "Wire.h"
#include "Yarn.hpp"

/**
 * @brief A coroutine that awaits the next signal emission and prints its
 *        payload. Defined as a free function so the Signal pointer lives
 *        in the coroutine frame (avoids the lambda-coroutine lifetime
 *        trap).
 */
YarnBall::Task<void> await_one_message(Telegraph::Signal<std::string, int> *sig) {
    auto [text, room] = co_await sig->next();
    std::cout << "[awaiter] room=" << room << " text=" << text << "\n";
    co_return;
}

int main() {
    Telegraph::Signal<std::string, int> chat;

    // Synchronous logger.
    auto logger_id = chat.connect([](std::string text, int room) {
        std::cout << "[sync logger] room=" << room << " text=" << text << "\n";
    });

    // Asynchronous historian (runs on a Yarn worker per emission).
    auto historian_id = chat.connect([](std::string text, int room) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::cout << "[async historian] persisted room=" << room
                  << " text=" << text << "\n";
    });

    // Coroutine awaiter for a single message.
    YarnBall::coSpawn(await_one_message(&chat));

    std::cout << "-- emit (sync) --\n";
    chat.emit("hello", 1);

    std::cout << "-- emitAsync --\n";
    chat.emitAsync("world", 2);

    // Wait for the async handlers + awaiter to complete.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Drop the historian so the next emit is synchronous-only.
    chat.disconnect(historian_id);
    chat.disconnect(logger_id);

    std::cout << "-- emit after disconnect (no handlers) --\n";
    chat.emit("bye", 0);

    return 0;
}
