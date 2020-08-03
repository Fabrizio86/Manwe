#ifndef YARNS_H
#define YARNS_H

#include "itask.hpp"
#include "queue.hpp"

#include <thread>
#include <vector>

namespace YarnBall {

    using Fibers = std::vector<std::thread>;

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
        void addTask(sITask task);

        /// \brief submit a fire and forget task
        /// \param task
        void invoke(Task task);

        /// \brief stops the thread pool
        void stop();

        /// \brief get max thread count
        /// \return returns the number of threads
        unsigned int getThreadsCount() const ;

    private:
        /// \brief Default constructor, internal initialization
        Yarns();

        /// \brief the function used by each threads to pull work from
        static void workCycle();
        static void backgroundWorkCycle();

    private:
        unsigned int cpuCores;
        unsigned int foregroundThreads;

        Queue<sITask> workQueue;
        Queue<Task> asyncQueue;
        Fibers fibers;
        Fibers backgroundFibers;
    };

}

#endif // YARNS_H
