//
// Created by Fabrizio Paino on 2020-09-01.
//

#include "Limiter.h"

#include <cmath>
#include <thread>

namespace YarnBall {

#define MIN_THREADS 4u

    uint Limiter::getMaxThreads() const {
        return maxThreads;
    }

    uint Limiter::getMaxAsync() const {
        return maxAsync;
    }

    uint Limiter::getWorkQueueThreshold() const {
        return workQueueThreshold;
    }

    uint Limiter::getAsyncWorkQueueThreshold() const {
        return asyncWorkQueueThreshold;
    }

    uint Limiter::calculateSize(uint base, uint max, uint &maxWorkload) {
        uint maxWorkQueue = floor(sqrt(base));
        maxWorkload = base * maxWorkQueue;

        return base * sqrt(base * max);
    }

    uint Limiter::computeThreadSize(bool isAsync) {
        uint logicalThreads = std::thread::hardware_concurrency();
        uint availableThreads = !isAsync
                                ? logicalThreads * ceil(sqrt(logicalThreads * 2))
                                : floor(logicalThreads * pow(logicalThreads, 0.25));
        uint result = std::max(availableThreads, MIN_THREADS);
        return result;
    }

    uint Limiter::getThreadNumbers() const {
        return this->threadNumbers;
    }

    uint Limiter::getAsyncThreads() const {
        return this->asyncThreads;
    }

    Limiter::Limiter() {
        this->threadNumbers = Limiter::computeThreadSize();
        this->asyncThreads = Limiter::computeThreadSize(true);

        this->maxThreads = Limiter::calculateSize(this->threadNumbers, MIN_THREADS, this->workQueueThreshold);
        this->maxAsync = Limiter::calculateSize(this->asyncThreads, MIN_THREADS, this->asyncWorkQueueThreshold);
    }
}