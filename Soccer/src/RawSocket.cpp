//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "RawSocket.h"

#include <algorithm>

#include "IoAwaiters.h"
#include "Reactor.h"
#include "NetUtil.h"
#include "PlatformNet.h"
#include "SocketException.h"

namespace Soccer {

    RawSocket RawSocket::open(int family, int ip_protocol) {
        YarnBall::ensureWsaStarted();

        const int fd = detail::openSocket(family, SOCK_RAW, ip_protocol);
        if (fd < 0) {
            throw SocketException("socket(SOCK_RAW)", YarnBall::lastSocketError());
        }

        // A blocking raw socket would stall a Yarn worker inside recv,
        // so a failure here is fatal.
        if (YarnBall::setSocketNonblocking(fd) != 0) {
            const int e = YarnBall::lastSocketError();
            (void) YarnBall::closeSocket(fd);
            throw SocketException("set_nonblocking", e);
        }
        return RawSocket(fd);
    }

    YarnBall::Task<std::size_t> RawSocket::recv(std::span<std::byte> buf,
                                                SocketAddress *sender) {
        if (this->sock < 0) {
            throw SocketException("recv on closed RawSocket");
        }
        while (true) {
            // Re-checked on every pass: close() wakes a parked receiver, which must then fail.
            if (this->sock < 0) {
                throw SocketException("recv on closed RawSocket");
            }
            sockaddr_storage from{};
            socklen_t fromlen = sizeof(from);

            ::ssize_t n;
            do {
                n = ::recvfrom(this->sock,
                               reinterpret_cast<char *>(buf.data()),
                               static_cast<int>(std::min<std::size_t>(buf.size(), 0x7fffffffu)), 0,
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

    YarnBall::Task<std::size_t> RawSocket::sendTo(std::span<const std::byte> buf,
                                                   const SocketAddress &destination) {
        if (this->sock < 0) {
            throw SocketException("sendTo on closed RawSocket");
        }
        while (true) {
            if (this->sock < 0) {
                throw SocketException("sendTo on closed RawSocket");
            }
            ::ssize_t n;
            do {
                n = ::sendto(this->sock,
                             reinterpret_cast<const char *>(buf.data()),
                             static_cast<int>(std::min<std::size_t>(buf.size(), 0x7fffffffu)), 0,
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

    void RawSocket::close() noexcept {
        const int fd = this->sock;
        if (fd >= 0) {
            // Closed first, so a woken recv / sendTo re-checks `sock` and fails
            // rather than using a descriptor number that is about to be reused.
            this->sock = -1;
#ifndef _WIN32
            // Wake any coroutine still parked on this fd before it is closed (and the number reused).
            (void) YarnBall::Reactor::instance()->cancelFd(fd);
#endif
            (void) YarnBall::closeSocket(fd);
        }
    }

}
