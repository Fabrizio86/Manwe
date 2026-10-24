//
// Created by Fabrizio Paino on 2022-01-14.
//

#include "Yarn.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <csignal>
#endif

#include "Coroutines.h"
#include "Log.h"
#include "RandomScheduler.hpp"
#include "SmallObjectPool.h"

namespace YarnBall {

    namespace {
        /**
         * @brief Handler slot for exceptions escaping detached tasks. Leaked on
         *        purpose: detached tasks can finish during static teardown.
         */
        struct UnhandledState {
            std::mutex mu;
            UnhandledExceptionHandler handler;
        };

        UnhandledState &unhandledState() {
            static UnhandledState *st = new UnhandledState;
            return *st;
        }
    }

    void setUnhandledExceptionHandler(UnhandledExceptionHandler handler) {
        auto &st = unhandledState();
        std::lock_guard<std::mutex> lk(st.mu);
        st.handler = std::move(handler);
    }

    void detail::reportDetachedException(std::exception_ptr e) noexcept {
        std::string what = "unknown exception";
        try {
            std::rethrow_exception(e);
        } catch (const CancelledException &) {
            return;
        } catch (const PoolShutdownError &) {
            // A callable that never ran because the pool was torn down: expected at exit, not a failure to report (and
            // the user's handler must not be entered during static teardown for it).
            return;
        } catch (const std::exception &ex) {
            what = ex.what();
        } catch (...) {
        }
        try {
            UnhandledExceptionHandler handler;
            {
                auto &st = unhandledState();
                std::lock_guard<std::mutex> lk(st.mu);
                handler = st.handler;
            }
            if (handler) {
                handler(e);
            } else {
                log::error("unhandled exception in detached task", {log::str("what", what)});
            }
        } catch (...) {
        }
    }

    namespace {
        /**
         * @brief Floor for the always-on worker count.
         */
        constexpr int kMinThreadsFloor = 4;

        /**
         * @brief Multiplier on hardware concurrency to compute MaxThreadCount.
         *        With hw=8 this yields a hard ceiling of ~28 workers.
         */
        constexpr double kOptimalMultiplier = 3.5;

        /**
         * @brief Number of tasks pulled from the injection queue per refill
         *        burst. Tuned to amortise the per-pull MPMC overhead without
         *        starving stealers.
         */
        constexpr int kPendingRefillBurst = 32;

        /**
         * @brief Per-fiber deque capacity. Must be a power of two; consumes
         *        @c capacity * sizeof(void*) bytes per worker.
         */
        constexpr size_t kFiberDequeCapacity = 1024;

        /**
         * @brief Maximum number of peer fibers consulted in a single steal
         *        attempt before giving up.
         */
        constexpr int kStealVisits = 4;

        /**
         * @brief Injection-queue depth at which we'll consider spawning a temp
         *        fiber. Avoids growing on transient single-task spikes.
         */
        constexpr size_t kGrowBacklogThreshold = 64;

        /**
         * @brief Period of the stall detector's check while work is outstanding.
         */
        constexpr auto kStallTick = std::chrono::milliseconds(10);

        /**
         * @brief Consecutive checks (@ref kStallTick apart) with queued work, no parked worker and not a single task
         *        finished before the pool is considered stalled (every worker blocked) and one more worker is added, or
         *        a parked one is poked. Bounds the growth rate to one worker per @c kStallTicks * kStallTick.
         */
        constexpr int kStallTicks = 5;

        /**
         * @brief Thread-local trampoline queue for
         *        @ref Yarn::enqueueInjection's last-resort
         *        inline-execution fallback (used once the local deque,
         *        every probed peer inbox, and the central injection
         *        queue are all full).
         *
         *        A task run inline this way may itself submit further
         *        work (coSpawn / scheduleOn), which re-enters
         *        @c dispatch on the same thread and, under sustained
         *        saturation -- the exact condition this fallback
         *        exists for -- lands right back in this same fallback.
         *        Naive recursion (task->run() -> dispatch() ->
         *        enqueueInjection() -> task->run() -> ...) would grow
         *        the native call stack without bound on a chained
         *        fan-out (e.g. a pipeline coroutine tree), risking a
         *        stack overflow at exactly peak load.
         *
         *        Rejecting the submission instead is not a safe
         *        alternative: @c ITask::exception() is a no-op for
         *        @c CoroutineITask (Coroutines.h) and @c CallableITask
         *        (below) -- the two task types that dominate this
         *        path -- so refusing to run a task here would silently
         *        strand a coroutine chain (or a syncWait caller)
         *        forever instead of failing loud. That is a worse
         *        production failure mode than the crash it would
         *        replace.
         *
         *        Instead this trampolines: when a drain loop is
         *        already active on this thread (non-null), a nested
         *        call just appends to it and returns immediately --
         *        an O(1) push, not a nested call. The outermost call
         *        becomes the drain loop and runs tasks (including any
         *        the loop itself pushes) until the queue is empty.
         *        Recursion depth is therefore O(1) regardless of
         *        fan-out depth, while every submitted task still runs
         *        to completion, in the same try/run/catch/exception
         *        shape as the normal dispatch path -- no task is ever
         *        silently dropped. The queue is heap-backed (bounded
         *        by available memory, i.e. the machine's real
         *        capacity) rather than the ~8 MiB native stack, so the
         *        failure mode under truly pathological backlog is
         *        graceful memory growth, not a stack-overflow crash.
         */
        thread_local std::vector<TaskPtr> *tlsInlineDrainQueue = nullptr;

        /**
         * @brief Internal adapter that lets a shared_ptr-owned task flow
         *        through the executor as a raw @c ITask*. The shared_ptr
         *        ref-count is touched exactly once on submission and once on
         *        destruction — never inside the hot per-pop / per-steal path.
         */
        class SharedOwnerAdapter final : public ITask {
        public:
            explicit SharedOwnerAdapter(sITask t) : inner(std::move(t)) {}

            ~SharedOwnerAdapter() override = default;

            void run() override { this->inner->run(); }

            void exception(std::exception_ptr e) override {
                this->inner->exception(std::move(e));
            }

            // Pool the alloc; same rationale as CoroutineITask above.
            static void *operator new(std::size_t) {
                return detail::poolAlloc<SharedOwnerAdapter>();
            }

            static void operator delete(void *p) noexcept {
                detail::poolFree<SharedOwnerAdapter>(p);
            }

        private:
            sITask inner;
        };
    } // namespace

    // Constant-initialised (no dynamic init), then computed on first Yarn construction, so a Yarn::instance() call made
    // from another translation unit's static initialiser cannot observe zeros or be overwritten afterwards.
    int Yarn::MinThreadCount = 0;
    int Yarn::MaxThreadCount = 0;

    /**
     * @brief Thread-local PRNG for stealing victim selection. Per-worker
     *        state eliminates the shared rng mutex on the hot path.
     */
    static thread_local std::mt19937 tls_stealRng{std::random_device{}()};

    namespace {
        // Written once, from the worker that runs ~Yarn (exit() inside a task); see shutdownWorkers. Volatile so the
        // store is not dropped as dead: the pointer is what keeps the allocation reachable for leak checkers.
        sFiber *volatile g_exitingWorkerKeepAlive = nullptr;

        void computeThreadCounts(int &minCount, int &maxCount) {
            minCount = std::max<int>(
                kMinThreadsFloor,
                static_cast<int>(std::thread::hardware_concurrency()));
            maxCount = std::max<int>(
                minCount,
                static_cast<int>(std::floor(minCount * kOptimalMultiplier)));
        }

        // Runs before the member initialisers below (which size queues from nothing, but keep the order explicit).
        inline bool threadCountsInit(int &minCount, int &maxCount) {
            computeThreadCounts(minCount, maxCount);
            return true;
        }
    }

    Yarn::Yarn()
        : fiberSnapshot(std::make_shared<const Fibers>()),
          injection(std::make_shared<InjectionQueue>(static_cast<std::size_t>(QueueSize::Huge))),
          scheduler(std::make_shared<RandomScheduler>()) {
#ifndef _WIN32
        // Suppress SIGPIPE so a peer disconnecting mid-write does not
        // terminate the whole process. The canonical setup for any
        // server runtime; without this, a single broken TCP / WebSocket
        // / pipe peer can kill us. Users who genuinely want SIGPIPE
        // can re-install their own handler after Yarn::instance().
        std::signal(SIGPIPE, SIG_IGN);
#endif
        threadCountsInit(Yarn::MinThreadCount, Yarn::MaxThreadCount);
        try {
            std::lock_guard<std::mutex> lk(this->cmu);
            this->fibers.resize(Yarn::MaxThreadCount);
            for (int i = 0; i < Yarn::MinThreadCount; ++i) {
                this->initializeNewThreadLocked(static_cast<FiberId>(i), /*markAsTemp=*/false);
            }
            this->rebuildSnapshotLocked();
        } catch (...) {
            // The destructor does not run for a throwing constructor, and the members the already-started workers use
            // (cmu and schedMu are declared last, so they go first) are destroyed during unwinding: stop and join the
            // workers before letting the exception leave.
            this->shutdownWorkers();
            throw;
        }
    }

    Yarn::~Yarn() {
        this->shutdownWorkers();
    }

    void Yarn::shutdownWorkers() {
        // Phase 1: stop all workers so anyone parked or mid-spin returns. Retired-but-unreaped fibers are collected
        // too: their thread may still be finishing its last drain, which reads pushPending.
        Fibers workers;
        {
            std::lock_guard<std::mutex> lk(this->cmu);
            // From here on no worker may be added: one spawned after the sweep below would never be stopped.
            this->shuttingDown = true;
            for (auto &f : this->fibers) {
                if (f) {
                    f->stop();
                    workers.push_back(f);
                }
            }
            for (auto &f : this->graveyard) {
                if (f) workers.push_back(f);
            }
        }
        // ~Yarn can run on a worker thread (exit() called from a task). That worker cannot join itself and keeps
        // running on its own Fiber after we return, so its Fiber must outlive this call: leak one reference on
        // purpose, skip joining/abandoning it, and let the process wind down.
        // The reference lives in a namespace-scope pointer, so the allocation stays reachable (leak checkers do not flag it).
        Fiber *const self = tls_currentFiber;
        for (auto &f : workers) {
            if (f.get() == self) {
                g_exitingWorkerKeepAlive = new sFiber(f);
                break;
            }
        }
        this->stopWatchdog();
        // Phase 1b: wait for every worker thread to be gone BEFORE any member of this
        // object is destroyed. Relying on ~Fiber to join (phase 2) is not enough: each
        // worker's thread_local snapshot cache holds a reference to every Fiber until that
        // worker leaves process(), so dropping `fibers` below is rarely the last reference,
        // ~Fiber does not run, ~Yarn returns, and a worker still inside stealFromPeers /
        // currentSnapshot then locks the destroyed `cmu` ("mutex lock failed: Invalid
        // argument" at process exit). Joined outside `cmu`: a worker may need it to finish.
        for (auto &f : workers) {
            if (f.get() != self) f->join();
        }
        // Tasks still sitting in a worker's inbox or deque are forwarded by ~Fiber through pushPending -> Yarn::dispatch, which would run against
        // this half-destroyed object (and recurse through the snapshot's Fiber references). Nothing runs them any more: let ~Fiber fail and delete them.
        for (auto &f : workers) {
            if (f.get() != self) f->abandonPending();
        }
        workers.clear();
        // Phase 2: dropping the sFibers triggers ~Fiber which joins. A worker
        // thread only gets here via the `self` path above, which never
        // destroys its own Fiber.
        {
            std::lock_guard<std::mutex> lk(this->cmu);
            this->fibers.clear();
            this->rebuildSnapshotLocked();
        }
        // Phase 3: collect any fibers that retired but were never reaped.
        this->reap();

        // Phase 4: anything stuck in the injection queue at shutdown never runs;
        // fail it (so Waitable::wait and friends return) and free it rather
        // than leaving the raw pointers to the queue destructor.
        TaskPtr t = nullptr;
        while (this->injection && this->injection->dequeue(t)) {
            detail::discardTask(t);
        }
    }

    Yarn *Yarn::instance() {
        static Yarn instance;
        return &instance;
    }

    sIScheduler Yarn::getScheduler() {
        std::lock_guard<std::mutex> lk(this->schedMu);
        return this->scheduler;
    }

    void Yarn::switchScheduler(sIScheduler s) {
        if (!s) return;
        std::lock_guard<std::mutex> lk(this->schedMu);
        this->scheduler = std::move(s);
    }

    Yarn::Stats Yarn::stats() const {
        Stats s;
        s.permanentWorkers = Yarn::MinThreadCount;
        s.maxWorkers = Yarn::MaxThreadCount;
        s.injectionDepth = this->injection->Size();
        // Count alive fibers + reapable retired ones. Hold @c cmu for
        // the short walk; the lock is uncontended in steady state.
        std::lock_guard<std::mutex> lk(this->cmu);
        for (const auto &f : this->fibers) {
            if (f) ++s.aliveWorkers;
        }
        s.reapableFibers = static_cast<int>(this->graveyard.size());
        return s;
    }

    namespace {
        /// Test hook state (see Yarn::failSubmissionsForTest), per thread so that unrelated pool traffic cannot consume
        /// it: submissions to let through, then submissions to fail.
        thread_local int tlsFailSkip = 0;
        thread_local int tlsFailCount = 0;

        /// Throws std::bad_alloc when the test hook says this submission must fail. One TLS load when unused.
        inline void maybeInjectSubmitFailure() {
            if (tlsFailCount <= 0) return;
            if (tlsFailSkip > 0) {
                --tlsFailSkip;
                return;
            }
            --tlsFailCount;
            throw std::bad_alloc();
        }

        /// Run a task on the calling thread and free it: the last resort when it cannot be queued anywhere.
        inline void runInlineAndFree(TaskPtr t) noexcept {
            try { t->run(); } catch (...) {
                try { t->exception(std::current_exception()); } catch (...) { }
            }
            delete t;
        }
    }

    bool Yarn::awaitQuiescence(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        constexpr int kSettlePolls = 5;
        int idlePolls = 0;
        for (;;) {
            bool idle = this->injection->Size() == 0;
            if (idle) {
                const auto snap = this->currentSnapshot();
                if (snap) {
                    for (const auto &f : *snap) {
                        if (f && (!f->isParked() || f->hasStealableWork())) {
                            idle = false;
                            break;
                        }
                    }
                }
            }
            idlePolls = idle ? idlePolls + 1 : 0;
            if (idlePolls >= kSettlePolls) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void Yarn::failSubmissionsForTest(int skip, int count) noexcept {
        tlsFailSkip = skip;
        tlsFailCount = count;
    }

    void Yarn::run(uITask task) {
        if (!task) return;
        // Anything that throws before dispatch() leaves the task in this by-value parameter, which destroys it once.
        maybeInjectSubmitFailure();
        this->reap();
        this->dispatch(task.release());
    }

    void Yarn::run(sITask task) {
        if (!task) return;
        maybeInjectSubmitFailure();
        this->reap();
        // One small allocation per submission carries the shared_ptr to the
        // worker. The pool fast path itself never sees a shared_ptr.
        uITask adapter{new SharedOwnerAdapter(std::move(task))};
        this->dispatch(adapter.release());
    }

    void Yarn::dispatch(TaskPtr task) noexcept {
        // dispatch owns the task from here on and cannot fail: every step either publishes it (queue or inline run) or
        // is a best-effort extra (snapshot, wake, grow) whose failure only means the next step is tried. A throw out of
        // seed()/tryPushInbox() can only come from the wake-up that follows the (noexcept) push, i.e. the task is
        // already published, so it is treated as success.

        // In-worker fast path: push directly to our own local deque. No lock,
        // no atomic CAS, no shared_ptr touch.
        if (Fiber *self = tls_currentFiber) {
            try {
                if (self->seed(task)) return;
            } catch (...) {
                return;
            }
            // Local deque full -- fall through to express / injection.
        }

        // Out-of-worker submit: try to publish into a round-robin-chosen
        // worker's bounded MPMC inbox. Partitions submit traffic across
        // workers so the central injection queue's tail counter is not
        // contended with peer dequeues. Workers drain their inbox first
        // in their process loop.
        std::shared_ptr<const Fibers> snap;
        try { snap = this->currentSnapshot(); } catch (...) { }
        if (snap && !snap->empty()) {
            const std::size_t n = snap->size();
            const std::size_t base =
                this->nextInboxIdx.fetch_add(1, std::memory_order_relaxed);
            // Probe up to three workers starting at the round-robin index.
            // With 32-slot inboxes and typical worker drain rates this is
            // almost always enough; if every probed inbox is full we fall
            // back to the central injection queue.
            for (std::size_t probe = 0; probe < 3; ++probe) {
                Fiber *target = (*snap)[(base + probe) % n].get();
                try {
                    if (target && target->tryPushInbox(task)) return;
                } catch (...) {
                    return;
                }
            }
        }

        this->enqueueInjection(task);
    }

    void Yarn::enqueueInjection(TaskPtr task) noexcept {
        if (this->injection->enqueue(task)) {
            // The task is published; waking or growing is best effort (the running workers drain the queue anyway).
            try {
                // Hot path: just wake someone. cmu is *not* taken unless we
                // observe genuine backlog with no parked worker to absorb it.
                if (this->wakeOneParked()) return;
                if (this->injection->Size() < kGrowBacklogThreshold) return;

                // Skip the cmu acquire entirely when the pool is already at
                // its temp-fiber ceiling -- there's nothing maybeGrowLocked
                // would do that we'd care about.
                const int maxTemps = Yarn::MaxThreadCount - Yarn::MinThreadCount;
                if (this->tempFiberCount.load(std::memory_order_acquire) >= maxTemps) {
                    return;
                }

                std::lock_guard<std::mutex> lk(this->cmu);
                this->maybeGrowLocked();
            } catch (...) {
            }
            return;
        }

        // Injection queue is full. Try to grow first; if that fails too, run
        // inline on the caller. Inline execution under pool exhaustion is the
        // standard back-pressure fallback (.NET, Folly, Tokio all do this).
        //
        // seed() is deliberately called AFTER releasing cmu, not
        // inside the lock scope: seed() can call wakeOneIdle ->
        // wakeOneParked -> currentSnapshot, and maybeGrowLocked just
        // rebuilt the snapshot under this same lock, which guarantees
        // *this* thread's cached snapshot is stale -- so if any other
        // worker anywhere happens to be parked, currentSnapshot's slow
        // path would try to re-lock cmu on the thread that already
        // holds it, self-deadlocking and wedging the whole pool (see
        // cmu's invariant comment in Yarn.hpp). `grown` is an sFiber
        // (shared_ptr), not a raw pointer, copied out while still
        // holding cmu, so the Fiber stays alive even if it retires
        // concurrently between releasing the lock and calling seed().
        sFiber grown;
        try {
            std::lock_guard<std::mutex> lk(this->cmu);
            int newId = this->maybeGrowLocked();
            if (newId >= 0 && this->fibers[newId]) grown = this->fibers[newId];
        } catch (...) {
            // Could not grow (e.g. thread creation failed): fall through to the inline fallback.
        }
        // The deque is single-producer (owner only); this thread is a foreign producer, so go through the MPMC inbox.
        try {
            if (grown && grown->tryPushInbox(task)) return;
        } catch (...) {
            return; // published; only the wake-up threw
        }

        // Trampoline instead of recurse -- see tlsInlineDrainQueue's
        // comment for the full rationale. A nested re-entry (this
        // thread is already draining) just parks the task and returns;
        // O(1), no added native stack depth.
        if (tlsInlineDrainQueue) {
            try {
                tlsInlineDrainQueue->push_back(task);
            } catch (...) {
                runInlineAndFree(task); // out of memory: nest rather than lose the task
            }
            return;
        }

        // We are the outermost call: become the drain loop. Any task
        // run below that submits further work re-enters this function,
        // sees tlsInlineDrainQueue set, and appends to `drain` instead
        // of recursing -- so this loop, not the call stack, absorbs
        // however deep the fan-out goes.
        //
        // The thread-local pointer is reset via RAII, not a plain
        // trailing assignment: nothing in the loop body is expected to
        // throw past its own try/catch (ITask's pooled operator delete
        // implementations are noexcept), but if something ever did, a
        // trailing `tlsInlineDrainQueue = nullptr;` would be skipped,
        // leaving the pointer dangling at this now-destroyed `drain` --
        // the next call on this thread would push_back through a
        // stale pointer into freed stack memory. The guard makes that
        // unreachable by construction instead of relying on the loop
        // body never throwing.
        struct DrainQueueGuard {
            explicit DrainQueueGuard(std::vector<TaskPtr> &d) noexcept {
                tlsInlineDrainQueue = &d;
            }
            ~DrainQueueGuard() noexcept { tlsInlineDrainQueue = nullptr; }
        };
        std::vector<TaskPtr> drain;
        try {
            drain.push_back(task);
        } catch (...) {
            runInlineAndFree(task);
            return;
        }
        DrainQueueGuard guard(drain);
        while (!drain.empty()) {
            TaskPtr t = drain.back();
            drain.pop_back();
            runInlineAndFree(t);
        }
    }

    int Yarn::maybeGrowLocked() {
        if (this->shuttingDown || this->maxLimitReachedLocked()) return -1;
        const FiberId id = this->firstUnusedTempIdLocked();
        if (id >= this->fibers.size()) return -1;
        this->initializeNewThreadLocked(id, /*markAsTemp=*/true);
        // Paired with the decrement in signalDone (id is in the temp range
        // because firstUnusedTempIdLocked starts the search at MinThreadCount).
        this->tempFiberCount.fetch_add(1, std::memory_order_release);
        this->rebuildSnapshotLocked();
        return static_cast<int>(id);
    }

    namespace {
        /**
         * @brief Per-thread cached snapshot used by @ref Yarn::currentSnapshot.
         *        Lives at namespace scope (not inside the function) so worker
         *        threads can explicitly release it via
         *        @ref Yarn::releaseThreadSnapshotCache before they exit,
         *        avoiding a self-join during thread_local teardown.
         */
        //
        // The cache is refreshed only when this thread next asks for a snapshot after a version bump, so a thread that
        // stops submitting keeps its last snapshot, and with it the retired Fibers in it, alive until it submits again
        // or exits. That only delays the final ~Fiber (a joined, thread-less object) of a few retired temp fibers.
        thread_local std::shared_ptr<const Fibers> tlsCachedSnapshot;
        thread_local std::uint64_t tlsCachedSnapshotVersion = 0;
    }

    std::shared_ptr<const Fibers> Yarn::currentSnapshot() {
        const std::uint64_t v =
            this->snapshotVersion.load(std::memory_order_acquire);
        if (v == tlsCachedSnapshotVersion && tlsCachedSnapshot) {
            return tlsCachedSnapshot;
        }

        std::lock_guard<std::mutex> lk(this->cmu);
        tlsCachedSnapshot = this->fiberSnapshot;
        tlsCachedSnapshotVersion =
            this->snapshotVersion.load(std::memory_order_relaxed);
        return tlsCachedSnapshot;
    }

    void Yarn::releaseThreadSnapshotCache() noexcept {
        tlsCachedSnapshot.reset();
        tlsCachedSnapshotVersion = 0;
    }

    bool Yarn::wakeOneParked() {
        // The caller has just published work (queue enqueue); pairs with the fence in Fiber::park() so a worker that
        // is about to sleep either is counted here or finds the work in its re-check.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        // Whoever publishes work makes sure the stall detector is watching it: a wake can be wasted (the woken worker
        // may pick another task and then block), and only the detector notices a pool in which every worker is stuck.
        this->armWatchdog();
        // Fast skip: when no worker is parked, no walk, no snapshot
        // copy, no shared_ptr ref-count traffic. Steady-state submit
        // loads with all workers busy hit this branch unconditionally,
        // which is the win the parkedWorkers counter exists for.
        if (this->parkedWorkers.load(std::memory_order_acquire) == 0) {
            return false;
        }
        auto snap = this->currentSnapshot();
        if (!snap) return false;
        for (const auto &f : *snap) {
            // A parked fiber stays "parked" until it actually runs again, so without the claim a burst of submissions
            // would poke the same first fiber every time and leave the others asleep next to queued work.
            if (f && f->isParked() && f->claimWake()) {
                f->poke();
                return true;
            }
        }
        return false;
    }

    void Yarn::armWatchdog() noexcept {
        if (this->wdArmed.load(std::memory_order_acquire)) return;
        try {
            std::lock_guard<std::mutex> lk(this->wdMu);
            if (this->wdStop) return;
            if (!this->wdThread.joinable()) {
                this->wdThread = std::thread(&Yarn::watchdogLoop, this);
            }
            this->wdArmed.store(true, std::memory_order_seq_cst);
        } catch (...) {
            // No watchdog thread (resource exhaustion): the pool still works, it just cannot repair a stall.
            return;
        }
        this->wdCv.notify_one();
    }

    void Yarn::stopWatchdog() noexcept {
        {
            std::lock_guard<std::mutex> lk(this->wdMu);
            this->wdStop = true;
        }
        this->wdCv.notify_all();
        // Never self-join: the watchdog thread does not run ~Yarn, but a worker may; the watchdog is a different thread.
        if (this->wdThread.joinable() && this->wdThread.get_id() != std::this_thread::get_id()) {
            this->wdThread.join();
        }
    }

    bool Yarn::hasOutstandingWork() {
        if (!this->injection->empty()) return true;
        const auto snap = this->currentSnapshot();
        if (!snap) return false;
        for (const auto &f : *snap) {
            if (f && f->hasStealableWork()) return true;
        }
        return false;
    }

    std::uint64_t Yarn::completedTotal() {
        std::uint64_t total = 0;
        const auto snap = this->currentSnapshot();
        if (snap) {
            for (const auto &f : *snap) {
                if (f) total += f->completedCount();
            }
        }
        return total;
    }

    void Yarn::watchdogLoop() {
        // Detects a pool in which every worker is blocked (a task waiting on work that sits in a queue behind it) and
        // repairs it by adding a worker. It sleeps on a condition variable until a submission arms it, ticks only while
        // work is outstanding, and disarms itself the moment the queues are empty, so an idle pool costs nothing.
        std::unique_lock<std::mutex> lk(this->wdMu);
        while (!this->wdStop) {
            this->wdCv.wait(lk, [this] { return this->wdStop || this->wdArmed.load(std::memory_order_acquire); });
            std::uint64_t last = 0;
            bool haveLast = false;
            int stagnant = 0;
            while (!this->wdStop && this->wdArmed.load(std::memory_order_acquire)) {
                this->wdCv.wait_for(lk, kStallTick, [this] { return this->wdStop; });
                if (this->wdStop) break;
                lk.unlock();
                try {
                    if (!this->hasOutstandingWork()) {
                        // Disarm, then look again: a producer that published after the check above but before the
                        // flag was cleared saw the flag still set and did not re-arm. Pairs with the fence in wakeOneParked.
                        this->wdArmed.store(false, std::memory_order_seq_cst);
                        std::atomic_thread_fence(std::memory_order_seq_cst);
                        if (this->hasOutstandingWork()) this->wdArmed.store(true, std::memory_order_seq_cst);
                        haveLast = false;
                        stagnant = 0;
                    } else {
                        const std::uint64_t now = this->completedTotal();
                        if (!haveLast || now != last) {
                            last = now;
                            haveLast = true;
                            stagnant = 0;
                        } else if (++stagnant >= kStallTicks) {
                            stagnant = 0;
                            this->resolveStall();
                        }
                    }
                } catch (...) {
                    // Best effort; try again on the next tick.
                }
                lk.lock();
            }
        }
        lk.unlock();
        this->releaseThreadSnapshotCache();
    }

    void Yarn::resolveStall() {
        // Work is queued and nothing has completed for kStallTicks ticks. If a worker is asleep it missed its wake:
        // poke it. Otherwise every worker is busy or blocked: add one.
        if (this->parkedWorkers.load(std::memory_order_acquire) > 0) {
            const auto snap = this->currentSnapshot();
            if (snap) {
                for (const auto &f : *snap) {
                    if (f && f->isParked()) f->poke();
                }
            }
            return;
        }
        std::lock_guard<std::mutex> lk(this->cmu);
        this->maybeGrowLocked();
    }

    TaskPtr Yarn::stealFromPeers(FiberId selfId) {
        auto snap = this->currentSnapshot();
        if (!snap || snap->size() < 2) return nullptr;

        const int n = static_cast<int>(snap->size());
        std::uniform_int_distribution<int> dist(0, n - 1);
        const int start = dist(tls_stealRng);

        for (int i = 0; i < std::min(kStealVisits, n); ++i) {
            const int idx = (start + i) % n;
            const sFiber &peer = (*snap)[idx];
            if (!peer || peer->id() == selfId) continue;
            if (TaskPtr t = peer->stealOne()) return t;
        }
        return nullptr;
    }

    void Yarn::rebuildSnapshotLocked() {
        auto snap = std::make_shared<Fibers>();
        snap->reserve(this->fibers.size());
        for (const auto &f : this->fibers) {
            if (f) snap->push_back(f);
        }
        this->fiberSnapshot = std::move(snap);
        // Release ordering pairs with the acquire load in currentSnapshot
        // so a thread that observes the bumped version sees the new
        // fiberSnapshot when it next takes cmu to refresh its cache.
        this->snapshotVersion.fetch_add(1, std::memory_order_release);
    }

    void Yarn::initializeNewThreadLocked(FiberId id, bool markAsTemp) {
        SignalDone signalDone = [this](FiberId fid) {
            {
                std::lock_guard<std::mutex> lk(this->cmu);
                if (fid < this->fibers.size() && this->fibers[fid]) {
                    this->graveyard.push_back(std::move(this->fibers[fid]));
                    this->fibers[fid] = nullptr;
                    this->rebuildSnapshotLocked();
                    // Counter publication must be release-ordered relative to
                    // the push so reap() observes the new entry once it sees
                    // the bumped count.
                    this->graveyardCount.store(this->graveyard.size(),
                                                std::memory_order_release);
                    // A temp fiber just exited; matched by the increment in
                    // maybeGrowLocked when it was spawned.
                    if (fid >= static_cast<FiberId>(Yarn::MinThreadCount)) {
                        this->tempFiberCount.fetch_sub(1, std::memory_order_release);
                    }
                }
            }
        };

        // Snapshot the live-fiber list lock-free, find ourselves, and refill.
        // Looking through cmu here would re-introduce the contention this whole
        // refactor exists to avoid.
        GetFromPending getFromPending = [this](FiberId fid) {
            auto snap = this->currentSnapshot();
            if (!snap) return;
            sFiber f;
            for (const auto &candidate : *snap) {
                if (candidate && candidate->id() == fid) {
                    f = candidate;
                    break;
                }
            }
            if (!f) return;

            TaskPtr t = nullptr;
            for (int i = 0; i < kPendingRefillBurst && this->injection->dequeue(t); ++i) {
                if (!f->seed(t)) {
                    if (!this->injection->enqueue(t)) {
                        // Queue refilled by concurrent producers: run it here rather than drop it.
                        try { t->run(); } catch (...) {
                            try { t->exception(std::current_exception()); } catch (...) { }
                        }
                        delete t;
                    }
                    break;
                }
            }
        };

        AnyPendingTasks anyPendingTasks = [this]() {
            return !this->injection->empty();
        };

        PushPending pushPending = [this](TaskPtr t) {
            // Route through the normal dispatch path -- which itself
            // tries the injection queue, then falls back through
            // enqueueInjection's trampolined inline-execution path
            // under saturation -- instead of duplicating a second,
            // unhardened "run it inline if the queue is full" here.
            // Keeps exactly one implementation of "what do we do with
            // a task that can't be queued normally" in the codebase,
            // so hardening applied there (e.g. the trampoline that
            // bounds native stack depth under sustained saturation)
            // automatically covers this call site too. Safe to call
            // from the retiring fiber's own thread: tls_currentFiber
            // still points at `this` fiber here, but its `running`
            // flag was already flipped false by the caller, so
            // dispatch's in-worker fast path (self->seed) correctly
            // declines and falls through to peer/injection/trampoline.
            this->dispatch(t);
        };

        TryStealFromPeers tryStealFromPeers = [this](FiberId fid) {
            return this->stealFromPeers(fid);
        };

        AnyPeerWork anyPeerWork = [this](FiberId fid) {
            auto snap = this->currentSnapshot();
            if (!snap) return false;
            for (const auto &peer : *snap) {
                if (peer && peer->id() != fid && peer->hasStealableWork()) return true;
            }
            return false;
        };

        WakeOneIdle wakeOneIdle = [this]() {
            this->wakeOneParked();
        };

        auto fiber = std::make_shared<Fiber>(
            id, kFiberDequeCapacity,
            std::move(signalDone),
            std::move(getFromPending),
            std::move(anyPendingTasks),
            std::move(pushPending),
            std::move(tryStealFromPeers),
            std::move(wakeOneIdle),
            markAsTemp,
            std::move(anyPeerWork),
            this);
        this->fibers[id] = std::move(fiber);
    }

    bool Yarn::maxLimitReachedLocked() const {
        // Permanent slots live in [0, MinThreadCount); temp slots live in
        // [MinThreadCount, end). We are full only when every temp slot is in use.
        const auto begin = this->fibers.begin() + Yarn::MinThreadCount;
        return !std::any_of(begin, this->fibers.end(),
                            [](const sFiber &f) { return f == nullptr; });
    }

    FiberId Yarn::firstUnusedTempIdLocked() const {
        auto it = std::find_if(this->fibers.begin() + Yarn::MinThreadCount,
                               this->fibers.end(),
                               [](const sFiber &f) { return f == nullptr; });
        return static_cast<FiberId>(std::distance(this->fibers.begin(), it));
    }

    void Yarn::reap() {
        // Hot-path fast skip: if there's nothing to join we never touch cmu.
        // graveyardCount is updated under cmu after the corresponding
        // graveyard.push_back, with release ordering paired against this
        // acquire load.
        if (this->graveyardCount.load(std::memory_order_acquire) == 0) return;

        std::vector<sFiber> toJoin;
        {
            std::lock_guard<std::mutex> lk(this->cmu);
            toJoin.swap(this->graveyard);
            this->graveyardCount.store(0, std::memory_order_release);
        }
        // Destruction here joins the worker threads on a non-worker thread.
    }
}
