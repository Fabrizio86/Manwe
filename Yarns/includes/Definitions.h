//
// Created by fabrizio on 2020-08-03.
//

#ifndef YARNS_DEFINITIONS_H
#define YARNS_DEFINITIONS_H

#include "iawaitable.hpp"
#include "itask.hpp"
#include "queue.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace YarnBall {

#ifndef uint
    using uint = unsigned int;
#endif

    using Locker = std::unique_lock<std::mutex>;

    /// \brief Definition for a task
    using Task = std::function<void()>;

    ///\brief Shorthand helper shared pointers
    using sIWaitable = std::shared_ptr<IAwaitable>;

    ///\brief Shorthand helper shared pointers
    using sITask = std::shared_ptr<ITask>;

    using sIScheduler = std::shared_ptr<class IScheduler>;

    using sFiber = std::shared_ptr<class Fiber>;

    using sAsyncFiber = std::shared_ptr<class AsyncFiber>;

    using Fibers = std::vector<sFiber>;

    using AsyncFibers = std::vector<sAsyncFiber>;

    using FiberId = std::thread::id;

    using WorkQueue = Queue<sITask>;

    using AsyncQueue = Queue<Task>;
}

#endif //YARNS_DEFINITIONS_H
