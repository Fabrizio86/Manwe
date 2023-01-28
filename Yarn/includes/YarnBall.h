//
// Created by Fabrizio Paino on 2022-01-16.
//

#ifndef YARN_YARNBALL_H
#define YARN_YARNBALL_H

#include "ITask.h"
#include "IWaitable.h"

namespace YarnBall {

    void Run(sITask task);

    void Post(sIWaitable operation);

}

#endif //YARN_YARNBALL_H
