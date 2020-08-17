//
// Created by Fabrizio Paino on 2020-09-01.
//

#ifndef YARNS_LIMITER_H
#define YARNS_LIMITER_H

#include "Definitions.h"

namespace YarnBall {

    class Limiter {
    public:
        Limiter();

        [[nodiscard]] uint getMaxThreads() const;

        [[nodiscard]] uint getMaxAsync() const;

        [[nodiscard]] uint getWorkQueueThreshold() const;

        [[nodiscard]] uint getAsyncWorkQueueThreshold() const;

        [[nodiscard]] uint getThreadNumbers() const;

        [[nodiscard]] uint getAsyncThreads() const;

    private:
        /// Compute the starting size of the thread pool within range and the max workload per thread
        /// \param base The base value of threads
        /// \param max The max number of threads
        /// \param maxWorkload Sets the max work items per threads before is overburden
        /// \return the number of threads to create
        static uint calculateSize(uint base, uint max, uint &maxWorkload);

        /// \brief computes the max size for the queues before new threads are spawned or tasks rearranged
        /// \param isAsync switch the computation to the async logic
        /// \return the number of threads to create
        static uint computeThreadSize(bool isAsync = false);

    private:
        uint threadNumbers{};
        uint asyncThreads{};
        uint maxThreads{};
        uint maxAsync{};
        uint workQueueThreshold{};
        uint asyncWorkQueueThreshold{};
    };

}

#endif //YARNS_LIMITER_H
