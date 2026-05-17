//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef YARN_TIMERS_H
#define YARN_TIMERS_H

#include <chrono>
#include <coroutine>

#include "Coroutines.h"
#include "Reactor.h"

namespace YarnBall {

    /**
     * @struct SleepAwaiter
     * @brief Awaiter that suspends the calling coroutine for at least
     *        @c duration. Resumption lands on a Yarn worker via the
     *        Reactor's @c schedule path.
     *
     * Sub-millisecond accuracy depends on the underlying kernel timer:
     *  - kqueue: NOTE_NSECONDS (high resolution, but bounded by the
     *    kernel's tick granularity).
     *  - epoll + timerfd: CLOCK_MONOTONIC nanoseconds.
     *  - io_uring: IORING_OP_TIMEOUT with kernel timespec.
     *  - Stub backends: zero — resume happens immediately.
     */
    struct SleepAwaiter {
        std::chrono::nanoseconds duration;

        /**
         * @brief Skip the suspend entirely for non-positive durations.
         */
        bool await_ready() const noexcept {
            return this->duration.count() <= 0;
        }

        void await_suspend(std::coroutine_handle<> h) noexcept {
            Reactor::instance()->registerTimer(this->duration, h);
        }

        void await_resume() const noexcept {}
    };

    /**
     * @brief Sleep for at least @p duration. Usage: @c co_await sleepFor(50ms);
     */
    template<typename Rep, typename Period>
    inline SleepAwaiter sleepFor(std::chrono::duration<Rep, Period> duration) noexcept {
        return SleepAwaiter{std::chrono::duration_cast<std::chrono::nanoseconds>(duration)};
    }

    /**
     * @brief Sleep until @p deadline (monotonic clock).
     */
    template<typename Clock, typename Duration>
    inline SleepAwaiter sleepUntil(std::chrono::time_point<Clock, Duration> deadline) noexcept {
        const auto now = Clock::now();
        if (deadline <= now) return SleepAwaiter{std::chrono::nanoseconds{0}};
        return SleepAwaiter{
            std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now)};
    }

}

#endif // YARN_TIMERS_H
