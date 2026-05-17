//
// Created by Fabrizio Paino on 2026-05-16.
//
// MANWE_UNTESTED_PLATFORM: see SerialPort.h header. POSIX termios
// setup and Reactor-driven read/write; not validated against actual
// hardware in the session that introduced this file.
//

#include "SerialPort.h"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
// Windows path: not implemented in this round.
#else
#include <fcntl.h>
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
         *        which rates they expose; the common range below is
         *        accepted on both macOS and Linux.
         */
        speed_t mapBaud(int baudRate) {
            switch (baudRate) {
                case 1200:   return B1200;
                case 1800:   return B1800;
                case 2400:   return B2400;
                case 4800:   return B4800;
                case 9600:   return B9600;
                case 19200:  return B19200;
                case 38400:  return B38400;
                case 57600:  return B57600;
                case 115200: return B115200;
                case 230400: return B230400;
#ifdef B460800
                case 460800: return B460800;
#endif
#ifdef B921600
                case 921600: return B921600;
#endif
                default: return B0;
            }
        }

        /**
         * @brief Apply a @c SerialConfig to an open fd via termios.
         *        Throws on tcgetattr / tcsetattr failure or on an
         *        unsupported parameter (only baud rate currently can
         *        be rejected).
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

            // Stop bits.
            if (cfg.stopBits == StopBits::Two) tio.c_cflag |= CSTOPB;
            else                                 tio.c_cflag &= ~CSTOPB;

            // Flow control.
#ifdef CRTSCTS
            if (cfg.flowControl == FlowControl::Hardware) tio.c_cflag |= CRTSCTS;
            else                                            tio.c_cflag &= ~CRTSCTS;
#endif
            if (cfg.flowControl == FlowControl::Software) tio.c_iflag |= (IXON | IXOFF);
            else                                            tio.c_iflag &= ~(IXON | IXOFF);

            // Always-on flags: ignore modem-status lines, enable receiver.
            tio.c_cflag |= (CLOCAL | CREAD);

            // Non-canonical reads with VMIN=0 / VTIME=0 so read returns
            // whatever is available (matching the Reactor-driven model).
            tio.c_cc[VMIN] = 0;
            tio.c_cc[VTIME] = 0;

            if (::tcsetattr(fd, TCSANOW, &tio) < 0) {
                throw SerialPortError(std::string("tcsetattr: ") +
                                       std::strerror(errno));
            }
        }
    }

    SerialPort SerialPort::open(const std::string &path,
                                  const SerialConfig &cfg) {
        const int fd = ::open(path.c_str(),
                                O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            throw SerialPortError("open(" + path + "): " +
                                   std::strerror(errno));
        }
        try {
            applyConfig(fd, cfg);
        } catch (...) {
            ::close(fd);
            throw;
        }
        return SerialPort(fd);
    }

    YarnBall::Task<std::size_t> SerialPort::read(std::span<std::byte> buf) {
        if (this->descriptor < 0) {
            throw SerialPortError("read on closed SerialPort");
        }
        while (true) {
            ::ssize_t n;
            do {
                n = ::read(this->descriptor, buf.data(), buf.size());
            } while (n < 0 && errno == EINTR);
            if (n >= 0) co_return static_cast<std::size_t>(n);
            const int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                co_await YarnBall::io::waitReadable(this->descriptor);
                continue;
            }
            throw SerialPortError(std::string("read: ") + std::strerror(err));
        }
    }

    YarnBall::Task<std::size_t> SerialPort::write(std::span<const std::byte> data) {
        if (this->descriptor < 0) {
            throw SerialPortError("write on closed SerialPort");
        }
        while (true) {
            ::ssize_t n;
            do {
                n = ::write(this->descriptor, data.data(), data.size());
            } while (n < 0 && errno == EINTR);
            if (n >= 0) co_return static_cast<std::size_t>(n);
            const int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                co_await YarnBall::io::waitWritable(this->descriptor);
                continue;
            }
            throw SerialPortError(std::string("write: ") + std::strerror(err));
        }
    }

    void SerialPort::close() noexcept {
        if (this->descriptor >= 0) {
            (void) ::close(this->descriptor);
            this->descriptor = -1;
        }
    }

#endif

}
