//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef SOCCER_ICMP_ECHO_H
#define SOCCER_ICMP_ECHO_H

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "SocketException.h"

namespace Soccer {

    /**
     * @brief ICMP message types used by Echo.
     */
    enum class IcmpType : std::uint8_t {
        EchoReply = 0,
        EchoRequest = 8,
    };

    /**
     * @brief Wire-format ICMPv4 Echo header (RFC 792). Packed so it lays
     *        out exactly as the kernel and peers expect.
     *
     * Fields are big-endian on the wire. @ref IcmpEcho::build and
     * @ref IcmpEcho::parse handle the conversion.
     *
     * Packing: GCC/Clang use @c __attribute__((packed)); MSVC has no
     * equivalent attribute, so we use @c #pragma pack instead. The
     * static_assert below catches any drift from the wire size.
     */
#if defined(_MSC_VER)
    #pragma pack(push, 1)
    struct IcmpEchoHeader {
        std::uint8_t type;
        std::uint8_t code;
        std::uint16_t checksum;   ///< In network byte order on the wire.
        std::uint16_t identifier; ///< In network byte order on the wire.
        std::uint16_t sequence;   ///< In network byte order on the wire.
    };
    #pragma pack(pop)
#else
    struct __attribute__((packed)) IcmpEchoHeader {
        std::uint8_t type;
        std::uint8_t code;
        std::uint16_t checksum;   ///< In network byte order on the wire.
        std::uint16_t identifier; ///< In network byte order on the wire.
        std::uint16_t sequence;   ///< In network byte order on the wire.
    };
#endif

    static_assert(sizeof(IcmpEchoHeader) == 8,
                  "IcmpEchoHeader must be exactly 8 bytes wire-format");

    /**
     * @class IcmpEcho
     * @brief Stateless helpers to build and parse ICMPv4 Echo Request /
     *        Reply packets. The actual send / receive happens through a
     *        @c RawSocket opened with @c IPPROTO_ICMP. IPv4 only: ICMPv6 uses
     *        other type numbers and a pseudo-header checksum.
     */
    class IcmpEcho {
    public:
        /**
         * @brief Compute the 16-bit RFC 1071 internet checksum over @p data.
         *        The result is in host byte order; place it into a packet
         *        as-is (the checksum field is computed before byte-swap).
         */
        static std::uint16_t checksum(std::span<const std::byte> data) noexcept {
            return finish(accumulate(0, data));
        }

        /// Largest echo payload that still fits one IPv4 datagram (65535 - 20 - 8).
        static constexpr std::size_t kMaxPayload = 65507;

        /**
         * @brief Build an ICMP Echo Request packet (header + payload).
         *
         * @param identifier 16-bit echo identifier (commonly the PID).
         * @param sequence   16-bit echo sequence number (caller-incremented).
         * @param payload    Optional payload bytes (RFC 792 allows any).
         * @return A heap-allocated byte vector ready for @c sendto.
         * @throws SocketException if @p payload is longer than @ref kMaxPayload.
         */
        static std::vector<std::byte> buildRequest(std::uint16_t identifier,
                                                    std::uint16_t sequence,
                                                    std::span<const std::byte> payload = {}) {
            if (payload.size() > kMaxPayload) {
                throw SocketException("IcmpEcho::buildRequest: payload too large");
            }
            std::vector<std::byte> packet(sizeof(IcmpEchoHeader) + payload.size());

            auto *hdr = reinterpret_cast<IcmpEchoHeader *>(packet.data());
            hdr->type = static_cast<std::uint8_t>(IcmpType::EchoRequest);
            hdr->code = 0;
            hdr->checksum = 0; // computed below over the zero'd field
            hdr->identifier = htons_u16(identifier);
            hdr->sequence = htons_u16(sequence);

            if (!payload.empty()) {
                std::memcpy(packet.data() + sizeof(IcmpEchoHeader),
                            payload.data(), payload.size());
            }

            const std::uint16_t cs = checksum(std::span<const std::byte>(packet));
            // checksum field is network byte order; htons of the host value.
            hdr->checksum = htons_u16(cs);

            return packet;
        }

        /**
         * @brief Parsed view of a received Echo Reply.
         */
        struct ParsedReply {
            IcmpType type;
            std::uint8_t code;
            std::uint16_t identifier; ///< host byte order
            std::uint16_t sequence;   ///< host byte order
            std::span<const std::byte> payload;
            bool checksum_valid;
        };

        /**
         * @brief Parse a received ICMPv4 packet.
         *
         * @param data       Bytes the kernel handed back (often with the IP
         *                   header on Linux raw sockets; if so, pass the
         *                   slice past it).
         * @param skip_ip    If true, skips the IPv4 header by reading IHL
         *                   from the first byte. Use on Linux SOCK_RAW.
         *                   macOS SOCK_DGRAM ICMP sockets strip it already.
         * @return std::nullopt if the packet is too short to be valid, is not
         *         IPv4 ICMP when @p skip_ip is set, or is not an Echo Request /
         *         Reply (other ICMP types, such as Destination Unreachable, do
         *         not have the echo layout). Compare @c type with
         *         @c IcmpType::EchoReply before treating it as an answer.
         */
        static std::optional<ParsedReply> parse(std::span<const std::byte> data,
                                                bool skip_ip = false) {
            if (skip_ip) {
                if (data.empty()) return std::nullopt;
                const auto first = static_cast<std::uint8_t>(data[0]);
                const std::size_t ihl = (first & 0x0F) * 4;
                if ((first >> 4) != 4) return std::nullopt; // not IPv4
                if (ihl < 20 || data.size() < ihl) return std::nullopt;
                if (static_cast<std::uint8_t>(data[9]) != 1) return std::nullopt; // not ICMP
                data = data.subspan(ihl);
            }
            if (data.size() < sizeof(IcmpEchoHeader)) return std::nullopt;

            const auto *hdr = reinterpret_cast<const IcmpEchoHeader *>(data.data());
            if (hdr->type != static_cast<std::uint8_t>(IcmpType::EchoReply) &&
                hdr->type != static_cast<std::uint8_t>(IcmpType::EchoRequest)) {
                return std::nullopt;
            }
            ParsedReply out;
            out.type = static_cast<IcmpType>(hdr->type);
            out.code = hdr->code;
            out.identifier = ntohs_u16(hdr->identifier);
            out.sequence = ntohs_u16(hdr->sequence);
            out.payload = data.subspan(sizeof(IcmpEchoHeader));

            // Re-verify the checksum with its own field counted as zero (bytes 2-3),
            // without copying the packet.
            const std::uint16_t wire = ntohs_u16(hdr->checksum);
            std::uint64_t sum = accumulate(0, data.first(2));
            sum = accumulate(sum, data.subspan(4));
            out.checksum_valid = (wire == finish(sum));
            return out;
        }

    private:
        /// Add @p data, read as big-endian 16-bit words (an odd tail byte is padded with
        /// zero), to @p sum. 64-bit, so no packet length can overflow it.
        static std::uint64_t accumulate(std::uint64_t sum, std::span<const std::byte> data) noexcept {
            const auto *p = reinterpret_cast<const std::uint8_t *>(data.data());
            std::size_t len = data.size();
            while (len >= 2) {
                sum += (static_cast<std::uint32_t>(p[0]) << 8) | p[1];
                p += 2;
                len -= 2;
            }
            if (len == 1) sum += static_cast<std::uint32_t>(p[0]) << 8;
            return sum;
        }

        /// Fold the carries back in and complement.
        static std::uint16_t finish(std::uint64_t sum) noexcept {
            while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
            return static_cast<std::uint16_t>(~sum & 0xFFFF);
        }

        /**
         * @brief Host -> network byte order for a 16-bit value, without
         *        pulling in @c <arpa/inet.h> from this header. A swap on
         *        little-endian hosts, the identity on big-endian ones.
         */
        static constexpr std::uint16_t htons_u16(std::uint16_t v) noexcept {
            if constexpr (std::endian::native == std::endian::little) {
                return static_cast<std::uint16_t>(
                    ((v & 0x00FFu) << 8) | ((v & 0xFF00u) >> 8));
            } else {
                return v;
            }
        }

        static constexpr std::uint16_t ntohs_u16(std::uint16_t v) noexcept {
            return htons_u16(v);
        }
    };

}

#endif // SOCCER_ICMP_ECHO_H
