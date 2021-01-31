//
// Created by fabrizio on 2020-08-03.
//

#ifndef YARNS_DEFINITIONS_H
#define YARNS_DEFINITIONS_H

#include "itask.hpp"
#include "queue.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <list>
#include <mutex>

namespace YarnBall {

#ifndef uint
    using uint = unsigned int;
#endif

    using Locker = std::unique_lock<std::mutex>;

    /// \brief Definition for a task
    using Task = std::function<void()>;

    ///\brief Shorthand helper shared pointers
    using sIWaitable = std::shared_ptr<class IAwaitable>;

    ///\brief Shorthand helper shared pointers
    using sITask = std::shared_ptr<ITask>;

    using sIScheduler = std::shared_ptr<class IScheduler>;

    using sFiber = std::shared_ptr<class Fiber>;

    using Fibers = std::list<sFiber>;

    using FiberId = std::thread::id;

    using WorkQueue = Queue<sITask>;

    using ThreadId = std::thread::id;
}

#endif //YARNS_DEFINITIONS_H
