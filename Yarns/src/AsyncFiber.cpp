//
// Created by Fabrizio Paino on 2020-08-25.
//

#include "AsyncFiber.h"

namespace YarnBall {

    using namespace std;

    [[maybe_unused]] Workload AsyncFiber::addWork(Task work) {
        auto workload = this->getWorkload();

        if (workload != Workload::Exhausted) {
            this->queue.push_back(move(work));
        }

        return workload;
    }

    [[maybe_unused]] Task AsyncFiber::stealWork() {
        // if we got less than 2 items, don't steal
        if (this->queueSize() < 3) return nullptr;

        Task task = this->queue.back();
        this->queue.pop_back();

        return task;
    }

    void AsyncFiber::work() {
        while (this->getState() != State::Aborting) {
            if (this->queue.empty()) {
                this->wait();
            }

            // thread could have been stopped after waiting, lets just exit
            if (this->getState() == State::Aborting) {
                return;
            }

            Task task = this->queue.front();
            this->queue.pop_front();

            try {
                this->setState(State::Running);
                if(task != nullptr) {
                    task();
                }
            }
            catch (...) {
                this->setState(State::Error);
            }
        }
    }

    size_t AsyncFiber::queueSize() {
        return this->queue.size();
    }

    AsyncFiber::AsyncFiber(uint upperLimit) : BaseThread(upperLimit) {
        this->detach();
    }

    void AsyncFiber::clearQueue() {
        this->queue.clear();
    }
}