//
// Regression tests for the third audit pass (wire).
//

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <new>
#include <stop_token>
#include <thread>
#include <vector>

#include "../Yarn/includes/Yarn.hpp"
#include "../Wire/includes/Wire.h"

using namespace std::chrono_literals;

namespace {
    template<typename Pred>
    bool wire_wait_for(Pred &&p, std::chrono::steady_clock::duration timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!p()) {
            if (std::chrono::steady_clock::now() >= deadline) return p();
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    /// Clears the thread's submission-failure hook on scope exit, however the test ends.
    struct WireSubmitFailureGuard {
        WireSubmitFailureGuard(int skip, int count) { YarnBall::Yarn::failSubmissionsForTest(skip, count); }
        ~WireSubmitFailureGuard() { YarnBall::Yarn::failSubmissionsForTest(0, 0); }
    };

    YarnBall::Task<std::size_t> selectIndex(std::vector<std::shared_ptr<Telegraph::Channel<int>>> chans) {
        auto r = co_await Telegraph::channelSelect(std::move(chans));
        co_return r.index;
    }

    YarnBall::Task<int> receiveForOutcome(std::shared_ptr<Telegraph::Channel<int>> ch, std::chrono::milliseconds t) {
        try {
            auto v = co_await ch->receiveFor(t);
            co_return v ? 1 : 2;
        } catch (const YarnBall::TimeoutException &) {
            co_return 3;
        } catch (const YarnBall::CancelledException &) {
            co_return 4;
        }
    }
}

// Connecting and disconnecting while other threads emit: the in-place edit of the handler list must be ordered after
// every emission's last read of it (ThreadSanitizer-clean).
TEST(audit3_wire_signal_connect_disconnect_races_emit_cleanly) {
    Telegraph::Signal<int> sig;
    std::atomic<long> calls{0};
    std::atomic<bool> stop{false};
    std::atomic<int> notRemoved{0};
    sig.connect([&](int) { calls.fetch_add(1, std::memory_order_relaxed); }); // never removed: emissions always see >= 1

    std::vector<std::thread> emitters;
    for (int t = 0; t < 3; ++t) {
        emitters.emplace_back([&, t] {
            while (!stop.load(std::memory_order_acquire)) {
                if (t == 0) {
                    sig.emitAsync(1);
                    std::this_thread::sleep_for(200us); // emitAsync queues a pool task per handler: do not outrun the pool
                } else {
                    sig.emit(1);
                }
            }
        });
    }
    std::vector<std::thread> writers;
    for (int t = 0; t < 2; ++t) {
        writers.emplace_back([&] {
            for (int i = 0; i < 3000; ++i) {
                auto c = sig.connectScoped([&](int) { calls.fetch_add(1, std::memory_order_relaxed); });
                if (i % 3 == 0) std::this_thread::yield();
                if (!c.disconnect()) notRemoved.fetch_add(1);
            }
        });
    }
    for (auto &w : writers) w.join();
    stop.store(true, std::memory_order_release);
    for (auto &e : emitters) e.join();
    EXPECT_EQ(notRemoved.load(), 0);
    EXPECT_EQ(sig.handlerCount(), std::size_t{1});
    EXPECT_TRUE(calls.load() > 0);
}

// A pull that cannot be started must not leave the pulls that were started parked on their channels: one of them
// would later consume a value nobody reads.
TEST(audit3_wire_select_stops_started_pulls_when_a_spawn_fails) {
    std::vector<std::shared_ptr<Telegraph::Channel<int>>> chans;
    for (int i = 0; i < 3; ++i) chans.push_back(std::make_shared<Telegraph::Channel<int>>());
    {
        WireSubmitFailureGuard guard(1, 1); // the second pull cannot be submitted
        EXPECT_THROWS_AS(YarnBall::syncWait(selectIndex(chans)), std::bad_alloc);
    }
    for (auto &c : chans) {
        EXPECT_TRUE(wire_wait_for([&] { return c->waiterCount() == 0; }, 2s));
        c->send(7); // buffered: no pull is left to take it
    }
    for (auto &c : chans) EXPECT_EQ(c->size(), std::size_t{1});
}

TEST(audit3_wire_select_with_no_pull_started_reports_the_failure) {
    std::vector<std::shared_ptr<Telegraph::Channel<int>>> chans;
    for (int i = 0; i < 2; ++i) chans.push_back(std::make_shared<Telegraph::Channel<int>>());
    {
        WireSubmitFailureGuard guard(0, 1);
        EXPECT_THROWS_AS(YarnBall::syncWait(selectIndex(chans)), std::bad_alloc);
    }
    EXPECT_EQ(chans[0]->waiterCount() + chans[1]->waiterCount(), std::size_t{0});
}

// The deadline and the caller's cancellation arrive together: whichever the timing, the call must end with one of the
// two exceptions and leave no waiter behind. (When the caller's scope is already stopped, CancelledException wins.)
TEST(audit3_wire_receive_for_reports_the_callers_cancellation_over_an_expired_deadline) {
    for (int i = 0; i < 20; ++i) {
        auto ch = std::make_shared<Telegraph::Channel<int>>();
        std::stop_source outer;
        std::thread canceller([&] {
            const auto at = std::chrono::steady_clock::now() + 3ms;
            while (std::chrono::steady_clock::now() < at) {
            }
            outer.request_stop();
        });
        const int r = YarnBall::syncWait(YarnBall::withCancellation(outer.get_token(), receiveForOutcome(ch, 3ms)));
        canceller.join();
        EXPECT_TRUE(r == 3 || r == 4);
        EXPECT_TRUE(wire_wait_for([&] { return ch->waiterCount() == 0; }, 2s));
    }
}

// Deterministic form of the above: the caller's scope is stopped before the call and the deadline is zero, so both are
// true from the start and the caller's cancellation must be what is reported.
TEST(audit3_wire_receive_for_with_a_stopped_scope_and_an_expired_deadline_reports_cancellation) {
    auto ch = std::make_shared<Telegraph::Channel<int>>();
    std::stop_source outer;
    outer.request_stop();
    EXPECT_EQ(YarnBall::syncWait(YarnBall::withCancellation(outer.get_token(), receiveForOutcome(ch, 0ms))), 4);
    EXPECT_TRUE(wire_wait_for([&] { return ch->waiterCount() == 0; }, 2s));
}
