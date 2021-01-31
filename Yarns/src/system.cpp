#include "system.hpp"
#include "Scheduler.h"
#include "Awaitable.h"

namespace YarnBall {

    std::thread::id ITask::id() { return this->createdBy; }

    void Invoke(Task task) {
        Scheduler::instance()->invoke(task);
    }
}
