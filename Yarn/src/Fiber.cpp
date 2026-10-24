//
// Created by Fabrizio Paino on 2022-01-16.
//

#include "Fiber.hpp"
#include "ITask.hpp"
#include "Yarn.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
    #include <windows.h>

/**
 * @brief Windows placeholder. A real implementation would query NUMA topology
 *        via GetNumaHighestNodeNumber / GetNumaProcessorNodeEx and call
 *        SetThreadAffinityMask. Kept as a no-op for now.
 */
static void SetNumaAffinity(YarnBall::FiberId) {
}

#elif defined(__linux__)
    #include <sched.h>
    #include <pthread.h>

/**
 * @brief Whether @c YARN_PIN_WORKERS=1 asked for hard pinning. Read once (the first Fiber is built while the pool is
 *        constructed, before any worker exists), so later spawns never call getenv concurrently with a user's setenv.
 */
static bool pinWorkersEnabled() {
    static const bool enabled = [] {
        const char *opt = std::getenv("YARN_PIN_WORKERS");
        return opt && opt[0] == '1';
    }();
    return enabled;
}

/**
 * @brief Linux affinity, opt-in (@c YARN_PIN_WORKERS=1): pin worker @p id to the (@p id mod N)-th CPU of the process's
 *        allowed set (cpuset / cgroup / taskset), so the workers spread over the CPUs instead of piling onto whichever
 *        one each happened to start on. Off by default because a hard single-CPU pin can never migrate off a busy core.
 */
static void SetNumaAffinity(YarnBall::FiberId id) {
    if (!pinWorkersEnabled()) return;
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return;
    const int count = CPU_COUNT(&allowed);
    if (count <= 0) return;
    int want = static_cast<int>(id % static_cast<unsigned>(count));
    int cpu = -1;
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (!CPU_ISSET(i, &allowed)) continue;
        if (want-- == 0) { cpu = i; break; }
    }
    if (cpu < 0) return;
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(cpu, &one);
    pthread_setaffinity_np(pthread_self(), sizeof(one), &one);
}

#elif defined(__APPLE__)
    #include <pthread.h>
    #include <mach/mach.h>
    #include <mach/thread_policy.h>

/**
 * @brief macOS affinity hint. The kernel cannot pin a thread to a specific
 *        core, but @c THREAD_AFFINITY_POLICY lets us tag threads so the
 *        scheduler tries to keep them on the same L2 group. All Yarn workers
 *        share affinity tag 1 to encourage cache locality for stealing.
 */
static void SetNumaAffinity(YarnBall::FiberId) {
    thread_t mach_thread = pthread_mach_thread_np(pthread_self());
    thread_affinity_policy_data_t policy = {1};
    thread_policy_set(mach_thread,
                      THREAD_AFFINITY_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy),
                      THREAD_AFFINITY_POLICY_COUNT);
}

#else
/**
 * @brief No-op fallback for unrecognised platforms.
 */
static void SetNumaAffinity(YarnBall::FiberId) {}
#endif


namespace YarnBall {

    /**
     * @brief Spin iterations spent trying steal+inject before parking.
     */
    static constexpr int kSpinBeforePark = 64;

    /**
     * @brief Idle ticks a temp fiber tolerates before retiring. Each tick is
     *        the duration of one park; the count gives temp fibers a grace
     *        window so they survive short pauses between bursts of work.
     */
    static constexpr int kTempIdleTicksBeforeRetire = 4;

    /**
     * @brief Maximum time a temp worker stays parked without an explicit
     *        wake before it re-evaluates (and, once idle long enough, retires).
     *        Permanent workers park until poked.
     */
    static constexpr auto kParkWaitTimeout = std::chrono::milliseconds(5);

    /**
     * @brief Capacity of each fiber's per-worker MPMC inbox. Must be a
     *        power of two. 32 entries is the sweet spot between
     *        "smooths over short bursts" and "occupies one cache page".
     */
    static constexpr std::size_t kInboxCapacity = 32;

    thread_local Fiber *tls_currentFiber = nullptr;

    void detail::discardTask(TaskPtr task) noexcept {
        if (!task) return;
        try {
            task->exception(std::make_exception_ptr(PoolShutdownError()));
        } catch (...) {
        }
        delete task;
    }

    Fiber::Fiber(FiberId id,
                 size_t dequeCapacity,
                 SignalDone signalDone,
                 GetFromPending getFromPending,
                 AnyPendingTasks anyPendingTasks,
                 PushPending pushPending,
                 TryStealFromPeers tryStealFromPeers,
                 WakeOneIdle wakeOneIdle,
                 bool temp,
                 AnyPeerWork anyPeerWork,
                 Yarn *owner)
        : temp(temp),
          signalDone(std::move(signalDone)),
          getFromPending(std::move(getFromPending)),
          anyPendingTasks(std::move(anyPendingTasks)),
          pushPending(std::move(pushPending)),
          tryStealFromPeers(std::move(tryStealFromPeers)),
          anyPeerWork(std::move(anyPeerWork)),
          wakeOneIdle(std::move(wakeOneIdle)),
          owner(owner),
          inbox(std::make_unique<MPMCQueue<TaskPtr>>(kInboxCapacity)),
          fiberId(id),
          deque(std::make_unique<Deque>(dequeCapacity)) {
        // Resolve the pinning option on the constructing thread (see pinWorkersEnabled) before the worker starts.
#ifdef __linux__
        (void) pinWorkersEnabled();
#endif
        this->thread = std::thread(&Fiber::process, this);
    }

    Fiber::~Fiber() {
        this->stop();
        if (this->thread.joinable()) {
            if (this->thread.get_id() == std::this_thread::get_id()) {
                // Reachable: a worker that drops the last reference to its own Fiber (its thread-local snapshot cache,
                // released at the end of process()) destroys it on its own thread. A join would self-deadlock, so
                // detach; process() touches no member after releasing the cache, so the thread can finish unharmed.
                this->thread.detach();
            } else {
                this->thread.join();
            }
        }

        // The worker is gone (or past its last member access), so whatever is left in the inbox and the local deque
        // was never run. Producers can squeeze a push into the inbox after the retirement drain, and a stopped worker
        // leaves its deque as is. Forward the tasks if the scheduler is still alive; otherwise (Yarn::~Yarn dropped
        // pushPending) fail them through ITask::exception so waiters wake up, and delete.
        auto reclaim = [this](TaskPtr t) {
            if (this->pushPending) this->pushPending(t);
            else detail::discardTask(t);
        };
        if (this->inbox) {
            TaskPtr stranded = nullptr;
            while (this->inbox->dequeue(stranded)) reclaim(stranded);
        }
        if (this->deque) {
            TaskPtr leftover = nullptr;
            while (this->deque->pop(leftover)) reclaim(leftover);
        }
    }

    bool Fiber::seed(TaskPtr task) {
        // Single-producer deque: only the owning worker may push.
        assert(tls_currentFiber == this);
        if (!task) return false;
        if (!this->running.load(std::memory_order_acquire)) return false;
        if (!this->deque->push(task)) return false;
        // Pokes ourselves (no-op if already running) and asks Yarn to wake
        // one parked peer so it can come steal. With the CV-timeout wake
        // removed from park(), this is the only way a parked peer learns
        // about new local work.
        this->poke();
        if (this->wakeOneIdle) this->wakeOneIdle();
        return true;
    }

    bool Fiber::tryPushInbox(TaskPtr task) {
        if (!task) return false;
        if (!this->running.load(std::memory_order_acquire)) return false;
        if (!this->inbox->enqueue(task)) {
            // Inbox full. Caller will probe another fiber or fall back
            // to the central injection queue.
            return false;
        }
        // Bump parkSignal even when parked reads false: closes the race
        // where this fiber is about to call park() between our enqueue
        // and the parked.load below. With parkSignal incremented,
        // parkSignal.wait(observed) on the about-to-park side sees a
        // mismatch and returns immediately. notify_one is only issued
        // when the worker is currently parked, sparing the kernel call
        // in the steady state.
        this->parkSignal.fetch_add(1, std::memory_order_release);
        // Store-load handshake with park(): either we see parked == true, or park()'s re-check sees our enqueue.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (this->parked.load(std::memory_order_acquire)) {
            this->wakeParked(false);
        } else if (this->wakeOneIdle) {
            // Target is busy: let an idle peer know it can steal from the inbox.
            this->wakeOneIdle();
        }
        // A retiring fiber may have run its final inbox drain between our running check and the enqueue: forward
        // whatever is left so the task is not stranded in a fiber nobody will run again.
        if (!this->running.load(std::memory_order_acquire) && this->pushPending) {
            this->drainInboxToPending();
        }
        return true;
    }

    TaskPtr Fiber::stealOne() {
        TaskPtr t = nullptr;
        if (this->deque->steal(t)) return t;
        // The inbox is MPMC, so a peer may take from it too; otherwise work routed to a busy worker's inbox
        // would wait for that worker even while the rest of the pool is idle.
        if (this->inbox->dequeue(t)) return t;
        return nullptr;
    }

    void Fiber::stop() {
        this->running.store(false, std::memory_order_release);
        // Bump the park signal and wake every waiter. A waiter checks the
        // value at suspension time, so even if the fiber is about to park it
        // observes the bump and skips the wait.
        this->parkSignal.fetch_add(1, std::memory_order_release);
        this->wakeParked(true);
    }

    void Fiber::wakeParked(bool all) noexcept {
        if (this->temp) {
            // Temp fibers sleep on tempCv. Passing through tempMu orders the caller's parkSignal bump before the
            // sleeper's predicate check or its wait, so the notify below cannot be lost.
            { std::lock_guard<std::mutex> lk(this->tempMu); }
            if (all) this->tempCv.notify_all();
            else this->tempCv.notify_one();
        } else if (all) {
            this->parkSignal.notify_all();
        } else {
            this->parkSignal.notify_one();
        }
    }

    bool Fiber::hasStealableWork() const noexcept {
        return !this->deque->empty() || !this->inbox->empty();
    }

    void Fiber::join() {
        if (this->thread.joinable() && this->thread.get_id() != std::this_thread::get_id()) {
            this->thread.join();
        }
    }

    void Fiber::abandonPending() noexcept {
        this->pushPending = nullptr;
    }

    void Fiber::poke() {
        // Fast skip when the worker isn't parked; spares a notify syscall
        // in the common busy-worker case.
        std::atomic_thread_fence(std::memory_order_seq_cst); // pairs with the one in park()
        if (!this->parked.load(std::memory_order_acquire)) return;
        this->parkSignal.fetch_add(1, std::memory_order_release);
        this->wakeParked(false);
    }

    void Fiber::process() {
        SetNumaAffinity(this->fiberId);
        tls_currentFiber = this;

        int idleTicks = 0;

        while (this->running.load(std::memory_order_acquire)) {
            // 0. Drain our inbox first. Submitters partition incoming
            //    work across worker inboxes (via tryPushInbox); the
            //    bounded MPMC ring per worker bypasses the central
            //    injection queue's contention. We pull only one entry
            //    per loop iteration so that the inbox can't starve the
            //    other sources (local deque / steal / park).
            TaskPtr fromInbox = nullptr;
            if (this->inbox->dequeue(fromInbox)) {
                idleTicks = 0;
                try {
                    fromInbox->run();
                } catch (...) {
                    try { fromInbox->exception(std::current_exception()); }
                    catch (...) { }
                }
                delete fromInbox;
                this->noteCompleted();
                continue;
            }

            // 1. Local LIFO pop. Hot path, no atomics beyond the deque itself.
            if (TaskPtr t = this->popLocal()) {
                idleTicks = 0;
                try {
                    t->run();
                } catch (...) {
                    try { t->exception(std::current_exception()); }
                    catch (...) { /* swallow secondary exceptions */ }
                }
                delete t;
                this->noteCompleted();
                continue;
            }

            // 2. Refill from injection queue.
            if (this->anyPendingTasks()) {
                this->getFromPending(this->fiberId);
                continue;
            }

            // 3. Try to steal a single task from a peer.
            if (TaskPtr t = this->trySteal()) {
                idleTicks = 0;
                try {
                    t->run();
                } catch (...) {
                    try { t->exception(std::current_exception()); }
                    catch (...) { }
                }
                delete t;
                this->noteCompleted();
                continue;
            }

            // 4. Spin for a bounded window trying steal+inject before parking.
            bool foundWork = false;
            for (int spin = 0; spin < kSpinBeforePark; ++spin) {
                if (this->anyPendingTasks()) { foundWork = true; break; }
                if (TaskPtr t = this->trySteal()) {
                    idleTicks = 0;
                    try { t->run(); } catch (...) {
                        try { t->exception(std::current_exception()); } catch (...) { }
                    }
                    delete t;
                    this->noteCompleted();
                    foundWork = true;
                    break;
                }
                std::this_thread::yield();
            }
            if (foundWork) continue;

            // 5. Temp fibers retire after a brief grace window of total idle.
            if (this->temp && ++idleTicks >= kTempIdleTicksBeforeRetire) {
                this->running.store(false, std::memory_order_release);
                // Store-load handshake with tryPushInbox: a producer either sees running == false and backs off, or
                // its enqueue is visible to the drain below (or it forwards the leftovers itself).
                std::atomic_thread_fence(std::memory_order_seq_cst);
                this->drainToPending();
                this->signalDone(this->fiberId);
                // Catch any stragglers a producer pushed into the inbox
                // after the first drain. Cheap when empty; otherwise they
                // are forwarded so a peer picks them up promptly rather
                // than leaving them until reap runs ~Fiber.
                this->drainToPending();
                if (this->owner) this->owner->releaseThreadSnapshotCache();
                tls_currentFiber = nullptr;
                return;
            }

            // 6. Park.
            this->park();
        }

        // Release this worker's cached snapshot before the thread exits.
        // If we leave the thread_local shared_ptr<Fiber> chain in place,
        // its destruction at thread_local teardown can trigger a ~Fiber
        // whose join() targets this very thread -- the classic self-join.
        if (this->owner) this->owner->releaseThreadSnapshotCache();
        tls_currentFiber = nullptr;
    }

    void Fiber::park() {
        // Snapshot parkSignal BEFORE marking ourselves parked. Any
        // subsequent poke() bumps it, so atomic::wait observes the
        // mismatch and returns immediately.
        const std::uint32_t expected =
            this->parkSignal.load(std::memory_order_acquire);

        // Mark ourselves parked AND bump the global parked-worker
        // counter so producers can fast-skip the snapshot walk in
        // wakeOneParked when nobody is asleep.
        //
        // Drop any wake claim left over from the previous sleep BEFORE publishing parked: a waker can only claim a
        // fiber it sees parked, so it can never find a stale claim on a freshly parked fiber (clearing it after waking
        // instead could erase a claim a waker took for the next sleep, or leave one that suppresses it).
        this->wakeClaimed.store(false, std::memory_order_release);
        this->parked.store(true, std::memory_order_release);
        if (this->owner) this->owner->noteParked(+1);
        // Publish "parked" before looking for work: a producer that enqueued before this point and then read
        // parked == false must be seen by the re-check below (store-load needs a full fence).
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Re-check sources under the parked=true post-condition. This
        // closes the race where a producer enqueued + poked between our
        // initial source check and the parked.store -- the producer's
        // poke incremented parkSignal, so the futex wait below returns
        // immediately.
        auto haveWork = [this] {
            if (!this->running.load(std::memory_order_acquire)) return true;
            if (!this->inbox->empty()) return true;
            if (!this->deque->empty()) return true;
            if (this->anyPendingTasks()) return true;
            // Work pushed into a peer's deque/inbox is announced only by a wake aimed at some parked worker; re-check
            // here so a push that raced with our parked.store is not slept through.
            if (this->anyPeerWork && this->anyPeerWork(this->fiberId)) return true;
            return false;
        };

        if (!haveWork()) {
            if (this->temp) {
                // A temp fiber must be able to notice sustained idleness and retire (see process()), but a futex wait
                // has no timeout: wait on the condition variable instead, which wakers interrupt via wakeParked().
                // Only temp fibers pay for the mutex; permanent workers keep the untimed futex wait below.
                std::unique_lock<std::mutex> lk(this->tempMu);
                this->tempCv.wait_for(lk, kParkWaitTimeout, [&] {
                    return this->parkSignal.load(std::memory_order_acquire) != expected;
                });
            } else {
                // Wait on the futex. Returns when parkSignal != expected (a
                // poke happened) or on a spurious wake.
                this->parkSignal.wait(expected, std::memory_order_acquire);
            }
        }

        this->parked.store(false, std::memory_order_release);
        if (this->owner) this->owner->noteParked(-1);
        // Pairs with the fence in poke()/wakeOneParked(): a waker that still read parked == true (and so counts us as
        // woken) published its work before that read, and this fence makes it visible to the source checks that follow.
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    void Fiber::noteCompleted() noexcept {
        this->completed.store(this->completed.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    TaskPtr Fiber::popLocal() {
        TaskPtr t = nullptr;
        if (this->deque->pop(t)) return t;
        return nullptr;
    }

    TaskPtr Fiber::trySteal() {
        if (!this->tryStealFromPeers) return nullptr;
        return this->tryStealFromPeers(this->fiberId);
    }

    void Fiber::drainInboxToPending() {
        TaskPtr stranded = nullptr;
        while (this->inbox->dequeue(stranded)) {
            if (this->pushPending) {
                this->pushPending(stranded);
            } else {
                try { stranded->run(); } catch (...) {
                    try { stranded->exception(std::current_exception()); } catch (...) { }
                }
                delete stranded;
            }
        }
    }

    void Fiber::drainToPending() {
        // Drain the inbox first. Producers may have pushed moments
        // before we decided to retire; forwarding the tasks to the
        // injection queue ensures they still run on a peer instead of
        // being silently freed by ~Fiber.
        this->drainInboxToPending();

        TaskPtr leftover = nullptr;
        while (this->deque->pop(leftover)) {
            if (this->pushPending) {
                this->pushPending(leftover);
            } else {
                // No pending handler: rather than silently leak, run the task
                // inline on the retiring thread.
                try { leftover->run(); } catch (...) {
                    try { leftover->exception(std::current_exception()); } catch (...) { }
                }
                delete leftover;
            }
        }
    }

    Workload Fiber::workload() const {
        const size_t currentSize = this->deque->size();
        if (currentSize == 0) return Workload::Idle;

        const size_t cap = this->deque->capacity();
        const float percent = (static_cast<float>(currentSize) / static_cast<float>(cap)) * 100.0f;

        if (percent <= static_cast<float>(Workload::Busy)) return Workload::Busy;
        if (percent < static_cast<float>(Workload::Burdened)) return Workload::Burdened;
        return Workload::Overburdened;
    }

    OsHandler Fiber::osHandler() {
        return this->thread.native_handle();
    }

    size_t Fiber::dequeCapacity() const noexcept {
        return this->deque->capacity();
    }
}
