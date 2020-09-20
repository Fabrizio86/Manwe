//
// Created by Fabrizio Paino on 2020-08-25.
//

#ifndef YARNS_BASETHREAD_H
#define YARNS_BASETHREAD_H

#include "Definitions.h"
#include "FiberState.h"
#include "IFiber.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace YarnBall {

    class BaseThread {
    public:
        BaseThread() = delete;

        ///\brief prevent copy constructor
        BaseThread(const BaseThread &) = delete;

        /// \brief prevent move semantics
        BaseThread(BaseThread &&) = delete;

        /// \brief prevent copying assignment
        BaseThread &operator=(const BaseThread &) = delete;

        /// \brief prevent move assignment
        BaseThread &operator=(BaseThread &&) = delete;

        explicit BaseThread(uint upperLimit);

        virtual ~BaseThread();

        State getState() const;

        Workload getWorkload();

        void stop();

        std::thread::id id();

        void isTemp();

    protected:
        std::mutex mu;

        std::condition_variable condition;

        void detach();

        void setState(State workState);

        State getState();

        void wait();

        virtual size_t queueSize();

        virtual void work();

        virtual void clearQueue();

    private:
        std::atomic<State> state{};
        uint queueThreshold{};
        uint upperLimit{};
        std::thread thread;
        bool temp{false};

        bool conditional();
    };
}

#endif //YARNS_BASETHREAD_H
