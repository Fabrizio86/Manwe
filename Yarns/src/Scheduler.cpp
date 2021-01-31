//
// Created by Fabrizio Paino on 2020-09-21.
//

#include "Scheduler.h"
#include "RandomScheduler.h"
#include "yarns.hpp"

#include <algorithm>

namespace YarnBall {

    struct SchedulerTask : public ITask {
        void run() override {
            this->task();
        }

        void exception(std::exception_ptr exception) override {}

        Task task;
    };

    Scheduler *Scheduler::instance() {
        static Scheduler scheduler;
        return &scheduler;

        // todo: at some point if the system is still locked and bloated we need to start kill blocking threads
        // todo: so we need a timer/signal way to check life-span of a thread (subclassing from std::thread?)
    }

    Scheduler::Scheduler() {
        this->scheduler = std::make_shared<RandomScheduler>();
    }

#define MAX_RETRY 3

    void Scheduler::submit(sITask task, bool isAsync) {
        sFiber currentThread;
        int retry = 0;

        // find a thread that is not exhausted
        do {
            int index = !isAsync ? this->scheduler->getNextFiber(task->id()) :
                        this->scheduler->getNextAsyncFiber(std::this_thread::get_id());

            auto fiber = !isAsync ? Yarns::instance()->fibers :
                         Yarns::instance()->asyncFibers;

            auto pos = fiber.begin();
            std::advance(pos, index);

            currentThread = *pos;
            retry++;
        } while (currentThread->getWorkload() == Workload::Exhausted && retry <= MAX_RETRY);

        // if is not exhausted, just add work
        if (currentThread->getWorkload() != Workload::Exhausted) {
            currentThread->addWork(task);
            return;
        }

        // otherwise calculate if we are within the bounds
        Limiter *limits = &Yarns::instance()->limits;
        auto currentSize = !isAsync ? Yarns::instance()->fiberSize() : Yarns::instance()->aFiberSize();
        auto maxSize = !isAsync ? limits->getMaxThreads() : limits->getMaxAsync();

        // if we have exhausted the max number of threads, just add it to the pending queue
        if (currentSize >= maxSize) {
            WorkQueue *queue = !isAsync ? &this->workQueue : &this->asyncQueue;
            queue->push(task);
            return;
        }

        // get current thread, if is exhausted the max retry lapsed, create a new thread
        auto threshold = !isAsync ? limits->getWorkQueueThreshold() : limits->getAsyncWorkQueueThreshold();
        auto newFiber = std::make_shared<Fiber>(threshold, true);

        if (isAsync) {
            newFiber->detach();
        }

        // add new task (give priority to new operations) then steal from current thread
        newFiber->addWork(task);

        // steal work from overburden fiber
        while (currentThread->getWorkload() != Normal) {
            newFiber->addWork(currentThread->stealWork());
        }

        if (isAsync) {
            Yarns::instance()->asyncFibers.push_back(newFiber);
        } else {
            Yarns::instance()->fibers.push_back(newFiber);
        }
    }

    void Scheduler::invoke(Task task) {
        auto aTask = std::make_shared<SchedulerTask>();
        aTask->task = task;
        Scheduler::submit(aTask, true);
    }

    Scheduler::~Scheduler() {
        this->asyncQueue.clear();
        this->workQueue.clear();
    }

    void Scheduler::cleanup(Fiber *f) {
        Locker lk(this->mu);

        auto instance = Yarns::instance();
        auto fibers = f->isDetached() ? &instance->asyncFibers : &instance->fibers;

        auto fiberIter = std::find_if(fibers->begin(), fibers->end(), [&f](const sFiber &fiber) {
            return fiber->id() == f->id();
        });

        sFiber sf = *fiberIter;

        if (!sf->isDetached())
            sf->detach();

        if (fiberIter != fibers->end())
            fibers->remove(sf);
    }

    void Scheduler::getWork(Fiber *fiber) {
        if (!this->workQueue.empty()) {
            sITask task = fiber->isDetached() ? this->asyncQueue.get() : this->workQueue.get();
            fiber->addWork(task);
        }
    }

}