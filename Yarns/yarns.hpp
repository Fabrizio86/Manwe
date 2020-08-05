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
        [[nodiscard]] uint getThreadsCount() const;

    private:
        /// \brief Default constructor, internal initialization
        Yarns();

        /// \brief the function used by each threads to pull work from
        static void workCycle();

        /// \brief the work cycle for the async tasks
        static void backgroundWorkCycle();

        /// \brief compute the mac number of items in queue to span new threads
        /// \param asyncThreads
        void computeThresholds(uint asyncThreads);

        /// \brief computes the thread count for the async operations
        static uint computeAsyncThreads();

        /// \brief computes the max size for the queues before new threads are spawned or tasks rearranged
        /// \return the number of threads to create
        static uint computeForegroundThreads();

    private:
        uint workQueueThreshold{};
        uint asyncQueueThreshold{};
        uint maxThreads{};
        uint maxAsync{};

        Queue<sITask> workQueue;
        Queue<Task> asyncQueue;
        Fibers fibers;
    };

}

#endif // YARNS_H
