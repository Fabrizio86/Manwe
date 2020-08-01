#include "waitable.hpp"
#include <thread>
#include <utility>

namespace YarnBall {

    Waitable::Waitable(Task task) : done{false}, task{std::move(task)} {}

    Waitable::~Waitable() = default;

    void Waitable::run() {
        this->task();
        this->Done();
    }

    void Waitable::Done() {
        if (!this->done) {
            this->done = true;
            this->cv.notify_all();
        }
    }

    void Waitable::exception(std::exception_ptr ex) {
        this->ex = ex;
        this->Done();
    }

    std::exception_ptr Waitable::getException() {
        return ex;
    }

    void Waitable::wait() {
        std::this_thread::yield();
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this] { return this->done; });
    }
}
