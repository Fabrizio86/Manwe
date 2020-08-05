#include "yarns.hpp"
#include "waitable.hpp"
#include <utility>
#include <cmath>

namespace YarnBall {

#define CYCLES 100
#define MIN_THREADS 4u

    using namespace std;

    void Yarns::workCycle() {
        sITask task = nullptr;

        // while the workQueue is valid, keep taking work
        while (Yarns::instance()->workQueue.isValid()) {
            task = Yarns::instance()->workQueue.get();

            // cancellation point
            if (!Yarns::instance()->workQueue.isValid()) {
                return;
            }

            // if we have a task
            if (task != nullptr) {
                try {
                    // run it
                    task->run();
                }
                catch (...) {
                    // pass any exception to the implementation
                    task->exception(current_exception());
                }
            }
        }
    }

    void Yarns::backgroundWorkCycle() {
        Task task = nullptr;

        // while the workQueue is valid, keep taking work
        while (Yarns::instance()->asyncQueue.isValid()) {
            task = Yarns::instance()->asyncQueue.get();

            // if we have a task
            if (task != nullptr) {
                try {
                    // run it
                    task();
                }
                catch (...) {}
            }
        }
    }

    Yarns::Yarns() {
        uint threadNumbers = Yarns::computeForegroundThreads();

        // create all the work threads
        for (uint i = 0; i < threadNumbers; ++i) {
            thread trd(Yarns::workCycle);
            this->fibers.push_back(move(trd));
        }

        uint asyncThreads = YarnBall::Yarns::computeAsyncThreads();

        // create all the background services tasks
        for (uint i = 0; i < asyncThreads; ++i) {
            thread trd(Yarns::backgroundWorkCycle);
            trd.detach();
        }

        this->computeThresholds(asyncThreads);
    }

    Yarns *Yarns::instance() {
        static Yarns instance;
        return &instance;
    }

    void Yarns::addTask(sITask task) {
        this->workQueue.push(task);

        // todo: after adding to queue do the threshold check
    }

    void Yarns::invoke(Task task) {
        // submit the operation
        this->asyncQueue.push(std::move(task));

        // todo: after adding to queue do the threshold check
    }

    unsigned int Yarns::getThreadsCount() const {
        return this->fibers.size();
    }

    void Yarns::stop() {
        // if workQueue is not valid we have already cleared the threads
        if (!this->workQueue.isValid()) {
            return;
        }

        this->workQueue.clear();
        this->asyncQueue.clear();

        // todo: this needs new logic now, or it will wait too long
        uint waitCycle = this->fibers.size() * CYCLES;

        // give enough time to CPU to wake up or do context switching
        this_thread::sleep_for(chrono::milliseconds(waitCycle));

        // join all the threads
        for (auto &thread : this->fibers) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    Yarns::~Yarns() {
        this->stop();
    }

    uint Yarns::computeForegroundThreads() {
        uint logicalThreads = thread::hardware_concurrency();
        uint availableThreads = logicalThreads * ceil(sqrt(logicalThreads * 2));
        uint result = std::max(availableThreads, MIN_THREADS);
        return result;
    }

    uint Yarns::computeAsyncThreads() {
        uint nCores = thread::hardware_concurrency();
        uint cores = floor(nCores * pow(nCores * 2, 1 / 4));
        uint result = std::max(cores, MIN_THREADS);
        return result;
    }

    void Yarns::computeThresholds(uint asyncThreads) {
        uint maxWorkPerQueue = floor(sqrt(this->fibers.size()));
        this->workQueueThreshold = this->fibers.size() * maxWorkPerQueue;
        this->maxAsync = asyncThreads * MIN_THREADS;
        this->maxThreads = this->fibers.size() * MIN_THREADS;

        // todo: at some point if the system is still locked and bloated we need to start kill blocking threads
        // todo: so we need a timer/signal way to check life-span of a thread (subclassing from std::thread?)
    }
}
