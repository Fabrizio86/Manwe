//
// Regression tests for the fourth audit pass (core).
//

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include "../Yarn/includes/AsyncSync.h"
#include "../Yarn/includes/Cancellation.h"
#include "../Yarn/includes/Coroutines.h"
#include "../Yarn/includes/Metrics.h"
#include "../Yarn/includes/MPMCQueue.h"
#include "../Yarn/includes/Timeout.h"
#include "../Yarn/includes/Timers.h"
#include "../Yarn/includes/Yarn.hpp"

using namespace std::chrono_literals;
using namespace YarnBall;

namespace {

    template<typename Pred>
    bool waitUntil(Pred &&p, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!p()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    struct TimedUser {
        AsyncSemaphore &sem;
        std::atomic<int> &done;
        std::atomic<int> &got;
        std::atomic<int> &lost;
        std::atomic<int> &holders;
        std::atomic<int> &maxHolders;
    };

    Task<> timedUser(TimedUser u, int i) {
        const bool ok = co_await u.sem.acquireFor(std::chrono::microseconds(50 + i % 400));
        if (ok) {
            const int now = ++u.holders;
            int seen = u.maxHolders.load();
            while (now > seen && !u.maxHolders.compare_exchange_weak(seen, now)) {
            }
            ++u.got;
            co_await sleepFor(std::chrono::microseconds(100 + i % 200));
            --u.holders;
            u.sem.release();
        } else {
            ++u.lost;
        }
        ++u.done;
    }
}

TEST(audit4_core_semaphore_acquire_for_times_out_and_does_not_eat_the_next_permit) {
    AsyncSemaphore sem{0};
    EXPECT_FALSE(syncWait(sem.acquireFor(20ms)));
    EXPECT_EQ(sem.available(), std::size_t{0});
    sem.release(); // nobody is parked any more: the permit must be available, not swallowed by the expired waiter
    EXPECT_EQ(sem.available(), std::size_t{1});
    EXPECT_TRUE(syncWait(sem.acquireFor(20ms)));
    EXPECT_EQ(sem.available(), std::size_t{0});
}

TEST(audit4_core_semaphore_acquire_for_is_woken_by_release) {
    AsyncSemaphore sem{0};
    std::thread releaser([&] {
        std::this_thread::sleep_for(30ms);
        sem.release();
    });
    EXPECT_TRUE(syncWait(sem.acquireFor(10s)));
    releaser.join();
    EXPECT_EQ(sem.available(), std::size_t{0});
}

TEST(audit4_core_semaphore_acquire_for_conserves_permits_under_racing_deadlines) {
    // Permits are granted and deadlines expire at the same moments; every grant must reach its caller (and be
    // released), so after the storm the semaphore holds exactly its initial permits and never ran over capacity.
    constexpr int kPermits = 3;
    constexpr int kUsers = 6000;
    AsyncSemaphore sem{kPermits};
    std::atomic<int> done{0}, got{0}, lost{0}, holders{0}, maxHolders{0};
    TimedUser u{sem, done, got, lost, holders, maxHolders};
    for (int i = 0; i < kUsers; ++i) {
        coSpawn(timedUser(u, i));
        if (i % 50 == 0) std::this_thread::sleep_for(100us);
    }
    EXPECT_TRUE(waitUntil([&] { return done.load() == kUsers; }, 20s));
    EXPECT_EQ(got.load() + lost.load(), kUsers);
    EXPECT_TRUE(maxHolders.load() <= kPermits);
    EXPECT_TRUE(waitUntil([&] { return sem.available() == std::size_t{kPermits}; }, 5s));
}

TEST(audit4_core_notify_for_times_out_and_reports_a_delivered_notification) {
    AsyncNotify n;
    EXPECT_FALSE(syncWait(n.notifiedFor(20ms)));
    EXPECT_EQ(n.waiterCount(), std::size_t{0}); // the expired waiter left the queue

    std::atomic<bool> woke{false};
    std::atomic<bool> result{false};
    coSpawn([](AsyncNotify &n, std::atomic<bool> &woke, std::atomic<bool> &result) -> Task<> {
        result = co_await n.notifiedFor(10s);
        woke = true;
    }(n, woke, result));
    EXPECT_TRUE(waitUntil([&] { return n.waiterCount() == 1; }, 5s));
    n.notifyOne();
    EXPECT_TRUE(waitUntil([&] { return woke.load(); }, 5s));
    EXPECT_TRUE(result.load());
}

TEST(audit4_core_notify_for_expired_waiter_does_not_consume_a_later_notification) {
    AsyncNotify n;
    EXPECT_FALSE(syncWait(n.notifiedFor(10ms)));
    std::atomic<bool> got{false};
    coSpawn([](AsyncNotify &n, std::atomic<bool> &got) -> Task<> {
        got = co_await n.notifiedFor(10s);
    }(n, got));
    EXPECT_TRUE(waitUntil([&] { return n.waiterCount() == 1; }, 5s));
    n.notifyOne();
    EXPECT_TRUE(waitUntil([&] { return got.load(); }, 5s));
}

TEST(audit4_core_acquire_for_in_a_cancelled_scope_throws_cancelled) {
    AsyncSemaphore sem{0};
    std::stop_source src;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> finished{false};
    coSpawn([](AsyncSemaphore &sem, std::stop_token tok, std::atomic<bool> &cancelled,
               std::atomic<bool> &finished) -> Task<> {
        try {
            co_await withCancellation(tok, [](AsyncSemaphore &sem) -> Task<> { (void) co_await sem.acquireFor(30s); }(sem));
        } catch (const CancelledException &) {
            cancelled = true;
        }
        finished = true;
    }(sem, src.get_token(), cancelled, finished));
    std::this_thread::sleep_for(30ms);
    src.request_stop();
    EXPECT_TRUE(waitUntil([&] { return finished.load(); }, 5s));
    EXPECT_TRUE(cancelled.load());
    sem.release();
    EXPECT_EQ(sem.available(), std::size_t{1});
}

TEST(audit4_core_mpmc_empty_is_never_true_while_an_element_is_present) {
    MPMCQueue<int> q(64);
    q.enqueue(1); // always present: the queue's size stays at 1 or 2
    std::atomic<bool> stop{false};
    std::thread churn([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            q.enqueue(2);
            int x = 0;
            q.dequeue(x);
        }
    });
    long falseEmpty = 0;
    const auto end = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < end) {
        for (int i = 0; i < 20000; ++i) {
            if (q.empty() || q.Size() == 0) ++falseEmpty;
        }
    }
    stop = true;
    churn.join();
    EXPECT_EQ(falseEmpty, 0L);
}

TEST(audit4_core_metrics_registry_address_is_stable) {
    // The registry is deliberately never destroyed (see the metrics_exit check, which runs the exit-time case).
    metrics::Registry *first = &metrics::Registry::instance();
    for (int i = 0; i < 100; ++i) EXPECT_TRUE(first == &metrics::Registry::instance());
    std::vector<std::thread> ts;
    std::atomic<int> same{0};
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([&] {
            if (&metrics::Registry::instance() == first) ++same;
        });
    }
    for (auto &t : ts) t.join();
    EXPECT_EQ(same.load(), 4);
}

TEST(audit4_core_external_submitters_with_nested_submissions_all_run) {
    // Many foreign threads submit work that submits more work while the pool grows and shrinks: exercises the
    // snapshot pin / refresh paths of dispatch from every angle.
    constexpr int kThreads = 6;
    constexpr int kPer = 4000;
    std::atomic<int> ran{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < kPer; ++i) {
                Yarn::instance()->run([&ran] {
                    Yarn::instance()->run([&ran] { ++ran; });
                });
            }
        });
    }
    for (auto &t : ts) t.join();
    EXPECT_TRUE(waitUntil([&] { return ran.load() == kThreads * kPer; }, 30s));
}
