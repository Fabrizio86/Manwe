//
// Created by Fabrizio Paino on 2022-01-16.
//

#include "Fiber.hpp"
#include "Workload.hpp"
#include "ITask.hpp"
#include "StopExecutionException.hpp"


namespace YarnBall {

    using namespace std;
    static const float OPTIMAL_QUEUE_MULTIPLIER = 4.75;
    static const int POWER = 2;
    static const int HDW_THREADS = thread::hardware_concurrency();
    const unsigned int Fiber::maxQueueSize = floor(pow(HDW_THREADS, POWER) * OPTIMAL_QUEUE_MULTIPLIER);

    void Fiber::execute(sITask task) {
        if (!this->running) return;

        this->queue->push_back(std::move(task));
        this->condition.notify_one();
    }

    void Fiber::process() {
        while (this->running) {

            // if we have no tasks assigned, and no more pending tasks
            if (this->queue->empty()) {
                // check if there are pending tasks
                if (this->anyPendingTasks()) {
                    this->getFromPending(this->fiberId);
                    continue;
                }

                // if we are temp, we don't wait, just exit
                if (this->temp) {
                    this->signalDone(this->fiberId);
                    return;
                }

                // wait for new work
                this->wait();
            }

            // after resuming from waiting, let's verify we still need to work
            if (!this->running || this->queue == nullptr || this->queue->empty()) return;

            sITask task = this->queue->front();
            this->queue->pop_front();

            if (task == nullptr) continue;

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

    void Fiber::wait() {
        if (!this->running) return;

        Locket lk(this->mu);
        this->condition.wait(lk, [this] { return this->waitCondition(); });
    }

    bool Fiber::waitCondition() {
        bool empty = !this->queue->empty();
        return !this->running || empty;
    }

    Workload Fiber::workload() {
        if (this->queue->size() == 0)
            return Workload::Idle;

        int queueSize = static_cast<int>(this->queue->size());
        int percent = (queueSize / this->maxQueueSize) * 100;

        if (percent <= Workload::Busy) return Workload::Busy;
        if (percent > Workload::Busy && percent < Workload::Burdened) return Workload::Burdened;

        return Workload::Overburdened;
    }

    FiberId Fiber::id() const {
        return this->fiberId;
    }

    void Fiber::markAsTemp() {
        this->temp = true;
    }

    OsHandler Fiber::osHandler() {
        return this->thread.native_handle();
    }

    void Fiber::stop() {
        this->running = false;
        this->condition.notify_one();
    }

    Fiber::Fiber(FiberId id, sQueue queue, SignalDone signalDone, GetFromPending getFromPending, AnyPendingTasks anyPendingTasks) : queue(queue),
                                                                                                                                    running(true),
                                                                                                                                    temp(false),
                                                                                                                                    fiberId(id),
                                                                                                                                    signalDone(signalDone),
                                                                                                                                    getFromPending(getFromPending),
                                                                                                                                    anyPendingTasks(anyPendingTasks) {
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