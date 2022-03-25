//
// Created by Fabrizio Paino on 2022-01-16.
//

#include "YarnBall.h"
#include "Yarn.h"
#include "Waitable.h"

namespace YarnBall {

    void Run(ITask *task) {
        std::shared_ptr<ITask> sTask(task);
        Yarn::instance()->Run(sTask);
    }

    void Run(sITask task) {
        Yarn::instance()->Run(task);
    }

    sIWaitable Post(Operation operation) {
        auto waitable = std::make_shared<Waitable>(operation);
        Run(waitable);
        return waitable;
    }
}