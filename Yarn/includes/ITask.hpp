#ifndef ITASK_H
#define ITASK_H

#include "Definitions.hpp"

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

    };

}

#endif
