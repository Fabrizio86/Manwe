//
// Created by Fabrizio Paino on 2020-08-26.
//

#include "RandGenerator.h"

namespace YarnBall {

    RandGenerator::RandGenerator() : randomEngine(device()) {}

    int RandGenerator::get(int max) {
        std::uniform_int_distribution<int> fiberIndex(0, max - 1);
        return fiberIndex(this->randomEngine);
    }
}