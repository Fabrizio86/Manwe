//
// Created by Fabrizio Paino on 2026-05-16.
//
// Http2 -- HTTP/2 client built on nghttp2 and the Yarn Reactor.
//
// When CMake finds nghttp2 (libnghttp2-dev / brew install nghttp2)
// this header is the user-facing API for a real, multiplexed
// HTTP/2 connection. When nghttp2 is not present, the entry points
// throw @c Http2NotImplemented so user code can still compile and
// fall back to HTTP/1.1.
//
// Design choice -- this wraps nghttp2 rather than re-implementing
// the protocol. See docs/http2.md for the rationale (CVE coverage,
// h2spec compliance, long-term maintenance).
//
// Typical use (client, h2 over TLS):
//
//     Soccer::TlsClientOptions tls;
//     tls.alpnProtocols = "h2";
//     tls.caBundleFile = "/etc/ssl/cert.pem";
//
//     auto conn = co_await Soccer::Http2Connection::connect(
//         "api.example.com", 443, tls);
//
//     // Many requests can be in flight at once on the same conn;
//     // each is its own stream and resumes independently.
//     auto r1 = co_await conn.request("GET",  "/v1/items", {}, "");
//     auto r2 = co_await conn.request("POST", "/v1/items", headers, body);
//

#ifndef SOCCER_HTTP2_H
#define SOCCER_HTTP2_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Coroutines.h"
#include "HttpClient.h"   // HttpHeader, HttpResponse
#include "HttpServer.h"   // HttpRequest, HttpResponse, HttpRouteHandler
#include "TcpListener.h"
#include "TcpStream.h"

#ifdef SOCCER_HAS_TLS
#include "TlsStream.h"
#endif

// Forward-declared so this header doesn't pull <nghttp2/nghttp2.h> into
// every translation unit.
struct nghttp2_session;

namespace Soccer {

    /**
     * @class Http2NotImplemented
     * @brief Thrown by every Http2Connection entry point when Soccer
     *        was built without nghttp2 (the @c SOCCER_HAS_HTTP2 compile
     *        flag is not set). Existing HTTP/1.1 code paths
     *        (@c HttpClient::get / post / pooled variants) are
     *        unaffected.
     */
    class Http2NotImplemented : public std::runtime_error {
    public:
        Http2NotImplemented()
            : std::runtime_error(
                "Soccer was built without nghttp2; install libnghttp2-dev "
                "(or `brew install nghttp2`) and rebuild to enable HTTP/2") {}
    };

    /**
     * @brief HTTP/2 response shape. Identical to @ref HttpResponse so
     *        client code can swap implementations without rewriting.
     *        The @c :status pseudo-header is decoded into
     *        @ref HttpResponse::status; other pseudo-headers
     *        (@c :scheme, @c :authority, @c :path) are dropped from
     *        the response.
     */
    using Http2Response = HttpResponse;

#ifdef SOCCER_HAS_HTTP2

    namespace detail {
        /**
         * @struct Http2Stream
         * @brief Per-stream state held by the connection. Allocated
         *        when @c request submits, freed after the awaiter has
         *        consumed the response.
         */
        struct Http2Stream {
            Http2Response response;
            std::atomic<bool> done{false};
            std::coroutine_handle<> waiter{};
            int errorCode = 0;
            // Request body bytes and read offset for the nghttp2
            // data_provider callback.
            std::string body;
            std::size_t bodyOffset{0};
        };
    } // namespace detail

    /**
     * @class Http2Connection
     * @brief A single multiplexed HTTP/2 connection. One TCP+TLS pipe
     *        carries N concurrent streams; @c request opens one stream
     *        and returns a @c Task that suspends only on THAT stream's
     *        response. Other streams on the same connection proceed
     *        independently.
     *
     * Lifetime: move-only. The destructor sends GOAWAY and closes the
     * underlying socket. Once a connection has been moved-from, it
     * holds no streams and is inert.
     *
     * Thread-safety: @c request can be called from any coroutine on
     * any worker; an internal session mutex serialises all nghttp2
     * API calls. The reader-driver coroutine is started at construction
     * and runs until the connection is destroyed or the peer GOAWAYs.
     */
    class Http2Connection final {
    public:
#ifdef SOCCER_HAS_TLS
        /**
         * @brief Open a TLS-protected HTTP/2 connection. ALPN MUST
         *        include @c "h2" -- the function configures the option
         *        if it's missing.
         */
        static YarnBall::Task<Http2Connection> connect(
            std::string host,
            std::uint16_t port,
            TlsClientOptions tlsOpts);
#endif

        /**
         * @brief Open a plaintext h2c connection. For cluster-internal
         *        traffic where TLS termination happens upstream.
         */
        static YarnBall::Task<Http2Connection> connectPlain(
            std::string host,
            std::uint16_t port);

        /**
         * @brief Issue one HTTP/2 request on a fresh stream. Suspends
         *        until @c on_stream_close fires for the assigned
         *        stream id.
         */
        YarnBall::Task<Http2Response> request(
            std::string method,
            std::string path,
            std::vector<HttpHeader> headers = {},
            std::string body = {});

        /**
         * @brief Send a GOAWAY frame and close the connection. Pending
         *        per-stream awaiters are resolved with a connection-
         *        level error. Idempotent.
         */
        void close() noexcept;

        Http2Connection(const Http2Connection &) = delete;
        Http2Connection &operator=(const Http2Connection &) = delete;
        Http2Connection(Http2Connection &&) noexcept;
        Http2Connection &operator=(Http2Connection &&) noexcept;
        ~Http2Connection();

        Http2Connection() = default;

        /**
         * @brief Forward-declaration only; the State is defined inside
         *        @c Http2.cpp so this header does not pull
         *        @c <nghttp2/nghttp2.h>. Public so the file-scope
         *        nghttp2 callbacks (which take a @c void* user data)
         *        can reach it without a friend dance.
         */
        struct State;

    private:
        /**
         * @brief Hide the nghttp2 session, the TLS/TCP stream, the
         *        stream map, and the mutexes behind a single
         *        heap-allocated state struct so the header does not
         *        need to know about any of them. shared_ptr because
         *        the driver coroutine outlives the user's call to
         *        @c request and may also outlive a moved-from
         *        @c Http2Connection wrapper briefly.
         */
        std::shared_ptr<State> state;
    };

    /**
     * @class Http2Server
     * @brief HTTP/2 server: bind a port, register routes, run the
     *        accept loop. Each accepted connection runs an nghttp2
     *        server session that multiplexes N concurrent streams;
     *        every fully-received request is dispatched to the
     *        matching route handler and the response is shipped back
     *        on the same stream.
     *
     * API mirrors @ref HttpServer so user code already written
     * against the HTTP/1.1 server can swap in the HTTP/2 variant
     * without rewriting routes. Per project policy this is h2c
     * (plain) in v1; h2-over-TLS follows the same Pipe abstraction
     * as the client.
     */
    class Http2Server final {
    public:
        /**
         * @brief Bind the listening socket synchronously at construction.
         *        Pass @c port == 0 to let the kernel assign one; read
         *        it back via @c localAddress().
         */
        Http2Server(const std::string &host, std::uint16_t port);

        Http2Server(const Http2Server &) = delete;
        Http2Server &operator=(const Http2Server &) = delete;
        Http2Server(Http2Server &&) noexcept;
        Http2Server &operator=(Http2Server &&) noexcept;
        ~Http2Server();

        /**
         * @brief Register a route. Exact match on @p method + @p path.
         *        Last-write-wins on duplicate registration. The
         *        @c HttpRouteHandler type comes from HttpServer so
         *        a route definition can be shared between the h1 and
         *        h2 servers verbatim.
         */
        void route(std::string method,
                    std::string path,
                    HttpRouteHandler handler);

        /**
         * @brief Run the accept loop. Returns when @p stop is
         *        requested. In-flight handlers keep running until
         *        they complete cooperatively.
         */
        YarnBall::Task<void> serve(std::stop_token stop = {});

        /**
         * @return Bound local address (useful when port == 0).
         */
        SocketAddress localAddress() const;

    private:
        struct ServerState;
        std::shared_ptr<ServerState> serverState;
    };


    /**
     * @class Http2ConnectionPool
     * @brief Process-wide pool keyed by @c host:port that holds
     *        long-lived multiplexed @c Http2Connection instances.
     *        Unlike the HTTP/1.1 pool, an h2 connection is reused
     *        for many concurrent requests, not idle-then-claim. The
     *        pool's job is to amortise the TLS+settings handshake
     *        across all requests to the same backend.
     *
     * Acquire returns a shared_ptr; the connection lives until the
     * last reference goes away or the pool evicts it on a dead-
     * connection detection (next request fails the handshake).
     */
    class Http2ConnectionPool final {
    public:
        /**
         * @brief Process-wide default pool used by helpers like
         *        @c http2GetPooled. Constructed lazily.
         */
        static Http2ConnectionPool &defaultPool();

        /**
         * @brief Get or create a connection to @p host:@p port over
         *        plain h2c. Subsequent calls return the same
         *        connection until it is closed (peer GOAWAY,
         *        transport error, or explicit eviction).
         */
        YarnBall::Task<std::shared_ptr<Http2Connection>>
        acquirePlain(std::string host, std::uint16_t port);

#ifdef SOCCER_HAS_TLS
        /**
         * @brief Same as @ref acquirePlain but over TLS with the
         *        given client options. The pool keys per (host, port,
         *        opts hash) so different TLS configurations get their
         *        own connection.
         */
        YarnBall::Task<std::shared_ptr<Http2Connection>>
        acquire(std::string host, std::uint16_t port, TlsClientOptions opts);
#endif

        /**
         * @brief Evict any cached connection to @p host:@p port. The
         *        connection will be reopened on the next acquire.
         *        Useful when the caller observes a transport error
         *        and wants the next request on a fresh socket.
         */
        void evict(std::string_view host, std::uint16_t port);

        /**
         * @return Number of distinct host:port keys currently cached.
         *         Diagnostic helper; not a stable scrape source.
         */
        std::size_t size() const;

    private:
        mutable std::mutex mu;
        std::unordered_map<std::string,
                            std::shared_ptr<Http2Connection>> entries;

        static std::string keyOf(std::string_view host, std::uint16_t port) {
            std::string k(host);
            k.push_back(':');
            k.append(std::to_string(port));
            return k;
        }
    };

#else // !SOCCER_HAS_HTTP2

    /**
     * @class Http2Connection
     * @brief Stub when Soccer is built without nghttp2. Every entry
     *        point throws @c Http2NotImplemented. The signatures
     *        match the real implementation so user code compiles
     *        regardless of build flags.
     */
    class Http2Connection final {
    public:
#ifdef SOCCER_HAS_TLS
        static YarnBall::Task<Http2Connection> connect(
            std::string, std::uint16_t, TlsClientOptions) {
            throw Http2NotImplemented{};
            co_return Http2Connection{};
        }
#endif
        static YarnBall::Task<Http2Connection> connectPlain(std::string, std::uint16_t) {
            throw Http2NotImplemented{};
            co_return Http2Connection{};
        }
        YarnBall::Task<Http2Response> request(
            std::string, std::string, std::vector<HttpHeader> = {}, std::string = {}) {
            throw Http2NotImplemented{};
            co_return Http2Response{};
        }
        void close() noexcept {}
        Http2Connection() = default;
        Http2Connection(const Http2Connection &) = delete;
        Http2Connection &operator=(const Http2Connection &) = delete;
        Http2Connection(Http2Connection &&) noexcept = default;
        Http2Connection &operator=(Http2Connection &&) noexcept = default;
    };

    /**
     * @class Http2Server
     * @brief Stub when Soccer is built without nghttp2. Construction
     *        and every entry point throw @c Http2NotImplemented.
     */
    class Http2Server final {
    public:
        Http2Server(const std::string &, std::uint16_t) {
            throw Http2NotImplemented{};
        }
        void route(std::string, std::string, HttpRouteHandler) {}
        YarnBall::Task<void> serve(std::stop_token = {}) {
            throw Http2NotImplemented{};
            co_return;
        }
        SocketAddress localAddress() const { return {}; }
    };

    /**
     * @class Http2ConnectionPool
     * @brief Stub when Soccer is built without nghttp2.
     */
    class Http2ConnectionPool final {
    public:
        static Http2ConnectionPool &defaultPool() {
            static Http2ConnectionPool p;
            return p;
        }
        YarnBall::Task<std::shared_ptr<Http2Connection>>
        acquirePlain(std::string, std::uint16_t) {
            throw Http2NotImplemented{};
            co_return std::shared_ptr<Http2Connection>{};
        }
#ifdef SOCCER_HAS_TLS
        YarnBall::Task<std::shared_ptr<Http2Connection>>
        acquire(std::string, std::uint16_t, TlsClientOptions) {
            throw Http2NotImplemented{};
            co_return std::shared_ptr<Http2Connection>{};
        }
#endif
        void evict(std::string_view, std::uint16_t) {}
        std::size_t size() const { return 0; }
    };

#endif // SOCCER_HAS_HTTP2

}

#endif // SOCCER_HTTP2_H
