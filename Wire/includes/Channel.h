//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef WIRE_CHANNEL_H
#define WIRE_CHANNEL_H

#include <coroutine>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

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
                } else {
                    this->buffer.push_back(std::move(value));
                    return true;
                }
            }

            // Hand off directly to the receiver, outside the lock.
            receiver->slot.emplace(std::move(value));
            std::unique_ptr<YarnBall::ITask> ct{
                new YarnBall::detail::CoroutineITask(receiver->handle)};
            YarnBall::Yarn::instance()->run(std::move(ct));
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
            }
            for (auto &w : to_wake) {
                // slot stays nullopt -> receiver returns nullopt.
                std::unique_ptr<YarnBall::ITask> ct{
                    new YarnBall::detail::CoroutineITask(w->handle)};
                YarnBall::Yarn::instance()->run(std::move(ct));
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
        };

        /**
         * @brief Awaiter that registers a Waiter on the channel and parks
         *        the coroutine until @c send or @c close wakes it.
         */
        struct ReceiveAwaiter {
            Channel *ch;
            std::shared_ptr<Waiter> waiter;

            bool await_ready() const noexcept { return false; }

            bool await_suspend(std::coroutine_handle<> h) {
                std::lock_guard<std::mutex> lk(ch->mu);
                // Recheck buffer + closed under the lock to close the race.
                if (!ch->buffer.empty()) {
                    waiter->slot.emplace(std::move(ch->buffer.front()));
                    ch->buffer.pop_front();
                    return false;
                }
                if (ch->closed_) {
                    return false;
                }
                waiter->handle = h;
                ch->waiters.push_back(waiter);
                return true;
            }

            void await_resume() noexcept {}
        };

        mutable std::mutex mu;
        std::deque<T> buffer;
        std::deque<std::shared_ptr<Waiter>> waiters;
        bool closed_ = false;
    };

}

#endif // WIRE_CHANNEL_H
