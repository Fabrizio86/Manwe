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
// Connection: close. Production HTTP servers want keep-alive + chunked
// + a routing DSL; this is the minimal "fast path to a working REST
// endpoint" tier.
//

#ifndef SOCCER_HTTP_SERVER_H
#define SOCCER_HTTP_SERVER_H

#include <algorithm>
#include <cctype>
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
#include "Coroutines.h"
#include "HttpClient.h"
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
        HttpServer(const std::string &host, std::uint16_t port)
            : listener(TcpListener::bind(host, port)) {
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
         * @brief Run the accept loop. Per-connection: parse one
         *        request, dispatch to the matching route (or 404),
         *        write the response, close. Returns when @p stop
         *        fires (between accepts) or the listener errors.
         */
        YarnBall::Task<void> serve(std::stop_token stop = {}) {
            // Capture the routes by shared_ptr so the per-connection
            // coroutines can outlive @c this without UAF (the serve()
            // coroutine's frame owns the route table, but per-connection
            // handlers are co_spawn'd and may run beyond serve()'s scope
            // in adversarial shutdown ordering).
            auto routesPtr = std::make_shared<RouteTable>(this->routes);

            auto factory = [routesPtr](TcpStream client) -> YarnBall::Task<void> {
                co_await handleOne(routesPtr, std::move(client));
                co_return;
            };

            co_await tcpServe(std::move(this->listener),
                               std::move(factory),
                               stop);
            co_return;
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
         * @brief Parse one request, dispatch to a route, write the
         *        response, close. Per-connection handler used by
         *        @ref serve.
         */
        static YarnBall::Task<void> handleOne(
            std::shared_ptr<RouteTable> routes,
            TcpStream client) {
            try {
                BufferedReader<TcpStream> r(&client);
                HttpRequest req = co_await parseRequest(r);

                HttpResponse resp;
                auto it = routes->find(routeKey(req.method, req.path));
                if (it == routes->end()) {
                    resp.status = 404;
                    resp.reason = "Not Found";
                    resp.body = "404 Not Found\n";
                } else {
                    resp = co_await it->second(std::move(req));
                }

                co_await writeResponse(client, resp);
            } catch (...) {
                // Swallow per-connection errors -- one bad client should
                // never tear down the server. A real production server
                // would log here.
            }
            co_return;
        }

        /**
         * @brief Read the request line + headers + body off a
         *        @ref BufferedReader. Mirrors HttpClient::parseStatusLine
         *        + parseHeaderLine but for the server side.
         */
        static YarnBall::Task<HttpRequest> parseRequest(
            BufferedReader<TcpStream> &r) {
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
                throw SocketException("HttpServer: malformed request line");
            }
            req.method = line.substr(0, sp1);
            req.path = line.substr(sp1 + 1, sp2 - sp1 - 1);

            // -- Headers until blank line.
            std::size_t headerBytes = line.size();
            while (true) {
                std::string h = co_await r.readLine();
                headerBytes += h.size();
                if (headerBytes > detail::kHttpMaxHeaderBytes) {
                    throw SocketException("HttpServer: header block too large");
                }
                if (h.empty() || h == "\r\n" || h == "\n") break;
                const std::size_t colon = h.find(':');
                if (colon == std::string::npos) continue;
                HttpHeader header;
                header.name = h.substr(0, colon);
                header.value = detail::trimAscii(h.substr(colon + 1));
                req.headers.push_back(std::move(header));
            }

            // -- Body (Content-Length-framed only; if absent, body is empty).
            const std::string clen = req.header("Content-Length");
            if (!clen.empty()) {
                const long long len = std::atoll(clen.c_str());
                if (len < 0 ||
                    static_cast<std::size_t>(len) > detail::kHttpMaxBodyBytes) {
                    throw SocketException("HttpServer: invalid Content-Length");
                }
                auto bytes = co_await r.readExact(static_cast<std::size_t>(len));
                req.body.assign(reinterpret_cast<const char *>(bytes.data()),
                                bytes.size());
            }
            co_return req;
        }

        /**
         * @brief Serialise + write the response. Adds Content-Length
         *        automatically; forces Connection: close.
         */
        static YarnBall::Task<void> writeResponse(TcpStream &client,
                                                    const HttpResponse &resp) {
            std::string head =
                "HTTP/1.1 " + std::to_string(resp.status) + " " +
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
    };

}

#endif // SOCCER_HTTP_SERVER_H
