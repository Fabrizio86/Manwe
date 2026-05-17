//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "AsyncSync.h"

#include "Yarn.hpp"

namespace YarnBall {

    /**
     * @struct AsyncMutexLockAwaiter
     * @brief Awaiter for @c AsyncMutex::lock(). On the fast path (mutex
     *        free), takes the lock and symmetric-transfers back to @c h.
     *        On the slow path, parks the handle on the mutex's FIFO
     *        waiters queue and suspends.
     *
     * Symmetric transfer (not inline @c h.resume()) is critical: calling
     * @c h.resume() before @c await_suspend has returned re-enters the
     * coroutine while it is still suspending, which the C++ coroutine
     * machinery does not expect and which manifested as SIGSEGV / access
     * violation on macOS-arm64 and on the Windows-2025 CI runner under
     * load (the WhenAll path had the same bug and the same fix).
     */
    struct AsyncMutexLockAwaiter {
        AsyncMutex *mutex;

        bool await_ready() noexcept { return false; }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
            {
                std::lock_guard<std::mutex> lk(this->mutex->mu);
                if (this->mutex->held) {
                    this->mutex->waiters.push_back(h);
                    return std::noop_coroutine();
                }
                this->mutex->held = true;
            }
            return h;
        }

        void await_resume() noexcept {
        }
    };

    Task<AsyncMutexGuard> AsyncMutex::lock() {
        co_await AsyncMutexLockAwaiter{this};
        co_return AsyncMutexGuard{this};
    }

    AsyncMutexGuard AsyncMutex::tryLock() noexcept {
        std::lock_guard<std::mutex> lk(this->mu);
        if (this->held) return AsyncMutexGuard{};
        this->held = true;
        return AsyncMutexGuard{this};
    }

    void AsyncMutex::release_one() noexcept {
        std::coroutine_handle<> next{};
        {
            std::lock_guard<std::mutex> lk(this->mu);
            if (this->waiters.empty()) {
                this->held = false;
                return;
            }
            next = this->waiters.front();
            this->waiters.pop_front();
            // Ownership transfers to @c next; @c held stays true.
        }
        // Dispatch through Yarn rather than inline-resuming the next
        // holder. Inline resume would (a) unbound the releaser's stack
        // under contention and (b) serialise everyone behind the
        // releaser's thread, defeating the pool. Yarn spreads the load.
        try {
            std::unique_ptr<ITask> ct{new detail::CoroutineITask(next)};
            Yarn::instance()->run(std::move(ct));
        } catch (...) {
            // Last-resort fallback so the coroutine never leaks.
            next.resume();
        }
    }


    /**
     * @struct AsyncSemAcquireAwaiter
     * @brief Awaiter for @c AsyncSemaphore::acquire(). Mirrors the
     *        mutex pattern: fast path takes a permit and
     *        symmetric-transfers back to @c h; slow path parks the
     *        handle. See @ref AsyncMutexLockAwaiter for why we use
     *        symmetric transfer instead of inline @c h.resume().
     */
    struct AsyncSemAcquireAwaiter {
        AsyncSemaphore *sem;

        bool await_ready() noexcept { return false; }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
            {
                std::lock_guard<std::mutex> lk(this->sem->mu);
                if (this->sem->count > 0) {
                    --this->sem->count;
                } else {
                    this->sem->waiters.push_back(h);
                    return std::noop_coroutine();
                }
            }
            return h;
        }

        void await_resume() noexcept {
        }
    };

    Task<void> AsyncSemaphore::acquire() {
        co_await AsyncSemAcquireAwaiter{this};
        co_return;
    }

    bool AsyncSemaphore::tryAcquire() noexcept {
        std::lock_guard<std::mutex> lk(this->mu);
        if (this->count == 0) return false;
        --this->count;
        return true;
    }

    void AsyncSemaphore::release() noexcept {
        std::coroutine_handle<> next{};
        {
            std::lock_guard<std::mutex> lk(this->mu);
            if (!this->waiters.empty()) {
                next = this->waiters.front();
                this->waiters.pop_front();
                // Permit transfers to the woken waiter; count unchanged.
            } else {
                ++this->count;
                return;
            }
        }
        try {
            std::unique_ptr<ITask> ct{new detail::CoroutineITask(next)};
            Yarn::instance()->run(std::move(ct));
        } catch (...) {
            next.resume();
        }
    }

    std::size_t AsyncSemaphore::available() const noexcept {
        std::lock_guard<std::mutex> lk(this->mu);
        return this->count;
    }


    /**
     * @struct AsyncNotifyAwaiter
     * @brief Awaiter for @c AsyncNotify::notified(). Always suspends
     *        (parking the handle on the FIFO waiters queue);
     *        @c notifyOne / @c notifyAll wakes the parked coroutine
     *        through Yarn.
     */
    struct AsyncNotifyAwaiter {
        AsyncNotify *notify;

        bool await_ready() noexcept { return false; }

        void await_suspend(std::coroutine_handle<> h) noexcept {
            std::lock_guard<std::mutex> lk(this->notify->mu);
            this->notify->waiters.push_back(h);
        }

        void await_resume() noexcept {
        }
    };

    Task<void> AsyncNotify::notified() {
        co_await AsyncNotifyAwaiter{this};
        co_return;
    }

    void AsyncNotify::notifyOne() noexcept {
        std::coroutine_handle<> next{};
        {
            std::lock_guard<std::mutex> lk(this->mu);
            if (this->waiters.empty()) return;
            next = this->waiters.front();
            this->waiters.pop_front();
        }
        try {
            std::unique_ptr<ITask> ct{new detail::CoroutineITask(next)};
            Yarn::instance()->run(std::move(ct));
        } catch (...) {
            next.resume();
        }
    }

    void AsyncNotify::notifyAll() noexcept {
        std::deque<std::coroutine_handle<>> toWake;
        {
            std::lock_guard<std::mutex> lk(this->mu);
            toWake = std::move(this->waiters);
            this->waiters.clear();
        }
        for (auto h : toWake) {
            try {
                std::unique_ptr<ITask> ct{new detail::CoroutineITask(h)};
                Yarn::instance()->run(std::move(ct));
            } catch (...) {
                h.resume();
            }
        }
    }

    std::size_t AsyncNotify::waiterCount() const noexcept {
        std::lock_guard<std::mutex> lk(this->mu);
        return this->waiters.size();
    }


    /**
     * @brief Internal helper: dispatch one resumption onto Yarn rather
     *        than inline-resume the caller. Centralises the
     *        try/catch fallback so each release-path can stay focused.
     */
    namespace {
        void dispatchToYarn(std::coroutine_handle<> h) noexcept {
            try {
                std::unique_ptr<ITask> ct{new detail::CoroutineITask(h)};
                Yarn::instance()->run(std::move(ct));
            } catch (...) {
                h.resume();
            }
        }
    }


    /**
     * @struct AsyncRwReadAwaiter
     * @brief Awaiter for @c AsyncRwLock::lockShared(). Fast path:
     *        no writer active or queued -> bump read count and
     *        skip suspend. Slow path: park as a reader waiter.
     */
    struct AsyncRwReadAwaiter {
        AsyncRwLock *lock;

        bool await_ready() noexcept { return false; }

        // Returning @c false from await_suspend tells the coroutine
        // machinery to skip the suspension entirely (the documented
        // "I changed my mind, resume me right away" path). Distinct
        // from calling @c h.resume() inline -- this is well-defined
        // and does not re-enter the coroutine mid-suspend.
        bool await_suspend(std::coroutine_handle<> h) noexcept {
            std::lock_guard<std::mutex> lk(this->lock->mu);
            if (this->lock->state >= 0 && this->lock->waiters.empty()) {
                ++this->lock->state;
                return false;
            }
            this->lock->waiters.push_back({h, /*isWriter=*/false});
            return true;
        }

        void await_resume() noexcept {
        }
    };

    /**
     * @struct AsyncRwWriteAwaiter
     * @brief Awaiter for @c AsyncRwLock::lockExclusive(). Fast path:
     *        no readers, no writer, no queue -> take exclusively and
     *        skip suspend.
     */
    struct AsyncRwWriteAwaiter {
        AsyncRwLock *lock;

        bool await_ready() noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            std::lock_guard<std::mutex> lk(this->lock->mu);
            if (this->lock->state == 0 && this->lock->waiters.empty()) {
                this->lock->state = -1;
                return false;
            }
            this->lock->waiters.push_back({h, /*isWriter=*/true});
            return true;
        }

        void await_resume() noexcept {
        }
    };

    Task<AsyncReadGuard> AsyncRwLock::lockShared() {
        co_await AsyncRwReadAwaiter{this};
        co_return AsyncReadGuard{this};
    }

    Task<AsyncWriteGuard> AsyncRwLock::lockExclusive() {
        co_await AsyncRwWriteAwaiter{this};
        co_return AsyncWriteGuard{this};
    }

    void AsyncRwLock::releaseReader() noexcept {
        std::coroutine_handle<> wake{};
        {
            std::lock_guard<std::mutex> lk(this->mu);
            --this->state;
            // Only when the last reader leaves AND a writer is at the
            // head do we promote the writer.
            if (this->state == 0 && !this->waiters.empty()
                && this->waiters.front().isWriter) {
                wake = this->waiters.front().handle;
                this->waiters.pop_front();
                this->state = -1;
            }
        }
        if (wake) dispatchToYarn(wake);
    }

    void AsyncRwLock::releaseWriter() noexcept {
        std::vector<std::coroutine_handle<>> toWake;
        {
            std::lock_guard<std::mutex> lk(this->mu);
            this->state = 0;
            if (!this->waiters.empty()) {
                if (this->waiters.front().isWriter) {
                    // Promote a single writer.
                    toWake.push_back(this->waiters.front().handle);
                    this->waiters.pop_front();
                    this->state = -1;
                } else {
                    // Drain the consecutive reader batch at the head.
                    while (!this->waiters.empty()
                           && !this->waiters.front().isWriter) {
                        toWake.push_back(this->waiters.front().handle);
                        this->waiters.pop_front();
                        ++this->state;
                    }
                }
            }
        }
        for (auto h : toWake) dispatchToYarn(h);
    }


    /**
     * @struct AsyncEventAwaiter
     * @brief Awaiter for @c AsyncEvent::wait(). Fast path: latched ->
     *        skip suspend. Slow path: park on the waiters queue.
     */
    struct AsyncEventAwaiter {
        AsyncEvent *event;

        bool await_ready() noexcept {
            return this->event->latched.load(std::memory_order_acquire);
        }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            std::lock_guard<std::mutex> lk(this->event->mu);
            // Re-check under the lock to close the race with set().
            if (this->event->latched.load(std::memory_order_acquire)) {
                return false;
            }
            this->event->waiters.push_back(h);
            return true;
        }

        void await_resume() noexcept {
        }
    };

    Task<void> AsyncEvent::wait() {
        co_await AsyncEventAwaiter{this};
        co_return;
    }

    void AsyncEvent::set() noexcept {
        std::deque<std::coroutine_handle<>> toWake;
        {
            std::lock_guard<std::mutex> lk(this->mu);
            if (this->latched.load(std::memory_order_acquire)) return;
            this->latched.store(true, std::memory_order_release);
            toWake = std::move(this->waiters);
            this->waiters.clear();
        }
        for (auto h : toWake) dispatchToYarn(h);
    }


    /**
     * @struct AsyncOnceAwaiter
     * @brief Awaiter for @c AsyncOnce::callOnce. State machine: the
     *        first caller becomes the runner; concurrent callers park
     *        on the waiters queue and resume when the runner finishes.
     */
    struct AsyncOnceAwaiter {
        AsyncOnce *once;

        /**
         * @brief @c kFirstCaller marks "I am the runner, execute fn".
         *        @c kAlreadyDone marks "fn already finished, just rethrow
         *        any captured exception". @c kPark marks "park as a
         *        waiter, fn is mid-run".
         */
        enum class Role { kFirstCaller, kAlreadyDone, kPark };

        Role role = Role::kPark;

        bool await_ready() noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            std::lock_guard<std::mutex> lk(this->once->mu);
            switch (this->once->state) {
                case AsyncOnce::State::Idle:
                    this->once->state = AsyncOnce::State::Running;
                    this->role = Role::kFirstCaller;
                    return false;
                case AsyncOnce::State::Done:
                    this->role = Role::kAlreadyDone;
                    return false;
                case AsyncOnce::State::Running:
                    this->role = Role::kPark;
                    this->once->waiters.push_back(h);
                    return true;
            }
            return true; // unreachable
        }

        void await_resume() noexcept {
        }
    };

    Task<void> AsyncOnce::callOnce(std::function<Task<void>()> fn) {
        AsyncOnceAwaiter aw{this};
        co_await aw;

        if (aw.role == AsyncOnceAwaiter::Role::kFirstCaller) {
            try {
                co_await fn();
            } catch (...) {
                this->exception = std::current_exception();
            }
            std::deque<std::coroutine_handle<>> wake;
            {
                std::lock_guard<std::mutex> lk(this->mu);
                this->state = State::Done;
                this->done.store(true, std::memory_order_release);
                wake = std::move(this->waiters);
                this->waiters.clear();
            }
            for (auto h : wake) dispatchToYarn(h);
        }

        if (this->exception) {
            std::rethrow_exception(this->exception);
        }
        co_return;
    }


    /**
     * @struct AsyncBarrierAwaiter
     * @brief Awaiter for @c AsyncBarrier::arrive(). Always decrements
     *        the countdown under the lock; if we are the @p N-th
     *        arriver, drain the waiters queue (Yarn-dispatched), reset
     *        the count, and skip our own suspension. Otherwise park.
     */
    struct AsyncBarrierAwaiter {
        AsyncBarrier *barrier;
        std::vector<std::coroutine_handle<>> toWake;

        bool await_ready() noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            std::lock_guard<std::mutex> lk(this->barrier->mu);
            --this->barrier->countdown;
            if (this->barrier->countdown == 0) {
                // We are the trigger. Capture every parked waiter to
                // wake (off-lock) and reset for the next cycle.
                this->toWake.reserve(this->barrier->waiters.size());
                for (auto w : this->barrier->waiters) this->toWake.push_back(w);
                this->barrier->waiters.clear();
                this->barrier->countdown = this->barrier->initial;
                return false; // don't suspend; the trigger continues
            }
            this->barrier->waiters.push_back(h);
            return true;
        }

        void await_resume() noexcept {
            // Off-lock: dispatch the wake-ups via Yarn.
            for (auto h : this->toWake) {
                try {
                    std::unique_ptr<ITask> ct{new detail::CoroutineITask(h)};
                    Yarn::instance()->run(std::move(ct));
                } catch (...) {
                    h.resume();
                }
            }
        }
    };

    Task<void> AsyncBarrier::arrive() {
        co_await AsyncBarrierAwaiter{this, {}};
        co_return;
    }

    std::size_t AsyncBarrier::arrived() const noexcept {
        std::lock_guard<std::mutex> lk(this->mu);
        return this->initial - this->countdown;
    }

}
