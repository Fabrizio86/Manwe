//
// Created by Fabrizio Paino on 2026-05-16.
//
// GPIO -- coroutine-driven access to /dev/gpiochip* on Linux.
//
// MANWE_UNTESTED_PLATFORM: ships without a hardware-attached
// validation run. The Linux character-device ioctls
// (GPIO_GET_LINEHANDLE_IOCTL / GPIO_GET_LINEEVENT_IOCTL) follow the
// canonical patterns documented in Documentation/userspace-api/gpio
// in the kernel tree. Validate on a Pi before depending on this in
// production.
//
// Typical use:
//
//     auto chip = Embedded::GpioChip::open("/dev/gpiochip0");
//     auto led  = chip.requestOutput(17);
//     led.set(true);                               // LED on
//
//     auto button = chip.requestInputEdge(27, Embedded::Edge::Rising);
//     auto event  = co_await button.waitForEvent(); // suspends on
//                                                   // the line fd
//
// This uses the GPIO character-device uAPI v1 (GPIO_GET_LINEHANDLE_IOCTL /
// GPIO_GET_LINEEVENT_IOCTL). The kernel deprecated v1 in 5.10 in favour of
// v2 (GPIO_V2_GET_LINE_IOCTL); v1 is still served by current kernels, but
// a port to v2 is due once it can be tested on hardware.
//
// Non-Linux platforms (macOS, Windows, *BSD): every entry point
// throws @c GpioError. There is no portable user-space GPIO ABI
// outside the Linux character device, so this round does not stub
// one.
//

#ifndef EMBEDDED_GPIO_H
#define EMBEDDED_GPIO_H

#include <memory>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "Coroutines.h"
#include "FdSlot.h"

namespace Embedded {

    /**
     * @enum Edge
     * @brief Which transitions of an input line generate a
     *        @c waitForEvent resumption.
     */
    enum class Edge { Rising, Falling, Both };

    /**
     * @struct GpioEvent
     * @brief One edge event read off an input line.
     */
    struct GpioEvent {
        /**
         * @brief Kernel-supplied timestamp (CLOCK_MONOTONIC nanoseconds)
         *        at which the edge was observed. Useful for measuring
         *        debounce windows and inter-event intervals.
         */
        std::uint64_t timestampNs{0};

        /**
         * @brief Which edge fired. For a line configured for a single
         *        edge, this always equals the requested edge. For
         *        @c Edge::Both, may be either Rising or Falling.
         */
        Edge edge{Edge::Rising};
    };

    /**
     * @class GpioError
     * @brief Thrown by GPIO operations on chip-open, ioctl, or read
     *        failure.
     */
    class GpioError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /**
     * @class GpioLine
     * @brief A single requested GPIO line. Created by
     *        @c GpioChip::requestOutput or @c GpioChip::requestInputEdge.
     *        Move-only; closing the underlying fd releases the line
     *        back to the kernel.
     *
     * The line fd is distinct from the chip fd: the kernel returns
     * one new fd per line request. Multiple coroutines awaiting the
     * same input line is not supported (one event per byte, one
     * reader per fd).
     */
    class GpioLine final {
    public:
        GpioLine() = default;

        GpioLine(const GpioLine &) = delete;
        GpioLine &operator=(const GpioLine &) = delete;

        GpioLine(GpioLine &&other) noexcept
            : slot(std::move(other.slot)),
              isOutput(std::exchange(other.isOutput, false)) {
        }

        GpioLine &operator=(GpioLine &&other) noexcept {
            if (this != &other) {
                this->close();
                this->slot = std::move(other.slot);
                this->isOutput = std::exchange(other.isOutput, false);
            }
            return *this;
        }

        ~GpioLine() { this->close(); }

        /**
         * @brief Drive an output line high (@c true) or low (@c false).
         *        Throws @c GpioError if called on an input-configured
         *        line, or on ioctl failure.
         */
        void set(bool high);

        /**
         * @brief Suspend until the next edge event on this input
         *        line. Returns a populated @c GpioEvent on resume.
         *
         * Implemented as @c waitReadable on the line fd followed by
         * a small @c read of the kernel's gpioevent_data struct.
         *
         * The event fd is non-blocking: a wake-up that finds no event
         * (a spurious one) waits again instead of blocking a worker.
         * Closing the line while a call is parked makes it throw
         * @c GpioError.
         *
         * @throws GpioError if the line was configured for output,
         *         or on read failure.
         */
        YarnBall::Task<GpioEvent> waitForEvent();

        int fd() const noexcept { return this->slot ? this->slot->get() : -1; }

        /**
         * @brief Release the line back to the kernel. Idempotent, any
         *        thread. A parked @c waitForEvent is woken and throws; the
         *        descriptor itself is released once no call is in flight,
         *        so it cannot be reused under a woken call.
         */
        void close() noexcept;

    private:
        friend class GpioChip;

        // Takes ownership of @p fd: closed again if the slot cannot be allocated.
        GpioLine(int fd, bool output) : slot(detail::FdSlot::adopt(fd)), isOutput(output) {
        }

        static YarnBall::Task<GpioEvent> waitForEventOn(std::shared_ptr<detail::FdSlot> slot, bool output);

        std::shared_ptr<detail::FdSlot> slot; // null once moved from
        bool isOutput = false;
    };

    /**
     * @class GpioChip
     * @brief A GPIO bank, identified by its character-device path
     *        (e.g. @c "/dev/gpiochip0" for the BCM2835 on Raspberry
     *        Pi 4). Move-only.
     */
    class GpioChip final {
    public:
        /**
         * @brief Open @p path and verify it is a GPIO character device
         *        (@c GPIO_GET_CHIPINFO_IOCTL). Throws @c GpioError on
         *        non-existent device, permission denied, or wrong
         *        device class.
         */
        static GpioChip open(const std::string &path);

        GpioChip(const GpioChip &) = delete;
        GpioChip &operator=(const GpioChip &) = delete;

        GpioChip(GpioChip &&other) noexcept
            : slot(std::move(other.slot)),
              lineCount(std::exchange(other.lineCount, 0)) {
        }

        GpioChip &operator=(GpioChip &&other) noexcept {
            if (this != &other) {
                this->close();
                this->slot = std::move(other.slot);
                this->lineCount = std::exchange(other.lineCount, 0);
            }
            return *this;
        }

        ~GpioChip() { this->close(); }

        /**
         * @brief Request @p pin as an output. @p initialHigh sets the
         *        starting level. Returns a @c GpioLine that owns a
         *        new fd; subsequent @c set calls drive the line.
         *        Throws @c GpioError for a pin the chip does not have.
         */
        GpioLine requestOutput(int pin, bool initialHigh = false);

        /**
         * @brief Request @p pin as an edge-triggered input. The
         *        returned @c GpioLine's @c waitForEvent suspends
         *        until the kernel delivers an event of the requested
         *        edge.
         */
        GpioLine requestInputEdge(int pin, Edge edge);

        int fd() const noexcept { return this->slot ? this->slot->get() : -1; }

        /**
         * @brief Close the chip. Idempotent, any thread: like
         *        @c GpioLine::close, the descriptor is released once no
         *        @c requestOutput / @c requestInputEdge is in flight on it.
         *        Lines already requested are unaffected.
         */
        void close() noexcept;

    private:
        // Takes ownership of @p fd: closed again if the slot cannot be allocated.
        GpioChip(int fd, std::uint32_t lines) : slot(detail::FdSlot::adopt(fd)), lineCount(lines) {
        }

        void checkPin(const char *what, int pin) const; // Linux only: throws for a pin outside the chip

        std::shared_ptr<detail::FdSlot> slot; // null once moved from
        std::uint32_t lineCount = 0; // number of lines the chip exposes; pins are validated against it
    };

}

#endif // EMBEDDED_GPIO_H
