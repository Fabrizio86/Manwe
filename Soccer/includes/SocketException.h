//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef SOCCER_SOCKETEXCEPTION_H
#define SOCCER_SOCKETEXCEPTION_H

#include <exception>
#include <string>
#include <utility>

namespace Soccer {

    /**
     * @class SocketException
     * @brief Error carrier for Soccer's networking primitives. Holds the
     *        @c errno value at the point of failure plus a human-readable
     *        message that names the failing syscall and context.
     *
     * Soccer's coroutine APIs throw this from @c await_resume on any
     * unrecoverable failure (the syscall path having already exhausted
     * EAGAIN/EINTR retries).
     */
    class SocketException : public std::exception {
    public:
        /**
         * @brief Construct with a free-form message; @c errorCode defaults
         *        to zero for cases where there is no syscall errno to report.
         */
        explicit SocketException(std::string message, int errorCode = 0)
            : msg(std::move(message)), code(errorCode) {
        }

        /**
         * @return The human-readable message.
         */
        const char *what() const noexcept override { return this->msg.c_str(); }

        /**
         * @return The underlying @c errno (or zero if none).
         */
        int errorCode() const noexcept { return this->code; }

    private:
        std::string msg;
        int code;
    };

}

#endif // SOCCER_SOCKETEXCEPTION_H
