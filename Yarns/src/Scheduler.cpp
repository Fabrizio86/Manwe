//
// Created by Fabrizio Paino on 2020-09-21.
//

#include "Scheduler.h"
#include "RandomScheduler.h"
#include "yarns.hpp"
#include "AsyncTask.h"

#include <algorithm>

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

    void Scheduler::submit(ITask *task) {
        int index = this->scheduler->getNextFiber(task->id());
        sFiber currentThread = Yarns::instance()->fibers.at(index);
        Workload workload = currentThread->addWork(task);

        // if the status is not exhausted the tas was submitted and we can return
        if (workload != Workload::Exhausted) return;

        Limiter *limits = &Yarns::instance()->limits;

        // if we can allocate more threads, do so and steal work from current fiber
        if (Yarns::instance()->fiberSize() < limits->getMaxThreads()) {
            auto newFiber = this->offloadWork(currentThread, task, limits->getWorkQueueThreshold());
            newFiber->markAsTemp();
            Yarns::instance()->fibers.push_back(newFiber);
        } else {
            std::shared_ptr<ITask> sTsk(task);
            // otherwise add it the pending assignment
            this->workQueue.push(sTsk);
        }
    }

    void Scheduler::invoke(Task task) {
        int index = this->scheduler->getNextAsyncFiber(std::this_thread::get_id());
        sFiber currentThread = Yarns::instance()->asyncFibers.at(index);
        ITask *aTsk = new AsyncTask(task);
        Workload workload = currentThread->addWork(aTsk);

        // if the status is not exhausted the tas was submitted and we can return
        if (workload != Workload::Exhausted) return;

        Limiter *limits = &Yarns::instance()->limits;

        if (Yarns::instance()->aFiberSize() < limits->getMaxAsync()) {
            auto newAsyncFiber = this->offloadWork(currentThread, aTsk, limits->getAsyncWorkQueueThreshold());

            newAsyncFiber->markAsTemp();
            newAsyncFiber->MarkAsync();
            Yarns::instance()->asyncFibers.push_back(newAsyncFiber);
        } else {
            std::shared_ptr<ITask> sTsk(aTsk);
            this->asyncQueue.push(sTsk);
        }
    }

    Scheduler::~Scheduler() {
        this->asyncQueue.clear();
        this->workQueue.clear();
    }

    sFiber Scheduler::offloadWork(sFiber currentThread, ITask *task, uint queueSize) {
        auto newFiber = std::make_shared<Fiber>(queueSize);
        newFiber->addWork(task);

        // steal work from overburden fiber
        while (currentThread->getWorkload() != Normal) {
            newFiber->addWork(currentThread->stealWork());
        }

        return newFiber;
    }

    void Scheduler::cleanup(Fiber *f, bool async) {
        auto id = f->id();
        auto instance = Yarns::instance();
        auto fibers = async ? instance->asyncFibers : instance->fibers;

        auto fiberIter = std::find_if(fibers.begin(), fibers.end(), [&id](const sFiber &fiber) {
            return fiber->id() == id;
        });

        (*fiberIter)->detach();
        std::remove(fibers.begin(), fibers.end(), *fiberIter), fibers.end();
    }

    void Scheduler::getWork(Fiber *fiber) {
        if (!this->workQueue.empty()) {
            fiber->addWork(this->workQueue.get().get());
        }
    }

}