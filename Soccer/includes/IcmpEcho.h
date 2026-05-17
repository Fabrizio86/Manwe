//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef SOCCER_ICMP_ECHO_H
#define SOCCER_ICMP_ECHO_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

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
     *        @c RawSocket opened with @c IPPROTO_ICMP.
     */
    class IcmpEcho {
    public:
        /**
         * @brief Compute the 16-bit RFC 1071 internet checksum over @p data.
         *        The result is in host byte order; place it into a packet
         *        as-is (the checksum field is computed before byte-swap).
         */
        static std::uint16_t checksum(std::span<const std::byte> data) noexcept {
            std::uint32_t sum = 0;
            const auto *p = reinterpret_cast<const std::uint8_t *>(data.data());
            std::size_t len = data.size();
            while (len >= 2) {
                sum += (static_cast<std::uint16_t>(p[0]) << 8) | p[1];
                p += 2;
                len -= 2;
            }
            if (len == 1) sum += static_cast<std::uint16_t>(p[0]) << 8;
            while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
            return static_cast<std::uint16_t>(~sum & 0xFFFF);
        }

        /**
         * @brief Build an ICMP Echo Request packet (header + payload).
         *
         * @param identifier 16-bit echo identifier (commonly the PID).
         * @param sequence   16-bit echo sequence number (caller-incremented).
         * @param payload    Optional payload bytes (RFC 792 allows any).
         * @return A heap-allocated byte vector ready for @c sendto.
         */
        static std::vector<std::byte> buildRequest(std::uint16_t identifier,
                                                    std::uint16_t sequence,
                                                    std::span<const std::byte> payload = {}) {
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
         * @return std::nullopt if the packet is too short to be valid.
         */
        static std::optional<ParsedReply> parse(std::span<const std::byte> data,
                                                bool skip_ip = false) {
            if (skip_ip) {
                if (data.empty()) return std::nullopt;
                const auto first = static_cast<std::uint8_t>(data[0]);
                const std::size_t ihl = (first & 0x0F) * 4;
                if (ihl < 20 || data.size() < ihl) return std::nullopt;
                data = data.subspan(ihl);
            }
            if (data.size() < sizeof(IcmpEchoHeader)) return std::nullopt;

            const auto *hdr = reinterpret_cast<const IcmpEchoHeader *>(data.data());
            ParsedReply out;
            out.type = static_cast<IcmpType>(hdr->type);
            out.code = hdr->code;
            out.identifier = ntohs_u16(hdr->identifier);
            out.sequence = ntohs_u16(hdr->sequence);
            out.payload = data.subspan(sizeof(IcmpEchoHeader));

            // Re-verify checksum by zeroing it and recomputing.
            std::vector<std::byte> tmp(data.begin(), data.end());
            auto *tmp_hdr = reinterpret_cast<IcmpEchoHeader *>(tmp.data());
            const std::uint16_t wire = ntohs_u16(tmp_hdr->checksum);
            tmp_hdr->checksum = 0;
            const std::uint16_t computed = checksum(std::span<const std::byte>(tmp));
            out.checksum_valid = (wire == computed);
            return out;
        }

    private:
        /**
         * @brief Host -> network byte order for a 16-bit value, without
         *        pulling in @c <arpa/inet.h> from this header.
         */
        static constexpr std::uint16_t htons_u16(std::uint16_t v) noexcept {
            return static_cast<std::uint16_t>(
                ((v & 0x00FFu) << 8) | ((v & 0xFF00u) >> 8));
        }

        static constexpr std::uint16_t ntohs_u16(std::uint16_t v) noexcept {
            return htons_u16(v);
        }
    };

}

#endif // SOCCER_ICMP_ECHO_H
