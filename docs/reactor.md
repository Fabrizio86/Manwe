# Reactor — async I/O

The Reactor turns OS-level readiness notifications into coroutine
resumptions on the Yarn pool. It is the bridge between blocking I/O
syscalls and Manwe's coroutine layer.

---

## Architecture

### Singleton with one event-loop thread

The Reactor is a process-wide singleton. Its constructor forces
`Yarn::instance()` to be created first, which guarantees Yarn outlives
the Reactor in static destruction order (reverse construction order).
This matters because the Reactor's shutdown path keeps scheduling
resumptions onto Yarn until its event loop thread is joined.

One dedicated thread runs the event loop. It is woken either by a real
I/O event or by a synthetic wake event triggered from `stop()`.

### Backends

| OS | Backend | Default | Wake mechanism |
|---|---|---|---|
| macOS / *BSD | `kqueue` | yes | `EVFILT_USER` |
| Linux | `epoll` + `eventfd` | yes | write to eventfd |
| Linux | `io_uring` + `eventfd` | opt-in via `YARN_USE_IO_URING` | write to eventfd registered via `IORING_OP_POLL_ADD` |
| Windows | IOCP scaffold | partial (readiness throws) | `PostQueuedCompletionStatus` |

All registration calls are **one-shot**: after the event fires, the kernel
auto-disarms it. If the same fd is awaited again, the awaiter re-registers.

The coroutine handle is stored as the per-event user data field (`udata`
on kqueue, `data.ptr` on epoll, the SQE's `user_data` on io_uring), so
completion lookup is O(1) — no map.

#### io_uring opt-in (Linux)

Build with `-DYARN_USE_IO_URING=ON` to use `io_uring` instead of `epoll`.
Requires `liburing-dev` (or equivalent) on the host. The implementation
uses `IORING_OP_POLL_ADD` to get readiness semantics matching the existing
public API; a future round will add true async ops
(`IORING_OP_READ` / `IORING_OP_WRITE`) for a proactor model with the same
awaiter shape.

For maximum throughput, additionally pass `-DYARN_IO_URING_SQPOLL=ON` —
the kernel polls the submission queue, removing the per-submit syscall.
On kernels older than 5.11 this requires `CAP_SYS_NICE`.

io_uring submission is single-threaded by design; Yarn serialises SQE
acquisition with an internal mutex. The cost is one mutex per
`register_*` call (uncontended in normal flow, since most awaiters fire
from the reactor thread itself).

> **Validation note.** The io_uring backend has been compiled but
> not exercised on a Linux host from this checkout. The kqueue path
> is what the test suite covers; io_uring callers should run the
> reactor section of the suite against the target kernel before
> production use.

#### Windows IOCP

The Reactor on Windows runs two surfaces side by side:

- A **WSAPoll readiness thread** that backs `waitReadable` /
  `waitWritable` and the high-level Soccer awaiters (`TcpStream::read`,
  `accept`, …). Same coroutine shape as kqueue/epoll.
- A **native IOCP proactor surface** — `Soccer::asyncRecvOverlapped`
  and `asyncSendOverlapped`. Caller associates a socket
  (`Reactor::instance()->associateIocp(fd)`) and the awaiter issues
  `WSARecv` / `WSASend` with an `OVERLAPPED` whose completion lands on
  the IOCP loop, which resumes the coroutine on a Yarn worker.

For zero-copy receive/send on Windows the IOCP surface is what you
want; for "give me a coroutine API that matches POSIX," the WSAPoll
surface is the drop-in.

### Wakeup on stop

`stop()` flips the `running` flag and triggers the synthetic wakeup
event. The event loop unblocks from `kevent` / `epoll_wait`, sees
`running == false`, and exits. The destructor then joins the thread.

### Resumption protocol

When a registered event fires, the loop calls `schedule(handle)`:

```cpp
void Reactor::schedule(coroutine_handle<> h) noexcept {
    std::unique_ptr<ITask> ct{new detail::CoroutineITask(h)};
    Yarn::instance()->run(std::move(ct));
}
```

The resumption lands on a Yarn worker, not on the reactor thread. This
isolates user code from the event loop and lets the loop get back to
polling immediately.

The `CoroutineITask` adapter never destroys the handle — frame ownership
is the coroutine's own concern (detached self-destroys, otherwise some
Task wrapper higher up the chain holds it).

### Failure handling

If a kernel registration call fails (`kevent` / `epoll_ctl` returning -1),
the Reactor immediately schedules the handle on Yarn anyway. The resumed
coroutine then makes the actual `read`/`write` syscall, which surfaces the
real error via `errno`. The point is: we never leak a coroutine on
registration failure.

### I/O awaiters

`IoAwaiters.h` exposes the awaiter facade users interact with:

- `waitReadable(fd)` / `waitWritable(fd)` — suspend until ready.
- `asyncRead(fd, buf, len)` — suspends, then issues one `::read`.
- `asyncWrite(fd, buf, len)` — loops with EAGAIN-driven suspend until
  every byte is out (or an error).

These are **readiness-based**: the awaiter suspends on `EVFILT_READ` /
`EPOLLIN`, then resumes on a worker, and the worker performs the actual
syscall. This matches kqueue and epoll semantics natively. A future
io_uring backend would offer a true proactor model (kernel does the
syscall, awaiter just collects bytes) — the public awaiter shape will
stay the same.

---

## Using it

### Wait for readability and read manually

```cpp
Task<ssize_t> read_one(int fd, char* buf, size_t len) {
    co_await YarnBall::io::waitReadable(fd);
    co_return ::read(fd, buf, len);
}
```

### Use the high-level helpers

```cpp
Task<std::string> read_message(int fd) {
    char buf[1024];
    ssize_t n = co_await YarnBall::io::asyncRead(fd, buf, sizeof(buf));
    if (n < 0) co_return "";
    co_return std::string(buf, static_cast<size_t>(n));
}

Task<void> echo(int fd) {
    auto msg = co_await read_message(fd);
    co_await YarnBall::io::asyncWrite(fd, msg.data(), msg.size());
    co_return;
}
```

### Spawning a per-connection coroutine

```cpp
// On accept:
int client = ::accept(listener_fd, nullptr, nullptr);
::fcntl(client, F_SETFL, O_NONBLOCK);

YarnBall::coSpawn(echo(client));   // detached, will self-clean
```

### Stopping the reactor

The reactor is shut down automatically at process exit. To stop it
explicitly (rare):

```cpp
YarnBall::Reactor::instance()->stop();
```

After `stop()`, registration calls still no-op safely; the event loop
thread exits on its next wakeup.

---

## Invariants & gotchas

- **One coroutine per fd at a time.** Both kqueue and epoll backends use
  one-shot semantics with the coroutine handle as `udata`. Registering a
  second coroutine while the first is pending overwrites the handle. If
  you need many readers on one fd, multiplex them above the awaiter
  layer.
- **Always set fds non-blocking.** The reactor reports readiness, but
  spurious wake-ups happen (especially under load). A blocking syscall
  after readiness can stall a worker. Use `fcntl(fd, F_SETFL, O_NONBLOCK)`.
- **Resumption is on a worker, never on the reactor thread.** Don't put
  expensive work in the reactor thread; you can't.
- **Coroutine handle must outlive its registration.** That means a `Task`
  wrapper somewhere up the chain — or the detached flag (set by
  `coSpawn`) — has to keep the frame alive until the event fires.
  Anything that destroys the handle prematurely will crash on resumption.
- **macOS affinity is a hint.** The Mach `THREAD_AFFINITY_POLICY` tag is
  a hint, not a pin; the kernel may still migrate workers. That's fine
  for Manwe's purposes (cache-friendly grouping, not strict locality).
