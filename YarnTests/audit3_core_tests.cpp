//
// Regression tests for the third audit pass (core).
//

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <new>
#include <utility>
#include <stop_token>
#include <thread>
#include <vector>

#include "../Yarn/includes/Coroutines.h"
#include "../Yarn/includes/SmallObjectPool.h"
#include "../Yarn/includes/Timeout.h"
#include "../Yarn/includes/Timers.h"
#include "../Yarn/includes/Yarn.hpp"
#include "../Yarn/includes/YarnBall.hpp"

using namespace std::chrono_literals;

// ---- Over-aligned locals in a Task frame ---------------------------------------------

namespace {
    struct alignas(64) CacheLine {
        char bytes[64];
    };

    /// Keeps an over-aligned local alive across a suspension and reports whether its address was 64-byte aligned.
    YarnBall::Task<bool> alignedAcrossSuspension() {
        CacheLine line{};
        volatile CacheLine *escaped = &line; // the optimiser cannot fold the address check away
        co_await YarnBall::sleepFor(1ms);
        co_return reinterpret_cast<std::uintptr_t>(const_cast<const CacheLine *>(escaped)) % alignof(CacheLine) == 0;
    }
}

// Clang allocates a frame with an over-aligned local through the promise's aligned operator new; GCC always calls the
// plain one, so there every frame comes from 64-byte aligned blocks. Either way an alignas(64) local must be aligned.
TEST(audit3_core_task_frame_honours_over_aligned_locals) {
    for (int i = 0; i < 50; ++i) {
        EXPECT_TRUE(YarnBall::syncWait(alignedAcrossSuspension()));
    }
}

// ---- Frame pool ----------------------------------------------------------------------------

TEST(audit3_core_frame_alloc_and_free_round_trip_every_size_class) {
    std::vector<std::pair<void *, std::size_t>> blocks;
    for (std::size_t n : {1u, 64u, 65u, 96u, 128u, 200u, 256u, 300u, 512u, 700u, 1024u, 1025u, 4096u}) {
        void *p = YarnBall::detail::frameAlloc(n);
        EXPECT_TRUE(p != nullptr);
        std::memset(p, 0xAB, n); // the whole requested size is writable
        blocks.emplace_back(p, n);
    }
    for (auto &[p, n] : blocks) YarnBall::detail::frameFree(p, n);
}

TEST(audit3_core_pool_blocks_are_interchangeable_with_malloc) {
    // The pooled and unpooled paths take full-slot malloc blocks from the same allocator, so a block from one may be
    // released through the other (translation units built with different frame-pool settings can mix them).
    using Pool = YarnBall::detail::SmallObjectPool<64, 8>;
    void *a = Pool::allocate();
    std::memset(a, 1, 64);
    std::free(a);
    void *b = std::malloc(64);
    std::memset(b, 2, 64);
    Pool::deallocate(b);
    void *c = Pool::allocate();
    std::memset(c, 3, 64); // the recycled block is a full slot
    Pool::deallocate(c);
}

namespace {
    /// Frees a pooled block from a static destructor, i.e. after main() has returned and other statics are being torn down.
    struct LateFree {
        void *block = nullptr;
        ~LateFree() {
            if (block) YarnBall::detail::frameFree(block, 100);
        }
    };
    LateFree gLateFree;
}

TEST(audit3_core_block_freed_during_static_destruction_is_safe) {
    // Allocated on another thread, so freeing it later takes the shared-pool path; the shared state must outlive statics.
    void *p = nullptr;
    std::thread([&] { p = YarnBall::detail::frameAlloc(100); }).join();
    EXPECT_TRUE(p != nullptr);
    gLateFree.block = p;
}

// ---- Timer arguments beyond the nanosecond range ---------------------------------------

TEST(audit3_core_to_nanos_saturates) {
    using namespace YarnBall::detail;
    EXPECT_TRUE(toNanos(std::chrono::seconds::max()) == std::chrono::nanoseconds::max());
    EXPECT_TRUE(toNanos(std::chrono::hours::max()) == std::chrono::nanoseconds::max());
    EXPECT_TRUE(toNanos(std::chrono::years(1000)) == std::chrono::nanoseconds::max());
    EXPECT_TRUE(toNanos(std::chrono::seconds(-5)) == std::chrono::nanoseconds::zero());
    EXPECT_TRUE(toNanos(std::chrono::milliseconds(3)) == std::chrono::nanoseconds(3'000'000));
}

TEST(audit3_core_deadline_token_with_huge_duration_is_not_signalled) {
    std::stop_source withdraw;
    auto a = YarnBall::deadlineToken(std::chrono::hours::max(), withdraw.get_token());
    auto b = YarnBall::deadlineToken(std::chrono::seconds::max(), withdraw.get_token());
    auto c = YarnBall::deadlineToken(std::chrono::years(1000), withdraw.get_token());
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(a.stop_requested());
    EXPECT_FALSE(b.stop_requested());
    EXPECT_FALSE(c.stop_requested());
    withdraw.request_stop();
}

// ---- Joined withTimeout whose spawn fails after the timer fired ------------------------

TEST(audit3_core_joined_timeout_publishes_when_spawn_fails_after_the_timer_won) {
    YarnBall::detail::TimeoutState<int> st;
    st.join = true;
    st.expire(); // the timer wins first and waits for a runner that will never exist
    EXPECT_FALSE(st.published);
    st.failSpawn(std::make_exception_ptr(std::bad_alloc()));
    EXPECT_TRUE(st.published);
    EXPECT_TRUE(st.exception != nullptr);
}

TEST(audit3_core_unjoined_timeout_fail_spawn_publishes_the_spawn_error) {
    YarnBall::detail::TimeoutState<int> st;
    st.failSpawn(std::make_exception_ptr(std::bad_alloc()));
    EXPECT_TRUE(st.published);
    bool isBadAlloc = false;
    try { std::rethrow_exception(st.exception); } catch (const std::bad_alloc &) { isBadAlloc = true; } catch (...) {}
    EXPECT_TRUE(isBadAlloc);
}

// ---- Submission bursts ---------------------------------------------------------------------

namespace {
    bool coreWaitUntil(const std::function<bool()> &p, std::chrono::milliseconds limit) {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < end) {
            if (p()) return true;
            std::this_thread::sleep_for(1ms);
        }
        return p();
    }
}

TEST(audit3_core_a_burst_of_external_submissions_runs_every_task_exactly_once) {
    constexpr int kTasks = 50000;
    std::vector<std::atomic<int>> ran(kTasks);
    for (auto &r : ran) r.store(0);
    for (int i = 0; i < kTasks; ++i) {
        YarnBall::run([&ran, i] { ran[i].fetch_add(1, std::memory_order_relaxed); });
    }
    EXPECT_TRUE(coreWaitUntil(
        [&] {
            for (auto &r : ran)
                if (r.load() == 0) return false;
            return true;
        },
        30000ms));
    std::this_thread::sleep_for(20ms);
    int wrong = 0;
    for (auto &r : ran) wrong += r.load() != 1;
    EXPECT_EQ(wrong, 0);
}

TEST(audit3_core_a_burst_to_one_inbox_still_spreads_over_the_workers) {
    // Consecutive submissions from one thread are aimed at one worker's inbox; the other workers must still pick the
    // tasks up (stealing from the inbox / being woken), or a burst of slow tasks would run one after the other.
    const int workers = static_cast<int>(std::thread::hardware_concurrency());
    if (workers < 4) return;
    constexpr int kTasks = 48;
    static constexpr auto kEach = 25ms;
    std::atomic<int> done{0};
    std::mutex idsMu;
    std::set<std::thread::id> ids;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kTasks; ++i) {
        YarnBall::run([&] {
            {
                std::lock_guard<std::mutex> lk(idsMu);
                ids.insert(std::this_thread::get_id());
            }
            std::this_thread::sleep_for(kEach);
            done.fetch_add(1);
        });
    }
    EXPECT_TRUE(coreWaitUntil([&] { return done.load() == kTasks; }, 30000ms));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    const auto serial = kEach * kTasks;
    std::lock_guard<std::mutex> lk(idsMu);
    EXPECT_TRUE(ids.size() >= 3);      // the burst reached several workers
    EXPECT_TRUE(elapsed < serial / 2); // and ran in parallel, not at one worker's pace (a 4-core runner gives ~4x)
}

TEST(audit3_core_submitting_while_the_pool_grows_and_shrinks_is_safe) {
    // Blocking tasks beyond the worker count make the pool add temp fibers (the worker list snapshot is replaced)
    // while other threads keep submitting and stealing: the snapshot a submitter or stealer is reading must stay valid.
    std::atomic<bool> stop{false};
    std::atomic<long> ran{0};
    std::atomic<long> submitted{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < 3; ++p) {
        producers.emplace_back([&] {
            while (!stop.load()) {
                for (int i = 0; i < 64; ++i) {
                    submitted.fetch_add(1, std::memory_order_relaxed);
                    YarnBall::run([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
                }
                std::this_thread::yield();
            }
        });
    }
    const int blockers = static_cast<int>(std::thread::hardware_concurrency()) * 2 + 2;
    for (int round = 0; round < 6; ++round) {
        std::atomic<int> blocked{0};
        for (int i = 0; i < blockers; ++i) {
            YarnBall::run([&blocked] {
                std::this_thread::sleep_for(60ms);
                blocked.fetch_add(1);
            });
        }
        EXPECT_TRUE(coreWaitUntil([&] { return blocked.load() == blockers; }, 30000ms));
    }
    stop.store(true);
    for (auto &t : producers) t.join();
    // The tasks capture locals of this test: none may be left pending when it returns.
    EXPECT_TRUE(coreWaitUntil([&] { return ran.load() == submitted.load(); }, 30000ms));
    EXPECT_TRUE(ran.load() > 0);
}
