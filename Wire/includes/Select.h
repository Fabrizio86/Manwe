//
// Created by Fabrizio Paino on 2026-05-16.
//
// channelSelect: pull from the first of N Telegraph::Channel<T> /
// BoundedChannel<T> instances to have a value available, returning
// the channel index + the value (or @c std::nullopt if every channel
// closed). Built on top of whenAny over receive() coroutines.
//
// Useful for event-loop-style dispatch where one consumer feeds from
// multiple producer streams without preferring any one.
//

#ifndef WIRE_SELECT_H
#define WIRE_SELECT_H

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "BoundedChannel.h"
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
         * @brief Pull one value from @p ch, storing it into @p slot.
         *        Empty optional means the channel was closed.
         */
        template<typename T>
        YarnBall::Task<std::optional<T>> selectPullChannel(
            std::shared_ptr<Channel<T>> ch) {
            co_return co_await ch->receive();
        }

        /**
         * @brief BoundedChannel variant.
         */
        template<typename T>
        YarnBall::Task<std::optional<T>> selectPullBounded(
            std::shared_ptr<BoundedChannel<T>> ch) {
            co_return co_await ch->receive();
        }
    }

    /**
     * @brief Pull from whichever of @p channels has a value first.
     *        Returns the index + value of the first to fire, or an
     *        empty optional if the winning channel was closed.
     *
     * @note Losing receive coroutines stay parked on their channels
     *       until either (a) their channel gets a value (which then
     *       sits unconsumed -- you must drain it later), or (b) the
     *       channel is closed. This is a single-shot select; do not
     *       use it as a hot loop without understanding the back-
     *       pressure implications.
     */
    template<typename T>
    YarnBall::Task<SelectResult<T>> channelSelect(
        std::vector<std::shared_ptr<Channel<T>>> channels) {
        std::vector<YarnBall::Task<std::optional<T>>> pulls;
        pulls.reserve(channels.size());
        for (auto &ch : channels) {
            pulls.push_back(detail::selectPullChannel<T>(ch));
        }
        auto winner = co_await YarnBall::whenAny(std::move(pulls));
        SelectResult<T> out;
        out.index = winner.index;
        out.value = std::move(winner.value);
        co_return out;
    }

    /**
     * @brief @ref BoundedChannel overload of @ref channelSelect.
     */
    template<typename T>
    YarnBall::Task<SelectResult<T>> channelSelect(
        std::vector<std::shared_ptr<BoundedChannel<T>>> channels) {
        std::vector<YarnBall::Task<std::optional<T>>> pulls;
        pulls.reserve(channels.size());
        for (auto &ch : channels) {
            pulls.push_back(detail::selectPullBounded<T>(ch));
        }
        auto winner = co_await YarnBall::whenAny(std::move(pulls));
        SelectResult<T> out;
        out.index = winner.index;
        out.value = std::move(winner.value);
        co_return out;
    }

}

#endif // WIRE_SELECT_H
