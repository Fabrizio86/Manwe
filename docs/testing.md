# Testing — in-tree harness

Manwe ships its own tiny test framework rather than vendoring doctest /
Catch2 / gtest. The whole thing is one header (`YarnTests/test_framework.hpp`)
and registers itself via static initialisers. Tests live in
`YarnTests/main.cpp`.

---

## Architecture

### Registrar pattern

Each `TEST(name)` macro expands to:

1. A forward declaration of the test function.
2. A static `Registrar` instance whose constructor pushes the test into a
   global `std::vector<TestCase>`.
3. The test function body.

```cpp
TEST(my_test) {
    EXPECT_EQ(1 + 1, 2);
}
```

becomes (roughly):

```cpp
static void test_fn_my_test();
static ::test::Registrar test_reg_my_test{"my_test", &test_fn_my_test};
static void test_fn_my_test() {
    EXPECT_EQ(1 + 1, 2);
}
```

Static-init order across translation units is unspecified, but every test
case file populates the same registry vector, so the *order of registration
within one file* is preserved and that's what the runner walks.

### Runner

`test::run_all()` walks the registry, calls each test, catches exceptions,
and prints `[ RUN ]` / `[ OK ]` / `[ FAIL ]` lines with microsecond
timings. A summary at the end lists failures with their names. Exit code
is 0 if every test passed, 1 otherwise.

### Assertions

All `EXPECT_*` macros throw `test::TestFailure` on failure with the
`__FILE__:__LINE__` context and a stringified description. The runner
catches and reports. Tests are expected to be self-contained — no
fixtures, no setup/teardown ceremony.

| Macro | Behaviour |
|---|---|
| `EXPECT_TRUE(cond)` | Fail if `!cond` |
| `EXPECT_FALSE(cond)` | Fail if `cond` |
| `EXPECT_EQ(a, b)` | Fail if `!(a == b)`; prints both values |
| `EXPECT_NE(a, b)` | Fail if `a == b` |
| `EXPECT_THROWS_AS(expr, T)` | Fail if `expr` doesn't throw a `T` |

### Why not doctest?

Three reasons:

1. **No vendoring tax.** The harness is ~150 lines. doctest is ~7000.
2. **Zero external dependency.** Builds anywhere a C++23 compiler exists.
3. **Custom matchers when needed.** Adding e.g. `EXPECT_NEAR(a, b, eps)`
   is a 5-line patch in `test_framework.hpp` rather than a vendor diff.

This isn't a permanent decision; if the suite grows past what the harness
comfortably handles (matchers, parallelism, fixtures), swapping in a real
framework is mechanical.

---

## Using it

### Writing a new test

Add it to `YarnTests/main.cpp` near related cases:

```cpp
TEST(my_new_thing_returns_expected) {
    auto result = my_new_thing(42);
    EXPECT_EQ(result, 84);
}
```

### Concurrency tests

Use the `wait_for` helper at the top of `main.cpp` for race-tolerant
spin-waiting:

```cpp
TEST(workers_eventually_drain) {
    std::atomic<int> count{0};
    for (int i = 0; i < 1000; ++i) {
        YarnBall::run(std::make_unique<BumpTask>(&count));
    }
    EXPECT_TRUE(wait_for([&] { return count.load() >= 1000; }, 10s));
    EXPECT_EQ(count.load(), 1000);
}
```

`wait_for` returns `false` if the deadline passes — pair it with a hard
`EXPECT_EQ` after for a clear failure message.

### Coroutine tests

`syncWait` is the natural test driver for `Task<T>`:

```cpp
TEST(my_coroutine_returns_42) {
    EXPECT_EQ(YarnBall::syncWait(my_coroutine()), 42);
}
```

Be mindful of the [lambda lifetime issue](coroutines.md#using-it) —
prefer free functions or named static helpers over lambda-bodied
coroutines whenever the coroutine outlives the enclosing expression.

### POSIX-only tests

Wrap with `#ifndef _WIN32` and use `SocketPair` (defined in `main.cpp`) for
fd-based tests. The reactor section of the suite is a worked example.

---

## Running

```bash
cmake --build build -j
./bin/YarnTests           # runs everything
./bin/YarnTests 2>&1 | grep FAIL   # find failures
```

The full suite runs in ~5 seconds; the 50k-task burst dominates wall
time. Run it a few times to catch intermittent races:

```bash
for i in $(seq 1 10); do ./bin/YarnTests 2>&1 | tail -1; done
```

### What "passing" means

**157 cases declared, 155 enabled on macOS/Linux with TLS+nghttp2.**
The other two are platform-conditional: `http2_stub_throws_not_implemented`
runs only on builds without nghttp2, and `soccer_overlapped_tcp_round_trip`
runs only on Windows. Coverage:

- **Concurrency primitives** — Chase-Lev deque (LIFO push/pop, FIFO
  steal, power-of-two cap, concurrent owner+thieves), MPMC queue
  (basic, full, dequeue out-param, 4×4 producer/consumer stress).
- **Yarn pool** — `unique_ptr` and `shared_ptr` paths, null-safety,
  50k-task burst, stats snapshot.
- **File I/O** — `readToString` / `readToBytes`, streaming read, missing
  file throws, write-then-read round-trip.
- **Waitable** — round-trip + exception propagation.
- **`Task<T>`** — int, void, single chain, 3-deep chain, 200-deep
  recursive chain, synchronous throw, exception propagation through
  `co_await`.
- **`coSpawn` / `scheduleOn`** — single spawn, 2k concurrent spawns,
  chained `co_await` inside a spawned task, hop to worker, null-pool no-op.
- **`whenAll` / `whenAny` / `withTimeout` / `deadlineToken`** — empty,
  aggregate, void, exception propagation, race wins/losses.
- **AsyncSync** — `AsyncMutex`, `AsyncSemaphore`, `AsyncRwLock`,
  `AsyncEvent`, `AsyncOnce`, `AsyncBarrier`, `AsyncNotify`.
- **`Stream<T>`** — basic generator, exceptions across `next()`,
  `streamMap` / `Filter` / `Take` / `Drop` combinators.
- **`JoinHandle<T>`** — happy path, drop without join, `done()` polling.
- **Cancellation** — `stop_token` visibility, `requestCancel` /
  `checkCancel` propagation through descendants.
- **Reactor** — `waitReadable`, `asyncRead`, `asyncWrite` under
  back-pressure, 16 concurrent coroutines on distinct fds.
- **Soccer (TCP)** — listener port discovery, echo round-trip, connect
  refused, half-close, 1 MiB transfer, 50 concurrent clients,
  Unix-domain round-trip + path-too-long rejection, `tcpServe` accept
  until stop.
- **Soccer (UDP)** — send + recvFrom on loopback, multicast loopback
  round-trip, invalid address rejection.
- **Soccer (TLS)** — handshake + round-trip with a self-signed cert
  (skipped if `openssl` is absent), `TlsClientOptions` mTLS fields.
- **Soccer (HTTP/1.1)** — client GET round-trip, server route + 404,
  connection pool reuse, max-idle-per-host enforcement.
- **Soccer (HTTP/2)** — stub throws, refused port surfaces, loopback
  client↔server multiplexed round-trip, server trailers (gRPC pattern),
  pool reuse per host.
- **Soccer (WebSocket)** — text loopback, binary loopback, continuation
  assembly, orphan-continuation rejection.
- **Soccer (BufferedReader)** — three-line read, read-exact.
- **Soccer (ICMP)** — checksum vector, build/parse round-trip,
  short-buffer rejection, loopback ping (root-only, auto-skipped).
- **Soccer (overlapped/IOCP)** — TCP round-trip on Windows-style
  overlapped surface (where the host supports it).
- **Wire** — Signal connect/disconnect/emit, async emit dispatch,
  coroutine `next()` resume, Channel send/receive, blocking receive,
  close wakes pending receivers, BoundedChannel + `channelSelect`.
- **Signals** — `SignalSet` captures self-raised signal, second
  instance throws.
- **Observability** — Counter inc, Gauge set/inc/dec, Histogram
  observe + snapshot, Prometheus scrape, `ScopeTimer`, structured-log
  JSON emission + level filtering + escaping, trace new-root,
  child-id semantics, traceparent round-trip + malformed rejection,
  promise-carrier propagation to descendants, thread-local Scope
  install/restore.

Every commit should keep all enabled cases green (155 on the
macOS/Linux+TLS+nghttp2 reference build).
