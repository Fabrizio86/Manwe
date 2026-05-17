//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef YARN_IO_AWAITERS_H
#define YARN_IO_AWAITERS_H

#include <coroutine>

#include "Coroutines.h"
#include "Reactor.h"

#ifndef _WIN32
    #include <cerrno>
    #include <cstddef>
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace YarnBall::io {

    /**
     * @struct WaitReadableAwaiter
     * @brief Awaiter that suspends the current coroutine until the given fd
     *        becomes readable. Resumption lands on a Yarn worker.
     */
    struct WaitReadableAwaiter {
        int fd;

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> h) noexcept {
            Reactor::instance()->registerReadable(this->fd, h);
        }

        void await_resume() const noexcept {}
    };

    /**
     * @struct WaitWritableAwaiter
     * @brief Awaiter that suspends the current coroutine until the given fd
     *        becomes writable. Resumption lands on a Yarn worker.
     */
    struct WaitWritableAwaiter {
        int fd;

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> h) noexcept {
            Reactor::instance()->registerWritable(this->fd, h);
        }

        void await_resume() const noexcept {}
    };

    /**
     * @brief Convenience factory: @c co_await waitReadable(fd).
     */
    inline WaitReadableAwaiter waitReadable(int fd) noexcept { return {fd}; }

    /**
     * @brief Convenience factory: @c co_await waitWritable(fd).
     */
    inline WaitWritableAwaiter waitWritable(int fd) noexcept { return {fd}; }

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
        co_await waitReadable(fd);
        ssize_t n;
        do {
            n = ::read(fd, buf, len);
        } while (n < 0 && errno == EINTR);
        co_return n;
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
        while (total < len) {
            ssize_t n;
            do {
                n = ::write(fd, p + total, len - total);
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
