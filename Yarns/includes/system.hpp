#ifndef SYSTEM_H
#define SYSTEM_H

#include "Definitions.h"
#include "iawaitable.hpp"
#include "itask.hpp"
#include "yarns.hpp"

namespace YarnBall {

    /// \brief post a task for execution and returns a handler to wait completion.
    template<class T>
    sIWaitable Promise(){
        auto wt = std::make_shared<T>();
        Yarns::instance()->submit(wt);
        return wt;
    }

    /// \brief submit a task for execution
    template<class T>
    void Submit(){
        auto sTask = std::make_shared<T>();
        Yarns::instance()->submit(sTask);
    }

    /// \brief invoke a background system task
    void Invoke(Task task);
}

#endif // SYSTEM_H
