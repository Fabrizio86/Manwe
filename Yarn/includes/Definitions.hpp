//
// Created by Fabrizio Paino on 2022-03-29.
//

#ifndef YARN_DEFINITIONS_HPP
#define YARN_DEFINITIONS_HPP

#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
#include "IScheduler.hpp"

namespace YarnBall {

    class ITask;

    using sITask = std::shared_ptr<ITask>;
    using FiberId = unsigned int;
    using Operation = std::function<void()>;
    using SignalDone = std::function<void(FiberId)>;
    using AnyPendingTasks = std::function<bool(void)>;
    using GetFromPending = std::function<void(FiberId)>;

    using OsHandler = std::thread::native_handle_type;

    using Queue = std::deque<sITask>;
    using sQueue = std::shared_ptr<Queue>;
    using sQueues = std::vector<sQueue>;
    using sIScheduler = std::shared_ptr<IScheduler>;

    using Locket = std::unique_lock<std::mutex>;
}

#endif //YARN_DEFINITIONS_HPP
