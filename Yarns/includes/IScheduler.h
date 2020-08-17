//
// Created by Fabrizio Paino on 2020-09-01.
//

#ifndef YARNS_ISCHEDULER_H
#define YARNS_ISCHEDULER_H

#include <vector>
#include "Fiber.h"
#include "AsyncFiber.h"
#include "Definitions.h"
#include "RandGenerator.h"
#include "queue.hpp"

namespace YarnBall {

    class IScheduler {
    public:

        /// \brief Default destructor
        virtual ~IScheduler() = default;

        /// \brief add task to the execution workQueue
        /// \param task to execute
        virtual void submit(sITask task) = 0;

        /// \brief submit a fire and forget task
        /// \param task
        virtual void invoke(Task task) = 0;

        /// \brief Signals the threads to end and clear their queue
        virtual void stop() = 0;
    };

}

#endif //YARNS_ISCHEDULER_H
