# Observability — metrics, structured logs, distributed traces

Manwe ships three observability primitives as part of `Yarn`:

| Component                | Header                | Namespace             |
|--------------------------|-----------------------|-----------------------|
| Metrics (Prometheus)     | `Yarn/includes/Metrics.h` | `YarnBall::metrics` |
| Structured JSON logs     | `Yarn/includes/Log.h`     | `YarnBall::log`     |
| W3C trace propagation    | `Yarn/includes/Trace.h`   | `YarnBall::trace`   |

All three are pay-as-you-go: the hot path of each is one atomic
operation or one level check, and registration only happens at
startup.

---

## Metrics — Prometheus exposition

Three primitives, one process-wide registry, scrape over HTTP.

### Counter — monotonic 64-bit

```cpp
using namespace YarnBall::metrics;

static auto& requests = Registry::instance().counter(
    "http_requests_total", "Total HTTP requests served");

requests.inc();          // one relaxed atomic fetch-add on the hot path
requests.inc(42);
```

### Gauge — signed double, up or down

```cpp
static auto& inflight = Registry::instance().gauge(
    "http_inflight_requests", "Currently in-flight requests");

inflight.inc();
co_await handle(req);
inflight.dec();
```

### Histogram — bucketed distribution

```cpp
static auto& latency = Registry::instance().histogram(
    "http_request_latency_ns",
    {1e3, 1e4, 1e5, 1e6, 1e7, 1e8},   // 1 µs … 100 ms bucket boundaries
    "Per-request handler latency");

auto _ = scopeTimer(latency);   // RAII; records elapsed ns on scope exit
co_await handle(req);
```

### Scrape

```cpp
srv.route("GET", "/metrics", [](HttpRequest) -> Task<HttpResponse> {
    co_return HttpResponse{
        .status = 200,
        .headers = {{"content-type", "text/plain; version=0.0.4"}},
        .body = Registry::instance().scrapeProm(),
    };
});
```

`scrapeProm()` returns the Prometheus text exposition format — drop
it behind a `/metrics` route and any Prometheus / OpenTelemetry
collector picks it up unchanged. Labels are not in v1 (a metric is
uniquely identified by name); add labelled flavours when a real
consumer needs them.

---

## Structured logs — one JSON line per emit

```cpp
using namespace YarnBall::log;

setMinLevel(Level::Info);   // optional; default is Info

info("request handled",
     { str("method", req.method),
       str("path", req.path),
       i64("status", resp.status),
       i64("latency_ns", latencyNs),
       b("cache_hit", false) });
```

Output (one line per record, default sink is stderr):

```json
{"ts":"2026-05-16T12:34:56.789Z","level":"info","msg":"request handled","method":"GET","path":"/v1/items/42","status":200,"latency_ns":314,"cache_hit":false}
```

### Sinks

`setSink` swaps the writer — push to a file, a Loki/Fluentd UDP
socket, an in-memory ring for tests, etc.:

```cpp
setSink([](std::string_view line) {
    static std::ofstream f("/var/log/manwe.json", std::ios::app);
    f.write(line.data(), line.size());
});
```

The default sink is `fwrite(stderr)` under a mutex — fine for the
typical container-stdout pipe, but you're expected to swap it out
when you ship a fleet.

### Hot-path cost

A level check + early return when below the minimum is the
fast-skip. Above the threshold: one stringified line + one sink call
under a mutex. **Log emit is not a microsecond-budget operation.**
Don't log on the inner loop.

### Field helpers

| Helper                          | JSON shape  |
|---------------------------------|-------------|
| `str(key, string_view value)`   | `"value"`   |
| `i64(key, int64_t value)`       | `42`        |
| `f64(key, double value)`        | `3.14`      |
| `b(key, bool value)`            | `true`      |

Add custom field types by writing a `Field` with a JSON-ready
`formatted` string.

---

## Distributed traces — W3C `traceparent`

Manwe's trace layer implements the W3C `traceparent` header
(`00-{32-hex traceId}-{16-hex spanId}-{2-hex flags}`).

### Five verbs

| Verb                                           | Purpose                                |
|------------------------------------------------|----------------------------------------|
| `trace::newRoot()`                             | Fresh trace at the edge of the system. |
| `trace::newChild(parent)`                      | Same `traceId`, new `spanId`.          |
| `trace::parseTraceparent(header)`              | Ingest an incoming header.             |
| `trace::toTraceparent(ctx)`                    | Emit for an outgoing request.          |
| `trace::Scope(ctx)`                            | RAII install on the thread-local.      |

### Server-side ingestion

```cpp
srv.route("GET", "/v1/things",
    [](HttpRequest req) -> Task<HttpResponse> {
        auto ctx = trace::parseTraceparent(req.header("traceparent"));
        if (ctx.empty()) ctx = trace::newRoot();
        co_await trace::installCurrent(ctx);
        // … handler body — `co_await trace::currentAsync()` works
        //   even across resumes on a different worker …
        co_return HttpResponse{ .status = 200 };
    });
```

### Client-side propagation

```cpp
auto ctx = co_await trace::currentAsync();
auto child = trace::newChild(ctx);
auto resp = co_await Http2Connection::connect(...)
    .request("GET", "/upstream",
             {{"traceparent", trace::toTraceparent(child)}}, "");
```

### Promise-carrier vs thread-local

`trace::Scope` installs on the **thread-local** — fine for synchronous
blocks and for non-coroutine code, but lost when a coroutine
resumes on a different worker.

`trace::installCurrent` / `trace::currentAsync` are **coroutine
awaiters** that read/write the trace context **on the promise frame**,
so it survives suspend/resume across workers. Inside a `Task<T>`,
prefer them.

Both coexist; inside a Task, the promise carrier wins on read.

---

## Putting it together

The standard production handler pattern:

```cpp
Task<HttpResponse> getItem(HttpRequest req) {
    static auto& reqs    = metrics::Registry::instance().counter("http_requests_total");
    static auto& latency = metrics::Registry::instance().histogram(
        "http_request_latency_ns", {1e3, 1e4, 1e5, 1e6, 1e7, 1e8});

    auto t = metrics::scopeTimer(latency);
    reqs.inc();

    auto traceCtx = trace::parseTraceparent(req.header("traceparent"));
    if (traceCtx.empty()) traceCtx = trace::newRoot();
    co_await trace::installCurrent(traceCtx);

    auto row = co_await db.fetch(req.path);

    log::info("item fetched", {
        log::str("path", req.path),
        log::str("trace_id", trace::hexTraceId(traceCtx)),
        log::i64("row_bytes", static_cast<std::int64_t>(row.size())),
    });

    co_return HttpResponse{ .status = 200, .body = std::move(row) };
}
```

One handler. One counter. One histogram. One trace context that
survives a database `co_await` resuming on a different worker. One
structured log line that ships to your log aggregator with the trace
id pre-filled so you can pivot to the trace view by clicking it.
