//
// Created by Fabrizio Paino on 2022-01-16.
//

#include "YarnBall.hpp"
#include "Yarn.hpp"
#include "Waitable.hpp"

namespace YarnBall {

    void Run(sITask task) {
        Yarn::instance()->Run(task);
    }

    void Post(sIWaitable operation) {
        Yarn::instance()->Run(operation);
    }
}