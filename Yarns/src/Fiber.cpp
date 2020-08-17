//
// Created by Fabrizio Paino on 2020-08-16.
//

#include "Fiber.h"

#include <utility>

namespace YarnBall {

    using namespace std;

    sITask Fiber::stealWork() {
        // if we got less than 2 items, don't steal
        if (this->queueSize() < 3) return nullptr;

        sITask task = std::move(this->queue.back());
        this->queue.pop_back();
        return task;
    }

    void Fiber::work() {
        while (this->getState() != State::Aborting) {
            if (this->queue.empty()) {
                this->signal(this);
                this->wait();
            }

            // thread could have been stopped after waiting, lets just exit
            if (this->getState() == State::Aborting) {
                return;
            }

            sITask task = this->queue.front();
            this->queue.pop_front();

            try {
                this->setState(State::Running);
                if(task != nullptr) {
                    task->run();
                }
            }
            catch (...) {
                this->setState(State::Error);
                task->exception(current_exception());
                this->setState(State::Running);
            }

            this->setState(State::Waiting);
        }
    }

    Workload Fiber::addWork(sITask work) {
        auto workload = this->getWorkload();

        if (workload != Workload::Exhausted) {
            this->queue.push_back(work);
            this->condition.notify_one();
        }

        return workload;
    }

    void Fiber::setSignaler(SignalScheduler signaler) {
        this->signal = signaler;
    }

    size_t Fiber::queueSize() {
        return this->queue.size();
    }

    Fiber::Fiber(uint upperLimit) : BaseThread(upperLimit) {
        this->signal = [](IFiber *) {};
    }

    void Fiber::clearQueue() {
       this->queue.clear();
    }
}