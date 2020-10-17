//
// Created by Fabrizio Paino on 2020-09-01.
//

#ifndef YARNS_ISCHEDULER_H
#define YARNS_ISCHEDULER_H

#include <vector>
#include "Fiber.h"
#include "Definitions.h"
#include "RandGenerator.h"
#include "queue.hpp"

namespace YarnBall {

    class IScheduler {
    public:

        /// \brief Default destructor
        virtual ~IScheduler() = default;

        /// Gets the next available fiber, that is different from the calling parent
        /// \param id the parent thread id that generated the task
        /// \return the index of the fiber to call
        virtual int getNextFiber(FiberId id) = 0;

        /// \brief generate the next async fiber id
        /// \param id the parent thread id that generated the task
        /// \return the next async fiber instance
        virtual int getNextAsyncFiber(FiberId id) = 0;
    };

}

#endif //YARNS_ISCHEDULER_H
