//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef WIRE_CHANNEL_H
#define WIRE_CHANNEL_H

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "Cancellation.h"
#include "Coroutines.h"

namespace Telegraph {

    /**
     * @class Channel
     * @brief Unbounded MPMC value channel with coroutine-aware @c receive
     *        and synchronous @c send.
     *
     * Semantics:
     *  - @c send is non-blocking and always succeeds (unless the channel
     *    is closed, in which case it returns @c false).
     *  - @c receive is a coroutine that resumes with the next value, or
     *    with @c nullopt if the channel was closed and is now empty.
     *  - @c close wakes every pending receiver with @c nullopt.
     *
     * Hand-off: if a receiver is waiting when @c send arrives, the value
     * goes directly to the waiter's output slot and the waiter is
     * resumed on a Yarn worker (no buffer hop, no extra copy beyond the
     * one already required by `T value` being passed by value to send()).
     *
     * Bounded variants and explicit backpressure can be added later;
     * v1 is unbounded to keep the protocol simple.
     *
     * A parked @c receive is cancellable (see @c Cancellation.h): inside
     * a stopped cancellation scope it leaves the waiter queue and throws
     * @c YarnBall::CancelledException, so a value sent afterwards goes to
     * the next receiver or the buffer, not to the abandoned one. A frame
     * destroyed while parked also leaves the queue. @ref pushFront puts a
     * value back at the head, for a receiver that obtained a value it
     * can no longer use (see @c channelSelect).
     */
    template<typename T>
    class Channel {
    public:
        Channel() = default;

        Channel(const Channel &) = delete;
        Channel(Channel &&) = delete;
        Channel &operator=(const Channel &) = delete;
        Channel &operator=(Channel &&) = delete;

        ~Channel() {
            this->close();
        }

        /**
         * @brief Push a value into the channel.
         * @return @c true on success; @c false if the channel was closed.
         */
        bool send(T value) {
            std::shared_ptr<Waiter> receiver;
            {
                std::lock_guard<std::mutex> lk(this->mu);
                if (this->closed_) return false;

                if (!this->waiters.empty()) {
                    receiver = std::move(this->waiters.front());
                    this->waiters.pop_front();
                    receiver->queued.store(false, std::memory_order_release);
                    // Under the lock, so a receiver frame destroyed before the wake runs can take the value back.
                    receiver->slot.emplace(std::move(value));
                } else {
                    this->buffer.push_back(std::move(value));
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
         *        receiver if there is one. Returns @c false (dropping the
         *        value) if the channel is closed.
         */
        bool pushFront(T value) {
            std::shared_ptr<Waiter> receiver;
            {
                std::lock_guard<std::mutex> lk(this->mu);
                if (this->closed_) return false;
                if (!this->waiters.empty()) {
                    receiver = std::move(this->waiters.front());
                    this->waiters.pop_front();
                    receiver->queued.store(false, std::memory_order_release);
                    receiver->slot.emplace(std::move(value));
                } else {
                    this->buffer.push_front(std::move(value));
                    return true;
                }
            }
            dispatch(receiver->handle);
            return true;
        }

        /**
         * @brief Receive a value. Suspends until one is available or the
         *        channel is closed.
         * @return The value, or @c std::nullopt if the channel was closed
         *         and is now drained.
         */
        YarnBall::Task<std::optional<T>> receive() {
            // Fast path: a value is already buffered, or channel is closed.
            {
                std::lock_guard<std::mutex> lk(this->mu);
                if (!this->buffer.empty()) {
                    T value = std::move(this->buffer.front());
                    this->buffer.pop_front();
                    co_return std::optional<T>(std::move(value));
                }
                if (this->closed_) {
                    co_return std::nullopt;
                }
            }

            auto waiter = std::make_shared<Waiter>();
            co_await ReceiveAwaiter{this, waiter};

            if (!waiter->slot.has_value()) {
                co_return std::nullopt;
            }
            co_return std::move(*waiter->slot);
        }

        /**
         * @brief Mark the channel closed and wake every pending receiver
         *        with @c nullopt. Idempotent.
         */
        void close() {
            std::deque<std::shared_ptr<Waiter>> to_wake;
            {
                std::lock_guard<std::mutex> lk(this->mu);
                if (this->closed_) return;
                this->closed_ = true;
                to_wake = std::move(this->waiters);
                this->waiters.clear();
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
            std::lock_guard<std::mutex> lk(this->mu);
            return this->closed_;
        }

        /**
         * @return Approximate number of buffered values (snapshot).
         */
        std::size_t size() const noexcept {
            std::lock_guard<std::mutex> lk(this->mu);
            return this->buffer.size();
        }

    private:
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
            Channel *ch;
            std::shared_ptr<Waiter> waiter;

            ReceiveAwaiter(Channel *c, std::shared_ptr<Waiter> w) noexcept
                : ch(c), waiter(std::move(w)) {
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
                if (orphan) this->ch->pushFront(std::move(*orphan));
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

        mutable std::mutex mu;
        std::deque<T> buffer;
        std::deque<std::shared_ptr<Waiter>> waiters;
        bool closed_ = false;
    };

}

#endif // WIRE_CHANNEL_H
