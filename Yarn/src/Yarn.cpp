//
// Created by Fabrizio Paino on 2022-01-14.
//

#include <cmath>
#include "Yarn.h"
#include "RandomScheduler.h"
#include "StopExecutionException.h"

void raiseSignal(YarnBall::sFiber fiber);

#ifdef _WIN32

#include <Windows.h>

void raiseSignal(YarnBall::sFiber fiber){
    TerminateThread(fiber->osHandler(), SIGUSR2);
}

#else

#include <pthread.h>

void raiseSignal(YarnBall::sFiber fiber) {
    pthread_kill(fiber->osHandler(), SIGUSR2);
}

#endif

namespace YarnBall {

#define MinThreads 4
#define OptimalMultiplier 3.5

    int Yarn::MinThreadCount = std::thread::hardware_concurrency() <= MinThreads ? MinThreads : static_cast<int>(std::thread::hardware_concurrency());
    int Yarn::MaxThreadCount = std::max(Yarn::MinThreadCount, (int) std::floor(Yarn::MinThreadCount * OptimalMultiplier));

    static const int RETRIES = static_cast<int>(std::sqrt(std::thread::hardware_concurrency()));

    void terminationSignal(int signal) {
        if (signal == SIGUSR2)
            throw StopExecutionException();
    }

    void Yarn::Run(sITask task) {
        // get a thread from the scheduler
        int index;
        int tries = 0;
        sFiber currentFiber;
        Workload compareLevel = Workload::Burdened;
        int currentPoolSize = static_cast<int>(count_if(this->fibers.begin(), this->fibers.end(), [](auto f) { return f != nullptr; }));

        do {
            if (tries > RETRIES && compareLevel == Workload::Burdened) {
                compareLevel = Workload::Overburdened;
                tries = 0;
            } else if (tries > RETRIES && compareLevel == Workload::Overburdened) {
                // we need new fiber, system is too busy
                this->shiftWork(currentFiber, task);
                return;
            }

            do {
                index = this->scheduler->ThreadIndex(currentPoolSize);
                currentFiber = this->fibers.at(index);
            } while (currentFiber == nullptr);

            tries++;

            // continue getting a new fiber since this one is too busy
        } while (currentFiber->workload() >= compareLevel);

        currentFiber->execute(task);
    }

    void Yarn::initializeNewThread(FiberId id, bool markAsTemp) {
        auto signalDone = [&](FiberId id) {
            std::lock_guard<std::mutex> lock(this->cmu);
            this->fibers[id] = nullptr;
            this->queues[id]->clear();
        };

        auto getFromPending = [&](FiberId id) {
            std::lock_guard<std::mutex> lock(this->dmu);
            auto currentFiber = this->fibers[id];

            while (!this->pending->empty()) {
                this->queues[id]->push_back(this->pending->front());
                this->pending->pop_front();

                if (currentFiber->workload() >= Workload::Burdened)
                    return;
            }
        };

        auto anyPendingTasks = [&]() {
            return !this->pending->empty();
        };

        std::lock_guard<std::mutex> lock(this->cmu);
        auto fiber = std::make_shared<Fiber>(id, this->queues[id], signalDone, getFromPending, anyPendingTasks);
        this->fibers[id] = fiber;

        if (markAsTemp)
            fiber->markAsTemp();
    }

    void Yarn::arrangeQueues(FiberId currentQueueId, FiberId newQueueId) {
        auto queue = this->queues[currentQueueId];
        auto newQueue = this->queues.at(newQueueId);

        int amountToShift = static_cast<int>(ceil(queue->size() * 0.25));

        for (int i = 0; i < amountToShift; ++i) {
            auto transferTask = queue->back();
            queue->pop_back();
            newQueue->push_front(transferTask);
        }
    }

    void Yarn::shiftWork(const sFiber &currentFiber, sITask task) {
        std::lock_guard<std::mutex> lock(this->mu);

        if (this->maxLimitReached()) {
            std::lock_guard<std::mutex> dataLock(this->dmu);
            this->pending->push_back(task);
            return;
        }

        auto newFiberId = this->firstUnusedId();
        this->arrangeQueues(currentFiber->id(), newFiberId);
        this->queues.at(newFiberId)->push_front(task);
        this->initializeNewThread(newFiberId, true);
    }

    Yarn::Yarn() {
        signal(SIGUSR2, terminationSignal);
        this->pending = std::make_shared<Queue>();

        this->fibers.resize(Yarn::MaxThreadCount);

        for (int i = 0; i < Yarn::MaxThreadCount; ++i)
            this->queues.push_back(std::make_shared<Queue>());

        for (int i = 0; i < Yarn::MinThreadCount; ++i) {
            this->initializeNewThread(i);
        }

        this->scheduler = std::make_shared<RandomScheduler>();
    }

    Yarn::~Yarn() {
        this->fibers.clear();
    }

    Yarn *Yarn::instance() {
        static Yarn instance;
        return &instance;
    }

    void Yarn::SwitchScheduler(sIScheduler scheduler) {
        if (scheduler == nullptr) return;
        auto currentScheduler = this->scheduler;
        this->scheduler = scheduler;
    }

    void Yarn::terminateFiber(FiberId id) {
        raiseSignal(this->fibers.at(static_cast<FiberId>(id)));
    }

    bool Yarn::maxLimitReached() {
        return !std::any_of(this->fibers.begin() + Yarn::MinThreadCount, this->fibers.end(), [](auto f) { return f == nullptr; });
    }

    FiberId Yarn::firstUnusedId() {
        auto result = std::find_if(this->fibers.begin(), this->fibers.end(), [](auto f) { return f == nullptr; });
        return static_cast<FiberId>(std::distance(this->fibers.begin(), result));
    }

}