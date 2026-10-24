# Wire — signals and channels

Wire is Manwe's push-style notification layer. It exposes two primitives:

- **`Signal<Args...>`** — typed multicast notification. Multiple handlers
  subscribe to one signal; emit broadcasts to all of them. Sync, async,
  and coroutine-awaitable variants.
- **`Channel<T>`** — unbounded value channel. Senders push, coroutine
  receivers pull. Close wakes pending receivers with `nullopt`.

Both are header-only and built on top of the Yarn coroutine + pool
runtime.

---

## Architecture

### `Signal<Args...>`

Per-instance signal hub (not a singleton). Internally holds:

- A list of `(SlotId, Handler)` pairs for connected handlers. Emissions
  work from a snapshot of it; `connect` / `disconnect` edit it in place
  when no emission holds a snapshot and copy it otherwise, so registering
  thousands of handlers stays cheap.
- A list of one-shot `NextSlot` waiters for `co_await signal.next()`
  consumers.
- A mutex guarding both lists; the lock is dropped before invoking
  any handler so a handler can `connect` / `disconnect` without deadlock.

Three dispatch paths:

| API | Where handlers run | Where `next()` waiters resume |
|---|---|---|
| `emit(args...)` | calling thread, in registration order | Yarn worker |
| `emitAsync(args...)` | each handler on a Yarn worker (separate task) | Yarn worker |
| `co_await sig.next()` | n/a — produces a tuple of args on resume | Yarn worker |

`next()` is a single-fire awaitable: each `co_await` registers one slot,
the next emit fires it and removes it. To keep receiving, await again.
Destroying the signal wakes parked `next()` waiters with
`CancelledException`, and a `next()` awaited after that throws it at once.

Handler exceptions: `emit` runs every handler and rethrows the first
exception once the last has run; `emitAsync` reports a throwing handler
through `YarnBall::setUnhandledExceptionHandler` and does not affect the
others.

`connectScoped(handler)` returns a move-only `ScopedConnection` that
disconnects the handler when it is destroyed (it may outlive the signal).
Neither it nor `disconnect` waits for a call already in flight, so a handler
that captures state by reference must not rely on disconnect alone to make
that state safe to destroy. A removed handler is destroyed after the
signal's lock is released, so its captures may call back into the signal.

### `Channel<T>`

Unbounded MPMC value channel:

- **`send(T)`** is **synchronous**. Either the value goes straight to a
  waiting receiver (the channel keeps a pending-receiver list), or it's
  buffered. Returns `false` if the channel was closed.
- **`receive()`** is a `Task<std::optional<T>>` coroutine. If a value is
  already buffered, it returns immediately. Otherwise the coroutine
  parks on the channel's waiter list until a sender hands one off or
  `close()` is called (returns `nullopt`).
- **`close()`** wakes every pending receiver with `nullopt` and marks
  the channel closed; subsequent `send` calls return `false`.
- **Soft cap.** `Channel(maxBuffered)` makes `send` return `false`
  instead of buffering past the cap (it never blocks); zero, the default,
  is unbounded. For producers that must be slowed down, use
  `BoundedChannel`.
- **`pushFront(T)`** puts a value back at the head of the channel (to the
  oldest parked receiver if there is one). It ignores the cap and is
  accepted by a closed channel, which still drains. `channelSelect` uses it
  to return a value a losing receive obtained.
- **`receiveFor(duration)`** is `receive()` with a deadline: it throws
  `YarnBall::TimeoutException` if nothing arrived in time, and never loses
  a value (see below).
- A parked `receive()` is cancellable: inside a stopped cancellation scope
  it leaves the waiter queue and throws `CancelledException`.

The hand-off path skips the buffer when a receiver is already waiting,
avoiding a buffer push + pop pair. Wake-ups schedule the parked
coroutine via `Yarn::run` (no nested resume from inside the sender).

---

## Using it

### Connect + emit

```cpp
Telegraph::Signal<int, std::string> events;

auto id = events.connect([](int code, std::string msg) {
    std::cout << "[" << code << "] " << msg << "\n";
});

events.emit(200, "ok");
events.emit(500, "boom");
events.disconnect(id);
```

### Async dispatch

```cpp
events.connect([](int, std::string m) {
    do_expensive_thing(m);  // runs on a Yarn worker, not the emitter
});
events.emitAsync(404, "not found");
// Emit returns immediately; the handler may still be running.
```

### Coroutine awaitable

```cpp
YarnBall::Task<void> wait_for_event(Telegraph::Signal<int, std::string>* sig) {
    auto [code, msg] = co_await sig->next();
    std::cout << "saw " << code << " " << msg << "\n";
    co_return;
}

YarnBall::coSpawn(wait_for_event(&events));
events.emit(1, "wake up");
```

### Channel producer / consumer

```cpp
auto channel = std::make_shared<Telegraph::Channel<int>>();

// Producer (any thread)
channel->send(1);
channel->send(2);
channel->close();

// Consumer coroutine
YarnBall::coSpawn([channel]() -> YarnBall::Task<void> {
    while (true) {
        auto v = co_await channel->receive();
        if (!v) break;
        std::cout << *v << "\n";
    }
    co_return;
}());
```

---

## Invariants & gotchas

- **`Signal` requires copyable `Args...`.** Every handler / `next()`
  waiter gets an independent copy.
- **Resumption never happens on the caller's lock.** When `emit` fires
  pending `next()` waiters, it does so via `Yarn::run`. There is no
  re-entrant resume from inside the signal's mutex.
- **`Channel::send` is synchronous and lock-light.** It locks briefly
  to buffer or to grab a pending receiver, then unlocks before
  scheduling the resume.
- **Once a `Channel` is closed it stays closed.** No reopen API; create
  a new instance.
- **`next()` only fires once per await.** Awaiters not yet
  parked when an emit arrives miss it; use a connected handler if you
  need every emit.

---

## `BoundedChannel<T>` — fixed-capacity backpressure

Same API shape as `Channel<T>` (`send` / `receive` / `close`), but with
a hard capacity. When the buffer is full, `send` is a **coroutine that
suspends** until a receiver drains a slot, instead of buffering
unbounded. This is the right primitive for any producer/consumer that
can outpace its consumer — log shippers, request fan-out queues, the
inbox between a network reader and a worker pool.

```cpp
auto ch = std::make_shared<Telegraph::BoundedChannel<int>>(/*capacity=*/256);

// Producer (any coroutine)
co_await ch->send(42);   // suspends if buffer is full

// Consumer
auto v = co_await ch->receive();
if (!v) /* closed */;
```

Same `close()` semantics: pending senders observe `false`, pending
receivers observe `nullopt`.

Timed variants: `sendFor(value, duration)` and `receiveFor(duration)`
throw `YarnBall::TimeoutException` when the deadline passes first. A
`sendFor` is exactly-once: it either delivered (returns `true`, or `false`
if the channel closed) or timed out having delivered nothing, in which case
the value is dropped (copy it first to retry).

## Timeouts: `receiveFor` / `sendFor`, not `withTimeout`

`YarnBall::withTimeout(ch->receive(), d)` races the receive against a
timer and discards the loser's result. If a sender hands the receive a
value at the instant the timer wins, that value is dropped; around
`BoundedChannel::send` the mirror case delivers the message while the
caller sees a timeout, so a retry duplicates it. Use `receiveFor` /
`sendFor`: they cancel the parked call (it leaves the queue without taking
or leaving anything) and a value handed over at the same instant is
returned, never dropped.

```cpp
try {
    auto v = co_await ch->receiveFor(500ms);   // nullopt: closed and drained
} catch (const YarnBall::TimeoutException &) {
    // nothing arrived; the channel is untouched
}
```

A task returned by `send` / `receive` reads the channel when it first runs,
so await it while the channel is alive. If moving a `T` throws while a value
is being handed over, the call that was moving it throws and the waiter it
was serving stays queued.

## `channelSelect` — race N channels

Pull from the first of N channels (mixed `Channel` and `BoundedChannel`
overloads) to have a value available. Returns a `SelectResult<T>`
carrying the winning index and the value:

```cpp
auto r = co_await Telegraph::channelSelect(std::vector{eventsA, eventsB, eventsC});
if (r.value) std::cout << "channel " << r.index << " produced " << *r.value << "\n";
else         std::cout << "every channel is closed and drained\n";
```

- `r.value` is an `std::optional<T>`: empty only when **every** channel is
  closed and drained. A channel that is closed and empty just drops out of
  the race while others are open or still hold data; to notice that one
  source ended, check `closed()` yourself.
- The losers are **cancelled**, not left parked: each loser's pending
  receive leaves its channel's waiter queue. A loser that was handed a
  value in the same instant puts it back at the **front** of its channel
  (`pushFront`), and the select returns only once every put-back is done,
  so no value is lost or reordered between consecutive selects. A fresh
  `channelSelect` starts a fresh race.
- When several channels are ready at once the start position rotates, so no
  channel is favoured by its position in the vector.
- If the caller is inside a stopped cancellation scope the select throws
  `CancelledException`.

---

## Rewrite history

The current Wire replaced an earlier `Centraline<Args...>` /
`Epoc` API. The rewrite addressed:

- A non-instantiable template in `Centraline::connect` (used a
  non-existent overload of `std::vector::push_back`).
- A `disconnect` that compared `std::function` to a raw function
  pointer (always false), leaving the slot list to grow without
  bound.
- Unsynchronised access — concurrent `connect` and `emit` raced.
- No coroutine integration.
- A per-signature singleton that caused two unrelated modules with
  matching argument signatures to share state.

The current `Signal<Args...>` is per-instance, locked, coroutine-
aware, and produces localised compile errors. `Channel<T>` replaces
`Epoc` for the producer/consumer pattern.
