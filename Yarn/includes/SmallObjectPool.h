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
     * @tparam Batch    Blocks moved between a thread's cache and the shared pool per lock hold. A pool whose blocks are
     *                  allocated on one thread and freed on others (coroutine frames) wants a large one, so that the
     *                  shared mutex is taken once per many blocks.
     */
    template<std::size_t SlotSize, std::size_t Batch = 16>
    class SmallObjectPool {
        static_assert(SlotSize >= 3 * sizeof(void *),
                      "SmallObjectPool slot must hold a batch header");

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
            if (tlsCount == 0) {
                // The local list is empty, so this thread may not have a flush-on-exit hook yet (blocks also arrive here
                // from threads that only ever free). After the hook has run, bypass the thread cache entirely.
                if (tlsDead) {
                    node->next = nullptr;
                    if (pushBatch(node, 1)) return;
                    std::free(node);
                    return;
                }
                armThreadExit();
            }
            node->next = tlsHead;
            tlsHead = node;
            ++tlsCount;
            if (tlsCount >= kHighWaterMark) {
                spillToGlobal();
            }
        }

    private:
        /**
         * @brief Freelist link (and, for the head of a batch in the shared pool, the batch header). Lives in the
         *        first bytes of an unused slot; the slot is repurposed as @c Node when free and as user data when
         *        allocated.
         */
        struct Node {
            Node *next;
            /// Only in the first node of a batch parked in the shared pool: the next batch, and this batch's length.
            Node *nextBatch;
            std::size_t count;
        };

        /**
         * @brief Batch size for moves between local and shared pools.
         *        Larger amortises the shared-pool lock; smaller keeps the
         *        worst-case latency of a batch move bounded.
         */
        static constexpr std::size_t kBatchSize = Batch;

        /**
         * @brief Spill point: when @c tlsCount reaches this many slots,
         *        spill @c kBatchSize back to the shared pool.
         */
        static constexpr std::size_t kHighWaterMark = 2 * kBatchSize;

        /**
         * @brief Cap on blocks parked in the shared pool. Blocks arriving past it are returned to the allocator, so a
         *        burst of allocations does not pin its peak footprint for the life of the process.
         */
        static constexpr std::size_t kMaxGlobal = 8192;

        /**
         * @brief Free a detached chain of blocks back to the allocator.
         */
        static void freeChain(Node *head) noexcept {
            while (head) {
                Node *next = head->next;
                std::free(head);
                head = next;
            }
        }

        /**
         * @brief Move up to @c kBatchSize nodes from the shared pool into
         *        the local cache. Called when the local cache is empty.
         */
        static void refillFromGlobal() {
            if (tlsDead) return;
            armThreadExit();
            Node *batch;
            {
                std::lock_guard<std::mutex> lk(globalMu);
                batch = globalHead;
                if (batch == nullptr) return;
                globalHead = batch->nextBatch;
                globalCount -= batch->count;
            }
            // A whole batch is adopted by relinking one pointer: the other blocks are not touched (they were freed by
            // other threads, so their cache lines belong to other cores).
            tlsHead = batch;
            tlsCount = batch->count;
        }

        /**
         * @brief Park the chain @p head (@p count blocks, null-terminated) in the shared pool.
         * @return @c false when the pool is full and the caller must give the blocks back to the allocator.
         */
        static bool pushBatch(Node *head, std::size_t count) noexcept {
            std::lock_guard<std::mutex> lk(globalMu);
            if (globalCount + count > kMaxGlobal) return false;
            head->nextBatch = globalHead;
            head->count = count;
            globalHead = head;
            globalCount += count;
            return true;
        }

        /**
         * @brief Move @c kBatchSize nodes from the local cache into the
         *        shared pool. Called when the local cache overflows.
         */
        static void spillToGlobal() noexcept {
            // Detach a kBatchSize-long sub-list from the front of the local cache, leaving the rest in place, and
            // park it in the shared pool as one batch.
            Node *batchHead = tlsHead;
            Node *batchTail = batchHead;
            for (std::size_t i = 1; i < kBatchSize; ++i) {
                batchTail = batchTail->next;
            }
            tlsHead = batchTail->next;
            tlsCount -= kBatchSize;
            batchTail->next = nullptr;
            // Shared pool full: give the surplus back to the allocator.
            if (!pushBatch(batchHead, kBatchSize)) freeChain(batchHead);
        }

        /**
         * @brief Return the whole local cache to the shared pool. Runs when
         *        the owning thread exits (via @ref ThreadExitFlush), so the
         *        blocks it cached are reusable instead of leaking.
         */
        static void flushLocal() noexcept {
            Node *h = tlsHead;
            const std::size_t count = tlsCount;
            tlsHead = nullptr;
            tlsCount = 0;
            if (!h) return;
            if (!pushBatch(h, count)) freeChain(h);
        }

        struct ThreadExitFlush {
            ~ThreadExitFlush() {
                flushLocal();
                tlsDead = true;
            }
        };

        /**
         * @brief Make sure this thread's exit flushes its cache. A
         *        function-local thread_local registers its destructor on
         *        first use; this is only reached when the local list is
         *        empty, never on the allocate/free fast path.
         */
        static void armThreadExit() noexcept {
            static thread_local ThreadExitFlush hook;
            (void) hook;
        }

        /**
         * @brief Thread-local LIFO cache. Single-producer / single-consumer
         *        (the thread itself), so no atomics needed.
         */
        static thread_local Node *tlsHead;
        static thread_local std::size_t tlsCount;
        static thread_local bool tlsDead;

        /**
         * @brief Shared pool. Protected by @c globalMu; touched only on
         *        batch transfers, not per allocation.
         */
        static Node *globalHead;
        static std::size_t globalCount;
        static std::mutex globalMu;
    };

    template<std::size_t SlotSize, std::size_t Batch>
    thread_local typename SmallObjectPool<SlotSize, Batch>::Node *
        SmallObjectPool<SlotSize, Batch>::tlsHead = nullptr;

    template<std::size_t SlotSize, std::size_t Batch>
    thread_local std::size_t SmallObjectPool<SlotSize, Batch>::tlsCount = 0;

    template<std::size_t SlotSize, std::size_t Batch>
    thread_local bool SmallObjectPool<SlotSize, Batch>::tlsDead = false;

    template<std::size_t SlotSize, std::size_t Batch>
    typename SmallObjectPool<SlotSize, Batch>::Node *
        SmallObjectPool<SlotSize, Batch>::globalHead = nullptr;

    template<std::size_t SlotSize, std::size_t Batch>
    std::size_t SmallObjectPool<SlotSize, Batch>::globalCount = 0;

    template<std::size_t SlotSize, std::size_t Batch>
    std::mutex SmallObjectPool<SlotSize, Batch>::globalMu;


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
        static_assert(alignof(T) <= alignof(std::max_align_t),
                      "SmallObjectPool does not support over-aligned types");
        return SmallObjectPool<poolSlotSize<sizeof(T), alignof(T)>()>::allocate();
    }

    template<typename T>
    inline void poolFree(void *p) noexcept {
        SmallObjectPool<poolSlotSize<sizeof(T), alignof(T)>()>::deallocate(p);
    }

    // ---- Coroutine frames ----------------------------------------------------------------------------------------

    /// Frame pools move 64 blocks per lock hold: frames are allocated by the thread that awaits a Task and freed by
    /// whichever worker finished it, which is the pattern a small batch handles worst.
    template<std::size_t Slot>
    using FramePool = SmallObjectPool<Slot, 64>;

    // Under AddressSanitizer a recycled frame would hide a use-after-free of the frame, which is exactly what the
    // sanitizer is run to find, so frames then go straight to the allocator (as they do with YARN_NO_FRAME_POOL).
#if defined(YARN_NO_FRAME_POOL) || defined(__SANITIZE_ADDRESS__)
#  define YARN_FRAME_POOL_DISABLED 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define YARN_FRAME_POOL_DISABLED 1
#  endif
#endif

    /**
     * @brief Allocate the frame of a Task coroutine. Frames up to 1 KiB come from the per-size thread-cached pools
     *        (a frame is created and destroyed on every @c co_await of a Task, usually on different threads, which the
     *        pools' shared spill handles); bigger ones go to the allocator. @ref frameFree must be given the same size.
     */
    inline void *frameAlloc(std::size_t n) {
#ifdef YARN_FRAME_POOL_DISABLED
        return ::operator new(n);
#else
        if (n <= 64) return FramePool<64>::allocate();
        if (n <= 96) return FramePool<96>::allocate();
        if (n <= 128) return FramePool<128>::allocate();
        if (n <= 192) return FramePool<192>::allocate();
        if (n <= 256) return FramePool<256>::allocate();
        if (n <= 384) return FramePool<384>::allocate();
        if (n <= 512) return FramePool<512>::allocate();
        if (n <= 768) return FramePool<768>::allocate();
        if (n <= 1024) return FramePool<1024>::allocate();
        return ::operator new(n);
#endif
    }

    inline void frameFree(void *p, std::size_t n) noexcept {
#ifdef YARN_FRAME_POOL_DISABLED
        ::operator delete(p);
        (void) n;
#else
        if (n <= 64) return FramePool<64>::deallocate(p);
        if (n <= 96) return FramePool<96>::deallocate(p);
        if (n <= 128) return FramePool<128>::deallocate(p);
        if (n <= 192) return FramePool<192>::deallocate(p);
        if (n <= 256) return FramePool<256>::deallocate(p);
        if (n <= 384) return FramePool<384>::deallocate(p);
        if (n <= 512) return FramePool<512>::deallocate(p);
        if (n <= 768) return FramePool<768>::deallocate(p);
        if (n <= 1024) return FramePool<1024>::deallocate(p);
        ::operator delete(p);
#endif
    }

}

#endif // YARN_SMALL_OBJECT_POOL_H
