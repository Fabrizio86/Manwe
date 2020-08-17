//
// Created by Fabrizio Paino on 2020-08-26.
//

#ifndef YARNS_AWAITABLE_H
#define YARNS_AWAITABLE_H

#include "yarns.hpp"
#include "system.hpp"

#include <utility>

namespace YarnBall {

    // Class representing the awaitable object.
    // If you need to wait for an operation to complete,
    // this will implement the wait pattern for the thread pool execution.
    class Awaitable final : public YarnBall::IAwaitable, public YarnBall::ITask {
    public:
        /// \brief Constructor accepting instance of Task
        /// \param task
        explicit Awaitable(YarnBall::Task task);

        /// \brief Default destructor
        ~Awaitable() override = default;

        /// \brief tells the caller to wait for the task to execute
        void wait() override;

        /// \brief executes the task
        void run() override;

        /// \brief tells if the task has completed
        void Done();

        /// \brief captures the exception
        void exception(std::exception_ptr e) override;

        /// \brief returns the exception thrown, if any or nullptr
        std::exception_ptr getException() override;

    private:
        bool done;
        YarnBall::Task task;
        std::mutex mu;
        std::condition_variable cv;
        std::exception_ptr ex;
    };

}

#endif //YARNS_AWAITABLE_H
