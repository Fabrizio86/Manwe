#ifndef QUEUE_H
#define QUEUE_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>

namespace YarnBall {

    template <class T>
    class Queue {
    public:
        // Constructor
        Queue() : valid(true) { }

        // Destructor
        ~Queue() {
            this->valid = false;
            this->clear();
        }

        // Disable copy constructor
        Queue &operator=(Queue &) = delete;

        // Add new item to workQueue
        void push(T value) {
            // locking for insertion
            std::lock_guard<std::mutex> lk(mut);

            // push item in list
            this->queue.push(value);

            // unlock and notify one thread
            this->condition.notify_one();
        }

        // Get item from workQueue
        T get() {
            // locking for item retrieval
            std::unique_lock<std::mutex> lk(this->mut);

            // wait for a task or the workQueue to be invalid
            this->condition.wait(lk, [this] {
                return !this->queue.empty() || !this->valid;
            });

            // if the workQueue is invalid, do nothing and return false
            if (!this->valid) {
                return nullptr;
            }

            // take first item from the workQueue
            T task = this->queue.front();

            // pop item from the workQueue
            this->queue.pop();

            // move item from workQueue into the calling reference
            return task;
        }

        // Clear the workQueue
        void clear() {
            std::lock_guard<std::mutex> lk(this->mut);

            while (!this->queue.empty()) {
                this->queue.pop();
            }

            this->valid = false;
            this->condition.notify_all();
        }

        // Check if workQueue is empty
        bool empty() {
            return this->queue.empty();
        }

        // Get workQueue size
        size_t size() {
            std::lock_guard<std::mutex> lk(this->mut);
            return this->queue.size();
        }

        // return if is valid
        bool isValid() {
            return this->valid;
        }

    private:
        std::mutex mut;
        std::atomic<bool> valid;
        std::queue<T> queue;
        std::condition_variable condition;
    };

}

#endif // QUEUE_H
