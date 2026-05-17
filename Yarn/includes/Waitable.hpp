//
// Created by Fabrizio Paino on 2022-01-16.
//

#ifndef YARN_WAITABLE_HPP
#define YARN_WAITABLE_HPP

#include <condition_variable>
#include <mutex>
#include <string>

#include "ITask.hpp"
#include "IWaitable.hpp"

namespace YarnBall {

    /**
     * @class Waitable
     * @brief Default IWaitable implementation. Subclasses override @c operation
     *        with the work to be performed; the base class handles the
     *        completion / failure plumbing.
     *
     * Thread-safety: @c notifyDone and @c exception both publish their state
     * under @c mu before notifying the condition variable, so observers of
     * @c hasFailed / @c errorMessage after a successful @c wait see a
     * consistent view.
     */
    class Waitable : public IWaitable {
    public:
        Waitable() = default;
        ~Waitable() = default;

        /**
         * @brief Block until @c operation has returned or thrown.
         */
        void wait() override;

        /**
         * @brief @c true if @c operation terminated with an exception.
         */
        bool hasFailed() override;

        /**
         * @brief Human-readable error string, populated by @ref exception.
         */
        std::string errorMessage() override;

        /**
         * @brief Override point: the work to be executed on a worker thread.
         *        The base implementation is a no-op.
         */
        virtual void operation();

    private:
        /**
         * @brief Invokes @c operation and signals completion.
         */
        void run() override;

        /**
         * @brief Marks the waitable as done and wakes all observers.
         */
        void notifyDone();

        /**
         * @brief Forwards the worker's caught exception, sets failure flags,
         *        and signals completion.
         */
        void exception(std::exception_ptr exception) override;

        /**
         * @brief Completion flag; protected by @ref mu.
         */
        bool done = false;

        /**
         * @brief Failure flag; protected by @ref mu.
         */
        bool failed = false;

        /**
         * @brief Lock for @ref done, @ref failed, @ref error, and the
         *        condition variable handshake.
         */
        std::mutex mu;

        /**
         * @brief Condition variable signalled by @ref notifyDone and
         *        @ref exception.
         */
        std::condition_variable cv;

        /**
         * @brief Human-readable error string. Protected by @ref mu.
         */
        std::string error;
    };
}

#endif //YARN_WAITABLE_HPP
