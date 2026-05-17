//
// Created by Fabrizio Paino on 2026-05-16.
//
// BoundedChannel<T>: MPMC channel with a fixed-capacity buffer and
// asynchronous send. Differs from Telegraph::Channel<T> in that send
// suspends when the buffer is full and resumes when a receiver pulls
// a value off; that's the natural backpressure primitive for a
// producer that can outrun its consumer.
//
// Receive semantics are identical to Channel<T>: returns std::optional
// (empty on close-after-drain), Yarn-dispatched wakeup, FIFO waiter
// queue.
//

#ifndef WIRE_BOUNDED_CHANNEL_H
#define WIRE_BOUNDED_CHANNEL_H

#include <coroutine>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "Coroutines.h"

namespace Telegraph {

    /**
     * @class BoundedChannel
     * @brief MPMC value channel with a fixed-capacity buffer. Both
     *        @c send and @c receive are coroutine-aware: @c send
     *        suspends if the buffer is full, @c receive suspends if
     *        it is empty. Wakeups dispatch through Yarn rather than
     *        inline-resuming the caller (which would unbound stacks
     *        under producer/consumer chains).
     *
     * Strict FIFO ordering on both sides: the oldest waiting sender
     * and the oldest waiting receiver are paired, never overtaken by
     * later arrivals.
     *
     * @tparam T Value type. Must be move-constructible.
     */
    template<typename T>
    class BoundedChannel {
    public:
        /**
         * @brief Construct with buffer capacity @p capacity. A capacity
         *        of zero degenerates to a synchronous rendezvous channel:
         *        every @c send pairs directly with a waiting receiver,
         *        and either side suspends until the other arrives.
         */
        explicit BoundedChannel(std::size_t capacity) noexcept
            : capacityVal(capacity) {
        }

        BoundedChannel(const BoundedChannel &) = delete;
        BoundedChannel(BoundedChannel &&) = delete;
        BoundedChannel &operator=(const BoundedChannel &) = delete;
        BoundedChannel &operator=(BoundedChannel &&) = delete;

        ~BoundedChannel() { this->close(); }

        /**
         * @brief Push a value. Suspends if the buffer is full; resumes
         *        when a receiver pulls one out and frees a slot.
         *
         * @return @c true on success, @c false if the channel was
         *         closed before the value could be delivered.
         */
        YarnBall::Task<bool> send(T value) {
            // Fast path: hand-off to a waiting receiver, or push into a
            // non-full buffer. Both happen synchronously under the lock.
            {
                std::shared_ptr<RecvWaiter> receiver;
                {
                    std::lock_guard<std::mutex> lk(this->mu);
                    if (this->closedFlag) co_return false;
                    if (!this->receivers.empty()) {
                        receiver = std::move(this->receivers.front());
                        this->receivers.pop_front();
                    } else if (this->buffer.size() < this->capacityVal) {
                        this->buffer.push_back(std::move(value));
                        co_return true;
                    }
                }
                if (receiver) {
                    receiver->slot.emplace(std::move(value));
                    dispatch(receiver->handle);
                    co_return true;
                }
            }

            // Slow path: buffer full, no receiver waiting. Park the
            // sender on the senders queue; a receiver will wake us
            // once a slot frees up (or close() will fail the send).
            auto sender = std::make_shared<SendWaiter>();
            sender->value = std::move(value);
            co_await SendAwaiter{this, sender};
            co_return sender->ok;
        }

        /**
         * @brief Receive a value. Suspends if the buffer is empty and
         *        no sender is waiting.
         * @return The value, or @c std::nullopt if the channel was
         *         closed and is now drained.
         */
        YarnBall::Task<std::optional<T>> receive() {
            // Fast path: buffer non-empty, or channel already closed.
            {
                std::shared_ptr<SendWaiter> wakeSender;
                std::lock_guard<std::mutex> lk(this->mu);
                if (!this->buffer.empty()) {
                    T value = std::move(this->buffer.front());
                    this->buffer.pop_front();
                    // Pull one parked sender into the now-empty slot if
                    // there is one queued. The sender's value goes into
                    // the buffer; the sender wakes on next iteration.
                    if (!this->senders.empty()) {
                        wakeSender = std::move(this->senders.front());
                        this->senders.pop_front();
                        this->buffer.push_back(std::move(wakeSender->value));
                    }
                    if (wakeSender) {
                        wakeSender->ok = true;
                        dispatch(wakeSender->handle);
                    }
                    co_return std::optional<T>(std::move(value));
                }
                if (this->closedFlag) {
                    co_return std::nullopt;
                }
            }

            // Slow path: buffer empty, no sender waiting.
            auto receiver = std::make_shared<RecvWaiter>();
            co_await ReceiveAwaiter{this, receiver};
            if (!receiver->slot.has_value()) co_return std::nullopt;
            co_return std::move(*receiver->slot);
        }

        /**
         * @brief Mark the channel closed. Wakes every pending receiver
         *        with @c nullopt and every pending sender with @c false.
         *        Idempotent.
         */
        void close() {
            std::deque<std::shared_ptr<RecvWaiter>> wakeRecv;
            std::deque<std::shared_ptr<SendWaiter>> wakeSend;
            {
                std::lock_guard<std::mutex> lk(this->mu);
                if (this->closedFlag) return;
                this->closedFlag = true;
                wakeRecv = std::move(this->receivers);
                this->receivers.clear();
                wakeSend = std::move(this->senders);
                this->senders.clear();
            }
            for (auto &r : wakeRecv) dispatch(r->handle);
            for (auto &s : wakeSend) {
                s->ok = false;
                dispatch(s->handle);
            }
        }

        bool closed() const noexcept {
            std::lock_guard<std::mutex> lk(this->mu);
            return this->closedFlag;
        }

        std::size_t size() const noexcept {
            std::lock_guard<std::mutex> lk(this->mu);
            return this->buffer.size();
        }

        std::size_t capacity() const noexcept { return this->capacityVal; }

    private:
        struct RecvWaiter {
            std::coroutine_handle<> handle{};
            std::optional<T> slot;
        };

        struct SendWaiter {
            std::coroutine_handle<> handle{};
            T value;
            bool ok = false;
        };

        struct ReceiveAwaiter {
            BoundedChannel *ch;
            std::shared_ptr<RecvWaiter> waiter;

            bool await_ready() const noexcept { return false; }

            bool await_suspend(std::coroutine_handle<> h) {
                std::lock_guard<std::mutex> lk(ch->mu);
                // Recheck under the lock to close the race window.
                if (!ch->buffer.empty()) {
                    waiter->slot.emplace(std::move(ch->buffer.front()));
                    ch->buffer.pop_front();
                    return false;
                }
                if (ch->closedFlag) return false;
                waiter->handle = h;
                ch->receivers.push_back(waiter);
                return true;
            }

            void await_resume() noexcept {
            }
        };

        struct SendAwaiter {
            BoundedChannel *ch;
            std::shared_ptr<SendWaiter> waiter;

            bool await_ready() const noexcept { return false; }

            bool await_suspend(std::coroutine_handle<> h) {
                std::lock_guard<std::mutex> lk(ch->mu);
                if (ch->closedFlag) {
                    waiter->ok = false;
                    return false;
                }
                // Race close: maybe a slot freed since the fast path
                // gave up. Take it if so; this also handles the zero-
                // capacity rendezvous case.
                if (!ch->receivers.empty()) {
                    auto receiver = std::move(ch->receivers.front());
                    ch->receivers.pop_front();
                    receiver->slot.emplace(std::move(waiter->value));
                    dispatch(receiver->handle);
                    waiter->ok = true;
                    return false;
                }
                if (ch->buffer.size() < ch->capacityVal) {
                    ch->buffer.push_back(std::move(waiter->value));
                    waiter->ok = true;
                    return false;
                }
                waiter->handle = h;
                ch->senders.push_back(waiter);
                return true;
            }

            void await_resume() noexcept {
            }
        };

        /**
         * @brief Wake @p h via the Yarn pool rather than inline. Inline
         *        resume would unbound the caller's stack under chained
         *        producer/consumer cascades and would serialise everyone
         *        on the waking thread.
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
        std::deque<std::shared_ptr<RecvWaiter>> receivers;
        std::deque<std::shared_ptr<SendWaiter>> senders;
        const std::size_t capacityVal;
        bool closedFlag = false;
    };

}

#endif // WIRE_BOUNDED_CHANNEL_H
