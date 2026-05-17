//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef YARN_WORKSTEALINGDEQUE_H
#define YARN_WORKSTEALINGDEQUE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace YarnBall {

    /**
     * @brief Cache line size in bytes, used to align hot atomic fields and avoid
     *        false sharing between the owner and stealer paths.
     */
    inline constexpr size_t WS_CacheLineSize = 64;

    /**
     * @class WorkStealingDeque
     * @brief Bounded single-producer / multi-consumer Chase-Lev work-stealing deque.
     *
     * Invariants:
     *  - Exactly one thread (the *owner*) calls push() and pop(). Those operate
     *    on the *bottom* of the deque in LIFO order, with no inter-owner CAS.
     *  - Any number of other threads (*thieves*) may concurrently call steal(),
     *    which operates on the *top* of the deque in FIFO order.
     *  - The element type @p T must be trivially copyable, pointer-sized, and
     *    naturally atomic on the host (e.g. a raw owning pointer). Slot reads
     *    and writes use plain assignment; visibility is established by fences
     *    plus acquire/release on @ref top and @ref bottom, per Chase & Lev (2005).
     *
     * Memory ordering follows the canonical formulation:
     *  - push:  buffer[b] write (relaxed) -> release fence -> bottom.store (relaxed)
     *  - pop:   bottom.store (relaxed) -> seq_cst fence -> top.load (relaxed)
     *  - steal: top.load (acquire) -> seq_cst fence -> bottom.load (acquire)
     *           -> CAS top (seq_cst on success).
     *
     * Capacity must be a power of two so the index wrap is a cheap mask.
     *
     * @tparam T Element type. Required to be trivially copyable and small enough
     *           that aligned assignment is naturally atomic (one pointer-width).
     */
    template<typename T>
    class WorkStealingDeque {
        static_assert(std::is_trivially_copyable_v<T>,
                      "WorkStealingDeque<T> requires a trivially copyable T (use a raw pointer)");
        static_assert(sizeof(T) <= sizeof(void *),
                      "WorkStealingDeque<T>: T must be pointer-sized for tear-free slot reads/writes");

    public:
        /**
         * @brief Construct a deque with the given fixed capacity.
         * @param capacity Number of slots. Must be a power of two and >= 2.
         * @throws std::invalid_argument if capacity is zero or not a power of two.
         */
        explicit WorkStealingDeque(size_t capacity)
            : mask(capacity - 1), buffer(capacity) {
            if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
                throw std::invalid_argument("WorkStealingDeque: capacity must be a power of two >= 2");
            }
            this->top.store(0, std::memory_order_relaxed);
            this->bottom.store(0, std::memory_order_relaxed);
        }

        WorkStealingDeque(const WorkStealingDeque &) = delete;
        WorkStealingDeque(WorkStealingDeque &&) = delete;
        WorkStealingDeque &operator=(const WorkStealingDeque &) = delete;
        WorkStealingDeque &operator=(WorkStealingDeque &&) = delete;

        /**
         * @brief Push an item onto the bottom of the deque. Owner-thread only.
         *
         * Fails (returns @c false) if the deque is at capacity; in that case the
         * caller retains ownership of the item and may route it elsewhere.
         *
         * @param item The element to push.
         * @return @c true on success, @c false if the deque is full.
         */
        bool push(T item) noexcept {
            int64_t b = this->bottom.load(std::memory_order_relaxed);
            int64_t t = this->top.load(std::memory_order_acquire);
            if (b - t >= static_cast<int64_t>(this->buffer.size())) {
                return false;
            }
            this->buffer[b & this->mask] = item;
            std::atomic_thread_fence(std::memory_order_release);
            this->bottom.store(b + 1, std::memory_order_relaxed);
            return true;
        }

        /**
         * @brief Pop the most recently pushed item (LIFO). Owner-thread only.
         *
         * The hand-shake with concurrent stealers happens only when the deque has
         * exactly one element: the owner and a thief race for it via CAS on @c top.
         *
         * @param[out] out Receives the popped item on success.
         * @return @c true if an item was popped; @c false if the deque was empty
         *         or the owner lost the single-element race to a stealer.
         */
        bool pop(T &out) noexcept {
            int64_t b = this->bottom.load(std::memory_order_relaxed) - 1;
            this->bottom.store(b, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            int64_t t = this->top.load(std::memory_order_relaxed);

            if (t <= b) {
                T item = this->buffer[b & this->mask];
                if (t == b) {
                    // Only one element left: race the stealers for it.
                    if (!this->top.compare_exchange_strong(
                            t, t + 1,
                            std::memory_order_seq_cst,
                            std::memory_order_relaxed)) {
                        // Lost the race.
                        this->bottom.store(b + 1, std::memory_order_relaxed);
                        return false;
                    }
                    this->bottom.store(b + 1, std::memory_order_relaxed);
                }
                out = item;
                return true;
            }

            // Deque was already empty; restore bottom.
            this->bottom.store(b + 1, std::memory_order_relaxed);
            return false;
        }

        /**
         * @brief Steal the oldest item (FIFO) from the top. Callable by any
         *        thread other than the owner (and tolerable from the owner too,
         *        though that defeats the purpose).
         *
         * @param[out] out Receives the stolen item on success.
         * @return @c true if an item was stolen; @c false if the deque was empty
         *         or the stealer lost the race to a concurrent pop/steal. Callers
         *         that distinguish "empty" from "contention" should re-call.
         */
        bool steal(T &out) noexcept {
            int64_t t = this->top.load(std::memory_order_acquire);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            int64_t b = this->bottom.load(std::memory_order_acquire);

            if (t < b) {
                T item = this->buffer[t & this->mask];
                if (!this->top.compare_exchange_strong(
                        t, t + 1,
                        std::memory_order_seq_cst,
                        std::memory_order_relaxed)) {
                    // Lost race with another stealer or with owner pop.
                    return false;
                }
                out = item;
                return true;
            }

            return false;
        }

        /**
         * @brief Approximate size of the deque. May be transiently negative
         *        from a stealer's perspective due to the owner's pop protocol;
         *        the return value is clamped to zero.
         */
        size_t size() const noexcept {
            int64_t b = this->bottom.load(std::memory_order_relaxed);
            int64_t t = this->top.load(std::memory_order_relaxed);
            int64_t s = b - t;
            return s > 0 ? static_cast<size_t>(s) : 0;
        }

        /**
         * @brief Total slot count (constant after construction).
         */
        size_t capacity() const noexcept { return this->buffer.size(); }

        /**
         * @brief Approximate emptiness. Useful as a quick gate before more
         *        expensive lookups.
         */
        bool empty() const noexcept { return this->size() == 0; }

    private:
        /**
         * @brief Bitmask for fast modulo by capacity (capacity - 1).
         */
        const size_t mask;

        /**
         * @brief Circular slot buffer. Slots are plain T; visibility is enforced
         *        by the release/seq_cst fences in push/pop/steal rather than by
         *        per-slot atomics.
         */
        std::vector<T> buffer;

        /**
         * @brief Top index, owned by stealers (and pop's contended branch).
         *        Aligned to its own cache line to avoid false sharing with
         *        @ref bottom.
         */
        alignas(WS_CacheLineSize) std::atomic<int64_t> top;

        /**
         * @brief Bottom index, owned by the producer (push/pop). Aligned to its
         *        own cache line so stealer traffic on @ref top doesn't pull this
         *        line into Shared state on the owner's core.
         */
        alignas(WS_CacheLineSize) std::atomic<int64_t> bottom;
    };

}

#endif //YARN_WORKSTEALINGDEQUE_H
