//
// Regression tests for the third audit pass (http).
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

    YarnBall::Task<std::string> rawExchange(std::uint16_t port, std::string request) {
        auto s = co_await Soccer::tcpConnect("127.0.0.1", port);
        co_await s.write(std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(request.data()), request.size()));
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

    std::string exchange(std::uint16_t port, std::string request) {
        return YarnBall::syncWait(YarnBall::withTimeout(rawExchange(port, std::move(request)), 8s));
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
// localAddress() survives serve()
// =========================================================================

TEST(audit3_http_server_local_address_is_valid_after_serve) {
    for (int i = 0; i < 20; ++i) {
        Soccer::HttpServer server("127.0.0.1", 0);
        const std::uint16_t port = server.localAddress().port();
        EXPECT_TRUE(port != 0);
        std::stop_source src;
        auto served = spawnTracked(server.serve(src.get_token()));
        // serve() has taken the listener: the address recorded at construction must remain.
        EXPECT_EQ(server.localAddress().port(), port);
        stopAndWait(src, served);
        EXPECT_EQ(server.localAddress().port(), port);
    }
}

TEST(audit3_http_trim_ascii_strips_only_space_and_tab) {
    EXPECT_EQ(Soccer::detail::trimAscii(" \t v \t "), std::string("v"));
    EXPECT_EQ(Soccer::detail::trimAscii("\x0b" "5" "\x0c"), std::string("\x0b" "5" "\x0c"));
    EXPECT_EQ(Soccer::detail::trimAscii(" \x0b" "5" "\x0c "), std::string("\x0b" "5" "\x0c"));
    EXPECT_EQ(Soccer::detail::trimAscii("\r\n x"), std::string("\r\n x"));
    EXPECT_EQ(Soccer::detail::trimAscii("   "), std::string());
}

TEST(audit3_http_server_rejects_vt_ff_around_header_values) {
    Soccer::HttpServer server("127.0.0.1", 0);
    auto counter = std::make_shared<std::atomic<int>>(0);
    server.route("POST", "/p", [counter](Soccer::HttpRequest req) -> YarnBall::Task<Soccer::HttpResponse> {
        counter->fetch_add(1);
        Soccer::HttpResponse r;
        r.body = std::to_string(req.body.size());
        co_return r;
    });
    const std::uint16_t port = server.localAddress().port();
    std::stop_source src;
    auto served = spawnTracked(server.serve(src.get_token()));

    // A strict proxy reads "\x0b5\x0c" as an invalid length; trimming it to 5 here would let the
    // two disagree about where the request ends.
    EXPECT_TRUE(startsWith(exchange(port,
        "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: \x0b" "5\x0c\r\n\r\nhello"), "HTTP/1.1 400"));
    EXPECT_TRUE(startsWith(exchange(port,
        "POST /p HTTP/1.1\r\nHost: x\r\nX-A: \x0b" "v\r\nContent-Length: 5\r\n\r\nhello"), "HTTP/1.1 400"));
    EXPECT_EQ(counter->load(), 0);
    // Space and tab are still optional whitespace.
    EXPECT_TRUE(startsWith(exchange(port,
        "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: \t 5 \t\r\n\r\nhello"), "HTTP/1.1 200"));
    EXPECT_EQ(counter->load(), 1);
    stopAndWait(src, served);
}

TEST(audit3_http_client_rejects_vt_ff_in_response_header_values) {
    auto serveReply = [](const std::string &reply) {
        auto l = Soccer::TcpListener::bind("127.0.0.1", 0);
        const std::uint16_t port = l.localAddress().port();
        YarnBall::coSpawn(replyOnce(std::move(l), reply));
        std::string failure;
        try {
            (void) YarnBall::syncWait(YarnBall::withTimeout(
                Soccer::HttpClient::get("127.0.0.1", port, "/"), 8s));
        } catch (const std::runtime_error &e) {
            failure = e.what();
        }
        return failure;
    };
    EXPECT_TRUE(serveReply("HTTP/1.1 200 OK\r\nContent-Length: \x0b" "0\x0c\r\n\r\n").find("control character") !=
                std::string::npos);
    EXPECT_EQ(serveReply("HTTP/1.1 200 OK\r\nContent-Length: \t0 \t\r\nConnection: close\r\n\r\n"), std::string());
}


// =========================================================================
// WebSocket
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

    const char *const kWsUpgrade =
        "GET /chat HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

    /// A masked client frame header (payload length up to 65535) followed by @p payload, which may be
    /// shorter than @p declaredLen to model a peer that announces a frame and then stalls.
    std::string maskedFrame(std::uint8_t opcode, bool fin, std::size_t declaredLen, const std::string &payload) {
        std::string f;
        f.push_back(static_cast<char>((fin ? 0x80 : 0x00) | opcode));
        if (declaredLen < 126) {
            f.push_back(static_cast<char>(0x80 | declaredLen));
        } else {
            f.push_back(static_cast<char>(0x80 | 126));
            f.push_back(static_cast<char>(declaredLen >> 8));
            f.push_back(static_cast<char>(declaredLen & 0xFF));
        }
        const char mask[4] = {0x11, 0x22, 0x33, 0x44};
        f.append(mask, 4);
        for (std::size_t i = 0; i < payload.size(); ++i) f.push_back(static_cast<char>(payload[i] ^ mask[i & 3]));
        return f;
    }

    /// Upgrade, send @p bytes, and return the status code of the first Close frame the server
    /// sends back (0 if none arrives within @p wait) and how long it took.
    YarnBall::Task<std::pair<unsigned, std::chrono::milliseconds>>
    wsFirstCloseCode(std::uint16_t port, std::string bytes, std::chrono::milliseconds wait) {
        auto s = co_await Soccer::tcpConnect("127.0.0.1", port);
        const std::string up = kWsUpgrade;
        co_await s.write(std::span<const std::byte>(reinterpret_cast<const std::byte *>(up.data()), up.size()));
        std::string head;
        std::array<std::byte, 1> one{};
        while (head.size() < 4 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0) {
            if (co_await s.read(one) == 0) co_return std::make_pair(0u, 0ms);
            head.push_back(static_cast<char>(one[0]));
        }
        co_await s.write(std::span<const std::byte>(reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()));
        const auto start = std::chrono::steady_clock::now();
        std::string in;
        std::array<std::byte, 512> buf{};
        unsigned code = 0;
        while (code == 0) {
            std::size_t n = 0;
            try {
                n = co_await YarnBall::withTimeout(s.read(buf), wait);
            } catch (const YarnBall::TimeoutException &) {
                break;
            } catch (const Soccer::SocketException &) {
                break;
            }
            if (n == 0) break;
            in.append(reinterpret_cast<const char *>(buf.data()), n);
            // Server frames are unmasked with short payloads here.
            std::size_t i = 0;
            while (i + 2 <= in.size()) {
                const std::size_t len = static_cast<unsigned char>(in[i + 1]) & 0x7F;
                if (i + 2 + len > in.size()) break;
                if ((static_cast<unsigned char>(in[i]) & 0x0F) == 8 && len >= 2) {
                    code = (static_cast<unsigned char>(in[i + 2]) << 8) | static_cast<unsigned char>(in[i + 3]);
                    break;
                }
                i += 2 + len;
            }
        }
        co_return std::make_pair(code, std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start));
    }

    unsigned closeCodeAnswering(std::uint16_t port, unsigned sentCode) {
        std::string payload;
        payload.push_back(static_cast<char>(sentCode >> 8));
        payload.push_back(static_cast<char>(sentCode & 0xFF));
        return YarnBall::syncWait(YarnBall::withTimeout(
            wsFirstCloseCode(port, maskedFrame(8, true, payload.size(), payload), 3s), 10s)).first;
    }

}

TEST(audit3_ws_accepts_registered_close_codes_1012_to_1014) {
    Soccer::WsOptions o;
    WsRig rig(o);
    // An acceptable code is echoed back; a protocol violation is answered with 1002.
    for (const unsigned ok : {1000u, 1001u, 1011u, 1012u, 1013u, 1014u, 3000u, 4999u}) {
        EXPECT_EQ(closeCodeAnswering(rig.port, ok), ok);
    }
    // Reserved or unassigned: 1004, 1005, 1006, 1015 may not appear on the wire; 1016..2999 are unassigned.
    for (const unsigned bad : {1004u, 1005u, 1006u, 1015u, 1016u, 2999u, 5000u}) {
        EXPECT_EQ(closeCodeAnswering(rig.port, bad), 1002u);
    }
}

TEST(audit3_ws_oversized_continuation_is_refused_before_its_payload_is_read) {
    Soccer::WsOptions o;
    o.maxMessageBytes = 1000;
    WsRig rig(o);
    // 600 bytes fit; the continuation announces 600 more but sends none of them. The message
    // would pass the cap, so it has to be refused from the frame header alone, not once the
    // whole payload (here: never) has been read into memory.
    const std::string first = maskedFrame(1, false, 600, std::string(600, 'a'));
    const std::string cont = maskedFrame(0, true, 600, "");
    const auto [code, took] = YarnBall::syncWait(YarnBall::withTimeout(
        wsFirstCloseCode(rig.port, first + cont, 3s), 10s));
    EXPECT_EQ(code, 1009u);
    EXPECT_TRUE(took < 2s);
}


#if defined(SOCCER_HAS_HTTP2) && !defined(_WIN32)

// =========================================================================
// HTTP/2: raw-socket peers
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

    std::string windowUpdate(int sid, std::uint32_t inc) {
        std::string p(4, '\0');
        for (int i = 0; i < 4; ++i) p[static_cast<std::size_t>(i)] = static_cast<char>((inc >> ((3 - i) * 8)) & 0xff);
        return h2Frame(8, 0, sid, p);
    }

    /// A plain blocking TCP peer driven from the test thread.
    struct RawPeer {
        int fd{-1};

        explicit RawPeer(std::uint16_t port, int rcvbuf = 0) {
            fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (rcvbuf != 0) ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
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
        ~RawPeer() {
            if (fd >= 0) ::close(fd);
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

    /// Counts the DATA payload bytes of one stream in a byte stream of HTTP/2 frames.
    struct FrameCounter {
        std::string buf;
        std::size_t dataBytes{0};
        bool ended{false};

        void feed(std::string &in) {
            buf += in;
            in.clear();
            while (buf.size() >= 9) {
                const std::size_t len = (static_cast<unsigned char>(buf[0]) << 16) |
                                        (static_cast<unsigned char>(buf[1]) << 8) | static_cast<unsigned char>(buf[2]);
                if (buf.size() < 9 + len) break;
                const int type = static_cast<unsigned char>(buf[3]);
                const int flags = static_cast<unsigned char>(buf[4]);
                if (type == 0) dataBytes += len;
                if ((type == 0 || type == 1) && (flags & 1)) ended = true;
                buf.erase(0, 9 + len);
            }
        }
    };

    YarnBall::Task<bool> h2Probe(std::uint16_t port) {
        try {
            auto conn = co_await Soccer::Http2Connection::connectPlain("127.0.0.1", port);
            auto r = co_await conn.request("GET", "/small", {}, {}, 1s);
            co_return r.status == 200 && r.body == "ok";
        } catch (...) {
            co_return false;
        }
    }

    struct H2Rig {
        std::unique_ptr<Soccer::Http2Server> server;
        std::stop_source stop;
        std::shared_ptr<std::atomic<bool>> served;
        std::uint16_t port{0};

        H2Rig(Soccer::Http2ServerOptions o, const std::function<void(Soccer::Http2Server &)> &setup) {
            server = std::make_unique<Soccer::Http2Server>("127.0.0.1", 0, o);
            port = server->localAddress().port();
            setup(*server);
            served = spawnTracked(server->serve(stop.get_token()));
        }
        ~H2Rig() { stopAndWait(stop, served); }
    };

    void routeBigAndSmall(Soccer::Http2Server &s, std::size_t bigBytes) {
        s.route("GET", "/big", [bigBytes](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.body = std::string(bigBytes, 'x');
            co_return r;
        });
        s.route("GET", "/small", [](Soccer::HttpRequest) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.body = "ok";
            co_return r;
        });
    }

}

TEST(audit3_h2_server_local_address_is_valid_right_after_serve) {
    // serve() hands the listener to a task that moves it on its first resume; a localAddress()
    // call racing with that used to throw (or race on the listener).
    for (int i = 0; i < 30; ++i) {
        Soccer::Http2Server server("127.0.0.1", 0);
        const std::uint16_t port = server.localAddress().port();
        std::stop_source src;
        auto served = spawnTracked(server.serve(src.get_token()));
        EXPECT_EQ(server.localAddress().port(), port);
        EXPECT_TRUE(port != 0);
        stopAndWait(src, served);
    }
}

TEST(audit3_h2_server_drops_a_peer_that_requests_a_big_response_and_never_reads) {
    Soccer::Http2ServerOptions o;
    o.writeTimeout = 500ms;
    o.maxConnections = 1;
    H2Rig rig(o, [](Soccer::Http2Server &s) { routeBigAndSmall(s, 64u << 20); });

    // A huge flow-control window, so only the socket buffers stand between the response and
    // the peer; then it never reads.
    RawPeer peer(rig.port, 4096);
    std::string settings;
    settings.push_back(0);
    settings.push_back(4);
    settings.append("\x7f\xff\xff\xff", 4);
    EXPECT_TRUE(peer.send(kPreface + h2Frame(4, 0, 0, settings) + windowUpdate(0, 0x7fff0000u) +
                          h2Frame(1, 0x5, 1, h2GetHeaders("/big"))));

    // With the only connection slot pinned, a second client is turned away until the server
    // gives the first one up.
    const auto start = std::chrono::steady_clock::now();
    bool served = false;
    while (!served && std::chrono::steady_clock::now() - start < 15s) {
        served = YarnBall::syncWait(YarnBall::withTimeout(h2Probe(rig.port), 5s));
        if (!served) std::this_thread::sleep_for(100ms);
    }
    EXPECT_TRUE(served);
    EXPECT_TRUE(std::chrono::steady_clock::now() - start < 12s);
}

TEST(audit3_h2_server_idle_timeout_does_not_cut_a_download_that_is_progressing) {
    Soccer::Http2ServerOptions o;
    o.idleTimeout = 600ms;
    o.requestTimeout = 600ms;
    constexpr std::size_t kBody = 1u << 20;
    H2Rig rig(o, [](Soccer::Http2Server &s) { routeBigAndSmall(s, kBody); });

    RawPeer peer(rig.port);
    EXPECT_TRUE(peer.send(kPreface + h2Frame(4, 0, 0, "") + h2Frame(1, 0x5, 1, h2GetHeaders("/big"))));
    FrameCounter fc;
    std::string in;
    bool closed = false;
    // The default window is 64 KiB: grant another 64 KiB every 100 ms. The whole download takes
    // far longer than the idle timeout, yet every step makes progress.
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 80 && !fc.ended && !closed; ++i) {
        (void) peer.send(windowUpdate(0, 65536) + windowUpdate(1, 65536));
        std::this_thread::sleep_for(100ms);
        closed = peer.recvFor(in, 0) < 0;
        fc.feed(in);
    }
    EXPECT_TRUE(fc.ended);
    EXPECT_EQ(fc.dataBytes, kBody);
    EXPECT_TRUE(std::chrono::steady_clock::now() - start > 600ms);
}

TEST(audit3_h2_server_idle_timeout_still_cuts_a_stalled_download) {
    Soccer::Http2ServerOptions o;
    o.idleTimeout = 600ms;
    o.requestTimeout = 600ms;
    H2Rig rig(o, [](Soccer::Http2Server &s) { routeBigAndSmall(s, 1u << 20); });

    RawPeer peer(rig.port);
    EXPECT_TRUE(peer.send(kPreface + h2Frame(4, 0, 0, "") + h2Frame(1, 0x5, 1, h2GetHeaders("/big"))));
    // The window is used up after 64 KiB and the peer never opens it again.
    FrameCounter fc;
    std::string in;
    bool closed = false;
    const auto start = std::chrono::steady_clock::now();
    while (!closed && std::chrono::steady_clock::now() - start < 6s) {
        closed = peer.recvFor(in, 100) < 0;
        fc.feed(in);
    }
    EXPECT_TRUE(closed);
    EXPECT_FALSE(fc.ended);
    EXPECT_TRUE(std::chrono::steady_clock::now() - start < 5s);
}


// =========================================================================
// HTTP/2 client against a scripted peer
// =========================================================================

namespace {

    /// A one-shot h2c peer on a plain blocking socket: accepts a connection, waits for the
    /// client's first request HEADERS, writes @p script, then holds the connection open until
    /// the client leaves (or 3 s pass).
    struct ScriptedH2Peer {
        std::uint16_t port{0};
        int listenFd{-1};
        std::thread th;

        explicit ScriptedH2Peer(std::string script) {
            listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
            int one = 1;
            ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(0x7f000001);
            if (::bind(listenFd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || ::listen(listenFd, 1) != 0) {
                ::close(listenFd);
                throw std::runtime_error("ScriptedH2Peer: cannot listen");
            }
            socklen_t len = sizeof a;
            ::getsockname(listenFd, reinterpret_cast<sockaddr *>(&a), &len);
            port = ntohs(a.sin_port);
            th = std::thread([this, script = std::move(script)] { run(script); });
        }
        ScriptedH2Peer(const ScriptedH2Peer &) = delete;
        ScriptedH2Peer &operator=(const ScriptedH2Peer &) = delete;
        ~ScriptedH2Peer() {
            if (th.joinable()) th.join();
            ::close(listenFd);
        }

    private:
        void run(const std::string &script) {
            pollfd lp{listenFd, POLLIN, 0};
            if (::poll(&lp, 1, 5000) <= 0) return;
            const int c = ::accept(listenFd, nullptr, nullptr);
            if (c < 0) return;
            std::string in;
            // Skip the 24-byte preface, then wait for a HEADERS frame.
            bool sawHeaders = false;
            const auto start = std::chrono::steady_clock::now();
            while (!sawHeaders && std::chrono::steady_clock::now() - start < 3s) {
                pollfd p{c, POLLIN, 0};
                if (::poll(&p, 1, 100) > 0) {
                    char buf[4096];
                    const ::ssize_t n = ::recv(c, buf, sizeof buf, 0);
                    if (n <= 0) break;
                    in.append(buf, static_cast<std::size_t>(n));
                }
                std::size_t i = 24;
                while (in.size() >= 24 && i + 9 <= in.size()) {
                    const std::size_t len = (static_cast<unsigned char>(in[i]) << 16) |
                                            (static_cast<unsigned char>(in[i + 1]) << 8) |
                                            static_cast<unsigned char>(in[i + 2]);
                    if (i + 9 + len > in.size()) break;
                    if (in[i + 3] == 1) sawHeaders = true;
                    i += 9 + len;
                }
            }
            if (sawHeaders) {
                const std::string out = h2Frame(4, 0, 0, "") + h2Frame(4, 1, 0, "") + script;
                (void) ::send(c, out.data(), out.size(), 0);
                // Hold the connection until the client has had its say.
                while (std::chrono::steady_clock::now() - start < 3s) {
                    pollfd p{c, POLLIN, 0};
                    if (::poll(&p, 1, 50) > 0) {
                        char buf[4096];
                        if (::recv(c, buf, sizeof buf, 0) <= 0) break;
                    }
                }
            }
            ::close(c);
        }
    };

    /// Issue one GET to the scripted peer; returns "status:body" or "threw:<what>".
    YarnBall::Task<std::string> h2ScriptedGet(std::uint16_t port) {
        try {
            auto conn = co_await Soccer::Http2Connection::connectPlain("127.0.0.1", port);
            auto r = co_await conn.request("GET", "/", {}, {}, 5s);
            co_return std::to_string(r.status) + ":" + r.body;
        } catch (const std::exception &e) {
            co_return std::string("threw:") + e.what();
        }
    }

    std::string scriptedGet(const std::string &script) {
        ScriptedH2Peer peer(script);
        return YarnBall::syncWait(YarnBall::withTimeout(h2ScriptedGet(peer.port), 10s));
    }

    const std::string kRstNoError = h2Frame(3, 0, 1, std::string(4, '\0'));
    const std::string kStatus200 = std::string(1, static_cast<char>(0x88)); // :status 200

}

TEST(audit3_h2_client_rst_stream_no_error_before_a_response_is_an_error) {
    const std::string r = scriptedGet(kRstNoError);
    EXPECT_TRUE(startsWith(r, "threw:"));
    EXPECT_TRUE(r.find("before a response was received") != std::string::npos);
}

TEST(audit3_h2_client_rst_stream_no_error_mid_body_is_an_error) {
    const std::string r = scriptedGet(h2Frame(1, 0x4, 1, kStatus200) + h2Frame(0, 0, 1, "abc") + kRstNoError);
    EXPECT_TRUE(startsWith(r, "threw:"));
    EXPECT_TRUE(r.find("before the response completed") != std::string::npos);
}

TEST(audit3_h2_client_complete_responses_still_succeed) {
    // Headers only, ended with END_STREAM.
    EXPECT_EQ(scriptedGet(h2Frame(1, 0x5, 1, kStatus200)), std::string("200:"));
    // Headers, then a body ended by END_STREAM on the last DATA frame.
    EXPECT_EQ(scriptedGet(h2Frame(1, 0x4, 1, kStatus200) + h2Frame(0, 0, 1, "ab") + h2Frame(0, 1, 1, "c")),
              std::string("200:abc"));
}

TEST(audit3_h2_client_vets_and_normalises_request_headers) {
    H2Rig rig({}, [](Soccer::Http2Server &s) {
        auto echo = [](Soccer::HttpRequest req) -> YarnBall::Task<Soccer::HttpResponse> {
            Soccer::HttpResponse r;
            r.body = "te=" + req.header("te") + "|conn=" + req.header("connection") + "|ka=" + req.header("keep-alive") +
                     "|up=" + req.header("upgrade") + "|cl=" + req.header("content-length") +
                     "|host=" + req.header("host") + "|a=" + req.header("x-a") + "|len=" + std::to_string(req.body.size());
            co_return r;
        };
        s.route("GET", "/x", echo);
        s.route("POST", "/x", echo);
    });
    auto one = [&](std::string method, std::string path, std::vector<Soccer::HttpHeader> hs, std::string body) {
        return YarnBall::syncWait(YarnBall::withTimeout(
            [](std::uint16_t port, std::string m, std::string p, std::vector<Soccer::HttpHeader> h,
               std::string b) -> YarnBall::Task<std::string> {
                try {
                    auto conn = co_await Soccer::Http2Connection::connectPlain("127.0.0.1", port);
                    auto r = co_await conn.request(std::move(m), std::move(p), std::move(h), std::move(b), 5s);
                    co_return std::to_string(r.status) + " " + r.body;
                } catch (const std::runtime_error &e) {
                    co_return std::string("threw:") + e.what();
                }
            }(rig.port, std::move(method), std::move(path), std::move(hs), std::move(body)),
            10s));
    };
    const std::string authority = "127.0.0.1:" + std::to_string(rig.port);

    // HTTP/1-style connection management is dropped (a peer resets the stream over it), the
    // caller's Content-Length is replaced by the real one, TE survives only as "trailers".
    EXPECT_EQ(one("POST", "/x",
                  {{"Connection", "keep-alive"}, {"Keep-Alive", "timeout=5"}, {"Upgrade", "h2c"},
                   {"TE", "gzip"}, {"Content-Length", "999"}, {"X-A", "ok"}},
                  "abc"),
              "200 te=|conn=|ka=|up=|cl=3|host=" + authority + "|a=ok|len=3");
    EXPECT_EQ(one("GET", "/x", {{"TE", "trailers"}, {"Connection", "TE"}}, ""),
              "200 te=trailers|conn=|ka=|up=|cl=|host=" + authority + "|a=|len=0");
    // A caller Host stands in for :authority, as it replaces the automatic one over HTTP/1.
    EXPECT_EQ(one("GET", "/x", {{"Host", "custom.example"}}, ""),
              "200 te=|conn=|ka=|up=|cl=|host=custom.example|a=|len=0");
    // An empty body on a method that carries one says so.
    EXPECT_EQ(one("POST", "/x", {}, ""), "200 te=|conn=|ka=|up=|cl=0|host=" + authority + "|a=|len=0");

    // What the HTTP/1 client refuses, this one refuses with the same kind of error.
    EXPECT_TRUE(startsWith(one("GET", "/x", {{"X-A", "a\r\nb: c"}}, ""), "threw:"));
    EXPECT_TRUE(startsWith(one("GET", "/x", {{"Bad Name", "v"}}, ""), "threw:"));
    EXPECT_TRUE(startsWith(one("GET", "/x", {{"Transfer-Encoding", "chunked"}}, ""), "threw:"));
    EXPECT_TRUE(startsWith(one("GET", "/x HTTP/1.1\r\nHost: evil", {}, ""), "threw:"));
    EXPECT_TRUE(startsWith(one("GE T", "/x", {}, ""), "threw:"));
}

#endif // SOCCER_HAS_HTTP2 && !_WIN32
