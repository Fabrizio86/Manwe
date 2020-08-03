#include "yarns.hpp"
#include "waitable.hpp"

#include <utility>
#include <sys/sysctl.h>
#include <unistd.h>

namespace YarnBall {

#define CYCLES 100

    using namespace std;
    using uint = unsigned int;

    // TODO: this needs to work on windows as well at some point
    int coreCount(){
        int cores;
        int numCPU;
        std::size_t len = sizeof(cores);
        cores = CTL_HW;
        sysctl(&cores, 1, &numCPU, &len, nullptr, 0);

        return cores;
    }

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
                catch (...) { }
            }
        }
    }

    Yarns::Yarns() : cpuCores(coreCount()) {
        uint minThreadCount = 4;
        uint availableThreads = thread::hardware_concurrency() * 2;
        this->foregroundThreads = std::max(availableThreads, minThreadCount);

        // create all the work threads
        for (uint i = 0; i < this->foregroundThreads; ++i) {
            thread trd(Yarns::workCycle);
            this->fibers.push_back(move(trd));
        }

        // create all the background services tasks
        for(uint i = 0; i < this->cpuCores; ++i) {
            thread trd(Yarns::backgroundWorkCycle);
            trd.detach();
            this->backgroundFibers.push_back(move(trd));
        }
    }

    Yarns *Yarns::instance() {
        static Yarns instance;
        return &instance;
    }

    void Yarns::addTask(sITask task) {
        this->workQueue.push(std::move(task));
    }

    void Yarns::invoke(Task task) {
        // submit the operation
        this->asyncQueue.push(std::move(task));
    }

    unsigned int Yarns::getThreadsCount() const {
        return this->foregroundThreads;
    }

    void Yarns::stop() {
        // if workQueue is not valid we have already cleared the threads
        if (!this->workQueue.isValid()) {
            return;
        }

        this->workQueue.clear();
        this->asyncQueue.clear();

        uint waitCycle = this->foregroundThreads * CYCLES;

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
}
