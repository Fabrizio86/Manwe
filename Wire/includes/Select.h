//
// Created by Fabrizio Paino on 2026-05-16.
//
// channelSelect: pull from the first of N Telegraph::Channel<T> /
// BoundedChannel<T> instances to have a value available, returning
// the channel index + the value (or @c std::nullopt once EVERY channel is
// closed and drained). One pull coroutine per channel races for a
// shared decision; a pull that finds its channel closed and drained drops
// out and the others keep racing; the losers are cancelled (see Cancellation.h) and hand
// back any value they obtained concurrently (even to a channel that has
// since closed), so a select never consumes a value it drops.
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
     * @class SelectResult
     * @brief Output of @ref channelSelect: the index of the channel
     *        that produced a value, plus the value itself.
     *        @c value == std::nullopt means every channel is closed and
     *        drained (nothing can ever arrive); @c index is then the channel
     *        whose end was observed last. While any channel is still open,
     *        or closed but holding buffered data, the select does not
     *        complete with nullopt: closed-and-empty channels just drop out
     *        of the race. To notice that one source ended, a caller checks
     *        @c Channel::closed() itself.
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
            std::size_t live = 0; ///< pulls whose channel is not yet known to be closed and drained
            std::size_t outstanding = 0; ///< pulls that have not finished yet, including their put-back of a value
            std::optional<T> result;
            std::exception_ptr exception{};
            std::coroutine_handle<> waiter{};
            std::stop_source stop;
        };

        /**
         * @brief Pull one value from @p ch for a select. The first pull to
         *        finish with a value wins; one that finds its channel closed
         *        and drained drops out unless it is the last live pull (then
         *        the select completes empty). A pull that finishes second holding a value
         *        puts it back at the head of its channel, so a select never
         *        swallows a value it did not return; a pull still parked on
         *        its channel when the select is decided is cancelled and
         *        leaves the channel's waiter queue. The caller is resumed only
         *        once every pull has finished (and so every put-back is done):
         *        otherwise its next select could take a later value of a
         *        channel before the earlier one is back at the head, or find
         *        every channel closed and empty while a value is still in
         *        flight to one of them.
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

            bool won = false;
            bool dropped = false;
            {
                std::lock_guard<std::mutex> lk(st->mu);
                if (!st->decided && !cancelled && !error && !value && --st->live > 0) {
                    dropped = true; // closed and drained, but another channel can still deliver
                } else if (!st->decided) {
                    // A cancelled pull can only be first if the whole select was cancelled from outside.
                    st->decided = true;
                    st->winning_index = idx;
                    st->result = std::move(value);
                    st->exception = error;
                    won = true;
                }
            }
            if (won) {
                st->stop.request_stop(); // release the pulls still parked on their channels right away
            } else if (!dropped && !cancelled && !error && value) {
                try {
                    ch->pushFront(std::move(*value));
                } catch (...) {
                    // Out of memory: the value cannot be returned, but the select must still complete.
                }
            }
            std::coroutine_handle<> w{};
            {
                std::lock_guard<std::mutex> lk(st->mu);
                if (--st->outstanding == 0 && st->decided) {
                    st->published = true;
                    w = std::exchange(st->waiter, {});
                }
            }
            if (w) w.resume();
            co_return;
        }

        template<typename T, typename Chan>
        YarnBall::Task<SelectResult<T>> selectImpl(std::vector<std::shared_ptr<Chan>> channels) {
            auto st = std::make_shared<SelectState<T>>();
            YarnBall::detail::TaskPromiseCore *outer = co_await YarnBall::detail::CurrentScopeAwaiter{};
            YarnBall::detail::ScopeLink link;
            link.follow(outer, st->stop);
            if (channels.empty()) co_return SelectResult<T>{};
            const std::size_t n = channels.size();
            st->live = n;
            st->outstanding = n;
            // Start the pulls at a rotating offset, so when several channels are ready at once none of them is
            // favoured by its position in the vector.
            static std::atomic<std::size_t> rotor{0};
            const std::size_t start = rotor.fetch_add(1, std::memory_order_relaxed) % n;
            for (std::size_t k = 0; k < n; ++k) {
                const std::size_t i = (start + k) % n;
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
     *        Returns the index + value of the first to deliver. A closed,
     *        drained channel is skipped; only when all of them are does the
     *        select complete, with an empty optional (see @ref SelectResult).
     *        A closed channel that still holds data delivers it first.
     *
     * Single-shot and lossless: the receives that lose are cancelled
     * and removed from their channel's waiter queue, and a value a
     * losing receive obtained in the same instant is returned to the
     * front of its channel, so no value is ever consumed and discarded.
     * If the caller is inside a stopped cancellation scope the select
     * throws @c YarnBall::CancelledException.
     *
     * @note With an empty @p channels vector the select completes at once
     *       with index 0 and an empty value.
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
