//
// Created by Fabrizio Paino on 2026-05-15.
//
// withTimeout: race a task against a sleep. If the task wins, returns
// its value; if the sleep wins, throws TimeoutException. The loser runs in
// a cancellation scope (see Cancellation.h) stopped when the race is
// decided: a task still parked in a cancellable await (I/O, lock, channel,
// sleep, ...) is woken with CancelledException and unwinds; work that is
// not parked finishes on its own and its result is discarded.
//
// The loser is not joined (unless withTimeout(task, d, joinLosers) is used):
// withTimeout returns (or throws TimeoutException) as soon as the race is
// decided, while a cancelled task may still be unwinding.
// Whatever the task borrows by reference (a stack buffer, a caller-owned
// stream) must therefore outlive the task itself, not just the withTimeout
// call; see the lifetime rule in WhenAny.h. A value the task produces at the
// instant the timer wins is destroyed, not delivered, so withTimeout is not a
// lossless way to receive from a channel, to take a semaphore permit (use AsyncSemaphore::acquireFor), to wait for
// a notification (AsyncNotify::notifiedFor) or to join a task.
//

#ifndef YARN_TIMEOUT_H
#define YARN_TIMEOUT_H

#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include "Cancellation.h"
#include "Coroutines.h"
#include "Timers.h"
#include "WhenAny.h"

namespace YarnBall {

    /**
     * @class TimeoutException
     * @brief Thrown by @ref withTimeout when the timer wins the race.
     *
     * On timeout the user task's cancellation scope is stopped: its
     * parked cancellable awaits wake with @c CancelledException and it
     * unwinds (see @c Cancellation.h). Work that is not parked in such
     * an await finishes on the Yarn pool and its result is discarded.
     */
    class TimeoutException : public std::runtime_error {
    public:
        TimeoutException() : std::runtime_error("operation timed out") {
        }

        explicit TimeoutException(const std::string &context)
            : std::runtime_error("operation timed out: " + context) {
        }
    };

    namespace detail {

        /**
         * @brief Heap coroutine standing in for a Reactor timer callback: resuming it ends the timeout side of a race once (see
         *        @ref TimeoutCore::expire), then the frame goes away when both references (the owner's and the resumer's) are released. The Reactor
         *        resumes a registered handle exactly once, by expiry or by @c cancelTimer, so no thread is held and no
         *        sleeper Task, runner or exception is involved.
         */
        struct TimerCb {
            struct promise_type {
                std::atomic<int> refs{2};

                TimerCb get_return_object() noexcept {
                    return TimerCb{std::coroutine_handle<promise_type>::from_promise(*this)};
                }

                std::suspend_always initial_suspend() noexcept { return {}; }

                struct Final {
                    bool await_ready() const noexcept { return false; }

                    void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                        if (h.promise().refs.fetch_sub(1, std::memory_order_acq_rel) == 1) h.destroy();
                    }

                    void await_resume() const noexcept {}
                };

                Final final_suspend() noexcept { return {}; }

                void return_void() noexcept {}

                void unhandled_exception() noexcept { std::terminate(); }
            };

            std::coroutine_handle<promise_type> h;
        };

        /**
         * @brief State shared by a @ref withTimeout caller, its runner and its timer. Has the shape
         *        @c WhenAnyAwaiter expects (finished / published / waiter / stop / exception), so the caller parks the
         *        same way a @c whenAny caller does.
         */
        struct TimeoutCore {
            std::atomic<bool> finished{false};
            bool published{false};
            std::size_t winning_index{0};
            std::exception_ptr exception{};
            std::coroutine_handle<> waiter{};
            std::mutex mu;
            std::stop_source stop;

            /// The owner's reference on the timer frame; address stays valid for @c cancelTimer until released.
            std::coroutine_handle<TimerCb::promise_type> timer{};

            /// Joined mode: a timeout is published only once the task has finished unwinding (both under @c mu).
            bool join = false;
            bool runnerDone = false;
            bool timeoutRecorded = false;

            TimeoutCore() = default;
            TimeoutCore(const TimeoutCore &) = delete;
            TimeoutCore &operator=(const TimeoutCore &) = delete;

            ~TimeoutCore() {
                if (this->timer && this->timer.promise().refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    this->timer.destroy();
                }
            }

            /// Settle the race with the timer as the winner, unless the task already won. Runs on the timer's resume.
            void expire() noexcept {
                if (this->finished.exchange(true, std::memory_order_acq_rel)) return;
                this->stop.request_stop();
                // A dead event loop fires every timer at once (see Reactor::registerTimer): that is not a timeout, and
                // sleepFor reports it as ReactorError too.
                const std::exception_ptr outcome = Reactor::instance()->isDead()
                    ? std::make_exception_ptr(ReactorError{})
                    : std::make_exception_ptr(TimeoutException());
                if (!this->join) {
                    this->exception = outcome;
                    this->publishAndWake();
                    return;
                }
                // Joined: record the timeout, but publish only when the task is gone too (whichever comes second).
                std::coroutine_handle<> w;
                {
                    std::lock_guard<std::mutex> lk(this->mu);
                    this->exception = outcome;
                    this->timeoutRecorded = true;
                    if (this->runnerDone) {
                        this->published = true;
                        w = std::exchange(this->waiter, {});
                    }
                }
                if (w) w.resume();
            }

            /// The task finished after the timer had already won (joined mode): publish if the timeout is recorded.
            void runnerFinishedLate() noexcept {
                std::coroutine_handle<> w;
                {
                    std::lock_guard<std::mutex> lk(this->mu);
                    this->runnerDone = true;
                    if (this->timeoutRecorded) {
                        this->published = true;
                        w = std::exchange(this->waiter, {});
                    }
                }
                if (w) w.resume();
            }

            /// Spawning the task failed: settle the race with @p ex (the task will never run) and take the timer back.
            void failSpawn(std::exception_ptr ex) noexcept {
                if (!this->finished.exchange(true, std::memory_order_acq_rel)) {
                    this->stop.request_stop();
                    this->exception = std::move(ex);
                    std::lock_guard<std::mutex> lk(this->mu);
                    this->published = true;
                } else if (this->join) {
                    // The timer won before the spawn failed and expire() is waiting for the runner, which will never
                    // run: report it gone so the recorded outcome is published.
                    this->runnerFinishedLate();
                }
                this->cancelTimer();
            }

            /// Mark the result published and resume the parked caller, if it is parked already.
            void publishAndWake() noexcept {
                std::coroutine_handle<> w;
                {
                    std::lock_guard<std::mutex> lk(this->mu);
                    this->published = true;
                    w = std::exchange(this->waiter, {});
                }
                if (w) w.resume();
            }

            /// Take the pending timer out of the Reactor; its frame is resumed once and finds the race decided.
            void cancelTimer() noexcept {
                if (this->timer) (void) Reactor::instance()->cancelTimer(this->timer);
            }
        };

        /**
         * @brief The frame of a timer: when resumed it calls @p fn(@p ctx) and then drops @p keep, the reference that
         *        holds @p ctx alive until the timer has run (dropped before the frame can outlive what it pins).
         */
        inline TimerCb makeTimerCb(std::shared_ptr<void> keep, void (*fn)(void *), void *ctx) {
            {
                const std::shared_ptr<void> mine = std::move(keep);
                fn(ctx);
            }
            co_return;
        }

        inline void expireTimeout(void *core) noexcept { static_cast<TimeoutCore *>(core)->expire(); }

        template<typename T>
        struct TimeoutState : TimeoutCore {
            using Value = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
            std::optional<Value> result;
        };

        /**
         * @brief Run the task of a @ref withTimeout in a cancellation scope of its own and, if it finishes first,
         *        publish its outcome and take the timer back.
         */
        template<typename T>
        Task<> timeoutRunner(std::shared_ptr<TimeoutState<T>> state, Task<T> task, trace::Context traceCtx) {
            const std::stop_token scope = state->stop.get_token();
            co_await InstallScopeAwaiter{&scope};
            if (!traceCtx.empty()) co_await trace::installCurrent(traceCtx);
            // The task runs in a block of its own so its frame is gone before the race is published.
            std::optional<typename TimeoutState<T>::Value> value;
            std::exception_ptr error;
            try {
                Task<T> work = std::move(task);
                if constexpr (std::is_void_v<T>) {
                    co_await std::move(work);
                    value.emplace();
                } else {
                    value.emplace(co_await std::move(work));
                }
            } catch (...) {
                error = std::current_exception();
            }
            const bool won = !state->finished.exchange(true, std::memory_order_acq_rel);
            if (won) {
                state->cancelTimer();
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
            // What this runner still holds dies before the caller can be resumed (a joined timeout promises the task is
            // gone, its result included, when withTimeout returns).
            value.reset();
            error = nullptr;
            if (won) {
                state->publishAndWake();
            } else if (state->join) {
                state->runnerFinishedLate();
            }
            co_return;
        }

        /**
         * @brief State of a @ref deadlineToken: the stop source the timer signals, the timer's frame, and the link to
         *        the token that may withdraw the deadline early.
         */
        struct DeadlineCore {
            std::stop_source src;
            std::atomic<bool> cancelled{false};
            std::coroutine_handle<TimerCb::promise_type> timer{};
            std::optional<std::stop_callback<CancelThunk>> withdraw;

            DeadlineCore() = default;
            DeadlineCore(const DeadlineCore &) = delete;
            DeadlineCore &operator=(const DeadlineCore &) = delete;

            ~DeadlineCore() {
                // Wait out a withdraw callback still running on another thread before the timer reference goes.
                this->withdraw.reset();
                if (this->timer && this->timer.promise().refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    this->timer.destroy();
                }
            }

            void fire() noexcept {
                // A dead event loop fires every timer at once (see Reactor::registerTimer): that is not a deadline.
                if (Reactor::instance()->isDead()) return;
                if (!this->cancelled.load(std::memory_order_seq_cst)) this->src.request_stop();
            }

            void cancelTimer() noexcept {
                if (this->timer) (void) Reactor::instance()->cancelTimer(this->timer);
            }

            static void onFire(void *p) noexcept { static_cast<DeadlineCore *>(p)->fire(); }

            static void onWithdraw(void *p) noexcept {
                auto *self = static_cast<DeadlineCore *>(p);
                self->cancelled.store(true, std::memory_order_seq_cst);
                self->cancelTimer();
            }
        };

    } // namespace detail


    namespace detail {

        template<typename T, typename Rep, typename Period>
        Task<T> withTimeoutImpl(Task<T> task, std::chrono::duration<Rep, Period> duration, bool join) {
            auto state = std::make_shared<TimeoutState<T>>();
            state->join = join;
            TaskPromiseCore *outer = co_await CurrentScopeAwaiter{};
            ScopeLink link;
            link.follow(outer, state->stop);

            const trace::Context traceCtx = outer ? outer->inheritedTrace() : trace::Context{};
            try {
                // The timer goes in first: a task that finishes at once then finds it to take back.
                auto timer = makeTimerCb(state, &expireTimeout, state.get());
                state->timer = timer.h;
                Reactor::instance()->registerTimer(detail::toNanos(duration), timer.h);
                coSpawn(timeoutRunner<T>(state, std::move(task), traceCtx));
            } catch (...) {
                state->failSpawn(std::current_exception());
            }

            co_await WhenAnyAwaiter<TimeoutCore>{state};

            if constexpr (!std::is_void_v<T>) {
                co_return std::move(*state->result);
            }
        }

    } // namespace detail

    /**
     * @brief Run @p task with a wall-clock cap. If @p task completes
     *        before @p duration elapses, returns its value. Otherwise
     *        throws @ref TimeoutException.
     *
     * @tparam T Result type of @p task. Move-constructible.
     *
     * Implementation: the task runs in a runner of its own, inside a
     * cancellation scope, while a plain Reactor timer (no sleeper coroutine,
     * no thread) counts down. Whichever finishes first settles the race: when
     * the task wins, the timer is taken back at once and nothing is thrown or
     * unwound for it; when the timer wins, the task's scope is stopped as
     * described in @ref whenAny.
     *
     * @warning On timeout the task is cancelled but not joined: it may still
     *          be running when this throws (use the @ref JoinLosers overload
     *          to wait for it). Do not hand it references to
     *          locals or caller-owned buffers/streams that die with the
     *          caller (share ownership instead), and do not rely on it to
     *          receive from a channel without loss.
     */
    template<typename T, typename Rep, typename Period>
    Task<T> withTimeout(Task<T> task,
                         std::chrono::duration<Rep, Period> duration) {
        return detail::withTimeoutImpl<T>(std::move(task), duration, false);
    }

    /**
     * @brief @ref withTimeout that does not return (or throw @ref TimeoutException) until the task has finished
     *        unwinding, so what the task borrows (a stack buffer, a caller-owned stream) only has to outlive this call.
     *        The task is still cancelled the moment the timer fires. A task that does not react to cancellation (it is
     *        computing, or parked in a foreign awaitable) therefore delays the timeout until it ends.
     */
    template<typename T, typename Rep, typename Period>
    Task<T> withTimeout(Task<T> task,
                         std::chrono::duration<Rep, Period> duration, JoinLosers) {
        return detail::withTimeoutImpl<T>(std::move(task), duration, true);
    }

    /**
     * @brief Return a @c std::stop_token that gets automatically
     *        signalled after @p duration. The underlying
     *        @c std::stop_source lives in a @c std::shared_ptr so the
     *        token stays valid for the full duration even if the
     *        caller never holds a reference to the source.
     *
     * Typical use:
     * @code
     * std::stop_token tok = deadlineToken(5s);
     * while (!tok.stop_requested()) {
     *     co_await schedule_on(Yarn::instance());
     *     // ... bounded work ...
     * }
     * @endcode
     *
     * The deadline is a plain Reactor timer, so no thread and no coroutine
     * chain is held for its duration. Pass @p cancel to withdraw it early:
     * if that token is stopped first, the timer is taken back (nothing is
     * thrown or unwound) and the returned token is never signalled. (Without
     * it the timer lives until it fires: it stays pinned for the whole
     * @p duration even if the returned token is dropped at once, so for
     * long durations that are usually abandoned early, pass @p cancel.)
     *
     * @note Needs a running Yarn pool and Reactor. Throws @c std::bad_alloc if
     *       the timer's own frame cannot be allocated. If the Reactor's loop
     *       is dead (@ref Reactor::isDead), registering the timer fires it at
     *       once and the token is left unsignalled rather than reporting a
     *       deadline that did not elapse (so on a dead loop a deadline never
     *       passes). If instead its timer table cannot grow (out of memory),
     *       that same immediate fire does signal the token early. A
     *       @p duration beyond the nanosecond range saturates.
     */
    template<typename Rep, typename Period>
    std::stop_token deadlineToken(std::chrono::duration<Rep, Period> duration,
                                  std::stop_token cancel = {}) {
        auto core = std::make_shared<detail::DeadlineCore>();
        std::stop_token token = core->src.get_token();
        auto timer = detail::makeTimerCb(core, &detail::DeadlineCore::onFire, core.get());
        core->timer = timer.h;
        if (cancel.stop_possible()) {
            core->withdraw.emplace(std::move(cancel),
                                   detail::CancelThunk{&detail::DeadlineCore::onWithdraw, core.get()});
        }
        Reactor::instance()->registerTimer(detail::toNanos(duration), timer.h);
        // A withdrawal that landed before the timer was registered found nothing to take back: redo it now.
        if (core->cancelled.load(std::memory_order_seq_cst)) core->cancelTimer();
        return token;
    }

}

#endif // YARN_TIMEOUT_H
