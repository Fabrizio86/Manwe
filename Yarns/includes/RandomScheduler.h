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
        RandomScheduler();

        ~RandomScheduler() override = default;

        /// Gets the next available fiber, that is different from the calling parent
        /// \param id the parent thread id that generated the task
        /// \return the index of the fiber to call
        int getNextFiber(FiberId id) override;

        /// \brief generate the next async fiber id
        /// \param id the parent thread id that generated the task
        /// \return the next async fiber instance
        int getNextAsyncFiber(FiberId id) override;

    private:
        RandGenerator generator;
    };

}

#endif //YARNS_RANDOMSCHEDULER_H
