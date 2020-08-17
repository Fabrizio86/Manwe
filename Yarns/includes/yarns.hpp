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

        /// \brief add task to the execution workQueue
        /// \param task to execute
        void submit(sITask task);

        /// \brief submit a fire and forget task
        /// \param task
        void invoke(Task task);

        /// Gets the pool limits
        /// \return instance of the limiter
        const Limiter* getLimits() const;

    private:
        /// \brief Default constructor, internal initialization
        Yarns();

        /// \brief stops the thread pool
        void stop();

    private:
        Limiter limits;
        WorkQueue workQueue;
        AsyncQueue asyncQueue;
        IScheduler* scheduler;
        Fibers fibers;
        AsyncFibers asyncFibers;
    };

}

#endif // YARNS_H
