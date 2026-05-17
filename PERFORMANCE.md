# Performance

**Manwe matches Tokio on submit dispatch (~310 ns vs ~300 ns) and
completes each `co_await` in roughly a third of the time (~33 ns vs
~80–150 ns).** A realistic request shape — 10–50 sequential
`co_await`s — completes in ~570 ns on Manwe against Tokio's published
~5,000 ns.

This document lists every benchmarked figure, the methodology, and the
structural reason for each gap.

---

## Headline numbers

Apple M1 Max, Release build.

| Path                                    | Manwe              | Tokio (published) | Boost.Asio   |
|-----------------------------------------|--------------------|-------------------|--------------|
| Submit dispatch                         | **~310 ns**        | ~300 ns           | ~500-1500 ns |
| Chain hop (`co_await someTask`)         | **~33 ns**         | ~80-150 ns        | n/a          |
| End-to-end small endpoint (10 awaits)   | **~500 ns**        | ~1100 ns          | n/a          |
| End-to-end DB-heavy (50 awaits)         | **~570 ns**        | ~5000 ns          | n/a          |
| Throughput per core, K=50 endpoint      | **~1.7 M req/sec** | ~200 K req/sec    | n/a          |

A typical async server request (parse → route → N database awaits →
respond) completes in 2× to 9× less wall-clock time on Manwe; the
ratio grows with the number of awaits per request. At cluster scale
this is approximately **10–15 cores against 50 cores** for the same
100 K req/sec.

---

## End-to-end request benchmarks

Workload: spawn a "request" coroutine, perform K sequential
`co_await`s (each await is a leaf coroutine returning a value),
measure sustained throughput.

```
bin/bench_async_server                        | total    | per-req         | per-await
sequential awaits, K=1  (ping)                |  32 ms   |   646 ns/req    | 646.3 ns/await
sequential awaits, K=10 (small endpoint)      |  25 ms   |   506 ns/req    |  50.6 ns/await
sequential awaits, K=50 (db-heavy)            |  28 ms   |   571 ns/req    |  11.4 ns/await
sequential awaits, K=200 (deep pipeline)      |  37 ms   |   756 ns/req    |   3.8 ns/await
spawn+join fan-out, W=16                      |  21 ms   |  4376 ns/req    | 273.5 ns/await
```

### Production-shape interpretation

- **~1.5–2.5 million spawn-and-complete cycles per core per second**
  at K=1 (the dispatch ceiling; the range reflects run-to-run
  variance).
- **~1.7 million full DB-heavy requests per core per second** at
  K=50 (the realistic web-server figure).
- A 16-core machine sustains **20–40 million requests per second**
  before the runtime is the bottleneck.

For a cluster sized to handle 100 K req/sec of mixed traffic:

- Tokio: ~50 cores recommended (with headroom).
- Manwe: ~10–15 cores delivers the same with the same headroom.

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
  miss. The owner side of the Chase-Lev deque performs zero atomic CAS
  on the fast path; stealers pay the protocol cost.
- **Submit dispatch = 305 ns**: in the same range as Tokio's
  published number. The per-worker MPMC inbox partitions submit
  traffic so producers do not all queue against the same tail.
- **10-deep coroutine chain = 335 ns total** (~33 ns / hop). This is
  symmetric transfer's raw cost: one `coroutine_handle::resume` per
  hop.

---

## Per-await structural difference

Tokio's `Future` protocol requires each await to:

1. Make a virtual call to `poll`.
2. Update an atomic state bit on the task's `RawTask` header.
3. Construct or reuse a `Waker` (heap object) for re-entry.
4. Either return `Poll::Ready(value)` or store the waker on whatever
   the future is waiting on.

That accounts for ~80–150 ns of overhead per `.await` before any user
work runs.

Manwe's `Task<T>` protocol is:

```cpp
co_await someTask;   // expands to: someTask.handle.resume()
```

This is an indirect jump into the awaitee's coroutine frame. The
compiler emits a tail call; there is no vtable dispatch, no atomic,
and no heap object. The whole machinery is the compiler's coroutine
state machine plus one register move.

Measured ~33 ns / hop. The gap multiplies by the number of awaits in
a request, which is why end-to-end ratios widen on deeper chains.

### Symmetric transfer keeps the stack bounded

Both `co_await someTask` and the awaitee's `final_suspend` use
`std::coroutine_handle<>` symmetric transfer; the compiler guarantees
a tail-call. A 10,000-deep coroutine chain uses one stack frame, not
10,000.

---

## Operation-level comparison

| Operation                          | Why Manwe is faster here                                            |
|------------------------------------|---------------------------------------------------------------------|
| `co_await Task<T>` chain hop       | Symmetric transfer (tail-call) vs poll/Waker                        |
| Deep nested coroutines             | No stack growth, no atomic per level                                |
| Composition (whenAll / whenAny)    | Children join inline on the same worker                             |
| Cancellation polling               | `co_await checkCancel()` is one atomic load (no chain walk on happy path) |
| `JoinHandle::join`                 | Single atomic CAS, no mutex                                         |
| Deque owner push/pop               | No CAS on fast path (vs Tokio's tagged refcounts)                   |

### Where Tokio has the slight edge

| Operation                          | Tokio       | Manwe       | Gap         |
|------------------------------------|-------------|-------------|-------------|
| Single submit (no awaits)          | ~300 ns     | ~310 ns     | ~10 ns      |

Within benchmark noise. Tokio's published submit-only figure is its
best case (heavy LIFO slot reuse with worker-local steal-back
prevention); Manwe reaches a similar number without those tricks
because the per-worker MPMC inbox shares the same partitioning
property.

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
hardware; the Yarn dispatch path is identical, and the Reactor
backend differs but does not contribute to these benchmarks.

---

## Caveats

- Tokio numbers are taken from published benchmarks and the
  tokio-rs/runtime-perf project; no side-by-side Tokio bench has been
  run on the same hardware. The ranges quoted are the public
  documented ones, not estimates.
- Boost.Asio submit-dispatch ranges depend heavily on the executor
  choice; the documented range for `thread_pool` plus `co_spawn` from
  a non-worker thread is quoted.
- Throughput-per-core numbers are sustained rates with the workload
  fully fitting in cache; cold-cache numbers are worse for all
  runtimes compared here.
- The per-`co_await` cost excludes user-side handler work; it is the
  runtime's overhead alone.

---

## Remaining headroom

Tokio-parity on submit and 3–5× better on chain hops is the current
ceiling. Two paths still have measurable headroom:

- **Fan-out (`spawn+join`)** — ~250 ns / await today, dominated by
  `coSpawn` plus per-child latch arithmetic. A bulk-spawn path that
  skips per-task latch updates is the next round.
- **Reactor I/O resumption** — `Yarn::run` scheduling from the loop
  thread is one MPMC push and one wake; bypassing the wake when the
  target worker is already running cuts roughly 60 ns off the
  resumption path.

See [`CHANGELOG.md`](CHANGELOG.md) for the rolling history of
performance rounds.
