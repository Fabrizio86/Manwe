//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef WIRE_CHANNEL_H
#define WIRE_CHANNEL_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "Cancellation.h"
#include "Coroutines.h"
#include "Deadline.h"

namespace Telegraph {

    /**
     * @class Channel
     * @brief Unbounded MPMC value channel with coroutine-aware @c receive
     *        and synchronous @c send.
     *
     * Semantics:
     *  - @c send is non-blocking and succeeds unless the channel is
     *    closed or its optional soft cap is reached (it returns @c false).
     *  - @c receive is a coroutine that resumes with the next value, or
     *    with @c nullopt if the channel was closed and is now empty.
     *  - @c close wakes every pending receiver with @c nullopt.
     *
     * Hand-off: if a receiver is waiting when @c send arrives, the value
     * goes directly to the waiter's output slot and the waiter is
     * resumed on a Yarn worker (no buffer hop, no extra copy beyond the
     * one already required by `T value` being passed by value to send()).
     *
     * The buffer is unbounded unless a soft cap is given to the
     * constructor: with a cap, @c send returns @c false instead of
     * buffering past it (it never blocks). For producers that must be
     * slowed down rather than refused, use @c BoundedChannel, whose
     * @c send suspends.
     *
     * A parked @c receive is cancellable (see @c Cancellation.h): inside
     * a stopped cancellation scope it leaves the waiter queue and throws
     * @c YarnBall::CancelledException, so a value sent afterwards goes to
     * the next receiver or the buffer, not to the abandoned one. A frame
     * destroyed while parked also leaves the queue. @ref pushFront puts a
     * value back at the head, for a receiver that obtained a value it
     * can no longer use (see @c channelSelect); it is accepted by a closed
     * channel too (a closed channel still drains).
     *
     * Timeouts: wrap @c receive in @ref receiveFor, not in
     * @c YarnBall::withTimeout. @c withTimeout races the receive against a
     * timer and discards the loser's result, so a value handed to the
     * receive at the instant the timer wins is dropped (and the same
     * holds for @c whenAny). @c receiveFor cancels the parked receive
     * instead, so every value is either returned or stays in the channel.
     *
     * A task returned by @c receive or @c receiveFor reads the channel when
     * it first runs: await it while the channel is alive. (A receive that is
     * already parked is safe against the channel being destroyed, see
     * below.)
     *
     * The channel's state is shared with the parked receivers, so a
     * receiver frame that outlives the channel (it was woken by the
     * destructor's @c close and has not been destroyed yet) never touches
     * freed memory.
     */
    template<typename T>
    class Channel {
    public:
        /**
         * @param maxBuffered Soft cap on buffered values; @c send returns
         *        @c false when it is reached. Zero (the default) means
         *        unbounded. @ref pushFront ignores the cap.
         */
        explicit Channel(std::size_t maxBuffered = 0) : core(std::make_shared<Core>(maxBuffered)) {
        }

        Channel(const Channel &) = delete;
        Channel(Channel &&) = delete;
        Channel &operator=(const Channel &) = delete;
        Channel &operator=(Channel &&) = delete;

        ~Channel() {
            this->close();
        }

        /**
         * @brief Push a value into the channel.
         * @return @c true on success; @c false if the channel was closed or
         *         its soft cap is reached (the value is dropped).
         */
        bool send(T value) {
            Core &c = *this->core;
            std::shared_ptr<Waiter> receiver;
            {
                std::lock_guard<std::mutex> lk(c.mu);
                if (c.closed_) return false;

                if (!c.waiters.empty()) {
                    // Hand over first, then dequeue: if moving the value throws, the receiver is still parked.
                    // Under the lock, so a receiver frame destroyed before the wake runs can take the value back.
                    c.waiters.front()->slot.emplace(std::move(value));
                    receiver = popWaiterLocked(c);
                } else {
                    if (c.maxBuffered != 0 && c.buffer.size() >= c.maxBuffered) return false;
                    c.buffer.push_back(std::move(value));
                    return true;
                }
            }

            // Wake the receiver, outside the lock.
            dispatch(receiver->handle);
            return true;
        }

        /**
         * @brief Put @p value back at the head of the channel, ahead of
         *        anything buffered: it goes straight to the oldest parked
         *        receiver if there is one. On a closed channel the value
         *        goes to the buffer, which receivers still drain. Always
         *        returns @c true.
         */
        bool pushFront(T value) {
            return pushFrontCore(*this->core, std::move(value));
        }

        /**
         * @brief Receive a value. Suspends until one is available or the
         *        channel is closed.
         * @return The value, or @c std::nullopt if the channel was closed
         *         and is now drained.
         */
        YarnBall::Task<std::optional<T>> receive() {
            // Fast path: a value is already buffered, or channel is closed. Works on the channel's core directly:
            // the shared_ptr is copied (two atomic RMWs) only when about to park.
            Core &c = *this->core;
            std::shared_ptr<Core> held;
            {
                std::lock_guard<std::mutex> lk(c.mu);
                if (!c.buffer.empty()) {
                    T value = std::move(c.buffer.front());
                    c.buffer.pop_front();
                    co_return std::optional<T>(std::move(value));
                }
                if (c.closed_) {
                    co_return std::nullopt;
                }
                held = this->core;
            }

            auto waiter = std::make_shared<Waiter>();
            co_await ReceiveAwaiter{std::move(held), waiter};

            if (!waiter->slot.has_value()) {
                co_return std::nullopt;
            }
            co_return std::move(*waiter->slot);
        }

        /**
         * @brief @ref receive with a deadline. Lossless: if the deadline
         *        passes first the parked receive is cancelled and leaves
         *        the queue, so a value sent afterwards goes to the next
         *        receiver or the buffer; a value handed over at the same
         *        instant is returned (or put back), never dropped.
         * @return The value, or @c std::nullopt if the channel was closed
         *         and is drained.
         * @throws YarnBall::TimeoutException if @p timeout elapsed first;
         *         @c YarnBall::CancelledException if the caller's own scope
         *         was stopped.
         */
        template<typename Rep, typename Period>
        YarnBall::Task<std::optional<T>> receiveFor(std::chrono::duration<Rep, Period> timeout) {
            detail::DeadlineGuard guard(timeout);
            YarnBall::detail::TaskPromiseCore *self = co_await YarnBall::detail::CurrentScopeAwaiter{};
            try {
                co_return co_await YarnBall::withCancellation(guard.token(), this->receive());
            } catch (const YarnBall::CancelledException &) {
                // The caller's own cancellation wins over a deadline that expired at the same time.
                if (guard.expired() && !(self && self->isCancellationRequested())) throw YarnBall::TimeoutException("Channel::receiveFor");
                throw;
            }
        }

        /**
         * @brief Mark the channel closed and wake every pending receiver
         *        with @c nullopt. Idempotent.
         */
        void close() {
            Core &c = *this->core;
            std::deque<std::shared_ptr<Waiter>> to_wake;
            {
                std::lock_guard<std::mutex> lk(c.mu);
                if (c.closed_) return;
                c.closed_ = true;
                to_wake = std::move(c.waiters);
                c.waiters.clear();
                for (auto &w : to_wake) w->queued.store(false, std::memory_order_release);
            }
            for (auto &w : to_wake) {
                // slot stays nullopt -> receiver returns nullopt.
                dispatch(w->handle);
            }
        }

        /**
         * @return @c true if @c close has been called.
         */
        bool closed() const noexcept {
            std::lock_guard<std::mutex> lk(this->core->mu);
            return this->core->closed_;
        }

        /**
         * @return Approximate number of buffered values (snapshot).
         */
        std::size_t size() const noexcept {
            std::lock_guard<std::mutex> lk(this->core->mu);
            return this->core->buffer.size();
        }

        /**
         * @return Number of receivers currently parked (snapshot).
         */
        std::size_t waiterCount() const noexcept {
            std::lock_guard<std::mutex> lk(this->core->mu);
            return this->core->waiters.size();
        }

    private:
        struct Waiter;

        /**
         * @brief Everything the channel guards. Shared with every parked
         *        receiver, so a receiver frame destroyed after the channel
         *        is gone still finds a live mutex and queue.
         */
        struct Core {
            explicit Core(std::size_t cap) noexcept : maxBuffered(cap) {
            }

            mutable std::mutex mu;
            std::deque<T> buffer;
            std::deque<std::shared_ptr<Waiter>> waiters;
            const std::size_t maxBuffered;
            bool closed_ = false;
        };

        /**
         * @brief Suspended-receiver state. The slot holds the value once
         *        a sender hands one off (or stays empty on close).
         */
        struct Waiter {
            std::coroutine_handle<> handle{};
            std::optional<T> slot;
            std::atomic<bool> queued{false};
        };

        /**
         * @brief Awaiter that registers a Waiter on the channel and parks
         *        the coroutine until @c send or @c close wakes it. Cancel
         *        protocol as in @c YarnBall::detail::ParkBase.
         */
        struct ReceiveAwaiter {
            std::shared_ptr<Core> ch;
            std::shared_ptr<Waiter> waiter;

            ReceiveAwaiter(std::shared_ptr<Core> c, std::shared_ptr<Waiter> w) noexcept
                : ch(std::move(c)), waiter(std::move(w)) {
            }
            ReceiveAwaiter(const ReceiveAwaiter &) = delete;
            ReceiveAwaiter &operator=(const ReceiveAwaiter &) = delete;

            ~ReceiveAwaiter() {
                this->bind.disarm();
                if (!this->proxy) return;
                std::optional<T> orphan;
                {
                    std::lock_guard<std::mutex> lk(this->ch->mu);
                    bool removed = false;
                    if (this->waiter->queued.load(std::memory_order_relaxed)) {
                        this->unlinkLocked();
                        removed = true;
                    }
                    // A wake that was handed this receiver but never ran (the frame is being destroyed): the value it
                    // carried is put back instead of dying with the receiver. The sender stored it under this lock.
                    if (this->proxy.retire(removed) && this->waiter->slot.has_value()) {
                        orphan.emplace(std::move(*this->waiter->slot));
                        this->waiter->slot.reset();
                    }
                }
                if (orphan) pushFrontCore(*this->ch, std::move(*orphan));
            }

            bool await_ready() const noexcept { return false; }

            template<typename Promise>
            bool await_suspend(std::coroutine_handle<Promise> h) {
                YarnBall::detail::TaskPromiseCore *core = YarnBall::detail::CancelBinding::ambient(h);
                bool armed = false;
                for (;;) {
                    {
                        std::lock_guard<std::mutex> lk(ch->mu);
                        if (this->earlyCancel) {
                            this->cancelled = true;
                            return false;
                        }
                        // Recheck buffer + closed under the lock to close the race.
                        if (!ch->buffer.empty()) {
                            waiter->slot.emplace(std::move(ch->buffer.front()));
                            ch->buffer.pop_front();
                            return false;
                        }
                        if (ch->closed_) {
                            return false;
                        }
                        if (!core || armed) {
                            waiter->handle = this->proxy.publish(h);
                            waiter->queued.store(true, std::memory_order_release);
                            ch->waiters.push_back(waiter);
                            return true;
                        }
                        if (core->isCancellationRequested()) {
                            this->cancelled = true;
                            return false;
                        }
                    }
                    this->bind.arm(core, &ReceiveAwaiter::cancelThunk, this);
                    armed = true;
                }
            }

            void await_resume() {
                if (this->cancelled) throw YarnBall::CancelledException{};
            }

        private:
            void unlinkLocked() noexcept {
                auto it = std::find(ch->waiters.begin(), ch->waiters.end(), this->waiter);
                if (it != ch->waiters.end()) ch->waiters.erase(it);
                this->waiter->queued.store(false, std::memory_order_release);
            }

            static void cancelThunk(void *p) noexcept {
                auto *self = static_cast<ReceiveAwaiter *>(p);
                std::coroutine_handle<> h{};
                {
                    std::lock_guard<std::mutex> lk(self->ch->mu);
                    if (self->waiter->queued.load(std::memory_order_relaxed)) {
                        h = self->waiter->handle;
                        self->unlinkLocked();
                        self->cancelled = true;
                    } else {
                        self->earlyCancel = true;
                    }
                }
                if (h) dispatch(h);
            }

            bool earlyCancel = false; // guarded by ch->mu
            bool cancelled = false;
            YarnBall::detail::WaitProxy proxy;
            YarnBall::detail::CancelBinding bind;
        };

        /**
         * @brief Wake @p h on the Yarn pool; if the pool refuses the
         *        task, resume inline so the receiver is never stranded.
         */
        static void dispatch(std::coroutine_handle<> h) noexcept {
            try {
                std::unique_ptr<YarnBall::ITask> ct{
                    new YarnBall::detail::CoroutineITask(h)};
                YarnBall::Yarn::instance()->run(std::move(ct));
            } catch (...) {
                h.resume();
            }
        }

        static std::shared_ptr<Waiter> popWaiterLocked(Core &c) {
            auto w = std::move(c.waiters.front());
            c.waiters.pop_front();
            w->queued.store(false, std::memory_order_release);
            return w;
        }

        static bool pushFrontCore(Core &c, T value) {
            std::shared_ptr<Waiter> receiver;
            {
                std::lock_guard<std::mutex> lk(c.mu);
                if (!c.closed_ && !c.waiters.empty()) {
                    c.waiters.front()->slot.emplace(std::move(value));
                    receiver = popWaiterLocked(c);
                } else {
                    // Closed: no receiver is parked, and the buffer is still drained by later receives.
                    c.buffer.push_front(std::move(value));
                    return true;
                }
            }
            dispatch(receiver->handle);
            return true;
        }

        std::shared_ptr<Core> core;
    };

}

#endif // WIRE_CHANNEL_H
