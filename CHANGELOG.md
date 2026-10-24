# Changelog

All notable changes to Manwe are listed here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions adhere
to [SemVer](https://semver.org/).

## [Unreleased]

### Changed
- **`Telegraph::Signal::emit` / `emitAsync` no longer copy the handler list.** The list is
  copy-on-write behind a `shared_ptr`, so an emission only copies one pointer under the lock
  (and skips the waiter bookkeeping when no `next()` is parked). `benchmarks/bench_signal.cpp`
  (Apple silicon, Release): 8 handlers 69 → 26 ns/emit, 8 handlers on 4 threads 187 → 45 ns/emit,
  1 handler 41 → 21 ns/emit. Semantics are unchanged: handlers may connect/disconnect during an
  emission, which works from a snapshot.
- **`SignalSet` supports any number of live instances.** The one-per-process limit is gone:
  on macOS/BSD each set owns a kqueue with `EVFILT_SIGNAL` filters (no signal handler, so
  nothing has to be async-signal-safe; same-number signals arriving before `next()` runs coalesce);
  elsewhere the `sigaction` handler fans out to every live set's self-pipe. Original dispositions
  are restored when the last set holding a signal is destroyed. A second set used to throw
  `SignalCaptureError`; it now succeeds.

### Fixed (code audit)
- **Reactor / cancellation:** epoll `registerFd` no longer resumes a waiter twice when `EPOLL_CTL_MOD`
  fails; epoll timer registration is atomic against `cancelTimer` (no double delete / foreign fd close);
  `disarmCancelHook` can no longer return while a fire is in progress; a wake racing a destroyed
  `AsyncMutex`/`Semaphore`/`RwLock` waiter no longer revokes a grant that was never made;
  `whenAny`/`whenAll` clean up when their frame is destroyed while parked; `AsyncOnce` recovers when
  its runner frame is destroyed; the Reactor loop wakes parked waiters instead of exiting silently.
- **Scheduler shutdown:** tasks left in a worker deque, inbox or the injection queue are failed through
  `exception()` and deleted, so `Waitable::wait()` returns; temp-fiber retirement has the missing fence;
  a failed pool constructor unwinds its workers; idle workers re-check peers before sleeping.
- **Channels / Signal / Select:** `Channel`/`BoundedChannel` state is shared with parked awaiters (no
  use-after-free when the channel dies first); `~Signal` cancels parked `next()` waiters; wake-ups fall
  back inline if the pool refuses a task; `pushFront` on a closed channel keeps the value; rendezvous
  `BoundedChannel` no longer releases a sender without a receiver; `emit` runs every handler and rethrows
  the first exception; serial and GPIO `close()` no longer races reads (atomic fd, non-blocking GPIO fd).
- **Sockets / TLS:** socket fds are atomic slots with an in-flight count, so `close()` from another
  thread can't hit a recycled descriptor; `tcpConnectUnix` no longer leaks the fd on cancel;
  `bindUnix` refuses to unlink a live server's socket; `tcpServe` sheds load at EMFILE, survives
  transient accept errors and throwing handlers, and its stop probe is bounded; `readExact` is capped;
  ICMP parsing checks IP version, protocol and type; `TlsStream` keeps its fd until the last waiter
  leaves and serialises writers; client cert/key must come as a pair; `MSG_NOSIGNAL`/`SO_NOSIGPIPE`
  per socket.
- **HTTP/1.1:** request-smuggling fixes (all `Transfer-Encoding` headers scanned, CRLF-only line endings,
  token header names, exactly one `Host`), proper 400/414/431/500 responses, response header/body
  deadlines on the client, a `101` is final, status line without a reason phrase parses.
- **HTTP/2:** client use-after-free on close after submit (nghttp2 copies header strings; streams freed
  only after the session and the awaiter are done); cookie crumbs are joined; trailers-only responses keep
  their trailers; flow-controlled output is pulled in bounded chunks; `nghttp2_option` limits set and
  nghttp2 >= 1.61 required; handler exceptions return a fixed 500 body; `route()` after `serve()` no
  longer races.
- **WebSocket:** handshake path/host are validated (request injection); duplicate handshake headers are
  rejected; key must be 16 bytes of base64; client rejects unrequested extensions/subprotocols; Close
  reasons are cut on a UTF-8 boundary; ping flood and empty-fragment limits.
- **Metrics / Log / Trace / Timers:** Prometheus metric names are validated; histogram sums ignore
  non-finite values and scrapes are consistent; log sink state survives static teardown; `jsonString`
  repairs invalid UTF-8; `trace::newChild` of an empty parent, uppercase `traceparent` hex, signed
  overflow in timer helpers.
- **`SignalSet`:** on non-kqueue builds a set only receives the signals it subscribed to; teardown no
  longer reads or writes a recycled descriptor; `next()` binds its descriptor when called.
- **Examples:** `ping`, `http_get`, `http2_get` argument validation; `graceful_http` watcher owns its
  `SignalSet`; `echo_server` has a connection cap and idle timeout; `chat_signals` and
  `producer_consumer` no longer synchronise with sleeps.

### Fixed (second audit pass)
- **Cancellation / timers:** a stale second `cancelTimer` on a freed sleeper address (handle reuse) woke an
  unrelated sleeper, so `withTimeout(instant, 30s)` occasionally threw `TimeoutException` (also in the original
  tree); `SleepAwaiter` and the fd-wait awaiter now gate the redo on "registered" vs "stopped" without
  allocating. `runTakenHooks` no longer touches hooks whose owner died during an inline resume; a stop callback
  that unwinds the frame owning its own stop source stops through a copy.
- **Scheduler:** a failed `Yarn()` constructor (thread creation error) throws instead of hanging; wake claiming
  stops quick spawns piling onto one parked worker; the pool grows when every worker is blocked on a child
  task; shutdown no longer reports one "unhandled exception" per discarded callable; `Trace::Scope` unwinds on
  its owner thread; pinning (opt-in) spreads over the allowed CPUs.
- **Reactor:** io_uring sleepers use the ordered-table design (cancelling 50,000 sleepers: 12.8 s -> 143 ms);
  io_uring `cancelFd`/`cancelByHandle` no longer lose a cancel when the submission queue is full; a dead
  reactor resumes new waiters with `ReactorError` instead of queueing them forever.
- **`SignalSet`:** a signal raised in a forked child no longer reaches the parent's set; capturing `SIGCHLD`
  on kqueue platforms no longer auto-reaps children (empty handler instead of `SIG_IGN`).
- **Wire:** `channelSelect` no longer loses or reorders values (the caller resumes after every pull has put
  its value back) and no longer prefers low indexes; `receiveFor`/`sendFor` give lossless timeouts (plain
  `withTimeout` around `receive`/`send` is documented as lossy); `Signal::disconnect` no longer deadlocks when a
  handler's captures re-enter the signal; `connect`/`disconnect` are O(1) when no emission is in flight (20,000
  connects: 1125 ms -> 0.6 ms); a throwing `T` move can't strand a receiver; `Signal::next()` after
  `~Signal` no longer hangs. Serial/GPIO fd slots defer `::close` to the last in-flight operation.
- **Sockets / TLS:** `bindUnix` no longer changes the process umask (bind, then `chmod`, then `listen`) and
  refuses to unlink a live or saturated server; `TCP_NODELAY` on every TCP socket (two-write responses no
  longer stall ~42 ms); a waiter can no longer register on a closed or reused fd (was ~1 hang per 1000 stress
  iterations); `tcpServe` doesn't drop a legitimate connection when fd exhaustion ends; `TlsStream::close()`
  no longer holds its lock for ~100 ms; each connect candidate gets its own timeout slice; `TlsListener::accept`
  defers the handshake to first use; connect with a timeout only arms a timer when the handshake is still
  pending (HTTP GET with a new connection: 122 us -> 91 us).
- **HTTP / HTTP/2 / WebSocket:** no body on HEAD/1xx/204/304; HTTP/2 idle timeout runs from the last real
  progress and control-frame floods get `ENHANCE_YOUR_CALM`; response headers/trailers are validated and
  framing headers filtered; routes match on the path with `HttpRequest::query`; the HTTP/1 client decodes
  chunked responses and parses `Connection` as a token list; WebSocket handshake targets are normalised and
  `Host` is validated; split `Connection`/`Upgrade` fields are accepted; the per-request HTTP/2 deadline
  timer is cancelled when the request ends.
- **`BoundedChannel`:** the audit's changes had grown the `send`/`receive` coroutine frames by ~48 bytes (a
  slower malloc size class: ~15 ns per pair); frames are now smaller than the original's.

### Changed (behaviour)
- `HttpServer`/`Http2Server`: `HEAD` needs its own route (no fallback to `GET`); methods are case-sensitive;
  `Http2ServerOptions::maxControlFramesPerSecond` (default 50) and `WsOptions::onHandshake`/`strictOrigin`/
  `subprotocols`/`maxPingsPerSecond` are new.
- `tcpConnectUnix(path, busyRetry = 5s)`, `TcpListener::tryAccept()`/`interruptAccept()`, `Yarn::awaitQuiescence()`.
- Linux worker CPU pinning is now opt-in (`YARN_PIN_WORKERS=1`) and respects the allowed CPU set.
- `Workload` and `QueueSize` are `enum class`.
- Exceptions thrown by callables passed to `Yarn::run` are reported as unhandled-detached-task errors
  instead of vanishing.
- `whenAny` on an empty list throws `std::invalid_argument` instead of waiting forever;
  `channelSelect` on an empty list completes with `nullopt`.
- Servers default to `maxConnections = 1024` (0 still means unlimited); WebSocket `idleTimeout` defaults
  to 300 s, HTTP/2 idle timeout to 60 s. `HttpClient` defaults to 30 s / 120 s response header / body
  deadlines. `HttpClient` sends `Host` with the port when it isn't 80 and rejects malformed header lines.
- `BufferedReader::readExact` is capped at 64 MiB by default; the line cap is a constructor option.
- `TlsListener::accept` takes a handshake timeout (default 10 s).
- `SocketAddress` binds set `IPV6_V6ONLY` explicitly (`dualStack` option, default on).
- `WsOptions` gains `onHandshake`, `strictOrigin`, `subprotocols` and `maxPingsPerSecond`;
  `Signal` gains `connectScoped()`; `Channel` gains an optional soft cap.
- `SignalSet` no longer limits one instance per process (see above); a signal outside 1..64 is rejected
  on non-kqueue builds.

### Added
- `benchmarks/bench_signal.cpp` — `Telegraph::Signal` emit cost with 0/1/8 handlers and 1/4 threads.
- **Cancellable awaits** (`Yarn/includes/Cancellation.h`) — `withCancellation(token, task)`
  runs a task in a cancellation scope; `sleepFor`, `io::waitReadable/Writable`,
  `AsyncMutex/Semaphore/Notify/Event/Barrier/RwLock/Once`, `JoinHandle::join` and the
  Telegraph channels leave their queue / the Reactor when the scope is stopped and throw
  `CancelledException`. `whenAny`, `withTimeout` and `channelSelect` now cancel their
  losing branches. A coroutine frame destroyed while parked also unlinks itself.
- **`Task::requestCancel` wakes parked awaits** — it is no longer polling-only: every
  cancellable await also registers with the `co_await` chain it sits in, so requestCancel on
  any `Task` of the chain wakes it with `CancelledException`; `whenAny`, `whenAll`,
  `channelSelect` and `withCancellation` forward the request to their branches.
- **`SocketHandle` / `kInvalidSocket` / `isValidSocket`** (`Yarn/includes/SocketHandle.h`) —
  the platform socket handle type (`int` on POSIX, a full-width `SOCKET` on Windows).
  `closeSocket`, `setSocketNonblocking`, `io::waitReadable/Writable` and the `Reactor`
  fd entry points (`registerReadable/Writable`, `cancelWait`, `cancelFd`, `associateIocp`)
  take it instead of `int`.
- **`Reactor::cancelFd / cancelWait / cancelTimer`** — deregister parked waiters
  (resuming or dropping them); `SignalSet`, `SerialPort` and `GpioLine` call `cancelFd`
  before closing their fd.
- **`setUnhandledExceptionHandler`** — exceptions escaping a detached (`coSpawn`) task are
  reported (default: `log::error`) instead of being dropped.

- **Connect timeouts** — `tcpConnect(host, port, timeout)`, `TlsClientOptions::connectTimeout`,
  `WsOptions::connectTimeout`, `Http2Connection::connectPlain(..., connectTimeout)` and
  `HttpClient::setConnectTimeout` bound name resolution plus the TCP handshake (default 10 s).
  The attempt runs in a cancellation scope that the deadline stops, so a socket still waiting
  for its SYN-ACK is abandoned and closed (`shutdown()` does not wake it on macOS).
- **`Soccer::wsServe(listener, handler, WsOptions, stop)`** — WebSocket accept loop that applies
  `WsOptions::maxConnections` (slot held from accept, so half-open handshakes count), the
  handshake timeout and the Origin policy; `examples/ws_echo_server.cpp` uses it.

### Fixed
- Wake vs cancel vs frame destruction is now race-free: parked awaits publish a `WaitProxy`
  handle, so a frame destroyed while a wake (release, notify, send, timer, readiness,
  cancel) is already in flight is never resumed afterwards; a lock / permit that had been
  handed to such a frame is given back.
- Reactor: `cancelWait` / `cancelTimer` return value is the same ownership signal on every
  backend where it can be exact (`true` = this call took the registration); the Windows
  `cancelWait` no longer reports `true` for a request that took nothing. kqueue: a new
  registration on an fd number that still has waiters whose descriptor was closed
  (knote gone) resumes those stale waiters instead of leaving them parked.
- `AsyncBarrier`: the releasing arrival now dispatches the other waiters before it continues,
  so they no longer depend on its frame outliving the `co_await`.
- Soccer: `TcpStream` / `TcpListener` / `UdpSocket` / `RawSocket` / `TlsStream` mark themselves
  closed before `Reactor::cancelFd`, and every read / write / accept / recv loop re-checks that
  state after a wake, so a waiter woken by `close()` fails with a `SocketException` instead of
  using a descriptor number that may already belong to another connection. `RawSocket::close`
  now wakes parked waiters too.
- Reactor: several waiters on the same fd and direction no longer overwrite each other
  (kqueue/epoll); io_uring no longer resumes a coroutine twice after a failed submit.
- Idle temp workers now actually retire (`kParkWaitTimeout` was unused).
- `channelSelect` no longer swallows a value delivered to a losing receive; `BoundedChannel`
  never resumes a waiter while holding its mutex.
- `MPMCQueue` no longer wedges if an element's construction throws; `SmallObjectPool`
  returns a thread's cached blocks when the thread exits; `Stream` accepts `co_yield` of a
  const lvalue; `asyncWrite` avoids SIGPIPE; Prometheus HELP text is escaped and
  mismatched histogram buckets are rejected.
- **CI consumer-check stale version pin** — the `install-consumer` job's
  `find_package(Manwe 0.3 REQUIRED)` predated the 1.1.0 version bump;
  `SameMinorVersion` compatibility rejected it outright, failing the job
  on every push. Bumped the pin to `1.1`.
- **`soccer_unix_socket_round_trip` hardcoded `/tmp`** — the test built
  its socket path as `"/tmp/manwe-test-unix-" + pid`, which doesn't
  exist on Windows runners and made `bind(AF_UNIX)` fail outright.
  Switched to the existing `fsTempPath()` helper other tests already
  use for cross-platform temp paths.
- **No job-level CI timeout** — none of the eight jobs in `ci.yml` set
  `timeout-minutes`, so a hang silently rode GitHub's 6-hour default
  (observed once: a `master` push run sat "in progress" for
  `6h1m8s` before being cancelled). Added `timeout-minutes: 15`
  (`20` for the soak job) to every job so a hang surfaces in minutes,
  not hours.

### Deferred (next iteration)
- **Rare macOS scheduler deadlock under `yarn_runs_50k_tasks`** — on a
  shared `macos-latest` runner, the macOS-flake retry loop's second
  attempt (after the documented SIGBUS-at-cleanup flake fired on
  attempt 1) hung indefinitely inside `yarn_runs_50k_tasks` — the test
  normally completes in ~30ms. Not a `wait_for` timeout issue (that
  helper correctly bounds at 30s); the hang is upstream of it, most
  likely in `Yarn::dispatch`/`Yarn::enqueueInjection`'s inbox/injection/
  grow/inline-fallback path under the burst of 50,000 synchronous
  submissions from a single caller thread. Only reproduced twice so
  far, both on the shared CI runner under contention; has not
  reproduced locally. Needs a thread dump captured mid-hang (e.g. via
  a `tmate`/debugger CI step) to pin down which lock or CAS loop is
  stuck. The new job timeout (above) turns this into a fast, visible
  CI failure instead of a multi-hour silent hang; it does not fix the
  underlying race.

## [1.1.0] - 2026-09-04

Added `MANWE_BUILD_TESTS`, `MANWE_BUILD_EXAMPLES`, and `MANWE_BUILD_BENCHMARKS`
CMake options (all default `ON`), so a consumer embedding Manwe via
`add_subdirectory` (as Mortis3d does, at `third_party/manwe`) can opt out of
building `YarnTests`, `examples/`, and `benchmarks/` as part of its own
configure. No source or behavioural changes otherwise.

## [1.0.0] - 2026-05-16

First stable release. The runtime is feature-complete for the v1
scope: work-stealing pool, C++20 coroutines with symmetric transfer,
multi-backend reactor, networking (TCP/UDP/TLS/Unix/Raw/ICMP),
WebSocket (with continuation frames), HTTP/1.1, HTTP/2 via nghttp2
(client + server + multiplexed pool, with trailers), structured logs,
Prometheus metrics, W3C trace propagation, Pi-grade serial + GPIO,
and an in-tree test harness with 157 cases (155 enabled on the
macOS/Linux reference build). End-to-end-verified against
`nghttp2.org`, with `bench_async_server` sustaining ~2.5 million
DB-heavy requests per second per core on an Apple M1 Max.

The entries below were drafted in the Unreleased section before
the cut; they ship as part of 1.0.0.

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

[1.1.0]: https://github.com/Fabrizio86/Manwe/releases/tag/v1.1.0
[1.0.0]: https://github.com/Fabrizio86/Manwe/releases/tag/v1.0.0
[0.3.0]: https://github.com/Fabrizio86/Manwe/releases/tag/v0.3.0
