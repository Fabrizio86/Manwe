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
        void submit(sITask task);

        /// \brief submit a fire and forget task
        /// \param task
        void invoke(Task task);

    private:
        Scheduler();

        sIScheduler scheduler;
        WorkQueue workQueue;
        AsyncQueue asyncQueue;

        /// \brief Transfer tasks from overworked fiber to a new one
        /// \param currentThread the newly created fiber
        /// \param task the current task
        /// \param the size of the queue
        template<class InType, class ConcreteClass, typename TaskType>
        InType offloadWork(InType currentThread, TaskType task, uint queueSize);

    };

}

#endif //YARNS_SCHEDULER_H
