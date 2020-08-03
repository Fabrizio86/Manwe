#ifndef IWAITABLE_H
#define IWAITABLE_H

#include "itask.hpp"

namespace YarnBall {

///\brief Interface for waitable tasks
    class IWaitable : public ITask {
    public:

        ///\brief Destructor
        virtual ~IWaitable() = default;

        ///\brief Wait method
        virtual void wait() = 0;

        ///\brief Returns the exception pointer
        /// \return instance of exception_ptr
        virtual std::exception_ptr getException() = 0;

    private:
        // change access to the public
        virtual void run() = 0;

        // change access to the public
        virtual void exception(std::exception_ptr exception) = 0;
    };

///\brief Shorthand helper shared pointers
    using sIWaitable = std::shared_ptr<IWaitable>;

}

#endif // IWAITABLE_H
