#include "yarns.hpp"
#include "waitable.hpp"

namespace YarnBall {

using namespace std;
using uint = unsigned int;

static const uint HARDWARE_SIZE = std::max(thread::hardware_concurrency(), 2u);
static const uint CYCLES = 100;
static const uint TOTAL_CYCLES = HARDWARE_SIZE * CYCLES;

Yarns::Yarns() {
    for(uint i = 0; i < HARDWARE_SIZE; ++i) {
        thread trd([&]{
            sITask task = nullptr;

            // while the queue is valid, keep taking work
            while(this->queue.isValid()) {
                task = this->queue.get();

                // cancellation point
                if(this->queue.isValid() == false) {
                    return;
                }

                // if we have a task
                if(task != nullptr) {
                    try{
                        // run it
                        task->run();
                    }
                    catch(...) {
                        // pass any exception to the implementation
                        task->exception(current_exception());
                    }
                }
            }
        });

        this->fibers.push_back(move(trd));
    }
}

Yarns *Yarns::instance() {
    static Yarns instance;
    return &instance;
}

void Yarns::addTask(sITask task) {
    this->queue.push(task);
}

sIWaitable Yarns::invoke(Task task) {
    // create shared pointer
    sIWaitable wt = make_shared<Waitable>(task);
    sITask si = wt;
    // submit the operation
    this->addTask(si);

    return wt;
}

unsigned int Yarns::getMaxThreads() const {
    return HARDWARE_SIZE;
}

void Yarns::stop() {
    // if not valid queue return
    if(!this->queue.isValid()) {
        return;
    }

    this->queue.clear();

    // give enough time to CPU to wake up or do contecxt switching
    this_thread::sleep_for(chrono::milliseconds(TOTAL_CYCLES));

    // join all the threads
    for(auto& thread : this->fibers) {
        if(thread.joinable()) {
            thread.join();
        }
    }
}

Yarns::~Yarns() {
    this->stop();
}

}
