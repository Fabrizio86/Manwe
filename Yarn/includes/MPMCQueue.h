//
// Created by Fabrizio Paino on 2025-05-30.
//

#ifndef MPMCQUEUE_H
#define MPMCQUEUE_H

#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace YarnBall {

    /**
     * @brief CPU cache line size in bytes. Used to align hot atomic fields and
     *        avoid false sharing between producer and consumer paths.
     */
    constexpr size_t CacheLineSize = 64;

    /**
     * @enum QueueSize
     * @brief Pre-set capacities for the MPMCQueue.
     *  - @c Light  : 512 slots — small auxiliary queues.
     *  - @c Medium : 1024 slots — moderate fan-in queues.
     *  - @c Huge   : 4096 slots — the executor's global injection queue.
     */
    enum QueueSize {
        Light = 512,
        Medium = 1024,
        Huge = 4096
    };

    /**
     * @struct Cell
     * @brief One slot in the bounded MPMC ring.
     *
     * @tparam T Element type. The cell holds raw aligned storage so the queue
     *           manages T's lifetime entirely: constructors fire on enqueue,
     *           destructors on dequeue, and any survivors are destroyed by the
     *           queue destructor. Each Cell is cache-line aligned to avoid
     *           false sharing with neighbouring slots.
     */
    template<typename T>
    struct alignas(CacheLineSize) Cell {
        /**
         * @brief Sequence number that drives the producer/consumer handshake.
         *        - @c seq == pos     => slot is empty and writable by a producer.
         *        - @c seq == pos + 1 => slot is full and readable by a consumer.
         *        - @c seq == pos + N (N == capacity) => recycled empty slot.
         */
        std::atomic<size_t> sequence{0};

        /**
         * @brief Raw aligned storage for one T. Lifetime is managed by the
         *        queue via placement-new (enqueue) and explicit ~T (dequeue).
         */
        alignas(T) std::byte storage[sizeof(T)];

        /**
         * @brief Pointer to the (possibly live) T within @ref storage.
         *        Uses @c std::launder so the strict-aliasing rules accept the
         *        reinterpret across the placement-new boundary.
         */
        T *ptr() noexcept {
            return std::launder(reinterpret_cast<T *>(&storage));
        }
    };

    /**
     * @class MPMCQueue
     * @brief Bounded lock-free multi-producer / multi-consumer ring queue.
     *
     * Implementation follows Dmitry Vyukov's classic design: each slot carries
     * a sequence number that producers and consumers use to coordinate without
     * a global lock. Memory ordering is acquire/release on the sequence loads
     * and stores, with relaxed CAS on @ref head / @ref tail (the handshake
     * synchronises through the sequence numbers).
     *
     * Capacity must be a power of two for the @c & @c mask trick to replace
     * modulo. The default capacity is @c QueueSize::Huge.
     *
     * @tparam T Element type. Required by the queue API; the queue does not
     *           require @c T to be default-constructible.
     */
    template<typename T>
    class MPMCQueue {
    public:
        /**
         * @brief Construct a queue with the given capacity.
         * @param capacity Number of slots; must be a power of two.
         * @throws std::invalid_argument If @p capacity is not a power of two
         *                               or is zero.
         */
        explicit MPMCQueue(size_t capacity = QueueSize::Huge)
            : size((capacity & (capacity - 1)) == 0 && capacity > 0
                       ? capacity
                       : throw std::invalid_argument("Capacity must be a power of two")),
              mask(capacity - 1),
              buffer(capacity) {
            for (size_t i = 0; i < size; ++i) {
                buffer[i].sequence.store(i, std::memory_order_relaxed);
            }
            head.store(0, std::memory_order_relaxed);
            tail.store(0, std::memory_order_relaxed);
        }

        /**
         * @brief Destructor. No concurrent access permitted; destroys any
         *        elements still live in @c [tail, head).
         */
        ~MPMCQueue() {
            size_t t = tail.load(std::memory_order_relaxed);
            size_t h = head.load(std::memory_order_relaxed);
            while (t != h) {
                buffer[t & mask].ptr()->~T();
                ++t;
            }
        }

        MPMCQueue(const MPMCQueue &) = delete;
        MPMCQueue &operator=(const MPMCQueue &) = delete;
        MPMCQueue(MPMCQueue &&) = delete;
        MPMCQueue &operator=(MPMCQueue &&) = delete;

        /**
         * @brief Enqueue by copy.
         * @return @c true on success, @c false if the queue is full.
         */
        bool enqueue(const T &item) { return emplace_impl(item); }

        /**
         * @brief Enqueue by move.
         * @return @c true on success, @c false if the queue is full.
         */
        bool enqueue(T &&item) { return emplace_impl(std::move(item)); }

        /**
         * @brief Pop the oldest element.
         * @return The element wrapped in std::optional, or @c std::nullopt if empty.
         */
        std::optional<T> pop_front() {
            size_t pos = tail.load(std::memory_order_relaxed);
            while (true) {
                Cell<T> &cell = buffer[pos & mask];
                size_t seq = cell.sequence.load(std::memory_order_acquire);
                intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

                if (diff == 0) {
                    if (tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                        T result = std::move(*cell.ptr());
                        cell.ptr()->~T();
                        cell.sequence.store(pos + size, std::memory_order_release);
                        return result;
                    }
                } else if (diff < 0) {
                    return std::nullopt;
                } else {
                    pos = tail.load(std::memory_order_relaxed);
                }
            }
        }

        /**
         * @brief Out-parameter form of @ref pop_front for callers that prefer
         *        not to pay for an @c optional in the success path.
         * @param[out] out Receives the dequeued element on success.
         * @return @c true on success, @c false if empty.
         */
        bool dequeue(T &out) {
            auto v = pop_front();
            if (!v) return false;
            out = std::move(*v);
            return true;
        }

        /**
         * @brief Single-threaded drain. The caller must guarantee no producer
         *        or consumer touches the queue concurrently. Sequences and
         *        indices are left in a consistent state for future enqueues.
         */
        void clear() {
            while (pop_front().has_value()) {
            }
        }

        /**
         * @brief Approximate number of elements currently in the queue.
         *        May be transiently inaccurate under contention.
         */
        size_t Size() const {
            size_t h = head.load(std::memory_order_relaxed);
            size_t t = tail.load(std::memory_order_relaxed);
            return h >= t ? h - t : 0;
        }

        /**
         * @brief Maximum capacity, fixed at construction.
         */
        size_t capacity() const noexcept { return size; }

        /**
         * @brief Approximate emptiness.
         */
        bool empty() const { return Size() == 0; }

    private:
        /**
         * @brief Common emplacement path used by both copy and move enqueue.
         */
        template<typename U>
        bool emplace_impl(U &&item) {
            size_t pos = head.load(std::memory_order_relaxed);
            while (true) {
                Cell<T> &cell = buffer[pos & mask];
                size_t seq = cell.sequence.load(std::memory_order_acquire);
                intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

                if (diff == 0) {
                    if (head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                        ::new (static_cast<void *>(&cell.storage)) T(std::forward<U>(item));
                        cell.sequence.store(pos + 1, std::memory_order_release);
                        return true;
                    }
                } else if (diff < 0) {
                    return false;
                } else {
                    pos = head.load(std::memory_order_relaxed);
                }
            }
        }

        /**
         * @brief Fixed capacity. Always a power of two.
         */
        const size_t size;

        /**
         * @brief @c capacity - 1, used to replace modulo with bitwise AND.
         */
        const size_t mask;

        /**
         * @brief Slot array. Storage lifetimes are managed by the queue, not
         *        by the vector.
         */
        std::vector<Cell<T>> buffer;

        /**
         * @brief Producer cursor. Cache-line aligned to prevent false sharing
         *        with @ref tail.
         */
        alignas(CacheLineSize) std::atomic<size_t> head;

        /**
         * @brief Consumer cursor. Cache-line aligned to prevent false sharing
         *        with @ref head.
         */
        alignas(CacheLineSize) std::atomic<size_t> tail;
    };
}

#endif //MPMCQUEUE_H
