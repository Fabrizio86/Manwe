#ifndef IWAITABLE_H
#define IWAITABLE_H

#include "itask.hpp"

namespace YarnBall {

///\brief Interface for waitable tasks
class IWaitable : public ITask {
public:

    ///\brief Destructor
    virtual ~IWaitable() override;

    ///\brief Wait method
    virtual void wait() = 0;

    ///\brief Returns the exception pointer
    /// \return instance of exception_ptr
    virtual std::exception_ptr getException() = 0;

protected:

    ///\brief The method called inside the thread
    virtual void run() override = 0;

    ///\brief Handles exceptions
    virtual void exception(std::exception_ptr exception) override = 0;
};

///\brief Shorthand helper shared pointers
using sIWaitable = std::shared_ptr<IWaitable>;

}

#endif // IWAITABLE_H
