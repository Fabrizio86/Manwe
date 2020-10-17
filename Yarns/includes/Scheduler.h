//
// Created by Fabrizio Paino on 2020-09-21.
//

#ifndef YARNS_SCHEDULER_H
#define YARNS_SCHEDULER_H

#include "Definitions.h"
#include "IScheduler.h"

namespace YarnBall {

    class Scheduler {
    public:
        ~Scheduler();

        static Scheduler *instance();

        /// \brief add task to the execution workQueue
        /// \param task to execute
        void submit(ITask* task);

        /// \brief submit a fire and forget task
        /// \param task
        void invoke(Task task);

    private:

        /// \brief clean aborted threads
        void cleanup(Fiber* fiber, bool async);

        void getWork(Fiber* fiber);

        Scheduler();

        sIScheduler scheduler;
        WorkQueue workQueue;
        WorkQueue asyncQueue;

        /// \brief Transfer tasks from overworked fiber to a new one
        /// \param currentThread the newly created fiber
        /// \param task the current task
        /// \param the size of the queue
        sFiber offloadWork(sFiber currentThread, ITask* task, uint queueSize);

        friend Fiber;
    };

}

#endif //YARNS_SCHEDULER_H
