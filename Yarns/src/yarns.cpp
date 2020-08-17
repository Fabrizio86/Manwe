#include "yarns.hpp"

#include <utility>
#include "system.hpp"
#include "RandomScheduler.h"

namespace YarnBall {
    Yarns *Yarns::instance() {
        static Yarns instance;
        return &instance;
    }

    void Yarns::submit(sITask task) {
        this->scheduler->submit(task);
    }

    void Yarns::invoke(Task task) {
        this->scheduler->invoke(std::move(task));
    }

    void Yarns::stop() {
        // if workQueue is not valid we have already cleared the threads
        if (!this->workQueue.isValid()) {
            return;
        }

        this->workQueue.clear();
        this->asyncQueue.clear();
        this->scheduler->stop();
    }

    Yarns::~Yarns() {
        this->stop();
        delete this->scheduler;
    }

    Yarns::Yarns() {
        this->scheduler = new RandomScheduler(&this->fibers,
                                              &this->asyncFibers,
                                              &this->workQueue,
                                              &this->asyncQueue,
                                              &this->limits);
    }

    const Limiter* Yarns::getLimits() const {
        return &this->limits;
    }
}
