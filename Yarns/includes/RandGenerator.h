//
// Created by Fabrizio Paino on 2020-08-26.
//

#ifndef YARNS_RANDGENERATOR_H
#define YARNS_RANDGENERATOR_H

#include <random>

namespace YarnBall {

    class RandGenerator final {
    public:
        RandGenerator();
        ~RandGenerator() = default;

        int get(int max);

    private:
        std::random_device device;
        std::default_random_engine randomEngine;
    };

}

#endif //YARNS_RANDGENERATOR_H
