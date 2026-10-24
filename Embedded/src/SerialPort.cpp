//
// Created by Fabrizio Paino on 2026-05-16.
//
// MANWE_UNTESTED_PLATFORM: see SerialPort.h header. POSIX termios
// setup and Reactor-driven read/write; not validated against actual
// hardware in the session that introduced this file.
//

#include "SerialPort.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#if defined(_WIN32)
// Windows path: not implemented in this round.
#else
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/sysmacros.h>
#endif
#include <termios.h>
#include <unistd.h>
#endif

#include "IoAwaiters.h"

namespace Embedded {

#if defined(_WIN32)

    SerialPort SerialPort::open(const std::string &, const SerialConfig &) {
        throw SerialPortError(
            "SerialPort on Windows requires the OVERLAPPED / CreateFile path; "
            "not implemented in this round.");
    }

    YarnBall::Task<std::size_t> SerialPort::read(std::span<std::byte>) {
        throw SerialPortError("SerialPort on Windows not implemented");
        co_return 0;
    }

    YarnBall::Task<std::size_t> SerialPort::write(std::span<const std::byte>) {
        throw SerialPortError("SerialPort on Windows not implemented");
        co_return 0;
    }

    void SerialPort::close() noexcept {}

#else

    namespace {
        /**
         * @brief Map an integer baud rate to the corresponding termios
         *        @c B* constant. Returns @c 0 (B0) if the rate is not
         *        supported on this platform. Hardware drivers vary in
         *        which rates they expose; every standard POSIX
         *        @c B* constant the platform defines is accepted (the
         *        rates from 1200 to 230400 everywhere, the rest where
         *        <termios.h> provides them).
         */
        speed_t mapBaud(int baudRate) {
            switch (baudRate) {
#define SERIAL_BAUD_CASE(rate) \
                case rate: return B##rate;
#ifdef B50
                SERIAL_BAUD_CASE(50)
#endif
#ifdef B75
                SERIAL_BAUD_CASE(75)
#endif
#ifdef B110
                SERIAL_BAUD_CASE(110)
#endif
#ifdef B134
                SERIAL_BAUD_CASE(134)
#endif
#ifdef B150
                SERIAL_BAUD_CASE(150)
#endif
#ifdef B200
                SERIAL_BAUD_CASE(200)
#endif
#ifdef B300
                SERIAL_BAUD_CASE(300)
#endif
#ifdef B600
                SERIAL_BAUD_CASE(600)
#endif
                SERIAL_BAUD_CASE(1200)
                SERIAL_BAUD_CASE(1800)
                SERIAL_BAUD_CASE(2400)
                SERIAL_BAUD_CASE(4800)
                SERIAL_BAUD_CASE(9600)
                SERIAL_BAUD_CASE(19200)
                SERIAL_BAUD_CASE(38400)
                SERIAL_BAUD_CASE(57600)
                SERIAL_BAUD_CASE(115200)
                SERIAL_BAUD_CASE(230400)
#ifdef B460800
                SERIAL_BAUD_CASE(460800)
#endif
#ifdef B500000
                SERIAL_BAUD_CASE(500000)
#endif
#ifdef B576000
                SERIAL_BAUD_CASE(576000)
#endif
#ifdef B921600
                SERIAL_BAUD_CASE(921600)
#endif
#ifdef B1000000
                SERIAL_BAUD_CASE(1000000)
#endif
#ifdef B1152000
                SERIAL_BAUD_CASE(1152000)
#endif
#ifdef B1500000
                SERIAL_BAUD_CASE(1500000)
#endif
#ifdef B2000000
                SERIAL_BAUD_CASE(2000000)
#endif
#ifdef B2500000
                SERIAL_BAUD_CASE(2500000)
#endif
#ifdef B3000000
                SERIAL_BAUD_CASE(3000000)
#endif
#ifdef B3500000
                SERIAL_BAUD_CASE(3500000)
#endif
#ifdef B4000000
                SERIAL_BAUD_CASE(4000000)
#endif
#undef SERIAL_BAUD_CASE
                default: return B0;
            }
        }

        /**
         * @brief True if @p fd is a pseudo-terminal. A pty cannot carry a character size or parity (Linux forces it to
         *        8 bits, no parity), so those settings cannot be read back from one.
         */
        bool isPseudoTerminal(int fd) {
#if defined(__linux__)
            struct stat st{};
            // Unix98 pty slaves: character devices with majors 136..143 (UNIX98_PTY_SLAVE_MAJOR).
            return ::fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) >= 136 && major(st.st_rdev) <= 143;
#else
            (void) fd;
            return false;
#endif
        }

        /**
         * @brief Apply a @c SerialConfig to an open fd via termios.
         *        Throws on tcgetattr / tcsetattr failure, on an
         *        unsupported parameter, or when the driver did not take
         *        the requested settings (read back and compared).
         */
        void applyConfig(int fd, const SerialConfig &cfg) {
            termios tio{};
            if (::tcgetattr(fd, &tio) < 0) {
                throw SerialPortError(std::string("tcgetattr: ") +
                                       std::strerror(errno));
            }
            ::cfmakeraw(&tio);

            const speed_t baud = mapBaud(cfg.baudRate);
            if (baud == B0) {
                throw SerialPortError("unsupported baud rate: " +
                                       std::to_string(cfg.baudRate));
            }
            if (::cfsetispeed(&tio, baud) < 0 ||
                ::cfsetospeed(&tio, baud) < 0) {
                throw SerialPortError(std::string("cfsetspeed: ") +
                                       std::strerror(errno));
            }

            // Data bits.
            tio.c_cflag &= ~CSIZE;
            switch (cfg.dataBits) {
                case 5: tio.c_cflag |= CS5; break;
                case 6: tio.c_cflag |= CS6; break;
                case 7: tio.c_cflag |= CS7; break;
                case 8: tio.c_cflag |= CS8; break;
                default:
                    throw SerialPortError("unsupported data bits: " +
                                           std::to_string(cfg.dataBits));
            }

            // Parity.
            switch (cfg.parity) {
                case Parity::None:
                    tio.c_cflag &= ~PARENB;
                    tio.c_iflag &= ~(INPCK | IGNPAR | PARMRK);
                    break;
                case Parity::Even:
                    tio.c_cflag |= PARENB;
                    tio.c_cflag &= ~PARODD;
                    break;
                case Parity::Odd:
                    tio.c_cflag |= PARENB;
                    tio.c_cflag |= PARODD;
                    break;
            }
            // cfmakeraw clears INPCK, which would leave parity generated on TX but never checked on RX. Policy:
            // check it, and drop a byte that fails (IGNPAR) rather than deliver it as data or as a 0x00 / marker.
            if (cfg.parity != Parity::None) {
                tio.c_iflag |= (INPCK | IGNPAR);
                tio.c_iflag &= ~PARMRK;
            }

            // Stop bits.
            if (cfg.stopBits == StopBits::Two) tio.c_cflag |= CSTOPB;
            else                                 tio.c_cflag &= ~CSTOPB;

            // Flow control.
#ifdef CRTSCTS
            if (cfg.flowControl == FlowControl::Hardware) tio.c_cflag |= CRTSCTS;
            else                                            tio.c_cflag &= ~CRTSCTS;
#else
            if (cfg.flowControl == FlowControl::Hardware) {
                throw SerialPortError("hardware flow control (CRTSCTS) is not supported on this platform");
            }
#endif
            if (cfg.flowControl == FlowControl::Software) tio.c_iflag |= (IXON | IXOFF);
            else                                            tio.c_iflag &= ~(IXON | IXOFF);

            // Always-on flags: ignore modem-status lines, enable receiver.
            tio.c_cflag |= (CLOCAL | CREAD);

            // Non-canonical reads with VMIN=1 / VTIME=0: on an O_NONBLOCK
            // fd an empty read fails with EAGAIN (so the Reactor wait is
            // reached) instead of returning 0, which would look like EOF.
            tio.c_cc[VMIN] = 1;
            tio.c_cc[VTIME] = 0;

            if (::tcsetattr(fd, TCSANOW, &tio) < 0) {
                throw SerialPortError(std::string("tcsetattr: ") +
                                       std::strerror(errno));
            }

            // tcsetattr reports success if any one requested change took effect, so check what the driver kept.
            termios got{};
            if (::tcgetattr(fd, &got) < 0) {
                throw SerialPortError(std::string("tcgetattr: ") +
                                       std::strerror(errno));
            }
            // A pseudo-terminal has no UART behind it: it keeps 8 bits and no parity whatever was asked, so those are
            // not compared there. Every real device is compared in full.
            tcflag_t mask = CSTOPB;
            if (!isPseudoTerminal(fd)) mask |= CSIZE | PARENB | PARODD;
#ifdef CRTSCTS
            mask |= CRTSCTS;
#endif
            tcflag_t imask = IXON | IXOFF;
            if (!isPseudoTerminal(fd)) imask |= INPCK | IGNPAR | PARMRK;
            if ((got.c_cflag & mask) != (tio.c_cflag & mask) || (got.c_iflag & imask) != (tio.c_iflag & imask) ||
                ::cfgetispeed(&got) != baud || ::cfgetospeed(&got) != baud) {
                throw SerialPortError("the serial driver did not accept the requested line settings");
            }
        }
    }

    SerialPort SerialPort::open(const std::string &path,
                                  const SerialConfig &cfg) {
        int fd = -1;
        // EBUSY: a port closed an instant ago can still be held exclusively (TIOCEXCL) by a read / write in flight
        // on it, whose descriptor is released when that call leaves (see FdSlot). Give it a few ms to do so.
        for (int attempt = 0;; ++attempt) {
            fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0 || errno != EBUSY || attempt >= 20) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (fd < 0) {
            throw SerialPortError("open(" + path + "): " +
                                   std::strerror(errno));
        }
#ifdef TIOCEXCL
        // Best effort: keep other (non-root) processes from opening the same device and interleaving bytes.
        (void) ::ioctl(fd, TIOCEXCL);
#endif
        try {
            applyConfig(fd, cfg);
        } catch (...) {
            ::close(fd);
            throw;
        }
        // Drop whatever the previous user left queued in the driver, now that the line settings are in force.
        (void) ::tcflush(fd, TCIOFLUSH);
        return SerialPort(fd);
    }

    YarnBall::Task<std::size_t> SerialPort::read(std::span<std::byte> buf) {
        // Copies the shared state now, not when the lazy task first runs, so the task may outlive this SerialPort.
        return readOn(this->slot, buf);
    }

    YarnBall::Task<std::size_t> SerialPort::write(std::span<const std::byte> data) {
        return writeOn(this->slot, data);
    }

    YarnBall::Task<std::size_t> SerialPort::readOn(std::shared_ptr<detail::FdSlot> slot, std::span<std::byte> buf) {
        // The guard pins the fd for the whole call: close() retires it but the OS descriptor is released only after the
        // guard is gone, so the number cannot be reused under a syscall below.
        detail::FdSlot::Guard guard(std::move(slot));
        const int fd = guard.fd();
        if (fd < 0) {
            throw SerialPortError("read on closed SerialPort");
        }
        while (true) {
            ::ssize_t n;
            do {
                n = ::read(fd, buf.data(), buf.size());
            } while (n < 0 && errno == EINTR);
            if (n >= 0) co_return static_cast<std::size_t>(n);
            const int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                co_await guard.readable();
                if (!guard.open()) {
                    throw SerialPortError("SerialPort closed during read");
                }
                continue;
            }
            throw SerialPortError(std::string("read: ") + std::strerror(err));
        }
    }

    YarnBall::Task<std::size_t> SerialPort::writeOn(std::shared_ptr<detail::FdSlot> slot,
                                                    std::span<const std::byte> data) {
        detail::FdSlot::Guard guard(std::move(slot));
        const int fd = guard.fd();
        if (fd < 0) {
            throw SerialPortError("write on closed SerialPort");
        }
        while (true) {
            ::ssize_t n;
            do {
                n = ::write(fd, data.data(), data.size());
            } while (n < 0 && errno == EINTR);
            if (n >= 0) co_return static_cast<std::size_t>(n);
            const int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                co_await guard.writable();
                if (!guard.open()) {
                    throw SerialPortError("SerialPort closed during write");
                }
                continue;
            }
            throw SerialPortError(std::string("write: ") + std::strerror(err));
        }
    }

    void SerialPort::close() noexcept {
        // Publishes "closed", wakes a read()/write() parked on the fd (it sees the closed port on resume instead of
        // hanging) and releases the descriptor once no call is in flight.
        if (this->slot) this->slot->close();
    }

#endif

}
