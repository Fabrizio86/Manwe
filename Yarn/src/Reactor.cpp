//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "Reactor.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

#include "Coroutines.h"
#include "Yarn.hpp"

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    #include <sys/event.h>
    #include <sys/types.h>
    #include <unistd.h>
    #define YARN_REACTOR_KQUEUE 1
#elif defined(__linux__) && defined(YARN_USE_IO_URING)
    #include <liburing.h>
    #include <sys/eventfd.h>
    #include <unistd.h>
    #include <poll.h>
    #define YARN_REACTOR_IOURING 1
#elif defined(__linux__)
    #include <sys/epoll.h>
    #include <sys/eventfd.h>
    #include <sys/timerfd.h>
    #include <unistd.h>
    #define YARN_REACTOR_EPOLL 1
#elif defined(_WIN32)
    #include <algorithm>
    #include <queue>
    #include <unordered_map>
    #include <vector>

    #include "PlatformNet.h"
    #include "WindowsOverlapped.h"
    #define YARN_REACTOR_IOCP 1
#else
    #define YARN_REACTOR_STUB 1
#endif

namespace YarnBall {

    Reactor *Reactor::instance() {
        // Force Yarn to construct first so its static-dtor runs AFTER ours.
        // We keep scheduling onto Yarn until our thread is joined; Yarn must
        // be alive for that window.
        Yarn::instance();
        static Reactor inst;
        return &inst;
    }

    void Reactor::schedule(std::coroutine_handle<> h) noexcept {
        if (!h) return;
        try {
            // CoroutineITask never destroys the handle; the frame's owner
            // (either the awaited Task wrapper up the chain, or -- for
            // detached/coSpawn tasks -- the coroutine itself at final
            // suspend) handles cleanup.
            std::unique_ptr<ITask> ct{new detail::CoroutineITask(h)};
            Yarn::instance()->run(std::move(ct));
        } catch (...) {
            // Last-resort fallback: resume on the reactor thread itself.
            // Loses pool isolation but does not leak the coroutine.
            h.resume();
        }
    }


#if defined(YARN_REACTOR_KQUEUE)

    Reactor::Reactor() {
        this->kq = ::kqueue();
        if (this->kq < 0) {
            throw std::runtime_error("Reactor: kqueue() failed");
        }
        struct kevent change;
        EV_SET(&change, /*ident=*/0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        if (::kevent(this->kq, &change, 1, nullptr, 0, nullptr) < 0) {
            ::close(this->kq);
            throw std::runtime_error("Reactor: failed to register wakeup event");
        }
        this->thread = std::thread(&Reactor::run, this);
    }

    Reactor::~Reactor() {
        this->stop();
        if (this->thread.joinable()) this->thread.join();
        if (this->kq >= 0) ::close(this->kq);
    }

    void Reactor::stop() {
        this->running.store(false, std::memory_order_release);
        struct kevent change;
        EV_SET(&change, /*ident=*/0, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        ::kevent(this->kq, &change, 1, nullptr, 0, nullptr);
    }

    void Reactor::registerReadable(int fd, std::coroutine_handle<> h) noexcept {
        struct kevent change;
        EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, h.address());
        if (::kevent(this->kq, &change, 1, nullptr, 0, nullptr) < 0) {
            this->schedule(h);
        }
    }

    void Reactor::registerWritable(int fd, std::coroutine_handle<> h) noexcept {
        struct kevent change;
        EV_SET(&change, fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, h.address());
        if (::kevent(this->kq, &change, 1, nullptr, 0, nullptr) < 0) {
            this->schedule(h);
        }
    }

    void Reactor::registerTimer(std::chrono::nanoseconds duration,
                                 std::coroutine_handle<> h) noexcept {
        // ident must be unique per active timer; the handle's address is
        // perfect (it's also stored as udata so the run loop can recover
        // the coroutine to resume).
        const std::uintptr_t ident = reinterpret_cast<std::uintptr_t>(h.address());
        const std::int64_t ns =
            duration.count() > 0 ? duration.count() : 0;
        struct kevent change;
        // NOTE_NSECONDS lets us pass nanoseconds directly to the kernel.
        EV_SET(&change, ident, EVFILT_TIMER,
               EV_ADD | EV_ONESHOT, NOTE_NSECONDS, ns, h.address());
        if (::kevent(this->kq, &change, 1, nullptr, 0, nullptr) < 0) {
            this->schedule(h);
        }
    }

    void Reactor::run() {
        constexpr int kBatch = 64;
        struct kevent events[kBatch];

        while (this->running.load(std::memory_order_acquire)) {
            int n = ::kevent(this->kq, nullptr, 0, events, kBatch, nullptr);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < n; ++i) {
                if (events[i].filter == EVFILT_USER) continue;
                auto h = std::coroutine_handle<>::from_address(events[i].udata);
                this->schedule(h);
            }
        }
    }

#elif defined(YARN_REACTOR_IOURING)

    namespace {
        /**
         * @brief Sentinel user_data used for the wake eventfd's completion.
         *        Distinguishable from any coroutine_handle::address()
         *        because std::coroutine_handle is always allocated at
         *        addresses ≥ alignof(promise_type) ≥ 1.
         */
        inline constexpr std::uintptr_t kWakeUserData = 1;

        /**
         * @brief Submission queue depth used at io_uring init.
         */
        inline constexpr unsigned kIoUringEntries = 256;

        /**
         * @brief Milliseconds the kernel SQPOLL thread sits idle before
         *        parking. Tuned to be long enough that bursty workloads
         *        avoid round-trip wakes, short enough that idle systems
         *        don't burn unnecessary CPU.
         */
        inline constexpr unsigned kSqThreadIdleMs = 2000;
    }

    Reactor::Reactor() {
        this->ring = new ::io_uring{};
        struct io_uring_params params;
        std::memset(&params, 0, sizeof(params));
#  ifdef YARN_IO_URING_SQPOLL
        // SQPOLL: kernel polls the submission queue, so userspace doesn't
        // need an io_uring_enter syscall on every submission. The kernel
        // thread parks after sq_thread_idle ms of inactivity; liburing's
        // io_uring_submit wakes it via IORING_SQ_NEED_WAKEUP under the
        // hood. Older kernels (< 5.11) require CAP_SYS_NICE.
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = kSqThreadIdleMs;
#  endif
        int r = ::io_uring_queue_init_params(kIoUringEntries, this->ring, &params);
        if (r < 0) {
            delete this->ring;
            this->ring = nullptr;
            throw std::runtime_error("Reactor: io_uring_queue_init_params failed");
        }

        this->wakefd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (this->wakefd < 0) {
            ::io_uring_queue_exit(this->ring);
            delete this->ring;
            this->ring = nullptr;
            throw std::runtime_error("Reactor: eventfd failed");
        }

        // Arm the wake eventfd watch.
        {
            std::lock_guard<std::mutex> lk(this->sqMu);
            io_uring_sqe *sqe = ::io_uring_get_sqe(this->ring);
            if (!sqe) {
                ::close(this->wakefd);
                ::io_uring_queue_exit(this->ring);
                delete this->ring;
                this->ring = nullptr;
                throw std::runtime_error("Reactor: failed to obtain SQE for wake watch");
            }
            ::io_uring_prep_poll_add(sqe, this->wakefd, POLLIN);
            ::io_uring_sqe_set_data(sqe, reinterpret_cast<void *>(kWakeUserData));
            ::io_uring_submit(this->ring);
        }

        this->thread = std::thread(&Reactor::run, this);
    }

    Reactor::~Reactor() {
        this->stop();
        if (this->thread.joinable()) this->thread.join();
        if (this->wakefd >= 0) ::close(this->wakefd);
        if (this->ring) {
            ::io_uring_queue_exit(this->ring);
            delete this->ring;
            this->ring = nullptr;
        }
    }

    void Reactor::stop() {
        this->running.store(false, std::memory_order_release);
        std::uint64_t v = 1;
        (void) ::write(this->wakefd, &v, sizeof(v));
    }

    void Reactor::registerReadable(int fd, std::coroutine_handle<> h) noexcept {
        std::lock_guard<std::mutex> lk(this->sqMu);
        io_uring_sqe *sqe = ::io_uring_get_sqe(this->ring);
        if (!sqe) {
            // SQ full; fall back to immediate resume. The user-level read
            // syscall will surface the failure mode honestly.
            this->schedule(h);
            return;
        }
        ::io_uring_prep_poll_add(sqe, fd, POLLIN);
        ::io_uring_sqe_set_data(sqe, h.address());
        if (::io_uring_submit(this->ring) < 0) {
            this->schedule(h);
        }
    }

    void Reactor::registerWritable(int fd, std::coroutine_handle<> h) noexcept {
        std::lock_guard<std::mutex> lk(this->sqMu);
        io_uring_sqe *sqe = ::io_uring_get_sqe(this->ring);
        if (!sqe) {
            this->schedule(h);
            return;
        }
        ::io_uring_prep_poll_add(sqe, fd, POLLOUT);
        ::io_uring_sqe_set_data(sqe, h.address());
        if (::io_uring_submit(this->ring) < 0) {
            this->schedule(h);
        }
    }

    void Reactor::registerTimer(std::chrono::nanoseconds duration,
                                 std::coroutine_handle<> h) noexcept {
        std::lock_guard<std::mutex> lk(this->sqMu);
        io_uring_sqe *sqe = ::io_uring_get_sqe(this->ring);
        if (!sqe) {
            this->schedule(h);
            return;
        }
        // io_uring_prep_timeout expects a __kernel_timespec; pack the
        // duration accordingly. The third arg is the wait count (0 = absolute
        // timeout fires once). flags=0 -> relative.
        struct __kernel_timespec ts;
        ts.tv_sec = static_cast<long long>(duration.count() / 1'000'000'000LL);
        ts.tv_nsec = static_cast<long long>(duration.count() % 1'000'000'000LL);
        ::io_uring_prep_timeout(sqe, &ts, 0, 0);
        ::io_uring_sqe_set_data(sqe, h.address());
        if (::io_uring_submit(this->ring) < 0) {
            this->schedule(h);
        }
    }

    void Reactor::run() {
        while (this->running.load(std::memory_order_acquire)) {
            io_uring_cqe *cqe = nullptr;
            int r = ::io_uring_wait_cqe(this->ring, &cqe);
            if (r < 0) {
                if (-r == EINTR) continue;
                break;
            }
            if (!cqe) continue;

            void *data = ::io_uring_cqe_get_data(cqe);
            if (reinterpret_cast<std::uintptr_t>(data) == kWakeUserData) {
                // Wake event: drain the eventfd, re-arm the watch, loop
                // will re-check running and exit if needed.
                std::uint64_t buf;
                (void) ::read(this->wakefd, &buf, sizeof(buf));
                ::io_uring_cqe_seen(this->ring, cqe);

                if (this->running.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> lk(this->sqMu);
                    io_uring_sqe *sqe = ::io_uring_get_sqe(this->ring);
                    if (sqe) {
                        ::io_uring_prep_poll_add(sqe, this->wakefd, POLLIN);
                        ::io_uring_sqe_set_data(sqe, reinterpret_cast<void *>(kWakeUserData));
                        ::io_uring_submit(this->ring);
                    }
                }
                continue;
            }

            auto h = std::coroutine_handle<>::from_address(data);
            ::io_uring_cqe_seen(this->ring, cqe);
            this->schedule(h);
        }
    }

#elif defined(YARN_REACTOR_EPOLL)

    namespace {
        /**
         * @brief Sentinel udata used to distinguish the wakeup eventfd
         *        from coroutine fds in the epoll completion list.
         */
        inline constexpr std::uintptr_t kWakeupSentinel = 1;
    }

    Reactor::Reactor() {
        this->epfd = ::epoll_create1(EPOLL_CLOEXEC);
        if (this->epfd < 0) throw std::runtime_error("Reactor: epoll_create1 failed");
        this->wakefd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (this->wakefd < 0) {
            ::close(this->epfd);
            throw std::runtime_error("Reactor: eventfd failed");
        }
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = reinterpret_cast<void *>(kWakeupSentinel);
        if (::epoll_ctl(this->epfd, EPOLL_CTL_ADD, this->wakefd, &ev) < 0) {
            ::close(this->wakefd);
            ::close(this->epfd);
            throw std::runtime_error("Reactor: failed to register wakefd");
        }
        this->thread = std::thread(&Reactor::run, this);
    }

    Reactor::~Reactor() {
        this->stop();
        if (this->thread.joinable()) this->thread.join();
        if (this->wakefd >= 0) ::close(this->wakefd);
        if (this->epfd >= 0) ::close(this->epfd);
    }

    void Reactor::stop() {
        this->running.store(false, std::memory_order_release);
        std::uint64_t v = 1;
        (void) ::write(this->wakefd, &v, sizeof(v));
    }

    void Reactor::registerReadable(int fd, std::coroutine_handle<> h) noexcept {
        struct epoll_event ev{};
        ev.events = EPOLLIN | EPOLLONESHOT;
        ev.data.ptr = h.address();
        if (::epoll_ctl(this->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            if (errno == EEXIST) {
                if (::epoll_ctl(this->epfd, EPOLL_CTL_MOD, fd, &ev) == 0) return;
            }
            this->schedule(h);
        }
    }

    void Reactor::registerWritable(int fd, std::coroutine_handle<> h) noexcept {
        struct epoll_event ev{};
        ev.events = EPOLLOUT | EPOLLONESHOT;
        ev.data.ptr = h.address();
        if (::epoll_ctl(this->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            if (errno == EEXIST) {
                if (::epoll_ctl(this->epfd, EPOLL_CTL_MOD, fd, &ev) == 0) return;
            }
            this->schedule(h);
        }
    }

    /**
     * @brief Heap-allocated entry that owns the timerfd we created for a
     *        sleep. The run loop's epoll completion handler casts
     *        data.ptr back to this, closes the fd, schedules the handle,
     *        and frees the entry. Tagged so we can distinguish it from
     *        the wakeup sentinel and from coroutine handles directly.
     */
    namespace {
        struct TimerEntry {
            int fd;
            std::coroutine_handle<> handle;
        };

        /**
         * @brief Low-bit-tag in data.ptr to mark "this is a TimerEntry".
         *        coroutine_handle::address() is aligned at least 8 bytes,
         *        so the low 3 bits are free. We OR in 0x2 for timers
         *        (0x1 is the wakeup sentinel).
         */
        constexpr std::uintptr_t kTimerTag = 0x2;

        inline void *tagTimer(TimerEntry *e) {
            return reinterpret_cast<void *>(
                reinterpret_cast<std::uintptr_t>(e) | kTimerTag);
        }

        inline bool isTimer(void *p) {
            return (reinterpret_cast<std::uintptr_t>(p) & 0x3) == kTimerTag;
        }

        inline TimerEntry *untagTimer(void *p) {
            return reinterpret_cast<TimerEntry *>(
                reinterpret_cast<std::uintptr_t>(p) & ~static_cast<std::uintptr_t>(0x3));
        }
    }

    void Reactor::registerTimer(std::chrono::nanoseconds duration,
                                 std::coroutine_handle<> h) noexcept {
        const int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (tfd < 0) {
            this->schedule(h);
            return;
        }
        struct itimerspec ts{};
        const long long ns = duration.count() > 0 ? duration.count() : 0;
        ts.it_value.tv_sec = static_cast<time_t>(ns / 1'000'000'000LL);
        ts.it_value.tv_nsec = static_cast<long>(ns % 1'000'000'000LL);
        if (::timerfd_settime(tfd, 0, &ts, nullptr) < 0) {
            ::close(tfd);
            this->schedule(h);
            return;
        }
        auto *entry = new TimerEntry{tfd, h};
        struct epoll_event ev{};
        ev.events = EPOLLIN | EPOLLONESHOT;
        ev.data.ptr = tagTimer(entry);
        if (::epoll_ctl(this->epfd, EPOLL_CTL_ADD, tfd, &ev) < 0) {
            delete entry;
            ::close(tfd);
            this->schedule(h);
        }
    }

    void Reactor::run() {
        constexpr int kBatch = 64;
        struct epoll_event events[kBatch];

        while (this->running.load(std::memory_order_acquire)) {
            int n = ::epoll_wait(this->epfd, events, kBatch, -1);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < n; ++i) {
                void *p = events[i].data.ptr;
                if (reinterpret_cast<std::uintptr_t>(p) == kWakeupSentinel) {
                    std::uint64_t buf;
                    (void) ::read(this->wakefd, &buf, sizeof(buf));
                    continue;
                }
                if (isTimer(p)) {
                    TimerEntry *entry = untagTimer(p);
                    std::uint64_t expirations;
                    (void) ::read(entry->fd, &expirations, sizeof(expirations));
                    ::close(entry->fd);
                    std::coroutine_handle<> h = entry->handle;
                    delete entry;
                    this->schedule(h);
                    continue;
                }
                auto h = std::coroutine_handle<>::from_address(p);
                this->schedule(h);
            }
        }
    }

#elif defined(YARN_REACTOR_IOCP)

    /**
     * @section windows_reactor_design Windows backend
     *
     * Two cooperating threads back the Reactor on Windows:
     *
     *  1. Readiness thread (@c WindowsState::pollThread):
     *     Runs WSAPoll over a snapshot of the registered (SOCKET, mask,
     *     handle) set plus a wakeup socket. On readiness, schedules the
     *     associated coroutine onto the Yarn pool, then drops the entry
     *     from the set (one-shot semantics matching epoll EPOLLONESHOT
     *     and io_uring poll_add). Also services the timer min-heap by
     *     capping its WSAPoll timeout on the next deadline.
     *
     *  2. IOCP completion thread (@c WindowsState::iocpThread):
     *     Runs GetQueuedCompletionStatus on the Reactor's IOCP. Each
     *     completion is delivered as an OverlappedCompletion (defined in
     *     WindowsOverlapped.h); the thread fills in @c bytes / @c err on
     *     the completion record and schedules its embedded coroutine.
     *
     * The two surfaces are independent: a socket can be registered for
     * readiness via registerReadable, associated with the IOCP via
     * @c associateIocp, or both. Soccer's portable TCP/UDP path uses the
     * readiness API exclusively; its @c asyncRecvOverlapped /
     * @c asyncSendOverlapped helpers use the IOCP path.
     */

    namespace {
        /**
         * @brief Sentinel completion key used by @c stop to break the
         *        IOCP completion thread out of GetQueuedCompletionStatus.
         */
        inline constexpr ULONG_PTR kIocpStopKey = static_cast<ULONG_PTR>(-1);

        /**
         * @brief Initial reservation for the WSAPoll fd vector. Sized so
         *        small workloads avoid heap traffic on every loop spin.
         */
        inline constexpr std::size_t kPollVectorReserve = 64;

        /**
         * @brief Hard cap on WSAPoll timeout in milliseconds. The kernel
         *        treats negative values as INFINITE; we cap finite waits
         *        well below INT_MAX so duration arithmetic does not
         *        overflow.
         */
        inline constexpr int kPollMaxTimeoutMs = 30 * 1000;

        /**
         * @brief Wakeup payload byte sent to the loopback wakeup socket
         *        when a producer needs to interrupt WSAPoll.
         */
        inline constexpr char kWakeupByte = 'w';

        /**
         * @brief One pending readiness registration: socket + interest
         *        mask (WSAPoll POLLIN/POLLOUT) + the coroutine handle to
         *        schedule when the kernel reports the requested event.
         */
        struct PollRegistration {
            SOCKET sock;
            short events;
            std::coroutine_handle<> handle;
        };

        /**
         * @brief One pending timer entry. Min-heap is keyed on @c deadline.
         */
        struct TimerEntry {
            std::chrono::steady_clock::time_point deadline;
            std::coroutine_handle<> handle;
        };

        struct TimerEntryGreater {
            bool operator()(const TimerEntry &a, const TimerEntry &b) const noexcept {
                return a.deadline > b.deadline;
            }
        };
    }

    /**
     * @struct WindowsState
     * @brief All Windows-specific Reactor state. Lives behind an opaque
     *        pointer in Reactor so the public header does not depend on
     *        @c <windows.h>.
     *
     * Locking: @c pollMu protects @c pending, @c timers, and the
     * @c needs_rebuild flag. The IOCP HANDLE is set once at construction
     * and is read-only thereafter. Producer threads (any caller of
     * registerReadable / registerWritable / registerTimer) acquire
     * @c pollMu briefly, mutate the registration set, and signal the
     * wakeup socket. The poll thread holds @c pollMu only while
     * snapshotting registrations; WSAPoll itself runs unlocked so
     * unrelated producers do not block on the poll wait.
     */
    struct WindowsState {
        /// IOCP HANDLE; lives for the Reactor's lifetime.
        HANDLE iocp = nullptr;

        /// IOCP completion thread.
        std::thread iocpThread;

        /// WSAPoll readiness thread.
        std::thread pollThread;

        /// Wakeup socket bound to a 127.0.0.1:ephemeral UDP endpoint. The
        /// poll thread always polls this for POLLIN; producers send a
        /// byte to the bound address to interrupt WSAPoll.
        SOCKET wakeSock = INVALID_SOCKET;

        /// Address the wakeup socket is bound to (kernel-assigned port).
        sockaddr_in wakeAddr{};

        /// Active readiness registrations. Mutated under @c pollMu by
        /// producers; the poll thread also moves entries out of here
        /// while building its WSAPOLLFD snapshot.
        std::vector<PollRegistration> pending;

        /// Pending timer entries, ordered as a min-heap on @c deadline.
        std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerEntryGreater> timers;

        /// Mutex guarding @c pending, @c timers.
        std::mutex pollMu;
    };

    namespace {
        /**
         * @brief Build the wakeup loopback socket for the WSAPoll thread.
         *        UDP keeps things simple -- no listen/accept dance, no
         *        connection state to tear down.
         */
        SOCKET makeWakeSocket(sockaddr_in &out_addr) {
            SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == INVALID_SOCKET) {
                throw std::runtime_error("Reactor: wake socket() failed");
            }
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = 0;
            addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
            if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
                ::closesocket(s);
                throw std::runtime_error("Reactor: wake bind() failed");
            }
            int alen = sizeof(addr);
            if (::getsockname(s, reinterpret_cast<sockaddr *>(&addr), &alen) != 0) {
                ::closesocket(s);
                throw std::runtime_error("Reactor: wake getsockname() failed");
            }
            u_long mode = 1;
            (void) ::ioctlsocket(s, FIONBIO, &mode);
            out_addr = addr;
            return s;
        }
    }

    Reactor::Reactor() {
        ensureWsaStarted();

        auto *st = new WindowsState();
        try {
            st->iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
            if (st->iocp == NULL) {
                throw std::runtime_error("Reactor: CreateIoCompletionPort failed");
            }
            st->wakeSock = makeWakeSocket(st->wakeAddr);
            st->pending.reserve(kPollVectorReserve);
        } catch (...) {
            if (st->wakeSock != INVALID_SOCKET) ::closesocket(st->wakeSock);
            if (st->iocp) ::CloseHandle(st->iocp);
            delete st;
            throw;
        }
        this->winstate = st;

        // Both threads need @c this and @c winstate. Spawn the IOCP
        // completion thread first (it is the one that owns the
        // overlapped completion path and is independent of the poll
        // thread). The poll thread is spawned via @c run for symmetry
        // with the other backends.
        st->iocpThread = std::thread([this]() {
            auto *s = this->winstate;
            while (this->running.load(std::memory_order_acquire)) {
                DWORD bytes = 0;
                ULONG_PTR key = 0;
                LPOVERLAPPED ov = nullptr;
                BOOL ok = ::GetQueuedCompletionStatus(s->iocp, &bytes, &key, &ov, INFINITE);
                if (!this->running.load(std::memory_order_acquire)) break;
                if (key == kIocpStopKey) break;
                if (ov == nullptr) {
                    // Spurious wake (timeout / posted with no overlapped
                    // and a non-stop key); harmless, loop back.
                    continue;
                }
                auto *c = static_cast<OverlappedCompletion *>(ov);
                c->bytes = bytes;
                c->err = ok ? 0 : static_cast<int>(::GetLastError());
                if (c->handle) this->schedule(c->handle);
            }
        });

        this->thread = std::thread(&Reactor::run, this);
    }

    Reactor::~Reactor() {
        this->stop();
        if (this->thread.joinable()) this->thread.join();
        if (this->winstate) {
            if (this->winstate->iocpThread.joinable()) {
                this->winstate->iocpThread.join();
            }
            if (this->winstate->wakeSock != INVALID_SOCKET) {
                ::closesocket(this->winstate->wakeSock);
            }
            if (this->winstate->iocp) ::CloseHandle(this->winstate->iocp);
            delete this->winstate;
            this->winstate = nullptr;
        }
    }

    void Reactor::stop() {
        bool was_running = this->running.exchange(false, std::memory_order_acq_rel);
        if (!was_running) return;
        if (!this->winstate) return;
        // Wake the IOCP completion thread.
        ::PostQueuedCompletionStatus(this->winstate->iocp,
                                     /*bytes=*/0,
                                     /*key=*/kIocpStopKey,
                                     /*overlapped=*/nullptr);
        // Wake the WSAPoll thread by sending a byte to the wakeup socket.
        if (this->winstate->wakeSock != INVALID_SOCKET) {
            const char b = kWakeupByte;
            (void) ::sendto(this->winstate->wakeSock, &b, 1, 0,
                            reinterpret_cast<sockaddr *>(&this->winstate->wakeAddr),
                            sizeof(this->winstate->wakeAddr));
        }
    }

    void Reactor::registerReadable(int fd, std::coroutine_handle<> h) noexcept {
        if (!this->winstate) {
            this->schedule(h);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(this->winstate->pollMu);
            this->winstate->pending.push_back(PollRegistration{
                static_cast<SOCKET>(static_cast<unsigned>(fd)),
                static_cast<short>(POLLRDNORM),
                h
            });
        }
        // Best-effort wake. Any error from sendto is silently ignored:
        // either the wake socket is closed (we are shutting down anyway)
        // or the kernel buffer is briefly full (the next iteration's
        // WSAPoll will time out and pick the new entry up regardless).
        const char b = kWakeupByte;
        (void) ::sendto(this->winstate->wakeSock, &b, 1, 0,
                        reinterpret_cast<sockaddr *>(&this->winstate->wakeAddr),
                        sizeof(this->winstate->wakeAddr));
    }

    void Reactor::registerWritable(int fd, std::coroutine_handle<> h) noexcept {
        if (!this->winstate) {
            this->schedule(h);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(this->winstate->pollMu);
            this->winstate->pending.push_back(PollRegistration{
                static_cast<SOCKET>(static_cast<unsigned>(fd)),
                static_cast<short>(POLLWRNORM),
                h
            });
        }
        const char b = kWakeupByte;
        (void) ::sendto(this->winstate->wakeSock, &b, 1, 0,
                        reinterpret_cast<sockaddr *>(&this->winstate->wakeAddr),
                        sizeof(this->winstate->wakeAddr));
    }

    void Reactor::registerTimer(std::chrono::nanoseconds duration,
                                 std::coroutine_handle<> h) noexcept {
        if (!this->winstate) {
            this->schedule(h);
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              (duration.count() > 0 ? duration : std::chrono::nanoseconds{0});
        {
            std::lock_guard<std::mutex> lk(this->winstate->pollMu);
            this->winstate->timers.push(TimerEntry{deadline, h});
        }
        const char b = kWakeupByte;
        (void) ::sendto(this->winstate->wakeSock, &b, 1, 0,
                        reinterpret_cast<sockaddr *>(&this->winstate->wakeAddr),
                        sizeof(this->winstate->wakeAddr));
    }

    bool Reactor::associateIocp(int fd) noexcept {
        if (!this->winstate || !this->winstate->iocp) return false;
        HANDLE h = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(static_cast<unsigned>(fd)));
        HANDLE r = ::CreateIoCompletionPort(h, this->winstate->iocp, /*key=*/0, 0);
        if (r == NULL) {
            // ERROR_INVALID_PARAMETER means the handle is already
            // associated with this IOCP; treat as success (idempotent).
            return ::GetLastError() == ERROR_INVALID_PARAMETER;
        }
        return true;
    }

    void *Reactor::iocpHandle() const noexcept {
        return this->winstate ? static_cast<void *>(this->winstate->iocp) : nullptr;
    }

    void Reactor::run() {
        if (!this->winstate) return;
        std::vector<WSAPOLLFD> fds;
        std::vector<std::coroutine_handle<>> readyHandles;
        fds.reserve(kPollVectorReserve);
        readyHandles.reserve(kPollVectorReserve);

        while (this->running.load(std::memory_order_acquire)) {
            // -- snapshot under the lock -----------------------------
            int timeoutMs = kPollMaxTimeoutMs;
            fds.clear();
            readyHandles.clear();
            std::vector<PollRegistration> active;
            std::vector<std::coroutine_handle<>> expiredTimers;
            {
                std::lock_guard<std::mutex> lk(this->winstate->pollMu);
                active = std::move(this->winstate->pending);
                this->winstate->pending.clear();

                // Drain expired timers while we hold the lock.
                const auto now = std::chrono::steady_clock::now();
                while (!this->winstate->timers.empty()
                       && this->winstate->timers.top().deadline <= now) {
                    expiredTimers.push_back(this->winstate->timers.top().handle);
                    this->winstate->timers.pop();
                }

                if (!this->winstate->timers.empty()) {
                    const auto next = this->winstate->timers.top().deadline;
                    const auto delta_ns =
                        std::chrono::duration_cast<std::chrono::milliseconds>(next - now);
                    long long ms = delta_ns.count();
                    if (ms < 0) ms = 0;
                    if (ms > kPollMaxTimeoutMs) ms = kPollMaxTimeoutMs;
                    timeoutMs = static_cast<int>(ms);
                }
            }

            // Schedule expired timers off-lock to avoid holding the
            // pollMu across Yarn::run dispatches.
            for (auto h : expiredTimers) this->schedule(h);

            // -- build poll set --------------------------------------
            // Wakeup socket always at index 0.
            WSAPOLLFD wake{};
            wake.fd = this->winstate->wakeSock;
            wake.events = POLLRDNORM;
            fds.push_back(wake);

            for (const auto &reg : active) {
                WSAPOLLFD pf{};
                pf.fd = reg.sock;
                pf.events = reg.events;
                fds.push_back(pf);
            }

            // -- wait ------------------------------------------------
            int n = ::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeoutMs);
            if (n == SOCKET_ERROR) {
                // Treat poll failure as fatal for this iteration: re-queue
                // any active registrations so they are not lost, then loop.
                if (!active.empty()) {
                    std::lock_guard<std::mutex> lk(this->winstate->pollMu);
                    for (auto &reg : active) {
                        this->winstate->pending.push_back(reg);
                    }
                }
                continue;
            }

            // -- service the wakeup socket ---------------------------
            if (fds[0].revents & (POLLRDNORM | POLLIN)) {
                char buf[256];
                while (true) {
                    int got = ::recvfrom(this->winstate->wakeSock,
                                          buf, sizeof(buf), 0,
                                          nullptr, nullptr);
                    if (got <= 0) break;
                }
            }

            // -- dispatch ready registrations; re-queue the rest -----
            // We iterate active[] in lockstep with fds[1..]. Anything
            // ready (or in error/hangup) gets scheduled and dropped;
            // the rest is pushed back into pending so the next loop
            // iteration picks it up.
            std::vector<PollRegistration> stillPending;
            stillPending.reserve(active.size());
            const short readyMask = POLLRDNORM | POLLWRNORM | POLLIN | POLLOUT
                                     | POLLERR | POLLHUP | POLLNVAL;
            for (std::size_t i = 0; i < active.size(); ++i) {
                const short revents = fds[i + 1].revents;
                if (revents & readyMask) {
                    this->schedule(active[i].handle);
                } else {
                    stillPending.push_back(active[i]);
                }
            }
            if (!stillPending.empty()) {
                std::lock_guard<std::mutex> lk(this->winstate->pollMu);
                for (auto &reg : stillPending) {
                    this->winstate->pending.push_back(reg);
                }
            }
        }
    }

#else // YARN_REACTOR_STUB

    Reactor::Reactor() {
        throw std::runtime_error("Reactor: no backend on this platform yet");
    }

    Reactor::~Reactor() = default;

    void Reactor::stop() {}
    void Reactor::registerReadable(int, std::coroutine_handle<>) noexcept {}
    void Reactor::registerWritable(int, std::coroutine_handle<>) noexcept {}
    void Reactor::registerTimer(std::chrono::nanoseconds,
                                 std::coroutine_handle<>) noexcept {}
    void Reactor::run() {}

#endif

}
