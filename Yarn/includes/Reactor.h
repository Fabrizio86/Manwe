//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef YARN_REACTOR_H
#define YARN_REACTOR_H

#include <atomic>
#include <chrono>
#include <coroutine>
#include <mutex>
#include <thread>

#if defined(__linux__) && defined(YARN_USE_IO_URING)
// liburing's struct, forward-declared so we don't pull <liburing.h> into
// every translation unit that includes this header. The full type lives
// in Reactor.cpp where the backend is implemented.
struct io_uring;
#endif

namespace YarnBall {

#if defined(_WIN32)
    /**
     * @brief Opaque Windows-only backend state. Holds the IOCP handle, the
     *        WSAPoll readiness thread's bookkeeping, the wakeup socket, the
     *        timer min-heap, and the registered (socket, mask, handle) set.
     *        Defined in Reactor.cpp so this header does not pull in
     *        @c <windows.h> or @c <winsock2.h>.
     */
    struct WindowsState;
#endif

    /**
     * @class Reactor
     * @brief Single-thread event loop that suspends coroutines on I/O
     *        readiness and resumes them on the Yarn pool.
     *
     * Backend selection:
     *  - macOS / *BSD: kqueue, with EVFILT_USER for wakeup/stop.
     *  - Linux (default): epoll, with eventfd for wakeup/stop.
     *  - Linux (when YARN_USE_IO_URING is defined): io_uring via liburing,
     *    using IORING_OP_POLL_ADD for readiness. Optional SQPOLL via the
     *    YARN_IO_URING_SQPOLL define for syscall-free submission.
     *  - Windows: dual surface. A WSAPoll-driven readiness thread services
     *    registerReadable / registerWritable (matching the
     *    epoll/kqueue/io_uring contract); a separate IOCP completion
     *    thread services proactor-style overlapped operations issued
     *    through @ref associateIocp + Soccer's overlapped API. Sockets
     *    can use either surface or both; the readiness thread does not
     *    look at IOCP-associated sockets unless they are also explicitly
     *    registered for readiness.
     *
     * Lifetime: singleton via @ref instance. Construction order is forced
     * so Yarn::instance() is built first; Yarn outlives the Reactor in
     * static dtor reverse order, which is what makes shutdown safe.
     *
     * Thread-safety: registerReadable / registerWritable are callable
     * from any thread, including the reactor's own loop. The io_uring
     * backend serialises SQE submissions internally; the kqueue / epoll
     * backends rely on the kernel's atomic registration semantics.
     */
    class Reactor final {
    public:
        Reactor(const Reactor &) = delete;
        Reactor(Reactor &&) = delete;
        Reactor &operator=(const Reactor &) = delete;
        Reactor &operator=(Reactor &&) = delete;

        /**
         * @brief Singleton accessor. First call constructs Yarn then
         *        Reactor; subsequent static destruction unwinds in
         *        reverse, so the reactor's shutdown can safely keep
         *        scheduling onto Yarn until its loop thread is joined.
         */
        static Reactor *instance();

        /**
         * @brief Register interest in @c fd becoming readable. On readiness,
         *        the reactor schedules a resumption of @c h onto Yarn.
         *
         * Backend behaviour:
         *  - kqueue / epoll / io_uring: one-shot registration via
         *    EV_ONESHOT / EPOLLONESHOT / IORING_OP_POLL_ADD (also one-shot
         *    by default). Re-await to re-register.
         *  - On registration failure the awaiter is scheduled immediately
         *    so the coroutine does not leak; the subsequent syscall will
         *    surface the error.
         *  - Windows: queues the registration onto the WSAPoll thread,
         *    which wakes via the loopback wakeup socket and rebuilds its
         *    poll set. One-shot semantics (re-await to re-arm).
         */
        void registerReadable(int fd, std::coroutine_handle<> h) noexcept;

        /**
         * @brief Register interest in @c fd becoming writable. See
         *        @ref registerReadable for semantics.
         */
        void registerWritable(int fd, std::coroutine_handle<> h) noexcept;

        /**
         * @brief Schedule @p h to be resumed after @p duration elapses.
         *
         * Backend specifics:
         *  - kqueue: EVFILT_TIMER, one-shot, with handle as both ident
         *    and udata. Sub-millisecond precision via NOTE_USECONDS.
         *  - epoll: a per-call @c timerfd_create, registered as
         *    EPOLLONESHOT; the fd is closed automatically after the
         *    timer fires.
         *  - io_uring: IORING_OP_TIMEOUT with the handle as user_data.
         *  - Windows: pushed onto a min-heap keyed by @c steady_clock
         *    deadline; the WSAPoll thread caps its wait time on the
         *    next deadline and dispatches expired entries.
         *
         * @note Sub-millisecond durations may be coarsened by the kernel
         *       depending on the backend; treat the resume time as a
         *       lower bound, not a hard real-time guarantee.
         */
        void registerTimer(std::chrono::nanoseconds duration,
                            std::coroutine_handle<> h) noexcept;

        /**
         * @brief Cooperative stop. Wakes the event loop thread, which
         *        exits once it observes @c running == false. Idempotent.
         */
        void stop();

#if defined(_WIN32)
        /**
         * @brief Associate @p fd (a Windows @c SOCKET held in an @c int)
         *        with the Reactor's IOCP. Required before issuing any
         *        proactor-style overlapped operation through Soccer.
         *
         * Idempotent: re-associating an already-associated socket is a
         * no-op (ERROR_INVALID_PARAMETER from the second call is
         * treated as success).
         *
         * Windows-only. Not declared on POSIX builds because the readiness
         * model there does not have an IOCP analogue.
         *
         * @return @c true on success.
         */
        bool associateIocp(int fd) noexcept;

        /**
         * @brief Raw IOCP HANDLE, exposed for code in the same TU as
         *        Soccer's overlapped awaiters that needs to issue
         *        operations on it (e.g. for @c GetAcceptExSockaddrs flows).
         *        Returned as @c void* so this header does not pull in
         *        @c <windows.h>.
         */
        void *iocpHandle() const noexcept;
#endif

    private:
        Reactor();
        ~Reactor();

        /**
         * @brief Event loop body; runs on @ref thread.
         */
        void run();

        /**
         * @brief Submit @p h for resumption on a Yarn worker.
         */
        void schedule(std::coroutine_handle<> h) noexcept;

        /**
         * @brief Cooperative-stop flag.
         */
        std::atomic<bool> running{true};

        /**
         * @brief Owning thread that runs @ref run.
         */
        std::thread thread;

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        /**
         * @brief kqueue descriptor.
         */
        int kq = -1;
#elif defined(__linux__) && defined(YARN_USE_IO_URING)
        /**
         * @brief liburing ring handle. Allocated in the constructor;
         *        freed in the destructor via io_uring_queue_exit.
         */
        ::io_uring *ring = nullptr;

        /**
         * @brief eventfd used to wake the loop on @ref stop. Registered
         *        via IORING_OP_POLL_ADD with a sentinel user_data so the
         *        loop can distinguish it from coroutine completions.
         */
        int wakefd = -1;

        /**
         * @brief Serialises SQE submission. io_uring's submission queue
         *        is single-producer by design; we lock briefly per
         *        register_* call.
         */
        std::mutex sqMu;
#elif defined(__linux__)
        /**
         * @brief epoll descriptor.
         */
        int epfd = -1;

        /**
         * @brief eventfd used to wake the event loop on @ref stop.
         */
        int wakefd = -1;
#elif defined(_WIN32)
        /**
         * @brief Opaque pointer to the Windows backend state (IOCP handle,
         *        WSAPoll thread, wakeup socket, timer heap, registration
         *        set). Defined in Reactor.cpp.
         */
        WindowsState *winstate = nullptr;
#endif
    };

}

#endif // YARN_REACTOR_H
