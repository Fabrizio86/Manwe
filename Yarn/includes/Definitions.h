//
// Created by Fabrizio Paino on 2022-03-29.
//

#ifndef YARN_DEFINITIONS_H
#define YARN_DEFINITIONS_H

#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>

namespace YarnBall {

    class ITask;

    using FiberId = std::thread::id;
    using Operation = std::function<void()>;
    using SignalDone = std::function<void(FiberId)>;
    using OsHandler = std::thread::native_handle_type;

    using sITask = std::shared_ptr<ITask>;
    using Queue = std::deque<sITask>;
    using sQueue = std::shared_ptr<Queue>;
    using sQueues = std::unordered_map<FiberId, sQueue>;

    using Locket = std::unique_lock<std::mutex>;
}

#endif //YARN_DEFINITIONS_H
