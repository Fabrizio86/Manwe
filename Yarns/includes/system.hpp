#ifndef SYSTEM_H
#define SYSTEM_H

#include "Definitions.h"
#include "iawaitable.hpp"
#include "itask.hpp"

namespace YarnBall {

    /// \brief post a task for execution and returns a handler to wait completion.
    sIWaitable Promise(Task task);

    /// \brief submit a task for execution
    void Submit(ITask *task);

    /// \brief invoke a background system task
    void Invoke(Task task);


}

#endif // SYSTEM_H
