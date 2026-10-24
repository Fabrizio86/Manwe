//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef WIRE_SIGNAL_H
#define WIRE_SIGNAL_H

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <memory>
#include <exception>
#include <mutex>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "Cancellation.h"
#include "Coroutines.h"

namespace Telegraph {

    /**
     * @class Signal
     * @brief Typed multicast notification. Per-instance (not a singleton).
     *        Thread-safe connect / disconnect / emit. Coroutine-aware via
     *        @ref next, which returns an awaiter that resumes with the
     *        next emission's argument tuple.
     *
     * Three emission modes:
     *  - @c emit: handlers run synchronously on the calling thread, in
     *    registration order.
     *  - @c emitAsync: each handler is dispatched to the Yarn pool as
     *    a separate task; the call returns immediately.
     *  - One-shot coroutine listeners registered via @c next() are
     *    resumed on the pool (via Yarn::run) so the emit caller does
     *    not own those resumptions' execution time.
     *
     * Args must be copy-constructible, since every handler / waiter
     * receives an independent copy.
     *
     * Lifetime and disconnect. Every emission works from a snapshot of
     * the handler list, so @ref disconnect does NOT wait for, or stop, a
     * call that is already running or already dispatched by @c emitAsync:
     * the handler can still run once after @c disconnect returns. A
     * handler that captures an object by pointer or reference must
     * therefore not rely on @c disconnect alone to make that object safe
     * to destroy; hold the captured state in a @c shared_ptr, or have the
     * handler check a flag. @ref connectScoped gives an RAII handle that
     * disconnects on destruction (with the same caveat).
     *
     * A removed handler is destroyed after the Signal's lock is released,
     * so its captures (a @ref ScopedConnection, say) may call back into the
     * Signal from their destructors. @ref connect and @ref disconnect edit
     * the handler list in place unless an emission is running, in which
     * case they copy it: registering many handlers stays cheap.
     *
     * Handler exceptions: @c emit runs every handler and rethrows the
     * first exception after the last; @c emitAsync reports an exception
     * through @c YarnBall::setUnhandledExceptionHandler.
     *
     * Destroying the Signal wakes every parked @c next() waiter with
     * @c YarnBall::CancelledException, and a @c next() awaited after that
     * throws it at once. The caller must still make sure no
     * @c emit / @c emitAsync call is running on it.
     */
    template<typename... Args>
    class Signal {
    public:
        using Handler = std::function<void(Args...)>;
        using SlotId = std::uint64_t;

        /**
         * @brief Awaiter helper. Stored as @c shared_ptr so the awaiter, the
         *        Signal's pending list, and any in-flight emission share
         *        ownership of the result slot.
         */
        struct NextSlot {
            std::coroutine_handle<> handle{};
            std::optional<std::tuple<Args...>> value;
            std::atomic<bool> queued{false};
            bool gone = false; // the Signal was destroyed while parked (written under the lock before the wake)
        };

    private:
        using HandlerList = std::vector<std::pair<SlotId, std::shared_ptr<const Handler>>>;

        /**
         * @brief The state shared by the Signal, its parked @c next()
         *        awaiters and its scoped connections, so none of them
         *        touches freed memory if it outlives the Signal.
         */
        struct Core {
            mutable std::mutex mu;
            SlotId next_id = 1;
            // Copy-on-write under mu while an emission holds it (copying only shared_ptrs); null when empty. Edited
            // in place when the Core holds the only reference, i.e. no emission has taken a snapshot of it.
            std::shared_ptr<const HandlerList> handlers;
            std::vector<std::shared_ptr<NextSlot>> pending;
            bool dead = false; ///< the Signal was destroyed: a next() that parks now would never be woken

            SlotId connect(std::shared_ptr<const Handler> h) {
                std::lock_guard<std::mutex> lk(this->mu);
                const SlotId id = this->next_id++;
                if (this->handlers && this->handlers.use_count() == 1) {
                    // Emissions take their snapshot under mu, so none holds this list: append in place (amortised O(1)).
                    const_cast<HandlerList &>(*this->handlers).emplace_back(id, std::move(h));
                    return id;
                }
                // Copy-on-write: a running emission holds the current list by shared_ptr, so it is never mutated.
                auto next = this->handlers ? std::make_shared<HandlerList>(*this->handlers) : std::make_shared<HandlerList>();
                next->emplace_back(id, std::move(h));
                this->handlers = std::move(next);
                return id;
            }

            bool disconnect(SlotId id) {
                // Declared before the lock, so they are destroyed after it is released: the dying handler's captures
                // may call back into the signal (e.g. a ScopedConnection), which locks mu again.
                std::shared_ptr<const Handler> dying;
                std::shared_ptr<const HandlerList> oldList;
                std::lock_guard<std::mutex> lk(this->mu);
                if (!this->handlers) return false;
                auto &cur = const_cast<HandlerList &>(*this->handlers);
                auto it = std::find_if(cur.begin(), cur.end(), [id](const auto &e) { return e.first == id; });
                if (it == cur.end()) return false;
                if (this->handlers.use_count() == 1) {
                    // No emission holds a snapshot: erase in place, without allocating.
                    dying = std::move(it->second);
                    cur.erase(it);
                    if (cur.empty()) oldList = std::move(this->handlers);
                } else {
                    auto next = std::make_shared<HandlerList>(cur);
                    next->erase(next->begin() + (it - cur.begin()));
                    oldList = std::move(this->handlers);
                    this->handlers = next->empty() ? nullptr : std::move(next);
                }
                return true;
            }
        };

    public:
        Signal() : core(std::make_shared<Core>()) {
        }

        Signal(const Signal &) = delete;
        Signal(Signal &&) = delete;
        Signal &operator=(const Signal &) = delete;
        Signal &operator=(Signal &&) = delete;

        ~Signal() {
            std::vector<std::shared_ptr<NextSlot>> orphans;
            std::shared_ptr<const HandlerList> dropped;
            {
                std::lock_guard<std::mutex> lk(this->core->mu);
                orphans = std::move(this->core->pending);
                this->core->pending.clear();
                for (auto &slot : orphans) {
                    slot->gone = true;
                    slot->queued.store(false, std::memory_order_release);
                }
                this->core->dead = true;
                dropped = std::move(this->core->handlers);
            }
            // Resume outside the lock, on the pool: each next() throws CancelledException from its await_resume.
            for (auto &slot : orphans) YarnBall::detail::resumeOnPool(slot->handle);
        }

        /**
         * @brief RAII handle for a connected handler: disconnects it when
         *        destroyed (or on @ref disconnect). Move-only. Safe to
         *        outlive the Signal. Like @ref Signal::disconnect it does
         *        not wait for a call that is already in flight.
         */
        class ScopedConnection {
        public:
            ScopedConnection() = default;
            ScopedConnection(const ScopedConnection &) = delete;
            ScopedConnection &operator=(const ScopedConnection &) = delete;
            ScopedConnection(ScopedConnection &&o) noexcept
                : owner(std::move(o.owner)), id(std::exchange(o.id, 0)) {
            }
            ScopedConnection &operator=(ScopedConnection &&o) noexcept {
                if (this != &o) {
                    this->disconnect();
                    this->owner = std::move(o.owner);
                    this->id = std::exchange(o.id, 0);
                }
                return *this;
            }
            ~ScopedConnection() { this->disconnect(); }

            /// @return @c true if the handler was still connected and is now removed. Allocation-free unless an
            /// emission is running; if memory runs out then, the handler stays connected and this returns @c false.
            bool disconnect() noexcept {
                const SlotId slot = std::exchange(this->id, 0);
                auto c = std::exchange(this->owner, {}).lock();
                if (!c || slot == 0) return false;
                try {
                    return c->disconnect(slot);
                } catch (...) {
                    return false;
                }
            }

            /// Give up ownership: the handler stays connected. Returns its id.
            SlotId release() noexcept {
                this->owner.reset();
                return std::exchange(this->id, 0);
            }

            SlotId slot() const noexcept { return this->id; }

        private:
            friend class Signal;
            ScopedConnection(std::weak_ptr<Core> c, SlotId s) noexcept : owner(std::move(c)), id(s) {
            }

            std::weak_ptr<Core> owner;
            SlotId id = 0;
        };

        /**
         * @brief Register a handler. Returns an opaque @c SlotId that can
         *        be passed to @ref disconnect.
         */
        SlotId connect(Handler h) {
            return this->core->connect(std::make_shared<const Handler>(std::move(h)));
        }

        /**
         * @brief Like @ref connect, but the returned handle disconnects the
         *        handler when it is destroyed.
         */
        [[nodiscard]] ScopedConnection connectScoped(Handler h) {
            return ScopedConnection{this->core, this->connect(std::move(h))};
        }

        /**
         * @brief Remove a previously-connected handler. An emission
         *        already in flight works from a snapshot, so the handler
         *        may still run once after this returns (see the class
         *        notes).
         * @return @c true if the slot existed and was removed.
         */
        bool disconnect(SlotId id) {
            return this->core->disconnect(id);
        }

        /**
         * @brief Number of currently connected handlers (excludes pending
         *        one-shot @c next() awaiters).
         */
        std::size_t handlerCount() const {
            std::lock_guard<std::mutex> lk(this->core->mu);
            return this->core->handlers ? this->core->handlers->size() : 0;
        }

        /**
         * @brief Number of parked @c next() awaiters (a snapshot). Lets a
         *        caller wait until a coroutine is actually listening before
         *        emitting, since @c next() does not see earlier emissions.
         */
        std::size_t waiterCount() const {
            std::lock_guard<std::mutex> lk(this->core->mu);
            return this->core->pending.size();
        }

        /**
         * @brief Synchronously broadcast to all connected handlers and to
         *        all pending one-shot @c next() awaiters.
         *
         * Handlers fire on the calling thread in registration order.
         * @c next() awaiters are resumed via Yarn::run, so their bodies
         * run on workers.
         *
         * Every handler runs even if an earlier one throws: the first
         * exception is rethrown after the last handler has run.
         */
        void emit(Args... args) {
            // Snapshot handlers + pending waiters under the lock; release
            // the lock before invoking either, so handlers can connect /
            // disconnect freely without re-entrant deadlock.
            std::shared_ptr<const HandlerList> handlers_copy;
            std::vector<std::shared_ptr<NextSlot>> waiters_copy;
            this->snapshot(handlers_copy, waiters_copy);

            this->wake(waiters_copy, args...);
            if (!handlers_copy) return;
            std::exception_ptr first;
            for (const auto &entry : *handlers_copy) {
                try {
                    (*entry.second)(args...);
                } catch (...) {
                    if (!first) first = std::current_exception();
                }
            }
            if (first) std::rethrow_exception(first);
        }

        /**
         * @brief Dispatch each connected handler as a separate task on the
         *        Yarn pool. Returns immediately. One-shot @c next() awaiters
         *        are resumed exactly as in @ref emit (also on the pool).
         *
         * A handler that throws is reported through the process-wide
         * handler installed with @c YarnBall::setUnhandledExceptionHandler
         * (by default logged); it does not affect the other handlers. There
         * is no backpressure: every emission queues one pool task per
         * handler, so a producer that outruns the pool grows the queue
         * without bound.
         */
        void emitAsync(Args... args) {
            std::shared_ptr<const HandlerList> handlers_copy;
            std::vector<std::shared_ptr<NextSlot>> waiters_copy;
            this->snapshot(handlers_copy, waiters_copy);

            this->wake(waiters_copy, args...);

            // Dispatch each handler as a TaskAdapter on the pool.
            if (handlers_copy) {
                for (const auto &entry : *handlers_copy) {
                    this->dispatch_handler(entry.second, args...);
                }
            }
        }

    private:
        void snapshot(std::shared_ptr<const HandlerList> &handlers_copy,
                      std::vector<std::shared_ptr<NextSlot>> &waiters_copy) {
            std::lock_guard<std::mutex> lk(this->core->mu);
            handlers_copy = this->core->handlers;
            if (!this->core->pending.empty()) {
                waiters_copy = std::move(this->core->pending);
                this->core->pending.clear();
                for (auto &slot : waiters_copy) slot->queued.store(false, std::memory_order_release);
            }
        }

        /// Hand the emission to every waiter taken out of the pending list and resume it on the pool. Nothing
        /// here throws past a waiter, so handlers are never skipped because of a wake.
        static void wake(std::vector<std::shared_ptr<NextSlot>> &waiters, const Args &... args) {
            for (auto &slot : waiters) {
                try {
                    slot->value.emplace(args...);
                } catch (...) {
                    slot->gone = true; // the copy failed: the waiter throws CancelledException rather than hang
                }
                YarnBall::detail::resumeOnPool(slot->handle);
            }
        }

    public:
        /**
         * @brief Awaiter returned from @ref next. Suspends until the next
         *        emission, then resumes with a tuple of the emitted args.
         *        Cancellable (see @c Cancellation.h): inside a stopped scope
         *        it leaves the pending list and throws
         *        @c YarnBall::CancelledException; a frame destroyed while
         *        parked also leaves it. If the Signal is destroyed while
         *        parked it throws @c YarnBall::CancelledException too.
         */
        struct NextAwaiter {
            std::shared_ptr<Core> sig;
            std::shared_ptr<NextSlot> slot;

            NextAwaiter(std::shared_ptr<Core> s, std::shared_ptr<NextSlot> sl) noexcept
                : sig(std::move(s)), slot(std::move(sl)) {
            }
            NextAwaiter(const NextAwaiter &) = delete;
            NextAwaiter &operator=(const NextAwaiter &) = delete;

            ~NextAwaiter() {
                this->bind.disarm();
                if (!this->proxy) return;
                bool removed = false;
                if (this->slot->queued.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> lk(sig->mu);
                    if (this->slot->queued.load(std::memory_order_relaxed)) {
                        this->unlinkLocked();
                        removed = true;
                    }
                }
                this->proxy.retire(removed);
            }

            bool await_ready() const noexcept { return false; }

            template<typename Promise>
            bool await_suspend(std::coroutine_handle<Promise> h) {
                YarnBall::detail::TaskPromiseCore *core = YarnBall::detail::CancelBinding::ambient(h);
                bool armed = false;
                for (;;) {
                    {
                        std::lock_guard<std::mutex> lk(sig->mu);
                        if (this->earlyCancel) {
                            this->cancelled = true;
                            return false;
                        }
                        if (sig->dead) {
                            // The Signal is already destroyed and nothing will ever wake this waiter.
                            this->slot->gone = true;
                            return false;
                        }
                        if (!core || armed) {
                            slot->handle = this->proxy.publish(h);
                            slot->queued.store(true, std::memory_order_release);
                            sig->pending.push_back(slot);
                            return true;
                        }
                        if (core->isCancellationRequested()) {
                            this->cancelled = true;
                            return false;
                        }
                    }
                    this->bind.arm(core, &NextAwaiter::cancelThunk, this);
                    armed = true;
                }
            }

            std::tuple<Args...> await_resume() {
                if (this->cancelled || this->slot->gone || !this->slot->value) throw YarnBall::CancelledException{};
                return std::move(*slot->value);
            }

        private:
            void unlinkLocked() noexcept {
                auto it = std::find(sig->pending.begin(), sig->pending.end(), this->slot);
                if (it != sig->pending.end()) sig->pending.erase(it);
                this->slot->queued.store(false, std::memory_order_release);
            }

            static void cancelThunk(void *p) noexcept {
                auto *self = static_cast<NextAwaiter *>(p);
                std::coroutine_handle<> h{};
                {
                    std::lock_guard<std::mutex> lk(self->sig->mu);
                    if (self->slot->queued.load(std::memory_order_relaxed)) {
                        h = self->slot->handle;
                        self->unlinkLocked();
                        self->cancelled = true;
                    } else {
                        self->earlyCancel = true;
                    }
                }
                if (h) YarnBall::detail::resumeOnPool(h);
            }

            bool earlyCancel = false; // guarded by sig->mu
            bool cancelled = false;
            YarnBall::detail::WaitProxy proxy;
            YarnBall::detail::CancelBinding bind;
        };

        /**
         * @brief Get an awaitable that resumes on the next @c emit.
         *        Single-fire: re-await for subsequent emissions.
         */
        NextAwaiter next() {
            return NextAwaiter{this->core, std::make_shared<NextSlot>()};
        }

    private:
        /**
         * @brief Internal ITask that calls a handler with a stored tuple of
         *        arguments. Used by @ref emitAsync.
         */
        struct HandlerTask : public YarnBall::ITask {
            std::shared_ptr<const Handler> h;
            std::tuple<Args...> args;

            HandlerTask(std::shared_ptr<const Handler> hh, std::tuple<Args...> aa)
                : h(std::move(hh)), args(std::move(aa)) {
            }

            void run() override {
                std::apply(*this->h, this->args);
            }

            void exception(std::exception_ptr e) override {
                YarnBall::detail::reportDetachedException(e);
            }
        };

        static void dispatch_handler(const std::shared_ptr<const Handler> &h, const Args &... args) {
            try {
                std::unique_ptr<YarnBall::ITask> task{new HandlerTask(h, std::tuple<Args...>(args...))};
                YarnBall::Yarn::instance()->run(std::move(task));
            } catch (...) {
                // The pool refused the task: run it here instead, so the handler is never silently skipped.
                try {
                    (*h)(args...);
                } catch (...) {
                    YarnBall::detail::reportDetachedException(std::current_exception());
                }
            }
        }

        std::shared_ptr<Core> core;
    };

}

#endif // WIRE_SIGNAL_H
