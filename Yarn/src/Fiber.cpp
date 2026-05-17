//
// Created by Fabrizio Paino on 2022-01-16.
//

#include "Fiber.hpp"
#include "ITask.hpp"
#include "Yarn.hpp"

#include <chrono>
#include <exception>
#include <utility>

#ifdef _WIN32
    #include <windows.h>

/**
 * @brief Windows placeholder. A real implementation would query NUMA topology
 *        via GetNumaHighestNodeNumber / GetNumaProcessorNodeEx and call
 *        SetThreadAffinityMask. Kept as a no-op for now.
 */
static void SetNumaAffinity() {
}

#elif defined(__linux__)
    #include <numa.h>
    #include <sched.h>
    #include <pthread.h>
    #include <unistd.h>

/**
 * @brief Linux affinity: pin the current thread to the CPU it currently
 *        happens to be running on. Keeps the worker local to its initial
 *        NUMA node without requiring an explicit policy.
 */
static void SetNumaAffinity() {
    int n = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    if (n <= 0) return;
    int cpu = sched_getcpu();
    if (cpu < 0) cpu = 0;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu % n, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
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
static void SetNumaAffinity() {
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
static void SetNumaAffinity() {}
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
     * @brief Maximum time a worker stays parked on the CV without an explicit
     *        wake. Short enough that peer-stealable work nobody notified us
     *        about still gets noticed; long enough that idle workers don't
     *        wake repeatedly.
     */
    static constexpr auto kParkWaitTimeout = std::chrono::milliseconds(5);

    /**
     * @brief Capacity of each fiber's per-worker MPMC inbox. Must be a
     *        power of two. 32 entries is the sweet spot between
     *        "smooths over short bursts" and "occupies one cache page".
     */
    static constexpr std::size_t kInboxCapacity = 32;

    /**
     * @brief Max number of inbox entries drained per process-loop pass.
     *        Bounded so a single dispatch loop iteration can't hog the
     *        worker against its other obligations (deque, steal, park).
     */
    static constexpr int kInboxDrainPerLoop = 1;

    thread_local Fiber *tls_currentFiber = nullptr;

    Fiber::Fiber(FiberId id,
                 size_t dequeCapacity,
                 SignalDone signalDone,
                 GetFromPending getFromPending,
                 AnyPendingTasks anyPendingTasks,
                 PushPending pushPending,
                 TryStealFromPeers tryStealFromPeers,
                 WakeOneIdle wakeOneIdle,
                 bool temp)
        : temp(temp),
          signalDone(std::move(signalDone)),
          getFromPending(std::move(getFromPending)),
          anyPendingTasks(std::move(anyPendingTasks)),
          pushPending(std::move(pushPending)),
          tryStealFromPeers(std::move(tryStealFromPeers)),
          wakeOneIdle(std::move(wakeOneIdle)),
          fiberId(id),
          deque(std::make_unique<Deque>(dequeCapacity)),
          inbox(std::make_unique<MPMCQueue<TaskPtr>>(kInboxCapacity)) {
        this->thread = std::thread(&Fiber::process, this);
    }

    Fiber::~Fiber() {
        this->stop();
        if (this->thread.joinable()) {
            if (this->thread.get_id() == std::this_thread::get_id()) {
                // Defensive only: Yarn arranges destruction off-worker via the
                // graveyard / reaper, so this branch should not be reachable.
                this->thread.detach();
            } else {
                this->thread.join();
            }
        }

        // Reclaim anything still in the inbox. Producers may have
        // squeezed a push in between this fiber's drainToPending and
        // signalDone; forward those tasks to pending if we still have
        // the callback (drained tasks then route to a peer), else
        // delete to avoid a leak.
        if (this->inbox) {
            TaskPtr stranded = nullptr;
            while (this->inbox->dequeue(stranded)) {
                if (this->pushPending) {
                    this->pushPending(stranded);
                } else {
                    delete stranded;
                }
            }
        }

        // Any tasks still sitting in the local deque at hard shutdown leak
        // by design: Yarn::~Yarn drains the injection queue first, so the
        // only way to land here with non-empty local state is mid-shutdown
        // where running user code is unsafe.
    }

    bool Fiber::seed(TaskPtr task) {
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

    bool Fiber::tryPushInbox(TaskPtr task) noexcept {
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
        if (this->parked.load(std::memory_order_acquire)) {
            this->parkSignal.notify_one();
        }
        return true;
    }

    TaskPtr Fiber::stealOne() {
        TaskPtr t = nullptr;
        if (this->deque->steal(t)) return t;
        return nullptr;
    }

    void Fiber::stop() {
        this->running.store(false, std::memory_order_release);
        // Bump the park signal and wake every waiter. atomic::wait on the
        // futex checks the value at suspension time, so even if a fiber is
        // about to park it will observe the bumped value and skip the
        // wait. No mutex required.
        this->parkSignal.fetch_add(1, std::memory_order_release);
        this->parkSignal.notify_all();
    }

    void Fiber::poke() {
        // Fast skip when the worker isn't parked; spares a notify syscall
        // in the common busy-worker case.
        if (!this->parked.load(std::memory_order_acquire)) return;
        this->parkSignal.fetch_add(1, std::memory_order_release);
        this->parkSignal.notify_one();
    }

    void Fiber::process() {
        SetNumaAffinity();
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
                    foundWork = true;
                    break;
                }
                std::this_thread::yield();
            }
            if (foundWork) continue;

            // 5. Temp fibers retire after a brief grace window of total idle.
            if (this->temp && ++idleTicks >= kTempIdleTicksBeforeRetire) {
                this->running.store(false, std::memory_order_release);
                this->drainToPending();
                this->signalDone(this->fiberId);
                // Catch any stragglers a producer may have pushed into
                // the inbox between drainToPending() above and the
                // snapshot rebuild inside signalDone(). The check is
                // cheap when empty; when non-empty we forward to
                // pending so a peer picks the task up promptly rather
                // than leaving it stuck until reap runs ~Fiber.
                this->drainToPending();
                Yarn::instance()->releaseThreadSnapshotCache();
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
        Yarn::instance()->releaseThreadSnapshotCache();
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
        this->parked.store(true, std::memory_order_release);
        Yarn::instance()->noteParked(+1);

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
            return false;
        };

        if (!haveWork()) {
            // Wait on the futex. Returns when parkSignal != expected (a
            // poke happened) or on a spurious wake.
            this->parkSignal.wait(expected, std::memory_order_acquire);
        }

        this->parked.store(false, std::memory_order_release);
        Yarn::instance()->noteParked(-1);
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

    void Fiber::drainToPending() {
        // Drain the inbox first. Producers may have pushed moments
        // before we decided to retire; forwarding the tasks to the
        // injection queue ensures they still run on a peer instead of
        // being silently freed by ~Fiber.
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
