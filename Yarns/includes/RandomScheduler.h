//
// Created by Fabrizio Paino on 2020-08-25.
//

#ifndef YARNS_RANDOMSCHEDULER_H
#define YARNS_RANDOMSCHEDULER_H

#include <vector>

#include "AsyncFiber.h"
#include "Definitions.h"
#include "Fiber.h"
#include "IScheduler.h"
#include "Limiter.h"
#include "queue.hpp"
#include "RandGenerator.h"

namespace YarnBall {

    class RandomScheduler final : public IScheduler {
    public:

        explicit RandomScheduler(Fibers* fibers, AsyncFibers* asyncFibers, WorkQueue* workQueue, AsyncQueue* asyncQueue, Limiter* limits);

        ~RandomScheduler() override = default;

        void submit(sITask task) override;

        void invoke(Task task) override;

        void stop() override;

    private:
        /// Gets the next available fiber, that is different from the calling parent
        /// \param id the parent thread id that generated the task
        /// \return the next random fiber
        sFiber getNextFiber(FiberId id);

        /// \brief generate the next async fiber id
        /// \return the next async fiber instance
        sAsyncFiber getNextAsyncFiber();

        /// \brief Transfer tasks from overworked fiber to a new one
        /// \param currentThread the newly created fiber
        /// \param task the current task
        /// \param the size of the queue
        template<class InType, class ConcreteClass, typename TaskType>
        InType offloadWork(InType currentThread, TaskType task, uint queueSize);

    private:
        Limiter* limits;
        RandGenerator generator;
        Fibers* fibers;
        AsyncFibers* asyncFibers;
        WorkQueue* workQueue;
        AsyncQueue* asyncQueue;
    };

}

#endif //YARNS_RANDOMSCHEDULER_H
