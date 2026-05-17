//
// Created by Fabrizio Paino on 2022-01-14.
//

#include "Yarn.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <random>
#include <stdexcept>
#include <utility>

#ifndef _WIN32
#include <csignal>
#endif

#include "RandomScheduler.hpp"
#include "SmallObjectPool.h"

namespace YarnBall {

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

    int Yarn::MinThreadCount = std::max<int>(
        kMinThreadsFloor,
        static_cast<int>(std::thread::hardware_concurrency()));

    int Yarn::MaxThreadCount = std::max<int>(
        Yarn::MinThreadCount,
        static_cast<int>(std::floor(Yarn::MinThreadCount * kOptimalMultiplier)));

    /**
     * @brief Thread-local PRNG for stealing victim selection. Per-worker
     *        state eliminates the shared rng mutex on the hot path.
     */
    static thread_local std::mt19937 tls_stealRng{std::random_device{}()};

    Yarn::Yarn()
        : injection(std::make_shared<InjectionQueue>(QueueSize::Huge)),
          fiberSnapshot(std::make_shared<const Fibers>()),
          scheduler(std::make_shared<RandomScheduler>()) {
#ifndef _WIN32
        // Suppress SIGPIPE so a peer disconnecting mid-write does not
        // terminate the whole process. The canonical setup for any
        // server runtime; without this, a single broken TCP / WebSocket
        // / pipe peer can kill us. Users who genuinely want SIGPIPE
        // can re-install their own handler after Yarn::instance().
        std::signal(SIGPIPE, SIG_IGN);
#endif
        std::lock_guard<std::mutex> lk(this->cmu);
        this->fibers.resize(Yarn::MaxThreadCount);
        for (int i = 0; i < Yarn::MinThreadCount; ++i) {
            this->initializeNewThreadLocked(static_cast<FiberId>(i), /*markAsTemp=*/false);
        }
        this->rebuildSnapshotLocked();
    }

    Yarn::~Yarn() {
        // Phase 1: stop all workers so anyone parked or mid-spin returns.
        {
            std::lock_guard<std::mutex> lk(this->cmu);
            for (auto &f : this->fibers) {
                if (f) f->stop();
            }
        }
        // Phase 2: dropping the sFibers triggers ~Fiber which joins. We are
        // never the worker thread here (destruction is driven by static
        // teardown on the main thread), so no self-join.
        {
            std::lock_guard<std::mutex> lk(this->cmu);
            this->fibers.clear();
            this->rebuildSnapshotLocked();
        }
        // Phase 3: collect any fibers that retired but were never reaped.
        this->reap();

        // Phase 4: anything stuck in the injection queue at shutdown is lost
        // on purpose — the InjectionQueue destructor will free the raw
        // pointers we leaked. Patch that:
        TaskPtr t = nullptr;
        while (this->injection && this->injection->dequeue(t)) {
            delete t;
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

    void Yarn::run(uITask task) {
        if (!task) return;
        this->reap();
        this->dispatch(task.release());
    }

    void Yarn::run(sITask task) {
        if (!task) return;
        this->reap();
        // One small allocation per submission carries the shared_ptr to the
        // worker. The pool fast path itself never sees a shared_ptr.
        this->dispatch(new SharedOwnerAdapter(std::move(task)));
    }

    void Yarn::dispatch(TaskPtr task) {
        // In-worker fast path: push directly to our own local deque. No lock,
        // no atomic CAS, no shared_ptr touch.
        if (Fiber *self = tls_currentFiber) {
            if (self->seed(task)) return;
            // Local deque full -- fall through to express / injection.
        }

        // Out-of-worker submit: try to publish into a round-robin-chosen
        // worker's bounded MPMC inbox. Partitions submit traffic across
        // workers so the central injection queue's tail counter is not
        // contended with peer dequeues. Workers drain their inbox first
        // in their process loop.
        auto snap = this->currentSnapshot();
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
                if (target && target->tryPushInbox(task)) return;
            }
        }

        this->enqueueInjection(task);
    }

    void Yarn::enqueueInjection(TaskPtr task) {
        if (this->injection->enqueue(task)) {
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
            return;
        }

        // Injection queue is full. Try to grow first; if that fails too, run
        // inline on the caller. Inline execution under pool exhaustion is the
        // standard back-pressure fallback (.NET, Folly, Tokio all do this).
        {
            std::lock_guard<std::mutex> lk(this->cmu);
            int newId = this->maybeGrowLocked();
            if (newId >= 0) {
                if (this->fibers[newId] && this->fibers[newId]->seed(task)) return;
            }
        }

        try { task->run(); } catch (...) {
            try { task->exception(std::current_exception()); } catch (...) { }
        }
        delete task;
    }

    int Yarn::maybeGrowLocked() {
        if (this->maxLimitReachedLocked()) return -1;
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
            if (f && f->isParked()) {
                f->poke();
                return true;
            }
        }
        return false;
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
                    this->injection->enqueue(t);
                    break;
                }
            }
        };

        AnyPendingTasks anyPendingTasks = [this]() {
            return !this->injection->empty();
        };

        PushPending pushPending = [this](TaskPtr t) {
            if (!this->injection->enqueue(t)) {
                // Backlog full at the worst time. Run inline rather than leak.
                try { t->run(); } catch (...) {
                    try { t->exception(std::current_exception()); } catch (...) { }
                }
                delete t;
            }
        };

        TryStealFromPeers tryStealFromPeers = [this](FiberId fid) {
            return this->stealFromPeers(fid);
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
            markAsTemp);
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
