//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef SOCCER_UDPSOCKET_H
#define SOCCER_UDPSOCKET_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "Coroutines.h"
#include "SocketAddress.h"

namespace Soccer {

    /**
     * @class UdpSocket
     * @brief Datagram socket with coroutine-driven send / recv.
     *
     * Each @c recvFrom / @c sendTo does exactly one syscall after the
     * kernel reports readiness. Datagram semantics: every receive returns
     * one whole datagram (truncated to the caller's buffer if the wire
     * datagram is larger).
     *
     * Move-only.
     */
    class UdpSocket final {
    public:
        UdpSocket() = default;

        /**
         * @brief Bind a UDP socket to @p host:@p port. Pass @c port == 0
         *        to let the kernel assign one; read it back via
         *        @c localAddress().
         *
         * @throws SocketException on resolution / socket / bind failure.
         */
        static UdpSocket bind(const std::string &host, std::uint16_t port);

        UdpSocket(const UdpSocket &) = delete;
        UdpSocket &operator=(const UdpSocket &) = delete;

        UdpSocket(UdpSocket &&other) noexcept : sock(std::exchange(other.sock, -1)) {
        }

        UdpSocket &operator=(UdpSocket &&other) noexcept {
            if (this != &other) {
                this->close();
                this->sock = std::exchange(other.sock, -1);
            }
            return *this;
        }

        ~UdpSocket() { this->close(); }

        /**
         * @brief Receive one datagram, suspending until readable. Writes
         *        the sender's address into @p sender.
         * @return Number of bytes received (may be 0 for a zero-length
         *         datagram).
         */
        YarnBall::Task<std::size_t> recvFrom(std::span<std::byte> buf, SocketAddress *sender);

        /**
         * @brief Send one datagram to @p destination, suspending on
         *        writability if the local send buffer is full.
         * @return Number of bytes sent.
         */
        YarnBall::Task<std::size_t> sendTo(std::span<const std::byte> buf,
                                            const SocketAddress &destination);

        /**
         * @brief Set the default destination for unaddressed sends.
         *
         * After @c connect, @c ::send / @c WSASend on this socket route to
         * @p peer without needing a per-call @c sockaddr. The kernel also
         * starts filtering inbound datagrams to those originating from
         * @p peer (matching the BSD-sockets contract).
         *
         * Required before issuing a Windows proactor-style
         * @c asyncSendOverlapped on a UDP socket: @c WSASend does not
         * accept a destination parameter, so the socket must be connected
         * first.
         *
         * @throws SocketException on the underlying @c ::connect failure.
         */
        void connect(const SocketAddress &peer);

        /**
         * @brief Join a multicast group on this socket. Subsequent
         *        @c recvFrom calls will receive datagrams addressed to
         *        @p groupAddress on top of unicast traffic to the bound
         *        port.
         *
         * IPv4 groups are addresses in @c 224.0.0.0/4 (e.g. @c "239.1.2.3").
         * IPv6 groups are addresses in @c ff00::/8 (e.g. @c "ff02::1").
         * Family is detected from the literal; no manual selection.
         *
         * Pass an explicit @p interfaceAddress (IPv4 address of the
         * NIC you want to receive on) to scope the join to one
         * interface — common for multi-NIC machines (Pi: eth0 vs wlan0)
         * and required on macOS for loopback delivery. The default
         * empty string asks the kernel to pick.
         *
         * @throws SocketException on parse / setsockopt failure.
         */
        void joinGroup(const std::string &groupAddress,
                        const std::string &interfaceAddress = "");

        /**
         * @brief Leave a previously-joined multicast group. Pass the
         *        same @p interfaceAddress you joined on, or empty if
         *        you joined with the kernel default.
         */
        void leaveGroup(const std::string &groupAddress,
                         const std::string &interfaceAddress = "");

        /**
         * @brief Time-to-live for outgoing multicast datagrams (IPv4) or
         *        hop limit (IPv6). Range 0-255. Default 1 (link-local
         *        only). 0 means the loopback only.
         */
        void setMulticastTtl(int ttl);

        /**
         * @brief Whether this socket should receive its own multicast
         *        sends on the local loopback. Default off; turn on for
         *        single-host testing.
         */
        void setMulticastLoop(bool enable);

        /**
         * @brief Choose the outgoing interface for multicast sends, by
         *        the interface's IPv4 address. Default: kernel routes
         *        via the default-route interface, which is usually
         *        wrong for embedded/multi-NIC machines.
         *
         * Pi/IoT use: pass the IP of the NIC the sensor network lives
         * on (e.g. @c "192.168.4.1" for an AP-mode wlan0).
         * macOS loopback testing: pass @c "127.0.0.1".
         *
         * IPv6 selection is by interface index rather than address and
         * is not exposed here; drop to @c fd() + @c IPV6_MULTICAST_IF
         * if you need it.
         */
        void setMulticastInterface(const std::string &ipv4InterfaceAddress);

        /**
         * @return The local bound address.
         */
        SocketAddress localAddress() const;

        int fd() const noexcept { return this->sock; }

        /**
         * @brief Close the socket. Idempotent.
         */
        void close() noexcept;

    private:
        explicit UdpSocket(int fd) noexcept : sock(fd) {
        }

        int sock = -1;
    };

}

#endif // SOCCER_UDPSOCKET_H
