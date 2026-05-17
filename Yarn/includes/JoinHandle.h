//
// Created by Fabrizio Paino on 2026-05-16.
//
// JoinHandle<T>: spawn a Task<T> on the Yarn pool AND retain a handle
// you can await later for its result. The gap between coSpawn (which
// is fire-and-forget) and whenAll (which awaits a batch up front).
//
// Typical use:
//
//   auto h = spawnJoinable(slowComputation(42));
//   // ... do other work in the meantime; the task runs in the
//   // background on the pool ...
//   int result = co_await h.join();
//
// Multiple consumers cannot await the same JoinHandle; doing so is
// undefined. Use a Telegraph::BoundedChannel or std::shared_future-
// style primitive if you need multi-consumer broadcast.
//
// Synchronisation model: lock-free latch via a single atomic word
// (no mutex, no condition variable). The word encodes one of:
//   - 0                   : no waiter, result not yet published
//   - <handle address>    : a waiter has parked on the handle
//   - kReadyBit (== 1)    : result has been published; any parked
//                           waiter has already been dispatched
//
// Coroutine frame addresses are at least pointer-aligned, so bit 0
// is safe to use as the "ready" sentinel.
//

#ifndef YARN_JOIN_HANDLE_H
#define YARN_JOIN_HANDLE_H

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "Coroutines.h"

namespace YarnBall {

    namespace detail {

        /**
         * @brief Sentinel for the "result has been published" state in
         *        @ref JoinState::state. Bit 0 of an aligned coroutine
         *        handle address is never set, so this value is
         *        unambiguous.
         */
        inline constexpr std::uintptr_t kJoinReadyBit = 1u;

        /**
         * @struct JoinState
         * @brief Shared latch state between the spawned task and the
         *        joiner. Lock-free: a single atomic word coordinates
         *        the publish/park handshake, and the value/exception
         *        slots are published under release / observed under
         *        acquire by way of the same word.
         *
         * @tparam T Result type; @c void is specialised below.
         */
        template<typename T>
        struct JoinState {
            /**
             * @brief Latch word. See file header for the encoding.
             */
            std::atomic<std::uintptr_t> state{0};

            /**
             * @brief Result slot. Written by the runner before the
             *        release exchange on @ref state. Read by the
             *        joiner after observing @c kJoinReadyBit.
             */
            std::optional<T> value;

            /**
             * @brief Captured exception, if the user task threw.
             *        Written/read under the same ordering as @ref value.
             */
            std::exception_ptr exception{};
        };

        template<>
        struct JoinState<void> {
            std::atomic<std::uintptr_t> state{0};
            std::exception_ptr exception{};
        };

        /**
         * @brief Publish the result and wake the parked waiter (if any).
         *        Called from the join runner once the user's task has
         *        completed.
         */
        template<typename T>
        inline void publishJoinReady(std::shared_ptr<JoinState<T>> &state) {
            const auto prev = state->state.exchange(kJoinReadyBit,
                                                    std::memory_order_acq_rel);
            if (prev == 0 || prev == kJoinReadyBit) return;
            auto h = std::coroutine_handle<>::from_address(
                reinterpret_cast<void *>(prev));
            // Resume the joiner via Yarn rather than inline -- chained
            // joins (each runner waking the next) would otherwise grow
            // the stack without bound.
            try {
                std::unique_ptr<ITask> ct{new CoroutineITask(h)};
                Yarn::instance()->run(std::move(ct));
            } catch (...) {
                h.resume();
            }
        }

        /**
         * @brief The "runner" coroutine: awaits the user's task, stores
         *        its result (or exception) in the shared state, and
         *        flips the latch via @ref publishJoinReady.
         */
        template<typename T>
        Task<void> joinRunner(std::shared_ptr<JoinState<T>> state,
                              Task<T> task) {
            try {
                if constexpr (std::is_void_v<T>) {
                    co_await std::move(task);
                } else {
                    state->value.emplace(co_await std::move(task));
                }
            } catch (...) {
                state->exception = std::current_exception();
            }
            publishJoinReady(state);
            co_return;
        }

        /**
         * @struct JoinAwaiter
         * @brief Awaiter for @c JoinHandle::join(). Fast-path: if the
         *        runner has already published, symmetric-transfer back
         *        to the joiner. Slow-path: CAS our handle into the
         *        latch word and suspend; the runner will dispatch our
         *        wake when it publishes.
         *
         * The slow-path CAS races with the runner's publish exchange:
         *  - We win (state was 0): we are parked; runner sees our
         *    address and dispatches our wake.
         *  - We lose (state was @c kJoinReadyBit): we resume inline.
         * No mutex, no missed wake.
         */
        template<typename T>
        struct JoinAwaiter {
            std::shared_ptr<JoinState<T>> state;

            bool await_ready() const noexcept {
                return state->state.load(std::memory_order_acquire) == kJoinReadyBit;
            }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
                std::uintptr_t expected = 0;
                const auto encoded =
                    reinterpret_cast<std::uintptr_t>(h.address());
                if (state->state.compare_exchange_strong(
                        expected, encoded,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return std::noop_coroutine();
                }
                // CAS failed: the only other value the runner ever
                // writes is kJoinReadyBit. Result is ready, resume
                // inline.
                return h;
            }

            auto await_resume() {
                if (state->exception) {
                    std::rethrow_exception(state->exception);
                }
                if constexpr (!std::is_void_v<T>) {
                    return std::move(*state->value);
                }
            }
        };

    } // namespace detail


    /**
     * @class JoinHandle
     * @brief Handle to a Task running in the background on the Yarn
     *        pool. Single-consumer: @c join() is the only way to
     *        retrieve the result; calling it more than once (or after
     *        the @c JoinHandle has been moved-from) is undefined.
     *
     * Move-only. Dropping the handle without joining is permitted and
     * does not stop the underlying task -- it continues running and
     * its result is discarded.
     */
    template<typename T>
    class JoinHandle final {
    public:
        JoinHandle() noexcept = default;

        explicit JoinHandle(std::shared_ptr<detail::JoinState<T>> s) noexcept
            : state(std::move(s)) {
        }

        JoinHandle(const JoinHandle &) = delete;
        JoinHandle &operator=(const JoinHandle &) = delete;

        JoinHandle(JoinHandle &&other) noexcept = default;
        JoinHandle &operator=(JoinHandle &&other) noexcept = default;

        ~JoinHandle() = default;

        /**
         * @brief Await this handle. Resumes when the background task
         *        completes, with its result (or its exception).
         */
        Task<T> join() {
            detail::JoinAwaiter<T> aw{state};
            if constexpr (std::is_void_v<T>) {
                co_await aw;
                co_return;
            } else {
                T value = co_await aw;
                co_return value;
            }
        }

        /**
         * @return @c true if the background task has finished. Cheap
         *         atomic load; non-blocking.
         */
        bool done() const noexcept {
            return state &&
                   state->state.load(std::memory_order_acquire) == detail::kJoinReadyBit;
        }

    private:
        std::shared_ptr<detail::JoinState<T>> state;
    };


    /**
     * @brief Spawn @p task on the Yarn pool and return a @ref JoinHandle
     *        for retrieving its result. The task starts running
     *        immediately; the joiner can await the handle at any
     *        later point.
     */
    template<typename T>
    JoinHandle<T> spawnJoinable(Task<T> task) {
        auto state = std::make_shared<detail::JoinState<T>>();
        coSpawn(detail::joinRunner<T>(state, std::move(task)));
        return JoinHandle<T>{state};
    }

}

#endif // YARN_JOIN_HANDLE_H
