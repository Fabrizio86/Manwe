//
// Created by Fabrizio Paino on 2022-01-14.
//

#ifndef YARN_YARN_H
#define YARN_YARN_H

#include "Fiber.h"
#include "ITask.h"
#include "IScheduler.h"

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

        void SwitchScheduler(IScheduler *scheduler);

    private:
        Yarn();

        static int ComputeThreadCount();

        sFiber find(FiberId id);

        void shiftWork(sFiber currentFiber, sITask task);

        sFiber get(int index);

        void terminateFiber(FiberId id);

        Fibers fibers;
        sQueues queues;
        IScheduler *scheduler;
    };

}

#endif //YARN_YARN_H
