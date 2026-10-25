//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "AsyncSync.h"

#include "Yarn.hpp"
#include "Timeout.h"

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
    struct AsyncMutexLockAwaiter : detail::ParkBase<AsyncMutexLockAwaiter> {
        AsyncMutex *mutex;

        explicit AsyncMutexLockAwaiter(AsyncMutex *m) noexcept
            : ParkBase(m->mu, m->waiters), mutex(m) {
        }

        ~AsyncMutexLockAwaiter() { this->abandon(); }

        // Ownership was handed over by a release, but the frame died before it could use it.
        void revokeGrant() noexcept { this->mutex->release_one(); }

        bool await_ready() noexcept { return false; }

        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) {
            return this->park(h, [this] {
                if (this->mutex->held) return false;
                this->mutex->held = true;
                return true;
            });
        }

        void await_resume() { this->throwIfCancelled(); }
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
            next = this->waiters.popFront();
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
    struct AsyncSemAcquireAwaiter : detail::ParkBase<AsyncSemAcquireAwaiter> {
        AsyncSemaphore *sem;

        explicit AsyncSemAcquireAwaiter(AsyncSemaphore *s) noexcept
            : ParkBase(s->mu, s->waiters), sem(s) {
        }

        ~AsyncSemAcquireAwaiter() { this->abandon(); }

        void revokeGrant() noexcept { this->sem->release(); }

        bool await_ready() noexcept { return false; }

        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) {
            return this->park(h, [this] {
                if (this->sem->count == 0) return false;
                --this->sem->count;
                return true;
            });
        }

        void await_resume() { this->throwIfCancelled(); }
    };

    Task<> AsyncSemaphore::acquire() {
        co_await AsyncSemAcquireAwaiter{this};
        co_return;
    }

    namespace {
        /**
         * @brief Wait for @p wait under a deadline. The deadline only stops the wait's own scope, so the park is
         *        either withdrawn under the primitive's lock or already granted and then reported as success.
         */
        Task<bool> timedWait(Task<> wait, std::chrono::nanoseconds timeout) {
            detail::TaskPromiseCore *outer = co_await detail::CurrentScopeAwaiter{};
            std::stop_source finished;
            struct Withdraw {
                std::stop_source *s;
                ~Withdraw() { this->s->request_stop(); }
            } withdraw{&finished};
            const std::stop_token deadline = deadlineToken(timeout, finished.get_token());
            try {
                co_await withCancellation(deadline, std::move(wait));
            } catch (const CancelledException &) {
                if (outer && outer->isCancellationRequested()) throw;
                co_return false;
            }
            co_return true;
        }
    }

    Task<bool> AsyncSemaphore::acquireFor(std::chrono::nanoseconds timeout) {
        return timedWait(this->acquire(), timeout);
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
                next = this->waiters.popFront();
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
    struct AsyncNotifyAwaiter : detail::ParkBase<AsyncNotifyAwaiter> {
        AsyncNotify *notify;

        explicit AsyncNotifyAwaiter(AsyncNotify *n) noexcept
            : ParkBase(n->mu, n->waiters), notify(n) {
        }

        ~AsyncNotifyAwaiter() { this->abandon(); }

        // Woken but destroyed before it ran: the notification would be swallowed while other waiters stay parked, so
        // pass it on. After a notifyAll this wakes one extra waiter, which the wait/notify pattern (re-check the
        // condition after every wake) tolerates.
        void revokeGrant() noexcept { this->notify->notifyOne(); }

        bool await_ready() noexcept { return false; }

        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) {
            return this->park(h, [] { return false; });
        }

        void await_resume() { this->throwIfCancelled(); }
    };

    Task<> AsyncNotify::notified() {
        co_await AsyncNotifyAwaiter{this};
        co_return;
    }

    Task<bool> AsyncNotify::notifiedFor(std::chrono::nanoseconds timeout) {
        return timedWait(this->notified(), timeout);
    }

    void AsyncNotify::notifyOne() noexcept {
        std::coroutine_handle<> next{};
        {
            std::lock_guard<std::mutex> lk(this->mu);
            if (this->waiters.empty()) return;
            next = this->waiters.popFront();
        }
        try {
            std::unique_ptr<ITask> ct{new detail::CoroutineITask(next)};
            Yarn::instance()->run(std::move(ct));
        } catch (...) {
            next.resume();
        }
    }

    void AsyncNotify::notifyAll() noexcept {
        std::vector<std::coroutine_handle<>> toWake;
        {
            std::lock_guard<std::mutex> lk(this->mu);
            this->waiters.takeAll(toWake);
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
    struct AsyncRwReadAwaiter : detail::ParkBase<AsyncRwReadAwaiter> {
        AsyncRwLock *lock;

        explicit AsyncRwReadAwaiter(AsyncRwLock *l) noexcept
            : ParkBase(l->mu, l->waiters), lock(l) {
        }

        ~AsyncRwReadAwaiter() { this->abandon(); }

        void revokeGrant() noexcept { this->lock->releaseReader(); }

        // A waiter leaving the queue can unblock the ones behind it.
        void fixupLocked(std::vector<std::coroutine_handle<>> &wake) noexcept {
            this->lock->promoteLocked(wake);
        }

        bool await_ready() noexcept { return false; }

        // Taking the fast path means resuming straight away (the symmetric transfer back to @c h); no re-entry happens
        // before await_suspend has returned.
        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) {
            return this->park(h, [this] {
                if (this->lock->state < 0 || !this->lock->waiters.empty()) return false;
                ++this->lock->state;
                return true;
            });
        }

        void await_resume() { this->throwIfCancelled(); }
    };

    /**
     * @struct AsyncRwWriteAwaiter
     * @brief Awaiter for @c AsyncRwLock::lockExclusive(). Fast path:
     *        no readers, no writer, no queue -> take exclusively and
     *        skip suspend.
     */
    struct AsyncRwWriteAwaiter : detail::ParkBase<AsyncRwWriteAwaiter> {
        AsyncRwLock *lock;

        explicit AsyncRwWriteAwaiter(AsyncRwLock *l) noexcept
            : ParkBase(l->mu, l->waiters), lock(l) {
            this->node.isWriter = true;
        }

        ~AsyncRwWriteAwaiter() { this->abandon(); }

        void revokeGrant() noexcept { this->lock->releaseWriter(); }

        void fixupLocked(std::vector<std::coroutine_handle<>> &wake) noexcept {
            this->lock->promoteLocked(wake);
        }

        bool await_ready() noexcept { return false; }

        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) {
            return this->park(h, [this] {
                if (this->lock->state != 0 || !this->lock->waiters.empty()) return false;
                this->lock->state = -1;
                return true;
            });
        }

        void await_resume() { this->throwIfCancelled(); }
    };

    Task<AsyncReadGuard> AsyncRwLock::lockShared() {
        co_await AsyncRwReadAwaiter{this};
        co_return AsyncReadGuard{this};
    }

    Task<AsyncWriteGuard> AsyncRwLock::lockExclusive() {
        co_await AsyncRwWriteAwaiter{this};
        co_return AsyncWriteGuard{this};
    }

    void AsyncRwLock::promoteLocked(std::vector<std::coroutine_handle<>> &wake) {
        if (this->waiters.empty()) return;
        if (this->waiters.front()->isWriter) {
            if (this->state == 0) {
                this->state = -1;
                wake.push_back(this->waiters.popFront());
            }
        } else if (this->state >= 0) {
            // Drain the consecutive reader batch at the head.
            while (!this->waiters.empty() && !this->waiters.front()->isWriter) {
                ++this->state;
                wake.push_back(this->waiters.popFront());
            }
        }
    }

    void AsyncRwLock::releaseReader() noexcept {
        std::vector<std::coroutine_handle<>> toWake;
        {
            std::lock_guard<std::mutex> lk(this->mu);
            --this->state;
            // Only when the last reader leaves can a writer at the head run.
            if (this->state == 0) this->promoteLocked(toWake);
        }
        for (auto h : toWake) dispatchToYarn(h);
    }

    void AsyncRwLock::releaseWriter() noexcept {
        std::vector<std::coroutine_handle<>> toWake;
        {
            std::lock_guard<std::mutex> lk(this->mu);
            this->state = 0;
            this->promoteLocked(toWake);
        }
        for (auto h : toWake) dispatchToYarn(h);
    }


    /**
     * @struct AsyncEventAwaiter
     * @brief Awaiter for @c AsyncEvent::wait(). Fast path: latched ->
     *        skip suspend. Slow path: park on the waiters queue.
     */
    struct AsyncEventAwaiter : detail::ParkBase<AsyncEventAwaiter> {
        AsyncEvent *event;

        explicit AsyncEventAwaiter(AsyncEvent *e) noexcept
            : ParkBase(e->mu, e->waiters), event(e) {
        }

        ~AsyncEventAwaiter() { this->abandon(); }

        bool await_ready() noexcept {
            return this->event->latched.load(std::memory_order_acquire);
        }

        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) {
            // The latch is re-checked under the lock to close the race with set().
            return this->park(h, [this] {
                return this->event->latched.load(std::memory_order_acquire);
            });
        }

        void await_resume() { this->throwIfCancelled(); }
    };

    Task<> AsyncEvent::wait() {
        co_await AsyncEventAwaiter{this};
        co_return;
    }

    void AsyncEvent::set() noexcept {
        std::vector<std::coroutine_handle<>> toWake;
        {
            std::lock_guard<std::mutex> lk(this->mu);
            if (this->latched.load(std::memory_order_acquire)) return;
            this->latched.store(true, std::memory_order_release);
            this->waiters.takeAll(toWake);
        }
        for (auto h : toWake) dispatchToYarn(h);
    }


    /**
     * @struct AsyncOnceAwaiter
     * @brief Awaiter for @c AsyncOnce::callOnce. State machine: the
     *        first caller becomes the runner; concurrent callers park
     *        on the waiters queue and resume when the runner finishes.
     */
    struct AsyncOnceAwaiter : detail::ParkBase<AsyncOnceAwaiter> {
        AsyncOnce *once;

        /**
         * @brief @c kFirstCaller marks "I am the runner, execute fn".
         *        @c kAlreadyDone marks "fn already finished, just rethrow
         *        any captured exception". @c kPark marks "park as a
         *        waiter, fn is mid-run".
         */
        enum class Role { kFirstCaller, kAlreadyDone, kPark };

        Role role = Role::kPark;

        explicit AsyncOnceAwaiter(AsyncOnce *o) noexcept
            : ParkBase(o->mu, o->waiters), once(o) {
        }

        ~AsyncOnceAwaiter() { this->abandon(); }

        bool await_ready() noexcept { return false; }

        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) {
            return this->park(h, [this] {
                switch (this->once->state) {
                    case AsyncOnce::State::Idle:
                        this->once->state = AsyncOnce::State::Running;
                        this->role = Role::kFirstCaller;
                        return true;
                    case AsyncOnce::State::Done:
                        this->role = Role::kAlreadyDone;
                        return true;
                    case AsyncOnce::State::Running:
                        break;
                }
                this->role = Role::kPark;
                return false;
            });
        }

        void await_resume() { this->throwIfCancelled(); }
    };

    Task<> AsyncOnce::callOnce(std::function<Task<>()> fn) {
        while (true) {
            AsyncOnceAwaiter aw{this};
            co_await aw;

            if (aw.role == AsyncOnceAwaiter::Role::kPark) {
                // Woken because the runner finished (or was cancelled): look again.
                continue;
            }

            if (aw.role == AsyncOnceAwaiter::Role::kFirstCaller) {
                // The runner's frame can be destroyed while it is suspended inside fn() (its owner dropped it, or a
                // scope tore it down). Without this the state would stay Running and every waiter parked forever:
                // hand the run back instead, exactly as for a cancelled runner.
                struct RunnerGuard {
                    AsyncOnce *once;
                    bool armed = true;

                    ~RunnerGuard() {
                        if (!this->armed) return;
                        std::vector<std::coroutine_handle<>> wake;
                        {
                            std::lock_guard<std::mutex> lk(this->once->mu);
                            this->once->state = State::Idle;
                            this->once->waiters.takeAll(wake);
                        }
                        for (auto h : wake) dispatchToYarn(h);
                    }
                } guard{this};
                bool runnerCancelled = false;
                try {
                    co_await fn();
                } catch (const CancelledException &) {
                    runnerCancelled = true;
                } catch (...) {
                    this->exception = std::current_exception();
                }
                guard.armed = false;
                std::vector<std::coroutine_handle<>> wake;
                {
                    std::lock_guard<std::mutex> lk(this->mu);
                    if (runnerCancelled) {
                        // Not completed: let the next caller start over.
                        this->state = State::Idle;
                    } else {
                        this->state = State::Done;
                        this->done.store(true, std::memory_order_release);
                    }
                    this->waiters.takeAll(wake);
                }
                for (auto h : wake) dispatchToYarn(h);
                if (runnerCancelled) throw CancelledException{};
            }
            break;
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
    struct AsyncBarrierAwaiter : detail::ParkBase<AsyncBarrierAwaiter> {
        AsyncBarrier *barrier;

        explicit AsyncBarrierAwaiter(AsyncBarrier *b) noexcept
            : ParkBase(b->mu, b->waiters), barrier(b) {
        }

        ~AsyncBarrierAwaiter() { this->abandon(); }

        // A waiter that leaves before the barrier trips withdraws its arrival.
        void fixupLocked(std::vector<std::coroutine_handle<>> &) noexcept {
            ++this->barrier->countdown;
        }

        bool await_ready() noexcept { return false; }

        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) {
            // Arrival and enqueue must be one atomic step under the lock, so the callback is armed first.
            if (detail::TaskPromiseCore *core = detail::CancelBinding::ambient(h)) {
                if (core->isCancellationRequested()) {
                    this->cancelled = true;
                    return h;
                }
                this->bind.arm(core, &AsyncBarrierAwaiter::cancelThunk, this);
            }
            std::vector<std::coroutine_handle<>> toWake;
            {
                std::lock_guard<std::mutex> lk(this->barrier->mu);
                if (this->earlyCancel) {
                    this->cancelled = true;
                    return h;
                }
                --this->barrier->countdown;
                if (this->barrier->countdown != 0) {
                    this->node.h = this->proxy.publish(h);
                    this->barrier->waiters.push_back(&this->node);
                    return std::noop_coroutine();
                }
                // We are the trigger: take every parked waiter and reset for the next cycle.
                this->barrier->waiters.takeAll(toWake);
                this->barrier->countdown = this->barrier->initial;
            }
            // Off-lock, before our own continuation runs: the wake-ups must not depend on this frame staying alive.
            for (auto w : toWake) dispatchToYarn(w);
            return h; // don't suspend; the trigger continues
        }

        void await_resume() { this->throwIfCancelled(); }
    };

    Task<> AsyncBarrier::arrive() {
        co_await AsyncBarrierAwaiter{this};
        co_return;
    }

    std::size_t AsyncBarrier::arrived() const noexcept {
        std::lock_guard<std::mutex> lk(this->mu);
        return this->initial - this->countdown;
    }

}
