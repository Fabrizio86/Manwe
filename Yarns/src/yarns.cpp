#include "yarns.hpp"

#include <utility>
#include "system.hpp"

namespace YarnBall {
    Yarns *Yarns::instance() {
        static Yarns instance;
        return &instance;
    }

    Yarns::Yarns() {
        // create all the work threads
        for (uint i = 0; i < this->limits.getThreadNumbers(); ++i) {
            auto newFiber = std::make_shared<Fiber>(this->limits.getWorkQueueThreshold());
            this->fibers.push_back(newFiber);
        }

        // create all the background services tasks
        for (uint i = 0; i < this->limits.getAsyncWorkQueueThreshold(); ++i) {
            auto newAsyncFiber = std::make_shared<Fiber>(this->limits.getAsyncWorkQueueThreshold());
            newAsyncFiber->MarkAsync();
            this->asyncFibers.push_back(newAsyncFiber);
        }
    }

    const Limiter* Yarns::getLimits() const {
        return &this->limits;
    }

    size_t Yarns::fiberSize() const {
        return this->fibers.size();
    }

    size_t Yarns::aFiberSize() const {
        return this->asyncFibers.size();
    }

    FiberId Yarns::getFiberId(int index) {
        if(index >= this->fiberSize() || index < 0) return std::this_thread::get_id();
        return this->fibers.at(index)->id();
    }

    FiberId Yarns::getAsyncFiberId(int index) {
        if(index >= this->aFiberSize() || index < 0) return std::this_thread::get_id();
        return this->asyncFibers.at(index)->id();
    }
}
