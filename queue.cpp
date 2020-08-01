#include "queue.hpp"

namespace YarnBall {

using namespace std;

Queue::Queue() : valid{ true } { }

Queue::~Queue(){
    this->valid = false;
    this->clear();
}

void Queue::push(sITask value) {
    // locking for insertion
    std::lock_guard<std::mutex> lk(mut);

    // push item in list
    this->queue.push(value);

    // unlock and notify one thread
    this->condition.notify_one();
}

sITask Queue::get() {
    // locking for item retrieval
    std::unique_lock<std::mutex> lk( this->mut );

    // wait for a task or the queue to be invalid
    this->condition.wait(lk, [this] {
        return !this->queue.empty() || !this->valid;
    });

    // if the queue is invalid, do nothing and return false
    if(!this->valid) {
        return nullptr;
    }

    // take first item from the queue
    sITask task = std::move(this->queue.front());

    // pop item from the queue
    this->queue.pop();

    // move item from queue into the calling reference
    return task;
}

void Queue::clear() {
    std::lock_guard<std::mutex> lk( this->mut );

    while(!this->queue.empty()) {
        this->queue.pop();
    }

    this->valid = false;
    this->condition.notify_all();
}

bool Queue::empty() {
    std::lock_guard<std::mutex> lk( this->mut );
    return this->queue.empty();
}

size_t Queue::size() {
    std::lock_guard<std::mutex> lk( this->mut );
    return this->queue.size();
}

bool Queue::isValid() {
    return this->valid;
}
}
