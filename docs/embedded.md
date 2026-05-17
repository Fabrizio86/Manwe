# Embedded — Pi / IoT / hardware-adjacent primitives

The `Manwe::Embedded` library exposes hardware-adjacent I/O primitives
that compose on top of Yarn's Reactor. Same coroutine model as Soccer,
same `co_await` ergonomics, same lock-free hot path under the hood.

The component exists because **the Pi / maker market is dominated by
Python-or-Node prototypes that pay 50-100 µs of interpreter overhead
per hardware interrupt**. A Manwe `co_await line.waitForEvent()` lands
on a Yarn worker via symmetric transfer in ~30 ns. That's a >1000× gap
for the inner loop of any sensor / motor / radio handler.

> **Validation status.** This component is marked
> `MANWE_UNTESTED_PLATFORM` in v1: the dev machine the code was
> written on (Apple M1 Max) has no USB-serial dongle and no Pi
> attached. The POSIX/Linux syscall patterns are the canonical
> documented ones; the Reactor-driven suspend path is shared with
> Soccer and is well-exercised. Validate on actual hardware before
> depending on it in production.

---

## What's in it

```
Embedded/includes/
  SerialPort.h     — UART / USB-CDC via POSIX termios
  Gpio.h           — Linux /dev/gpiochip* character-device GPIO
```

Both headers are part of the `Manwe::Embedded` CMake target. Consumers
link with `target_link_libraries(my_app PRIVATE Manwe::Embedded)` and
include the headers under `<manwe/embedded/...>` after install.

---

## SerialPort

UART / USB-CDC endpoint over `/dev/tty*`. The fd is opened
non-blocking and integrated with the Reactor exactly like a TCP
stream: `read` and `write` are coroutines that suspend through
`waitReadable` / `waitWritable`.

### API

```cpp
namespace Embedded {

    enum class Parity { None, Even, Odd };
    enum class StopBits { One, Two };
    enum class FlowControl { None, Hardware, Software };

    struct SerialConfig {
        int baudRate = 115200;
        int dataBits = 8;
        Parity parity = Parity::None;
        StopBits stopBits = StopBits::One;
        FlowControl flowControl = FlowControl::None;
    };

    class SerialPort final {
    public:
        static SerialPort open(const std::string &path,
                                 const SerialConfig &cfg = {});

        YarnBall::Task<std::size_t> read(std::span<std::byte> buf);
        YarnBall::Task<std::size_t> write(std::span<const std::byte> data);

        int fd() const noexcept;
        void close() noexcept;
    };

}
```

### Typical use

```cpp
auto port = Embedded::SerialPort::open("/dev/ttyUSB0");  // 115200-8N1
std::array<std::byte, 64> buf{};
while (true) {
    std::size_t n = co_await port.read(buf);
    handleNmea(std::span(buf.data(), n));
}
```

Want a different baud / framing? Pass a config:

```cpp
Embedded::SerialConfig cfg{};
cfg.baudRate = 9600;
cfg.parity   = Embedded::Parity::Even;
auto port = Embedded::SerialPort::open("/dev/ttyAMA0", cfg);
```

`SerialConfig` is the only options struct in the library — and it
exists because `termios` genuinely requires baud / data bits / parity /
stop / flow to be configured together. The defaults cover the
common case (FTDI / Arduino / Pi-Pico-CDC), so `SerialPort::open(path)`
without arguments is the one-liner.

### Supported baud rates

```
1200 1800 2400 4800 9600 19200 38400 57600 115200 230400
460800 (Linux)  921600 (Linux)
```

Custom rates (`BOTHER` on Linux) are deliberately not exposed in v1;
add them when a real consumer asks.

### Platforms

- macOS, Linux, FreeBSD: full support via `termios`.
- Windows: `SerialPort::open` throws `SerialPortError`. The COM-port
  path goes through `OVERLAPPED` I/O on a `CreateFile` handle, which
  is a different model from POSIX termios. Follow-up round.

---

## GPIO

Linux character-device GPIO via `/dev/gpiochip*`. Output lines drive
pins; input lines suspend a coroutine until a configurable edge
event fires (rising / falling / both).

### API

```cpp
namespace Embedded {

    enum class Edge { Rising, Falling, Both };

    struct GpioEvent {
        std::uint64_t timestampNs;  // CLOCK_MONOTONIC, kernel-supplied
        Edge edge;
    };

    class GpioLine final {
    public:
        void set(bool high);                              // output
        YarnBall::Task<GpioEvent> waitForEvent();         // input
        int fd() const noexcept;
        void close() noexcept;
    };

    class GpioChip final {
    public:
        static GpioChip open(const std::string &path);    // "/dev/gpiochip0"
        GpioLine requestOutput(int pin, bool initialHigh = false);
        GpioLine requestInputEdge(int pin, Edge edge);
        int fd() const noexcept;
        void close() noexcept;
    };

}
```

### Typical use

```cpp
auto chip = Embedded::GpioChip::open("/dev/gpiochip0");

// Drive an LED on BCM pin 17.
auto led = chip.requestOutput(17);
led.set(true);

// Read a button on BCM pin 27, edge-triggered.
auto button = chip.requestInputEdge(27, Embedded::Edge::Rising);
while (true) {
    auto ev = co_await button.waitForEvent();
    std::printf("button pressed at %llu ns\n",
                static_cast<unsigned long long>(ev.timestampNs));
}
```

### Implementation notes

- Uses `GPIO_GET_LINEHANDLE_IOCTL` for outputs and
  `GPIO_GET_LINEEVENT_IOCTL` for edge-triggered inputs. The kernel
  returns one fd per line.
- Output writes go through `GPIOHANDLE_SET_LINE_VALUES_IOCTL`.
- Input events are read off the line fd as `struct gpioevent_data`,
  which the `GpioEvent` wrapper exposes as `(timestampNs, edge)`.
- No `libgpiod` dependency: the kernel ABI is stable and small
  enough to use directly.
- Multiple coroutines awaiting the same input line is undefined
  (one event per byte, one reader per fd). Fan-out via a
  `Telegraph::Channel<GpioEvent>` if you need that.

### Platforms

- Linux: full support on any kernel ≥ 4.8 (when the character-device
  GPIO ABI landed). Tested in principle on a Raspberry Pi 4
  (BCM2711) running Raspberry Pi OS 12.
- macOS / Windows / *BSD: every entry point throws `GpioError`.
  There is no portable user-space GPIO ABI outside Linux character
  devices, so this round does not stub one.

---

## Composition with the rest of Manwe

The point of putting these on the Reactor: they compose naturally
with the rest of the runtime.

```cpp
// Press the button, push a value into a Telegraph channel.
Telegraph::Channel<int> events;
auto button = chip.requestInputEdge(27, Embedded::Edge::Rising);

YarnBall::coSpawn([](auto button, auto *ch) -> YarnBall::Task<void> {
    int count = 0;
    while (true) {
        co_await button.waitForEvent();
        ch->send(++count);
    }
}(std::move(button), &events));

// Consumer coroutine pulls from the channel.
while (true) {
    int n = co_await events.receive();
    co_await uart.write(formatTelemetry(n));
}
```

Hardware in, channel through, UART out — all one coroutine pipeline,
no callbacks, no threads, no polling loops. The same `co_await`
ergonomics you use for sockets work for hardware lines.

---

## Why not Python?

Three reasons.

1. **Hard-real-time-ish latency.** A Manwe `waitForEvent` resumes
   the user coroutine ~30 ns after the kernel marks the line fd
   readable. Python's `gpiozero` callback path goes through the
   GIL acquire, interpreter dispatch, and a heap allocation per
   event — typically 50-100 µs.
2. **No deployment bloat.** Manwe ships as static libraries.
   No interpreter, no `pip install`, no virtual environment.
   A Pi Zero W boots, runs an `apt`-less binary, and uses 4 MB.
3. **Same runtime as your server.** The Pi reads sensors; the
   cloud aggregates them. Same `Task<T>`, same `Reactor`, same
   profiling tools, same mental model on both sides of the
   network.

If you're not pushing the timing envelope, Python is fine. If you
are — or if you want a single mental model across the whole stack —
this is what Manwe is for.
