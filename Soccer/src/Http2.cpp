//
// Created by Fabrizio Paino on 2026-05-16.
//
// Http2Connection -- coroutine wrapper around nghttp2 (RFC 7540 +
// RFC 7541 HPACK). See docs/http2.md for the design rationale.
//
// Architecture:
//
//   nghttp2_session  <----+-----> session mutex (serialises all
//                         |       nghttp2_session_* calls)
//                         |
//   per-stream map   <----+       std::unordered_map<int32_t, Http2Stream>
//                                 (response state, awaiter handle,
//                                  body buffer for request data)
//
//   driver coroutine -----> reads from TcpStream/TlsStream, feeds
//                           bytes via nghttp2_session_mem_recv, drains
//                           the resulting output via mem_send.
//                           Callbacks resume per-stream waiters
//                           inside the recv call.
//
//   request() ------> takes session mutex, nghttp2_submit_request,
//                     releases mutex, kicks the driver to push the
//                     freshly enqueued frames, then suspends on the
//                     per-stream "done" signal.
//

#include "Http2.h"

#ifdef SOCCER_HAS_HTTP2

#include <nghttp2/nghttp2.h>

#include <cstring>
#include <utility>

#include "Coroutines.h"
#include "IoAwaiters.h"
#include "SocketException.h"
#include "TcpStream.h"

#ifdef SOCCER_HAS_TLS
#include "TlsStream.h"
#endif

namespace Soccer {

    // -----------------------------------------------------------------
    // Internal state
    // -----------------------------------------------------------------

    /**
     * @brief Type-erased async pipe. Both @c TcpStream and (when
     *        @c SOCCER_HAS_TLS is set) @c TlsStream get a concrete
     *        subclass below. We type-erase so the rest of the file
     *        does not need to template on stream type.
     */
    struct Http2Pipe {
        virtual ~Http2Pipe() = default;
        virtual YarnBall::Task<std::size_t> readBytes(std::span<std::byte> buf) = 0;
        virtual YarnBall::Task<void> writeAll(std::span<const std::byte> buf) = 0;
        virtual int fd() const noexcept = 0;
        virtual void close() noexcept = 0;
    };

    namespace {
        /**
         * @brief TcpStream-backed pipe.
         */
        struct TcpPipe final : Http2Pipe {
            TcpStream tcp;
            explicit TcpPipe(TcpStream s) noexcept : tcp(std::move(s)) {}

            YarnBall::Task<std::size_t> readBytes(std::span<std::byte> buf) override {
                co_return co_await this->tcp.read(buf);
            }
            YarnBall::Task<void> writeAll(std::span<const std::byte> buf) override {
                std::size_t off = 0;
                while (off < buf.size()) {
                    std::size_t n = co_await this->tcp.write(buf.subspan(off));
                    if (n == 0) break;
                    off += n;
                }
                co_return;
            }
            int fd() const noexcept override { return this->tcp.fd(); }
            void close() noexcept override { this->tcp.close(); }
        };

#ifdef SOCCER_HAS_TLS
        /**
         * @brief TlsStream-backed pipe. Same API; lets the rest of the
         *        file ignore which underlying transport is in use.
         */
        struct TlsPipe final : Http2Pipe {
            TlsStream tls;
            explicit TlsPipe(TlsStream s) noexcept : tls(std::move(s)) {}

            YarnBall::Task<std::size_t> readBytes(std::span<std::byte> buf) override {
                co_return co_await this->tls.read(buf);
            }
            YarnBall::Task<void> writeAll(std::span<const std::byte> buf) override {
                co_await this->tls.write(buf);
                co_return;
            }
            int fd() const noexcept override { return this->tls.fd(); }
            void close() noexcept override { this->tls.close(); }
        };
#endif
    }

    struct Http2Connection::State {
        /// Type-erased async pipe (TcpPipe for h2c, TlsPipe for h2).
        std::unique_ptr<Http2Pipe> pipe;

        /// nghttp2 session handle.
        ::nghttp2_session *session{nullptr};

        /// Serialises every nghttp2 API call. nghttp2 sessions are not
        /// thread-safe to call concurrently.
        std::mutex sessionMu;

        /// Serialises writes to the underlying stream. Drains pulled
        /// out of the session under sessionMu are written outside it.
        std::mutex writeMu;

        /// In-flight streams.
        std::mutex mapMu;
        std::unordered_map<std::int32_t, std::unique_ptr<detail::Http2Stream>> streams;

        /// Authority string (host:port) used for the :authority pseudo-
        /// header on every request. Captured at connect time so we do
        /// not need to call getpeername (which doesn't survive the TLS
        /// abstraction cleanly anyway).
        std::string authority;

        /// Scheme to emit on the @c :scheme pseudo-header for every
        /// outbound request. @c "http" for h2c, @c "https" for h2-over-
        /// TLS. Mismatched scheme is a hard 400 on RFC-strict servers
        /// (nghttpx, Envoy with strict validation).
        std::string scheme;

        /// Set after the driver loop exits. Connection is unusable.
        std::atomic<bool> closed{false};

        explicit State(std::unique_ptr<Http2Pipe> p, std::string auth,
                          std::string sch) noexcept
            : pipe(std::move(p)),
              authority(std::move(auth)),
              scheme(std::move(sch)) {}

        ~State() {
            if (this->session) {
                ::nghttp2_session_del(this->session);
                this->session = nullptr;
            }
        }
    };

    // -----------------------------------------------------------------
    // nghttp2 callbacks
    // -----------------------------------------------------------------

    namespace {
        /**
         * @brief on_header_callback: append the (name, value) pair to
         *        the response for the matching stream. Decodes
         *        @c :status into @c HttpResponse::status; drops other
         *        pseudo-headers.
         */
        int onHeader(::nghttp2_session * /*session*/,
                       const ::nghttp2_frame *frame,
                       const std::uint8_t *name, std::size_t namelen,
                       const std::uint8_t *value, std::size_t valuelen,
                       std::uint8_t /*flags*/,
                       void *user_data) noexcept {
            auto *state = static_cast<Http2Connection::State *>(user_data);
            if (frame->hd.type != NGHTTP2_HEADERS) return 0;
            std::string_view nm(reinterpret_cast<const char *>(name), namelen);
            std::string_view va(reinterpret_cast<const char *>(value), valuelen);

            detail::Http2Stream *st = nullptr;
            {
                std::lock_guard<std::mutex> lk(state->mapMu);
                auto it = state->streams.find(frame->hd.stream_id);
                if (it != state->streams.end()) st = it->second.get();
            }
            if (!st) return 0;

            // NGHTTP2_HCAT_HEADERS = post-response header block, i.e.
            // trailers. The initial response uses HCAT_RESPONSE.
            const bool isTrailer = frame->headers.cat == NGHTTP2_HCAT_HEADERS;

            if (nm == ":status") {
                st->response.status = std::atoi(std::string(va).c_str());
                return 0;
            }
            if (!nm.empty() && nm[0] == ':') return 0; // skip other pseudo-headers
            if (isTrailer) {
                st->response.trailers.push_back({std::string(nm), std::string(va)});
            } else {
                st->response.headers.push_back({std::string(nm), std::string(va)});
            }
            return 0;
        }

        /**
         * @brief on_data_chunk_recv_callback: append @p data bytes to
         *        the matching stream's response body.
         */
        int onDataChunk(::nghttp2_session * /*session*/,
                          std::uint8_t /*flags*/,
                          std::int32_t stream_id,
                          const std::uint8_t *data, std::size_t len,
                          void *user_data) noexcept {
            auto *state = static_cast<Http2Connection::State *>(user_data);
            detail::Http2Stream *st = nullptr;
            {
                std::lock_guard<std::mutex> lk(state->mapMu);
                auto it = state->streams.find(stream_id);
                if (it != state->streams.end()) st = it->second.get();
            }
            if (!st) return 0;
            st->response.body.append(reinterpret_cast<const char *>(data), len);
            return 0;
        }

        /**
         * @brief on_stream_close_callback: flip the stream's done
         *        flag and resume any waiter. The waiter coroutine
         *        consumes the response and removes the stream entry
         *        from the map (here we cannot, because nghttp2 may
         *        still reference the stream until this call returns).
         */
        int onStreamClose(::nghttp2_session * /*session*/,
                            std::int32_t stream_id,
                            std::uint32_t error_code,
                            void *user_data) noexcept {
            auto *state = static_cast<Http2Connection::State *>(user_data);
            std::coroutine_handle<> waiter{};
            {
                std::lock_guard<std::mutex> lk(state->mapMu);
                auto it = state->streams.find(stream_id);
                if (it == state->streams.end()) return 0;
                it->second->errorCode = static_cast<int>(error_code);
                it->second->done.store(true, std::memory_order_release);
                waiter = it->second->waiter;
                it->second->waiter = {};
            }
            if (waiter) {
                // Resume via Yarn so we don't grow the recv stack
                // (callbacks fire inside nghttp2_session_mem_recv).
                // Spell as unique_ptr<ITask> to disambiguate from the
                // CallableITask template overload of Yarn::run.
                try {
                    std::unique_ptr<YarnBall::ITask> ct{
                        new YarnBall::detail::CoroutineITask(waiter)};
                    YarnBall::Yarn::instance()->run(std::move(ct));
                } catch (...) {
                    waiter.resume();
                }
            }
            return 0;
        }

        /**
         * @brief Read callback for nghttp2_data_provider. Feeds the
         *        request body from the stream's buffer.
         */
        ::ssize_t requestBodyRead(::nghttp2_session * /*session*/,
                                    std::int32_t /*stream_id*/,
                                    std::uint8_t *buf, std::size_t length,
                                    std::uint32_t *data_flags,
                                    ::nghttp2_data_source *source,
                                    void * /*user_data*/) noexcept {
            auto *st = static_cast<detail::Http2Stream *>(source->ptr);
            const std::size_t remaining = st->body.size() - st->bodyOffset;
            const std::size_t toCopy = std::min(remaining, length);
            std::memcpy(buf, st->body.data() + st->bodyOffset, toCopy);
            st->bodyOffset += toCopy;
            if (st->bodyOffset == st->body.size()) {
                *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            }
            return static_cast<::ssize_t>(toCopy);
        }
    }

    // -----------------------------------------------------------------
    // Driver coroutine
    // -----------------------------------------------------------------

    namespace {
        /**
         * @brief Drain queued output from the session under
         *        @p state->sessionMu, accumulate into a local buffer,
         *        then write under @p state->writeMu so concurrent
         *        request() submits can also drain without serialising
         *        on the slow write syscall.
         */
        YarnBall::Task<void> drainAndWrite(std::shared_ptr<Http2Connection::State> state) {
            std::vector<std::uint8_t> outBuf;
            {
                std::lock_guard<std::mutex> lk(state->sessionMu);
                while (true) {
                    const std::uint8_t *data = nullptr;
                    ::ssize_t n = ::nghttp2_session_mem_send(state->session, &data);
                    if (n <= 0) break;
                    outBuf.insert(outBuf.end(), data, data + n);
                }
            }
            if (outBuf.empty()) co_return;
            std::lock_guard<std::mutex> lk(state->writeMu);
            co_await state->pipe->writeAll(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(outBuf.data()),
                outBuf.size()));
            co_return;
        }

        /**
         * @brief Long-lived reader: pull bytes off the wire, feed
         *        nghttp2 (callbacks fire synchronously inside the
         *        feed call), then drain whatever nghttp2 produced in
         *        response back onto the wire. Exits on EOF or peer
         *        GOAWAY.
         */
        YarnBall::Task<void> driver(std::shared_ptr<Http2Connection::State> state) {
            std::array<std::byte, 8192> buf{};
            while (!state->closed.load(std::memory_order_acquire)) {
                std::size_t n;
                try {
                    n = co_await state->pipe->readBytes(buf);
                } catch (const SocketException &) {
                    break;
                }
                if (n == 0) break;
                {
                    std::lock_guard<std::mutex> lk(state->sessionMu);
                    const ::ssize_t fed = ::nghttp2_session_mem_recv(
                        state->session,
                        reinterpret_cast<const std::uint8_t *>(buf.data()),
                        n);
                    if (fed < 0) break;
                }
                co_await drainAndWrite(state);

                // No-more-streams shutdown.
                {
                    std::lock_guard<std::mutex> lk(state->sessionMu);
                    if (::nghttp2_session_want_read(state->session) == 0 &&
                        ::nghttp2_session_want_write(state->session) == 0) {
                        break;
                    }
                }
            }
            state->closed.store(true, std::memory_order_release);

            // Resume any orphaned waiters with an error.
            std::vector<std::coroutine_handle<>> orphans;
            {
                std::lock_guard<std::mutex> lk(state->mapMu);
                for (auto &kv : state->streams) {
                    if (!kv.second->done.load(std::memory_order_acquire)) {
                        kv.second->errorCode = NGHTTP2_INTERNAL_ERROR;
                        kv.second->done.store(true, std::memory_order_release);
                        if (kv.second->waiter) {
                            orphans.push_back(kv.second->waiter);
                            kv.second->waiter = {};
                        }
                    }
                }
            }
            for (auto h : orphans) h.resume();
            co_return;
        }

        /**
         * @brief Build the standard set of client callbacks. nghttp2's
         *        callback object is small; we recreate it per session
         *        to keep this self-contained.
         */
        ::nghttp2_session_callbacks *makeClientCallbacks() {
            ::nghttp2_session_callbacks *cb = nullptr;
            ::nghttp2_session_callbacks_new(&cb);
            ::nghttp2_session_callbacks_set_on_header_callback(cb, &onHeader);
            ::nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, &onDataChunk);
            ::nghttp2_session_callbacks_set_on_stream_close_callback(cb, &onStreamClose);
            return cb;
        }
    }

    // -----------------------------------------------------------------
    // Per-stream awaiter
    // -----------------------------------------------------------------

    namespace {
        /**
         * @brief Suspend until the per-stream done flag is set by the
         *        nghttp2 close callback (or by the driver on a hard
         *        connection shutdown). On resume, copy the response
         *        out of the stream-state and remove the stream from
         *        the connection map.
         */
        struct StreamAwaiter {
            std::shared_ptr<Http2Connection::State> state;
            std::int32_t streamId;

            bool await_ready() const noexcept {
                std::lock_guard<std::mutex> lk(state->mapMu);
                auto it = state->streams.find(streamId);
                if (it == state->streams.end()) return true;
                return it->second->done.load(std::memory_order_acquire);
            }

            bool await_suspend(std::coroutine_handle<> h) noexcept {
                std::lock_guard<std::mutex> lk(state->mapMu);
                auto it = state->streams.find(streamId);
                if (it == state->streams.end()) return false;
                if (it->second->done.load(std::memory_order_acquire)) return false;
                it->second->waiter = h;
                return true;
            }

            Http2Response await_resume() {
                std::unique_ptr<detail::Http2Stream> st;
                {
                    std::lock_guard<std::mutex> lk(state->mapMu);
                    auto it = state->streams.find(streamId);
                    if (it == state->streams.end()) {
                        throw std::runtime_error("HTTP/2 stream vanished");
                    }
                    st = std::move(it->second);
                    state->streams.erase(it);
                }
                if (st->errorCode != 0) {
                    throw std::runtime_error("HTTP/2 stream error: nghttp2 code " +
                                              std::to_string(st->errorCode));
                }
                return std::move(st->response);
            }
        };
    }

    // -----------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------

    namespace {
        /**
         * @brief Common setup once a pipe (plain or TLS) is in hand:
         *        wire up the nghttp2 client session, send SETTINGS,
         *        spawn the driver, return the populated State.
         */
        YarnBall::Task<std::shared_ptr<Http2Connection::State>>
        finishHandshake(std::unique_ptr<Http2Pipe> pipe,
                          std::string authority,
                          std::string scheme) {
            auto state = std::make_shared<Http2Connection::State>(
                std::move(pipe), std::move(authority), std::move(scheme));

            ::nghttp2_session_callbacks *cb = makeClientCallbacks();
            if (::nghttp2_session_client_new(&state->session, cb,
                                              state.get()) != 0) {
                ::nghttp2_session_callbacks_del(cb);
                throw std::runtime_error("nghttp2_session_client_new failed");
            }
            ::nghttp2_session_callbacks_del(cb);

            ::nghttp2_settings_entry iv[1] = {
                {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
            };
            {
                std::lock_guard<std::mutex> lk(state->sessionMu);
                ::nghttp2_submit_settings(state->session,
                                           NGHTTP2_FLAG_NONE, iv, 1);
            }
            co_await drainAndWrite(state);

            YarnBall::coSpawn(driver(state));
            co_return state;
        }
    }

    YarnBall::Task<Http2Connection> Http2Connection::connectPlain(
        std::string host, std::uint16_t port) {
        TcpStream tcp = co_await tcpConnect(host, port);
        std::string authority = host + ":" + std::to_string(port);
        auto pipe = std::make_unique<TcpPipe>(std::move(tcp));
        auto state = co_await finishHandshake(
            std::move(pipe), std::move(authority), "http");
        Http2Connection conn;
        conn.state = std::move(state);
        co_return conn;
    }

#ifdef SOCCER_HAS_TLS
    YarnBall::Task<Http2Connection> Http2Connection::connect(
        std::string host, std::uint16_t port, TlsClientOptions tlsOpts) {
        // Ensure ALPN advertises "h2" -- the server will reject the
        // connection or downgrade to HTTP/1.1 otherwise. Append rather
        // than overwrite so callers who want to advertise both can.
        if (tlsOpts.alpnProtocols.find("h2") == std::string::npos) {
            tlsOpts.alpnProtocols =
                tlsOpts.alpnProtocols.empty()
                    ? std::string("h2")
                    : (tlsOpts.alpnProtocols + ",h2");
        }
        TlsStream tls = co_await TlsStream::connect(host, port, tlsOpts);
        std::string authority = host + ":" + std::to_string(port);
        auto pipe = std::make_unique<TlsPipe>(std::move(tls));
        auto state = co_await finishHandshake(
            std::move(pipe), std::move(authority), "https");
        Http2Connection conn;
        conn.state = std::move(state);
        co_return conn;
    }
#endif

    YarnBall::Task<Http2Response> Http2Connection::request(
        std::string method,
        std::string path,
        std::vector<HttpHeader> headers,
        std::string body) {
        if (!this->state || this->state->closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("Http2Connection::request: connection closed");
        }
        auto state = this->state; // shared_ptr copy for the awaiter

        // Build HPACK header list. nghttp2 takes a flat vector of
        // (name, value, name_len, value_len, flags).
        std::vector<::nghttp2_nv> nv;
        nv.reserve(4 + headers.size());

        auto add = [&nv](std::string_view k, std::string_view v) {
            nv.push_back(::nghttp2_nv{
                const_cast<std::uint8_t *>(reinterpret_cast<const std::uint8_t *>(k.data())),
                const_cast<std::uint8_t *>(reinterpret_cast<const std::uint8_t *>(v.data())),
                k.size(), v.size(),
                NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE,
            });
        };

        add(":method", method);
        add(":scheme", state->scheme);
        add(":authority", state->authority);
        add(":path", path);
        for (const auto &h : headers) add(h.name, h.value);

        // Create per-stream state, set body for the data provider.
        auto streamState = std::make_unique<detail::Http2Stream>();
        streamState->body = std::move(body);

        ::nghttp2_data_provider provider{};
        ::nghttp2_data_provider *providerPtr = nullptr;
        if (!streamState->body.empty()) {
            provider.source.ptr = streamState.get();
            provider.read_callback = &requestBodyRead;
            providerPtr = &provider;
        }

        std::int32_t streamId;
        {
            std::lock_guard<std::mutex> lk(state->sessionMu);
            streamId = ::nghttp2_submit_request(
                state->session, nullptr,
                nv.data(), nv.size(),
                providerPtr, streamState.get());
            if (streamId < 0) {
                throw std::runtime_error(std::string("nghttp2_submit_request: ") +
                                          ::nghttp2_strerror(streamId));
            }
        }
        {
            std::lock_guard<std::mutex> lk(state->mapMu);
            state->streams.emplace(streamId, std::move(streamState));
        }

        // Push the freshly enqueued frames onto the wire so the peer
        // sees the request without waiting for any inbound activity.
        co_await drainAndWrite(state);

        // Suspend until the stream completes.
        co_return co_await StreamAwaiter{state, streamId};
    }

    void Http2Connection::close() noexcept {
        if (!this->state) return;
        if (this->state->closed.load(std::memory_order_acquire)) return;
        {
            std::lock_guard<std::mutex> lk(this->state->sessionMu);
            ::nghttp2_submit_goaway(this->state->session, NGHTTP2_FLAG_NONE,
                                     0, NGHTTP2_NO_ERROR, nullptr, 0);
        }
        // Best-effort drain; if it can't be flushed, the driver will
        // shut the connection down on the next iteration anyway.
        try {
            YarnBall::syncWait(drainAndWrite(this->state));
        } catch (...) {}
        this->state->closed.store(true, std::memory_order_release);
        this->state->pipe->close();
    }

    Http2Connection::Http2Connection(Http2Connection &&other) noexcept
        : state(std::move(other.state)) {}

    Http2Connection &Http2Connection::operator=(Http2Connection &&other) noexcept {
        if (this != &other) {
            this->close();
            this->state = std::move(other.state);
        }
        return *this;
    }

    Http2Connection::~Http2Connection() {
        this->close();
    }

    // =================================================================
    // Server side
    // =================================================================

    /**
     * @brief Per-stream state for a server connection. Holds the
     *        request being assembled, then (after the handler runs)
     *        the response being shipped back out.
     */
    namespace {
        struct ServerStream {
            HttpRequest req;
            std::string respBody;
            std::size_t respBodyOffset{0};
            std::vector<std::pair<std::string, std::string>> respHeaders;
            /**
             * @brief Response trailers (HTTP/2). When non-empty, the
             *        body's data provider emits
             *        NGHTTP2_DATA_FLAG_NO_END_STREAM so the trailer
             *        HEADERS frame can follow and close the stream.
             */
            std::vector<std::pair<std::string, std::string>> respTrailers;
            std::string respStatusStr;
            bool handlerStarted{false};
            bool responseSubmitted{false};
        };
    }

    /**
     * @brief Per-connection server state: nghttp2 server session, the
     *        underlying pipe, in-flight per-stream maps, and the
     *        shared route table.
     */
    struct Http2ServerConnectionState {
        std::unique_ptr<Http2Pipe> pipe;
        ::nghttp2_session *session{nullptr};
        std::mutex sessionMu;
        std::mutex writeMu;

        std::mutex streamsMu;
        std::unordered_map<std::int32_t, std::unique_ptr<ServerStream>> streams;

        /// Shared route table from the owning Http2Server. Read-only
        /// after the server starts; no mutex needed.
        const std::unordered_map<std::string, HttpRouteHandler> *routes{nullptr};

        std::atomic<bool> closed{false};

        Http2ServerConnectionState(std::unique_ptr<Http2Pipe> p,
                                       const std::unordered_map<std::string, HttpRouteHandler> *r) noexcept
            : pipe(std::move(p)), routes(r) {}

        ~Http2ServerConnectionState() {
            if (this->session) {
                ::nghttp2_session_del(this->session);
                this->session = nullptr;
            }
        }
    };

    namespace {
        std::string routeKey(std::string_view method, std::string_view path) {
            std::string k;
            k.reserve(method.size() + 1 + path.size());
            for (char c : method) k.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            k.push_back(' ');
            k.append(path);
            return k;
        }

        /**
         * @brief Drain queued bytes off the server session and write
         *        them to the wire. Same pattern as the client side.
         */
        YarnBall::Task<void> serverDrainAndWrite(
            std::shared_ptr<Http2ServerConnectionState> state) {
            std::vector<std::uint8_t> outBuf;
            {
                std::lock_guard<std::mutex> lk(state->sessionMu);
                while (true) {
                    const std::uint8_t *data = nullptr;
                    ::ssize_t n = ::nghttp2_session_mem_send(state->session, &data);
                    if (n <= 0) break;
                    outBuf.insert(outBuf.end(), data, data + n);
                }
            }
            if (outBuf.empty()) co_return;
            std::lock_guard<std::mutex> lk(state->writeMu);
            co_await state->pipe->writeAll(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(outBuf.data()),
                outBuf.size()));
            co_return;
        }

        /**
         * @brief Server-side response-body read callback. Feeds bytes
         *        from the ServerStream's respBody into nghttp2 as
         *        nghttp2 pulls them.
         */
        ::ssize_t serverBodyRead(::nghttp2_session *session,
                                   std::int32_t stream_id,
                                   std::uint8_t *buf, std::size_t length,
                                   std::uint32_t *data_flags,
                                   ::nghttp2_data_source *source,
                                   void * /*user_data*/) noexcept {
            auto *st = static_cast<ServerStream *>(source->ptr);
            const std::size_t remaining = st->respBody.size() - st->respBodyOffset;
            const std::size_t toCopy = std::min(remaining, length);
            std::memcpy(buf, st->respBody.data() + st->respBodyOffset, toCopy);
            st->respBodyOffset += toCopy;
            if (st->respBodyOffset == st->respBody.size()) {
                *data_flags |= NGHTTP2_DATA_FLAG_EOF;
                if (!st->respTrailers.empty()) {
                    // Suppress END_STREAM so the trailer HEADERS we
                    // submit immediately below can close the stream
                    // instead. Submitting the trailer here (inside
                    // the data_provider callback at EOF) is the
                    // pattern nghttp2's own examples use; it
                    // guarantees ordering between the last DATA
                    // frame and the trailer HEADERS frame.
                    *data_flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;

                    std::vector<::nghttp2_nv> trailNv;
                    trailNv.reserve(st->respTrailers.size());
                    for (const auto &h : st->respTrailers) {
                        trailNv.push_back(::nghttp2_nv{
                            const_cast<std::uint8_t *>(
                                reinterpret_cast<const std::uint8_t *>(h.first.data())),
                            const_cast<std::uint8_t *>(
                                reinterpret_cast<const std::uint8_t *>(h.second.data())),
                            h.first.size(), h.second.size(),
                            NGHTTP2_NV_FLAG_NONE,
                        });
                    }
                    const int rc = ::nghttp2_submit_trailer(
                        session, stream_id,
                        trailNv.data(), trailNv.size());
                    if (rc != 0) {
                        std::fprintf(stderr,
                                      "nghttp2_submit_trailer failed: %s (%d)\n",
                                      ::nghttp2_strerror(rc), rc);
                    }
                }
            }
            return static_cast<::ssize_t>(toCopy);
        }

        /**
         * @brief Submit the response on @p streamId after the handler
         *        produced @p resp. Builds the header block and (when
         *        body is non-empty) installs the data provider.
         */
        void submitResponseLocked(Http2ServerConnectionState *state,
                                    std::int32_t streamId,
                                    ServerStream *st,
                                    const HttpResponse &resp) {
            st->respStatusStr = std::to_string(resp.status);
            st->respBody = resp.body;
            st->respBodyOffset = 0;
            st->respHeaders.clear();
            st->respTrailers.clear();
            // Copy user headers; nghttp2 needs stable pointers, so we
            // store the strings in the stream-state vector and feed
            // nghttp2_nv pointers into it.
            for (const auto &h : resp.headers) {
                st->respHeaders.emplace_back(h.name, h.value);
            }
            for (const auto &h : resp.trailers) {
                st->respTrailers.emplace_back(h.name, h.value);
            }
            // Always emit content-length so the peer can validate
            // framing without relying on END_STREAM semantics.
            st->respHeaders.emplace_back("content-length",
                                          std::to_string(st->respBody.size()));

            std::vector<::nghttp2_nv> nv;
            nv.reserve(1 + st->respHeaders.size());

            auto add = [&nv](std::string_view k, std::string_view v) {
                nv.push_back(::nghttp2_nv{
                    const_cast<std::uint8_t *>(reinterpret_cast<const std::uint8_t *>(k.data())),
                    const_cast<std::uint8_t *>(reinterpret_cast<const std::uint8_t *>(v.data())),
                    k.size(), v.size(),
                    NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE,
                });
            };
            add(":status", st->respStatusStr);
            for (const auto &h : st->respHeaders) add(h.first, h.second);

            ::nghttp2_data_provider provider{};
            ::nghttp2_data_provider *providerPtr = nullptr;
            if (!st->respBody.empty()) {
                provider.source.ptr = st;
                provider.read_callback = &serverBodyRead;
                providerPtr = &provider;
            }

            ::nghttp2_submit_response(state->session, streamId,
                                       nv.data(), nv.size(), providerPtr);
            // Trailers are submitted inside the data provider's EOF
            // call so nghttp2's frame queue serialises them correctly
            // after the last DATA frame.
            st->responseSubmitted = true;
        }

        /**
         * @brief Run a route handler for one (streamId, request) pair,
         *        then submit the response. Spawned by the on_frame_recv
         *        callback when a complete request arrives.
         */
        YarnBall::Task<void> runOneHandler(
            std::shared_ptr<Http2ServerConnectionState> state,
            std::int32_t streamId,
            HttpRequest req) {
            HttpResponse resp;
            try {
                const auto key = routeKey(req.method, req.path);
                auto it = state->routes->find(key);
                if (it != state->routes->end()) {
                    resp = co_await it->second(std::move(req));
                } else {
                    resp.status = 404;
                    resp.body = "not found";
                }
            } catch (const std::exception &e) {
                resp.status = 500;
                resp.body = e.what();
            }
            ServerStream *st = nullptr;
            {
                std::lock_guard<std::mutex> lk(state->streamsMu);
                auto it = state->streams.find(streamId);
                if (it == state->streams.end()) co_return;
                st = it->second.get();
            }
            {
                std::lock_guard<std::mutex> lk(state->sessionMu);
                submitResponseLocked(state.get(), streamId, st, resp);
            }
            co_await serverDrainAndWrite(state);
            co_return;
        }

        // ---- nghttp2 server callbacks -------------------------------

        int serverOnBeginHeaders(::nghttp2_session * /*session*/,
                                   const ::nghttp2_frame *frame,
                                   void *user_data) noexcept {
            if (frame->hd.type != NGHTTP2_HEADERS ||
                frame->headers.cat != NGHTTP2_HCAT_REQUEST) return 0;
            auto *state = static_cast<Http2ServerConnectionState *>(user_data);
            std::lock_guard<std::mutex> lk(state->streamsMu);
            state->streams.emplace(frame->hd.stream_id,
                                    std::make_unique<ServerStream>());
            return 0;
        }

        int serverOnHeader(::nghttp2_session * /*session*/,
                             const ::nghttp2_frame *frame,
                             const std::uint8_t *name, std::size_t namelen,
                             const std::uint8_t *value, std::size_t valuelen,
                             std::uint8_t /*flags*/,
                             void *user_data) noexcept {
            if (frame->hd.type != NGHTTP2_HEADERS) return 0;
            auto *state = static_cast<Http2ServerConnectionState *>(user_data);

            ServerStream *st = nullptr;
            {
                std::lock_guard<std::mutex> lk(state->streamsMu);
                auto it = state->streams.find(frame->hd.stream_id);
                if (it != state->streams.end()) st = it->second.get();
            }
            if (!st) return 0;

            std::string_view nm(reinterpret_cast<const char *>(name), namelen);
            std::string_view va(reinterpret_cast<const char *>(value), valuelen);

            if (nm == ":method") {
                st->req.method = std::string(va);
                return 0;
            }
            if (nm == ":path") {
                st->req.path = std::string(va);
                return 0;
            }
            // Drop other pseudo-headers (:scheme, :authority) for v1;
            // they're available to handlers via the underlying socket
            // if they really need them.
            if (!nm.empty() && nm[0] == ':') return 0;
            st->req.headers.push_back({std::string(nm), std::string(va)});
            return 0;
        }

        int serverOnDataChunk(::nghttp2_session * /*session*/,
                                std::uint8_t /*flags*/,
                                std::int32_t stream_id,
                                const std::uint8_t *data, std::size_t len,
                                void *user_data) noexcept {
            auto *state = static_cast<Http2ServerConnectionState *>(user_data);
            std::lock_guard<std::mutex> lk(state->streamsMu);
            auto it = state->streams.find(stream_id);
            if (it == state->streams.end()) return 0;
            it->second->req.body.append(reinterpret_cast<const char *>(data), len);
            return 0;
        }

        /**
         * @brief Fires on every received frame. We dispatch the
         *        handler when END_STREAM is observed on the HEADERS
         *        (body-less request) or on a DATA frame (body request).
         *        The dispatch is a coSpawn so we don't run the user
         *        coroutine inside the nghttp2 callback stack.
         */
        int serverOnFrameRecv(::nghttp2_session * /*session*/,
                                const ::nghttp2_frame *frame,
                                void *user_data) noexcept {
            auto *state = static_cast<Http2ServerConnectionState *>(user_data);

            const bool endStream =
                (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;
            const bool isHeaders = frame->hd.type == NGHTTP2_HEADERS;
            const bool isData = frame->hd.type == NGHTTP2_DATA;
            if (!endStream || (!isHeaders && !isData)) return 0;

            // Mark the stream as ready for dispatch. The actual
            // handler spawn happens in the driver loop after this
            // recv call returns, where we hold the shared_ptr to
            // the connection state by value and can capture it
            // into the coroutine.
            const std::int32_t streamId = frame->hd.stream_id;
            std::lock_guard<std::mutex> lk(state->streamsMu);
            auto it = state->streams.find(streamId);
            if (it == state->streams.end()) return 0;
            it->second->handlerStarted = true;
            return 0;
        }

        int serverOnStreamClose(::nghttp2_session * /*session*/,
                                  std::int32_t stream_id,
                                  std::uint32_t /*error_code*/,
                                  void *user_data) noexcept {
            auto *state = static_cast<Http2ServerConnectionState *>(user_data);
            std::lock_guard<std::mutex> lk(state->streamsMu);
            state->streams.erase(stream_id);
            return 0;
        }

        ::nghttp2_session_callbacks *makeServerCallbacks() {
            ::nghttp2_session_callbacks *cb = nullptr;
            ::nghttp2_session_callbacks_new(&cb);
            ::nghttp2_session_callbacks_set_on_begin_headers_callback(cb, &serverOnBeginHeaders);
            ::nghttp2_session_callbacks_set_on_header_callback(cb, &serverOnHeader);
            ::nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, &serverOnDataChunk);
            ::nghttp2_session_callbacks_set_on_frame_recv_callback(cb, &serverOnFrameRecv);
            ::nghttp2_session_callbacks_set_on_stream_close_callback(cb, &serverOnStreamClose);
            return cb;
        }
    }

    /**
     * @brief Per-connection driver: read bytes, feed nghttp2, drain
     *        output, repeat. After each recv we walk the streams
     *        map and dispatch handler for any newly-completed
     *        requests (the on_frame_recv callback marked them).
     */
    namespace {
        YarnBall::Task<void> serverDriver(
            std::shared_ptr<Http2ServerConnectionState> state) {
            // Send initial SETTINGS.
            ::nghttp2_settings_entry iv[1] = {
                {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
            };
            {
                std::lock_guard<std::mutex> lk(state->sessionMu);
                ::nghttp2_submit_settings(state->session,
                                           NGHTTP2_FLAG_NONE, iv, 1);
            }
            co_await serverDrainAndWrite(state);

            std::array<std::byte, 8192> buf{};
            while (!state->closed.load(std::memory_order_acquire)) {
                std::size_t n;
                try {
                    n = co_await state->pipe->readBytes(buf);
                } catch (const SocketException &) {
                    break;
                }
                if (n == 0) break;
                {
                    std::lock_guard<std::mutex> lk(state->sessionMu);
                    const ::ssize_t fed = ::nghttp2_session_mem_recv(
                        state->session,
                        reinterpret_cast<const std::uint8_t *>(buf.data()),
                        n);
                    if (fed < 0) break;
                }
                // Dispatch any handler-started streams whose handler
                // hasn't actually been spawned yet. The callback only
                // flips the flag; spawn happens here so the driver's
                // shared_ptr is captured by value into the handler
                // coroutine.
                std::vector<std::pair<std::int32_t, HttpRequest>> ready;
                {
                    std::lock_guard<std::mutex> lk(state->streamsMu);
                    for (auto &kv : state->streams) {
                        if (kv.second->handlerStarted &&
                            !kv.second->responseSubmitted) {
                            // Move req out; mark responseSubmitted=true
                            // pre-emptively so a subsequent driver
                            // tick does not re-dispatch the same
                            // request before the handler completes.
                            ready.emplace_back(kv.first, std::move(kv.second->req));
                            kv.second->responseSubmitted = true;
                        }
                    }
                }
                for (auto &[sid, req] : ready) {
                    YarnBall::coSpawn(runOneHandler(state, sid, std::move(req)));
                }
                co_await serverDrainAndWrite(state);
                {
                    std::lock_guard<std::mutex> lk(state->sessionMu);
                    if (::nghttp2_session_want_read(state->session) == 0 &&
                        ::nghttp2_session_want_write(state->session) == 0) {
                        break;
                    }
                }
            }
            state->closed.store(true, std::memory_order_release);
            co_return;
        }
    }

    // ---- Http2Server class --------------------------------------------

    struct Http2Server::ServerState {
        TcpListener listener;
        std::unordered_map<std::string, HttpRouteHandler> routes;

        explicit ServerState(TcpListener l) noexcept
            : listener(std::move(l)) {}
    };

    Http2Server::Http2Server(const std::string &host, std::uint16_t port)
        : serverState(std::make_shared<ServerState>(TcpListener::bind(host, port))) {
    }

    Http2Server::Http2Server(Http2Server &&) noexcept = default;
    Http2Server &Http2Server::operator=(Http2Server &&) noexcept = default;
    Http2Server::~Http2Server() = default;

    void Http2Server::route(std::string method,
                                std::string path,
                                HttpRouteHandler handler) {
        this->serverState->routes[routeKey(method, path)] = std::move(handler);
    }

    SocketAddress Http2Server::localAddress() const {
        return this->serverState->listener.localAddress();
    }

    namespace {
        // Free coroutine that owns its parameters by value. Used by
        // Http2Server::serve so the per-connection handler does NOT
        // live inside a lambda-coroutine closure (which would dangle:
        // the closure is a temporary in tcpServe's frame; the
        // coroutine's implicit @c this would point at freed memory).
        YarnBall::Task<void> runServerConnection(
            std::shared_ptr<Http2Server::ServerState> server,
            TcpStream client) {
            auto pipe = std::make_unique<TcpPipe>(std::move(client));
            auto state = std::make_shared<Http2ServerConnectionState>(
                std::move(pipe), &server->routes);

            ::nghttp2_session_callbacks *cb = makeServerCallbacks();
            if (::nghttp2_session_server_new(&state->session, cb,
                                              state.get()) != 0) {
                ::nghttp2_session_callbacks_del(cb);
                co_return;
            }
            ::nghttp2_session_callbacks_del(cb);

            co_await serverDriver(state);
            co_return;
        }
    }

    YarnBall::Task<void> Http2Server::serve(std::stop_token stop) {
        // The factory below is NOT a coroutine. It forwards into the
        // free @c runServerConnection coroutine, whose own frame owns
        // @c server and @c client by value -- the only safe lifetime
        // pattern for tcpServe handlers (see HttpServer::serve for the
        // sibling case and docs/coroutines.md for the rationale).
        auto serverState = this->serverState;
        auto factory = [serverState](TcpStream client) -> YarnBall::Task<void> {
            return runServerConnection(serverState, std::move(client));
        };

        co_await tcpServe(std::move(this->serverState->listener),
                           factory, stop);
        co_return;
    }

    // =================================================================
    // Http2ConnectionPool
    // =================================================================

    Http2ConnectionPool &Http2ConnectionPool::defaultPool() {
        static Http2ConnectionPool p;
        return p;
    }

    YarnBall::Task<std::shared_ptr<Http2Connection>>
    Http2ConnectionPool::acquirePlain(std::string host, std::uint16_t port) {
        const std::string k = keyOf(host, port);
        {
            std::lock_guard<std::mutex> lk(this->mu);
            auto it = this->entries.find(k);
            if (it != this->entries.end()) co_return it->second;
        }
        auto conn = std::make_shared<Http2Connection>(
            co_await Http2Connection::connectPlain(host, port));
        std::lock_guard<std::mutex> lk(this->mu);
        // Race: another acquirer may have populated the entry while
        // we were connecting. Prefer theirs; let our local conn drop.
        auto [ins, fresh] = this->entries.emplace(k, conn);
        co_return ins->second;
    }

#ifdef SOCCER_HAS_TLS
    YarnBall::Task<std::shared_ptr<Http2Connection>>
    Http2ConnectionPool::acquire(std::string host, std::uint16_t port,
                                    TlsClientOptions opts) {
        // Key includes whether TLS-verify is on so insecure-test and
        // production configs do not share a connection.
        std::string k = keyOf(host, port);
        k.push_back('|');
        k.append(opts.caBundleFile);
        k.push_back('|');
        k.append(opts.clientCertFile);
        k.push_back('|');
        k.append(opts.alpnProtocols);
        {
            std::lock_guard<std::mutex> lk(this->mu);
            auto it = this->entries.find(k);
            if (it != this->entries.end()) co_return it->second;
        }
        auto conn = std::make_shared<Http2Connection>(
            co_await Http2Connection::connect(host, port, std::move(opts)));
        std::lock_guard<std::mutex> lk(this->mu);
        auto [ins, fresh] = this->entries.emplace(k, conn);
        co_return ins->second;
    }
#endif

    void Http2ConnectionPool::evict(std::string_view host, std::uint16_t port) {
        const std::string k = keyOf(host, port);
        std::lock_guard<std::mutex> lk(this->mu);
        this->entries.erase(k);
    }

    std::size_t Http2ConnectionPool::size() const {
        std::lock_guard<std::mutex> lk(this->mu);
        return this->entries.size();
    }

}

#endif // SOCCER_HAS_HTTP2
