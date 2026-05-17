# How Manwe Got Fast

A plain-English tour of the design decisions behind Manwe's
performance numbers. No coroutine jargon, no atomic memory-order
talk, no assembly listings. If you've ever wondered "what actually
makes one async runtime faster than another in production?", this
is for you.

The numbers themselves are in [`PERFORMANCE.md`](PERFORMANCE.md).
This is the *why*.

---

## The one-sentence summary

Manwe is fast because it does **less work, in more direct paths,
with fewer coordination points** than the runtimes it competes
with. Every fast system is fast for the same reason. The interesting
question is always: which corners did you cut, and which corners
did you refuse to cut?

---

## The core insight: optimise for what happens most often

Every async runtime has two main paths through it:

- **Spawning** new work (e.g. a request hits your server,
  you spin up a coroutine to handle it).
- **Awaiting** intermediate results inside that work (e.g.
  the handler waits on the database, then the cache, then
  the user service, then formats a response).

In a real production server, every single request is **one spawn**
followed by **10-50 awaits**. The await path is hit 10-50× more
often than the spawn path.

Tokio — the most-deployed async runtime in production, written in
Rust — optimised the spawn path. Its spawn-dispatch number is
genuinely excellent: around 300 nanoseconds. The trade-off is that
the await path is comparatively expensive, around 80-150 nanoseconds
per await.

Manwe made the opposite trade-off. **We stay within noise of Tokio on
spawn (310 ns vs 300 ns) and spend the rest of the budget on the
await path, which lands at roughly 33 ns — three to five times
cheaper per await.**

When a request does 20 awaits, the Manwe handler finishes about 1.5
microseconds ahead of the Tokio handler. Multiply that across every
request in a 100,000-req/sec service and you save **150 milliseconds
of CPU time per wall-clock second** — the equivalent of 1.5 cores.
Across a fleet, that's tens of servers.

---

## How awaits became cheap: doing nothing, in a direct line

When your code says "await this database query", something has to
happen under the hood to suspend you, run other work, and wake you
up when the query is done.

Most async runtimes handle this by **polling**: every step in the
chain implements a "poll me to see if I'm ready" function. When
you await something, the runtime:

1. Calls poll.
2. If not ready, parks a "I want to be told when this is ready"
   callback object on whatever you're waiting for.
3. Eventually the thing finishes, the callback fires, you're
   re-scheduled.
4. The runtime calls poll again. If ready, returns the value.

That's a lot of bookkeeping per await, and it has to happen even
for awaits that **complete immediately** (e.g. a value already in
a cache). Most awaits in a real server are immediate — the value
is already there, no actual suspension needed.

Manwe uses a different mechanism the language committee designed
into C++20 itself: **symmetric transfer**. When you await
something, the compiler emits a direct jump into that something's
code. When that code finishes, it emits a direct jump back to you.
No polling. No callback objects. No state-machine bookkeeping. The
CPU executes the equivalent of a function call and a function
return.

This is why Manwe's await costs ~33 nanoseconds while Tokio's costs
~80-150 nanoseconds. Tokio pays for flexibility — arbitrary futures
composed at runtime — that Manwe doesn't need, because the C++
compiler can lower the whole structure into direct branches at
compile time.

**Doing less work is faster than doing the same work more cleverly.**

---

## How dispatch became cheap: don't route through one place

When you submit a piece of work to an async runtime from outside
(say, your network reactor woke up and now needs to schedule the
"handle this packet" handler), most runtimes push it into a
central queue and let worker threads pull from it.

Central queues become bottlenecks under load. Every producer
contending for the same queue position means every producer waits
on every other producer. This is the kind of bottleneck that
shows up only at peak load and then becomes the entire problem.

Manwe gives each worker thread **its own small inbox**. The
runtime round-robins submissions across worker inboxes before it
ever touches the central queue. A typical submit doesn't see any
of the other producers because each one is pushing to a different
worker's inbox.

Under burst load with thousands of submissions per millisecond,
this turns a contended pile-up into a parallelised stream. The
central queue is still there as a fallback when individual inboxes
fill — but on the happy path, it's not in the way.

Same principle, different layer.

---

## How parking became cheap: tell the OS the truth

When a worker thread has nothing to do, it has to wait somewhere
for new work. The traditional way is **mutex + condition variable**
— a textbook pattern from the 1990s. Every wake-up requires
acquiring the mutex, signalling the condition variable, releasing
the mutex. Roughly 100 nanoseconds per wake on modern hardware,
and the mutex acquire serialises waking parties.

Manwe uses the OS-level "futex" primitive directly (via C++20's
`std::atomic::wait` / `notify_one`). The protocol is: a worker
publishes "I am about to park, expecting value X". The OS checks
the value — if it's still X, the worker sleeps; if it changed
(because a producer just bumped it), the worker doesn't sleep.

This collapses the classical "missed wake-up" race condition into
a single atomic instruction plus an OS syscall. No mutex. No
condition variable. Wakes are sub-50 ns.

The result: when a producer pushes work and the target worker is
already running, the kernel doesn't get involved at all. We just
bump the atomic counter and the worker picks up the work on its
next loop iteration. The futex syscall fires only when a worker is
actually parked.

---

## How allocation became cheap: don't allocate

Every coroutine in C++ has a frame — a small heap object holding
its local state. Most runtimes call `malloc` for every coroutine,
which is a global system call with all the contention that
implies.

Manwe routes coroutine allocations through a per-process pool with
a thread-local cache. Most allocations land in the thread-local
cache and don't even touch shared state. Cross-thread spill is
batched.

The result: under typical workloads, `malloc` doesn't appear in
profiles. We allocate without allocating.

---

## How cancellation became cheap: don't poll unless asked

Tokio bakes cancellation into every poll: each `.await` implicitly
checks "should I cancel?". That's a small atomic load per await,
multiplied by however many awaits a real workload performs. It
adds up.

Manwe makes cancellation an explicit operation:
`co_await checkCancel()`. The user inserts the check where they
want it (typically at the top of each loop iteration, or before
expensive work). One atomic load happens **only** at those points.

Most awaits don't pay for cancellation. The ones that need it,
do. **Pay only for what you use.**

The pattern walks one pointer chain to propagate cancellation from
parent to child tasks — so cancelling a top-level request cancels
its sub-tasks automatically — but the walk happens at the check
site, not on every await.

---

## How joining became cheap: one atomic, no mutex

When you spawn a task and later want its result, you need a
synchronisation primitive: the spawned task publishes its result,
the joiner waits. Most runtimes use a mutex + a condition variable
pair for this.

Manwe encodes the entire state in a single atomic pointer-sized
word:
- The word starts at 0 (no result yet, nobody waiting).
- The joiner stores its "I am waiting" handle into the word.
- The spawned task, when done, atomically swaps the word for a
  "result is ready" sentinel and wakes whoever was waiting.

One atomic operation on each side. No mutex. No condition variable.
No missed wakeups. No re-checking loops. The whole thing fits in
8 bytes of state per spawned task.

---

## How memory layout became cheap: respect the cache

Modern CPUs are not slow at computing — they are slow at
**moving data between cache levels**. A correctly predicted L1
hit costs ~1 nanosecond. An L2 hit costs ~10 nanoseconds. An L3
hit costs ~30 nanoseconds. A main-memory miss costs ~200
nanoseconds.

The fast paths in Manwe — worker dispatch, deque push/pop, atomic
counters — all fit in L1 or hit lines that are kept hot by being
touched frequently. Hot fields shared between threads are padded
to cache-line size (64 bytes on ARM, 128 on some x86) to avoid
**false sharing** — the situation where two unrelated atomics
sit on the same cache line, so every write to one invalidates the
other across all cores.

Cache-line padding is invisible in the profile. It shows up in
the bench number.

---

## What we refused to cut

We could have gone even faster by giving up on some things. We
chose not to:

- **Exceptions still propagate through `co_await` chains.** A
  task that throws is delivered to its joiner. Many high-perf
  runtimes ban exceptions entirely; we keep them because real
  C++ code uses them.
- **`std::shared_ptr` remains the public API for owned tasks.**
  Internally, the hot path uses raw pointers (we wrap once at
  the boundary), but user code can stay idiomatic.
- **Standard C++ all the way through.** No assembly, no intrinsics
  beyond what `std::atomic` provides, no compiler-specific tricks.
  This means Manwe compiles cleanly on AppleClang, gcc, MSVC,
  and works on Apple Silicon, x86_64, and arm64 Linux.
- **No external dependencies.** No Boost. No Folly. No third-party
  channel libraries. We use the C++ standard library and what the
  OS gives us.

A runtime that's only fast on one compiler, one OS, and one
processor is not useful. We are fast everywhere.

---

## The summary, again

We are not fast because of one trick. We are fast because of
many small decisions, made consistently in one direction:

> Do less. Do it in a more direct path. Don't coordinate
> when you don't have to. Pay attention to memory layout.
> Don't optimise the path that runs once when the path next
> to it runs fifty times.

If you take one thing away: **the fastest operation is the one
you don't perform**. Every nanosecond Manwe saves came from
removing something, not adding something. The benchmarks aren't
proof of optimisation. They're proof of restraint.

---

For the numbers: [`PERFORMANCE.md`](PERFORMANCE.md).
For the API: per-subsystem docs in [`docs/`](docs/).
For the running history of perf rounds: [`CHANGELOG.md`](CHANGELOG.md).
