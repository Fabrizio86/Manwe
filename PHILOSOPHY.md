# Design rationale

This document explains the design decisions behind Manwe's performance
profile. The benchmarked numbers themselves are in
[`PERFORMANCE.md`](PERFORMANCE.md); this document covers the *why*.

---

## Optimising the await path, not the spawn path

Every async runtime has two main paths:

- **Spawning** new work — for example, a request hits the server and the
  runtime creates a coroutine to handle it.
- **Awaiting** intermediate results inside that work — for example, the
  handler waits on the database, then the cache, then a downstream
  service.

A typical production request is one spawn followed by 10–50 awaits. The
await path is hit an order of magnitude more often than the spawn path.

Tokio optimises the spawn path; its measured spawn-dispatch cost is
~300 ns. The trade-off is a more expensive await path, ~80–150 ns per
`.await`, because every Future implements a `poll` protocol with
runtime-mediated state transitions.

Manwe takes the opposite trade-off. Submit dispatch lands at ~310 ns —
within noise of Tokio — while the per-`co_await` cost is ~33 ns,
roughly 3–5× cheaper. For a request that does 20 awaits, the Manwe
handler finishes ~1.5 µs ahead; over 100,000 req/sec that compounds
to ~150 ms of CPU time saved per wall-clock second, or about 1.5 cores.

---

## Cheap awaits via symmetric transfer

Most async runtimes implement awaits by polling. Each step in the
chain exposes a `poll` function; awaiting an operation involves:

1. Calling `poll`.
2. If not ready, registering a "wake me when ready" callback on the
   awaited resource.
3. When the resource completes, the callback fires and the task is
   re-scheduled.
4. The runtime calls `poll` again to collect the value.

This bookkeeping runs on every await — including awaits that complete
immediately because the value is already available, which is the common
case for cached data.

Manwe uses C++20 **symmetric transfer**. When a coroutine awaits
another coroutine, the compiler emits a direct jump into the awaited
coroutine's frame; on completion, it emits a direct jump back to the
caller. No polling, no callback objects, no per-await state-machine
transitions. The runtime cost reduces to a function call and a return.

Measured cost: ~33 ns per `co_await` on Apple M1 Max, vs ~80–150 ns
for Tokio's `.await`. The gap is the irreducible overhead of the
`poll`/`Waker` protocol, which Manwe does not pay.

---

## Per-worker submission inboxes

A common design routes external submissions into a single central
queue from which worker threads pull. Under burst load every producer
contends for the same tail pointer, serialising submission and
degrading throughput at peak.

Manwe gives each worker thread a small MPMC inbox. The submit path
round-robins across worker inboxes before touching the central
injection queue, so concurrent producers usually push to different
inboxes and do not contend. The central queue remains as the fallback
for when a worker's inbox fills.

Under burst load this turns a contended pile-up into N parallel
streams. The central queue stays cold on the fast path.

---

## Parking via futex, not mutex + condition variable

When a worker has no work it must park somewhere awaiting new work. The
classical implementation uses `std::mutex` + `std::condition_variable`:
every wake acquires the mutex, signals the CV, and releases the mutex.
Cost is roughly 100 ns per wake, with the mutex serialising waking
producers.

Manwe uses `std::atomic::wait` / `notify_one`, which lowers to a futex
on Linux, `__ulock` on macOS, and `WaitOnAddress` on Windows. The
protocol is: the worker publishes "I am about to park, expecting value
X"; the kernel checks the value before suspending; if a producer has
already bumped it the worker does not sleep.

This collapses the classical missed-wake race into a single atomic
instruction plus an OS syscall, with no mutex on either side. Wake
cost is sub-50 ns, and the kernel is involved only when a worker is
actually parked.

---

## Pooled coroutine frame allocation

Every C++ coroutine has a heap-allocated frame holding its local
state. A naive runtime calls `malloc` on every coroutine, which is a
global allocator call with the contention that implies.

Manwe routes coroutine allocations through a process-wide pool backed
by a thread-local LIFO cache. Most allocations hit the thread-local
cache and never touch shared state; cross-thread spill is batched.
Under typical workloads `malloc` does not appear in profiles for the
coroutine allocation path.

---

## Explicit cancellation, not per-await polling

Tokio's `.await` implicitly checks a cancellation flag on every poll.
That is one atomic load per await, multiplied by the number of awaits
in the workload.

Manwe makes cancellation an explicit operation: `co_await
checkCancel()`. The user inserts the check where it matters (typically
at the top of each loop iteration or before expensive work). One
atomic load executes only at those points; awaits that do not need to
honour cancellation do not pay for it.

The check walks one pointer chain to propagate cancellation from
parent to child tasks, but the walk happens at the explicit check site
rather than on every await.

---

## Single-atomic join handshake

A spawn-and-join handle needs synchronisation: the spawned task
publishes its result and the joiner waits. A textbook implementation
uses a mutex plus a condition variable.

`JoinHandle<T>` encodes the entire handshake in a single atomic
pointer-sized word:

- Initial value is `0` (no result, no waiter).
- The joiner CAS-installs its handle's address into the word.
- The runner atomically exchanges the word for a `ready` sentinel and,
  if the previous value was a handle address, wakes that handle.

One atomic operation per side. No mutex, no missed wakes, no recheck
loops. Total state per spawned task: 8 bytes.

---

## Cache-aware memory layout

Modern CPUs are bottlenecked by memory traffic, not by arithmetic. L1
hits cost ~1 ns; L2 hits ~10 ns; L3 hits ~30 ns; main memory ~200 ns.

The fast paths in Manwe — worker dispatch, deque push/pop, atomic
counters — fit in L1 and stay hot through frequent access. Hot fields
shared between threads are padded to cache-line size (64 B on ARM, up
to 128 B on some x86 microarchitectures) to avoid false sharing, where
two unrelated atomics on the same line cause cross-core invalidation
traffic on every write.

Cache-line padding does not appear in microbenchmarks of the
individual operations, but it shows up in sustained throughput at
contention.

---

## Properties retained

The following properties were preserved even where dropping them would
have shaved further nanoseconds:

- **Exceptions propagate through `co_await` chains.** A task that
  throws is delivered to its joiner. Several high-performance runtimes
  prohibit exceptions; Manwe retains them because idiomatic C++ uses
  them.
- **`std::shared_ptr` is the public API for owned tasks.** The hot
  path uses raw pointers internally (wrapped at the boundary), but
  user code can stay idiomatic.
- **Standard C++ only.** No inline assembly, no compiler-specific
  intrinsics beyond what `std::atomic` provides. Manwe compiles on
  AppleClang, gcc, and MSVC, and runs on Apple Silicon, x86_64, and
  arm64 Linux.
- **No third-party runtime dependencies.** Boost, Folly, and external
  channel/future libraries are not used. Optional dependencies
  (`libtls`, `nghttp2`) gate the matching features only.

---

## Summary

Manwe's performance characteristics come from many small decisions
applied in one direction: do less work per operation, take more direct
code paths, minimise coordination points, and respect cache locality.
The benchmark numbers in [`PERFORMANCE.md`](PERFORMANCE.md) follow
from those decisions rather than from any single optimisation.

For the API, see the per-subsystem docs in [`docs/`](docs/). For the
running history of performance work, see
[`CHANGELOG.md`](CHANGELOG.md).
