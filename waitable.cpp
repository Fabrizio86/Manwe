#include "waitable.hpp"
#include <thread>

namespace YarnBall {

Waitable::Waitable(Task task) : done{false}, task{task} { }

Waitable::~Waitable() { }

void Waitable::run() {
    this->task();
    this->Done();
}

void Waitable::Done() {
    if(this->done != true) {
        this->done = true;
        this->cv.notify_all();
    }
}

void Waitable::exception(std::exception_ptr ex) {
    this->ex = ex;
    this->Done();
}

std::exception_ptr Waitable::getException() {
    return  ex;
}

void Waitable::wait() {
    std::this_thread::yield();
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [this]{ return this->done; });
}

IWaitable::~IWaitable() { }

}
