//
// Created by Fabrizio Paino on 2026-05-15.
//
// whenAny: race a vector of tasks; resume with the index + value of the
// first one to finish. Every branch runs inside a cancellation scope (see
// Cancellation.h) that is stopped the moment one branch wins, so losers
// parked on a timer, an fd, a lock, a channel, ... stop waiting and unwind
// with CancelledException (discarded) instead of lingering. Work that is not
// parked in a cancellable await runs to its natural end and its result is
// dropped. withTimeout is the canonical consumer: whenAny(task, sleepFor(d)).
//

#ifndef YARN_WHEN_ANY_H
#define YARN_WHEN_ANY_H

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <variant>
#include <vector>

#include "Cancellation.h"
#include "Coroutines.h"

namespace YarnBall {

    namespace detail {

        /**
         * @struct WhenAnyState
         * @brief Shared state between @c whenAny and its per-task runners.
         *        First runner to win the @c finished flag stores its
         *        (index, value-or-exception) and resumes the awaiter; all
         *        subsequent winners drop their result on the floor.
         *
         * @tparam T Per-subtask result type. Stored as @c std::optional<T>
         *           so @c T need not be default-constructible.
         */
        template<typename T>
        struct WhenAnyState {
            /// Set exactly once by whichever runner wins.
            std::atomic<bool> finished{false};

            /// Set (under @c mu) only after index/result/exception are written; the awaiter keys off this, not @c finished.
            bool published{false};

            /// Index of the winning subtask. Only meaningful once
            /// @c finished is observed true.
            std::size_t winning_index{0};

            /// Value or exception from the winning subtask.
            std::optional<T> result;
            std::exception_ptr exception{};

            /// Awaiter handle, set under the latch protocol.
            std::coroutine_handle<> waiter{};

            /// Guards @c waiter publication against the runner that wins
            /// before the awaiter has registered.
            std::mutex mu;

            /// Stopped by the winning runner: the cancellation scope of every branch.
            std::stop_source stop;
        };

        /**
         * @brief @c void specialisation of @ref WhenAnyState. Same shape
         *        minus the value-bearing optional.
         */
        struct WhenAnyVoidState {
            std::atomic<bool> finished{false};
            bool published{false};
            std::size_t winning_index{0};
            std::exception_ptr exception{};
            std::coroutine_handle<> waiter{};
            std::mutex mu;
            std::stop_source stop;
        };

        /**
         * @brief Per-subtask runner for value-bearing tasks. Awaits its
         *        subtask, then attempts to flip the latch; the winner
         *        publishes its (index, value) and resumes the awaiter.
         */
        template<typename T>
        Task<> whenAnyRunner(std::shared_ptr<WhenAnyState<T>> state,
                                   std::size_t idx,
                                   Task<T> task) {
            const std::stop_token scope = state->stop.get_token();
            co_await InstallScopeAwaiter{&scope};
            try {
                auto value = co_await std::move(task);
                if (!state->finished.exchange(true, std::memory_order_acq_rel)) {
                    state->stop.request_stop();
                    state->winning_index = idx;
                    try {
                        state->result.emplace(std::move(value));
                    } catch (...) {
                        state->exception = std::current_exception();
                    }
                    std::coroutine_handle<> w;
                    {
                        std::lock_guard<std::mutex> lk(state->mu);
                        state->published = true;
                        w = state->waiter;
                    }
                    if (w) w.resume();
                }
            } catch (...) {
                if (!state->finished.exchange(true, std::memory_order_acq_rel)) {
                    state->stop.request_stop();
                    state->winning_index = idx;
                    state->exception = std::current_exception();
                    std::coroutine_handle<> w;
                    {
                        std::lock_guard<std::mutex> lk(state->mu);
                        state->published = true;
                        w = state->waiter;
                    }
                    if (w) w.resume();
                }
            }
            co_return;
        }

        /**
         * @brief @c void runner. Same shape as the value runner; the
         *        winner publishes only the index.
         */
        inline Task<> whenAnyRunnerVoid(std::shared_ptr<WhenAnyVoidState> state,
                                                std::size_t idx,
                                                Task<> task) {
            const std::stop_token scope = state->stop.get_token();
            co_await InstallScopeAwaiter{&scope};
            try {
                co_await std::move(task);
                if (!state->finished.exchange(true, std::memory_order_acq_rel)) {
                    state->stop.request_stop();
                    state->winning_index = idx;
                    std::coroutine_handle<> w;
                    {
                        std::lock_guard<std::mutex> lk(state->mu);
                        state->published = true;
                        w = state->waiter;
                    }
                    if (w) w.resume();
                }
            } catch (...) {
                if (!state->finished.exchange(true, std::memory_order_acq_rel)) {
                    state->stop.request_stop();
                    state->winning_index = idx;
                    state->exception = std::current_exception();
                    std::coroutine_handle<> w;
                    {
                        std::lock_guard<std::mutex> lk(state->mu);
                        state->published = true;
                        w = state->waiter;
                    }
                    if (w) w.resume();
                }
            }
            co_return;
        }

        /**
         * @brief Awaiter for @c whenAny. Symmetric-transfer back to the
         *        caller if a runner already won; otherwise publishes the
         *        waiter handle under the lock and suspends.
         */
        template<typename State>
        struct WhenAnyAwaiter {
            std::shared_ptr<State> state;

            bool await_ready() const noexcept {
                std::lock_guard<std::mutex> lk(this->state->mu);
                return this->state->published;
            }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
                {
                    std::lock_guard<std::mutex> lk(this->state->mu);
                    // Race: a runner may have flipped @c finished after
                    // await_ready returned false but before we take the
                    // lock. Re-check; if it has, symmetric-transfer back.
                    if (this->state->published) {
                        return h;
                    }
                    this->state->waiter = h;
                }
                return std::noop_coroutine();
            }

            void await_resume() {
                if (this->state->exception) {
                    std::rethrow_exception(this->state->exception);
                }
            }
        };

    } // namespace detail


    /**
     * @struct WhenAnyResult
     * @brief Return type of value-bearing @ref whenAny: the winning
     *        subtask's index plus its result.
     */
    template<typename T>
    struct WhenAnyResult {
        std::size_t index;
        T value;
    };

    /**
     * @brief Race @p tasks concurrently; resume with the index + value
     *        of the first one to finish. The losers' cancellation scope is
     *        stopped as soon as the winner is known: their parked
     *        cancellable awaits wake with @c CancelledException (swallowed
     *        here) and they unwind; anything not parked runs to its end
     *        with the result discarded. If the caller is itself inside a
     *        cancellation scope, stopping that scope stops every branch
     *        and whenAny then throws @c CancelledException.
     *
     * @throws Rethrows the exception of the winning subtask if it
     *         finished by throwing.
     *
     * @note An empty input vector parks the caller forever (no runner
     *       will ever flip the latch). This matches the @c whenAll
     *       contract of mirroring the parallelism cardinality of the
     *       input. Callers should check @c tasks.empty() upstream.
     */
    template<typename T>
    Task<WhenAnyResult<T>> whenAny(std::vector<Task<T>> tasks) {
        auto state = std::make_shared<detail::WhenAnyState<T>>();
        detail::TaskPromiseCore *outer = co_await detail::CurrentScopeAwaiter{};
        detail::ScopeLink link;
        link.follow(outer, state->stop);

        for (std::size_t i = 0; i < tasks.size(); ++i) {
            coSpawn(detail::whenAnyRunner<T>(state, i, std::move(tasks[i])));
        }

        co_await detail::WhenAnyAwaiter<detail::WhenAnyState<T>>{state};

        co_return WhenAnyResult<T>{state->winning_index,
                                   std::move(*state->result)};
    }

    /**
     * @brief @c void overload of @ref whenAny. Returns the index of
     *        the winning subtask; rethrows its exception if it threw.
     */
    inline Task<std::size_t> whenAny(std::vector<Task<>> tasks) {
        auto state = std::make_shared<detail::WhenAnyVoidState>();
        detail::TaskPromiseCore *outer = co_await detail::CurrentScopeAwaiter{};
        detail::ScopeLink link;
        link.follow(outer, state->stop);

        for (std::size_t i = 0; i < tasks.size(); ++i) {
            coSpawn(detail::whenAnyRunnerVoid(state, i, std::move(tasks[i])));
        }

        co_await detail::WhenAnyAwaiter<detail::WhenAnyVoidState>{state};

        co_return state->winning_index;
    }

}

#endif // YARN_WHEN_ANY_H
