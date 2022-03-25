#ifndef ITASK_H
#define ITASK_H

#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>

namespace YarnBall {

    ///\brief Interface for Yarn to call upon
    class ITask {
    public:
        ///\brief Virtual destructor
        virtual ~ITask() = default;

        ///\brief The method called inside the thread
        virtual void run() = 0;

        ///\brief Handles exceptions
        virtual void exception(std::exception_ptr exception) = 0;

        std::thread::id id();

    private:
        std::thread::id createdBy{std::this_thread::get_id()};
    };

    using sITask = std::shared_ptr<ITask>;
    using Operation = std::function<void()>;
    using FiberId = std::thread::id;
    using Queue = std::deque<sITask>;
    using sQueue = std::shared_ptr<Queue>;
    using sQueues = std::unordered_map<FiberId, sQueue>;
    using SignalDone = std::function<void(FiberId)>;
    using OsHandler = std::thread::native_handle_type;

    class StopExecutionException {
    };
}

#endif
