//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "Reactor.h"

#include <algorithm>
#include <limits>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "Coroutines.h"
#include "Yarn.hpp"

// The kernel readiness queues (kqueue, io_uring) hand a coroutine from the thread that registered it to the reactor
// thread without any synchronisation ThreadSanitizer can see, so it would report the frame's earlier writes as racing
// with the resume. Mark the hand-off explicitly: release at registration, acquire where the reactor schedules it.
#if defined(__SANITIZE_THREAD__)
    #define YARN_TSAN_ENABLED 1
#elif defined(__has_feature)
    #if __has_feature(thread_sanitizer)
        #define YARN_TSAN_ENABLED 1
    #endif
#endif
#if defined(YARN_TSAN_ENABLED)
    extern "C" void __tsan_acquire(void *addr);
    extern "C" void __tsan_release(void *addr);
    #define YARN_TSAN_RELEASE(p) __tsan_release(p)
    #define YARN_TSAN_ACQUIRE(p) __tsan_acquire(p)
#else
    #define YARN_TSAN_RELEASE(p) ((void) 0)
    #define YARN_TSAN_ACQUIRE(p) ((void) 0)
#endif

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

    namespace {
        Reactor *g_reactorForExit = nullptr;
    }

    Reactor *Reactor::instance() {
        // Force Yarn to construct first so its static-dtor runs AFTER ours.
        // We keep scheduling onto Yarn until our thread is joined; Yarn must
        // be alive for that window.
        Yarn::instance();
        // Immortal: coroutines that outlive main() (detached tasks closing sockets, withTimeout sleepers) may still
        // call cancelFd & co. during static destruction. At exit only the loop thread is stopped and joined, at the
        // point where the Reactor used to be destroyed, i.e. while Yarn (constructed first, destroyed last) is alive.
        static Reactor *const inst = [] {
            auto *r = new Reactor;
            g_reactorForExit = r;
            std::atexit([] { g_reactorForExit->retire(); });
            return r;
        }();
        return inst;
    }

    void Reactor::retire() noexcept {
        this->stop();
        if (this->thread.joinable()) this->thread.join();
    }

    void Reactor::schedule(std::coroutine_handle<> h) noexcept {
        if (!h) return;
        YARN_TSAN_ACQUIRE(h.address());
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

    void Reactor::registerFd(int fd, std::coroutine_handle<> h, bool writable) noexcept {
        bool refused = false;
        bool backoff = false; // the kernel is out of memory for knotes: retry shortly instead of spinning on EAGAIN
        std::vector<void *> stale;
        {
            // Hold fdMu across the kevent call: the readiness event can only be consumed by the run loop after the
            // handle is in the table.
            std::lock_guard<std::mutex> lk(this->fdMu);
            if (this->dead.load(std::memory_order_acquire)) {
                // The loop is gone (the flag is set under this lock by abandonWaiters): nothing would ever fire this
                // registration, so resume it now.
                refused = true;
            } else {
                detail::FdWaiters &w = this->fdWaiters[fd];
                // Waiters already parked on this fd number must still have a live knote behind them. If it is gone, the
                // fd they waited on was closed without cancelFd (the kernel dropped the knote) and this number has been
                // reused: they would otherwise sit here until an unrelated socket's readiness woke them, or forever.
                // Resume them; their next syscall reports the closed descriptor.
                struct kevent probe;
                if (!w.readers.empty()) {
                    EV_SET(&probe, fd, EVFILT_READ, EV_ENABLE, 0, 0, nullptr);
                    if (::kevent(this->kq, &probe, 1, nullptr, 0, nullptr) < 0 && errno == ENOENT) w.readers.takeAll(stale);
                }
                if (!w.writers.empty()) {
                    EV_SET(&probe, fd, EVFILT_WRITE, EV_ENABLE, 0, 0, nullptr);
                    if (::kevent(this->kq, &probe, 1, nullptr, 0, nullptr) < 0 && errno == ENOENT) w.writers.takeAll(stale);
                }
                detail::DirWaiters &d = writable ? w.writers : w.readers;
                d.push(h.address());
                struct kevent change;
                EV_SET(&change, fd, writable ? EVFILT_WRITE : EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, nullptr);
                if (::kevent(this->kq, &change, 1, nullptr, 0, nullptr) < 0) {
                    backoff = errno == ENOMEM;
                    d.remove(h.address());
                    if (w.empty()) this->fdWaiters.erase(fd);
                    refused = true;
                }
            }
        }
        for (void *p : stale) this->schedule(std::coroutine_handle<>::from_address(p));
        if (refused) {
            // A descriptor-level failure (EBADF, ...) resumes now so the waiter's own syscall reports it. Resource
            // exhaustion is not that: the syscall would say EAGAIN and the waiter would register again at full speed.
            if (backoff) {
                this->registerTimer(std::chrono::milliseconds{1}, h);
            } else {
                this->schedule(h);
            }
        }
    }

    void Reactor::registerReadable(SocketHandle fd, std::coroutine_handle<> h) noexcept {
        this->registerFd(fd, h, /*writable=*/false);
    }

    void Reactor::registerWritable(SocketHandle fd, std::coroutine_handle<> h) noexcept {
        this->registerFd(fd, h, /*writable=*/true);
    }

    bool Reactor::cancelWait(SocketHandle fd, std::coroutine_handle<> h, bool resume) noexcept {
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(this->fdMu);
            auto it = this->fdWaiters.find(fd);
            if (it == this->fdWaiters.end()) return false;
            detail::FdWaiters &w = it->second;
            struct kevent change;
            if (w.readers.remove(h.address())) {
                found = true;
                if (w.readers.empty()) {
                    EV_SET(&change, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
                    (void) ::kevent(this->kq, &change, 1, nullptr, 0, nullptr);
                }
            } else if (w.writers.remove(h.address())) {
                found = true;
                if (w.writers.empty()) {
                    EV_SET(&change, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
                    (void) ::kevent(this->kq, &change, 1, nullptr, 0, nullptr);
                }
            }
            if (w.empty()) this->fdWaiters.erase(it);
        }
        if (found && resume) this->schedule(h);
        return found;
    }

    std::size_t Reactor::cancelFd(SocketHandle fd) noexcept {
        std::vector<void *> woken;
        {
            std::lock_guard<std::mutex> lk(this->fdMu);
            auto it = this->fdWaiters.find(fd);
            if (it == this->fdWaiters.end()) return 0;
            struct kevent change;
            if (!it->second.readers.empty()) {
                EV_SET(&change, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
                (void) ::kevent(this->kq, &change, 1, nullptr, 0, nullptr);
            }
            if (!it->second.writers.empty()) {
                EV_SET(&change, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
                (void) ::kevent(this->kq, &change, 1, nullptr, 0, nullptr);
            }
            it->second.readers.takeAll(woken);
            it->second.writers.takeAll(woken);
            this->fdWaiters.erase(it);
        }
        for (void *p : woken) this->schedule(std::coroutine_handle<>::from_address(p));
        return woken.size();
    }

    namespace {
        /// Longest timer accepted: keeps `now + duration` (and the nanosecond count handed to the kernel) from overflowing.
        inline constexpr long long kMaxTimerNs = 3'000'000'000'000'000'000LL;
        /// Identifier of the single EVFILT_TIMER knote that serves every sleep (its own namespace, separate from fds).
        inline constexpr std::uintptr_t kTimerIdent = 1;
    }

    void Reactor::armKernelTimerLocked(std::chrono::steady_clock::time_point deadline) noexcept {
        auto rel = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (rel < 1) rel = 1; // a zero period would be rejected or fire unpredictably; one nanosecond is "now"
        struct kevent change;
        // Re-adding the same ident replaces the pending timer, so an earlier deadline simply re-arms it.
        EV_SET(&change, kTimerIdent, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS, static_cast<std::intptr_t>(rel), nullptr);
        if (::kevent(this->kq, &change, 1, nullptr, 0, nullptr) == 0) {
            this->timerArmed = true;
            this->timerArmedFor = deadline;
        } else {
            // Cannot happen for a valid kqueue; if it does, poke the loop so it re-evaluates instead of sitting on a
            // deadline nobody is waiting for.
            this->timerArmed = false;
            struct kevent poke;
            EV_SET(&poke, /*ident=*/0, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
            (void) ::kevent(this->kq, &poke, 1, nullptr, 0, nullptr);
        }
    }

    void Reactor::registerTimer(std::chrono::nanoseconds duration,
                                 std::coroutine_handle<> h) noexcept {
        // One kernel timer serves every sleep: the deadline goes into an ordered table and the timer is armed for the
        // earliest. A register or cancel is then a table update, with no syscall unless the earliest deadline moves.
        const long long ns = duration.count() < 0 ? 0 : (duration.count() > kMaxTimerNs ? kMaxTimerNs : duration.count());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
        YARN_TSAN_RELEASE(h.address());
        bool armed = false;
        try {
            // Insert and arm under one hold of timerSetMu: a concurrent cancelTimer (a sleep's stop callback can fire
            // as soon as it is armed, before this call returns) either runs before the entry exists (and finds nothing)
            // or after it is fully registered, so it never sees a half-built entry.
            std::lock_guard<std::mutex> lk(this->timerSetMu);
            if (!this->dead.load(std::memory_order_acquire) && this->timerIndex.find(h.address()) == this->timerIndex.end()) {
                auto it = this->timers.emplace(deadline, h.address());
                try {
                    this->timerIndex.emplace(h.address(), it);
                    armed = true;
                } catch (...) {
                    this->timers.erase(it);
                    throw;
                }
                if (it == this->timers.begin() && (!this->timerArmed || deadline < this->timerArmedFor)) {
                    this->armKernelTimerLocked(deadline);
                }
            }
        } catch (...) {
            armed = false;
        }
        // Not tracked (loop dead, duplicate handle, out of memory): wake it now rather than strand it.
        if (!armed) this->schedule(h);
    }

    bool Reactor::cancelTimer(std::coroutine_handle<> h, bool resume) noexcept {
        bool found = false;
        {
            // Whoever erases the index entry (this, the run loop on expiry, or abandonWaiters) owns the single resume.
            // The kernel timer is left alone: if this was the earliest, the loop wakes once for nothing and re-arms.
            std::lock_guard<std::mutex> lk(this->timerSetMu);
            auto it = this->timerIndex.find(h.address());
            if (it != this->timerIndex.end()) {
                this->timers.erase(it->second);
                this->timerIndex.erase(it);
                found = true;
            }
        }
        if (found && resume) this->schedule(h);
        return found;
    }

    void Reactor::run() {
        constexpr int kBatch = 64;
        struct kevent events[kBatch];
        std::vector<void *> ready;
        std::vector<void *> due;

        while (this->running.load(std::memory_order_acquire)) {
            int n = ::kevent(this->kq, nullptr, 0, events, kBatch, nullptr);
            if (n < 0) {
                if (errno == EINTR) continue;
                this->abandonWaiters("kevent", errno);
                break;
            }
            for (int i = 0; i < n; ++i) {
                const struct kevent &ev = events[i];
                if (ev.filter == EVFILT_USER) {
                    // A poke may mean arming the timer failed: make sure it is armed for the earliest deadline.
                    std::lock_guard<std::mutex> lk(this->timerSetMu);
                    if (!this->timers.empty() && (!this->timerArmed || this->timers.begin()->first < this->timerArmedFor)) {
                        this->armKernelTimerLocked(this->timers.begin()->first);
                    }
                    continue;
                }
                if (ev.filter == EVFILT_TIMER) {
                    due.clear();
                    {
                        std::lock_guard<std::mutex> lk(this->timerSetMu);
                        const auto now = std::chrono::steady_clock::now();
                        while (!this->timers.empty() && this->timers.begin()->first <= now) {
                            due.push_back(this->timers.begin()->second);
                            this->timerIndex.erase(this->timers.begin()->second);
                            this->timers.erase(this->timers.begin());
                        }
                        this->timerArmed = false;
                        if (!this->timers.empty()) this->armKernelTimerLocked(this->timers.begin()->first);
                    }
                    for (void *d : due) this->schedule(std::coroutine_handle<>::from_address(d));
                    continue;
                }
                const int fd = static_cast<int>(ev.ident);
                ready.clear();
                {
                    std::lock_guard<std::mutex> lk(this->fdMu);
                    auto it = this->fdWaiters.find(fd);
                    if (it == this->fdWaiters.end()) continue;
                    (ev.filter == EVFILT_WRITE ? it->second.writers : it->second.readers).takeAll(ready);
                    if (it->second.empty()) this->fdWaiters.erase(it);
                }
                for (void *p : ready) this->schedule(std::coroutine_handle<>::from_address(p));
            }
        }
    }

    void Reactor::abandonWaiters(const char *what, int err) noexcept {
        std::fprintf(stderr, "Reactor: %s failed (%s); loop exiting, waking parked waiters\n", what, std::strerror(err));
        std::vector<void *> woken;
        {
            std::lock_guard<std::mutex> lk(this->fdMu);
            this->dead.store(true, std::memory_order_release);
            for (auto &kv : this->fdWaiters) {
                kv.second.readers.takeAll(woken);
                kv.second.writers.takeAll(woken);
            }
            this->fdWaiters.clear();
        }
        {
            // The loop that would fire these is gone: resume the sleepers too rather than strand them.
            std::lock_guard<std::mutex> lk(this->timerSetMu);
            this->dead.store(true, std::memory_order_release); // also under this lock: see registerTimer
            for (auto &kv : this->timers) woken.push_back(kv.second);
            this->timers.clear();
            this->timerIndex.clear();
        }
        for (void *p : woken) this->schedule(std::coroutine_handle<>::from_address(p));
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
         * @brief Low-bit tag of the user_data of the single timeout operation that serves every sleep:
         *        (generation << 2) | kTimerTag. Handles are at least 8-aligned, the wake sentinel is 1 and removal
         *        requests carry tag 2, so the low two bits tell the four apart.
         */
        inline constexpr std::uintptr_t kTimerTag = 3;

        /// Longest sleep we arm; keeps now() + duration far from steady_clock overflow (about 100 years).
        inline constexpr long long kMaxTimerNs = 3'000'000'000'000'000'000LL;

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
            // There is no epoll fallback inside this backend (the Reactor's layout is chosen at compile time): a kernel or
            // sandbox that disables io_uring needs a build without YARN_USE_IO_URING.
            throw std::runtime_error(std::string("Reactor: io_uring_queue_init_params failed (") + std::strerror(-r)
                                     + "); io_uring is unavailable here, build without YARN_USE_IO_URING to use epoll");
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
        {
            // The loop has exited: sleeps still armed are dropped without resuming, as on epoll.
            std::lock_guard<std::mutex> lk(this->sqMu);
            this->timerIndex.clear();
            this->timers.clear();
        }
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
        [[maybe_unused]] const auto rc = ::write(this->wakefd, &v, sizeof(v));
    }

    namespace {
        /**
         * @brief Tag in the low bits of a user_data that marks the completion
         *        of a poll/timeout *removal* request; the remaining bits are
         *        the handle address whose operation it targeted.
         */
        inline constexpr std::uintptr_t kRemovalTag = 2;

        /**
         * @brief Push queued SQEs, retrying transient errors. A failed
         *        io_uring_submit leaves the prepared SQEs in the ring (the
         *        next submit sends them), so callers must NOT treat the
         *        operation as lost.
         */
        int submitRetry(::io_uring *ring) noexcept {
            int r = 0;
            for (int i = 0; i < 1000; ++i) {
                r = ::io_uring_submit(ring);
                // A short submit (the kernel stopped part-way) leaves SQEs queued with nobody to push them.
                if (r >= 0 && ::io_uring_sq_ready(ring) == 0) return r;
                if (r < 0 && r != -EINTR && r != -EAGAIN && r != -EBUSY) return r;
                std::this_thread::yield();
            }
            return r;
        }
    }

    void Reactor::pokeLoop() noexcept {
        std::uint64_t v = 1;
        [[maybe_unused]] const auto rc = ::write(this->wakefd, &v, sizeof(v));
    }

    io_uring_sqe *Reactor::getSqeLocked() noexcept {
        // A full submission queue is a transient condition: submitting what is queued frees the slots.
        for (int i = 0; i < 1000; ++i) {
            if (io_uring_sqe *sqe = ::io_uring_get_sqe(this->ring)) return sqe;
            (void) submitRetry(this->ring);
            std::this_thread::yield();
        }
        return nullptr;
    }

    bool Reactor::trackPollLocked(int fd, std::coroutine_handle<> h) noexcept {
        try {
            this->pollFds[h.address()] = fd;
            return true;
        } catch (...) {
            return false;
        }
    }

    void Reactor::registerReadable(SocketHandle fd, std::coroutine_handle<> h) noexcept {
        bool resumeNow = false;
        bool backoff = false; // ring full or out of memory: retry shortly instead of spinning on EAGAIN
        {
            std::lock_guard<std::mutex> lk(this->sqMu);
            io_uring_sqe *sqe = nullptr;
            if (!this->dead.load(std::memory_order_relaxed) && this->trackPollLocked(fd, h)) sqe = this->getSqeLocked();
            if (!sqe) {
                // Loop dead, or SQ wedged / out of memory: fall back to immediate resume. The user-level read
                // syscall will surface the failure mode honestly.
                this->pollFds.erase(h.address());
                resumeNow = true;
                backoff = !this->dead.load(std::memory_order_relaxed);
            } else {
                ::io_uring_prep_poll_add(sqe, fd, POLLIN);
                YARN_TSAN_RELEASE(h.address());
                ::io_uring_sqe_set_data(sqe, h.address());
                // On failure the SQE is still queued and will complete (and resume h) once submitted; scheduling h here
                // as well would resume it twice. Make the loop submit it instead.
                if (submitRetry(this->ring) < 0) this->pokeLoop();
            }
        }
        if (resumeNow) {
            if (backoff) {
                this->registerTimer(std::chrono::milliseconds{1}, h);
            } else {
                this->schedule(h);
            }
        }
    }

    void Reactor::registerWritable(SocketHandle fd, std::coroutine_handle<> h) noexcept {
        bool resumeNow = false;
        bool backoff = false; // ring full or out of memory: retry shortly instead of spinning on EAGAIN
        {
            std::lock_guard<std::mutex> lk(this->sqMu);
            io_uring_sqe *sqe = nullptr;
            if (!this->dead.load(std::memory_order_relaxed) && this->trackPollLocked(fd, h)) sqe = this->getSqeLocked();
            if (!sqe) {
                this->pollFds.erase(h.address());
                resumeNow = true;
                backoff = !this->dead.load(std::memory_order_relaxed);
            } else {
                ::io_uring_prep_poll_add(sqe, fd, POLLOUT);
                YARN_TSAN_RELEASE(h.address());
                ::io_uring_sqe_set_data(sqe, h.address());
                if (submitRetry(this->ring) < 0) this->pokeLoop();
            }
        }
        if (resumeNow) {
            if (backoff) {
                this->registerTimer(std::chrono::milliseconds{1}, h);
            } else {
                this->schedule(h);
            }
        }
    }

    bool Reactor::armKernelTimerLocked(std::chrono::steady_clock::time_point deadline) noexcept {
        if (this->timerArmed) {
            // Replace the armed timeout (it fires later than we now need). Its completion carries the old generation
            // and is ignored.
            io_uring_sqe *rm = this->getSqeLocked();
            if (!rm) {
                this->pokeLoop();
                return false;
            }
            ::io_uring_prep_timeout_remove(rm, (this->timerGen << 2) | kTimerTag, 0);
            ::io_uring_sqe_set_data(rm, reinterpret_cast<void *>(kRemovalTag));
            this->timerArmed = false;
        }
        io_uring_sqe *sqe = this->getSqeLocked();
        if (!sqe) {
            this->pokeLoop(); // the loop re-evaluates the table
            return false;
        }
        const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count();
        this->timerSpec[0] = ns / 1'000'000'000LL;
        this->timerSpec[1] = ns % 1'000'000'000LL;
        ++this->timerGen;
        // Absolute CLOCK_MONOTONIC deadline: steady_clock on Linux.
        ::io_uring_prep_timeout(sqe, reinterpret_cast<struct __kernel_timespec *>(this->timerSpec), 0, IORING_TIMEOUT_ABS);
        ::io_uring_sqe_set_data(sqe, reinterpret_cast<void *>((this->timerGen << 2) | kTimerTag));
        this->timerArmed = true;
        this->timerArmedFor = deadline;
        if (submitRetry(this->ring) < 0) {
            this->pokeLoop(); // still queued; the loop's next submit sends it
            return true;
        }
#  ifdef YARN_IO_URING_SQPOLL
        // With SQPOLL the kernel thread reads the SQE (and the timespec it points at) asynchronously; the member timespec
        // may be rewritten by the next arm, so wait until this one has been consumed.
        while (::io_uring_sq_ready(this->ring) != 0) std::this_thread::yield();
#  endif
        return true;
    }

    void Reactor::registerTimer(std::chrono::nanoseconds duration,
                                 std::coroutine_handle<> h) noexcept {
        // One kernel timeout serves every sleep: the deadline goes into an ordered table and the timeout is armed for
        // the earliest. A cancel is then a table erase instead of a kernel-side scan (quadratic with many sleepers).
        const long long ns = duration.count() < 0 ? 0 : (duration.count() > kMaxTimerNs ? kMaxTimerNs : duration.count());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
        YARN_TSAN_RELEASE(h.address());
        bool armed = false;
        try {
            std::lock_guard<std::mutex> lk(this->sqMu);
            if (!this->dead.load(std::memory_order_relaxed) && this->timerIndex.find(h.address()) == this->timerIndex.end()) {
                auto it = this->timers.emplace(deadline, h.address());
                try {
                    this->timerIndex.emplace(h.address(), it);
                    armed = true;
                } catch (...) {
                    this->timers.erase(it);
                    throw;
                }
                if (it == this->timers.begin() && (!this->timerArmed || deadline < this->timerArmedFor)) {
                    this->armKernelTimerLocked(deadline);
                }
            }
        } catch (...) {
            armed = false;
        }
        // Not tracked (loop dead, duplicate handle, out of memory): wake it now rather than strand it.
        if (!armed) this->schedule(h);
    }

    bool Reactor::cancelByHandle(void *key, bool resume) noexcept {
        std::lock_guard<std::mutex> lk(this->sqMu);
        io_uring_sqe *sqe = this->getSqeLocked();
        if (!sqe) return false;
        ::io_uring_prep_poll_remove(sqe, reinterpret_cast<std::uintptr_t>(key));
        // Resuming needs nothing more: the cancelled operation completes (-ECANCELED) and the loop resumes its handle as
        // for any other completion. Dropping marks the handle so that completion is swallowed.
        if (!resume) {
            this->droppedHandles.push_back(key);
            this->droppedPending.fetch_add(1, std::memory_order_release);
        }
        ::io_uring_sqe_set_data(sqe, reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(key) | kRemovalTag));
        if (submitRetry(this->ring) < 0) this->pokeLoop();
        return true;
    }

    bool Reactor::cancelWait(SocketHandle, std::coroutine_handle<> h, bool resume) noexcept {
        return this->cancelByHandle(h.address(), resume);
    }

    bool Reactor::cancelTimer(std::coroutine_handle<> h, bool resume) noexcept {
        bool found = false;
        {
            // Whoever erases the index entry (this, the loop on expiry, or abandonWaiters) owns the single resume. The
            // kernel timeout is left alone: if this was the earliest, the loop wakes once for nothing and re-arms.
            std::lock_guard<std::mutex> lk(this->sqMu);
            auto it = this->timerIndex.find(h.address());
            if (it != this->timerIndex.end()) {
                this->timers.erase(it->second);
                this->timerIndex.erase(it);
                found = true;
            }
        }
        if (found && resume) this->schedule(h);
        return found;
    }

    std::size_t Reactor::cancelFd(SocketHandle fd) noexcept {
        // One poll removal per waiter rather than a single cancel-by-fd request: IORING_ASYNC_CANCEL_FD needs kernel
        // 5.19 and is only known at compile time, while a poll removal works wherever io_uring polls do. The entries stay
        // in pollFds; each cancelled poll completes with -ECANCELED and the loop erases it and resumes its handle.
        std::lock_guard<std::mutex> lk(this->sqMu);
        std::vector<void *> keys;
        try {
            for (const auto &kv : this->pollFds) {
                if (kv.second == fd) keys.push_back(kv.first);
            }
        } catch (...) {
            return 0;
        }
        std::size_t n = 0;
        for (void *key : keys) {
            io_uring_sqe *sqe = this->getSqeLocked();
            if (!sqe) break;
            ::io_uring_prep_poll_remove(sqe, reinterpret_cast<std::uintptr_t>(key));
            ::io_uring_sqe_set_data(sqe, reinterpret_cast<void *>(kRemovalTag));
            ++n;
        }
        if (n != 0 && submitRetry(this->ring) < 0) this->pokeLoop();
        return n;
    }

    void Reactor::abandonWaiters(const char *what, int err) noexcept {
        std::fprintf(stderr, "Reactor: %s failed (%s); loop exiting, waking parked waiters\n", what, std::strerror(err));
        std::vector<void *> woken;
        {
            std::lock_guard<std::mutex> lk(this->sqMu);
            this->dead.store(true, std::memory_order_release);
            for (const auto &kv : this->pollFds) woken.push_back(kv.first);
            this->pollFds.clear();
            // The loop that would fire these is gone: resume the sleepers too rather than strand them.
            for (const auto &kv : this->timers) woken.push_back(kv.second);
            this->timers.clear();
            this->timerIndex.clear();
        }
        for (void *p : woken) this->schedule(std::coroutine_handle<>::from_address(p));
    }

    void Reactor::run() {
        std::vector<void *> due;
        while (this->running.load(std::memory_order_acquire)) {
            io_uring_cqe *cqe = nullptr;
            int r = ::io_uring_wait_cqe(this->ring, &cqe);
            if (r < 0) {
                if (-r == EINTR) continue;
                this->abandonWaiters("io_uring_wait_cqe", -r);
                break;
            }
            if (!cqe) continue;

            void *data = ::io_uring_cqe_get_data(cqe);
            if (reinterpret_cast<std::uintptr_t>(data) == kWakeUserData) {
                // Wake event: drain the eventfd, re-arm the watch, loop
                // will re-check running and exit if needed.
                std::uint64_t buf;
                [[maybe_unused]] const auto rc = ::read(this->wakefd, &buf, sizeof(buf));
                ::io_uring_cqe_seen(this->ring, cqe);

                if (this->running.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> lk(this->sqMu);
                    // The watch must be re-armed or later pokes (and stop()) no longer wake the loop: keep trying for a
                    // slot rather than give up when the queue is full for a moment.
                    io_uring_sqe *sqe = this->getSqeLocked();
                    while (!sqe && this->running.load(std::memory_order_acquire)) sqe = this->getSqeLocked();
                    if (sqe) {
                        ::io_uring_prep_poll_add(sqe, this->wakefd, POLLIN);
                        ::io_uring_sqe_set_data(sqe, reinterpret_cast<void *>(kWakeUserData));
                    }
                    // A poke may mean an arm of the kernel timeout failed for lack of an SQE: re-evaluate the table.
                    if (!this->timers.empty()
                        && (!this->timerArmed || this->timers.begin()->first < this->timerArmedFor)) {
                        (void) this->armKernelTimerLocked(this->timers.begin()->first);
                    }
                    // Also pushes any SQE a failed submit left queued.
                    ::io_uring_submit(this->ring);
                }
                continue;
            }

            const std::uintptr_t bits = reinterpret_cast<std::uintptr_t>(data);
            if ((bits & 0x3) == kTimerTag) {
                const int res = cqe->res;
                ::io_uring_cqe_seen(this->ring, cqe);
                if (res == -ECANCELED) continue; // replaced by an earlier deadline: the new timeout serves the table
                due.clear();
                {
                    std::lock_guard<std::mutex> lk(this->sqMu);
                    const auto now = std::chrono::steady_clock::now();
                    while (!this->timers.empty() && this->timers.begin()->first <= now) {
                        due.push_back(this->timers.begin()->second);
                        this->timerIndex.erase(this->timers.begin()->second);
                        this->timers.erase(this->timers.begin());
                    }
                    if ((bits >> 2) == this->timerGen) {
                        this->timerArmed = false;
                        if (!this->timers.empty()) (void) this->armKernelTimerLocked(this->timers.begin()->first);
                    }
                }
                for (void *d : due) this->schedule(std::coroutine_handle<>::from_address(d));
                continue;
            }
            if ((bits & 0x3) == kRemovalTag) {
                // Completion of a removal request. -ENOENT means the target had already completed, so a drop mark left
                // for it would never be consumed: clear it so it cannot swallow a later wait on a reused address.
                const int res = cqe->res;
                ::io_uring_cqe_seen(this->ring, cqe);
                if (res == -ENOENT && (bits & ~static_cast<std::uintptr_t>(0x3)) != 0
                    && this->droppedPending.load(std::memory_order_acquire) > 0) {
                    void *key = reinterpret_cast<void *>(bits & ~static_cast<std::uintptr_t>(0x3));
                    std::lock_guard<std::mutex> lk(this->sqMu);
                    for (std::size_t i = 0; i < this->droppedHandles.size(); ++i) {
                        if (this->droppedHandles[i] == key) {
                            this->droppedHandles.erase(this->droppedHandles.begin() + static_cast<std::ptrdiff_t>(i));
                            this->droppedPending.fetch_sub(1, std::memory_order_release);
                            break;
                        }
                    }
                }
                continue;
            }

            ::io_uring_cqe_seen(this->ring, cqe);
            {
                std::lock_guard<std::mutex> lk(this->sqMu);
                this->pollFds.erase(data);
            }
            if (this->droppedPending.load(std::memory_order_acquire) > 0) {
                bool dropped = false;
                {
                    std::lock_guard<std::mutex> lk(this->sqMu);
                    for (std::size_t i = 0; i < this->droppedHandles.size(); ++i) {
                        if (this->droppedHandles[i] == data) {
                            this->droppedHandles.erase(this->droppedHandles.begin() + static_cast<std::ptrdiff_t>(i));
                            this->droppedPending.fetch_sub(1, std::memory_order_release);
                            dropped = true;
                            break;
                        }
                    }
                }
                if (dropped) continue;
            }
            this->schedule(std::coroutine_handle<>::from_address(data));
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

    namespace {
        /// data.ptr for the shared timerfd (1 is the wakeup eventfd; real handles are at least 8-aligned pointers).
        constexpr std::uintptr_t kTimerSentinel = 2;

        /// Longest sleep we arm; keeps now() + duration far from steady_clock overflow (about 100 years).
        constexpr long long kMaxTimerNs = 3'000'000'000'000'000'000LL;
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
        this->timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (this->timerfd >= 0) {
            ev.events = EPOLLIN; // level-triggered: the run loop drains it when it handles the expiry
            ev.data.ptr = reinterpret_cast<void *>(kTimerSentinel);
            if (::epoll_ctl(this->epfd, EPOLL_CTL_ADD, this->timerfd, &ev) < 0) {
                ::close(this->timerfd);
                this->timerfd = -1;
            }
        }
        if (this->timerfd < 0) {
            ::close(this->wakefd);
            ::close(this->epfd);
            throw std::runtime_error("Reactor: failed to create the timerfd");
        }
        this->thread = std::thread(&Reactor::run, this);
    }

    Reactor::~Reactor() {
        this->stop();
        if (this->thread.joinable()) this->thread.join();
        // The run loop has exited (joined above), so nothing else touches the timer table. Sleeps still armed are
        // dropped without resuming, as before: their frames belong to the Tasks awaiting them.
        {
            std::lock_guard<std::mutex> lk(this->timerSetMu);
            this->timerIndex.clear();
            this->timers.clear();
        }
        if (this->timerfd >= 0) ::close(this->timerfd);
        if (this->wakefd >= 0) ::close(this->wakefd);
        if (this->epfd >= 0) ::close(this->epfd);
    }

    void Reactor::stop() {
        this->running.store(false, std::memory_order_release);
        std::uint64_t v = 1;
        [[maybe_unused]] const auto rc = ::write(this->wakefd, &v, sizeof(v));
    }

    namespace {
        /// Low-bit tag for "this event is for an fd": data.u64 = fd << 2 | 3 (0 handle, 1 wakeup sentinel, 2 timer).
        constexpr std::uint64_t kFdTag = 0x3;
        inline std::uint64_t tagFd(int fd) { return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(fd)) << 2) | kFdTag; }
        inline bool isFdEvent(std::uint64_t u) { return (u & 0x3) == kFdTag; }
        inline int untagFd(std::uint64_t u) { return static_cast<int>(static_cast<std::uint32_t>(u >> 2)); }
    }

    bool Reactor::armFdLocked(int fd, const detail::FdWaiters &w) noexcept {
        struct epoll_event ev{};
        ev.events = EPOLLONESHOT | (!w.readers.empty() ? EPOLLIN : 0u) | (!w.writers.empty() ? EPOLLOUT : 0u);
        ev.data.u64 = tagFd(fd);
        if (::epoll_ctl(this->epfd, EPOLL_CTL_ADD, fd, &ev) == 0) return true;
        if (errno == EEXIST && ::epoll_ctl(this->epfd, EPOLL_CTL_MOD, fd, &ev) == 0) return true;
        return false;
    }

    void Reactor::registerFd(int fd, std::coroutine_handle<> h, bool writable) noexcept {
        std::vector<void *> woken;
        bool backoff = false; // epoll_ctl ADD ran out of memory or watches: retry shortly instead of spinning on EAGAIN
        {
            std::lock_guard<std::mutex> lk(this->fdMu);
            if (this->dead.load(std::memory_order_acquire)) {
                // The loop is gone (the flag is set under this lock by abandonWaiters): resume now, nothing would fire.
                woken.push_back(h.address());
            } else {
                detail::FdWaiters &w = this->fdWaiters[fd];
                // A fresh kernel registration (ADD succeeds) means the fd is new (a closed fd's registration is gone with it), so
                // any waiter still recorded belongs to a closed fd: it can never be woken by the kernel, hand it to the pool so
                // its next syscall reports the error instead of leaving it parked.
                struct epoll_event ev{};
                ev.events = EPOLLONESHOT | (writable ? EPOLLOUT : EPOLLIN);
                ev.data.u64 = tagFd(fd);
                bool ok = false;
                bool queued = false; // h is already in the waiter table (and so will be collected by takeAll)
                if (::epoll_ctl(this->epfd, EPOLL_CTL_ADD, fd, &ev) == 0) {
                    w.readers.takeAll(woken);
                    w.writers.takeAll(woken);
                    (writable ? w.writers : w.readers).push(h.address());
                    ok = true;
                } else if (errno == EEXIST) {
                    (writable ? w.writers : w.readers).push(h.address());
                    queued = true;
                    ev.events = EPOLLONESHOT | (!w.readers.empty() ? EPOLLIN : 0u) | (!w.writers.empty() ? EPOLLOUT : 0u);
                    ok = ::epoll_ctl(this->epfd, EPOLL_CTL_MOD, fd, &ev) == 0;
                } else {
                    backoff = errno == ENOMEM || errno == ENOSPC;
                }
                if (!ok) {
                    w.readers.takeAll(woken);
                    w.writers.takeAll(woken);
                    if (!queued && !backoff) woken.push_back(h.address());
                    this->fdWaiters.erase(fd);
                }
            }
        }
        for (void *p : woken) this->schedule(std::coroutine_handle<>::from_address(p));
        if (backoff) this->registerTimer(std::chrono::milliseconds{1}, h);
    }

    bool Reactor::cancelWait(SocketHandle fd, std::coroutine_handle<> h, bool resume) noexcept {
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(this->fdMu);
            auto it = this->fdWaiters.find(fd);
            if (it == this->fdWaiters.end()) return false;
            detail::FdWaiters &w = it->second;
            found = w.readers.remove(h.address()) || w.writers.remove(h.address());
            // An armed one-shot event for a direction nobody waits on any more is harmless: the run loop ignores it and
            // re-arms whatever is still waiting.
            if (w.empty()) this->fdWaiters.erase(it);
        }
        if (found && resume) this->schedule(h);
        return found;
    }

    std::size_t Reactor::cancelFd(SocketHandle fd) noexcept {
        std::vector<void *> woken;
        {
            std::lock_guard<std::mutex> lk(this->fdMu);
            auto it = this->fdWaiters.find(fd);
            if (it == this->fdWaiters.end()) return 0;
            it->second.readers.takeAll(woken);
            it->second.writers.takeAll(woken);
            this->fdWaiters.erase(it);
            (void) ::epoll_ctl(this->epfd, EPOLL_CTL_DEL, fd, nullptr);
        }
        for (void *p : woken) this->schedule(std::coroutine_handle<>::from_address(p));
        return woken.size();
    }

    void Reactor::registerReadable(SocketHandle fd, std::coroutine_handle<> h) noexcept {
        this->registerFd(fd, h, /*writable=*/false);
    }

    void Reactor::registerWritable(SocketHandle fd, std::coroutine_handle<> h) noexcept {
        this->registerFd(fd, h, /*writable=*/true);
    }

    void Reactor::armTimerfdLocked(std::chrono::steady_clock::time_point deadline) noexcept {
        struct itimerspec ts{};
        auto rel = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (rel < 1) rel = 1; // an all-zero it_value would disarm instead of firing at once
        // A 32-bit time_t cannot hold the far end of the timer range; timerfd_settime would reject it forever.
        long long secs = rel / 1'000'000'000LL;
        if constexpr (sizeof(time_t) < sizeof(long long)) {
            secs = std::min<long long>(secs, std::numeric_limits<time_t>::max());
        }
        ts.it_value.tv_sec = static_cast<time_t>(secs);
        ts.it_value.tv_nsec = static_cast<long>(rel % 1'000'000'000LL);
        if (::timerfd_settime(this->timerfd, 0, &ts, nullptr) == 0) {
            this->timerArmed = true;
            this->timerArmedFor = deadline;
        } else {
            // Cannot happen for a valid fd and value; if it does, wake the loop so it re-evaluates instead of
            // sitting on a deadline nobody is waiting for.
            this->timerArmed = false;
            const std::uint64_t one = 1;
            [[maybe_unused]] const auto rc = ::write(this->wakefd, &one, sizeof(one));
        }
    }

    void Reactor::registerTimer(std::chrono::nanoseconds duration,
                                 std::coroutine_handle<> h) noexcept {
        // One timerfd serves every sleep: the deadline goes into an ordered table and the fd is armed for the earliest.
        const long long ns = duration.count() < 0 ? 0 : (duration.count() > kMaxTimerNs ? kMaxTimerNs : duration.count());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
        bool armed = false;
        try {
            // Insert and arm under one hold of timerSetMu: a concurrent cancelTimer (the sleep's stop callback can fire
            // as soon as it is armed, before this call returns) either runs before the entry exists (and finds nothing)
            // or after it is fully registered, so it never sees a half-built entry.
            std::lock_guard<std::mutex> lk(this->timerSetMu);
            if (!this->dead.load(std::memory_order_acquire) && this->timerIndex.find(h.address()) == this->timerIndex.end()) {
                auto it = this->timers.emplace(deadline, h.address());
                try {
                    this->timerIndex.emplace(h.address(), it);
                    armed = true;
                } catch (...) {
                    this->timers.erase(it);
                    throw;
                }
                if (it == this->timers.begin() && (!this->timerArmed || deadline < this->timerArmedFor)) {
                    this->armTimerfdLocked(deadline);
                }
            }
        } catch (...) {
            armed = false;
        }
        // Not tracked (loop dead, duplicate handle, out of memory): wake it now rather than strand it.
        if (!armed) this->schedule(h);
    }

    bool Reactor::cancelTimer(std::coroutine_handle<> h, bool resume) noexcept {
        bool found = false;
        {
            // Whoever erases the index entry (this, the run loop on expiry, or abandonWaiters) owns the single resume.
            // The timerfd is left alone: if this was the earliest, the loop wakes once for nothing and re-arms.
            std::lock_guard<std::mutex> lk(this->timerSetMu);
            auto it = this->timerIndex.find(h.address());
            if (it != this->timerIndex.end()) {
                this->timers.erase(it->second);
                this->timerIndex.erase(it);
                found = true;
            }
        }
        if (found && resume) this->schedule(h);
        return found;
    }

    void Reactor::run() {
        constexpr int kBatch = 64;
        struct epoll_event events[kBatch];
        std::vector<void *> ready;
        std::vector<void *> due;

        while (this->running.load(std::memory_order_acquire)) {
            int n = ::epoll_wait(this->epfd, events, kBatch, -1);
            if (n < 0) {
                if (errno == EINTR) continue;
                this->abandonWaiters("epoll_wait", errno);
                break;
            }
            for (int i = 0; i < n; ++i) {
                void *p = events[i].data.ptr;
                if (reinterpret_cast<std::uintptr_t>(p) == kWakeupSentinel) {
                    std::uint64_t buf;
                    [[maybe_unused]] const auto rc = ::read(this->wakefd, &buf, sizeof(buf));
                    {
                        // A poke may mean arming the timerfd failed: make sure it is armed for the earliest deadline.
                        std::lock_guard<std::mutex> lk(this->timerSetMu);
                        if (!this->timers.empty() && (!this->timerArmed || this->timers.begin()->first < this->timerArmedFor)) {
                            this->armTimerfdLocked(this->timers.begin()->first);
                        }
                    }
                    continue;
                }
                if (reinterpret_cast<std::uintptr_t>(p) == kTimerSentinel) {
                    std::uint64_t expirations;
                    [[maybe_unused]] const auto rc = ::read(this->timerfd, &expirations, sizeof(expirations));
                    due.clear();
                    {
                        std::lock_guard<std::mutex> lk(this->timerSetMu);
                        const auto now = std::chrono::steady_clock::now();
                        while (!this->timers.empty() && this->timers.begin()->first <= now) {
                            due.push_back(this->timers.begin()->second);
                            this->timerIndex.erase(this->timers.begin()->second);
                            this->timers.erase(this->timers.begin());
                        }
                        this->timerArmed = false;
                        if (!this->timers.empty()) this->armTimerfdLocked(this->timers.begin()->first);
                    }
                    for (void *d : due) this->schedule(std::coroutine_handle<>::from_address(d));
                    continue;
                }
                if (isFdEvent(events[i].data.u64)) {
                    const int fd = untagFd(events[i].data.u64);
                    const std::uint32_t got = events[i].events;
                    ready.clear();
                    {
                        std::lock_guard<std::mutex> lk(this->fdMu);
                        auto it = this->fdWaiters.find(fd);
                        if (it == this->fdWaiters.end()) continue;
                        detail::FdWaiters &w = it->second;
                        // An error or hang-up is delivered to both directions so each side observes it on its next syscall.
                        const bool err = (got & (EPOLLERR | EPOLLHUP)) != 0;
                        if (!w.readers.empty() && (err || (got & EPOLLIN))) w.readers.takeAll(ready);
                        if (!w.writers.empty() && (err || (got & EPOLLOUT))) w.writers.takeAll(ready);
                        if (w.empty()) {
                            this->fdWaiters.erase(it);
                        } else if (!this->armFdLocked(fd, w)) {
                            // The other direction cannot be re-armed (fd closed meanwhile): let it run and fail on its own.
                            w.readers.takeAll(ready);
                            w.writers.takeAll(ready);
                            this->fdWaiters.erase(it);
                        }
                    }
                    for (void *r : ready) this->schedule(std::coroutine_handle<>::from_address(r));
                    continue;
                }
                auto h = std::coroutine_handle<>::from_address(p);
                this->schedule(h);
            }
        }
    }

    void Reactor::abandonWaiters(const char *what, int err) noexcept {
        std::fprintf(stderr, "Reactor: %s failed (%s); loop exiting, waking parked waiters\n", what, std::strerror(err));
        std::vector<void *> woken;
        {
            std::lock_guard<std::mutex> lk(this->fdMu);
            this->dead.store(true, std::memory_order_release);
            for (auto &kv : this->fdWaiters) {
                kv.second.readers.takeAll(woken);
                kv.second.writers.takeAll(woken);
            }
            this->fdWaiters.clear();
        }
        {
            // The loop that would fire these is gone: resume the sleepers too rather than strand them.
            std::lock_guard<std::mutex> lk(this->timerSetMu);
            this->dead.store(true, std::memory_order_release); // also under this lock: see registerTimer
            for (auto &kv : this->timers) woken.push_back(kv.second);
            this->timers.clear();
            this->timerIndex.clear();
        }
        for (void *p : woken) this->schedule(std::coroutine_handle<>::from_address(p));
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
            std::uint64_t seq; ///< registration order; a CancelRequest only reaches registrations made before it
        };

        /**
         * @brief One pending timer entry. Min-heap is keyed on @c deadline.
         */
        struct TimerEntry {
            std::chrono::steady_clock::time_point deadline;
            std::coroutine_handle<> handle;
            std::uint64_t id;
        };

        /**
         * @brief A cancelWait / cancelFd request for a registration that may
         *        be sitting in the poll thread's private snapshot (so the
         *        caller cannot remove it from @c pending). @c handle is null
         *        for "every registration on @c sock".
         */
        struct CancelRequest {
            std::coroutine_handle<> handle;
            SOCKET sock;
            bool resume;
            std::uint64_t upTo; ///< only registrations with seq <= upTo: a later one on the same handle address is new
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

        /// Cancellations the poll thread has yet to apply to its snapshot.
        std::vector<CancelRequest> cancels;

        /// Armed timers by handle address -> id of the live heap entry. An
        /// entry whose id no longer matches was cancelled and is skipped.
        std::unordered_map<void *, std::uint64_t> liveTimers;
        std::uint64_t nextTimerId = 0;

        /// Source of PollRegistration::seq. Guarded by pollMu.
        std::uint64_t nextRegSeq = 0;

        /// The readiness thread gave up: register* resume their waiter at once. Guarded by pollMu.
        bool dead = false;

        /// Mutex guarding @c pending, @c timers, @c cancels, @c liveTimers.
        std::mutex pollMu;
    };

    namespace {
        /**
         * @brief Rebuild the timer heap without the entries cancelTimer or a re-arm left behind, once they outnumber
         *        the live ones, so a long-lived deadline that is cancelled over and over does not pile up. pollMu held.
         */
        void compactTimersLocked(WindowsState &s) {
            constexpr std::size_t kMinHeap = 64;
            if (s.timers.size() < kMinHeap || s.timers.size() <= 2 * s.liveTimers.size()) return;
            std::vector<TimerEntry> keep;
            keep.reserve(s.liveTimers.size());
            while (!s.timers.empty()) {
                const TimerEntry &top = s.timers.top();
                auto live = s.liveTimers.find(top.handle.address());
                if (live != s.liveTimers.end() && live->second == top.id) keep.push_back(top);
                s.timers.pop();
            }
            s.timers = std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerEntryGreater>(
                TimerEntryGreater{}, std::move(keep));
        }

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

    namespace {
        /**
         * @brief The Winsock code for the Win32 error an overlapped socket operation completed with, so errorCode()
         *        reads the same on this path as on the readiness path (WSAECONNRESET, not ERROR_NETNAME_DELETED).
         *        Codes without a Winsock counterpart are returned unchanged.
         */
        int toWinsockError(DWORD e) noexcept {
            switch (e) {
                case ERROR_NETNAME_DELETED: return WSAECONNRESET;
                case ERROR_PORT_UNREACHABLE: return WSAECONNRESET;
                case ERROR_CONNECTION_ABORTED: return WSAECONNABORTED;
                case ERROR_CONNECTION_REFUSED: return WSAECONNREFUSED;
                case ERROR_NETWORK_UNREACHABLE: return WSAENETUNREACH;
                case ERROR_HOST_UNREACHABLE: return WSAEHOSTUNREACH;
                case ERROR_SEM_TIMEOUT: return WSAETIMEDOUT;
                case ERROR_MORE_DATA: return WSAEMSGSIZE;
                default: return static_cast<int>(e);
            }
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
                if (!ok && ov == nullptr) {
                    // The dequeue itself failed (the port is unusable), not an I/O that completed with an error: retrying
                    // would spin on the same error forever.
                    std::fprintf(stderr, "Reactor: GetQueuedCompletionStatus failed (error %lu); IOCP thread exiting\n",
                                 static_cast<unsigned long>(::GetLastError()));
                    break;
                }
                if (ov == nullptr) {
                    // Spurious wake (timeout / posted with no overlapped
                    // and a non-stop key); harmless, loop back.
                    continue;
                }
                auto *c = static_cast<OverlappedCompletion *>(ov);
                c->bytes = bytes;
                c->err = ok ? 0 : toWinsockError(::GetLastError());
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

    void Reactor::registerReadable(SocketHandle fd, std::coroutine_handle<> h) noexcept {
        if (!this->winstate) {
            this->schedule(h);
            return;
        }
        bool resumeNow = false;
        {
            std::lock_guard<std::mutex> lk(this->winstate->pollMu);
            if (this->winstate->dead) {
                // The readiness thread is gone; resume now (below, off the lock) so the syscall reports the failure.
                resumeNow = true;
            } else {
                this->winstate->pending.push_back(PollRegistration{
                    static_cast<SOCKET>(fd),
                    static_cast<short>(POLLRDNORM),
                    h,
                    ++this->winstate->nextRegSeq
                });
            }
        }
        if (resumeNow) {
            this->schedule(h);
            return;
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

    void Reactor::registerWritable(SocketHandle fd, std::coroutine_handle<> h) noexcept {
        if (!this->winstate) {
            this->schedule(h);
            return;
        }
        bool resumeNow = false;
        {
            std::lock_guard<std::mutex> lk(this->winstate->pollMu);
            if (this->winstate->dead) {
                // The readiness thread is gone; resume now (below, off the lock) so the syscall reports the failure.
                resumeNow = true;
            } else {
                this->winstate->pending.push_back(PollRegistration{
                    static_cast<SOCKET>(fd),
                    static_cast<short>(POLLWRNORM),
                    h,
                    ++this->winstate->nextRegSeq
                });
            }
        }
        if (resumeNow) {
            this->schedule(h);
            return;
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
        // Clamp like the other backends: now() + nanoseconds::max() would overflow the clock (about 100 years is plenty).
        constexpr long long kMaxTimerNs = 3'000'000'000'000'000'000LL;
        const long long ns = duration.count() < 0 ? 0 : (duration.count() > kMaxTimerNs ? kMaxTimerNs : duration.count());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
        bool resumeNow = false;
        {
            std::lock_guard<std::mutex> lk(this->winstate->pollMu);
            // Not tracked (loop dead, handle already armed, out of memory): wake it now rather than strand it or
            // overwrite the live registration of the same handle.
            if (this->winstate->dead || this->winstate->liveTimers.contains(h.address())) {
                resumeNow = true;
            } else {
                try {
                    const std::uint64_t id = ++this->winstate->nextTimerId;
                    this->winstate->liveTimers.emplace(h.address(), id);
                    try {
                        this->winstate->timers.push(TimerEntry{deadline, h, id});
                    } catch (...) {
                        this->winstate->liveTimers.erase(h.address());
                        throw;
                    }
                } catch (...) {
                    resumeNow = true;
                }
                if (!resumeNow) {
                    try {
                        compactTimersLocked(*this->winstate); // only housekeeping: skipping it is harmless
                    } catch (...) {
                    }
                }
            }
        }
        if (resumeNow) {
            this->schedule(h);
            return;
        }
        const char b = kWakeupByte;
        (void) ::sendto(this->winstate->wakeSock, &b, 1, 0,
                        reinterpret_cast<sockaddr *>(&this->winstate->wakeAddr),
                        sizeof(this->winstate->wakeAddr));
    }

    bool Reactor::cancelWait(SocketHandle fd, std::coroutine_handle<> h, bool resume) noexcept {
        if (!this->winstate) return false;
        const SOCKET sock = static_cast<SOCKET>(fd);
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(this->winstate->pollMu);
            auto &pending = this->winstate->pending;
            for (auto it = pending.begin(); it != pending.end(); ++it) {
                if (it->handle == h) {
                    pending.erase(it);
                    found = true;
                    break;
                }
            }
            if (!found) {
                // Possibly in the poll thread's snapshot right now: let it apply the cancellation.
                this->winstate->cancels.push_back(CancelRequest{h, sock, resume, this->winstate->nextRegSeq});
            }
        }
        if (found && resume) {
            this->schedule(h);
        } else if (!found) {
            const char b = kWakeupByte;
            (void) ::sendto(this->winstate->wakeSock, &b, 1, 0,
                            reinterpret_cast<sockaddr *>(&this->winstate->wakeAddr),
                            sizeof(this->winstate->wakeAddr));
        }
        // Not found: the poll thread's snapshot holds it and applies the queued request (or its own readiness event)
        // exactly once; this call took nothing, as on the other backends.
        return found;
    }

    bool Reactor::cancelTimer(std::coroutine_handle<> h, bool resume) noexcept {
        if (!this->winstate) return false;
        {
            std::lock_guard<std::mutex> lk(this->winstate->pollMu);
            if (this->winstate->liveTimers.erase(h.address()) == 0) return false;
            try {
                compactTimersLocked(*this->winstate); // only housekeeping: skipping it is harmless
            } catch (...) {
            }
        }
        if (resume) this->schedule(h);
        return true;
    }

    std::size_t Reactor::cancelFd(SocketHandle fd) noexcept {
        if (!this->winstate) return 0;
        const SOCKET sock = static_cast<SOCKET>(fd);
        std::vector<std::coroutine_handle<>> woken;
        {
            std::lock_guard<std::mutex> lk(this->winstate->pollMu);
            auto &pending = this->winstate->pending;
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->sock == sock) {
                    woken.push_back(it->handle);
                    it = pending.erase(it);
                } else {
                    ++it;
                }
            }
            this->winstate->cancels.push_back(CancelRequest{std::coroutine_handle<>{}, sock, true, this->winstate->nextRegSeq});
        }
        const char b = kWakeupByte;
        (void) ::sendto(this->winstate->wakeSock, &b, 1, 0,
                        reinterpret_cast<sockaddr *>(&this->winstate->wakeAddr),
                        sizeof(this->winstate->wakeAddr));
        for (auto h : woken) this->schedule(h);
        return woken.size();
    }

    bool Reactor::associateIocp(SocketHandle fd) noexcept {
        if (!this->winstate || !this->winstate->iocp) return false;
        HANDLE h = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(fd));
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
        constexpr int kMaxPollFailures = 100;
        int pollFailures = 0;

        while (this->running.load(std::memory_order_acquire)) {
            // -- snapshot under the lock -----------------------------
            int timeoutMs = kPollMaxTimeoutMs;
            fds.clear();
            readyHandles.clear();
            std::vector<PollRegistration> active;
            std::vector<std::coroutine_handle<>> expiredTimers;
            std::vector<CancelRequest> cancelReqs;
            {
                std::lock_guard<std::mutex> lk(this->winstate->pollMu);
                active = std::move(this->winstate->pending);
                this->winstate->pending.clear();
                cancelReqs = std::move(this->winstate->cancels);
                this->winstate->cancels.clear();

                // Drain expired timers while we hold the lock.
                const auto now = std::chrono::steady_clock::now();
                while (!this->winstate->timers.empty()
                       && this->winstate->timers.top().deadline <= now) {
                    const TimerEntry top = this->winstate->timers.top();
                    this->winstate->timers.pop();
                    auto live = this->winstate->liveTimers.find(top.handle.address());
                    // A missing / re-armed entry was cancelled; its handle has been dealt with already.
                    if (live != this->winstate->liveTimers.end() && live->second == top.id) {
                        this->winstate->liveTimers.erase(live);
                        expiredTimers.push_back(top.handle);
                    }
                }

                if (!this->winstate->timers.empty()) {
                    const auto next = this->winstate->timers.top().deadline;
                    const auto delta_ns =
                        std::chrono::duration_cast<std::chrono::milliseconds>(next - now);
                    long long ms = delta_ns.count() + 1; // truncation would wake early and spin up to 1 ms
                    if (ms < 0) ms = 0;
                    if (ms > kPollMaxTimeoutMs) ms = kPollMaxTimeoutMs;
                    timeoutMs = static_cast<int>(ms);
                }
            }

            // Schedule expired timers off-lock to avoid holding the
            // pollMu across Yarn::run dispatches.
            for (auto h : expiredTimers) this->schedule(h);

            // Apply cancellations to the registrations this thread now owns.
            for (const auto &c : cancelReqs) {
                for (std::size_t i = 0; i < active.size();) {
                    const bool match = active[i].seq <= c.upTo
                                       && (c.handle ? active[i].handle == c.handle : active[i].sock == c.sock);
                    if (!match) {
                        ++i;
                        continue;
                    }
                    if (c.resume) this->schedule(active[i].handle);
                    active.erase(active.begin() + static_cast<std::ptrdiff_t>(i));
                    if (c.handle) break;
                }
            }

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
                // A persistent failure must not busy-spin: back off, and after
                // a long run of failures give up and resume the parked waiters
                // so they observe the error on their next syscall.
                const int werr = ::WSAGetLastError();
                if (++pollFailures >= kMaxPollFailures) {
                    std::fprintf(stderr, "Reactor: WSAPoll keeps failing (error %d); readiness thread exiting\n", werr);
                    for (auto &reg : active) this->schedule(reg.handle);
                    std::vector<PollRegistration> orphaned;
                    std::vector<std::coroutine_handle<>> sleepers;
                    {
                        std::lock_guard<std::mutex> lk(this->winstate->pollMu);
                        this->winstate->dead = true;
                        orphaned = std::move(this->winstate->pending);
                        this->winstate->pending.clear();
                        // Nothing will fire the timers any more: resume their sleepers too (live entries only; a
                        // cancelled one was already dealt with by its canceller).
                        while (!this->winstate->timers.empty()) {
                            const TimerEntry top = this->winstate->timers.top();
                            this->winstate->timers.pop();
                            auto live = this->winstate->liveTimers.find(top.handle.address());
                            if (live != this->winstate->liveTimers.end() && live->second == top.id) {
                                this->winstate->liveTimers.erase(live);
                                sleepers.push_back(top.handle);
                            }
                        }
                    }
                    this->dead.store(true, std::memory_order_release);
                    for (auto &reg : orphaned) this->schedule(reg.handle);
                    for (auto h : sleepers) this->schedule(h);
                    break;
                }
                if (!active.empty()) {
                    std::lock_guard<std::mutex> lk(this->winstate->pollMu);
                    for (auto &reg : active) {
                        this->winstate->pending.push_back(reg);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            pollFailures = 0;

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
        // retire() joins only this thread (the destructor, which also joins the IOCP completion thread, never runs for
        // the immortal Reactor). On a normal stop take the completion thread down here, so no completion can still be
        // scheduling onto Yarn once the process starts destroying it.
        if (!this->running.load(std::memory_order_acquire) && this->winstate->iocpThread.joinable()) {
            this->winstate->iocpThread.join();
        }
    }

#else // YARN_REACTOR_STUB

    Reactor::Reactor() {
        throw std::runtime_error("Reactor: no backend on this platform yet");
    }

    Reactor::~Reactor() = default;

    void Reactor::stop() {}
    void Reactor::registerReadable(SocketHandle, std::coroutine_handle<>) noexcept {}
    void Reactor::registerWritable(SocketHandle, std::coroutine_handle<>) noexcept {}
    void Reactor::registerTimer(std::chrono::nanoseconds,
                                 std::coroutine_handle<>) noexcept {}
    bool Reactor::cancelWait(SocketHandle, std::coroutine_handle<>, bool) noexcept { return false; }
    bool Reactor::cancelTimer(std::coroutine_handle<>, bool) noexcept { return false; }
    std::size_t Reactor::cancelFd(SocketHandle) noexcept { return 0; }
    void Reactor::run() {}

#endif

}
