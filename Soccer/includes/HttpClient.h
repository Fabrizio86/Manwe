//
// Created by Fabrizio Paino on 2026-05-15.
//
// HttpClient: tiny HTTP/1.1 client built on Soccer::TcpStream +
// BufferedReader. Supports GET / POST, Content-Length-framed bodies,
// arbitrary request/response headers. NOT chunked transfer (would
// require a stateful body decoder; doable but kept out of v1 to keep
// the surface small). NO TLS yet -- pair with Soccer::TlsStream once
// TLS is wired on the target platform.
//
// Designed for example/test/scripting use, not as a production-grade
// HTTP stack. Headers are stored case-sensitively as a vector of
// (name, value) pairs; lookups are linear scans (fine for the small
// header sets typical of real responses).
//

#ifndef SOCCER_HTTP_CLIENT_H
#define SOCCER_HTTP_CLIENT_H

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "BufferedReader.h"
#include "Coroutines.h"
#include "PlatformNet.h"
#include "SocketException.h"
#include "TcpStream.h"

namespace Soccer {

    namespace detail {
        /**
         * @brief Maximum bytes accepted for a single response body. Cap
         *        prevents an adversarial peer from forcing unbounded
         *        memory use via a huge or never-ending Content-Length.
         */
        inline constexpr std::size_t kHttpMaxBodyBytes = 16 * 1024 * 1024;

        /**
         * @brief Maximum size of the response status line + header
         *        block combined. Protects against malformed responses
         *        that omit the blank-line terminator.
         */
        inline constexpr std::size_t kHttpMaxHeaderBytes = 64 * 1024;

        /**
         * @brief Maximum number of 1xx interim responses (e.g. a
         *        proactive "100 Continue") tolerated before the final
         *        response, per request. Real servers send at most a
         *        handful (typically one); without a cap, a malicious
         *        or broken peer sending an endless stream of otherwise-
         *        valid 1xx responses would keep HttpClient looping
         *        forever waiting for a final status that never comes.
         */
        inline constexpr int kHttpMaxInterimResponses = 8;

        /**
         * @brief Trim leading + trailing ASCII whitespace from @p s
         *        in place. Used to clean up header values after the
         *        @c name:value split.
         */
        inline std::string trimAscii(std::string s) {
            std::size_t b = 0;
            while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
            std::size_t e = s.size();
            while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
            return s.substr(b, e - b);
        }

        /**
         * @brief Case-insensitive ASCII equality. Header field names
         *        are case-insensitive per RFC 9110 5.1.
         */
        inline bool iequalsAscii(std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                const unsigned char ca = std::tolower(
                    static_cast<unsigned char>(a[i]));
                const unsigned char cb = std::tolower(
                    static_cast<unsigned char>(b[i]));
                if (ca != cb) return false;
            }
            return true;
        }

        /**
         * @brief True if @p s contains CR, LF or NUL, i.e. cannot be put
         *        into a start line or header field without enabling
         *        header injection / response splitting.
         */
        inline bool hasCtlLineBreak(std::string_view s) noexcept {
            return s.find_first_of(std::string_view("\r\n\0", 3)) != std::string_view::npos;
        }

        /**
         * @brief Strict Content-Length extraction. Returns false if the
         *        header is absent; throws if any occurrence is not a
         *        plain decimal number, if occurrences disagree, or if the
         *        value exceeds @p limit (default @ref kHttpMaxBodyBytes;
         *        zero means no limit beyond the 12-digit syntax bound).
         */
        template<typename HeaderVec>
        inline bool strictContentLength(const HeaderVec &headers, std::size_t &out,
                                        std::size_t limit = kHttpMaxBodyBytes) {
            bool found = false;
            std::size_t value = 0;
            for (const auto &h : headers) {
                if (!iequalsAscii(h.name, "Content-Length")) continue;
                const std::string &v = h.value;
                if (v.empty() || v.size() > 12 ||
                    v.find_first_not_of("0123456789") != std::string::npos) {
                    throw std::runtime_error("HTTP: invalid Content-Length");
                }
                const std::size_t n = static_cast<std::size_t>(std::stoull(v));
                if (found && n != value) {
                    throw std::runtime_error("HTTP: conflicting Content-Length headers");
                }
                value = n;
                found = true;
            }
            if (found && limit != 0 && value > limit) {
                throw std::runtime_error("HTTP: invalid Content-Length");
            }
            out = value;
            return found;
        }
    }


    /**
     * @brief One HTTP request header. Stored as a (name, value) pair.
     */
    struct HttpHeader {
        std::string name;
        std::string value;
    };


    /**
     * @class HttpResponse
     * @brief Parsed HTTP response: status code, optional reason phrase,
     *        header vector, body bytes.
     */
    struct HttpResponse {
        /// HTTP status (e.g. 200, 404). Defaults to 200, not 0: this
        /// struct is also what a server route handler constructs and
        /// returns (see HttpServer.h), and a handler that forgets to
        /// set status on a success response is a far more common
        /// mistake than one that means to send status 0. Parsing a
        /// real response (HttpClient's readResponse) always
        /// overwrites this from the wire, so the default only matters
        /// for a freshly-constructed HttpResponse.
        int status = 200;

        /// Reason phrase from the status line (e.g. "OK").
        std::string reason;

        /// All response headers, in wire order.
        std::vector<HttpHeader> headers;

        /// Raw response body. Empty if Content-Length was 0 or absent.
        std::string body;

        /**
         * @brief Trailers (HTTP/2 only). Sent by the server as a
         *        second HEADERS frame after the body, terminating
         *        the stream. The canonical use is gRPC's
         *        @c grpc-status / @c grpc-message trailers; HTTP/1.1
         *        and the older path-less servers ignore this field.
         *
         * On the server side, populate this in your route handler's
         * returned @c HttpResponse to have nghttp2 emit a trailer
         * frame. On the client side, this is populated by the h2
         * client wrapper from the post-body HEADERS frame.
         */
        std::vector<HttpHeader> trailers;

        /**
         * @brief Case-insensitive lookup of the first header with the
         *        given @p name. Returns empty string if not present.
         */
        std::string header(std::string_view name) const {
            for (const auto &h : this->headers) {
                if (detail::iequalsAscii(h.name, name)) return h.value;
            }
            return {};
        }
    };


    /**
     * @class HttpConnectionPool
     * @brief Per-process idle pool for HTTP/1.1 keep-alive
     *        connections, keyed by @c "host:port".
     *
     * Acquire returns either an existing idle stream (LRU-most-recent
     * within a host's deque) or moves @c std::nullopt -- in which
     * case the caller does a fresh @c tcpConnect. Release puts the
     * stream back if the per-host idle count is below
     * @ref maxIdlePerHost; otherwise the stream is closed.
     *
     * Streams returned to the pool MUST be at a message boundary
     * (one full response consumed) and MUST NOT have been signalled
     * @c Connection: close by the server. The HttpClient pooled
     * request path enforces both conditions.
     *
     * Lock-protected; the locking is per-pool, not per-host. For
     * very high QPS against many hosts this can be sharded, but
     * v1 keeps a single mutex.
     */
    class HttpConnectionPool final {
    public:
        /**
         * @brief Default cap on idle connections per @c host:port.
         *        Sized for typical web-tier deployments (16 in flight
         *        to any given backend, ~50ms request latency, 320
         *        rps/peer cap with no queueing).
         */
        static constexpr std::size_t kDefaultMaxIdlePerHost = 16;

        /**
         * @brief Process-wide default pool. Used by @c HttpClient's
         *        pooled request entry points.
         */
        static HttpConnectionPool &defaultPool();

        /**
         * @brief Adjust the per-host idle cap. Default
         *        @c kDefaultMaxIdlePerHost. Existing idle entries
         *        above the new cap are evicted lazily on next
         *        @c release.
         */
        void setMaxIdlePerHost(std::size_t n) noexcept;

        /**
         * @brief Try to obtain an existing idle connection to
         *        @p host:@p port. Returns an empty optional if none.
         *        Connections idle longer than the max idle age are discarded
         *        rather than returned. (This does not touch the socket; use
         *        @ref acquireLive to also skip connections the peer has closed.)
         */
        std::optional<TcpStream> acquire(std::string_view host, std::uint16_t port);

        /**
         * @brief Like @ref acquire, but also discards connections whose peer has
         *        closed or broken them in the meantime (a non-blocking peek finds
         *        EOF or an error), so a request is not sent into a dead socket.
         */
        std::optional<TcpStream> acquireLive(std::string_view host, std::uint16_t port);

        /**
         * @brief Return a healthy, at-message-boundary connection to
         *        the pool. The pool takes ownership; if the per-host
         *        limit is reached the stream is closed.
         */
        void release(std::string_view host, std::uint16_t port, TcpStream stream);

        /**
         * @return Current count of idle connections to @p host:@p port.
         *         Test/diagnostic helper; not a stable scrape source.
         */
        std::size_t idleCount(std::string_view host, std::uint16_t port) const;

        /**
         * @brief Default longest an idle connection may sit in the pool before
         *        it is considered stale. Servers close keep-alive connections
         *        after a few tens of seconds; reusing one past that point just
         *        fails the request.
         */
        static constexpr std::chrono::milliseconds kDefaultMaxIdleAge{30000};

        /**
         * @brief Set the longest an idle connection may wait in the pool. Older
         *        ones are dropped instead of handed out. Zero disables the age
         *        limit (the liveness check in @ref acquire still applies).
         */
        void setMaxIdleAge(std::chrono::milliseconds age) noexcept;

    private:
        struct IdleConn {
            TcpStream stream;
            std::chrono::steady_clock::time_point lastUsed;
        };

        static std::string keyOf(std::string_view host, std::uint16_t port) {
            std::string k(host);
            k.push_back(':');
            k.append(std::to_string(port));
            return k;
        }

        /// True if the peer has closed (or broken) @p s, or sent bytes nobody asked for.
        static bool looksDead(const TcpStream &s) noexcept;

        mutable std::mutex mu;
        std::unordered_map<std::string, std::deque<IdleConn>> idle;
        std::atomic<std::size_t> maxIdlePerHost{kDefaultMaxIdlePerHost};
        std::atomic<std::int64_t> maxIdleAgeMs{kDefaultMaxIdleAge.count()};
    };


    /**
     * @class HttpClient
     * @brief Minimal HTTP/1.1 client.
     *
     * @c get / @c post open a fresh TCP connection per call with
     * @c Connection: close framing -- simple and adversarial-network
     * safe. @c getPooled / @c postPooled go through
     * @c HttpConnectionPool::defaultPool and use @c Connection:
     * keep-alive, returning the stream to the pool when the server
     * honours keep-alive and the response is Content-Length-framed.
     * Use the pooled variants for any production workload that
     * issues more than a handful of requests to the same backend.
     */
    class HttpClient final {
    public:
        /**
         * @brief Issue an HTTP GET. Returns the parsed response.
         *
         * @param host    Server hostname or IP literal.
         * @param port    Server port.
         * @param path    Request target (e.g. @c "/" or @c "/api/x").
         * @param extraHeaders Additional headers to attach to the
         *                       request (Host is added automatically).
         *
         * @throws SocketException on connect/IO failures.
         * @throws std::runtime_error if the response is malformed.
         */
        static YarnBall::Task<HttpResponse> get(
            std::string host,
            std::uint16_t port,
            std::string path,
            std::vector<HttpHeader> extraHeaders = {}) {
            co_return co_await HttpClient::request(
                "GET", std::move(host), port, std::move(path),
                std::move(extraHeaders), {});
        }

        /**
         * @brief Issue an HTTP POST with @p body bytes. Adds
         *        Content-Length automatically.
         */
        static YarnBall::Task<HttpResponse> post(
            std::string host,
            std::uint16_t port,
            std::string path,
            std::string body,
            std::vector<HttpHeader> extraHeaders = {}) {
            co_return co_await HttpClient::request(
                "POST", std::move(host), port, std::move(path),
                std::move(extraHeaders), std::move(body));
        }

        /**
         * @brief Pooled GET. Uses @c HttpConnectionPool::defaultPool
         *        for connection reuse with @c Connection: keep-alive.
         *        Falls back to a fresh connect when no idle connection
         *        is available.
         */
        static YarnBall::Task<HttpResponse> getPooled(
            std::string host,
            std::uint16_t port,
            std::string path,
            std::vector<HttpHeader> extraHeaders = {}) {
            co_return co_await HttpClient::pooledRequest(
                "GET", std::move(host), port, std::move(path),
                std::move(extraHeaders), {});
        }

        /**
         * @brief Pooled POST. See @ref getPooled.
         */
        static YarnBall::Task<HttpResponse> postPooled(
            std::string host,
            std::uint16_t port,
            std::string path,
            std::string body,
            std::vector<HttpHeader> extraHeaders = {}) {
            co_return co_await HttpClient::pooledRequest(
                "POST", std::move(host), port, std::move(path),
                std::move(extraHeaders), std::move(body));
        }

        /**
         * @brief Default longest a new connection (name resolution plus the TCP
         *        handshake) may take before the request fails with a
         *        @c SocketException; applies to every request this class makes
         *        on a fresh connection.
         */
        static constexpr std::chrono::milliseconds kDefaultConnectTimeout{10000};

        /**
         * @brief Change the process-wide connect timeout. Zero disables it
         *        (the kernel's own connect timeout still applies).
         */
        static void setConnectTimeout(std::chrono::milliseconds t) noexcept {
            connectTimeoutMs().store(t.count(), std::memory_order_relaxed);
        }

        /// @return The current connect timeout.
        static std::chrono::milliseconds connectTimeout() noexcept {
            return std::chrono::milliseconds{connectTimeoutMs().load(std::memory_order_relaxed)};
        }

    private:
        static std::atomic<long long> &connectTimeoutMs() noexcept {
            static std::atomic<long long> v{kDefaultConnectTimeout.count()};
            return v;
        }

        /**
         * @brief Shared response-parsing path used by both @ref request
         *        and @ref pooledRequest: read the status line, skip
         *        past any 1xx interim responses (e.g. a proactively
         *        sent "100 Continue" ahead of the real answer -- some
         *        servers send these unconditionally for requests with
         *        a body, without the client asking for them), read the
         *        final status line + headers, then frame the body.
         *
         *        Per RFC 9110 §6.4.1 / §15.4.5, a @c 204 or @c 304
         *        response never carries a body regardless of any
         *        @c Content-Length header -- servers legitimately send
         *        @c Content-Length on a @c 304 reflecting the cached
         *        resource's length, purely for bookkeeping. Treating
         *        that as a real body to read would wait forever for
         *        bytes an RFC-compliant server will never send (the
         *        connection has no reason to close: it's sitting at a
         *        clean boundary waiting for the next request). Such
         *        responses are handled first and unconditionally
         *        report no body, before Content-Length is even
         *        consulted -- this also covers the equally-broken case
         *        where such a response has NO Content-Length at all,
         *        which would otherwise fall into the read-to-EOF path
         *        and hang exactly the same way.
         *
         * @param[out] outStreamReusable @c true iff the stream is left
         *        at a clean next-message boundary that a caller may
         *        safely pool (a body-forbidden response, or one framed
         *        by a valid, present Content-Length); @c false for an
         *        EOF-framed (identity) response, where the socket must
         *        be read to closure and cannot be reused.
         */
        static YarnBall::Task<HttpResponse> readResponse(
            BufferedReader<TcpStream> &r, bool &outStreamReusable,
            bool *sawResponseByte = nullptr) {
            outStreamReusable = false;
            if (sawResponseByte) *sawResponseByte = false;
            HttpResponse resp;
            // Counted across the interim responses too, so a stream of 1xx
            // responses cannot make the client buffer more than one cap's worth.
            std::size_t header_bytes = 0;

            // Loop past any 1xx interim response(s) to the real,
            // final status line + header block. Bounded by
            // kHttpMaxInterimResponses so a peer sending an endless
            // stream of otherwise-valid 1xx responses cannot hang this
            // loop forever.
            for (int interimCount = 0; ; ++interimCount) {
                if (interimCount >= detail::kHttpMaxInterimResponses) {
                    throw std::runtime_error(
                        "HttpClient: too many 1xx interim responses");
                }
                resp = HttpResponse{};
                std::string status_line = co_await r.readLine();
                if (status_line.empty()) {
                    throw std::runtime_error(
                        "HttpClient: connection closed before status line");
                }
                if (sawResponseByte) *sawResponseByte = true;
                HttpClient::parseStatusLine(status_line, resp);

                header_bytes += status_line.size();
                while (true) {
                    std::string line = co_await r.readLine();
                    header_bytes += line.size();
                    if (header_bytes > detail::kHttpMaxHeaderBytes) {
                        throw std::runtime_error("HttpClient: header block too large");
                    }
                    if (line.empty()) {
                        throw std::runtime_error(
                            "HttpClient: connection closed inside header block");
                    }
                    if (line == "\r\n" || line == "\n") break;
                    HttpClient::parseHeaderLine(line, resp);
                }

                if (resp.status < 100 || resp.status >= 200) break;
                // 1xx: no body ever accompanies it: discard and loop
                // for the response it precedes.
            }

            if (resp.status == 204 || resp.status == 304) {
                // No body by definition; stream is already at the next
                // message boundary (nothing beyond the headers was
                // ever sent), so it's safe to pool.
                outStreamReusable = true;
                co_return resp;
            }

            // Chunked framing is not implemented. Reading such a response
            // as identity or Content-Length framed would hand the caller
            // raw chunk framing and, on a pooled connection, leave unread
            // bytes that desynchronise the next response.
            if (!resp.header("Transfer-Encoding").empty()) {
                throw std::runtime_error("HttpClient: Transfer-Encoding not supported");
            }
            std::size_t clenValue = 0;
            if (detail::strictContentLength(resp.headers, clenValue)) {
                auto bytes = co_await r.readExact(clenValue);
                if (bytes.size() != clenValue) {
                    throw std::runtime_error("HttpClient: truncated response body");
                }
                resp.body.assign(reinterpret_cast<const char *>(bytes.data()),
                                 bytes.size());
                outStreamReusable = true;
            } else {
                // Identity-framed: read to EOF; cannot pool afterwards.
                std::string body_acc;
                while (!r.eof()) {
                    auto chunk = co_await r.readExact(4096);
                    if (chunk.empty()) break;
                    if (body_acc.size() + chunk.size() > detail::kHttpMaxBodyBytes) {
                        throw std::runtime_error("HttpClient: body too large");
                    }
                    body_acc.append(reinterpret_cast<const char *>(chunk.data()),
                                    chunk.size());
                }
                resp.body = std::move(body_acc);
                outStreamReusable = false;
            }
            co_return resp;
        }

        /**
         * @brief Pooled-connection request flow. Acquires from the
         *        default pool, sends with @c Connection: keep-alive,
         *        parses the response via @ref readResponse, and
         *        returns the connection to the pool when the parse
         *        left the stream at a clean boundary AND the response
         *        did not include @c Connection: close. Otherwise the
         *        stream is dropped (closed).
         */
        static YarnBall::Task<HttpResponse> pooledRequest(
            std::string method,
            std::string host,
            std::uint16_t port,
            std::string path,
            std::vector<HttpHeader> extra,
            std::string body) {
            checkRequestParts(method, host, path, extra);
            auto &pool = HttpConnectionPool::defaultPool();

            std::string req = method;
            req += ' ';
            req += path;
            req += " HTTP/1.1\r\n";
            req += "Host: " + host + "\r\n";
            req += "Connection: keep-alive\r\n";
            req += "User-Agent: manwe-soccer-http/0.3\r\n";
            for (const auto &h : extra) {
                req += h.name + ": " + h.value + "\r\n";
            }
            if (!body.empty()) {
                req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            }
            req += "\r\n";

            const bool idempotent = isIdempotentMethod(method);
            for (int attempt = 0; ; ++attempt) {
                // Only the first attempt may use a pooled connection; the retry
                // below always dials a fresh one.
                std::optional<TcpStream> reused;
                if (attempt == 0) reused = pool.acquireLive(host, port);
                const bool wasReused = reused.has_value();
                TcpStream stream = reused ? std::move(*reused)
                                           : co_await tcpConnect(host, port, connectTimeout());

                bool sawResponse = false;
                try {
                    co_await stream.write(std::span<const std::byte>(
                        reinterpret_cast<const std::byte *>(req.data()), req.size()));
                    if (!body.empty()) {
                        co_await stream.write(std::span<const std::byte>(
                            reinterpret_cast<const std::byte *>(body.data()),
                            body.size()));
                    }

                    BufferedReader<TcpStream> r(&stream);
                    bool reusable = false;
                    HttpResponse resp = co_await HttpClient::readResponse(r, reusable, &sawResponse);

                    // Return the connection to the pool only if the parse left
                    // it at a clean boundary and the server didn't explicitly
                    // close it.
                    if (reusable && !detail::iequalsAscii(resp.header("Connection"), "close")) {
                        pool.release(host, port, std::move(stream));
                    }
                    co_return resp;
                } catch (...) {
                    // A pooled connection can die between the liveness check and
                    // the write (the server closes keep-alives on its own
                    // schedule). If nothing of a response was seen and the
                    // request is safe to repeat, redo it once on a new connection.
                    if (wasReused && idempotent && !sawResponse) continue;
                    throw;
                }
            }
        }

        /**
         * @brief Common request path. Caller picks the method + body.
         */
        static YarnBall::Task<HttpResponse> request(
            std::string method,
            std::string host,
            std::uint16_t port,
            std::string path,
            std::vector<HttpHeader> extra,
            std::string body) {
            // Build the request preamble.
            checkRequestParts(method, host, path, extra);
            std::string req = method;
            req += ' ';
            req += path;
            req += " HTTP/1.1\r\n";
            req += "Host: " + host + "\r\n";
            req += "Connection: close\r\n";
            req += "User-Agent: manwe-soccer-http/0.3\r\n";
            for (const auto &h : extra) {
                req += h.name + ": " + h.value + "\r\n";
            }
            if (!body.empty()) {
                req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            }
            req += "\r\n";

            auto stream = co_await tcpConnect(host, port, connectTimeout());

            // Send preamble + body in two writes; the kernel coalesces
            // these for us. Splitting is fine for v1.
            co_await stream.write(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(req.data()), req.size()));
            if (!body.empty()) {
                co_await stream.write(std::span<const std::byte>(
                    reinterpret_cast<const std::byte *>(body.data()),
                    body.size()));
            }

            // Parse the response with a BufferedReader so we can do
            // line-based header parsing without per-byte read calls.
            BufferedReader<TcpStream> r(&stream);
            bool reusable = false; // unused: this connection is never pooled.
            HttpResponse resp = co_await HttpClient::readResponse(r, reusable);
            co_return resp;
        }

        /// Methods RFC 9110 9.2.2 defines as idempotent: safe to repeat after a failed attempt.
        static bool isIdempotentMethod(std::string_view m) {
            for (std::string_view idem : {"GET", "HEAD", "PUT", "DELETE", "OPTIONS", "TRACE"}) {
                if (detail::iequalsAscii(m, idem)) return true;
            }
            return false;
        }

        /**
         * @brief Reject CR/LF/NUL in anything interpolated into the
         *        request head (request-line / header injection).
         */
        static void checkRequestParts(const std::string &method,
                                       const std::string &host,
                                       const std::string &path,
                                       const std::vector<HttpHeader> &extra) {
            bool bad = detail::hasCtlLineBreak(method) || detail::hasCtlLineBreak(host) ||
                       detail::hasCtlLineBreak(path) || path.find(' ') != std::string::npos;
            for (const auto &h : extra) {
                if (detail::hasCtlLineBreak(h.name) || detail::hasCtlLineBreak(h.value)) bad = true;
            }
            if (bad) {
                throw std::runtime_error("HttpClient: control characters in request");
            }
        }

        /**
         * @brief Parse "HTTP/x.y CODE REASON\\r\\n" into @p resp.
         */
        static void parseStatusLine(const std::string &line, HttpResponse &resp) {
            // Tokenise: skip the HTTP-version, take the code, take the
            // rest as reason.
            const std::size_t sp1 = line.find(' ');
            if (sp1 == std::string::npos) {
                throw std::runtime_error("HttpClient: malformed status line");
            }
            const std::size_t sp2 = line.find(' ', sp1 + 1);
            const std::string code =
                line.substr(sp1 + 1, (sp2 == std::string::npos ? line.size() : sp2)
                                      - sp1 - 1);
            if (code.size() != 3 ||
                code.find_first_not_of("0123456789") != std::string::npos) {
                throw std::runtime_error("HttpClient: malformed status code");
            }
            resp.status = std::atoi(code.c_str());
            if (sp2 != std::string::npos) {
                resp.reason = detail::trimAscii(line.substr(sp2 + 1));
            }
        }

        /**
         * @brief Parse "Name: Value\\r\\n" and append to @p resp.headers.
         *        Tolerates absent CR; trims whitespace around the value.
         */
        static void parseHeaderLine(const std::string &line, HttpResponse &resp) {
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) return; // skip malformed line
            HttpHeader h;
            h.name = line.substr(0, colon);
            h.value = detail::trimAscii(line.substr(colon + 1));
            resp.headers.push_back(std::move(h));
        }
    };


    // ---- HttpConnectionPool inline definitions -----------------------

    inline HttpConnectionPool &HttpConnectionPool::defaultPool() {
        static HttpConnectionPool p;
        return p;
    }

    inline void HttpConnectionPool::setMaxIdlePerHost(std::size_t n) noexcept {
        this->maxIdlePerHost.store(n, std::memory_order_relaxed);
    }

    inline void HttpConnectionPool::setMaxIdleAge(std::chrono::milliseconds age) noexcept {
        this->maxIdleAgeMs.store(age.count(), std::memory_order_relaxed);
    }

    inline bool HttpConnectionPool::looksDead(const TcpStream &s) noexcept {
        // An idle keep-alive connection has nothing to read. EOF means the peer
        // closed it, data means the stream is out of step, an error other than
        // "would block" means it is broken.
        char c;
        const ::ssize_t n = ::recv(s.fd(), &c, 1, MSG_PEEK);
        if (n == 0) return true;
        if (n > 0) return true;
        return !YarnBall::isWouldBlock(YarnBall::lastSocketError());
    }

    inline std::optional<TcpStream>
    HttpConnectionPool::acquire(std::string_view host, std::uint16_t port) {
        const std::string k = keyOf(host, port);
        const std::int64_t ageMs = this->maxIdleAgeMs.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(this->mu);
        auto it = this->idle.find(k);
        if (it == this->idle.end()) return std::nullopt;
        auto &dq = it->second;
        while (!dq.empty()) {
            IdleConn c = std::move(dq.back());
            dq.pop_back();
            const bool stale = ageMs > 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - c.lastUsed).count() > ageMs;
            if (stale) {
                // Everything older sits in front of this one: drop it all.
                dq.clear();
                break;
            }
            return std::move(c.stream);
        }
        return std::nullopt;
    }

    inline std::optional<TcpStream>
    HttpConnectionPool::acquireLive(std::string_view host, std::uint16_t port) {
        while (true) {
            std::optional<TcpStream> s = this->acquire(host, port);
            if (!s || !looksDead(*s)) return s;
            // The peer closed it while it sat idle: drop it (closing our end) and try the next.
        }
    }

    inline void HttpConnectionPool::release(std::string_view host,
                                              std::uint16_t port,
                                              TcpStream stream) {
        const std::string k = keyOf(host, port);
        const std::size_t cap = this->maxIdlePerHost.load(std::memory_order_relaxed);
        const std::int64_t ageMs = this->maxIdleAgeMs.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(this->mu);
        auto &dq = this->idle[k];
        // Drop what has gone stale while it waited; the oldest are at the front.
        while (ageMs > 0 && !dq.empty() &&
               std::chrono::duration_cast<std::chrono::milliseconds>(now - dq.front().lastUsed).count() > ageMs) {
            dq.pop_front();
        }
        if (dq.size() >= cap) {
            // Over the cap: drop the stream by letting its destructor
            // run. Caller's move means we already own it.
            (void) stream;
            return;
        }
        dq.push_back(IdleConn{std::move(stream), now});
    }

    inline std::size_t
    HttpConnectionPool::idleCount(std::string_view host, std::uint16_t port) const {
        const std::string k = keyOf(host, port);
        std::lock_guard<std::mutex> lk(this->mu);
        auto it = this->idle.find(k);
        return it == this->idle.end() ? 0 : it->second.size();
    }

}

#endif // SOCCER_HTTP_CLIENT_H
