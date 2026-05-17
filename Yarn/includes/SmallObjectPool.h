//
// Created by Fabrizio Paino on 2026-05-16.
//

#ifndef YARN_SMALL_OBJECT_POOL_H
#define YARN_SMALL_OBJECT_POOL_H

#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <new>

namespace YarnBall::detail {

    /**
     * @class SmallObjectPool
     * @brief Per-process fixed-size freelist with a thread-local cache.
     *
     * Used to back @c operator @c new / @c operator @c delete on internal
     * ITask wrappers (CoroutineITask, SharedOwnerAdapter, HandlerTask).
     * Replaces the default malloc/free with a freelist hit on the steady
     * state, dropping the alloc/free cost from ~100-200 ns to a handful of
     * ns per pair.
     *
     * Each thread keeps a small LIFO of freed blocks. When the local cache
     * runs dry, we refill in a batch from a shared (mutex-protected) pool;
     * when the local cache grows past a high-water mark, we spill a batch
     * back. Local pops / pushes are atomic-free; only batches contend.
     *
     * The single class is parameterised on a slot size; the cost of
     * picking a power-of-two slot size somewhat larger than the actual
     * object is a few bytes of slack. We use one pool per size we need
     * (see @c SmallObjectPool<32> / @c <64> etc.) so each instantiation
     * has its own statics.
     *
     * @tparam SlotSize Size in bytes of each allocation. Must be at least
     *                  @c sizeof(void*) so an unused slot can hold the
     *                  freelist link.
     */
    template<std::size_t SlotSize>
    class SmallObjectPool {
        static_assert(SlotSize >= sizeof(void *),
                      "SmallObjectPool slot must hold a freelist link");

    public:
        /**
         * @brief Pop a slot from the local cache; refill from the shared
         *        pool if empty; finally fall back to @c std::malloc. Never
         *        returns nullptr (throws via the bad_alloc path of malloc).
         */
        static void *allocate() {
            if (tlsHead == nullptr) {
                refillFromGlobal();
                if (tlsHead == nullptr) {
                    void *p = std::malloc(SlotSize);
                    if (!p) throw std::bad_alloc();
                    return p;
                }
            }
            Node *n = tlsHead;
            tlsHead = n->next;
            --tlsCount;
            return n;
        }

        /**
         * @brief Push a slot back into the local cache, spilling a batch
         *        to the shared pool when the cache crosses the high-water
         *        mark. Safe to call on any thread.
         */
        static void deallocate(void *p) noexcept {
            auto *node = static_cast<Node *>(p);
            node->next = tlsHead;
            tlsHead = node;
            ++tlsCount;
            if (tlsCount >= kHighWaterMark) {
                spillToGlobal();
            }
        }

    private:
        /**
         * @brief Freelist link. Lives in the first @c sizeof(void*) bytes
         *        of an unused slot; the slot is repurposed as @c Node when
         *        free and as user data when allocated.
         */
        struct Node {
            Node *next;
        };

        /**
         * @brief Batch size for moves between local and shared pools.
         *        Larger amortises the shared-pool lock; smaller keeps the
         *        worst-case latency of a batch move bounded.
         */
        static constexpr std::size_t kBatchSize = 16;

        /**
         * @brief Spill point: when @c tlsCount reaches this many slots,
         *        spill @c kBatchSize back to the shared pool.
         */
        static constexpr std::size_t kHighWaterMark = 2 * kBatchSize;

        /**
         * @brief Move up to @c kBatchSize nodes from the shared pool into
         *        the local cache. Called when the local cache is empty.
         */
        static void refillFromGlobal() {
            std::lock_guard<std::mutex> lk(globalMu);
            for (std::size_t i = 0; i < kBatchSize && globalHead != nullptr; ++i) {
                Node *n = globalHead;
                globalHead = n->next;
                n->next = tlsHead;
                tlsHead = n;
                ++tlsCount;
            }
        }

        /**
         * @brief Move @c kBatchSize nodes from the local cache into the
         *        shared pool. Called when the local cache overflows.
         */
        static void spillToGlobal() noexcept {
            // Detach a kBatchSize-long sub-list from the front of the
            // local cache, leaving the rest in place. Splice into the
            // shared pool under its mutex.
            Node *batchHead = tlsHead;
            Node *batchTail = batchHead;
            for (std::size_t i = 1; i < kBatchSize; ++i) {
                batchTail = batchTail->next;
            }
            tlsHead = batchTail->next;
            tlsCount -= kBatchSize;

            std::lock_guard<std::mutex> lk(globalMu);
            batchTail->next = globalHead;
            globalHead = batchHead;
        }

        /**
         * @brief Thread-local LIFO cache. Single-producer / single-consumer
         *        (the thread itself), so no atomics needed.
         */
        static thread_local Node *tlsHead;
        static thread_local std::size_t tlsCount;

        /**
         * @brief Shared pool. Protected by @c globalMu; touched only on
         *        batch transfers, not per allocation.
         */
        static Node *globalHead;
        static std::mutex globalMu;
    };

    template<std::size_t SlotSize>
    thread_local typename SmallObjectPool<SlotSize>::Node *
        SmallObjectPool<SlotSize>::tlsHead = nullptr;

    template<std::size_t SlotSize>
    thread_local std::size_t SmallObjectPool<SlotSize>::tlsCount = 0;

    template<std::size_t SlotSize>
    typename SmallObjectPool<SlotSize>::Node *
        SmallObjectPool<SlotSize>::globalHead = nullptr;

    template<std::size_t SlotSize>
    std::mutex SmallObjectPool<SlotSize>::globalMu;


    /**
     * @brief Pick the smallest power-of-two slot size that accommodates
     *        @p Bytes and @p Align. Used to consolidate distinct ITask
     *        sizes into a small set of pools (32, 64, 128, ...).
     */
    template<std::size_t Bytes, std::size_t Align = alignof(std::max_align_t)>
    constexpr std::size_t poolSlotSize() {
        std::size_t n = 32;
        while (n < Bytes || n < Align) n *= 2;
        return n;
    }

    /**
     * @brief Allocate / deallocate via the pool sized for the given type.
     *        Used by class-level @c operator @c new / @c delete overloads:
     *
     * @code
     *   void* operator new(size_t)    { return poolAlloc<MyClass>(); }
     *   void  operator delete(void* p){ poolFree<MyClass>(p); }
     * @endcode
     */
    template<typename T>
    inline void *poolAlloc() {
        return SmallObjectPool<poolSlotSize<sizeof(T), alignof(T)>()>::allocate();
    }

    template<typename T>
    inline void poolFree(void *p) noexcept {
        SmallObjectPool<poolSlotSize<sizeof(T), alignof(T)>()>::deallocate(p);
    }

}

#endif // YARN_SMALL_OBJECT_POOL_H
