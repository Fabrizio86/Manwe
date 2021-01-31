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
            newAsyncFiber->detach();
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

        auto pos = this->fibers.begin();
        std::advance(pos, index);

        return pos->get()->id();
    }

    FiberId Yarns::getAsyncFiberId(int index) {
        if(index >= this->aFiberSize() || index < 0) return std::this_thread::get_id();

        auto pos = this->asyncFibers.begin();
        std::advance(pos, index);

        return pos->get()->id();
    }

    Yarns::~Yarns() {
        for(auto fiber : this->fibers) {
            fiber->detach();
            fiber->stop();
        }

        this->asyncFibers.clear();
    }
}
