//
// FdSlot -- an fd slot that close() can retire while read/write/wait calls are still in flight.
//
// Closing a descriptor that a woken coroutine is about to use is a descriptor-reuse bug: between the coroutine's last
// "is it still open?" check and its syscall the number can be handed to an unrelated open(). So every operation holds
// a Guard (an in-flight count taken BEFORE the fd is loaded) for its whole duration, and close() only publishes -1,
// wakes the parked waiters and parks the fd in @c pending; the ::close() is done by whoever drops the count to zero
// (close() itself when nothing is in flight). The slot is held by shared_ptr so a Guard also outlives its owner object.
//

#ifndef EMBEDDED_FD_SLOT_H
#define EMBEDDED_FD_SLOT_H

#include <atomic>
#include <memory>

#include "IoAwaiters.h"
#include "Reactor.h"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace Embedded::detail {

    class FdSlot {
    public:
        explicit FdSlot(int fd) noexcept : fd(fd) {
        }

        FdSlot(const FdSlot &) = delete;
        FdSlot &operator=(const FdSlot &) = delete;

        /// @brief Take ownership of @p fd in a shared slot; if the allocation fails the fd is closed before the throw,
        ///        so a failed wrap never leaks the descriptor.
        static std::shared_ptr<FdSlot> adopt(int fd) {
            try {
                return std::make_shared<FdSlot>(fd);
            } catch (...) {
#ifndef _WIN32
                if (fd >= 0) (void) ::close(fd);
#endif
                throw;
            }
        }

        ~FdSlot() { this->close(); }

        /**
         * @brief Scoped use of the fd. @c fd() is the descriptor, valid (not closed, not reused) for the Guard's
         *        lifetime, or -1 if the slot was already closed when the Guard was taken.
         */
        class Guard {
        public:
            Guard() noexcept = default;

            explicit Guard(std::shared_ptr<FdSlot> s) noexcept : slot(std::move(s)) {
                if (!this->slot) return;
                // Count first, load second: a close() that retires the fd after this load sees the count and defers.
                this->slot->inflight.fetch_add(1, std::memory_order_seq_cst);
                this->value = this->slot->fd.load(std::memory_order_seq_cst);
            }

            Guard(const Guard &) = delete;
            Guard &operator=(const Guard &) = delete;

            Guard(Guard &&o) noexcept : slot(std::move(o.slot)), value(o.value) {
            }

            ~Guard() {
                if (this->slot) this->slot->leave();
            }

            int fd() const noexcept { return this->value; }

            /**
             * @brief Awaiter for "fd readable" / "fd writable" that cannot miss a concurrent close(). cancelFd only
             *        wakes waiters already registered, so a close() landing between the caller's last check and the
             *        registration would leave the call parked forever on a still-open fd; registering first and then
             *        re-checking the slot closes that window (either close() sees the waiter, or the waiter sees
             *        the close and cancels itself).
             */
            template<typename Inner>
            struct Wait {
                Inner inner;
                std::shared_ptr<FdSlot> slot;
                int value;

                Wait(std::shared_ptr<FdSlot> s, int fd) : inner(fd), slot(std::move(s)), value(fd) {
                }

                bool await_ready() const noexcept { return false; }

                template<typename Promise>
                bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
                    // Locals only after the inner suspend: once registered, the wait may complete and free this awaiter.
                    const std::shared_ptr<FdSlot> s = this->slot;
                    const int fd = this->value;
                    if (!this->inner.await_suspend(h)) return false;
                    if (s->get() != fd) (void) YarnBall::Reactor::instance()->cancelFd(fd);
                    return true;
                }

                void await_resume() { this->inner.await_resume(); }
            };

            Wait<YarnBall::io::WaitReadableAwaiter> readable() const { return {this->slot, this->value}; }
            Wait<YarnBall::io::WaitWritableAwaiter> writable() const { return {this->slot, this->value}; }

            /// @brief False once close() has run (so a woken call stops instead of issuing another syscall).
            bool open() const noexcept {
                return this->slot && this->slot->fd.load(std::memory_order_acquire) == this->value;
            }

        private:
            std::shared_ptr<FdSlot> slot;
            int value = -1;
        };

        int get() const noexcept { return this->fd.load(std::memory_order_acquire); }

        /// @brief Retire the fd. Idempotent; wakes parked waiters; the ::close() waits for in-flight operations.
        void close() noexcept {
            const int f = this->fd.exchange(-1, std::memory_order_seq_cst);
            if (f < 0) return;
            YarnBall::Reactor::instance()->cancelFd(f);
            this->pending.store(f, std::memory_order_seq_cst);
            if (this->inflight.load(std::memory_order_seq_cst) == 0) this->release();
        }

    private:
        void leave() noexcept {
            if (this->inflight.fetch_sub(1, std::memory_order_seq_cst) == 1) this->release();
        }

        /// @brief Close the parked fd, once, by whichever side gets here first.
        void release() noexcept {
            const int p = this->pending.exchange(-1, std::memory_order_seq_cst);
#ifndef _WIN32
            if (p >= 0) (void) ::close(p);
#else
            (void) p;
#endif
        }

        std::atomic<int> fd;
        std::atomic<int> pending{-1};
        std::atomic<int> inflight{0};
    };

}

#endif // EMBEDDED_FD_SLOT_H
