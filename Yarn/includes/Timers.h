//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef YARN_TIMERS_H
#define YARN_TIMERS_H

#include <atomic>
#include <chrono>
#include <coroutine>
#include <limits>
#include <memory>
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
     *  - Backends without a timer implementation (the stub): registerTimer does nothing, so the sleeper is not
     *    resumed by the Reactor.
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
        bool await_suspend(std::coroutine_handle<Promise> h) {
            detail::TaskPromiseCore *core = detail::CancelBinding::ambient(h);
            if (!core) {
                // The proxy frame is allocated here; a failure surfaces at the co_await with nothing registered.
                this->handle = this->proxy.publish(h);
                this->armed = true;
                Reactor::instance()->registerTimer(this->duration, this->handle);
                return true;
            }
            if (core->isCancellationRequested()) {
                this->cancelled.store(true, std::memory_order_relaxed);
                return false;
            }
            this->handle = this->proxy.publish(h);
            // The proxy's gate decides which of "the timer got registered" and "a stop arrived" comes second, and that
            // one (and only that one) cancels the timer. See onCancel.
            this->bind.arm(core, &SleepAwaiter::onCancel, this);
            if (this->cancelled.load(std::memory_order_relaxed)) {
                // Stopped while arming: nothing is registered yet, so the proxy was never visible to anyone.
                this->bind.disarm();
                this->proxy.reclaim();
                return false;
            }
            // Keeps the proxy (its gate, and its address) allocated after the timer is registered, when this awaiter may
            // already be gone. Taken only now: the early return above reclaims the proxy outright.
            const auto pin = this->proxy.pin();
            const auto proxyHandle = this->handle;
            this->armed = true;
            Reactor::instance()->registerTimer(this->duration, proxyHandle);
            // A stop that landed before the timer was registered found nothing to cancel: redo it now. Only locals from
            // here on: the timer may already have resumed (and destroyed) this awaiter. The redo happens only if the
            // stop really came first (then nobody has resumed the proxy, so its address is still ours). Redoing it
            // unconditionally would cancel by the address of a proxy a stop had already woken and freed, which a later
            // sleeper may have reused: that sleeper would be woken early (a 30 s withTimeout "timing out" at once).
            if (pin.gate().exchange(kRegistered, std::memory_order_acq_rel) == kStopped) {
                (void) Reactor::instance()->cancelTimer(proxyHandle);
            }
            return true;
        }

        void await_resume() {
            this->resumed = true;
            if (this->cancelled.load(std::memory_order_relaxed)) throw CancelledException{};
            // A dead event loop resumes sleepers early rather than strand them: do not report that as a finished sleep.
            if (this->armed && Reactor::instance()->isDead()) throw ReactorError{};
        }

    private:
        static void onCancel(void *p) noexcept {
            auto *self = static_cast<SleepAwaiter *>(p);
            self->cancelled.store(true, std::memory_order_relaxed);
            // Timer not registered yet: leave the cancel to await_suspend, which sees kStopped once it has registered.
            if (self->proxy.gate().exchange(kStopped, std::memory_order_acq_rel) == kRegistered) {
                (void) Reactor::instance()->cancelTimer(self->handle);
            }
        }

        static constexpr int kRegistered = 1, kStopped = 2; // gate values (idle is 0)

        std::coroutine_handle<> handle{}; // the proxy, once published
        std::atomic<bool> cancelled{false};
        bool armed = false;
        bool resumed = false;
        detail::WaitProxy proxy;
        detail::CancelBinding bind;
    };

    namespace detail {
        /**
         * @brief Convert a (possibly huge) wait to nanoseconds, saturating at the representable range instead of
         *        overflowing: @c duration_cast to a narrower integer rep is undefined for out-of-range values.
         */
        inline std::chrono::nanoseconds clampToNanos(std::chrono::duration<long double, std::nano> d) noexcept {
            if (!(d.count() > 0.0L)) return std::chrono::nanoseconds{0};
            constexpr long double kMax = static_cast<long double>(std::chrono::nanoseconds::max().count());
            if (d.count() >= kMax) return std::chrono::nanoseconds::max();
            return std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(d.count())};
        }

        /// @ref clampToNanos for any duration type (hours::max(), years(1000), a double of seconds, ...).
        template<typename Rep, typename Period>
        inline std::chrono::nanoseconds toNanos(std::chrono::duration<Rep, Period> d) noexcept {
            return clampToNanos(std::chrono::duration<long double, std::nano>(d));
        }
    }

    /**
     * @brief Sleep for at least @p duration. Usage: @c co_await sleepFor(50ms);
     *        Waits too long for nanosecond range (e.g. @c hours::max()) saturate.
     */
    template<typename Rep, typename Period>
    inline SleepAwaiter sleepFor(std::chrono::duration<Rep, Period> duration) noexcept {
        return SleepAwaiter{detail::toNanos(duration)};
    }

    /**
     * @brief Sleep until @p deadline, measured against @c Clock::now() at the call. The wait is converted to a
     *        relative duration once, so the sleep itself runs on the Reactor's monotonic timer: pass a steady clock
     *        (a @c system_clock deadline is not re-evaluated if the wall clock is adjusted while sleeping).
     */
    template<typename Clock, typename Duration>
    inline SleepAwaiter sleepUntil(std::chrono::time_point<Clock, Duration> deadline) noexcept {
        using Wide = std::chrono::duration<long double, std::nano>;
        const auto now = Clock::now();
        if (deadline <= now) return SleepAwaiter{std::chrono::nanoseconds{0}};
        // Subtract in long double: time_point::max() - now overflows the integer rep.
        return SleepAwaiter{detail::clampToNanos(
            Wide(deadline.time_since_epoch()) - Wide(now.time_since_epoch()))};
    }

}

#endif // YARN_TIMERS_H
