//
// Created by Fabrizio Paino on 2022-01-16.
//

#ifndef YARN_YARNBALL_HPP
#define YARN_YARNBALL_HPP

#include "ITask.hpp"
#include "IWaitable.hpp"

namespace YarnBall {

    void Run(sITask task);

    void Post(sIWaitable operation);

}

#endif //YARN_YARNBALL_HPP
