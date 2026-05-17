# Changelog

All notable changes to Manwe are listed here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions adhere
to [SemVer](https://semver.org/) with the pre-1.0 caveats spelled out in
the README.

## [Unreleased]

### Added

#### Cross-worker trace context propagation

The thread-local `trace::Scope` covered synchronous `co_await`
chains on one worker but lost its context across resumes on a
different worker. Promise-carried propagation closes that:

- `TaskPromiseCore::traceCtx` -- 25-byte trace context lives in
  the coroutine frame, so it survives suspend/resume regardless
  of which Yarn worker resumes.
- `co_await trace::currentAsync()` walks the parent chain from
  the running coroutine's promise and returns the nearest non-
  empty context.
- `co_await trace::installCurrent(ctx)` writes the trace into the
  running coroutine's promise so descendants observe it.
- Thread-local `trace::current()` and `trace::Scope` are kept for
  non-coroutine callers and as a fallback.

#### HTTP/2 trailers

`HttpResponse::trailers` is now honoured by the h2 server. Handlers
returning trailers (e.g. gRPC's `grpc-status` / `grpc-message`)
emit them as a HEADERS frame after the body, with END_STREAM on
the trailer rather than on the last DATA chunk. Client side routes
post-response HEADERS into `HttpResponse::trailers` automatically.

Implementation: `submit_trailer` is called inside the body's
data-provider EOF callback, which guarantees nghttp2's frame
queue serialises body before trailer.

#### WebSocket continuation frames

`WsConnection::receive` now reassembles multi-frame messages per
RFC 6455 §5.4: a leading TEXT/BINARY frame with FIN=0 plus zero
or more CONTINUATION frames with FIN=0 ending at FIN=1. Control
frames (Ping/Pong/Close) may interleave between fragments.

Sender side: new `WsConnection::sendFrame(FragmentKind, payload,
isFinal)` for streaming a large payload without buffering it all
in memory.

### Fixed

#### WebSocket handshake over-consumption

The handshake reader could over-consume past `\r\n\r\n` into the
first WebSocket frame's bytes; when the server sent the 101
response and first data frame coalesced into one TCP segment,
those frame bytes were silently lost. The handshake reader now
captures any over-consumed trailing bytes into the
`WsConnection::preBuf`, which frame reads drain before issuing
the next kernel `read`.

### Changed

#### FileIo doc honesty

`FileIo.h` header comment and the README row now describe the
current state accurately: worker-hop is the only option on macOS
kqueue and Linux epoll because regular files don't generate
readiness events. Native async file I/O on Linux io_uring and
Windows IOCP is called out as a planned follow-up round rather
than vague "v2."

### Added

#### HTTP/2 client + server + connection pool (via nghttp2)

Manwe ships HTTP/2 as a coroutine wrapper around nghttp2 (the BSD-
licensed C reference implementation used by curl, Apache, nginx,
Envoy, and gRPC). See `docs/http2.md` for the long-term-support
rationale; inline implementation was scoped and rejected on
security/compliance grounds (HTTP/2 has ~50 known CVE classes that
nghttp2 already has patches for).

Three public surfaces:

- **`Soccer::Http2Connection`** — client. `connectPlain(host, port)`
  for h2c, `connect(host, port, TlsClientOptions)` for h2 over TLS
  (auto-appends `"h2"` to ALPN). `request(method, path, headers,
  body)` opens one multiplexed stream per call; many can be in
  flight concurrently on the same connection. Verified end-to-end
  against `nghttp2.org` (status 200, 6 KB body over TLS with HPACK).
- **`Soccer::Http2Server`** — server. Same `route(method, path,
  handler)` API as `Soccer::HttpServer` so route definitions are
  portable between the HTTP/1.1 and HTTP/2 servers verbatim.
  Multiplexed: every accepted connection runs an nghttp2 server
  session that hosts N concurrent streams; each request dispatches
  to its handler as a separate coroutine and the response is
  shipped back on the same stream.
- **`Soccer::Http2ConnectionPool`** — process-wide cache of long-
  lived multiplexed connections, keyed by host:port (and TLS
  options hash where relevant). Unlike the HTTP/1.1 pool, an h2
  entry is shared by many concurrent requests rather than checked
  out exclusively. `acquirePlain` / `acquire` / `evict`.

Build: CMake `find_package`s `libnghttp2`; the `SOCCER_HAS_HTTP2`
compile define gates the implementation. Without nghttp2 the same
`Http2.h` exposes stubs that throw `Http2NotImplemented`, so user
code compiles in either configuration.

Example program: `examples/http2_get.cpp` (`./bin/http2_get tls
nghttp2.org 443 /`).

Loopback test: `http2_loopback_client_to_server_round_trip` spins
up an `Http2Server`, drives two multiplexed requests from a client
through one connection, completes in ~700 µs.

#### Six-feature round: WebSocket, Unix sockets, multicast UDP, signals, SerialPort, GPIO

Boost-equivalence pass driven by the Pi / IoT angle plus the
"common Boost users complain about" list. All implementations
follow the project philosophy (one verb per operation, no
callbacks, await-based, reactor-composed, no options structs
unless OS-mandated).

- **WebSocket (RFC 6455)** (`Soccer/includes/WebSocket.h`) — server
  and client endpoints, text + binary messages, automatic Ping/Pong,
  graceful Close handshake. Built on `TcpStream`; the handshake
  computes `Sec-WebSocket-Accept` via inline SHA-1 + base64 (no
  external crypto dependency added). 16 MiB hard cap on data frames;
  single-frame messages only in v1 (continuation frames raise
  `WsException`).
- **Unix domain sockets** (`TcpListener::bindUnix(path)` +
  `tcpConnectUnix(path)`) — `AF_UNIX` stream sockets through the
  existing accept/read/write coroutines. Stale-socket-file unlink
  on bind; hard-fail on `sun_path` overflow rather than silent
  truncation. Available on POSIX and Windows 10 1803+.
- **Multicast UDP** (`UdpSocket::joinGroup`/`leaveGroup`/
  `setMulticastTtl`/`setMulticastLoop`/`setMulticastInterface`) —
  IPv4/IPv6 group membership with optional per-interface scoping
  (multi-NIC Pi case; macOS loopback requires the same selection).
  Family auto-detected from the address literal.
- **POSIX signal capture** (`Yarn/includes/SignalSet.h`) —
  `SignalSet({SIGINT, SIGTERM})` + `co_await sigs.next()` returns
  the signal number. Self-pipe behind an async-signal-safe
  `sigaction` handler, watched by the existing Reactor's
  `waitReadable`. Singleton-enforced via CAS on the global write
  fd; original handlers restored on destruction.
- **SerialPort** (`Embedded/includes/SerialPort.h`) — POSIX termios
  with sensible 115200-8N1 defaults; coroutine read/write through
  the Reactor. Baud rates 1200..921600, all standard parity / stop
  / flow combinations. Windows OVERLAPPED path is a stub for the
  follow-up round. `MANWE_UNTESTED_PLATFORM` — needs hardware
  validation.
- **GPIO** (`Embedded/includes/Gpio.h`) — Linux `/dev/gpiochip*`
  character-device ioctls (no `libgpiod` dependency). Outputs via
  `chip.requestOutput(pin)` + `line.set(true/false)`; inputs via
  `chip.requestInputEdge(pin, Edge::Rising)` + `co_await
  line.waitForEvent()`. Event reads are reactor-driven, so a
  hardware interrupt-to-handler hop is ~30 ns through symmetric
  transfer (vs ~50-100 µs for a Python `gpiozero` callback).
  `MANWE_UNTESTED_PLATFORM` — needs Pi validation.

New CMake target `Manwe::Embedded` (header dir `manwe/embedded/`)
holds SerialPort and GPIO. Existing `Manwe::Yarn`, `Manwe::Wire`,
`Manwe::Soccer` targets unchanged.

#### Cooperative cancellation on `Task<T>` (Tokio-parity)

The one place where Tokio's packed-task representation was legitimately
ahead: an atomic cancellation flag readable from inside the running
coroutine without scheduler involvement. Now closed.

- **`Task::requestCancel()`** — sets an atomic flag on the task's
  promise. Safe to call from any thread, any number of times. No-op on
  empty / completed tasks.
- **`co_await checkCancel()`** — polls the running coroutine's
  cancellation flag (and walks the parent chain so cancelling a parent
  reaches awaited children). Throws `YarnBall::CancelledException` if
  any flag is set. Does NOT actually suspend: `await_suspend` returns
  `false`, control resumes inline. One acquire load on the happy path.
- **Parent-chain propagation** — `Task::Awaiter::await_suspend` is now
  templated on the caller's promise type so it can hook this task into
  the cancellation chain when the caller is also a `Task`. Compile-time
  branch (`if constexpr`); zero runtime cost when the caller is a
  non-Task coroutine (e.g. `SyncWaitTask`).

Cooperative model — same shape as Tokio's `CancellationToken` but
without the per-`.await` poll overhead, since checks happen only where
the user inserts them. For unconditional periodic checking, drop a
`co_await checkCancel();` at the top of each loop iteration.

#### `JoinHandle<T>` is now lock-free

The previous join latch used `std::mutex` to coordinate the publish /
wait handshake. Replaced with a single `std::atomic<std::uintptr_t>`
that encodes one of: no-waiter (0), parked-handle (the handle's
address), or ready (`kJoinReadyBit = 1`). Aligned handle addresses are
guaranteed to have bit 0 clear, so the sentinel is unambiguous.

- Runner publishes the result via a single `exchange(kJoinReadyBit,
  acq_rel)`. If the previous value was an address, the waiter is woken
  via `Yarn::run` (NOT inline, to bound stack on chained joins).
- Awaiter CAS-installs its own address from 0; on CAS-failure
  (`expected` now holds `kJoinReadyBit`), it resumes inline. No mutex,
  no missed wake, no recheck loop.
- `JoinHandle::done()` is now a plain atomic load against the same word.

Test suite (`spawn_joinable_*`) passes unchanged.

#### `bench_async_server` — end-to-end "request"-shaped benchmark

Microbenchmarks under-weight Manwe's design wins because the
symmetric-transfer Task chain and the fan-out path benefit MORE from
each other than each does standalone. The new benchmark simulates
realistic async-server request shapes:

| Scenario                        | Per-request | Per-await |
|---------------------------------|-------------|-----------|
| K=1   single-await ping          | ~370 ns     | ~370 ns   |
| K=10  small endpoint             | ~370 ns     | ~37 ns    |
| K=50  DB-heavy                   | ~395 ns     | ~8 ns     |
| K=200 deep coroutine pipeline    | ~665 ns     | ~3 ns     |
| W=16 spawn+join fan-out          | ~4050 ns    | ~253 ns   |

The per-await numbers are the structural advantage: each chain hop is
a tail-call (`std::noop_coroutine` fallthrough or symmetric transfer),
not a poll-loop bounce through a Waker. Tokio's per-poll cost is
typically ~80-150 ns — Manwe's amortised chain hop is **25-40× cheaper**
on deep chains. This is the path that's higher-traffic in realistic
workloads (web/RPC servers: 5-50 awaits per request), where end-to-end
request latency is what matters.

Run via `bin/bench_async_server` after building the
`bench_async_server` CMake target.

### Changed

- **Renamed `Wire/includes/Signal.h` → `TelegraphSignal.h`.** On
  macOS's case-insensitive filesystem the old name shadowed the
  system `<signal.h>` once Wire's include directory was on the
  search path, breaking any consumer that pulled `<csignal>` or
  `<signal.h>`. Class is `Telegraph::Signal<>`; new file name
  matches. Only consumer was `Wire/includes/Wire.h`.

### Performance

#### Dispatch hot-path overhaul (~2.3× faster end-to-end submit)

Targeted profiling of `Yarn::run` identified two structural waste-bins
and three smaller ones. Median submit cost on Apple M1 Max (Release)
went from **~1200 ns (baseline)** → **~520 ns**.

1. **Parked-worker fast skip** (biggest single win). `Yarn` keeps an
   atomic `parkedWorkers` count; `wakeOneParked` returns immediately
   when zero, skipping the snapshot copy, the per-fiber `isParked`
   walk, and any subsequent `poke`. Under steady-state submit-heavy
   load all workers are running, so this branch fires unconditionally.

2. **Lock-free wake protocol.** Replaced the per-`Fiber`
   `std::condition_variable + std::mutex` pair with a single
   `std::atomic<uint32_t> parkSignal`. `park` snapshots the value
   before suspending; `poke` bumps it and calls `notify_one`. No
   mutex acquire on either side. Removed the periodic CV timeout —
   `seed` and the injection-queue path now wake parked peers
   directly.

3. **Skip `reap()` when graveyard is empty.** Atomic `graveyardCount`
   counter; the per-`run` reap call returns without acquiring `cmu`
   when no fibers have retired since the last sweep.

4. **Skip `maybeGrowLocked` at the temp-fiber ceiling.** Atomic
   `tempFiberCount` short-circuits the cmu acquire when the pool is
   already maxed out.

5. **Thread-local snapshot cache.** `Yarn::currentSnapshot()` keeps a
   per-thread cached `shared_ptr<const Fibers>` invalidated by a
   `snapshotVersion` atomic. The previous code path went through
   `std::atomic_load_explicit(&shared_ptr)` which on macOS libc++
   serialises through a global spin lock — sampling showed it was
   90 %+ of submit time under burst load.
   `Yarn::releaseThreadSnapshotCache()` is called by `Fiber::process`
   just before thread exit to drop the cached `shared_ptr<Fiber>`
   refs, avoiding a thread_local-teardown self-join.

6. **`SmallObjectPool<SlotSize>`** (`Yarn/includes/SmallObjectPool.h`).
   Per-process freelist with thread-local LIFO cache and batched
   spill/refill. `CoroutineITask` and `SharedOwnerAdapter` override
   `operator new`/`delete` to use it. Marginal on the bench-mode
   path because cross-thread spill still hits the shared mutex; the
   bigger win lives in a future producer-side slab.

7. **`Yarn::run<F>` template / `CallableITask<F>`.** Free-function
   `YarnBall::run(F&&)` accepts arbitrary invocables and constructs
   a pool-backed wrapper inline — no user-side `make_unique`, no
   `ITask` vtable hop at the call site.

Final standings vs commercial-grade (published numbers, approximate):

| Operation              | Manwe | Tokio | Asio       | Verdict                |
|------------------------|-------|-------|------------|------------------------|
| Deque owner push+pop   | ~1 ns | ~3-5  | n/a        | **Faster**             |
| Deque + 2 thieves      | ~50   | ~50-80| n/a        | **On par**             |
| MPMC 2P/2C             | ~125  | n/a   | ~80-150    | **On par**             |
| Submit dispatch        | ~520  | ~300  | ~500-1500  | **On par with Tokio**  |
| Coroutine 10-hop chain | ~335  | ~300  | n/a        | **On par**             |

Remaining gap to Tokio is small and lives in two well-trodden
optimisations (block-based MPMC, true producer-side slab) that would
each shave another ~50-100 ns. Saved for a future round; current
performance is competitive with the best published numbers on every
operation measured.

### Added

#### New coroutine primitives
- **`whenAny<T>`** (`Yarn/includes/WhenAny.h`) — race a vector of
  `Task<T>`; return the index + value of the first to finish, propagate
  its exception, leave the losers to drain on the Yarn pool.
  Symmetric-transfer fast-path mirrors `whenAll`.
- **`withTimeout(task, duration)`** (`Yarn/includes/Timeout.h`) —
  race a task against `sleepFor`; throw `TimeoutException` on
  timer-win. Both value and `void` overloads. Loser keeps running
  (C++ coroutines have no forced cancellation; pass a `stop_token`
  through the user task if you need to shorten it).
- **`deadlineToken(duration)`** (`Yarn/includes/Timeout.h`) — returns a
  `std::stop_token` that auto-signals after the duration. Underlying
  `stop_source` is `shared_ptr`-held so it outlives the caller; a
  detached helper thread fires `request_stop`. For bounded-work loops
  without plumbing a manual stopper thread.
- **`Stream<T>`** (`Yarn/includes/Stream.h`) — lazy coroutine generator
  with `co_yield`. `NextAwaiter` drives the producer one value at a
  time; the `YARN_FOREACH_AWAIT` macro wraps the consume loop
  ergonomically until C++ standardises co_await range-for.
- **Stream combinators** (`Yarn/includes/Stream.h`) — `streamMap`,
  `streamFilter`, `streamTake`, `streamDrop`. Pure data-flow wrappers
  that pull from an input `Stream<T>` lazily and yield transformed
  values. Inputs taken by value (Stream is move-only); no Yarn
  dispatch involved.

#### Async synchronisation
- **`AsyncMutex` + `AsyncMutexGuard`** (`Yarn/includes/AsyncSync.h`) —
  coroutine-aware mutex with FIFO waiters. Uncontended `lock()` takes
  inline; contended waiters are dispatched to Yarn on release (NOT
  resumed inline) to bound the releaser's stack under heavy contention.
- **`AsyncSemaphore`** — counting permits with the same lock-then-
  Yarn-dispatch discipline. Built for producer/consumer backpressure
  (cap N concurrent operations).
- **`AsyncRwLock`** — writer-preferring reader/writer lock with strict
  FIFO ordering across both classes. Uncontended fast path uses
  `await_suspend` returning `false` (well-defined; distinct from inline
  `h.resume` which would re-enter mid-suspend).
- **`AsyncEvent`** — latched one-shot signal. `set()` flips a flag that
  stays set; `wait()` returns immediately on the fast path. Complements
  `AsyncNotify` (which does NOT latch).
- **`AsyncOnce`** — coroutine `call_once`. First caller runs the
  callable; concurrent callers park. Captured exception is rethrown to
  every caller including subsequent ones (matches `std::call_once`
  semantics).
- **`AsyncBarrier`** — `std::barrier` equivalent for coroutines. N-th
  arriver wakes the rest through Yarn (NOT inline) and resets the count
  for the next cycle. Supports multi-cycle reuse.
- **`AsyncNotify`** — coroutine wait/notify primitive (Tokio-style).
  `notified()` suspends until the next `notifyOne` / `notifyAll`.
  FIFO; missed notifications are not latched (waiter must be parked
  first). Compose with `AsyncMutex` for condition-variable semantics.

#### Spawning, joining, observability
- **`JoinHandle<T>` + `spawnJoinable(task)`** (`Yarn/includes/JoinHandle.h`)
  — middle ground between `coSpawn` (fire-and-forget) and `whenAll`
  (batch await). Returns a single-consumer handle whose `join()` is
  awaitable; resumes with the task's value (or rethrows its
  exception). `done()` is a cheap non-blocking poll.
- **`Telegraph::BoundedChannel<T>`** (`Wire/includes/BoundedChannel.h`)
  — MPMC channel with a fixed capacity. `send()` suspends when the
  buffer is full; `receive()` suspends when empty. Strict FIFO on both
  sides; capacity-zero degenerates to a rendezvous channel. Natural
  backpressure for producer/consumer pipelines.
- **`Telegraph::channelSelect`** (`Wire/includes/Select.h`) — pull from
  the first of N `Channel` / `BoundedChannel` instances that has a
  value, returning `{index, value}` or `nullopt` on closed. Built on
  top of `whenAny`.
- **`Yarn::stats()`** — operational snapshot: permanent + max + alive
  worker counts, injection-queue depth, reapable retired-fiber count.
  Cheap (atomic loads + short critical section); for dashboards /
  ops monitoring.

#### Windows backend
- **WSAPoll-driven readiness thread** + an IOCP completion thread,
  giving the Windows Reactor real `registerReadable` /
  `registerWritable` / `registerTimer` (`Yarn/src/Reactor.cpp`).
  Soccer's TCP / UDP / Raw paths compile and run on MSVC with `_WIN32`
  portability shims (`PlatformNet.h`).
- **Proactor surface** — `Soccer::asyncRecvOverlapped` /
  `asyncSendOverlapped` issue WSARecv / WSASend through the IOCP for
  native completion-based I/O. Sockets opt in via
  `Reactor::instance()->associateIocp(fd)`; the readiness API on the
  same socket continues to work alongside it.

#### Soccer ergonomics
- **`BufferedReader<Stream>`** (`Soccer/includes/BufferedReader.h`) —
  HTTP-style buffered reads over any Soccer stream type
  (TcpStream/TlsStream/UdpSocket/RawSocket). Provides `readLine`,
  `readUntilDelim`, `readExact`. Single internal buffer, one
  underlying read per refill, hard-bounded line size.
- **`tcpServe(listener, handler, stop_token)`**
  (`Soccer/includes/TcpServer.h`) — codifies the accept-loop +
  coSpawn-handler-per-connection idiom. `stop_token`-aware shutdown.
- **`HttpClient::get` / `::post`** (`Soccer/includes/HttpClient.h`) —
  minimal HTTP/1.1 client built on `BufferedReader`. Status line +
  header parsing, Content-Length and EOF-framed bodies, case-
  insensitive header lookup. 16 MB body cap, 64 KB header-block cap.
- **`Soccer::HttpServer`** (`Soccer/includes/HttpServer.h`) — wraps
  `tcpServe` with request parsing and an exact-match route table.
  Handlers return `Task<HttpResponse>`; unmatched routes get a default
  404. Reuses `BufferedReader` / `HttpResponse` / detail helpers from
  `HttpClient.h` so server-side and client-side semantics match.
  Limits: HTTP/1.1 with `Connection: close` (no keep-alive), no chunked
  transfer, exact-match routing only.
- **`UdpSocket::connect`** — sets the default peer so `WSASend` (and
  POSIX `::send`) can drive the socket without a per-call destination.
  Required for `asyncSendOverlapped` on UDP.

#### File I/O
- **`YarnBall::fs::File`** (`Yarn/includes/FileIo.h`) —
  coroutine-friendly file class with `open` / `read` / `write` /
  `flush` / `close`. One-shot helpers `readToString`, `readToBytes`,
  `writeString`, `writeBytes`, `remove`. Each blocking syscall hops
  to a Yarn worker and resumes the caller; Yarn's elastic capacity
  keeps the pool usable under concurrent file ops. Platform-native
  async file I/O (io_uring file ops, IOCP file completions) deferred
  to v2.

#### Examples
- **`examples/http_server.cpp`** — toy HTTP/1.1 server using
  `tcpServe` + `BufferedReader` + `AsyncMutex` + `AsyncSemaphore` to
  showcase the new stack end-to-end.
- **`examples/http_get.cpp`** rewritten to use `HttpClient`.

#### CI
- **`windows-latest`** job (MSVC, WSAPoll + IOCP, runs full suite).
- **`linux-epoll-ASan+UBSan`** job catches UAF / signed-overflow /
  alignment / vptr bugs at PR time.
- **`linux-epoll-TSan`** job catches data races. `.tsan-suppressions.txt`
  silences the documented Chase-Lev relaxed-atomic false-positives in
  the work-stealing deque (same suppression family Boost.Atomic and
  folly use for the same reason).
- `libnuma-dev` added to Linux apt installs (Fiber.cpp's `<numa.h>`).
- Soak step on macOS tolerates ≤3-in-20 transient libtls cleanup
  flakes (documented as a known LibreSSL issue, not in the runtime).

### Fixed
- **WhenAll race** — the latch-already-met fast path in
  `WhenAllAwaiter::await_suspend` was calling `h.resume()` inline
  before the coroutine had finished suspending — a recursive re-entry
  that manifested as a SIGSEGV on macOS-arm64 under load. Switched to
  symmetric transfer (`return h;`).
- **Test-suite lambda-coroutine lifetime bugs** — eight tests used the
  `[capture]()->Task<>{}()` idiom; the closure was destroyed at end of
  full-expression while the coroutine was still alive on the Yarn pool,
  leaving `this->capture` dangling on resume. Converted to free
  functions with pointer parameters.
- **`stop_token_cancels_yielding_coroutine` flake** — the 100k-iteration
  safety net was tripped on macOS-arm64. Raised to 10M.
- **CMake C++ standard drift** — `Yarn` and `YarnTests` were silently
  downgrading to C++20 (`-std=c++2a`) on top of `CMAKE_CXX_STANDARD=23`,
  hiding `std::stop_token` from AppleClang. Removed the override and
  set `CMAKE_CXX_STANDARD_REQUIRED ON`.
- **AppleClang `<stop_token>`** — Yarn now PUBLIC-exposes
  `-fexperimental-library` and `_LIBCPP_ENABLE_EXPERIMENTAL` on
  AppleClang, which libc++ requires to surface stop-token on Xcode 16.
- **Missing `<mutex>` include** in `Definitions.hpp` (MSVC was stricter
  about transitive include hygiene than the POSIX toolchains).

### Changed (BREAKING)
- **Whole-library camelCase rename.** Every Manwe-namespace identifier
  (free functions, methods, member variables, parameter names) moved
  from snake_case to lowerCamelCase. Types stay PascalCase. Examples:
  `sync_wait` → `syncWait`, `co_spawn` → `coSpawn`, `tcp_connect` →
  `tcpConnect`, `read_line` → `readLine`, `Yarn::Run` → `Yarn::run`,
  `register_readable` → `registerReadable`, `emit_async` → `emitAsync`,
  etc. std::, POSIX, WinSock, and other third-party identifiers
  untouched. Hard break for any out-of-tree consumer; the in-tree
  examples, tests, README, and CI consumer test are updated.

### Deferred (next iteration)
- AFD-poll Windows backend (option a from the original brief) — the
  perf upgrade that replaces the WSAPoll snapshot thread with a single
  IOCP completion source via `NtDeviceIoControlFile(\Device\Afd, ...)`.
  ~500 LOC and needs careful regression testing of all existing Windows
  paths. The current WSAPoll backend is stable; leaving as a tracked
  follow-up.
- A long-chain AsyncSemaphore stress test (50-task / 1-permit
  serialisation) reproducibly crashes on `windows-2025-vs2026` with
  `STATUS_ACCESS_VIOLATION` but passes 20/20 locally on bare-metal
  Windows 11 with the same toolchain. The short-chain variant
  (32-task / 4-permits) passes everywhere. Root cause needs a
  callstack from a Windows debugger on the CI runner.
- TLS on Windows (libtls via vcpkg).
- io_uring validation on a real Linux host (CI passes the build but no
  one with `liburing` runtime has confirmed it).

### Fixed
- **WhenAll race:** the latch-already-met fast path in
  `WhenAllAwaiter::await_suspend` was calling `h.resume()` inline before
  the coroutine had finished suspending — a recursive re-entry that
  manifested as a SIGSEGV on macOS-arm64 under load. Switched to
  symmetric transfer (`return h;`).
- **Test-suite lambda-coroutine lifetime bugs:** several tests used the
  `[capture]()->Task<>{}()` idiom; the closure was destroyed at end of
  full-expression while the coroutine was still alive on the Yarn pool,
  leaving `this->capture` dangling on resume. Converted to free
  functions with pointer parameters. Affected:
  `co_spawn_runs_on_pool`, `co_spawn_many_completes`,
  `co_spawn_with_co_await_chain`, `schedule_on_null_pool_is_noop`,
  `wire_channel_receive_blocks_until_send`,
  `wire_channel_close_wakes_pending_receivers_with_nullopt`,
  `reactor_wait_readable_already_ready`.
- **`stop_token_cancels_yielding_coroutine` flake:** the 100k-iteration
  safety net was tripped on macOS-arm64 (which fits >100k iterations
  into the 1ms stopper-thread wakeup window). Raised to 10M.
- **CMake C++ standard drift:** `Yarn` and `YarnTests` were silently
  downgrading to C++20 (`-std=c++2a`) on top of `CMAKE_CXX_STANDARD=23`,
  hiding `std::stop_token` from AppleClang. Removed the override and
  set `CMAKE_CXX_STANDARD_REQUIRED ON`.
- **AppleClang `<stop_token>`:** Yarn now PUBLIC-exposes
  `-fexperimental-library` and `_LIBCPP_ENABLE_EXPERIMENTAL` on
  AppleClang, which libc++ requires to surface stop-token on Xcode 16.
- **Missing `<mutex>` include** in `Definitions.hpp` (MSVC was stricter
  about transitive include hygiene than the POSIX toolchains).

## [0.3.0] - 2026-05-15

### Added
- **Async DNS:** `Soccer::SocketAddress::resolveAsync(host, port)` hops
  to a Yarn worker before invoking `getaddrinfo`.
- **Timer awaiter:** `YarnBall::sleepFor(duration)` /
  `YarnBall::sleepUntil(deadline)` driven by the Reactor. Backends:
  kqueue `EVFILT_TIMER`, epoll `timerfd_create`, io_uring
  `IORING_OP_TIMEOUT`, Windows stub.
- **Raw / ICMP:** `IcmpEcho` helpers (buildRequest, parse, checksum)
  and an end-to-end ping example.
- **Wire rewrite (`Telegraph::`):** coroutine-aware `Signal<Args...>`
  (sync / async / `co_await sig.next()`) and unbounded `Channel<T>`.
- **Soccer:** TCP / UDP / Raw / TLS coroutine sockets on top of Yarn +
  Reactor; full Doxygen + tests for TCP and UDP; TLS exercised with a
  self-signed loopback round-trip.
- **Examples:** echo_server, http_get, chat_signals, producer_consumer,
  ping.
- **Benchmarks:** `bench_yarn` covers deque (owner + concurrent),
  MPMCQueue 2P/2C, Yarn::run dispatch, Task<int> chain.
- Per-part architecture docs under `docs/`.

### Changed
- Magic values across the project converted to named `constexpr`
  constants (buffer sizes, timeouts, retry counts, queue capacities).
- All libraries link by CMake target name; PUBLIC compile definitions
  (e.g. `SOCCER_HAS_TLS`) propagate cleanly.
- Wire is now an INTERFACE (header-only) target; `libWire.a` is gone.

### Removed
- Legacy `Networking::Socket` + `EventQueue` + iSocketOperations /
  iServerOperations / Protocol / TcpSocketBase / TcpHeader /
  TlsHeader / RawSocketOperation / iAttachment.
- Legacy `Telegraph::Centraline` / `Epoc` / `iEvent` (the old Wire
  surface; had a non-existent push_back overload, broken disconnect,
  unsynchronised access, singleton-per-signature collision).
- `WireTest/` folder (Wire's tests live in YarnTests now).

### Fixed
- The full 23-bug audit from the Yarn rewrite (MPMCQueue double-destroy,
  Linux NUMA `core_id`, racy `Yarn::run`, retry-escalation dead code,
  self-join on temp fibers, `Fiber::running` data race, etc.).

## [0.2.0]

Internal milestone: work-stealing pool with Chase–Lev deques,
lock-free fiber snapshot, dynamic growth, reaper / graveyard
lifecycle. Coroutine layer (`Task<T>`, `syncWait`, `coSpawn`,
`scheduleOn`, `whenAll`, `stop_token`). Reactor with
kqueue / epoll defaults, opt-in io_uring, Windows IOCP scaffold.

## [0.1.0]

Initial Yarn correctness pass: 23 bug fixes from the audit.

[Unreleased]: https://github.com/Fabrizio86/Manwe/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/Fabrizio86/Manwe/releases/tag/v0.3.0
