//
// Created by Fabrizio Paino on 2022-01-16.
//

#ifndef YARN_RANDOMSCHEDULER_HPP
#define YARN_RANDOMSCHEDULER_HPP

#include <random>
#include "IScheduler.hpp"

namespace YarnBall {

    /**
     * @class RandomScheduler
     * @brief Default IScheduler implementation that returns uniformly random
     *        indices in @c [0, maxValue). Returns @c -1 for @c maxValue <= 0.
     */
    class RandomScheduler : public IScheduler {
    public:
        RandomScheduler();
        ~RandomScheduler() override = default;

        int ThreadIndex(int maxValue) override;

    private:
        /**
         * @brief Seed source. Used only at construction.
         */
        std::random_device device;

        /**
         * @brief PRNG state.
         */
        std::default_random_engine randomEngine;
    };

}

#endif //YARN_RANDOMSCHEDULER_HPP
