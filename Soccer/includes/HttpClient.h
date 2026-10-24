//
// Created by Fabrizio Paino on 2026-05-15.
//
// HttpClient: tiny HTTP/1.1 client built on Soccer::TcpStream +
// BufferedReader. Supports GET / POST, Content-Length-framed and
// chunked-coded bodies (a lone "chunked" Transfer-Encoding, size-bounded;
// trailers are read and discarded), arbitrary request/response headers.
// NO TLS yet -- pair with Soccer::TlsStream once
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
#include <span>
#include <stdexcept>
#include <string_view>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "BufferedReader.h"
#include "Coroutines.h"
#include "IoDeadline.h"
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

        /// RFC 9110 5.6.2 tchar: the characters allowed in a method or a header field name.
        inline bool isTokenChar(unsigned char c) noexcept {
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
            switch (c) {
                case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
                case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
                    return true;
                default:
                    return false;
            }
        }

        /// True if @p s is a non-empty RFC 9110 token.
        inline bool isHttpToken(std::string_view s) noexcept {
            if (s.empty()) return false;
            for (const char c : s) {
                if (!isTokenChar(static_cast<unsigned char>(c))) return false;
            }
            return true;
        }

        /// True if @p s holds a control character a header field value may not carry
        /// (anything below 0x20 other than HT, or DEL): CR, LF and NUL included.
        inline bool hasFieldValueCtl(std::string_view s) noexcept {
            for (const char ch : s) {
                const unsigned char c = static_cast<unsigned char>(ch);
                if ((c < 0x20 && c != '\t') || c == 0x7f) return true;
            }
            return false;
        }

        /// True if @p s cannot be a request target: it holds a space, a control
        /// character or DEL (so it cannot split the request line either).
        inline bool hasTargetCtl(std::string_view s) noexcept {
            for (const char ch : s) {
                const unsigned char c = static_cast<unsigned char>(ch);
                if (c <= 0x20 || c == 0x7f) return true;
            }
            return false;
        }

        /**
         * @brief True if any header field is named Transfer-Encoding, whatever its
         *        value (an empty or folded one counts): every occurrence is looked
         *        at, not just the first, so a decoy ahead of the real one hides nothing.
         */
        template<typename HeaderVec>
        inline bool hasTransferEncoding(const HeaderVec &headers) {
            for (const auto &h : headers) {
                if (iequalsAscii(h.name, "Transfer-Encoding")) return true;
            }
            return false;
        }

        /**
         * @brief True if the comma-separated token list @p value (a Connection or
         *        Upgrade field value) holds @p token, compared case-insensitively.
         */
        inline bool headerListHasToken(std::string_view value, std::string_view token) {
            std::size_t pos = 0;
            while (pos <= value.size()) {
                std::size_t comma = value.find(',', pos);
                if (comma == std::string_view::npos) comma = value.size();
                std::string_view item = value.substr(pos, comma - pos);
                while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
                while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
                if (iequalsAscii(item, token)) return true;
                pos = comma + 1;
            }
            return false;
        }

        /**
         * @brief True if a header a handler sets on a response is one the server
         *        frames itself (length and connection management), so the handler's
         *        copy is dropped: a second Content-Length, or a Transfer-Encoding next
         *        to it, would make the response ambiguous.
         */
        inline bool isServerManagedResponseHeader(std::string_view name) noexcept {
            return iequalsAscii(name, "Content-Length") || iequalsAscii(name, "Connection") ||
                   iequalsAscii(name, "Transfer-Encoding") || iequalsAscii(name, "Keep-Alive") ||
                   iequalsAscii(name, "Upgrade") || iequalsAscii(name, "Proxy-Connection");
        }

        /// True if a response with @p status never carries content: 1xx, 204 and 304.
        inline bool statusHasNoBody(int status) noexcept {
            return (status >= 100 && status < 200) || status == 204 || status == 304;
        }

        /**
         * @brief The Content-Length a server puts on a response, and whether it
         *        sends the body. HEAD, 1xx, 204 and 304 never carry content:
         *        a HEAD answer gets the length the GET would have had (the
         *        handler's own Content-Length @p stated if it set one, else the
         *        body's size), 304 only a length the handler stated, 1xx and 204 none.
         *        An empty result means "no Content-Length header".
         */
        inline std::string framedContentLength(int status, bool head, std::string_view stated,
                                               std::size_t bodySize, bool &sendBody) {
            sendBody = !head && !statusHasNoBody(status);
            if (sendBody) return std::to_string(bodySize);
            if (status == 204 || (status >= 100 && status < 200)) return {};
            const bool numeric = !stated.empty() && stated.size() <= 12 &&
                                 stated.find_first_not_of("0123456789") == std::string_view::npos;
            if (numeric) return std::string(stated);
            if (status == 304) return {};
            return std::to_string(bodySize);
        }

        /// The standard reason phrase for @p status ("" when there is none to offer).
        inline const char *reasonPhrase(int status) noexcept {
            switch (status) {
                case 100: return "Continue";
                case 101: return "Switching Protocols";
                case 200: return "OK";
                case 201: return "Created";
                case 202: return "Accepted";
                case 203: return "Non-Authoritative Information";
                case 204: return "No Content";
                case 205: return "Reset Content";
                case 206: return "Partial Content";
                case 300: return "Multiple Choices";
                case 301: return "Moved Permanently";
                case 302: return "Found";
                case 303: return "See Other";
                case 304: return "Not Modified";
                case 307: return "Temporary Redirect";
                case 308: return "Permanent Redirect";
                case 400: return "Bad Request";
                case 401: return "Unauthorized";
                case 403: return "Forbidden";
                case 404: return "Not Found";
                case 405: return "Method Not Allowed";
                case 406: return "Not Acceptable";
                case 408: return "Request Timeout";
                case 409: return "Conflict";
                case 410: return "Gone";
                case 411: return "Length Required";
                case 412: return "Precondition Failed";
                case 413: return "Content Too Large";
                case 414: return "URI Too Long";
                case 415: return "Unsupported Media Type";
                case 417: return "Expectation Failed";
                case 422: return "Unprocessable Content";
                case 426: return "Upgrade Required";
                case 429: return "Too Many Requests";
                case 431: return "Request Header Fields Too Large";
                case 500: return "Internal Server Error";
                case 501: return "Not Implemented";
                case 502: return "Bad Gateway";
                case 503: return "Service Unavailable";
                case 504: return "Gateway Timeout";
                case 505: return "HTTP Version Not Supported";
                default: return "";
            }
        }

        /**
         * @brief Split a request target at its first '?': @p path gets the part
         *        before it, @p query the part after (without the '?').
         */
        inline void splitQuery(std::string_view target, std::string &path, std::string &query) {
            const std::size_t q = target.find('?');
            if (q == std::string_view::npos) {
                path.assign(target);
                query.clear();
            } else {
                path.assign(target.substr(0, q));
                query.assign(target.substr(q + 1));
            }
        }

        /**
         * @brief A TcpStream view that counts the bytes read through it, so the
         *        pooled request path can compare what the BufferedReader pulled off
         *        the socket with what the response actually used.
         */
        struct CountingStream {
            TcpStream *stream;
            std::size_t total{0};

            auto fd() const noexcept { return this->stream->fd(); }

            YarnBall::Task<std::size_t> read(std::span<std::byte> buf) {
                const std::size_t n = co_await this->stream->read(buf);
                this->total += n;
                co_return n;
            }
        };

        /**
         * @brief Host header value for @p host / @p port: an IPv6 literal is
         *        bracketed, and the port is left off when it equals @p omitPort
         *        (pass 0 to always include it).
         */
        inline std::string hostHeaderValue(std::string_view host, std::uint16_t port,
                                           std::uint16_t omitPort) {
            std::string out;
            if (host.find(':') != std::string_view::npos && (host.empty() || host.front() != '[')) {
                out.push_back('[');
                out.append(host);
                out.push_back(']');
            } else {
                out.append(host);
            }
            if (port != 0 && port != omitPort) {
                out.push_back(':');
                out.append(std::to_string(port));
            }
            return out;
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

        /// Most distinct @c host:port keys that may hold idle connections at once.
        static constexpr std::size_t kMaxHosts = 256;

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
         * @brief Issue an HTTP HEAD. The response carries the headers a GET would
         *        have (Content-Length included) and never a body, whatever the
         *        Content-Length says.
         */
        static YarnBall::Task<HttpResponse> head(
            std::string host,
            std::uint16_t port,
            std::string path,
            std::vector<HttpHeader> extraHeaders = {}) {
            co_return co_await HttpClient::request(
                "HEAD", std::move(host), port, std::move(path),
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

        /**
         * @brief Default longest the response head (status line and headers) may
         *        take to arrive once the request is being sent; a server that
         *        accepts the connection and then says nothing fails the request
         *        with a @c SocketException after this long.
         */
        static constexpr std::chrono::milliseconds kDefaultResponseHeaderTimeout{30000};

        /**
         * @brief Default longest the response body may take to arrive once its
         *        head is in (the whole body, not the gap between two reads).
         */
        static constexpr std::chrono::milliseconds kDefaultResponseBodyTimeout{120000};

        /**
         * @brief Change the process-wide response deadlines. Zero disables the
         *        respective limit.
         */
        static void setResponseTimeouts(std::chrono::milliseconds header,
                                        std::chrono::milliseconds body) noexcept {
            responseHeaderTimeoutMs().store(header.count(), std::memory_order_relaxed);
            responseBodyTimeoutMs().store(body.count(), std::memory_order_relaxed);
        }

        /// @return The current response-head deadline.
        static std::chrono::milliseconds responseHeaderTimeout() noexcept {
            return std::chrono::milliseconds{responseHeaderTimeoutMs().load(std::memory_order_relaxed)};
        }

        /// @return The current response-body deadline.
        static std::chrono::milliseconds responseBodyTimeout() noexcept {
            return std::chrono::milliseconds{responseBodyTimeoutMs().load(std::memory_order_relaxed)};
        }

    private:
        static std::atomic<long long> &connectTimeoutMs() noexcept {
            static std::atomic<long long> v{kDefaultConnectTimeout.count()};
            return v;
        }

        static std::atomic<long long> &responseHeaderTimeoutMs() noexcept {
            static std::atomic<long long> v{kDefaultResponseHeaderTimeout.count()};
            return v;
        }

        static std::atomic<long long> &responseBodyTimeoutMs() noexcept {
            static std::atomic<long long> v{kDefaultResponseBodyTimeout.count()};
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
         *        by a valid, present Content-Length or by chunked coding,
         *        and neither side asked to close the connection: a
         *        @c close token in Connection, or HTTP/1.0 without
         *        @c keep-alive); @c false for an EOF-framed (identity)
         *        response, where the socket must be read to closure and
         *        cannot be reused.
         * @param headRequest The request was HEAD: the response carries no body
         *        whatever its Content-Length says.
         * @param[out] consumedOut Bytes of the stream the response occupied
         *        (head, body and chunk framing), for the leftover check.
         *
         *        Transfer-Encoding is handled for exactly one case: a single
         *        @c chunked coding, no Content-Length beside it. Chunk sizes
         *        are hex of at most 16 digits and bounded by the body cap
         *        (@ref detail::kHttpMaxBodyBytes) in total, chunk extensions
         *        are ignored, and the trailer section is read and discarded
         *        within the header-block bounds. Anything else throws.
         */
        template<typename Reader>
        static YarnBall::Task<HttpResponse> readResponse(
            Reader &r, bool &outStreamReusable,
            bool *sawResponseByte = nullptr, IoDeadline *deadline = nullptr,
            bool headRequest = false, std::size_t *consumedOut = nullptr) {
            outStreamReusable = false;
            if (sawResponseByte) *sawResponseByte = false;
            // Bytes taken off the reader for this response (lines and body), so the
            // caller can tell whether the reader swallowed more than the response.
            std::size_t consumed = 0;
            bool http10 = false;
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
                http10 = status_line.compare(0, 8, "HTTP/1.0") == 0;

                header_bytes += status_line.size();
                consumed += status_line.size();
                while (true) {
                    std::string line = co_await r.readLine();
                    header_bytes += line.size();
                    consumed += line.size();
                    if (header_bytes > detail::kHttpMaxHeaderBytes) {
                        throw std::runtime_error("HttpClient: header block too large");
                    }
                    if (line.empty()) {
                        throw std::runtime_error(
                            "HttpClient: connection closed inside header block");
                    }
                    if (line == "\r\n" || line == "\n") break;
                    if (line.back() != '\n') {
                        throw std::runtime_error("HttpClient: connection closed inside header block");
                    }
                    HttpClient::parseHeaderLine(line, resp);
                }

                if (resp.status < 100 || resp.status >= 200 || resp.status == 101) break;
                // 1xx: no body ever accompanies it: discard and loop
                // for the response it precedes.
            }

            // 101 Switching Protocols is final: what follows on the connection
            // belongs to the new protocol, not to HTTP, so nothing more is read here
            // and the stream is never reusable.
            if (resp.status == 101) {
                outStreamReusable = false;
                co_return resp;
            }

            // The head is in: from here on the body clock runs.
            if (deadline) deadline->arm(r.raw()->fd(), responseBodyTimeout());

            // Whether the connection may carry another exchange: the Connection
            // field is a token list (possibly split over several fields), and an
            // HTTP/1.0 peer closes unless it says keep-alive.
            std::string connectionTokens;
            for (const auto &h : resp.headers) {
                if (detail::iequalsAscii(h.name, "Connection")) {
                    if (!connectionTokens.empty()) connectionTokens += ',';
                    connectionTokens += h.value;
                }
            }
            const bool keepAlive = !detail::headerListHasToken(connectionTokens, "close") &&
                                   (!http10 || detail::headerListHasToken(connectionTokens, "keep-alive"));

            if (headRequest || resp.status == 204 || resp.status == 304) {
                // No body by definition (whatever Content-Length says); stream is
                // already at the next message boundary (nothing beyond the headers
                // was ever sent), so it's safe to pool.
                outStreamReusable = keepAlive;
                if (consumedOut) *consumedOut = consumed;
                co_return resp;
            }

            // Every header is looked at: an empty Transfer-Encoding ahead of a
            // "chunked" one must not read as "no Transfer-Encoding".
            if (detail::hasTransferEncoding(resp.headers)) {
                // Only a lone "chunked" coding is understood, and never next to a
                // Content-Length (RFC 9112 6.3: the two disagreeing about where the
                // message ends is a smuggling primitive). A pre-1.1 peer cannot chunk.
                std::size_t teCount = 0;
                bool chunkedOnly = true;
                for (const auto &h : resp.headers) {
                    if (!detail::iequalsAscii(h.name, "Transfer-Encoding")) continue;
                    ++teCount;
                    if (!detail::iequalsAscii(h.value, "chunked")) chunkedOnly = false;
                }
                if (teCount != 1 || !chunkedOnly || http10 ||
                    !resp.header("Content-Length").empty()) {
                    throw std::runtime_error("HttpClient: unsupported Transfer-Encoding");
                }
                std::string chunked;
                while (true) {
                    const std::string sizeLine = co_await r.readLine();
                    consumed += sizeLine.size();
                    if (sizeLine.size() < 3 || sizeLine.size() > 1024 ||
                        sizeLine[sizeLine.size() - 2] != '\r' || sizeLine.back() != '\n') {
                        throw std::runtime_error("HttpClient: malformed chunk size line");
                    }
                    std::size_t digits = 0;
                    std::uint64_t chunkLen = 0;
                    for (; digits + 2 < sizeLine.size(); ++digits) {
                        const char c = sizeLine[digits];
                        unsigned v;
                        if (c >= '0' && c <= '9') v = static_cast<unsigned>(c - '0');
                        else if (c >= 'a' && c <= 'f') v = static_cast<unsigned>(c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') v = static_cast<unsigned>(c - 'A' + 10);
                        else break;
                        if (digits >= 16) throw std::runtime_error("HttpClient: chunk size too large");
                        chunkLen = (chunkLen << 4) | v;
                    }
                    // What follows the size is an extension (";...") and is ignored; a
                    // bare CR or NUL inside it is not allowed.
                    const std::string_view rest = std::string_view(sizeLine).substr(
                        digits, sizeLine.size() - 2 - digits);
                    if (digits == 0 || (!rest.empty() && rest.front() != ';') ||
                        rest.find_first_of(std::string_view("\r\0", 2)) != std::string_view::npos) {
                        throw std::runtime_error("HttpClient: malformed chunk size line");
                    }
                    if (chunkLen == 0) break;
                    if (chunkLen > detail::kHttpMaxBodyBytes ||
                        chunked.size() + chunkLen > detail::kHttpMaxBodyBytes) {
                        throw std::runtime_error("HttpClient: body too large");
                    }
                    std::size_t left = static_cast<std::size_t>(chunkLen);
                    while (left != 0) {
                        const std::size_t want = std::min<std::size_t>(left, 64 * 1024);
                        auto bytes = co_await r.readExact(want);
                        if (bytes.size() != want) {
                            throw std::runtime_error("HttpClient: truncated chunked body");
                        }
                        chunked.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
                        consumed += want;
                        left -= want;
                    }
                    auto crlf = co_await r.readExact(2);
                    if (crlf.size() != 2 || crlf[0] != std::byte{'\r'} || crlf[1] != std::byte{'\n'}) {
                        throw std::runtime_error("HttpClient: malformed chunk terminator");
                    }
                    consumed += 2;
                }
                // Trailer section: header-shaped lines up to a blank one. Read to keep
                // the stream aligned, within the same bounds as the head; discarded.
                std::size_t trailerBytes = 0;
                while (true) {
                    std::string line = co_await r.readLine();
                    consumed += line.size();
                    trailerBytes += line.size();
                    if (trailerBytes > detail::kHttpMaxHeaderBytes) {
                        throw std::runtime_error("HttpClient: trailer block too large");
                    }
                    if (line.empty() || line.back() != '\n') {
                        throw std::runtime_error("HttpClient: connection closed inside trailers");
                    }
                    if (line == "\r\n") break;
                }
                resp.body = std::move(chunked);
                outStreamReusable = keepAlive;
                if (consumedOut) *consumedOut = consumed;
                co_return resp;
            }
            std::size_t clenValue = 0;
            if (detail::strictContentLength(resp.headers, clenValue)) {
                auto bytes = co_await r.readExact(clenValue);
                if (bytes.size() != clenValue) {
                    throw std::runtime_error("HttpClient: truncated response body");
                }
                resp.body.assign(reinterpret_cast<const char *>(bytes.data()),
                                 bytes.size());
                consumed += bytes.size();
                outStreamReusable = keepAlive;
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
            if (consumedOut) *consumedOut = consumed;
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
            // An empty target would put "GET  HTTP/1.1" on the wire.
            if (path.empty()) path = "/";
            auto &pool = HttpConnectionPool::defaultPool();

            const std::string req = buildRequestHead(method, host, port, path, extra,
                                                     body.size(), "keep-alive");

            const bool idempotent = isIdempotentMethod(method);
            for (int attempt = 0; ; ++attempt) {
                // Only the first attempt may use a pooled connection; the retry
                // below always dials a fresh one.
                std::optional<TcpStream> conn;
                if (attempt == 0) conn = pool.acquireLive(host, port);
                const bool wasReused = conn.has_value();
                // Not `reused ? std::move(*reused) : co_await tcpConnect(...)`: GCC
                // evaluates the co_await arm even when the pool had a connection,
                // dialling (and wasting) a second one on every reuse.
                if (!wasReused) {
                    TcpStream dialled = co_await tcpConnect(host, port, connectTimeout());
                    conn.emplace(std::move(dialled));
                }
                TcpStream &stream = *conn;

                bool sawResponse = false;
                // Declared after `stream`, so it is disarmed before the fd is closed or pooled.
                IoDeadline dl(stream.fd(), responseHeaderTimeout());
                try {
                    co_await stream.write(std::span<const std::byte>(
                        reinterpret_cast<const std::byte *>(req.data()), req.size()));
                    if (!body.empty()) {
                        co_await stream.write(std::span<const std::byte>(
                            reinterpret_cast<const std::byte *>(body.data()),
                            body.size()));
                    }

                    detail::CountingStream counted{&stream};
                    BufferedReader<detail::CountingStream> r(&counted);
                    bool reusable = false;
                    std::size_t consumed = 0;
                    HttpResponse resp = co_await HttpClient::readResponse(
                        r, reusable, &sawResponse, &dl, detail::iequalsAscii(method, "HEAD"), &consumed);

                    // Return the connection to the pool only if the parse left
                    // it at a clean boundary, nobody asked to close it, and the
                    // reader did not pull in bytes beyond this response (those
                    // are unexplained and would be lost to the next exchange).
                    dl.disarm();
                    if (reusable && counted.total == consumed) {
                        pool.release(host, port, std::move(stream));
                    }
                    co_return resp;
                } catch (...) {
                    // A request that ran out of time is not retried: that would only
                    // double the wait.
                    if (dl.expired()) throw SocketException("HttpClient: response timed out");
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
            if (path.empty()) path = "/";
            const std::string req = buildRequestHead(method, host, port, path, extra,
                                                     body.size(), "close");

            auto stream = co_await tcpConnect(host, port, connectTimeout());
            // Declared after `stream`, so it is disarmed before the fd is closed.
            IoDeadline dl(stream.fd(), responseHeaderTimeout());
            try {
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
                HttpResponse resp = co_await HttpClient::readResponse(
                    r, reusable, nullptr, &dl, detail::iequalsAscii(method, "HEAD"));
                co_return resp;
            } catch (...) {
                if (dl.expired()) throw SocketException("HttpClient: response timed out");
                throw;
            }
        }

        /// Methods RFC 9110 9.2.2 defines as idempotent: safe to repeat after a failed attempt.
        static bool isIdempotentMethod(std::string_view m) {
            for (std::string_view idem : {"GET", "HEAD", "PUT", "DELETE", "OPTIONS", "TRACE"}) {
                if (detail::iequalsAscii(m, idem)) return true;
            }
            return false;
        }

        /**
         * @brief Reject anything that cannot be put into the request head as it
         *        is: a method or header name that is not a token, control
         *        characters (CR, LF, NUL, ...) in the host, target or a header
         *        value, a space in the target. These are the request-line /
         *        header injection vectors. A caller-supplied Transfer-Encoding is
         *        refused too: the body is always sent Content-Length-framed.
         */
        static void checkRequestParts(const std::string &method,
                                       const std::string &host,
                                       const std::string &path,
                                       const std::vector<HttpHeader> &extra) {
            bool bad = !detail::isHttpToken(method) || host.empty() ||
                       detail::hasTargetCtl(host) || detail::hasTargetCtl(path);
            for (const auto &h : extra) {
                if (!detail::isHttpToken(h.name) || detail::hasFieldValueCtl(h.value) ||
                    detail::iequalsAscii(h.name, "Transfer-Encoding")) {
                    bad = true;
                }
            }
            if (bad) {
                throw std::runtime_error("HttpClient: invalid method, host, target or header in request");
            }
        }

        /**
         * @brief Serialise the request line and headers. A caller-supplied Host or
         *        User-Agent replaces the automatic one and a caller-supplied
         *        Content-Length is dropped (the real length is always sent), so no
         *        header appears twice. Methods that normally carry a body send
         *        @c Content-Length: 0 when there is none.
         */
        static std::string buildRequestHead(const std::string &method,
                                            const std::string &host,
                                            std::uint16_t port,
                                            const std::string &path,
                                            const std::vector<HttpHeader> &extra,
                                            std::size_t bodySize,
                                            const char *connection) {
            bool haveHost = false, haveAgent = false;
            for (const auto &h : extra) {
                if (detail::iequalsAscii(h.name, "Host")) haveHost = true;
                else if (detail::iequalsAscii(h.name, "User-Agent")) haveAgent = true;
            }
            std::string req = method;
            req += ' ';
            req += path;
            req += " HTTP/1.1\r\n";
            if (!haveHost) req += "Host: " + detail::hostHeaderValue(host, port, 80) + "\r\n";
            req += "Connection: ";
            req += connection;
            req += "\r\n";
            if (!haveAgent) req += "User-Agent: manwe-soccer-http/0.3\r\n";
            for (const auto &h : extra) {
                if (detail::iequalsAscii(h.name, "Content-Length")) continue;
                req += h.name + ": " + h.value + "\r\n";
            }
            const bool expectsBody = detail::iequalsAscii(method, "POST") ||
                                     detail::iequalsAscii(method, "PUT") ||
                                     detail::iequalsAscii(method, "PATCH");
            if (bodySize != 0 || expectsBody) {
                req += "Content-Length: " + std::to_string(bodySize) + "\r\n";
            }
            req += "\r\n";
            return req;
        }

        /// Drop the line terminator (CRLF, or a bare LF) from a line read off the wire.
        static std::string stripLineEnd(std::string line) {
            if (!line.empty() && line.back() == '\n') line.pop_back();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }

        /**
         * @brief Parse "HTTP/x.y CODE[ REASON]\\r\\n" into @p resp. The reason
         *        phrase is optional (RFC 9112 4); the version must look like
         *        HTTP/<digit>.<digit>.
         */
        static void parseStatusLine(const std::string &rawLine, HttpResponse &resp) {
            const std::string line = stripLineEnd(rawLine);
            if (line.size() < 12 || line.compare(0, 5, "HTTP/") != 0 ||
                !std::isdigit(static_cast<unsigned char>(line[5])) || line[6] != '.' ||
                !std::isdigit(static_cast<unsigned char>(line[7])) || line[8] != ' ') {
                throw std::runtime_error("HttpClient: malformed status line");
            }
            const std::size_t sp2 = line.find(' ', 9);
            const std::string code =
                line.substr(9, (sp2 == std::string::npos ? line.size() : sp2) - 9);
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
         *        Trims whitespace around the value. A line that is not a valid
         *        field (obs-fold continuation, no colon, a name that is not a
         *        token, a stray CR or NUL) is refused rather than skipped: a
         *        skipped line could be the Transfer-Encoding a proxy would read.
         */
        static void parseHeaderLine(const std::string &rawLine, HttpResponse &resp) {
            const std::string line = stripLineEnd(rawLine);
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos ||
                !detail::isHttpToken(std::string_view(line).substr(0, colon))) {
                throw std::runtime_error("HttpClient: malformed header line");
            }
            HttpHeader h;
            h.name = line.substr(0, colon);
            h.value = detail::trimAscii(line.substr(colon + 1));
            if (detail::hasFieldValueCtl(h.value)) {
                throw std::runtime_error("HttpClient: control character in header value");
            }
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
        std::optional<TcpStream> out;
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
            out = std::move(c.stream);
            break;
        }
        // A host with nothing idle keeps no entry, so the map cannot grow with every
        // host ever contacted.
        if (dq.empty()) this->idle.erase(it);
        return out;
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
        if (this->idle.size() >= kMaxHosts && this->idle.find(k) == this->idle.end()) {
            // Too many hosts hold idle connections: forget the ones that have gone stale
            // or empty, and if that frees no room this connection is simply not pooled.
            for (auto it = this->idle.begin(); it != this->idle.end();) {
                auto &old = it->second;
                while (ageMs > 0 && !old.empty() &&
                       std::chrono::duration_cast<std::chrono::milliseconds>(now - old.front().lastUsed).count() > ageMs) {
                    old.pop_front();
                }
                it = old.empty() ? this->idle.erase(it) : std::next(it);
            }
            if (this->idle.size() >= kMaxHosts) return;
        }
        auto &dq = this->idle[k];
        // Drop what has gone stale while it waited; the oldest are at the front.
        while (ageMs > 0 && !dq.empty() &&
               std::chrono::duration_cast<std::chrono::milliseconds>(now - dq.front().lastUsed).count() > ageMs) {
            dq.pop_front();
        }
        if (dq.size() >= cap) {
            // Over the cap: drop the stream by letting its destructor
            // run. Caller's move means we already own it.
            if (dq.empty()) this->idle.erase(k);
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
