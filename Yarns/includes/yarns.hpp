#ifndef YARNS_H
#define YARNS_H

#include "Definitions.h"
#include "itask.hpp"
#include "IScheduler.h"
#include "Limiter.h"
#include "queue.hpp"

namespace YarnBall {

    ///\brief Light weight thread-pool class, for submitting tasks and async operations
    class Yarns final {
    public:
        ///\brief prevent copy constructor
        Yarns(const Yarns &) = delete;

        /// \brief prevent move semantics
        Yarns(Yarns &&) = delete;

        /// \brief prevent copying assignment
        Yarns &operator=(const Yarns &) = delete;

        /// \brief prevent move assignment
        Yarns &operator=(Yarns &&) = delete;

        ///\brief Default destructor
        ~Yarns();

        /// \brief Instance of the Yarns
        /// \return returns the instance of Yarns
        static Yarns *instance();

        /// Gets the pool limits
        /// \return instance of the limiter
        const Limiter* getLimits() const;

        size_t fiberSize() const;

        size_t aFiberSize() const;

        FiberId getFiberId(int index);

        FiberId getAsyncFiberId(int index);

    private:
        /// \brief Default constructor, internal initialization
        Yarns();

        Limiter limits;
        Fibers fibers;
        Fibers asyncFibers;

        friend class Scheduler;
    };

}

#endif // YARNS_H
