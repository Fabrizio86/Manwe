//
// Created by Fabrizio Paino on 2020-08-25.
//

#include "BaseThread.h"

namespace YarnBall {

    using namespace std::chrono_literals;

    BaseThread::BaseThread(uint upperLimit) : upperLimit(upperLimit),
                                              state(State::Running) {
        this->thread = std::thread(&BaseThread::work, this);
        this->queueThreshold = this->upperLimit * 0.5;
    }

    void BaseThread::detach() {
        this->thread.detach();
    }

    Workload BaseThread::getWorkload() {
        size_t size = this->queueSize();

        return size >= this->upperLimit ? Workload::Exhausted
                                        : size >= this->queueThreshold && size < this->upperLimit
                                          ? Workload::Taxed : Workload::Normal;
    }

    void BaseThread::stop() {
        // if we have already exited, no need to go further
        if (this->state == State::Aborting) return;

        this->state = State::Aborting;
        this->clearQueue();
        this->condition.notify_one();
    }

    BaseThread::~BaseThread() {
        this->stop();

        if (this->thread.joinable()) {
            this->thread.join();
        }
    }

    void BaseThread::setState(State workState) {
        this->state = workState;
    }

    State BaseThread::getState() {
        return this->state;
    }

    std::thread::id BaseThread::id() {
        return this->thread.get_id();
    }

    size_t BaseThread::queueSize() {
        return 0;
    }

    void BaseThread::wait() {
        if (this->state == State::Aborting) return;

        Locker lk(this->mu);

        if (this->temp) {
            bool expired = !this->condition.wait_for(lk, 2s, [this] { return conditional(); });
            if (expired) {
                this->setState(State::Aborting);
            }
        } else {
            this->condition.wait(lk, [this] { return conditional(); });
        }
    }

    void BaseThread::work() {}

    void BaseThread::clearQueue() {}

    bool BaseThread::conditional() {
        auto empty = this->queueSize() != 0;
        auto aborting = this->getState() == State::Aborting;
        return aborting || empty;
    }

    void BaseThread::isTemp() {
        this->temp = true;
    }

    void BaseThread::join() {
        if (this->thread.joinable())
            this->thread.join();
    }
}
