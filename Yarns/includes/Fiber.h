//
// Created by Fabrizio Paino on 2020-08-16.
//

#ifndef YARNS_FIBER_H
#define YARNS_FIBER_H

#include "Definitions.h"
#include "FiberState.h"

namespace YarnBall {

    class Fiber final {
    public:
        ///\brief prevent copy constructor
        Fiber(const Fiber &) = delete;

        /// \brief prevent move semantics
        Fiber(Fiber &&) = delete;

        explicit Fiber(uint upperLimit, bool temp = false);

        ~Fiber();

        void stop();

        ThreadId id();

        void detach();

        void join();

        void addWork(sITask work);

        size_t queueSize();

        Workload getWorkload();

        State getState();

        sITask stealWork();

        bool isDetached() const;

    private:
        bool temp{false};
        State state;
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
