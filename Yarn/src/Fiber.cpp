//
// Created by Fabrizio Paino on 2022-01-16.
//

#include "Fiber.hpp"
#include "Workload.hpp"
#include "ITask.hpp"
#include "StopExecutionException.hpp"

#ifdef _WIN32
    #include <windows.h>

void SetNumaAffinity() {

}

#elif defined(__linux__)
    #include <numa.h>
    #include <sched.h>

void SetNumaAffinity() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset); // Set to a core on NUMA node X
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}
#elif __APPLE__
#include <pthread.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>

void SetNumaAffinity() {
    // Get the Mach thread port for the current thread
    thread_t thread = mach_thread_self();

    // Define an affinity tag — same tag groups threads together
    // Use a positive, non-zero integer (e.g., thread ID mod N)
    // You can change this value if you want groups of threads to share cache
    thread_affinity_policy_data_t policy = {1};

    // Apply the affinity policy
    thread_policy_set(thread, THREAD_AFFINITY_POLICY, (thread_policy_t) &policy, THREAD_AFFINITY_POLICY_COUNT);
    // Clean up
    mach_port_deallocate(mach_task_self(), thread);
}
#else
void SetNumaAffinity() {

}
#endif


namespace YarnBall {
    using namespace std;
    static const float OPTIMAL_QUEUE_MULTIPLIER = 4.75;
    static const int POWER = 2;
    static const int HDW_THREADS = thread::hardware_concurrency();
    const unsigned int Fiber::maxQueueSize = floor(pow(HDW_THREADS, POWER) * OPTIMAL_QUEUE_MULTIPLIER);

    void Fiber::execute(sITask task) {
        if (!this->running) return;

        this->queue->enqueue(std::move(task));
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

            auto task = this->queue->pop_front();

            if (task.has_value()) {
                auto taskValue = task.value();
                try {
                    taskValue->run();
                } catch (const StopExecutionException &e) {
                    taskValue->exception(std::current_exception());
                }
                catch (...) {
                    taskValue->exception(std::current_exception());
                }
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
        size_t currentSize = this->queue->Size();

        if (currentSize == 0)
            return Workload::Idle;

        // Convert to float to avoid integer division
        float percent = (static_cast<float>(currentSize) / this->maxQueueSize) * 100.0f;

        if (percent <= static_cast<float>(Workload::Busy))
            return Workload::Busy;

        if (percent < static_cast<float>(Workload::Burdened))
            return Workload::Burdened;

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

    Fiber::Fiber(FiberId id, sQueue queue,
                 SignalDone signalDone,
                 GetFromPending getFromPending,
                 AnyPendingTasks anyPendingTasks) : running(true),
                                                    temp(false),
                                                    signalDone(signalDone),
                                                    getFromPending(getFromPending),
                                                    anyPendingTasks(anyPendingTasks),
                                                    queue(queue),
                                                    fiberId(id) {
        SetNumaAffinity();
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
