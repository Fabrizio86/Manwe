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
        // Read-only is enough: the line ioctls need no write access to the chip node (the kernel's own gpio tools
        // open it with flags 0).
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
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
        return GpioChip(fd, info.lines);
    }

    void GpioChip::checkPin(const char *what, int pin) const {
        if (pin < 0) throw GpioError(std::string(what) + ": negative pin");
        if (static_cast<std::uint32_t>(pin) >= this->lineCount) {
            throw GpioError(std::string(what) + ": pin " + std::to_string(pin) + " is out of range (chip has " +
                             std::to_string(this->lineCount) + " lines)");
        }
    }

    void GpioChip::close() noexcept {
        if (this->slot) this->slot->close();
    }

    GpioLine GpioChip::requestOutput(int pin, bool initialHigh) {
        // The guard keeps the chip descriptor from being released (and its number reused) under the ioctl below.
        detail::FdSlot::Guard guard(this->slot);
        const int chipFd = guard.fd();
        if (chipFd < 0) {
            throw GpioError("requestOutput on closed GpioChip");
        }
        this->checkPin("requestOutput", pin);
        struct gpiohandle_request req{};
        req.lineoffsets[0] = static_cast<std::uint32_t>(pin);
        req.flags = GPIOHANDLE_REQUEST_OUTPUT;
        req.default_values[0] = initialHigh ? 1 : 0;
        req.lines = 1;
        std::strncpy(req.consumer_label, "manwe-out",
                      sizeof(req.consumer_label) - 1);
        if (::ioctl(chipFd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
            throw GpioError(std::string("GPIO_GET_LINEHANDLE_IOCTL: ") +
                             std::strerror(errno));
        }
        return GpioLine(req.fd, /*output=*/true);
    }

    GpioLine GpioChip::requestInputEdge(int pin, Edge edge) {
        detail::FdSlot::Guard guard(this->slot);
        const int chipFd = guard.fd();
        if (chipFd < 0) {
            throw GpioError("requestInputEdge on closed GpioChip");
        }
        this->checkPin("requestInputEdge", pin);
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
        if (::ioctl(chipFd, GPIO_GET_LINEEVENT_IOCTL, &req) < 0) {
            throw GpioError(std::string("GPIO_GET_LINEEVENT_IOCTL: ") +
                             std::strerror(errno));
        }
        // The event fd is blocking by default; make it non-blocking so a wake-up with nothing to read cannot park a
        // pool worker inside read().
        const int flags = ::fcntl(req.fd, F_GETFL);
        if (flags < 0 || ::fcntl(req.fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            const int e = errno;
            ::close(req.fd);
            throw GpioError(std::string("fcntl(O_NONBLOCK): ") + std::strerror(e));
        }
        return GpioLine(req.fd, /*output=*/false);
    }

    void GpioLine::set(bool high) {
        detail::FdSlot::Guard guard(this->slot);
        const int fd = guard.fd();
        if (fd < 0) {
            throw GpioError("set on closed GpioLine");
        }
        if (!this->isOutput) {
            throw GpioError("set called on input-configured GpioLine");
        }
        struct gpiohandle_data data{};
        data.values[0] = high ? 1 : 0;
        if (::ioctl(fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0) {
            throw GpioError(std::string("GPIOHANDLE_SET_LINE_VALUES_IOCTL: ") +
                             std::strerror(errno));
        }
    }

    YarnBall::Task<GpioEvent> GpioLine::waitForEvent() {
        // Copies the shared state now, not when the lazy task first runs, so the task may outlive this GpioLine.
        return waitForEventOn(this->slot, this->isOutput);
    }

    YarnBall::Task<GpioEvent> GpioLine::waitForEventOn(std::shared_ptr<detail::FdSlot> slot, bool output) {
        // The guard pins the fd until this call is done: close() retires it but releases the descriptor only after.
        detail::FdSlot::Guard guard(std::move(slot));
        const int fd = guard.fd();
        if (fd < 0) {
            throw GpioError("waitForEvent called on closed GpioLine");
        }
        if (output) {
            throw GpioError("waitForEvent called on output-configured GpioLine");
        }

        struct gpioevent_data raw{};
        while (true) {
            ::ssize_t n;
            do {
                n = ::read(fd, &raw, sizeof(raw));
            } while (n < 0 && errno == EINTR);
            if (n >= 0) {
                if (n < static_cast<::ssize_t>(sizeof(raw))) {
                    throw GpioError("read(gpioevent_data): short read");
                }
                break;
            }
            const int err = errno;
            if (err != EAGAIN && err != EWOULDBLOCK) {
                throw GpioError(std::string("read(gpioevent_data): ") + std::strerror(err));
            }
            co_await guard.readable();
            if (!guard.open()) {
                throw GpioError("GpioLine closed while waiting for an event");
            }
        }
        GpioEvent ev{};
        ev.timestampNs = raw.timestamp;
        ev.edge = (raw.id == GPIOEVENT_EVENT_RISING_EDGE)
                  ? Edge::Rising : Edge::Falling;
        co_return ev;
    }

    void GpioLine::close() noexcept {
        // Wakes a waitForEvent() parked on this line (it reports the closed line instead of hanging) and releases the
        // descriptor once no call is in flight.
        if (this->slot) this->slot->close();
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

    YarnBall::Task<GpioEvent> GpioLine::waitForEventOn(std::shared_ptr<detail::FdSlot>, bool) {
        throw GpioError("GpioLine::waitForEvent: not supported on this platform");
        co_return GpioEvent{};
    }

    void GpioLine::close() noexcept {}

#endif

}
