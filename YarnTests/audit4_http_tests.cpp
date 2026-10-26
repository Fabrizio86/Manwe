//
// Regression tests for the fourth audit pass (http).
//

#include "test_framework.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
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

#ifndef _WIN32
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

    template<typename Pred>
    bool waitFor(Pred &&p, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!p()) {
            if (std::chrono::steady_clock::now() >= deadline) return p();
            std::this_thread::sleep_for(2ms);
        }
        return true;
    }

    bool startsWith(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

    YarnBall::Task<void> trackServe(YarnBall::Task<void> t, std::shared_ptr<std::atomic<bool>> done) {
        try {
            co_await std::move(t);
        } catch (...) {
        }
        done->store(true);
        co_return;
    }

    std::shared_ptr<std::atomic<bool>> spawnTracked(YarnBall::Task<void> t) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        YarnBall::coSpawn(trackServe(std::move(t), done));
        return done;
    }

    void stopAndWait(std::stop_source &src, const std::shared_ptr<std::atomic<bool>> &done) {
        src.request_stop();
        waitFor([&] { return done->load(); }, 3s);
    }

    /// Accepts one connection, reads the request head, answers with @p reply and closes.
    YarnBall::Task<void> replyOnce(Soccer::TcpListener l, std::string reply) {
        try {
            auto c = co_await l.accept();
            std::string got;
            std::array<std::byte, 1024> buf{};
            while (got.find("\r\n\r\n") == std::string::npos) {
                const std::size_t n = co_await c.read(buf);
                if (n == 0) co_return;
                got.append(reinterpret_cast<const char *>(buf.data()), n);
            }
            co_await c.write(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(reply.data()), reply.size()));
        } catch (...) {
        }
        co_return;
    }

} // namespace


// =========================================================================
// HTTP client: strict line endings
// =========================================================================

TEST(audit4_http_client_refuses_bare_lf_in_the_response_head) {
    auto serveReply = [](const std::string &reply) {
        auto l = Soccer::TcpListener::bind("127.0.0.1", 0);
        const std::uint16_t port = l.localAddress().port();
        YarnBall::coSpawn(replyOnce(std::move(l), reply));
        std::string failure;
        try {
            auto r = YarnBall::syncWait(YarnBall::withTimeout(
                Soccer::HttpClient::get("127.0.0.1", port, "/"), 8s));
            if (r.status != 200) failure = "unexpected status";
        } catch (const std::runtime_error &e) {
            failure = e.what();
        }
        return failure;
    };
    EXPECT_TRUE(serveReply("HTTP/1.1 200 OK\nContent-Length: 0\r\n\r\n").find("CRLF") != std::string::npos);
    EXPECT_TRUE(serveReply("HTTP/1.1 200 OK\r\nContent-Length: 0\nConnection: close\r\n\r\n").find("CRLF") !=
                std::string::npos);
    EXPECT_TRUE(serveReply("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\n").find("CRLF") != std::string::npos);
    EXPECT_EQ(serveReply("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"), std::string());
}


TEST(audit5_http_client_refuses_chunked_with_an_empty_content_length) {
    auto serveReply = [](const std::string &reply) {
        auto l = Soccer::TcpListener::bind("127.0.0.1", 0);
        const std::uint16_t port = l.localAddress().port();
        YarnBall::coSpawn(replyOnce(std::move(l), reply));
        try {
            auto r = YarnBall::syncWait(YarnBall::withTimeout(
                Soccer::HttpClient::get("127.0.0.1", port, "/"), 8s));
            return std::string(r.status == 200 ? "accepted" : "unexpected status");
        } catch (const std::runtime_error &e) {
            return std::string(e.what());
        }
    };
    EXPECT_TRUE(serveReply("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length:\r\n"
                           "Connection: close\r\n\r\n0\r\n\r\n")
                        .find("Transfer-Encoding") != std::string::npos);
}


// =========================================================================
// WebSocket: Origin must match Host by default
// =========================================================================

namespace {

    struct WsRig {
        std::uint16_t port{0};
        std::stop_source stop;
        std::shared_ptr<std::atomic<bool>> served;

        explicit WsRig(Soccer::WsOptions opts) {
            auto l = Soccer::TcpListener::bind("127.0.0.1", 0);
            port = l.localAddress().port();
            auto handler = [](Soccer::WsConnection ws) -> YarnBall::Task<void> {
                try {
                    while (true) (void) co_await ws.receive();
                } catch (...) {
                }
                co_return;
            };
            served = spawnTracked(Soccer::wsServe(std::move(l), handler, std::move(opts), stop.get_token()));
        }

        ~WsRig() { stopAndWait(stop, served); }
    };

    std::string wsUpgrade(const std::string &host, const std::string &extra) {
        return "GET /chat HTTP/1.1\r\nHost: " + host + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" + extra + "\r\n";
    }

    YarnBall::Task<std::string> wsStatusLine(std::uint16_t port, std::string request) {
        auto s = co_await Soccer::tcpConnect("127.0.0.1", port);
        co_await s.write(std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(request.data()), request.size()));
        std::string head;
        std::array<std::byte, 1> one{};
        try {
            while (head.size() < 4 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0) {
                if (co_await s.read(one) == 0) break;
                head.push_back(static_cast<char>(one[0]));
            }
        } catch (const Soccer::SocketException &) {
        }
        co_return head.substr(0, 12);
    }

    std::string wsStatus(std::uint16_t port, const std::string &host, const std::string &extra) {
        return YarnBall::syncWait(YarnBall::withTimeout(wsStatusLine(port, wsUpgrade(host, extra)), 8s));
    }

}

TEST(audit4_ws_default_origin_policy_requires_the_origin_to_match_the_host) {
    WsRig rig{Soccer::WsOptions{}};
    // Another site's page: refused.
    EXPECT_TRUE(startsWith(wsStatus(rig.port, "app.example", "Origin: https://evil.example\r\n"), "HTTP/1.1 403"));
    EXPECT_TRUE(startsWith(wsStatus(rig.port, "app.example", "Origin: null\r\n"), "HTTP/1.1 403"));
    // Same host, different port or a look-alike userinfo: refused.
    EXPECT_TRUE(startsWith(wsStatus(rig.port, "app.example:8080", "Origin: https://app.example:9090\r\n"),
                           "HTTP/1.1 403"));
    EXPECT_TRUE(startsWith(wsStatus(rig.port, "app.example", "Origin: https://app.example@evil.example\r\n"),
                           "HTTP/1.1 403"));
    // The server's own pages, and clients that send no Origin, get in.
    EXPECT_TRUE(startsWith(wsStatus(rig.port, "app.example", "Origin: https://app.example\r\n"), "HTTP/1.1 101"));
    EXPECT_TRUE(startsWith(wsStatus(rig.port, "App.Example:8080", "Origin: http://app.example:8080\r\n"),
                           "HTTP/1.1 101"));
    EXPECT_TRUE(startsWith(wsStatus(rig.port, "[::1]:8080", "Origin: http://[::1]:8080\r\n"), "HTTP/1.1 101"));
    EXPECT_TRUE(startsWith(wsStatus(rig.port, "app.example", ""), "HTTP/1.1 101"));
}

TEST(audit4_ws_same_origin_rule_can_be_switched_off_and_does_not_override_a_policy) {
    Soccer::WsOptions open;
    open.requireSameOrigin = false;
    {
        WsRig rig(open);
        EXPECT_TRUE(startsWith(wsStatus(rig.port, "app.example", "Origin: https://evil.example\r\n"),
                               "HTTP/1.1 101"));
    }
    Soccer::WsOptions listed;
    listed.allowedOrigins = {"https://partner.example"};
    WsRig rig(listed);
    EXPECT_TRUE(startsWith(wsStatus(rig.port, "app.example", "Origin: https://partner.example\r\n"),
                           "HTTP/1.1 101"));
    EXPECT_TRUE(startsWith(wsStatus(rig.port, "app.example", "Origin: https://app.example\r\n"), "HTTP/1.1 403"));
}


#if defined(SOCCER_HAS_HTTP2) && !defined(_WIN32)

// =========================================================================
// HTTP/2 server: raw-socket peers
// =========================================================================

namespace {

    const std::string kPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

    std::string h2Frame(int type, int flags, int sid, const std::string &p) {
        std::string f;
        f.push_back(static_cast<char>(p.size() >> 16));
        f.push_back(static_cast<char>(p.size() >> 8));
        f.push_back(static_cast<char>(p.size()));
        f.push_back(static_cast<char>(type));
        f.push_back(static_cast<char>(flags));
        for (int i = 3; i >= 0; --i) f.push_back(static_cast<char>((sid >> (i * 8)) & 0xff));
        return f + p;
    }

    /// HPACK block for GET @p path on http://x (static-table indices, literals without indexing).
    std::string h2GetHeaders(const std::string &path) {
        std::string hb;
        hb.push_back(static_cast<char>(0x82)); // :method GET
        hb.push_back(static_cast<char>(0x86)); // :scheme http
        hb.push_back(0x04);                    // :path, literal
        hb.push_back(static_cast<char>(path.size()));
        hb += path;
        hb.push_back(0x01); // :authority, literal
        hb.push_back(1);
        hb += "x";
        return hb;
    }

    std::string be32(std::uint32_t v) {
        std::string p(4, '\0');
        for (int i = 0; i < 4; ++i) p[static_cast<std::size_t>(i)] = static_cast<char>((v >> ((3 - i) * 8)) & 0xff);
        return p;
    }

    std::string windowUpdate(int sid, std::uint32_t inc) { return h2Frame(8, 0, sid, be32(inc)); }

    std::string rstStream(int sid, std::uint32_t code) { return h2Frame(3, 0, sid, be32(code)); }

    /// SETTINGS with SETTINGS_INITIAL_WINDOW_SIZE = @p window.
    std::string initialWindowSettings(std::uint32_t window) {
        std::string p;
        p.push_back(0);
        p.push_back(4);
        p += be32(window);
        return h2Frame(4, 0, 0, p);
    }

    /// A plain blocking TCP peer driven from the test thread.
    struct RawPeer {
        int fd{-1};

        explicit RawPeer(std::uint16_t port) {
            fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            a.sin_addr.s_addr = htonl(0x7f000001);
            if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
                ::close(fd);
                fd = -1;
                throw std::runtime_error("RawPeer: connect failed");
            }
        }
        RawPeer(const RawPeer &) = delete;
        RawPeer &operator=(const RawPeer &) = delete;
        ~RawPeer() { closeNow(); }

        void closeNow() {
            if (fd >= 0) ::close(fd);
            fd = -1;
        }

        bool send(const std::string &s) const {
            std::size_t off = 0;
            while (off < s.size()) {
                const ::ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
                if (n <= 0) return false;
                off += static_cast<std::size_t>(n);
            }
            return true;
        }

        /// Append what arrives within @p ms to @p out. Returns -1 once the peer has closed
        /// (EOF or error), else the number of bytes added.
        int recvFor(std::string &out, int ms) const {
            int total = 0;
            while (true) {
                pollfd p{fd, POLLIN, 0};
                if (::poll(&p, 1, total == 0 ? ms : 0) <= 0) return total;
                char buf[65536];
                const ::ssize_t n = ::recv(fd, buf, sizeof buf, 0);
                if (n <= 0) return -1;
                out.append(buf, static_cast<std::size_t>(n));
                total += static_cast<int>(n);
            }
        }
    };

    std::size_t openFds() {
        std::size_t n = 0;
        if (DIR *d = ::opendir("/dev/fd")) {
            while (::readdir(d) != nullptr) ++n;
            ::closedir(d);
        }
        return n;
    }

    YarnBall::Task<bool> h2Probe(std::uint16_t port) {
        try {
            auto conn = co_await Soccer::Http2Connection::connectPlain("127.0.0.1", port);
            auto r = co_await conn.request("GET", "/small", {}, {}, 1s);
            co_return r.status == 200 && r.body == "ok";
        } catch (...) {
            co_return false;
        }
    }

    struct SlowState {
        std::atomic<int> started{0};
        std::atomic<bool> release{false};
    };

    struct H2Rig {
        std::unique_ptr<Soccer::Http2Server> server;
        std::stop_source stop;
        std::shared_ptr<std::atomic<bool>> served;
        std::shared_ptr<SlowState> slow = std::make_shared<SlowState>();
        std::uint16_t port{0};

        /// Routes: /slow blocks until released, /small answers "ok", /big answers @p bigBytes.
        H2Rig(Soccer::Http2ServerOptions o, std::size_t bigBytes = 0) {
            server = std::make_unique<Soccer::Http2Server>("127.0.0.1", 0, o);
            port = server->localAddress().port();
            auto st = slow;
            server->route("GET", "/slow", [st](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
                st->started.fetch_add(1);
                for (int i = 0; i < 1000 && !st->release.load(); ++i) co_await YarnBall::sleepFor(10ms);
                Soccer::HttpResponse r;
                r.body = "slow";
                co_return r;
            });
            server->route("GET", "/small", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
                Soccer::HttpResponse r;
                r.body = "ok";
                co_return r;
            });
            server->route("GET", "/big", [bigBytes](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
                Soccer::HttpResponse r;
                r.body = std::string(bigBytes, 'x');
                co_return r;
            });
            served = spawnTracked(server->serve(stop.get_token()));
        }
        ~H2Rig() {
            slow->release.store(true);
            stopAndWait(stop, served);
        }
    };

}

TEST(audit4_h2_server_counts_running_handlers_of_reset_streams_against_the_stream_cap) {
    Soccer::Http2ServerOptions o;
    o.maxConcurrentStreams = 2;
    H2Rig rig(o);

    RawPeer peer(rig.port);
    EXPECT_TRUE(peer.send(kPreface + h2Frame(4, 0, 0, "")));
    // Open a stream with a complete request and cancel it at once, six times over: the
    // handlers keep running after the cancel, so only the first two may start.
    for (int i = 0; i < 6; ++i) {
        const int sid = 1 + 2 * i;
        EXPECT_TRUE(peer.send(h2Frame(1, 0x5, sid, h2GetHeaders("/slow"))));
        waitFor([&] { return rig.slow->started.load() > i; }, 300ms);
        EXPECT_TRUE(peer.send(rstStream(sid, 8)));
        std::string in;
        (void) peer.recvFor(in, 20);
    }
    EXPECT_EQ(rig.slow->started.load(), 2);

    // Once the handlers end the capacity comes back.
    rig.slow->release.store(true);
    std::string in;
    (void) peer.recvFor(in, 300);
    EXPECT_TRUE(peer.send(h2Frame(1, 0x5, 13, h2GetHeaders("/slow"))));
    EXPECT_TRUE(waitFor([&] { return rig.slow->started.load() == 3; }, 3s));
}

TEST(audit4_h2_server_releases_the_descriptor_when_the_driver_ends_with_handlers_running) {
    Soccer::Http2ServerOptions o;
    o.maxConnections = 4;
    H2Rig rig(o);
    EXPECT_TRUE(YarnBall::syncWait(YarnBall::withTimeout(h2Probe(rig.port), 5s)));
    std::this_thread::sleep_for(200ms);
    const std::size_t baseline = openFds();

    {
        RawPeer peer(rig.port);
        EXPECT_TRUE(peer.send(kPreface + h2Frame(4, 0, 0, "") + h2Frame(1, 0x5, 1, h2GetHeaders("/slow"))));
        EXPECT_TRUE(waitFor([&] { return rig.slow->started.load() == 1; }, 3s));
        // The peer leaves while its handler is still running.
    }
    // The connection's descriptor goes with the driver, not with the handler.
    EXPECT_TRUE(waitFor([&] { return openFds() <= baseline; }, 3s));
    EXPECT_FALSE(rig.slow->release.load());
}

TEST(audit4_h2_server_does_not_count_one_byte_window_grants_as_download_progress) {
    Soccer::Http2ServerOptions o;
    o.idleTimeout = 600ms;
    o.requestTimeout = 600ms;
    H2Rig rig(o, 1u << 20);

    RawPeer peer(rig.port);
    EXPECT_TRUE(peer.send(kPreface + initialWindowSettings(0) + h2Frame(1, 0x5, 1, h2GetHeaders("/big"))));
    // The peer opens the stream window a byte at a time, far inside the idle timeout. That
    // is not a download that is moving.
    std::string in;
    bool closed = false;
    const auto start = std::chrono::steady_clock::now();
    while (!closed && std::chrono::steady_clock::now() - start < 6s) {
        (void) peer.send(windowUpdate(1, 1) + windowUpdate(0, 1));
        std::this_thread::sleep_for(50ms);
        closed = peer.recvFor(in, 0) < 0;
    }
    EXPECT_TRUE(closed);
    EXPECT_TRUE(std::chrono::steady_clock::now() - start < 5s);
}

#endif
