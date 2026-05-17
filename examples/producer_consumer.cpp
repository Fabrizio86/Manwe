//
// examples/producer_consumer.cpp -- Wire::Channel<T> producer / consumer.
//
// Demonstrates: a producer coroutine that pushes integers into a Channel
// and a consumer coroutine that drains until close. Tracks how the
// producer + consumer run concurrently on the Yarn pool.

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "Coroutines.h"
#include "Wire.h"
#include "Yarn.hpp"

/**
 * @brief Producer: push N integers into the channel with small pauses
 *        between them, then close.
 */
YarnBall::Task<void> produce(std::shared_ptr<Telegraph::Channel<int>> ch, int n) {
    for (int i = 0; i < n; ++i) {
        co_await YarnBall::scheduleOn(YarnBall::Yarn::instance());
        ch->send(i * 10);
        std::cout << "[producer] sent " << (i * 10) << "\n";
    }
    ch->close();
    std::cout << "[producer] closed channel\n";
    co_return;
}

/**
 * @brief Consumer: receive until the channel is closed; print every value.
 */
YarnBall::Task<int> consume(std::shared_ptr<Telegraph::Channel<int>> ch) {
    int count = 0;
    while (true) {
        auto v = co_await ch->receive();
        if (!v) break; // channel closed
        std::cout << "[consumer] got " << *v << "\n";
        ++count;
    }
    co_return count;
}

int main() {
    auto channel = std::make_shared<Telegraph::Channel<int>>();

    YarnBall::coSpawn(produce(channel, 5));
    int received = YarnBall::syncWait(consume(channel));

    std::cout << "consumed " << received << " values\n";
    return 0;
}
