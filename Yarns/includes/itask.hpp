#ifndef ITASK_H
#define ITASK_H

#include <exception>
#include <functional>
#include <memory>
#include <thread>

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
        std::thread::id createdBy{ std::this_thread::get_id() };
    };
}

#endif
