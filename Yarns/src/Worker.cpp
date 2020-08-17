//
// Created by Fabrizio Paino on 2020-08-16.
//

#include "Worker.h"

namespace YarnBall {

    using namespace std;

    Worker::Worker() : state(State::Idle) {
        this->thread = std::thread(&Worker::work, this);
    }

    State Worker::getState() const {
        return state;
    }

    void Worker::addWork(sITask work) {
        this->queue.push_back(move(work));
    }

    void Worker::wait() {
        // locking for item retrieval
        unique_lock<mutex> lk(this->mu);

        // wait for a task or the workQueue to be invalid
        this->condition.wait(lk, [this] {
            return !this->queue.empty() || this->state != State::Aborting;
        });
    }

    sITask Worker::stealWork() {
        if (this->queue.empty()) return nullptr;

        // we are stealing work lets make sure is protected.
        lock_guard<mutex> lk(this->mu);

        sITask task = this->queue.back();
        this->queue.pop_back();

        return task;
    }

    void Worker::work() {
        while (this->state != State::Aborting) {
            if (this->queue.empty()) {
                this->state = State::Waiting;
                this->wait();
            }

            // thread could have been stopped after waiting, lets just exit
            if(this->state == State::Aborting) {
                return;
            }

            sITask task = this->queue.front();
            this->queue.pop_front();

            try {
                this->state = State::Running;
                task->run();
            }
            catch (...) {
                this->state = State::Error;
                task->exception(current_exception());
                this->state = State::Running;
            }
        }
    }

    Worker::~Worker() {
        this->stop();
        this->thread.join();
    }

    void Worker::stop() {
        // if we have already exited, no need to go further
        if(this->state == State::Aborting) return;

        // store current state
        State currentState = this->state;

        // switch state
        this->state = State::Aborting;

        // if a thread is sleeping, signal to wakeup
        if(currentState == State::Waiting) {
            this->condition.notify_one();
        }

        this->queue.clear();
    }

}