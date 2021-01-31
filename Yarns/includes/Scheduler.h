//
// Created by Fabrizio Paino on 2020-09-21.
//

#ifndef YARNS_SCHEDULER_H
#define YARNS_SCHEDULER_H

#include "Definitions.h"
#include "IScheduler.h"
#include "Fiber.h"


namespace YarnBall {

    class Scheduler {
    public:
        ~Scheduler();

        static Scheduler *instance();

        /// \brief add task to the execution workQueue
        /// \param task to execute
        void submit(sITask task, bool isAsync = false);

        /// \brief submit a fire and forget task
        /// \param task
        void invoke(Task task);

    private:

        std::mutex mu;

        /// \brief clean aborted threads
        void cleanup(Fiber* fiber);

        void getWork(Fiber* fiber);

        Scheduler();

        sIScheduler scheduler;
        WorkQueue workQueue;
        WorkQueue asyncQueue;

        friend Fiber;
    };

}

#endif //YARNS_SCHEDULER_H
