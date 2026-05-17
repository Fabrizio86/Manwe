//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "RawSocket.h"

#include "IoAwaiters.h"
#include "PlatformNet.h"
#include "SocketException.h"

namespace Soccer {

    RawSocket RawSocket::open(int family, int ip_protocol) {
        YarnBall::ensureWsaStarted();

        const int fd = static_cast<int>(::socket(family, SOCK_RAW, ip_protocol));
        if (fd < 0) {
            throw SocketException("socket(SOCK_RAW)", YarnBall::lastSocketError());
        }

        // Best-effort non-blocking; we ignore failures here because the
        // raw-socket recv path tolerates a blocking syscall on the worker
        // (an awaiter retry will arrive via waitReadable).
        (void) YarnBall::setSocketNonblocking(fd);
        return RawSocket(fd);
    }

    YarnBall::Task<std::size_t> RawSocket::recv(std::span<std::byte> buf,
                                                SocketAddress *sender) {
        if (this->sock < 0) {
            throw SocketException("recv on closed RawSocket");
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

    YarnBall::Task<std::size_t> RawSocket::sendTo(std::span<const std::byte> buf,
                                                   const SocketAddress &destination) {
        if (this->sock < 0) {
            throw SocketException("sendTo on closed RawSocket");
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

    void RawSocket::close() noexcept {
        if (this->sock >= 0) {
            (void) YarnBall::closeSocket(this->sock);
            this->sock = -1;
        }
    }

}
