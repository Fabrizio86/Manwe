//
// Created by Fabrizio Paino on 2025-05-30.
//

#ifndef MPMCQUEUE_H
#define MPMCQUEUE_H

#include <atomic>
#include <vector>
#include <stdexcept>
#include <new>
#include <optional>

namespace YarnBall {
    /**
     * The constant CacheLineSize defines the size of a CPU cache line in bytes.
     * It is often used for memory alignment purposes to improve performance by
     * avoiding false sharing and ensuring efficient cache usage.
     */
    constexpr size_t CacheLineSize = 64;

    /**
     * @enum QueueSize
     * @brief Enum representing different predefined queue sizes for the MPMCQueue.
     *
     * This enum provides specific sizes for different use cases:
     * - Light:   Represents a queue size of 512 elements.
     * - Medium:  Represents a queue size of 1024 elements.
     * - Huge:    Represents a queue size of 4096 elements.
     */
    enum QueueSize {
        Light = 512,
        Medium = 1024,
        Huge = 4096
    };

    template<typename T>
    /**
     * @struct Cell
     * @brief Represents a data container for a single slot in the MPMC queue.
     *
     * This structure is aligned to `CacheLineSize` to avoid false sharing
     * and improve performance in concurrent environments. Each `Cell` holds
     * a sequence counter and a generic type storage.
     *
     * @tparam T The type of the object stored in the cell.
     */
    struct alignas(CacheLineSize) Cell {
        /**
         * Represents a sequence number associated with a cell in the MPMCQueue.
         * Used for determining whether a cell is ready for an operation (enqueue or dequeue).
         *
         * This atomic variable ensures thread-safe access in a multi-producer, multi-consumer context.
         * It helps maintain coordination between producers and consumers by tracking the state
         * of cells in the queue.
         */
        std::atomic<size_t> sequence;
        /**
         * Holds an instance of type T. This member acts as the storage
         * for the data within a Cell of the MPMCQueue structure.
         *
         * The `storage` is used in conjunction with atomic operations
         * to ensure thread safety in the context of multiple producers
         * and consumers.
         */
        T storage;
    };

    template<typename T>
    /**
     * @class MPMCQueue
     * @brief A high-performance multi-producer, multi-consumer lock-free queue implementation.
     *
     * This queue is designed to facilitate concurrent enqueue and dequeue operations
     * in a multi-threaded environment, using lock-free techniques and atomic operations.
     *
     * The queue size must be a power of two for proper functionality.
     *
     * @tparam T The type of elements to be stored in the queue.
     */
    class MPMCQueue {
        /**
         * Constructs an instance of MPMCQueue with the specified capacity.
         * The capacity must be a power of two. Initializes the internal sequence numbers
         * for queue cells, and sets both head and tail pointers to 0.
         *
         * @param capacity The capacity of the queue. Defaults to `QueueSize::Huge` if not specified.
         *                 Must be a power of two. Throws `std::invalid_argument` if this condition is not met.
         *
         * @throws std::invalid_argument if the specified capacity is not a power of two.
         */
    public:
        explicit MPMCQueue(size_t capacity = QueueSize::Huge) : size(capacity), mask(capacity - 1), buffer(capacity) {
            if ((capacity & (capacity - 1)) != 0) {
                throw std::invalid_argument("Capacity must be a power of two");
            }

            for (size_t i = 0; i < size; ++i) {
                buffer[i].sequence.store(i, std::memory_order_relaxed);
            }

            head.store(0, std::memory_order_relaxed);
            tail.store(0, std::memory_order_relaxed);
        }

        /**
         * Adds an item to the queue if there is available capacity.
         *
         * @param item The item to be enqueued into the queue.
         * @return True if the item was successfully enqueued, false if the queue is full.
         */
        bool enqueue(const T &item) {
            size_t pos = head.load(std::memory_order_relaxed);
            while (true) {
                Cell<T> &cell = buffer[pos & mask];
                size_t seq = cell.sequence.load(std::memory_order_acquire);
                intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

                if (diff == 0) {
                    if (head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                        new(&cell.storage) T(item); // Placement new
                        cell.sequence.store(pos + 1, std::memory_order_release);
                        return true;
                    }
                } else if (diff < 0) {
                    return false; // Queue full
                } else {
                    pos = head.load(std::memory_order_relaxed); // Retry
                }
            }
        }

        /**
         * Attempts to dequeue an element from the queue. If successful, the dequeued
         * element is stored in the provided output parameter and removed from the queue.
         *
         * This function operates in a lock-free manner and adheres to the Multiple Producers,
         * Multiple Consumers (MPMC) queue semantics. It ensures atomicity during the dequeue
         * operation using atomic compare-and-swap (CAS) operations.
         *
         * @param[out] out A reference to a variable where the dequeued element will be stored.
         *                 If the operation is unsuccessful, the contents of this variable remain unmodified.
         * @return True if an element was successfully dequeued, false if the queue is empty.
         *
         * This function uses manual destruction of objects stored in the queue to avoid
         * unnecessary overhead. The sequence number and memory ordering mechanisms are
         * utilized to maintain thread safety and consistency.
         */
        bool dequeue(T &out) {
            size_t pos = tail.load(std::memory_order_relaxed);
            while (true) {
                Cell<T> &cell = buffer[pos & mask];
                size_t seq = cell.sequence.load(std::memory_order_acquire);
                intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

                if (diff == 0) {
                    if (tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                        out = std::move(cell.storage);
                        cell.storage.~T(); // Destroy object manually
                        cell.sequence.store(pos + size, std::memory_order_release);
                        return true;
                    }
                } else if (diff < 0) {
                    return false; // Queue empty
                } else {
                    pos = tail.load(std::memory_order_relaxed); // Retry
                }
            }
        }

        /**
         * Removes and returns the front element from the multi-producer, multi-consumer (MPMC) queue.
         *
         * The function retrieves the front element from the queue in a thread-safe manner, using atomic
         * operations to ensure proper coordination between multiple producers and consumers. If the queue
         * is empty, it returns `std::nullopt`.
         *
         * @tparam T The type of elements stored in the queue.
         * @return An `std::optional<T>` containing the front element if available, or `std::nullopt` if the
         *         queue is empty.
         *
         * @note This function assumes that the MPMC queue follows a ring-buffer implementation and that
         *       the capacity of the queue is a power of 2.
         *
         * @warning This function assumes that the type `T` has a callable destructor and supports move
         *          semantics, as the front element is moved out of the queue buffer.
         *
         * @details The implementation checks the sequence number of the front cell in the buffer to
         *          determine if it is safe to retrieve the element. If the cell is not ready, the function
         *          will handle the empty condition or retry.
         */
        std::optional<T> pop_front() {
            size_t pos = tail.load(std::memory_order_relaxed);
            while (true) {
                Cell<T> &cell = buffer[pos & mask];
                size_t seq = cell.sequence.load(std::memory_order_acquire);
                intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

                if (diff == 0) {
                    if (tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                        T result = std::move(cell.storage);
                        cell.storage.~T();
                        cell.sequence.store(pos + size, std::memory_order_release);
                        return result;
                    }
                } else if (diff < 0) {
                    return std::nullopt; // Queue empty
                } else {
                    pos = tail.load(std::memory_order_relaxed); // Retry
                }
            }
        }

        /**
         * Clears all elements from the queue and resets its internal state.
         *
         * This method performs the following operations:
         * - Dequeues and destroys all remaining objects in the queue.
         * - Resets the sequence numbers of all cells in the buffer to their original indices.
         * - Resets the `head` and `tail` pointers to their initial positions.
         *
         * The method ensures that any memory previously allocated for the objects
         * within the queue is properly released. It is thread-unsafe and should be
         * used with caution in multi-threaded environments.
         */
        void clear() {
            // Destroy remaining objects in the queue
            T temp;
            while (dequeue(temp)) {
                // destructed in dequeue()
            }

            // Reset the sequence numbers and internal pointers
            for (size_t i = 0; i < size; ++i) {
                buffer[i].sequence.store(i, std::memory_order_relaxed);
            }

            head.store(0, std::memory_order_relaxed);
            tail.store(0, std::memory_order_relaxed);
        }

        /**
         * Calculates the current size of the queue based on the head and tail indices.
         *
         * This method computes the number of elements currently stored in the queue by
         * subtracting the value of the tail index from the head index. The calculation
         * takes into account that the indices are updated atomically and may roll over
         * due to the use of a circular buffer. If the head index is greater than or
         * equal to the tail index, the size is computed as (head - tail). If the tail
         * index is ahead of the head, the returned size is 0 since the queue must be empty.
         *
         * @return The current size of the queue.
         */
        size_t Size() const {
            size_t h = head.load(std::memory_order_relaxed);
            size_t t = tail.load(std::memory_order_relaxed);
            return h >= t ? h - t : 0;
        }

        /**
         * Checks if the queue is empty.
         *
         * @return True if the queue contains no elements, false otherwise.
         */
        bool empty() const {
            return this->Size() == 0;
        }

        /**
         * Represents the fixed capacity of the MPMCQueue.
         *
         * This variable defines the size of the buffer used to store elements
         * in the queue. It is a constant, initialized at the time of construction,
         * and must be a power of two to ensure efficient indexing and operation.
         *
         * Any modifications to the queue, such as enqueueing and dequeueing,
         * are constrained within the range of this size.
         */
    private:
        const size_t size;
        /**
         * A bitmask derived from the size of the queue, used for efficient modulo operations.
         * This assumes the queue size is a power of two, enabling the replacement of modulo
         * operations with bitwise AND for improved performance.
         */
        const size_t mask;
        /**
         * A vector of `Cell<T>` objects that serves as the underlying storage buffer
         * for the multi-producer, multi-consumer queue.
         *
         * Each `Cell<T>` in the `buffer` represents a slot in the queue and is used
         * to encapsulate both the data (storage) and the sequence number required
         * for synchronization during enqueue and dequeue operations. The vector
         * size is determined by the queue's capacity.
         *
         * This member variable is initialized during the construction of the
         * `MPMCQueue` and its elements' sequence numbers are set accordingly to
         * represent available slots in the queue.
         */
        std::vector<Cell<T> > buffer;

        /**
         * @brief Atomic head pointer for a lock-free multi-producer, multi-consumer queue.
         *
         * This variable represents the current head position of the queue. It is used to coordinate
         * producer operations in the queue in a thread-safe manner. This variable is aligned to
         * the cache line size to minimize false sharing between threads when accessing
         * adjacent variables in memory.
         *
         * @note The alignment to CacheLineSize ensures optimal memory access performance
         * in concurrent environments. The atomic nature of the variable is critical to guarantee
         * safety when multiple threads interact with the queue.
         */
        alignas(CacheLineSize) std::atomic<size_t> head;
        /**
         * @brief Represents the tail index of the MPMC queue used for tracking the position of the next element to be dequeued.
         *
         * This variable is aligned to the cache line size to minimize false sharing and improve performance in a
         * concurrent multi-producer multi-consumer environment. It is an atomic variable to ensure thread-safe
         * operations without requiring external synchronization mechanisms.
         *
         * The value of `tail` is updated atomically when elements are dequeued from the queue, and provides
         * a critical mechanism for maintaining the internal state and consistency of the queue.
         */
        alignas(CacheLineSize) std::atomic<size_t> tail;
    };
}

#endif //MPMCQUEUE_H
