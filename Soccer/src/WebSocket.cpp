//
// Created by Fabrizio Paino on 2026-05-16.
//

#include "WebSocket.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>

#include "AsyncSync.h"
#include "Coroutines.h"
#include "IoDeadline.h"
#include "NetUtil.h"
#include "SocketException.h"
#include "TcpServer.h"

namespace Soccer {

    namespace {

        // RFC 6455 protocol GUID, appended to Sec-WebSocket-Key before
        // hashing to produce Sec-WebSocket-Accept.
        constexpr const char *kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

        // Maximum control-frame payload per the spec. Anything larger is
        // a protocol error.
        constexpr std::size_t kMaxControlPayload = 125;

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

        /// True if @p key is the base64 text of exactly 16 bytes (RFC 6455 4.1: the
        /// Sec-WebSocket-Key nonce): 22 alphabet characters followed by "==".
        bool validWsKey(std::string_view key) {
            if (key.size() != 24 || key.compare(22, 2, "==") != 0) return false;
            for (std::size_t i = 0; i < 22; ++i) {
                const char c = key[i];
                const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '+' || c == '/';
                if (!ok) return false;
            }
            return true;
        }

        /// A peer-supplied string made safe to put in an exception message (and so in a
        /// log line): control and non-ASCII bytes become '?', and it is cut at 64 bytes.
        std::string sanitize(std::string_view in) {
            constexpr std::size_t kMax = 64;
            std::string out;
            out.reserve(std::min(in.size(), kMax) + 3);
            for (std::size_t i = 0; i < in.size() && i < kMax; ++i) {
                const unsigned char c = static_cast<unsigned char>(in[i]);
                out.push_back((c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?');
            }
            if (in.size() > kMax) out += "...";
            return out;
        }

        // ----------------------------------------------------------------
        // Random helpers for the client-side nonce and per-frame masks.
        // Not cryptographically strong -- they don't need to be. Both
        // are protocol scaffolding, not secrecy.
        //
        // randomMask() is on the hot per-frame send path (every masked
        // client->server frame, including every fragment of a
        // fragmented send, calls it once). Constructing a fresh
        // std::random_device per call opens (and, on destruction,
        // closes) a handle to the OS entropy source on most
        // implementations -- a syscall-class cost per frame. Seed one
        // generator per thread instead and reuse it; masking has no
        // cryptographic requirement, so a thread-local generator loses
        // nothing in correctness while removing the per-frame syscall.
        // ----------------------------------------------------------------

        // RFC 6455 section 5.3 requires masking keys to be unpredictable,
        // so draw them straight from the OS entropy source (refilled in
        // batches to avoid a syscall per frame) rather than from a
        // 32-bit-seeded Mersenne Twister.
        std::uint32_t randomWord() {
            thread_local std::array<std::uint32_t, 64> pool{};
            thread_local std::size_t pos = pool.size();
            if (pos == pool.size()) {
                std::random_device rd;
                for (auto &w : pool) w = rd();
                pos = 0;
            }
            return pool[pos++];
        }

        std::array<std::uint8_t, 16> randomNonce() {
            std::array<std::uint8_t, 16> out{};
            for (std::size_t i = 0; i < out.size(); ++i) {
                if (i % 4 == 0) {
                    const std::uint32_t w = randomWord();
                    for (std::size_t j = 0; j < 4; ++j) {
                        out[i + j] = static_cast<std::uint8_t>(w >> (j * 8));
                    }
                }
            }
            return out;
        }

        std::uint32_t randomMask() {
            return randomWord();
        }

        // Strict UTF-8 validation (no overlongs, surrogates, or > U+10FFFF).
        bool validUtf8(const std::byte *p, std::size_t n) {
            std::size_t i = 0;
            while (i < n) {
                const std::uint8_t c = std::to_integer<std::uint8_t>(p[i]);
                std::size_t len;
                std::uint32_t cp;
                if (c < 0x80) { ++i; continue; }
                else if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
                else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
                else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
                else return false;
                if (i + len > n) return false;
                for (std::size_t k = 1; k < len; ++k) {
                    const std::uint8_t cc = std::to_integer<std::uint8_t>(p[i + k]);
                    if ((cc & 0xC0) != 0x80) return false;
                    cp = (cp << 6) | (cc & 0x3F);
                }
                if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
                    cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
                i += len;
            }
            return true;
        }

        // ----------------------------------------------------------------
        // ASCII case-insensitive equality, for header-name matching.
        // ----------------------------------------------------------------

        bool iequals(std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            auto lower = [](char c) {
                return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
            };
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (lower(a[i]) != lower(b[i])) return false;
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
            TcpStream &s, std::vector<std::byte> &trailing, std::size_t maxBytes) {
            std::string accum;
            accum.reserve(512);
            std::array<std::byte, 256> chunk{};
            while (maxBytes == 0 || accum.size() < maxBytes) {
                const std::size_t n = co_await s.read(chunk);
                if (n == 0) throw WsException("connection closed before headers");
                accum.append(reinterpret_cast<const char *>(chunk.data()), n);
                const auto end = accum.find("\r\n\r\n");
                if (end != std::string::npos) {
                    const auto headerSize = end + 4;
                    if (maxBytes != 0 && headerSize > maxBytes) break;
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
            throw WsException("HTTP headers exceeded " + std::to_string(maxBytes) + " bytes");
        }

        /// A protocol violation, with the close code to fail the connection with.
        class WsFrameError : public WsException {
        public:
            WsFrameError(std::uint16_t closeCode, const std::string &what)
                : WsException(what), code(closeCode) {}
            std::uint16_t code;
        };

        /// Disarms a deadline on every exit path of the scope.
        struct DisarmOnExit {
            IoDeadline &d;
            ~DisarmOnExit() { d.disarm(); }
        };

    } // anonymous namespace

    /**
     * @brief Everything a connection owns. Heap-allocated and shared with each
     *        operation in flight, so the move-only WsConnection handle can be moved
     *        or destroyed while a receive / send is still running.
     */
    struct WsConnection::Impl {
        Impl(TcpStream s, bool server, std::vector<std::byte> pre, WsOptions o, WsHandshakeInfo hs)
            : stream(std::move(s)), preBuf(std::move(pre)), serverSide(server), opts(std::move(o)),
              info(std::move(hs)) {
        }

        TcpStream stream;
        /// Bytes the handshake reader over-consumed past the header terminator;
        /// frame reads drain them before touching the socket (a peer may send its
        /// first frame in the same segment as the 101).
        std::vector<std::byte> preBuf;
        std::size_t preOffset{0};
        bool serverSide;
        std::atomic<bool> open{true};
        /// Set when a frame write failed or timed out: part of that frame may be on the
        /// wire, so nothing more may follow it (guarded by writeMu).
        bool writeBroken{false};
        WsOptions opts;
        /// The opening handshake (request target, headers, subprotocol).
        WsHandshakeInfo info;
        /// Pong rate window (touched only by the one receive() in flight).
        std::chrono::steady_clock::time_point pingWindowStart{std::chrono::steady_clock::now()};
        std::size_t pingsInWindow{0};
        /// Serialises frame writes: the Pong receive() emits, sendText, close and
        /// fragmented sends all go through it, so frames never interleave.
        YarnBall::AsyncMutex writeMu;
        // Declared after `stream` so they are destroyed (disarmed) before it closes the fd.
        IoDeadline readDeadline;
        IoDeadline writeDeadline;
    };

    namespace {

        // ----------------------------------------------------------------
        // Frame I/O. Parses incoming frames into (opcode, payload),
        // builds outgoing frames with or without masking.
        // ----------------------------------------------------------------

        struct WsFrame {
            std::uint8_t opcode;
            bool fin;
            std::vector<std::byte> payload;
        };

        /// @p assembledBytes: payload already gathered for the fragmented message in progress
        /// (0 when none), so a continuation that would push it past maxMessageBytes is refused
        /// from its header, before its payload is read into memory.
        YarnBall::Task<WsFrame> readFrame(WsConnection::Impl &im, bool assembling,
                                          std::size_t assembledBytes) {
            TcpStream &s = im.stream;
            const bool isServer = im.serverSide;
            const WsOptions &o = im.opts;
            const DisarmOnExit disarm{im.readDeadline};

            // Waiting for a frame to begin is "idle" -- unless a fragmented
            // message is half assembled, where the peer owes us the next piece.
            im.readDeadline.arm(s.fd(), (assembling && o.frameTimeout.count() > 0) ? o.frameTimeout
                                                                                     : o.idleTimeout);
            std::array<std::byte, 1> first{};
            co_await exactRead(s, im.preBuf, im.preOffset, first);
            // The frame has started: the rest of it must arrive promptly.
            im.readDeadline.arm(s.fd(), o.frameTimeout);
            std::array<std::byte, 1> second{};
            co_await exactRead(s, im.preBuf, im.preOffset, second);
            const std::uint8_t b0 = std::to_integer<std::uint8_t>(first[0]);
            const std::uint8_t b1 = std::to_integer<std::uint8_t>(second[0]);

            WsFrame f{};
            f.fin = (b0 & 0x80) != 0;
            const std::uint8_t rsv = b0 & 0x70;
            if (rsv != 0) throw WsFrameError(1002, "RSV bits set; no extensions negotiated");
            f.opcode = b0 & 0x0F;
            const bool masked = (b1 & 0x80) != 0;
            // RFC 6455 §5.1: a server MUST close the connection upon
            // receiving an unmasked frame, and a client MUST do the
            // same for a masked frame. This is the protocol's specific
            // defence against cache-poisoning attacks on transparent
            // proxies that would otherwise misinterpret unmasked
            // WebSocket traffic as plaintext HTTP -- it is a security
            // control, not a style rule, so it is enforced before any
            // further bytes of this frame are trusted.
            if (masked != isServer) {
                throw WsFrameError(1002, isServer
                    ? "protocol violation: unmasked frame received from client"
                    : "protocol violation: masked frame received from server");
            }
            std::uint64_t plen = b1 & 0x7F;

            if (plen == 126) {
                std::array<std::byte, 2> ext{};
                co_await exactRead(s, im.preBuf, im.preOffset, ext);
                plen = (std::uint64_t(std::to_integer<std::uint8_t>(ext[0])) << 8) |
                        std::uint64_t(std::to_integer<std::uint8_t>(ext[1]));
                if (plen < 126) throw WsFrameError(1002, "non-minimal payload length encoding");
            } else if (plen == 127) {
                std::array<std::byte, 8> ext{};
                co_await exactRead(s, im.preBuf, im.preOffset, ext);
                plen = 0;
                for (int i = 0; i < 8; ++i) {
                    plen = (plen << 8) | std::uint64_t(std::to_integer<std::uint8_t>(ext[i]));
                }
                if ((plen >> 63) != 0 || plen <= 0xFFFF) {
                    throw WsFrameError(1002, "invalid 64-bit payload length");
                }
            }

            // Control frames must be short and not fragmented.
            const bool isControl = (f.opcode & 0x08) != 0;
            if (isControl) {
                if (plen > kMaxControlPayload) {
                    throw WsFrameError(1002, "control frame payload exceeds 125 bytes");
                }
                if (!f.fin) {
                    throw WsFrameError(1002, "fragmented control frame");
                }
            } else if (o.maxMessageBytes != 0 &&
                       (assembledBytes > o.maxMessageBytes || plen > o.maxMessageBytes - assembledBytes)) {
                throw WsFrameError(1009, "data frame payload exceeds the message size cap");
            }

            std::array<std::byte, 4> mask{};
            if (masked) {
                co_await exactRead(s, im.preBuf, im.preOffset, mask);
            }

            // Grow the buffer as bytes actually arrive instead of trusting
            // the declared length up front: a peer announcing a 16 MiB
            // frame and sending nothing must not pin 16 MiB per connection.
            const std::size_t total = static_cast<std::size_t>(plen);
            constexpr std::size_t kReadChunk = 64 * 1024;
            while (f.payload.size() < total) {
                const std::size_t off = f.payload.size();
                const std::size_t take = std::min(kReadChunk, total - off);
                f.payload.resize(off + take);
                co_await exactRead(s, im.preBuf, im.preOffset,
                                   std::span<std::byte>(f.payload.data() + off, take));
            }
            if (masked) {
                for (std::size_t i = 0; i < f.payload.size(); ++i) {
                    f.payload[i] = std::byte(
                        std::to_integer<std::uint8_t>(f.payload[i]) ^
                        std::to_integer<std::uint8_t>(mask[i & 3]));
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
                    hdr[hdrLen++] = std::byte((static_cast<std::uint64_t>(plen) >> (i * 8)) & 0xFF);
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

            // Header and payload go out in one write so a frame is never split
            // apart by a suspension between the two. The payload is
            // copied (and masked when required) rather than mutating
            // caller storage.
            std::vector<std::byte> out(hdrLen + payload.size());
            std::memcpy(out.data(), hdr.data(), hdrLen);
            for (std::size_t i = 0; i < payload.size(); ++i) {
                const std::uint8_t b = std::to_integer<std::uint8_t>(payload[i]);
                out[hdrLen + i] = std::byte(maskOutgoing
                    ? static_cast<std::uint8_t>(b ^ std::to_integer<std::uint8_t>(mask[i & 3]))
                    : b);
            }
            co_await s.write(out);
            co_return;
        }

        /**
         * @brief Write one frame while holding the connection's write lock, under
         *        the write deadline. Every outgoing frame -- data, Pong, Close --
         *        goes through here, which is what keeps concurrent senders from
         *        interleaving bytes on the wire.
         */
        YarnBall::Task<void> sendLocked(std::shared_ptr<WsConnection::Impl> im,
                                          std::uint8_t opcode,
                                          std::span<const std::byte> payload,
                                          bool isFinal) {
            auto guard = co_await im->writeMu.lock();
            // A sender queued behind one whose write failed would put its frame after a
            // partial one: the peer would read garbage. It fails instead.
            if (im->writeBroken) throw WsException("WebSocket connection broken by a failed write");
            const DisarmOnExit disarm{im->writeDeadline};
            im->writeDeadline.arm(im->stream.fd(), im->opts.writeTimeout);
            try {
                co_await writeFrame(im->stream, opcode, payload, !im->serverSide, isFinal);
            } catch (const SocketException &) {
                im->writeBroken = true;
                im->open.store(false);
                if (im->writeDeadline.expired()) throw WsException("WebSocket write timed out");
                throw;
            } catch (...) {
                im->writeBroken = true;
                im->open.store(false);
                throw;
            }
            co_return;
        }

        std::vector<std::byte> closeBody(std::uint16_t code, std::string_view reason) {
            // A control frame payload is at most 125 bytes: 2 for the code
            // plus up to 123 of reason.
            if (reason.size() > 123) {
                // Cut on a code point boundary: back up over continuation bytes so the
                // reason stays valid UTF-8 (a peer must close with 1007 on invalid text).
                std::size_t cut = 123;
                while (cut > 0 && (static_cast<unsigned char>(reason[cut]) & 0xC0) == 0x80) --cut;
                reason = reason.substr(0, cut);
            }
            std::vector<std::byte> body;
            body.reserve(2 + reason.size());
            body.push_back(std::byte((code >> 8) & 0xFF));
            body.push_back(std::byte(code & 0xFF));
            for (char c : reason) body.push_back(std::byte(static_cast<std::uint8_t>(c)));
            return body;
        }

        /**
         * @brief Fail the connection: mark it closed, send a Close frame with
         *        @p code (best effort -- the peer may already be gone), then throw
         *        WsException(@p what).
         */
        YarnBall::Task<void> failConnection(std::shared_ptr<WsConnection::Impl> im,
                                              std::uint16_t code, std::string what) {
            if (im->open.exchange(false)) {
                const std::vector<std::byte> body = closeBody(code, "");
                try {
                    co_await sendLocked(im, kOpClose, body, true);
                } catch (...) {
                    // Nothing more can be done for a peer that is already gone.
                }
            }
            throw WsException(what);
        }

        /// Lower-case host and port of an authority ("host", "host:port", "[v6]:port"); port 0 when absent.
        bool splitAuthority(std::string_view a, std::string &host, unsigned &port) {
            port = 0;
            std::string_view portText;
            if (!a.empty() && a.front() == '[') {
                const std::size_t close = a.find(']');
                if (close == std::string_view::npos) return false;
                host.assign(a.substr(0, close + 1));
                const std::string_view rest = a.substr(close + 1);
                if (!rest.empty()) {
                    if (rest.front() != ':') return false;
                    portText = rest.substr(1);
                }
            } else {
                const std::size_t colon = a.find(':');
                host.assign(a.substr(0, colon));
                if (colon != std::string_view::npos) portText = a.substr(colon + 1);
            }
            if (host.empty()) return false;
            for (char &c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!portText.empty()) {
                if (portText.size() > 5) return false;
                unsigned v = 0;
                for (char c : portText) {
                    if (c < '0' || c > '9') return false;
                    v = v * 10 + static_cast<unsigned>(c - '0');
                }
                if (v == 0 || v > 65535) return false;
                port = v;
            }
            return true;
        }

        /// True if the Origin header value names the same host (and port) as the Host header.
        bool originMatchesHost(const std::string &origin, const std::string &hostHeader) {
            const std::size_t sep = origin.find("://");
            if (sep == std::string::npos) return false;
            std::string scheme = origin.substr(0, sep);
            for (char &c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            unsigned defPort = 0;
            if (scheme == "http" || scheme == "ws") defPort = 80;
            else if (scheme == "https" || scheme == "wss") defPort = 443;
            else return false;
            std::string_view auth = std::string_view(origin).substr(sep + 3);
            if (auth.find_first_of("/?#@") != std::string_view::npos) return false;
            std::string oHost, hHost;
            unsigned oPort = 0, hPort = 0;
            if (!splitAuthority(auth, oHost, oPort) || !splitAuthority(hostHeader, hHost, hPort)) return false;
            if (oHost != hHost) return false;
            if (oPort == 0) oPort = defPort;
            // A Host without a port is the default one of whichever scheme fronts the server.
            return hPort == 0 ? oPort == defPort : oPort == hPort;
        }

        bool originAllowed(const WsOptions &o, bool haveOrigin, const std::string &origin,
                           const std::string &hostHeader) {
            if (o.allowedOrigins.empty() && !o.originCheck) {
                // No policy: a browser Origin must be the server's own, unless the
                // same-origin rule is switched off. Strict mode refuses every one.
                if (!haveOrigin) return true;
                if (o.strictOrigin) return false;
                return !o.requireSameOrigin || originMatchesHost(origin, hostHeader);
            }
            if (!haveOrigin) return o.allowMissingOrigin;
            if (!o.allowedOrigins.empty()) {
                bool listed = false;
                for (const auto &allowed : o.allowedOrigins) {
                    if (iequals(allowed, origin)) { listed = true; break; }
                }
                if (!listed) return false;
            }
            if (o.originCheck && !o.originCheck(origin)) return false;
            return true;
        }

        /// Best-effort bodyless HTTP error on a connection that is about to be dropped.
        YarnBall::Task<void> rejectHandshake(TcpStream &stream, int status) {
            const char *reason = "Bad Request";
            std::string extra;
            switch (status) {
                case 401: reason = "Unauthorized"; break;
                case 403: reason = "Forbidden"; break;
                case 404: reason = "Not Found"; break;
                case 429: reason = "Too Many Requests"; break;
                case 500: reason = "Internal Server Error"; break;
                case 503: reason = "Service Unavailable"; break;
                case 426:
                    reason = "Upgrade Required";
                    extra = "Sec-WebSocket-Version: 13\r\n";
                    break;
                default:
                    if (status != 400) reason = status >= 500 ? "Server Error" : "Client Error";
                    break;
            }
            const std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" +
                                     extra + "Content-Length: 0\r\nConnection: close\r\n\r\n";
            co_await stream.write(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(resp.data()), resp.size()));
            co_return;
        }

        /// A handshake message split into its first line and its header fields.
        struct ParsedHead {
            std::string firstLine;
            std::vector<HttpHeader> headers;
        };

        /**
         * @brief Split a handshake head (the text up to and including the blank line)
         *        into first line and headers, strictly: lines end in CRLF only, a CR,
         *        LF or NUL anywhere else is refused, every field is "token: value"
         *        (so no obs-fold continuation and no colon-less junk is skipped over)
         *        and values carry no control characters.
         * @return false if the head is malformed.
         */
        bool parseHead(const std::string &raw, ParsedHead &out) {
            if (raw.size() < 4) return false;
            const std::string_view head(raw.data(), raw.size() - 4); // minus the closing CRLF CRLF
            for (std::size_t i = 0; i < head.size(); ++i) {
                const char c = head[i];
                if (c == '\0') return false;
                if (c == '\n' && (i == 0 || head[i - 1] != '\r')) return false;
                if (c == '\r' && (i + 1 >= head.size() || head[i + 1] != '\n')) return false;
            }
            std::size_t pos = 0;
            bool first = true;
            while (pos <= head.size()) {
                std::size_t eol = head.find("\r\n", pos);
                if (eol == std::string_view::npos) eol = head.size();
                const std::string_view line = head.substr(pos, eol - pos);
                pos = eol + 2;
                if (first) {
                    out.firstLine.assign(line);
                    first = false;
                    continue;
                }
                const auto colon = line.find(':');
                if (colon == std::string_view::npos || !detail::isHttpToken(line.substr(0, colon))) {
                    return false;
                }
                std::size_t lead = colon + 1;
                while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
                std::size_t end = line.size();
                while (end > lead && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
                HttpHeader h;
                h.name.assign(line.substr(0, colon));
                h.value.assign(line.substr(lead, end - lead));
                if (detail::hasFieldValueCtl(h.value)) return false;
                out.headers.push_back(std::move(h));
            }
            return !out.firstLine.empty();
        }

        /// Number of fields named @p name.
        std::size_t countHeaders(const std::vector<HttpHeader> &hs, std::string_view name) {
            std::size_t n = 0;
            for (const auto &h : hs) {
                if (iequals(h.name, name)) ++n;
            }
            return n;
        }

        /// Value of the first field named @p name (empty if absent).
        std::string firstHeader(const std::vector<HttpHeader> &hs, std::string_view name) {
            for (const auto &h : hs) {
                if (iequals(h.name, name)) return h.value;
            }
            return {};
        }

        /// The comma-separated tokens of every field named @p name. @p ok is cleared if
        /// one of them is not a valid token.
        std::vector<std::string> headerTokens(const std::vector<HttpHeader> &hs,
                                              std::string_view name, bool &ok) {
            std::vector<std::string> out;
            for (const auto &h : hs) {
                if (!iequals(h.name, name)) continue;
                std::string_view list = h.value;
                std::size_t i = 0;
                while (i <= list.size()) {
                    std::size_t j = list.find(',', i);
                    if (j == std::string_view::npos) j = list.size();
                    std::string_view t = list.substr(i, j - i);
                    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
                    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.remove_suffix(1);
                    if (!t.empty()) {
                        if (!detail::isHttpToken(t)) ok = false;
                        out.emplace_back(t);
                    }
                    i = j + 1;
                }
            }
            return out;
        }

        /// The values of every field named @p name joined with ", " (a list-valued field
        /// may be split over several lines, RFC 9110 5.3).
        std::string joinedHeader(const std::vector<HttpHeader> &hs, std::string_view name) {
            std::string out;
            for (const auto &h : hs) {
                if (!iequals(h.name, name)) continue;
                if (!out.empty()) out += ", ";
                out += h.value;
            }
            return out;
        }

        /**
         * @brief Reduce the request target of an upgrade to origin-form, the only
         *        shape a handshake hook can reason about. An absolute-form target
         *        ("http://host/path") loses its scheme and authority; anything that
         *        is not a path (a bare word, "*", an authority) or carries a fragment
         *        is refused. Returns false for the refusals.
         */
        bool normaliseTarget(std::string_view target, std::string &out) {
            if (target.find('#') != std::string_view::npos) return false;
            if (!target.empty() && target.front() == '/') {
                out.assign(target);
                return true;
            }
            std::size_t skip = 0;
            if (target.size() > 7 && iequals(target.substr(0, 7), "http://")) skip = 7;
            else if (target.size() > 8 && iequals(target.substr(0, 8), "https://")) skip = 8;
            if (skip == 0) return false;
            std::string_view rest = target.substr(skip);
            const std::size_t end = rest.find_first_of("/?");
            if (end == 0) return false; // no authority
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
            out = rest.empty() || rest.front() == '?' ? "/" + std::string(rest) : std::string(rest);
            return true;
        }

        bool hasToken(std::string_view list, std::string_view token) {
            std::size_t i = 0;
            while (i < list.size()) {
                std::size_t j = list.find(',', i);
                if (j == std::string_view::npos) j = list.size();
                std::string_view t = list.substr(i, j - i);
                while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
                while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.remove_suffix(1);
                if (iequals(t, token)) return true;
                i = j + 1;
            }
            return false;
        }

    } // anonymous namespace


    YarnBall::Task<WsConnection> WsConnection::serverHandshake(TcpStream stream, WsOptions options) {
        std::vector<std::byte> trailing;
        IoDeadline hs(stream.fd(), options.handshakeTimeout);
        std::string raw;
        try {
            raw = co_await readUntilHeaderEnd(stream, trailing, options.maxHandshakeBytes);
        } catch (const WsException &) {
            if (hs.expired()) throw WsException("WebSocket handshake timed out");
            throw;
        } catch (const SocketException &) {
            if (hs.expired()) throw WsException("WebSocket handshake timed out");
            throw;
        }

        // Parse the request line + headers (case-insensitive). Body
        // is irrelevant -- a WebSocket upgrade has no body.
        ParsedHead head;
        const bool wellFormed = parseHead(raw, head);

        int rejectStatus = 0;
        std::string why;
        auto refuse = [&](int status, std::string reason) {
            rejectStatus = status;
            why = std::move(reason);
        };

        WsHandshakeInfo info;
        std::string upgrade, connection, key, version, origin;
        bool haveOrigin = false;
        bool subprotocolsOk = true;
        if (!wellFormed) {
            refuse(400, "malformed WebSocket handshake request");
        } else {
            // Upgrade and Connection are token lists: all their fields count.
            upgrade = joinedHeader(head.headers, "upgrade");
            connection = joinedHeader(head.headers, "connection");
            key = firstHeader(head.headers, "sec-websocket-key");
            version = firstHeader(head.headers, "sec-websocket-version");
            origin = firstHeader(head.headers, "origin");
            haveOrigin = countHeaders(head.headers, "origin") != 0;
            info.offeredSubprotocols = headerTokens(head.headers, "sec-websocket-protocol", subprotocolsOk);
        }

        const std::string &requestLine = head.firstLine;
        std::string target;
        if (rejectStatus != 0) {
            // already refused
        } else if (requestLine.compare(0, 4, "GET ") != 0) {
            refuse(400, "WebSocket upgrade must be a GET request");
        } else if (requestLine.size() < 14 || requestLine.compare(requestLine.size() - 9, 9, " HTTP/1.1") != 0) {
            refuse(400, "WebSocket upgrade must be HTTP/1.1");
        } else if (detail::hasTargetCtl(std::string_view(requestLine).substr(4, requestLine.size() - 13))) {
            refuse(400, "invalid request target in WebSocket upgrade");
        } else if (!normaliseTarget(std::string_view(requestLine).substr(4, requestLine.size() - 13), target)) {
            refuse(400, "WebSocket upgrade target must be an origin-form path without a fragment");
        } else if (countHeaders(head.headers, "host") != 1 ||
                   joinedHeader(head.headers, "host").empty() ||
                   detail::hasTargetCtl(joinedHeader(head.headers, "host"))) {
            // HTTP/1.1 requires exactly one Host (RFC 9112 3.2).
            refuse(400, "WebSocket upgrade needs exactly one Host header");
        } else if (countHeaders(head.headers, "sec-websocket-key") > 1 ||
                   countHeaders(head.headers, "sec-websocket-version") > 1 ||
                   countHeaders(head.headers, "origin") > 1) {
            // A duplicated header is read one way here and another by a proxy in front.
            refuse(400, "duplicate header in WebSocket upgrade");
        } else if (!hasToken(upgrade, "websocket")) {
            refuse(400, "missing or wrong Upgrade header");
        } else if (!hasToken(connection, "upgrade")) {
            // Connection may be "Upgrade" or list it among other tokens.
            refuse(400, "missing Upgrade in Connection header");
        } else if (version != "13") {
            refuse(426, "unsupported Sec-WebSocket-Version: " + sanitize(version));
        } else if (!validWsKey(key)) {
            // Key is a base64-encoded 16-byte nonce: 24 characters ending "==".
            refuse(400, "invalid Sec-WebSocket-Key");
        } else if (!subprotocolsOk) {
            refuse(400, "invalid Sec-WebSocket-Protocol");
        } else if (!originAllowed(options, haveOrigin, origin, joinedHeader(head.headers, "host"))) {
            refuse(403, "WebSocket Origin not allowed: " + sanitize(origin));
        }

        std::string chosenSubprotocol;
        if (rejectStatus == 0) {
            info.path = std::move(target);
            info.headers = std::move(head.headers);
            if (options.onHandshake) {
                WsHandshakeDecision decision;
                bool hookThrew = false;
                try {
                    decision = options.onHandshake(info);
                } catch (...) {
                    hookThrew = true;
                }
                if (hookThrew) {
                    refuse(500, "WebSocket handshake hook failed");
                } else if (decision.status != 0) {
                    refuse((decision.status >= 400 && decision.status <= 599) ? decision.status : 403,
                           "WebSocket upgrade refused by the handshake hook");
                } else if (!decision.subprotocol.empty()) {
                    bool offered = false;
                    for (const auto &o : info.offeredSubprotocols) {
                        if (o == decision.subprotocol) offered = true;
                    }
                    if (!offered || !detail::isHttpToken(decision.subprotocol)) {
                        refuse(500, "handshake hook chose a subprotocol the client did not offer");
                    } else {
                        chosenSubprotocol = std::move(decision.subprotocol);
                    }
                }
            }
        }
        if (rejectStatus != 0) {
            try {
                co_await rejectHandshake(stream, rejectStatus);
            } catch (...) {
                // The peer is gone; the refusal is thrown below either way.
            }
            throw WsException(why);
        }
        info.subprotocol = chosenSubprotocol;

        const std::string accept = computeAccept(key);
        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n";
        if (!chosenSubprotocol.empty()) resp += "Sec-WebSocket-Protocol: " + chosenSubprotocol + "\r\n";
        resp += "\r\n";
        try {
            co_await stream.write(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(resp.data()), resp.size()));
        } catch (const SocketException &) {
            if (hs.expired()) throw WsException("WebSocket handshake timed out");
            throw;
        }
        hs.disarm(); // before the fd moves into the connection

        co_return WsConnection(std::make_shared<Impl>(std::move(stream), /*server=*/true,
                                                      std::move(trailing), std::move(options),
                                                      std::move(info)));
    }

    YarnBall::Task<WsConnection> WsConnection::connect(std::string host,
                                                         std::uint16_t port,
                                                         std::string path,
                                                         WsOptions options) {
        // Validated before anything is sent or connected: the host and the target go
        // into the request line and Host header verbatim, so a CR, LF or space in
        // either would let a caller-supplied string inject headers or a second request.
        if (path.empty()) path = "/";
        if (host.empty() || detail::hasTargetCtl(host) || detail::hasTargetCtl(path)) {
            throw WsException("invalid host or path for WebSocket connect");
        }
        std::string offered;
        for (const auto &p : options.subprotocols) {
            if (!detail::isHttpToken(p)) throw WsException("invalid WebSocket subprotocol");
            if (!offered.empty()) offered += ", ";
            offered += p;
        }

        TcpStream s = co_await tcpConnect(host, port, options.connectTimeout);
        IoDeadline hs(s.fd(), options.handshakeTimeout);

        const auto nonce = randomNonce();
        const std::string key = base64Encode(nonce.data(), nonce.size());

        std::string req =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + detail::hostHeaderValue(host, port, 0) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n";
        if (!offered.empty()) req += "Sec-WebSocket-Protocol: " + offered + "\r\n";
        req += "\r\n";

        std::vector<std::byte> trailing;
        std::string raw;
        try {
            co_await s.write(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(req.data()), req.size()));
            raw = co_await readUntilHeaderEnd(s, trailing, options.maxHandshakeBytes);
        } catch (const WsException &) {
            if (hs.expired()) throw WsException("WebSocket handshake timed out");
            throw;
        } catch (const SocketException &) {
            if (hs.expired()) throw WsException("WebSocket handshake timed out");
            throw;
        }
        hs.disarm();

        ParsedHead head;
        if (!parseHead(raw, head)) throw WsException("malformed WebSocket handshake response");
        {
            // "HTTP/1.1 101 ..." -- the status code token must be exactly 101.
            const std::string &line = head.firstLine;
            const auto sp = line.find(' ');
            if (sp == std::string::npos || line.compare(0, 5, "HTTP/") != 0 ||
                line.compare(sp + 1, 3, "101") != 0 ||
                (line.size() > sp + 4 && line[sp + 4] != ' ')) {
                throw WsException("server did not upgrade: " + sanitize(line));
            }
        }
        const std::string acceptHeader = firstHeader(head.headers, "sec-websocket-accept");
        const std::string upgrade = firstHeader(head.headers, "upgrade");
        const std::string connection = firstHeader(head.headers, "connection");

        if (!iequals(upgrade, "websocket") || !hasToken(connection, "upgrade")) {
            throw WsException("server response lacks Upgrade: websocket / Connection: Upgrade");
        }
        const std::string expected = computeAccept(key);
        if (acceptHeader != expected) {
            throw WsException("Sec-WebSocket-Accept mismatch");
        }
        // RFC 6455 4.1: an extension or subprotocol the client did not ask for fails the connection.
        if (countHeaders(head.headers, "sec-websocket-extensions") != 0) {
            throw WsException("server selected a WebSocket extension that was not requested");
        }
        WsHandshakeInfo info;
        info.path = std::move(path);
        {
            bool ok = true;
            const auto chosen = headerTokens(head.headers, "sec-websocket-protocol", ok);
            if (!ok || chosen.size() > 1) throw WsException("invalid Sec-WebSocket-Protocol in response");
            if (!chosen.empty()) {
                bool requested = false;
                for (const auto &p : options.subprotocols) {
                    if (p == chosen.front()) requested = true;
                }
                if (!requested) throw WsException("server selected a WebSocket subprotocol that was not requested");
                info.subprotocol = chosen.front();
            }
        }
        info.headers = std::move(head.headers);
        co_return WsConnection(std::make_shared<Impl>(std::move(s), /*server=*/false,
                                                      std::move(trailing), std::move(options),
                                                      std::move(info)));
    }

    WsConnection &WsConnection::operator=(WsConnection &&other) noexcept {
        if (this != &other) {
            WsConnection taken(std::move(other));
            std::swap(this->impl, taken.impl); // `taken` now holds, and on scope exit drops, our old state
        }
        return *this;
    }

    WsConnection::~WsConnection() {
        // Dropping the handle drops the connection. An operation still parked on the
        // socket keeps the heap state alive, so shut the socket down to wake it
        // instead of leaving the peer connected until that operation ends.
        if (this->impl) {
            this->impl->open.store(false);
            const int fd = this->impl->stream.fd();
            if (fd >= 0) detail::shutdownSocket(fd);
            this->impl.reset();
        }
    }

    const WsHandshakeInfo &WsConnection::handshake() const noexcept {
        static const WsHandshakeInfo empty;
        return this->impl ? this->impl->info : empty;
    }

    bool WsConnection::isOpen() const noexcept {
        return this->impl && this->impl->open.load(std::memory_order_acquire);
    }

    namespace {

        YarnBall::Task<WsMessage> receiveImpl(std::shared_ptr<WsConnection::Impl> im) {
            if (!im || !im->open.load()) {
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
            const std::size_t maxMessage = im->opts.maxMessageBytes;
            // Non-final continuation frames with no payload add nothing to the message
            // yet each one re-arms the frame timeout; a run of them is an attack.
            constexpr std::size_t kMaxEmptyFragments = 32;
            std::size_t emptyFragments = 0;

            while (true) {
                WsFrame f;
                std::uint16_t violationCode = 0;
                std::string violation;
                try {
                    f = co_await readFrame(*im, assembling, assembling ? assemblyBuf.size() : 0);
                } catch (const WsFrameError &e) {
                    // co_await is not allowed in a handler: fail just below.
                    violationCode = e.code;
                    violation = e.what();
                } catch (const WsException &) {
                    im->open.store(false); // framing is lost; the stream is unusable
                    if (im->readDeadline.expired()) throw WsException("WebSocket read timed out");
                    throw;
                } catch (const SocketException &) {
                    im->open.store(false);
                    if (im->readDeadline.expired()) throw WsException("WebSocket read timed out");
                    throw;
                }
                if (violationCode != 0) co_await failConnection(im, violationCode, violation);
                switch (f.opcode) {
                    case kOpText:
                    case kOpBinary: {
                        if (assembling) {
                            // RFC §5.4: cannot start a new data message
                            // while a previous one is still being framed.
                            co_await failConnection(im, 1002, "new data frame mid-message");
                        }
                        if (f.fin) {
                            WsMessage m;
                            m.type = (f.opcode == kOpText) ? WsMessageType::Text
                                                              : WsMessageType::Binary;
                            if (m.type == WsMessageType::Text &&
                                !validUtf8(f.payload.data(), f.payload.size())) {
                                co_await failConnection(im, 1007, "invalid UTF-8 in text message");
                            }
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
                            co_await failConnection(im, 1002, "orphan continuation frame");
                        }
                        if (f.payload.empty() && !f.fin) {
                            if (++emptyFragments > kMaxEmptyFragments) {
                                co_await failConnection(im, 1008, "too many empty message fragments");
                            }
                        } else {
                            emptyFragments = 0;
                        }
                        if (maxMessage != 0 && assemblyBuf.size() + f.payload.size() > maxMessage) {
                            co_await failConnection(im, 1009, "multi-frame message exceeds the size cap");
                        }
                        assemblyBuf.insert(assemblyBuf.end(),
                                            f.payload.begin(), f.payload.end());
                        if (f.fin) {
                            WsMessage m;
                            m.type = assemblyType;
                            if (m.type == WsMessageType::Text &&
                                !validUtf8(assemblyBuf.data(), assemblyBuf.size())) {
                                co_await failConnection(im, 1007, "invalid UTF-8 in text message");
                            }
                            m.payload = std::move(assemblyBuf);
                            co_return m;
                        }
                        break;
                    }
                    case kOpPing: {
                        // Echo payload as Pong. Control frames are allowed
                        // to interleave with continuation; assembly state
                        // stays untouched. The write lock keeps the Pong
                        // from landing inside another task's frame.
                        if (im->opts.maxPingsPerSecond != 0) {
                            const auto now = std::chrono::steady_clock::now();
                            if (now - im->pingWindowStart >= std::chrono::seconds{1}) {
                                im->pingWindowStart = now;
                                im->pingsInWindow = 0;
                            }
                            // Past the budget a Ping goes unanswered (RFC 6455 5.5.2 lets an
                            // endpoint answer only some of them).
                            if (++im->pingsInWindow > im->opts.maxPingsPerSecond) break;
                        }
                        co_await sendLocked(im, kOpPong, f.payload, true);
                        break;
                    }
                    case kOpPong:
                        // Unsolicited Pong is allowed and ignored.
                        break;
                    case kOpClose: {
                        // RFC 6455 5.5.1: empty, or a 2-byte code (valid
                        // range, not reserved) plus a UTF-8 reason.
                        bool bad = f.payload.size() == 1;
                        if (f.payload.size() >= 2) {
                            const unsigned code =
                                (std::to_integer<unsigned>(f.payload[0]) << 8) |
                                std::to_integer<unsigned>(f.payload[1]);
                            const bool okCode =
                                (code >= 1000 && code <= 1014 && code != 1004 &&
                                 code != 1005 && code != 1006) ||
                                (code >= 3000 && code <= 4999);
                            bad = !okCode ||
                                  !validUtf8(f.payload.data() + 2, f.payload.size() - 2);
                        }
                        if (bad) {
                            co_await failConnection(im, 1002, "invalid Close frame payload");
                        }
                        if (im->open.exchange(false)) {
                            // Echo the Close frame back. The peer often
                            // closes the socket immediately after sending
                            // its Close, so the write can race with the
                            // peer's RST -- on Linux that surfaces as
                            // EPIPE / ECONNRESET. Either way the
                            // connection is gone, so swallow the transport
                            // error and proceed to close locally.
                            try {
                                co_await sendLocked(im, kOpClose, f.payload, true);
                            } catch (const SocketException &) {
                                // Peer torn down before our echo landed.
                            } catch (const WsException &) {
                                // Echo timed out; same outcome.
                            }
                        }
                        throw WsException("connection closed by peer");
                    }
                    default:
                        co_await failConnection(im, 1002, "unknown opcode");
                }
            }
        }

        YarnBall::Task<void> sendDataImpl(std::shared_ptr<WsConnection::Impl> im, std::uint8_t op,
                                            std::span<const std::byte> payload, bool isFinal) {
            if (!im || !im->open.load()) throw WsException("send on closed WsConnection");
            co_await sendLocked(im, op, payload, isFinal);
            co_return;
        }

        YarnBall::Task<void> closeImpl(std::shared_ptr<WsConnection::Impl> im, std::uint16_t code,
                                         std::string reason) {
            if (!im || !im->open.exchange(false)) co_return;
            const std::vector<std::byte> body = closeBody(code, reason);
            // Same peer-disappeared race as the receive() Close-echo path:
            // if the peer has already closed the socket, our Close write
            // can surface as EPIPE / ECONNRESET on Linux. Swallow it;
            // the local connection is closing either way.
            try {
                co_await sendLocked(im, kOpClose, body, true);
            } catch (const SocketException &) {
                // Peer already gone; nothing left to flush.
            } catch (const WsException &) {
                // Write timed out; the connection is closing regardless.
            }
            co_return;
        }

    } // anonymous namespace

    // The public operations are plain functions that hand the shared state to a
    // free coroutine, so nothing in a running operation depends on `this` (a Task is
    // lazy; the handle may be moved or destroyed before its first resume).

    YarnBall::Task<WsMessage> WsConnection::receive() {
        return receiveImpl(this->impl);
    }

    YarnBall::Task<void> WsConnection::sendText(std::string_view payload) {
        return sendDataImpl(this->impl, kOpText,
                            std::span<const std::byte>(
                                reinterpret_cast<const std::byte *>(payload.data()),
                                payload.size()),
                            true);
    }

    YarnBall::Task<void> WsConnection::sendBinary(std::span<const std::byte> payload) {
        return sendDataImpl(this->impl, kOpBinary, payload, true);
    }

    YarnBall::Task<void> WsConnection::sendFrame(FragmentKind kind,
                                                    std::span<const std::byte> payload,
                                                    bool isFinal) {
        std::uint8_t op;
        switch (kind) {
            case FragmentKind::Text:         op = kOpText; break;
            case FragmentKind::Binary:       op = kOpBinary; break;
            case FragmentKind::Continuation: op = kOpContinuation; break;
            default:                          op = kOpContinuation; break;
        }
        return sendDataImpl(this->impl, op, payload, isFinal);
    }

    YarnBall::Task<void> WsConnection::close(std::uint16_t code,
                                              std::string_view reason) {
        return closeImpl(this->impl, code, std::string(reason));
    }

    namespace {

        /// One accepted connection: handshake under the options' limits, then the handler.
        YarnBall::Task<void> wsServeConnection(std::shared_ptr<const WsHandler> handler,
                                                 std::shared_ptr<const WsOptions> options,
                                                 TcpStream stream) {
            try {
                WsConnection ws = co_await WsConnection::serverHandshake(std::move(stream), *options);
                co_await (*handler)(std::move(ws));
            } catch (...) {
                // A refused or failed handshake, or a handler that threw: the connection
                // is gone either way, and nothing may escape a detached task.
            }
            co_return;
        }

    } // anonymous namespace

    YarnBall::Task<void> wsServe(TcpListener listener,
                                 WsHandler handler,
                                 WsOptions options,
                                 std::stop_token stop) {
        auto sharedHandler = std::make_shared<const WsHandler>(std::move(handler));
        auto sharedOptions = std::make_shared<const WsOptions>(std::move(options));
        const std::size_t maxConnections = sharedOptions->maxConnections;
        // tcpServe holds a ConnectionLimiter slot from accept until the connection task
        // ends, which is what makes the cap cover connections still in their handshake.
        auto factory = [sharedHandler, sharedOptions](TcpStream stream) -> YarnBall::Task<void> {
            return wsServeConnection(sharedHandler, sharedOptions, std::move(stream));
        };
        co_await tcpServe(std::move(listener), factory, stop, maxConnections);
        co_return;
    }

}
