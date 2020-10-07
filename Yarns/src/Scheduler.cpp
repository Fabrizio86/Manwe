//
// Created by Fabrizio Paino on 2020-09-21.
//

#include "Scheduler.h"
#include "RandomScheduler.h"
#include "yarns.hpp"

namespace YarnBall {

    Scheduler *Scheduler::instance() {
        static Scheduler scheduler;
        return &scheduler;

        // todo: at some point if the system is still locked and bloated we need to start kill blocking threads
        // todo: so we need a timer/signal way to check life-span of a thread (subclassing from std::thread?)
    }

    Scheduler::Scheduler() {
        this->scheduler = std::make_shared<RandomScheduler>();
    }

    void Scheduler::submit(sITask task) {
        int index = this->scheduler->getNextFiber(task->id());
        sFiber currentThread = Yarns::instance()->fibers.at(index);
        Workload workload = currentThread->addWork(task);

        // if the status is not exhausted the tas was submitted and we can return
        if (workload != Workload::Exhausted) return;

        Limiter* limits = &Yarns::instance()->limits;

        // if we can allocate more threads, do so and steal work from current fiber
        if (Yarns::instance()->fiberSize() < limits->getMaxThreads()) {
            auto newFiber = this->offloadWork<sFiber, Fiber, sITask>(currentThread, task,
                                                                     limits->getWorkQueueThreshold());
            newFiber->isTemp();
            Yarns::instance()->fibers.push_back(newFiber);
        } else {
            // otherwise add it the pending assignment
            this->workQueue.push(task);
        }
    }

    void Scheduler::invoke(Task task) {
        int index = this->scheduler->getNextAsyncFiber(std::this_thread::get_id());
        sAsyncFiber currentThread = Yarns::instance()->asyncFibers.at(index);
        Workload workload = currentThread->addWork(task);

        // if the status is not exhausted the tas was submitted and we can return
        if (workload != Workload::Exhausted) return;

        Limiter* limits = &Yarns::instance()->limits;

        if (Yarns::instance()->aFiberSize() < limits->getMaxAsync()) {
            auto newAsyncFiber = this->offloadWork<sAsyncFiber, AsyncFiber, Task>(currentThread, task,
                                                                                  limits->getAsyncWorkQueueThreshold());
            Yarns::instance()->asyncFibers.push_back(newAsyncFiber);
        } else {
            this->asyncQueue.push(task);
        }
    }

    Scheduler::~Scheduler() {
        this->asyncQueue.clear();
        this->workQueue.clear();
    }

    template<class InType, class ConcreteClass, typename TaskType>
    InType Scheduler::offloadWork(InType currentThread, TaskType task, uint queueSize) {
        auto newFiber = std::make_shared<ConcreteClass>(queueSize);
        newFiber->addWork(task);

        // steal work from overburden fiber
        while (currentThread->getWorkload() != Normal) {
            newFiber->addWork(currentThread->stealWork());
        }

        return newFiber;
    }
}