//
// Created by Fabrizio Paino on 2026-05-15.
//
// withTimeout: race a task against a sleep. If the task wins, returns
// its value; if the sleep wins, throws TimeoutException. The "losing"
// task keeps running on the Yarn pool to its natural completion --
// C++ coroutines have no first-class cancellation, so plumb a
// std::stop_token through if you need to actively shorten it.
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

#include "Coroutines.h"
#include "Timers.h"
#include "WhenAny.h"

namespace YarnBall {

    /**
     * @class TimeoutException
     * @brief Thrown by @ref withTimeout when the timer wins the race.
     *
     * The losing user task is NOT cancelled (C++ coroutines do not
     * support forced cancellation). It continues on the Yarn pool to
     * its natural completion; its eventual result is discarded.
     * Wire a @c std::stop_token through to the user task to actively
     * shorten its wall-clock cost.
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
        Task<void> storeResultInto(std::shared_ptr<std::optional<T>> dst,
                                     Task<T> src) {
            *dst = co_await std::move(src);
            co_return;
        }

        /**
         * @brief Sleeper task used as the timeout side of the race.
         */
        template<typename Rep, typename Period>
        Task<void> timeout_sleeper(std::chrono::duration<Rep, Period> duration) {
            co_await sleepFor(duration);
            co_return;
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
     * us which side won. The loser keeps running until its own
     * completion (C++ coroutines have no forced cancellation);
     * pass a @c stop_token if you want to actively shorten the user
     * side.
     */
    template<typename T, typename Rep, typename Period>
    Task<T> withTimeout(Task<T> task,
                         std::chrono::duration<Rep, Period> duration) {
        auto slot = std::make_shared<std::optional<T>>();

        std::vector<Task<void>> tasks;
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
    Task<void> withTimeout(Task<void> task,
                            std::chrono::duration<Rep, Period> duration) {
        std::vector<Task<void>> tasks;
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
     * @note The signal fires from a detached helper thread that
     *       @c sleep_for s the duration. This is OK for one-shot
     *       deadlines, but creating thousands of deadlineTokens per
     *       second would be wasteful -- prefer reusing a single
     *       @c std::stop_source paired with a Reactor-driven timer
     *       in that case.
     */
    template<typename Rep, typename Period>
    std::stop_token deadlineToken(std::chrono::duration<Rep, Period> duration) {
        auto src = std::make_shared<std::stop_source>();
        std::thread([src, duration]() {
            std::this_thread::sleep_for(duration);
            src->request_stop();
        }).detach();
        return src->get_token();
    }

}

#endif // YARN_TIMEOUT_H
