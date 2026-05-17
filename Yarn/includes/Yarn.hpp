//
// Created by Fabrizio Paino on 2022-01-14.
//

#ifndef YARN_YARN_HPP
#define YARN_YARN_HPP

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

#include "Fiber.hpp"
#include "ITask.hpp"
#include "SmallObjectPool.h"

namespace YarnBall {

    /**
     * @class Yarn
     * @brief Process-wide singleton thread pool.
     *
     * Architecture:
     *  - A fixed-size slot vector @c fibers holds up to @c MaxThreadCount
     *    workers. The first @c MinThreadCount slots are permanent; the rest
     *    are spawned on demand (temp fibers) and self-retire when idle.
     *  - Each fiber owns a Chase-Lev work-stealing deque (its local hot queue).
     *  - A single MPMC injection queue (@c injection) collects submissions
     *    from non-worker threads and overflow from full local deques.
     *  - Workers cycle through: local-pop -> injection-drain -> peer-steal ->
     *    spin -> park.
     *
     * Submission dispatch (Yarn::run):
     *  - If the caller is itself a worker (TLS @c tls_currentFiber is set),
     *    the task is pushed directly to that fiber's local deque — no
     *    injection traffic, no shared_ptr work.
     *  - Otherwise the task goes through the injection queue. One parked
     *    worker is woken if any; if none are parked and there is sustained
     *    backlog, a new temp fiber is spawned (up to MaxThreadCount).
     *
     * Ownership:
     *  - Tasks live in the executor as raw @c ITask* pointers. The dequeueing
     *    worker is responsible for @c delete after @c run().
     *  - The public API still accepts @c sITask for back-compat; it is
     *    adapted internally to an owned wrapper. The @c uITask overload is
     *    the zero-overhead path.
     */
    class Yarn final {
    public:
        Yarn(const Yarn &) = delete;
        Yarn(Yarn &&) = delete;
        Yarn &operator=(const Yarn &) = delete;
        Yarn &operator=(Yarn &&) = delete;

        ~Yarn();

        /**
         * @brief Singleton accessor (thread-safe via C++11 magic statics).
         */
        static Yarn *instance();

        /**
         * @brief Submit an owned task. Ownership is transferred to the pool.
         *        Null tasks are silently ignored.
         */
        void run(uITask task);

        /**
         * @brief Submit a co-owned task. The shared_ptr is wrapped in an
         *        internal adapter so the hot path stays free of ref-count
         *        atomics; the original sITask continues to behave normally
         *        for callers that retain a reference (e.g. Waitable).
         */
        void run(sITask task);

        /**
         * @brief Submit an arbitrary invocable. Allocates a small pooled
         *        wrapper for @p fn instead of going through @c ITask +
         *        virtual dispatch + a heap allocation. The pool's
         *        thread-local cache means a steady-state submit costs a
         *        handful of nanoseconds beyond the queue write itself,
         *        not the ~100 ns of @c malloc.
         *
         * Constraints:
         *  - @c F must be invocable as @c f().
         *  - @c F is move-stored into the wrapper, so move-only callables
         *    (lambdas with @c unique_ptr captures, etc.) are fine.
         *  - Exceptions thrown by @p fn are swallowed silently
         *    (no @c ITask::exception equivalent). Wrap the body in
         *    @c try/catch if you need failure handling.
         */
        template<typename F>
        void run(F &&fn);

        /**
         * @brief Replace the active scheduler. Thread-safe with respect to
         *        concurrent @c Run() calls.
         */
        void switchScheduler(sIScheduler scheduler);

        /**
         * @struct Stats
         * @brief Operational snapshot for observability / dashboards.
         *        Cheap to compute (atomic loads + a single short
         *        critical section to count alive fibers); call as
         *        often as you like.
         */
        struct Stats {
            /// Permanent worker count (the @c MinThreadCount floor).
            int permanentWorkers = 0;

            /// Hard ceiling on workers (the @c MaxThreadCount cap).
            int maxWorkers = 0;

            /// Currently-alive workers (permanent + spawned temp).
            int aliveWorkers = 0;

            /// Tasks currently sitting in the global injection queue.
            std::size_t injectionDepth = 0;

            /// Tasks waiting to be reaped (retired temp fibers whose
            /// joins have not yet been collected).
            int reapableFibers = 0;
        };

        /**
         * @brief Snapshot pool state. Safe to call from any thread
         *        (including a Yarn worker). Counts are sampled
         *        independently; the snapshot is best-effort, not a
         *        consistent global view.
         */
        Stats stats() const;

    private:
        Yarn();

        /**
         * @brief Upper bound on the worker pool. Computed once from
         *        @c std::thread::hardware_concurrency.
         */
        static int MaxThreadCount;

        /**
         * @brief Always-on worker count. Permanent fibers occupy slot
         *        indices @c [0, MinThreadCount).
         */
        static int MinThreadCount;

        /**
         * @brief Spawns a fiber into the given (currently empty) slot.
         *        Must be called with @c cmu held.
         */
        void initializeNewThreadLocked(FiberId id, bool markAsTemp);

        /**
         * @brief @c true if all temp slots are occupied (cannot grow).
         *        Must be called with @c cmu held.
         */
        bool maxLimitReachedLocked() const;

        /**
         * @brief Returns the lowest free temp-slot index, or @c fibers.size()
         *        if all temp slots are occupied.
         *        Must be called with @c cmu held.
         */
        FiberId firstUnusedTempIdLocked() const;

        /**
         * @brief Dispatch hot path for an already-owning raw task. Tries
         *        in-worker local push first; falls back to injection + grow.
         *        Takes ownership of @c task.
         */
        void dispatch(TaskPtr task);

        /**
         * @brief Push to the injection queue and wake / grow as appropriate.
         *        Takes ownership of @c task on success; on hard exhaustion
         *        the task is run inline on the calling thread.
         */
        void enqueueInjection(TaskPtr task);

        /**
         * @brief Spawn a temp fiber if there is a free slot and the backlog
         *        warrants it. Returns the new fiber's id or @c -1 if no
         *        growth happened.
         */
        int maybeGrowLocked();

        /**
         * @brief Walk fibers and notify one that is currently parked.
         *        Reads the lock-free snapshot, never touches @c cmu.
         * @return @c true if a parked worker was found and woken.
         */
        bool wakeOneParked();

        /**
         * @brief Random-peer steal hook handed to each fiber. Visits up to
         *        @c kStealVisits peers chosen uniformly at random.
         *        Reads the lock-free snapshot, never touches @c cmu.
         */
        TaskPtr stealFromPeers(FiberId selfId);

        /**
         * @brief Rebuild @ref fiberSnapshot from the current @c fibers state.
         *        Must be called with @c cmu held immediately after any
         *        mutation to the live-fiber set. Bumps @ref snapshotVersion
         *        so per-thread caches refresh on their next read.
         */
        void rebuildSnapshotLocked();

        /**
         * @brief Lock-free read of the current snapshot. Uses a per-thread
         *        cached copy that is refreshed only when @ref snapshotVersion
         *        changes (a rare grow/retire event). The hot path performs a
         *        single acquire-load on the version counter.
         */
        std::shared_ptr<const Fibers> currentSnapshot();

    public:
        /**
         * @brief Release this thread's cached snapshot, if any.
         *
         * Must be called just before a worker thread exits, because
         * the cached snapshot holds @c shared_ptr<Fiber> entries — if
         * those refs were dropped during thread_local teardown they
         * would, in some shutdown orderings, trigger @c ~Fiber on the
         * exiting worker (self-join). Releasing explicitly while the
         * thread is still alive avoids the hazard.
         *
         * Non-worker threads are free to ignore this.
         */
        void releaseThreadSnapshotCache() noexcept;

        /**
         * @brief Bump the parked-worker counter. Called by @c Fiber::park
         *        when a worker is about to suspend. Reads on the dispatch
         *        hot path are the @ref wakeOneParked fast-skip.
         */
        void noteParked(int delta) noexcept {
            this->parkedWorkers.fetch_add(delta, std::memory_order_acq_rel);
        }

    private:

        /**
         * @brief Reap retired fibers (join their threads, drop the sFiber).
         *        Called opportunistically by @ref Run and unconditionally
         *        from the destructor.
         */
        void reap();

        /**
         * @brief Snapshot the scheduler under @c schedMu.
         */
        sIScheduler getScheduler();

        /**
         * @brief Live worker slots. Indexed by FiberId. Protected by @c cmu.
         *        Only the slow path (grow / retire / shutdown) reads this; the
         *        hot path uses @ref fiberSnapshot.
         */
        Fibers fibers;

        /**
         * @brief Immutable snapshot of the live (non-null) fibers. Rebuilt
         *        under @c cmu after every mutation to @ref fibers.
         *
         * The hot read path does NOT touch this shared_ptr directly — that
         * would serialise every reader through the libc++ shared_ptr-atomic
         * spin lock and dominates submit-heavy workloads (90 %+ of submit
         * time in profiling). Instead, readers go through @ref
         * currentSnapshot, which uses a per-thread cached copy invalidated
         * via @ref snapshotVersion. Writers (under @c cmu) replace this
         * shared_ptr and bump the version.
         */
        std::shared_ptr<const Fibers> fiberSnapshot;

        /**
         * @brief Monotonic counter bumped (under @c cmu) every time
         *        @ref fiberSnapshot is replaced. Readers compare it against
         *        their thread-local cached version to decide whether to
         *        refresh.
         */
        std::atomic<std::uint64_t> snapshotVersion{0};

        /**
         * @brief Retired temp fibers awaiting join. Protected by @c cmu.
         */
        Fibers graveyard;

        /**
         * @brief Count of entries in @ref graveyard, exposed as an atomic so
         *        the dispatch hot path can short-circuit @c reap() without
         *        acquiring @c cmu. Updated under @c cmu, but read lock-free.
         */
        std::atomic<std::size_t> graveyardCount{0};

        /**
         * @brief Count of currently-live temp fibers (slots @c [MinThreadCount,
         *        MaxThreadCount)). Exposed atomically so the dispatch hot path
         *        can skip @c maybeGrowLocked when the pool is already at max
         *        without acquiring @c cmu.
         */
        std::atomic<int> tempFiberCount{0};

        /**
         * @brief Count of fibers currently parked. Bumped by Fiber::park
         *        before suspending and decremented after resuming. The
         *        submitter checks this atomically before invoking
         *        @ref wakeOneParked -- when zero, the whole snapshot walk
         *        and per-fiber @c isParked check are skipped. Under
         *        steady submit-heavy load the workers are almost never
         *        parked, so this is the dominant fast path.
         */
        std::atomic<int> parkedWorkers{0};

        /**
         * @brief Round-robin index for choosing a per-worker inbox.
         *        Incremented relaxed on every dispatch; the modulo against
         *        the live-fiber count picks a worker. Cheap, no fairness
         *        guarantees beyond "spreads across workers over time" --
         *        good enough for partitioning MPMC contention.
         */
        std::atomic<std::size_t> nextInboxIdx{0};

        /**
         * @brief Global injection queue. Internally lock-free MPMC, no
         *        external synchronization needed.
         */
        sInjectionQueue injection;

        /**
         * @brief Active scheduler (pluggable). Protected by @c schedMu.
         */
        sIScheduler scheduler;

        /**
         * @brief Covers @c fibers and @c graveyard transitions.
         */
        mutable std::mutex cmu;

        /**
         * @brief Covers swaps of @c scheduler.
         */
        std::mutex schedMu;

    };

    namespace detail {

        /**
         * @class CallableITask
         * @brief Concrete @c ITask wrapping an arbitrary invocable @p F.
         *
         * Defined as a template so each call to @c Yarn::run<F> instantiates
         * a wrapper whose @c run() inlines @p F's call operator directly --
         * no @c std::function indirection and no @c ITask vtable hop at
         * the point of invocation (only the one virtual call to enter the
         * wrapper). The instance is allocated from a per-size
         * @c SmallObjectPool so the alloc/free pair is a freelist hit.
         *
         * @c F is stored by move; the wrapper itself is destroyed by the
         * worker via @c delete @c this through the @c ITask base, which
         * dispatches to @c CallableITask::operator @c delete (because
         * @c ~ITask is virtual).
         */
        template<typename F>
        class CallableITask final : public ITask {
        public:
            explicit CallableITask(F &&fn) : f(std::move(fn)) {}
            explicit CallableITask(const F &fn) : f(fn) {}

            void run() override {
                try { this->f(); } catch (...) {}
            }

            void exception(std::exception_ptr) override {}

            static void *operator new(std::size_t) {
                return ::YarnBall::detail::poolAlloc<CallableITask>();
            }

            static void operator delete(void *p) noexcept {
                ::YarnBall::detail::poolFree<CallableITask>(p);
            }

        private:
            F f;
        };

    } // namespace detail

    template<typename F>
    inline void Yarn::run(F &&fn) {
        using Wrapper = detail::CallableITask<std::decay_t<F>>;
        // Construct the wrapper through our pool-backed operator new. The
        // dispatch path takes ownership via the raw ITask*.
        std::unique_ptr<ITask> task{new Wrapper(std::forward<F>(fn))};
        this->run(std::move(task));
    }

}

#endif //YARN_YARN_HPP
