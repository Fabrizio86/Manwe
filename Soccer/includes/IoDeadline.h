//
// IoDeadline: an RAII "this connection must make progress by T" guard.
//
// When the deadline passes the socket is shut down (not closed), which wakes
// whatever coroutine is parked reading or writing it; that operation then fails
// with EOF or a socket error and the owner unwinds. The same watchdog + guard +
// shutdown(fd) scheme TlsListener::completeHandshake has always used, extracted
// so every server can apply it (slowloris protection) without a sleeping
// coroutine frame per connection.
//
// It only covers sockets that are connected: shutdown() does not wake a socket
// still in the middle of connect() on every platform (macOS), so the connect
// phase has its own limit -- tcpConnect(host, port, timeout) -- which abandons
// the attempt through a cancellation scope (Cancellation.h) instead.
//
// Safety against fd reuse: the deadline is disarmed under the guard's mutex
// before the owner closes or releases the fd (the destructor does it too), and
// the watchdog re-checks that flag under the same mutex before it calls
// shutdown(). A late timer therefore never touches a descriptor that has been
// closed and handed to another connection -- provided the IoDeadline is destroyed
// (or disarmed) before its owner closes the fd, which is the contract of the
// raw-fd overloads. The overloads taking a TcpStream do not depend on it: they
// hold a weak reference to the stream's socket slot and shut the descriptor down
// only while the slot still owns it.
//
// Cost model: one sweeper coroutine for the whole process, alive only while some
// deadline is armed. It wakes every kSweepInterval, shuts down expired sockets
// and drops disarmed entries, so a connection that finished early costs a list
// slot until the next tick instead of a sleeping coroutine frame for the whole
// timeout. Expiry is therefore accurate to about kSweepInterval.
//

#ifndef SOCCER_IODEADLINE_H
#define SOCCER_IODEADLINE_H

#include <atomic>
#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "Coroutines.h"
#include "NetUtil.h"
#include "TcpStream.h"
#include "Timers.h"

namespace Soccer {

    namespace detail {

        /// Granularity of the deadline sweeper.
        inline constexpr std::chrono::milliseconds kDeadlineSweepInterval{100};

        /**
         * @brief The one place the deadline sweeper sleeps. When Yarn grows a
         *        cancellable sleep, replace this body (and let the sweeper be
         *        woken early) -- nothing else in Soccer depends on how it waits.
         */
        inline auto deadlineSleep(std::chrono::milliseconds d) noexcept {
            return YarnBall::sleepFor(d);
        }

        /// Shared between an armed deadline and the sweeper.
        struct DeadlineGuard {
            std::mutex mu;
            bool over = false;                 ///< the owner is done; the fd must not be touched
            std::atomic<bool> fired{false};    ///< the deadline passed and the fd was shut down
        };

        class DeadlineSweeper {
        public:
            static DeadlineSweeper &instance() {
                // Leaked on purpose: the sweeper coroutine may still be running
                // on a pool thread while static destructors run at exit.
                static DeadlineSweeper *s = new DeadlineSweeper;
                return *s;
            }

            void add(std::shared_ptr<DeadlineGuard> g, int fd,
                     std::chrono::steady_clock::time_point when,
                     SocketSlot::Watch watch = {}) {
                bool spawn = false;
                const DeadlineGuard *mine = g.get();
                {
                    std::lock_guard<std::mutex> lk(this->mu);
                    this->entries.push_back(Entry{std::move(g), fd, when, std::move(watch)});
                    if (!this->running) {
                        this->running = true;
                        spawn = true;
                    }
                }
                if (spawn) {
                    try {
                        YarnBall::coSpawn(sweepLoop());
                    } catch (...) {
                        // No sweeper started: let the next add() try again instead of
                        // leaving `running` set with nobody to clear it, which would
                        // park every later deadline forever.
                        std::lock_guard<std::mutex> lk(this->mu);
                        this->running = false;
                        // And take our entry back out: nobody will ever sweep it, and the
                        // caller sees the throw and holds no deadline.
                        std::erase_if(this->entries, [mine](const Entry &e) { return e.g.get() == mine; });
                        throw;
                    }
                }
            }

        private:
            struct Entry {
                std::shared_ptr<DeadlineGuard> g;
                int fd;
                std::chrono::steady_clock::time_point when;
                SocketSlot::Watch watch; ///< empty for a raw fd
            };

            static YarnBall::Task<void> sweepLoop() {
                auto &self = instance();
                while (true) {
                    co_await deadlineSleep(kDeadlineSweepInterval);
                    if (!self.sweepOnce()) co_return;
                }
            }

            /// @return false when nothing is left armed (the sweeper then exits).
            bool sweepOnce() {
                const auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lk(this->mu);
                std::size_t keep = 0;
                for (std::size_t i = 0; i < this->entries.size(); ++i) {
                    Entry &e = this->entries[i];
                    bool drop = false;
                    {
                        std::lock_guard<std::mutex> gl(e.g->mu);
                        if (e.g->over) {
                            drop = true;
                        } else if (e.when <= now) {
                            e.g->fired.store(true, std::memory_order_release);
                            // Wakes the parked wait; the operation then fails. Through the slot when we
                            // have one, so a descriptor the owner already closed is left alone.
                            if (e.watch) (void) e.watch.shutdownIf(e.fd);
                            else shutdownSocket(e.fd);
                            drop = true;
                        }
                    }
                    if (!drop) {
                        if (keep != i) this->entries[keep] = std::move(e);
                        ++keep;
                    }
                }
                this->entries.resize(keep);
                if (this->entries.empty()) {
                    this->running = false;
                    return false;
                }
                return true;
            }

            std::mutex mu;
            std::vector<Entry> entries;
            bool running = false;
        };

    } // namespace detail

    /**
     * @class IoDeadline
     * @brief Move-only RAII deadline on one socket fd. See the file comment.
     *
     * Declare it AFTER (so it is destroyed BEFORE) the object that owns the fd.
     * One instance covers one phase at a time: @ref arm replaces the previous
     * deadline. @ref expired stays true once any deadline of this instance fired.
     */
    class IoDeadline final {
    public:
        IoDeadline() = default;

        IoDeadline(int fd, std::chrono::milliseconds limit) { this->arm(fd, limit); }

        /// Deadline on a stream's socket that stays safe if the stream is closed first.
        /// Arm it once the stream has reached its final place: moving the stream detaches it.
        IoDeadline(const TcpStream &s, std::chrono::milliseconds limit) { this->arm(s, limit); }

        IoDeadline(const IoDeadline &) = delete;
        IoDeadline &operator=(const IoDeadline &) = delete;

        IoDeadline(IoDeadline &&o) noexcept
            : guard(std::move(o.guard)), firedBefore(o.firedBefore) {
            o.firedBefore = false;
        }

        IoDeadline &operator=(IoDeadline &&o) noexcept {
            if (this != &o) {
                this->disarm(); // folds a fired deadline of ours into firedBefore
                this->guard = std::move(o.guard);
                this->firedBefore = this->firedBefore || o.firedBefore;
                o.firedBefore = false;
            }
            return *this;
        }

        ~IoDeadline() { this->disarm(); }

        /**
         * @brief Start a deadline of @p limit on @p fd, replacing any earlier one.
         *        A zero or negative limit means "no limit": the instance is left
         *        disarmed.
         */
        void arm(int fd, std::chrono::milliseconds limit) {
            this->disarm();
            if (limit.count() <= 0 || fd < 0) return;
            this->guard = std::make_shared<detail::DeadlineGuard>();
            try {
                detail::DeadlineSweeper::instance().add(
                    this->guard, fd, std::chrono::steady_clock::now() + limit);
            } catch (...) {
                this->guard.reset(); // nothing armed
                throw;
            }
        }

        /// @copydoc IoDeadline(const TcpStream &, std::chrono::milliseconds)
        void arm(const TcpStream &s, std::chrono::milliseconds limit) {
            this->disarm();
            const int fd = s.fd();
            if (limit.count() <= 0 || fd < 0) return;
            this->guard = std::make_shared<detail::DeadlineGuard>();
            try {
                detail::DeadlineSweeper::instance().add(
                    this->guard, fd, std::chrono::steady_clock::now() + limit, s.slot().watch());
            } catch (...) {
                this->guard.reset();
                throw;
            }
        }

        /**
         * @brief Cancel the current deadline. After this returns the sweeper will
         *        never shut @c fd down on this deadline's behalf. Idempotent.
         */
        void disarm() noexcept {
            if (!this->guard) return;
            {
                std::lock_guard<std::mutex> lk(this->guard->mu);
                this->guard->over = true;
            }
            if (this->guard->fired.load(std::memory_order_acquire)) this->firedBefore = true;
            this->guard.reset();
        }

        /// @return true if a deadline of this instance passed and shut the socket down.
        bool expired() const noexcept {
            return this->firedBefore ||
                   (this->guard && this->guard->fired.load(std::memory_order_acquire));
        }

    private:
        std::shared_ptr<detail::DeadlineGuard> guard;
        bool firedBefore = false;
    };

}

#endif // SOCCER_IODEADLINE_H
