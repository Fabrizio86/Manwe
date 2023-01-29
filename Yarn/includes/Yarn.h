//
// Created by Fabrizio Paino on 2022-01-14.
//

#ifndef YARN_YARN_H
#define YARN_YARN_H

#include "Fiber.h"
#include "ITask.h"
#include "IScheduler.h"

#include <mutex>

namespace YarnBall {

    class Yarn final {
    public:
        ///\brief prevent copy constructor
        Yarn(const Yarn &) = delete;

        /// \brief prevent move semantics
        Yarn(Yarn &&) = delete;

        /// \brief prevent copying assignment
        Yarn &operator=(const Yarn &) = delete;

        /// \brief prevent move assignment
        Yarn &operator=(Yarn &&) = delete;

        ~Yarn();

        static Yarn *instance();

        void Run(sITask task);

        void SwitchScheduler(sIScheduler scheduler);

    private:
        Yarn();

        static int MaxThreadCount;
        static int MinThreadCount;

        void shiftWork(const sFiber& currentFiber, sITask task);
        bool maxLimitReached();
        void initializeNewThread(FiberId id, bool markAsTemp = false);
        void arrangeQueues(FiberId currentQueueId, FiberId newQueueId);
        FiberId firstUnusedId();

        void terminateFiber(FiberId id);

        Fibers fibers;
        sQueues queues;
        sQueue pending;
        sIScheduler scheduler;
        std::mutex mu;
        std::mutex cmu;
        std::mutex dmu;
    };

}

#endif //YARN_YARN_H
