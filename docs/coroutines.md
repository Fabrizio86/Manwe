# Coroutines — `Task<T>`, awaiters, combinators

A C++20 coroutine layer on top of Yarn. The model is asynchronous, lazy,
and uses symmetric transfer for everything that can chain on a single
worker — the scheduler only gets involved when work genuinely suspends
(I/O, explicit `scheduleOn`, `coSpawn`).

## Why this design wins on real servers

Every `co_await someTask` in Manwe is a tail-call — one
`std::coroutine_handle::resume()` that the compiler lowers to a
register-shuffle plus an indirect jump. No `Waker` heap object, no
`poll()` virtual call, no atomic state bits per hop. The cost on an
Apple M1 Max is **~33 ns / hop** measured, **~3 ns / hop amortised** on
deep chains where the symmetric-transfer chain is the entire workload.

Tokio's poll-loop futures pay **~80-150 ns per `.await`** for the
same operation — that's the irreducible overhead of the
`poll(&mut self, &mut Context)` protocol every Future has to
implement. Manwe's protocol is `coroutine_handle::resume()`.

This is the architectural advantage that compounds on every await in
every request. At K=50 awaits/request (a typical DB-heavy endpoint)
the difference is **3-5× end-to-end** vs Tokio. See the
[README](../README.md#performance) for the full breakdown.

---

## Architecture

### `Task<T>`

`Task<T>` is a move-only wrapper around a `coroutine_handle<promise_type>`.
It is **lazy**: the coroutine body does not run on construction. Instead:

- `initial_suspend()` returns `std::suspend_always`, so the body is
  paused at the entry until something resumes it.
- `co_await someTask` registers the caller as the awaited task's
  continuation and **symmetric-transfers** into the task. The whole
  awaited chain runs on the same worker; no scheduler bounce.
- `final_suspend()` returns an awaiter whose `await_suspend` either
  transfers to the stored continuation, or — for *detached* tasks
  (set by `coSpawn`) — destroys the coroutine frame in place.

The detached flag is the trick that makes `coSpawn` leak-free even when
the spawned coroutine suspends across reactor I/O: there is no external
"owner" that has to remember to destroy the frame later. The coroutine
self-cleans at its own final suspend point.

### Result storage

For `Task<T>`, the promise holds aligned storage `std::byte[sizeof(T)]` so
`T` does not need to be default-constructible. The destructor of the
promise destroys the live `T` if one was produced. `Task<void>` has a
trivial specialisation.

### `syncWait`

Blocks the calling thread until a task completes and returns its result
(or rethrows the exception). Internally:

1. Builds an inner *helper* coroutine that does `co_return co_await task`.
2. The helper's final-suspend awaiter signals a `ManualResetEvent`.
3. The caller blocks on `event.wait()`.

The helper holds the awaited `Task<T>` in its frame, which keeps the
coroutine alive until the helper is destroyed by `syncWait` on return.

`syncWait` is for tests and top-level entry. **Do not call it from inside
a worker** — it would block the worker thread on its own pool.

### `coSpawn`

Fire-and-forget submission of a `Task<T>` to the Yarn pool:

1. `task.release()` hands over the handle.
2. The promise's `detached` flag is set.
3. A `CoroutineITask` wrapping the handle is submitted via `Yarn::run`.

`CoroutineITask` is a deliberately dumb adapter: it calls `handle.resume()`
once and then never touches the handle again. Whether the coroutine
completed (and self-destroyed) or suspended on an awaiter that took over
ownership, the ITask is finished.

### `scheduleOn(Yarn*)`

A trivial awaiter that suspends the current coroutine and resubmits its
handle to a pool. After `co_await`, execution continues on a worker. Used
to:

- Move work off whichever thread happened to resume us.
- Yield to the pool inside a loop (a natural place to observe a
  `stop_token`).

### `whenAll`

Aggregates N concurrent tasks and resumes when every one has reported in.
Implementation uses a per-state atomic latch counter that starts at `N+1`
— the extra `1` is "the awaiter has registered itself". The counter is
decremented by every completing subtask AND by the awaiter on
registration; whoever brings it to zero owns the resumption. This closes
the race between "all subtasks finish before the awaiter suspends" and
"awaiter registers before any subtask finishes".

Sub-tasks are dispatched via `coSpawn`, so the pool spreads them across
workers. The aggregator coroutine suspends on a latch awaiter.

Exception handling: the first exception observed is stored under a small
mutex; later ones are silently dropped. `await_resume` rethrows that first
exception, so the caller sees a single representative failure. Sibling
tasks are NOT auto-cancelled; if you need that semantic, pass a
`std::stop_token` and have the siblings check it.

### Cooperative cancellation (`std::stop_token`)

Manwe does not add a custom cancellation primitive. The pattern is:

```cpp
Task<int> work(std::stop_token tok) {
    while (!tok.stop_requested()) {
        co_await scheduleOn(Yarn::instance());
        // ... step of work ...
    }
    co_return ...;
}
```

The token is passed as an ordinary parameter — it lives in the coroutine
frame for the lifetime of the body. Auto-propagation through nested
`co_await`s is **not** provided in v1; pass it explicitly to children, or
store it in shared state.

### Internal cancellation (`requestCancel` / `checkCancel`)

`Task<T>::requestCancel()` flips an atomic in the current task's promise.
Inside any descendant, `co_await checkCancel()` walks the parent chain
once and throws `CancelledException` if any ancestor has been cancelled.
The walk happens only at the explicit check site — there is no per-await
cancellation tax.

```cpp
Task<void> longLoop() {
    for (;;) {
        co_await checkCancel();
        co_await stepOfWork();
    }
}

auto t = longLoop();
t.requestCancel();   // descendants exit on their next checkCancel
```

### Trace context that survives resume on a different worker

`co_await scheduleOn(...)` or any genuinely suspending I/O may resume
the coroutine on a different Yarn worker, which means the thread-local
`trace::current()` is no longer the one your handler installed.
`trace::currentAsync()` and `trace::installCurrent(ctx)` are coroutine
awaiters that read / write the trace context **on the promise itself**,
so it survives suspend / resume regardless of which worker resumes.

```cpp
Task<void> handleRequest(Soccer::HttpRequest req) {
    auto ctx = trace::parseTraceparent(req.header("traceparent"));
    co_await trace::installCurrent(ctx.empty() ? trace::newRoot() : ctx);

    auto rows = co_await db.query("SELECT ...");   // may suspend
    // `co_await trace::currentAsync()` here returns the right context
    // even though we may have resumed on a different worker.
    co_return;
}
```

`trace::Scope` (RAII thread-local) is still the right primitive for
non-coroutine code or a synchronous handler block. The two coexist;
inside a Task, the promise carrier wins.

---

## Using it

### Define a Task

```cpp
YarnBall::Task<int> compute(int x) {
    co_return x * 2;
}

YarnBall::Task<void> log_line(std::string s) {
    std::cout << s << "\n";
    co_return;
}
```

### Block on a Task from non-coroutine code

```cpp
int result = YarnBall::syncWait(compute(21));   // 42
YarnBall::syncWait(log_line("done"));
```

### Spawn a Task on the pool

```cpp
YarnBall::coSpawn(log_line("background"));
// returns immediately; the task runs on a worker
```

> **Lambda lifetime warning.** Do NOT `coSpawn` a coroutine whose body is
> a lambda capturing by-reference or by-value: the lambda closure is a
> temporary that dies at the end of the full expression, while the
> coroutine still references the captures through the implicit `this`. Use
> a free function with parameters instead:
>
> ```cpp
> static Task<void> do_thing(int id, std::atomic<int>* counter) { ... }
> coSpawn(do_thing(7, &counter));   // captures live in the coroutine frame
> ```

### Chain tasks with `co_await`

```cpp
Task<int> pipeline() {
    int a = co_await compute(10);
    int b = co_await compute(a);
    co_return a + b;
}
```

The whole pipeline runs on a single worker (or wherever the outermost
caller is); chained `co_await` is a symmetric tail-call, not a schedule.

### Hop to a worker

```cpp
Task<void> on_listener() {
    int fd = co_await accept_connection();
    co_await scheduleOn(Yarn::instance());   // off the listener thread
    co_await handle_client(fd);
}
```

### Run things concurrently

```cpp
std::vector<Task<int>> jobs;
for (int i = 0; i < 16; ++i) jobs.push_back(compute(i));

auto results = syncWait(whenAll(std::move(jobs)));
// results[i] == i * 2 for every i
```

If any job throws, `whenAll` rethrows the first observed exception from
`await_resume`.

### Cancellation

```cpp
std::stop_source src;
std::thread cancel_thread([&] {
    std::this_thread::sleep_for(100ms);
    src.request_stop();
});

int n = syncWait(work(src.get_token()));
cancel_thread.join();
```

---

## Invariants

- **Lazy by construction.** A `Task<T>` that is created and dropped does
  nothing — its coroutine body never runs.
- **`co_await` is tail-call.** A chain of `co_await someTask` stays on one
  worker until something genuinely suspends.
- **Detached coroutines self-destroy.** Any `coSpawn`'d coroutine
  cleans itself up at final suspend; no Task wrapper required.
- **`syncWait` is for entry points.** Never call it from a worker.
- **`whenAll` does not cancel siblings on exception.** Pass a
  `stop_token` if you need that.

---

## Combinators beyond `whenAll`

### `whenAny<T>` — first-to-finish wins

Returns a `WhenAnyResult<T>` containing the index + value of the first
subtask to complete, and rethrows its exception if it threw.

```cpp
std::vector<Task<int>> probes;
probes.push_back(askMirror("us-east"));
probes.push_back(askMirror("eu-west"));
probes.push_back(askMirror("ap-south"));

auto r = co_await whenAny(std::move(probes));
std::cout << "fastest mirror: " << r.index << " -> " << r.value;
```

The losers keep running on the Yarn pool until their own completion
(C++ coroutines have no forced cancellation). Pair with a `stop_token`
if you want them actively shortened.

### `withTimeout(task, duration)` — race against a clock

```cpp
try {
    auto resp = co_await withTimeout(httpGet(url), 5s);
    process(resp);
} catch (const TimeoutException&) {
    log("upstream took too long");
}
```

Implementation is `whenAny(task, sleepFor(duration))` with a typed
wrapper. Both `Task<void>` and `Task<T>` overloads exist. Same loser
semantics as `whenAny`.

### `deadlineToken(duration)` — a stop_token that fires after a delay

When you want to share one deadline with multiple sub-tasks without
spinning up a `withTimeout` per child, `deadlineToken` returns a
`std::stop_token` that flips after `duration`:

```cpp
auto tok = deadlineToken(2s);
auto r = co_await whenAll(
    fetchPrimary(tok),
    fetchSecondary(tok),
    fetchTertiary(tok));
```

Backed by a small detached coroutine that sleeps and calls
`request_stop()`. Cheap, but don't create thousands per second — pass
one token to N children instead.

---

## Async synchronisation — `AsyncMutex`, `AsyncSemaphore`

`std::mutex` cannot be held across a `co_await`: the worker the
coroutine resumes on may not be the worker that took the lock, so the
release fails. `AsyncMutex` and `AsyncSemaphore` are coroutine-aware:
they suspend the coroutine on contention, FIFO-park the handle, and
dispatch the wakeup through Yarn rather than inline-resuming the
releaser (which would unbound the releaser's stack).

```cpp
AsyncMutex m;

Task<void> bumpUnderLock(int* counter) {
    auto guard = co_await m.lock();   // RAII guard
    ++*counter;
    co_return;                         // guard releases on scope exit
}
```

Backpressure with `AsyncSemaphore`:

```cpp
AsyncSemaphore inflight(/*permits=*/64);

Task<void> handle(Request r) {
    co_await inflight.acquire();
    try {
        co_await doWork(std::move(r));
    } catch (...) {
        inflight.release();
        throw;
    }
    inflight.release();
}
```

Both also expose `tryLock` / `tryAcquire` for non-suspending attempts.

---

## `Stream<T>` — lazy coroutine generator

A coroutine return type that `co_yield`s zero or more values, then
completes (or throws). Each pull resumes the producer until the next
`co_yield`; the producer suspends and the consumer wakes with the
value.

```cpp
Stream<int> rangeUntil(int n) {
    for (int i = 0; i < n; ++i) co_yield i;
}

Task<int> sumStream() {
    int total = 0;
    YARN_FOREACH_AWAIT(int v, rangeUntil(100)) {
        total += v;
    }
    co_return total;
}
```

Or pull one at a time:

```cpp
auto s = rangeUntil(100);
while (auto v = co_await s.next()) {
    handle(*v);
}
```

`Stream` is move-only and single-consumer; concurrent consumption is
undefined. Exceptions thrown from the producer body surface on the
consumer's next `next()` call.

---

## `AsyncNotify` — coroutine wait/notify

A standalone wait/notify primitive: one coroutine parks via
`notified()`, another fires `notifyOne()` or `notifyAll()`.
Notifications are NOT latched — a notify with no waiters is dropped,
and a subsequent `notified()` parks until the next notify.

```cpp
AsyncNotify ready;
bool flag = false;

// waiter
co_await ready.notified();
assert(flag);

// notifier (after publishing the flag)
flag = true;
ready.notifyOne();
```

Compose with `AsyncMutex` for textbook condition-variable patterns
(release the lock, suspend, re-acquire on wake).

---

## `JoinHandle<T>` + `spawnJoinable(task)`

The gap between `coSpawn` (fire-and-forget) and `whenAll` (batch):
spawn a task, get a handle, await the handle later.

```cpp
auto h = spawnJoinable(slowHash(bigBuffer));
// ... do other work; the hash runs in the background ...
auto digest = co_await h.join();
```

`JoinHandle` is move-only and single-consumer. Dropping the handle
without joining is fine; the task continues running and its result is
discarded. `h.done()` is a cheap non-blocking poll for completion.
