#ifndef IAWAITABLE_H
#define IAWAITABLE_H

#include "Definitions.h"

#include <memory>

namespace YarnBall {

    ///\brief Interface for an awaitable tasks
    class IAwaitable {
    public:

        ///\brief Destructor
        virtual ~IAwaitable() = default;

        ///\brief Wait method
        virtual void wait() = 0;

        ///\brief Returns the exception pointer
        /// \return instance of exception_ptr
        virtual std::exception_ptr getException() = 0;
    };

}

#endif // IAWAITABLE_H
