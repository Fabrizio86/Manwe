//
// Created by Fabrizio Paino on 2020-08-25.
//

#include "BaseThread.h"

namespace YarnBall {

    BaseThread::BaseThread(uint upperLimit) : upperLimit(upperLimit),
                                              state(State::Idle) {
        this->thread = std::thread(&BaseThread::work, this);
        this->queueThreshold = this->upperLimit * 0.5;
    }

    void BaseThread::detach() {
        this->thread.detach();
    }

    State BaseThread::getState() const {
        return this->state;
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

    void BaseThread::setIdleSince(const DateTime &idleSince) {
        this->idleSince = idleSince;
    }

    size_t BaseThread::queueSize() {
        return 0;
    }

    void BaseThread::wait() {
        if(this->state == State::Aborting) return;
        this->setIdleSince(std::chrono::system_clock::now());

        Locker lk(this->mu);

        this->condition.wait(lk, [this] {
            auto empty = this->queueSize() != 0;
            auto aborting = this->getState() == State::Aborting;
            return aborting || empty;
        });
    }

    void BaseThread::work() { }

    void BaseThread::clearQueue() { }
}
