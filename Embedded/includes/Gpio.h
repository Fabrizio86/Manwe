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
// Non-Linux platforms (macOS, Windows, *BSD): every entry point
// throws @c GpioError. There is no portable user-space GPIO ABI
// outside the Linux character device, so this round does not stub
// one.
//

#ifndef EMBEDDED_GPIO_H
#define EMBEDDED_GPIO_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "Coroutines.h"

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
            : lineFd(std::exchange(other.lineFd, -1)),
              isOutput(other.isOutput) {
        }

        GpioLine &operator=(GpioLine &&other) noexcept {
            if (this != &other) {
                this->close();
                this->lineFd = std::exchange(other.lineFd, -1);
                this->isOutput = other.isOutput;
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
         * @throws GpioError if the line was configured for output,
         *         or on read failure.
         */
        YarnBall::Task<GpioEvent> waitForEvent();

        int fd() const noexcept { return this->lineFd; }

        /**
         * @brief Release the line back to the kernel. Idempotent.
         */
        void close() noexcept;

    private:
        friend class GpioChip;

        GpioLine(int fd, bool output) noexcept
            : lineFd(fd), isOutput(output) {
        }

        int lineFd = -1;
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
            : chipFd(std::exchange(other.chipFd, -1)) {
        }

        GpioChip &operator=(GpioChip &&other) noexcept {
            if (this != &other) {
                this->close();
                this->chipFd = std::exchange(other.chipFd, -1);
            }
            return *this;
        }

        ~GpioChip() { this->close(); }

        /**
         * @brief Request @p pin as an output. @p initialHigh sets the
         *        starting level. Returns a @c GpioLine that owns a
         *        new fd; subsequent @c set calls drive the line.
         */
        GpioLine requestOutput(int pin, bool initialHigh = false);

        /**
         * @brief Request @p pin as an edge-triggered input. The
         *        returned @c GpioLine's @c waitForEvent suspends
         *        until the kernel delivers an event of the requested
         *        edge.
         */
        GpioLine requestInputEdge(int pin, Edge edge);

        int fd() const noexcept { return this->chipFd; }

        void close() noexcept;

    private:
        explicit GpioChip(int fd) noexcept : chipFd(fd) {
        }

        int chipFd = -1;
    };

}

#endif // EMBEDDED_GPIO_H
