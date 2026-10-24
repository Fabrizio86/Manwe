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
#include <chrono>
#include <coroutine>
#include <cstddef>
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
     * Timeouts: use @ref sendFor / @ref receiveFor, not
     * @c YarnBall::withTimeout around @c send / @c receive. @c withTimeout
     * races the call against a timer and discards the loser's result, so a
     * receive can lose the value it was handed when the timer wins, and a
     * send can deliver while its caller sees a timeout (a retry then
     * duplicates the message). The @c ...For helpers cancel the parked call
     * instead.
     *
     * A task returned by @c send / @c receive reads the channel when it
     * first runs: await it while the channel is alive. (A call that is
     * already parked is safe against the channel being destroyed, see
     * below.)
     *
     * If moving a @c T throws while a value is being handed over, the call
     * that was moving it throws and the waiter it was serving stays queued.
     *
     * The channel's state is shared with the parked waiters, so a waiter
     * frame that outlives the channel (woken by the destructor's @c close
     * and not destroyed yet) never touches freed memory.
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
        explicit BoundedChannel(std::size_t capacity)
            : core(std::make_shared<Core>(capacity)) {
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
            // Works on the channel's core directly: the shared_ptr is copied only when about to park.
            Core &c = *this->core;
            std::shared_ptr<Core> held;
            {
                std::shared_ptr<RecvWaiter> receiver;
                {
                    std::lock_guard<std::mutex> lk(c.mu);
                    if (c.closedFlag) co_return false;
                    if (!c.receivers.empty()) {
                        // Hand over first, then dequeue: if moving the value throws, the receiver is still parked.
                        // Under the lock, so a receiver destroyed before the wake runs can take the value back.
                        c.receivers.front()->slot.emplace(std::move(value));
                        receiver = c.popReceiverLocked();
                    } else if (c.buffer.size() < c.capacityVal) {
                        c.buffer.push_back(std::move(value));
                        co_return true;
                    } else {
                        held = this->core;
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
            // The awaiter owns the waiter and hands back the outcome, so the frame holds no second reference to it
            // (every byte of a coroutine frame is paid for on every call, parked or not). `held` keeps the core alive.
            co_return co_await SendAwaiter{held.get(), std::move(value)};
        }

        /**
         * @brief Receive a value. Suspends if the buffer is empty and
         *        no sender is waiting.
         * @return The value, or @c std::nullopt if the channel was
         *         closed and is now drained.
         */
        YarnBall::Task<std::optional<T>> receive() {
            // Fast path: buffer non-empty, a sender is parked, or channel already closed.
            // Works on the channel's core directly: the shared_ptr is copied only when about to park.
            Core &c = *this->core;
            std::shared_ptr<Core> held;
            std::optional<T> got;
            std::coroutine_handle<> wake{};
            bool drainedClosed = false;
            {
                std::lock_guard<std::mutex> lk(c.mu);
                if (!c.buffer.empty()) {
                    got.emplace(std::move(c.buffer.front()));
                    c.buffer.pop_front();
                    // Pull one parked sender into the freed slot, if there is a free slot (a pushFront can leave the
                    // buffer at or over capacity, and a rendezvous channel has no slot at all). The sender's value goes
                    // into the buffer; the sender is woken after the unlock.
                    if (c.buffer.size() < c.capacityVal && !c.senders.empty()) {
                        c.buffer.push_back(std::move(*c.senders.front()->value));
                        wake = c.popSenderLocked()->takeOk();
                    }
                } else if (!c.senders.empty()) {
                    // Rendezvous (capacity 0): a parked sender hands its value
                    // straight to us, since the buffer can never hold it.
                    got.emplace(std::move(*c.senders.front()->value));
                    wake = c.popSenderLocked()->takeOk();
                } else if (c.closedFlag) {
                    drainedClosed = true;
                } else {
                    held = this->core;
                }
            }
            // Never resume a coroutine under the channel mutex: dispatch() may fall back to an inline resume.
            if (wake) dispatch(wake);
            if (got) co_return std::move(got);
            if (drainedClosed) co_return std::nullopt;

            // Slow path: buffer empty, no sender waiting.
            co_return co_await ReceiveAwaiter{held.get()};
        }

        /**
         * @brief @ref send with a deadline. Exactly once: either the value
         *        was delivered (the result is @c true, or @c false if the
         *        channel closed), or the deadline passed first, the parked
         *        send left the queue, the value was NOT delivered and is
         *        dropped (copy it first if the caller wants to retry). A
         *        send that completes at the same instant as the deadline
         *        reports success, so a retry never duplicates a message.
         * @throws YarnBall::TimeoutException if @p timeout elapsed first;
         *         @c YarnBall::CancelledException if the caller's own scope
         *         was stopped.
         */
        template<typename Rep, typename Period>
        YarnBall::Task<bool> sendFor(T value, std::chrono::duration<Rep, Period> timeout) {
            detail::DeadlineGuard guard(timeout);
            try {
                co_return co_await YarnBall::withCancellation(guard.token(), this->send(std::move(value)));
            } catch (const YarnBall::CancelledException &) {
                if (guard.expired()) throw YarnBall::TimeoutException("BoundedChannel::sendFor");
                throw;
            }
        }

        /**
         * @brief @ref receive with a deadline. Lossless: a receive that
         *        times out leaves the queue without taking a value, and a
         *        value handed over at the same instant is returned (or put
         *        back).
         * @throws YarnBall::TimeoutException if @p timeout elapsed first;
         *         @c YarnBall::CancelledException if the caller's own scope
         *         was stopped.
         */
        template<typename Rep, typename Period>
        YarnBall::Task<std::optional<T>> receiveFor(std::chrono::duration<Rep, Period> timeout) {
            detail::DeadlineGuard guard(timeout);
            try {
                co_return co_await YarnBall::withCancellation(guard.token(), this->receive());
            } catch (const YarnBall::CancelledException &) {
                if (guard.expired()) throw YarnBall::TimeoutException("BoundedChannel::receiveFor");
                throw;
            }
        }

        /**
         * @brief Put @p value back at the head of the channel, ahead of
         *        anything buffered: it goes straight to the oldest parked
         *        receiver if there is one. The buffer may exceed the
         *        capacity by one per value pushed back (including on a
         *        rendezvous channel, where it holds the value until a
         *        receiver takes it); parked senders are released only once
         *        it has drained below the capacity again. On a closed
         *        channel the value goes to the buffer, which receivers
         *        still drain. Always returns @c true.
         */
        bool pushFront(T value) {
            return pushFrontCore(*this->core, std::move(value));
        }

        /**
         * @brief Mark the channel closed. Wakes every pending receiver
         *        with @c nullopt and every pending sender with @c false.
         *        Idempotent.
         */
        void close() {
            Core &c = *this->core;
            std::deque<std::shared_ptr<RecvWaiter>> wakeRecv;
            std::deque<std::shared_ptr<SendWaiter>> wakeSend;
            {
                std::lock_guard<std::mutex> lk(c.mu);
                if (c.closedFlag) return;
                c.closedFlag = true;
                wakeRecv = std::move(c.receivers);
                c.receivers.clear();
                wakeSend = std::move(c.senders);
                c.senders.clear();
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
            std::lock_guard<std::mutex> lk(this->core->mu);
            return this->core->closedFlag;
        }

        std::size_t size() const noexcept {
            std::lock_guard<std::mutex> lk(this->core->mu);
            return this->core->buffer.size();
        }

        std::size_t capacity() const noexcept { return this->core->capacityVal; }

        /// @return Number of receivers currently parked (snapshot).
        std::size_t receiverCount() const noexcept {
            std::lock_guard<std::mutex> lk(this->core->mu);
            return this->core->receivers.size();
        }

        /// @return Number of senders currently parked (snapshot).
        std::size_t senderCount() const noexcept {
            std::lock_guard<std::mutex> lk(this->core->mu);
            return this->core->senders.size();
        }

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

            /// Mark the send delivered; returns the handle to wake.
            std::coroutine_handle<> takeOk() noexcept {
                this->ok = true;
                return this->handle;
            }
        };

        /**
         * @brief Everything the channel guards. Shared with every parked
         *        waiter, so a waiter frame destroyed after the channel is
         *        gone still finds a live mutex and queues.
         */
        struct Core {
            explicit Core(std::size_t capacity) noexcept : capacityVal(capacity) {
            }

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

            mutable std::mutex mu;
            std::deque<T> buffer;
            std::deque<std::shared_ptr<RecvWaiter>> receivers;
            std::deque<std::shared_ptr<SendWaiter>> senders;
            const std::size_t capacityVal;
            bool closedFlag = false;
        };

        static bool pushFrontCore(Core &c, T value) {
            std::shared_ptr<RecvWaiter> receiver;
            {
                std::lock_guard<std::mutex> lk(c.mu);
                if (!c.closedFlag && !c.receivers.empty()) {
                    c.receivers.front()->slot.emplace(std::move(value));
                    receiver = c.popReceiverLocked();
                } else {
                    c.buffer.push_front(std::move(value));
                    return true;
                }
            }
            dispatch(receiver->handle);
            return true;
        }

        /**
         * @brief Common cancel/destroy handling for a parked waiter of
         *        either kind (cancel protocol as in
         *        @c YarnBall::detail::ParkBase). @p Queue is the member
         *        deque the waiter sits in.
         */
        template<typename Waiter, typename Queue, typename Self>
        struct ParkedWait {
            // Not owning: the coroutine frame that awaits this holds a reference to the core for the awaiter's whole life.
            Core *ch;
            std::shared_ptr<Waiter> waiter;
            bool earlyCancel = false; // guarded by ch->mu
            bool cancelled = false;
            YarnBall::detail::WaitProxy proxy;
            YarnBall::detail::CancelBinding bind;

            ParkedWait(Core *c, std::shared_ptr<Waiter> w) noexcept
                : ch(c), waiter(std::move(w)) {
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
                auto &q = Self::queueOf(*this->ch);
                auto it = std::find(q.begin(), q.end(), this->waiter);
                if (it != q.end()) q.erase(it);
                this->waiter->queued.store(false, std::memory_order_release);
            }

            void park(std::coroutine_handle<> h) {
                this->waiter->handle = this->proxy.publish(h);
                this->waiter->queued.store(true, std::memory_order_release);
                Self::queueOf(*this->ch).push_back(this->waiter);
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

            explicit ReceiveAwaiter(Core *c)
                : Base(c, std::make_shared<RecvWaiter>()) {
            }

            static std::deque<std::shared_ptr<RecvWaiter>> &queueOf(Core &c) noexcept { return c.receivers; }
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
                if (orphan) pushFrontCore(*this->ch, std::move(*orphan));
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
                            if (this->ch->buffer.size() < this->ch->capacityVal && !this->ch->senders.empty()) {
                                this->ch->buffer.push_back(std::move(*this->ch->senders.front()->value));
                                wake = this->ch->popSenderLocked()->takeOk();
                            }
                        } else if (!this->ch->senders.empty()) {
                            this->waiter->slot.emplace(std::move(*this->ch->senders.front()->value));
                            wake = this->ch->popSenderLocked()->takeOk();
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

            std::optional<T> await_resume() {
                this->throwIfCancelled();
                std::optional<T> result = std::move(this->waiter->slot);
                this->waiter->slot.reset();
                return result;
            }
        };

        struct SendAwaiter : ParkedWait<SendWaiter, std::deque<std::shared_ptr<SendWaiter>>, SendAwaiter> {
            using Base = ParkedWait<SendWaiter, std::deque<std::shared_ptr<SendWaiter>>, SendAwaiter>;

            SendAwaiter(Core *c, T &&value)
                : Base(c, std::make_shared<SendWaiter>()) {
                this->waiter->value.emplace(std::move(value));
            }

            static std::deque<std::shared_ptr<SendWaiter>> &queueOf(Core &c) noexcept { return c.senders; }
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
                            this->ch->receivers.front()->slot.emplace(std::move(*this->waiter->value));
                            wake = this->ch->popReceiverLocked()->handle;
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

            bool await_resume() {
                this->throwIfCancelled();
                return this->waiter->ok;
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

        std::shared_ptr<Core> core;
    };

}

#endif // WIRE_BOUNDED_CHANNEL_H
