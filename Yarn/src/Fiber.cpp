//
// Created by Fabrizio Paino on 2022-01-16.
//

#include "Fiber.h"
#include "Workload.h"

namespace YarnBall {

    using Locket = std::unique_lock<std::mutex>;

    void Fiber::execute(sITask task) {
        if (!this->running) return;

        this->queue->push_back(task);
        this->condition.notify_one();
    }

    void Fiber::process() {
        while (this->running) {
            if (this->queue->empty()) {
                // if we are temp, we don't wait, just exit
                if (this->temp) {
                    this->signalDone(this->id());
                    return;
                }

                // wait for new work
                this->wait();
            }

            // after resuming from waiting, let's verify we still need to work
            if (!this->running) return;

            sITask task = this->queue->front();
            this->queue->pop_front();

            try {
                task->run();
            }
            catch (const YarnBall::StopExecutionException &e) {}
            catch (...) {
                task->exception(std::current_exception());
            }
        }
    }

    void Fiber::stop() {
        this->running = false;
        this->queue->clear();
        this->condition.notify_all();
    }

    Fiber::Fiber(sQueue queue) : temp(false), queue(queue) {
        this->thread = std::thread(&Fiber::process, this);
    }

    Fiber::~Fiber() {
        this->stop();

        if(this->temp)
            this->thread.detach();

        if (this->thread.joinable())
            this->thread.join();
    }

    void Fiber::wait() {
        if (!this->running) return;

        Locket lk(this->mu);
        this->condition.wait(lk, [this] { return this->waitCondition(); });
    }

    bool Fiber::waitCondition() {
        bool empty = this->queue->size() != 0;
        return !this->running || empty;
    }

    Workload Fiber::workload() {
        if (this->queue->size() == 0)
                return Workload::Idle;

        auto percent = ((float) this->queue->size() / this->maxQueueSize()) * 100;

        if (percent <= Workload::Busy) return Workload::Busy;
        if (percent > Workload::Busy && percent < Workload::Burdened) return Workload::Burdened;

        return Workload::Overburdened;
    }

    FiberId Fiber::id() {
        return this->thread.get_id();
    }

    unsigned int Fiber::maxQueueSize() {
        return std::thread::hardware_concurrency() * std::thread::hardware_concurrency();
    }

    void Fiber::markAsTemp(SignalDone signalDone) {
        this->temp = true;
        this->signalDone = signalDone;
    }

    OsHandler Fiber::osHandler() {
        return this->thread.native_handle();
    }
}