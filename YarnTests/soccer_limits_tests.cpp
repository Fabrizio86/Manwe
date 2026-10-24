//
// Tests for Soccer's server resource limits, deadlines, connection-pool
// freshness, WebSocket write serialisation / Origin policy, HTTP/2 teardown,
// UDP truncation reporting and close-on-exec.
//

#include "test_framework.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../Yarn/includes/Coroutines.h"
#include "../Yarn/includes/Timeout.h"
#include "../Yarn/includes/Timers.h"
#include "../Soccer/includes/ConnectionLimiter.h"
#include "../Soccer/includes/IoDeadline.h"
#include "../Soccer/includes/Soccer.h"
#include "../Soccer/includes/WebSocket.h"
#include "../Soccer/includes/Http2.h"

#ifndef _WIN32
    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <sys/resource.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

    /// Connect, send @p request in one write, read until the peer closes (or errors).
    YarnBall::Task<std::string> rawExchange(std::uint16_t port, std::string request) {
        auto s = co_await Soccer::tcpConnect("127.0.0.1", port);
        if (!request.empty()) {
            co_await s.write(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(request.data()), request.size()));
        }
        std::string out;
        std::array<std::byte, 1024> buf{};
        try {
            while (true) {
                const std::size_t n = co_await s.read(buf);
                if (n == 0) break;
                out.append(reinterpret_cast<const char *>(buf.data()), n);
            }
        } catch (const Soccer::SocketException &) {
        }
        co_return out;
    }

    std::string exchange(std::uint16_t port, std::string request) {
        return YarnBall::syncWait(YarnBall::withTimeout(rawExchange(port, std::move(request)), 8s));
    }

    template<typename Pred>
    bool waitUntil(Pred &&p, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!p()) {
            if (std::chrono::steady_clock::now() >= deadline) return p();
            std::this_thread::sleep_for(2ms);
        }
        return true;
    }

} // namespace


// =========================================================================
// IoDeadline / ConnectionLimiter
// =========================================================================

TEST(limits_connection_limiter_counts_and_refuses) {
    Soccer::ConnectionLimiter lim(2);
    auto a = lim.tryAcquire();
    auto b = lim.tryAcquire();
    EXPECT_TRUE(static_cast<bool>(a));
    EXPECT_TRUE(static_cast<bool>(b));
    auto c = lim.tryAcquire();
    EXPECT_FALSE(static_cast<bool>(c));
    EXPECT_EQ(lim.active(), static_cast<std::size_t>(2));
    a = Soccer::ConnectionLimiter::Slot{};
    EXPECT_EQ(lim.active(), static_cast<std::size_t>(1));
    EXPECT_TRUE(static_cast<bool>(lim.tryAcquire()));

    Soccer::ConnectionLimiter unlimited(0);
    std::vector<Soccer::ConnectionLimiter::Slot> many;
    for (int i = 0; i < 100; ++i) many.push_back(unlimited.tryAcquire());
    EXPECT_TRUE(static_cast<bool>(many.back()));
}

namespace {
    YarnBall::Task<void> acceptAndHold(Soccer::TcpListener l, std::shared_ptr<std::atomic<bool>> stop) {
        auto s = co_await l.accept();
        while (!stop->load()) co_await YarnBall::sleepFor(5ms);
        co_return;
    }
}

TEST(limits_io_deadline_shuts_down_a_stalled_read_but_not_after_disarm) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    auto stop = std::make_shared<std::atomic<bool>>(false);
    YarnBall::coSpawn(acceptAndHold(std::move(listener), stop));

    auto client = YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port));

    // A disarmed deadline must never fire, however late the sweeper looks at it.
    {
        Soccer::IoDeadline d(client.fd(), 100ms);
        d.disarm();
        std::this_thread::sleep_for(350ms);
        EXPECT_FALSE(d.expired());
    }

    // An armed one wakes a read that would otherwise park forever.
    Soccer::IoDeadline d(client.fd(), 150ms);
    const auto start = std::chrono::steady_clock::now();
    std::array<std::byte, 16> buf{};
    std::size_t n = 1;
    try {
        n = YarnBall::syncWait(YarnBall::withTimeout(client.read(buf), 5s));
    } catch (const Soccer::SocketException &) {
        n = 0;
    }
    const auto took = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(n, static_cast<std::size_t>(0));
    EXPECT_TRUE(d.expired());
    EXPECT_TRUE(took >= 100ms);
    EXPECT_TRUE(took < 3s);
    stop->store(true);
}


// =========================================================================
// HttpServer limits
// =========================================================================

TEST(limits_http_server_header_timeout_cuts_off_slowloris) {
    Soccer::HttpServerOptions o;
    o.headerTimeout = 250ms;
    Soccer::HttpServer server("127.0.0.1", 0, o);
    const std::uint16_t port = server.localAddress().port();
    std::stop_source src;
    YarnBall::coSpawn(server.serve(src.get_token()));

    const auto start = std::chrono::steady_clock::now();
    // A request head that never finishes.
    const std::string reply = exchange(port, "GET / HTTP/1.1\r\nHost: x");
    const auto took = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(reply.empty());
    EXPECT_TRUE(took >= 150ms);
    EXPECT_TRUE(took < 4s);

    src.request_stop();
    std::this_thread::sleep_for(50ms);
}

TEST(limits_http_server_rejects_oversize_headers_and_bodies) {
    Soccer::HttpServerOptions o;
    o.maxHeaderCount = 2;
    o.maxBodyBytes = 16;
    Soccer::HttpServer server("127.0.0.1", 0, o);
    server.route("POST", "/p", [](Soccer::HttpRequest req) -> YarnBall::Task<Soccer::HttpResponse> {
        Soccer::HttpResponse r;
        r.body = std::to_string(req.body.size());
        co_return r;
    });
    const std::uint16_t port = server.localAddress().port();
    std::stop_source src;
    YarnBall::coSpawn(server.serve(src.get_token()));

    // Within limits.
    auto ok = exchange(port, "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nabcd");
    EXPECT_TRUE(ok.rfind("HTTP/1.1 200", 0) == 0);

    // Content-Length over the cap: refused before the body is read.
    auto big = exchange(port, "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 17\r\n\r\n01234567890123456");
    EXPECT_TRUE(big.rfind("HTTP/1.1 413", 0) == 0);

    // Too many header fields.
    auto many = exchange(port, "POST /p HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n");
    EXPECT_TRUE(many.rfind("HTTP/1.1 431", 0) == 0);

    src.request_stop();
    std::this_thread::sleep_for(50ms);
}

TEST(limits_http_server_connection_cap_answers_503) {
    Soccer::HttpServerOptions o;
    o.maxConnections = 1;
    Soccer::HttpServer server("127.0.0.1", 0, o);
    server.route("GET", "/x", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
        Soccer::HttpResponse r;
        r.body = "ok";
        co_return r;
    });
    const std::uint16_t port = server.localAddress().port();
    std::stop_source src;
    YarnBall::coSpawn(server.serve(src.get_token()));

    // First connection occupies the only slot and says nothing.
    auto holder = YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port));
    std::this_thread::sleep_for(150ms);

    auto refused = exchange(port, "GET /x HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(refused.rfind("HTTP/1.1 503", 0) == 0);

    holder.close();
    std::this_thread::sleep_for(150ms);
    auto served = exchange(port, "GET /x HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(served.rfind("HTTP/1.1 200", 0) == 0);

    src.request_stop();
    std::this_thread::sleep_for(50ms);
}


// =========================================================================
// HttpConnectionPool freshness
// =========================================================================

namespace {
    YarnBall::Task<void> acceptAndDrop(Soccer::TcpListener l, int count) {
        for (int i = 0; i < count; ++i) (void) co_await l.accept();
        co_return;
    }
}

TEST(limits_http_pool_drops_connections_idle_past_the_max_age) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    auto stop = std::make_shared<std::atomic<bool>>(false);
    YarnBall::coSpawn(acceptAndHold(std::move(listener), stop));

    Soccer::HttpConnectionPool pool;
    pool.setMaxIdleAge(80ms);
    pool.release("127.0.0.1", port, YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port)));
    EXPECT_EQ(pool.idleCount("127.0.0.1", port), static_cast<std::size_t>(1));
    std::this_thread::sleep_for(200ms);
    EXPECT_FALSE(pool.acquire("127.0.0.1", port).has_value());
    stop->store(true);
}

TEST(limits_http_pool_acquire_live_skips_connections_the_peer_closed) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    YarnBall::coSpawn(acceptAndDrop(std::move(listener), 1));

    Soccer::HttpConnectionPool pool;
    pool.release("127.0.0.1", port, YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port)));
    std::this_thread::sleep_for(150ms); // let the FIN arrive
    EXPECT_FALSE(pool.acquireLive("127.0.0.1", port).has_value());
    EXPECT_EQ(pool.idleCount("127.0.0.1", port), static_cast<std::size_t>(0));
}

namespace {
    /// Reads one request head off @p s; returns false on EOF.
    YarnBall::Task<bool> readRequestHead(Soccer::TcpStream &s) {
        std::string head;
        std::array<std::byte, 512> buf{};
        while (head.find("\r\n\r\n") == std::string::npos) {
            const std::size_t n = co_await s.read(buf);
            if (n == 0) co_return false;
            head.append(reinterpret_cast<const char *>(buf.data()), n);
        }
        co_return true;
    }

    /// Connection 1: answer one request keep-alive, then swallow the next request and
    /// close without answering (a stale pooled connection). Connection 2: answer normally.
    YarnBall::Task<void> flakyKeepAliveServer(Soccer::TcpListener l) {
        static constexpr char kFirst[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: keep-alive\r\n\r\nfirst";
        static constexpr char kSecond[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecond";
        {
            auto c1 = co_await l.accept();
            if (co_await readRequestHead(c1)) {
                co_await c1.write(std::span<const std::byte>(
                    reinterpret_cast<const std::byte *>(kFirst), sizeof(kFirst) - 1));
            }
            (void) co_await readRequestHead(c1); // the reused request arrives...
            c1.close();                          // ...and is dropped unanswered
        }
        auto c2 = co_await l.accept();
        if (co_await readRequestHead(c2)) {
            co_await c2.write(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(kSecond), sizeof(kSecond) - 1));
        }
        co_return;
    }
}

TEST(limits_http_pooled_get_retries_once_on_a_stale_connection) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    YarnBall::coSpawn(flakyKeepAliveServer(std::move(listener)));

    auto r1 = YarnBall::syncWait(Soccer::HttpClient::getPooled("127.0.0.1", port, "/a"));
    EXPECT_EQ(r1.body, std::string("first"));
    // The pooled connection is closed under the client's feet; the idempotent GET is repeated on a new one.
    auto r2 = YarnBall::syncWait(Soccer::HttpClient::getPooled("127.0.0.1", port, "/b"));
    EXPECT_EQ(r2.body, std::string("second"));
}


// =========================================================================
// WebSocket: handshake timeout, Origin policy, serialised writers
// =========================================================================

namespace {
    struct WsServerResult {
        std::atomic<bool> done{false};
        std::atomic<bool> ok{false};
        std::mutex mu;
        std::string error;
    };

    YarnBall::Task<void> wsHandshakeOnce(Soccer::TcpListener l, Soccer::WsOptions opts,
                                         std::shared_ptr<WsServerResult> out) {
        try {
            auto client = co_await l.accept();
            auto ws = co_await Soccer::WsConnection::serverHandshake(std::move(client), opts);
            out->ok = true;
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lk(out->mu);
            out->error = e.what();
        }
        out->done = true;
        co_return;
    }

    std::string wsUpgradeRequest(const std::string &origin) {
        std::string r =
            "GET /chat HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";
        if (!origin.empty()) r += "Origin: " + origin + "\r\n";
        return r + "\r\n";
    }
}

TEST(limits_ws_handshake_times_out_on_a_silent_peer) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    auto out = std::make_shared<WsServerResult>();
    Soccer::WsOptions o;
    o.handshakeTimeout = 200ms;
    YarnBall::coSpawn(wsHandshakeOnce(std::move(listener), o, out));

    auto silent = YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port));
    EXPECT_TRUE(waitUntil([&] { return out->done.load(); }, 4s));
    EXPECT_FALSE(out->ok.load());
    std::lock_guard<std::mutex> lk(out->mu);
    EXPECT_TRUE(out->error.find("timed out") != std::string::npos);
}

TEST(limits_ws_origin_allow_list_and_callback) {
    Soccer::WsOptions o;
    o.allowedOrigins = {"https://good.example"};

    {   // Listed origin: accepted.
        auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
        const std::uint16_t port = listener.localAddress().port();
        auto out = std::make_shared<WsServerResult>();
        YarnBall::coSpawn(wsHandshakeOnce(std::move(listener), o, out));
        auto reply = exchange(port, wsUpgradeRequest("https://GOOD.example"));
        EXPECT_TRUE(reply.rfind("HTTP/1.1 101", 0) == 0);
        EXPECT_TRUE(waitUntil([&] { return out->done.load(); }, 3s));
        EXPECT_TRUE(out->ok.load());
    }
    {   // Foreign origin: 403 and the handshake fails.
        auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
        const std::uint16_t port = listener.localAddress().port();
        auto out = std::make_shared<WsServerResult>();
        YarnBall::coSpawn(wsHandshakeOnce(std::move(listener), o, out));
        auto reply = exchange(port, wsUpgradeRequest("https://evil.example"));
        EXPECT_TRUE(reply.rfind("HTTP/1.1 403", 0) == 0);
        EXPECT_TRUE(waitUntil([&] { return out->done.load(); }, 3s));
        EXPECT_FALSE(out->ok.load());
    }
    {   // No Origin header (non-browser client): allowed by default, refused when asked.
        Soccer::WsOptions strict = o;
        strict.allowMissingOrigin = false;
        auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
        const std::uint16_t port = listener.localAddress().port();
        auto out = std::make_shared<WsServerResult>();
        YarnBall::coSpawn(wsHandshakeOnce(std::move(listener), strict, out));
        auto reply = exchange(port, wsUpgradeRequest(""));
        EXPECT_TRUE(reply.rfind("HTTP/1.1 403", 0) == 0);
    }
    {   // Callback policy.
        Soccer::WsOptions cb;
        cb.originCheck = [](std::string_view origin) { return origin.ends_with(".internal"); };
        auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
        const std::uint16_t port = listener.localAddress().port();
        auto out = std::make_shared<WsServerResult>();
        YarnBall::coSpawn(wsHandshakeOnce(std::move(listener), cb, out));
        auto reply = exchange(port, wsUpgradeRequest("https://app.other"));
        EXPECT_TRUE(reply.rfind("HTTP/1.1 403", 0) == 0);
    }
}

namespace {
    YarnBall::Task<void> wsBurstSender(Soccer::WsConnection *ws, char fill, int count,
                                       std::shared_ptr<std::atomic<int>> finished) {
        const std::string payload(200 * 1024, fill);
        for (int i = 0; i < count; ++i) co_await ws->sendText(payload);
        finished->fetch_add(1);
        co_return;
    }

    /// Sets @p done only after every object that touches the Reactor on destruction (the
    /// listener, the connection) is gone, so the task has nothing left to do when the test
    /// returns -- a coroutine still unwinding while the process tears down its statics
    /// would otherwise call into an already-destroyed Reactor.
    YarnBall::Task<void> wsConcurrentSendServer(Soccer::TcpListener l, int senders, int perSender,
                                                std::shared_ptr<std::atomic<bool>> done) {
        {
            auto client = co_await l.accept();
            l.close();
            auto ws = co_await Soccer::WsConnection::serverHandshake(std::move(client));
            auto finished = std::make_shared<std::atomic<int>>(0);
            for (int i = 0; i < senders; ++i) {
                YarnBall::coSpawn(wsBurstSender(&ws, static_cast<char>('a' + i), perSender, finished));
            }
            while (finished->load() < senders) co_await YarnBall::sleepFor(5ms);
            co_await ws.close();
        }
        done->store(true);
        co_return;
    }

    /// Returns "" when every message was one uniform run of a single letter, else a description.
    YarnBall::Task<std::string> wsCollectUniform(std::uint16_t port, int expected) {
        auto ws = co_await Soccer::WsConnection::connect("127.0.0.1", port, "/");
        for (int i = 0; i < expected; ++i) {
            auto m = co_await ws.receive();
            const auto text = m.text();
            if (text.size() != 200 * 1024) co_return "wrong size " + std::to_string(text.size());
            for (char c : text) {
                if (c != text[0]) co_return "interleaved frame at message " + std::to_string(i);
            }
        }
        try { (void) co_await ws.receive(); } catch (const Soccer::WsException &) {}
        co_return std::string();
    }
}

TEST(limits_ws_concurrent_senders_never_interleave_frames) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    constexpr int kSenders = 4;
    constexpr int kPer = 12;
    auto serverDone = std::make_shared<std::atomic<bool>>(false);
    YarnBall::coSpawn(wsConcurrentSendServer(std::move(listener), kSenders, kPer, serverDone));
    const std::string verdict = YarnBall::syncWait(
        YarnBall::withTimeout(wsCollectUniform(port, kSenders * kPer), 30s));
    EXPECT_EQ(verdict, std::string());
    EXPECT_TRUE(waitUntil([&] { return serverDone->load(); }, 5s));
}


// =========================================================================
// TLS client handshake timeout
// =========================================================================

#ifdef SOCCER_HAS_TLS
TEST(limits_tls_client_handshake_times_out_against_a_silent_server) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    auto stop = std::make_shared<std::atomic<bool>>(false);
    YarnBall::coSpawn(acceptAndHold(std::move(listener), stop));

    Soccer::TlsClientOptions opts;
    opts.insecure_no_verify_cert = true;
    opts.insecure_no_verify_name = true;
    opts.handshakeTimeout = 300ms;
    const auto start = std::chrono::steady_clock::now();
    bool threw = false;
    try {
        (void) YarnBall::syncWait(YarnBall::withTimeout(
            Soccer::TlsStream::connect("127.0.0.1", port, opts), 8s));
    } catch (const Soccer::SocketException &e) {
        threw = std::string(e.what()).find("timed out") != std::string::npos;
    }
    EXPECT_TRUE(threw);
    EXPECT_TRUE(std::chrono::steady_clock::now() - start < 4s);
    stop->store(true);
}
#endif


// =========================================================================
// HTTP/2: close() wakes waiters, handler cancellation
// =========================================================================

#ifdef SOCCER_HAS_HTTP2
namespace {
    struct H2Outcome {
        std::atomic<bool> done{false};
        std::atomic<bool> threw{false};
    };

    YarnBall::Task<void> h2RequestAndRecord(std::shared_ptr<Soccer::Http2Connection> conn,
                                            std::shared_ptr<H2Outcome> out) {
        try {
            (void) co_await conn->request("GET", "/slow");
        } catch (const std::exception &) {
            out->threw = true;
        }
        out->done = true;
        co_return;
    }
}

TEST(limits_h2_close_fails_pending_requests_and_stops_the_driver) {
    Soccer::Http2Server server("127.0.0.1", 0);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    server.route("GET", "/slow",
        [cancelled](Soccer::HttpRequest req) -> YarnBall::Task<Soccer::HttpResponse> {
            // Poll the request's cancel token: it must fire once the client goes away.
            for (int i = 0; i < 600 && !req.cancel.stop_requested(); ++i) {
                co_await YarnBall::sleepFor(10ms);
            }
            if (req.cancel.stop_requested()) cancelled->store(true);
            co_return Soccer::HttpResponse{};
        });
    const std::uint16_t port = server.localAddress().port();
    std::stop_source src;
    YarnBall::coSpawn(server.serve(src.get_token()));

    auto conn = std::make_shared<Soccer::Http2Connection>(
        YarnBall::syncWait(Soccer::Http2Connection::connectPlain("127.0.0.1", port)));
    auto out = std::make_shared<H2Outcome>();
    YarnBall::coSpawn(h2RequestAndRecord(conn, out));
    std::this_thread::sleep_for(200ms); // the request is now parked waiting for a response that will not come

    conn->close();
    EXPECT_TRUE(waitUntil([&] { return out->done.load(); }, 2s));
    EXPECT_TRUE(out->threw.load());
    EXPECT_TRUE(conn->isClosed());
    // And the server noticed: the handler's cancel token fired.
    EXPECT_TRUE(waitUntil([&] { return cancelled->load(); }, 3s));

    src.request_stop();
    std::this_thread::sleep_for(50ms);
}

namespace {
    struct H2Frame {
        std::uint8_t type;
        std::uint8_t flags;
        std::uint32_t stream;
        std::string payload;
    };

    std::string h2Frame(std::uint8_t type, std::uint8_t flags, std::uint32_t stream,
                        const std::string &payload) {
        std::string f;
        f.push_back(static_cast<char>((payload.size() >> 16) & 0xff));
        f.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
        f.push_back(static_cast<char>(payload.size() & 0xff));
        f.push_back(static_cast<char>(type));
        f.push_back(static_cast<char>(flags));
        for (int i = 3; i >= 0; --i) f.push_back(static_cast<char>((stream >> (i * 8)) & 0xff));
        return f + payload;
    }

    /// A request header block: :method (GET or POST), :scheme http, :path /, :authority x.
    std::string h2RequestBlock(bool post) {
        std::string b;
        b.push_back(static_cast<char>(post ? 0x83 : 0x82));
        b.push_back(static_cast<char>(0x86));
        b.push_back(static_cast<char>(0x84));
        b.push_back(static_cast<char>(0x41));
        b.push_back(static_cast<char>(0x01));
        b.push_back('x');
        return b;
    }

    std::string h2Rst(std::uint32_t stream, std::uint32_t code) {
        std::string p;
        for (int i = 3; i >= 0; --i) p.push_back(static_cast<char>((code >> (i * 8)) & 0xff));
        return h2Frame(0x3, 0, stream, p);
    }

    std::vector<H2Frame> parseH2(const std::string &bytes) {
        std::vector<H2Frame> out;
        std::size_t i = 0;
        while (i + 9 <= bytes.size()) {
            const auto b = [&](std::size_t k) { return static_cast<std::uint8_t>(bytes[i + k]); };
            const std::size_t len = (std::size_t(b(0)) << 16) | (std::size_t(b(1)) << 8) | b(2);
            if (i + 9 + len > bytes.size()) break;
            H2Frame f;
            f.type = b(3);
            f.flags = b(4);
            f.stream = (std::uint32_t(b(5) & 0x7f) << 24) | (std::uint32_t(b(6)) << 16) |
                       (std::uint32_t(b(7)) << 8) | b(8);
            f.payload = bytes.substr(i + 9, len);
            out.push_back(std::move(f));
            i += 9 + len;
        }
        return out;
    }

    std::uint32_t be32(const std::string &p, std::size_t off) {
        return (std::uint32_t(static_cast<std::uint8_t>(p[off])) << 24) |
               (std::uint32_t(static_cast<std::uint8_t>(p[off + 1])) << 16) |
               (std::uint32_t(static_cast<std::uint8_t>(p[off + 2])) << 8) |
               std::uint32_t(static_cast<std::uint8_t>(p[off + 3]));
    }

    /// Send raw bytes to an h2c server and collect what comes back until it closes or 1.5 s pass.
    YarnBall::Task<std::string> h2RawExchange(std::uint16_t port, std::string bytes) {
        auto s = co_await Soccer::tcpConnect("127.0.0.1", port);
        Soccer::IoDeadline cutoff(s.fd(), 1500ms);
        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" + h2Frame(0x4, 0, 0, "");
        const std::string all = preface + bytes;
        co_await s.write(std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(all.data()), all.size()));
        std::string out;
        std::array<std::byte, 4096> buf{};
        try {
            while (true) {
                const std::size_t n = co_await s.read(buf);
                if (n == 0) break;
                out.append(reinterpret_cast<const char *>(buf.data()), n);
            }
        } catch (const Soccer::SocketException &) {
        }
        co_return out;
    }

    struct H2Fixture {
        std::unique_ptr<Soccer::Http2Server> server;
        std::stop_source stop;
        std::uint16_t port = 0;
        explicit H2Fixture(Soccer::Http2ServerOptions o) {
            server = std::make_unique<Soccer::Http2Server>("127.0.0.1", 0, o);
            server->route("GET", "/", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
                co_return Soccer::HttpResponse{};
            });
            port = server->localAddress().port();
            YarnBall::coSpawn(server->serve(stop.get_token()));
        }
        ~H2Fixture() {
            stop.request_stop();
            std::this_thread::sleep_for(50ms);
        }
    };
}

TEST(limits_h2_server_closes_a_connection_that_resets_streams_in_a_flood) {
    Soccer::Http2ServerOptions o;
    o.maxResetsPerSecond = 20;
    H2Fixture fx(o);

    std::string burst;
    for (std::uint32_t i = 0; i < 80; ++i) {
        const std::uint32_t id = 2 * i + 1;
        burst += h2Frame(0x1, 0x4 | 0x1, id, h2RequestBlock(false)); // HEADERS, END_HEADERS | END_STREAM
        burst += h2Rst(id, 8);                                      // CANCEL
    }
    const auto frames = parseH2(YarnBall::syncWait(h2RawExchange(fx.port, burst)));
    bool calm = false;
    for (const auto &f : frames) {
        if (f.type == 0x7 && f.payload.size() >= 8 && be32(f.payload, 4) == 0xb) calm = true; // GOAWAY ENHANCE_YOUR_CALM
    }
    EXPECT_TRUE(calm);
}

TEST(limits_h2_server_refuses_streams_beyond_the_concurrency_cap) {
    Soccer::Http2ServerOptions o;
    o.maxConcurrentStreams = 2;
    H2Fixture fx(o);

    std::string reqs;
    for (std::uint32_t id : {1u, 3u, 5u}) {
        reqs += h2Frame(0x1, 0x4, id, h2RequestBlock(true)); // END_HEADERS only: the streams stay open
    }
    const auto frames = parseH2(YarnBall::syncWait(h2RawExchange(fx.port, reqs)));
    bool refusedThird = false;
    bool refusedFirst = false;
    for (const auto &f : frames) {
        if (f.type == 0x3 && f.payload.size() == 4 && be32(f.payload, 0) == 0x7) { // RST_STREAM REFUSED_STREAM
            if (f.stream == 5) refusedThird = true;
            if (f.stream == 1) refusedFirst = true;
        }
    }
    EXPECT_TRUE(refusedThird);
    EXPECT_FALSE(refusedFirst);
}

TEST(limits_h2_server_caps_request_bodies_buffered_across_a_connection) {
    Soccer::Http2ServerOptions o;
    o.maxConnectionBodyBytes = 100;
    H2Fixture fx(o);

    std::string reqs = h2Frame(0x1, 0x4, 1, h2RequestBlock(true));          // POST, body to follow
    reqs += h2Frame(0x0, 0, 1, std::string(150, 'x'));                       // DATA beyond the budget
    const auto frames = parseH2(YarnBall::syncWait(h2RawExchange(fx.port, reqs)));
    bool calm = false;
    for (const auto &f : frames) {
        if (f.type == 0x7 && f.payload.size() >= 8 && be32(f.payload, 4) == 0xb) calm = true;
    }
    EXPECT_TRUE(calm);
}

namespace {
    YarnBall::Task<std::string> h2MixedCaseHeaders(std::uint16_t port) {
        auto conn = co_await Soccer::Http2Connection::connectPlain("127.0.0.1", port);
        // Named, not a braced temporary inside the co_await: GCC 14 ICEs on that.
        std::vector<Soccer::HttpHeader> hdrs{{"X-Test", "hello"}};
        auto r = co_await conn.request("GET", "/echo", std::move(hdrs));
        co_return r.header("x-echo") + "/" + r.body;
    }
}

TEST(limits_h2_mixed_case_header_names_are_folded_to_lowercase) {
    Soccer::Http2Server server("127.0.0.1", 0);
    server.route("GET", "/echo", [](Soccer::HttpRequest req) -> YarnBall::Task<Soccer::HttpResponse> {
        Soccer::HttpResponse r;
        r.headers.push_back({"X-Echo", "1"});
        r.body = req.header("X-Test");
        co_return r;
    });
    const std::uint16_t port = server.localAddress().port();
    std::stop_source src;
    YarnBall::coSpawn(server.serve(src.get_token()));
    EXPECT_EQ(YarnBall::syncWait(YarnBall::withTimeout(h2MixedCaseHeaders(port), 5s)), std::string("1/hello"));
    src.request_stop();
    std::this_thread::sleep_for(50ms);
}

namespace {
    YarnBall::Task<int> h2StatusOf(std::uint16_t port, std::string path) {
        auto conn = co_await Soccer::Http2Connection::connectPlain("127.0.0.1", port);
        auto r = co_await conn.request("GET", std::move(path));
        co_return r.status;
    }
}

TEST(limits_h2_server_enforces_header_count_and_still_serves_normal_requests) {
    Soccer::Http2ServerOptions o;
    o.maxHeaderCount = 3;
    Soccer::Http2Server server("127.0.0.1", 0, o);
    server.route("GET", "/ok", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
        Soccer::HttpResponse r;
        r.body = "fine";
        co_return r;
    });
    const std::uint16_t port = server.localAddress().port();
    std::stop_source src;
    YarnBall::coSpawn(server.serve(src.get_token()));

    EXPECT_EQ(YarnBall::syncWait(h2StatusOf(port, "/ok")), 200);

    // Five extra headers exceed the cap: the stream is reset, the request fails.
    bool failed = false;
    try {
        auto conn = YarnBall::syncWait(Soccer::Http2Connection::connectPlain("127.0.0.1", port));
        std::vector<Soccer::HttpHeader> hs;
        for (int i = 0; i < 6; ++i) hs.push_back({"x-h" + std::to_string(i), "v"});
        (void) YarnBall::syncWait(YarnBall::withTimeout(conn.request("GET", "/ok", hs), 5s));
    } catch (const std::exception &) {
        failed = true;
    }
    EXPECT_TRUE(failed);

    src.request_stop();
    std::this_thread::sleep_for(50ms);
}
#endif


// =========================================================================
// UDP truncation, close-on-exec
// =========================================================================

TEST(limits_udp_recv_reports_truncated_datagrams) {
    auto rx = Soccer::UdpSocket::bind("127.0.0.1", 0);
    auto tx = Soccer::UdpSocket::bind("127.0.0.1", 0);
    const auto dest = Soccer::SocketAddress::resolve("127.0.0.1", rx.localAddress().port());

    std::array<std::byte, 100> payload{};
    for (auto &b : payload) b = std::byte{0x5a};
    (void) YarnBall::syncWait(tx.sendTo(payload, dest));

    std::array<std::byte, 10> small{};
    bool truncated = false;
    Soccer::SocketAddress from;
    const std::size_t n = YarnBall::syncWait(
        YarnBall::withTimeout(rx.recvFrom(small, &from, &truncated), 5s));
    EXPECT_EQ(n, static_cast<std::size_t>(10));
    EXPECT_TRUE(truncated);

    (void) YarnBall::syncWait(tx.sendTo(std::span<const std::byte>(payload.data(), 10), dest));
    truncated = true;
    const std::size_t m = YarnBall::syncWait(
        YarnBall::withTimeout(rx.recvFrom(small, &from, &truncated), 5s));
    EXPECT_EQ(m, static_cast<std::size_t>(10));
    EXPECT_FALSE(truncated); // exactly fits
}

#ifndef _WIN32
TEST(limits_sockets_are_close_on_exec) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    EXPECT_TRUE((::fcntl(listener.fd(), F_GETFD) & FD_CLOEXEC) != 0);
    const std::uint16_t port = listener.localAddress().port();

    auto accepted = std::make_shared<std::atomic<int>>(-1);
    auto acceptOne = [](Soccer::TcpListener l, std::shared_ptr<std::atomic<int>> fdOut,
                        std::shared_ptr<std::atomic<bool>> stop) -> YarnBall::Task<void> {
        auto s = co_await l.accept();
        fdOut->store(::fcntl(s.fd(), F_GETFD));
        while (!stop->load()) co_await YarnBall::sleepFor(5ms);
        co_return;
    };
    auto stop = std::make_shared<std::atomic<bool>>(false);
    // The lambda is invoked immediately and holds no captures, so its frame
    // does not depend on the closure's lifetime.
    YarnBall::coSpawn(acceptOne(std::move(listener), accepted, stop));

    auto client = YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port));
    EXPECT_TRUE((::fcntl(client.fd(), F_GETFD) & FD_CLOEXEC) != 0);
    EXPECT_TRUE(waitUntil([&] { return accepted->load() != -1; }, 3s));
    EXPECT_TRUE((accepted->load() & FD_CLOEXEC) != 0);

    auto udp = Soccer::UdpSocket::bind("127.0.0.1", 0);
    EXPECT_TRUE((::fcntl(udp.fd(), F_GETFD) & FD_CLOEXEC) != 0);
    stop->store(true);
}
#endif



// =========================================================================
// Connect timeout (cancellation scope, not shutdown)
// =========================================================================

#ifndef _WIN32
namespace {
    /// A listener that never accepts, with its accept queue filled by raw connections, so
    /// the next SYN to it goes unanswered and a connect() to it hangs.
    struct FullBacklog {
        int listenFd = -1;
        std::uint16_t port = 0;
        std::vector<int> fillers;

        FullBacklog() {
            listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = 0;
            ::bind(listenFd, reinterpret_cast<sockaddr *>(&a), sizeof(a));
            ::listen(listenFd, 0);
            socklen_t len = sizeof(a);
            ::getsockname(listenFd, reinterpret_cast<sockaddr *>(&a), &len);
            port = ntohs(a.sin_port);
            // Where the queue is bounded by a large system limit (somaxconn is 128 on macOS)
            // it takes that many connections to fill it, so lift the descriptor limit first.
            rlimit rl{};
            if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < 1024) {
                rl.rlim_cur = rl.rlim_max < 1024 ? rl.rlim_max : 1024;
                (void) ::setrlimit(RLIMIT_NOFILE, &rl);
            }
            for (int i = 0; i < 200; ++i) {
                const int c = ::socket(AF_INET, SOCK_STREAM, 0);
                ::fcntl(c, F_SETFL, ::fcntl(c, F_GETFL) | O_NONBLOCK);
                ::connect(c, reinterpret_cast<sockaddr *>(&a), sizeof(a)); // EINPROGRESS or done
                fillers.push_back(c);
            }
            std::this_thread::sleep_for(100ms);
        }
        ~FullBacklog() {
            for (int c : fillers) ::close(c);
            ::close(listenFd);
        }
        FullBacklog(const FullBacklog &) = delete;
        FullBacklog &operator=(const FullBacklog &) = delete;
    };

    /// @return 1 = timed out as asked, 0 = the connect went through (this platform queued it
    ///         anyway; nothing to assert), -1 = failed some other way.
    int connectTimeoutOutcome(std::uint16_t port, std::chrono::milliseconds limit,
                              std::chrono::steady_clock::duration &elapsed) {
        const auto start = std::chrono::steady_clock::now();
        int outcome = -1;
        try {
            auto s = YarnBall::syncWait(YarnBall::withTimeout(Soccer::tcpConnect("127.0.0.1", port, limit), 10s));
            outcome = 0;
        } catch (const Soccer::SocketException &e) {
            if (std::string(e.what()).find("timed out") != std::string::npos) outcome = 1;
        } catch (const std::exception &) {
        }
        elapsed = std::chrono::steady_clock::now() - start;
        return outcome;
    }
}

namespace {
    std::size_t openFdCount() {
        std::size_t n = 0;
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator("/dev/fd", ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            ++n;
        }
        return n;
    }
}

TEST(limits_tcp_connect_times_out_while_the_handshake_is_still_pending) {
    FullBacklog fx;
    std::chrono::steady_clock::duration elapsed{};
    const int outcome = connectTimeoutOutcome(fx.port, 300ms, elapsed);
    EXPECT_TRUE(outcome != -1);
    if (outcome == 1) {
        EXPECT_TRUE(elapsed >= 250ms);
        EXPECT_TRUE(elapsed < 3s);
        // The abandoned attempts closed their sockets instead of leaking them (measured after
        // the first one, which may itself start the runtime's own descriptors).
        std::this_thread::sleep_for(100ms);
        const std::size_t fdsBefore = openFdCount();
        std::chrono::steady_clock::duration ignored{};
        for (int i = 0; i < 12; ++i) (void) connectTimeoutOutcome(fx.port, 100ms, ignored);
        EXPECT_TRUE(waitUntil([&] { return openFdCount() <= fdsBefore; }, 2s));
    } else {
        std::cout << "       (queue not full on this platform: timeout path not exercised)\n";
    }
}

TEST(limits_http_client_connect_uses_the_connect_timeout) {
    FullBacklog fx;
    Soccer::HttpClient::setConnectTimeout(300ms);
    bool timedOut = false;
    bool connected = false;
    const auto start = std::chrono::steady_clock::now();
    try {
        (void) YarnBall::syncWait(YarnBall::withTimeout(
            Soccer::HttpClient::get("127.0.0.1", fx.port, "/"), 3s));
        connected = true;
    } catch (const Soccer::SocketException &e) {
        timedOut = std::string(e.what()).find("timed out") != std::string::npos;
    } catch (const std::exception &) {
        // The handshake completed after all (this platform queued it) and the request then
        // sat waiting on a peer that never answers.
        connected = true;
    }
    Soccer::HttpClient::setConnectTimeout(Soccer::HttpClient::kDefaultConnectTimeout);
    EXPECT_TRUE(timedOut || connected);
    if (timedOut) EXPECT_TRUE(std::chrono::steady_clock::now() - start < 3s);
}

TEST(limits_tcp_connect_with_a_timeout_still_connects_normally) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    auto client = YarnBall::syncWait(YarnBall::withTimeout(Soccer::tcpConnect("127.0.0.1", port, 2s), 5s));
    EXPECT_TRUE(client.fd() >= 0);
}


// =========================================================================
// close() wakes a parked waiter, which fails instead of using the closed fd
// =========================================================================

namespace {
    struct WakeResult {
        std::atomic<bool> done{false};
        std::atomic<bool> threw{false};
        std::mutex mu;
        std::string what;
    };

    YarnBall::Task<void> parkedRead(Soccer::TcpStream *s, std::shared_ptr<WakeResult> out) {
        std::array<std::byte, 16> buf{};
        try {
            (void) co_await s->read(buf);
        } catch (const std::exception &e) {
            out->threw = true;
            std::lock_guard<std::mutex> lk(out->mu);
            out->what = e.what();
        }
        out->done = true;
        co_return;
    }

    YarnBall::Task<void> parkedAccept(Soccer::TcpListener *l, std::shared_ptr<WakeResult> out) {
        try {
            (void) co_await l->accept();
        } catch (const std::exception &e) {
            out->threw = true;
            std::lock_guard<std::mutex> lk(out->mu);
            out->what = e.what();
        }
        out->done = true;
        co_return;
    }

    YarnBall::Task<void> parkedRecv(Soccer::UdpSocket *u, std::shared_ptr<WakeResult> out) {
        std::array<std::byte, 16> buf{};
        try {
            (void) co_await u->recvFrom(buf, nullptr);
        } catch (const std::exception &e) {
            out->threw = true;
            std::lock_guard<std::mutex> lk(out->mu);
            out->what = e.what();
        }
        out->done = true;
        co_return;
    }
}

TEST(limits_tcp_stream_close_wakes_a_parked_reader_which_then_fails) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    auto client = std::make_unique<Soccer::TcpStream>(YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port)));
    auto peer = YarnBall::syncWait(YarnBall::withTimeout(listener.accept(), 5s)); // sends nothing
    auto out = std::make_shared<WakeResult>();
    YarnBall::coSpawn(parkedRead(client.get(), out));
    std::this_thread::sleep_for(150ms);
    EXPECT_FALSE(out->done.load());

    client->close();
    EXPECT_TRUE(waitUntil([&] { return out->done.load(); }, 3s));
    EXPECT_TRUE(out->threw.load());
    EXPECT_EQ(client->fd(), -1);
}

TEST(limits_tcp_listener_close_wakes_a_parked_accept_which_then_fails) {
    auto listener = std::make_unique<Soccer::TcpListener>(Soccer::TcpListener::bind("127.0.0.1", 0));
    auto out = std::make_shared<WakeResult>();
    YarnBall::coSpawn(parkedAccept(listener.get(), out));
    std::this_thread::sleep_for(150ms);
    EXPECT_FALSE(out->done.load());

    listener->close();
    EXPECT_TRUE(waitUntil([&] { return out->done.load(); }, 3s));
    EXPECT_TRUE(out->threw.load());
}

TEST(limits_udp_close_wakes_a_parked_receiver_which_then_fails) {
    auto udp = std::make_unique<Soccer::UdpSocket>(Soccer::UdpSocket::bind("127.0.0.1", 0));
    auto out = std::make_shared<WakeResult>();
    YarnBall::coSpawn(parkedRecv(udp.get(), out));
    std::this_thread::sleep_for(150ms);
    EXPECT_FALSE(out->done.load());

    udp->close();
    EXPECT_TRUE(waitUntil([&] { return out->done.load(); }, 3s));
    EXPECT_TRUE(out->threw.load());
}
#endif // !_WIN32


// =========================================================================
// wsServe: connection cap, handshake timeout, Origin policy
// =========================================================================

namespace {
    struct WsServeState {
        std::atomic<int> handled{0};
        std::atomic<bool> release{false};
    };

    YarnBall::Task<void> wsHold(std::shared_ptr<WsServeState> st, Soccer::WsConnection ws) {
        st->handled.fetch_add(1);
        while (!st->release.load()) co_await YarnBall::sleepFor(5ms);
        try { co_await ws.close(); } catch (...) {}
        co_return;
    }

    struct WsServeFixture {
        std::shared_ptr<WsServeState> state = std::make_shared<WsServeState>();
        std::stop_source stop;
        std::uint16_t port = 0;
        explicit WsServeFixture(Soccer::WsOptions o) {
            auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
            port = listener.localAddress().port();
            auto st = state;
            YarnBall::coSpawn(Soccer::wsServe(
                std::move(listener),
                [st](Soccer::WsConnection ws) -> YarnBall::Task<void> { return wsHold(st, std::move(ws)); },
                std::move(o), stop.get_token()));
        }
        ~WsServeFixture() {
            state->release = true;
            stop.request_stop();
            std::this_thread::sleep_for(50ms);
        }
    };

    /// Sends @p request and returns the response head; the connection is then dropped.
    YarnBall::Task<std::string> wsUpgradeReplyHead(std::uint16_t port, std::string request) {
        auto s = co_await Soccer::tcpConnect("127.0.0.1", port);
        co_await s.write(std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(request.data()), request.size()));
        std::string head;
        std::array<std::byte, 256> buf{};
        while (head.find("\r\n\r\n") == std::string::npos) {
            const std::size_t n = co_await s.read(buf);
            if (n == 0) break;
            head.append(reinterpret_cast<const char *>(buf.data()), n);
        }
        co_return head;
    }

    YarnBall::Task<std::shared_ptr<Soccer::WsConnection>> wsDial(std::uint16_t port) {
        co_return std::make_shared<Soccer::WsConnection>(
            co_await Soccer::WsConnection::connect("127.0.0.1", port, "/"));
    }
}

TEST(limits_ws_serve_enforces_max_connections_including_half_open_handshakes) {
    Soccer::WsOptions o;
    o.maxConnections = 2;
    WsServeFixture fx(o);

    auto a = YarnBall::syncWait(YarnBall::withTimeout(wsDial(fx.port), 5s));
    auto b = YarnBall::syncWait(YarnBall::withTimeout(wsDial(fx.port), 5s));
    EXPECT_TRUE(waitUntil([&] { return fx.state->handled.load() == 2; }, 3s));

    // The third is over the cap: closed without an answer.
    const std::string reply = exchange(fx.port, wsUpgradeRequest(""));
    EXPECT_TRUE(reply.find("101") == std::string::npos);
    EXPECT_EQ(fx.state->handled.load(), 2);

    // Once a slot frees up, a new client is served again.
    fx.state->release = true;
    EXPECT_TRUE(waitUntil([&] {
        try {
            auto c = YarnBall::syncWait(YarnBall::withTimeout(wsDial(fx.port), 2s));
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }, 5s));
}

TEST(limits_ws_serve_drops_a_silent_peer_after_the_handshake_timeout_and_frees_its_slot) {
    Soccer::WsOptions o;
    o.maxConnections = 1;
    o.handshakeTimeout = 200ms;
    WsServeFixture fx(o);

    auto silent = YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", fx.port)); // holds the only slot, says nothing
    std::this_thread::sleep_for(700ms);                                         // handshake timeout + sweeper tick
    auto ok = YarnBall::syncWait(YarnBall::withTimeout(wsDial(fx.port), 5s));
    EXPECT_TRUE(waitUntil([&] { return fx.state->handled.load() == 1; }, 3s));
}

TEST(limits_ws_serve_applies_the_origin_policy) {
    Soccer::WsOptions o;
    o.allowedOrigins = {"https://good.example"};
    WsServeFixture fx(o);

    EXPECT_TRUE(exchange(fx.port, wsUpgradeRequest("https://evil.example")).rfind("HTTP/1.1 403", 0) == 0);
    EXPECT_EQ(fx.state->handled.load(), 0);
    EXPECT_TRUE(YarnBall::syncWait(YarnBall::withTimeout(
        wsUpgradeReplyHead(fx.port, wsUpgradeRequest("https://good.example")), 5s)).rfind("HTTP/1.1 101", 0) == 0);
    EXPECT_TRUE(waitUntil([&] { return fx.state->handled.load() == 1; }, 3s));
}


// =========================================================================
// WebSocket write timeout against a peer that stops reading
// =========================================================================

namespace {
    YarnBall::Task<void> wsFloodUntilTimeout(Soccer::TcpListener l, std::shared_ptr<WsServerResult> out) {
        try {
            auto client = co_await l.accept();
            Soccer::WsOptions o;
            o.writeTimeout = 300ms;
            auto ws = co_await Soccer::WsConnection::serverHandshake(std::move(client), o);
            const std::vector<std::byte> chunk(1024 * 1024, std::byte{0x41});
            for (int i = 0; i < 400; ++i) co_await ws.sendBinary(chunk);
            out->ok = true; // never blocked: the test is vacuous
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lk(out->mu);
            out->error = e.what();
        }
        out->done = true;
        co_return;
    }

    /// Upgrades, then never reads again.
    YarnBall::Task<void> wsStopReadingClient(std::uint16_t port, std::shared_ptr<std::atomic<bool>> stop) {
        auto s = co_await Soccer::tcpConnect("127.0.0.1", port);
        const std::string req = wsUpgradeRequest("");
        co_await s.write(std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(req.data()), req.size()));
        std::array<std::byte, 256> buf{};
        std::string head;
        while (head.find("\r\n\r\n") == std::string::npos) {
            const std::size_t n = co_await s.read(buf);
            if (n == 0) co_return;
            head.append(reinterpret_cast<const char *>(buf.data()), n);
        }
        while (!stop->load()) co_await YarnBall::sleepFor(10ms);
        co_return;
    }
}

TEST(limits_ws_write_times_out_against_a_peer_that_stops_reading) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    auto out = std::make_shared<WsServerResult>();
    auto stop = std::make_shared<std::atomic<bool>>(false);
    YarnBall::coSpawn(wsFloodUntilTimeout(std::move(listener), out));
    YarnBall::coSpawn(wsStopReadingClient(port, stop));

    EXPECT_TRUE(waitUntil([&] { return out->done.load(); }, 20s));
    stop->store(true);
    EXPECT_FALSE(out->ok.load());
    std::lock_guard<std::mutex> lk(out->mu);
    EXPECT_TRUE(out->error.find("timed out") != std::string::npos);
}


// =========================================================================
// HTTP/2 client: 1xx interim responses and trailers
// =========================================================================

#ifdef SOCCER_HAS_HTTP2
namespace {
    /// HPACK literal header field without indexing, new name, no Huffman (names and values < 127 bytes).
    std::string hpackLiteral(const std::string &name, const std::string &value) {
        std::string b;
        b.push_back(0x00);
        b.push_back(static_cast<char>(name.size()));
        b += name;
        b.push_back(static_cast<char>(value.size()));
        b += value;
        return b;
    }

    /// ":status <code>" as a literal with an indexed name (":status" is static entry 8).
    std::string hpackStatus(const std::string &code) {
        std::string b;
        b.push_back(0x08);
        b.push_back(static_cast<char>(code.size()));
        return b + code;
    }

    YarnBall::Task<std::string> readExactString(Soccer::TcpStream &s, std::size_t n) {
        std::string out;
        std::array<std::byte, 4096> buf{};
        while (out.size() < n) {
            const std::size_t got = co_await s.read(std::span<std::byte>(buf.data(), std::min(buf.size(), n - out.size())));
            if (got == 0) throw Soccer::SocketException("peer closed");
            out.append(reinterpret_cast<const char *>(buf.data()), got);
        }
        co_return out;
    }

    YarnBall::Task<void> writeString(Soccer::TcpStream &s, const std::string &data) {
        co_await s.write(std::span<const std::byte>(reinterpret_cast<const std::byte *>(data.data()), data.size()));
        co_return;
    }

    /// A scripted h2c server: waits for the first request, then sends @p interim 1xx responses,
    /// the final response (with a header, a body and trailers).
    YarnBall::Task<void> scriptedH2Server(Soccer::TcpListener l, int interim, std::shared_ptr<std::atomic<bool>> stop) {
        try {
            auto s = co_await l.accept();
            (void) co_await readExactString(s, 24); // client preface
            co_await writeString(s, h2Frame(0x4, 0, 0, ""));
            std::uint32_t stream = 0;
            while (stream == 0) {
                const std::string hdr = co_await readExactString(s, 9);
                const std::size_t len = (std::size_t(static_cast<std::uint8_t>(hdr[0])) << 16) |
                                        (std::size_t(static_cast<std::uint8_t>(hdr[1])) << 8) |
                                        std::size_t(static_cast<std::uint8_t>(hdr[2]));
                const std::uint8_t type = static_cast<std::uint8_t>(hdr[3]);
                const std::uint8_t flags = static_cast<std::uint8_t>(hdr[4]);
                if (len > 0) (void) co_await readExactString(s, len);
                if (type == 0x4 && (flags & 0x1) == 0) co_await writeString(s, h2Frame(0x4, 0x1, 0, "")); // ACK
                if (type == 0x1) stream = be32(hdr, 5) & 0x7fffffff;
            }
            std::string out;
            for (int i = 0; i < interim; ++i) {
                out += h2Frame(0x1, 0x4, stream, hpackStatus("103") + hpackLiteral("link", "</style.css>"));
            }
            out += h2Frame(0x1, 0x4, stream, std::string(1, static_cast<char>(0x88)) + hpackLiteral("x-final", "yes"));
            out += h2Frame(0x0, 0, stream, "hello");
            out += h2Frame(0x1, 0x4 | 0x1, stream, hpackLiteral("x-trailer", "t1"));
            co_await writeString(s, out);
            while (!stop->load()) co_await YarnBall::sleepFor(10ms);
        } catch (const std::exception &) {
        }
        co_return;
    }

    YarnBall::Task<Soccer::Http2Response> h2GetFrom(std::uint16_t port) {
        auto conn = co_await Soccer::Http2Connection::connectPlain("127.0.0.1", port);
        co_return co_await conn.request("GET", "/");
    }

    bool hasHeader(const std::vector<Soccer::HttpHeader> &hs, const std::string &name) {
        for (const auto &h : hs) {
            if (h.name == name) return true;
        }
        return false;
    }
}

TEST(limits_h2_client_skips_1xx_responses_and_separates_trailers) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    auto stop = std::make_shared<std::atomic<bool>>(false);
    YarnBall::coSpawn(scriptedH2Server(std::move(listener), 2, stop));

    const auto r = YarnBall::syncWait(YarnBall::withTimeout(h2GetFrom(port), 5s));
    stop->store(true);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, std::string("hello"));
    EXPECT_TRUE(hasHeader(r.headers, "x-final"));
    EXPECT_FALSE(hasHeader(r.headers, "link"));      // belonged to the interim responses
    EXPECT_FALSE(hasHeader(r.headers, "x-trailer")); // trailers are not headers
    EXPECT_TRUE(hasHeader(r.trailers, "x-trailer"));
    EXPECT_EQ(r.trailers.size(), static_cast<std::size_t>(1));
}

TEST(limits_h2_client_rejects_a_flood_of_1xx_responses) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    auto stop = std::make_shared<std::atomic<bool>>(false);
    YarnBall::coSpawn(scriptedH2Server(std::move(listener), 20, stop));

    bool failed = false;
    try {
        (void) YarnBall::syncWait(YarnBall::withTimeout(h2GetFrom(port), 5s));
    } catch (const std::exception &) {
        failed = true;
    }
    stop->store(true);
    EXPECT_TRUE(failed);
}

TEST(limits_h2_server_trailers_reach_the_client_as_trailers) {
    Soccer::Http2Server server("127.0.0.1", 0);
    server.route("GET", "/t", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
        Soccer::HttpResponse r;
        r.body = "payload";
        r.trailers.push_back({"x-checksum", "abc"});
        co_return r;
    });
    const std::uint16_t port = server.localAddress().port();
    std::stop_source src;
    YarnBall::coSpawn(server.serve(src.get_token()));

    auto conn = YarnBall::syncWait(Soccer::Http2Connection::connectPlain("127.0.0.1", port));
    const auto r = YarnBall::syncWait(YarnBall::withTimeout(conn.request("GET", "/t"), 5s));
    EXPECT_EQ(r.body, std::string("payload"));
    EXPECT_TRUE(hasHeader(r.trailers, "x-checksum"));
    EXPECT_FALSE(hasHeader(r.headers, "x-checksum"));
    src.request_stop();
    std::this_thread::sleep_for(50ms);
}


// =========================================================================
// HTTP/2 server: header list cap, request trickle
// =========================================================================

namespace {
    struct H2CountingServer {
        std::unique_ptr<Soccer::Http2Server> server;
        std::stop_source stop;
        std::shared_ptr<std::atomic<int>> calls = std::make_shared<std::atomic<int>>(0);
        std::uint16_t port = 0;
        explicit H2CountingServer(Soccer::Http2ServerOptions o) {
            server = std::make_unique<Soccer::Http2Server>("127.0.0.1", 0, o);
            auto c = calls;
            server->route("GET", "/", [c](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
                c->fetch_add(1);
                co_return Soccer::HttpResponse{};
            });
            port = server->localAddress().port();
            YarnBall::coSpawn(server->serve(stop.get_token()));
        }
        ~H2CountingServer() {
            stop.request_stop();
            std::this_thread::sleep_for(50ms);
        }
    };
}

TEST(limits_h2_server_resets_a_request_whose_header_list_is_too_big) {
    Soccer::Http2ServerOptions o;
    o.maxHeaderListBytes = 1024;
    H2CountingServer fx(o);

    // :method GET, :scheme http, :path /, :authority x, and x-big with a 4000-byte value
    // (HPACK string length 4000 = 127 + 3873 -> 0x7f 0xa1 0x1e).
    std::string block = h2RequestBlock(false);
    block.push_back(0x00);
    block.push_back(0x05);
    block += "x-big";
    block.push_back(0x7f);
    block.push_back(static_cast<char>(0xa1));
    block.push_back(0x1e);
    block += std::string(4000, 'a');
    const auto frames = parseH2(YarnBall::syncWait(h2RawExchange(
        fx.port, h2Frame(0x1, 0x4 | 0x1, 1, block))));

    bool refused = false;
    bool answered = false;
    for (const auto &f : frames) {
        if ((f.type == 0x3 && f.stream == 1) || f.type == 0x7) refused = true; // RST_STREAM on 1, or GOAWAY
        if (f.type == 0x1 && f.stream == 1) answered = true;                   // a response
    }
    EXPECT_TRUE(refused);
    EXPECT_FALSE(answered);
    EXPECT_EQ(fx.calls->load(), 0);

    // A request within the budget still goes through on a fresh connection.
    const auto ok = parseH2(YarnBall::syncWait(h2RawExchange(fx.port, h2Frame(0x1, 0x4 | 0x1, 1, h2RequestBlock(false)))));
    bool served = false;
    for (const auto &f : ok) {
        if (f.type == 0x1 && f.stream == 1) served = true;
    }
    EXPECT_TRUE(served);
    EXPECT_EQ(fx.calls->load(), 1);
}

namespace {
    /// Sends an unfinished request header block, then keeps it alive with a CONTINUATION frame
    /// every 100 ms. Records when (if) the server hangs up.
    YarnBall::Task<void> h2TrickleClient(std::uint16_t port, std::shared_ptr<std::atomic<long long>> closedAfterMs) {
        auto s = co_await Soccer::tcpConnect("127.0.0.1", port);
        const auto start = std::chrono::steady_clock::now();
        const std::string head = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" + h2Frame(0x4, 0, 0, "") +
                                 h2Frame(0x1, 0x0, 1, h2RequestBlock(false)); // HEADERS without END_HEADERS
        co_await writeString(s, head);
        auto closed = std::make_shared<std::atomic<bool>>(false);
        auto reader = [](Soccer::TcpStream *st, std::shared_ptr<std::atomic<bool>> flag) -> YarnBall::Task<void> {
            std::array<std::byte, 1024> buf{};
            try {
                while (true) {
                    if (co_await st->read(buf) == 0) break;
                }
            } catch (const std::exception &) {
            }
            flag->store(true);
            co_return;
        };
        YarnBall::coSpawn(reader(&s, closed));
        const std::string more = h2Frame(0x9, 0x0, 1, hpackLiteral("a", "b")); // CONTINUATION
        for (int i = 0; i < 60 && !closed->load(); ++i) {
            try {
                co_await writeString(s, more);
            } catch (const std::exception &) {
                break;
            }
            co_await YarnBall::sleepFor(100ms);
        }
        // Give the reader a moment to observe EOF if the write was what noticed.
        for (int i = 0; i < 20 && !closed->load(); ++i) co_await YarnBall::sleepFor(10ms);
        closedAfterMs->store(closed->load()
            ? std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()
            : -1);
        s.close(); // wakes the reader before the stream goes away
        co_await YarnBall::sleepFor(20ms);
        co_return;
    }
}

TEST(limits_h2_server_cuts_off_a_request_that_trickles_in_forever) {
    Soccer::Http2ServerOptions o;
    o.requestTimeout = 500ms;
    H2CountingServer fx(o);

    auto closedAfterMs = std::make_shared<std::atomic<long long>>(-2);
    YarnBall::syncWait(YarnBall::withTimeout(h2TrickleClient(fx.port, closedAfterMs), 20s));
    // Each CONTINUATION arrives well inside the request timeout, so only a limit on the
    // request as a whole can end this; it must do so long before the client gives up (6 s).
    EXPECT_TRUE(closedAfterMs->load() >= 400);
    EXPECT_TRUE(closedAfterMs->load() < 4000);
    EXPECT_EQ(fx.calls->load(), 0);
}
#endif // SOCCER_HAS_HTTP2


// =========================================================================
// TLS server: handshake timeout, mutual TLS
// =========================================================================

#ifdef SOCCER_HAS_TLS
namespace {
    /// A scratch directory under the system temp dir, removed on destruction.
    struct TlsScratch {
        std::filesystem::path dir;
        TlsScratch() {
            dir = std::filesystem::temp_directory_path() /
                  ("soccer_tls_" + std::to_string(::getpid()) + "_" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(dir);
        }
        ~TlsScratch() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
        std::string path(const std::string &name) const { return (dir / name).string(); }

        bool run(const std::string &cmd) const { return std::system((cmd + " >/dev/null 2>&1").c_str()) == 0; }

        /// Self-signed CA (ca<tag>.pem/.key).
        bool makeCa(const std::string &tag) const {
            return run("openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 1 -keyout " + path("ca" + tag + ".key") +
                       " -out " + path("ca" + tag + ".pem") + " -subj '/CN=test-ca-" + tag + "'");
        }
        /// Leaf signed by CA @p caTag (leaf<tag>.pem/.key).
        bool makeLeaf(const std::string &tag, const std::string &caTag) const {
            return run("openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout " + path("leaf" + tag + ".key") +
                       " -out " + path("leaf" + tag + ".csr") + " -subj '/CN=client-" + tag + "'") &&
                   run("openssl x509 -req -in " + path("leaf" + tag + ".csr") + " -CA " + path("ca" + caTag + ".pem") +
                       " -CAkey " + path("ca" + caTag + ".key") + " -CAcreateserial -days 1 -out " +
                       path("leaf" + tag + ".pem"));
        }
        /// Self-signed server certificate for 127.0.0.1 (server.pem/.key).
        bool makeServer() const {
            return run("openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 1 -keyout " + path("server.key") +
                       " -out " + path("server.pem") +
                       " -subj '/CN=localhost' -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1'");
        }
    };

    struct TlsServerOutcome {
        std::mutex mu;
        std::vector<std::string> results; ///< per connection: "ok" or the error text
        std::atomic<int> count{0};
    };

    /// Accepts @p connections peers; completes each handshake within @p timeout and echoes one read.
    YarnBall::Task<void> tlsServeSequence(Soccer::TlsListener listener, int connections,
                                          std::chrono::milliseconds timeout,
                                          std::shared_ptr<TlsServerOutcome> out) {
        for (int i = 0; i < connections; ++i) {
            std::string result = "ok";
            try {
                auto pending = co_await listener.acceptPending();
                auto tls = co_await Soccer::TlsListener::completeHandshake(std::move(pending), timeout);
                std::array<std::byte, 64> buf{};
                const std::size_t n = co_await tls.read(buf);
                if (n > 0) co_await tls.write(std::span<const std::byte>(buf.data(), n));
            } catch (const std::exception &e) {
                result = e.what();
            }
            {
                std::lock_guard<std::mutex> lk(out->mu);
                out->results.push_back(result);
            }
            out->count.fetch_add(1);
        }
        co_return;
    }

    /// Connect, ping, and report whether the echo came back.
    YarnBall::Task<bool> tlsPing(std::uint16_t port, Soccer::TlsClientOptions opts) {
        try {
            auto tls = co_await Soccer::TlsStream::connect("127.0.0.1", port, opts);
            const std::string msg = "ping";
            co_await tls.write(std::span<const std::byte>(reinterpret_cast<const std::byte *>(msg.data()), msg.size()));
            std::array<std::byte, 64> buf{};
            const std::size_t n = co_await tls.read(buf);
            co_return n == msg.size();
        } catch (const std::exception &) {
            co_return false;
        }
    }
}

TEST(limits_tls_server_handshake_times_out_on_a_silent_peer) {
    TlsScratch sc;
    if (!sc.makeServer()) { std::cout << "       (skipped: openssl unavailable)\n"; return; }
    auto listener = Soccer::TlsListener::bind("127.0.0.1", 0, sc.path("server.pem"), sc.path("server.key"));
    const std::uint16_t port = listener.localAddress().port();
    auto out = std::make_shared<TlsServerOutcome>();
    YarnBall::coSpawn(tlsServeSequence(std::move(listener), 1, 300ms, out));

    const auto start = std::chrono::steady_clock::now();
    auto silent = YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port)); // connects, then says nothing
    EXPECT_TRUE(waitUntil([&] { return out->count.load() == 1; }, 5s));
    EXPECT_TRUE(std::chrono::steady_clock::now() - start < 4s);
    std::lock_guard<std::mutex> lk(out->mu);
    EXPECT_TRUE(!out->results.empty() && out->results[0].find("timed out") != std::string::npos);
}

TEST(limits_tls_server_requires_a_client_certificate_when_a_client_ca_is_set) {
    TlsScratch sc;
    if (!sc.makeServer() || !sc.makeCa("good") || !sc.makeCa("other") ||
        !sc.makeLeaf("good", "good") || !sc.makeLeaf("rogue", "other")) {
        std::cout << "       (skipped: openssl unavailable)\n";
        return;
    }
    Soccer::TlsServerOptions so;
    so.clientCaFile = sc.path("cagood.pem");
    auto listener = Soccer::TlsListener::bind("127.0.0.1", 0, sc.path("server.pem"), sc.path("server.key"), so);
    const std::uint16_t port = listener.localAddress().port();
    auto out = std::make_shared<TlsServerOutcome>();
    YarnBall::coSpawn(tlsServeSequence(std::move(listener), 3, 5s, out));

    Soccer::TlsClientOptions base;
    base.insecure_no_verify_cert = true;
    base.insecure_no_verify_name = true;

    {   // A certificate signed by the configured CA: accepted.
        Soccer::TlsClientOptions good = base;
        good.clientCertFile = sc.path("leafgood.pem");
        good.clientKeyFile = sc.path("leafgood.key");
        EXPECT_TRUE(YarnBall::syncWait(YarnBall::withTimeout(tlsPing(port, good), 10s)));
    }
    {   // No certificate at all: refused.
        EXPECT_FALSE(YarnBall::syncWait(YarnBall::withTimeout(tlsPing(port, base), 10s)));
    }
    {   // A certificate from some other CA: refused.
        Soccer::TlsClientOptions rogue = base;
        rogue.clientCertFile = sc.path("leafrogue.pem");
        rogue.clientKeyFile = sc.path("leafrogue.key");
        EXPECT_FALSE(YarnBall::syncWait(YarnBall::withTimeout(tlsPing(port, rogue), 10s)));
    }
    EXPECT_TRUE(waitUntil([&] { return out->count.load() == 3; }, 5s));
    std::lock_guard<std::mutex> lk(out->mu);
    EXPECT_EQ(out->results.size(), static_cast<std::size_t>(3));
    if (out->results.size() == 3) {
        EXPECT_EQ(out->results[0], std::string("ok"));
        EXPECT_TRUE(out->results[1] != "ok");
        EXPECT_TRUE(out->results[2] != "ok");
    }
}

TEST(limits_tls_server_without_a_client_ca_accepts_clients_without_certificates) {
    TlsScratch sc;
    if (!sc.makeServer()) { std::cout << "       (skipped: openssl unavailable)\n"; return; }
    auto listener = Soccer::TlsListener::bind("127.0.0.1", 0, sc.path("server.pem"), sc.path("server.key"));
    const std::uint16_t port = listener.localAddress().port();
    auto out = std::make_shared<TlsServerOutcome>();
    YarnBall::coSpawn(tlsServeSequence(std::move(listener), 1, 5s, out));

    Soccer::TlsClientOptions base;
    base.insecure_no_verify_cert = true;
    base.insecure_no_verify_name = true;
    EXPECT_TRUE(YarnBall::syncWait(YarnBall::withTimeout(tlsPing(port, base), 10s)));
}
#endif // SOCCER_HAS_TLS
