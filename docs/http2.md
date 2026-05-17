# HTTP/2 — client, server, pool

Manwe ships HTTP/2 as a coroutine wrapper around **nghttp2** (the BSD-
licensed reference implementation used by curl, Apache, nginx, Envoy,
and gRPC). Three public surfaces: client (`Http2Connection`), server
(`Http2Server`), and a multiplexed connection pool
(`Http2ConnectionPool`).

The server-side `route(method, path, handler)` signature is identical
to `HttpServer` — a handler written for HTTP/1.1 ports to HTTP/2
verbatim.

End-to-end verified against `nghttp2.org` over TLS, against an
in-process loopback (`Http2Server` ↔ `Http2Connection`, two multiplexed
streams), and against a gRPC-style trailers round-trip.

---

## Build

CMake auto-detects nghttp2 (`brew install nghttp2`, `apt install
libnghttp2-dev`, or vendor it). When it's present, the implementation
links in; when it isn't, every entry point throws `Http2NotImplemented`
so caller code still compiles.

The same header (`Soccer/includes/Http2.h`) covers both modes —
production code can `try`/`catch` the stub and fall back to HTTP/1.1
without conditional compilation.

---

## Client — `Http2Connection`

### TLS

```cpp
#include "Soccer/includes/Http2.h"

using namespace Soccer;

Soccer::TlsClientOptions tls;
tls.alpnProtocols = "h2";            // appended if missing
tls.caBundleFile  = "/etc/ssl/cert.pem";

auto conn = co_await Http2Connection::connect("api.example.com", 443, tls);

// Many requests in flight on the same connection -- each is a stream.
auto a = co_await conn.request("GET",  "/v1/items/42", {}, "");
auto b = co_await conn.request("POST", "/v1/items",
                                {{"content-type", "application/json"}},
                                R"({"id":7})");
```

### Plain (h2c) — for cluster-internal traffic behind a TLS-terminating
proxy:

```cpp
auto conn = co_await Http2Connection::connectPlain("svc.internal", 8080);
auto resp = co_await conn.request("GET", "/health", {}, "");
```

### Response

`Http2Response` is a type alias for `HttpResponse` — the same shape
your HTTP/1.1 code already uses. The `:status` pseudo-header is decoded
into `resp.status`; the rest of the pseudo-headers are dropped from the
response. Post-response trailers (e.g. gRPC's `grpc-status` /
`grpc-message`) land in `resp.trailers`.

```cpp
if (resp.status != 200) throw std::runtime_error("upstream " + std::to_string(resp.status));
auto contentType = resp.header("content-type");   // case-insensitive
for (const auto& [k, v] : resp.trailers) { /* ... */ }
```

### Lifetime

Move-only. The destructor sends `GOAWAY` and closes the socket. A
reader-driver coroutine started at construction runs until the
connection is destroyed or the peer GOAWAYs. `close()` is idempotent.

### Concurrency

`request` may be called from any coroutine on any worker. An internal
session mutex serialises nghttp2 API calls; only the awaiter's resume
suspends on the wire response.

---

## Server — `Http2Server`

```cpp
Http2Server srv("0.0.0.0", 8080);

srv.route("GET", "/hello",
    [](HttpRequest req) -> Task<HttpResponse> {
        co_return HttpResponse{ .status = 200, .body = "hi\n" };
    });

srv.route("POST", "/echo",
    [](HttpRequest req) -> Task<HttpResponse> {
        co_return HttpResponse{
            .status = 200,
            .headers = {{"content-type", "application/octet-stream"}},
            .body = std::move(req.body),
        };
    });

co_await srv.serve(/*stop = */ stopToken);
```

Multiplexed: every accepted connection runs an nghttp2 server session
that hosts N concurrent streams; each request dispatches to its
handler as a separate coroutine, and the response is shipped back on
the same stream.

### Trailers (gRPC)

Setting `HttpResponse::trailers` on the returned response emits them
as a HEADERS frame after the body, with `END_STREAM` on the trailer
frame rather than on the last DATA chunk:

```cpp
srv.route("POST", "/grpc.Service/Method",
    [](HttpRequest req) -> Task<HttpResponse> {
        HttpResponse r;
        r.status = 200;
        r.headers = {{"content-type", "application/grpc"}};
        r.body    = serializeProtoFrame(/* … */);
        r.trailers = {{"grpc-status", "0"}, {"grpc-message", "OK"}};
        co_return r;
    });
```

Implementation routes the trailer through nghttp2's data-provider EOF
callback, which guarantees body frames serialise before the trailer.

### h2c only (currently)

v1 ships h2c (cleartext) on the server side. h2-over-TLS server-side
reuses the same Pipe abstraction as the client and is a planned
follow-up round.

---

## Multiplexed connection pool — `Http2ConnectionPool`

The HTTP/1.1 pool is "idle-then-claim". The HTTP/2 pool is "one
long-lived connection per backend, shared across all concurrent
requests" — which is the whole point of multiplexing.

```cpp
auto& pool = Http2ConnectionPool::defaultPool();

auto conn = co_await pool.acquire("api.example.com", 443, tls);
auto r1 = co_await conn->request("GET", "/v1/a", {}, "");
auto r2 = co_await conn->request("GET", "/v1/b", {}, "");

// On transport error, evict so the next acquire reopens.
pool.evict("api.example.com", 443);
```

The key is `(host, port [, hash(tls-opts)])`. `acquirePlain` is the
non-TLS variant. The pool returns a `shared_ptr<Http2Connection>` —
the connection lives until the last reference goes away or the pool
evicts it.

---

## Capabilities

| Capability                          | Status |
|-------------------------------------|--------|
| Client h2c + h2-over-TLS            | shipping |
| Server h2c                          | shipping |
| Multiplexed streams (one conn, N concurrent) | shipping |
| Multiplexed pool keyed by host:port  | shipping |
| ALPN negotiation (`h2`)             | shipping (auto-appended to TLS opts) |
| HPACK header compression            | shipping (delegated to nghttp2) |
| Per-stream / per-conn flow control  | shipping (nghttp2 default) |
| HTTP/2 trailers (gRPC)              | shipping |
| HEADERS / DATA / SETTINGS / PING / GOAWAY / RST_STREAM / WINDOW_UPDATE / PRIORITY / CONTINUATION frames | shipping (nghttp2) |
| mTLS (client cert)                  | API surface in (`TlsClientOptions::clientCertFile`) |
| Server h2 over TLS                  | follow-up round |
| Server push                         | one `nghttp2_submit_push_promise` call away — no consumer yet |
| Streaming response body (Task<Stream<bytes>>) | follow-up round |
| `h2spec` conformance run in CI       | follow-up round (nghttp2 itself is h2spec-conformant) |

---

## What this unlocks

- **gRPC.** gRPC is HTTP/2-only. With h2 + trailers in, the gRPC story
  is live (server returns `grpc-status` / `grpc-message` as trailers,
  client surfaces them on `resp.trailers`).
- **Service-mesh deployments.** Envoy / Istio / Linkerd default to
  HTTP/2 between sidecars. Manwe slots in as the backend without
  forcing the mesh to downgrade.
- **Multiplex over one TCP connection.** A single connection per
  backend carries many concurrent requests, vs HTTP/1.1's
  connection-per-request keep-alive. Cuts handshake cost to one
  per process.

---

## Why nghttp2 and not an inline implementation?

The HTTP/2 wire format is ~3,000 lines of careful state-machine code
(framing, HPACK, flow control, stream state, connection state, ALPN).
There are roughly fifty published CVE classes against HTTP/2
implementations; nghttp2 already carries the patches and the
conformance pass against `h2spec`. Re-implementing inline would buy
zero external dependency at the cost of owning the security backlog of
a wire protocol that turnkey libraries already solve.

The wrapper is ~600 lines on top of nghttp2's callback model; the
performance budget for the wrapper is dominated by the syscall cost
of TLS reads, not by callback indirection.
