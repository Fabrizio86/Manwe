//
// Small socket helpers shared by the Soccer sources: close-on-exec, and a
// portable full shutdown used to wake a coroutine parked on a socket.
//

#ifndef SOCCER_NETUTIL_H
#define SOCCER_NETUTIL_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "IoAwaiters.h"
#include "PlatformNet.h"
#include "Reactor.h"
#include "SocketException.h"

#ifndef _WIN32
#include <netinet/tcp.h>
#include <sys/un.h>
#else
#include <afunix.h>
#endif

namespace Soccer {

    namespace detail {

        template<bool Writable> struct SlotWait;

        /**
         * @brief Owner of one socket descriptor that other threads may @c close() while
         *        a coroutine is using it.
         *
         * A plain @c int shared that way is a data race, and worse: a reader that tested
         * it and then called @c recv could do so on a descriptor number @c close() had
         * already released and the kernel had handed to another connection. Here the
         * number is an atomic, every syscall is made inside a @ref Use (which counts it
         * in), and @ref retire defers the real @c close until no @ref Use is left, so a
         * descriptor is never closed underneath a syscall that loaded it.
         *
         * A coroutine that has to wait for readiness keeps its @ref Use across the wait
         * and waits through @ref waitReadable / @ref waitWritable: the descriptor then
         * stays open (and its number unreused) until the waiter has left, and the wait
         * re-checks the slot after registering, so a closer that took the descriptor out
         * between the waiter's EAGAIN and its registration (and so had nobody to wake) is
         * not missed. Woken waiters re-take a @ref Use, find the slot empty and fail.
         *
         * The counters live in a shared block that a registering waiter also holds, so the
         * re-check never reads an owner that finished and was destroyed in the meantime.
         *
         * Moving requires that no operation is in flight on the source.
         */
        class SocketSlot final {
            template<bool> friend struct SlotWait;
            struct State {
                static constexpr int kMaxDeferred = 4;
                std::atomic<int> fdValue{-1};
                std::atomic<int> busy{0};
                std::atomic<bool> interrupted{false}; ///< set by @ref SocketSlot::interrupt; sticky
                std::mutex mu;          ///< guards deferred / nDeferred
                int deferred[kMaxDeferred]{};
                int nDeferred = 0;

                void end() noexcept {
                    if (this->busy.fetch_sub(1, std::memory_order_seq_cst) == 1) this->flush();
                }
                /// Close every retired descriptor. Only called with no Use outstanding.
                void flush() noexcept {
                    int todo[kMaxDeferred];
                    int n = 0;
                    {
                        std::lock_guard<std::mutex> lk(this->mu);
                        for (int i = 0; i < this->nDeferred; ++i) todo[n++] = this->deferred[i];
                        this->nDeferred = 0;
                    }
                    for (int i = 0; i < n; ++i) (void) YarnBall::closeSocket(todo[i]);
                }
            };

        public:
            SocketSlot() : st(std::make_shared<State>()) {}
            /// Takes ownership of @p fd, also when the shared block cannot be allocated (it is closed, then bad_alloc).
            explicit SocketSlot(int fd) {
                try {
                    this->st = std::make_shared<State>();
                } catch (...) {
                    if (fd >= 0) (void) YarnBall::closeSocket(fd);
                    throw;
                }
                this->st->fdValue.store(fd, std::memory_order_relaxed);
            }
            SocketSlot(const SocketSlot &) = delete;
            SocketSlot &operator=(const SocketSlot &) = delete;
            /// Allocates a fresh block (so a @ref Watch on @p o does not follow the descriptor); if that throws, @p o is
            /// untouched. The interrupted flag moves along with the descriptor.
            SocketSlot(SocketSlot &&o) : st(std::make_shared<State>()) {
                this->st->interrupted.store(o.interrupted(), std::memory_order_relaxed);
                this->st->fdValue.store(o.take(), std::memory_order_relaxed);
            }

            /// Scope of one syscall (or of one wait) on the descriptor; see @ref begin.
            class Use final {
            public:
                Use(const Use &) = delete;
                Use &operator=(const Use &) = delete;
                ~Use() { this->state->end(); }
                /// The descriptor, or -1 when the slot was closed.
                int fd() const noexcept { return this->value; }

            private:
                friend class SocketSlot;
                template<bool> friend struct SlotWait;
                Use(std::shared_ptr<State> s, int v) noexcept : state(std::move(s)), value(v) {}
                std::shared_ptr<State> state;
                int value;
            };

            /// @return The current descriptor, or -1. Only for reporting: use @ref begin to do I/O.
            int get() const noexcept { return this->st->fdValue.load(std::memory_order_acquire); }

            /// Begin a syscall: the descriptor stays open until the returned scope ends.
            Use begin() const noexcept {
                this->st->busy.fetch_add(1, std::memory_order_seq_cst);
                return Use(this->st, this->st->fdValue.load(std::memory_order_seq_cst));
            }

            /// Take the descriptor out of the slot without closing it (-1 when empty).
            int take() noexcept { return this->st->fdValue.exchange(-1, std::memory_order_seq_cst); }

            /**
             * @brief Ask whoever is waiting on this slot to give up, without closing it. Sticky: later waits see it
             *        too. The caller still has to wake a parked waiter (Reactor::cancelFd); a waiter that registers
             *        after the flag was set wakes itself, see @ref SlotWait.
             */
            void interrupt() noexcept { this->st->interrupted.store(true, std::memory_order_seq_cst); }

            /// Whether @ref interrupt was called.
            bool interrupted() const noexcept { return this->st->interrupted.load(std::memory_order_seq_cst); }

            /// Put @p fd in an empty slot. The interrupted flag is left as it is.
            void reset(int fd) noexcept { this->st->fdValue.store(fd, std::memory_order_seq_cst); }

            /**
             * @brief Move-assignment tail: the (already closed) slot takes over @p o's descriptor and its interrupted
             *        flag, so it ends up exactly like the slot it adopted -- an interrupt on the old occupant does not
             *        leak into the new one. No allocation.
             */
            void adopt(SocketSlot &o) noexcept {
                this->st->interrupted.store(o.st->interrupted.load(std::memory_order_seq_cst), std::memory_order_seq_cst);
                this->st->fdValue.store(o.take(), std::memory_order_seq_cst);
            }

            /**
             * @brief Close a descriptor obtained from @ref take, once no @ref Use is left.
             *        Several descriptors may be pending at once (a slot that is closed,
             *        refilled and closed again while a waiter is still leaving); beyond
             *        a handful the oldest is closed at once.
             */
            void retire(int fd) noexcept {
                if (fd < 0) return;
                int now = -1;
                {
                    std::lock_guard<std::mutex> lk(this->st->mu);
                    if (this->st->nDeferred == State::kMaxDeferred) {
                        now = this->st->deferred[0];
                        for (int i = 1; i < State::kMaxDeferred; ++i) this->st->deferred[i - 1] = this->st->deferred[i];
                        --this->st->nDeferred;
                    }
                    this->st->deferred[this->st->nDeferred++] = fd;
                }
                if (now >= 0) (void) YarnBall::closeSocket(now);
                if (this->st->busy.load(std::memory_order_seq_cst) == 0) this->st->flush();
            }

            /**
             * @brief Non-owning reference to the slot's descriptor, for a timer that may
             *        fire after the owner closed or released it. @ref shutdownIf only acts
             *        while the slot still holds that very descriptor, and keeps it open for
             *        the duration of the call, so it can never hit a recycled number.
             */
            class Watch final {
            public:
                Watch() = default;
                explicit operator bool() const noexcept { return !this->w.expired(); }
                /// Shut @p fd down if the slot still owns it. @return whether it did.
                bool shutdownIf(int fd) const noexcept {
                    const auto st = this->w.lock();
                    if (!st) return false;
                    st->busy.fetch_add(1, std::memory_order_seq_cst);
                    bool done = false;
                    if (st->fdValue.load(std::memory_order_seq_cst) == fd) {
                        shutdownSocketImpl(fd);
                        done = true;
                    }
                    st->end();
                    return done;
                }

            private:
                friend class SocketSlot;
                explicit Watch(const std::shared_ptr<State> &s) : w(s) {}
                std::weak_ptr<State> w;
            };

            /// A @ref Watch on this slot.
            Watch watch() const { return Watch(this->st); }

        private:
            static void shutdownSocketImpl(int fd) noexcept {
#ifdef _WIN32
                (void) ::shutdown(static_cast<SOCKET>(static_cast<unsigned>(fd)), SD_BOTH);
#else
                (void) ::shutdown(fd, SHUT_RDWR);
#endif
            }

            std::shared_ptr<State> st;
        };

        /**
         * @brief Wait for @p Writable (else readable) readiness of the descriptor a
         *        @ref SocketSlot::Use holds, without losing a concurrent close.
         *
         * Plain @c io::waitReadable leaves a window: a closer that wakes the waiters of
         * the descriptor after the owner decided to wait but before it registered finds
         * nobody, and the waiter then sleeps on a descriptor nobody will ever signal.
         * After registering, this awaiter looks at the slot again; if it was emptied it
         * wakes the descriptor's waiters itself (the closer's own wake, if it comes later,
         * finds the registration). Keep the @ref SocketSlot::Use alive across the wait.
         */
        template<bool Writable>
        struct SlotWait {
            explicit SlotWait(const SocketSlot::Use &u) noexcept
                : state(u.state), fd(u.value), inner(u.value) {}
            SlotWait(const SlotWait &) = delete;
            SlotWait &operator=(const SlotWait &) = delete;

            bool await_ready() const noexcept { return false; }

            template<typename Promise>
            bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
                // Locals only after the inner registration: the wait may complete, and
                // destroy this awaiter and the whole frame, on another thread right away.
                const std::shared_ptr<SocketSlot::State> keep = this->state;
                const int waitFd = this->fd;
                const bool parked = this->inner.await_suspend(h);
                if (parked && (keep->fdValue.load(std::memory_order_seq_cst) != waitFd
                               || keep->interrupted.load(std::memory_order_seq_cst))) {
                    (void) YarnBall::Reactor::instance()->cancelFd(waitFd);
                }
                return parked;
            }

            void await_resume() { this->inner.await_resume(); }

        private:
            std::shared_ptr<SocketSlot::State> state;
            int fd;
            YarnBall::io::impl::FdWaitAwaiter<Writable> inner;
        };

        /// Wait until the descriptor held by @p u is readable; see @ref SlotWait.
        inline SlotWait<false> waitReadable(const SocketSlot::Use &u) noexcept { return SlotWait<false>(u); }
        /// Wait until the descriptor held by @p u is writable; see @ref SlotWait.
        inline SlotWait<true> waitWritable(const SocketSlot::Use &u) noexcept { return SlotWait<true>(u); }

        /// Flags for @c send: no SIGPIPE on a closed peer where the platform supports it per call.
#if defined(MSG_NOSIGNAL)
        inline constexpr int kSendFlags = MSG_NOSIGNAL;
#else
        inline constexpr int kSendFlags = 0;
#endif

        /**
         * @brief Make writes on @p fd fail with EPIPE instead of raising SIGPIPE, on the
         *        platforms that have the per-socket option (macOS, the BSDs). Elsewhere
         *        the per-call @c MSG_NOSIGNAL (@ref kSendFlags) does it; best effort, no throw.
         */
        inline void suppressSigpipe(int fd) noexcept {
#if defined(SO_NOSIGPIPE)
            const int on = 1;
            (void) ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                                reinterpret_cast<const char *>(&on), sizeof(on));
#else
            (void) fd;
#endif
        }

        /**
         * @brief Fill a @c sockaddr_un from a filesystem path and return its length.
         *        Throws @c SocketException for an empty path, a path with an embedded NUL
         *        (which would silently truncate it, or select the Linux abstract
         *        namespace when leading), or one too long for the platform's @c sun_path
         *        (truncating would bind or connect to the wrong socket).
         */
        inline socklen_t makeUnixAddr(sockaddr_un *addr, const std::string &path) {
            if (path.empty()) {
                throw SocketException("AF_UNIX path is empty");
            }
            if (path.find('\0') != std::string::npos) {
                throw SocketException("AF_UNIX path contains a NUL byte");
            }
            if (path.size() >= sizeof(addr->sun_path)) {
                throw SocketException("AF_UNIX path too long for sun_path: " + path);
            }
            std::memset(addr, 0, sizeof(*addr));
            addr->sun_family = AF_UNIX;
            std::memcpy(addr->sun_path, path.c_str(), path.size());
            return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
        }

        /**
         * @brief Mark @p fd close-on-exec so a fork+exec elsewhere in the process
         *        does not leak the socket into the child. No-op on Windows, where
         *        sockets are not inherited unless a handle-inheriting
         *        CreateProcess asks for it.
         */
        inline void setCloexec(int fd) noexcept {
#ifndef _WIN32
            const int flags = ::fcntl(fd, F_GETFD, 0);
            if (flags >= 0) (void) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
#else
            (void) fd;
#endif
        }

        /**
         * @brief @c ::socket with close-on-exec set atomically where the platform
         *        supports @c SOCK_CLOEXEC (Linux, the BSDs), and right after
         *        creation elsewhere (macOS).
         * @return The new fd, or -1 with the platform error code set.
         */
        inline int openSocket(int family, int type, int protocol = 0) noexcept {
#if !defined(_WIN32) && defined(SOCK_CLOEXEC)
            return static_cast<int>(::socket(family, type | SOCK_CLOEXEC, protocol));
#else
            const int fd = static_cast<int>(::socket(family, type, protocol));
            if (fd >= 0) setCloexec(fd);
            return fd;
#endif
        }

        /**
         * @brief Shut both directions of @p fd down without closing it. Parked
         *        reads and writes on the socket wake up (EOF / error), and the fd
         *        number stays reserved until its owner closes it.
         */
        inline void shutdownSocket(int fd) noexcept {
#ifdef _WIN32
            (void) ::shutdown(static_cast<SOCKET>(static_cast<unsigned>(fd)), SD_BOTH);
#else
            (void) ::shutdown(fd, SHUT_RDWR);
#endif
        }


        /**
         * @brief Disable Nagle's algorithm on a TCP socket, best effort. A response written
         *        as two small sends otherwise waits for the peer's delayed ACK (about 40 ms)
         *        before the second one leaves. On a non-TCP socket (AF_UNIX) the option does
         *        not exist and the call just fails, which is ignored.
         */
        inline void setNoDelay(int fd) noexcept {
            const int on = 1;
            (void) ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                                reinterpret_cast<const char *>(&on), sizeof(on));
        }

        /**
         * @brief @c steady_clock::now() + @p limit, saturating at @c time_point::max() instead of overflowing: a huge
         *        limit (@c milliseconds::max(), centuries) would otherwise wrap into the past and fire at once. A zero or
         *        negative limit yields "now".
         */
        inline std::chrono::steady_clock::time_point deadlineAfter(std::chrono::milliseconds limit) noexcept {
            using Clock = std::chrono::steady_clock;
            const auto now = Clock::now();
            if (limit.count() <= 0) return now;
            // Room left before the clock's range ends, rounded down to whole milliseconds so the comparison below is
            // done in milliseconds (converting a huge limit to the clock's finer unit would itself overflow).
            const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
            if (limit >= room) return Clock::time_point::max();
            return now + limit;
        }

        /// Smallest share of a connect budget an earlier candidate address gets (see @ref connectSlice).
        inline constexpr std::chrono::milliseconds kMinConnectSlice{250};

        /**
         * @brief Share of a connect budget one candidate address may use. The last
         *        candidate gets everything that is left; an earlier one gets an equal share
         *        (but at least @ref kMinConnectSlice, capped by what is left), so a blackholed
         *        first address cannot consume the whole budget and starve the others.
         * @param remaining Time left of the overall budget.
         * @param candidatesLeft Candidates still to try, this one included (>= 1).
         */
        inline std::chrono::milliseconds connectSlice(std::chrono::milliseconds remaining,
                                                      std::size_t candidatesLeft) noexcept {
            if (candidatesLeft <= 1) return remaining;
            const auto share = remaining / static_cast<std::chrono::milliseconds::rep>(candidatesLeft);
            return std::min(remaining, std::max(kMinConnectSlice, share));
        }
    }

}

#endif // SOCCER_NETUTIL_H
