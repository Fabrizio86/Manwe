//
// Created by Fabrizio Paino on 2022-01-15.
//

#ifndef YARN_FIBER_HPP
#define YARN_FIBER_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "ITask.hpp"
#include "Workload.hpp"

namespace YarnBall {

    class Yarn;

    namespace detail {
        /**
         * @brief Dispose of a task that will never run because the pool is shutting down: report
         *        @c std::runtime_error("... pool shut down ...") through @c ITask::exception (so completion hooks such as
         *        @c Waitable::wait unblock) and delete it. Exceptions from the hook are swallowed.
         */
        void discardTask(TaskPtr task) noexcept;

        /**
         * @brief The error handed to @c ITask::exception for a task that is discarded because the pool is shutting down.
         *        A distinct type so that fire-and-forget callables (whose @c exception() reports to the unhandled-exception
         *        handler) can tell an expected shutdown discard from a real failure and stay quiet about it.
         */
        class PoolShutdownError final : public std::runtime_error {
        public:
            PoolShutdownError() : std::runtime_error("YarnBall: pool shut down before the task could run") {}
        };
    }

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
         * @param anyPeerWork       Optional: reports work sitting in a peer's queues; consulted before sleeping.
         * @param owner             The pool this worker belongs to. The worker reports its parked state to it and drops its
         *                          snapshot cache through it, so it never has to go through @c Yarn::instance() (which would
         *                          block on the singleton's construction guard while the pool is still being built or is
         *                          unwinding from a failed construction). May be null for a stand-alone fiber.
         */
        Fiber(FiberId id,
              size_t dequeCapacity,
              SignalDone signalDone,
              GetFromPending getFromPending,
              AnyPendingTasks anyPendingTasks,
              PushPending pushPending,
              TryStealFromPeers tryStealFromPeers,
              WakeOneIdle wakeOneIdle,
              bool temp,
              AnyPeerWork anyPeerWork = {},
              Yarn *owner = nullptr);

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
        bool tryPushInbox(TaskPtr task);

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
         * @brief Block until this fiber's OS thread has exited. No-op when it is
         *        already joined or when called from that thread. Call @ref stop first.
         */
        void join();

        /**
         * @brief Drop the callback that forwards stranded tasks back into the scheduler. For Yarn::~Yarn only, after
         *        @ref join: ~Fiber then deletes stranded tasks instead of dispatching them into a scheduler that is
         *        being torn down. The fiber's thread must already be gone.
         */
        void abandonPending() noexcept;

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
         * @brief Reserve the next wake-up of this fiber for the caller. Returns @c true to exactly one caller per park:
         *        the claim is released when the fiber next goes to sleep. Lets concurrent wakers fan out over different
         *        parked fibers instead of all poking the first one (which has not yet marked itself running).
         */
        bool claimWake() noexcept {
            return !this->wakeClaimed.exchange(true, std::memory_order_acq_rel);
        }

        /**
         * @brief Number of tasks this worker has finished. Written only by the worker, read by the pool's stall
         *        detector as a progress signal.
         */
        std::uint64_t completedCount() const noexcept {
            return this->completed.load(std::memory_order_relaxed);
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
        friend class Yarn;

        /**
         * @brief Push a task onto the local deque. OWNER THREAD ONLY: the deque is single-producer, so this may only be
         *        called from this fiber's own worker thread (Yarn does so from @c dispatch and the injection refill,
         *        both running on the worker). Foreign threads go through @ref tryPushInbox.
         *
         * Returns @c false if the deque is full or the fiber is no longer accepting work; in that case the caller
         * retains ownership.
         *
         * @param task Raw owning pointer; on success ownership moves to the deque.
         * @return @c true on success.
         */
        bool seed(TaskPtr task);

        /**
         * @brief @c true if this fiber's deque or inbox holds a task a peer could take.
         */
        bool hasStealableWork() const noexcept;

        /**
         * @brief Bump the park signal and wake the sleeping worker, if any (all waiters when @p all).
         */
        void wakeParked(bool all) noexcept;

        /**
         * @brief Worker thread entry point. Drives the pop/refill/steal/park
         *        cycle until @ref running becomes false.
         */
        void process();

        /**
         * @brief Sleep until a wake source bumps @ref parkSignal or work becomes visible. Permanent workers wait on the
         *        futex; temp workers wait on @ref tempCv with a timeout so they can notice sustained idleness.
         */
        void park();

        /**
         * @brief Count one finished task (owner thread only).
         */
        void noteCompleted() noexcept;

        /**
         * @brief Drain any remaining local tasks into the injection queue.
         *        Called by a retiring temp fiber so stragglers aren't lost.
         */
        void drainToPending();

        /**
         * @brief Forward everything in the inbox (MPMC, safe from any thread) to the injection path.
         */
        void drainInboxToPending();

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
         * @brief Set by the waker that has taken responsibility for waking this fiber (see @ref claimWake); cleared by
         *        @ref park before it publishes @ref parked, so a fiber that is visibly parked always has a free claim.
         */
        std::atomic<bool> wakeClaimed{false};

        /**
         * @brief Tasks completed by this worker (progress signal for the stall detector).
         */
        std::atomic<std::uint64_t> completed{0};

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
         * @brief Peer-queue probe used by @ref park's last-chance re-check. May be empty.
         */
        AnyPeerWork anyPeerWork;

        /**
         * @brief Wake-one signal used to attract a stealer when we push to the
         *        local deque while peers may be parked.
         */
        WakeOneIdle wakeOneIdle;

        /**
         * @brief The owning pool (parked-worker accounting, snapshot cache release). Null for a stand-alone fiber.
         */
        Yarn *const owner;

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
         * @brief Timed-wait pair used only by temp fibers (a futex wait has no timeout). Wakers bump @ref parkSignal,
         *        then pass through @c tempMu before notifying so the sleeper cannot miss the change.
         */
        std::mutex tempMu;
        std::condition_variable tempCv;

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
