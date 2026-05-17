//
// Created by Fabrizio Paino on 2026-05-15.
//
// AsyncMutex and AsyncSemaphore: coroutine-aware synchronization
// primitives. A blocked acquire suspends the coroutine and parks the
// handle on an in-mutex FIFO queue; release wakes one waiter via the
// Yarn pool (NOT inline, to avoid stack-unbounded re-entry under
// contention).
//
// These do NOT replace std::mutex for synchronous code. They are for
// guarding shared state ACROSS coroutine suspension points, where a
// std::mutex would either deadlock (if held across an await and the
// same worker tried to re-acquire) or block a Yarn worker thread.
//

#ifndef YARN_ASYNC_SYNC_H
#define YARN_ASYNC_SYNC_H

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "Coroutines.h"

namespace YarnBall {

    class AsyncMutex;

    /**
     * @class AsyncMutexGuard
     * @brief RAII handle returned by @c AsyncMutex::lock(). Releases the
     *        mutex on destruction. Move-only.
     */
    class AsyncMutexGuard final {
    public:
        AsyncMutexGuard() noexcept = default;

        explicit AsyncMutexGuard(AsyncMutex *m) noexcept : owner(m) {
        }

        AsyncMutexGuard(const AsyncMutexGuard &) = delete;
        AsyncMutexGuard &operator=(const AsyncMutexGuard &) = delete;

        AsyncMutexGuard(AsyncMutexGuard &&other) noexcept
            : owner(std::exchange(other.owner, nullptr)) {
        }

        AsyncMutexGuard &operator=(AsyncMutexGuard &&other) noexcept;

        ~AsyncMutexGuard();

        /**
         * @brief @c true if this guard currently holds the mutex.
         */
        bool ownsLock() const noexcept { return this->owner != nullptr; }

        /**
         * @brief Release the mutex early (instead of waiting for the
         *        guard's destructor). Idempotent.
         */
        void unlock() noexcept;

    private:
        AsyncMutex *owner = nullptr;
    };

    /**
     * @class AsyncMutex
     * @brief Coroutine-aware mutex. @c lock() returns a @c Task that
     *        suspends until the mutex is free, then resumes on a Yarn
     *        worker with an @ref AsyncMutexGuard.
     *
     * Semantics:
     *  - Uncontended @c lock() acquires inline (no Yarn hop).
     *  - Contended @c lock() parks the coroutine handle on an internal
     *    FIFO waiters queue. @c unlock dispatches the next waiter onto
     *    Yarn (NOT inline) so a long unlock chain does not blow the
     *    releaser's stack and so the next holder always runs on a
     *    fresh worker context.
     *  - Strict FIFO ordering: no waiter starves.
     *
     * NOT recursive: a holder that @c co_await s @c lock() on the same
     * @c AsyncMutex will deadlock (its own resumption is parked behind
     * itself). Mirror @c std::mutex if you need this asserted.
     */
    class AsyncMutex final {
    public:
        AsyncMutex() = default;
        AsyncMutex(const AsyncMutex &) = delete;
        AsyncMutex &operator=(const AsyncMutex &) = delete;
        AsyncMutex(AsyncMutex &&) = delete;
        AsyncMutex &operator=(AsyncMutex &&) = delete;

        ~AsyncMutex() = default;

        /**
         * @brief Asynchronously acquire the mutex.
         * @return A @c Task that resumes with an @c AsyncMutexGuard
         *         holding the lock.
         */
        Task<AsyncMutexGuard> lock();

        /**
         * @brief Non-suspending try-acquire.
         * @return Engaged guard on success, empty guard on contention.
         */
        AsyncMutexGuard tryLock() noexcept;

    private:
        friend class AsyncMutexGuard;
        friend struct AsyncMutexLockAwaiter;

        /**
         * @brief Release the mutex and, if any waiters are queued,
         *        dispatch the head onto Yarn. Called by
         *        @ref AsyncMutexGuard destructor / @c unlock().
         */
        void release_one() noexcept;

        std::mutex mu;

        /**
         * @brief @c true if the mutex is currently held; the
         *        @c waiters queue is then the FIFO of suspended
         *        @c lock() calls.
         */
        bool held = false;

        std::deque<std::coroutine_handle<>> waiters;
    };

    inline AsyncMutexGuard &AsyncMutexGuard::operator=(AsyncMutexGuard &&other) noexcept {
        if (this != &other) {
            this->unlock();
            this->owner = std::exchange(other.owner, nullptr);
        }
        return *this;
    }

    inline AsyncMutexGuard::~AsyncMutexGuard() {
        this->unlock();
    }

    inline void AsyncMutexGuard::unlock() noexcept {
        if (this->owner) {
            this->owner->release_one();
            this->owner = nullptr;
        }
    }


    /**
     * @class AsyncSemaphore
     * @brief Counting coroutine-aware semaphore. @c acquire() returns a
     *        @c Task that decrements the count, suspending if the count
     *        is zero. @c release() bumps the count and dispatches one
     *        waiter (if any) onto Yarn.
     *
     * Built for producer/consumer-style backpressure: cap N concurrent
     * operations, @c acquire() before each, @c release() after each.
     */
    class AsyncSemaphore final {
    public:
        /**
         * @brief Construct with @p initial permits available.
         */
        explicit AsyncSemaphore(std::size_t initial) noexcept : count(initial) {
        }

        AsyncSemaphore(const AsyncSemaphore &) = delete;
        AsyncSemaphore &operator=(const AsyncSemaphore &) = delete;
        AsyncSemaphore(AsyncSemaphore &&) = delete;
        AsyncSemaphore &operator=(AsyncSemaphore &&) = delete;

        ~AsyncSemaphore() = default;

        /**
         * @brief Asynchronously take one permit; suspends if the count
         *        is zero.
         */
        Task<void> acquire();

        /**
         * @brief Non-suspending take.
         * @return @c true on success.
         */
        bool tryAcquire() noexcept;

        /**
         * @brief Return one permit. If any coroutines are blocked in
         *        @c acquire(), dispatches one onto Yarn instead of
         *        bumping the count.
         */
        void release() noexcept;

        /**
         * @brief Current available permits (advisory; for diagnostics).
         */
        std::size_t available() const noexcept;

    private:
        friend struct AsyncSemAcquireAwaiter;

        mutable std::mutex mu;
        std::size_t count;
        std::deque<std::coroutine_handle<>> waiters;
    };


    /**
     * @class AsyncNotify
     * @brief Coroutine-aware wait/notify primitive (Tokio-style).
     *        @c notified() suspends until the next @c notifyOne /
     *        @c notifyAll. FIFO across waiters; wakeups dispatch
     *        through Yarn to avoid unbounded notifier stacks.
     *
     * Useful when one coroutine needs to wait for a stand-alone event
     * (a flag flip, a buffer becoming non-empty, an external system
     * signalling). Compose with @ref AsyncMutex for textbook
     * condition-variable patterns:
     *
     * @code
     * AsyncMutex mu;
     * AsyncNotify cv;
     * bool ready = false;
     *
     * // waiter
     * auto g = co_await mu.lock();
     * while (!ready) {
     *     g.unlock();
     *     co_await cv.notified();
     *     g = co_await mu.lock();
     * }
     *
     * // notifier
     * auto g = co_await mu.lock();
     * ready = true;
     * g.unlock();
     * cv.notifyOne();
     * @endcode
     *
     * @note @c notifyOne / @c notifyAll with no parked waiters is a
     *       no-op (NOT latched); a subsequent @c notified() will park
     *       until the next notification. Use a separate @c bool /
     *       atomic flag if you need missed-notify recovery.
     */
    class AsyncNotify final {
    public:
        AsyncNotify() = default;
        AsyncNotify(const AsyncNotify &) = delete;
        AsyncNotify &operator=(const AsyncNotify &) = delete;
        AsyncNotify(AsyncNotify &&) = delete;
        AsyncNotify &operator=(AsyncNotify &&) = delete;

        ~AsyncNotify() = default;

        /**
         * @brief Suspend until @c notifyOne or @c notifyAll wakes us.
         */
        Task<void> notified();

        /**
         * @brief Wake the oldest parked waiter (FIFO). No-op if none.
         */
        void notifyOne() noexcept;

        /**
         * @brief Wake every parked waiter. No-op if none.
         */
        void notifyAll() noexcept;

        /**
         * @brief Snapshot of currently-parked waiters. Advisory; the
         *        count may have changed by the time the caller acts
         *        on it.
         */
        std::size_t waiterCount() const noexcept;

    private:
        friend struct AsyncNotifyAwaiter;

        mutable std::mutex mu;
        std::deque<std::coroutine_handle<>> waiters;
    };


    class AsyncRwLock;

    /**
     * @class AsyncReadGuard
     * @brief RAII handle returned by @c AsyncRwLock::lockShared(). Drops
     *        the shared lock on destruction. Move-only.
     */
    class AsyncReadGuard final {
    public:
        AsyncReadGuard() noexcept = default;

        explicit AsyncReadGuard(AsyncRwLock *m) noexcept : owner(m) {
        }

        AsyncReadGuard(const AsyncReadGuard &) = delete;
        AsyncReadGuard &operator=(const AsyncReadGuard &) = delete;

        AsyncReadGuard(AsyncReadGuard &&other) noexcept
            : owner(std::exchange(other.owner, nullptr)) {
        }

        AsyncReadGuard &operator=(AsyncReadGuard &&other) noexcept;
        ~AsyncReadGuard();

        bool ownsLock() const noexcept { return this->owner != nullptr; }

        /** @brief Release the shared lock early. Idempotent. */
        void unlock() noexcept;

    private:
        AsyncRwLock *owner = nullptr;
    };

    /**
     * @class AsyncWriteGuard
     * @brief RAII handle returned by @c AsyncRwLock::lockExclusive().
     *        Drops the exclusive lock on destruction. Move-only.
     */
    class AsyncWriteGuard final {
    public:
        AsyncWriteGuard() noexcept = default;

        explicit AsyncWriteGuard(AsyncRwLock *m) noexcept : owner(m) {
        }

        AsyncWriteGuard(const AsyncWriteGuard &) = delete;
        AsyncWriteGuard &operator=(const AsyncWriteGuard &) = delete;

        AsyncWriteGuard(AsyncWriteGuard &&other) noexcept
            : owner(std::exchange(other.owner, nullptr)) {
        }

        AsyncWriteGuard &operator=(AsyncWriteGuard &&other) noexcept;
        ~AsyncWriteGuard();

        bool ownsLock() const noexcept { return this->owner != nullptr; }

        /** @brief Release the exclusive lock early. Idempotent. */
        void unlock() noexcept;

    private:
        AsyncRwLock *owner = nullptr;
    };

    /**
     * @class AsyncRwLock
     * @brief Coroutine-aware reader/writer lock. Multiple readers run
     *        concurrently; a writer holds the lock alone.
     *
     * Fairness: strict FIFO across both classes. A waiting writer
     * blocks new readers from acquiring even if no writer currently
     * holds the lock — this prevents writer starvation in read-heavy
     * workloads. The trade-off is slightly lower read throughput
     * under contention; it matches @c std::shared_mutex's
     * "writer-preferring" semantics.
     *
     * Wakeups dispatch through Yarn (NOT inline-resume) to bound the
     * releaser's stack under cascades.
     *
     * Not recursive: a holder of either guard that re-acquires on
     * the same @c AsyncRwLock will deadlock.
     */
    class AsyncRwLock final {
    public:
        AsyncRwLock() = default;
        AsyncRwLock(const AsyncRwLock &) = delete;
        AsyncRwLock(AsyncRwLock &&) = delete;
        AsyncRwLock &operator=(const AsyncRwLock &) = delete;
        AsyncRwLock &operator=(AsyncRwLock &&) = delete;

        ~AsyncRwLock() = default;

        /**
         * @brief Acquire a shared (reader) lock. Suspends if a writer
         *        is active or queued ahead of us. On wake, the read
         *        count has already been bumped on our behalf.
         */
        Task<AsyncReadGuard> lockShared();

        /**
         * @brief Acquire the exclusive (writer) lock. Suspends until
         *        the lock is free AND no readers are active.
         */
        Task<AsyncWriteGuard> lockExclusive();

    private:
        friend class AsyncReadGuard;
        friend class AsyncWriteGuard;
        friend struct AsyncRwReadAwaiter;
        friend struct AsyncRwWriteAwaiter;

        /**
         * @brief Reader release path. Decrements the read count; if it
         *        hits zero AND a writer is queued at the head, wakes
         *        that writer.
         */
        void releaseReader() noexcept;

        /**
         * @brief Writer release path. Drains the FIFO -- if the next
         *        waiter is a writer, wake one writer; if readers,
         *        wake the whole consecutive reader batch.
         */
        void releaseWriter() noexcept;

        struct Waiter {
            std::coroutine_handle<> handle;
            bool isWriter;
        };

        std::mutex mu;

        /**
         * @brief Lock state. @c -1 == writer holds exclusively. @c >= 0
         *        is the active reader count. Transitions only under
         *        @c mu.
         */
        int state = 0;

        std::deque<Waiter> waiters;
    };

    // ------------- AsyncReadGuard / AsyncWriteGuard inlines ----------

    inline AsyncReadGuard &AsyncReadGuard::operator=(AsyncReadGuard &&other) noexcept {
        if (this != &other) {
            this->unlock();
            this->owner = std::exchange(other.owner, nullptr);
        }
        return *this;
    }

    inline AsyncReadGuard::~AsyncReadGuard() { this->unlock(); }

    inline void AsyncReadGuard::unlock() noexcept {
        if (this->owner) {
            this->owner->releaseReader();
            this->owner = nullptr;
        }
    }

    inline AsyncWriteGuard &AsyncWriteGuard::operator=(AsyncWriteGuard &&other) noexcept {
        if (this != &other) {
            this->unlock();
            this->owner = std::exchange(other.owner, nullptr);
        }
        return *this;
    }

    inline AsyncWriteGuard::~AsyncWriteGuard() { this->unlock(); }

    inline void AsyncWriteGuard::unlock() noexcept {
        if (this->owner) {
            this->owner->releaseWriter();
            this->owner = nullptr;
        }
    }


    /**
     * @class AsyncEvent
     * @brief Latched one-shot signal. @c set() flips a flag that stays
     *        set forever; @c wait() suspends if not yet set, else
     *        resumes immediately.
     *
     * Differs from @ref AsyncNotify: a notification IS latched. Use
     * this for "wait for initialisation / first event" patterns where
     * late waiters should still observe the signal.
     *
     * Thread-safe; idempotent set().
     */
    class AsyncEvent final {
    public:
        AsyncEvent() = default;
        AsyncEvent(const AsyncEvent &) = delete;
        AsyncEvent(AsyncEvent &&) = delete;
        AsyncEvent &operator=(const AsyncEvent &) = delete;
        AsyncEvent &operator=(AsyncEvent &&) = delete;

        ~AsyncEvent() = default;

        /**
         * @brief Suspend until the event is set. Resumes immediately
         *        (fast path, no Yarn hop) if the event is already set
         *        when this is called.
         */
        Task<void> wait();

        /**
         * @brief Flip the latch. All currently parked waiters are woken
         *        via Yarn. Subsequent @c wait() calls return without
         *        suspending. Idempotent.
         */
        void set() noexcept;

        /**
         * @return @c true if @c set() has been called.
         */
        bool isSet() const noexcept {
            return this->latched.load(std::memory_order_acquire);
        }

    private:
        friend struct AsyncEventAwaiter;

        std::atomic<bool> latched{false};
        std::mutex mu;
        std::deque<std::coroutine_handle<>> waiters;
    };


    /**
     * @class AsyncOnce
     * @brief Coroutine-aware @c std::call_once. The first @c callOnce
     *        runs its callable to completion; concurrent callers
     *        suspend until the first finishes, then return without
     *        running the callable.
     *
     * Typical use: lazy resource initialisation in a coroutine
     * context. @c std::call_once would block a Yarn worker; this
     * suspends the coroutine instead.
     *
     * If the callable throws, the @c AsyncOnce is marked failed and
     * every subsequent / concurrent @c callOnce will rethrow the
     * same exception (matching @c std::call_once's "exceptional
     * return is not consumed" semantics).
     */
    class AsyncOnce final {
    public:
        AsyncOnce() = default;
        AsyncOnce(const AsyncOnce &) = delete;
        AsyncOnce(AsyncOnce &&) = delete;
        AsyncOnce &operator=(const AsyncOnce &) = delete;
        AsyncOnce &operator=(AsyncOnce &&) = delete;

        ~AsyncOnce() = default;

        /**
         * @brief Run @p fn exactly once across all coroutines that
         *        call this AsyncOnce. The first caller runs it; all
         *        others park until the first completes, then return.
         *
         * @p fn returns a @c Task<void>; it may itself co_await.
         */
        Task<void> callOnce(std::function<Task<void>()> fn);

        /**
         * @return @c true once the inner callable has finished
         *         (successfully or with an exception).
         */
        bool isCompleted() const noexcept {
            return this->done.load(std::memory_order_acquire);
        }

    private:
        friend struct AsyncOnceAwaiter;

        enum class State {
            Idle,      // nobody has called yet
            Running,   // a coroutine is mid-run
            Done       // run completed (success or exception captured)
        };

        std::atomic<bool> done{false};
        std::mutex mu;
        State state = State::Idle;
        std::deque<std::coroutine_handle<>> waiters;
        std::exception_ptr exception{};
    };


    /**
     * @class AsyncBarrier
     * @brief Cyclic barrier for @c N coroutines. Each @c arrive()
     *        decrements the active count and suspends; when the
     *        @c N-th caller arrives, all parked waiters are woken
     *        and the count resets to @c N for the next cycle.
     *
     * Matches @c std::barrier semantics (sans the optional completion
     * callable). Useful for "all phases of a parallel computation
     * must complete before any can begin the next phase" patterns.
     *
     * Wakeups dispatch through Yarn (NOT inline-resume) so the
     * @c N-th arriver's stack stays bounded even when @c N is large.
     */
    class AsyncBarrier final {
    public:
        /**
         * @brief Construct expecting @p n arrivals per cycle. @p n
         *        must be at least 1 (a 0-count barrier would
         *        immediately complete and is rejected here to fail
         *        loud rather than silently passing through).
         */
        explicit AsyncBarrier(std::size_t n) noexcept
            : initial(n), countdown(n) {
        }

        AsyncBarrier(const AsyncBarrier &) = delete;
        AsyncBarrier(AsyncBarrier &&) = delete;
        AsyncBarrier &operator=(const AsyncBarrier &) = delete;
        AsyncBarrier &operator=(AsyncBarrier &&) = delete;

        ~AsyncBarrier() = default;

        /**
         * @brief Arrive at the barrier. Decrements the active count
         *        and suspends until @p n arrivals have happened (where
         *        @p n is the constructor argument). Once all @p n
         *        have arrived, every parked waiter resumes through
         *        Yarn and the barrier resets for the next cycle.
         */
        Task<void> arrive();

        /**
         * @brief @c initial - countdown. Snapshot of how many callers
         *        have arrived in the current cycle. Advisory.
         */
        std::size_t arrived() const noexcept;

    private:
        friend struct AsyncBarrierAwaiter;

        const std::size_t initial;
        mutable std::mutex mu;
        std::size_t countdown;
        std::deque<std::coroutine_handle<>> waiters;
    };

}

#endif // YARN_ASYNC_SYNC_H
