//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef YARN_IO_AWAITERS_H
#define YARN_IO_AWAITERS_H

#include <atomic>
#include <coroutine>
#include <stop_token>

#include "Cancellation.h"
#include "Coroutines.h"
#include "Reactor.h"

#ifndef _WIN32
    #include <cerrno>
    #include <cstddef>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace YarnBall::io {

    namespace impl {

        /**
         * @brief Shared implementation of the fd-readiness awaiters. Outside
         *        a cancellation scope it is a plain Reactor registration;
         *        inside one, a stop request takes the coroutine out of the
         *        Reactor and wakes it with @c CancelledException. A frame
         *        destroyed while parked is forgotten by the Reactor.
         */
        template<bool Writable>
        struct FdWaitAwaiter {
            SocketHandle fd;

            FdWaitAwaiter(const FdWaitAwaiter &) = delete;
            FdWaitAwaiter &operator=(const FdWaitAwaiter &) = delete;

            explicit FdWaitAwaiter(SocketHandle f) noexcept : fd(f) {}

            ~FdWaitAwaiter() {
                this->bind.disarm();
                // Frame destroyed while parked: the Reactor may or may not still hold the proxy. Marking it dead first
                // makes whichever resume reaches it (ours below, or one already in flight) a no-op.
                if (this->armed && !this->resumed && this->proxy.kill()) {
                    (void) Reactor::instance()->cancelWait(this->fd, this->handle);
                }
                this->proxy.drop();
            }

            bool await_ready() const noexcept { return false; }

            template<typename Promise>
            bool await_suspend(std::coroutine_handle<Promise> h) {
                YarnBall::detail::TaskPromiseCore *core = YarnBall::detail::CancelBinding::ambient(h);
                if (!core) {
                    // The proxy frame is allocated here; a failure surfaces at the co_await with nothing registered.
                    this->handle = this->proxy.publish(h);
                    this->armed = true;
                    this->registerWait(this->handle);
                    return true;
                }
                if (core->isCancellationRequested()) {
                    this->cancelled.store(true, std::memory_order_relaxed);
                    return false;
                }
                this->handle = this->proxy.publish(h);
                this->bind.arm(core, &FdWaitAwaiter::onCancel, this);
                if (this->cancelled.load(std::memory_order_relaxed)) {
                    // Stopped while arming: nothing is registered yet, so the proxy was never visible to anyone.
                    this->bind.disarm();
                    this->proxy.reclaim();
                    return false;
                }
                // Keeps the proxy (its gate, and its address) allocated after the registration, when the wait may already
                // have completed and destroyed this awaiter. Taken only now: the early return above reclaims the proxy.
                const auto pin = this->proxy.pin();
                const auto proxyHandle = this->handle;
                const SocketHandle waitFd = this->fd;
                this->armed = true;
                this->registerWait(proxyHandle);
                // A stop that landed before the registration found nothing to cancel: redo it now. Only locals from
                // here on. The redo happens only if the stop really came first, and the pin guarantees the address is
                // still this wait's: a later waiter on the same fd cannot have reused it and be woken early.
                if (pin.gate().exchange(kRegistered, std::memory_order_acq_rel) == kStopped) {
                    (void) Reactor::instance()->cancelWait(waitFd, proxyHandle);
                }
                return true;
            }

            void await_resume() {
                this->resumed = true;
                if (this->cancelled.load(std::memory_order_relaxed)) throw CancelledException{};
                // A dead event loop resumes waiters rather than strand them; without this a retry loop would spin.
                if (this->armed && Reactor::instance()->isDead()) throw ReactorError{};
            }

        private:
            void registerWait(std::coroutine_handle<> h) noexcept {
                if constexpr (Writable) {
                    Reactor::instance()->registerWritable(this->fd, h);
                } else {
                    Reactor::instance()->registerReadable(this->fd, h);
                }
            }

            static void onCancel(void *p) noexcept {
                auto *self = static_cast<FdWaitAwaiter *>(p);
                self->cancelled.store(true, std::memory_order_relaxed);
                // Not registered yet: leave the cancel to await_suspend, which sees kStopped once it has registered.
                if (self->proxy.gate().exchange(kStopped, std::memory_order_acq_rel) == kRegistered) {
                    (void) Reactor::instance()->cancelWait(self->fd, self->handle);
                }
            }

            static constexpr int kRegistered = 1, kStopped = 2; // proxy gate values (idle is 0)

            std::coroutine_handle<> handle{}; // the proxy, once published
            std::atomic<bool> cancelled{false};
            bool armed = false;
            bool resumed = false;
            YarnBall::detail::WaitProxy proxy;
            YarnBall::detail::CancelBinding bind;
        };

    } // namespace impl

    /**
     * @brief Awaiter that suspends the current coroutine until the given fd
     *        becomes readable. Resumption lands on a Yarn worker. Cancellable
     *        (see Cancellation.h).
     */
    using WaitReadableAwaiter = impl::FdWaitAwaiter<false>;

    /**
     * @brief Awaiter that suspends the current coroutine until the given fd
     *        becomes writable. Resumption lands on a Yarn worker. Cancellable
     *        (see Cancellation.h).
     */
    using WaitWritableAwaiter = impl::FdWaitAwaiter<true>;

    /**
     * @brief Convenience factory: @c co_await waitReadable(fd).
     */
    inline WaitReadableAwaiter waitReadable(SocketHandle fd) noexcept { return WaitReadableAwaiter{fd}; }

    /**
     * @brief Convenience factory: @c co_await waitWritable(fd).
     */
    inline WaitWritableAwaiter waitWritable(SocketHandle fd) noexcept { return WaitWritableAwaiter{fd}; }

#ifndef _WIN32

    /**
     * @brief Asynchronous read. Suspends until @p fd is readable, then issues
     *        a single @c ::read syscall (with @c EINTR retry).
     *
     * @return Number of bytes read (0 on EOF), or @c -1 on error (errno
     *         carries the failure cause from the worker that performed
     *         the syscall).
     */
    inline Task<ssize_t> asyncRead(int fd, void *buf, std::size_t len) {
        while (true) {
            co_await waitReadable(fd);
            ssize_t n;
            do {
                n = ::read(fd, buf, len);
            } while (n < 0 && errno == EINTR);
            // Spurious wake / another reader won the data: wait again.
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            co_return n;
        }
    }

    /**
     * @brief Asynchronous write. Loops until @p len bytes are written or the
     *        peer hangs up. Suspends on @c EAGAIN by waiting for writability.
     *
     * @return Number of bytes written, or @c -1 on error.
     */
    inline Task<ssize_t> asyncWrite(int fd, const void *buf, std::size_t len) {
        std::size_t total = 0;
        const auto *p = static_cast<const unsigned char *>(buf);
        // A peer that hung up must surface as EPIPE, not kill the process with SIGPIPE: send(MSG_NOSIGNAL) where it
        // exists, SO_NOSIGPIPE elsewhere. Non-sockets (pipes, files) fall back to plain write, which is covered by the
        // process-wide SIGPIPE ignore Yarn installs when its pool is created (a coroutine running here implies the pool
        // exists); a program that re-installed a SIGPIPE handler after that gets its own handler for pipe writes.
        bool useSend = true;
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
        {
            const int one = 1;
            (void) ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        }
#endif
        while (total < len) {
            ssize_t n;
            do {
#ifdef MSG_NOSIGNAL
                n = useSend ? ::send(fd, p + total, len - total, MSG_NOSIGNAL)
                            : ::write(fd, p + total, len - total);
                if (n < 0 && errno == ENOTSOCK) {
                    useSend = false;
                    n = ::write(fd, p + total, len - total);
                }
#else
                (void) useSend;
                n = ::write(fd, p + total, len - total);
#endif
            } while (n < 0 && errno == EINTR);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    co_await waitWritable(fd);
                    continue;
                }
                co_return -1;
            }
            if (n == 0) break;
            total += static_cast<std::size_t>(n);
        }
        co_return static_cast<ssize_t>(total);
    }

#endif // _WIN32

} // namespace YarnBall::io

#endif // YARN_IO_AWAITERS_H
