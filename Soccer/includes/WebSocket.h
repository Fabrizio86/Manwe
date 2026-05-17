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
//  - Single-frame messages only (continuation frames are rejected;
//    add reassembly when a real use case demands it)
//  - No extensions (permessage-deflate etc.)
//  - No subprotocols
//
// API: one verb per operation. No callbacks, no on_message handler
// slot, no options structs. Receive returns the next message;
// internal control frames (Ping/Close) are handled transparently so
// user code only sees data frames.
//

#ifndef SOCCER_WEBSOCKET_H
#define SOCCER_WEBSOCKET_H

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Coroutines.h"
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
         * @throws WsException on malformed request, missing headers,
         *         wrong protocol version, or socket failure.
         */
        static YarnBall::Task<WsConnection> serverHandshake(TcpStream stream);

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
                                                     std::string path);

        WsConnection(const WsConnection &) = delete;
        WsConnection &operator=(const WsConnection &) = delete;
        WsConnection(WsConnection &&) noexcept = default;
        WsConnection &operator=(WsConnection &&) noexcept = default;
        ~WsConnection() = default;

        /**
         * @brief Receive the next data message. Suspends until a Text
         *        or Binary frame arrives. Ping frames are answered
         *        with Pong internally; Close frames are echoed and
         *        cause the connection to be marked closed (the next
         *        call throws).
         *
         * @throws WsException if the connection has already been
         *         closed, on protocol violations (continuation
         *         frames, reserved bits set), or on peer-initiated
         *         close (after the close handshake completes).
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
        bool isOpen() const noexcept { return this->open; }

    private:
        WsConnection(TcpStream s, bool isServer,
                       std::vector<std::byte> initialPrebuf = {}) noexcept
            : stream(std::move(s)),
              preBuf(std::move(initialPrebuf)),
              serverSide(isServer) {
        }

        TcpStream stream;
        /**
         * @brief Bytes that the handshake reader over-consumed past
         *        the @c "\r\n\r\n" header terminator. Frame reads
         *        drain this first before issuing a kernel @c read.
         *        Necessary because a server can legitimately send the
         *        first frame in the same TCP segment as its 101
         *        response.
         */
        std::vector<std::byte> preBuf;
        std::size_t preOffset{0};
        bool serverSide{true};
        bool open{true};
    };

}

#endif // SOCCER_WEBSOCKET_H
