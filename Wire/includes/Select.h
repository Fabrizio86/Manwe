//
// Created by Fabrizio Paino on 2026-05-16.
//
// channelSelect: pull from the first of N Telegraph::Channel<T> /
// BoundedChannel<T> instances to have a value available, returning
// the channel index + the value (or @c std::nullopt if every channel
// closed). One pull coroutine per channel races for a shared decision; the
// losers are cancelled (see Cancellation.h) and hand back any value they
// obtained concurrently, so a select never consumes a value it drops.
//
// Useful for event-loop-style dispatch where one consumer feeds from
// multiple producer streams without preferring any one.
//

#ifndef WIRE_SELECT_H
#define WIRE_SELECT_H

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

#include "BoundedChannel.h"
#include "Cancellation.h"
#include "Channel.h"
#include "Coroutines.h"
#include "WhenAny.h"

namespace Telegraph {

    /**
     * @struct SelectResult
     * @brief Output of @ref channelSelect: the index of the channel
     *        that produced a value, plus the value itself.
     *        @c value == std::nullopt means every channel was closed
     *        and there were no more values to deliver.
     */
    template<typename T>
    struct SelectResult {
        std::size_t index = 0;
        std::optional<T> value;
    };

    namespace detail {

        /**
         * @brief State shared by the pull coroutines of one select. Has the
         *        shape @c YarnBall::detail::WhenAnyAwaiter expects.
         */
        template<typename T>
        struct SelectState {
            std::mutex mu;
            bool decided = false;
            bool published = false;
            std::size_t winning_index = 0;
            std::optional<T> result;
            std::exception_ptr exception{};
            std::coroutine_handle<> waiter{};
            std::stop_source stop;
        };

        /**
         * @brief Pull one value from @p ch for a select. The first pull to
         *        finish wins. A pull that finishes second holding a value
         *        puts it back at the head of its channel, so a select never
         *        swallows a value it did not return; a pull still parked on
         *        its channel when the select is decided is cancelled and
         *        leaves the channel's waiter queue.
         */
        template<typename T, typename Chan>
        YarnBall::Task<> selectPull(std::shared_ptr<SelectState<T>> st,
                                    std::size_t idx,
                                    std::shared_ptr<Chan> ch) {
            const std::stop_token scope = st->stop.get_token();
            co_await YarnBall::detail::InstallScopeAwaiter{&scope};

            std::optional<T> value;
            std::exception_ptr error;
            bool cancelled = false;
            try {
                value = co_await ch->receive();
            } catch (const YarnBall::CancelledException &) {
                cancelled = true;
                error = std::current_exception();
            } catch (...) {
                error = std::current_exception();
            }

            std::coroutine_handle<> w{};
            bool won = false;
            {
                std::lock_guard<std::mutex> lk(st->mu);
                if (!st->decided) {
                    // A cancelled pull can only be first if the whole select was cancelled from outside.
                    st->decided = true;
                    st->winning_index = idx;
                    st->result = std::move(value);
                    st->exception = error;
                    st->published = true;
                    w = st->waiter;
                    won = true;
                }
            }
            if (!won) {
                if (!cancelled && !error && value) ch->pushFront(std::move(*value));
                co_return;
            }
            st->stop.request_stop();
            if (w) w.resume();
            co_return;
        }

        template<typename T, typename Chan>
        YarnBall::Task<SelectResult<T>> selectImpl(std::vector<std::shared_ptr<Chan>> channels) {
            auto st = std::make_shared<SelectState<T>>();
            YarnBall::detail::TaskPromiseCore *outer = co_await YarnBall::detail::CurrentScopeAwaiter{};
            YarnBall::detail::ScopeLink link;
            link.follow(outer, st->stop);
            for (std::size_t i = 0; i < channels.size(); ++i) {
                YarnBall::coSpawn(selectPull<T, Chan>(st, i, channels[i]));
            }
            co_await YarnBall::detail::WhenAnyAwaiter<SelectState<T>>{st};
            SelectResult<T> out;
            out.index = st->winning_index;
            out.value = std::move(st->result);
            co_return out;
        }

    }

    /**
     * @brief Pull from whichever of @p channels has a value first.
     *        Returns the index + value of the first to fire, or an
     *        empty optional if the winning channel was closed.
     *
     * Single-shot and lossless: the receives that lose are cancelled
     * and removed from their channel's waiter queue, and a value a
     * losing receive obtained in the same instant is returned to the
     * front of its channel, so no value is ever consumed and discarded.
     * If the caller is inside a stopped cancellation scope the select
     * throws @c YarnBall::CancelledException.
     *
     * @note With an empty @p channels vector the select never completes.
     */
    template<typename T>
    YarnBall::Task<SelectResult<T>> channelSelect(
        std::vector<std::shared_ptr<Channel<T>>> channels) {
        return detail::selectImpl<T, Channel<T>>(std::move(channels));
    }

    /**
     * @brief @ref BoundedChannel overload of @ref channelSelect.
     */
    template<typename T>
    YarnBall::Task<SelectResult<T>> channelSelect(
        std::vector<std::shared_ptr<BoundedChannel<T>>> channels) {
        return detail::selectImpl<T, BoundedChannel<T>>(std::move(channels));
    }

}

#endif // WIRE_SELECT_H
