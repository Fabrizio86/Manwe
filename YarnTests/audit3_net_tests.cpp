//
// Regression tests for the third audit pass (net).
//

#include "test_framework.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "../Yarn/includes/Coroutines.h"
#include "../Yarn/includes/Timeout.h"
#include "../Yarn/includes/Timers.h"
#include "../Soccer/includes/Soccer.h"

#ifndef _WIN32
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

    template<typename Pred>
    bool waitUntil3(Pred &&p, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!p()) {
            if (std::chrono::steady_clock::now() >= deadline) return p();
            std::this_thread::sleep_for(2ms);
        }
        return true;
    }

    std::span<const std::byte> bytes3(const char *s, std::size_t n) {
        return {reinterpret_cast<const std::byte *>(s), n};
    }

    bool acceptThrows(Soccer::TcpListener &l, std::string *what = nullptr) {
        try {
            (void) YarnBall::syncWait(YarnBall::withTimeout(l.accept(), 2s));
        } catch (const Soccer::SocketException &e) {
            if (what) *what = e.what();
            return true;
        }
        return false;
    }

} // namespace

TEST(audit3_net_deadline_saturates_instead_of_overflowing) {
    const auto now = std::chrono::steady_clock::now();
    const auto farAway = Soccer::detail::deadlineAfter(std::chrono::milliseconds::max());
    EXPECT_TRUE(farAway >= now + std::chrono::hours{24 * 365});
    EXPECT_TRUE(farAway == std::chrono::steady_clock::time_point::max());
    const auto nearBy = Soccer::detail::deadlineAfter(500ms);
    EXPECT_TRUE(nearBy > now && nearBy < now + 1h);
    // The deadline is taken before the clock is read again (operand order of <= is unspecified, so keep them apart).
    const auto zero = Soccer::detail::deadlineAfter(0ms);
    EXPECT_TRUE(zero <= std::chrono::steady_clock::now());
    const auto negative = Soccer::detail::deadlineAfter(std::chrono::milliseconds::min());
    EXPECT_TRUE(negative <= std::chrono::steady_clock::now());
}

#ifndef _WIN32

namespace {
    /// Two connected non-blocking AF_UNIX sockets.
    void pair3(int sv[2]) {
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        ::fcntl(sv[0], F_SETFL, ::fcntl(sv[0], F_GETFL) | O_NONBLOCK);
        ::fcntl(sv[1], F_SETFL, ::fcntl(sv[1], F_GETFL) | O_NONBLOCK);
    }
}

TEST(audit3_net_io_deadline_never_shuts_down_a_number_reused_after_the_stream_died) {
    int sv[2];
    pair3(sv);
    const int oldFd = sv[0];
    Soccer::IoDeadline d;
    {
        Soccer::TcpStream a(sv[0]);
        d.arm(a, 200ms);
    } // the stream dies, its watch expires, the descriptor is closed
    // The lowest free number goes to an unrelated socket pair.
    int sv2[2];
    pair3(sv2);
    EXPECT_TRUE(sv2[0] == oldFd || sv2[1] == oldFd);
    std::this_thread::sleep_for(450ms); // the deadline passes meanwhile
    const char c = 'x';
    EXPECT_EQ(::send(sv2[0], &c, 1, 0), 1); // a stray shutdown would make this EPIPE ...
    char got = 0;
    EXPECT_TRUE(waitUntil3([&] { return ::recv(sv2[1], &got, 1, 0) == 1; }, 2s)); // ... or this read EOF
    EXPECT_EQ(got, 'x');
    EXPECT_FALSE(d.expired()); // nothing was shut down
    ::close(sv[1]);
    ::close(sv2[0]);
    ::close(sv2[1]);
}

TEST(audit3_net_io_deadline_survives_a_stream_move_followed_by_the_death_of_the_moved_from) {
    int sv[2];
    pair3(sv);
    Soccer::TcpStream b;
    Soccer::IoDeadline d;
    {
        Soccer::TcpStream a(sv[0]);
        d.arm(a, 150ms);
        b = Soccer::TcpStream(std::move(a));
    } // the moved-from stream is destroyed; b owns the descriptor
    std::this_thread::sleep_for(400ms);
    // Moving detached the deadline (see IoDeadline): the live socket is untouched, not shut down by raw number.
    const char c = 'y';
    EXPECT_EQ(::send(sv[1], &c, 1, 0), 1);
    std::array<std::byte, 4> buf{};
    EXPECT_EQ(YarnBall::syncWait(YarnBall::withTimeout(b.read(buf), 2s)), static_cast<std::size_t>(1));
    EXPECT_FALSE(d.expired());
    ::close(sv[1]);
}

TEST(audit3_net_io_deadline_with_a_huge_limit_does_not_fire) {
    int sv[2];
    pair3(sv);
    {
        Soccer::IoDeadline raw(sv[0], std::chrono::milliseconds::max());
        Soccer::IoDeadline cent(sv[0], std::chrono::hours{24LL * 365 * 300});
        Soccer::TcpStream s(sv[1]);
        Soccer::IoDeadline viaStream(s, std::chrono::milliseconds::max());
        std::this_thread::sleep_for(450ms); // used to wrap into the past and fire within one sweep
        EXPECT_FALSE(raw.expired());
        EXPECT_FALSE(cent.expired());
        EXPECT_FALSE(viaStream.expired());
        const char c = 'z';
        EXPECT_EQ(::send(sv[0], &c, 1, 0), 1);
    }
    ::close(sv[0]);
}

TEST(audit3_net_connect_with_a_huge_timeout_connects) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    auto a = YarnBall::syncWait(YarnBall::withTimeout(
        Soccer::tcpConnect("localhost", port, std::chrono::milliseconds::max()), 5s));
    EXPECT_TRUE(a.fd() >= 0);
    auto b = YarnBall::syncWait(YarnBall::withTimeout(
        Soccer::tcpConnect("127.0.0.1", port, std::chrono::milliseconds::max()), 5s));
    EXPECT_TRUE(b.fd() >= 0);

    const std::string path = (std::filesystem::temp_directory_path() /
                              ("a3_huge_" + std::to_string(::getpid()) + ".sock")).string();
    auto ul = Soccer::TcpListener::bindUnix(path);
    auto u = YarnBall::syncWait(YarnBall::withTimeout(
        Soccer::tcpConnectUnix(path, std::chrono::milliseconds::max()), 5s));
    EXPECT_TRUE(u.fd() >= 0);
    ul.close();
    ::unlink(path.c_str());
    ::unlink((path + ".lock").c_str());
}

#if defined(__linux__)
TEST(audit3_net_unix_connect_with_a_huge_busy_retry_keeps_waiting_for_room) {
    // A listener with an accept queue of one: after it is full, a non-blocking connect gets EAGAIN.
    const std::string path = (std::filesystem::temp_directory_path() /
                              ("a3_busy_" + std::to_string(::getpid()) + ".sock")).string();
    ::unlink(path.c_str());
    const int ls = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un u{};
    u.sun_family = AF_UNIX;
    std::strncpy(u.sun_path, path.c_str(), sizeof(u.sun_path) - 1);
    EXPECT_EQ(::bind(ls, reinterpret_cast<sockaddr *>(&u), sizeof(u)), 0);
    EXPECT_EQ(::listen(ls, 0), 0);
    ::fcntl(ls, F_SETFL, ::fcntl(ls, F_GETFL) | O_NONBLOCK); // the drain below must not block when the queue is empty
    std::vector<int> fillers;
    int lastErr = 0;
    for (int i = 0; i < 16 && lastErr == 0; ++i) {
        const int c = ::socket(AF_UNIX, SOCK_STREAM, 0);
        ::fcntl(c, F_SETFL, ::fcntl(c, F_GETFL) | O_NONBLOCK);
        if (::connect(c, reinterpret_cast<sockaddr *>(&u), sizeof(u)) != 0) lastErr = errno;
        fillers.push_back(c);
    }
    EXPECT_EQ(lastErr, EAGAIN);

    auto outcome = std::make_shared<std::atomic<int>>(0); // 1 = connected, 2 = failed
    YarnBall::coSpawn([](std::string p, std::shared_ptr<std::atomic<int>> out) -> YarnBall::Task<void> {
        try {
            auto s = co_await Soccer::tcpConnectUnix(p, std::chrono::milliseconds::max());
            out->store(1);
        } catch (...) {
            out->store(2);
        }
    }(path, outcome));
    std::this_thread::sleep_for(400ms);
    EXPECT_EQ(outcome->load(), 0); // used to give up at once: the deadline wrapped into the past
    // Make room; the retry then gets through.
    for (int i = 0; i < 4; ++i) {
        const int a = ::accept(ls, nullptr, nullptr);
        if (a >= 0) ::close(a);
    }
    EXPECT_TRUE(waitUntil3([&] { return outcome->load() != 0; }, 5s));
    EXPECT_EQ(outcome->load(), 1);
    for (int c : fillers) ::close(c);
    ::close(ls);
    ::unlink(path.c_str());
}
#endif

TEST(audit3_net_interrupt_accept_is_uniform_and_leaves_the_backlog_alone) {
    auto l = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = l.localAddress().port();
    auto c1 = YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port)); // queued before the interrupt
    EXPECT_TRUE(l.interruptAccept());

    std::string what;
    EXPECT_TRUE(acceptThrows(l, &what));
    EXPECT_TRUE(what.find("interrupted") != std::string::npos);

    // tryAccept is unaffected and returns the queued connection (it used to throw EINVAL on Linux).
    std::optional<Soccer::TcpStream> s1;
    EXPECT_TRUE(waitUntil3([&] { s1 = l.tryAccept(); return s1.has_value(); }, 2s));
    // A connection made after the interrupt still completes and is not reset.
    auto c2 = YarnBall::syncWait(YarnBall::withTimeout(Soccer::tcpConnect("127.0.0.1", port), 2s));
    std::optional<Soccer::TcpStream> s2;
    EXPECT_TRUE(waitUntil3([&] { s2 = l.tryAccept(); return s2.has_value(); }, 2s));
    EXPECT_EQ(YarnBall::syncWait(c2.write(bytes3("hi", 2))), static_cast<std::size_t>(2));
    std::array<std::byte, 8> buf{};
    EXPECT_EQ(YarnBall::syncWait(YarnBall::withTimeout(s2->read(buf), 2s)), static_cast<std::size_t>(2));
    // The first queued connection was not reset either.
    EXPECT_EQ(YarnBall::syncWait(c1.write(bytes3("a", 1))), static_cast<std::size_t>(1));
    EXPECT_EQ(YarnBall::syncWait(YarnBall::withTimeout(s1->read(buf), 2s)), static_cast<std::size_t>(1));
    // Closing a listener makes accept fail with the closed message, not the interrupt one.
    l.close();
    EXPECT_TRUE(acceptThrows(l, &what));
    EXPECT_TRUE(what.find("closed") != std::string::npos);
    EXPECT_FALSE(l.interruptAccept());
}

TEST(audit3_net_interrupt_flag_follows_the_listener_it_belongs_to) {
    // Move construction carries the flag.
    {
        auto l = Soccer::TcpListener::bind("127.0.0.1", 0);
        EXPECT_TRUE(l.interruptAccept());
        Soccer::TcpListener m(std::move(l));
        std::string what;
        EXPECT_TRUE(acceptThrows(m, &what));
        EXPECT_TRUE(what.find("interrupted") != std::string::npos);
    }
    // Move assignment of a fresh listener over an interrupted one clears it.
    {
        auto l = Soccer::TcpListener::bind("127.0.0.1", 0);
        EXPECT_TRUE(l.interruptAccept());
        l = Soccer::TcpListener::bind("127.0.0.1", 0);
        const std::uint16_t port = l.localAddress().port();
        auto client = YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port));
        auto server = YarnBall::syncWait(YarnBall::withTimeout(l.accept(), 2s));
        EXPECT_TRUE(server.fd() >= 0);
    }
    // Move assignment of an interrupted listener over a fresh one installs the flag.
    {
        auto fresh = Soccer::TcpListener::bind("127.0.0.1", 0);
        auto stopped = Soccer::TcpListener::bind("127.0.0.1", 0);
        EXPECT_TRUE(stopped.interruptAccept());
        fresh = std::move(stopped);
        std::string what;
        EXPECT_TRUE(acceptThrows(fresh, &what));
        EXPECT_TRUE(what.find("interrupted") != std::string::npos);
    }
    // The slot itself.
    {
        Soccer::detail::SocketSlot a;
        a.interrupt();
        Soccer::detail::SocketSlot b(std::move(a));
        EXPECT_TRUE(b.interrupted());
        Soccer::detail::SocketSlot c;
        c.interrupt();
        Soccer::detail::SocketSlot d;
        c.adopt(d);
        EXPECT_FALSE(c.interrupted());
    }
}

TEST(audit3_net_udp_connected_send_and_recv) {
    auto a = Soccer::UdpSocket::bind("127.0.0.1", 0);
    auto b = Soccer::UdpSocket::bind("127.0.0.1", 0);
    a.connect(b.localAddress());
    b.connect(a.localAddress());
    EXPECT_EQ(YarnBall::syncWait(a.send(bytes3("ping", 4))), static_cast<std::size_t>(4));
    std::array<std::byte, 16> buf{};
    bool cut = true;
    EXPECT_EQ(YarnBall::syncWait(YarnBall::withTimeout(b.recv(buf, &cut), 2s)), static_cast<std::size_t>(4));
    EXPECT_FALSE(cut);
    EXPECT_EQ(std::memcmp(buf.data(), "ping", 4), 0);
    EXPECT_EQ(YarnBall::syncWait(b.send(bytes3("pong!", 5))), static_cast<std::size_t>(5));
    std::array<std::byte, 3> small{};
    EXPECT_EQ(YarnBall::syncWait(YarnBall::withTimeout(a.recv(small, &cut), 2s)), static_cast<std::size_t>(3));
    EXPECT_TRUE(cut); // the datagram was longer than the buffer
    a.close();
    EXPECT_THROWS_AS(YarnBall::syncWait(a.send(bytes3("x", 1))), Soccer::SocketException);
    // recv parks like recvFrom and is woken by close.
    std::atomic<int> got{0};
    YarnBall::coSpawn([](Soccer::UdpSocket *s, std::atomic<int> *out) -> YarnBall::Task<void> {
        std::array<std::byte, 4> k{};
        try {
            (void) co_await s->recv(k);
            out->store(2);
        } catch (const Soccer::SocketException &) {
            out->store(1);
        }
    }(&b, &got));
    std::this_thread::sleep_for(100ms);
    b.close();
    EXPECT_TRUE(waitUntil3([&] { return got.load() != 0; }, 2s));
    EXPECT_EQ(got.load(), 1);
}

TEST(audit3_net_unnamed_unix_peer_prints_as_empty) {
    const std::string path = (std::filesystem::temp_directory_path() /
                              ("a3_unnamed_" + std::to_string(::getpid()) + ".sock")).string();
    auto l = Soccer::TcpListener::bindUnix(path);
    auto client = YarnBall::syncWait(Soccer::tcpConnectUnix(path));
    Soccer::SocketAddress peer;
    auto server = YarnBall::syncWait(YarnBall::withTimeout(l.accept(&peer), 2s));
    EXPECT_EQ(peer.to_string(), std::string());
#if !defined(__linux__)
    // macOS reports an unbound client as a full-size sockaddr_un with a zeroed path.
    sockaddr_un un{};
    un.sun_family = AF_UNIX;
    Soccer::SocketAddress full(reinterpret_cast<sockaddr *>(&un), static_cast<socklen_t>(sizeof(un)));
    EXPECT_EQ(full.to_string(), std::string());
#endif
    l.close();
    ::unlink(path.c_str());
    ::unlink((path + ".lock").c_str());
}

#endif // !_WIN32

TEST(audit3_net_empty_host_resolves_to_the_wildcard_address) {
    const auto all = Soccer::SocketAddress::resolveAll("", 4242);
    EXPECT_TRUE(!all.empty());
    for (const auto &a : all) {
        EXPECT_EQ(a.port(), 4242);
        const std::string s = a.to_string();
        EXPECT_TRUE(s == "0.0.0.0:4242" || s == "[::]:4242");
    }
    auto l = Soccer::TcpListener::bind("", 0);
    EXPECT_TRUE(l.localAddress().port() != 0);
    const std::string ls = l.localAddress().to_string();
    EXPECT_TRUE(ls.rfind("0.0.0.0:", 0) == 0 || ls.rfind("[::]:", 0) == 0);
    // And reachable through the loopback.
    auto c = YarnBall::syncWait(YarnBall::withTimeout(
        Soccer::tcpConnect("127.0.0.1", l.localAddress().port()), 2s));
    EXPECT_TRUE(c.fd() >= 0);
}

TEST(audit3_net_tcp_shutdown_write_is_a_half_close) {
    auto l = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = l.localAddress().port();
    auto client = YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port));
    auto server = YarnBall::syncWait(YarnBall::withTimeout(l.accept(), 2s));
    EXPECT_EQ(YarnBall::syncWait(client.write(bytes3("req", 3))), static_cast<std::size_t>(3));
    client.shutdownWrite();
    client.shutdownWrite(); // idempotent
    std::array<std::byte, 16> buf{};
    EXPECT_EQ(YarnBall::syncWait(YarnBall::withTimeout(server.read(buf), 2s)), static_cast<std::size_t>(3));
    EXPECT_EQ(YarnBall::syncWait(YarnBall::withTimeout(server.read(buf), 2s)), static_cast<std::size_t>(0)); // FIN
    // The other direction is still open.
    EXPECT_EQ(YarnBall::syncWait(server.write(bytes3("resp", 4))), static_cast<std::size_t>(4));
    EXPECT_EQ(YarnBall::syncWait(YarnBall::withTimeout(client.read(buf), 2s)), static_cast<std::size_t>(4));
    // Writing after the half-close fails.
    EXPECT_THROWS_AS(YarnBall::syncWait(client.write(bytes3("more", 4))), Soccer::SocketException);
    client.close();
    client.shutdownWrite(); // no-op on a closed stream
    Soccer::TcpStream never;
    never.shutdownWrite();
}

TEST(audit3_net_connect_reports_a_real_error_not_a_timeout_when_every_address_refuses) {
    // Reserve a port nobody listens on.
    std::uint16_t port = 0;
    {
        auto tmp = Soccer::TcpListener::bind("127.0.0.1", 0);
        port = tmp.localAddress().port();
    }
#ifdef _WIN32
    // Windows retries a refused loopback SYN for about two seconds per address before reporting the refusal.
    constexpr auto budget = 30s;
#else
    constexpr auto budget = 3s;
#endif
    try {
        (void) YarnBall::syncWait(YarnBall::withTimeout(Soccer::tcpConnect("localhost", port, budget), budget * 3));
        EXPECT_TRUE(false);
    } catch (const Soccer::SocketException &e) {
        EXPECT_TRUE(std::string(e.what()).find("timed out") == std::string::npos);
#ifndef _WIN32
        EXPECT_EQ(e.errorCode(), ECONNREFUSED);
#endif
    }
}
