#include "yarns.hpp"
#include "system.hpp"
#include <utility>
#include <cmath>

namespace YarnBall {

#define CYCLES 100
#define MIN_THREADS 4u
#define MIN_TEMP_WORKER 2u
#define LIFE_SPAN 6

    using namespace std;

    void Yarns::doWork() {
        sITask task = Yarns::instance()->workQueue.get();

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

    void Yarns::asyncDoWork() {
        Task task = Yarns::instance()->asyncQueue.get();

        // if we have a task
        if (task != nullptr) {
            try {
                // run it
                task();
            }
            catch (...) {}
        }
    }

    void Yarns::workCycle() {
        // while the workQueue is valid, keep taking work
        while (Yarns::instance()->workQueue.isValid()) {
            Yarns::doWork();
        }
    }

    void Yarns::tempWorkCycle() {
        uint lifeSpan = LIFE_SPAN;

        // while the workQueue is valid, keep taking work
        while (lifeSpan) {
            Yarns::doWork();

            if(Yarns::instance()->workQueue.size() == 0)
                break;

            // if we are still over the limit, keep this thread running
            lifeSpan = Yarns::instance()->workQueue.size() >= Yarns::instance()->workQueueThreshold ?
                       LIFE_SPAN :
                       lifeSpan - 1;
        }

        Yarns::instance()->tempThreads--;
    }

    void Yarns::tempAsyncWorkCycle() {
        uint lifeSpan = LIFE_SPAN;

        // while the workQueue is valid, keep taking work
        while (Yarns::instance()->asyncQueue.isValid()) {
            // if our time is up
            if (lifeSpan == 0) break;

            Yarns::asyncDoWork();

            // if we are still over the limit, keep this thread running
            lifeSpan = Yarns::instance()->asyncQueue.size() >= Yarns::instance()->asyncQueueThreshold ?
                       LIFE_SPAN :
                       lifeSpan - 1;
        }

        Yarns::instance()->tempAsyncThreads--;
    }

    void Yarns::backgroundWorkCycle() {
        // while the workQueue is valid, keep taking work
        while (Yarns::instance()->asyncQueue.isValid()) {
            Yarns::asyncDoWork();
        }
    }

    Yarns::Yarns() {
        uint threadNumbers = Yarns::computeForegroundThreads();

        // create all the work threads
        for (uint i = 0; i < threadNumbers; ++i) {
            thread trd(Yarns::workCycle);
            this->fibers.push_back(move(trd));
        }

        this->asyncThreads = YarnBall::Yarns::computeAsyncThreads();

        // create all the background services tasks
        for (uint i = 0; i < asyncThreads; ++i) {
            thread trd(Yarns::backgroundWorkCycle);
            trd.detach();
        }

        this->computeThresholds();
    }

    Yarns *Yarns::instance() {
        static Yarns instance;
        return &instance;
    }

    void Yarns::edgeDetectedForeground() {
        uint flr = floor(sqrt(this->fibers.size()));
        uint tmpThreads = std::max(flr, MIN_TEMP_WORKER);
        uint totalThreads = this->tempThreads + this->fibers.size() + tmpThreads;

        // if we reach the limit of threads return
        if (totalThreads > this->maxThreads) return;

        for (int i = 0; i < tmpThreads; ++i) {
            thread trd(Yarns::tempWorkCycle);
            trd.detach();
            this->tempThreads++;
        }
    }

    void Yarns::edgeDetectedAsync() {
        uint flr = floor(sqrt(this->asyncThreads));
        uint tmpThreads = std::max(flr, MIN_TEMP_WORKER);
        uint totalThreads = this->tempAsyncThreads + this->asyncThreads + tmpThreads;

        // if we reach the limit of threads return
        if (totalThreads > this->maxAsync) return;

        for (int i = 0; i < tmpThreads; ++i) {
            thread trd(Yarns::tempWorkCycle);
            trd.detach();
            this->tempAsyncThreads++;
        }
    }

    void Yarns::submit(sITask task) {
        this->workQueue.push(std::move(task));

        if (this->workQueue.size() >= this->workQueueThreshold) {
            this->edgeDetectedForeground();
        }
    }

    void Yarns::invoke(Task task) {
        // submit the operation
        this->asyncQueue.push(std::move(task));
        uint asyncQueueSize = this->asyncQueue.size();

        if (asyncQueueSize >= this->asyncQueueThreshold) {
            this->edgeDetectedAsync();
        }
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

    void Yarns::computeThresholds() {
        uint maxWorkQueue = floor(sqrt(this->fibers.size()));
        uint maxAsyncQueue = floor(sqrt(this->asyncThreads));

        this->workQueueThreshold = this->fibers.size() * maxWorkQueue;
        this->asyncQueueThreshold = this->asyncThreads * maxAsyncQueue;

        this->maxAsync = this->asyncThreads * sqrt(this->asyncThreads * MIN_THREADS);
        this->maxThreads = this->fibers.size() * sqrt(this->fibers.size() * MIN_THREADS);

        // todo: at some point if the system is still locked and bloated we need to start kill blocking threads
        // todo: so we need a timer/signal way to check life-span of a thread (subclassing from std::thread?)
    }

    uint Yarns::getQueueThreshold() const {
        return this->workQueueThreshold;
    }

    uint Yarns::getAsyncQueueThreshold() const {
        return this->asyncQueueThreshold;
    }

    uint Yarns::getMaxThreads() const {
        return this->maxThreads;
    }

    uint Yarns::getMaxAsync() const {
        return this->maxAsync;
    }

    int Yarns::size() const {
        return this->fibers.size();
    }

    int Yarns::asyncSize() const {
        return this->asyncThreads;
    }
}
