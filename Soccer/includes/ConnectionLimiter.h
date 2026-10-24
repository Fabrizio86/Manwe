//
// ConnectionLimiter: a cap on concurrently served connections.
//

#ifndef SOCCER_CONNECTIONLIMITER_H
#define SOCCER_CONNECTIONLIMITER_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

namespace Soccer {

    /**
     * @class ConnectionLimiter
     * @brief Counts live connections against a maximum. @ref tryAcquire hands out
     *        an RAII @ref Slot; hold it for the lifetime of the connection and the
     *        count drops when it is destroyed (on any exit path, including the
     *        connection coroutine being destroyed while parked).
     *
     * A maximum of zero means unlimited: slots are still counted, so
     * @ref active stays meaningful.
     *
     * Used by the built-in servers (see their options structs), including
     * @c wsServe for WebSocket. It is public so a hand-written accept loop can
     * apply the same cap.
     */
    class ConnectionLimiter final {
    public:
        /// Move-only ticket for one live connection. Empty (false) when refused.
        class Slot final {
        public:
            Slot() noexcept = default;
            Slot(const Slot &) = delete;
            Slot &operator=(const Slot &) = delete;
            Slot(Slot &&o) noexcept : count(std::move(o.count)) {}
            Slot &operator=(Slot &&o) noexcept {
                if (this != &o) {
                    this->release();
                    this->count = std::move(o.count);
                }
                return *this;
            }
            ~Slot() { this->release(); }

            explicit operator bool() const noexcept { return this->count != nullptr; }

        private:
            friend class ConnectionLimiter;
            explicit Slot(std::shared_ptr<std::atomic<std::size_t>> c) noexcept
                : count(std::move(c)) {}
            void release() noexcept {
                if (this->count) {
                    this->count->fetch_sub(1, std::memory_order_acq_rel);
                    this->count.reset();
                }
            }
            std::shared_ptr<std::atomic<std::size_t>> count;
        };

        explicit ConnectionLimiter(std::size_t maxConnections = 0)
            : max(maxConnections),
              count(std::make_shared<std::atomic<std::size_t>>(0)) {}

        /// @return A slot, or an empty one when @c maxConnections are already live.
        Slot tryAcquire() {
            // Compare-exchange, so a refused attempt never raises the count (a transient
            // over-count would falsely refuse a caller while a slot is in fact free).
            std::size_t cur = this->count->load(std::memory_order_relaxed);
            do {
                if (this->max != 0 && cur >= this->max) return Slot{};
            } while (!this->count->compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel,
                                                         std::memory_order_relaxed));
            return Slot{this->count};
        }

        /// @return Connections currently holding a slot.
        std::size_t active() const noexcept { return this->count->load(std::memory_order_acquire); }

    private:
        std::size_t max;
        std::shared_ptr<std::atomic<std::size_t>> count;
    };

}

#endif // SOCCER_CONNECTIONLIMITER_H
