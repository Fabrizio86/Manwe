//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "UdpSocket.h"

#include <algorithm>

#ifndef _WIN32
#include <net/if.h>
#include <sys/socket.h>
#include <sys/uio.h>
#endif

#include "IoAwaiters.h"
#include "Reactor.h"
#include "NetUtil.h"
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

    UdpSocket UdpSocket::bind(const std::string &host, std::uint16_t port, bool dualStack) {
        YarnBall::ensureWsaStarted();
        const auto addr = SocketAddress::resolve(host, port);

        const int fd = detail::openSocket(addr.family(), SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) throw SocketException("socket", YarnBall::lastSocketError());

        if (addr.family() == AF_INET6) {
            // Explicit, so "::" means the same on every OS (the defaults differ).
            const int v6only = dualStack ? 0 : 1;
            if (::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                             reinterpret_cast<const char *>(&v6only), sizeof(v6only)) < 0) {
                const int e = YarnBall::lastSocketError();
                (void) YarnBall::closeSocket(fd);
                throw SocketException("setsockopt(IPV6_V6ONLY)", e);
            }
        }

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
                                                    SocketAddress *sender,
                                                    bool *truncated) {
        if (truncated) *truncated = false;
        while (true) {
            // Re-checked on every pass: close() wakes a parked receiver, which must then fail.
            // The Use stays alive across the wait: the descriptor cannot be closed (and its
            // number reused) between the EAGAIN and the registration.
            auto use = this->sock.begin();
            const int fd = use.fd();
            sockaddr_storage from{};
            socklen_t fromlen = sizeof(from);
            bool cut = false;
            ::ssize_t n = -1;
            int err = 0;
            {
                if (fd < 0) {
                    throw SocketException("recvFrom on closed UdpSocket");
                }
#ifndef _WIN32
                // recvmsg, not recvfrom: only msg_flags reports MSG_TRUNC, i.e. that
                // the datagram was longer than the buffer and the tail was discarded.
                iovec iov{};
                iov.iov_base = buf.data();
                iov.iov_len = buf.size();
                msghdr msg{};
                msg.msg_name = &from;
                msg.msg_namelen = sizeof(from);
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;
                do {
                    msg.msg_namelen = sizeof(from);
                    n = ::recvmsg(fd, &msg, 0);
                } while (n < 0 && YarnBall::isEintr(YarnBall::lastSocketError()));
                if (n >= 0) {
                    fromlen = msg.msg_namelen;
                    cut = (msg.msg_flags & MSG_TRUNC) != 0;
                }
#else
                do {
                    fromlen = sizeof(from);
                    n = ::recvfrom(fd,
                                   reinterpret_cast<char *>(buf.data()),
                                   static_cast<int>(std::min<std::size_t>(buf.size(), 0x7fffffffu)), 0,
                                   reinterpret_cast<sockaddr *>(&from), &fromlen);
                } while (n < 0 && YarnBall::isEintr(YarnBall::lastSocketError()));
                // WinSock fails an oversized datagram with WSAEMSGSIZE but still fills the buffer.
                if (n < 0 && YarnBall::lastSocketError() == WSAEMSGSIZE) {
                    n = static_cast<::ssize_t>(std::min<std::size_t>(buf.size(), 0x7fffffffu));
                    cut = true;
                }
#endif
                if (n < 0) err = YarnBall::lastSocketError();
            }

            if (n >= 0) {
                if (sender) {
                    *sender = SocketAddress(reinterpret_cast<sockaddr *>(&from), fromlen);
                }
                if (truncated) *truncated = cut;
                co_return static_cast<std::size_t>(n);
            }
            if (YarnBall::isWouldBlock(err)) {
                co_await detail::waitReadable(use);
                continue;
            }
            throw SocketException("recvfrom", err);
        }
    }

    YarnBall::Task<std::size_t> UdpSocket::sendTo(std::span<const std::byte> buf,
                                                  const SocketAddress &destination) {
        while (true) {
            auto use = this->sock.begin(); // held across the wait, see recvFrom()
            const int fd = use.fd();
            ::ssize_t n = -1;
            int err = 0;
            {
                if (fd < 0) {
                    throw SocketException("sendTo on closed UdpSocket");
                }
                do {
                    n = ::sendto(fd,
                                 reinterpret_cast<const char *>(buf.data()),
                                 static_cast<int>(std::min<std::size_t>(buf.size(), 0x7fffffffu)), 0,
                                 destination.data(), destination.length());
                } while (n < 0 && YarnBall::isEintr(YarnBall::lastSocketError()));
                if (n < 0) err = YarnBall::lastSocketError();
            }

            if (n >= 0) co_return static_cast<std::size_t>(n);
            if (YarnBall::isWouldBlock(err)) {
                co_await detail::waitWritable(use);
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
                if (!interfaceAddress.empty()) {
                    // IPv6 picks an interface by index, so the name is resolved here.
#ifndef _WIN32
                    const unsigned idx = ::if_nametoindex(interfaceAddress.c_str());
                    if (idx == 0) {
                        throw SocketException(op + ": unknown interface: " + interfaceAddress);
                    }
                    req.ipv6mr_interface = idx;
#else
                    throw SocketException(op + ": an interface cannot be selected for an IPv6 group on Windows");
#endif
                }
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
        auto use = this->sock.begin();
        if (use.fd() < 0) {
            throw SocketException("joinGroup on closed UdpSocket");
        }
        multicastMembership(use.fd(), "IP_ADD_MEMBERSHIP",
                            IP_ADD_MEMBERSHIP, IPV6_JOIN_GROUP,
                            groupAddress, interfaceAddress);
    }

    void UdpSocket::leaveGroup(const std::string &groupAddress,
                                const std::string &interfaceAddress) {
        auto use = this->sock.begin();
        if (use.fd() < 0) {
            throw SocketException("leaveGroup on closed UdpSocket");
        }
        multicastMembership(use.fd(), "IP_DROP_MEMBERSHIP",
                            IP_DROP_MEMBERSHIP, IPV6_LEAVE_GROUP,
                            groupAddress, interfaceAddress);
    }

    namespace {
        /// Address family of the socket (AF_INET / AF_INET6), so a multicast option is
        /// set on the stack the socket actually uses.
        int socketFamily(int fd) {
            sockaddr_storage ss{};
            socklen_t len = sizeof(ss);
            if (::getsockname(fd, reinterpret_cast<sockaddr *>(&ss), &len) < 0) {
                throw SocketException("getsockname", YarnBall::lastSocketError());
            }
            return reinterpret_cast<const sockaddr *>(&ss)->sa_family;
        }
    }

    void UdpSocket::setMulticastTtl(int ttl) {
        if (ttl < 0 || ttl > 255) {
            throw SocketException("setMulticastTtl: ttl must be in 0..255");
        }
        auto use = this->sock.begin();
        const int fd = use.fd();
        if (fd < 0) {
            throw SocketException("setMulticastTtl on closed UdpSocket");
        }
        if (socketFamily(fd) == AF_INET6) {
            const int hops = ttl;
            if (::setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
                             reinterpret_cast<const char *>(&hops), sizeof(hops)) < 0) {
                throw SocketException("setsockopt IPV6_MULTICAST_HOPS", YarnBall::lastSocketError());
            }
            return;
        }
        // POSIX takes a byte here; WinSock wants a DWORD, and a one-byte value fails or reads garbage.
#ifdef _WIN32
        const DWORD v4ttl = static_cast<DWORD>(ttl);
#else
        const unsigned char v4ttl = static_cast<unsigned char>(ttl);
#endif
        if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL,
                         reinterpret_cast<const char *>(&v4ttl), sizeof(v4ttl)) < 0) {
            throw SocketException("setsockopt IP_MULTICAST_TTL", YarnBall::lastSocketError());
        }
    }

    void UdpSocket::setMulticastInterface(const std::string &ipv4InterfaceAddress) {
        auto use = this->sock.begin();
        if (use.fd() < 0) {
            throw SocketException("setMulticastInterface on closed UdpSocket");
        }
        in_addr addr{};
        if (::inet_pton(AF_INET, ipv4InterfaceAddress.c_str(), &addr) != 1) {
            throw SocketException("setMulticastInterface: not a valid IPv4 address: "
                                  + ipv4InterfaceAddress);
        }
        if (::setsockopt(use.fd(), IPPROTO_IP, IP_MULTICAST_IF,
                         reinterpret_cast<const char *>(&addr), sizeof(addr)) < 0) {
            throw SocketException("setsockopt IP_MULTICAST_IF",
                                  YarnBall::lastSocketError());
        }
    }

    void UdpSocket::setMulticastLoop(bool enable) {
        auto use = this->sock.begin();
        const int fd = use.fd();
        if (fd < 0) {
            throw SocketException("setMulticastLoop on closed UdpSocket");
        }
        if (socketFamily(fd) == AF_INET6) {
            const unsigned int v6on = enable ? 1u : 0u;
            if (::setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
                             reinterpret_cast<const char *>(&v6on), sizeof(v6on)) < 0) {
                throw SocketException("setsockopt IPV6_MULTICAST_LOOP", YarnBall::lastSocketError());
            }
            return;
        }
#ifdef _WIN32
        const DWORD v4on = enable ? 1 : 0; // WinSock wants a DWORD, POSIX a byte
#else
        const unsigned char v4on = enable ? 1 : 0;
#endif
        if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP,
                         reinterpret_cast<const char *>(&v4on), sizeof(v4on)) < 0) {
            throw SocketException("setsockopt IP_MULTICAST_LOOP", YarnBall::lastSocketError());
        }
    }

    void UdpSocket::connect(const SocketAddress &peer) {
        auto use = this->sock.begin();
        if (use.fd() < 0) {
            throw SocketException("connect on closed UdpSocket");
        }
        if (::connect(use.fd(), peer.data(), peer.length()) < 0) {
            throw SocketException("connect (UDP)", YarnBall::lastSocketError());
        }
    }

    SocketAddress UdpSocket::localAddress() const {
        SocketAddress addr;
        addr.lengthMut() = sizeof(sockaddr_storage);
        auto use = this->sock.begin();
        if (use.fd() < 0) {
            throw SocketException("localAddress on closed UdpSocket");
        }
        if (::getsockname(use.fd(), addr.dataMut(), &addr.lengthMut()) < 0) {
            throw SocketException("getsockname", YarnBall::lastSocketError());
        }
        return addr;
    }

    void UdpSocket::close() noexcept {
        // Taken out of the slot first, so a woken recvFrom / sendTo re-checks it and fails
        // rather than using a descriptor number that is about to be reused.
        const int fd = this->sock.take();
        if (fd >= 0) {
#ifndef _WIN32
            // Wake any coroutine still parked on this fd before it is closed (and the number reused).
            (void) YarnBall::Reactor::instance()->cancelFd(fd);
#endif
            this->sock.retire(fd);
        }
    }

}
