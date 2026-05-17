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

- A list of `(SlotId, Handler)` pairs for connected handlers.
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

## `channelSelect` — race N channels

Pull from the first of N channels (mixed `Channel` and `BoundedChannel`
overloads) to have a value available. Returns a `SelectResult<T>`
carrying the winning index and value:

```cpp
auto r = co_await Telegraph::channelSelect(std::vector{eventsA, eventsB, eventsC});
std::cout << "channel " << r.index << " produced " << r.value << "\n";
```

The losers stay parked on their original channels — calling
`channelSelect` again resumes the race from a fresh set of waiters.

---

## Why not the previous Wire?

The original Wire was `Centraline<Args...>` (singleton-per-signature
signal) plus `Epoc` (string-keyed event registry). It had several
issues that the rewrite addresses:

- `Centraline::connect`'s body used a non-existent overload of
  `std::vector::push_back` — the template wouldn't compile when actually
  instantiated.
- `disconnect` compared `std::function` to a raw function pointer, which
  always returns false; the slot list grew without bound.
- All access was unsynchronised; concurrent `connect` + `emit` was a
  data race.
- No coroutine integration.
- Per-signature singleton meant two unrelated modules sharing a
  parameter signature collided unintentionally.

The new `Signal<Args...>` is per-instance, locked, coroutine-aware, and
its compile-time errors are localised. `Channel<T>` replaces `Epoc` for
the producer/consumer pattern (which is what most `Epoc` users actually
wanted under a fancier name).
