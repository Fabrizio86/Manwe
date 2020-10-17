//
// Created by Fabrizio Paino on 2020-08-16.
//

#ifndef YARNS_FIBER_H
#define YARNS_FIBER_H

#include "IFiber.h"
#include "Definitions.h"

namespace YarnBall {

    class Fiber final : public IFiber {
    public:
        ///\brief prevent copy constructor
        Fiber(const Fiber &) = delete;

        /// \brief prevent move semantics
        Fiber(Fiber &&) = delete;

        explicit Fiber(uint upperLimit);

        ~Fiber();

        State getState();

        void stop();

        ThreadId id();

        void detach() override;

        void join() override;

        Workload addWork(ITask* work);

        Workload addWork(sITask work) override;

        size_t queueSize();

        void markAsTemp() override;

        Workload getWorkload() override;

        sITask stealWork();

        virtual void MarkAsync() override;

    private:
        bool isTemp{false};
        bool isAsync{false};
        std::atomic<State> state{};
        uint queueThreshold{};
        uint upperLimit{};
        std::thread thread;
        std::mutex mu;
        std::condition_variable condition;
        std::deque<sITask> queue;

        friend class Scheduler;

    private:
        void work();

        void clearQueue();

        void wait();

        bool conditional();
    };

}

#endif //YARNS_FIBER_H
