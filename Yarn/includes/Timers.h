//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef YARN_TIMERS_H
#define YARN_TIMERS_H

#include <atomic>
#include <chrono>
#include <coroutine>
#include <stop_token>

#include "Cancellation.h"
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

        SleepAwaiter(const SleepAwaiter &) = delete;
        SleepAwaiter &operator=(const SleepAwaiter &) = delete;

        explicit SleepAwaiter(std::chrono::nanoseconds d) noexcept : duration(d) {}

        ~SleepAwaiter() {
            this->bind.disarm();
            // Frame destroyed while the timer is armed: the Reactor may or may not still hold the proxy. Marking it dead
            // first makes whichever resume reaches it (ours below, or one already in flight) a no-op.
            if (this->armed && !this->resumed && this->proxy.kill()) {
                (void) Reactor::instance()->cancelTimer(this->handle);
            }
            this->proxy.drop();
        }

        /**
         * @brief Skip the suspend entirely for non-positive durations.
         */
        bool await_ready() const noexcept {
            return this->duration.count() <= 0;
        }

        /**
         * @brief Inside a cancellation scope (see Cancellation.h) a stop
         *        request, or Task::requestCancel on the chain, cancels the
         *        timer and wakes the coroutine with @c CancelledException
         *        instead of letting it sleep out the full duration.
         */
        template<typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
            detail::TaskPromiseCore *core = detail::CancelBinding::ambient(h);
            if (!core) {
                this->armed = true;
                this->handle = this->proxy.publish(h);
                Reactor::instance()->registerTimer(this->duration, this->handle);
                return true;
            }
            if (core->isCancellationRequested()) {
                this->cancelled.store(true, std::memory_order_relaxed);
                return false;
            }
            this->handle = this->proxy.publish(h);
            const auto watch = this->bind.armWatched(core, &SleepAwaiter::onCancel, this);
            if (this->cancelled.load(std::memory_order_relaxed)) {
                // Stopped while arming: nothing is registered yet, so the proxy was never visible to anyone.
                this->bind.disarm();
                this->proxy.reclaim();
                return false;
            }
            const auto proxyHandle = this->handle;
            this->armed = true;
            Reactor::instance()->registerTimer(this->duration, proxyHandle);
            // A stop that landed before the timer was registered found nothing to cancel: redo it now. Only locals from
            // here on: the timer may already have resumed (and destroyed) this awaiter.
            if (watch->load(std::memory_order_acquire)) (void) Reactor::instance()->cancelTimer(proxyHandle);
            return true;
        }

        void await_resume() {
            this->resumed = true;
            if (this->cancelled.load(std::memory_order_relaxed)) throw CancelledException{};
        }

    private:
        static void onCancel(void *p) noexcept {
            auto *self = static_cast<SleepAwaiter *>(p);
            self->cancelled.store(true, std::memory_order_relaxed);
            (void) Reactor::instance()->cancelTimer(self->handle);
        }

        std::coroutine_handle<> handle{}; // the proxy, once published
        std::atomic<bool> cancelled{false};
        bool armed = false;
        bool resumed = false;
        detail::WaitProxy proxy;
        detail::CancelBinding bind;
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
