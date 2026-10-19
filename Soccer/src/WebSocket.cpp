//
// Created by Fabrizio Paino on 2026-05-16.
//

#include "WebSocket.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <sstream>
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
        Impl(TcpStream s, bool server, std::vector<std::byte> pre, WsOptions o)
            : stream(std::move(s)), preBuf(std::move(pre)), serverSide(server), opts(std::move(o)) {
        }

        TcpStream stream;
        /// Bytes the handshake reader over-consumed past the header terminator;
        /// frame reads drain them before touching the socket (a peer may send its
        /// first frame in the same segment as the 101).
        std::vector<std::byte> preBuf;
        std::size_t preOffset{0};
        bool serverSide;
        std::atomic<bool> open{true};
        WsOptions opts;
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

        YarnBall::Task<WsFrame> readFrame(WsConnection::Impl &im, bool assembling) {
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
            } else if (o.maxMessageBytes != 0 && plen > o.maxMessageBytes) {
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
            const DisarmOnExit disarm{im->writeDeadline};
            im->writeDeadline.arm(im->stream.fd(), im->opts.writeTimeout);
            try {
                co_await writeFrame(im->stream, opcode, payload, !im->serverSide, isFinal);
            } catch (const SocketException &) {
                if (im->writeDeadline.expired()) throw WsException("WebSocket write timed out");
                throw;
            }
            co_return;
        }

        std::vector<std::byte> closeBody(std::uint16_t code, std::string_view reason) {
            // A control frame payload is at most 125 bytes: 2 for the code
            // plus up to 123 of reason.
            if (reason.size() > 123) reason = reason.substr(0, 123);
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

        bool originAllowed(const WsOptions &o, bool haveOrigin, const std::string &origin) {
            if (o.allowedOrigins.empty() && !o.originCheck) return true;
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
            if (status == 403) reason = "Forbidden";
            if (status == 426) {
                reason = "Upgrade Required";
                extra = "Sec-WebSocket-Version: 13\r\n";
            }
            const std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" +
                                     extra + "Content-Length: 0\r\nConnection: close\r\n\r\n";
            co_await stream.write(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(resp.data()), resp.size()));
            co_return;
        }

        /// Peel "Name: value" lines (after the first line) into a callback.
        template<typename Fn>
        void forEachHeader(std::istringstream &iss, Fn &&fn) {
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                const auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string name = line.substr(0, colon);
                std::size_t lead = colon + 1;
                while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
                std::size_t end = line.size();
                while (end > lead && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
                fn(name, line.substr(lead, end - lead));
            }
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
        const auto headerEnd = raw.find("\r\n\r\n");
        std::string headers = raw.substr(0, headerEnd);

        std::istringstream iss(headers);
        std::string requestLine;
        std::getline(iss, requestLine);
        if (!requestLine.empty() && requestLine.back() == '\r') requestLine.pop_back();

        std::string upgrade, connection, key, version, origin;
        bool haveOrigin = false;
        forEachHeader(iss, [&](const std::string &name, std::string value) {
            if (iequals(name, "upgrade"))         upgrade = std::move(value);
            else if (iequals(name, "connection")) connection = std::move(value);
            else if (iequals(name, "sec-websocket-key")) key = std::move(value);
            else if (iequals(name, "sec-websocket-version")) version = std::move(value);
            else if (iequals(name, "origin")) { origin = std::move(value); haveOrigin = true; }
        });

        int rejectStatus = 0;
        std::string why;
        auto refuse = [&](int status, std::string reason) {
            rejectStatus = status;
            why = std::move(reason);
        };
        if (requestLine.compare(0, 4, "GET ") != 0) {
            refuse(400, "WebSocket upgrade must be a GET request");
        } else if (requestLine.size() < 13 || requestLine.compare(requestLine.size() - 9, 9, " HTTP/1.1") != 0) {
            refuse(400, "WebSocket upgrade must be HTTP/1.1");
        } else if (!iequals(upgrade, "websocket")) {
            refuse(400, "missing or wrong Upgrade header");
        } else if (!hasToken(connection, "upgrade")) {
            // Connection may be "Upgrade" or list it among other tokens.
            refuse(400, "missing Upgrade in Connection header");
        } else if (version != "13") {
            refuse(426, "unsupported Sec-WebSocket-Version: " + version);
        } else if (key.size() != 24 || key.compare(22, 2, "==") != 0) {
            // Key is a base64-encoded 16-byte nonce: 24 characters ending "==".
            refuse(400, "invalid Sec-WebSocket-Key");
        } else if (!originAllowed(options, haveOrigin, origin)) {
            refuse(403, "WebSocket Origin not allowed: " + origin);
        }
        if (rejectStatus != 0) {
            try {
                co_await rejectHandshake(stream, rejectStatus);
            } catch (...) {
                // The peer is gone; the refusal is thrown below either way.
            }
            throw WsException(why);
        }

        const std::string accept = computeAccept(key);
        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        try {
            co_await stream.write(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(resp.data()), resp.size()));
        } catch (const SocketException &) {
            if (hs.expired()) throw WsException("WebSocket handshake timed out");
            throw;
        }
        hs.disarm(); // before the fd moves into the connection

        co_return WsConnection(std::make_shared<Impl>(std::move(stream), /*server=*/true,
                                                      std::move(trailing), std::move(options)));
    }

    YarnBall::Task<WsConnection> WsConnection::connect(std::string host,
                                                         std::uint16_t port,
                                                         std::string path,
                                                         WsOptions options) {
        TcpStream s = co_await tcpConnect(host, port, options.connectTimeout);
        IoDeadline hs(s.fd(), options.handshakeTimeout);

        const auto nonce = randomNonce();
        const std::string key = base64Encode(nonce.data(), nonce.size());

        std::string req =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + ":" + std::to_string(port) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";

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

        const auto headerEnd = raw.find("\r\n\r\n");
        std::string headers = raw.substr(0, headerEnd);

        std::istringstream iss(headers);
        std::string line;
        if (!std::getline(iss, line)) throw WsException("no status line in response");
        if (!line.empty() && line.back() == '\r') line.pop_back();
        {
            // "HTTP/1.1 101 ..." -- the status code token must be exactly 101.
            const auto sp = line.find(' ');
            if (sp == std::string::npos || line.compare(0, 5, "HTTP/") != 0 ||
                line.compare(sp + 1, 3, "101") != 0 ||
                (line.size() > sp + 4 && line[sp + 4] != ' ')) {
                throw WsException("server did not upgrade: " + line);
            }
        }
        std::string acceptHeader, upgrade, connection;
        forEachHeader(iss, [&](const std::string &name, std::string value) {
            if (iequals(name, "sec-websocket-accept")) acceptHeader = std::move(value);
            else if (iequals(name, "upgrade")) upgrade = std::move(value);
            else if (iequals(name, "connection")) connection = std::move(value);
        });

        if (!iequals(upgrade, "websocket") || !hasToken(connection, "upgrade")) {
            throw WsException("server response lacks Upgrade: websocket / Connection: Upgrade");
        }
        const std::string expected = computeAccept(key);
        if (acceptHeader != expected) {
            throw WsException("Sec-WebSocket-Accept mismatch");
        }
        co_return WsConnection(std::make_shared<Impl>(std::move(s), /*server=*/false,
                                                      std::move(trailing), std::move(options)));
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

            while (true) {
                WsFrame f;
                std::uint16_t violationCode = 0;
                std::string violation;
                try {
                    f = co_await readFrame(*im, assembling);
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
                                (code >= 1000 && code <= 1011 && code != 1004 &&
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
