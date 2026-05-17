//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "UdpSocket.h"

#include "IoAwaiters.h"
#include "PlatformNet.h"
#include "SocketException.h"

namespace Soccer {

    namespace {
        void set_nonblocking(int fd) {
            if (YarnBall::setSocketNonblocking(fd) != 0) {
                throw SocketException("set_nonblocking", YarnBall::lastSocketError());
            }
        }
    }

    UdpSocket UdpSocket::bind(const std::string &host, std::uint16_t port) {
        YarnBall::ensureWsaStarted();
        const auto addr = SocketAddress::resolve(host, port);

        const int fd = static_cast<int>(::socket(addr.family(), SOCK_DGRAM, IPPROTO_UDP));
        if (fd < 0) throw SocketException("socket", YarnBall::lastSocketError());

        if (::bind(fd, addr.data(), addr.length()) < 0) {
            const int e = YarnBall::lastSocketError();
            (void) YarnBall::closeSocket(fd);
            throw SocketException("bind", e);
        }

        try {
            set_nonblocking(fd);
        } catch (...) {
            (void) YarnBall::closeSocket(fd);
            throw;
        }

        return UdpSocket(fd);
    }

    YarnBall::Task<std::size_t> UdpSocket::recvFrom(std::span<std::byte> buf,
                                                    SocketAddress *sender) {
        if (this->sock < 0) {
            throw SocketException("recvFrom on closed UdpSocket");
        }
        while (true) {
            sockaddr_storage from{};
            socklen_t fromlen = sizeof(from);

            ::ssize_t n;
            do {
                n = ::recvfrom(this->sock,
                               reinterpret_cast<char *>(buf.data()),
                               static_cast<int>(buf.size()), 0,
                               reinterpret_cast<sockaddr *>(&from), &fromlen);
            } while (n < 0 && YarnBall::isEintr(YarnBall::lastSocketError()));

            if (n >= 0) {
                if (sender) {
                    *sender = SocketAddress(reinterpret_cast<sockaddr *>(&from), fromlen);
                }
                co_return static_cast<std::size_t>(n);
            }
            const int err = YarnBall::lastSocketError();
            if (YarnBall::isWouldBlock(err)) {
                co_await YarnBall::io::waitReadable(this->sock);
                continue;
            }
            throw SocketException("recvfrom", err);
        }
    }

    YarnBall::Task<std::size_t> UdpSocket::sendTo(std::span<const std::byte> buf,
                                                  const SocketAddress &destination) {
        if (this->sock < 0) {
            throw SocketException("sendTo on closed UdpSocket");
        }
        while (true) {
            ::ssize_t n;
            do {
                n = ::sendto(this->sock,
                             reinterpret_cast<const char *>(buf.data()),
                             static_cast<int>(buf.size()), 0,
                             destination.data(), destination.length());
            } while (n < 0 && YarnBall::isEintr(YarnBall::lastSocketError()));

            if (n >= 0) co_return static_cast<std::size_t>(n);
            const int err = YarnBall::lastSocketError();
            if (YarnBall::isWouldBlock(err)) {
                co_await YarnBall::io::waitWritable(this->sock);
                continue;
            }
            throw SocketException("sendto", err);
        }
    }

    namespace {
        /**
         * @brief Auto-detect IPv4 vs IPv6 from an address literal and
         *        return the corresponding @c sockaddr family. Used by
         *        the multicast helpers to pick the right setsockopt
         *        level + struct without making the user specify it.
         *        Returns @c AF_UNSPEC if the string parses as neither.
         */
        int detectMulticastFamily(const std::string &addr,
                                   in_addr *out4,
                                   in6_addr *out6) {
            if (::inet_pton(AF_INET, addr.c_str(), out4) == 1) return AF_INET;
            if (::inet_pton(AF_INET6, addr.c_str(), out6) == 1) return AF_INET6;
            return AF_UNSPEC;
        }
    }

    namespace {
        /**
         * @brief Shared body for join / leave group: identical except
         *        for the @c IP_*_MEMBERSHIP opt level. Auto-detects
         *        the group family and, if @p interfaceAddress is
         *        non-empty, scopes the membership to that interface.
         */
        void multicastMembership(int sock,
                                  const std::string &op,
                                  int v4opt,
                                  int v6opt,
                                  const std::string &groupAddress,
                                  const std::string &interfaceAddress) {
            in_addr v4{};
            in6_addr v6{};
            const int family = detectMulticastFamily(groupAddress, &v4, &v6);
            if (family == AF_INET) {
                ip_mreq req{};
                req.imr_multiaddr = v4;
                if (interfaceAddress.empty()) {
                    req.imr_interface.s_addr = htonl(INADDR_ANY);
                } else {
                    in_addr iface{};
                    if (::inet_pton(AF_INET, interfaceAddress.c_str(), &iface) != 1) {
                        throw SocketException(op + ": invalid IPv4 interface: "
                                              + interfaceAddress);
                    }
                    req.imr_interface = iface;
                }
                if (::setsockopt(sock, IPPROTO_IP, v4opt,
                                 reinterpret_cast<const char *>(&req), sizeof(req)) < 0) {
                    throw SocketException("setsockopt " + op,
                                          YarnBall::lastSocketError());
                }
                return;
            }
            if (family == AF_INET6) {
                ipv6_mreq req{};
                req.ipv6mr_multiaddr = v6;
                req.ipv6mr_interface = 0; // index 0 = kernel default
                if (::setsockopt(sock, IPPROTO_IPV6, v6opt,
                                 reinterpret_cast<const char *>(&req), sizeof(req)) < 0) {
                    throw SocketException("setsockopt " + op,
                                          YarnBall::lastSocketError());
                }
                return;
            }
            throw SocketException(op + ": not a valid IPv4/IPv6 address: " + groupAddress);
        }
    }

    void UdpSocket::joinGroup(const std::string &groupAddress,
                               const std::string &interfaceAddress) {
        if (this->sock < 0) {
            throw SocketException("joinGroup on closed UdpSocket");
        }
        multicastMembership(this->sock, "IP_ADD_MEMBERSHIP",
                            IP_ADD_MEMBERSHIP, IPV6_JOIN_GROUP,
                            groupAddress, interfaceAddress);
    }

    void UdpSocket::leaveGroup(const std::string &groupAddress,
                                const std::string &interfaceAddress) {
        if (this->sock < 0) {
            throw SocketException("leaveGroup on closed UdpSocket");
        }
        multicastMembership(this->sock, "IP_DROP_MEMBERSHIP",
                            IP_DROP_MEMBERSHIP, IPV6_LEAVE_GROUP,
                            groupAddress, interfaceAddress);
    }

    void UdpSocket::setMulticastTtl(int ttl) {
        if (this->sock < 0) {
            throw SocketException("setMulticastTtl on closed UdpSocket");
        }
        // Same call applies to both stacks but with different option
        // names; we set both so the call works regardless of the bound
        // family. The wrong-family setsockopt is a no-op + EINVAL/ENOPROTOOPT
        // on most kernels; we tolerate that by checking only fatal failures.
        const unsigned char v4ttl = static_cast<unsigned char>(ttl);
        const int v6ttl = ttl;
        (void) ::setsockopt(this->sock, IPPROTO_IP, IP_MULTICAST_TTL,
                            reinterpret_cast<const char *>(&v4ttl), sizeof(v4ttl));
        (void) ::setsockopt(this->sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
                            reinterpret_cast<const char *>(&v6ttl), sizeof(v6ttl));
    }

    void UdpSocket::setMulticastInterface(const std::string &ipv4InterfaceAddress) {
        if (this->sock < 0) {
            throw SocketException("setMulticastInterface on closed UdpSocket");
        }
        in_addr addr{};
        if (::inet_pton(AF_INET, ipv4InterfaceAddress.c_str(), &addr) != 1) {
            throw SocketException("setMulticastInterface: not a valid IPv4 address: "
                                  + ipv4InterfaceAddress);
        }
        if (::setsockopt(this->sock, IPPROTO_IP, IP_MULTICAST_IF,
                         reinterpret_cast<const char *>(&addr), sizeof(addr)) < 0) {
            throw SocketException("setsockopt IP_MULTICAST_IF",
                                  YarnBall::lastSocketError());
        }
    }

    void UdpSocket::setMulticastLoop(bool enable) {
        if (this->sock < 0) {
            throw SocketException("setMulticastLoop on closed UdpSocket");
        }
        const unsigned char v4on = enable ? 1 : 0;
        const unsigned int v6on = enable ? 1u : 0u;
        (void) ::setsockopt(this->sock, IPPROTO_IP, IP_MULTICAST_LOOP,
                            reinterpret_cast<const char *>(&v4on), sizeof(v4on));
        (void) ::setsockopt(this->sock, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
                            reinterpret_cast<const char *>(&v6on), sizeof(v6on));
    }

    void UdpSocket::connect(const SocketAddress &peer) {
        if (this->sock < 0) {
            throw SocketException("connect on closed UdpSocket");
        }
        if (::connect(this->sock, peer.data(), peer.length()) < 0) {
            throw SocketException("connect (UDP)", YarnBall::lastSocketError());
        }
    }

    SocketAddress UdpSocket::localAddress() const {
        SocketAddress addr;
        addr.lengthMut() = sizeof(sockaddr_storage);
        if (::getsockname(this->sock, addr.dataMut(), &addr.lengthMut()) < 0) {
            throw SocketException("getsockname", YarnBall::lastSocketError());
        }
        return addr;
    }

    void UdpSocket::close() noexcept {
        if (this->sock >= 0) {
            (void) YarnBall::closeSocket(this->sock);
            this->sock = -1;
        }
    }

}
