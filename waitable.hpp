#ifndef WAITABLE_H
#define WAITABLE_H

#include "iwaitable.hpp"
#include <mutex>

namespace YarnBall {

// Class representing the awaitable object.
// If you need to wait for an operation to complete,
// this will implement the wait pattern for the thread pool execution.
    class Waitable final : public IWaitable {
    public:
        explicit Waitable(Task task);

        ~Waitable() override;

        void wait() override;

        void run() override;

        void Done();

        void exception(std::exception_ptr ex) override;

        std::exception_ptr getException() override;

    private:
        bool done;
        Task task;
        std::mutex mu;
        std::condition_variable cv;
        std::exception_ptr ex;
    };

}

#endif // WAITABLE_H
