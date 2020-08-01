#ifndef ITASK_H
#define ITASK_H

#include <exception>
#include <functional>
#include <memory>

namespace YarnBall {

    using Task = std::function<void()>;

///\brief Interface for Yarn to call upon
    class ITask {
    public:
        ///\brief Virtual destructor
        virtual ~ITask() = default;

        ///\brief The method called inside the thread
        virtual void run() = 0;

        ///\brief Handles exceptions
        virtual void exception(std::exception_ptr exception) = 0;
    };

///\brief Shorthand helper shared pointers
    using sITask = std::shared_ptr<ITask>;

}

#endif // ITASK_H
