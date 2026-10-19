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
     */
    template<typename... Args>
    class Signal {
    public:
        using Handler = std::function<void(Args...)>;
        using SlotId = std::uint64_t;

        Signal() = default;

        Signal(const Signal &) = delete;
        Signal(Signal &&) = delete;
        Signal &operator=(const Signal &) = delete;
        Signal &operator=(Signal &&) = delete;

        ~Signal() = default;

        /**
         * @brief Register a handler. Returns an opaque @c SlotId that can
         *        be passed to @ref disconnect.
         */
        SlotId connect(Handler h) {
            std::lock_guard<std::mutex> lk(this->mu);
            const SlotId id = this->next_id++;
            this->handlers.emplace_back(id, std::move(h));
            return id;
        }

        /**
         * @brief Remove a previously-connected handler. An emission
         *        already in flight works from a snapshot, so the handler
         *        may still run once after this returns.
         * @return @c true if the slot existed and was removed.
         */
        bool disconnect(SlotId id) {
            std::lock_guard<std::mutex> lk(this->mu);
            for (auto it = this->handlers.begin(); it != this->handlers.end(); ++it) {
                if (it->first == id) {
                    this->handlers.erase(it);
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief Number of currently connected handlers (excludes pending
         *        one-shot @c next() awaiters).
         */
        std::size_t handlerCount() const {
            std::lock_guard<std::mutex> lk(this->mu);
            return this->handlers.size();
        }

        /**
         * @brief Synchronously broadcast to all connected handlers and to
         *        all pending one-shot @c next() awaiters.
         *
         * Handlers fire on the calling thread in registration order.
         * @c next() awaiters are resumed via Yarn::run, so their bodies
         * run on workers.
         */
        void emit(Args... args) {
            // Snapshot handlers + pending waiters under the lock; release
            // the lock before invoking either, so handlers can connect /
            // disconnect freely without re-entrant deadlock.
            std::vector<std::pair<SlotId, Handler>> handlers_copy;
            std::vector<std::shared_ptr<NextSlot>> waiters_copy;
            {
                std::lock_guard<std::mutex> lk(this->mu);
                handlers_copy = this->handlers;
                waiters_copy = std::move(this->pending);
                this->pending.clear();
                for (auto &slot : waiters_copy) slot->queued.store(false, std::memory_order_release);
            }

            for (auto &slot : waiters_copy) {
                slot->value.emplace(args...);
                // Wake on a worker rather than recursively resuming here.
                std::unique_ptr<YarnBall::ITask> ct{
                    new YarnBall::detail::CoroutineITask(slot->handle)};
                YarnBall::Yarn::instance()->run(std::move(ct));
            }
            for (auto &[id, h] : handlers_copy) {
                h(args...);
            }
        }

        /**
         * @brief Dispatch each connected handler as a separate task on the
         *        Yarn pool. Returns immediately. One-shot @c next() awaiters
         *        are resumed exactly as in @ref emit (also on the pool).
         */
        void emitAsync(Args... args) {
            std::vector<std::pair<SlotId, Handler>> handlers_copy;
            std::vector<std::shared_ptr<NextSlot>> waiters_copy;
            {
                std::lock_guard<std::mutex> lk(this->mu);
                handlers_copy = this->handlers;
                waiters_copy = std::move(this->pending);
                this->pending.clear();
                for (auto &slot : waiters_copy) slot->queued.store(false, std::memory_order_release);
            }

            for (auto &slot : waiters_copy) {
                slot->value.emplace(args...);
                std::unique_ptr<YarnBall::ITask> ct{
                    new YarnBall::detail::CoroutineITask(slot->handle)};
                YarnBall::Yarn::instance()->run(std::move(ct));
            }

            // Dispatch each handler as a TaskAdapter on the pool.
            for (auto &[id, h] : handlers_copy) {
                this->dispatch_handler(h, std::make_tuple(args...));
            }
        }

        /**
         * @brief Awaiter helper. Stored as @c shared_ptr so the awaiter, the
         *        Signal's pending list, and any in-flight emission share
         *        ownership of the result slot.
         */
        struct NextSlot {
            std::coroutine_handle<> handle{};
            std::optional<std::tuple<Args...>> value;
            std::atomic<bool> queued{false};
        };

        /**
         * @brief Awaiter returned from @ref next. Suspends until the next
         *        emission, then resumes with a tuple of the emitted args.
         *        Cancellable (see @c Cancellation.h): inside a stopped scope
         *        it leaves the pending list and throws
         *        @c YarnBall::CancelledException; a frame destroyed while
         *        parked also leaves it.
         */
        struct NextAwaiter {
            Signal *sig;
            std::shared_ptr<NextSlot> slot;

            NextAwaiter(Signal *s, std::shared_ptr<NextSlot> sl) noexcept
                : sig(s), slot(std::move(sl)) {
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
                if (this->cancelled) throw YarnBall::CancelledException{};
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
                if (h) {
                    try {
                        std::unique_ptr<YarnBall::ITask> ct{new YarnBall::detail::CoroutineITask(h)};
                        YarnBall::Yarn::instance()->run(std::move(ct));
                    } catch (...) {
                        h.resume();
                    }
                }
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
            return NextAwaiter{this, std::make_shared<NextSlot>()};
        }

    private:
        /**
         * @brief Internal ITask that calls a handler with a stored tuple of
         *        arguments. Used by @ref emitAsync.
         */
        struct HandlerTask : public YarnBall::ITask {
            Handler h;
            std::tuple<Args...> args;

            HandlerTask(Handler hh, std::tuple<Args...> aa)
                : h(std::move(hh)), args(std::move(aa)) {
            }

            void run() override {
                std::apply(this->h, this->args);
            }

            void exception(std::exception_ptr) override {
            }
        };

        void dispatch_handler(Handler h, std::tuple<Args...> args) {
            std::unique_ptr<YarnBall::ITask> task{
                new HandlerTask(std::move(h), std::move(args))};
            YarnBall::Yarn::instance()->run(std::move(task));
        }

        mutable std::mutex mu;
        SlotId next_id = 1;
        std::vector<std::pair<SlotId, Handler>> handlers;
        std::vector<std::shared_ptr<NextSlot>> pending;
    };

}

#endif // WIRE_SIGNAL_H
