//
// Created by Fabrizio Paino on 2020-08-16.
//

#ifndef YARNS_WORKER_H
#define YARNS_WORKER_H

#include "Definitions.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace YarnBall {

    class Worker {
    public:
        Worker();
        ~Worker();

        [[nodiscard]] State getState() const;

        void addWork(sITask work);
        void stop();
        sITask stealWork();

    private:
        std::mutex mu;
        std::condition_variable condition;
        std::atomic<State> state;
        std::deque<sITask> queue;

        std::thread thread;

        void wait();
        void work();

    };

}

#endif //YARNS_WORKER_H
