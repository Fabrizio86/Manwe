//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef YARN_WHEN_ALL_H
#define YARN_WHEN_ALL_H

#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "Coroutines.h"

namespace YarnBall {

    namespace detail {

        /**
         * @struct WhenAllState
         * @brief Shared state between the @c whenAll coroutine and its
         *        spawned per-task workers.
         *
         * The atomic counter @c remaining starts at @c N+1, where N is the
         * number of subtasks and the extra @c 1 represents "the awaiter has
         * registered". This eliminates the classic race where every subtask
         * completes before the @c whenAll coroutine reaches its suspend
         * point — whoever brings the counter to zero owns the resumption.
         *
         * @tparam T Element type collected from each subtask. Stored as
         *           @c std::optional<T> so @c T need not be default-
         *           constructible. Specialised for @c void below via
         *           @ref WhenAllVoidState.
         */
        template<typename T>
        struct WhenAllState {
            /**
             * @brief Per-subtask result slots.
             */
            std::vector<std::optional<T>> results;

            /**
             * @brief First exception captured (if any). All exceptions after
             *        the first are discarded; the first one is rethrown from
             *        @c await_resume.
             */
            std::exception_ptr exception{};

            /**
             * @brief Guards @ref exception against concurrent writes from
             *        multiple failing subtasks.
             */
            std::mutex ex_mu;

            /**
             * @brief Latch counter, starts at @c N+1, hits zero when both
             *        every subtask has completed AND the awaiter has
             *        registered itself.
             */
            std::atomic<size_t> remaining{0};

            /**
             * @brief Handle to resume when the latch reaches zero. Written
             *        once by the awaiter under the latch protocol; readers
             *        observe the write via the @c fetch_sub acq_rel ordering.
             */
            std::coroutine_handle<> waiter{};

            explicit WhenAllState(size_t n) : results(n), remaining(n + 1) {
            }

            /**
             * @brief Record an exception from subtask index @p idx. Only the
             *        first concurrent caller wins; subsequent ones are
             *        silently discarded.
             */
            void recordException(std::exception_ptr ex) noexcept {
                std::lock_guard<std::mutex> lk(this->ex_mu);
                if (!this->exception) this->exception = std::move(ex);
            }

            /**
             * @brief Mark one subtask complete. Returns @c true iff this was
             *        the call that brought the latch to zero; the caller
             *        must then resume @ref waiter.
             */
            bool completeOne() noexcept {
                return this->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1;
            }

            /**
             * @brief Register the awaiter's handle and consume its latch
             *        slot. Returns @c true iff every subtask had already
             *        completed by the time we registered; the caller should
             *        resume inline.
             */
            bool registerWaiter(std::coroutine_handle<> h) noexcept {
                this->waiter = h;
                return this->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1;
            }
        };

        /**
         * @brief Latch state for the @c void specialisation of @c whenAll.
         *        Same protocol as @ref WhenAllState minus the results slot.
         */
        struct WhenAllVoidState {
            std::exception_ptr exception{};
            std::mutex ex_mu;
            std::atomic<size_t> remaining{0};
            std::coroutine_handle<> waiter{};

            explicit WhenAllVoidState(size_t n) : remaining(n + 1) {
            }

            void recordException(std::exception_ptr ex) noexcept {
                std::lock_guard<std::mutex> lk(this->ex_mu);
                if (!this->exception) this->exception = std::move(ex);
            }

            bool completeOne() noexcept {
                return this->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1;
            }

            bool registerWaiter(std::coroutine_handle<> h) noexcept {
                this->waiter = h;
                return this->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1;
            }
        };

        /**
         * @brief Per-subtask wrapper for value-bearing tasks. Awaits the
         *        subtask, stores the result (or the exception), then
         *        latches @ref WhenAllState. Detached via @c coSpawn so
         *        the frame self-destroys at final suspend.
         */
        template<typename T>
        Task<void> whenAllRunner(std::shared_ptr<WhenAllState<T>> state,
                                   size_t idx,
                                   Task<T> task) {
            try {
                auto value = co_await std::move(task);
                state->results[idx].emplace(std::move(value));
            } catch (...) {
                state->recordException(std::current_exception());
            }
            if (state->completeOne()) {
                // We were the last to complete; resume the awaiter.
                if (state->waiter) state->waiter.resume();
            }
            co_return;
        }

        /**
         * @brief Per-subtask wrapper for @c Task<void>.
         */
        inline Task<void> whenAllRunnerVoid(std::shared_ptr<WhenAllVoidState> state,
                                               Task<void> task) {
            try {
                co_await std::move(task);
            } catch (...) {
                state->recordException(std::current_exception());
            }
            if (state->completeOne()) {
                if (state->waiter) state->waiter.resume();
            }
            co_return;
        }

        /**
         * @brief Awaiter returned to the @c whenAll coroutine. Registers
         *        the coroutine handle as the latch waiter; if the latch
         *        was already at 1 (all subtasks done), resumes the awaiter
         *        inline by reporting @c await_ready=false but returning
         *        from @c await_suspend in a way that yields right back.
         */
        template<typename State>
        struct WhenAllAwaiter {
            std::shared_ptr<State> state;

            bool await_ready() const noexcept {
                // Fast-path: zero-subtask whenAll. Counter starts at 1
                // (just the waiter slot). We'd suspend, decrement, see 0,
                // and resume — short-circuit that.
                return this->state->remaining.load(std::memory_order_acquire) <= 1;
            }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
                if (this->state->registerWaiter(h)) {
                    // Every subtask had already completed by the time we
                    // got here. Symmetric-transfer back to @c h so the
                    // resume happens AFTER await_suspend has returned.
                    // Calling h.resume() inline here would re-enter the
                    // very coroutine that is still suspending -- a known
                    // recursion / lifetime hazard that manifested as a
                    // SIGSEGV on macOS-arm64 under load.
                    return h;
                }
                return std::noop_coroutine();
            }

            void await_resume() const {
                if (this->state->exception) {
                    std::rethrow_exception(this->state->exception);
                }
            }
        };

    } // namespace detail


    /**
     * @brief Run @p tasks concurrently and wait until every one has
     *        completed. Returns the aggregated results in input order; if
     *        any subtask threw, rethrows the first exception observed.
     *
     * Each subtask is dispatched via @c coSpawn, so the pool is free to
     * distribute them across workers. The aggregating coroutine itself
     * suspends on a latch and resumes once the last subtask reports in.
     *
     * @tparam T Per-subtask result type (must not be @c void; see the
     *           dedicated overload below).
     */
    template<typename T>
    Task<std::vector<T>> whenAll(std::vector<Task<T>> tasks) {
        auto state = std::make_shared<detail::WhenAllState<T>>(tasks.size());

        for (size_t i = 0; i < tasks.size(); ++i) {
            coSpawn(detail::whenAllRunner<T>(state, i, std::move(tasks[i])));
        }

        co_await detail::WhenAllAwaiter<detail::WhenAllState<T>>{state};

        std::vector<T> out;
        out.reserve(state->results.size());
        for (auto &r : state->results) {
            out.emplace_back(std::move(*r));
        }
        co_return out;
    }

    /**
     * @brief @c void overload of @ref whenAll. Completes when every
     *        @c Task<void> has finished; rethrows the first exception
     *        observed, if any.
     */
    inline Task<void> whenAll(std::vector<Task<void>> tasks) {
        auto state = std::make_shared<detail::WhenAllVoidState>(tasks.size());

        for (auto &t : tasks) {
            coSpawn(detail::whenAllRunnerVoid(state, std::move(t)));
        }

        co_await detail::WhenAllAwaiter<detail::WhenAllVoidState>{state};
        co_return;
    }

}

#endif // YARN_WHEN_ALL_H
