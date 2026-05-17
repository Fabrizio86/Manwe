//
// Created by Fabrizio Paino on 2026-05-16.
//

#include "WebSocket.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <sstream>
#include <string>

#include "Coroutines.h"
#include "SocketException.h"

namespace Soccer {

    namespace {

        // RFC 6455 protocol GUID, appended to Sec-WebSocket-Key before
        // hashing to produce Sec-WebSocket-Accept.
        constexpr const char *kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

        // Maximum control-frame payload per the spec. Anything larger is
        // a protocol error.
        constexpr std::size_t kMaxControlPayload = 125;

        // Hard cap on data-frame payload we'll accept. Configurable in
        // principle, but no knob in v1: 16 MB covers every realistic
        // single-frame WebSocket message and bounds the worst-case
        // allocation a peer can force.
        constexpr std::size_t kMaxDataPayload = 16ull * 1024ull * 1024ull;

        // WebSocket opcodes.
        constexpr std::uint8_t kOpContinuation = 0x0;
        constexpr std::uint8_t kOpText = 0x1;
        constexpr std::uint8_t kOpBinary = 0x2;
        constexpr std::uint8_t kOpClose = 0x8;
        constexpr std::uint8_t kOpPing = 0x9;
        constexpr std::uint8_t kOpPong = 0xA;

        // ----------------------------------------------------------------
        // SHA-1 (RFC 3174). Inline implementation -- ~60 lines. Used only
        // for the handshake; no perf-sensitive path touches it.
        // ----------------------------------------------------------------

        struct Sha1 {
            std::uint32_t h[5];
            std::uint8_t buf[64];
            std::uint64_t total;
            std::size_t bufLen;

            void init() {
                h[0] = 0x67452301u; h[1] = 0xEFCDAB89u; h[2] = 0x98BADCFEu;
                h[3] = 0x10325476u; h[4] = 0xC3D2E1F0u;
                total = 0; bufLen = 0;
            }

            static std::uint32_t rol(std::uint32_t x, int n) {
                return (x << n) | (x >> (32 - n));
            }

            void processBlock(const std::uint8_t *block) {
                std::uint32_t w[80];
                for (int i = 0; i < 16; ++i) {
                    w[i] = (std::uint32_t(block[i * 4]) << 24) |
                            (std::uint32_t(block[i * 4 + 1]) << 16) |
                            (std::uint32_t(block[i * 4 + 2]) << 8) |
                            std::uint32_t(block[i * 4 + 3]);
                }
                for (int i = 16; i < 80; ++i) {
                    w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
                }
                std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
                for (int i = 0; i < 80; ++i) {
                    std::uint32_t f, k;
                    if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
                    else if (i < 40) { f = b ^ c ^ d;             k = 0x6ED9EBA1u; }
                    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
                    else             { f = b ^ c ^ d;             k = 0xCA62C1D6u; }
                    std::uint32_t t = rol(a, 5) + f + e + k + w[i];
                    e = d; d = c; c = rol(b, 30); b = a; a = t;
                }
                h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
            }

            void update(const void *data, std::size_t len) {
                const std::uint8_t *p = static_cast<const std::uint8_t *>(data);
                total += len;
                while (len > 0) {
                    const std::size_t take = std::min(len, std::size_t{64} - bufLen);
                    std::memcpy(buf + bufLen, p, take);
                    bufLen += take; p += take; len -= take;
                    if (bufLen == 64) {
                        processBlock(buf);
                        bufLen = 0;
                    }
                }
            }

            void finish(std::uint8_t out[20]) {
                const std::uint64_t bits = total * 8;
                std::uint8_t pad = 0x80;
                update(&pad, 1);
                while (bufLen != 56) {
                    std::uint8_t z = 0;
                    update(&z, 1);
                }
                std::uint8_t lenbe[8];
                for (int i = 0; i < 8; ++i) lenbe[i] = static_cast<std::uint8_t>(bits >> (56 - i * 8));
                update(lenbe, 8);
                for (int i = 0; i < 5; ++i) {
                    out[i * 4]     = static_cast<std::uint8_t>(h[i] >> 24);
                    out[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
                    out[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
                    out[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
                }
            }
        };

        // ----------------------------------------------------------------
        // Base64 encode. Decode not needed -- we only emit the accept
        // header on server side and the random nonce on client side.
        // ----------------------------------------------------------------

        std::string base64Encode(const std::uint8_t *data, std::size_t len) {
            static constexpr char kTable[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve(((len + 2) / 3) * 4);
            std::size_t i = 0;
            for (; i + 3 <= len; i += 3) {
                std::uint32_t v = (std::uint32_t(data[i]) << 16) |
                                   (std::uint32_t(data[i + 1]) << 8) |
                                   std::uint32_t(data[i + 2]);
                out.push_back(kTable[(v >> 18) & 0x3F]);
                out.push_back(kTable[(v >> 12) & 0x3F]);
                out.push_back(kTable[(v >> 6) & 0x3F]);
                out.push_back(kTable[v & 0x3F]);
            }
            if (i < len) {
                std::uint32_t v = std::uint32_t(data[i]) << 16;
                if (i + 1 < len) v |= std::uint32_t(data[i + 1]) << 8;
                out.push_back(kTable[(v >> 18) & 0x3F]);
                out.push_back(kTable[(v >> 12) & 0x3F]);
                out.push_back((i + 1 < len) ? kTable[(v >> 6) & 0x3F] : '=');
                out.push_back('=');
            }
            return out;
        }

        std::string computeAccept(const std::string &key) {
            Sha1 s; s.init();
            s.update(key.data(), key.size());
            s.update(kWsGuid, std::strlen(kWsGuid));
            std::uint8_t digest[20];
            s.finish(digest);
            return base64Encode(digest, 20);
        }

        // ----------------------------------------------------------------
        // Random helpers for the client-side nonce and per-frame masks.
        // Not cryptographically strong -- they don't need to be. Both
        // are protocol scaffolding, not secrecy.
        // ----------------------------------------------------------------

        std::array<std::uint8_t, 16> randomNonce() {
            std::random_device rd;
            std::mt19937_64 gen(rd());
            std::array<std::uint8_t, 16> out{};
            for (auto &b : out) b = static_cast<std::uint8_t>(gen());
            return out;
        }

        std::uint32_t randomMask() {
            std::random_device rd;
            std::mt19937 gen(rd());
            return gen();
        }

        // ----------------------------------------------------------------
        // ASCII case-insensitive equality, for header-name matching.
        // ----------------------------------------------------------------

        bool iequals(std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                char ca = a[i] | 0x20; // tolower for ASCII letters
                char cb = b[i] | 0x20;
                if (ca != cb) return false;
            }
            return true;
        }

        // ----------------------------------------------------------------
        // Raw read N bytes / read until "\r\n\r\n" off the underlying
        // TcpStream. Used during the handshake only; per-frame body
        // reads use the same exactRead helper.
        // ----------------------------------------------------------------

        /**
         * @brief Read exactly @p buf.size() bytes from a logical source
         *        that is @p preBuf (consumed first, starting at
         *        @p preOffset) followed by reads on @p s. The preBuf
         *        layer carries handshake-trailing bytes that the
         *        connection's frame parser must not lose.
         */
        YarnBall::Task<void> exactRead(TcpStream &s,
                                          std::vector<std::byte> &preBuf,
                                          std::size_t &preOffset,
                                          std::span<std::byte> buf) {
            std::size_t got = 0;
            while (got < buf.size()) {
                if (preOffset < preBuf.size()) {
                    const std::size_t avail = preBuf.size() - preOffset;
                    const std::size_t take = std::min(avail, buf.size() - got);
                    std::memcpy(buf.data() + got, preBuf.data() + preOffset, take);
                    preOffset += take;
                    got += take;
                    if (preOffset == preBuf.size()) {
                        preBuf.clear();
                        preOffset = 0;
                    }
                    continue;
                }
                const std::size_t n = co_await s.read(buf.subspan(got));
                if (n == 0) throw WsException("connection closed mid-read");
                got += n;
            }
            co_return;
        }

        /**
         * @brief Read until the HTTP @c "\r\n\r\n" separator and return
         *        the header block. Any bytes read past the separator
         *        (the start of the first WebSocket frame, if the peer
         *        coalesced) are captured into @p trailing so the
         *        WsConnection can prepend them to its first frame
         *        read. Without this, a server that sends its first
         *        frame in the same TCP segment as its 101 response
         *        would silently lose those bytes.
         */
        YarnBall::Task<std::string> readUntilHeaderEnd(
            TcpStream &s, std::vector<std::byte> &trailing) {
            std::string accum;
            accum.reserve(512);
            std::array<std::byte, 256> chunk{};
            while (accum.size() < 16 * 1024) {
                const std::size_t n = co_await s.read(chunk);
                if (n == 0) throw WsException("connection closed before headers");
                accum.append(reinterpret_cast<const char *>(chunk.data()), n);
                const auto end = accum.find("\r\n\r\n");
                if (end != std::string::npos) {
                    const auto headerSize = end + 4;
                    if (accum.size() > headerSize) {
                        trailing.insert(
                            trailing.end(),
                            reinterpret_cast<const std::byte *>(accum.data() + headerSize),
                            reinterpret_cast<const std::byte *>(accum.data() + accum.size()));
                    }
                    accum.resize(headerSize);
                    co_return accum;
                }
            }
            throw WsException("HTTP headers exceeded 16 KiB");
        }

        // ----------------------------------------------------------------
        // Frame I/O. Parses incoming frames into (opcode, payload),
        // builds outgoing frames with or without masking.
        // ----------------------------------------------------------------

        struct WsFrame {
            std::uint8_t opcode;
            bool fin;
            std::vector<std::byte> payload;
        };

        YarnBall::Task<WsFrame> readFrame(TcpStream &s,
                                              std::vector<std::byte> &preBuf,
                                              std::size_t &preOffset) {
            std::array<std::byte, 2> hdr{};
            co_await exactRead(s, preBuf, preOffset, hdr);
            const std::uint8_t b0 = std::to_integer<std::uint8_t>(hdr[0]);
            const std::uint8_t b1 = std::to_integer<std::uint8_t>(hdr[1]);

            WsFrame f{};
            f.fin = (b0 & 0x80) != 0;
            const std::uint8_t rsv = b0 & 0x70;
            if (rsv != 0) throw WsException("RSV bits set; no extensions negotiated");
            f.opcode = b0 & 0x0F;
            const bool masked = (b1 & 0x80) != 0;
            std::uint64_t plen = b1 & 0x7F;

            if (plen == 126) {
                std::array<std::byte, 2> ext{};
                co_await exactRead(s, preBuf, preOffset, ext);
                plen = (std::uint64_t(std::to_integer<std::uint8_t>(ext[0])) << 8) |
                        std::uint64_t(std::to_integer<std::uint8_t>(ext[1]));
            } else if (plen == 127) {
                std::array<std::byte, 8> ext{};
                co_await exactRead(s, preBuf, preOffset, ext);
                plen = 0;
                for (int i = 0; i < 8; ++i) {
                    plen = (plen << 8) | std::uint64_t(std::to_integer<std::uint8_t>(ext[i]));
                }
            }

            // Control frames must be short and not fragmented.
            const bool isControl = (f.opcode & 0x08) != 0;
            if (isControl) {
                if (plen > kMaxControlPayload) {
                    throw WsException("control frame payload exceeds 125 bytes");
                }
                if (!f.fin) {
                    throw WsException("fragmented control frame");
                }
            } else if (plen > kMaxDataPayload) {
                throw WsException("data frame payload exceeds 16 MiB cap");
            }

            std::array<std::byte, 4> mask{};
            if (masked) {
                co_await exactRead(s, preBuf, preOffset, mask);
            }

            f.payload.resize(static_cast<std::size_t>(plen));
            if (!f.payload.empty()) {
                co_await exactRead(s, preBuf, preOffset, f.payload);
                if (masked) {
                    for (std::size_t i = 0; i < f.payload.size(); ++i) {
                        f.payload[i] = std::byte(
                            std::to_integer<std::uint8_t>(f.payload[i]) ^
                            std::to_integer<std::uint8_t>(mask[i & 3]));
                    }
                }
            }
            co_return f;
        }

        YarnBall::Task<void> writeFrame(TcpStream &s,
                                          std::uint8_t opcode,
                                          std::span<const std::byte> payload,
                                          bool maskOutgoing,
                                          bool isFinal = true) {
            // Header is up to 14 bytes (2 + 8 ext-length + 4 mask).
            std::array<std::byte, 14> hdr{};
            std::size_t hdrLen = 0;
            hdr[hdrLen++] = std::byte((isFinal ? 0x80 : 0x00) | (opcode & 0x0F));
            const std::size_t plen = payload.size();
            const std::uint8_t maskBit = maskOutgoing ? 0x80 : 0x00;
            if (plen < 126) {
                hdr[hdrLen++] = std::byte(maskBit | static_cast<std::uint8_t>(plen));
            } else if (plen <= 0xFFFF) {
                hdr[hdrLen++] = std::byte(maskBit | 126);
                hdr[hdrLen++] = std::byte((plen >> 8) & 0xFF);
                hdr[hdrLen++] = std::byte(plen & 0xFF);
            } else {
                hdr[hdrLen++] = std::byte(maskBit | 127);
                for (int i = 7; i >= 0; --i) {
                    hdr[hdrLen++] = std::byte((plen >> (i * 8)) & 0xFF);
                }
            }

            std::array<std::byte, 4> mask{};
            if (maskOutgoing) {
                const std::uint32_t m = randomMask();
                mask[0] = std::byte((m >> 24) & 0xFF);
                mask[1] = std::byte((m >> 16) & 0xFF);
                mask[2] = std::byte((m >> 8) & 0xFF);
                mask[3] = std::byte(m & 0xFF);
                hdr[hdrLen++] = mask[0];
                hdr[hdrLen++] = mask[1];
                hdr[hdrLen++] = mask[2];
                hdr[hdrLen++] = mask[3];
            }

            co_await s.write(std::span<const std::byte>(hdr.data(), hdrLen));

            if (payload.empty()) co_return;

            if (!maskOutgoing) {
                co_await s.write(payload);
                co_return;
            }
            // Masked: copy and XOR into a scratch buffer rather than
            // mutating caller storage. Single allocation, freed when
            // the coroutine frame unwinds.
            std::vector<std::byte> scratch(payload.size());
            for (std::size_t i = 0; i < payload.size(); ++i) {
                scratch[i] = std::byte(
                    std::to_integer<std::uint8_t>(payload[i]) ^
                    std::to_integer<std::uint8_t>(mask[i & 3]));
            }
            co_await s.write(scratch);
            co_return;
        }

    } // anonymous namespace


    YarnBall::Task<WsConnection> WsConnection::serverHandshake(TcpStream stream) {
        std::vector<std::byte> trailing;
        std::string raw = co_await readUntilHeaderEnd(stream, trailing);

        // Parse the request line + headers (case-insensitive). Body
        // is irrelevant -- a WebSocket upgrade has no body.
        const auto headerEnd = raw.find("\r\n\r\n");
        std::string headers = raw.substr(0, headerEnd);

        // Collect the header values we need; skip the request line.
        std::istringstream iss(headers);
        std::string line;
        std::getline(iss, line); // request line; drop trailing \r

        std::string upgrade, connection, key, version;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            // Strip leading whitespace from value.
            std::size_t s = 0;
            while (s < value.size() && (value[s] == ' ' || value[s] == '\t')) ++s;
            value = value.substr(s);
            if (iequals(name, "upgrade"))         upgrade = value;
            else if (iequals(name, "connection")) connection = value;
            else if (iequals(name, "sec-websocket-key")) key = value;
            else if (iequals(name, "sec-websocket-version")) version = value;
        }

        if (!iequals(upgrade, "websocket")) {
            throw WsException("missing or wrong Upgrade header");
        }
        // Connection may be "Upgrade" or include "Upgrade" among other tokens.
        if (connection.find("Upgrade") == std::string::npos &&
            connection.find("upgrade") == std::string::npos) {
            throw WsException("missing Upgrade in Connection header");
        }
        if (version != "13") {
            throw WsException("unsupported Sec-WebSocket-Version: " + version);
        }
        if (key.empty()) {
            throw WsException("missing Sec-WebSocket-Key");
        }

        const std::string accept = computeAccept(key);
        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        co_await stream.write(std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(resp.data()), resp.size()));

        co_return WsConnection(std::move(stream), /*isServer=*/true,
                                 std::move(trailing));
    }

    YarnBall::Task<WsConnection> WsConnection::connect(std::string host,
                                                         std::uint16_t port,
                                                         std::string path) {
        TcpStream s = co_await tcpConnect(host, port);

        const auto nonce = randomNonce();
        const std::string key = base64Encode(nonce.data(), nonce.size());

        std::string req =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + ":" + std::to_string(port) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        co_await s.write(std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(req.data()), req.size()));

        std::vector<std::byte> trailing;
        std::string raw = co_await readUntilHeaderEnd(s, trailing);
        const auto headerEnd = raw.find("\r\n\r\n");
        std::string headers = raw.substr(0, headerEnd);

        std::istringstream iss(headers);
        std::string line;
        if (!std::getline(iss, line)) throw WsException("no status line in response");
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find("101") == std::string::npos) {
            throw WsException("server did not upgrade: " + line);
        }
        std::string acceptHeader;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            std::size_t lead = 0;
            while (lead < value.size() && (value[lead] == ' ' || value[lead] == '\t')) ++lead;
            value = value.substr(lead);
            if (iequals(name, "sec-websocket-accept")) acceptHeader = value;
        }

        const std::string expected = computeAccept(key);
        if (acceptHeader != expected) {
            throw WsException("Sec-WebSocket-Accept mismatch");
        }
        co_return WsConnection(std::move(s), /*isServer=*/false,
                                 std::move(trailing));
    }

    YarnBall::Task<WsMessage> WsConnection::receive() {
        if (!this->open) {
            throw WsException("receive on closed WsConnection");
        }
        // Per RFC 6455 §5.4 a single application message may span
        // multiple frames: a leading TEXT or BINARY frame with FIN=0,
        // zero or more CONTINUATION frames with FIN=0, and a final
        // CONTINUATION (or the leading frame itself) with FIN=1.
        // Control frames (Ping/Pong/Close) may interleave between
        // fragments and must be handled without disturbing assembly.
        bool assembling = false;
        WsMessageType assemblyType{WsMessageType::Text};
        std::vector<std::byte> assemblyBuf;

        while (true) {
            WsFrame f = co_await readFrame(this->stream,
                                              this->preBuf, this->preOffset);
            switch (f.opcode) {
                case kOpText:
                case kOpBinary: {
                    if (assembling) {
                        // RFC §5.4: cannot start a new data message
                        // while a previous one is still being framed.
                        throw WsException("new data frame mid-message");
                    }
                    if (f.fin) {
                        WsMessage m;
                        m.type = (f.opcode == kOpText) ? WsMessageType::Text
                                                          : WsMessageType::Binary;
                        m.payload = std::move(f.payload);
                        co_return m;
                    }
                    // Multi-frame message begins.
                    assembling = true;
                    assemblyType = (f.opcode == kOpText) ? WsMessageType::Text
                                                            : WsMessageType::Binary;
                    assemblyBuf = std::move(f.payload);
                    break;
                }
                case kOpContinuation: {
                    if (!assembling) {
                        throw WsException("orphan continuation frame");
                    }
                    if (assemblyBuf.size() + f.payload.size() > kMaxDataPayload) {
                        throw WsException("multi-frame message exceeds 16 MiB cap");
                    }
                    assemblyBuf.insert(assemblyBuf.end(),
                                        f.payload.begin(), f.payload.end());
                    if (f.fin) {
                        WsMessage m;
                        m.type = assemblyType;
                        m.payload = std::move(assemblyBuf);
                        co_return m;
                    }
                    break;
                }
                case kOpPing: {
                    // Echo payload as Pong. Control frames are allowed
                    // to interleave with continuation; assembly state
                    // stays untouched.
                    co_await writeFrame(this->stream, kOpPong, f.payload,
                                          /*maskOutgoing=*/!this->serverSide);
                    break;
                }
                case kOpPong:
                    // Unsolicited Pong is allowed and ignored.
                    break;
                case kOpClose: {
                    if (this->open) {
                        // Echo the Close frame back. The peer often
                        // closes the socket immediately after sending
                        // its Close, so the write can race with the
                        // peer's RST -- on Linux that surfaces as
                        // EPIPE / ECONNRESET. Either way the
                        // connection is gone, so swallow the transport
                        // error and proceed to close locally.
                        try {
                            co_await writeFrame(this->stream, kOpClose,
                                                  f.payload,
                                                  /*maskOutgoing=*/!this->serverSide);
                        } catch (const SocketException &) {
                            // Peer torn down before our echo landed.
                        }
                        this->open = false;
                    }
                    throw WsException("connection closed by peer");
                }
                default:
                    throw WsException("unknown opcode");
            }
        }
    }

    YarnBall::Task<void> WsConnection::sendText(std::string_view payload) {
        if (!this->open) throw WsException("send on closed WsConnection");
        co_await writeFrame(this->stream, kOpText,
                              std::span<const std::byte>(
                                  reinterpret_cast<const std::byte *>(payload.data()),
                                  payload.size()),
                              /*maskOutgoing=*/!this->serverSide);
        co_return;
    }

    YarnBall::Task<void> WsConnection::sendBinary(std::span<const std::byte> payload) {
        if (!this->open) throw WsException("send on closed WsConnection");
        co_await writeFrame(this->stream, kOpBinary, payload,
                              /*maskOutgoing=*/!this->serverSide);
        co_return;
    }

    YarnBall::Task<void> WsConnection::sendFrame(FragmentKind kind,
                                                    std::span<const std::byte> payload,
                                                    bool isFinal) {
        if (!this->open) throw WsException("send on closed WsConnection");
        std::uint8_t op;
        switch (kind) {
            case FragmentKind::Text:         op = kOpText; break;
            case FragmentKind::Binary:       op = kOpBinary; break;
            case FragmentKind::Continuation: op = kOpContinuation; break;
            default:                          op = kOpContinuation; break;
        }
        co_await writeFrame(this->stream, op, payload,
                              /*maskOutgoing=*/!this->serverSide, isFinal);
        co_return;
    }

    YarnBall::Task<void> WsConnection::close(std::uint16_t code,
                                              std::string_view reason) {
        if (!this->open) co_return;
        std::vector<std::byte> body;
        body.reserve(2 + reason.size());
        body.push_back(std::byte((code >> 8) & 0xFF));
        body.push_back(std::byte(code & 0xFF));
        for (char c : reason) body.push_back(std::byte(static_cast<std::uint8_t>(c)));
        // Same peer-disappeared race as the receive() Close-echo path:
        // if the peer has already closed the socket, our Close write
        // can surface as EPIPE / ECONNRESET on Linux. Swallow it;
        // the local connection is closing either way.
        try {
            co_await writeFrame(this->stream, kOpClose, body,
                                  /*maskOutgoing=*/!this->serverSide);
        } catch (const SocketException &) {
            // Peer already gone; nothing left to flush.
        }
        this->open = false;
        co_return;
    }

}
