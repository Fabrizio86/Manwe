# Soccer — coroutine networking

Soccer is the networking layer of Manwe. It exposes coroutine-based TCP,
UDP, raw IP, and (optionally) TLS sockets, all driven by the same
`Yarn::Reactor` that powers the rest of the async stack. There are no
callbacks and no dedicated event-loop threads — every read, write,
accept, and handshake is a `co_await` that suspends on kernel readiness
and resumes on a Yarn worker.

---

## Architecture

### Layering

```
  Soccer::TcpListener / TcpStream / UdpSocket / RawSocket / Tls{Listener,Stream}
                              |
                              v
  YarnBall::io::waitReadable / waitWritable / asyncRead / asyncWrite
                              |
                              v
  YarnBall::Reactor  (kqueue / epoll / io_uring / IOCP)
                              |
                              v
  YarnBall::Yarn  (work-stealing pool, coroutine resumption)
```

Soccer doesn't open its own event loop, doesn't own threads, and doesn't
hold a Yarn singleton. Every awaiting syscall registers the current
coroutine handle with the Reactor; the Reactor resumes the handle via
`Yarn::run` when the fd is ready.

### Ownership model

- Every socket type is **move-only** and owns one fd. The destructor
  closes the fd; `close()` is idempotent and safe to call multiple times.
- `release()` (on `TcpStream`) hands the fd off to a higher-level wrapper
  (e.g. `TlsStream` after a server-side handshake). The source is left
  empty (`fd == -1`) so its destructor does not double-close.
- Listeners own their listening fd; each `accept()` produces an
  independently-owned `TcpStream`.

### Non-blocking everywhere

Every fd Soccer creates is set non-blocking immediately after creation.
The syscall loops follow the same pattern:

```cpp
while (true) {
    ssize_t n = syscall(...);
    if (n >= 0) co_return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        co_await io::waitReadable(fd);  // or waitWritable
        continue;
    }
    throw SocketException(errno, "syscall name");
}
```

`EINTR` is retried inside the inner `do/while`. This means user code
never sees `EINTR`, never sees `EAGAIN`, and either gets a result or an
exception.

### Connect semantics

`tcpConnect` (and `TlsStream::connect`) initiates a non-blocking
`::connect`. If the kernel returns `EINPROGRESS`, the coroutine suspends
on writability of the socket, then reads `SO_ERROR` to surface the real
result of the connection attempt.

### TLS path

When `SOCCER_HAS_TLS` is set (libtls / LibreSSL present on the host):

- `TlsStream::connect` resolves, opens a non-blocking socket, performs
  the TCP handshake (`EINPROGRESS` + writable-wait), then drives
  `tls_handshake` to completion.
- `TlsListener::bind` reads cert + key, configures a server context.
  `TlsListener::accept` accepts a TCP connection via the bundled
  `TcpListener`, hands the fd to `tls_accept_socket`, drives the server
  handshake, and finally adopts the fd (via `TcpStream::release`) into a
  fully-formed `TlsStream`.
- All libtls calls use the **public** API — no struct-internal access.
  Both `tls_read`/`tls_write` and `tls_handshake` are driven by
  `TLS_WANT_POLLIN` / `TLS_WANT_POLLOUT` returns, with the corresponding
  `waitReadable` / `waitWritable` awaiter.

### Raw / ICMP

`RawSocket::open(family, ip_protocol)` is the generic constructor;
`RawSocket::icmp()` is sugar for `IPPROTO_ICMP`. Privilege requirements
are the kernel's (CAP_NET_RAW on Linux, root on macOS for most
protocols), not Soccer's.

For ICMP Echo specifically, Soccer ships stateless helpers in
`IcmpEcho.h`:

- `IcmpEcho::buildRequest(identifier, sequence, payload)` returns a
  ready-to-`sendto` packet (header + payload) with the RFC 1071
  checksum filled in.
- `IcmpEcho::parse(bytes, skip_ip=true)` reads a received reply,
  optionally skipping the IPv4 header that `SOCK_RAW` exposes,
  validates the checksum, and returns identifier / sequence / payload.
- `IcmpEcho::checksum(span)` is exposed for callers building custom
  ICMP types.

There's a worked example at `examples/ping.cpp` showing the full loop.
Unit tests cover the checksum, build/parse round-trip, and short-buffer
rejection. An integration ping-loopback test runs only when the
process is root (and skips quietly otherwise).

### WebSocket (RFC 6455)

`WsConnection` lives on top of `TcpStream` and implements the
WebSocket protocol with one verb per operation:

- `WsConnection::serverHandshake(TcpStream)` consumes the upgrade
  request, validates `Upgrade`, `Connection`, `Sec-WebSocket-Version`,
  computes `Sec-WebSocket-Accept` via inline SHA-1 + base64, and writes
  the 101 response. Any bytes the reader over-consumed past
  `\r\n\r\n` are captured in `preBuf` and drained on the next frame
  read — this matters when a server packs its 101 response and the
  first data frame into one TCP segment.
- `WsConnection::connect(host, port, path)` is the client mirror.
- `receive()` returns the next data message. **Multi-frame messages
  are reassembled** per RFC 6455 §5.4: a leading TEXT/BINARY frame
  with FIN=0 plus zero or more CONTINUATION frames with FIN=0 ending
  at FIN=1. Control frames (Ping/Pong/Close) may interleave between
  fragments. Ping is answered with Pong internally; Close is echoed
  and marks the connection closed.
- `sendText(string_view)` / `sendBinary(span<byte>)` emit one frame
  each. Client side masks outgoing per spec.
- `sendFrame(FragmentKind, payload, isFinal)` is the advanced API for
  streaming a large payload across multiple frames without
  materialising it in memory:

  ```cpp
  co_await ws.sendFrame(FragmentKind::Text,         first,  false);
  co_await ws.sendFrame(FragmentKind::Continuation, middle, false);
  co_await ws.sendFrame(FragmentKind::Continuation, last,   true);
  ```

- `close(code, reason)` is idempotent; subsequent operations throw.

Out of scope in v1: extensions (no `permessage-deflate`), no
subprotocols, 16 MiB hard cap on data-frame payload. SHA-1 and base64
are implemented inline in `Soccer/src/WebSocket.cpp`, so the
WebSocket subsystem adds no external crypto dependency beyond the
optional `libtls` already used for TLS.

### Unix domain sockets

`TcpListener::bindUnix(path)` + `tcpConnectUnix(path)` reuse the
existing TCP accept/read/write coroutines with `AF_UNIX` SOCK_STREAM
under the hood. Identical await API; the only differences are:

- A filesystem path instead of (host, port).
- `bindUnix` unlinks an existing socket file before binding (routine
  cleanup of stale post-crash leftovers; regular files / directories
  at that path are left alone so `bind` surfaces the canonical
  EADDRINUSE).
- `sun_path` overflow (> 104–108 bytes depending on platform) throws
  rather than silently truncating.

Available on POSIX and Windows 10 1803+. On Windows the family
supports stream sockets only.

### Multicast UDP

`UdpSocket` ships five setsockopt-backed helpers:

- `joinGroup(addr, ifaceAddr = "")` / `leaveGroup(addr, ifaceAddr = "")`
  — IPv4 and IPv6 group membership, family auto-detected from the
  literal. Optional interface address scopes the join to one NIC
  (mandatory for multi-NIC Pi boards and for macOS loopback).
- `setMulticastTtl(int)` — hop limit for outgoing multicast.
- `setMulticastLoop(bool)` — whether the socket should observe its
  own sends on local loopback.
- `setMulticastInterface(ipv4)` — outgoing NIC selection. Pi
  use: pick `eth0` vs `wlan0` by their IP. macOS loopback test
  use: pass `"127.0.0.1"`.

Per-interface IPv6 selection is by interface index rather than
address and is not exposed; users who need it drop to `fd()` +
`IPV6_MULTICAST_IF` directly.

---

## Using it

### TCP echo server

```cpp
YarnBall::Task<void> handle_client(Soccer::TcpStream client) {
    std::array<std::byte, 4096> buf{};
    while (true) {
        std::size_t n = co_await client.read(buf);
        if (n == 0) break;  // peer closed
        co_await client.write(std::span<const std::byte>(buf.data(), n));
    }
}

YarnBall::Task<void> echo_server(std::uint16_t port) {
    auto listener = Soccer::TcpListener::bind("0.0.0.0", port);
    while (true) {
        auto client = co_await listener.accept();
        YarnBall::coSpawn(handle_client(std::move(client)));
    }
}

int main() {
    YarnBall::syncWait(echo_server(8080));
}
```

### TCP client

```cpp
YarnBall::Task<std::string> fetch(std::string host, std::uint16_t port) {
    auto stream = co_await Soccer::tcpConnect(host, port);
    std::string req = "GET / HTTP/1.0\r\nHost: " + host + "\r\n\r\n";
    co_await stream.write(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(req.data()), req.size()));

    std::string out;
    std::array<std::byte, 4096> buf{};
    while (true) {
        std::size_t n = co_await stream.read(buf);
        if (n == 0) break;
        out.append(reinterpret_cast<const char*>(buf.data()), n);
    }
    co_return out;
}
```

### UDP datagram

```cpp
YarnBall::Task<void> udp_server(std::uint16_t port) {
    auto sock = Soccer::UdpSocket::bind("0.0.0.0", port);
    std::array<std::byte, 1500> buf{};
    Soccer::SocketAddress sender;
    while (true) {
        std::size_t n = co_await sock.recvFrom(buf, &sender);
        // Echo back to the sender.
        co_await sock.sendTo(std::span<const std::byte>(buf.data(), n), sender);
    }
}
```

### TLS client (libtls required)

```cpp
#ifdef SOCCER_HAS_TLS
YarnBall::Task<void> https_get(std::string host) {
    auto stream = co_await Soccer::TlsStream::connect(host, 443);
    std::string req = "GET / HTTP/1.0\r\nHost: " + host + "\r\n\r\n";
    co_await stream.write(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(req.data()), req.size()));
    // ...read response...
}
#endif
```

### TLS server (libtls required)

```cpp
#ifdef SOCCER_HAS_TLS
YarnBall::Task<void> https_server(std::uint16_t port,
                                  std::string cert, std::string key) {
    auto listener = Soccer::TlsListener::bind("0.0.0.0", port, cert, key);
    while (true) {
        auto client = co_await listener.accept();
        YarnBall::coSpawn(handle_tls_client(std::move(client)));
    }
}
#endif
```

---

## Invariants & gotchas

- **All sockets are non-blocking.** You will never see a blocking syscall
  inside Soccer. The kernel reports readiness; Soccer translates that
  into a coroutine resumption.
- **Resumption lands on a Yarn worker.** When a socket is ready, the
  Reactor schedules the coroutine on the pool, not on its own loop
  thread. Heavy user logic does not stall the reactor.
- **DNS is synchronous.** `SocketAddress::resolve` calls `getaddrinfo` on
  the calling thread. For non-blocking lookup, wrap the call site in
  `co_await scheduleOn(YarnBall::Yarn::instance())` so the resolution
  runs on a worker.
- **Listening sockets bind synchronously.** `TcpListener::bind` and
  `UdpSocket::bind` are not coroutines — they execute one-shot syscalls
  that return quickly.
- **The Reactor's one-shot watches assume one coroutine per fd at a
  time.** Don't have two coroutines awaiting readability on the same fd
  simultaneously; either serialize or multiplex above this layer.
- **TLS uses the public libtls API only.** No struct-internal access
  (the previous version's `TlsHeader.h` reached into `struct tls`; that
  is gone).
- **Raw sockets require kernel privileges.** Expect EPERM without
  CAP_NET_RAW on Linux or root on macOS.
- **Windows is supported** — Soccer compiles + runs on MSVC. The
  default path goes through the Reactor's WSAPoll readiness thread;
  the opt-in proactor surface (`asyncRecvOverlapped` /
  `asyncSendOverlapped`) routes through IOCP after the caller has
  `Reactor::instance()->associateIocp(stream.fd())`.

---

## Higher-level helpers

### `BufferedReader<Stream>`

Wraps any Soccer stream (`TcpStream`, `TlsStream`, `UdpSocket`,
`RawSocket`) to add line- and delimiter-bounded reads on top of the
fixed-byte primitives. Uses a single contiguous buffer with read /
write cursors; one underlying read per refill; hard-bounded line size
(64 KiB) so an adversarial peer that never sends the delimiter cannot
force unbounded memory use.

```cpp
auto stream = co_await tcpConnect(host, port);
BufferedReader<TcpStream> r(&stream);

std::string greeting = co_await r.readLine();
auto fixed = co_await r.readExact(2048);   // std::vector<std::byte>
std::string headerBlock = co_await r.readUntilDelim(std::byte{'\0'});
```

### `tcpServe(listener, handler, stopToken = {})`

The accept-loop + per-connection `coSpawn` idiom in two lines:

```cpp
auto listener = TcpListener::bind("0.0.0.0", 8080);
co_await tcpServe(std::move(listener),
    [](TcpStream client) -> Task<void> {
        // handle one connection
        co_return;
    });
```

A passed `std::stop_token` causes the loop to return after the next
accept once `stopRequested()` becomes true. In-flight handlers are NOT
cancelled — pass the same token through if you want them to shorten.

### `HttpClient::get / post`

Minimal HTTP/1.1 client built on `BufferedReader`. Status line +
header parsing, both Content-Length and EOF-framed bodies, case-
insensitive header lookup, 16 MB body cap and 64 KB header-block cap
so an adversarial server cannot OOM the client.

```cpp
auto resp = co_await HttpClient::get("example.com", 80, "/");
if (resp.status == 200) {
    std::cout << resp.body;
}

// case-insensitive header lookup
auto ct = resp.header("content-type");
```

### Windows-native proactor — `asyncRecvOverlapped` / `asyncSendOverlapped`

For Windows-only zero-copy WSARecv / WSASend through the Reactor's
IOCP:

```cpp
auto stream = co_await tcpConnect(host, port);
Reactor::instance()->associateIocp(stream.fd());

std::array<std::byte, 4096> buf;
std::size_t n = co_await asyncRecvOverlapped(stream, buf);
```

UDP variant requires `UdpSocket::connect(peer)` first — `WSASend` on a
datagram socket has no destination parameter.
