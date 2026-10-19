//
// Created by Fabrizio Paino on 2026-05-16.
//
// HttpServer: minimal HTTP/1.1 server class. Wraps tcpServe with
// request parsing and a route table; the user supplies handlers
// returning Task<HttpResponse>. Reuses BufferedReader for the wire
// protocol so server-side parsing matches the client-side semantics
// in HttpClient.
//
// Scope: GET / POST / arbitrary method, Content-Length-framed bodies,
// case-insensitive header lookup, exact method+path routing (no DSL).
// Chunked transfer encoding is not supported (matches HttpClient).
// Keep-alive is not supported either -- every response carries
// Connection: close, so there is no idle keep-alive phase to time out;
// the limits in HttpServerOptions bound every phase of the one request. Production HTTP servers want keep-alive + chunked
// + a routing DSL; this is the minimal "fast path to a working REST
// endpoint" tier.
//

#ifndef SOCCER_HTTP_SERVER_H
#define SOCCER_HTTP_SERVER_H

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "BufferedReader.h"
#include "ConnectionLimiter.h"
#include "Coroutines.h"
#include "HttpClient.h"
#include "IoDeadline.h"
#include "SocketException.h"
#include "TcpListener.h"
#include "TcpServer.h"
#include "TcpStream.h"

namespace Soccer {

    /**
     * @class HttpRequest
     * @brief Parsed HTTP request: method, target path, headers, body.
     *        Body is read in full before the handler runs (only fits
     *        if Content-Length is reasonable -- bounded by
     *        @ref detail::kHttpMaxBodyBytes).
     */
    struct HttpRequest {
        std::string method;
        std::string path;
        std::vector<HttpHeader> headers;
        std::string body;

        /**
         * @brief Fires when the request is no longer wanted. Set by the HTTP/2
         *        server when the client resets the stream or the connection
         *        goes away before the handler has answered; a long-running
         *        handler can poll it (or register a @c std::stop_callback) and
         *        stop early. Default-constructed (never fires) for HTTP/1.
         */
        std::stop_token cancel;

        /**
         * @brief Case-insensitive header lookup. Returns empty string
         *        if @p name is not present.
         */
        std::string header(std::string_view name) const {
            for (const auto &h : this->headers) {
                if (detail::iequalsAscii(h.name, name)) return h.value;
            }
            return {};
        }
    };

    /**
     * @struct HttpServerOptions
     * @brief Resource limits for @ref HttpServer. Every duration and size here
     *        treats zero as "unlimited".
     *
     * The timeouts are slowloris defences: a peer that connects and then trickles
     * (or never sends) bytes is cut off instead of holding a connection and its
     * memory forever.
     */
    struct HttpServerOptions {
        /// From accept until the request line and all headers have arrived.
        std::chrono::milliseconds headerTimeout{10000};
        /// For the request body, once the headers are in.
        std::chrono::milliseconds bodyTimeout{30000};
        /// For writing the response (a client that never reads it).
        std::chrono::milliseconds writeTimeout{30000};
        /// Connections served at once; further ones get a 503 and are closed.
        std::size_t maxConnections{0};
        /// Request line plus header block, in bytes (431 beyond).
        std::size_t maxHeaderBytes{detail::kHttpMaxHeaderBytes};
        /// Header fields per request (431 beyond).
        std::size_t maxHeaderCount{100};
        /// Request body, judged from Content-Length before any body byte is read (413 beyond).
        std::size_t maxBodyBytes{detail::kHttpMaxBodyBytes};
    };

    /**
     * @class HttpProtocolError
     * @brief A request the server refuses; carries the HTTP status to answer with.
     */
    class HttpProtocolError : public SocketException {
    public:
        HttpProtocolError(int status, const std::string &message)
            : SocketException(message), httpStatus(status) {
        }

        int status() const noexcept { return this->httpStatus; }

    private:
        int httpStatus;
    };

    /**
     * @typedef HttpRouteHandler
     * @brief Per-route handler signature. Returns a @c Task that
     *        resumes with the response to send.
     */
    using HttpRouteHandler =
        std::function<YarnBall::Task<HttpResponse>(HttpRequest)>;

    /**
     * @class HttpServer
     * @brief Tiny HTTP/1.1 server. Bind a port, register routes via
     *        @ref route, call @c serve() to run the accept loop.
     *
     * Move-only. The server holds its route table by value, so the
     * handlers must be std::function-compatible (lambdas with shared
     * state work fine; capture state via shared_ptr).
     */
    class HttpServer final {
    public:
        /**
         * @brief Construct an HTTP server bound to @p host:@p port.
         *        Bind happens synchronously. Pass @c port == 0 to let
         *        the kernel pick; read the bound port back from
         *        @ref localAddress.
         */
        HttpServer(const std::string &host, std::uint16_t port, HttpServerOptions options = {})
            : listener(TcpListener::bind(host, port)), opts(options) {
        }

        HttpServer(const HttpServer &) = delete;
        HttpServer &operator=(const HttpServer &) = delete;
        HttpServer(HttpServer &&) noexcept = default;
        HttpServer &operator=(HttpServer &&) noexcept = default;

        ~HttpServer() = default;

        /**
         * @brief Register a route. Exact match on @p method + @p path.
         *        The same (method, path) pair registered twice is
         *        last-write-wins.
         */
        void route(std::string method,
                   std::string path,
                   HttpRouteHandler handler) {
            this->routes[routeKey(method, path)] = std::move(handler);
        }

        /**
         * @brief Replace the resource limits. Takes effect for the next
         *        @ref serve call; a running accept loop keeps the limits it
         *        started with.
         */
        void setOptions(const HttpServerOptions &options) { this->opts = options; }

        /**
         * @brief Run the accept loop. Per-connection: parse one
         *        request, dispatch to the matching route (or 404),
         *        write the response, close. Returns when @p stop
         *        fires (between accepts) or the listener errors.
         */
        YarnBall::Task<void> serve(std::stop_token stop = {}) {
            // Deliberately NOT a coroutine itself. Task is lazy: a
            // member-function coroutine's body -- including a line as
            // early as "copy this->routes into a shared_ptr", which is
            // exactly what this function used to do first -- only
            // executes on the coroutine's FIRST RESUME, not at the
            // call to serve() itself. A caller that does
            //   coSpawn(server.serve(stop)); ...; src.request_stop();
            // and then returns/destroys `server` without waiting for
            // the spawned task to actually finish races that first
            // resume against the HttpServer object's destruction: if a
            // worker thread resumes the coroutine after `server` is
            // already gone, the old code's capture of this->routes
            // (and its std::move(this->listener) a few lines later)
            // read from freed memory -- the exact use-after-free
            // (SIGBUS while copy-constructing a dangling shared_ptr)
            // that surfaced in Soccer::Http2Server::serve's identical
            // pattern; see the long comment there (Http2.cpp) for the
            // full diagnosis.
            //
            // Making serve() an ordinary (non-coroutine) function
            // fixes this at the root: the captures below run
            // synchronously, on the caller's own thread, while `this`
            // is unquestionably still valid (we are inside a live
            // member-function call). The Task that serveHttp returns
            // then owns everything it needs by value and has no
            // dependency on `this` for the rest of its lifetime,
            // however much later or on whichever thread it actually
            // gets resumed.
            auto routesPtr = std::make_shared<RouteTable>(this->routes);
            return serveHttp(std::move(routesPtr), std::move(this->listener), stop, this->opts);
        }

        /**
         * @return The address the listener bound to. Useful when the
         *         caller passed port 0 and wants the kernel-assigned
         *         port back.
         */
        SocketAddress localAddress() const {
            return this->listener.localAddress();
        }

    private:
        using RouteTable = std::unordered_map<std::string, HttpRouteHandler>;

        /**
         * @brief Canonicalise (method, path) into a single key for the
         *        route table. Method is upper-cased so handlers don't
         *        need to be case-aware.
         */
        static std::string routeKey(std::string_view method,
                                     std::string_view path) {
            std::string out;
            out.reserve(method.size() + 1 + path.size());
            for (char c : method) {
                out.push_back(static_cast<char>(std::toupper(
                    static_cast<unsigned char>(c))));
            }
            out.push_back(' ');
            out.append(path);
            return out;
        }

        /**
         * @brief Free-standing accept loop, called by @ref serve.
         *        Owns @p routesPtr and @p listener by value so it has
         *        no dependency on the HttpServer object's lifetime --
         *        see the long comment on @ref serve for why that
         *        matters. The factory lambda passed to tcpServe is
         *        NOT a coroutine itself; it forwards into the free
         *        @c handleOne coroutine below, whose own frame owns
         *        @c routesPtr and the accepted client by value (the
         *        same lambda-coroutine-UAF avoidance @c handleOne's
         *        own docblock already explains).
         */
        static YarnBall::Task<void> serveHttp(
            std::shared_ptr<RouteTable> routesPtr,
            TcpListener listener,
            std::stop_token stop,
            HttpServerOptions options) {
            auto limiter = std::make_shared<ConnectionLimiter>(options.maxConnections);
            auto factory = [routesPtr, limiter, options](TcpStream client) -> YarnBall::Task<void> {
                return handleOne(routesPtr, std::move(client), options, limiter->tryAcquire());
            };
            co_await tcpServe(std::move(listener), std::move(factory), stop);
            co_return;
        }

        /**
         * @brief Parse one request, dispatch to a route, write the
         *        response, close. Per-connection handler used by
         *        @ref serveHttp.
         */
        static YarnBall::Task<void> handleOne(
            std::shared_ptr<RouteTable> routes,
            TcpStream client,
            HttpServerOptions options,
            ConnectionLimiter::Slot slot) {
            // Declared after `client` (a parameter, so it outlives locals) and
            // therefore disarmed before the fd is closed.
            IoDeadline deadline;
            int errorStatus = 0;
            try {
                if (!slot) {
                    throw HttpProtocolError(503, "HttpServer: connection limit reached");
                }
                deadline.arm(client.fd(), options.headerTimeout);
                BufferedReader<TcpStream> r(&client);
                HttpRequest req = co_await parseRequest(r, options, deadline);

                HttpResponse resp;
                auto it = routes->find(routeKey(req.method, req.path));
                if (it == routes->end()) {
                    resp.status = 404;
                    resp.reason = "Not Found";
                    resp.body = "404 Not Found\n";
                } else {
                    resp = co_await it->second(std::move(req));
                }

                deadline.arm(client.fd(), options.writeTimeout);
                co_await writeResponse(client, resp);
            } catch (const HttpProtocolError &e) {
                errorStatus = e.status();
            } catch (...) {
                // Swallow per-connection errors -- one bad client should
                // never tear down the server. A real production server
                // would log here.
            }
            if (errorStatus != 0 && !deadline.expired()) {
                try {
                    deadline.arm(client.fd(), std::chrono::seconds{2});
                    co_await writeError(client, errorStatus);
                } catch (...) {
                    // The peer is gone; nothing to tell it.
                }
            }
            co_return;
        }

        /**
         * @brief Best-effort bodyless error response for a refused request.
         */
        static YarnBall::Task<void> writeError(TcpStream &client, int status) {
            const char *reason = "Bad Request";
            switch (status) {
                case 413: reason = "Content Too Large"; break;
                case 431: reason = "Request Header Fields Too Large"; break;
                case 501: reason = "Not Implemented"; break;
                case 503: reason = "Service Unavailable"; break;
                default: break;
            }
            const std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                                     "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            co_await client.write(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(head.data()), head.size()));
            co_return;
        }

        /**
         * @brief Read the request line + headers + body off a
         *        @ref BufferedReader. Mirrors HttpClient::parseStatusLine
         *        + parseHeaderLine but for the server side.
         */
        static YarnBall::Task<HttpRequest> parseRequest(
            BufferedReader<TcpStream> &r,
            const HttpServerOptions &options,
            IoDeadline &deadline) {
            HttpRequest req;

            // -- Request line: "GET /path HTTP/1.1\r\n"
            std::string line = co_await r.readLine();
            if (line.empty()) {
                throw SocketException("HttpServer: empty request");
            }
            const std::size_t sp1 = line.find(' ');
            const std::size_t sp2 = (sp1 == std::string::npos)
                ? std::string::npos
                : line.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) {
                throw HttpProtocolError(400, "HttpServer: malformed request line");
            }
            req.method = line.substr(0, sp1);
            req.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
            if (req.method.empty() || req.path.empty() ||
                line.compare(sp2 + 1, 7, "HTTP/1.") != 0) {
                throw HttpProtocolError(400, "HttpServer: malformed request line");
            }

            // -- Headers until blank line.
            std::size_t headerBytes = line.size();
            if (options.maxHeaderBytes != 0 && headerBytes > options.maxHeaderBytes) {
                throw HttpProtocolError(431, "HttpServer: header block too large");
            }
            while (true) {
                std::string h = co_await r.readLine();
                headerBytes += h.size();
                if (options.maxHeaderBytes != 0 && headerBytes > options.maxHeaderBytes) {
                    throw HttpProtocolError(431, "HttpServer: header block too large");
                }
                if (h.empty()) {
                    // EOF inside the header block: the request is truncated.
                    throw SocketException("HttpServer: connection closed inside header block");
                }
                if (h == "\r\n" || h == "\n") break;
                const std::size_t colon = h.find(':');
                // No colon (or an obs-fold continuation / whitespace before
                // the colon) is a malformed header; do not skip it, since
                // a proxy might interpret it differently.
                if (colon == std::string::npos || colon == 0 ||
                    std::isspace(static_cast<unsigned char>(h[0])) ||
                    std::isspace(static_cast<unsigned char>(h[colon - 1]))) {
                    throw HttpProtocolError(400, "HttpServer: malformed header line");
                }
                if (options.maxHeaderCount != 0 && req.headers.size() >= options.maxHeaderCount) {
                    throw HttpProtocolError(431, "HttpServer: too many header fields");
                }
                HttpHeader header;
                header.name = h.substr(0, colon);
                header.value = detail::trimAscii(h.substr(colon + 1));
                req.headers.push_back(std::move(header));
            }

            // -- Body (Content-Length-framed only; if absent, body is empty).
            //
            // Transfer-Encoding is checked and rejected BEFORE looking
            // at Content-Length, not silently ignored: chunked framing
            // isn't implemented (documented limitation), but silently
            // treating a Transfer-Encoding: chunked request as if it
            // had no body would drop the client's actual body bytes
            // unread on the wire with no error anywhere. Worse, RFC
            // 9112 6.1 requires REJECTING a request that carries both
            // Transfer-Encoding and Content-Length outright, because a
            // server that picks one while a front-end proxy in front
            // of it picks the other is exactly the CL.TE request-
            // smuggling primitive (the two disagree about where this
            // request ends and the next one begins on the same
            // connection). Throwing here for ANY Transfer-Encoding
            // closes both the data-loss bug and the smuggling gap in
            // one check, without needing to detect the conflict case
            // specially.
            if (!req.header("Transfer-Encoding").empty()) {
                throw HttpProtocolError(501, "HttpServer: Transfer-Encoding not supported");
            }
            std::size_t len = 0;
            bool hasLen = false;
            try {
                // No library cap here: the operator's maxBodyBytes decides (and may be 0).
                hasLen = detail::strictContentLength(req.headers, len, 0);
            } catch (const std::runtime_error &e) {
                throw HttpProtocolError(400, e.what());
            }
            if (hasLen && options.maxBodyBytes != 0 && len > options.maxBodyBytes) {
                throw HttpProtocolError(413, "HttpServer: request body too large");
            }
            if (hasLen && len != 0) {
                // The headers are in: from here on the body clock runs.
                deadline.arm(r.raw()->fd(), options.bodyTimeout);
                if (detail::iequalsAscii(req.header("Expect"), "100-continue")) {
                    static constexpr char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
                    co_await r.raw()->write(std::span<const std::byte>(
                        reinterpret_cast<const std::byte *>(kContinue), sizeof(kContinue) - 1));
                }
                auto bytes = co_await r.readExact(len);
                if (bytes.size() != len) {
                    throw SocketException("HttpServer: truncated request body");
                }
                req.body.assign(reinterpret_cast<const char *>(bytes.data()),
                                bytes.size());
            }
            deadline.disarm();
            co_return req;
        }

        /**
         * @brief Serialise + write the response. Adds Content-Length
         *        automatically; forces Connection: close.
         */
        static YarnBall::Task<void> writeResponse(TcpStream &client,
                                                    const HttpResponse &resp) {
            // Defends against a route handler that explicitly sets an
            // out-of-range status (not just the "forgot to set it"
            // case the 200 default handles): a status outside
            // [100, 599] is not a valid HTTP status line and most
            // clients/proxies cannot parse it, so degrade to 500
            // rather than putting an unparseable status line on the
            // wire.
            const int status = (resp.status >= 100 && resp.status <= 599)
                ? resp.status : 500;
            // Reject CR/LF/NUL in anything written into the head, so a
            // handler echoing untrusted data cannot split the response.
            bool unsafe = detail::hasCtlLineBreak(resp.reason);
            for (const auto &h : resp.headers) {
                if (detail::hasCtlLineBreak(h.name) || detail::hasCtlLineBreak(h.value) ||
                    h.name.find(':') != std::string::npos) {
                    unsafe = true;
                }
            }
            if (unsafe) {
                throw SocketException("HttpServer: control characters in response head");
            }
            std::string head =
                "HTTP/1.1 " + std::to_string(status) + " " +
                (resp.reason.empty() ? std::string("OK") : resp.reason) +
                "\r\n";
            // User-supplied headers, then the auto-added ones.
            bool hasContentLength = false;
            for (const auto &h : resp.headers) {
                if (detail::iequalsAscii(h.name, "Content-Length")) {
                    hasContentLength = true;
                }
                head += h.name + ": " + h.value + "\r\n";
            }
            if (!hasContentLength) {
                head += "Content-Length: " +
                        std::to_string(resp.body.size()) + "\r\n";
            }
            head += "Connection: close\r\n\r\n";

            co_await client.write(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(head.data()),
                head.size()));
            if (!resp.body.empty()) {
                co_await client.write(std::span<const std::byte>(
                    reinterpret_cast<const std::byte *>(resp.body.data()),
                    resp.body.size()));
            }
            co_return;
        }

        TcpListener listener;
        RouteTable routes;
        HttpServerOptions opts;
    };

}

#endif // SOCCER_HTTP_SERVER_H
