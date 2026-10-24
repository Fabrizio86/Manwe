//
// Regression tests for the Embedded module: the in-flight protocol that keeps a serial port's descriptor open while an
// operation is parked on it (FdSlot), and close() waking a parked reader. A pseudo-terminal stands in for hardware.
//
// POSIX only (pty); the Windows serial path is a stub.
//

#include "test_framework.hpp"

#ifndef _WIN32

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#if defined(__linux__)
#include <pty.h>
#else
#include <util.h>
#endif

#include "../Embedded/includes/FdSlot.h"
#include "../Embedded/includes/SerialPort.h"
#include "../Yarn/includes/Coroutines.h"
#include "../Yarn/includes/JoinHandle.h"
#include "../Yarn/includes/Yarn.hpp"

using namespace std::chrono_literals;

namespace {
    bool embIsOpen(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

    template<typename Pred>
    bool embWaitUntil(Pred p, std::chrono::milliseconds limit) {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < end) {
            if (p()) return true;
            std::this_thread::sleep_for(2ms);
        }
        return p();
    }

    /// A pseudo-terminal: the "device" side that SerialPort opens by name, and the master that plays the hardware.
    struct Pty {
        int master = -1;
        int slave = -1;
        char name[128] = {};

        Pty() {
            if (::openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) throw std::runtime_error("openpty failed");
            if (::ttyname_r(slave, name, sizeof name) != 0) throw std::runtime_error("ttyname_r failed");
        }
        Pty(const Pty &) = delete;
        Pty &operator=(const Pty &) = delete;
        ~Pty() {
            if (slave >= 0) ::close(slave);
            if (master >= 0) ::close(master);
        }

        /// A pty keeps its exclusive flag while the master is open; clear it so a port can be reopened by name.
        Embedded::SerialPort open() {
            for (int i = 0;; ++i) {
                try {
                    (void) ::ioctl(slave, TIOCNXCL);
                    return Embedded::SerialPort::open(name);
                } catch (const Embedded::SerialPortError &) {
                    if (i > 500) throw;
                    std::this_thread::sleep_for(2ms);
                }
            }
        }
    };

    /// Reads once. 1 = got a byte, -1 = the port was closed under the read.
    YarnBall::Task<int> embReadOnce(Embedded::SerialPort *p, char *out) {
        std::array<std::byte, 16> b{};
        try {
            const auto n = co_await p->read(b);
            out[0] = static_cast<char>(b[0]);
            co_return static_cast<int>(n);
        } catch (const Embedded::SerialPortError &) {
            co_return -1;
        }
    }
}

TEST(audit_embedded_fd_slot_keeps_the_descriptor_open_while_a_guard_is_in_flight) {
    int fds[2];
    EXPECT_EQ(::pipe(fds), 0);
    auto slot = std::make_shared<Embedded::detail::FdSlot>(fds[0]);
    {
        Embedded::detail::FdSlot::Guard g(slot);
        EXPECT_TRUE(g.fd() == fds[0] && g.open());
        slot->close();
        EXPECT_FALSE(g.open());
        EXPECT_TRUE(embIsOpen(fds[0])); // not yet returned to the OS
        EXPECT_EQ(slot->get(), -1);
        Embedded::detail::FdSlot::Guard late(slot);
        EXPECT_EQ(late.fd(), -1); // new operations see it closed
    }
    EXPECT_FALSE(embIsOpen(fds[0])); // released by the last leaver
    slot->close();                   // idempotent
    ::close(fds[1]);

    int fds2[2];
    EXPECT_EQ(::pipe(fds2), 0);
    auto idle = std::make_shared<Embedded::detail::FdSlot>(fds2[0]);
    idle->close(); // nothing in flight: closed at once
    EXPECT_FALSE(embIsOpen(fds2[0]));
    ::close(fds2[1]);
}

TEST(audit_embedded_serial_port_moves_data_and_close_wakes_a_parked_reader) {
    Pty pty;
    auto port = pty.open();
    const int pfd = port.fd();

    char c = 0;
    EXPECT_EQ(::write(pty.master, "x", 1), 1);
    EXPECT_TRUE(YarnBall::syncWait(embReadOnce(&port, &c)) == 1 && c == 'x');

    const std::array<std::byte, 2> out{std::byte{'h'}, std::byte{'i'}};
    EXPECT_EQ(YarnBall::syncWait(port.write(out)), std::size_t(2));
    char got[4] = {};
    pollfd p{pty.master, POLLIN, 0};
    (void) ::poll(&p, 1, 2000);
    EXPECT_EQ(::read(pty.master, got, 4), 2);

    // A reader parked in the Reactor is woken by close() from another thread and fails; the descriptor is released.
    auto h = YarnBall::spawnJoinable(embReadOnce(&port, &c));
    std::this_thread::sleep_for(100ms);
    port.close();
    EXPECT_EQ(YarnBall::syncWait(h.join()), -1);
    EXPECT_TRUE(embWaitUntil([&] { return !embIsOpen(pfd); }, 1s));
    EXPECT_EQ(port.fd(), -1);
}

TEST(audit_embedded_serial_port_destroyed_with_a_parked_reader_releases_the_descriptor) {
    Pty pty;
    auto port = std::make_unique<Embedded::SerialPort>(pty.open());
    const int fd = port->fd();
    char c = 0;
    auto h = YarnBall::spawnJoinable(embReadOnce(port.get(), &c));
    std::this_thread::sleep_for(100ms);
    port->close();
    EXPECT_EQ(YarnBall::syncWait(h.join()), -1);
    port.reset();
    EXPECT_TRUE(embWaitUntil([&] { return !embIsOpen(fd); }, 1s));
}

TEST(audit_embedded_serial_port_close_racing_a_wake_never_closes_an_unrelated_descriptor) {
    // The reader is woken by data at the very moment close() runs. The descriptor number may be reused by the
    // kernel right after: a descriptor opened by someone else must never be closed by the port's late cleanup.
    Pty pty;
    for (int i = 0; i < 300; ++i) {
        auto port = pty.open();
        char c = 0;
        auto h = YarnBall::spawnJoinable(embReadOnce(&port, &c));
        if (i % 2) EXPECT_EQ(::write(pty.master, "y", 1), 1);
        port.close();
        const int other = ::open("/dev/null", O_RDONLY);
        const int r = YarnBall::syncWait(h.join());
        EXPECT_TRUE(r == -1 || r == 1);
        EXPECT_TRUE(embIsOpen(other));
        ::close(other);
        ::tcflush(pty.master, TCIOFLUSH);
    }
}

#endif // !_WIN32
