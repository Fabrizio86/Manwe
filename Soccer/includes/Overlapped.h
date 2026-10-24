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

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <span>

#include "Cancellation.h"
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
        /// Largest length handed to one WSARecv / WSASend (WSABUF::len is a ULONG; a longer buffer
        /// would be truncated modulo 2^32). The call transfers fewer bytes than asked, as a short
        /// read or write always may.
        inline constexpr std::size_t kOverlappedMaxChunk = 0x7fffffffu;

        /**
         * @brief Shared body of the overlapped awaiters: one operation issued on an
         *        IOCP-associated socket, resumed by the Reactor's completion thread.
         *
         * Lifetime. The OVERLAPPED record is heap-allocated and the awaiter
         * resumes through a @c WaitProxy, mirroring the POSIX fd awaiters:
         *  - a stop request (cancellation scope or @c Task::requestCancel) calls
         *    @c CancelIoEx; the operation then completes as aborted and the awaiter
         *    throws @c CancelledException;
         *  - an awaiter destroyed while the operation is still in flight cancels
         *    it, waits for the kernel to finish with the caller's buffer, and
         *    LEAKS the (small) record: the completion thread may still dequeue
         *    its packet and touch it, so freeing it would be a use-after-free.
         *    The proxy makes that late resume a no-op.
         */
        class OverlappedOp {
        public:
            OverlappedOp(const OverlappedOp &) = delete;
            OverlappedOp &operator=(const OverlappedOp &) = delete;

            ~OverlappedOp() {
                this->bind.disarm();
                if (this->inFlight) {
                    // Frame destroyed with the operation outstanding.
                    (void) this->proxy.kill();
                    (void) ::CancelIoEx(this->socketHandle(), this->oc);
                    DWORD ignored = 0;
                    DWORD flags = 0;
                    (void) ::WSAGetOverlappedResult(this->sock(), this->oc, &ignored, TRUE, &flags);
                    this->oc = nullptr; // leaked on purpose, see above
                }
                this->proxy.drop();
                delete this->oc;
            }

            bool await_ready() noexcept { return false; }

        protected:
            OverlappedOp(int fd, void *data, std::size_t len)
                : fd(fd), oc(new YarnBall::OverlappedCompletion{}) {
                this->wsabuf.buf = static_cast<CHAR *>(data);
                this->wsabuf.len = static_cast<ULONG>(std::min(len, kOverlappedMaxChunk));
            }

            YarnBall::OverlappedCompletion *ocPtr() const noexcept { return this->oc; }
            SOCKET sock() const noexcept { return static_cast<SOCKET>(static_cast<unsigned>(this->fd)); }
            HANDLE socketHandle() const noexcept { return reinterpret_cast<HANDLE>(this->sock()); }

            /// @p issue starts the WSA call and returns its result code (0 or SOCKET_ERROR).
            template<typename Promise, typename Issue>
            bool suspendAndIssue(std::coroutine_handle<Promise> h, Issue issue) {
                YarnBall::detail::TaskPromiseCore *core = YarnBall::detail::CancelBinding::ambient(h);
                std::shared_ptr<std::atomic<bool>> watch;
                if (core) {
                    // Armed before the call, so a stop that lands later finds an operation to cancel.
                    watch = this->bind.armWatched(core, &OverlappedOp::onCancel, this);
                    if (this->cancelled.load(std::memory_order_acquire)) {
                        this->bind.disarm();
                        return false; // stopped before anything was issued; await_resume throws
                    }
                }
                this->oc->handle = this->proxy.publish(h);
                this->started = true;
                this->inFlight = true; // set before the call: once it is issued `this` may be gone
                const HANDLE sockHandle = this->socketHandle();
                OVERLAPPED *const ov = this->oc;
                if (issue() != 0) {
                    const int err = ::WSAGetLastError();
                    if (err != WSA_IO_PENDING) {
                        // Hard failure before suspending: report it without suspending (resuming
                        // the handle from inside await_suspend would re-enter the coroutine).
                        this->inFlight = false;
                        this->bind.disarm();
                        this->proxy.reclaim();
                        this->oc->err = err; // nonzero: await_resume reports it
                        return false;
                    }
                }
                // Only locals from here on: the completion may already have resumed (and
                // destroyed) this awaiter. A stop that landed before the call was issued found
                // nothing to cancel: redo it.
                if (watch && watch->load(std::memory_order_acquire)) (void) ::CancelIoEx(sockHandle, ov);
                return true;
            }

            std::size_t finish(const char *what) {
                this->inFlight = false;
                this->bind.disarm();
                if (!this->started) throw YarnBall::CancelledException{}; // stopped before the call was issued
                const int err = this->oc->err;
                const std::size_t bytes = static_cast<std::size_t>(this->oc->bytes);
                if (this->cancelled.load(std::memory_order_acquire) &&
                    (err == ERROR_OPERATION_ABORTED || err == WSA_OPERATION_ABORTED)) {
                    throw YarnBall::CancelledException{};
                }
                if (err != 0) {
                    throw SocketException(what, err);
                }
                return bytes;
            }

            int fd;
            WSABUF wsabuf{};

        private:
            static void onCancel(void *p) noexcept {
                auto *self = static_cast<OverlappedOp *>(p);
                self->cancelled.store(true, std::memory_order_release);
                (void) ::CancelIoEx(self->socketHandle(), self->oc);
            }

            YarnBall::OverlappedCompletion *oc;
            bool started = false;  ///< the WSA call was issued
            bool inFlight = false; ///< issued and not yet resumed
            std::atomic<bool> cancelled{false};
            YarnBall::detail::WaitProxy proxy;
            YarnBall::detail::CancelBinding bind;
        };

        /**
         * @brief Awaiter for a single WSARecv-driven read on a socket
         *        already associated with the Reactor's IOCP.
         *
         * WSARecv returning 0 immediately still queues a completion to the
         * IOCP, so the awaiter suspends exactly once and the IOCP path delivers
         * the byte count; WSA_IO_PENDING is the ordinary asynchronous case; any
         * other error is thrown without suspending.
         */
        struct OverlappedRecvAwaiter : OverlappedOp {
            OverlappedRecvAwaiter(int sock, void *data, std::size_t len) 
                : OverlappedOp(sock, data, len) {
            }

            template<typename Promise>
            bool await_suspend(std::coroutine_handle<Promise> h) {
                return this->suspendAndIssue(h, [this]() {
                    DWORD bytes = 0;
                    DWORD flags = 0;
                    return ::WSARecv(this->sock(), &this->wsabuf, /*dwBufferCount=*/1, &bytes, &flags,
                                     static_cast<OVERLAPPED *>(this->ocPtr()),
                                     /*lpCompletionRoutine=*/nullptr);
                });
            }

            std::size_t await_resume() { return this->finish("WSARecv"); }
        };

        /**
         * @brief Awaiter for a single WSASend on an IOCP-associated socket.
         *        Same shape as @ref OverlappedRecvAwaiter; the result is the
         *        kernel-reported count which may be partial. Soccer's
         *        higher-level helper loops until the buffer is drained.
         */
        struct OverlappedSendAwaiter : OverlappedOp {
            OverlappedSendAwaiter(int sock, const void *data, std::size_t len) 
                : OverlappedOp(sock, const_cast<void *>(data), len) {
                // WSABUF::buf is non-const PCHAR even for sends; the
                // const_cast is the conventional WinSock idiom.
            }

            template<typename Promise>
            bool await_suspend(std::coroutine_handle<Promise> h) {
                return this->suspendAndIssue(h, [this]() {
                    DWORD bytes = 0;
                    return ::WSASend(this->sock(), &this->wsabuf, /*dwBufferCount=*/1, &bytes, /*flags=*/0,
                                     static_cast<OVERLAPPED *>(this->ocPtr()),
                                     /*lpCompletionRoutine=*/nullptr);
                });
            }

            std::size_t await_resume() { return this->finish("WSASend"); }
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
