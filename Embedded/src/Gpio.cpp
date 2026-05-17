//
// Created by Fabrizio Paino on 2026-05-16.
//
// MANWE_UNTESTED_PLATFORM: see Gpio.h. Linux character-device GPIO
// ioctls; not validated against a real Pi in the session that
// introduced this file.
//

#include "Gpio.h"

#include <cerrno>
#include <cstring>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include "IoAwaiters.h"

namespace Embedded {

#if defined(__linux__)

    GpioChip GpioChip::open(const std::string &path) {
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            throw GpioError("open(" + path + "): " + std::strerror(errno));
        }
        // Verify it really is a GPIO device. GPIO_GET_CHIPINFO_IOCTL
        // is cheap and rejects regular files / wrong device classes.
        struct gpiochip_info info{};
        if (::ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0) {
            const int e = errno;
            ::close(fd);
            throw GpioError(std::string("GPIO_GET_CHIPINFO_IOCTL: ") +
                             std::strerror(e));
        }
        return GpioChip(fd);
    }

    void GpioChip::close() noexcept {
        if (this->chipFd >= 0) {
            (void) ::close(this->chipFd);
            this->chipFd = -1;
        }
    }

    GpioLine GpioChip::requestOutput(int pin, bool initialHigh) {
        if (this->chipFd < 0) {
            throw GpioError("requestOutput on closed GpioChip");
        }
        struct gpiohandle_request req{};
        req.lineoffsets[0] = static_cast<std::uint32_t>(pin);
        req.flags = GPIOHANDLE_REQUEST_OUTPUT;
        req.default_values[0] = initialHigh ? 1 : 0;
        req.lines = 1;
        std::strncpy(req.consumer_label, "manwe-out",
                      sizeof(req.consumer_label) - 1);
        if (::ioctl(this->chipFd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
            throw GpioError(std::string("GPIO_GET_LINEHANDLE_IOCTL: ") +
                             std::strerror(errno));
        }
        return GpioLine(req.fd, /*output=*/true);
    }

    GpioLine GpioChip::requestInputEdge(int pin, Edge edge) {
        if (this->chipFd < 0) {
            throw GpioError("requestInputEdge on closed GpioChip");
        }
        struct gpioevent_request req{};
        req.lineoffset = static_cast<std::uint32_t>(pin);
        req.handleflags = GPIOHANDLE_REQUEST_INPUT;
        switch (edge) {
            case Edge::Rising:
                req.eventflags = GPIOEVENT_REQUEST_RISING_EDGE; break;
            case Edge::Falling:
                req.eventflags = GPIOEVENT_REQUEST_FALLING_EDGE; break;
            case Edge::Both:
                req.eventflags = GPIOEVENT_REQUEST_BOTH_EDGES; break;
        }
        std::strncpy(req.consumer_label, "manwe-in",
                      sizeof(req.consumer_label) - 1);
        if (::ioctl(this->chipFd, GPIO_GET_LINEEVENT_IOCTL, &req) < 0) {
            throw GpioError(std::string("GPIO_GET_LINEEVENT_IOCTL: ") +
                             std::strerror(errno));
        }
        return GpioLine(req.fd, /*output=*/false);
    }

    void GpioLine::set(bool high) {
        if (this->lineFd < 0) {
            throw GpioError("set on closed GpioLine");
        }
        if (!this->isOutput) {
            throw GpioError("set called on input-configured GpioLine");
        }
        struct gpiohandle_data data{};
        data.values[0] = high ? 1 : 0;
        if (::ioctl(this->lineFd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0) {
            throw GpioError(std::string("GPIOHANDLE_SET_LINE_VALUES_IOCTL: ") +
                             std::strerror(errno));
        }
    }

    YarnBall::Task<GpioEvent> GpioLine::waitForEvent() {
        if (this->lineFd < 0) {
            throw GpioError("waitForEvent on closed GpioLine");
        }
        if (this->isOutput) {
            throw GpioError("waitForEvent called on output-configured GpioLine");
        }
        co_await YarnBall::io::waitReadable(this->lineFd);

        struct gpioevent_data raw{};
        ::ssize_t n;
        do {
            n = ::read(this->lineFd, &raw, sizeof(raw));
        } while (n < 0 && errno == EINTR);
        if (n < static_cast<::ssize_t>(sizeof(raw))) {
            throw GpioError(std::string("read(gpioevent_data): ") +
                             std::strerror(errno));
        }
        GpioEvent ev{};
        ev.timestampNs = raw.timestamp;
        ev.edge = (raw.id == GPIOEVENT_EVENT_RISING_EDGE)
                  ? Edge::Rising : Edge::Falling;
        co_return ev;
    }

    void GpioLine::close() noexcept {
        if (this->lineFd >= 0) {
            (void) ::close(this->lineFd);
            this->lineFd = -1;
        }
    }

#else // non-Linux

    GpioChip GpioChip::open(const std::string &) {
        throw GpioError(
            "GpioChip requires a Linux /dev/gpiochip* character device; "
            "no portable user-space GPIO ABI exists on this platform.");
    }

    void GpioChip::close() noexcept {}

    GpioLine GpioChip::requestOutput(int, bool) {
        throw GpioError("GpioChip::requestOutput: not supported on this platform");
    }

    GpioLine GpioChip::requestInputEdge(int, Edge) {
        throw GpioError("GpioChip::requestInputEdge: not supported on this platform");
    }

    void GpioLine::set(bool) {
        throw GpioError("GpioLine::set: not supported on this platform");
    }

    YarnBall::Task<GpioEvent> GpioLine::waitForEvent() {
        throw GpioError("GpioLine::waitForEvent: not supported on this platform");
        co_return GpioEvent{};
    }

    void GpioLine::close() noexcept {}

#endif

}
