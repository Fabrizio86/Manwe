//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef YARN_REACTOR_H
#define YARN_REACTOR_H

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include "SocketHandle.h"

#if defined(__linux__) && defined(YARN_USE_IO_URING)
// liburing's struct, forward-declared so we don't pull <liburing.h> into
// every translation unit that includes this header. The full type lives
// in Reactor.cpp where the backend is implemented.
struct io_uring;
struct io_uring_sqe;
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
     * @brief Thrown by Yarn's own fd-wait and sleep awaiters when the
     *        Reactor's event loop has died (see @ref Reactor::isDead): the
     *        wake they got was a "loop is gone" resume, not readiness or a
     *        timer.
     */
    class ReactorError : public std::runtime_error {
    public:
        ReactorError() : std::runtime_error("Reactor: event loop terminated") {}
    };

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
     * backends keep their per-fd waiter table under @c fdMu and issue the
     * kernel registration while holding it, so the run loop can only consume
     * a readiness event after the waiter is in the table.
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
         *  - kqueue: deadlines in an ordered table served by one
         *    EVFILT_TIMER knote (absolute, NOTE_NSECONDS) armed for the
         *    earliest; a cancel is an O(log n) table erase.
         *  - epoll: deadlines in an ordered table served by one shared
         *    @c timerfd armed for the earliest; a cancel is an O(log n)
         *    table erase.
         *  - io_uring: the same ordered table, served by one
         *    IORING_OP_TIMEOUT (absolute deadline) armed for the earliest.
         *  - Windows: pushed onto a min-heap keyed by @c steady_clock
         *    deadline; the WSAPoll thread caps its wait time on the
         *    next deadline and dispatches expired entries.
         *
         * @note Sub-millisecond durations may be coarsened by the kernel
         *       depending on the backend; treat the resume time as a
         *       lower bound, not a hard real-time guarantee.
         * @note If the event loop has died (@ref isDead), @p h is already
         *       registered, or the timer table cannot grow, @p h is scheduled
         *       at once instead of being queued forever or lost.
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
         * Ownership contract (kqueue, epoll; Windows with the exception below): exactly one
         * party resumes a waiter. This call returns @c true iff it took the
         * registration out before readiness claimed it, and then it alone
         * decides the handle's fate (resumed now, or never resumed by the
         * Reactor). It returns @c false when there is no such wait or the
         * readiness event already claimed @p h: that resume is in flight or
         * done and cannot be stopped from here.
         *
         * Windows: a registration the readiness thread is polling at that
         * moment is not in the pending list, so the request is queued for
         * that thread and this call returns @c false even though the
         * cancellation will be applied. With @c resume = @c true the handle is
         * still resumed exactly once (by the cancellation or by the readiness
         * that beat it). With @c resume = @c false it is resumed by the
         * readiness if that wins and otherwise never, and the caller cannot
         * tell which; as on io_uring, a frame being destroyed must make its
         * handle tolerate a late resume (Yarn's awaiters do, through a
         * detail::WaitProxy).
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
         *        same ownership contract applies: exact on kqueue, epoll,
         *        io_uring and Windows.
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
         * @brief Whether the event loop thread has exited on a fatal error
         *        (not a normal @ref stop). Everything parked was resumed when
         *        it died, and any later register* call resumes its waiter
         *        immediately; Yarn's own awaiters then throw
         *        @ref ReactorError rather than loop on a wait that can never
         *        complete.
         */
        bool isDead() const noexcept { return this->dead.load(std::memory_order_acquire); }

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

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__linux__)
        /**
         * @brief Called by the loop thread when it exits on a fatal error: logs it, sets @ref dead and resumes every
         *        parked waiter so it observes the failure instead of staying parked forever. Registrations that
         *        arrive afterwards are resumed at once.
         */
        void abandonWaiters(const char *what, int err) noexcept;
#endif

        /**
         * @brief Submit @p h for resumption on a Yarn worker.
         */
        void schedule(std::coroutine_handle<> h) noexcept;

        /**
         * @brief Cooperative-stop flag.
         */
        std::atomic<bool> running{true};

        /**
         * @brief Set (once) when the loop thread died on a fatal error. Checked by the register* calls under the same
         *        lock abandonWaiters sets it under, so a registration either lands before the sweep (and is woken by
         *        it) or sees the flag and is resumed at once.
         */
        std::atomic<bool> dead{false};

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

        /// Armed timers ordered by deadline: O(log n) insert, O(1) earliest. Guarded by @ref timerSetMu.
        using TimerMap = std::multimap<std::chrono::steady_clock::time_point, void *>;
        TimerMap timers;

        /**
         * @brief Handle address -> its @ref timers node, so @ref cancelTimer erases in O(log n) without a syscall. An
         *        address is in here exactly while its timer is armed; whoever erases it (fire, cancel, shutdown) owns
         *        the one resume. Guarded by @ref timerSetMu.
         */
        std::unordered_map<void *, TimerMap::iterator> timerIndex;

        /**
         * @brief Whether the one kernel timer (a single EVFILT_TIMER knote serves every sleep) is armed, and for which
         *        deadline. A cancel never re-arms (the extra expiry is harmless: the loop finds nothing due and
         *        re-arms for what is left), so this can be earlier than the first entry of @ref timers but never
         *        later. Guarded by @ref timerSetMu.
         */
        bool timerArmed = false;
        std::chrono::steady_clock::time_point timerArmedFor{};

        /// Guards @ref timers, @ref timerIndex and the arming state against registerTimer / cancelTimer and the loop.
        std::mutex timerSetMu;

        /// Arm the kernel timer for @p deadline (never in the past: an already-due deadline fires at once). Held lock.
        void armKernelTimerLocked(std::chrono::steady_clock::time_point deadline) noexcept;
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
         * @brief Armed fd polls: handle address -> fd, so @ref cancelFd can report how many waiters it cancelled (and
         *        none when there are none). Entries leave when the poll completes or is cancelled by fd. Guarded by
         *        @ref sqMu.
         */
        std::unordered_map<void *, int> pollFds;

        /// Armed timers ordered by deadline (same design as the epoll backend). Guarded by @ref sqMu.
        using TimerMap = std::multimap<std::chrono::steady_clock::time_point, void *>;
        TimerMap timers;

        /// Handle address -> its @ref timers node; present exactly while the timer is armed. Whoever erases it (fire,
        /// cancel, shutdown) owns the one resume. Guarded by @ref sqMu.
        std::unordered_map<void *, TimerMap::iterator> timerIndex;

        /**
         * @brief State of the single kernel IORING_OP_TIMEOUT serving @ref timers: whether one is armed, for which
         *        deadline, and its generation (carried in the operation's user_data so a stale completion of a
         *        replaced timeout is ignored). A cancel never touches the kernel timeout (the extra expiry is
         *        harmless), so @c timerArmedFor can be earlier than the first entry but never later. @c timerSpec is
         *        the absolute deadline the kernel reads; it stays valid here for SQPOLL. Guarded by @ref sqMu.
         */
        bool timerArmed = false;
        std::chrono::steady_clock::time_point timerArmedFor{};
        std::uint64_t timerGen = 0;
        std::int64_t timerSpec[2] = {0, 0}; ///< tv_sec, tv_nsec: layout of __kernel_timespec

        /// Arm (or re-arm for an earlier deadline) the kernel timeout for @p deadline. sqMu held. False when no SQE
        /// could be obtained; the loop is poked and re-evaluates the table.
        bool armKernelTimerLocked(std::chrono::steady_clock::time_point deadline) noexcept;

        /// Get an SQE, flushing the submission queue first if it is full. sqMu held. Null only if the ring stays full.
        io_uring_sqe *getSqeLocked() noexcept;

        /// Track @p h as parked on @p fd. False (nothing recorded) when out of memory. sqMu held.
        bool trackPollLocked(int fd, std::coroutine_handle<> h) noexcept;

        /**
         * @brief Wake the loop so it submits SQEs left queued by a failed
         *        io_uring_submit.
         */
        void pokeLoop() noexcept;

        /**
         * @brief Ask the kernel to cancel the poll whose user_data is @p key.
         */
        bool cancelByHandle(void *key, bool resume) noexcept;
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
         * @brief The single timerfd all sleeps share (CLOCK_MONOTONIC, level-triggered in the epoll set). It is armed
         *        for the earliest deadline in @ref timers, so any number of concurrent sleeps costs one fd.
         */
        int timerfd = -1;

        /// Armed timers ordered by deadline: O(log n) insert, O(1) earliest. Guarded by @ref timerSetMu.
        using TimerMap = std::multimap<std::chrono::steady_clock::time_point, void *>;
        TimerMap timers;

        /**
         * @brief Handle address -> its @ref timers node, so @ref cancelTimer erases in O(log n) without a scan. An
         *        address is in here exactly while its timer is armed; whoever erases it (fire, cancel, shutdown) owns
         *        the one resume. Guarded by @ref timerSetMu.
         */
        std::unordered_map<void *, TimerMap::iterator> timerIndex;

        /**
         * @brief Whether @ref timerfd is armed, and for which deadline. A cancel never re-arms (the extra expiry is
         *        harmless: the run loop finds nothing due and re-arms for what is left), so this can be earlier than
         *        the first entry of @ref timers but never later. Guarded by @ref timerSetMu.
         */
        bool timerArmed = false;
        std::chrono::steady_clock::time_point timerArmedFor{};

        /**
         * @brief Guards @ref timers, @ref timerIndex and the timerfd arming state against concurrent registerTimer /
         *        cancelTimer callers and the run-loop thread.
         */
        std::mutex timerSetMu;

        /**
         * @brief Arm @ref timerfd for @p deadline (never in the past: an already-due deadline fires at once).
         *        timerSetMu held.
         */
        void armTimerfdLocked(std::chrono::steady_clock::time_point deadline) noexcept;

        /**
         * @brief Waiters per fd. Guarded by @ref fdMu.
         */
        std::unordered_map<int, detail::FdWaiters> fdWaiters;
        std::mutex fdMu;

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
