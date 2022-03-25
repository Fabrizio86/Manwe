//
// Created by Fabrizio Paino on 2022-01-16.
//

#ifndef YARN_YARNBALL_H
#define YARN_YARNBALL_H

#include "ITask.h"
#include "IWaitable.h"

namespace YarnBall {

    void Run(ITask* task);

    void Run(sITask task);

    sIWaitable Post(Operation operation);

}

#endif //YARN_YARNBALL_H
