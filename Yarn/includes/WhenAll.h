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
#include <stop_token>
#include <utility>
#include <vector>

#include "Cancellation.h"
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
             * @brief Handle (a WaitProxy's, see Cancellation.h) to resume when
             *        the latch reaches zero, or null. Taken by the runner that
             *        brings the latch to zero, or by the awaiter's destructor
             *        when its frame dies while parked.
             */
            std::atomic<void *> waiterAddr{nullptr};

            /**
             * @brief Cancellation scope handed to the subtasks when whenAll
             *        itself runs inside one (see Cancellation.h); stopped
             *        when that outer scope is. Unused (no allocation) otherwise.
             */
            std::stop_source stop{std::nostopstate};
            bool scoped = false;

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

            /// Resume the parked awaiter (if it is still there). Called by the runner that completed the latch.
            void resumeWaiter() noexcept {
                if (void *a = this->waiterAddr.exchange(nullptr, std::memory_order_acq_rel)) {
                    std::coroutine_handle<>::from_address(a).resume();
                }
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
            std::atomic<void *> waiterAddr{nullptr};
            std::stop_source stop{std::nostopstate};
            bool scoped = false;

            explicit WhenAllVoidState(size_t n) : remaining(n + 1) {
            }

            void recordException(std::exception_ptr ex) noexcept {
                std::lock_guard<std::mutex> lk(this->ex_mu);
                if (!this->exception) this->exception = std::move(ex);
            }

            bool completeOne() noexcept {
                return this->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1;
            }

            void resumeWaiter() noexcept {
                if (void *a = this->waiterAddr.exchange(nullptr, std::memory_order_acq_rel)) {
                    std::coroutine_handle<>::from_address(a).resume();
                }
            }
        };

        /**
         * @brief Per-subtask wrapper for value-bearing tasks. Awaits the
         *        subtask, stores the result (or the exception), then
         *        latches @ref WhenAllState. Detached via @c coSpawn so
         *        the frame self-destroys at final suspend.
         */
        template<typename T>
        Task<> whenAllRunner(std::shared_ptr<WhenAllState<T>> state,
                                   size_t idx,
                                   Task<T> task,
                                   trace::Context traceCtx) {
            std::stop_token scope;
            if (state->scoped) {
                scope = state->stop.get_token();
                co_await InstallScopeAwaiter{&scope};
            }
            if (!traceCtx.empty()) co_await trace::installCurrent(traceCtx);
            try {
                // The child's frame dies with this block, before the latch is released (see joinRunner).
                Task<T> work = std::move(task);
                state->results[idx].emplace(co_await std::move(work));
            } catch (...) {
                state->recordException(std::current_exception());
            }
            // The last to complete resumes the awaiter.
            if (state->completeOne()) state->resumeWaiter();
            co_return;
        }

        /**
         * @brief Per-subtask wrapper for @c Task<void>.
         */
        inline Task<> whenAllRunnerVoid(std::shared_ptr<WhenAllVoidState> state,
                                               Task<> task,
                                               trace::Context traceCtx) {
            std::stop_token scope;
            if (state->scoped) {
                scope = state->stop.get_token();
                co_await InstallScopeAwaiter{&scope};
            }
            if (!traceCtx.empty()) co_await trace::installCurrent(traceCtx);
            try {
                Task<> work = std::move(task);
                co_await std::move(work);
            } catch (...) {
                state->recordException(std::current_exception());
            }
            if (state->completeOne()) state->resumeWaiter();
            co_return;
        }

        /**
         * @brief Awaiter returned to the @c whenAll coroutine. Parks behind a
         *        WaitProxy (see Cancellation.h) so that a frame destroyed while
         *        parked is never resumed by a late runner; its destructor then
         *        also stops the subtasks' scope, if there is one.
         */
        template<typename State>
        struct WhenAllAwaiter {
            std::shared_ptr<State> state;
            WaitProxy proxy;
            bool parked = false;  // the proxy is published in state->waiterAddr
            bool resumed = false; // await_resume ran, i.e. the frame was not destroyed while parked

            explicit WhenAllAwaiter(std::shared_ptr<State> s) noexcept : state(std::move(s)) {}
            WhenAllAwaiter(const WhenAllAwaiter &) = delete;
            WhenAllAwaiter &operator=(const WhenAllAwaiter &) = delete;

            ~WhenAllAwaiter() {
                if (!this->parked) return;
                void *expected = this->proxy.handle().address();
                const bool removed = this->state->waiterAddr.compare_exchange_strong(
                    expected, nullptr, std::memory_order_acq_rel);
                this->proxy.retire(removed);
                // Destroyed while parked: nobody will look at the results, stop the subtasks that can be stopped.
                if (!this->resumed) this->state->stop.request_stop();
            }

            bool await_ready() const noexcept {
                // Fast-path: zero-subtask whenAll. Counter starts at 1
                // (just the waiter slot). We'd suspend, decrement, see 0,
                // and resume — short-circuit that.
                return this->state->remaining.load(std::memory_order_acquire) <= 1;
            }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) {
                const auto ph = this->proxy.publish(h);
                this->parked = true;
                this->state->waiterAddr.store(ph.address(), std::memory_order_release);
                // Nothing below may touch *this unless we return h: once the latch can reach zero a runner may resume
                // the proxy, and the resumed frame may destroy this awaiter.
                if (this->state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    // Every subtask had already completed, so no runner will ever take the proxy. Symmetric-transfer
                    // back to @c h so the resume happens AFTER await_suspend has returned (calling h.resume() inline
                    // would re-enter the coroutine that is still suspending: a SIGSEGV on macOS-arm64 under load).
                    this->state->waiterAddr.store(nullptr, std::memory_order_relaxed);
                    this->proxy.reclaim();
                    this->parked = false;
                    return h;
                }
                return std::noop_coroutine();
            }

            void await_resume() {
                this->resumed = true;
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
     * If the caller is inside a cancellation scope (see Cancellation.h),
     * every subtask runs in a scope that follows it, so stopping the outer
     * scope cancels their parked awaits.
     *
     * There is no fail-fast: every subtask runs to completion (or to its next
     *        cancellable await, when an outer scope stops) even after another
     *        one threw; only the first exception is reported. Destroying the
     *        awaiting frame while it is parked detaches it: the subtasks run on
     *        with their results dropped (and are stopped, if whenAll runs inside
     *        a cancellation scope).
     *
     * @tparam T Per-subtask result type (must not be @c void; see the
     *           dedicated overload below).
     */
    template<typename T>
    Task<std::vector<T>> whenAll(std::vector<Task<T>> tasks) {
        auto state = std::make_shared<detail::WhenAllState<T>>(tasks.size());
        detail::TaskPromiseCore *outer = co_await detail::CurrentScopeAwaiter{};
        detail::ScopeLink link;
        if (outer) {
            state->stop = std::stop_source();
            state->scoped = true;
            link.follow(outer, state->stop);
        }

        const trace::Context traceCtx = outer ? outer->inheritedTrace() : trace::Context{};
        size_t spawned = 0;
        try {
            for (; spawned < tasks.size(); ++spawned) {
                coSpawn(detail::whenAllRunner<T>(state, spawned, std::move(tasks[spawned]), traceCtx));
            }
        } catch (...) {
            // Spawning failed part-way: settle the latch slots of the runners that will never exist (the awaiter's own
            // slot keeps it from reaching zero here), wait for the ones that do, then report the failure.
            state->recordException(std::current_exception());
            for (size_t j = spawned; j < tasks.size(); ++j) state->completeOne();
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
    inline Task<> whenAll(std::vector<Task<>> tasks) {
        auto state = std::make_shared<detail::WhenAllVoidState>(tasks.size());
        detail::TaskPromiseCore *outer = co_await detail::CurrentScopeAwaiter{};
        detail::ScopeLink link;
        if (outer) {
            state->stop = std::stop_source();
            state->scoped = true;
            link.follow(outer, state->stop);
        }

        const trace::Context traceCtx = outer ? outer->inheritedTrace() : trace::Context{};
        size_t spawned = 0;
        try {
            for (; spawned < tasks.size(); ++spawned) {
                coSpawn(detail::whenAllRunnerVoid(state, std::move(tasks[spawned]), traceCtx));
            }
        } catch (...) {
            state->recordException(std::current_exception());
            for (size_t j = spawned; j < tasks.size(); ++j) state->completeOne();
        }

        co_await detail::WhenAllAwaiter<detail::WhenAllVoidState>{state};
        co_return;
    }

}

#endif // YARN_WHEN_ALL_H
