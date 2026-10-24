//
// examples/chat_signals.cpp -- Multi-subscriber notification via Wire.
//
// Demonstrates: Telegraph::Signal<...> with synchronous + asynchronous
// emit, plus an awaitable Signal::next() for coroutine consumers.

#include <chrono>
#include <iostream>
#include <latch>
#include <mutex>
#include <string>
#include <thread>

#include "Coroutines.h"
#include "Wire.h"
#include "Yarn.hpp"

/// Handlers run on several threads: print each line whole.
static void say(const std::string &line) {
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    std::cout << line << "\n";
}

/**
 * @brief A coroutine that awaits the next signal emission and prints its
 *        payload. Defined as a free function so the Signal pointer lives
 *        in the coroutine frame (avoids the lambda-coroutine lifetime
 *        trap).
 */
YarnBall::Task<> await_one_message(Telegraph::Signal<std::string, int> *sig, std::latch *done) {
    auto [text, room] = co_await sig->next();
    say("[awaiter] room=" + std::to_string(room) + " text=" + text);
    done->count_down();
    co_return;
}

int main() {
    Telegraph::Signal<std::string, int> chat;
    std::latch awaiterDone(1);   // the next() awaiter has printed
    std::latch asyncHandled(2);  // both handlers have run for the emitAsync() message (room 2)

    // Synchronous logger.
    auto logger_id = chat.connect([&asyncHandled](std::string text, int room) {
        say("[logger] room=" + std::to_string(room) + " text=" + text);
        if (room == 2) asyncHandled.count_down();
    });

    // Slow historian: every handler runs inline on the emitting thread under
    // emit(), and as its own task on a Yarn worker under emitAsync().
    auto historian_id = chat.connect([&asyncHandled](std::string text, int room) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        say("[historian] persisted room=" + std::to_string(room) + " text=" + text);
        if (room == 2) asyncHandled.count_down();
    });

    // Coroutine awaiter for a single message.
    YarnBall::coSpawn(await_one_message(&chat, &awaiterDone));

    // next() only catches emissions that happen after it has suspended, so
    // wait until the awaiter shows up in the signal's waiter list.
    while (chat.waiterCount() == 0) std::this_thread::yield();

    std::cout << "-- emit (sync) --\n";
    chat.emit("hello", 1);
    awaiterDone.wait();

    std::cout << "-- emitAsync --\n";
    chat.emitAsync("world", 2);

    // Wait for both async handlers to finish.
    asyncHandled.wait();

    // Drop the historian so the next emit is synchronous-only.
    chat.disconnect(historian_id);
    chat.disconnect(logger_id);

    std::cout << "-- emit after disconnect (no handlers) --\n";
    chat.emit("bye", 0);

    return 0;
}
