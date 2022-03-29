//
// Created by Fabrizio Paino on 2022-01-14.
//

#include <cmath>
#include "Yarn.h"
#include "RandomScheduler.h"
#include "StopExecutionException.h"

void raiseSignal(YarnBall::sFiber fiber);

#ifdef _WIN32

void raiseSignal(YarnBall::sFiber fiber){

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

    static const int RETRIES = std::sqrt(std::thread::hardware_concurrency());

    void terminationSignal(int signal) {
        if (signal == SIGUSR2)
            throw StopExecutionException();
    }

    void Yarn::Run(sITask task) {
        // get a thread from the scheduler
        int index;
        int tries = 0;
        int fibersCount = this->fibers.size();
        FiberId currentId = std::this_thread::get_id();
        sFiber currentFiber;
        Workload compareLevel = Workload::Burdened;

        do {
            if (tries > RETRIES && compareLevel == Workload::Burdened) {
                compareLevel = Workload::Overburdened;
                tries = 0;
            } else if (tries > RETRIES && compareLevel == Workload::Overburdened) {
                // we need new fiber, system is too busy
                this->shiftWork(currentFiber, task);
                return;
            }

            index = this->scheduler->ThreadIndex(fibersCount);
            currentFiber = this->get(index);

            // if this is the same thread, try another
            if (currentFiber->id() == currentId) continue;

            tries++;

            // continue getting a new fiber since this one is too busy
        } while (currentFiber->workload() >= compareLevel);

        currentFiber->execute(task);
    }

    void Yarn::shiftWork(sFiber currentFiber, sITask task) {
        auto queue = this->queues[currentFiber->id()];
        auto newQueue = std::make_shared<Queue>();

        newQueue->push_front(task);

        int amountToShift = queue->size() / 2;

        for (int i = 0; i < amountToShift; ++i) {
            sITask task = queue->back();
            queue->pop_back();
            newQueue->push_front(task);
        }

        auto fiber = std::make_shared<Fiber>(newQueue);
        this->queues[fiber->id()] = newQueue;

        fiber->markAsTemp([&](FiberId id) {
            this->queues.erase(id);
            this->fibers.remove_if([&id](const sFiber &fiber) { return fiber->id() == id; });
        });

        fibers.push_back(fiber);
    }

    Yarn::Yarn() {

        signal(SIGUSR2, terminationSignal);

        for (int i = 0; i < Yarn::ComputeThreadCount(); ++i) {
            auto queue = std::make_shared<Queue>();
            auto fiber = std::make_shared<Fiber>(queue);
            this->queues[fiber->id()] = queue;
            this->fibers.push_back(fiber);
        }

        this->scheduler = new RandomScheduler();
    }

    Yarn::~Yarn() {
        for (const sFiber &fiber: this->fibers)
            fiber->stop();

        if (this->scheduler != nullptr)
            delete this->scheduler;
    }

    Yarn *Yarn::instance() {
        static Yarn instance;
        return &instance;
    }

    int Yarn::ComputeThreadCount() {
        int computedCount = std::floor(std::thread::hardware_concurrency() * OptimalMultiplier);
        return std::max(MinThreads, computedCount);
    }

    sFiber Yarn::get(int index) {
        auto fiber = this->fibers.begin();
        std::advance(fiber, index);
        return *fiber;
    }

    void Yarn::SwitchScheduler(IScheduler *scheduler) {
        if (scheduler == nullptr) return;
        auto currentScheduler = this->scheduler;
        this->scheduler = scheduler;
        delete currentScheduler;
    }

    void Yarn::terminateFiber(FiberId id) {
        raiseSignal(this->find(id));
    }

    sFiber Yarn::find(FiberId id) {
        for (auto i = this->fibers.begin(); i != this->fibers.end(); i++) {
            auto currentFiber = *i;
            if (currentFiber->id() == id)
                return currentFiber;
        }

        return nullptr;
    }
}