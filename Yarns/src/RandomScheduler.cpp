//
// Created by Fabrizio Paino on 2020-08-25.
//

#include "RandomScheduler.h"
#include "Limiter.h"
#include "yarns.hpp"

namespace YarnBall {

    RandomScheduler::RandomScheduler() = default;

    int RandomScheduler::getNextAsyncFiber(FiberId id) {
        FiberId currentThread;
        int randIndex;

        // we don't want to assign the task to the same thread that created it,
        // in case it waits for the child task to complete
        do {
            randIndex = this->generator.get(Yarns::instance()->aFiberSize());
            currentThread = Yarns::instance()->getAsyncFiberId(randIndex);
        } while (id == currentThread);

        return randIndex;
    }

    int RandomScheduler::getNextFiber(FiberId id) {
        FiberId currentThread;
        int randIndex;

        // we don't want to assign the task to the same thread that created it,
        // in case it waits for the child task to complete
        do {
            randIndex = this->generator.get(Yarns::instance()->fiberSize());
            currentThread = Yarns::instance()->getFiberId(randIndex);
        } while (id == currentThread);

        return randIndex;
    }

}