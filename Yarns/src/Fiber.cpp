//
// Created by Fabrizio Paino on 2020-08-16.
//

#include "Fiber.h"
#include "Scheduler.h"

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
                Scheduler::instance()->getWork(this);
                this->wait();
            }

            // thread could have been stopped after waiting, lets just exit
            if (this->getState() == State::Aborting) {
                Scheduler::instance()->cleanup(this);
                return;
            }

            sITask task = this->queue.front();
            this->queue.pop_front();

            try {
                if (task != nullptr || task.get() != nullptr) {
                    task->run();
                }
            }
            catch (...) {
                task->exception(current_exception());
            }
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

    size_t Fiber::queueSize() {
        return this->queue.size();
    }

    void Fiber::clearQueue() {
        this->queue.clear();
    }

    Fiber::~Fiber() {
        this->join();
    }

    Fiber::Fiber(uint upperLimit) : BaseThread(upperLimit) { }
}