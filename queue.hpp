#ifndef QUEUE_H
#define QUEUE_H

#include "itask.hpp"

#include <queue>
#include <mutex>

namespace YarnBall {

    class Queue final {
    public:
        // Constructor
        Queue();

        // Destructor
        ~Queue();

        // Disable copy constructor
        Queue &operator=(Queue &) = delete;

        // Add new item to queue
        void push(sITask value);

        // Get item from queue
        sITask get();

        // Clear the queue
        void clear();

        // Check if queue is empty
        bool empty();

        // Get queue size
        size_t size();

        // return if is valid
        bool isValid();

    private:
        std::mutex mut;
        std::atomic<bool> valid;
        std::queue<sITask> queue;
        std::condition_variable condition;
    };

}

#endif // QUEUE_H
