//
// Created by Fabrizio Paino on 2022-01-16.
//

#include "RandomScheduler.hpp"

namespace YarnBall {

    int RandomScheduler::ThreadIndex(int maxValue) {
        std::uniform_int_distribution<int> fiberIndex(0, maxValue - 1);
        return fiberIndex(this->randomEngine);
    }

    RandomScheduler::RandomScheduler() : randomEngine(device()) {}

}