//
// Created by Fabrizio Paino on 2026-05-15.
//
// Windows-only proactor-style I/O surface. Issues overlapped WSARecv /
// WSASend through the Reactor's IOCP and resumes the awaiting coroutine
// when the kernel posts the completion. Use this for the lowest-overhead
// async path on Windows; the cross-platform readiness API on the same
// TcpStream / UdpSocket continues to work alongside it.
//
// Usage (Windows):
//
//   YarnBall::Reactor::instance()->associateIocp(stream.fd());
//   std::array<std::byte, 4096> buf;
//   std::size_t n = co_await Soccer::asyncRecvOverlapped(stream, buf);
//
// Issuing a readiness-style read (TcpStream::read) on the same socket
// after that is also fine; the Reactor's two surfaces are independent.
//

#ifndef SOCCER_OVERLAPPED_H
#define SOCCER_OVERLAPPED_H

#ifdef _WIN32

#include <coroutine>
#include <cstddef>
#include <span>

#include "Coroutines.h"
#include "PlatformNet.h"
#include "Reactor.h"
#include "SocketAddress.h"
#include "SocketException.h"
#include "TcpStream.h"
#include "UdpSocket.h"
#include "WindowsOverlapped.h"

namespace Soccer {

    namespace detail {
        /**
         * @brief Awaiter for a single WSARecv-driven read on a socket
         *        already associated with the Reactor's IOCP.
         *
         * Lifetime: the @ref OverlappedCompletion lives inside this awaiter
         * (a member, not a heap allocation) and is referenced by the kernel
         * until the IOCP completion thread dequeues it. The awaiter is a
         * member of the coroutine's frame, so the kernel-visible storage
         * survives until the coroutine resumes.
         *
         * Failure modes:
         *  - WSARecv that returns 0 immediately (success synchronously):
         *    the kernel still queues a completion to the IOCP, so we
         *    suspend exactly once and the IOCP path delivers @c bytes.
         *  - WSARecv that returns SOCKET_ERROR with WSA_IO_PENDING:
         *    the standard async case; we suspend and wait for the
         *    completion.
         *  - WSARecv with any other error: the awaiter records the error
         *    in @c oc.err and resumes inline (await_ready returns true).
         */
        struct OverlappedRecvAwaiter {
            int fd;
            WSABUF wsabuf{};
            YarnBall::OverlappedCompletion oc{};
            int sync_err = 0;
            bool sync_done = false;

            OverlappedRecvAwaiter(int sock, void *data, std::size_t len) noexcept
                : fd(sock) {
                this->wsabuf.buf = static_cast<CHAR *>(data);
                this->wsabuf.len = static_cast<ULONG>(len);
            }

            bool await_ready() noexcept { return false; }

            void await_suspend(std::coroutine_handle<> h) noexcept {
                this->oc.handle = h;
                DWORD bytes = 0;
                DWORD flags = 0;
                int rc = ::WSARecv(static_cast<SOCKET>(static_cast<unsigned>(this->fd)),
                                   &this->wsabuf, /*dwBufferCount=*/1,
                                   &bytes, &flags,
                                   static_cast<OVERLAPPED *>(&this->oc),
                                   /*lpCompletionRoutine=*/nullptr);
                if (rc == 0) {
                    // Success synchronous; the kernel still posts a
                    // completion (notifications are not skipped unless
                    // SetFileCompletionNotificationModes opted in), so we
                    // simply wait for the IOCP path to deliver @c bytes.
                    return;
                }
                const int err = ::WSAGetLastError();
                if (err == WSA_IO_PENDING) return; // expected async path
                // Hard failure pre-suspend: capture the error and resume
                // immediately on the calling worker.
                this->oc.err = err;
                this->sync_err = err;
                this->sync_done = true;
                h.resume();
            }

            std::size_t await_resume() {
                if (this->oc.err != 0) {
                    throw SocketException("WSARecv", this->oc.err);
                }
                return static_cast<std::size_t>(this->oc.bytes);
            }
        };

        /**
         * @brief Awaiter for a single WSASend on an IOCP-associated socket.
         *        Same shape as @ref OverlappedRecvAwaiter; @c bytes is the
         *        kernel-reported count which may be partial. Soccer's
         *        higher-level helper loops until the buffer is drained.
         */
        struct OverlappedSendAwaiter {
            int fd;
            WSABUF wsabuf{};
            YarnBall::OverlappedCompletion oc{};

            OverlappedSendAwaiter(int sock, const void *data, std::size_t len) noexcept
                : fd(sock) {
                // WSABUF::buf is non-const PCHAR even for sends; the
                // const_cast is the conventional WinSock idiom.
                this->wsabuf.buf = const_cast<CHAR *>(static_cast<const CHAR *>(data));
                this->wsabuf.len = static_cast<ULONG>(len);
            }

            bool await_ready() noexcept { return false; }

            void await_suspend(std::coroutine_handle<> h) noexcept {
                this->oc.handle = h;
                DWORD bytes = 0;
                int rc = ::WSASend(static_cast<SOCKET>(static_cast<unsigned>(this->fd)),
                                   &this->wsabuf, /*dwBufferCount=*/1,
                                   &bytes, /*flags=*/0,
                                   static_cast<OVERLAPPED *>(&this->oc),
                                   /*lpCompletionRoutine=*/nullptr);
                if (rc == 0) return;
                const int err = ::WSAGetLastError();
                if (err == WSA_IO_PENDING) return;
                this->oc.err = err;
                h.resume();
            }

            std::size_t await_resume() {
                if (this->oc.err != 0) {
                    throw SocketException("WSASend", this->oc.err);
                }
                return static_cast<std::size_t>(this->oc.bytes);
            }
        };
    }

    /**
     * @brief Issue a single overlapped recv on @p stream's socket via IOCP.
     *
     * @pre @p stream must already be associated with the Reactor's IOCP
     *      via @c Reactor::instance()->associateIocp(stream.fd()). Calling
     *      this without the association will return immediately with the
     *      kernel error @c ERROR_INVALID_HANDLE.
     *
     * @return Bytes received. 0 indicates the peer half-closed.
     */
    inline YarnBall::Task<std::size_t>
    asyncRecvOverlapped(TcpStream &stream, std::span<std::byte> buf) {
        detail::OverlappedRecvAwaiter aw{stream.fd(), buf.data(), buf.size()};
        std::size_t n = co_await aw;
        co_return n;
    }

    /**
     * @brief Issue a single overlapped send. May complete partially; the
     *        helper does NOT loop. Callers that want the buffer fully
     *        flushed should wrap this in a loop.
     */
    inline YarnBall::Task<std::size_t>
    asyncSendOverlapped(TcpStream &stream, std::span<const std::byte> buf) {
        detail::OverlappedSendAwaiter aw{stream.fd(), buf.data(), buf.size()};
        std::size_t n = co_await aw;
        co_return n;
    }

    /**
     * @brief Overlapped recv on a UDP socket.
     *
     * Sender address is not delivered; if you need it, use the readiness
     * path (UdpSocket::recvFrom) which goes through @c recvfrom and fills
     * the @c SocketAddress out-param. WSARecvFrom-based proactor support
     * can be added the same way as @ref asyncRecvOverlapped if needed.
     */
    inline YarnBall::Task<std::size_t>
    asyncRecvOverlapped(UdpSocket &sock, std::span<std::byte> buf) {
        detail::OverlappedRecvAwaiter aw{sock.fd(), buf.data(), buf.size()};
        std::size_t n = co_await aw;
        co_return n;
    }

    /**
     * @brief Overlapped send on a UDP socket.
     *
     * @pre @p sock must be connected to a peer (@ref UdpSocket::connect)
     *      before this call -- @c WSASend has no destination parameter,
     *      so the kernel relies on the socket's connected state to know
     *      where to deliver the datagram. Calling this on an unconnected
     *      UDP socket returns the kernel error @c WSAENOTCONN, which the
     *      awaiter surfaces as @ref SocketException.
     */
    inline YarnBall::Task<std::size_t>
    asyncSendOverlapped(UdpSocket &sock, std::span<const std::byte> buf) {
        detail::OverlappedSendAwaiter aw{sock.fd(), buf.data(), buf.size()};
        std::size_t n = co_await aw;
        co_return n;
    }

}

#endif // _WIN32

#endif // SOCCER_OVERLAPPED_H
