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
// case-insensitive header lookup, exact method+path routing (no DSL; the
// query string is split off the target and exposed as HttpRequest::query).
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
#include <array>
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
#include "Log.h"

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
        /// The request target without its query string; routes match on this.
        std::string path;
        /// The query string (the part after the first '?', without it); empty if none.
        std::string query;
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
         * @brief HTTP/2 only: the @c :authority and @c :scheme pseudo-headers of the
         *        request (the server also synthesises a @c Host header from the
         *        authority). Empty for HTTP/1, where the Host header carries it.
         */
        std::string authority;
        std::string scheme;

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
     *        treats zero as "unlimited" (an explicit opt-out; the defaults are finite).
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
        /// Connections served at once; further ones get a 503 and are closed. Zero: unlimited.
        std::size_t maxConnections{1024};
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
            : listener(TcpListener::bind(host, port)), bound(listener.localAddress()), opts(options) {
        }

        HttpServer(const HttpServer &) = delete;
        HttpServer &operator=(const HttpServer &) = delete;
        HttpServer(HttpServer &&) noexcept = default;
        HttpServer &operator=(HttpServer &&) noexcept = default;

        ~HttpServer() = default;

        /**
         * @brief Register a route. Exact match on @p method + @p path (the
         *        request target without its query string; methods are
         *        case-sensitive, as in HTTP).
         *        The same (method, path) pair registered twice is
         *        last-write-wins. Routes are snapshotted by @ref serve: register
         *        them first, a later call does not affect a running server.
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
         *        fires (between accepts) or the listener errors. Callable once:
         *        it consumes the listener, a second call throws.
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
            if (this->served) {
                throw SocketException("HttpServer::serve: already called (the listener is consumed by the first call)");
            }
            this->served = true;
            auto routesPtr = std::make_shared<RouteTable>(this->routes);
            return serveHttp(std::move(routesPtr), std::move(this->listener), stop, this->opts);
        }

        /**
         * @return The address the listener bound to. Useful when the
         *         caller passed port 0 and wants the kernel-assigned
         *         port back. Recorded at construction, so it stays valid
         *         after @ref serve has taken the listener over.
         */
        SocketAddress localAddress() const {
            return this->bound;
        }

    private:
        using RouteTable = std::unordered_map<std::string, HttpRouteHandler>;

        /**
         * @brief Canonicalise (method, path) into a single key for the
         *        route table. The method is NOT case-folded: methods are
         *        case-sensitive (RFC 9110 9.1), so "get" is not "GET".
         */
        static std::string routeKey(std::string_view method,
                                     std::string_view path) {
            std::string out;
            out.reserve(method.size() + 1 + path.size());
            out.append(method);
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
                BufferedReader<TcpStream> r(&client, detail::kBufferedReaderDefaultCapacity,
                                            lineBound(options));
                HttpRequest req = co_await parseRequest(r, options, deadline);

                HttpResponse resp;
                const std::string method = req.method;
                auto it = routes->find(routeKey(req.method, req.path));
                if (it == routes->end()) {
                    resp.status = 404;
                    resp.reason = "Not Found";
                    resp.body = "404 Not Found\n";
                } else {
                    bool handlerFailed = false;
                    try {
                        resp = co_await it->second(std::move(req));
                    } catch (const std::exception &e) {
                        handlerFailed = true;
                        YarnBall::log::error("HttpServer: route handler threw",
                                             {YarnBall::log::str("what", e.what())});
                    } catch (...) {
                        handlerFailed = true;
                        YarnBall::log::error("HttpServer: route handler threw a non-standard exception");
                    }
                    // Nothing has been written yet, so the client still gets an answer
                    // (a fixed one: the exception text stays in the log).
                    if (handlerFailed) throw HttpProtocolError(500, "HttpServer: route handler failed");
                }

                deadline.arm(client.fd(), options.writeTimeout);
                co_await writeResponse(client, resp, method);
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
                    // The refused request's bytes may still be unread; closing on them can turn the
                    // answer into a reset that discards it. Half-close and drain briefly instead.
                    client.shutdownWrite();
                    deadline.arm(client.fd(), std::chrono::milliseconds{250});
                    std::array<std::byte, 1024> sink{};
                    while (co_await client.read(sink) != 0) {
                    }
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
                case 417: reason = "Expectation Failed"; break;
                case 414: reason = "URI Too Long"; break;
                case 431: reason = "Request Header Fields Too Large"; break;
                case 500: reason = "Internal Server Error"; break;
                case 501: reason = "Not Implemented"; break;
                case 503: reason = "Service Unavailable"; break;
                case 505: reason = "HTTP Version Not Supported"; break;
                default: break;
            }
            const std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                                     "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            co_await client.write(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(head.data()), head.size()));
            co_return;
        }

        /**
         * @brief Read one line of the request head and return it without its CRLF.
         *        Only CRLF ends a line (a bare LF is a request-smuggling hazard
         *        when a proxy in front reads it differently), and a CR, LF or NUL
         *        anywhere else in it makes the request malformed. EOF before any
         *        byte sets @p eof; EOF in the middle of a line is a
         *        truncated one (both are plain @c SocketException, nobody is
         *        left to answer). A line longer than the reader's bound is
         *        answered with 414 (request line) or 431 (header field).
         */
        /// Longest single head line the reader accepts: the header budget itself
        /// (so maxHeaderBytes above 64 KiB is honoured), clamped to [1 KiB, 16 MiB];
        /// zero (unlimited total) keeps the reader's default per-line bound.
        static std::size_t lineBound(const HttpServerOptions &o) noexcept {
            if (o.maxHeaderBytes == 0) return detail::kBufferedReaderMaxLine;
            return std::clamp<std::size_t>(o.maxHeaderBytes, 1024, std::size_t{16} << 20);
        }

        static YarnBall::Task<std::string> readHeadLine(BufferedReader<TcpStream> &r,
                                                         bool requestLine,
                                                         std::size_t &headerBytes,
                                                         bool &eof) {
            eof = false;
            std::string line;
            try {
                line = co_await r.readLine();
            } catch (const LineTooLongException &) {
                throw HttpProtocolError(requestLine ? 414 : 431,
                                        "HttpServer: request line or header field too long");
            }
            headerBytes += line.size();
            if (line.empty()) {
                eof = true;
                co_return line;
            }
            if (line.back() != '\n') {
                throw SocketException("HttpServer: connection closed inside the request head");
            }
            line.pop_back();
            if (line.empty() || line.back() != '\r') {
                throw HttpProtocolError(400, "HttpServer: bare LF in request head");
            }
            line.pop_back();
            if (line.find_first_of(std::string_view("\r\n\0", 3)) != std::string::npos) {
                throw HttpProtocolError(400, "HttpServer: stray control character in request head");
            }
            co_return line;
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
            std::size_t headerBytes = 0;

            // -- Request line: "GET /path HTTP/1.1\r\n"
            bool eof = false;
            std::string line = co_await readHeadLine(r, true, headerBytes, eof);
            if (eof) {
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
            const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
            const std::string_view version = std::string_view(line).substr(sp2 + 1);
            // The method is a token; the target is non-empty with no space, control
            // character or fragment in it.
            if (!detail::isHttpToken(req.method) || target.empty() ||
                detail::hasTargetCtl(target) || target.find('#') != std::string::npos) {
                throw HttpProtocolError(400, "HttpServer: malformed request line");
            }
            // Routes match on the path; the query string travels separately.
            detail::splitQuery(target, req.path, req.query);
            if (version.size() != 8 || version.compare(0, 5, "HTTP/") != 0 ||
                !std::isdigit(static_cast<unsigned char>(version[5])) || version[6] != '.' ||
                !std::isdigit(static_cast<unsigned char>(version[7]))) {
                throw HttpProtocolError(400, "HttpServer: malformed request line");
            }
            if (version != "HTTP/1.1" && version != "HTTP/1.0") {
                throw HttpProtocolError(505, "HttpServer: HTTP version not supported");
            }
            const bool http11 = version == "HTTP/1.1";

            // -- Headers until blank line.
            if (options.maxHeaderBytes != 0 && headerBytes > options.maxHeaderBytes) {
                throw HttpProtocolError(431, "HttpServer: header block too large");
            }
            while (true) {
                const std::string h = co_await readHeadLine(r, false, headerBytes, eof);
                if (options.maxHeaderBytes != 0 && headerBytes > options.maxHeaderBytes) {
                    throw HttpProtocolError(431, "HttpServer: header block too large");
                }
                if (eof) {
                    // EOF inside the header block: the request is truncated.
                    throw SocketException("HttpServer: connection closed inside header block");
                }
                if (h.empty()) break; // the blank line that ends the block
                const std::size_t colon = h.find(':');
                // No colon, an obs-fold continuation, or a name that is not an RFC
                // 9110 token (whitespace before the colon included) is a malformed
                // header; do not skip it, since a proxy might interpret it differently.
                if (colon == std::string::npos ||
                    !detail::isHttpToken(std::string_view(h).substr(0, colon))) {
                    throw HttpProtocolError(400, "HttpServer: malformed header line");
                }
                if (options.maxHeaderCount != 0 && req.headers.size() >= options.maxHeaderCount) {
                    throw HttpProtocolError(431, "HttpServer: too many header fields");
                }
                HttpHeader header;
                header.name = h.substr(0, colon);
                header.value = detail::trimAscii(h.substr(colon + 1));
                if (detail::hasFieldValueCtl(header.value)) {
                    throw HttpProtocolError(400, "HttpServer: control character in header value");
                }
                req.headers.push_back(std::move(header));
            }

            // HTTP/1.1 requires exactly one Host (RFC 9112 3.2).
            if (http11) {
                int hosts = 0;
                for (const auto &hd : req.headers) {
                    if (detail::iequalsAscii(hd.name, "Host")) ++hosts;
                }
                if (hosts != 1) {
                    throw HttpProtocolError(400, "HttpServer: HTTP/1.1 request needs exactly one Host header");
                }
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
            // connection). EVERY header is scanned -- an empty
            // Transfer-Encoding ahead of a "chunked" one is still a
            // Transfer-Encoding -- and the combination with a
            // Content-Length is a 400, any other use a 501.
            if (detail::hasTransferEncoding(req.headers)) {
                for (const auto &hd : req.headers) {
                    if (detail::iequalsAscii(hd.name, "Content-Length")) {
                        throw HttpProtocolError(400, "HttpServer: Transfer-Encoding with Content-Length");
                    }
                }
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
            // Expect: 100-continue is the one expectation understood (RFC 9110
            // 10.1.1); any other value is answered with 417.
            for (const auto &hd : req.headers) {
                if (detail::iequalsAscii(hd.name, "Expect") &&
                    !detail::iequalsAscii(hd.value, "100-continue")) {
                    throw HttpProtocolError(417, "HttpServer: unsupported Expect value");
                }
            }
            if (hasLen && len != 0) {
                // The headers are in: from here on the body clock runs.
                deadline.arm(r.raw()->fd(), options.bodyTimeout);
                // An interim 100 is an HTTP/1.1 construct: never sent to a 1.0 client.
                if (http11 && detail::iequalsAscii(req.header("Expect"), "100-continue")) {
                    static constexpr char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
                    co_await r.raw()->write(std::span<const std::byte>(
                        reinterpret_cast<const std::byte *>(kContinue), sizeof(kContinue) - 1));
                }
                // Straight into the string, a bounded chunk at a time, so the body is
                // not held twice.
                constexpr std::size_t kBodyChunk = 64 * 1024;
                req.body.reserve(std::min<std::size_t>(len, kBodyChunk));
                while (req.body.size() < len) {
                    const std::size_t want = std::min(kBodyChunk, len - req.body.size());
                    auto bytes = co_await r.readExact(want);
                    if (bytes.size() != want) {
                        throw SocketException("HttpServer: truncated request body");
                    }
                    req.body.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
                }
            }
            deadline.disarm();
            co_return req;
        }

        /**
         * @brief Serialise + write the response. Adds Content-Length
         *        automatically; forces Connection: close. Length and connection
         *        headers the handler set are dropped (the server frames the
         *        message itself). A response to HEAD, and any 1xx, 204 or 304,
         *        carries no body: HEAD gets the Content-Length the GET would have
         *        had, 304 only one the handler stated, 1xx and 204 none.
         */
        static YarnBall::Task<void> writeResponse(TcpStream &client,
                                                    const HttpResponse &resp,
                                                    std::string_view method) {
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
            bool unsafe = detail::hasFieldValueCtl(resp.reason);
            for (const auto &h : resp.headers) {
                if (!detail::isHttpToken(h.name) || detail::hasFieldValueCtl(h.value)) {
                    unsafe = true;
                }
            }
            if (unsafe) {
                // Nothing has been written yet, so handleOne can still answer with a
                // plain 500 instead of dropping the connection.
                throw HttpProtocolError(500, "HttpServer: control characters in response head");
            }
            const std::string reason = resp.reason.empty()
                ? std::string(detail::reasonPhrase(status)) : resp.reason;
            std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
            // User-supplied headers, then the auto-added ones.
            std::string statedLength;
            for (const auto &h : resp.headers) {
                if (detail::iequalsAscii(h.name, "Content-Length") && statedLength.empty()) {
                    statedLength = h.value;
                }
                if (detail::isServerManagedResponseHeader(h.name)) continue;
                head += h.name + ": " + h.value + "\r\n";
            }
            bool sendBody = true;
            const std::string length = detail::framedContentLength(
                status, method == "HEAD", statedLength, resp.body.size(), sendBody);
            if (!length.empty()) head += "Content-Length: " + length + "\r\n";
            head += "Connection: close\r\n\r\n";

            co_await client.write(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(head.data()),
                head.size()));
            if (sendBody && !resp.body.empty()) {
                co_await client.write(std::span<const std::byte>(
                    reinterpret_cast<const std::byte *>(resp.body.data()),
                    resp.body.size()));
            }
            co_return;
        }

        TcpListener listener;
        SocketAddress bound; ///< listener's address, read before serve() moves it away
        RouteTable routes;
        HttpServerOptions opts;
        bool served{false};
    };

}

#endif // SOCCER_HTTP_SERVER_H
