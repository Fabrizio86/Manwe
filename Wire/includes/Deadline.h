//
// Created by Fabrizio Paino on 2026-10-23.
//
// DeadlineGuard: the deadline behind Channel::receiveFor and
// BoundedChannel::sendFor.
//

#ifndef WIRE_DEADLINE_H
#define WIRE_DEADLINE_H

#include <chrono>
#include <stop_token>

#include "Timeout.h"

namespace Telegraph::detail {

    /**
     * @brief A deadline timer (see @c YarnBall::deadlineToken) that is
     *        withdrawn when the guard goes out of scope, so a call that
     *        finishes early does not leave its timer running until it fires.
     */
    class DeadlineGuard {
    public:
        template<typename Rep, typename Period>
        explicit DeadlineGuard(std::chrono::duration<Rep, Period> timeout)
            : withdraw(), tok(YarnBall::deadlineToken(timeout, this->withdraw.get_token())) {
        }

        DeadlineGuard(const DeadlineGuard &) = delete;
        DeadlineGuard &operator=(const DeadlineGuard &) = delete;

        ~DeadlineGuard() { this->withdraw.request_stop(); }

        /// The token that is stopped when the deadline passes.
        std::stop_token token() const { return this->tok; }

        /// @return @c true if the deadline has passed.
        bool expired() const noexcept { return this->tok.stop_requested(); }

    private:
        std::stop_source withdraw;
        std::stop_token tok;
    };

}

#endif // WIRE_DEADLINE_H
