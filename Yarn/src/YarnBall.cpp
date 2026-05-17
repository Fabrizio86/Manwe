//
// Created by Fabrizio Paino on 2022-01-16.
//

#include "YarnBall.hpp"
#include "Yarn.hpp"

namespace YarnBall {

    void run(uITask task) {
        Yarn::instance()->run(std::move(task));
    }

    void run(sITask task) {
        Yarn::instance()->run(std::move(task));
    }

    void post(sIWaitable operation) {
        // shared_ptr<IWaitable> doesn't implicitly convert to shared_ptr<ITask>;
        // static_pointer_cast keeps the control block intact and lets the
        // caller observe completion through their original sIWaitable.
        Yarn::instance()->run(std::static_pointer_cast<ITask>(std::move(operation)));
    }
}
