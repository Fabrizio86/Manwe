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

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "Cancellation.h"
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
     * Parked @c send and @c receive calls are cancellable (see
     * @c Cancellation.h): inside a stopped cancellation scope they leave
     * their queue and throw @c YarnBall::CancelledException (a cancelled
     * send's value is dropped), and a frame destroyed while parked also
     * leaves its queue (a value already handed to it goes back to the channel). @ref pushFront returns a value to the head of the
     * channel (see @c channelSelect).
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
                        receiver = this->popReceiverLocked();
                        // Under the lock, so a receiver destroyed before the wake runs can take the value back.
                        receiver->slot.emplace(std::move(value));
                    } else if (this->buffer.size() < this->capacityVal) {
                        this->buffer.push_back(std::move(value));
                        co_return true;
                    }
                }
                if (receiver) {
                    dispatch(receiver->handle);
                    co_return true;
                }
            }

            // Slow path: buffer full, no receiver waiting. Park the
            // sender on the senders queue; a receiver will wake us
            // once a slot frees up (or close() will fail the send).
            auto sender = std::make_shared<SendWaiter>();
            sender->value.emplace(std::move(value));
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
            // Fast path: buffer non-empty, a sender is parked, or channel already closed.
            std::optional<T> got;
            std::coroutine_handle<> wake{};
            bool drainedClosed = false;
            {
                std::lock_guard<std::mutex> lk(this->mu);
                if (!this->buffer.empty()) {
                    got.emplace(std::move(this->buffer.front()));
                    this->buffer.pop_front();
                    // Pull one parked sender into the now-empty slot if
                    // there is one queued. The sender's value goes into
                    // the buffer; the sender is woken after the unlock.
                    if (!this->senders.empty()) {
                        auto sender = this->popSenderLocked();
                        this->buffer.push_back(std::move(*sender->value));
                        sender->ok = true;
                        wake = sender->handle;
                    }
                } else if (!this->senders.empty()) {
                    // Rendezvous (capacity 0): a parked sender hands its value
                    // straight to us, since the buffer can never hold it.
                    auto sender = this->popSenderLocked();
                    got.emplace(std::move(*sender->value));
                    sender->ok = true;
                    wake = sender->handle;
                } else if (this->closedFlag) {
                    drainedClosed = true;
                }
            }
            // Never resume a coroutine under the channel mutex: dispatch() may fall back to an inline resume.
            if (wake) dispatch(wake);
            if (got) co_return std::move(got);
            if (drainedClosed) co_return std::nullopt;

            // Slow path: buffer empty, no sender waiting.
            auto receiver = std::make_shared<RecvWaiter>();
            co_await ReceiveAwaiter{this, receiver};
            if (!receiver->slot.has_value()) co_return std::nullopt;
            co_return std::move(*receiver->slot);
        }

        /**
         * @brief Put @p value back at the head of the channel, ahead of
         *        anything buffered (it may briefly exceed the capacity by
         *        one): it goes straight to the oldest parked receiver if
         *        there is one. Returns @c false (dropping the value) if the
         *        channel is closed.
         */
        bool pushFront(T value) {
            std::shared_ptr<RecvWaiter> receiver;
            {
                std::lock_guard<std::mutex> lk(this->mu);
                if (this->closedFlag) return false;
                if (!this->receivers.empty()) {
                    receiver = this->popReceiverLocked();
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
                for (auto &r : wakeRecv) r->queued.store(false, std::memory_order_release);
                for (auto &w : wakeSend) w->queued.store(false, std::memory_order_release);
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
            std::atomic<bool> queued{false};
        };

        struct SendWaiter {
            std::coroutine_handle<> handle{};
            // std::optional<T>, not a plain T: this struct is default-
            // constructed via std::make_shared<SendWaiter>() in send()
            // before the value is known, so a bare T member would
            // silently require T to be default-constructible --
            // contradicting this class's own documented "T must be
            // move-constructible" contract (and its sibling Channel<T>,
            // which only ever wraps T in std::optional and has no such
            // requirement). optional<T> starts empty regardless of T,
            // matching the promised contract exactly.
            std::optional<T> value;
            bool ok = false;
            std::atomic<bool> queued{false};
        };

        std::shared_ptr<RecvWaiter> popReceiverLocked() {
            auto r = std::move(this->receivers.front());
            this->receivers.pop_front();
            r->queued.store(false, std::memory_order_release);
            return r;
        }

        std::shared_ptr<SendWaiter> popSenderLocked() {
            auto w = std::move(this->senders.front());
            this->senders.pop_front();
            w->queued.store(false, std::memory_order_release);
            return w;
        }

        /**
         * @brief Common cancel/destroy handling for a parked waiter of
         *        either kind (cancel protocol as in
         *        @c YarnBall::detail::ParkBase). @p Queue is the member
         *        deque the waiter sits in.
         */
        template<typename Waiter, typename Queue, typename Self>
        struct ParkedWait {
            BoundedChannel *ch;
            std::shared_ptr<Waiter> waiter;
            Queue BoundedChannel::*queue;
            bool earlyCancel = false; // guarded by ch->mu
            bool cancelled = false;
            YarnBall::detail::WaitProxy proxy;
            YarnBall::detail::CancelBinding bind;

            ParkedWait(BoundedChannel *c, std::shared_ptr<Waiter> w, Queue BoundedChannel::*q) noexcept
                : ch(c), waiter(std::move(w)), queue(q) {
            }
            ParkedWait(const ParkedWait &) = delete;
            ParkedWait &operator=(const ParkedWait &) = delete;

            void abandon() noexcept {
                this->bind.disarm();
                if (!this->proxy) return;
                bool removed = false;
                if (this->waiter->queued.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> lk(this->ch->mu);
                    if (this->waiter->queued.load(std::memory_order_relaxed)) {
                        this->unlinkLocked();
                        removed = true;
                    }
                }
                // A wake already in flight is dropped; a value it carried goes with the destroyed waiter.
                this->proxy.retire(removed);
            }

            void unlinkLocked() noexcept {
                auto &q = this->ch->*(this->queue);
                auto it = std::find(q.begin(), q.end(), this->waiter);
                if (it != q.end()) q.erase(it);
                this->waiter->queued.store(false, std::memory_order_release);
            }

            void park(std::coroutine_handle<> h) {
                this->waiter->handle = this->proxy.publish(h);
                this->waiter->queued.store(true, std::memory_order_release);
                (this->ch->*(this->queue)).push_back(this->waiter);
            }

            static void cancelThunk(void *p) noexcept {
                auto *self = static_cast<Self *>(p);
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

            void throwIfCancelled() const {
                if (this->cancelled) throw YarnBall::CancelledException{};
            }
        };

        struct ReceiveAwaiter : ParkedWait<RecvWaiter, std::deque<std::shared_ptr<RecvWaiter>>, ReceiveAwaiter> {
            using Base = ParkedWait<RecvWaiter, std::deque<std::shared_ptr<RecvWaiter>>, ReceiveAwaiter>;

            ReceiveAwaiter(BoundedChannel *c, std::shared_ptr<RecvWaiter> w) noexcept
                : Base(c, std::move(w), &BoundedChannel::receivers) {
            }
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
                    // carried is put back instead of dying with the receiver. It was stored under this lock.
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
                    std::coroutine_handle<> wake{};
                    {
                        std::lock_guard<std::mutex> lk(this->ch->mu);
                        if (this->earlyCancel) {
                            this->cancelled = true;
                            return false;
                        }
                        // Recheck under the lock to close the race window.
                        if (!this->ch->buffer.empty()) {
                            this->waiter->slot.emplace(std::move(this->ch->buffer.front()));
                            this->ch->buffer.pop_front();
                            if (!this->ch->senders.empty()) {
                                auto sender = this->ch->popSenderLocked();
                                this->ch->buffer.push_back(std::move(*sender->value));
                                sender->ok = true;
                                wake = sender->handle;
                            }
                        } else if (!this->ch->senders.empty()) {
                            auto sender = this->ch->popSenderLocked();
                            this->waiter->slot.emplace(std::move(*sender->value));
                            sender->ok = true;
                            wake = sender->handle;
                        } else if (this->ch->closedFlag) {
                            return false;
                        } else if (!core || armed) {
                            this->park(h);
                            return true;
                        } else if (core->isCancellationRequested()) {
                            this->cancelled = true;
                            return false;
                        }
                    }
                    if (wake) {
                        dispatch(wake);
                        return false;
                    }
                    if (this->waiter->slot.has_value()) return false;
                    this->bind.arm(core, &Base::cancelThunk, this);
                    armed = true;
                }
            }

            void await_resume() { this->throwIfCancelled(); }
        };

        struct SendAwaiter : ParkedWait<SendWaiter, std::deque<std::shared_ptr<SendWaiter>>, SendAwaiter> {
            using Base = ParkedWait<SendWaiter, std::deque<std::shared_ptr<SendWaiter>>, SendAwaiter>;

            SendAwaiter(BoundedChannel *c, std::shared_ptr<SendWaiter> w) noexcept
                : Base(c, std::move(w), &BoundedChannel::senders) {
            }
            ~SendAwaiter() { this->abandon(); }

            bool await_ready() const noexcept { return false; }

            template<typename Promise>
            bool await_suspend(std::coroutine_handle<Promise> h) {
                YarnBall::detail::TaskPromiseCore *core = YarnBall::detail::CancelBinding::ambient(h);
                bool armed = false;
                for (;;) {
                    std::coroutine_handle<> wake{};
                    bool delivered = false;
                    {
                        std::lock_guard<std::mutex> lk(this->ch->mu);
                        if (this->earlyCancel) {
                            this->cancelled = true;
                            return false;
                        }
                        if (this->ch->closedFlag) {
                            this->waiter->ok = false;
                            return false;
                        }
                        // Race close: maybe a slot freed since the fast path
                        // gave up. Take it if so; this also handles the zero-
                        // capacity rendezvous case.
                        if (!this->ch->receivers.empty()) {
                            auto receiver = this->ch->popReceiverLocked();
                            receiver->slot.emplace(std::move(*this->waiter->value));
                            wake = receiver->handle;
                            delivered = true;
                        } else if (this->ch->buffer.size() < this->ch->capacityVal) {
                            this->ch->buffer.push_back(std::move(*this->waiter->value));
                            delivered = true;
                        } else if (!core || armed) {
                            this->park(h);
                            return true;
                        } else if (core->isCancellationRequested()) {
                            this->cancelled = true;
                            return false;
                        }
                    }
                    if (delivered) {
                        this->waiter->ok = true;
                        if (wake) dispatch(wake);
                        return false;
                    }
                    this->bind.arm(core, &Base::cancelThunk, this);
                    armed = true;
                }
            }

            void await_resume() { this->throwIfCancelled(); }
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
