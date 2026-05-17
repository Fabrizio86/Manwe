# Yarn — Work-stealing thread pool

Yarn is the execution substrate that every other piece of Manwe sits on top
of. It is a single process-wide thread pool with per-worker Chase–Lev
work-stealing deques, a global lock-free injection queue for external
submissions, dynamic growth on backlog, and a lock-free snapshot of the
live worker set on the hot path.

## Headline numbers

Apple M1 Max, Release build, `bin/bench_yarn`:

| Operation                              | Manwe       | Tokio (published) |
|----------------------------------------|-------------|-------------------|
| Submit dispatch (`Yarn::run`)          | **~310 ns** | ~300 ns           |
| Chase-Lev deque owner push+pop         | **~1 ns**   | ~3-5 ns           |
| Deque + 2 thieves                      | ~50 ns      | ~50-80 ns         |
| MPMC 2P/2C                             | ~125 ns     | n/a               |

Submit dispatch sits inside Tokio's published best band. Combined with
the per-hop `Task<T>` cost of ~33 ns (vs Tokio's ~80-150 ns / `.await`),
this is what lets `bench_async_server` sustain **2.5-2.7 million
spawn-and-complete cycles per core per second** on realistic
await-heavy workloads. See the [README](../README.md#the-numbers) for
the full end-to-end table and [`PERFORMANCE.md`](../PERFORMANCE.md)
for methodology.

---

## Architecture

### Workers (`Fiber`)

Each pool worker is a `Fiber` — one OS thread bound to one Chase–Lev
work-stealing deque. The deque is owner-only at the bottom (push/pop in
LIFO order, no atomic CAS on the fast path) and multi-thief at the top
(steal in FIFO order via a single CAS). For a small element type (we hold
raw `ITask*`) the fast path of `push` / `pop` is a handful of relaxed
atomic ops plus one release fence.

The pool starts with `MinThreadCount` permanent workers
(≥ `hardware_concurrency()`) and grows up to `MaxThreadCount`
(`floor(min * 3.5)`) by spawning *temp* fibers when the injection queue
backlogs. Temp fibers retire when they observe four consecutive idle ticks
(~20 ms of no work).

### Injection queue

External submitters (any thread that is not currently a Yarn worker) push
into a single MPMC lock-free injection queue. Workers drain it in bursts
of 32 into their local deque whenever the local deque empties. The queue
is also the fallback for in-worker submissions when the local deque is
full.

### Lock-free fiber snapshot

The hot path on stealing and waking parked workers must read the live
worker set thousands of times per second. Walking the `fibers` vector under
a mutex turns out to be the bottleneck — every steal attempt serialises
on `cmu`. Yarn maintains a `std::shared_ptr<const Fibers>` *snapshot* that
is rebuilt whenever a worker is added or retired, and atomically swapped
into place. Stealers and the wake-one path acquire-load the snapshot,
iterate it lock-free, and never touch `cmu`. The mutex is only entered on
genuine pool-shape changes.

### Park / wake protocol

When a worker has nothing to do (no local work, no injection backlog, no
peer steals), it `park()`s on its per-fiber condition variable. The
`parked` flag is an atomic that producers check cheaply: if no worker is
parked, the producer doesn't acquire any worker's mutex.

When a producer notices an injected task with no parked worker AND the
injection backlog is over a threshold (64), it takes `cmu` and spawns a
temp fiber.

### Graveyard + reaper

When a temp fiber retires it cannot destroy itself — that would mean
joining its own thread. Instead it pushes its `shared_ptr<Fiber>` to a
graveyard. `Yarn::run` opportunistically reaps the graveyard at the start
of every call, joining the threads (already exited) on a non-worker thread.
The destructor of `Yarn` does a final reap.

### Ownership of tasks

Tasks travel through the executor as **raw owning** `ITask*`. Both the
public `Run(unique_ptr<ITask>)` and `Run(sITask)` entry points reduce to
that — `unique_ptr` releases, `sITask` is wrapped in an internal owning
adapter exactly once. The fast path never touches a shared_ptr reference
count.

### Workload bucketing

`Fiber::workload()` returns one of four bands based on local deque
occupancy: Idle / Busy / Burdened / Overburdened. The scheduler uses this
to keep load somewhat balanced, but in practice the work-stealing protocol
makes precise scheduling unnecessary — random-victim steals win.

---

## Using it

### Submit a task

`ITask` is the unit of work:

```cpp
class PrintTask : public YarnBall::ITask {
public:
    explicit PrintTask(std::string m) : msg(std::move(m)) {}
    void run() override { std::cout << msg << "\n"; }
    void exception(std::exception_ptr) override {}
private:
    std::string msg;
};

// Preferred: zero-refcount, ownership transferred.
std::unique_ptr<YarnBall::ITask> t =
    std::make_unique<PrintTask>("hello");
YarnBall::run(std::move(t));
```

The `sITask` overload is kept for cases where the caller wants to retain a
reference (e.g. waitables):

```cpp
auto w = std::make_shared<MyWaitable>();
YarnBall::post(w);   // sITask path
w->wait();           // observe completion from outside
```

### Wait for a task to finish

`Waitable` wraps a task with a built-in completion latch:

```cpp
class CalcWaitable : public YarnBall::Waitable {
public:
    void operation() override { result = expensive_thing(); }
    int result = 0;
};

auto w = std::make_shared<CalcWaitable>();
YarnBall::post(w);
w->wait();
if (w->hasFailed()) {
    std::cerr << w->errorMessage();
} else {
    std::cout << w->result;
}
```

### Custom scheduler

Swap the default `RandomScheduler` if you want different victim selection
in the steal path:

```cpp
auto mine = std::make_shared<MyScheduler>();
YarnBall::Yarn::instance()->switchScheduler(mine);
```

`IScheduler::ThreadIndex(maxValue)` returns an index in `[0, maxValue)`.
Returning a negative value tells callers to skip selection.

### Pool sizing

Read-only knobs (compile-time):

- `MinThreadCount` = `max(4, hardware_concurrency())`
- `MaxThreadCount` = `floor(MinThreadCount * 3.5)`
- Per-fiber deque capacity = `1024` (raw pointer slots)
- Injection queue capacity = `QueueSize::Huge` = `4096`

If you need to size differently, those are the values to tweak in
`Yarn/src/Yarn.cpp`.

---

## Invariants worth knowing

- **One worker, one deque.** The bottom of a Chase-Lev deque is touched
  only by its owner. Don't share fibers across threads.
- **Tasks own themselves.** Once `Run` accepts a task, the executor will
  `delete` it after `run()`. Don't free it yourself.
- **`cmu` is cold.** Workers never take `cmu` on the steal hot path; that's
  the whole point of the snapshot. If you find yourself adding a
  `lock_guard<mutex>(cmu)` in a worker path, you're undoing the
  optimisation.
- **Destruction is from the main thread.** `Yarn::~Yarn` runs at static
  teardown, joins all workers (never from inside one), and drains the
  graveyard.
