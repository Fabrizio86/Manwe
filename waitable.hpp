#ifndef WAITABLE_H
#define WAITABLE_H

#include "iwaitable.hpp"
#include <mutex>

namespace YarnBall {

// Class representing the waitable object.
// If you need to wait for an operation to complete,
// this will implement the wait pattern for the threadpool execution.
class Waitable final : public IWaitable {
public:
    Waitable(Task task);
    ~Waitable();

    void wait();
    void run();
    void Done();
    void exception(std::exception_ptr ex);
    std::exception_ptr getException();

private:
    bool done;
    Task task;
    std::mutex mu;
    std::condition_variable cv;
    std::exception_ptr ex;
};

}

#endif // WAITABLE_H
