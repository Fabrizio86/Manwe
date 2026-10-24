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
// The loser is not joined: withTimeout returns (or throws TimeoutException) as
// soon as the race is decided, while a cancelled task may still be unwinding.
// Whatever the task borrows by reference (a stack buffer, a caller-owned
// stream) must therefore outlive the task itself, not just the withTimeout
// call; see the lifetime rule in WhenAny.h. A value the task produces at the
// instant the timer wins is destroyed, not delivered, so withTimeout is not a
// lossless way to receive from a channel.
//

#ifndef YARN_TIMEOUT_H
#define YARN_TIMEOUT_H

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
         * @brief Index reserved for the user task in the @ref withTimeout
         *        race; the timer always lives at @ref kTimeoutTimerIndex.
         *        The whenAny result's @c index field therefore tells us
         *        which side fired without an extra channel.
         */
        inline constexpr std::size_t kTimeoutUserIndex = 0;
        inline constexpr std::size_t kTimeoutTimerIndex = 1;

        /**
         * @brief Adapter that awaits @p src and stores the result into
         *        @p dst. Defined as a free template (not an inline
         *        lambda) so the captured shared_ptr lives in the
         *        coroutine frame as a by-value parameter -- a
         *        @c [dst, src]()->Task<>{}() closure would be destroyed
         *        at end of full-expression while the coroutine is still
         *        alive on the Yarn pool.
         */
        template<typename T>
        Task<> storeResultInto(std::shared_ptr<std::optional<T>> dst,
                                     Task<T> src) {
            *dst = co_await std::move(src);
            co_return;
        }

        /**
         * @brief Sleeper task used as the timeout side of the race.
         */
        template<typename Rep, typename Period>
        Task<> timeout_sleeper(std::chrono::duration<Rep, Period> duration) {
            co_await sleepFor(duration);
            co_return;
        }

        /**
         * @brief Body of a @ref deadlineToken timer: sleep on the Reactor, then stop @p src. Ends quietly, without
         *        stopping @p src, when @p cancel is stopped first.
         */
        template<typename Rep, typename Period>
        Task<> deadlineSleeper(std::shared_ptr<std::stop_source> src, std::stop_token cancel,
                                    std::chrono::duration<Rep, Period> duration) {
            try {
                if (cancel.stop_possible()) {
                    co_await withCancellation(cancel, timeout_sleeper(duration));
                } else {
                    co_await timeout_sleeper(duration);
                }
            } catch (const CancelledException &) {
                co_return;
            }
            src->request_stop();
        }

    } // namespace detail


    /**
     * @brief Run @p task with a wall-clock cap. If @p task completes
     *        before @p duration elapses, returns its value. Otherwise
     *        throws @ref TimeoutException.
     *
     * @tparam T Result type of @p task. Move-constructible.
     *
     * Implementation: stores @p task's eventual result in a shared
     * @c optional and races the storer coroutine against a sleeper
     * via @ref whenAny. The whenAny result's @c index field tells
     * us which side won. Whichever side loses is cancelled as
     * described in @ref whenAny; in particular the sleeper is woken
     * and torn down as soon as the task finishes, instead of keeping
     * its frame alive for the whole timeout.
     *
     * @warning On timeout the task is cancelled but not joined: it may still
     *          be running when this throws. Do not hand it references to
     *          locals or caller-owned buffers/streams that die with the
     *          caller (share ownership instead), and do not rely on it to
     *          receive from a channel without loss.
     */
    template<typename T, typename Rep, typename Period>
    Task<T> withTimeout(Task<T> task,
                         std::chrono::duration<Rep, Period> duration) {
        auto slot = std::make_shared<std::optional<T>>();

        std::vector<Task<>> tasks;
        tasks.reserve(2);
        tasks.push_back(detail::storeResultInto<T>(slot, std::move(task)));
        tasks.push_back(detail::timeout_sleeper(duration));

        const std::size_t idx = co_await whenAny(std::move(tasks));
        if (idx == detail::kTimeoutTimerIndex) {
            throw TimeoutException();
        }
        if (!slot->has_value()) {
            // The user task finished by throwing; the exception was
            // already rethrown out of whenAny above, so reaching here
            // with no value is a logic-error trap.
            throw std::logic_error("withTimeout: user task won but produced no value");
        }
        co_return std::move(**slot);
    }

    /**
     * @brief @c void overload of @ref withTimeout.
     */
    template<typename Rep, typename Period>
    Task<> withTimeout(Task<> task,
                            std::chrono::duration<Rep, Period> duration) {
        std::vector<Task<>> tasks;
        tasks.reserve(2);
        tasks.push_back(std::move(task));
        tasks.push_back(detail::timeout_sleeper(duration));

        const std::size_t idx = co_await whenAny(std::move(tasks));
        if (idx == detail::kTimeoutTimerIndex) {
            throw TimeoutException();
        }
        co_return;
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
     * The deadline is a Reactor timer driving a detached coroutine, so no
     * thread is held for its duration. Pass @p cancel to withdraw it early:
     * if that token is stopped first, the timer is torn down and the returned
     * token is never signalled. (Without it the timer lives until it fires: a
     * suspended coroutine and a Reactor timer stay pinned for the whole
     * @p duration even if the returned token is dropped at once, so for
     * long durations that are usually abandoned early, pass @p cancel.)
     *
     * @note Needs a running Yarn pool and Reactor; throws whatever
     *       @c coSpawn throws if the timer cannot be started.
     */
    template<typename Rep, typename Period>
    std::stop_token deadlineToken(std::chrono::duration<Rep, Period> duration,
                                  std::stop_token cancel = {}) {
        auto src = std::make_shared<std::stop_source>();
        std::stop_token token = src->get_token();
        coSpawn(detail::deadlineSleeper(std::move(src), std::move(cancel), duration));
        return token;
    }

}

#endif // YARN_TIMEOUT_H
