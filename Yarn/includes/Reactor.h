//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef YARN_REACTOR_H
#define YARN_REACTOR_H

#include <atomic>
#include <chrono>
#include <coroutine>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "SocketHandle.h"

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

    namespace detail {

        /**
         * @brief Coroutine handles waiting on one fd in one direction, in
         *        arrival order. The first handle is stored inline so the
         *        common single-waiter case never allocates.
         */
        struct DirWaiters {
            void *first = nullptr;
            std::vector<void *> rest;

            bool empty() const noexcept { return this->first == nullptr; }

            void push(void *h) {
                if (!this->first) this->first = h; else this->rest.push_back(h);
            }

            /// Move every handle into @p out and leave this empty.
            void takeAll(std::vector<void *> &out) {
                if (!this->first) return;
                out.push_back(this->first);
                for (void *p : this->rest) out.push_back(p);
                this->first = nullptr;
                this->rest.clear();
            }

            /// Remove one handle; @c true if it was present.
            bool remove(void *h) noexcept {
                if (this->first == h) {
                    if (this->rest.empty()) {
                        this->first = nullptr;
                    } else {
                        this->first = this->rest.front();
                        this->rest.erase(this->rest.begin());
                    }
                    return true;
                }
                for (auto it = this->rest.begin(); it != this->rest.end(); ++it) {
                    if (*it == h) {
                        this->rest.erase(it);
                        return true;
                    }
                }
                return false;
            }
        };

        /**
         * @brief What is waiting on one fd. Any number of coroutines may wait
         *        in each direction; a readiness event wakes all of that
         *        direction's waiters (the loser of a race sees EAGAIN and
         *        waits again).
         */
        struct FdWaiters {
            DirWaiters readers;
            DirWaiters writers;

            bool empty() const noexcept { return this->readers.empty() && this->writers.empty(); }
        };

    } // namespace detail

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
        void registerReadable(SocketHandle fd, std::coroutine_handle<> h) noexcept;

        /**
         * @brief Register interest in @c fd becoming writable. See
         *        @ref registerReadable for semantics.
         */
        void registerWritable(SocketHandle fd, std::coroutine_handle<> h) noexcept;

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
         * @brief Take @p h out of a still-pending readable/writable wait on
         *        @p fd. With @p resume the coroutine is then resumed on the
         *        Yarn pool as if the fd had become ready (it re-checks the
         *        fd itself); without it the handle is simply forgotten, for
         *        a frame that is being destroyed.
         *
         * Ownership contract (kqueue, epoll, Windows; exact): exactly one
         * party resumes a waiter. This call returns @c true iff it took the
         * registration out before readiness claimed it, and then it alone
         * decides the handle's fate (resumed now, or never resumed by the
         * Reactor). It returns @c false when there is no such wait or the
         * readiness event already claimed @p h: that resume is in flight or
         * done and cannot be stopped from here.
         *
         * io_uring: completion is asynchronous, so the kernel is only asked
         * to cancel the poll and @c true means "requested". With @p resume
         * the handle is still resumed exactly once (by the cancellation or
         * by the readiness that beat it). With @c resume = @c false the
         * Reactor cannot tell whether that resume already happened, so a
         * caller that must know whether @p h may still be resumed has to use
         * @c resume = @c true and hand in a handle that tolerates the
         * resume (Yarn's own awaiters register a detail::WaitProxy and never
         * pass @c resume = @c false).
         *
         * @return See above.
         */
        bool cancelWait(SocketHandle fd, std::coroutine_handle<> h, bool resume = true) noexcept;

        /**
         * @brief Same as @ref cancelWait for a timer armed by
         *        @ref registerTimer: a pending timer is removed and @p h is
         *        resumed early (or forgotten when @p resume is false). The
         *        same ownership contract applies: exact on kqueue, epoll and
         *        Windows, "requested" on io_uring.
         */
        bool cancelTimer(std::coroutine_handle<> h, bool resume = true) noexcept;

        /**
         * @brief Deregister @p fd: every coroutine parked on it, in either
         *        direction, is removed and resumed on the Yarn pool, where
         *        its next syscall reports the closed / bad descriptor.
         *        Call it from the code that owns the fd, just before
         *        @c close(fd): the kernel drops a closed fd from its
         *        interest set silently, so without this a waiter would stay
         *        parked forever (and could be woken by an unrelated fd that
         *        later reuses the number).
         *
         * @return Number of waiters resumed (a lower bound on io_uring).
         *
         * Registering a wait on an fd number that still has waiters whose
         * descriptor was closed without this call is detected on kqueue (the
         * knote is gone): those stale waiters are resumed first, so a reused
         * number never inherits or strands them.
         */
        std::size_t cancelFd(SocketHandle fd) noexcept;

        /**
         * @brief Cooperative stop. Wakes the event loop thread, which
         *        exits once it observes @c running == false. Idempotent.
         */
        void stop();

#if defined(_WIN32)
        /**
         * @brief Associate @p fd (a Windows @c SOCKET) with the Reactor's
         *        IOCP. Required before issuing any
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
        bool associateIocp(SocketHandle fd) noexcept;

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
         * @brief Stop and join the loop thread at process exit (the Reactor
         *        object itself is never destroyed).
         */
        void retire() noexcept;

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

        /**
         * @brief Waiters per fd. kqueue keeps one knote per (fd, filter), so
         *        a second waiter in the same direction would overwrite the
         *        first one's udata; the handles live here instead and the
         *        knote only signals readiness. Guarded by @ref fdMu.
         */
        std::unordered_map<int, detail::FdWaiters> fdWaiters;
        std::mutex fdMu;

        void registerFd(int fd, std::coroutine_handle<> h, bool writable) noexcept;
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

        /**
         * @brief Handles whose pending poll/timeout was cancelled with
         *        resume == false; the completion of such an operation is
         *        swallowed instead of resuming the (destroyed) coroutine.
         *        Guarded by @ref sqMu.
         */
        std::vector<void *> droppedHandles;

        /**
         * @brief Fast-path guard for @ref droppedHandles so the completion
         *        loop only takes @ref sqMu when a drop is outstanding.
         */
        std::atomic<int> droppedPending{0};

        /**
         * @brief Wake the loop so it submits SQEs left queued by a failed
         *        io_uring_submit.
         */
        void pokeLoop() noexcept;

        /**
         * @brief Ask the kernel to cancel the poll (or timeout when
         *        @p timer) whose user_data is @p key.
         */
        bool cancelByHandle(void *key, bool resume, bool timer) noexcept;
#elif defined(__linux__)
        /**
         * @brief epoll descriptor.
         */
        int epfd = -1;

        /**
         * @brief eventfd used to wake the event loop on @ref stop.
         */
        int wakefd = -1;

        /**
         * @brief Heap-allocated @c TimerEntry pointers (opaque here;
         *        the concrete type lives in Reactor.cpp) created by
         *        @ref registerTimer that have not yet fired or been
         *        freed. The run loop removes an entry (and deletes it)
         *        when its timer fires; @c ~Reactor reclaims any left
         *        over when the loop exits with timers still pending,
         *        so a Reactor destroyed mid-flight does not leak them.
         *        Guarded by @ref timerSetMu.
         */
        std::vector<void *> pendingTimers;

        /**
         * @brief Guards @ref pendingTimers against concurrent
         *        registerTimer callers and the run-loop thread.
         */
        std::mutex timerSetMu;

        /**
         * @brief Fallback for when a timerfd cannot be created (fd exhaustion with many concurrent sleeps): deadline-ordered
         *        handles serviced by the run loop through the epoll_wait timeout. Guarded by @ref timerSetMu.
         */
        std::multimap<std::chrono::steady_clock::time_point, void *> softTimers;

        /**
         * @brief Waiters per fd. Guarded by @ref fdMu.
         */
        std::unordered_map<int, detail::FdWaiters> fdWaiters;
        std::mutex fdMu;

        /**
         * @brief Timer entries removed by @ref cancelTimer whose memory is
         *        released by the run loop once the current event batch (which
         *        may still name them) has been processed. Guarded by
         *        @ref timerSetMu.
         */
        std::vector<void *> deadTimers;

        /**
         * @brief (Re)arm @p fd for the directions in @p w, one-shot. Returns false when epoll refuses the fd. fdMu held.
         */
        bool armFdLocked(int fd, const detail::FdWaiters &w) noexcept;

        /**
         * @brief Add a waiter in one direction and arm the union. Falls back to scheduling @p h at once when epoll
         *        refuses the fd (regular files, bad fds), as before.
         */
        void registerFd(int fd, std::coroutine_handle<> h, bool writable) noexcept;
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
