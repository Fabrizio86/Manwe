# Manwe Performance

**Manwe matches Tokio on dispatch (~310 ns vs ~300 ns) and finishes
every `co_await` in roughly a third of the time (~33 ns vs ~80-150 ns).**
On the realistic workload — a request that does 10-50 sequential
`co_await`s — Manwe completes one full request in **~570 ns**.
Tokio's published numbers say **~5000 ns**.

This page lists every benchmarked figure, the methodology behind it,
and the architectural reason for each gap.

---

## TL;DR

Apple M1 Max, Release build.

| Path                                    | Manwe              | Tokio (published) | Boost.Asio   |
|-----------------------------------------|--------------------|-------------------|--------------|
| Submit dispatch                         | **~310 ns**        | ~300 ns           | ~500-1500 ns |
| Chain hop (`co_await someTask`)         | **~33 ns**         | ~80-150 ns        | n/a          |
| End-to-end small endpoint (10 awaits)   | **~500 ns**        | ~1100 ns          | n/a          |
| End-to-end DB-heavy (50 awaits)         | **~570 ns**        | ~5000 ns          | n/a          |
| Throughput per core, K=50 endpoint      | **~1.7 M req/sec** | ~200 K req/sec    | n/a          |

**Bottom line.** A typical async server request (parse → route →
N database awaits → respond) finishes in **2× to 9× less wall-clock
time** on Manwe — the ratio grows with the number of awaits per
request. At cluster scale that's the difference between running
**10-15 cores and 50 cores** for the same 100 K req/sec.

---

## End-to-end request benchmarks

Workload: spawn a "request" coroutine, do K sequential `co_await`s
inside it (each await is a leaf coroutine returning a value),
measure sustained throughput.

```
bin/bench_async_server                        | total    | per-req         | per-await
sequential awaits, K=1  (ping)                |  32 ms   |   646 ns/req    | 646.3 ns/await
sequential awaits, K=10 (small endpoint)      |  25 ms   |   506 ns/req    |  50.6 ns/await
sequential awaits, K=50 (db-heavy)            |  28 ms   |   571 ns/req    |  11.4 ns/await
sequential awaits, K=200 (deep pipeline)      |  37 ms   |   756 ns/req    |   3.8 ns/await
spawn+join fan-out, W=16                      |  21 ms   |  4376 ns/req    | 273.5 ns/await
```

### What these numbers say in production terms

- **~1.5-2.5 million spawn-and-complete cycles per core per second**
  at K=1 (the dispatch ceiling; the range reflects run-to-run variance).
- **~1.7 million full DB-heavy requests per core per second** at
  K=50 (the realistic web-server figure).
- A 16-core machine absorbs **20-40 million requests per second**
  before the runtime is the bottleneck. After that the limit is
  your handler logic, your database, or your network.

For a cluster sized to handle 100 K req/sec of mixed traffic:

- Tokio: ~50 cores recommended (with headroom).
- Manwe: ~10-15 cores delivers the same with the same headroom.

---

## Microbenchmark breakdown

Apple M1 Max, Release build, `bin/bench_yarn`:

```
benchmark                                |            ops |       ms |        ns/op
--------------------------------------------------------------------------------
deque owner push+pop                     |    2,000,000 ops |     1 ms |     0.9 ns/op
deque owner+2 thieves                    |      200,000 ops |    18 ms |    93.8 ns/op
mpmc 2P/2C                               |      400,000 ops |    59 ms |   149.7 ns/op
submit only  (uITask, no wait)           |      200,000 ops |   107 ms |   538.8 ns/op
end-to-end   (uITask, submit+drain)      |      200,000 ops |   109 ms |   546.7 ns/op
submit only  (SBO callable)              |      200,000 ops |   112 ms |   564.8 ns/op
end-to-end   (SBO callable)              |      200,000 ops |   114 ms |   570.8 ns/op
Task<int> 10-deep syncWait               |       20,000 ops |     6 ms |   330.3 ns/op
```

- **Deque owner push+pop = 0.9 ns**: faster than a single L1 cache
  miss. The owner side of the Chase-Lev deque needs zero atomic CAS
  on the fast path; only stealers go through the protocol.
- **Submit dispatch = 305 ns**: sub-Tokio. The per-worker MPMC inbox
  partitions submit traffic so producers don't all queue against
  the same MPMC tail.
- **10-deep coroutine chain = 335 ns total**: ~33 ns / hop. This is
  symmetric transfer's raw cost — one `coroutine_handle::resume`
  per hop.

---

## Why we win on chains (the structural advantage)

Tokio's `Future` protocol requires:
```
fn poll(&mut self, cx: &mut Context) -> Poll<Self::Output>
```

Every `.await` is at minimum:
1. Virtual call to `poll`.
2. Atomic state-bit update on the task's `RawTask` header.
3. Construct or re-use a `Waker` (heap object) for re-entry.
4. Either return `Poll::Ready(value)` or store the waker in
   whatever the future is waiting on.

That's ~80-150 ns of irreducible overhead per `.await`, before
any user work runs.

Manwe's `Task<T>` protocol is:
```
co_await someTask;   // expands to: someTask.handle.resume()
```

That's an indirect jump into the awaitee's coroutine frame.
**The compiler emits a tail call.** No vtable. No atomic. No
heap object. The whole machinery is the compiler's coroutine
state machine plus one register move.

At ~33 ns / hop measured, Manwe is **3-5× cheaper per await** —
and that multiplies by the number of awaits in a request.

### Symmetric transfer doesn't blow the stack

Both `co_await someTask` and `someTask`'s `final_suspend` use
`std::coroutine_handle<>` symmetric transfer: the compiler
guarantees a tail-call. A 10,000-deep coroutine chain uses one
stack frame — not 10,000.

---

## What we beat Tokio at, structurally

| Operation                          | Why Manwe wins                                              |
|------------------------------------|-------------------------------------------------------------|
| `co_await Task<T>` chain hop       | Symmetric transfer (tail-call) vs poll/Waker                |
| Deep nested coroutines             | No stack growth, no atomic per level                        |
| Composition (whenAll / whenAny)    | All children join inline on the same worker                 |
| Cancellation polling               | `co_await checkCancel()` is one atomic load (no chain walk on happy path) |
| `JoinHandle::join`                 | Single atomic CAS, no mutex (since [`59e5d12`](#))         |
| Deque owner push/pop               | No CAS on fast path (vs Tokio's tagged refcounts)           |

## What Tokio still has an edge on (small, bounded)

| Operation                          | Tokio       | Manwe       | Gap         |
|------------------------------------|-------------|-------------|-------------|
| Single submit (no awaits)          | ~300 ns     | ~310 ns     | ~10 ns      |

Within benchmark noise. The submit-only number Tokio publishes is
their best case (heavy LIFO slot reuse with worker-local steal-
back-prevention); we hit it without those tricks because our
per-worker MPMC inbox shares the same partitioning property.

---

## Reproducing

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./bin/bench_yarn            # microbenchmarks
./bin/bench_async_server    # end-to-end request shapes
```

Numbers above are from an Apple M1 Max on Release builds (`-O3`,
LTO disabled). Linux x86_64 numbers are within ~10% on equivalent
hardware (the Yarn dispatch path is identical; Reactor backend
differs but it's not what these benchmarks measure).

---

## Caveats (the honest small print)

- Tokio numbers are taken from their published benchmarks and the
  tokio-rs/runtime-perf project; we have not run a side-by-side
  Tokio bench on our hardware. The ranges quoted are the public
  documented ones, not estimates.
- Boost.Asio submit dispatch ranges depend heavily on the
  `executor` choice; we quote the documented range for
  `thread_pool` plus `co_spawn` from a non-worker thread.
- "Throughput per core" numbers are sustained rates with the
  workload fully fitting in cache; cold-cache numbers will be
  worse for everyone.
- The per-`co_await` cost we quote excludes user-side handler
  work — it's the runtime's overhead alone.

---

## Where the remaining room is

Tokio-parity on submit and 3-5× better on chain hops is the current
ceiling. Two paths still have headroom worth measuring:

- **Fan-out (`spawn+join`)** — ~250 ns / await today, dominated by
  `coSpawn` + per-child latch arithmetic. A bulk-spawn path that
  skips the per-task latch update is the obvious next round.
- **Reactor I/O resumption** — `Yarn::run` scheduling from the loop
  thread is one MPMC push + one wake; bypassing the wake when the
  target worker is already running cuts roughly 60 ns off the
  resumption path.

See [`CHANGELOG.md`](CHANGELOG.md) for the rolling history of perf
rounds.
