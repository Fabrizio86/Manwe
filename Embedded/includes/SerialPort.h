//
// Created by Fabrizio Paino on 2026-05-16.
//
// SerialPort -- async UART / USB-CDC I/O on top of the Yarn Reactor.
//
// MANWE_UNTESTED_PLATFORM: this header and its .cpp ship without a
// validation run in the session that introduced them, because the
// dev box (Apple M1 Max) has no USB-serial dongle attached to verify
// the syscalls end-to-end. The termios setup is the canonical POSIX
// pattern; the read/write loop is shared with TcpStream so the async
// suspension path is already proven. Validate before depending on
// this in production.
//
// Typical use:
//
//     auto port = Embedded::SerialPort::open("/dev/ttyUSB0");
//     std::array<std::byte, 64> buf{};
//     std::size_t n = co_await port.read(buf);
//
// Supported platforms in this round:
//  - macOS (Apple Silicon and Intel)
//  - Linux (kernel >= 3.10; new termios2 path not used in v1)
//  - Windows: throws at construction. Win32 COM ports go through
//    OVERLAPPED I/O on CreateFile handles, not the POSIX termios
//    model; a follow-up round will implement that path.
//

#ifndef EMBEDDED_SERIAL_PORT_H
#define EMBEDDED_SERIAL_PORT_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "Coroutines.h"

namespace Embedded {

    /**
     * @enum Parity
     * @brief Per-byte parity-checking mode.
     */
    enum class Parity { None, Even, Odd };

    /**
     * @enum StopBits
     * @brief Number of stop bits framing each byte.
     */
    enum class StopBits { One, Two };

    /**
     * @enum FlowControl
     * @brief Flow-control discipline. Hardware = RTS/CTS, Software =
     *        XON/XOFF.
     */
    enum class FlowControl { None, Hardware, Software };

    /**
     * @struct SerialConfig
     * @brief Inseparable line-discipline parameters. Defaults to
     *        @c 115200-8N1 no flow -- the modal Arduino / FTDI dongle
     *        configuration. Per project policy, options structs are
     *        avoided unless the underlying OS API requires multiple
     *        inputs together; termios genuinely does, so this struct
     *        exists.
     */
    struct SerialConfig {
        int baudRate = 115200;
        int dataBits = 8;
        Parity parity = Parity::None;
        StopBits stopBits = StopBits::One;
        FlowControl flowControl = FlowControl::None;
    };

    /**
     * @class SerialPortError
     * @brief Thrown on open / configure / read / write failure.
     */
    class SerialPortError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /**
     * @class SerialPort
     * @brief Coroutine-driven UART / USB-CDC endpoint. Move-only.
     *
     * Read/write semantics match @c TcpStream: the fd is opened
     * non-blocking, the awaiter suspends through the Reactor's
     * @c waitReadable / @c waitWritable, and the syscall is retried
     * on EINTR / EAGAIN.
     */
    class SerialPort final {
    public:
        /**
         * @brief Open and configure a serial device at @p path.
         *        Defaults to 115200-8N1 no flow control. Throws
         *        @c SerialPortError if the device cannot be opened
         *        or the requested config is unsupported by the
         *        kernel driver.
         */
        static SerialPort open(const std::string &path,
                                const SerialConfig &cfg = {});

        SerialPort(const SerialPort &) = delete;
        SerialPort &operator=(const SerialPort &) = delete;

        SerialPort(SerialPort &&other) noexcept
            : descriptor(std::exchange(other.descriptor, -1)) {
        }

        SerialPort &operator=(SerialPort &&other) noexcept {
            if (this != &other) {
                this->close();
                this->descriptor = std::exchange(other.descriptor, -1);
            }
            return *this;
        }

        ~SerialPort() { this->close(); }

        /**
         * @brief Read up to @c buf.size() bytes from the port,
         *        suspending until data is available. Returns the
         *        actual number of bytes read (may be smaller than
         *        the buffer, especially for line-mode devices).
         *
         * @throws SerialPortError on a fatal syscall failure
         *         (EBADF, ENXIO, etc.).
         */
        YarnBall::Task<std::size_t> read(std::span<std::byte> buf);

        /**
         * @brief Write up to @c data.size() bytes to the port,
         *        suspending on writability when the kernel's TX
         *        buffer is full. Short writes are possible; the
         *        caller drives any total-bytes loop.
         *
         * @throws SerialPortError on a fatal syscall failure.
         */
        YarnBall::Task<std::size_t> write(std::span<const std::byte> data);

        /**
         * @return Underlying fd, or -1 if closed.
         */
        int fd() const noexcept { return this->descriptor; }

        /**
         * @brief Close the port. Idempotent.
         */
        void close() noexcept;

    private:
        explicit SerialPort(int fd) noexcept : descriptor(fd) {
        }

        int descriptor = -1;
    };

}

#endif // EMBEDDED_SERIAL_PORT_H
