//
// Created by Fabrizio Paino on 2022-01-16.
//

#include "Fiber.h"
#include "Workload.h"
#include "ITask.h"
#include "StopExecutionException.h"


namespace YarnBall {

    using Locket = std::unique_lock<std::mutex>;

    static const float OPTIMAL_QUEUE_MULTIPLIER = 1.39;
    const unsigned int Fiber::maxQueueSize = floor(
            pow(std::thread::hardware_concurrency(), 2) * OPTIMAL_QUEUE_MULTIPLIER);

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
            catch (const StopExecutionException &e) {
                task->exception(std::current_exception());
            }
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

        int percent = (this->queue->size() / this->maxQueueSize) * 100;

        if (percent <= Workload::Busy) return Workload::Busy;
        if (percent > Workload::Busy && percent < Workload::Burdened) return Workload::Burdened;

        return Workload::Overburdened;
    }

    FiberId Fiber::id() {
        return this->thread.get_id();
    }

    void Fiber::markAsTemp(SignalDone signalDone) {
        this->temp = true;
        this->signalDone = signalDone;
    }

    OsHandler Fiber::osHandler() {
        return this->thread.native_handle();
    }

    Fiber::Fiber(sQueue queue) : running(true), temp(false), queue(queue) {
        this->thread = std::thread(&Fiber::process, this);
    }

    Fiber::~Fiber() {
        this->stop();

        if (this->temp)
            this->thread.detach();

        if (this->thread.joinable())
            this->thread.join();
    }

}