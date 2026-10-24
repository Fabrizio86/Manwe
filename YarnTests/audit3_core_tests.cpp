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

using namespace std::chrono_literals;

// ---- Over-aligned locals in a Task frame ---------------------------------------------

namespace {
    struct alignas(64) CacheLine {
        char bytes[64];
    };

#if defined(__clang__)
    /// Keeps an over-aligned local alive across a suspension and reports whether its address was 64-byte aligned.
    YarnBall::Task<bool> alignedAcrossSuspension() {
        CacheLine line{};
        volatile CacheLine *escaped = &line; // the optimiser cannot fold the address check away
        co_await YarnBall::sleepFor(1ms);
        co_return reinterpret_cast<std::uintptr_t>(const_cast<const CacheLine *>(escaped)) % alignof(CacheLine) == 0;
    }
#endif
}

// Clang allocates a frame with an over-aligned local through the promise's aligned operator new. GCC never calls an
// aligned operator new for a coroutine frame (it assumes the frame base is aligned and uses the plain one), so the check
// would measure the allocator's luck there.
#if defined(__clang__)
TEST(audit3_core_task_frame_honours_over_aligned_locals) {
    for (int i = 0; i < 50; ++i) {
        EXPECT_TRUE(YarnBall::syncWait(alignedAcrossSuspension()));
    }
}
#endif

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
