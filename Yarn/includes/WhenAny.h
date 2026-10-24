//
// Created by Fabrizio Paino on 2026-05-15.
//
// whenAny: race a vector of tasks; resume with the index + value of the
// first one to finish. Every branch runs inside a cancellation scope (see
// Cancellation.h) that is stopped the moment one branch wins, so losers
// parked on a timer, an fd, a lock, a channel, ... stop waiting and unwind
// with CancelledException (discarded) instead of lingering. Work that is not
// parked in a cancellable await runs to its natural end and its result is
// dropped. withTimeout is the canonical consumer; it is built the same way, with one runner for
// the task and a plain Reactor timer (not a sleepFor branch) as the other side.
//
// LOSERS ARE NOT JOINED (by default). whenAny resumes its caller as soon as the
// winner is known; a cancelled loser is only *asked* to stop and may still be
// unwinding (or, if it is not parked in a cancellable await, still running)
// after whenAny has returned. Joining was rejected as the default: a loser
// that cannot be cancelled (computation, a foreign awaitable) would then hold
// the winner hostage, and withTimeout would stop being a timeout. The
// consequence is a lifetime rule for the caller: everything a branch touches
// must outlive the branch, not just the whenAny call. Do NOT race a task that
// borrows a stack local or a caller-owned buffer/stream by reference
// (withTimeout(client.read(buf), d) with `buf` a local, then returning): after
// the timeout the cancelled read may still touch `buf`. Give such branches
// shared ownership (std::shared_ptr to the buffer or the connection), put the
// deadline inside the branch (a deadlineToken / withCancellation scope around
// the I/O) and join it yourself, or ask for the joined form:
// whenAny(tasks, joinLosers) / withTimeout(task, d, joinLosers) resume the
// caller only after every loser has finished, which makes borrowing safe at the
// price of waiting for a loser that does not react to cancellation. Likewise a
// value a losing branch produces at the instant it loses is destroyed, not
// delivered: do not use a race to receive from a channel or queue when no item
// may be lost; use the primitive's own timed form.
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
#include <stdexcept>
#include <stop_token>
#include <utility>
#include <variant>
#include <vector>

#include "Cancellation.h"
#include "Coroutines.h"

namespace YarnBall {

    namespace detail {

        /**
         * @class WhenAnyState
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

            /// Joined mode: the caller resumes only when every runner has finished; the last one to do so publishes.
            bool join = false;
            std::atomic<std::size_t> remaining{0};
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
            bool join = false;
            std::atomic<std::size_t> remaining{0};
        };

        /**
         * @brief Publish the outcome and resume the parked caller, if it is parked already.
         */
        template<typename State>
        void publishWhenAny(State &state) noexcept {
            std::coroutine_handle<> w;
            {
                std::lock_guard<std::mutex> lk(state.mu);
                state.published = true;
                w = std::exchange(state.waiter, {});
            }
            if (w) w.resume();
        }

        /**
         * @brief The end of a runner: publish at once when this runner won (default), or, in joined mode, when it is
         *        the last runner to finish. The winner's writes happen before the publish through @c remaining.
         */
        template<typename State>
        void settleRunner(State &state, bool won) noexcept {
            if (!state.join) {
                if (won) publishWhenAny(state);
            } else if (state.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                publishWhenAny(state);
            }
        }

        /**
         * @brief Per-subtask runner for value-bearing tasks. Awaits its
         *        subtask, then attempts to flip the latch; the winner
         *        publishes its (index, value) and resumes the awaiter.
         */
        template<typename T>
        Task<> whenAnyRunner(std::shared_ptr<WhenAnyState<T>> state,
                                   std::size_t idx,
                                   Task<T> task,
                                   trace::Context traceCtx) {
            const std::stop_token scope = state->stop.get_token();
            co_await InstallScopeAwaiter{&scope};
            if (!traceCtx.empty()) co_await trace::installCurrent(traceCtx);
            // Run the branch in a block of its own so its frame is destroyed before the race is published (the awaiter
            // may return and let the process exit the moment it is resumed).
            std::optional<T> value;
            std::exception_ptr error;
            try {
                Task<T> work = std::move(task);
                value.emplace(co_await std::move(work));
            } catch (...) {
                error = std::current_exception();
            }
            const bool won = !state->finished.exchange(true, std::memory_order_acq_rel);
            if (won) {
                state->stop.request_stop();
                state->winning_index = idx;
                if (error) {
                    state->exception = error;
                } else {
                    try {
                        state->result.emplace(std::move(*value));
                    } catch (...) {
                        state->exception = std::current_exception();
                    }
                }
            }
            // A loser's result (or error) dies here, before settleRunner can resume the caller: joinLosers promises
            // every branch is gone, results included, when whenAny returns.
            value.reset();
            error = nullptr;
            settleRunner(*state, won);
            co_return;
        }

        /**
         * @brief @c void runner. Same shape as the value runner; the
         *        winner publishes only the index.
         */
        inline Task<> whenAnyRunnerVoid(std::shared_ptr<WhenAnyVoidState> state,
                                                std::size_t idx,
                                                Task<> task,
                                                trace::Context traceCtx) {
            const std::stop_token scope = state->stop.get_token();
            co_await InstallScopeAwaiter{&scope};
            if (!traceCtx.empty()) co_await trace::installCurrent(traceCtx);
            std::exception_ptr error;
            try {
                Task<> work = std::move(task);
                co_await std::move(work);
            } catch (...) {
                error = std::current_exception();
            }
            const bool won = !state->finished.exchange(true, std::memory_order_acq_rel);
            if (won) {
                state->stop.request_stop();
                state->winning_index = idx;
                if (error) state->exception = error;
            }
            error = nullptr;
            settleRunner(*state, won);
            co_return;
        }

        /**
         * @brief Settle the race with @p ex when spawning a runner failed, unless a runner has already won. The awaiter
         *        has not parked yet at that point, so only the result is published. @p unspawned runners were never
         *        started; in joined mode they no longer count, and when that leaves nothing running the result is
         *        published here (otherwise the last running runner does it).
         */
        template<typename State>
        void failWhenAny(State &state, std::exception_ptr ex, std::size_t unspawned = 0) noexcept {
            if (!state.finished.exchange(true, std::memory_order_acq_rel)) {
                state.stop.request_stop();
                state.exception = std::move(ex);
                if (!state.join) {
                    std::lock_guard<std::mutex> lk(state.mu);
                    state.published = true;
                    return;
                }
            } else if (!state.join) {
                return;
            }
            if (state.remaining.fetch_sub(unspawned, std::memory_order_acq_rel) == unspawned) {
                std::lock_guard<std::mutex> lk(state.mu);
                state.published = true;
            }
        }

        /**
         * @brief Awaiter for @c whenAny. Symmetric-transfer back to the
         *        caller if a runner already won; otherwise parks behind a
         *        WaitProxy (see Cancellation.h) published under the lock. A
         *        frame destroyed while parked is never resumed by a late
         *        winner, and its destructor stops every branch.
         */
        template<typename State>
        struct WhenAnyAwaiter {
            std::shared_ptr<State> state;
            WaitProxy proxy;
            bool parked = false;  // the proxy was published in state->waiter
            bool resumed = false; // await_resume ran, i.e. the frame was not destroyed while parked

            explicit WhenAnyAwaiter(std::shared_ptr<State> s) noexcept : state(std::move(s)) {}
            WhenAnyAwaiter(const WhenAnyAwaiter &) = delete;
            WhenAnyAwaiter &operator=(const WhenAnyAwaiter &) = delete;

            ~WhenAnyAwaiter() {
                if (!this->parked) return;
                bool removed = false;
                {
                    std::lock_guard<std::mutex> lk(this->state->mu);
                    // Still there: no winner took it, so nothing is in flight toward the proxy.
                    if (this->state->waiter == this->proxy.handle()) {
                        this->state->waiter = {};
                        removed = true;
                    }
                }
                this->proxy.retire(removed);
                if (!this->resumed) this->state->stop.request_stop();
            }

            bool await_ready() const noexcept {
                std::lock_guard<std::mutex> lk(this->state->mu);
                return this->state->published;
            }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) {
                std::lock_guard<std::mutex> lk(this->state->mu);
                // Race: a runner may have flipped @c finished after
                // await_ready returned false but before we take the
                // lock. Re-check; if it has, symmetric-transfer back.
                if (this->state->published) return h;
                this->state->waiter = this->proxy.publish(h);
                this->parked = true;
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
     * @class WhenAnyResult
     * @brief Return type of value-bearing @ref whenAny: the winning
     *        subtask's index plus its result.
     */
    template<typename T>
    struct WhenAnyResult {
        std::size_t index;
        T value;
    };

    /**
     * @brief Tag selecting the joined form of @ref whenAny and @ref withTimeout: the winner is still decided at once and
     *        the losers are cancelled at once, but the caller is resumed only after every loser has finished unwinding.
     *        What a branch borrows then needs to outlive the call only, and a value a losing branch produced is
     *        destroyed before the caller resumes. The price is that a loser which cannot be cancelled (computation, a
     *        foreign awaitable) delays the caller until it ends: with @ref withTimeout the timeout then fires late.
     */
    struct JoinLosers {
        explicit JoinLosers() = default;
    };

    /// The tag value: @c whenAny(tasks, joinLosers).
    inline constexpr JoinLosers joinLosers{};

    namespace detail {

        template<typename T>
        Task<WhenAnyResult<T>> whenAnyImpl(std::vector<Task<T>> tasks, bool join) {
            if (tasks.empty()) throw std::invalid_argument("whenAny: no tasks to race");
            auto state = std::make_shared<WhenAnyState<T>>();
            state->join = join;
            state->remaining.store(tasks.size(), std::memory_order_relaxed);
            TaskPromiseCore *outer = co_await CurrentScopeAwaiter{};
            ScopeLink link;
            link.follow(outer, state->stop);

            const trace::Context traceCtx = outer ? outer->inheritedTrace() : trace::Context{};
            std::size_t spawned = 0;
            try {
                for (; spawned < tasks.size(); ++spawned) {
                    coSpawn(whenAnyRunner<T>(state, spawned, std::move(tasks[spawned]), traceCtx));
                }
            } catch (...) {
                failWhenAny(*state, std::current_exception(), tasks.size() - spawned);
            }

            co_await WhenAnyAwaiter<WhenAnyState<T>>{state};

            co_return WhenAnyResult<T>{state->winning_index, std::move(*state->result)};
        }

        inline Task<std::size_t> whenAnyVoidImpl(std::vector<Task<>> tasks, bool join) {
            if (tasks.empty()) throw std::invalid_argument("whenAny: no tasks to race");
            auto state = std::make_shared<WhenAnyVoidState>();
            state->join = join;
            state->remaining.store(tasks.size(), std::memory_order_relaxed);
            TaskPromiseCore *outer = co_await CurrentScopeAwaiter{};
            ScopeLink link;
            link.follow(outer, state->stop);

            const trace::Context traceCtx = outer ? outer->inheritedTrace() : trace::Context{};
            std::size_t spawned = 0;
            try {
                for (; spawned < tasks.size(); ++spawned) {
                    coSpawn(whenAnyRunnerVoid(state, spawned, std::move(tasks[spawned]), traceCtx));
                }
            } catch (...) {
                failWhenAny(*state, std::current_exception(), tasks.size() - spawned);
            }

            co_await WhenAnyAwaiter<WhenAnyVoidState>{state};

            co_return state->winning_index;
        }

    } // namespace detail

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
     * Destroying the awaiting frame while it is parked stops every branch
     * (their results are dropped) and a branch that finishes later does not
     * resume it.
     *
     * @throws std::invalid_argument if @p tasks is empty (no runner could ever
     *         win, so the caller would otherwise park forever).
     */
    template<typename T>
    Task<WhenAnyResult<T>> whenAny(std::vector<Task<T>> tasks) {
        return detail::whenAnyImpl<T>(std::move(tasks), false);
    }

    /// @ref whenAny that resumes the caller only after every loser has finished; see @ref JoinLosers.
    template<typename T>
    Task<WhenAnyResult<T>> whenAny(std::vector<Task<T>> tasks, JoinLosers) {
        return detail::whenAnyImpl<T>(std::move(tasks), true);
    }

    /**
     * @brief @c void overload of @ref whenAny. Returns the index of
     *        the winning subtask; rethrows its exception if it threw.
     */
    inline Task<std::size_t> whenAny(std::vector<Task<>> tasks) {
        return detail::whenAnyVoidImpl(std::move(tasks), false);
    }

    /// @c void overload of the joined form; see @ref JoinLosers.
    inline Task<std::size_t> whenAny(std::vector<Task<>> tasks, JoinLosers) {
        return detail::whenAnyVoidImpl(std::move(tasks), true);
    }

}

#endif // YARN_WHEN_ANY_H
