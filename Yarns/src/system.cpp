#include "system.hpp"
#include "yarns.hpp"
#include "Awaitable.h"

namespace YarnBall {

    std::thread::id ITask::id() { return this->createdBy; }

    void Invoke(Task task) {
        Yarns::instance()->invoke(std::move(task));
    }
}
