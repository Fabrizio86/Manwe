//
// Created by Fabrizio Paino on 2026-05-16.
//

#include "SignalSet.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#if !defined(MANWE_SIGNALSET_FORCE_PIPE) && (defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
#include <sys/event.h>
#include <sys/time.h>
#define MANWE_SIGNALSET_KQUEUE 1
#endif
#endif

#include "IoAwaiters.h"

namespace YarnBall {

#if !defined(_WIN32)

    /**
     * @brief State shared between a SignalSet and the @c next() tasks it handed out. It owns the descriptor, so the
     *        number cannot be recycled while a task is still parked on it; @c closed is checked before every syscall.
     */
    struct SignalSet::State {
        int readFd = -1;
        std::atomic<bool> closed{false};

        State() = default;
        State(const State &) = delete;
        State &operator=(const State &) = delete;

        ~State() {
            if (this->readFd >= 0) ::close(this->readFd);
        }
    };

    namespace {
        /**
         * @brief Process-wide bookkeeping for signal dispositions, shared by every live SignalSet. The first set to
         *        capture a signal replaces its disposition and remembers the original; the last one to release it
         *        restores the original.
         */
        struct Disposition {
            int refs = 0;
            struct sigaction original{};
        };

        std::mutex g_dispMu;
        std::map<int, Disposition> g_disp;

#if !defined(MANWE_SIGNALSET_KQUEUE)
        /**
         * @brief One live set's entry in the handler's fan-out table. @c fd holds the write end of its self-pipe plus
         *        one (0 = free slot, so the zero-initialised table is valid before any dynamic initialisation) and
         *        @c mask the signals it subscribed to (bit sig-1). Loaded from the handler (async-signal-safe: plain
         *        atomic loads), written under g_dispMu.
         */
        struct Slot {
            std::atomic<int> fd{0};
            std::atomic<int> pid{0}; ///< creating process: a fork() child inherits the pipe and the handler, but is not us
            std::atomic<std::uint64_t> mask{0};
        };

        constexpr int kMaxSets = 64;
        constexpr int kMaxSignal = 64; // the mask is 64 bits wide
        Slot g_slots[kMaxSets];

        /// Handlers currently between their first and last access of the table; teardown waits for it to drain.
        std::atomic<int> g_handlersInFlight{0};

        /**
         * @brief @c sigaction body installed for every captured signal. Only @c write(2) is allowed in
         *        async-signal-safe code; we emit one byte holding the signal number to each live set that subscribed
         *        to it.
         */
        extern "C" void manwe_signal_handler(int sig) {
            const int savedErrno = errno;
            // Counted before the table is read, so a teardown that unpublished a slot and then saw the counter at zero
            // knows no handler can still write to that slot's descriptor (sequentially consistent on both sides).
            g_handlersInFlight.fetch_add(1, std::memory_order_seq_cst);
            const unsigned char b = static_cast<unsigned char>(sig);
            const int self = static_cast<int>(::getpid()); // async-signal-safe
            const std::uint64_t bit = (sig >= 1 && sig <= kMaxSignal) ? (std::uint64_t{1} << (sig - 1)) : 0;
            for (auto &slot : g_slots) {
                const int fd1 = slot.fd.load(std::memory_order_seq_cst);
                // Ignore partial / EAGAIN: losing one notification under extreme pressure is acceptable.
                if (fd1 != 0 && (slot.mask.load(std::memory_order_relaxed) & bit) != 0
                    && slot.pid.load(std::memory_order_relaxed) == self) (void) ::write(fd1 - 1, &b, 1);
            }
            g_handlersInFlight.fetch_sub(1, std::memory_order_seq_cst);
            errno = savedErrno;
        }
#else
        /// kqueue builds: EVFILT_SIGNAL reports the signal whatever the handler does; installing one (rather than
        /// SIG_IGN) keeps the default action away without making SIGCHLD auto-reap or surviving exec.
        extern "C" void manwe_signal_noop(int) {}
#endif

        /**
         * @brief Take a reference on @p sig, replacing its disposition on the first one. kqueue builds install an
         *        empty handler (EVFILT_SIGNAL still reports the signal, and the default action cannot kill the
         *        process); the others install the self-pipe handler.
         * @return 0, or an errno value if sigaction failed.
         */
        int acquireSignal(int sig) {
            std::lock_guard<std::mutex> lk(g_dispMu);
            auto it = g_disp.find(sig);
            if (it != g_disp.end()) {
                ++it->second.refs;
                return 0;
            }
            struct sigaction act{};
#if defined(MANWE_SIGNALSET_KQUEUE)
            act.sa_handler = &manwe_signal_noop;
#else
            act.sa_handler = &manwe_signal_handler;
#endif
            // SA_RESTART so syscalls in user code don't get cut short by our handler returning EINTR.
            act.sa_flags = SA_RESTART;
            sigemptyset(&act.sa_mask);
            Disposition d;
            if (::sigaction(sig, &act, &d.original) < 0) return errno;
            d.refs = 1;
            g_disp.emplace(sig, d);
            return 0;
        }

        void releaseSignal(int sig) noexcept {
            std::lock_guard<std::mutex> lk(g_dispMu);
            auto it = g_disp.find(sig);
            if (it == g_disp.end()) return;
            if (--it->second.refs > 0) return;
            (void) ::sigaction(sig, &it->second.original, nullptr);
            g_disp.erase(it);
        }

        void markNonBlockingCloexec(int fd, bool nonBlocking) {
            int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags < 0 || (nonBlocking && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
                || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
                throw SignalCaptureError(std::string("fcntl: ") + std::strerror(errno));
            }
        }
    }

    void SignalSet::teardown() noexcept {
        // Reverse order so a signal listed twice ends up with its true original handler rather than ours.
        for (std::size_t i = this->installedSignals.size(); i-- > 0;) releaseSignal(this->installedSignals[i]);
        this->installedSignals.clear();
#if !defined(MANWE_SIGNALSET_KQUEUE)
        if (this->fanSlot >= 0) {
            // Unpublish the write end BEFORE closing it so a handler does not write to a recycled descriptor.
            {
                std::lock_guard<std::mutex> lk(g_dispMu);
                g_slots[this->fanSlot].fd.store(0, std::memory_order_seq_cst);
                g_slots[this->fanSlot].mask.store(0, std::memory_order_relaxed);
                this->fanSlot = -1;
            }
            // A handler that read the slot before it was unpublished may still be about to write: wait it out.
            while (g_handlersInFlight.load(std::memory_order_seq_cst) != 0) std::this_thread::yield();
        }
        if (this->writeFd >= 0) {
            // Closing the write end also puts the read end at EOF, which wakes a next() that is about to park.
            ::close(this->writeFd);
            this->writeFd = -1;
        }
#endif
        if (this->state) {
            this->state->closed.store(true, std::memory_order_seq_cst);
#if defined(MANWE_SIGNALSET_KQUEUE)
            if (this->state->readFd >= 0) {
                // A user event that stays triggered keeps the kqueue readable, so a next() that has not parked yet
                // wakes at once instead of missing the cancelFd below.
                struct kevent change;
                EV_SET(&change, 1, EVFILT_USER, EV_ADD, 0, 0, nullptr);
                (void) ::kevent(this->state->readFd, &change, 1, nullptr, 0, nullptr);
                EV_SET(&change, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
                (void) ::kevent(this->state->readFd, &change, 1, nullptr, 0, nullptr);
            }
#endif
            // A next() parked on the descriptor would otherwise stay parked forever: wake it so it observes the
            // closed set and throws. The descriptor itself is closed by ~State once the last such task is gone.
            if (this->state->readFd >= 0) Reactor::instance()->cancelFd(this->state->readFd);
            this->state.reset();
        }
    }

    SignalSet::SignalSet(std::initializer_list<int> signals) {
        try {
            this->installedSignals.reserve(signals.size());
            this->state = std::make_shared<State>();
#if defined(MANWE_SIGNALSET_KQUEUE)
            // One kqueue per set: EVFILT_SIGNAL is per-kqueue, so every set sees every signal it asked for. A kqueue
            // descriptor is itself pollable, which is how the Reactor waits on it.
            this->state->readFd = ::kqueue();
            if (this->state->readFd < 0) throw SignalCaptureError(std::string("kqueue: ") + std::strerror(errno));
            markNonBlockingCloexec(this->state->readFd, false);
#else
            std::uint64_t mask = 0;
            for (int sig : signals) {
                if (sig < 1 || sig > kMaxSignal) {
                    throw SignalCaptureError(std::string("sigaction(") + std::to_string(sig) + "): invalid signal number");
                }
                mask |= std::uint64_t{1} << (sig - 1);
            }
            int fds[2];
#if defined(__linux__)
            // Atomically close-on-exec and non-blocking: no window in which a concurrent fork+exec inherits the pipe.
            // Non-blocking so the reader can poll cleanly and the handler's write cannot stall when the pipe fills.
            if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) throw SignalCaptureError(std::string("pipe2: ") + std::strerror(errno));
            this->state->readFd = fds[0];
            this->writeFd = fds[1];
#else
            if (::pipe(fds) < 0) throw SignalCaptureError(std::string("pipe: ") + std::strerror(errno));
            this->state->readFd = fds[0];
            this->writeFd = fds[1];
            markNonBlockingCloexec(this->state->readFd, true);
            markNonBlockingCloexec(this->writeFd, true);
#endif
            {
                std::lock_guard<std::mutex> lk(g_dispMu);
                for (int i = 0; i < kMaxSets && this->fanSlot < 0; ++i) {
                    if (g_slots[i].fd.load(std::memory_order_relaxed) == 0) this->fanSlot = i;
                }
                if (this->fanSlot < 0) throw SignalCaptureError("too many active SignalSets");
                // Mask first: once the fd is visible the handler must already know what this set subscribed to.
                g_slots[this->fanSlot].mask.store(mask, std::memory_order_relaxed);
                g_slots[this->fanSlot].pid.store(static_cast<int>(::getpid()), std::memory_order_relaxed);
                g_slots[this->fanSlot].fd.store(this->writeFd + 1, std::memory_order_seq_cst);
            }
#endif
            for (int sig : signals) {
                if (const int e = acquireSignal(sig)) {
                    throw SignalCaptureError(std::string("sigaction(") + std::to_string(sig) + "): " + std::strerror(e));
                }
                this->installedSignals.push_back(sig);
#if defined(MANWE_SIGNALSET_KQUEUE)
                struct kevent change;
                EV_SET(&change, static_cast<uintptr_t>(sig), EVFILT_SIGNAL, EV_ADD | EV_CLEAR, 0, 0, nullptr);
                if (::kevent(this->state->readFd, &change, 1, nullptr, 0, nullptr) < 0) {
                    throw SignalCaptureError(std::string("kevent(EVFILT_SIGNAL ") + std::to_string(sig) + "): "
                                             + std::strerror(errno));
                }
#endif
            }
        } catch (...) {
            this->teardown();
            throw;
        }
    }

    SignalSet::~SignalSet() {
        this->teardown();
    }

    YarnBall::Task<int> SignalSet::next() {
        // Not a coroutine itself: the state is captured here, at call time, rather than when the lazy task first runs
        // (by which time the set may be gone).
        return SignalSet::wait(this->state);
    }

    YarnBall::Task<int> SignalSet::wait(std::shared_ptr<State> st) {
        // The shared state owns the descriptor, so it stays valid (and unrecycled) for as long as this frame lives.
        const int fd = st->readFd;
        while (true) {
            if (st->closed.load(std::memory_order_seq_cst)) throw SignalCaptureError("signal set closed");
#if defined(MANWE_SIGNALSET_KQUEUE)
            struct kevent ev;
            const struct timespec none{0, 0};
            const int n = ::kevent(fd, nullptr, 0, &ev, 1, &none);
            if (n == 1) {
                if (ev.filter == EVFILT_USER) throw SignalCaptureError("signal set closed");
                co_return static_cast<int>(ev.ident);
            }
            if (n == 0) {
                co_await YarnBall::io::waitReadable(fd);
                continue;
            }
            if (errno == EINTR) continue;
            throw SignalCaptureError(std::string("kevent(signal set): ") + std::strerror(errno));
#else
            unsigned char b = 0;
            ::ssize_t n;
            do {
                n = ::read(fd, &b, 1);
            } while (n < 0 && errno == EINTR);

            if (n == 1) co_return static_cast<int>(b);

            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                co_await YarnBall::io::waitReadable(fd);
                continue;
            }
            if (n == 0) {
                throw SignalCaptureError("signal pipe closed");
            }
            throw SignalCaptureError(std::string("read(signal pipe): ") + std::strerror(errno));
#endif
        }
    }

#else // _WIN32

    SignalSet::SignalSet(std::initializer_list<int>) {
        throw SignalCaptureError(
            "SignalSet on Windows is not implemented in this round; "
            "use SetConsoleCtrlHandler directly until the IOCP-backed "
            "signal capture path lands.");
    }

    SignalSet::~SignalSet() = default;

    YarnBall::Task<int> SignalSet::next() {
        throw SignalCaptureError("SignalSet on Windows is not implemented");
        co_return 0; // unreachable, satisfies the coroutine machinery
    }

#endif

}
