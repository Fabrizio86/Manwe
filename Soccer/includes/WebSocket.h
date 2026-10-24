//
// Created by Fabrizio Paino on 2026-05-16.
//
// WebSocket (RFC 6455) -- text + binary messaging on top of TcpStream.
//
// Scope of this round:
//  - Server-side handshake from an already-accepted TcpStream
//  - Client-side handshake against ws://host:port/path
//  - Text and binary messages
//  - Automatic Ping/Pong
//  - Graceful Close handshake
//  - Fragmented messages (continuation frames) are reassembled, bounded by
//    WsOptions::maxMessageBytes
//  - No extensions (permessage-deflate etc.)
//  - Subprotocols: offered by the client (WsOptions::subprotocols), chosen by the
//    server's handshake hook (WsOptions::onHandshake)
//
// API: one verb per operation. Receive returns the next message;
// internal control frames (Ping/Close) are handled transparently so
// user code only sees data frames. The only configuration is the optional
// WsOptions (timeouts, size caps, Origin policy).
//

#ifndef SOCCER_WEBSOCKET_H
#define SOCCER_WEBSOCKET_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Coroutines.h"
#include "HttpClient.h" // HttpHeader
#include "TcpListener.h"
#include "TcpStream.h"

namespace Soccer {

    /**
     * @brief Frame opcode classification, returned by @c receive.
     */
    enum class WsMessageType { Text, Binary };

    /**
     * @struct WsMessage
     * @brief One application message off a WebSocket connection.
     *        Payload is held as raw bytes; text payloads are
     *        re-interpretable via @ref text without a copy.
     */
    struct WsMessage {
        WsMessageType type;
        std::vector<std::byte> payload;

        /**
         * @brief Zero-copy view of the payload as UTF-8 text. Only
         *        meaningful when @c type == @c Text; for @c Binary
         *        the bytes may not be valid UTF-8.
         */
        std::string_view text() const noexcept {
            return std::string_view(
                reinterpret_cast<const char *>(this->payload.data()),
                this->payload.size());
        }
    };

    /**
     * @class WsException
     * @brief Thrown by WebSocket operations on protocol violations,
     *        handshake failures, or peer close.
     */
    class WsException : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /**
     * @struct WsHandshakeInfo
     * @brief What the opening handshake carried. On a server connection it is the
     *        upgrade request: its target, every header (cookies, Authorization, ...)
     *        and the subprotocols the client offered. On a client connection it is
     *        the requested target plus the server's 101 response headers. Handed to
     *        @ref WsOptions::onHandshake before the 101 goes out, and kept on the
     *        connection (@ref WsConnection::handshake) for the handler.
     */
    struct WsHandshakeInfo {
        /// Request target of the upgrade (path and query). On a server it is always
        /// origin-form: an absolute-form target is reduced to its path before the hook sees it.
        std::string path;
        /// Every header of the request (server) or of the 101 response (client), in wire order.
        std::vector<HttpHeader> headers;
        /// Server: the Sec-WebSocket-Protocol tokens the client offered, in order.
        std::vector<std::string> offeredSubprotocols;
        /// The subprotocol both sides settled on; empty when none was negotiated.
        std::string subprotocol;

        /// Case-insensitive lookup of the first header named @p name; empty if absent.
        std::string header(std::string_view name) const {
            for (const auto &h : this->headers) {
                if (detail::iequalsAscii(h.name, name)) return h.value;
            }
            return {};
        }
    };

    /**
     * @struct WsHandshakeDecision
     * @brief Answer of @ref WsOptions::onHandshake: accept (optionally choosing a
     *        subprotocol) or refuse with an HTTP status.
     */
    struct WsHandshakeDecision {
        /// 0 accepts the upgrade; a value in 400..599 refuses it with that status
        /// (e.g. 401 or 403) and no body. Anything else counts as 403.
        int status{0};
        /// Accepting: the subprotocol to answer with. Must be one of
        /// @ref WsHandshakeInfo::offeredSubprotocols, or empty for none.
        std::string subprotocol;

        static WsHandshakeDecision accept(std::string subprotocol = {}) {
            return WsHandshakeDecision{0, std::move(subprotocol)};
        }

        static WsHandshakeDecision reject(int status = 403) {
            return WsHandshakeDecision{status, {}};
        }
    };

    /**
     * @struct WsOptions
     * @brief Limits and policy for a WebSocket endpoint. Durations and sizes
     *        treat zero as "unlimited" (an explicit opt-out; the defaults are finite).
     */
    struct WsOptions {
        /// Client: time allowed to resolve the host and establish the TCP connection.
        std::chrono::milliseconds connectTimeout{10000};
        /// Time allowed for the opening handshake (request in, 101 out; or on the client, request out, 101 in).
        std::chrono::milliseconds handshakeTimeout{10000};
        /// Once a frame has started arriving (and between the fragments of one
        /// message), time allowed for the rest of it. Cuts off a peer that sends
        /// half a frame and stalls.
        std::chrono::milliseconds frameTimeout{30000};
        /// Time allowed between messages with nothing arriving; a connection quiet for
        /// longer is dropped, so peers that stay connected should ping. Zero turns the
        /// limit off (a quiet connection is normal for WebSocket, but a dead one is then
        /// never reaped).
        std::chrono::milliseconds idleTimeout{300000};
        /// Time allowed for one frame write to drain (a peer that stops reading).
        std::chrono::milliseconds writeTimeout{30000};
        /// Handshake request/response size, headers included.
        std::size_t maxHandshakeBytes{16 * 1024};
        /// One message, reassembled from its fragments (close code 1009 beyond).
        std::size_t maxMessageBytes{16 * 1024 * 1024};

        /// Server (@ref wsServe): connections served at once, handshake included; a
        /// connection accepted beyond it is closed at once. Zero means unlimited.
        std::size_t maxConnections{1024};

        /// Pings answered per second; further ones in the same second get no Pong (RFC
        /// 6455 5.5.2 allows that), so a ping flood costs the peer more than it costs us.
        /// Zero answers every Ping.
        std::size_t maxPingsPerSecond{50};

        /// Server: Origin header values (exact, case-insensitive) a browser may connect from,
        /// e.g. "https://app.example.com". Empty means no allow-list.
        std::vector<std::string> allowedOrigins;
        /// Server: optional extra policy, called with the Origin header value when one is
        /// present. Return false to refuse. Applied in addition to @ref allowedOrigins.
        std::function<bool(std::string_view origin)> originCheck;
        /// Server: whether a handshake with no Origin header (non-browser clients) is accepted
        /// when an allow-list or @ref originCheck is configured.
        bool allowMissingOrigin{true};
        /// Server: strict Origin mode. A handshake carrying an Origin header is refused
        /// unless @ref allowedOrigins lists it or @ref originCheck accepts it -- with
        /// neither configured, every browser Origin is refused (only non-browser clients
        /// get in). Off by default for compatibility: with no allow-list a server accepts
        /// any Origin, which lets a page on another site open a socket that carries the
        /// visitor's cookies (cross-site WebSocket hijacking). Turn it on, or set an
        /// allow-list, for any endpoint that authenticates by cookie.
        bool strictOrigin{false};
        /// Server: called with the parsed upgrade request after the Origin policy has
        /// passed and before the 101 is sent. It can refuse (401, 403, ...) and choose
        /// the subprotocol, from the path, cookies, Authorization header and so on.
        /// An exception from it refuses the upgrade with 500. Unset: accept.
        std::function<WsHandshakeDecision(const WsHandshakeInfo &)> onHandshake;

        /// Client: subprotocols to offer in Sec-WebSocket-Protocol, in preference order.
        /// A 101 that names a subprotocol not in this list (or any when it is empty) is
        /// rejected, as is one that carries Sec-WebSocket-Extensions (none are offered).
        std::vector<std::string> subprotocols;
    };

    /**
     * @class WsConnection
     * @brief Coroutine-driven WebSocket endpoint. Move-only.
     *
     * Lifecycle:
     *  - Server: accept the TcpStream from a listener, then call
     *    @ref serverHandshake to consume the upgrade request and
     *    return a ready-to-use connection.
     *  - Client: call @ref connect with the target endpoint. The
     *    helper resolves, connects, and performs the handshake.
     *
     * After construction:
     *  - @ref receive returns the next data message; Ping/Pong/Close
     *    control frames are handled internally.
     *  - @ref sendText / @ref sendBinary emit one frame each.
     *  - @ref close emits a Close frame and marks the connection as
     *    no longer open. Subsequent send / receive calls throw.
     */
    class WsConnection final {
    public:
        /**
         * @brief Read the incoming HTTP/1.1 upgrade request directly
         *        off @p stream, validate the WebSocket headers, and
         *        write the 101 Switching Protocols response. On
         *        success returns a server-side @c WsConnection that
         *        owns @p stream.
         *
         * The peer's @c Sec-WebSocket-Key is hashed with the protocol
         * GUID and base64-encoded to produce @c Sec-WebSocket-Accept,
         * per RFC 6455 section 4.2.2.
         *
         * A refused handshake (bad request, wrong version, Origin not
         * allowed by @p options, refused by @ref WsOptions::onHandshake) is
         * answered with a bodyless 400 / 426 / 403 / hook status on a
         * best-effort basis before the exception is thrown.
         *
         * @throws WsException on malformed request, missing headers,
         *         wrong protocol version, refused Origin, handshake timeout, or
         *         socket failure.
         */
        static YarnBall::Task<WsConnection> serverHandshake(TcpStream stream, WsOptions options = {});

        /**
         * @brief Connect to @c ws://@p host:@p port@p path and
         *        perform the client-side handshake. Returns a
         *        client-side @c WsConnection on success.
         *
         * The client generates a random 16-byte nonce, base64-encodes
         * it, sends it as @c Sec-WebSocket-Key, and verifies the
         * server's @c Sec-WebSocket-Accept on the response.
         *
         * @throws WsException on resolve/connect failure, non-101
         *         response, or accept-key mismatch.
         */
        static YarnBall::Task<WsConnection> connect(std::string host,
                                                     std::uint16_t port,
                                                     std::string path,
                                                     WsOptions options = {});

        WsConnection(const WsConnection &) = delete;
        WsConnection &operator=(const WsConnection &) = delete;
        WsConnection(WsConnection &&) noexcept = default;
        WsConnection &operator=(WsConnection &&other) noexcept;
        ~WsConnection();

        /**
         * @brief Receive the next data message. Suspends until a Text
         *        or Binary frame arrives. Ping frames are answered
         *        with Pong internally; Close frames are echoed and
         *        cause the connection to be marked closed (the next
         *        call throws).
         *
         * A protocol violation fails the connection: a Close frame with the
         * matching code (1002, 1007 invalid UTF-8, 1009 too big) is sent best-effort
         * and the connection is marked closed.
         *
         * Concurrent senders are safe: every write goes through one per-connection
         * lock, so a Pong produced here never interleaves with a @ref sendText.
         * Only one @c receive may be in flight at a time.
         *
         * @throws WsException if the connection has already been
         *         closed, on protocol violations (continuation
         *         frames, reserved bits set), on a frame/idle timeout, or on
         *         peer-initiated close (after the close handshake completes).
         */
        YarnBall::Task<WsMessage> receive();

        /**
         * @brief Send @p payload as a single Text frame.
         */
        YarnBall::Task<void> sendText(std::string_view payload);

        /**
         * @brief Send @p payload as a single Binary frame.
         */
        YarnBall::Task<void> sendBinary(std::span<const std::byte> payload);

        /**
         * @brief Send one frame of a multi-frame (fragmented) message.
         *        Advanced API; prefer @ref sendText / @ref sendBinary
         *        for the common single-frame case.
         *
         * Use to stream a large payload without materialising it all
         * in memory:
         * @code
         *   co_await ws.sendFrame(Text,   firstChunk,  false); // FIN=0
         *   co_await ws.sendFrame(Cont,   middleChunk, false); // FIN=0
         *   co_await ws.sendFrame(Cont,   lastChunk,   true ); // FIN=1
         * @endcode
         *
         * The caller is responsible for ordering: the first frame
         * must carry @c Text or @c Binary; subsequent frames must
         * carry @c Continuation. RFC 6455 §5.4 forbids interleaving
         * other data messages (control frames are allowed).
         */
        enum class FragmentKind { Text, Binary, Continuation };
        YarnBall::Task<void> sendFrame(FragmentKind kind,
                                         std::span<const std::byte> payload,
                                         bool isFinal);

        /**
         * @brief Send a Close frame with the given status code and
         *        optional reason, then mark the connection closed.
         *        Idempotent.
         *
         * Standard codes: 1000 normal, 1001 going away, 1002 protocol
         * error, 1003 unsupported data, 1008 policy violation,
         * 1011 internal error.
         */
        YarnBall::Task<void> close(std::uint16_t code = 1000,
                                    std::string_view reason = "");

        /**
         * @return @c true while the connection is open. Flipped to
         *         @c false by a peer Close, a local @ref close, or a
         *         protocol error.
         */
        bool isOpen() const noexcept;

        /**
         * @brief What the opening handshake carried: the upgrade request (target,
         *        headers, offered and chosen subprotocol) on a server connection, the
         *        101 response headers on a client one. Valid while the connection
         *        exists; empty on a moved-from handle.
         */
        const WsHandshakeInfo &handshake() const noexcept;

        /**
         * @brief Heap state of a connection (defined in WebSocket.cpp). It lives
         *        on the heap, shared with every operation in flight, so moving
         *        the move-only handle never invalidates a running @c receive or
         *        send. Public only so the .cpp's file-scope helpers can name it.
         */
        struct Impl;

    private:
        explicit WsConnection(std::shared_ptr<Impl> state) noexcept : impl(std::move(state)) {
        }

        std::shared_ptr<Impl> impl;
    };

    /**
     * @brief Per-connection callback of @ref wsServe: runs once the opening
     *        handshake has succeeded and owns the connection until it returns.
     */
    using WsHandler = std::function<YarnBall::Task<void>(WsConnection)>;

    /**
     * @brief WebSocket server loop: accept connections on @p listener, perform the
     *        server handshake on each, and run @p handler with the resulting
     *        @ref WsConnection on the Yarn pool.
     *
     * Applies the whole of @p options so a hand-written accept loop need not:
     *  - @c maxConnections caps the connections served at once (the slot is held
     *    from accept, so half-open handshakes count); one beyond it is closed
     *    without a reply. Backed by @ref ConnectionLimiter.
     *  - @c handshakeTimeout cuts off a peer that never completes the upgrade.
     *  - @c allowedOrigins / @c originCheck / @c allowMissingOrigin refuse a
     *    browser Origin the server does not serve (403).
     *  - @c strictOrigin and @c onHandshake add the stricter Origin mode and the
     *    per-request hook (path / cookies / Authorization, 401 / 403, subprotocol);
     *    the handler can read the same request via @ref WsConnection::handshake.
     *  - the remaining fields (frame / idle / write timeouts, size caps) become the
     *    connection's own limits.
     *
     * A connection whose handshake fails is dropped; an exception escaping
     * @p handler is swallowed (the connection is closed with the handler's frame).
     * The loop ends when @p stop is requested (in-flight connections keep running)
     * or the listener fails with a non-transient error.
     */
    YarnBall::Task<void> wsServe(TcpListener listener,
                                 WsHandler handler,
                                 WsOptions options = {},
                                 std::stop_token stop = {});

}

#endif // SOCCER_WEBSOCKET_H
