//
// Created by Fabrizio Paino on 2020-08-26.
//

#include "Awaitable.h"

namespace YarnBall {

    Awaitable::Awaitable(YarnBall::Task task)  : done{false}, task{task} { }

    void Awaitable::wait() {
        std::this_thread::yield();
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&done = this->done] { return done; });
    }

    void Awaitable::run() {
        this->task();
        this->Done();
    }

    void Awaitable::Done() {
        if (!this->done) {
            this->done = true;
            this->cv.notify_all();
        }
    }

    void Awaitable::exception(std::exception_ptr e) {
        this->ex = e;
        this->Done();
    }

    std::exception_ptr Awaitable::getException() {
        return this->ex;
    }
}
