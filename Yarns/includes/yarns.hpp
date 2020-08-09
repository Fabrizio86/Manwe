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
        void submit(sITask task);

        /// \brief submit a fire and forget task
        /// \param task
        void invoke(Task task);

        /// \brief returns the value at which the system triggers spawning of new threads
        [[nodiscard]] uint getQueueThreshold() const;

        /// \brief returns the value at which the system triggers spawning of new async threads
        [[nodiscard]] uint getAsyncQueueThreshold() const;

        /// \brief returns the max allowed number of threads for this system
        [[nodiscard]] uint getMaxThreads() const;

        /// \brief returns the max allowed number of background threads for this system
        [[nodiscard]] uint getMaxAsync() const;

    private:
        /// \brief Default constructor, internal initialization
        Yarns();

        /// \brief stops the thread pool
        void stop();

        /// \brief the function used by each threads to pull work from
        static void workCycle();

        /// \brief work cycle for the temporary threads
        static void tempWorkCycle();

        /// \brief work cycle for the temporary background threads
        static void tempAsyncWorkCycle();

        /// \brief perform the work on the submitted task
        static void doWork();

        /// \brief perform the work on the submitted task
        static void asyncDoWork();

        /// \brief the work cycle for the async tasks
        static void backgroundWorkCycle();

        /// \brief compute the max number of items in queue to span new threads
        void computeThresholds();

        /// \brief computes the thread count for the async operations
        static uint computeAsyncThreads();

        /// \brief computes the max size for the queues before new threads are spawned or tasks rearranged
        /// \return the number of threads to create
        static uint computeForegroundThreads();

        /// \brief detects and spawn new threads
        void edgeDetectedForeground();

        /// \brief detect and spawn new background threads
        void edgeDetectedAsync();

    private:
        uint workQueueThreshold{};
        uint asyncQueueThreshold{};
        uint maxThreads{};
        uint maxAsync{};
        uint asyncThreads{};
        std::atomic<uint> tempThreads{};
        std::atomic<uint> tempAsyncThreads{};

        Queue<sITask> workQueue;
        Queue<Task> asyncQueue;
        Fibers fibers;
    };

}

#endif // YARNS_H
