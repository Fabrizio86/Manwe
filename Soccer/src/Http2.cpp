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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <stop_token>
#include <utility>

#include "Log.h"

#include "ConnectionLimiter.h"
#include "Cancellation.h"
#include "Coroutines.h"
#include "IoAwaiters.h"
#include "IoDeadline.h"
#include "NetUtil.h"
#include "SocketException.h"
#include "TcpStream.h"
#include "Timers.h"

#ifdef SOCCER_HAS_TLS
#include "TlsStream.h"
#endif

namespace Soccer {

    namespace {
        /**
         * @brief Hard cap on the accumulated size of a single HTTP/2
         *        request or response body. DATA frames carry no
         *        upfront length the way HTTP/1.1's Content-Length
         *        does, and HTTP/2 flow control only paces delivery --
         *        it does not bound the total bytes a stream can carry
         *        over its lifetime. Without this cap a peer can stream
         *        an unbounded body and grow the accumulating
         *        @c std::string without limit. Matches the 16 MiB
         *        ceiling @c HttpClient.h and @c WebSocket.cpp already
         *        enforce elsewhere in Soccer.
         */
        inline constexpr std::size_t kHttp2MaxBodyBytes = 16ull * 1024ull * 1024ull;

        /**
         * @brief Cap on the response headers a client accepts per stream: the
         *        accumulated size of every header field (name + value + 32 bytes of
         *        overhead, as in RFC 7541), counting interim 1xx responses and
         *        trailers too, and the number of 1xx interim responses.
         */
        inline constexpr std::size_t kHttp2MaxHeaderBytes = 64ull * 1024ull;
        inline constexpr int kHttp2MaxInterimResponses = 8;

        /// HTTP/2 header field names must be lowercase (RFC 9113 8.2.1); a peer treats an
        /// uppercase name as a protocol error, so callers' HTTP/1-style names are folded.
        std::string lowerAscii(std::string_view in) {
            std::string out(in);
            for (char &c : out) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
            }
            return out;
        }

        /**
         * @brief Hand @p h to the Yarn pool instead of resuming it on the
         *        caller's stack. Used wherever a waiter is woken from inside an
         *        nghttp2 callback or from close().
         */
        void resumeOnYarn(std::coroutine_handle<> h) noexcept {
            try {
                // Spelled as unique_ptr<ITask> to disambiguate from the
                // CallableITask template overload of Yarn::run.
                std::unique_ptr<YarnBall::ITask> ct{new YarnBall::detail::CoroutineITask(h)};
                YarnBall::Yarn::instance()->run(std::move(ct));
            } catch (...) {
                h.resume();
            }
        }

        struct OptionDeleter {
            void operator()(::nghttp2_option *o) const noexcept { ::nghttp2_option_del(o); }
        };
        using OptionPtr = std::unique_ptr<::nghttp2_option, OptionDeleter>;

        /**
         * @brief Session options shared by both ends: bounds on what a peer can make
         *        the session buffer or burn CPU on. CONTINUATION flooding (a header
         *        block spread over unbounded CONTINUATION frames, CVE-2024-28182)
         *        is cut off at a handful of frames; the number of SETTINGS entries and
         *        of unacknowledged SETTINGS / PING acks we are willing to queue are
         *        capped. Returns null if nghttp2 cannot allocate (the session then
         *        runs on the library defaults, which already include these limits).
         */
        OptionPtr makeSessionOptions() {
            ::nghttp2_option *raw = nullptr;
            if (::nghttp2_option_new(&raw) != 0) return nullptr;
            OptionPtr opt(raw);
            // Each setter exists only in libraries recent (or patched) enough; the
            // build probes for them, see Soccer/CMakeLists.txt.
#ifdef SOCCER_NGHTTP2_HAS_MAX_CONTINUATIONS
            ::nghttp2_option_set_max_continuations(raw, 8);
#endif
#ifdef SOCCER_NGHTTP2_HAS_MAX_SETTINGS
            ::nghttp2_option_set_max_settings(raw, 32);
#endif
#ifdef SOCCER_NGHTTP2_HAS_MAX_OUTBOUND_ACK
            ::nghttp2_option_set_max_outbound_ack(raw, 100);
#endif
            return opt;
        }
    }

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
        /// Shut the socket down without closing it: wakes a coroutine parked reading it.
        void shutdown() noexcept {
            const int f = this->fd();
            if (f >= 0) detail::shutdownSocket(f);
        }
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
                    // A write that moves nothing would drop the rest of the buffer and leave
                    // the peer's HPACK state out of step with ours: the connection is lost.
                    if (n == 0) throw SocketException("Http2: write made no progress");
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

        /// Whether a writer coroutine is currently flushing session output (see
        /// flushSession). Guarded by sessionMu, so output order always equals nghttp2's
        /// emission order (HPACK state depends on it) and no mutex is ever held across
        /// a suspension point.
        bool writing{false};

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

        /// steady_clock time (ns) the write in progress began, 0 when none.
        std::atomic<std::int64_t> writeStartedNs{0};

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
         * @brief on_begin_headers_callback: a new header block starts on a stream
         *        (response, interim response or trailers). Resets the per-block
         *        flag that tells trailers from the real response headers.
         */
        int onBeginHeaders(::nghttp2_session * /*session*/,
                             const ::nghttp2_frame *frame,
                             void *user_data) noexcept {
            auto *state = static_cast<Http2Connection::State *>(user_data);
            if (frame->hd.type != NGHTTP2_HEADERS) return 0;
            std::lock_guard<std::mutex> lk(state->mapMu);
            auto it = state->streams.find(frame->hd.stream_id);
            if (it != state->streams.end()) it->second->statusInBlock = false;
            return 0;
        }

        /**
         * @brief on_header_callback: append the (name, value) pair to
         *        the response for the matching stream. Decodes
         *        @c :status into @c HttpResponse::status; drops other
         *        pseudo-headers. Headers of 1xx interim responses are
         *        dropped (they are not part of the final response); a header
         *        block that follows the final response is its trailers. The
         *        total header size and the number of interim responses are
         *        capped so a peer cannot make the client accumulate without limit.
         */
        int onHeader(::nghttp2_session *session,
                       const ::nghttp2_frame *frame,
                       const std::uint8_t *name, std::size_t namelen,
                       const std::uint8_t *value, std::size_t valuelen,
                       std::uint8_t /*flags*/,
                       void *user_data) noexcept try {
            auto *state = static_cast<Http2Connection::State *>(user_data);
            if (frame->hd.type != NGHTTP2_HEADERS) return 0;
            std::string_view nm(reinterpret_cast<const char *>(name), namelen);
            std::string_view va(reinterpret_cast<const char *>(value), valuelen);

            // Held for the whole callback: the awaiter takes the response out under
            // this mutex once the stream is done, so a callback running after the stream
            // was failed (connection lost, timeout) must not be writing into it then.
            std::lock_guard<std::mutex> lk(state->mapMu);
            auto sit = state->streams.find(frame->hd.stream_id);
            detail::Http2Stream *st = sit == state->streams.end() ? nullptr : sit->second.get();
            if (!st || st->headersRejected || st->done.load(std::memory_order_acquire)) return 0;

            st->headerBytes += namelen + valuelen + 32;
            bool reject = st->headerBytes > kHttp2MaxHeaderBytes;

            if (nm == ":status") {
                const int status = std::atoi(std::string(va).c_str());
                st->statusInBlock = true;
                if (status >= 100 && status < 200) {
                    st->informational = true;
                    if (++st->interimCount > kHttp2MaxInterimResponses) reject = true;
                } else {
                    st->informational = false;
                    st->finalSeen = true;
                    st->response.status = status;
                }
            } else if (!(!nm.empty() && nm[0] == ':') && !reject && !st->informational) {
                // A block with no :status that follows the final response is the trailers.
                // (Decided from the stream's own state, not from the frame category, which
                // nghttp2 also uses for a final response that follows an interim one.)
                if (st->finalSeen && !st->statusInBlock) {
                    st->response.trailers.push_back({std::string(nm), std::string(va)});
                } else {
                    st->response.headers.push_back({std::string(nm), std::string(va)});
                }
            }

            if (reject) {
                st->headersRejected = true;
                st->response.headers.clear();
                st->response.trailers.clear();
                ::nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE,
                                             frame->hd.stream_id, NGHTTP2_ENHANCE_YOUR_CALM);
            }
            return 0;
        } catch (...) {
            // Out of memory while storing a header: a callback may not throw through
            // nghttp2, so fail the session; the driver then ends the connection.
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }

        /**
         * @brief on_data_chunk_recv_callback: append @p data bytes to
         *        the matching stream's response body, up to
         *        @ref kHttp2MaxBodyBytes. A peer that exceeds the cap
         *        has the stream reset rather than being allowed to
         *        grow the accumulation buffer without limit. The
         *        rejection is a one-way latch (@c bodyRejected): a
         *        single @c nghttp2_submit_rst_stream call does not
         *        stop nghttp2 from invoking this callback again for
         *        DATA frames already parsed out of the same
         *        @c nghttp2_session_mem_recv batch, so clearing the
         *        body without latching would let the cap silently
         *        reset itself and keep re-accumulating instead of
         *        durably rejecting the stream.
         */
        int onDataChunk(::nghttp2_session *session,
                          std::uint8_t /*flags*/,
                          std::int32_t stream_id,
                          const std::uint8_t *data, std::size_t len,
                          void *user_data) noexcept try {
            auto *state = static_cast<Http2Connection::State *>(user_data);
            std::lock_guard<std::mutex> lk(state->mapMu); // see onHeader
            auto sit = state->streams.find(stream_id);
            detail::Http2Stream *st = sit == state->streams.end() ? nullptr : sit->second.get();
            if (!st || st->done.load(std::memory_order_acquire)) return 0;
            if (st->bodyRejected) return 0;
            if (st->response.body.size() + len > kHttp2MaxBodyBytes) {
                st->bodyRejected = true;
                st->response.body.clear();
                st->response.body.shrink_to_fit();
                ::nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE,
                                             stream_id, NGHTTP2_ENHANCE_YOUR_CALM);
                return 0;
            }
            st->response.body.append(reinterpret_cast<const char *>(data), len);
            return 0;
        } catch (...) {
            return NGHTTP2_ERR_CALLBACK_FAILURE; // out of memory, see onHeader
        }

        /**
         * @brief on_frame_recv_callback: note that the peer ended a stream normally
         *        (END_STREAM on the response's HEADERS or DATA). A stream that merely
         *        closes -- RST_STREAM(NO_ERROR), say -- has not delivered a response.
         */
        int onFrameRecv(::nghttp2_session * /*session*/,
                          const ::nghttp2_frame *frame,
                          void *user_data) noexcept {
            if ((frame->hd.type != NGHTTP2_HEADERS && frame->hd.type != NGHTTP2_DATA) ||
                (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) == 0) return 0;
            auto *state = static_cast<Http2Connection::State *>(user_data);
            std::lock_guard<std::mutex> lk(state->mapMu);
            auto it = state->streams.find(frame->hd.stream_id);
            if (it != state->streams.end()) it->second->endStreamSeen = true;
            return 0;
        }

        /**
         * @brief on_stream_close_callback: flip the stream's done
         *        flag and resume any waiter. The stream object is freed
         *        by whichever of this callback and the awaiter's
         *        @c await_resume comes second (see Http2Stream), never
         *        while the session might still read its body.
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
                detail::Http2Stream &st = *it->second;
                // The session is finished with the stream: from here nghttp2 no longer
                // reaches the body provider's source, so the object may be freed.
                st.sessionClosed = true;
                if (!st.done.load(std::memory_order_acquire)) {
                    st.errorCode = static_cast<int>(error_code);
                    st.done.store(true, std::memory_order_release);
                }
                waiter = st.waiter;
                st.waiter = {};
                if (st.consumed) state->streams.erase(it); // the awaiter already left
            }
            // Resume via Yarn so we don't grow the recv stack
            // (callbacks fire inside nghttp2_session_mem_recv).
            if (waiter) resumeOnYarn(waiter);
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
        /// Monotonic clock in nanoseconds, for the lock-free timestamps below.
        inline std::int64_t steadyNowNs() noexcept {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        /// Most bytes pulled out of the session into one write. The next batch is
        /// pulled only after this one has been written, so a peer that reads slowly
        /// paces nghttp2 (and the body providers behind it) instead of letting whole
        /// response bodies pile up in memory.
        inline constexpr std::size_t kFlushChunkBytes = 64 * 1024;

        /**
         * @brief Move the session's output onto the wire in bounded chunks: pull up to
         *        @ref kFlushChunkBytes out of nghttp2 under @p state->sessionMu, write
         *        it with no lock held, and only then pull again. One coroutine at a time
         *        is the writer (@c writing); a caller that finds one active just returns,
         *        the writer will pick up whatever it submitted before looking again.
         */
        template<typename S>
        YarnBall::Task<void> flushSession(std::shared_ptr<S> state) {
            std::vector<std::uint8_t> chunk;
            bool first = true;
            while (true) {
                {
                    std::lock_guard<std::mutex> lk(state->sessionMu);
                    if (first && state->writing) co_return; // active writer will flush
                    chunk.clear();
                    while (chunk.size() < kFlushChunkBytes) {
                        const std::uint8_t *data = nullptr;
                        ::ssize_t n = ::nghttp2_session_mem_send(state->session, &data);
                        if (n <= 0) break;
                        chunk.insert(chunk.end(), data, data + n);
                    }
                    if (chunk.empty()) {
                        state->writing = false;
                        co_return;
                    }
                    state->writing = true;
                    first = false;
                }
                // When this write began: a timeout that finds one stuck for its whole
                // length shuts the connection down (see requestDeadline).
                state->writeStartedNs.store(steadyNowNs(), std::memory_order_release);
                // The server bounds each chunk with its write deadline (see
                // Http2ServerConnectionState::beginWrite); the client has its own
                // per-request deadlines instead.
                try {
                    if constexpr (requires { state->beginWrite(); }) state->beginWrite();
                    co_await state->pipe->writeAll(std::span<const std::byte>(
                        reinterpret_cast<const std::byte *>(chunk.data()),
                        chunk.size()));
                } catch (...) {
                    state->writeStartedNs.store(0, std::memory_order_release);
                    if constexpr (requires { state->endWrite(false); }) state->endWrite(false);
                    std::lock_guard<std::mutex> lk(state->sessionMu);
                    state->writing = false;
                    throw;
                }
                state->writeStartedNs.store(0, std::memory_order_release);
                if constexpr (requires { state->endWrite(true); }) state->endWrite(true);
            }
        }

        YarnBall::Task<void> drainAndWrite(std::shared_ptr<Http2Connection::State> state) {
            co_await flushSession(std::move(state));
            co_return;
        }

        /**
         * @brief Fail every request still waiting on this connection: mark its stream
         *        done with a "connection lost" error and wake its awaiter. Safe to call
         *        from any thread, any number of times (close(), the driver's exit,
         *        a failed write); a stream is failed once. A request registered after
         *        this ran sees @c closed set and refuses itself, see Http2Connection::request.
         */
        void failOutstandingStreams(Http2Connection::State &state) noexcept;

        /// Fail one stream that has not finished and wake its awaiter; no-op otherwise.
        void failStream(Http2Connection::State &state, std::int32_t id, bool timedOut) noexcept {
            std::coroutine_handle<> waiter{};
            {
                std::lock_guard<std::mutex> lk(state.mapMu);
                auto it = state.streams.find(id);
                if (it == state.streams.end() || it->second->done.load(std::memory_order_acquire)) return;
                it->second->errorCode = NGHTTP2_CANCEL;
                it->second->timedOut = timedOut;
                it->second->connectionLost = !timedOut;
                it->second->done.store(true, std::memory_order_release);
                waiter = it->second->waiter;
                it->second->waiter = {};
            }
            if (waiter) resumeOnYarn(waiter);
        }

        void failOutstandingStreams(Http2Connection::State &state) noexcept {
            std::vector<std::coroutine_handle<>> orphans;
            {
                std::lock_guard<std::mutex> lk(state.mapMu);
                for (auto &kv : state.streams) {
                    if (!kv.second->done.load(std::memory_order_acquire)) {
                        kv.second->errorCode = NGHTTP2_INTERNAL_ERROR;
                        kv.second->connectionLost = true;
                        kv.second->done.store(true, std::memory_order_release);
                        if (kv.second->waiter) {
                            orphans.push_back(kv.second->waiter);
                            kv.second->waiter = {};
                        }
                    }
                }
            }
            for (auto h : orphans) resumeOnYarn(h);
        }

        /**
         * @brief Declare the connection unusable: refuse new requests, wake the
         *        reader parked on the socket (shutdown makes its read return), and
         *        fail the requests in flight. Does not close the fd -- that happens
         *        when the last reference to the state goes away.
         */
        void abortConnection(Http2Connection::State &state) noexcept {
            state.closed.store(true, std::memory_order_release);
            state.pipe->shutdown();
            failOutstandingStreams(state);
        }

        /**
         * @brief Best-effort GOAWAY drain + socket shutdown, run detached
         *        via @c coSpawn instead of blocking the caller. Spelled
         *        as a free (non-lambda) coroutine, not an
         *        immediately-invoked lambda-coroutine, for the same
         *        reason @c runServerConnection is below: a lambda's
         *        captures are not guaranteed to outlive the coroutine
         *        frame the way a by-value parameter to a named
         *        coroutine function does. Owns @p state by value
         *        (shared_ptr), so it stays valid even if the
         *        @c Http2Connection that triggered the close has
         *        already been destroyed by the time this runs.
         *
         *        The socket is shut down, not closed: the driver coroutine may be
         *        parked reading it, and closing an fd under a parked wait neither
         *        wakes it reliably nor keeps the number from being reused. The
         *        shutdown wakes the driver, which then exits; the fd is closed with
         *        the state.
         */
        YarnBall::Task<void> closeConnectionAsync(
            std::shared_ptr<Http2Connection::State> state) {
            try {
                co_await drainAndWrite(state);
            } catch (...) {
                // Peer already gone; nothing left to flush. Fall
                // through to shut the socket down regardless.
            }
            state->pipe->shutdown();
            co_return;
        }

        /**
         * @brief Long-lived reader: pull bytes off the wire, feed
         *        nghttp2 (callbacks fire synchronously inside the
         *        feed call), then drain whatever nghttp2 produced in
         *        response back onto the wire. Exits on EOF, peer
         *        GOAWAY, a transport error, or the connection being closed
         *        locally (close() shuts the socket down to wake it).
         */
        YarnBall::Task<void> driver(std::shared_ptr<Http2Connection::State> state) {
            std::array<std::byte, 8192> buf{};
            try {
                while (!state->closed.load(std::memory_order_acquire)) {
                    std::size_t n;
                    try {
                        n = co_await state->pipe->readBytes(buf);
                    } catch (const SocketException &) {
                        break;
                    }
                    if (n == 0) break;
                    bool fatal = false;
                    {
                        std::lock_guard<std::mutex> lk(state->sessionMu);
                        const ::ssize_t fed = ::nghttp2_session_mem_recv(
                            state->session,
                            reinterpret_cast<const std::uint8_t *>(buf.data()),
                            n);
                        fatal = fed < 0;
                    }
                    if (fatal) {
                        // Flush the GOAWAY nghttp2 queued for the protocol error.
                        try { co_await drainAndWrite(state); } catch (...) {}
                        break;
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
            } catch (...) {
                // A failed write (or anything else unexpected) ends the connection;
                // the waiters are failed below rather than left parked forever.
            }
            state->closed.store(true, std::memory_order_release);
            failOutstandingStreams(*state);
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
            ::nghttp2_session_callbacks_set_on_begin_headers_callback(cb, &onBeginHeaders);
            ::nghttp2_session_callbacks_set_on_header_callback(cb, &onHeader);
            ::nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, &onDataChunk);
            ::nghttp2_session_callbacks_set_on_frame_recv_callback(cb, &onFrameRecv);
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
                // The stream object stays in the map until the session has closed it
                // too (see Http2Stream): nghttp2 may still pull the request body from it.
                struct Taken {
                    Http2Response response;
                    bool bodyRejected, headersRejected, connectionLost, timedOut;
                    bool finalSeen, endStreamSeen;
                    int errorCode;
                } st;
                {
                    std::lock_guard<std::mutex> lk(state->mapMu);
                    auto it = state->streams.find(streamId);
                    if (it == state->streams.end()) {
                        throw std::runtime_error("HTTP/2 stream vanished");
                    }
                    detail::Http2Stream &src = *it->second;
                    st = Taken{std::move(src.response), src.bodyRejected, src.headersRejected,
                               src.connectionLost, src.timedOut, src.finalSeen, src.endStreamSeen,
                               src.errorCode};
                    src.consumed = true;
                    if (src.sessionClosed) state->streams.erase(it);
                }
                // Checked ahead of errorCode and unconditionally, not
                // inferred from whatever nghttp2 reports as the
                // stream's close reason: we know authoritatively (we
                // set the latch ourselves in onDataChunk) that the
                // response body was truncated and must not be handed
                // to the caller as if it were complete.
                if (st.bodyRejected) {
                    throw std::runtime_error(
                        "HTTP/2 response body exceeded the per-stream cap; "
                        "stream was reset");
                }
                if (st.headersRejected) {
                    throw std::runtime_error(
                        "HTTP/2 response headers exceeded the cap; stream was reset");
                }
                if (st.timedOut) {
                    throw SocketException("HTTP/2 request timed out");
                }
                if (st.connectionLost) {
                    throw std::runtime_error(
                        "HTTP/2 connection closed before the response completed");
                }
                if (st.errorCode != 0) {
                    throw std::runtime_error("HTTP/2 stream error: nghttp2 code " +
                                              std::to_string(st.errorCode));
                }
                // A stream that closed without error but never delivered a complete
                // response (RST_STREAM(NO_ERROR) before :status, or part-way through the
                // body) is not a success: nothing here may be passed off as one.
                if (!st.finalSeen) {
                    throw std::runtime_error(
                        "HTTP/2 stream closed before a response was received");
                }
                if (!st.endStreamSeen) {
                    throw std::runtime_error(
                        "HTTP/2 stream closed before the response completed");
                }
                return std::move(st.response);
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
            const OptionPtr opt = makeSessionOptions();
            if (::nghttp2_session_client_new2(&state->session, cb,
                                               state.get(), opt.get()) != 0) {
                ::nghttp2_session_callbacks_del(cb);
                throw std::runtime_error("nghttp2_session_client_new failed");
            }
            ::nghttp2_session_callbacks_del(cb);

            // No server push (we never look at pushed streams, so refuse them up
            // front), and a bound on the header list the server may send back.
            ::nghttp2_settings_entry iv[3] = {
                {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
                {NGHTTP2_SETTINGS_ENABLE_PUSH, 0},
                {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE,
                 static_cast<std::uint32_t>(kHttp2MaxHeaderBytes)},
            };
            {
                std::lock_guard<std::mutex> lk(state->sessionMu);
                ::nghttp2_submit_settings(state->session,
                                           NGHTTP2_FLAG_NONE, iv, 3);
            }
            co_await drainAndWrite(state);

            YarnBall::coSpawn(driver(state));
            co_return state;
        }
    }

    YarnBall::Task<Http2Connection> Http2Connection::connectPlain(
        std::string host, std::uint16_t port, std::chrono::milliseconds connectTimeout) {
        TcpStream tcp = co_await tcpConnect(host, port, connectTimeout);
        std::string authority = detail::hostHeaderValue(host, port, 0);
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
        if (tls.alpnSelected() != "h2") {
            throw std::runtime_error("Http2Connection::connect: server did not negotiate h2 via ALPN");
        }
        std::string authority = detail::hostHeaderValue(host, port, 0);
        auto pipe = std::make_unique<TlsPipe>(std::move(tls));
        auto state = co_await finishHandshake(
            std::move(pipe), std::move(authority), "https");
        Http2Connection conn;
        conn.state = std::move(state);
        co_return conn;
    }
#endif

    namespace {
        /**
         * @brief Per-request deadline. Sleeps for @p limit, then resets the stream (CANCEL)
         *        and fails it if it is still waiting. Holds the connection state weakly,
         *        so a finished request does not keep the socket open until the timer fires.
         *        @p cancel is stopped when the request ends: the timer is then torn
         *        down at once instead of sleeping (and pinning its frame) for the full
         *        limit, which at a high request rate piled up without bound.
         */
        YarnBall::Task<void> deadlineSleep(std::chrono::milliseconds limit) {
            co_await YarnBall::sleepFor(limit);
            co_return;
        }

        YarnBall::Task<void> requestDeadline(std::weak_ptr<Http2Connection::State> weak,
                                             std::int32_t id,
                                             std::chrono::milliseconds limit,
                                             std::stop_token cancel) {
            try {
                co_await YarnBall::withCancellation(cancel, deadlineSleep(limit));
            } catch (const YarnBall::CancelledException &) {
                co_return;
            }
            auto state = weak.lock();
            if (!state) co_return;
            {
                std::lock_guard<std::mutex> lk(state->mapMu);
                auto it = state->streams.find(id);
                if (it == state->streams.end() || it->second->done.load(std::memory_order_acquire)) {
                    co_return;
                }
            }
            {
                std::lock_guard<std::mutex> lk(state->sessionMu);
                ::nghttp2_submit_rst_stream(state->session, NGHTTP2_FLAG_NONE, id, NGHTTP2_CANCEL);
            }
            failStream(*state, id, /*timedOut=*/true);
            // The request that submitted this stream may itself be the connection's
            // writer, parked in a write to a peer that never reads: nothing wakes it
            // but a shutdown. A write that has been going on for most of the timeout
            // is such a one (a healthy chunk takes far less), so give the connection up.
            const std::int64_t wrote = state->writeStartedNs.load(std::memory_order_acquire);
            if (wrote != 0 &&
                steadyNowNs() - wrote >= std::chrono::duration_cast<std::chrono::nanoseconds>(limit).count() / 2) {
                abortConnection(*state);
                co_return;
            }
            try {
                co_await drainAndWrite(state);
            } catch (...) {
                abortConnection(*state);
            }
            co_return;
        }
    }

    YarnBall::Task<Http2Response> Http2Connection::request(
        std::string method,
        std::string path,
        std::vector<HttpHeader> headers,
        std::string body,
        std::chrono::milliseconds timeout) {
        if (!this->state || this->state->closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("Http2Connection::request: connection closed");
        }
        auto state = this->state; // shared_ptr copy for the awaiter

        // Build HPACK header list. nghttp2 takes a flat vector of
        // (name, value, name_len, value_len, flags).
        //
        // The entries are submitted with NGHTTP2_NV_FLAG_NONE, so nghttp2 copies every
        // name and value into the frame at submit time. They must not be passed no-copy:
        // the frame can sit queued in the session (behind the peer's concurrent-stream
        // limit, or until the writer gets to it) long after this coroutine, and with it
        // `method`, `path` and the lowercased names, is gone.
        std::vector<::nghttp2_nv> nv;
        nv.reserve(4 + headers.size());

        auto add = [&nv](std::string_view k, std::string_view v) {
            nv.push_back(::nghttp2_nv{
                const_cast<std::uint8_t *>(reinterpret_cast<const std::uint8_t *>(k.data())),
                const_cast<std::uint8_t *>(reinterpret_cast<const std::uint8_t *>(v.data())),
                k.size(), v.size(),
                NGHTTP2_NV_FLAG_NONE,
            });
        };

        // The same refusals as the HTTP/1 client: a method or header name that is not a
        // token, a control character in a header value or the target, a caller-framed body.
        if (!detail::isHttpToken(method) || path.empty() || detail::hasTargetCtl(path)) {
            throw std::runtime_error("Http2Connection::request: invalid method or target");
        }
        const std::string *authority = &state->authority;
        std::vector<std::string> lowerNames;
        lowerNames.reserve(headers.size());
        std::vector<std::size_t> kept; // indices into `headers` that go on the wire
        for (std::size_t i = 0; i < headers.size(); ++i) {
            const auto &h = headers[i];
            if (!detail::isHttpToken(h.name) || detail::hasFieldValueCtl(h.value) ||
                detail::iequalsAscii(h.name, "Transfer-Encoding")) {
                throw std::runtime_error("Http2Connection::request: invalid header in request");
            }
            // Connection-specific fields have no meaning in HTTP/2 and a peer resets the
            // stream over them (RFC 9113 8.2.2); a caller's HTTP/1-style copy is dropped.
            // The length is always the real one (as in the HTTP/1 client), and a caller's
            // Host replaces :authority, which stands in for it.
            if (detail::iequalsAscii(h.name, "Connection") || detail::iequalsAscii(h.name, "Keep-Alive") ||
                detail::iequalsAscii(h.name, "Proxy-Connection") || detail::iequalsAscii(h.name, "Upgrade") ||
                detail::iequalsAscii(h.name, "Content-Length")) {
                continue;
            }
            if (detail::iequalsAscii(h.name, "TE")) {
                if (!detail::iequalsAscii(detail::trimAscii(h.value), "trailers")) continue;
            } else if (detail::iequalsAscii(h.name, "Host")) {
                if (h.value.empty()) throw std::runtime_error("Http2Connection::request: invalid header in request");
                authority = &h.value;
                continue;
            }
            kept.push_back(i);
        }
        add(":method", method);
        add(":scheme", state->scheme);
        add(":authority", *authority);
        add(":path", path);
        for (const std::size_t i : kept) {
            lowerNames.push_back(lowerAscii(headers[i].name));
            add(lowerNames.back(), headers[i].value);
        }
        const bool expectsBody = detail::iequalsAscii(method, "POST") ||
                                 detail::iequalsAscii(method, "PUT") ||
                                 detail::iequalsAscii(method, "PATCH");
        const std::string contentLength = std::to_string(body.size());
        if (!body.empty() || expectsBody) add("content-length", contentLength);

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
            // The stream is registered in the map while sessionMu is
            // still held (callbacks run under sessionMu, so none can
            // observe the stream id before it is in the map). The closed
            // flag is re-checked after registration: if the driver has
            // already swept orphans, nobody would ever resume this
            // request.
            std::lock_guard<std::mutex> lk(state->sessionMu);
            detail::Http2Stream *raw = streamState.get();
            streamId = ::nghttp2_submit_request(
                state->session, nullptr,
                nv.data(), nv.size(),
                providerPtr, raw);
            if (streamId < 0) {
                throw std::runtime_error(std::string("nghttp2_submit_request: ") +
                                          ::nghttp2_strerror(streamId));
            }
            std::lock_guard<std::mutex> mk(state->mapMu);
            state->streams.emplace(streamId, std::move(streamState));
            if (state->closed.load(std::memory_order_acquire)) {
                // The session has accepted the request, and nghttp2 keeps pointers into the
                // stream object (the body provider's source): it must not be freed here.
                // Cancel the stream and leave the object in the map; it is released when
                // the session closes the stream or the connection state is destroyed.
                raw->consumed = true;
                raw->done.store(true, std::memory_order_release);
                ::nghttp2_submit_rst_stream(state->session, NGHTTP2_FLAG_NONE,
                                             streamId, NGHTTP2_CANCEL);
                throw std::runtime_error("Http2Connection::request: connection closed");
            }
        }

        // Stops the deadline timer however this coroutine ends (response, error,
        // or the frame being destroyed).
        struct StopOnExit {
            std::stop_source src;
            ~StopOnExit() { src.request_stop(); }
        } deadlineStop;
        if (timeout.count() > 0) {
            YarnBall::coSpawn(requestDeadline(state, streamId, timeout, deadlineStop.src.get_token()));
        }

        // Push the freshly enqueued frames onto the wire so the peer
        // sees the request without waiting for any inbound activity.
        try {
            co_await drainAndWrite(state);
        } catch (...) {
            // The transport failed: every stream on this connection is lost, this
            // one included. (The stream stays in the map until the state dies; nghttp2
            // still holds pointers into it.)
            abortConnection(*state);
            throw;
        }

        // Suspend until the stream completes.
        co_return co_await StreamAwaiter{state, streamId};
    }

    bool Http2Connection::isClosed() const noexcept {
        if (!this->state || this->state->closed.load(std::memory_order_acquire)) return true;
        // A peer GOAWAY (or an exhausted stream-id space) leaves in-flight streams
        // running but refuses new ones: for the pool that is as good as closed.
        std::lock_guard<std::mutex> lk(this->state->sessionMu);
        return ::nghttp2_session_check_request_allowed(this->state->session) == 0;
    }

    void Http2Connection::close() noexcept {
        if (!this->state) return;
        // exchange, not load-then-store: makes the "close exactly
        // once" guard atomic against a concurrent close() (e.g. one
        // thread's explicit close() racing another's ~Http2Connection
        // teardown of a moved-from copy) and against the driver
        // observing EOF at the same time.
        if (this->state->closed.exchange(true, std::memory_order_acq_rel)) return;
        {
            std::lock_guard<std::mutex> lk(this->state->sessionMu);
            ::nghttp2_submit_goaway(this->state->session, NGHTTP2_FLAG_NONE,
                                     0, NGHTTP2_NO_ERROR, nullptr, 0);
        }
        // Requests still waiting are failed now: they would otherwise wait on
        // the driver, which is parked reading a socket the peer may never write to.
        failOutstandingStreams(*this->state);
        // Drain the GOAWAY and shut the socket down (which also wakes the
        // parked driver) on the Yarn pool rather
        // than blocking here with syncWait. close() runs from
        // ~Http2Connection(), which fires wherever the object goes out
        // of scope -- including from inside a coroutine running on a
        // Yarn worker, where syncWait would tie up that worker for the
        // full round trip; syncWait's own contract explicitly forbids
        // calling it from a Yarn worker. The spawned task holds
        // shared_ptr<State> by value, so it stays valid regardless of
        // this Http2Connection's own lifetime.
        YarnBall::coSpawn(closeConnectionAsync(this->state));
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
            /// A handler was dispatched for this stream and has not yet answered or ended
            /// (guarded by streamsMu). While it runs, the stream keeps counting against
            /// the concurrent-stream cap even after the client resets it.
            bool handlerActive{false};
            /**
             * @brief Latched once the accumulated request body exceeds
             *        the per-stream cap (see kHttp2MaxBodyBytes). Once
             *        set, serverOnDataChunk stops touching req.body,
             *        and the driver's dispatch walk skips this stream
             *        instead of handing a truncated request body to a
             *        route handler. See onDataChunk's docblock for why
             *        this must be a one-way latch rather than a
             *        clear-and-keep-counting reset.
             */
            bool bodyRejected{false};
            /// Latched when the request headers exceed the configured caps. Written under
            /// the connection's streamsMu: updateDeadline reads it from other threads.
            bool rejected{false};
            /// A Host header (real or synthesised from :authority) is already in req.headers.
            bool hasHost{false};
            /// Index in req.headers of the joined cookie header, or -1.
            int cookieIndex{-1};
            std::size_t headerBytes{0};
            /// Body bytes accounted to the connection-wide budget for this stream.
            std::size_t bodyBytes{0};
            /// Fires when the stream closes (reset by the client, or the connection
            /// ends); the request handed to the handler carries its token.
            std::stop_source cancel;
            std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
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
        bool writing{false};
        /// steady_clock time (ns) the write in progress began, 0 when none.
        std::atomic<std::int64_t> writeStartedNs{0};

        std::mutex streamsMu;
        std::unordered_map<std::int32_t, std::unique_ptr<ServerStream>> streams;
        /// Request-body bytes held across all streams (guarded by streamsMu).
        std::size_t bodyBytesInFlight{0};
        /// Handlers still running for streams that have since closed (reset by the client);
        /// they hold their share of the concurrent-stream cap until they end (guarded by streamsMu).
        std::size_t orphanHandlers{0};

        /// Snapshot of the owning Http2Server's route table, taken when serve()
        /// started. Immutable, shared by every connection; no mutex needed.
        std::shared_ptr<const std::unordered_map<std::string, HttpRouteHandler>> routes;

        Http2ServerOptions opts;

        std::atomic<bool> closed{false};
        /// Set from a callback when the peer misbehaves (reset flood, body budget
        /// blown); the driver closes the connection after flushing the GOAWAY.
        std::atomic<bool> abusive{false};
        /// Handlers dispatched and not yet finished.
        std::atomic<int> activeHandlers{0};

        /// Reset-rate window (touched only from callbacks, i.e. under sessionMu).
        std::chrono::steady_clock::time_point resetWindowStart{std::chrono::steady_clock::now()};
        std::size_t resetsInWindow{0};

        /// Window of the frames that make no progress (PING, SETTINGS, PRIORITY, and
        /// WINDOW_UPDATE with no stream open), likewise touched under sessionMu.
        std::chrono::steady_clock::time_point controlWindowStart{std::chrono::steady_clock::now()};
        std::size_t controlInWindow{0};

        /// steady_clock time (ns) of the last real progress: a request begun or
        /// finished, a response sent (the connection's start counts). The idle timeout
        /// runs from here, so frames that carry no work (PING, SETTINGS, ...) cannot
        /// keep an idle connection alive by resetting it.
        std::atomic<std::int64_t> lastProgressNs{steadyNowNs()};
        /// Response body bytes pulled into the output and not yet credited as progress
        /// (set by the body read callback, consumed by endWrite).
        std::atomic<std::size_t> bodyPulledBytes{0};
        /// Pulled body bytes that must add up before they count as progress: a peer
        /// that opens its window a byte at a time does not keep the connection alive.
        static constexpr std::size_t kMinProgressBytes = 16 * 1024;

        /// Deadline against stalled peers, re-armed as the connection's state
        /// changes (see updateDeadline). Declared after `pipe` so it is destroyed
        /// -- disarmed -- before the socket closes.
        std::mutex deadlineMu;
        IoDeadline deadline;
        /// A chunk is being written and `deadline` is the write deadline (guarded by
        /// deadlineMu); updateDeadline leaves it alone until the write ends.
        bool writeArmed{false};

        Http2ServerConnectionState(std::unique_ptr<Http2Pipe> p,
                                       std::shared_ptr<const std::unordered_map<std::string, HttpRouteHandler>> r,
                                       const Http2ServerOptions &o) noexcept
            : pipe(std::move(p)), routes(std::move(r)), opts(o) {}

        /**
         * @brief Choose the deadline that fits what the connection is doing:
         *        a request still being received -> requestTimeout (the peer owes
         *        us bytes); nothing in flight -> idleTimeout; handlers running and
         *        nothing owed -> none (a slow handler is not the peer's fault).
         *        Called before each read and when the last handler finishes.
         */
        void updateDeadline() {
            std::lock_guard<std::mutex> dl(this->deadlineMu);
            this->updateDeadlineLocked();
        }

        /**
         * @brief A chunk of output is about to be written: the peer must take it within
         *        writeTimeout. Without this a peer that requests a large response and
         *        never reads it (the write parks on a full socket buffer, and the
         *        read-side deadlines are off while a handler runs) would hold the
         *        connection, its descriptor and its slot for ever. Replaces the read-side
         *        deadline for the duration of the write.
         */
        void beginWrite() {
            std::lock_guard<std::mutex> dl(this->deadlineMu);
            if (this->closed.load(std::memory_order_acquire)) return;
            this->writeArmed = true;
            this->deadline.arm(this->pipe->fd(), this->opts.writeTimeout);
        }

        /// The chunk was written (@p progressed) or the write failed: count the progress
        /// towards the idle clock and go back to the deadline the connection's state calls for.
        void endWrite(bool progressed) {
            // Only a chunk that carried response body counts as progress: PING and SETTINGS
            // replies are written too, and must not keep an idle connection (or one whose
            // peer has stopped granting window) alive.
            if (progressed &&
                this->bodyPulledBytes.load(std::memory_order_acquire) >= kMinProgressBytes) {
                this->bodyPulledBytes.store(0, std::memory_order_release);
                this->lastProgressNs.store(steadyNowNs(), std::memory_order_release);
            }
            std::lock_guard<std::mutex> dl(this->deadlineMu);
            this->writeArmed = false;
            try {
                this->updateDeadlineLocked();
            } catch (...) {
                // Could not re-arm (the sweeper failed to start): run without a deadline
                // rather than fail a write that went through.
                this->deadline.disarm();
            }
        }

        void updateDeadlineLocked() {
            if (this->closed.load(std::memory_order_acquire)) {
                this->deadline.disarm();
                return;
            }
            if (this->writeArmed) return; // the write deadline stands until the chunk is out
            bool incomplete = false;
            {
                std::lock_guard<std::mutex> lk(this->streamsMu);
                for (const auto &kv : this->streams) {
                    if (!kv.second->handlerStarted && !kv.second->rejected && !kv.second->bodyRejected) {
                        incomplete = true;
                        break;
                    }
                }
            }
            const int fd = this->pipe->fd();
            if (incomplete) {
                this->deadline.arm(fd, this->opts.requestTimeout);
            } else if (this->activeHandlers.load(std::memory_order_acquire) == 0) {
                // Armed for what is left of the idle period, not a fresh one: this runs
                // before every read, and a fresh one would let any frame (a PING every
                // few seconds) hold the connection open indefinitely.
                std::chrono::milliseconds left = this->opts.idleTimeout;
                if (left.count() > 0) {
                    const auto idle = std::chrono::nanoseconds{
                        steadyNowNs() - this->lastProgressNs.load(std::memory_order_acquire)};
                    left = std::max(std::chrono::milliseconds{1},
                                    left - std::chrono::duration_cast<std::chrono::milliseconds>(idle));
                }
                this->deadline.arm(fd, left);
            } else {
                this->deadline.disarm();
            }
        }

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
            k.append(method); // methods are case-sensitive (RFC 9110 9.1)
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
            co_await flushSession(std::move(state));
            co_return;
        }

        /// True if @p resp can be sent: a final status, header and trailer names that are
        /// tokens (so no pseudo-header, no empty name) and values free of control characters.
        bool validResponse(const HttpResponse &resp) {
            if (resp.status < 200 || resp.status > 599) return false;
            for (const auto &h : resp.headers) {
                if (!detail::isHttpToken(h.name) || detail::hasFieldValueCtl(h.value)) return false;
            }
            for (const auto &h : resp.trailers) {
                if (!detail::isHttpToken(h.name) || detail::hasFieldValueCtl(h.value)) return false;
            }
            return true;
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
                                   void *user_data) noexcept try {
            auto *st = static_cast<ServerStream *>(source->ptr);
            const std::size_t remaining = st->respBody.size() - st->respBodyOffset;
            const std::size_t toCopy = std::min(remaining, length);
            std::memcpy(buf, st->respBody.data() + st->respBodyOffset, toCopy);
            st->respBodyOffset += toCopy;
            if (toCopy != 0) {
                // The peer's window let body out; endWrite credits it as progress once
                // enough has moved.
                auto *state = static_cast<Http2ServerConnectionState *>(user_data);
                state->bodyPulledBytes.fetch_add(toCopy, std::memory_order_acq_rel);
            }
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
                        // The stream cannot be finished without its trailers: have nghttp2
                        // reset it (INTERNAL_ERROR) rather than leave it half-open.
                        YarnBall::log::error("Http2Server: nghttp2_submit_trailer failed",
                                             {YarnBall::log::str("what", ::nghttp2_strerror(rc))});
                        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
                    }
                }
            }
            return static_cast<::ssize_t>(toCopy);
        } catch (...) {
            // Out of memory building the trailers: reset this stream, keep the connection.
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }

        /**
         * @brief Submit the response on @p streamId after the handler
         *        produced @p resp. Builds the header block and (when
         *        body is non-empty) installs the data provider.
         */
        void submitResponseLocked(Http2ServerConnectionState *state,
                                    std::int32_t streamId,
                                    ServerStream *st,
                                    const HttpResponse &resp,
                                    bool headRequest) {
            st->respStatusStr = std::to_string(resp.status);
            // HEAD, 1xx, 204 and 304 carry no DATA (nor trailers); a conformant client
            // treats content after them as a protocol error.
            bool sendBody = true;
            std::string statedLength;
            for (const auto &h : resp.headers) {
                if (detail::iequalsAscii(h.name, "Content-Length")) {
                    statedLength = h.value;
                    break;
                }
            }
            const std::string length = detail::framedContentLength(
                resp.status, headRequest, statedLength, resp.body.size(), sendBody);
            st->respBody = sendBody ? resp.body : std::string();
            st->respBodyOffset = 0;
            st->respHeaders.clear();
            st->respTrailers.clear();
            // Copy user headers; nghttp2 needs stable pointers, so we
            // store the strings in the stream-state vector and feed
            // nghttp2_nv pointers into it. Length and connection-management
            // fields are the server's to write: a handler's copy is dropped.
            for (const auto &h : resp.headers) {
                if (detail::isServerManagedResponseHeader(h.name)) continue;
                st->respHeaders.emplace_back(lowerAscii(h.name), h.value);
            }
            if (sendBody) {
                for (const auto &h : resp.trailers) {
                    st->respTrailers.emplace_back(lowerAscii(h.name), h.value);
                }
            }
            // Emit content-length so the peer can validate framing without
            // relying on END_STREAM semantics.
            if (!length.empty()) st->respHeaders.emplace_back("content-length", length);

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
            // A trailers-only response (no body, trailers) needs the provider too: the
            // trailers are submitted from its end-of-data call, and without it the
            // response would end at the headers.
            if (!st->respBody.empty() || !st->respTrailers.empty()) {
                provider.source.ptr = st;
                provider.read_callback = &serverBodyRead;
                providerPtr = &provider;
            }

            const int rc = ::nghttp2_submit_response(state->session, streamId,
                                       nv.data(), nv.size(), providerPtr);
            if (rc != 0) {
                // Invalid response (e.g. a header nghttp2 rejects): reset
                // the stream rather than leaving the client waiting.
                ::nghttp2_submit_rst_stream(state->session, NGHTTP2_FLAG_NONE,
                                             streamId, NGHTTP2_INTERNAL_ERROR);
            }
            // Trailers are submitted inside the data provider's EOF
            // call so nghttp2's frame queue serialises them correctly
            // after the last DATA frame.
            // The driver reads this flag under streamsMu (sessionMu -> streamsMu is
            // the established order), so the write takes it too.
            {
                std::lock_guard<std::mutex> sk(state->streamsMu);
                st->responseSubmitted = true;
            }
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
            // Counted from dispatch (the driver incremented it) to here, so the
            // connection is not considered idle while a handler is working.
            struct Finished {
                Http2ServerConnectionState *st;
                std::int32_t streamId;
                /// The handler gave up its share of the stream cap itself (see below).
                bool released{false};
                ~Finished() {
                    if (!released) {
                        std::lock_guard<std::mutex> sk(st->streamsMu);
                        auto it = st->streams.find(streamId);
                        if (it != st->streams.end()) {
                            it->second->handlerActive = false;
                        } else if (st->orphanHandlers != 0) {
                            --st->orphanHandlers;
                        }
                    }
                    st->lastProgressNs.store(steadyNowNs(), std::memory_order_release);
                    if (st->activeHandlers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        st->updateDeadline();
                    }
                }
            } finished{state.get(), streamId};

            // The client may have reset the stream (or the connection may have died)
            // between dispatch and now: there is nobody to answer, so do not run the handler.
            if (state->closed.load(std::memory_order_acquire) || req.cancel.stop_requested()) {
                co_return;
            }
            {
                std::lock_guard<std::mutex> sk(state->streamsMu);
                if (state->streams.find(streamId) == state->streams.end()) co_return;
            }

            HttpResponse resp;
            const bool headRequest = req.method == "HEAD";
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
                // The peer gets a fixed body; what the exception said stays in the log.
                YarnBall::log::error("Http2Server: route handler threw",
                                     {YarnBall::log::str("what", e.what())});
                resp = HttpResponse{};
                resp.status = 500;
                resp.body = "internal server error";
            } catch (...) {
                YarnBall::log::error("Http2Server: route handler threw a non-standard exception");
                resp = HttpResponse{};
                resp.status = 500;
                resp.body = "internal server error";
            }
            if (state->closed.load(std::memory_order_acquire)) co_return;
            // Same rule as HttpServer: a response whose status, header names or values
            // cannot go on the wire is answered with a fixed 500.
            if (!validResponse(resp)) {
                YarnBall::log::error("Http2Server: route handler returned an invalid response");
                resp = HttpResponse{};
                resp.status = 500;
                resp.body = "internal server error";
            }
            try {
                {
                    // Look the stream up and use it under sessionMu: stream
                    // close callbacks run under sessionMu and free the
                    // ServerStream, so a pointer fetched earlier could dangle
                    // by the time the response is submitted.
                    std::lock_guard<std::mutex> lk(state->sessionMu);
                    ServerStream *st = nullptr;
                    {
                        std::lock_guard<std::mutex> sk(state->streamsMu);
                        auto it = state->streams.find(streamId);
                        if (it == state->streams.end()) co_return; // reset while the handler ran
                        st = it->second.get();
                        st->handlerActive = false;
                        finished.released = true;
                    }
                    submitResponseLocked(state.get(), streamId, st, resp, headRequest);
                }
                co_await serverDrainAndWrite(state);
            } catch (...) {
                // The transport is gone; the driver notices on its own read and
                // winds the connection down. Nothing here may escape a detached task.
            }
            co_return;
        }

        // ---- nghttp2 server callbacks -------------------------------

        int serverOnBeginHeaders(::nghttp2_session *session,
                                   const ::nghttp2_frame *frame,
                                   void *user_data) noexcept try {
            if (frame->hd.type != NGHTTP2_HEADERS ||
                frame->headers.cat != NGHTTP2_HCAT_REQUEST) return 0;
            auto *state = static_cast<Http2ServerConnectionState *>(user_data);
            std::lock_guard<std::mutex> lk(state->streamsMu);
            // nghttp2 only applies our advertised MAX_CONCURRENT_STREAMS once the
            // peer has acknowledged the SETTINGS, so enforce it here too. A stream
            // that is refused gets no entry; its remaining callbacks find nothing.
            // A stream the client has reset still counts while its handler runs.
            if (state->opts.maxConcurrentStreams != 0 &&
                state->streams.size() + state->orphanHandlers >= state->opts.maxConcurrentStreams) {
                ::nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE,
                                             frame->hd.stream_id, NGHTTP2_REFUSED_STREAM);
                return 0;
            }
            auto st = std::make_unique<ServerStream>();
            st->req.cancel = st->cancel.get_token();
            state->streams.emplace(frame->hd.stream_id, std::move(st));
            state->lastProgressNs.store(steadyNowNs(), std::memory_order_release);
            return 0;
        } catch (...) {
            // Out of memory: a callback may not throw through nghttp2. Failing the
            // session ends the connection (the driver sees the error from mem_recv).
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }

        int serverOnHeader(::nghttp2_session *session,
                             const ::nghttp2_frame *frame,
                             const std::uint8_t *name, std::size_t namelen,
                             const std::uint8_t *value, std::size_t valuelen,
                             std::uint8_t /*flags*/,
                             void *user_data) noexcept try {
            if (frame->hd.type != NGHTTP2_HEADERS) return 0;
            auto *state = static_cast<Http2ServerConnectionState *>(user_data);

            // Request trailers must not be merged into the request headers,
            // where a handler cannot tell them from real headers.
            if (frame->headers.cat != NGHTTP2_HCAT_REQUEST) return 0;

            ServerStream *st = nullptr;
            {
                std::lock_guard<std::mutex> lk(state->streamsMu);
                auto it = state->streams.find(frame->hd.stream_id);
                if (it != state->streams.end()) st = it->second.get();
            }
            if (!st || st->rejected) return 0;

            // Header-list caps: nghttp2 decodes (and bounds) one field at a time, so a
            // peer could still stream an endless number of fields; stop at the budget.
            st->headerBytes += namelen + valuelen + 32;
            if ((state->opts.maxHeaderListBytes != 0 && st->headerBytes > state->opts.maxHeaderListBytes) ||
                (state->opts.maxHeaderCount != 0 && st->req.headers.size() >= state->opts.maxHeaderCount)) {
                {
                    std::lock_guard<std::mutex> lk(state->streamsMu);
                    st->rejected = true;
                }
                st->req.headers.clear();
                ::nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE,
                                             frame->hd.stream_id, NGHTTP2_ENHANCE_YOUR_CALM);
                return 0;
            }

            std::string_view nm(reinterpret_cast<const char *>(name), namelen);
            std::string_view va(reinterpret_cast<const char *>(value), valuelen);

            if (nm == ":method") {
                st->req.method = std::string(va);
                return 0;
            }
            if (nm == ":path") {
                // Routes match on the path; the query string travels separately.
                detail::splitQuery(va, st->req.path, st->req.query);
                return 0;
            }
            if (nm == ":scheme") {
                st->req.scheme = std::string(va);
                return 0;
            }
            if (nm == ":authority") {
                // Handlers written against HTTP/1 look for Host: synthesise it (RFC 9113
                // 8.3.1: :authority takes the place of Host). Pseudo-headers come first, so
                // this precedes any real host field, which is then ignored.
                st->req.authority = std::string(va);
                if (!st->hasHost) {
                    st->req.headers.push_back({"host", std::string(va)});
                    st->hasHost = true;
                }
                return 0;
            }
            if (!nm.empty() && nm[0] == ':') return 0;
            if (nm == "host") {
                if (st->hasHost) return 0;
                st->hasHost = true;
            } else if (nm == "cookie") {
                // RFC 9113 8.2.3: the cookie crumbs of one request are separate fields;
                // HTTP/1 handlers expect a single header, so join them with "; ".
                if (st->cookieIndex >= 0) {
                    st->req.headers[static_cast<std::size_t>(st->cookieIndex)].value.append("; ").append(va);
                    return 0;
                }
                st->cookieIndex = static_cast<int>(st->req.headers.size());
            }
            st->req.headers.push_back({std::string(nm), std::string(va)});
            return 0;
        } catch (...) {
            return NGHTTP2_ERR_CALLBACK_FAILURE; // out of memory, see serverOnBeginHeaders
        }

        int serverOnDataChunk(::nghttp2_session *session,
                                std::uint8_t /*flags*/,
                                std::int32_t stream_id,
                                const std::uint8_t *data, std::size_t len,
                                void *user_data) noexcept try {
            auto *state = static_cast<Http2ServerConnectionState *>(user_data);
            std::lock_guard<std::mutex> lk(state->streamsMu);
            auto it = state->streams.find(stream_id);
            if (it == state->streams.end()) return 0;
            // Untrusted input: cap accumulated request-body size so a
            // client cannot force unbounded memory growth by streaming
            // an ever-larger body across DATA frames (HTTP/2 flow
            // control only paces delivery; it does not bound the
            // total). Latched, not a plain clear-and-reset: a single
            // nghttp2_submit_rst_stream call does not stop nghttp2
            // from invoking this callback again for DATA frames
            // already parsed out of the same nghttp2_session_mem_recv
            // batch, so clearing without latching would let the cap
            // silently reset itself instead of durably rejecting the
            // stream (see onDataChunk's docblock, same issue on the
            // client side).
            ServerStream &st = *it->second;
            if (st.bodyRejected || st.rejected) return 0;
            const std::size_t maxBody = state->opts.maxBodyBytes;
            const std::size_t maxConn = state->opts.maxConnectionBodyBytes;
            if (maxConn != 0 && state->bodyBytesInFlight + len > maxConn) {
                // Too much buffered across the connection's streams: the peer is
                // either abusive or has far more in flight than we agreed to hold.
                st.bodyRejected = true;
                state->bodyBytesInFlight -= st.bodyBytes;
                st.bodyBytes = 0;
                st.req.body.clear();
                st.req.body.shrink_to_fit();
                state->abusive.store(true, std::memory_order_release);
                ::nghttp2_submit_goaway(session, NGHTTP2_FLAG_NONE,
                                         ::nghttp2_session_get_last_proc_stream_id(session),
                                         NGHTTP2_ENHANCE_YOUR_CALM, nullptr, 0);
                return 0;
            }
            if (maxBody != 0 && st.req.body.size() + len > maxBody) {
                st.bodyRejected = true;
                state->bodyBytesInFlight -= st.bodyBytes;
                st.bodyBytes = 0;
                st.req.body.clear();
                st.req.body.shrink_to_fit();
                ::nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE,
                                             stream_id, NGHTTP2_ENHANCE_YOUR_CALM);
                return 0;
            }
            st.req.body.append(reinterpret_cast<const char *>(data), len);
            st.bodyBytes += len;
            state->bodyBytesInFlight += len;
            return 0;
        } catch (...) {
            return NGHTTP2_ERR_CALLBACK_FAILURE; // out of memory, see serverOnBeginHeaders
        }

        /**
         * @brief Fires on every received frame. We dispatch the
         *        handler when END_STREAM is observed on the HEADERS
         *        (body-less request) or on a DATA frame (body request).
         *        The dispatch is a coSpawn so we don't run the user
         *        coroutine inside the nghttp2 callback stack.
         */
        int serverOnFrameRecv(::nghttp2_session *session,
                                const ::nghttp2_frame *frame,
                                void *user_data) noexcept {
            auto *state = static_cast<Http2ServerConnectionState *>(user_data);

            // Frames that do no work but cost us a reply or a parse: a flood of them is
            // an attack (and, spaced out, a way to hold an idle connection open).
            if (state->opts.maxControlFramesPerSecond != 0) {
                const std::uint8_t type = frame->hd.type;
                bool control = type == NGHTTP2_PRIORITY ||
                    (type == NGHTTP2_PING && (frame->hd.flags & NGHTTP2_FLAG_ACK) == 0) ||
                    (type == NGHTTP2_SETTINGS && (frame->hd.flags & NGHTTP2_FLAG_ACK) == 0);
                if (type == NGHTTP2_WINDOW_UPDATE) {
                    // Per-stream updates are part of normal transfers; one with no stream
                    // open at all is not.
                    std::lock_guard<std::mutex> lk(state->streamsMu);
                    control = state->streams.empty();
                }
                if (control) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - state->controlWindowStart >= std::chrono::seconds{1}) {
                        state->controlWindowStart = now;
                        state->controlInWindow = 0;
                    }
                    if (++state->controlInWindow > state->opts.maxControlFramesPerSecond &&
                        !state->abusive.exchange(true, std::memory_order_acq_rel)) {
                        ::nghttp2_submit_goaway(session, NGHTTP2_FLAG_NONE,
                                                 ::nghttp2_session_get_last_proc_stream_id(session),
                                                 NGHTTP2_ENHANCE_YOUR_CALM, nullptr, 0);
                    }
                    return 0;
                }
            }

            if (frame->hd.type == NGHTTP2_RST_STREAM && state->opts.maxResetsPerSecond != 0) {
                // Rapid reset: a client that opens streams and cancels them as fast as it
                // can makes us start work it then discards. Bound the rate.
                const auto now = std::chrono::steady_clock::now();
                if (now - state->resetWindowStart >= std::chrono::seconds{1}) {
                    state->resetWindowStart = now;
                    state->resetsInWindow = 0;
                }
                if (++state->resetsInWindow > state->opts.maxResetsPerSecond &&
                    !state->abusive.exchange(true, std::memory_order_acq_rel)) {
                    ::nghttp2_submit_goaway(session, NGHTTP2_FLAG_NONE,
                                             ::nghttp2_session_get_last_proc_stream_id(session),
                                             NGHTTP2_ENHANCE_YOUR_CALM, nullptr, 0);
                }
                return 0;
            }

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
            state->lastProgressNs.store(steadyNowNs(), std::memory_order_release);
            return 0;
        }

        int serverOnStreamClose(::nghttp2_session * /*session*/,
                                  std::int32_t stream_id,
                                  std::uint32_t /*error_code*/,
                                  void *user_data) noexcept {
            auto *state = static_cast<Http2ServerConnectionState *>(user_data);
            std::lock_guard<std::mutex> lk(state->streamsMu);
            auto it = state->streams.find(stream_id);
            if (it == state->streams.end()) return 0;
            // The stream is over, whether the response went out or the client reset
            // it: tell a handler still working on it that nobody is waiting, and give
            // its buffered body back to the connection budget.
            it->second->cancel.request_stop();
            state->bodyBytesInFlight -= it->second->bodyBytes;
            if (it->second->handlerActive) ++state->orphanHandlers;
            state->streams.erase(it);
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
            std::array<std::byte, 8192> buf{};
            try {
                // Send initial SETTINGS.
                std::array<::nghttp2_settings_entry, 2> iv{};
                std::size_t ivCount = 0;
                if (state->opts.maxConcurrentStreams != 0) {
                    iv[ivCount++] = {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,
                                     static_cast<std::uint32_t>(state->opts.maxConcurrentStreams)};
                }
                if (state->opts.maxHeaderListBytes != 0) {
                    iv[ivCount++] = {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE,
                                     static_cast<std::uint32_t>(state->opts.maxHeaderListBytes)};
                }
                {
                    std::lock_guard<std::mutex> lk(state->sessionMu);
                    ::nghttp2_submit_settings(state->session,
                                               NGHTTP2_FLAG_NONE, iv.data(), ivCount);
                }
                co_await serverDrainAndWrite(state);

                while (!state->closed.load(std::memory_order_acquire)) {
                    // Whatever the connection is doing decides how long a silent peer is tolerated.
                    state->updateDeadline();
                    std::size_t n;
                    try {
                        n = co_await state->pipe->readBytes(buf);
                    } catch (const SocketException &) {
                        break;
                    }
                    if (n == 0) break;
                    bool fatal = false;
                    {
                        std::lock_guard<std::mutex> lk(state->sessionMu);
                        const ::ssize_t fed = ::nghttp2_session_mem_recv(
                            state->session,
                            reinterpret_cast<const std::uint8_t *>(buf.data()),
                            n);
                        fatal = fed < 0;
                    }
                    if (fatal || state->abusive.load(std::memory_order_acquire)) {
                        // Flush the GOAWAY nghttp2 queued for the protocol error / abuse.
                        try { co_await serverDrainAndWrite(state); } catch (...) {}
                        break;
                    }
                    // Dispatch any handler-started streams whose handler
                    // hasn't actually been spawned yet. The callback only
                    // flips the flag; spawn happens here so the driver's
                    // shared_ptr is captured by value into the handler
                    // coroutine.
                    std::vector<std::pair<std::int32_t, HttpRequest>> ready;
                    bool trickling = false;
                    {
                        const auto now = std::chrono::steady_clock::now();
                        std::lock_guard<std::mutex> lk(state->streamsMu);
                        for (auto &kv : state->streams) {
                            // A rejected stream was already reset in the
                            // callbacks; its request is a deliberately-cleared
                            // partial fragment, not a usable request. Skip it
                            // here too (rather than relying solely on nghttp2
                            // to suppress further frame-recv events for a reset
                            // stream) so a route handler is never invoked
                            // with a truncated request. Mark it submitted so
                            // this entry is not re-considered every tick.
                            if (kv.second->bodyRejected || kv.second->rejected) {
                                kv.second->responseSubmitted = true;
                                continue;
                            }
                            if (kv.second->handlerStarted &&
                                !kv.second->responseSubmitted) {
                                // Move req out; mark responseSubmitted=true
                                // pre-emptively so a subsequent driver
                                // tick does not re-dispatch the same
                                // request before the handler completes.
                                ready.emplace_back(kv.first, std::move(kv.second->req));
                                kv.second->responseSubmitted = true;
                                kv.second->handlerActive = true;
                            } else if (!kv.second->handlerStarted &&
                                       state->opts.requestTimeout.count() > 0 &&
                                       now - kv.second->started > state->opts.requestTimeout) {
                                // A request still unfinished long after it began: a peer
                                // trickling bytes keeps resetting the idle deadline, so
                                // measure the request itself.
                                trickling = true;
                            }
                        }
                    }
                    if (trickling) break;
                    for (auto &[sid, req] : ready) {
                        state->activeHandlers.fetch_add(1, std::memory_order_acq_rel);
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
            } catch (...) {
                // A failed write or anything unexpected ends the connection.
            }
            state->closed.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> dl(state->deadlineMu);
                state->deadline.disarm();
            }
            // Tell handlers still working that nobody is waiting for them, and wake
            // anything parked on the socket.
            {
                std::lock_guard<std::mutex> lk(state->streamsMu);
                for (auto &kv : state->streams) kv.second->cancel.request_stop();
            }
            // Handlers still running hold the connection state: close the descriptor now,
            // so neither it nor the connection slot outlives the driver.
            state->pipe->shutdown();
            state->pipe->close();
            co_return;
        }
    }

    // ---- Http2Server class --------------------------------------------

    struct Http2Server::ServerState {
        TcpListener listener;
        /// The listener's address, read at construction: serve() moves the listener away
        /// (on whichever thread first resumes it), after which it cannot be asked.
        SocketAddress bound;
        std::unordered_map<std::string, HttpRouteHandler> routes;
        Http2ServerOptions opts;
        /// Set by the first serve(): it consumes the listener.
        std::atomic<bool> served{false};

        ServerState(TcpListener l, const Http2ServerOptions &o)
            : listener(std::move(l)), bound(listener.localAddress()), opts(o) {}
    };

    Http2Server::Http2Server(const std::string &host, std::uint16_t port, Http2ServerOptions options)
        : serverState(std::make_shared<ServerState>(TcpListener::bind(host, port), options)) {
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
        return this->serverState->bound;
    }

    namespace {
        // Free coroutine that owns its parameters by value. Used by
        // Http2Server::serve so the per-connection handler does NOT
        // live inside a lambda-coroutine closure (which would dangle:
        // the closure is a temporary in tcpServe's frame; the
        // coroutine's implicit @c this would point at freed memory).
        using RouteSnapshot = std::shared_ptr<const std::unordered_map<std::string, HttpRouteHandler>>;

        YarnBall::Task<void> runServerConnection(
            std::shared_ptr<Http2Server::ServerState> server,
            RouteSnapshot routes,
            TcpStream client) {
            auto pipe = std::make_unique<TcpPipe>(std::move(client));
            auto state = std::make_shared<Http2ServerConnectionState>(
                std::move(pipe), std::move(routes), server->opts);

            ::nghttp2_session_callbacks *cb = makeServerCallbacks();
            const OptionPtr opt = makeSessionOptions();
            if (::nghttp2_session_server_new2(&state->session, cb,
                                               state.get(), opt.get()) != 0) {
                ::nghttp2_session_callbacks_del(cb);
                co_return;
            }
            ::nghttp2_session_callbacks_del(cb);

            co_await serverDriver(state);
            co_return;
        }
    }

    namespace {
        // Free coroutine that owns @p serverState and @p stop by
        // value. Used by Http2Server::serve (below) for the same
        // reason runServerConnection above is a free coroutine rather
        // than a lambda: the factory forwards into it, owning its
        // parameters by value instead of depending on anything with a
        // shorter lifetime.
        YarnBall::Task<void> serveHttp2(
            std::shared_ptr<Http2Server::ServerState> serverState,
            RouteSnapshot routes,
            std::stop_token stop) {
            auto factory = [serverState, routes](TcpStream client) -> YarnBall::Task<void> {
                return runServerConnection(serverState, routes, std::move(client));
            };
            const std::size_t maxConnections = serverState->opts.maxConnections;
            co_await tcpServe(std::move(serverState->listener), factory, stop, maxConnections);
            co_return;
        }
    }

    YarnBall::Task<void> Http2Server::serve(std::stop_token stop) {
        // Deliberately NOT a coroutine itself. Task is lazy: a
        // member-function coroutine's body -- including a line as
        // early as "copy this->serverState into a local", which is
        // exactly what this function used to do -- only executes on
        // the coroutine's FIRST RESUME, not at the call to serve()
        // itself. A caller that does
        //   coSpawn(server.serve(stop)); ...; stopSrc.request_stop();
        // and then returns/destroys `server` without waiting for the
        // spawned task to actually finish races that first resume
        // against the Http2Server object's destruction: if a worker
        // thread resumes the coroutine after `server` is already
        // gone, the old code's "auto serverState = this->serverState;"
        // read freed memory -- a genuine, observed use-after-free
        // (SIGBUS while copy-constructing the dangling shared_ptr),
        // not a hypothetical one.
        //
        // Making serve() an ordinary (non-coroutine) function fixes
        // this at the root for every caller: the shared_ptr copy
        // below runs synchronously, on the caller's own thread, while
        // `this` is unquestionably still valid (we are inside a live
        // member-function call). The Task that serveHttp2 returns
        // then owns serverState by value and has no dependency on
        // `this` at all for the rest of its lifetime, however much
        // later or on whichever thread it actually gets resumed.
        if (this->serverState->served.exchange(true)) {
            throw SocketException("Http2Server::serve: already called (the listener is consumed by the first call)");
        }
        // The route table is copied here, on the caller's thread, so connections read an
        // immutable snapshot: a route() made while the server runs cannot race with them.
        auto routes = std::make_shared<const std::unordered_map<std::string, HttpRouteHandler>>(
            this->serverState->routes);
        return serveHttp2(this->serverState, std::move(routes), stop);
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
            if (it != this->entries.end()) {
                if (!it->second->isClosed()) co_return it->second;
                this->entries.erase(it); // dead connection: reconnect
            }
        }
        auto conn = std::make_shared<Http2Connection>(
            co_await Http2Connection::connectPlain(host, port));
        std::lock_guard<std::mutex> lk(this->mu);
        // Race: another acquirer may have populated the entry while
        // we were connecting. Prefer theirs; let our local conn drop.
        co_return this->insertLocked(k, std::move(conn));
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
        k.push_back('|');
        k.append(opts.caBundleDir);
        k.push_back('|');
        k.append(opts.clientKeyFile);
        k.push_back('|');
        k.push_back(opts.insecure_no_verify_cert ? '1' : '0');
        k.push_back(opts.insecure_no_verify_name ? '1' : '0');
        {
            std::lock_guard<std::mutex> lk(this->mu);
            auto it = this->entries.find(k);
            if (it != this->entries.end()) {
                if (!it->second->isClosed()) co_return it->second;
                this->entries.erase(it); // dead connection: reconnect
            }
        }
        auto conn = std::make_shared<Http2Connection>(
            co_await Http2Connection::connect(host, port, std::move(opts)));
        std::lock_guard<std::mutex> lk(this->mu);
        // Another acquirer may have populated the entry while we were connecting;
        // prefer theirs unless it has died since (same rule as acquirePlain).
        co_return this->insertLocked(k, std::move(conn));
    }
#endif

    std::shared_ptr<Http2Connection>
    Http2ConnectionPool::insertLocked(const std::string &k, std::shared_ptr<Http2Connection> conn) {
        auto found = this->entries.find(k);
        if (found != this->entries.end()) {
            if (!found->second->isClosed()) return found->second;
            found->second = conn;
            return conn;
        }
        if (this->entries.size() >= this->limit) {
            for (auto it = this->entries.begin(); it != this->entries.end();) {
                it = it->second->isClosed() ? this->entries.erase(it) : std::next(it);
            }
            // Still full of live connections: this one is handed back uncached rather
            // than letting the table grow with every host ever contacted.
            if (this->entries.size() >= this->limit) return conn;
        }
        this->entries.emplace(k, conn);
        return conn;
    }

    void Http2ConnectionPool::evict(std::string_view host, std::uint16_t port) {
        const std::string k = keyOf(host, port);
        std::lock_guard<std::mutex> lk(this->mu);
        // TLS entries are keyed "host:port|options"; drop those too.
        for (auto it = this->entries.begin(); it != this->entries.end();) {
            const std::string &ek = it->first;
            if (ek == k || (ek.size() > k.size() && ek.compare(0, k.size(), k) == 0 &&
                            ek[k.size()] == '|')) {
                it = this->entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::size_t Http2ConnectionPool::size() const {
        std::lock_guard<std::mutex> lk(this->mu);
        return this->entries.size();
    }

}

#endif // SOCCER_HAS_HTTP2
