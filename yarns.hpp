#ifndef YARNS_H
#define YARNS_H

#include "iwaitable.hpp"
#include "queue.hpp"

#include <thread>
#include <vector>

namespace YarnBall {

    using Fibers = std::vector<std::thread>;

///\brief Light weight thread-pool class, for submitting tasks and async operations
    class Yarns final {
    public:
        ///\brief prevent copy and move semantics
        Yarns(const Yarns &) = delete;

        Yarns(Yarns &&) = delete;

        Yarns &operator=(const Yarns &) = delete;

        Yarns &operator=(Yarns &&) = delete;

        ///\brief Default destructor
        ~Yarns();

        /// \brief Instance of the Yarns
        /// \return returns the instance of Yarns
        static Yarns *instance();

        /// \brief add task to the execution queue
        /// \param task to execute
        void addTask(sITask task);

        ///\brief submit an async task
        ///\return returns waitable object
        sIWaitable invoke(Task task);

        /// \brief stops the thread pool
        void stop();

        /// \brief get max thread count
        /// \return returns the number of threads
        static unsigned int getMaxThreads() ;

    private:
        // Constructor
        Yarns();

    private:
        Queue queue;
        Fibers fibers;
    };

}

#endif // YARNS_H
