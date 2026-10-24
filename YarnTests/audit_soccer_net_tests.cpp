//
// Regression tests for the code-audit fixes: soccer_net.
//

#include "test_framework.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <csignal>
#include <random>
#include <stdexcept>
#include <stop_token>
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
    #include <netinet/tcp.h>
    #include <poll.h>
    #include <sys/resource.h>
    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <sys/un.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

    template<typename Pred>
    bool netWaitUntil(Pred &&p, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!p()) {
            if (std::chrono::steady_clock::now() >= deadline) return p();
            std::this_thread::sleep_for(2ms);
        }
        return true;
    }

    /// In-memory stream for BufferedReader: hands out its content in chunks, then EOF.
    struct FakeStream {
        std::string data;
        std::size_t pos = 0;
        std::size_t chunk = 7;

        YarnBall::Task<std::size_t> read(std::span<std::byte> buf) {
            const std::size_t n = std::min({buf.size(), chunk, data.size() - pos});
            std::memcpy(buf.data(), data.data() + pos, n);
            pos += n;
            co_return n;
        }
    };

    std::span<const std::byte> bytesOf(const std::string &s) {
        return {reinterpret_cast<const std::byte *>(s.data()), s.size()};
    }

    std::string tempPath(const char *name) {
        return (std::filesystem::temp_directory_path() /
                (std::string(name) + "_" + std::to_string(::getpid()) + ".sock")).string();
    }

} // namespace


// =========================================================================
// IcmpEcho
// =========================================================================

TEST(audit_net_icmp_checksum_does_not_overflow_on_large_buffers) {
    // 300000 bytes of 0xFF: every word is 0xFFFF, so the folded sum is 0xFFFF and the
    // checksum 0. A 32-bit accumulator wraps long before the end of this buffer.
    std::vector<std::byte> big(300000, std::byte{0xFF});
    EXPECT_EQ(Soccer::IcmpEcho::checksum(big), static_cast<std::uint16_t>(0));
}

TEST(audit_net_icmp_build_request_rejects_oversized_payload) {
    std::vector<std::byte> payload(Soccer::IcmpEcho::kMaxPayload + 1);
    EXPECT_THROWS_AS(Soccer::IcmpEcho::buildRequest(1, 1, payload), Soccer::SocketException);
    std::vector<std::byte> ok(Soccer::IcmpEcho::kMaxPayload);
    EXPECT_EQ(Soccer::IcmpEcho::buildRequest(1, 1, ok).size(),
              sizeof(Soccer::IcmpEchoHeader) + ok.size());
}

TEST(audit_net_icmp_parse_only_accepts_echo_in_ipv4_icmp) {
    auto echo = Soccer::IcmpEcho::buildRequest(0x1234, 9);

    // Wrap in a minimal IPv4 header.
    auto wrap = [&](std::uint8_t versionIhl, std::uint8_t proto) {
        std::vector<std::byte> pkt(20, std::byte{0});
        pkt[0] = std::byte{versionIhl};
        pkt[9] = std::byte{proto};
        pkt.insert(pkt.end(), echo.begin(), echo.end());
        return pkt;
    };
    EXPECT_TRUE(Soccer::IcmpEcho::parse(wrap(0x45, 1), true).has_value());
    EXPECT_FALSE(Soccer::IcmpEcho::parse(wrap(0x65, 1), true).has_value()); // version 6
    EXPECT_FALSE(Soccer::IcmpEcho::parse(wrap(0x45, 6), true).has_value()); // TCP, not ICMP

    // Destination Unreachable (type 3) does not have the echo layout.
    auto unreachable = echo;
    unreachable[0] = std::byte{3};
    EXPECT_FALSE(Soccer::IcmpEcho::parse(unreachable, false).has_value());

    // The checksum is verified without disturbing the input.
    auto corrupt = echo;
    corrupt[1] = std::byte{1};
    auto parsed = Soccer::IcmpEcho::parse(corrupt, false);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->checksum_valid);
    EXPECT_TRUE(Soccer::IcmpEcho::parse(echo, false)->checksum_valid);
}


// =========================================================================
// BufferedReader
// =========================================================================

TEST(audit_net_buffered_reader_read_exact_is_bounded) {
    FakeStream fs;
    fs.data = std::string(100, 'x');
    Soccer::BufferedReader<FakeStream> r(&fs);
    EXPECT_THROWS_AS(YarnBall::syncWait(r.readExact(1000, 50)), Soccer::SocketException);
    // Nothing was consumed by the refused call.
    EXPECT_EQ(YarnBall::syncWait(r.readExact(10, 50)).size(), static_cast<std::size_t>(10));
    // A hostile length well past the default cap is refused without reserving it.
    EXPECT_THROWS_AS(YarnBall::syncWait(r.readExact(~static_cast<std::size_t>(0))),
                     Soccer::SocketException);
}

TEST(audit_net_buffered_reader_line_cap_and_eof_partial) {
    {
        FakeStream fs;
        fs.data = std::string(100, 'a');
        Soccer::BufferedReader<FakeStream> r(&fs, 32, /*maxLine=*/50);
        EXPECT_THROWS_AS(YarnBall::syncWait(r.readLine()), Soccer::SocketException);
        EXPECT_EQ(r.maxLine(), static_cast<std::size_t>(50));
    }
    {   // The cap is reported as its own type, so a parser can answer "too large".
        FakeStream fs;
        fs.data = std::string(100, 'a');
        Soccer::BufferedReader<FakeStream> r(&fs, 32, /*maxLine=*/50);
        EXPECT_THROWS_AS(YarnBall::syncWait(r.readLine()), Soccer::LineTooLongException);
    }
    {
        FakeStream fs;
        fs.data = "one\ntwo";
        Soccer::BufferedReader<FakeStream> r(&fs);
        bool terminated = false;
        EXPECT_EQ(YarnBall::syncWait(r.readLine(&terminated)), std::string("one\n"));
        EXPECT_TRUE(terminated);
        // The peer stopped mid-line: the partial record is returned but flagged.
        EXPECT_EQ(YarnBall::syncWait(r.readLine(&terminated)), std::string("two"));
        EXPECT_FALSE(terminated);
        EXPECT_EQ(YarnBall::syncWait(r.readLine(&terminated)), std::string());
        EXPECT_FALSE(terminated);
    }
}


// =========================================================================
// SocketAddress
// =========================================================================

#ifndef _WIN32
TEST(audit_net_socket_address_unix_to_string_and_bad_length) {
    sockaddr_un un{};
    un.sun_family = AF_UNIX;
    std::strcpy(un.sun_path, "/tmp/some.sock");
    const auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + std::strlen(un.sun_path) + 1);
    Soccer::SocketAddress a(reinterpret_cast<sockaddr *>(&un), len);
    EXPECT_EQ(a.to_string(), std::string("/tmp/some.sock"));

    // An impossible length gives an empty address, not a truncated copy.
    Soccer::SocketAddress bad(reinterpret_cast<sockaddr *>(&un),
                              static_cast<socklen_t>(sizeof(sockaddr_storage) + 1));
    EXPECT_EQ(bad.length(), static_cast<socklen_t>(0));
    EXPECT_EQ(bad.family(), AF_UNSPEC);
}
#endif


// =========================================================================
// TcpStream / TcpListener
// =========================================================================

namespace {
    YarnBall::Task<void> acceptAndKeep(Soccer::TcpListener &l, Soccer::TcpStream &out,
                                       Soccer::SocketAddress &peer) {
        out = co_await l.accept(&peer);
        co_return;
    }

    YarnBall::Task<bool> readThrowsOnClose(Soccer::TcpStream &s, std::atomic<bool> &parked) {
        std::array<std::byte, 8> buf{};
        parked = true;
        try {
            (void) co_await s.read(buf);
        } catch (const Soccer::SocketException &) {
            co_return true;
        }
        co_return false;
    }
}

TEST(audit_net_tcp_close_from_another_thread_wakes_a_parked_reader) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    Soccer::TcpStream server;
    Soccer::SocketAddress peer;
    YarnBall::coSpawn(acceptAndKeep(listener, server, peer));
    auto client = YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port));
    EXPECT_TRUE(netWaitUntil([&] { return server.fd() >= 0; }, 5s));

    // accept(&peer) reported the client's address.
    EXPECT_EQ(peer.family(), AF_INET);
    EXPECT_TRUE(peer.to_string().rfind("127.0.0.1:", 0) == 0);

    std::atomic<bool> parked{false};
    auto result = std::make_shared<std::atomic<int>>(-1);
    YarnBall::coSpawn([](Soccer::TcpStream &s, std::atomic<bool> &p, std::shared_ptr<std::atomic<int>> r)
                          -> YarnBall::Task<void> {
        *r = (co_await readThrowsOnClose(s, p)) ? 1 : 0;
    }(client, parked, result));
    EXPECT_TRUE(netWaitUntil([&] { return parked.load(); }, 5s));
    std::this_thread::sleep_for(50ms);

    std::thread closer([&] { client.close(); });
    closer.join();
    EXPECT_TRUE(netWaitUntil([&] { return result->load() != -1; }, 5s));
    EXPECT_EQ(result->load(), 1);
    EXPECT_EQ(client.fd(), -1);
}

TEST(audit_net_tcp_empty_read_buffer_returns_zero_immediately) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    auto client = YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port));
    // Nothing is ever sent: a read into a real buffer would park, an empty one must not.
    std::span<std::byte> none;
    EXPECT_EQ(YarnBall::syncWait(YarnBall::withTimeout(client.read(none), 2s)), static_cast<std::size_t>(0));
    client.close();
    EXPECT_THROWS_AS(YarnBall::syncWait(client.read(none)), Soccer::SocketException);
}

#ifndef _WIN32
TEST(audit_net_unix_bind_refuses_live_server_and_replaces_stale_socket) {
    const std::string path = tempPath("audit_net_live");
    std::filesystem::remove(path);
    {
        auto first = Soccer::TcpListener::bindUnix(path);

        // Owner-only: the socket is created under a restrictive umask.
        struct stat st{};
        EXPECT_EQ(::stat(path.c_str(), &st), 0);
        EXPECT_EQ(static_cast<int>(st.st_mode & 077), 0);

        // A live server keeps its path.
        EXPECT_THROWS_AS(Soccer::TcpListener::bindUnix(path), Soccer::SocketException);
        EXPECT_EQ(::stat(path.c_str(), &st), 0);
    }
    // The listener is gone but its socket file is still there: a stale socket is replaced.
    EXPECT_TRUE(std::filesystem::exists(path));
    {
        auto second = Soccer::TcpListener::bindUnix(path);
        EXPECT_TRUE(second.fd() >= 0);
    }
    std::filesystem::remove(path);
}

TEST(audit_net_unix_paths_are_validated) {
    EXPECT_THROWS_AS(Soccer::TcpListener::bindUnix(""), Soccer::SocketException);
    EXPECT_THROWS_AS(Soccer::TcpListener::bindUnix(std::string("\0abstract", 9)), Soccer::SocketException);
    EXPECT_THROWS_AS(Soccer::TcpListener::bindUnix(std::string("/tmp/a\0b", 8)), Soccer::SocketException);
    EXPECT_THROWS_AS(YarnBall::syncWait(Soccer::tcpConnectUnix("")), Soccer::SocketException);
    EXPECT_THROWS_AS(YarnBall::syncWait(Soccer::tcpConnectUnix(std::string("/tmp/a\0b", 8))),
                     Soccer::SocketException);
    EXPECT_THROWS_AS(YarnBall::syncWait(Soccer::tcpConnectUnix(std::string(300, 'p'))),
                     Soccer::SocketException);
}
#endif


// =========================================================================
// tcpServe
// =========================================================================

namespace {
    std::atomic<int> g_factoryCalls{0};
    std::atomic<int> g_served{0};

    /// First call throws something that is not a SocketException; later ones serve.
    YarnBall::Task<void> flakyFactory(Soccer::TcpStream client) {
        // The throw has to come from calling the handler itself, so this is wrapped below.
        g_served++;
        std::array<std::byte, 8> buf{};
        std::size_t n = co_await client.read(buf);
        if (n > 0) co_await client.write(std::span<const std::byte>(buf.data(), n));
    }

    YarnBall::Task<std::string> echoOnce(std::uint16_t port) {
        auto s = co_await Soccer::tcpConnect("127.0.0.1", port, 3s);
        const std::string msg = "ping";
        co_await s.write(bytesOf(msg));
        std::array<std::byte, 8> buf{};
        std::string got;
        while (got.size() < msg.size()) {
            std::size_t n = co_await s.read(buf);
            if (n == 0) break;
            got.append(reinterpret_cast<const char *>(buf.data()), n);
        }
        co_return got;
    }
}

TEST(audit_net_tcp_serve_survives_a_throwing_handler_factory) {
    g_factoryCalls = 0;
    g_served = 0;
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();

    std::stop_source src;
    YarnBall::coSpawn(Soccer::tcpServe(
        std::move(listener),
        [](Soccer::TcpStream s) -> YarnBall::Task<void> {
            if (g_factoryCalls++ == 0) throw std::runtime_error("handler factory failed");
            return flakyFactory(std::move(s));
        },
        src.get_token()));

    // The first connection is dropped by the failing factory; the loop must keep serving.
    try {
        (void) YarnBall::syncWait(YarnBall::withTimeout(echoOnce(port), 3s));
    } catch (...) {
    }
    EXPECT_EQ(YarnBall::syncWait(YarnBall::withTimeout(echoOnce(port), 5s)), std::string("ping"));
    src.request_stop();
    std::this_thread::sleep_for(50ms);
}

namespace {
    std::atomic<int> g_strayHandlers{0};

    YarnBall::Task<void> serveAndFlag(Soccer::TcpListener l, std::stop_token t, std::atomic<bool> *finished) {
        co_await Soccer::tcpServe(std::move(l),
                                  [](Soccer::TcpStream) -> YarnBall::Task<void> {
                                      g_strayHandlers++;
                                      co_return;
                                  },
                                  t, 0);
        finished->store(true);
    }
}

TEST(audit_net_tcp_serve_stop_wakes_the_accept_at_once_without_a_stray_connection) {
    // The accept is woken through the Reactor on every POSIX platform: no loopback probe is queued (so no handler runs
    // for it) and request_stop does not wait for a connect to settle.
    g_strayHandlers = 0;
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    std::stop_source src;
    std::atomic<bool> finished{false};
    YarnBall::coSpawn(serveAndFlag(std::move(listener), src.get_token(), &finished));
    std::this_thread::sleep_for(100ms); // let the accept park
    const auto t0 = std::chrono::steady_clock::now();
    src.request_stop();
    const auto stopCall = std::chrono::steady_clock::now() - t0;
    EXPECT_TRUE(netWaitUntil([&] { return finished.load(); }, 2s));
    EXPECT_TRUE(stopCall < 1s); // the call only flips a flag and wakes the reactor; no connect to wait for
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(g_strayHandlers.load(), 0);
}

TEST(audit_net_listener_interrupt_is_sticky_and_unparks_a_pending_accept) {
#ifndef _WIN32
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    std::atomic<int> outcome{0}; // 1 = accept threw, 2 = accepted something
    auto waiter = [](Soccer::TcpListener *l, std::atomic<int> *out) -> YarnBall::Task<void> {
        try {
            (void) co_await l->accept();
            out->store(2);
        } catch (const Soccer::SocketException &) {
            out->store(1);
        }
    };
    YarnBall::coSpawn(waiter(&listener, &outcome));
    std::this_thread::sleep_for(100ms);
    EXPECT_TRUE(listener.interruptAccept());
    EXPECT_TRUE(netWaitUntil([&] { return outcome.load() != 0; }, 2s));
    EXPECT_EQ(outcome.load(), 1);
    // Later accepts fail at once instead of parking, and a listener closed afterwards is still fine.
    outcome = 0;
    YarnBall::coSpawn(waiter(&listener, &outcome));
    EXPECT_TRUE(netWaitUntil([&] { return outcome.load() != 0; }, 2s));
    EXPECT_EQ(outcome.load(), 1);
    listener.close();
#endif
}

TEST(audit_net_transient_accept_errors_cover_network_errors) {
#ifndef _WIN32
    for (int e : {ECONNABORTED, EPROTO, ENETDOWN, ENETUNREACH, EHOSTUNREACH, EHOSTDOWN,
                  EOPNOTSUPP, ENOPROTOOPT, EMFILE, ENFILE}) {
        EXPECT_TRUE(Soccer::isTransientAcceptError(e));
    }
    EXPECT_FALSE(Soccer::isTransientAcceptError(EBADF));
    EXPECT_FALSE(Soccer::isTransientAcceptError(EINVAL));
    EXPECT_TRUE(Soccer::isFdExhaustionError(EMFILE));
    EXPECT_FALSE(Soccer::isFdExhaustionError(ECONNABORTED));
#endif
}


// =========================================================================
// IoDeadline
// =========================================================================

#ifndef _WIN32
TEST(audit_net_io_deadline_move_assignment_keeps_the_fired_state) {
    int fds[2];
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    {
        Soccer::IoDeadline a(fds[0], 100ms);
        EXPECT_TRUE(netWaitUntil([&] { return a.expired(); }, 5s));
        // Replacing it with a fresh, unarmed deadline must not forget that it fired.
        a = Soccer::IoDeadline{};
        EXPECT_TRUE(a.expired());

        // And a fired deadline moved INTO another instance keeps firing there.
        Soccer::IoDeadline b(fds[1], 100ms);
        EXPECT_TRUE(netWaitUntil([&] { return b.expired(); }, 5s));
        Soccer::IoDeadline c = std::move(b);
        EXPECT_TRUE(c.expired());
        EXPECT_FALSE(b.expired());
    }
    ::close(fds[0]);
    ::close(fds[1]);
}
#endif


// =========================================================================
// UdpSocket
// =========================================================================

TEST(audit_net_udp_multicast_setters_validate_and_report) {
    auto udp = Soccer::UdpSocket::bind("127.0.0.1", 0);
    EXPECT_THROWS_AS(udp.setMulticastTtl(-1), Soccer::SocketException);
    EXPECT_THROWS_AS(udp.setMulticastTtl(256), Soccer::SocketException);
    udp.setMulticastTtl(0);
    udp.setMulticastTtl(255);
    udp.setMulticastLoop(true);
    udp.setMulticastLoop(false);
    udp.close();
    EXPECT_THROWS_AS(udp.setMulticastTtl(1), Soccer::SocketException);
    EXPECT_THROWS_AS(udp.setMulticastLoop(true), Soccer::SocketException);
}


// =========================================================================
// TlsStream
// =========================================================================

#ifdef SOCCER_HAS_TLS
TEST(audit_net_tls_client_cert_and_key_must_be_set_together) {
    Soccer::TlsClientOptions certOnly;
    certOnly.clientCertFile = "/nonexistent/cert.pem";
    EXPECT_THROWS_AS(YarnBall::syncWait(Soccer::TlsStream::connect("127.0.0.1", 1, certOnly)),
                     Soccer::SocketException);

    Soccer::TlsClientOptions keyOnly;
    keyOnly.clientKeyFile = "/nonexistent/key.pem";
    try {
        (void) YarnBall::syncWait(Soccer::TlsStream::connect("127.0.0.1", 1, keyOnly));
        EXPECT_TRUE(false);
    } catch (const Soccer::SocketException &e) {
        // The configuration is refused up front, not after a connection attempt.
        EXPECT_TRUE(std::string(e.what()).find("together") != std::string::npos);
    }
}

namespace {
    struct LazyTlsOutcome {
        std::atomic<bool> accepted{false};
        std::atomic<bool> done{false};
        std::atomic<long long> acceptMs{-1};
        std::string error;
    };

    YarnBall::Task<void> lazyTlsServer(Soccer::TlsListener l, std::shared_ptr<LazyTlsOutcome> out) {
        const auto t0 = std::chrono::steady_clock::now();
        auto s = co_await l.accept(300ms);
        out->acceptMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        out->accepted = true;
        try {
            std::array<std::byte, 8> buf{};
            (void) co_await s.read(buf); // drives the handshake, which the silent peer never answers
        } catch (const std::exception &e) {
            out->error = e.what();
        }
        out->done = true;
    }
}

TEST(audit_net_tls_accept_defers_a_stalled_handshake_and_bounds_it) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("audit_tls_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const std::string cert = (dir / "s.pem").string(), key = (dir / "s.key").string();
    const int rc = std::system(("openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 1 -keyout " +
                                key + " -out " + cert + " -subj '/CN=localhost' >/dev/null 2>&1").c_str());
    if (rc != 0) {
        std::cout << "       (skipped: openssl unavailable)\n";
        std::filesystem::remove_all(dir);
        return;
    }
    auto listener = Soccer::TlsListener::bind("127.0.0.1", 0, cert, key);
    const std::uint16_t port = listener.localAddress().port();
    auto out = std::make_shared<LazyTlsOutcome>();
    YarnBall::coSpawn(lazyTlsServer(std::move(listener), out));

    auto silent = YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port)); // connects, then says nothing
    EXPECT_TRUE(netWaitUntil([&] { return out->accepted.load(); }, 3s));
    EXPECT_TRUE(out->acceptMs.load() < 250); // accept() did not wait on the handshake
    EXPECT_TRUE(netWaitUntil([&] { return out->done.load(); }, 5s));
    EXPECT_TRUE(out->error.find("timed out") != std::string::npos);
    std::filesystem::remove_all(dir);
}
#endif


// =========================================================================
// Second-pass audit regressions
// =========================================================================

#ifndef _WIN32

namespace {
    // Handlers are free functions over namespace-scope counters: a capturing coroutine lambda
    // would read its captures through a closure that may be gone when the frame resumes.
    std::atomic<int> gHandled{0};
    std::atomic<int> gLive{0};

    YarnBall::Task<void> countingHandler(Soccer::TcpStream s) {
        gHandled.fetch_add(1);
        gLive.fetch_add(1);
        std::array<std::byte, 64> b{};
        try {
            while ((co_await s.read(b)) != 0) {}
        } catch (...) {
        }
        gLive.fetch_sub(1);
    }
}

namespace {
    /// Connect raw non-blocking clients to the Unix listener at @p path until a connect no longer
    /// goes through (its accept queue is full). @return the errno of the refused connect, or 0
    /// if the queue never filled.
    int saturateUnixBacklog(const std::string &path, std::vector<int> &clients) {
        rlimit rl{};
        if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
            rl.rlim_cur = std::min<rlim_t>(rl.rlim_max, 16384);
            (void) ::setrlimit(RLIMIT_NOFILE, &rl);
        }
        for (int i = 0; i < 9000; ++i) {
            const int c = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (c < 0) return errno;
            ::fcntl(c, F_SETFL, ::fcntl(c, F_GETFL) | O_NONBLOCK);
            sockaddr_un u{};
            u.sun_family = AF_UNIX;
            std::strncpy(u.sun_path, path.c_str(), sizeof(u.sun_path) - 1);
            if (::connect(c, reinterpret_cast<sockaddr *>(&u), sizeof(u)) != 0) {
                const int e = errno;
                ::close(c);
                return e;
            }
            clients.push_back(c);
        }
        return 0;
    }
}

TEST(audit_net_unix_bind_does_not_touch_the_process_umask) {
    const mode_t before = ::umask(022);
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 40; ++i) {
                const std::string path = tempPath(("umask" + std::to_string(t) + "_" + std::to_string(i)).c_str());
                try {
                    auto l = Soccer::TcpListener::bindUnix(path);
                    struct stat st{};
                    if (::stat(path.c_str(), &st) != 0 || (st.st_mode & 0777) != 0600) ++failures;
                } catch (...) {
                    ++failures;
                }
                ::unlink(path.c_str());
                ::unlink((path + ".lock").c_str());
            }
        });
    }
    for (auto &th : threads) th.join();
    const mode_t after = ::umask(before);
    EXPECT_EQ(static_cast<int>(after), 022); // formerly left at 0077 by overlapping scopes
    EXPECT_EQ(failures.load(), 0);
}

TEST(audit_net_unix_bind_refuses_a_live_server_with_a_full_backlog) {
    const std::string path = tempPath("satur");
    auto live = Soccer::TcpListener::bindUnix(path);
    // Fill the accept queue (nobody accepts): further connects get EAGAIN on Linux and
    // ECONNREFUSED on macOS / the BSDs, which a naive probe mistakes for a stale socket.
    std::vector<int> clients;
    EXPECT_TRUE(saturateUnixBacklog(path, clients) != 0);
    bool refused = false;
    try {
        auto second = Soccer::TcpListener::bindUnix(path);
    } catch (const Soccer::SocketException &e) {
        refused = (e.errorCode() == EADDRINUSE);
    }
    EXPECT_TRUE(refused);
    for (int c : clients) ::close(c);
    live.close();
    // Once the first server is gone its leftover socket file is stale and may be replaced.
    auto again = Soccer::TcpListener::bindUnix(path);
    EXPECT_TRUE(again.fd() >= 0);
    again.close();
    ::unlink(path.c_str());
    ::unlink((path + ".lock").c_str());
}

TEST(audit_net_tcp_sockets_have_nodelay_and_two_writes_do_not_stall) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    auto client = YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port));
    auto server = YarnBall::syncWait(YarnBall::withTimeout(listener.accept(), 5s));
    for (int fd : {client.fd(), server.fd()}) {
        int on = 0;
        socklen_t len = sizeof(on);
        EXPECT_EQ(::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, &len), 0);
        EXPECT_TRUE(on != 0);
    }

    // A reply written as two small sends: with Nagle on, the second waits for the peer's
    // delayed ACK (tens of ms). Compare medians so one scheduling hiccup cannot fail it.
    struct Pair {
        static YarnBall::Task<void> run(Soccer::TcpStream &c, Soccer::TcpStream &s, int rounds,
                                        std::vector<long long> &us) {
            const std::array<std::byte, 4> part{};
            std::array<std::byte, 8> in{};
            for (int i = 0; i < rounds; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                (void) co_await s.write(part);
                (void) co_await s.write(part);
                std::size_t got = 0;
                while (got < in.size()) got += co_await c.read(std::span<std::byte>(in).subspan(got));
                us.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            }
        }
    };
    std::vector<long long> us;
    YarnBall::syncWait(YarnBall::withTimeout(Pair::run(client, server, 15, us), 20s));
    std::sort(us.begin(), us.end());
    EXPECT_TRUE(us[us.size() / 2] < 20000);
}

TEST(audit_net_socket_slot_keeps_every_retired_descriptor_until_the_last_use_ends) {
    int a[2], b[2];
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, a), 0);
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, b), 0);
    Soccer::detail::SocketSlot slot(a[0]);
    {
        auto use = slot.begin();
        slot.retire(slot.take());
        slot.reset(b[0]);
        slot.retire(slot.take()); // second retire while the first is still pending
        EXPECT_TRUE(::fcntl(a[0], F_GETFD) != -1); // neither closed under the running Use
        EXPECT_TRUE(::fcntl(b[0], F_GETFD) != -1);
    }
    EXPECT_EQ(::fcntl(a[0], F_GETFD), -1);
    EXPECT_EQ(::fcntl(b[0], F_GETFD), -1);
    ::close(a[1]);
    ::close(b[1]);
}

namespace {
    YarnBall::Task<void> gapReader(Soccer::TcpStream *s, std::atomic<int> *done) {
        std::array<std::byte, 16> b{};
        try {
            (void) co_await s->read(b);
        } catch (...) {
        }
        done->store(1);
    }
    YarnBall::Task<void> gapRecv(Soccer::UdpSocket *u, std::atomic<int> *done) {
        std::array<std::byte, 16> b{};
        try {
            (void) co_await u->recvFrom(b, nullptr);
        } catch (...) {
        }
        done->store(1);
    }
}

TEST(audit_net_close_never_strands_a_waiter_that_is_about_to_park) {
    // The owner closes at a random instant while a reader is between its EAGAIN and its
    // registration; socket churn in another thread recycles descriptor numbers, so a waiter
    // that registered on a closed number would sleep forever. 1 hang in ~1000 before the fix.
    std::atomic<bool> stopNoise{false};
    std::thread noise([&] {
        std::deque<int> q;
        while (!stopNoise.load()) {
            const int s = ::socket(AF_INET, SOCK_STREAM, 0);
            if (s >= 0) q.push_back(s);
            if (q.size() > 20) {
                ::close(q.front());
                q.pop_front();
            }
            std::this_thread::yield();
        }
        for (int x : q) ::close(x);
    });
    std::mt19937 rng(7);
    int hangs = 0;
    for (int i = 0; i < 4000 && hangs == 0; ++i) {
        int sv[2];
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        ::fcntl(sv[0], F_SETFL, O_NONBLOCK);
        auto s = std::make_unique<Soccer::TcpStream>(sv[0]);
        auto done = std::make_shared<std::atomic<int>>(0);
        YarnBall::coSpawn(gapReader(s.get(), done.get()));
        const int spin = static_cast<int>(rng() % 4000);
        { volatile int sink = 0; for (int k = 0; k < spin; ++k) sink = k; (void) sink; }
        s->close();
        if (!netWaitUntil([&] { return done->load() != 0; }, 3s)) {
            ++hangs;
            s.release(); // the stranded reader still points at it: leak rather than crash
        }
        ::close(sv[1]);
    }
    stopNoise = true;
    noise.join();
    EXPECT_EQ(hangs, 0);
}

TEST(audit_net_udp_close_never_strands_a_waiter_that_is_about_to_park) {
    // Shutdown does not wake a datagram socket, so only the re-check after registration can.
    std::mt19937 rng(11);
    int hangs = 0;
    for (int i = 0; i < 1500 && hangs == 0; ++i) {
        auto u = std::make_unique<Soccer::UdpSocket>(Soccer::UdpSocket::bind("127.0.0.1", 0));
        auto done = std::make_shared<std::atomic<int>>(0);
        YarnBall::coSpawn(gapRecv(u.get(), done.get()));
        const int spin = static_cast<int>(rng() % 4000);
        { volatile int sink = 0; for (int k = 0; k < spin; ++k) sink = k; (void) sink; }
        u->close();
        if (!netWaitUntil([&] { return done->load() != 0; }, 3s)) {
            ++hangs;
            u.release();
        }
    }
    EXPECT_EQ(hangs, 0);
}

TEST(audit_net_try_accept_never_waits) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    EXPECT_FALSE(listener.tryAccept().has_value());
    auto c = YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port));
    std::optional<Soccer::TcpStream> got;
    EXPECT_TRUE(netWaitUntil([&] { got = listener.tryAccept(); return got.has_value(); }, 3s));
    EXPECT_TRUE(got && got->fd() >= 0);
    EXPECT_FALSE(listener.tryAccept().has_value());
}

TEST(audit_net_accept_error_classes) {
#if defined(__linux__)
    EXPECT_TRUE(Soccer::isTransientAcceptError(EPERM)); // a firewall rejected the connection
#endif
    // Per-connection errors retry at once; only memory pressure pauses.
    EXPECT_FALSE(Soccer::isAcceptBackoffError(ECONNABORTED));
    EXPECT_FALSE(Soccer::isAcceptBackoffError(EPROTO));
    EXPECT_FALSE(Soccer::isAcceptBackoffError(EHOSTUNREACH));
    EXPECT_TRUE(Soccer::isAcceptBackoffError(ENOMEM));
    EXPECT_TRUE(Soccer::isAcceptBackoffError(ENOBUFS));
}

TEST(audit_net_connection_limiter_refusals_never_raise_the_count) {
    Soccer::ConnectionLimiter lim(2);
    auto a = lim.tryAcquire();
    auto b = lim.tryAcquire();
    EXPECT_TRUE(static_cast<bool>(a) && static_cast<bool>(b));
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> peak{0};
    std::atomic<int> wrongGrants{0};
    std::vector<std::thread> hammer;
    for (int t = 0; t < 4; ++t) {
        hammer.emplace_back([&] {
            while (!stop.load()) {
                if (lim.tryAcquire()) ++wrongGrants; // both slots are taken: never granted
                const auto n = lim.active();
                std::size_t p = peak.load();
                while (n > p && !peak.compare_exchange_weak(p, n)) {}
            }
        });
    }
    std::this_thread::sleep_for(150ms);
    stop = true;
    for (auto &th : hammer) th.join();
    EXPECT_EQ(wrongGrants.load(), 0);
    // The old fetch_add / fetch_sub dance showed refused attempts as extra live connections
    // (up to the number of racing callers), which could also refuse a caller while a slot was free.
    EXPECT_TRUE(peak.load() <= 2);
    b = Soccer::ConnectionLimiter::Slot{};
    EXPECT_TRUE(static_cast<bool>(lim.tryAcquire())); // a freed slot is always grantable
}

TEST(audit_net_connect_budget_is_shared_out_between_candidates) {
    using Soccer::detail::connectSlice;
    EXPECT_EQ(connectSlice(10000ms, 1).count(), 10000);
    EXPECT_EQ(connectSlice(10000ms, 2).count(), 5000);
    EXPECT_EQ(connectSlice(10000ms, 4).count(), 2500);
    EXPECT_EQ(connectSlice(300ms, 5).count(), 250);  // never below the floor ...
    EXPECT_EQ(connectSlice(100ms, 5).count(), 100);  // ... unless less than that is left
    EXPECT_EQ(connectSlice(100ms, 1).count(), 100);
}

TEST(audit_net_io_deadline_on_a_stream_ignores_a_descriptor_the_owner_already_closed) {
    int sv[2];
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    ::fcntl(sv[0], F_SETFL, O_NONBLOCK);
    Soccer::TcpStream a(sv[0]);
    Soccer::IoDeadline d(a, 150ms);
    const int oldFd = a.fd();
    a.close();
    // The lowest free number is handed out again: an unrelated socket now owns it.
    int sv2[2];
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv2), 0);
    std::this_thread::sleep_for(450ms); // the deadline fires meanwhile
    if (sv2[0] == oldFd || sv2[1] == oldFd) {
        const char c = 'x';
        EXPECT_EQ(::send(sv2[0], &c, 1, 0), 1); // a stray shutdown would make this fail with EPIPE
        EXPECT_EQ(::send(sv2[1], &c, 1, 0), 1);
    }
    ::close(sv[1]);
    ::close(sv2[0]);
    ::close(sv2[1]);
}

TEST(audit_net_udp_bind_sets_v6only_explicitly) {
    for (const bool dual : {true, false}) {
        try {
            auto u = Soccer::UdpSocket::bind("::", 0, dual);
            int v6only = -1;
            socklen_t len = sizeof(v6only);
            EXPECT_EQ(::getsockopt(u.fd(), IPPROTO_IPV6, IPV6_V6ONLY, &v6only, &len), 0);
            EXPECT_EQ(v6only, dual ? 0 : 1);
        } catch (const Soccer::SocketException &) {
            std::cout << "       (skipped: no IPv6)\n";
            return;
        }
    }
}

TEST(audit_net_unresolvable_names_fail_after_a_single_lookup) {
    EXPECT_THROWS_AS(Soccer::SocketAddress::resolve("no-such-host.invalid", 80), Soccer::SocketException);
    EXPECT_TRUE(Soccer::SocketAddress::resolve("localhost", 80).port() == 80);
    EXPECT_TRUE(Soccer::SocketAddress::resolve("127.0.0.1", 81).port() == 81);
}

TEST(audit_net_tcp_serve_stops_without_a_stray_connection_where_it_can) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    gHandled = 0;
    gLive = 0;
    auto finished = std::make_shared<std::atomic<bool>>(false);
    std::stop_source ss;
    std::function<YarnBall::Task<void>(Soccer::TcpStream)> handler = countingHandler;
    YarnBall::coSpawn([](Soccer::TcpListener l, std::function<YarnBall::Task<void>(Soccer::TcpStream)> h,
                         std::stop_token t, std::shared_ptr<std::atomic<bool>> f) -> YarnBall::Task<void> {
        try {
            co_await Soccer::tcpServe(std::move(l), std::move(h), t, 0);
        } catch (...) {
        }
        f->store(true);
    }(std::move(listener), handler, ss.get_token(), finished));
    std::this_thread::sleep_for(150ms);
    const auto t0 = std::chrono::steady_clock::now();
    ss.request_stop();
    EXPECT_TRUE(netWaitUntil([&] { return finished->load(); }, 3s));
    EXPECT_TRUE(std::chrono::steady_clock::now() - t0 < 2s);
#if defined(__linux__)
    EXPECT_EQ(gHandled.load(), 0); // woken by a shutdown, not by a connection of its own
#endif
}

#if defined(__linux__)
TEST(audit_net_unix_connect_waits_out_a_full_backlog_instead_of_failing) {
    const std::string path = tempPath("busyconn");
    auto server = std::make_shared<Soccer::TcpListener>(Soccer::TcpListener::bindUnix(path));
    std::vector<int> fillers;
    EXPECT_EQ(saturateUnixBacklog(path, fillers), EAGAIN);
    // No retry allowance: the old behaviour, fail at once.
    EXPECT_THROWS_AS(YarnBall::syncWait(Soccer::tcpConnectUnix(path, 0ms)), Soccer::SocketException);

    std::thread drain([&] {
        std::this_thread::sleep_for(200ms);
        for (int i = 0; i < 4; ++i) {
            try {
                (void) YarnBall::syncWait(YarnBall::withTimeout(server->accept(), 2s));
            } catch (...) {
            }
        }
    });
    auto c = YarnBall::syncWait(YarnBall::withTimeout(Soccer::tcpConnectUnix(path, 5s), 8s));
    EXPECT_TRUE(c.fd() >= 0);
    drain.join();
    for (int f : fillers) ::close(f);
    server->close();
    ::unlink(path.c_str());
    ::unlink((path + ".lock").c_str());
}
#endif

TEST(audit_net_tcp_serve_does_not_drop_a_connection_after_fd_exhaustion_ends) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    int up[2], down[2];
    EXPECT_EQ(::pipe(up), 0);   // child -> parent
    EXPECT_EQ(::pipe(down), 0); // parent -> child
    const pid_t pid = ::fork();
    // Whatever happens below (a failed expectation throws), the child is not left blocked on its pipe
    // and is always reaped, the descriptor limit is put back, and the pipes are closed.
    struct Cleanup {
        pid_t pid;
        int *pipes;
        rlimit saved{};
        bool limited = false;
        ~Cleanup() {
            if (limited) (void) ::setrlimit(RLIMIT_NOFILE, &saved);
            if (pid > 0) {
                ::kill(pid, SIGKILL);
                int st = 0;
                while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
                }
            }
            for (int i = 0; i < 4; ++i) ::close(pipes[i]);
        }
    };
    int allPipes[4] = {up[0], up[1], down[0], down[1]};
    if (pid == 0) {
        // Child: plain syscalls only. Open many connections, hold them, close them on request,
        // then make exactly one more.
        ::close(listener.fd());
        auto sig = [&](char c) { [[maybe_unused]] const auto rc = ::write(up[1], &c, 1); };
        auto wait = [&] { char c = 0; [[maybe_unused]] const auto rc = ::read(down[0], &c, 1); return c; };
        auto dial = [&]() {
            const int s = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::connect(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0) {
                ::close(s);
                return -1;
            }
            return s;
        };
        // The parent is multithreaded: after fork only async-signal-safe calls are allowed (no allocation), so
        // plain syscalls and a fixed array.
        int many[40];
        int nMany = 0;
        for (int i = 0; i < 40; ++i) {
            const int s = dial();
            if (s >= 0) many[nMany++] = s;
        }
        sig('a');
        wait();
        for (int i = 0; i < nMany; ++i) ::close(many[i]);
        sig('c');
        wait();
        const int last = dial();
        sig('e');
        wait();
        if (last >= 0) ::close(last);
        ::_exit(0);
    }
    EXPECT_TRUE(pid > 0);
    Cleanup cleanup{pid, allPipes};
    gHandled = 0;
    gLive = 0;
    std::function<YarnBall::Task<void>(Soccer::TcpStream)> handler = countingHandler;
    std::stop_source ss;
    YarnBall::coSpawn([](Soccer::TcpListener l, std::function<YarnBall::Task<void>(Soccer::TcpStream)> h,
                         std::stop_token t) -> YarnBall::Task<void> {
        try {
            co_await Soccer::tcpServe(std::move(l), std::move(h), t, 0);
        } catch (...) {
        }
    }(std::move(listener), handler, ss.get_token()));
    std::this_thread::sleep_for(100ms);

    // Leave only a handful of descriptors below the highest one in use.
    rlimit &old = cleanup.saved;
    EXPECT_EQ(::getrlimit(RLIMIT_NOFILE, &old), 0);
    int highest = 0;
    for (int fd = 0; fd < 1024; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1) highest = fd;
    }
    rlimit tight = old;
    tight.rlim_cur = static_cast<rlim_t>(highest + 8);
    cleanup.limited = ::setrlimit(RLIMIT_NOFILE, &tight) == 0;

    char c = 0;
    EXPECT_EQ(::read(up[0], &c, 1), 1); // 'a': the child has made its connections
    std::this_thread::sleep_for(500ms); // the server hits EMFILE and sheds
    [[maybe_unused]] const auto wb = ::write(down[1], "b", 1);
    EXPECT_EQ(::read(up[0], &c, 1), 1); // 'c': the child closed them all
    EXPECT_TRUE(netWaitUntil([&] { return gLive.load() == 0; }, 5s));
    std::this_thread::sleep_for(300ms);
    if (cleanup.limited) {
        (void) ::setrlimit(RLIMIT_NOFILE, &old);
        cleanup.limited = false;
    }
    const int before = gHandled.load();
    [[maybe_unused]] const auto wd = ::write(down[1], "d", 1);
    EXPECT_EQ(::read(up[0], &c, 1), 1); // 'e': one more connection was made
    // Formerly swallowed by the shedding accept that was still parked.
    EXPECT_TRUE(netWaitUntil([&] { return gHandled.load() > before; }, 3s));
    [[maybe_unused]] const auto wf = ::write(down[1], "f", 1);
    int status = 0;
    ::waitpid(pid, &status, 0);
    cleanup.pid = -1; // reaped
    ss.request_stop();
    std::this_thread::sleep_for(200ms);
}

#endif // !_WIN32

#ifdef SOCCER_HAS_TLS
namespace {
    struct TlsCloseFixture {
        std::filesystem::path dir;
        std::string cert, key;
        bool ok = false;
        TlsCloseFixture() {
            dir = std::filesystem::temp_directory_path() / ("audit_tlsc_" + std::to_string(::getpid()));
            std::filesystem::create_directories(dir);
            cert = (dir / "s.pem").string();
            key = (dir / "s.key").string();
            ok = std::system(("openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 1 -keyout " +
                              key + " -out " + cert + " -subj '/CN=localhost' >/dev/null 2>&1").c_str()) == 0;
        }
        ~TlsCloseFixture() { std::filesystem::remove_all(dir); }
    };

    YarnBall::Task<void> tlsBulkWriter(std::shared_ptr<Soccer::TlsStream> s) {
        std::vector<std::byte> big(64u << 20);
        try {
            (void) co_await s->write(big);
        } catch (...) {
        }
    }
}

TEST(audit_net_tls_close_does_not_block_behind_a_stalled_peer_and_empty_reads_return_zero) {
    TlsCloseFixture fx;
    if (!fx.ok) {
        std::cout << "       (skipped: openssl unavailable)\n";
        return;
    }
    auto listener = Soccer::TlsListener::bind("127.0.0.1", 0, fx.cert, fx.key);
    const std::uint16_t port = listener.localAddress().port();
    auto serverSide = std::make_shared<std::shared_ptr<Soccer::TlsStream>>();
    auto accepted = std::make_shared<std::atomic<bool>>(false);
    YarnBall::coSpawn([](Soccer::TlsListener l, std::shared_ptr<std::shared_ptr<Soccer::TlsStream>> out,
                         std::shared_ptr<std::atomic<bool>> flag) -> YarnBall::Task<void> {
        auto st = std::make_shared<Soccer::TlsStream>(co_await l.accept());
        co_await st->handshake();
        *out = st;
        flag->store(true);
        co_await YarnBall::sleepFor(10s); // keep the listener alive for the test
    }(std::move(listener), serverSide, accepted));

    Soccer::TlsClientOptions opts;
    opts.insecure_no_verify_cert = true;
    opts.insecure_no_verify_name = true;
    auto client = YarnBall::syncWait(Soccer::TlsStream::connect("127.0.0.1", port, opts)); // never reads
    EXPECT_TRUE(netWaitUntil([&] { return accepted->load(); }, 5s));

    // An empty buffer is not an end-of-stream probe.
    std::array<std::byte, 1> one{};
    EXPECT_EQ(YarnBall::syncWait(client.read(std::span<std::byte>(one.data(), 0))), 0u);

    YarnBall::coSpawn(tlsBulkWriter(*serverSide));
    std::this_thread::sleep_for(1200ms); // the writer is now stuck on a full send buffer
    const auto t0 = std::chrono::steady_clock::now();
    (*serverSide)->close();
    const auto took = std::chrono::steady_clock::now() - t0;
    EXPECT_TRUE(took < 750ms); // close never waits for the stalled peer; generous so a loaded CI machine does not flake
    client.close();
}
#endif
