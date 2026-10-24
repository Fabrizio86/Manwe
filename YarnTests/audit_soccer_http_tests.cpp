//
// Regression tests for the code-audit fixes: soccer_http.
//

#include "test_framework.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../Yarn/includes/Coroutines.h"
#include "../Yarn/includes/Timeout.h"
#include "../Yarn/includes/Timers.h"
#include "../Soccer/includes/Soccer.h"
#include "../Soccer/includes/WebSocket.h"
#include "../Soccer/includes/Http2.h"

using namespace std::chrono_literals;

namespace {

    // ---- raw TCP helpers ------------------------------------------------

    YarnBall::Task<std::string> auditRawExchange(std::uint16_t port, std::string request) {
        auto s = co_await Soccer::tcpConnect("127.0.0.1", port);
        if (!request.empty()) {
            co_await s.write(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(request.data()), request.size()));
        }
        std::string out;
        std::array<std::byte, 2048> buf{};
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

    std::string auditExchange(std::uint16_t port, std::string request) {
        return YarnBall::syncWait(YarnBall::withTimeout(auditRawExchange(port, std::move(request)), 8s));
    }

    template<typename Pred>
    bool auditWaitUntil(Pred &&p, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!p()) {
            if (std::chrono::steady_clock::now() >= deadline) return p();
            std::this_thread::sleep_for(2ms);
        }
        return true;
    }

    bool startsWith(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

    /// A running HttpServer with a "/ok" route (GET and POST) and a "/boom" route that throws.
    struct HttpFixture {
        std::unique_ptr<Soccer::HttpServer> server;
        std::stop_source stop;
        std::uint16_t port{0};
        std::shared_ptr<std::atomic<int>> handled = std::make_shared<std::atomic<int>>(0);

        explicit HttpFixture(Soccer::HttpServerOptions o = {}) {
            server = std::make_unique<Soccer::HttpServer>("127.0.0.1", 0, o);
            port = server->localAddress().port();
            auto counter = handled;
            auto ok = [counter](Soccer::HttpRequest req) -> YarnBall::Task<Soccer::HttpResponse> {
                counter->fetch_add(1);
                Soccer::HttpResponse r;
                r.body = "ok:" + req.body;
                co_return r;
            };
            server->route("GET", "/ok", ok);
            server->route("POST", "/ok", ok);
            server->route("GET", "/boom", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
                throw std::runtime_error("secret-internal-detail");
                co_return Soccer::HttpResponse{};
            });
            server->route("GET", "/badhead", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
                Soccer::HttpResponse r;
                r.headers.push_back({"X-Test", "a\r\nInjected: yes"});
                co_return r;
            });
            YarnBall::coSpawn(server->serve(stop.get_token()));
        }

        ~HttpFixture() {
            stop.request_stop();
            std::this_thread::sleep_for(30ms);
        }
    };

    // ---- raw mock HTTP server for client tests -----------------------------

    /// Accepts one connection, reads the request head, sends @p reply (then optionally stays silent).
    struct MockHttp {
        std::uint16_t port{0};
        std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();
        std::shared_ptr<std::string> requestHead = std::make_shared<std::string>();
        std::shared_ptr<std::atomic<bool>> release = std::make_shared<std::atomic<bool>>(false);
    };

    YarnBall::Task<void> mockHttpOnce(Soccer::TcpListener l, std::string reply, bool hold,
                                      std::shared_ptr<std::mutex> mu, std::shared_ptr<std::string> head,
                                      std::shared_ptr<std::atomic<bool>> release) {
        try {
            auto c = co_await l.accept();
            std::string got;
            std::array<std::byte, 1024> buf{};
            while (got.find("\r\n\r\n") == std::string::npos) {
                const std::size_t n = co_await c.read(buf);
                if (n == 0) break;
                got.append(reinterpret_cast<const char *>(buf.data()), n);
            }
            {
                std::lock_guard<std::mutex> lk(*mu);
                *head = got;
            }
            if (!reply.empty()) {
                co_await c.write(std::span<const std::byte>(
                    reinterpret_cast<const std::byte *>(reply.data()), reply.size()));
            }
            if (hold) {
                while (!release->load()) co_await YarnBall::sleepFor(10ms);
            }
        } catch (...) {
        }
        co_return;
    }

    MockHttp startMock(std::string reply, bool hold = false) {
        auto l = Soccer::TcpListener::bind("127.0.0.1", 0);
        MockHttp m;
        m.port = l.localAddress().port();
        YarnBall::coSpawn(mockHttpOnce(std::move(l), std::move(reply), hold, m.mu, m.requestHead, m.release));
        return m;
    }

    Soccer::HttpResponse clientGet(std::uint16_t port, const std::string &path,
                                   std::vector<Soccer::HttpHeader> hdrs = {}) {
        return YarnBall::syncWait(YarnBall::withTimeout(
            Soccer::HttpClient::get("127.0.0.1", port, path, std::move(hdrs)), 10s));
    }

} // namespace


// =========================================================================
// HttpServer: request smuggling and malformed-request handling
// =========================================================================

TEST(audit_http_server_rejects_empty_transfer_encoding_decoy) {
    HttpFixture f;
    // An empty Transfer-Encoding ahead of a chunked one used to read as "none".
    auto r = auditExchange(f.port,
        "POST /ok HTTP/1.1\r\nHost: x\r\nTransfer-Encoding:\r\nTransfer-Encoding: chunked\r\n\r\n"
        "0\r\n\r\nGET /ok HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(startsWith(r, "HTTP/1.1 501"));
    EXPECT_EQ(f.handled->load(), 0);
}

TEST(audit_http_server_rejects_transfer_encoding_with_content_length) {
    HttpFixture f;
    auto r = auditExchange(f.port,
        "POST /ok HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\nabcd");
    EXPECT_TRUE(startsWith(r, "HTTP/1.1 400"));
    EXPECT_EQ(f.handled->load(), 0);
}

TEST(audit_http_server_rejects_bare_lf_and_stray_cr) {
    HttpFixture f;
    // Bare LF as the request-line terminator.
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /ok HTTP/1.1\nHost: x\r\n\r\n"), "HTTP/1.1 400"));
    // Bare LF as a header terminator (the line would be one header for a CRLF-only proxy).
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /ok HTTP/1.1\r\nHost: x\nX-A: 1\r\n\r\n"), "HTTP/1.1 400"));
    // A CR inside a header value.
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /ok HTTP/1.1\r\nHost: x\r\nX-A: a\rb\r\n\r\n"), "HTTP/1.1 400"));
    // A NUL inside a header value.
    EXPECT_TRUE(startsWith(auditExchange(f.port,
        std::string("GET /ok HTTP/1.1\r\nHost: x\r\nX-A: a\0b\r\n\r\n", 40)), "HTTP/1.1 400"));
    EXPECT_EQ(f.handled->load(), 0);
}

TEST(audit_http_server_validates_header_names_and_targets) {
    HttpFixture f;
    // Space before the colon, and a name with a character outside the token set.
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /ok HTTP/1.1\r\nHost: x\r\nX-A : 1\r\n\r\n"), "HTTP/1.1 400"));
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /ok HTTP/1.1\r\nHost: x\r\nX(A): 1\r\n\r\n"), "HTTP/1.1 400"));
    // obs-fold continuation.
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /ok HTTP/1.1\r\nHost: x\r\nX-A: 1\r\n  folded\r\n\r\n"), "HTTP/1.1 400"));
    // Fragment in the target, control character in the target.
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /ok#frag HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 400"));
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /o\x01k HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 400"));
    // Unsupported version.
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /ok HTTP/2.0\r\nHost: x\r\n\r\n"), "HTTP/1.1 505"));
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /ok HTTP/1.x\r\nHost: x\r\n\r\n"), "HTTP/1.1 400"));
    EXPECT_EQ(f.handled->load(), 0);
}

TEST(audit_http_server_requires_exactly_one_host_for_http11) {
    HttpFixture f;
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /ok HTTP/1.1\r\n\r\n"), "HTTP/1.1 400"));
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /ok HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n"), "HTTP/1.1 400"));
    EXPECT_EQ(f.handled->load(), 0);
    // HTTP/1.0 needs none, and a well-formed 1.1 request still works.
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /ok HTTP/1.0\r\n\r\n"), "HTTP/1.1 200"));
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /ok HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 200"));
}

TEST(audit_http_server_answers_overlong_lines_with_proper_status) {
    HttpFixture f;
    const std::string big(70 * 1024, 'a');
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /" + big + " HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 414"));
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /ok HTTP/1.1\r\nHost: x\r\nX-A: " + big + "\r\n\r\n"), "HTTP/1.1 431"));
}

TEST(audit_http_server_max_header_bytes_above_64k_is_honoured) {
    Soccer::HttpServerOptions o;
    o.maxHeaderBytes = 256 * 1024;
    HttpFixture f(o);
    const std::string big(100 * 1024, 'a'); // one header line past the old fixed 64 KiB line bound
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /ok HTTP/1.1\r\nHost: x\r\nX-A: " + big + "\r\n\r\n"), "HTTP/1.1 200"));
    // A smaller budget still bounds the line.
    Soccer::HttpServerOptions small;
    small.maxHeaderBytes = 2048;
    HttpFixture g(small);
    const std::string mid(3000, 'a');
    EXPECT_TRUE(startsWith(auditExchange(g.port, "GET /ok HTTP/1.1\r\nHost: x\r\nX-A: " + mid + "\r\n\r\n"), "HTTP/1.1 431"));
}

TEST(audit_http_server_reads_a_large_body_intact) {
    HttpFixture f;
    std::string body(300 * 1024, 'q');
    body[12345] = 'Z';
    auto r = auditExchange(f.port,
        "POST /ok HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    EXPECT_TRUE(startsWith(r, "HTTP/1.1 200"));
    EXPECT_TRUE(r.size() > body.size());
    EXPECT_TRUE(r.compare(r.size() - body.size(), body.size(), body) == 0);
}

TEST(audit_http_server_handler_exception_gives_plain_500_without_detail) {
    HttpFixture f;
    auto r = auditExchange(f.port, "GET /boom HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(startsWith(r, "HTTP/1.1 500"));
    EXPECT_TRUE(r.find("secret-internal-detail") == std::string::npos);
}

TEST(audit_http_server_unsafe_response_head_gives_500) {
    HttpFixture f;
    auto r = auditExchange(f.port, "GET /badhead HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(startsWith(r, "HTTP/1.1 500"));
    EXPECT_TRUE(r.find("Injected") == std::string::npos);
}

TEST(audit_http_server_second_serve_throws_and_default_connection_cap_is_finite) {
    Soccer::HttpServer server("127.0.0.1", 0);
    EXPECT_TRUE(Soccer::HttpServerOptions{}.maxConnections > 0);
    std::stop_source src;
    YarnBall::coSpawn(server.serve(src.get_token()));
    bool threw = false;
    try {
        (void) server.serve(src.get_token());
    } catch (const Soccer::SocketException &) {
        threw = true;
    }
    EXPECT_TRUE(threw);
    src.request_stop();
    std::this_thread::sleep_for(30ms);
}


// =========================================================================
// HttpClient
// =========================================================================

TEST(audit_http_client_rejects_empty_transfer_encoding_decoy) {
    auto m = startMock("HTTP/1.1 200 OK\r\nTransfer-Encoding:\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    bool threw = false;
    try {
        (void) clientGet(m.port, "/");
    } catch (const std::runtime_error &) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(audit_http_client_accepts_status_line_without_reason_phrase) {
    auto m = startMock("HTTP/1.1 200\r\nContent-Length: 2\r\n\r\nhi");
    auto r = clientGet(m.port, "/");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, std::string("hi"));
    EXPECT_EQ(r.reason, std::string(""));
}

TEST(audit_http_client_rejects_non_http_status_line_and_junk_header) {
    {
        auto m = startMock("ICY 200 OK\r\nContent-Length: 0\r\n\r\n");
        bool threw = false;
        try { (void) clientGet(m.port, "/"); } catch (const std::runtime_error &) { threw = true; }
        EXPECT_TRUE(threw);
    }
    {   // A header line with no colon used to be skipped silently.
        auto m = startMock("HTTP/1.1 200 OK\r\njunk-without-colon\r\nContent-Length: 0\r\n\r\n");
        bool threw = false;
        try { (void) clientGet(m.port, "/"); } catch (const std::runtime_error &) { threw = true; }
        EXPECT_TRUE(threw);
    }
}

TEST(audit_http_client_returns_101_without_reading_on) {
    // The server keeps the connection open after the 101: the client must not wait for more.
    auto m = startMock("HTTP/1.1 101 Switching Protocols\r\nUpgrade: x\r\nConnection: Upgrade\r\n\r\nraw-bytes", true);
    auto r = clientGet(m.port, "/");
    EXPECT_EQ(r.status, 101);
    EXPECT_EQ(r.body, std::string(""));
    m.release->store(true);
}

TEST(audit_http_client_response_header_timeout_fires) {
    auto m = startMock("", true); // accepts, reads the request, never answers
    const auto oldHeader = Soccer::HttpClient::responseHeaderTimeout();
    const auto oldBody = Soccer::HttpClient::responseBodyTimeout();
    Soccer::HttpClient::setResponseTimeouts(300ms, 5s);
    const auto start = std::chrono::steady_clock::now();
    bool threw = false;
    try {
        (void) clientGet(m.port, "/");
    } catch (const Soccer::SocketException &e) {
        threw = std::string(e.what()).find("timed out") != std::string::npos;
    }
    Soccer::HttpClient::setResponseTimeouts(oldHeader, oldBody);
    m.release->store(true);
    EXPECT_TRUE(threw);
    EXPECT_TRUE(std::chrono::steady_clock::now() - start < 5s);
    EXPECT_TRUE(Soccer::HttpClient::kDefaultResponseHeaderTimeout.count() > 0);
    EXPECT_TRUE(Soccer::HttpClient::kDefaultResponseBodyTimeout.count() > 0);
}

TEST(audit_http_client_response_body_timeout_fires) {
    // Headers arrive, the promised body never does.
    auto m = startMock("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort", true);
    const auto oldHeader = Soccer::HttpClient::responseHeaderTimeout();
    const auto oldBody = Soccer::HttpClient::responseBodyTimeout();
    Soccer::HttpClient::setResponseTimeouts(5s, 300ms);
    bool threw = false;
    try {
        (void) clientGet(m.port, "/");
    } catch (const std::exception &) {
        threw = true;
    }
    Soccer::HttpClient::setResponseTimeouts(oldHeader, oldBody);
    m.release->store(true);
    EXPECT_TRUE(threw);
}

TEST(audit_http_client_refuses_injection_in_target_host_and_header_names) {
    auto expectRefused = [](auto &&fn) {
        bool threw = false;
        try { fn(); } catch (const std::runtime_error &) { threw = true; }
        return threw;
    };
    EXPECT_TRUE(expectRefused([] { (void) clientGet(1, "/a\r\nX: y"); }));
    EXPECT_TRUE(expectRefused([] { (void) clientGet(1, "/a b"); }));
    EXPECT_TRUE(expectRefused([] { (void) clientGet(1, "/", {{"Bad:Name", "v"}}); }));
    EXPECT_TRUE(expectRefused([] { (void) clientGet(1, "/", {{"X", "a\r\nInjected: 1"}}); }));
    EXPECT_TRUE(expectRefused([] { (void) clientGet(1, "/", {{"Transfer-Encoding", "chunked"}}); }));
    EXPECT_TRUE(expectRefused([] {
        (void) YarnBall::syncWait(Soccer::HttpClient::get("bad host", 1, "/"));
    }));
}

TEST(audit_http_client_request_head_has_no_duplicate_framing_headers) {
    auto m = startMock("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    auto r = YarnBall::syncWait(YarnBall::withTimeout(
        Soccer::HttpClient::post("127.0.0.1", m.port, "/p", "", {{"Content-Length", "999"}, {"Host", "custom.example"}}), 10s));
    EXPECT_EQ(r.status, 200);
    std::string head;
    {
        std::lock_guard<std::mutex> lk(*m.mu);
        head = *m.requestHead;
    }
    auto count = [&](const std::string &needle) {
        std::size_t n = 0, pos = 0;
        while ((pos = head.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
        return n;
    };
    EXPECT_EQ(count("Content-Length:"), static_cast<std::size_t>(1));
    EXPECT_TRUE(head.find("Content-Length: 0\r\n") != std::string::npos);
    EXPECT_EQ(count("Host:"), static_cast<std::size_t>(1));
    EXPECT_TRUE(head.find("Host: custom.example\r\n") != std::string::npos);
}

TEST(audit_http_connection_pool_drops_empty_host_entries) {
    Soccer::HttpConnectionPool pool;
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    auto stop = std::make_shared<std::atomic<bool>>(false);
    auto holder = [](Soccer::TcpListener l, std::shared_ptr<std::atomic<bool>> stop) -> YarnBall::Task<void> {
        auto c = co_await l.accept();
        while (!stop->load()) co_await YarnBall::sleepFor(10ms);
        co_return;
    };
    YarnBall::coSpawn(holder(std::move(listener), stop));
    auto s = YarnBall::syncWait(Soccer::tcpConnect("127.0.0.1", port));
    pool.release("127.0.0.1", port, std::move(s));
    EXPECT_EQ(pool.idleCount("127.0.0.1", port), static_cast<std::size_t>(1));
    auto taken = pool.acquire("127.0.0.1", port);
    EXPECT_TRUE(taken.has_value());
    EXPECT_EQ(pool.idleCount("127.0.0.1", port), static_cast<std::size_t>(0));
    EXPECT_FALSE(pool.acquire("127.0.0.1", port).has_value());
    stop->store(true);
}


// =========================================================================
// WebSocket
// =========================================================================

namespace {

    std::string wsRequest(const std::string &extraHeaders = "", const std::string &key = "dGhlIHNhbXBsZSBub25jZQ==",
                          const std::string &path = "/chat") {
        return "GET " + path + " HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n" + extraHeaders + "\r\n";
    }

    struct WsFixture {
        std::uint16_t port{0};
        std::stop_source stop;
        std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();
        std::shared_ptr<Soccer::WsHandshakeInfo> seen = std::make_shared<Soccer::WsHandshakeInfo>();
        std::shared_ptr<std::atomic<bool>> connected = std::make_shared<std::atomic<bool>>(false);

        WsFixture(Soccer::WsOptions opts, Soccer::WsHandler handler = {}) {
            auto l = Soccer::TcpListener::bind("127.0.0.1", 0);
            port = l.localAddress().port();
            if (!handler) {
                auto mu_ = mu;
                auto seen_ = seen;
                auto connected_ = connected;
                handler = [mu_, seen_, connected_](Soccer::WsConnection ws) -> YarnBall::Task<void> {
                    {
                        std::lock_guard<std::mutex> lk(*mu_);
                        *seen_ = ws.handshake();
                    }
                    connected_->store(true);
                    try {
                        while (true) {
                            auto m = co_await ws.receive();
                            (void) m;
                        }
                    } catch (...) {
                    }
                    co_return;
                };
            }
            YarnBall::coSpawn(Soccer::wsServe(std::move(l), std::move(handler), std::move(opts), stop.get_token()));
        }

        ~WsFixture() {
            stop.request_stop();
            std::this_thread::sleep_for(30ms);
        }
    };

    /// Client frame (masked, FIN set unless @p fin is false).
    std::string clientFrame(std::uint8_t opcode, const std::string &payload, bool fin = true) {
        std::string f;
        f.push_back(static_cast<char>((fin ? 0x80 : 0x00) | opcode));
        f.push_back(static_cast<char>(0x80 | payload.size())); // payload < 126 in these tests
        const char mask[4] = {0x11, 0x22, 0x33, 0x44};
        f.append(mask, 4);
        for (std::size_t i = 0; i < payload.size(); ++i) f.push_back(static_cast<char>(payload[i] ^ mask[i & 3]));
        return f;
    }

    /// Everything the server sends until it closes the socket or @p quiet passes with nothing new.
    YarnBall::Task<std::string> readUntilQuiet(Soccer::TcpStream &s, std::chrono::milliseconds quiet) {
        std::string out;
        std::array<std::byte, 2048> buf{};
        while (true) {
            std::size_t n = 0;
            try {
                n = co_await YarnBall::withTimeout(s.read(buf), quiet);
            } catch (const YarnBall::TimeoutException &) {
                break;
            } catch (const Soccer::SocketException &) {
                break;
            }
            if (n == 0) break;
            out.append(reinterpret_cast<const char *>(buf.data()), n);
        }
        co_return out;
    }

    /// Open a raw connection, do the upgrade, return the stream and the 101 response head.
    struct RawWs {
        Soccer::TcpStream stream;
        std::string head;
    };

    YarnBall::Task<RawWs> rawWsConnect(std::uint16_t port, std::string request) {
        auto s = co_await Soccer::tcpConnect("127.0.0.1", port);
        co_await s.write(std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(request.data()), request.size()));
        std::string head;
        std::array<std::byte, 1> one{};
        while (head.size() < 4 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0) {
            const std::size_t n = co_await s.read(one);
            if (n == 0) break;
            head.push_back(static_cast<char>(one[0]));
        }
        co_return RawWs{std::move(s), std::move(head)};
    }

    /// The response head to @p request (the connection is dropped right after, whether or not it upgraded).
    std::string wsHead(std::uint16_t port, std::string request) {
        return YarnBall::syncWait(YarnBall::withTimeout(rawWsConnect(port, std::move(request)), 8s)).head;
    }

    /// Decode the unmasked server frames in @p raw into (opcode, payload) pairs.
    std::vector<std::pair<int, std::string>> parseServerFrames(const std::string &raw) {
        std::vector<std::pair<int, std::string>> out;
        std::size_t i = 0;
        while (i + 2 <= raw.size()) {
            const int op = static_cast<unsigned char>(raw[i]) & 0x0F;
            std::size_t len = static_cast<unsigned char>(raw[i + 1]) & 0x7F;
            i += 2;
            if (len == 126) {
                if (i + 2 > raw.size()) break;
                len = (static_cast<unsigned char>(raw[i]) << 8) | static_cast<unsigned char>(raw[i + 1]);
                i += 2;
            }
            if (i + len > raw.size()) break;
            out.emplace_back(op, raw.substr(i, len));
            i += len;
        }
        return out;
    }

    bool utf8Valid(const std::string &s) {
        std::size_t i = 0;
        while (i < s.size()) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            if (len == 0 || i + len > s.size()) return false;
            for (std::size_t k = 1; k < len; ++k) {
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
            }
            i += len;
        }
        return true;
    }

} // namespace

TEST(audit_ws_client_refuses_injection_in_path_and_host) {
    // Validated before any connection is attempted: port 1 would refuse otherwise (SocketException).
    auto expectWsError = [](std::string host, std::string path) {
        try {
            (void) YarnBall::syncWait(Soccer::WsConnection::connect(std::move(host), 1, std::move(path)));
        } catch (const Soccer::WsException &) {
            return true;
        } catch (...) {
            return false;
        }
        return false;
    };
    EXPECT_TRUE(expectWsError("127.0.0.1", "/x HTTP/1.1\r\nX-Injected: 1\r\n\r\nGET /y"));
    EXPECT_TRUE(expectWsError("127.0.0.1", "/a\r\nB: c"));
    EXPECT_TRUE(expectWsError("127.0.0.1", "/a b"));
    EXPECT_TRUE(expectWsError("127.0.0.1\r\nX: y", "/"));
    EXPECT_TRUE(expectWsError("", "/"));
}

TEST(audit_ws_server_rejects_duplicate_security_headers_and_bad_key) {
    Soccer::WsOptions o;
    WsFixture f(o);
    EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest("Origin: https://a.example\r\nOrigin: https://b.example\r\n")), "HTTP/1.1 400"));
    EXPECT_TRUE(startsWith(auditExchange(f.port,
        "GET /c HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"), "HTTP/1.1 400"));
    // Upgrade and Connection are token lists: a repeated or split field is joined, not refused.
    EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest("Upgrade: websocket\r\n")).substr(0, 12), "HTTP/1.1 101"));
    EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest("Connection: keep-alive\r\n")).substr(0, 12), "HTTP/1.1 101"));
    EXPECT_TRUE(startsWith(auditExchange(f.port,
        "GET /c HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Version: 13\r\n\r\n"),
        "HTTP/1.1 400"));
    // Right length and trailer, but not base64.
    EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest("", "!!!!!!!!!!!!!!!!!!!!!!==")), "HTTP/1.1 400"));
    // Malformed head: bare LF, obs-fold.
    EXPECT_TRUE(startsWith(auditExchange(f.port,
        "GET /c HTTP/1.1\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"), "HTTP/1.1 400"));
    EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest("X-A: 1\r\n folded\r\n")), "HTTP/1.1 400"));
    // The well-formed request still upgrades.
    EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest()).substr(0, 12), "HTTP/1.1 101"));
}

TEST(audit_ws_handshake_error_messages_do_not_carry_raw_peer_text) {
    auto listener = Soccer::TcpListener::bind("127.0.0.1", 0);
    const std::uint16_t port = listener.localAddress().port();
    auto err = std::make_shared<std::string>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto run = [](Soccer::TcpListener l, std::shared_ptr<std::string> err,
                  std::shared_ptr<std::atomic<bool>> done) -> YarnBall::Task<void> {
        try {
            auto c = co_await l.accept();
            auto ws = co_await Soccer::WsConnection::serverHandshake(std::move(c));
        } catch (const std::exception &e) {
            *err = e.what();
        }
        done->store(true);
        co_return;
    };
    YarnBall::coSpawn(run(std::move(listener), err, done));
    auditExchange(port,
        "GET /c HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: \xE9\xE9-evil\r\n\r\n");
    EXPECT_TRUE(auditWaitUntil([&] { return done->load(); }, 3s));
    EXPECT_TRUE(err->find("unsupported") != std::string::npos);
    for (char c : *err) EXPECT_TRUE(static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7f);
}

TEST(audit_ws_handshake_hook_sees_request_and_can_refuse) {
    Soccer::WsOptions o;
    o.onHandshake = [](const Soccer::WsHandshakeInfo &info) {
        if (info.header("Authorization") != "Bearer good") return Soccer::WsHandshakeDecision::reject(401);
        return Soccer::WsHandshakeDecision::accept();
    };
    WsFixture f(o);
    EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest()), "HTTP/1.1 401"));
    EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest("Authorization: Bearer bad\r\n")), "HTTP/1.1 401"));

    auto raw = YarnBall::syncWait(YarnBall::withTimeout(
        rawWsConnect(f.port, wsRequest("Authorization: Bearer good\r\nCookie: sid=42\r\n", "dGhlIHNhbXBsZSBub25jZQ==", "/room/7?x=1")), 8s));
    EXPECT_TRUE(startsWith(raw.head, "HTTP/1.1 101"));
    EXPECT_TRUE(auditWaitUntil([&] { return f.connected->load(); }, 3s));
    std::lock_guard<std::mutex> lk(*f.mu);
    EXPECT_EQ(f.seen->path, std::string("/room/7?x=1"));
    EXPECT_EQ(f.seen->header("cookie"), std::string("sid=42"));
}

TEST(audit_ws_handshake_hook_exception_and_bad_subprotocol_refuse) {
    {
        Soccer::WsOptions o;
        o.onHandshake = [](const Soccer::WsHandshakeInfo &) -> Soccer::WsHandshakeDecision {
            throw std::runtime_error("hook failed");
        };
        WsFixture f(o);
        EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest()), "HTTP/1.1 500"));
    }
    {
        Soccer::WsOptions o;
        o.onHandshake = [](const Soccer::WsHandshakeInfo &) {
            return Soccer::WsHandshakeDecision::accept("not-offered");
        };
        WsFixture f(o);
        EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest("Sec-WebSocket-Protocol: chat\r\n")), "HTTP/1.1 500"));
    }
}

TEST(audit_ws_subprotocol_is_negotiated_end_to_end) {
    Soccer::WsOptions o;
    o.onHandshake = [](const Soccer::WsHandshakeInfo &info) {
        for (const auto &p : info.offeredSubprotocols) {
            if (p == "v2.chat") return Soccer::WsHandshakeDecision::accept(p);
        }
        return Soccer::WsHandshakeDecision::accept();
    };
    WsFixture f(o);
    Soccer::WsOptions c;
    c.subprotocols = {"v1.chat", "v2.chat"};
    auto ws = YarnBall::syncWait(YarnBall::withTimeout(
        Soccer::WsConnection::connect("127.0.0.1", f.port, "/chat", c), 8s));
    EXPECT_EQ(ws.handshake().subprotocol, std::string("v2.chat"));
    EXPECT_TRUE(auditWaitUntil([&] { return f.connected->load(); }, 3s));
    std::lock_guard<std::mutex> lk(*f.mu);
    EXPECT_EQ(f.seen->subprotocol, std::string("v2.chat"));
}

TEST(audit_ws_strict_origin_refuses_browser_origins_without_an_allow_list) {
    Soccer::WsOptions lax;
    {
        WsFixture f(lax);
        EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest("Origin: https://evil.example\r\n")).substr(0, 12), "HTTP/1.1 101"));
    }
    Soccer::WsOptions strict;
    strict.strictOrigin = true;
    WsFixture f(strict);
    EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest("Origin: https://evil.example\r\n")), "HTTP/1.1 403"));
    // A client without an Origin header (not a browser) still gets in.
    EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest()).substr(0, 12), "HTTP/1.1 101"));
}

TEST(audit_ws_client_rejects_unrequested_extension_or_subprotocol) {
    auto fakeServer = [](std::string extraHeaders) {
        auto l = Soccer::TcpListener::bind("127.0.0.1", 0);
        const std::uint16_t port = l.localAddress().port();
        auto run = [](Soccer::TcpListener l, std::string extra) -> YarnBall::Task<void> {
            try {
                auto c = co_await l.accept();
                std::string got;
                std::array<std::byte, 1024> buf{};
                while (got.find("\r\n\r\n") == std::string::npos) {
                    const std::size_t n = co_await c.read(buf);
                    if (n == 0) co_return;
                    got.append(reinterpret_cast<const char *>(buf.data()), n);
                }
                const auto k = got.find("Sec-WebSocket-Key: ");
                const std::string key = got.substr(k + 19, 24);
                // Accept value for this key: computed by a real server handshake, so reuse one.
                auto accept = co_await [](std::string key) -> YarnBall::Task<std::string> {
                    auto l2 = Soccer::TcpListener::bind("127.0.0.1", 0);
                    const auto p2 = l2.localAddress().port();
                    auto srv = [](Soccer::TcpListener l) -> YarnBall::Task<void> {
                        try {
                            auto c2 = co_await l.accept();
                            auto ws = co_await Soccer::WsConnection::serverHandshake(std::move(c2));
                        } catch (...) {}
                        co_return;
                    };
                    YarnBall::coSpawn(srv(std::move(l2)));
                    auto s = co_await Soccer::tcpConnect("127.0.0.1", p2);
                    const std::string req = "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                            "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
                    co_await s.write(std::span<const std::byte>(
                        reinterpret_cast<const std::byte *>(req.data()), req.size()));
                    std::string resp;
                    std::array<std::byte, 512> b2{};
                    while (resp.find("\r\n\r\n") == std::string::npos) {
                        const std::size_t n = co_await s.read(b2);
                        if (n == 0) break;
                        resp.append(reinterpret_cast<const char *>(b2.data()), n);
                    }
                    const auto a = resp.find("Sec-WebSocket-Accept: ");
                    co_return resp.substr(a + 22, 28);
                }(key);
                const std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                          "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n" + extra + "\r\n";
                co_await c.write(std::span<const std::byte>(
                    reinterpret_cast<const std::byte *>(reply.data()), reply.size()));
                co_await YarnBall::sleepFor(300ms);
            } catch (...) {}
            co_return;
        };
        YarnBall::coSpawn(run(std::move(l), std::move(extraHeaders)));
        return port;
    };
    auto attempt = [](std::uint16_t port, Soccer::WsOptions o = {}) {
        try {
            auto ws = YarnBall::syncWait(YarnBall::withTimeout(
                Soccer::WsConnection::connect("127.0.0.1", port, "/", std::move(o)), 8s));
            return std::string("connected");
        } catch (const Soccer::WsException &e) {
            return std::string(e.what());
        }
    };
    EXPECT_TRUE(attempt(fakeServer("Sec-WebSocket-Extensions: permessage-deflate\r\n")).find("extension") != std::string::npos);
    EXPECT_TRUE(attempt(fakeServer("Sec-WebSocket-Protocol: chat\r\n")).find("subprotocol") != std::string::npos);
    // Control: no extras is fine.
    EXPECT_EQ(attempt(fakeServer("")), std::string("connected"));
    // A requested subprotocol is accepted.
    Soccer::WsOptions want;
    want.subprotocols = {"chat"};
    EXPECT_EQ(attempt(fakeServer("Sec-WebSocket-Protocol: chat\r\n"), want), std::string("connected"));
}

TEST(audit_ws_close_reason_is_cut_on_a_code_point_boundary) {
    Soccer::WsOptions o;
    auto handler = [](Soccer::WsConnection ws) -> YarnBall::Task<void> {
        std::string reason;
        for (int i = 0; i < 100; ++i) reason += "\xC3\xA9"; // 200 bytes of 2-byte characters
        co_await ws.close(1000, reason);
        co_return;
    };
    WsFixture f(o, handler);
    auto raw = YarnBall::syncWait(YarnBall::withTimeout(rawWsConnect(f.port, wsRequest()), 8s));
    EXPECT_TRUE(startsWith(raw.head, "HTTP/1.1 101"));
    auto frames = parseServerFrames(YarnBall::syncWait(YarnBall::withTimeout(readUntilQuiet(raw.stream, 500ms), 8s)));
    EXPECT_EQ(frames.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(frames[0].first, 8);
    EXPECT_TRUE(frames[0].second.size() <= 125);
    EXPECT_TRUE(utf8Valid(frames[0].second.substr(2)));
    EXPECT_EQ(frames[0].second.size(), static_cast<std::size_t>(2 + 122));
}

TEST(audit_ws_ping_flood_is_not_answered_in_full) {
    Soccer::WsOptions o;
    o.maxPingsPerSecond = 5;
    WsFixture f(o);
    auto raw = YarnBall::syncWait(YarnBall::withTimeout(rawWsConnect(f.port, wsRequest()), 8s));
    EXPECT_TRUE(startsWith(raw.head, "HTTP/1.1 101"));
    std::string flood;
    for (int i = 0; i < 40; ++i) flood += clientFrame(0x9, "p");
    YarnBall::syncWait(raw.stream.write(std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(flood.data()), flood.size())));
    auto frames = parseServerFrames(YarnBall::syncWait(YarnBall::withTimeout(readUntilQuiet(raw.stream, 600ms), 8s)));
    std::size_t pongs = 0;
    for (const auto &fr : frames) pongs += fr.first == 0xA;
    EXPECT_TRUE(pongs >= 1);
    EXPECT_TRUE(pongs < 40);
}

TEST(audit_ws_run_of_empty_fragments_fails_the_connection) {
    Soccer::WsOptions o;
    WsFixture f(o);
    auto raw = YarnBall::syncWait(YarnBall::withTimeout(rawWsConnect(f.port, wsRequest()), 8s));
    EXPECT_TRUE(startsWith(raw.head, "HTTP/1.1 101"));
    std::string burst = clientFrame(0x1, "a", false);
    for (int i = 0; i < 100; ++i) burst += clientFrame(0x0, "", false);
    YarnBall::syncWait(raw.stream.write(std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(burst.data()), burst.size())));
    auto frames = parseServerFrames(YarnBall::syncWait(YarnBall::withTimeout(readUntilQuiet(raw.stream, 600ms), 8s)));
    bool closed1008 = false;
    for (const auto &fr : frames) {
        if (fr.first == 8 && fr.second.size() >= 2 &&
            ((static_cast<unsigned char>(fr.second[0]) << 8) | static_cast<unsigned char>(fr.second[1])) == 1008) {
            closed1008 = true;
        }
    }
    EXPECT_TRUE(closed1008);
}

TEST(audit_ws_defaults_are_finite) {
    Soccer::WsOptions o;
    EXPECT_TRUE(o.maxConnections > 0);
    EXPECT_TRUE(o.idleTimeout.count() > 0);
    EXPECT_FALSE(o.strictOrigin); // documented: default stays backwards compatible
}


// =========================================================================
// Second-pass fixes: HttpServer, HttpClient, WebSocket targets
// =========================================================================

namespace {

    /// A running HttpServer with the routes the second-pass tests poke at.
    struct H1Fixture2 {
        std::unique_ptr<Soccer::HttpServer> server;
        std::stop_source stop;
        std::uint16_t port{0};

        H1Fixture2() {
            server = std::make_unique<Soccer::HttpServer>("127.0.0.1", 0);
            port = server->localAddress().port();
            server->route("GET", "/q", [](Soccer::HttpRequest req) -> YarnBall::Task<Soccer::HttpResponse> {
                Soccer::HttpResponse r;
                r.body = req.path + "|" + req.query;
                co_return r;
            });
            server->route("GET", "/n204", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
                Soccer::HttpResponse r;
                r.status = 204;
                r.body = "must-not-be-sent";
                co_return r;
            });
            server->route("GET", "/n304", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
                Soccer::HttpResponse r;
                r.status = 304;
                r.body = "must-not-be-sent";
                co_return r;
            });
            server->route("HEAD", "/q", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
                Soccer::HttpResponse r;
                r.body = "twelve bytes";
                co_return r;
            });
            server->route("GET", "/te", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
                Soccer::HttpResponse r;
                r.headers.push_back({"Transfer-Encoding", "chunked"});
                r.headers.push_back({"Content-Length", "999"});
                r.headers.push_back({"Connection", "keep-alive"});
                r.headers.push_back({"X-Keep", "yes"});
                r.body = "abc";
                co_return r;
            });
            server->route("POST", "/p", [](Soccer::HttpRequest req) -> YarnBall::Task<Soccer::HttpResponse> {
                Soccer::HttpResponse r;
                r.body = req.body;
                co_return r;
            });
            YarnBall::coSpawn(server->serve(stop.get_token()));
        }

        ~H1Fixture2() {
            stop.request_stop();
            std::this_thread::sleep_for(30ms);
        }
    };

    bool contains(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }

} // namespace

TEST(audit2_http_server_head_204_304_carry_no_body) {
    H1Fixture2 f;
    const auto head = auditExchange(f.port, "HEAD /q HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(startsWith(head, "HTTP/1.1 200 OK"));
    EXPECT_TRUE(contains(head, "Content-Length: 12\r\n"));
    EXPECT_TRUE(head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0);

    const auto n204 = auditExchange(f.port, "GET /n204 HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(startsWith(n204, "HTTP/1.1 204 No Content"));
    EXPECT_FALSE(contains(n204, "Content-Length"));
    EXPECT_FALSE(contains(n204, "must-not-be-sent"));

    const auto n304 = auditExchange(f.port, "GET /n304 HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(startsWith(n304, "HTTP/1.1 304 Not Modified"));
    EXPECT_FALSE(contains(n304, "must-not-be-sent"));
}

TEST(audit2_http_server_uses_standard_reason_phrases) {
    H1Fixture2 f;
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 404 Not Found"));
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /q HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 200 OK"));
}

TEST(audit2_http_server_routes_on_the_path_and_exposes_the_query) {
    H1Fixture2 f;
    const auto r = auditExchange(f.port, "GET /q?a=1&b=2 HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(startsWith(r, "HTTP/1.1 200 OK"));
    EXPECT_TRUE(contains(r, "/q|a=1&b=2"));
    EXPECT_TRUE(contains(auditExchange(f.port, "GET /q? HTTP/1.1\r\nHost: x\r\n\r\n"), "/q|"));
}

TEST(audit2_http_server_methods_are_case_sensitive) {
    H1Fixture2 f;
    EXPECT_TRUE(startsWith(auditExchange(f.port, "get /q HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 404"));
    EXPECT_TRUE(startsWith(auditExchange(f.port, "GET /q HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 200"));
}

TEST(audit2_http_server_expect_handling) {
    H1Fixture2 f;
    // An unknown expectation is refused.
    EXPECT_TRUE(startsWith(auditExchange(f.port,
        "POST /p HTTP/1.1\r\nHost: x\r\nExpect: something-else\r\nContent-Length: 2\r\n\r\nhi"), "HTTP/1.1 417"));
    // 100-continue is answered for HTTP/1.1 ...
    const auto h11 = auditExchange(f.port,
        "POST /p HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\nhi");
    EXPECT_TRUE(startsWith(h11, "HTTP/1.1 100 Continue"));
    EXPECT_TRUE(contains(h11, "HTTP/1.1 200 OK"));
    // ... and never sent to an HTTP/1.0 client.
    const auto h10 = auditExchange(f.port,
        "POST /p HTTP/1.0\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\nhi");
    EXPECT_FALSE(contains(h10, "100 Continue"));
    EXPECT_TRUE(startsWith(h10, "HTTP/1.1 200 OK"));
}

TEST(audit2_http_server_drops_handler_framing_headers) {
    H1Fixture2 f;
    const auto r = auditExchange(f.port, "GET /te HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(startsWith(r, "HTTP/1.1 200 OK"));
    EXPECT_FALSE(contains(r, "Transfer-Encoding"));
    EXPECT_FALSE(contains(r, "keep-alive"));
    EXPECT_FALSE(contains(r, "999"));
    EXPECT_TRUE(contains(r, "Content-Length: 3\r\n"));
    EXPECT_TRUE(contains(r, "X-Keep: yes\r\n"));
    EXPECT_TRUE(contains(r, "Connection: close\r\n"));
}

// ---- HttpClient ---------------------------------------------------------

TEST(audit2_http_client_decodes_chunked_bodies) {
    auto m = startMock("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                       "5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: 1\r\n\r\n");
    auto r = clientGet(m.port, "/");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, std::string("hello world"));
}

TEST(audit2_http_client_rejects_malformed_or_hostile_chunking) {
    const char *bad[] = {
        // not hex
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhi\r\n0\r\n\r\n",
        // 17 hex digits: would overflow a 64-bit size
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10000000000000000\r\n",
        // beyond the body cap
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1000001\r\n",
        // chunk data not followed by CRLF
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhiXX0\r\n\r\n",
        // chunked next to Content-Length
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\n2\r\nhi\r\n0\r\n\r\n",
        // a coding other than plain chunked
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n",
        // size line with a stray CR in the extension
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1;a\rb\r\nx\r\n0\r\n\r\n",
    };
    for (const char *reply : bad) {
        auto m = startMock(reply);
        bool threw = false;
        try { (void) clientGet(m.port, "/"); } catch (const std::runtime_error &) { threw = true; }
        EXPECT_TRUE(threw);
    }
}

TEST(audit2_http_client_chunk_trailers_are_bounded) {
    std::string reply = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n";
    for (int i = 0; i < 3000; ++i) reply += "X-T" + std::to_string(i) + ": " + std::string(40, 'v') + "\r\n";
    reply += "\r\n";
    auto m = startMock(reply);
    bool threw = false;
    try { (void) clientGet(m.port, "/"); } catch (const std::runtime_error &) { threw = true; }
    EXPECT_TRUE(threw);
}

TEST(audit2_http_client_head_has_no_body_and_empty_path_becomes_root) {
    auto m = startMock("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n", true);
    auto r = YarnBall::syncWait(YarnBall::withTimeout(Soccer::HttpClient::head("127.0.0.1", m.port, ""), 10s));
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.body.empty());
    EXPECT_EQ(r.header("Content-Length"), std::string("100"));
    {
        std::lock_guard<std::mutex> lk(*m.mu);
        EXPECT_TRUE(startsWith(*m.requestHead, "HEAD / HTTP/1.1\r\n"));
    }
    m.release->store(true);
}

TEST(audit2_http_client_pooling_follows_the_connection_header_and_leftovers) {
    auto &pool = Soccer::HttpConnectionPool::defaultPool();
    auto pooledGet = [&](const std::string &reply) {
        auto m = startMock(reply, true);
        auto r = YarnBall::syncWait(YarnBall::withTimeout(
            Soccer::HttpClient::getPooled("127.0.0.1", m.port, "/"), 10s));
        const std::size_t idle = pool.idleCount("127.0.0.1", m.port);
        m.release->store(true);
        return std::make_pair(r.status, idle);
    };
    // Reusable: a plain keep-alive response.
    EXPECT_EQ(pooledGet("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi").second, static_cast<std::size_t>(1));
    // close among other tokens, in any case, ends pooling.
    EXPECT_EQ(pooledGet("HTTP/1.1 200 OK\r\nConnection: keep-alive, Close\r\nContent-Length: 2\r\n\r\nhi").second,
              static_cast<std::size_t>(0));
    // HTTP/1.0 closes unless it says keep-alive.
    EXPECT_EQ(pooledGet("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nhi").second, static_cast<std::size_t>(0));
    EXPECT_EQ(pooledGet("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 2\r\n\r\nhi").second,
              static_cast<std::size_t>(1));
    // Bytes past the response (here a second response) were already swallowed by the reader:
    // the connection is out of step and must not be pooled.
    EXPECT_EQ(pooledGet("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhiHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n").second,
              static_cast<std::size_t>(0));
    // A chunked response pools like any other once fully read.
    EXPECT_EQ(pooledGet("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\n\r\n").second,
              static_cast<std::size_t>(1));
}

// ---- WebSocket upgrade target and Host ---------------------------------------

TEST(audit2_ws_handshake_target_is_normalised_and_host_validated) {
    Soccer::WsOptions o;
    o.onHandshake = [](const Soccer::WsHandshakeInfo &info) {
        if (info.path == "/deny") return Soccer::WsHandshakeDecision::reject(403);
        return Soccer::WsHandshakeDecision::accept();
    };
    WsFixture f(o);
    // Absolute-form must not slip past a hook that blocks the path.
    EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest("", "dGhlIHNhbXBsZSBub25jZQ==", "http://evil/deny")), "HTTP/1.1 403"));
    // Targets that are not origin-form (or carry a fragment) are refused outright.
    for (const char *target : {"foo", "*", "/a#b", "http://", "http:///x", "ftp://h/x"}) {
        EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest("", "dGhlIHNhbXBsZSBub25jZQ==", target)), "HTTP/1.1 400"));
    }
    // Host: exactly one.
    EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest("Host: y\r\n")), "HTTP/1.1 400"));
    EXPECT_TRUE(startsWith(auditExchange(f.port,
        "GET /c HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"), "HTTP/1.1 400"));
    // An absolute-form target is reduced to its path for the handler.
    auto raw = YarnBall::syncWait(YarnBall::withTimeout(
        rawWsConnect(f.port, wsRequest("", "dGhlIHNhbXBsZSBub25jZQ==", "http://evil.example:8080/room?x=1")), 8s));
    EXPECT_TRUE(startsWith(raw.head, "HTTP/1.1 101"));
    EXPECT_TRUE(auditWaitUntil([&] { return f.connected->load(); }, 3s));
    std::lock_guard<std::mutex> lk(*f.mu);
    EXPECT_EQ(f.seen->path, std::string("/room?x=1"));
}

TEST(audit2_ws_connection_header_split_over_fields_is_accepted) {
    Soccer::WsOptions o;
    WsFixture f(o);
    EXPECT_TRUE(startsWith(wsHead(f.port, "GET /x HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
        "Connection: keep-alive\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n").substr(0, 12),
        "HTTP/1.1 101"));
    // Origin and key stay single-valued.
    EXPECT_TRUE(startsWith(wsHead(f.port, wsRequest("Origin: https://a.example\r\nOrigin: https://b.example\r\n")), "HTTP/1.1 400"));
}


// =========================================================================
// HTTP/2
// =========================================================================

#ifdef SOCCER_HAS_HTTP2

namespace {

    struct H2Fixture {
        std::unique_ptr<Soccer::Http2Server> server;
        std::stop_source stop;
        std::uint16_t port{0};

        explicit H2Fixture(const std::function<void(Soccer::Http2Server &)> &setup,
                           Soccer::Http2ServerOptions o = {}) {
            server = std::make_unique<Soccer::Http2Server>("127.0.0.1", 0, o);
            port = server->localAddress().port();
            setup(*server);
            YarnBall::coSpawn(server->serve(stop.get_token()));
        }

        ~H2Fixture() {
            stop.request_stop();
            std::this_thread::sleep_for(30ms);
        }
    };

    YarnBall::Task<Soccer::Http2Response> h2One(std::uint16_t port, std::string method, std::string path,
                                                 std::vector<Soccer::HttpHeader> headers = {},
                                                 std::string body = {},
                                                 std::chrono::milliseconds timeout = Soccer::Http2Connection::kDefaultRequestTimeout) {
        auto conn = co_await Soccer::Http2Connection::connectPlain("127.0.0.1", port);
        co_return co_await conn.request(std::move(method), std::move(path), std::move(headers), std::move(body), timeout);
    }

    Soccer::Http2Response h2Sync(std::uint16_t port, std::string method, std::string path,
                                 std::vector<Soccer::HttpHeader> headers = {}, std::string body = {}) {
        return YarnBall::syncWait(YarnBall::withTimeout(
            h2One(port, std::move(method), std::move(path), std::move(headers), std::move(body)), 10s));
    }

} // namespace

TEST(audit_h2_server_joins_cookie_crumbs) {
    H2Fixture f([](Soccer::Http2Server &s) {
        s.route("GET", "/c", [](Soccer::HttpRequest req) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.body = req.header("cookie");
            co_return r;
        });
    });
    auto resp = h2Sync(f.port, "GET", "/c", {{"cookie", "a=1"}, {"cookie", "b=2"}, {"cookie", "c=3"}});
    EXPECT_EQ(resp.status, 200);
    EXPECT_EQ(resp.body, std::string("a=1; b=2; c=3"));
}

TEST(audit_h2_server_exposes_authority_and_scheme) {
    H2Fixture f([](Soccer::Http2Server &s) {
        s.route("GET", "/a", [](Soccer::HttpRequest req) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.body = req.authority + "|" + req.scheme + "|" + req.header("host");
            co_return r;
        });
    });
    auto resp = h2Sync(f.port, "GET", "/a");
    const std::string authority = "127.0.0.1:" + std::to_string(f.port);
    EXPECT_EQ(resp.body, authority + "|http|" + authority);
}

TEST(audit_h2_server_handler_exception_does_not_leak_detail) {
    H2Fixture f([](Soccer::Http2Server &s) {
        s.route("GET", "/boom", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            throw std::runtime_error("secret-internal-detail");
            co_return Soccer::HttpResponse{};
        });
    });
    auto resp = h2Sync(f.port, "GET", "/boom");
    EXPECT_EQ(resp.status, 500);
    EXPECT_TRUE(resp.body.find("secret-internal-detail") == std::string::npos);
    EXPECT_FALSE(resp.body.empty());
}

TEST(audit_h2_server_trailers_only_response_keeps_trailers) {
    H2Fixture f([](Soccer::Http2Server &s) {
        s.route("GET", "/t", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.trailers.push_back({"grpc-status", "7"});
            co_return r;
        });
    });
    auto resp = h2Sync(f.port, "GET", "/t");
    EXPECT_EQ(resp.status, 200);
    EXPECT_TRUE(resp.body.empty());
    EXPECT_EQ(resp.trailers.size(), static_cast<std::size_t>(1));
    if (!resp.trailers.empty()) {
        EXPECT_EQ(resp.trailers[0].name, std::string("grpc-status"));
        EXPECT_EQ(resp.trailers[0].value, std::string("7"));
    }
}

TEST(audit_h2_server_route_snapshot_and_single_serve) {
    Soccer::Http2Server server("127.0.0.1", 0);
    const std::uint16_t port = server.localAddress().port();
    server.route("GET", "/one", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
        Soccer::HttpResponse r;
        r.body = "one";
        co_return r;
    });
    std::stop_source src;
    YarnBall::coSpawn(server.serve(src.get_token()));
    // Registered after serve(): the running server works from its snapshot and never sees it.
    server.route("GET", "/late", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
        Soccer::HttpResponse r;
        r.body = "late";
        co_return r;
    });
    EXPECT_EQ(h2Sync(port, "GET", "/one").body, std::string("one"));
    EXPECT_EQ(h2Sync(port, "GET", "/late").status, 404);
    bool threw = false;
    try {
        (void) server.serve(src.get_token());
    } catch (const Soccer::SocketException &) {
        threw = true;
    }
    EXPECT_TRUE(threw);
    src.request_stop();
    std::this_thread::sleep_for(30ms);
    EXPECT_TRUE(Soccer::Http2ServerOptions{}.maxConnections > 0);
    EXPECT_TRUE(Soccer::Http2ServerOptions{}.idleTimeout.count() > 0);
}

TEST(audit_h2_client_request_times_out_and_leaves_the_connection_usable) {
    H2Fixture f([](Soccer::Http2Server &s) {
        s.route("GET", "/slow", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            co_await YarnBall::sleepFor(2s);
            Soccer::HttpResponse r;
            r.body = "late";
            co_return r;
        });
        s.route("GET", "/fast", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.body = "fast";
            co_return r;
        });
    });
    auto run = [](std::uint16_t port) -> YarnBall::Task<std::string> {
        auto conn = co_await Soccer::Http2Connection::connectPlain("127.0.0.1", port);
        std::string result;
        try {
            (void) co_await conn.request("GET", "/slow", {}, {}, 300ms);
            result = "no-timeout";
        } catch (const Soccer::SocketException &e) {
            result = std::string(e.what()).find("timed out") != std::string::npos ? "timed-out" : "other";
        }
        // The connection survives: another stream on it still works.
        auto ok = co_await conn.request("GET", "/fast");
        co_return result + "|" + ok.body;
    };
    EXPECT_EQ(YarnBall::syncWait(YarnBall::withTimeout(run(f.port), 10s)), std::string("timed-out|fast"));
    EXPECT_TRUE(Soccer::Http2Connection::kDefaultRequestTimeout.count() > 0);
}

TEST(audit_h2_client_close_with_queued_and_inflight_requests_is_safe) {
    // The server admits one stream at a time and is slow, so the client's further requests are
    // parked inside nghttp2 (their header blocks and body providers queued). Closing the
    // connection then fails them all; nothing may touch freed request memory afterwards.
    Soccer::Http2ServerOptions so;
    so.maxConcurrentStreams = 1;
    H2Fixture f([](Soccer::Http2Server &s) {
        s.route("POST", "/slow", [](Soccer::HttpRequest req) -> YarnBall::Task<Soccer::HttpResponse> {
            co_await YarnBall::sleepFor(500ms);
            Soccer::HttpResponse r;
            r.body = "echo:" + req.body;
            co_return r;
        });
    }, so);
    auto done = std::make_shared<std::atomic<int>>(0);
    auto run = [](std::uint16_t port, std::shared_ptr<std::atomic<int>> done) -> YarnBall::Task<void> {
        auto conn = std::make_shared<Soccer::Http2Connection>(
            co_await Soccer::Http2Connection::connectPlain("127.0.0.1", port));
        auto one = [](std::shared_ptr<Soccer::Http2Connection> c, int i,
                      std::shared_ptr<std::atomic<int>> done) -> YarnBall::Task<void> {
            try {
                // Named, not a braced temporary inside the co_await: GCC 14 ICEs on that.
                std::vector<Soccer::HttpHeader> pad{{"X-Pad", std::string(200, 'p')}};
                (void) co_await c->request("POST", "/slow/" + std::to_string(i) + std::string(40, 'x'),
                                           std::move(pad), std::string(4096, 'b'));
            } catch (...) {
            }
            done->fetch_add(1);
            co_return;
        };
        for (int i = 0; i < 4; ++i) YarnBall::coSpawn(one(conn, i, done));
        co_await YarnBall::sleepFor(150ms);
        conn->close();
        co_await YarnBall::sleepFor(300ms);
        co_return;
    };
    YarnBall::syncWait(YarnBall::withTimeout(run(f.port, done), 10s));
    EXPECT_TRUE(auditWaitUntil([&] { return done->load() == 4; }, 5s));
}

TEST(audit_h2_connection_pool_stays_bounded) {
    EXPECT_TRUE(Soccer::Http2ConnectionPool::kMaxEntries > 0);
}

// ---- second-pass HTTP/2 fixes -----------------------------------------------

namespace {

    /// Connect to an h2c server by hand, send the preface and an empty SETTINGS, then send
    /// @p pings PING frames @p interval apart (all at once when interval is zero). Returns
    /// how long the server took to close the connection, or -1 if it was still open after @p limit.
    YarnBall::Task<long long> h2PingUntilClosed(std::uint16_t port, int pings,
                                                 std::chrono::milliseconds interval,
                                                 std::chrono::milliseconds limit) {
        auto s = co_await Soccer::tcpConnect("127.0.0.1", port);
        const auto start = std::chrono::steady_clock::now();
        std::string out = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        out.append("\x00\x00\x00\x04\x00\x00\x00\x00\x00", 9);
        std::string ping("\x00\x00\x08\x06\x00\x00\x00\x00\x00", 9);
        ping.append(8, 'p');
        auto sendAll = [&](const std::string &b) -> YarnBall::Task<bool> {
            try {
                co_await s.write(std::span<const std::byte>(
                    reinterpret_cast<const std::byte *>(b.data()), b.size()));
            } catch (const Soccer::SocketException &) {
                co_return false;
            }
            co_return true;
        };
        bool open = co_await sendAll(out);
        std::array<std::byte, 4096> buf{};
        int sent = 0;
        std::string burst;
        if (interval.count() == 0) {
            for (int i = 0; i < pings; ++i) burst += ping;
            open = open && co_await sendAll(burst);
            sent = pings;
        }
        while (open) {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= limit) co_return -1;
            if (sent < pings) {
                open = co_await sendAll(ping);
                ++sent;
            }
            if (interval.count()) co_await YarnBall::sleepFor(interval);
            // Take what the server has sent (ACKs, a possible GOAWAY) and notice a close.
            while (open) {
                try {
                    const std::size_t n = co_await YarnBall::withTimeout(s.read(buf), interval.count() ? 5ms : 100ms);
                    if (n == 0) open = false;
                } catch (const YarnBall::TimeoutException &) {
                    break;
                } catch (const Soccer::SocketException &) {
                    open = false;
                }
            }
        }
        co_return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
    }

} // namespace

TEST(audit2_h2_server_idle_timeout_is_not_reset_by_pings) {
    Soccer::Http2ServerOptions so;
    so.idleTimeout = 600ms;
    H2Fixture f([](Soccer::Http2Server &) {}, so);
    // A PING every 100 ms (well under the control-frame cap) used to hold the connection forever.
    const long long closedAfter = YarnBall::syncWait(YarnBall::withTimeout(
        h2PingUntilClosed(f.port, 1000, 100ms, 6s), 12s));
    EXPECT_TRUE(closedAfter > 0 && closedAfter < 3000);
}

TEST(audit2_h2_server_closes_a_control_frame_flood) {
    H2Fixture f([](Soccer::Http2Server &) {});
    const long long closedAfter = YarnBall::syncWait(YarnBall::withTimeout(
        h2PingUntilClosed(f.port, 400, 0ms, 4s), 12s));
    EXPECT_TRUE(closedAfter >= 0 && closedAfter < 3000);
}

TEST(audit2_h2_server_head_204_304_carry_no_data) {
    H2Fixture f([](Soccer::Http2Server &s) {
        s.route("HEAD", "/h", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.body = "hello";
            co_return r;
        });
        s.route("GET", "/n204", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.status = 204;
            r.body = "nope";
            co_return r;
        });
        s.route("GET", "/n304", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.status = 304;
            r.body = "nope";
            co_return r;
        });
    });
    // A conformant client (nghttp2) resets the stream if DATA follows a HEAD response.
    auto head = h2Sync(f.port, "HEAD", "/h");
    EXPECT_EQ(head.status, 200);
    EXPECT_TRUE(head.body.empty());
    std::string len;
    for (const auto &h : head.headers) if (h.name == "content-length") len = h.value;
    EXPECT_EQ(len, std::string("5"));
    auto n204 = h2Sync(f.port, "GET", "/n204");
    EXPECT_EQ(n204.status, 204);
    EXPECT_TRUE(n204.body.empty());
    auto n304 = h2Sync(f.port, "GET", "/n304");
    EXPECT_EQ(n304.status, 304);
    EXPECT_TRUE(n304.body.empty());
}

TEST(audit2_h2_server_routes_on_the_path_and_exposes_the_query) {
    H2Fixture f([](Soccer::Http2Server &s) {
        s.route("GET", "/q", [](Soccer::HttpRequest req) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.body = req.path + "|" + req.query;
            co_return r;
        });
    });
    EXPECT_EQ(h2Sync(f.port, "GET", "/q?a=1&b=2").body, std::string("/q|a=1&b=2"));
    EXPECT_EQ(h2Sync(f.port, "GET", "/q").body, std::string("/q|"));
    // Methods are case-sensitive.
    EXPECT_EQ(h2Sync(f.port, "get", "/q").status, 404);
}

TEST(audit2_h2_server_validates_and_filters_response_headers) {
    H2Fixture f([](Soccer::Http2Server &s) {
        s.route("GET", "/dup", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.headers.push_back({"Content-Length", "999"});
            r.headers.push_back({"Connection", "close"});
            r.headers.push_back({"Transfer-Encoding", "chunked"});
            r.headers.push_back({"X-Keep", "yes"});
            r.body = "abc";
            co_return r;
        });
        s.route("GET", "/badvalue", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.headers.push_back({"X-Test", "a\r\nb"});
            co_return r;
        });
        s.route("GET", "/badname", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.headers.push_back({"bad name", "v"});
            co_return r;
        });
        s.route("GET", "/pseudo", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.trailers.push_back({":status", "200"});
            co_return r;
        });
        s.route("GET", "/emptytrailer", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.body = "x";
            r.trailers.push_back({"", "v"});
            co_return r;
        });
    });
    auto dup = h2Sync(f.port, "GET", "/dup");
    EXPECT_EQ(dup.status, 200);
    EXPECT_EQ(dup.body, std::string("abc"));
    int lengths = 0, keeps = 0, framing = 0;
    for (const auto &h : dup.headers) {
        if (h.name == "content-length") { ++lengths; EXPECT_EQ(h.value, std::string("3")); }
        if (h.name == "x-keep") ++keeps;
        if (h.name == "connection" || h.name == "transfer-encoding") ++framing;
    }
    EXPECT_EQ(lengths, 1);
    EXPECT_EQ(keeps, 1);
    EXPECT_EQ(framing, 0);
    for (const char *path : {"/badvalue", "/badname", "/pseudo", "/emptytrailer"}) {
        auto r = h2Sync(f.port, "GET", path);
        EXPECT_EQ(r.status, 500);
    }
}

#endif // SOCCER_HAS_HTTP2
