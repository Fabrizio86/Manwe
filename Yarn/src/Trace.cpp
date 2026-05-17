//
// Created by Fabrizio Paino on 2026-05-16.
//

#include "Trace.h"

#include <random>

namespace YarnBall::trace {

    namespace {
        /**
         * @brief Thread-local current context. Read by @c current(),
         *        written by @c Scope's lifecycle.
         */
        thread_local Context g_current{};

        /**
         * @brief Per-thread PRNG for id generation. Seeded once per
         *        thread on first call.
         */
        std::mt19937_64 &rng() {
            static thread_local std::mt19937_64 gen(std::random_device{}());
            return gen;
        }

        /**
         * @brief Render a byte span as lowercase hex. @p out must have
         *        room for @c 2 * bytes.size() characters.
         */
        void hexEncode(const std::uint8_t *bytes, std::size_t n, char *out) {
            static constexpr char kHex[] = "0123456789abcdef";
            for (std::size_t i = 0; i < n; ++i) {
                out[i * 2]     = kHex[(bytes[i] >> 4) & 0x0F];
                out[i * 2 + 1] = kHex[bytes[i] & 0x0F];
            }
        }

        /**
         * @brief Decode a hex character to its 4-bit value, or @c -1
         *        if the character is not a hex digit.
         */
        int hexNibble(char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        /**
         * @brief Decode @p hexLen hex characters from @p in into
         *        bytes (each pair becomes one byte). Returns @c false
         *        if any character is not a hex digit. @p out must
         *        have room for @c hexLen/2 bytes.
         */
        bool hexDecode(const char *in, std::size_t hexLen, std::uint8_t *out) {
            if (hexLen % 2 != 0) return false;
            for (std::size_t i = 0; i < hexLen; i += 2) {
                const int hi = hexNibble(in[i]);
                const int lo = hexNibble(in[i + 1]);
                if (hi < 0 || lo < 0) return false;
                out[i / 2] = static_cast<std::uint8_t>((hi << 4) | lo);
            }
            return true;
        }

        bool allZero(const std::uint8_t *b, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) if (b[i] != 0) return false;
            return true;
        }
    }

    Context newRoot() {
        Context c;
        auto &g = rng();
        for (std::size_t i = 0; i < c.traceId.size(); i += 8) {
            std::uint64_t v = g();
            for (int j = 0; j < 8 && i + j < c.traceId.size(); ++j) {
                c.traceId[i + j] = static_cast<std::uint8_t>(v >> (j * 8));
            }
        }
        std::uint64_t s = g();
        for (std::size_t i = 0; i < c.spanId.size(); ++i) {
            c.spanId[i] = static_cast<std::uint8_t>(s >> (i * 8));
        }
        // Guarantee non-zero ids; the spec treats all-zero as invalid.
        if (allZero(c.traceId.data(), c.traceId.size())) c.traceId[0] = 1;
        if (allZero(c.spanId.data(), c.spanId.size())) c.spanId[0] = 1;
        c.flags = 0x01; // sampled
        return c;
    }

    Context newChild(const Context &parent) {
        Context c = parent;
        std::uint64_t s = rng()();
        for (std::size_t i = 0; i < c.spanId.size(); ++i) {
            c.spanId[i] = static_cast<std::uint8_t>(s >> (i * 8));
        }
        if (allZero(c.spanId.data(), c.spanId.size())) c.spanId[0] = 1;
        return c;
    }

    Context parseTraceparent(std::string_view header) {
        // Strict W3C: 00-<32hex>-<16hex>-<2hex>. Length must match.
        if (header.size() != 55) return {};
        if (header[0] != '0' || header[1] != '0') return {};
        if (header[2] != '-' || header[35] != '-' || header[52] != '-') return {};
        Context c{};
        if (!hexDecode(header.data() + 3,  32, c.traceId.data())) return {};
        if (!hexDecode(header.data() + 36, 16, c.spanId.data())) return {};
        std::uint8_t f;
        if (!hexDecode(header.data() + 53, 2, &f)) return {};
        c.flags = f;
        if (allZero(c.traceId.data(), c.traceId.size())) return {};
        if (allZero(c.spanId.data(), c.spanId.size())) return {};
        return c;
    }

    std::string toTraceparent(const Context &ctx) {
        if (ctx.empty()) return {};
        char buf[55];
        buf[0] = '0'; buf[1] = '0'; buf[2] = '-';
        hexEncode(ctx.traceId.data(), 16, buf + 3);
        buf[35] = '-';
        hexEncode(ctx.spanId.data(), 8, buf + 36);
        buf[52] = '-';
        hexEncode(&ctx.flags, 1, buf + 53);
        return std::string(buf, 55);
    }

    std::string hexTraceId(const Context &ctx) {
        char buf[32];
        hexEncode(ctx.traceId.data(), 16, buf);
        return std::string(buf, 32);
    }

    std::string hexSpanId(const Context &ctx) {
        char buf[16];
        hexEncode(ctx.spanId.data(), 8, buf);
        return std::string(buf, 16);
    }

    const Context &current() noexcept {
        return g_current;
    }

    Scope::Scope(const Context &ctx) noexcept : saved(g_current) {
        g_current = ctx;
    }

    Scope::~Scope() noexcept {
        g_current = this->saved;
    }

}
