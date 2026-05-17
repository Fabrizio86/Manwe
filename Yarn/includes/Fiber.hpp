//
// Created by Fabrizio Paino on 2022-01-15.
//

#ifndef YARN_FIBER_HPP
#define YARN_FIBER_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "ITask.hpp"
#include "Workload.hpp"

namespace YarnBall {

    /**
     * @class Fiber
     * @brief A pool worker. Owns one work-stealing deque and exactly one OS
     *        thread that runs @ref process.
     *
     * Lifecycle:
     *  - @c running starts as @c true; flipped to @c false by @ref stop or by
     *    the worker itself when it decides to retire (temp fibers only).
     *  - Permanent fibers (@c temp == false) live until Yarn destruction.
     *  - Temp fibers retire when both their local deque and the global
     *    injection queue are observed empty AND no peer steal succeeds in the
     *    spin window. On retirement they push themselves to Yarn's graveyard
     *    via @c signalDone so the reaper can join the OS thread.
     *
     * Work discovery, in priority order, per loop iteration:
     *  1. Pop from the local deque (LIFO, cache-warm).
     *  2. Refill from the injection queue if it has tasks.
     *  3. Try to steal from a randomly chosen peer.
     *  4. Spin a bounded number of iterations doing (2)+(3) before parking.
     *  5. Park on the per-fiber condition variable.
     *
     * External submitters (Yarn::run from a non-worker thread) push to the
     * injection queue then call WakeOneIdle to unblock one parked worker.
     * In-worker submitters push directly to their own deque and notify a parked
     * peer (so a stealer can pick up the slack).
     */
    class Fiber final {
    public:
        /**
         * @brief Construct and immediately spawn the worker thread.
         *
         * @param id                Slot identifier within the pool.
         * @param dequeCapacity     Capacity of the local work-stealing deque
         *                          (must be power of two).
         * @param signalDone        Retirement callback (temp fibers only).
         * @param getFromPending    Pulls a burst from the injection queue into
         *                          our local deque.
         * @param anyPendingTasks   Returns true if the injection queue is non-empty.
         * @param pushPending       Pushes overflow into the injection queue.
         * @param tryStealFromPeers Attempts to steal from a peer fiber.
         * @param wakeOneIdle       Wakes one parked worker, if any.
         * @param temp              If true, the fiber retires when idle.
         */
        Fiber(FiberId id,
              size_t dequeCapacity,
              SignalDone signalDone,
              GetFromPending getFromPending,
              AnyPendingTasks anyPendingTasks,
              PushPending pushPending,
              TryStealFromPeers tryStealFromPeers,
              WakeOneIdle wakeOneIdle,
              bool temp);

        /**
         * @brief Joins the OS thread. Safe to call from any thread except the
         *        worker's own thread (Yarn arranges this via the graveyard /
         *        reaper, never from inside @ref process).
         */
        ~Fiber();

        Fiber(const Fiber &) = delete;
        Fiber(Fiber &&) = delete;
        Fiber &operator=(const Fiber &) = delete;
        Fiber &operator=(Fiber &&) = delete;

        /**
         * @brief Push a task onto the local deque from outside the worker.
         *
         * Used by Yarn when it wants to seed a freshly spawned temp fiber with
         * the task that triggered the spawn. Returns @c false if the deque is
         * full or the fiber is no longer accepting work; in that case the
         * caller retains ownership.
         *
         * @param task Raw owning pointer; on success ownership moves to the deque.
         * @return @c true on success.
         */
        bool seed(TaskPtr task);

        /**
         * @brief Enqueue a task into this fiber's per-worker inbox.
         *
         * The inbox is a small bounded MPMC ring (multiple submitters
         * enqueue; the owning worker dequeues). Used by @c Yarn::dispatch
         * to partition submit traffic across workers, bypassing the
         * central injection queue (and its tail-counter contention with
         * peer dequeues).
         *
         * @return @c true on success; @c false if the inbox is full or
         *         the fiber is no longer accepting work, in which case
         *         the caller should try another fiber or fall back to
         *         the central injection queue.
         */
        bool tryPushInbox(TaskPtr task) noexcept;

        /**
         * @brief Steal one task from this fiber's deque on behalf of a peer.
         *        Multi-thief safe.
         *
         * @return The stolen TaskPtr or @c nullptr if the deque was empty or
         *         the stealer lost the race.
         */
        TaskPtr stealOne();

        /**
         * @brief Cooperative shutdown: stop pulling work and unblock any wait.
         *        Idempotent and safe to call concurrently with @ref process.
         */
        void stop();

        /**
         * @brief Wake the fiber if it is parked, so it can re-evaluate its
         *        work sources.
         */
        void poke();

        /**
         * @brief Workload bucket derived from local deque depth vs capacity.
         */
        Workload workload() const;

        /**
         * @brief @c true while the fiber is currently parked on its CV.
         */
        bool isParked() const noexcept {
            return this->parked.load(std::memory_order_acquire);
        }

        /**
         * @brief Slot identifier supplied at construction.
         */
        [[nodiscard]] FiberId id() const noexcept { return this->fiberId; }

        /**
         * @brief Native thread handle. Exposed for diagnostics; do not use to
         *        terminate the thread.
         */
        OsHandler osHandler();

        /**
         * @brief Test-only helper: returns the underlying deque capacity.
         */
        size_t dequeCapacity() const noexcept;

    private:
        /**
         * @brief Worker thread entry point. Drives the pop/refill/steal/park
         *        cycle until @ref running becomes false.
         */
        void process();

        /**
         * @brief Park on @ref condition until a wake source signals or a task
         *        becomes locally visible.
         */
        void park();

        /**
         * @brief Drain any remaining local tasks into the injection queue.
         *        Called by a retiring temp fiber so stragglers aren't lost.
         */
        void drainToPending();

        /**
         * @brief Pop a task from the local deque if any.
         */
        TaskPtr popLocal();

        /**
         * @brief Try a single peer-steal attempt.
         */
        TaskPtr trySteal();

        /**
         * @brief Cooperative-stop flag. Reads on the worker side use acquire to
         *        pair with writes from @ref stop (release).
         */
        std::atomic<bool> running{true};

        /**
         * @brief True while the worker is suspended in @ref park. Producers
         *        consult this to decide whether to call @ref poke.
         */
        std::atomic<bool> parked{false};

        /**
         * @brief Temp fibers self-retire when they observe sustained idleness;
         *        permanent fibers do not. Set once at construction.
         */
        const bool temp;

        /**
         * @brief Retirement callback. Invoked from the worker's own thread on
         *        the way out; transfers this Fiber to Yarn's graveyard.
         */
        SignalDone signalDone;

        /**
         * @brief Pull a burst from the injection queue into our deque.
         */
        GetFromPending getFromPending;

        /**
         * @brief Predicate used in the wait condition.
         */
        AnyPendingTasks anyPendingTasks;

        /**
         * @brief Overflow path for tasks we cannot accept locally (e.g. during
         *        retirement drain).
         */
        PushPending pushPending;

        /**
         * @brief Random-peer steal hook supplied by Yarn.
         */
        TryStealFromPeers tryStealFromPeers;

        /**
         * @brief Wake-one signal used to attract a stealer when we push to the
         *        local deque while peers may be parked.
         */
        WakeOneIdle wakeOneIdle;

        /**
         * @brief Park signal. Bumped by any producer that wants to wake this
         *        fiber. @c park() loads the value, marks itself parked, then
         *        @c std::atomic<>::wait()s on the captured value -- the kernel
         *        futex atomically checks the still-equal predicate before
         *        sleeping, which closes the classic missed-wake race without
         *        a mutex. This replaced an earlier
         *        @c std::condition_variable + @c std::mutex pair; profiling
         *        showed the mutex acquire on every @ref poke was a 50-100 ns
         *        per-wake tax and accumulated visibly under submit-heavy
         *        loads.
         */
        std::atomic<std::uint32_t> parkSignal{0};

        /**
         * @brief Per-worker MPMC inbox. Yarn::dispatch picks a target
         *        worker (round-robin) and pushes here before considering
         *        the central injection queue. The owning worker drains
         *        the inbox first thing in its process loop.
         *
         * Replaces an earlier 1-deep @c expressSlot. The single slot
         *        captured the peak case (sub-Tokio submit) but fell back
         *        to MPMC injection too aggressively under sustained
         *        burst, where the slot was always full. A small MPMC
         *        ring per worker smooths the median while keeping the
         *        partitioned-contention property.
         *
         * The ring lives on the heap to keep @c sizeof(Fiber) bounded
         *        and to avoid the cache-line alignment of @c Cell<T>
         *        bleeding into the Fiber header. One small alloc per
         *        Fiber at construction.
         */
        std::unique_ptr<MPMCQueue<TaskPtr>> inbox;

        /**
         * @brief Slot id, fixed at construction.
         */
        const FiberId fiberId;

        /**
         * @brief Local work-stealing deque. Owned by this fiber; stolen from
         *        by peers via @ref stealOne.
         */
        std::unique_ptr<Deque> deque;

        /**
         * @brief OS thread executing @ref process. Joined in the destructor.
         */
        std::thread thread;
    };

    using sFiber = std::shared_ptr<Fiber>;
    using Fibers = std::vector<sFiber>;

    /**
     * @brief Thread-local pointer to the currently executing Fiber. Set by
     *        @c Fiber::process. Non-worker threads see @c nullptr. Yarn::run
     *        uses this to dispatch in-worker submissions directly to the local
     *        deque without touching the global injection queue.
     */
    extern thread_local Fiber *tls_currentFiber;
}

#endif //YARN_FIBER_HPP
