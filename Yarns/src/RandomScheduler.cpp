//
// Created by Fabrizio Paino on 2020-08-25.
//

#include "RandomScheduler.h"
#include "Limiter.h"

namespace YarnBall {

    RandomScheduler::RandomScheduler(Fibers *fibers,
                                     AsyncFibers *asyncFibers,
                                     WorkQueue *workQueue,
                                     AsyncQueue *asyncQueue,
                                     Limiter *limits) :
            limits(limits),
            fibers(fibers),
            asyncFibers(asyncFibers),
            workQueue(workQueue),
            asyncQueue(asyncQueue) {

        // create all the work threads
        for (uint i = 0; i < this->limits->getThreadNumbers(); ++i) {
            auto newFiber = std::make_shared<Fiber>(this->limits->getWorkQueueThreshold());
            newFiber->setSignaler([&workQueue = this->workQueue](IFiber *fiber) {
                if (!workQueue->empty()) {
                    fiber->addWork(workQueue->get());
                }
            });
            this->fibers->push_back(newFiber);
        }

        // create all the background services tasks
        for (uint i = 0; i < this->limits->getAsyncWorkQueueThreshold(); ++i) {
            auto newAsyncFiber = std::make_shared<AsyncFiber>(this->limits->getAsyncWorkQueueThreshold());
            this->asyncFibers->push_back(newAsyncFiber);
        }

        // todo: at some point if the system is still locked and bloated we need to start kill blocking threads
        // todo: so we need a timer/signal way to check life-span of a thread (subclassing from std::thread?)
    }

    void RandomScheduler::stop() {
        this->fibers->clear();
        this->asyncFibers->clear();
    }

    void RandomScheduler::submit(sITask task) {
        sFiber currentThread = this->getNextFiber(task->id());
        Workload workload = currentThread->addWork(task);

        // if the status is not exhausted the tas was submitted and we can return
        if (workload != Workload::Exhausted) return;

        // if we can allocate more threads, do so and steal work from current fiber
        if (this->fibers->size() < this->limits->getMaxThreads()) {
            auto newFiber = this->offloadWork<sFiber, Fiber, sITask>(currentThread, task, this->limits->getWorkQueueThreshold());
            newFiber->isTemp();
            this->fibers->push_back(newFiber);
        } else {
            // otherwise add it the pending assignment
            this->workQueue->push(task);
        }
    }

    void RandomScheduler::invoke(Task task) {
        auto asyncFiber = this->getNextAsyncFiber();
        Workload workload = asyncFiber->addWork(task);

        // if the status is not exhausted the tas was submitted and we can return
        if (workload != Workload::Exhausted) return;

        if (this->asyncFibers->size() < this->limits->getMaxAsync()) {
            auto newAsyncFiber = this->offloadWork<sAsyncFiber, AsyncFiber, Task>(asyncFiber, task, this->limits->getAsyncWorkQueueThreshold());
            this->asyncFibers->push_back(newAsyncFiber);
        } else {
            this->asyncQueue->push(task);
        }
    }

    template<class InType, class ConcreteClass, typename TaskType>
    InType RandomScheduler::offloadWork(InType currentThread, TaskType task, uint queueSize) {
        auto newFiber = std::make_shared<ConcreteClass>(queueSize);
        newFiber->addWork(task);

        // steal work from overburden fiber
        while (currentThread->getWorkload() != Normal) {
            newFiber->addWork(currentThread->stealWork());
        }

        return newFiber;
    }

    sAsyncFiber RandomScheduler::getNextAsyncFiber() {
        int randIndex = this->generator.get(this->asyncFibers->size());
        return this->asyncFibers->at(randIndex);
    }

    sFiber RandomScheduler::getNextFiber(FiberId id) {
        sFiber currentThread;

        // we don't want to assign the task to the same thread that created it,
        // in case it waits for the child task to complete
        do {
            int randIndex = this->generator.get(this->fibers->size());
            currentThread = this->fibers->at(randIndex);
        } while (id == currentThread->id());

        return currentThread;
    }

}