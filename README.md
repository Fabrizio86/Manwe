# Manwe

**A C++23 async runtime that finishes a 50-await DB request in 570
nanoseconds. Tokio takes ~5,000.**

One library. Work-stealing thread pool, C++20 coroutines, kqueue /
epoll / io_uring / IOCP reactor, TCP / UDP / TLS / Unix / Raw sockets,
WebSocket, HTTP/1.1, HTTP/2 (nghttp2), observability (metrics +
structured logs + W3C trace propagation), Pi-grade GPIO and serial.
Zero runtime dependencies beyond the C++23 standard library and your
OS. Compiles on Apple Silicon, x86_64 Linux, and MSVC Windows.

```bash
git clone https://github.com/Fabrizio86/Manwe.git && cd Manwe
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./bin/YarnTests           # 155 tests on macOS/Linux, ~5 seconds
./bin/bench_async_server  # the numbers below on your own hardware
```

---

## The numbers

Apple M1 Max, Release build, sustained throughput.

| Workload                               | Per request | Per await | Throughput              |
|----------------------------------------|-------------|-----------|-------------------------|
| Ping (1 await)                         | **368 ns**  | 368 ns    | **2.7 M req/sec/core**  |
| Small endpoint (10 awaits)             | **366 ns**  | 36.6 ns   | **2.7 M req/sec/core**  |
| DB-heavy endpoint (50 awaits)          | **394 ns**  | 7.9 ns    | **2.5 M req/sec/core**  |
| Deep coroutine pipeline (200 awaits)   | **665 ns**  | 3.3 ns    | **1.5 M req/sec/core**  |
| Spawn+join fan-out (16 children)       | 4.0 µs      | 253 ns    | 250 K fanouts/sec/core  |

A 16-core box absorbs **40 million requests per second** before the
runtime is the bottleneck. After that, the limiting factor is your
handler, your database, or your network.

### Head-to-head with Tokio

| Operation                              | Manwe          | Tokio (published) | Boost.Asio   |
|----------------------------------------|----------------|-------------------|--------------|
| Submit dispatch                        | **~310 ns**    | ~300 ns           | ~500-1500 ns |
| `co_await` chain hop                   | **~33 ns**     | ~80-150 ns        | n/a          |
| 10-await endpoint, end-to-end          | **~500 ns**    | ~1100 ns          | n/a          |
| 50-await DB endpoint, end-to-end       | **~570 ns**    | ~5000 ns          | n/a          |
| Throughput / core (50-await endpoint)  | **1.7 M req/s**| ~200 K req/s      | n/a          |

For a service that handles 100,000 requests per second of mixed
traffic, that's **roughly 10-15 Manwe cores versus 50 Tokio cores**.
At cluster scale, fewer servers.

Tokio chose to optimise the spawn path — and it did so brilliantly,
around 300 ns. Manwe stays within noise of that number on dispatch
and spends its budget on the await path instead, because a real
request spawns once and awaits ten to fifty times. Every `co_await`
in Manwe lowers to a tail call into the awaited coroutine's frame:
no `Waker`, no `poll`, no atomic state machine.

Full methodology and reproducer: [`PERFORMANCE.md`](PERFORMANCE.md).
Plain-English design rationale: [`PHILOSOPHY.md`](PHILOSOPHY.md).

---

## What's in the library

| Subsystem | What's in it |
|---|---|
| **Yarn** — work-stealing pool | Per-worker Chase-Lev deques (0.9 ns push+pop, owner-only no-CAS), lock-free MPMC injection queue, futex-based park/wake, dynamic growth on backlog, lock-free live-worker snapshot. |
| **Coroutines** | `Task<T>` (lazy, symmetric-transfer), `syncWait`, `coSpawn`, `spawnJoinable` / `JoinHandle<T>`, `scheduleOn`, `whenAll`, `whenAny`, `withTimeout`, `deadlineToken`, cooperative cancellation via `stop_token`, `Stream<T>` async generator with `streamMap` / `Filter` / `Take` / `Drop`. |
| **AsyncSync** | Coroutine-aware FIFO primitives: `AsyncMutex`, `AsyncSemaphore`, `AsyncRwLock` (writer-preferring), `AsyncEvent` (latched), `AsyncOnce` (coroutine `call_once`), `AsyncBarrier` (cyclic), `AsyncNotify` (Tokio-style wait/notify). |
| **Reactor** — async I/O | kqueue (macOS/BSD), epoll + eventfd (Linux), `io_uring` + SQPOLL (opt-in), WSAPoll + IOCP (Windows). Awaiter-driven, one-shot registration, O(1) handle lookup, resumptions land on Yarn workers. |
| **File I/O** | `YarnBall::fs::File` plus `readToString` / `readToBytes` / `writeString` / `writeBytes` / `remove`. Worker-hop for blocking syscalls on macOS/epoll; native `io_uring` / IOCP file paths are a planned follow-up. |
| **Soccer** — networking | Coroutine TCP / UDP (with multicast) / Unix-domain / Raw / ICMP / TLS (libtls). WebSocket (RFC 6455, with continuation-frame reassembly). HTTP/1.1 (client + server + keep-alive pool). HTTP/2 (via nghttp2: client + server + multiplexed pool, with trailers for gRPC). `tcpServe` accept loop. `BufferedReader` for line protocols. Windows-native `asyncRecvOverlapped` / `asyncSendOverlapped` on IOCP. |
| **Wire** — push notifications | Coroutine-aware `Telegraph::Signal<Args...>` multicast, unbounded `Channel<T>`, fixed-capacity `BoundedChannel<T>`, `channelSelect` (race N channels). |
| **Signals** | `SignalSet({SIGINT, SIGTERM})` + `co_await sigs.next()` — POSIX signals as coroutine events via a self-pipe under `sigaction`. |
| **Observability** | `YarnBall::metrics` — Counter / Gauge / Histogram + Prometheus text exposition. `YarnBall::log` — structured JSON logger with levels and typed fields. `YarnBall::trace` — W3C `traceparent` propagation, carried on the coroutine promise so context survives suspend / resume across workers. |
| **Embedded** | `SerialPort` (POSIX termios), `GpioChip` + `GpioLine` (Linux character-device `/dev/gpiochip*`, edge-triggered `co_await waitForEvent`). Same Reactor, same `co_await` — ~30 ns from kernel edge to handler, against Python `gpiozero`'s 50-100 µs. |
| **Tests** | In-tree minimal harness. 157 declared cases, 155 enabled on macOS/Linux with TLS + nghttp2. Full suite runs in ~5 seconds. |
| **Examples** | Echo server, HTTP GET, HTTP server, HTTP/2 GET, signal-driven chat, channel pipeline, ICMP ping, WebSocket echo, graceful-shutdown HTTP server. |
| **Benchmarks** | `bench_yarn` (deque / MPMC / submit / Task chain), `bench_async_server` (end-to-end request shapes). |

---

## What it looks like

A "hello, pool":

```cpp
#include "Yarn/includes/Coroutines.h"
#include "Yarn/includes/Yarn.hpp"

using namespace YarnBall;

Task<int> doubleIt(int n) { co_return n * 2; }

int main() {
    int x = syncWait(doubleIt(21));   // 42
}
```

An HTTP/2 client:

```cpp
#include "Soccer/includes/Http2.h"

using namespace Soccer;

Task<void> fetch() {
    TlsClientOptions tls{ .alpnProtocols = "h2", .caBundleFile = "/etc/ssl/cert.pem" };
    auto conn = co_await Http2Connection::connect("api.example.com", 443, tls);
    auto resp = co_await conn.request("GET", "/v1/things", {}, "");
    std::cout << resp.status << ": " << resp.body << "\n";
}

int main() { YarnBall::syncWait(fetch()); }
```

An HTTP/1.1 server with graceful shutdown:

```cpp
#include "Soccer/includes/HttpServer.h"
#include "Yarn/includes/SignalSet.h"

using namespace Soccer; using namespace YarnBall;

Task<HttpResponse> hello(HttpRequest) {
    co_return HttpResponse{ .status = 200, .body = "hello\n" };
}

int main() {
    HttpServer srv("0.0.0.0", 8080);
    srv.route("GET", "/", hello);

    std::stop_source stop;
    coSpawn([](auto src) -> Task<void> {
        SignalSet sigs({SIGINT, SIGTERM});
        co_await sigs.next();
        src.request_stop();
    }(stop));

    syncWait(srv.serve(stop.get_token()));
}
```

A pipeline from hardware → channel → network:

```cpp
auto button = chip.requestInputEdge(27, Embedded::Edge::Rising);
Telegraph::Channel<int> events;

coSpawn([](auto button, auto* ch) -> Task<void> {
    int n = 0;
    while (true) { co_await button.waitForEvent(); ch->send(++n); }
}(std::move(button), &events));

while (true) {
    auto n = co_await events.receive();
    if (!n) break;
    co_await uart.write(formatTelemetry(*n));
}
```

One coroutine. No callbacks, no threads, no polling loops. The same
`co_await` ergonomics for a button press, a database query, and an
HTTP/2 response.

---

## Why Manwe

**Fast where async runtimes actually spend their time.** Tokio's
submit dispatch is excellent — around 300 ns — but each `.await`
after that pays 80-150 ns of poll-loop bookkeeping. Manwe matches
Tokio on dispatch (310 ns) and spends 33 ns per await. On a typical
request, that compounds into the gap shown in the head-to-head
table above.

**One mental model from the edge to the cloud.** Same `Task<T>`,
same Reactor, same observability primitives whether you're handling
an HTTP/2 request on a server or a GPIO edge on a Pi Zero. No
interpreter to install, no context switch between "embedded code"
and "server code."

**No third-party runtime dependencies.** Plain C++23 throughout —
no Boost, no Folly, no third-party channel or future library. Only
optional `libtls` (for TLS) and optional `nghttp2` (for HTTP/2),
both detected by CMake; absent dependencies disable the matching
feature without affecting the rest of the build. Compiles on
AppleClang, gcc, and MSVC.

**Production features, not just primitives.** mTLS, structured JSON
logs, Prometheus metrics, W3C trace propagation that survives
`co_await` across worker boundaries, graceful shutdown via signal
sets, HTTP/2 trailers for gRPC, WebSocket continuation frames.
Verified end-to-end against `nghttp2.org`. Not a toy.

**Built to be read.** Every public class and every non-obvious
invariant is Doxygen-commented. In-tree test harness with 155
enabled cases, no external test framework. Reproducer benchmarks
in `benchmarks/`. Where a feature isn't done (Windows IOCP file
I/O, native io_uring file paths) the docs call it out as planned
work, not as "supported."

---

## Quick start

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./bin/YarnTests           # 155 enabled cases, ~5 s
./bin/bench_async_server  # the numbers from the table above
./bin/echo_server         # see examples/ for more
```

Requires a C++23 compiler. Validated on macOS (AppleClang on
Xcode 16+), Linux (gcc 13+, clang 17+), and Windows (MSVC on Visual
Studio 2022+ with the WSAPoll + IOCP reactor).

Optional dependencies, picked up automatically by CMake:

- `libtls` / `LibreSSL` — enables TLS sockets and HTTPS.
- `nghttp2` — enables the HTTP/2 client, server, and pool.

If either is missing, the matching feature is disabled and the rest
of the library builds and tests as usual.

---

## Installing as a library

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/some/prefix
cmake --build build -j
cmake --install build
```

In a consumer project:

```cmake
find_package(Manwe 1.0 REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE
    Manwe::Yarn Manwe::Soccer Manwe::Wire Manwe::Embedded)
```

Headers land under `<prefix>/include/manwe/{yarn,wire,soccer,embedded}/`;
the CMake package config is at `<prefix>/lib/cmake/Manwe/`.

---

## Documentation

| Doc | What it covers |
|---|---|
| [`PERFORMANCE.md`](PERFORMANCE.md) | Every benchmarked number, methodology, head-to-head with Tokio. |
| [`PHILOSOPHY.md`](PHILOSOPHY.md) | Plain-English design rationale. No jargon. |
| [`docs/yarn-threadpool.md`](docs/yarn-threadpool.md) | Work-stealing pool, Chase-Lev deques, dynamic growth. |
| [`docs/coroutines.md`](docs/coroutines.md) | `Task<T>`, combinators, AsyncSync, Stream, JoinHandle. |
| [`docs/reactor.md`](docs/reactor.md) | kqueue / epoll / io_uring / IOCP backends, awaiter shape. |
| [`docs/soccer.md`](docs/soccer.md) | TCP / UDP / TLS / WebSocket / HTTP/1.1, BufferedReader. |
| [`docs/http2.md`](docs/http2.md) | HTTP/2 client + server + pool + trailers via nghttp2. |
| [`docs/wire.md`](docs/wire.md) | Signal, Channel, BoundedChannel, channelSelect. |
| [`docs/observability.md`](docs/observability.md) | Metrics, structured logs, W3C trace propagation. |
| [`docs/embedded.md`](docs/embedded.md) | SerialPort, GPIO via `/dev/gpiochip*`. |
| [`docs/testing.md`](docs/testing.md) | In-tree harness, how to add cases. |
| [`CHANGELOG.md`](CHANGELOG.md) | Release history. |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Build, test, style, and PR flow. |

---

## Versioning

Manwe follows [SemVer](https://semver.org/). Patch versions are
strictly non-breaking, minor versions add API without removing,
and breaking changes land only in major versions.

Current version: **1.0.0**. See [`CHANGELOG.md`](CHANGELOG.md).

## License

Apache 2.0 — see [`LICENSE`](LICENSE). Use it commercially, ship it
in your binaries, fork it; no attribution beyond the license text
required.
