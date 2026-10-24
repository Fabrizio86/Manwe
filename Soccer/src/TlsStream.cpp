//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifdef SOCCER_HAS_TLS

#include "TlsStream.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>

#include <cerrno>
#include <poll.h>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <tls.h>

#include "AsyncSync.h"
#include "IoAwaiters.h"
#include "NetUtil.h"
#include "PlatformNet.h"
#include "Reactor.h"
#include "IoDeadline.h"
#include "SocketAddress.h"
#include "SocketException.h"
#include "TcpStream.h"

namespace Soccer {

    /**
     * @struct TlsStream::Impl
     * @brief Private state: libtls context, optional config (server-side
     *        owns the config; client-side leaves it null and lets libtls
     *        use its defaults), and the underlying socket fd.
     */
    struct TlsStream::Impl {
        ::tls *ctx = nullptr;
        ::tls_config *cfg = nullptr;
        int fd = -1;
        /// A libtls context is not safe for concurrent use, and a connection normally has a reader coroutine and writer coroutines
        /// running on different threads at once. Every tls_read / tls_write call (each is non-blocking: it returns TLS_WANT_POLL*
        /// instead of waiting) is made under this lock; the waits for readiness happen outside it. Also guards the fields below.
        mutable std::mutex callMu;
        /// Serialises whole write() calls: two writers must not interleave their partial tls_write retries (the bytes of one
        /// message would land inside another's, and libtls requires a WANT_POLL retry to repeat the same arguments).
        YarnBall::AsyncMutex writeMu;
        /// Coroutines parked on the descriptor (counted under callMu from the moment they decide to wait).
        int waiters = 0;
        /// Descriptor released by close() but not yet closed because a waiter may still register on it.
        int retiredFd = -1;
        /// Server-side stream accepted before its handshake: true until the first handshake() completes it.
        std::atomic<bool> hsPending{false};
        /// Longest the deferred handshake may take (zero or negative: unbounded). Fixed at construction.
        std::chrono::milliseconds hsTimeout{0};
        /// Serialises the deferred handshake; hsError (the sticky failure text) is only touched under it.
        YarnBall::AsyncMutex hsMu;
        std::string hsError;

        void leave() noexcept {
            int toClose = -1;
            {
                std::lock_guard<std::mutex> lk(this->callMu);
                if (--this->waiters == 0) toClose = std::exchange(this->retiredFd, -1);
            }
            if (toClose >= 0) (void) YarnBall::closeSocket(toClose);
        }
    };

    namespace {
        /// Holds the descriptor open while its owner waits on it, so a concurrent close() cannot hand the number to
        /// another connection between the owner's decision to wait and the Reactor registration. Created under callMu.
        template<typename ImplT>
        struct WaitTicket {
            std::shared_ptr<ImplT> im;
            explicit WaitTicket(std::shared_ptr<ImplT> i) noexcept : im(std::move(i)) {}
            WaitTicket(const WaitTicket &) = delete;
            WaitTicket &operator=(const WaitTicket &) = delete;
            ~WaitTicket() { im->leave(); }
        };
    }

    TlsStream::TlsStream() : impl(std::make_shared<Impl>()) {
    }

    /// The shared state for an adopting constructor. The constructor takes ownership of the context, the config
    /// and the descriptor even when the allocation fails: they are released before the exception propagates.
    std::shared_ptr<TlsStream::Impl> TlsStream::makeAdoptingImpl(::tls *ctx, ::tls_config *cfg, int fd) {
        try {
            return std::make_shared<Impl>();
        } catch (...) {
            if (ctx) ::tls_free(ctx);
            if (cfg) ::tls_config_free(cfg);
            if (fd >= 0) (void) YarnBall::closeSocket(fd);
            throw;
        }
    }

    TlsStream::TlsStream(::tls *ctx, ::tls_config *cfg, int fd)
        : impl(makeAdoptingImpl(ctx, cfg, fd)) {
        this->impl->ctx = ctx;
        this->impl->cfg = cfg;
        this->impl->fd = fd;
    }

    TlsStream::TlsStream(::tls *ctx, int fd, std::chrono::milliseconds handshakeTimeout)
        : impl(makeAdoptingImpl(ctx, nullptr, fd)) {
        this->impl->ctx = ctx;
        this->impl->fd = fd;
        this->impl->hsTimeout = handshakeTimeout;
        this->impl->hsPending.store(true, std::memory_order_release);
    }

    TlsStream::TlsStream(TlsStream &&) noexcept = default;
    TlsStream &TlsStream::operator=(TlsStream &&other) noexcept {
        if (this != &other) {
            this->close(); // release the context and fd this stream still owns
            this->impl = std::move(other.impl);
        }
        return *this;
    }

    TlsStream::~TlsStream() {
        this->close();
    }

    void TlsStream::close() noexcept {
        if (!this->impl) return;
        // Take the TLS objects and the descriptor out under the lock (which serialises with
        // in-flight tls_read / tls_write, so the context is never freed underneath them), then
        // do the slow part -- the close_notify -- outside it, so a concurrent reader or writer
        // is never held up behind the alert, and nothing is woken from inside the lock.
        ::tls *ctx = nullptr;
        ::tls_config *cfg = nullptr;
        int fd = -1;
        {
            std::lock_guard<std::mutex> lk(this->impl->callMu);
            ctx = std::exchange(this->impl->ctx, nullptr);
            cfg = std::exchange(this->impl->cfg, nullptr);
            fd = std::exchange(this->impl->fd, -1);
        }
        if (ctx) {
            // Best-effort close_notify on the still-open descriptor: one attempt, plus one more
            // after a zero-wait poll if the socket was momentarily full. The socket is
            // non-blocking, so this never waits for the peer; a full send buffer just drops the alert.
            int rc = ::tls_close(ctx);
            if ((rc == TLS_WANT_POLLIN || rc == TLS_WANT_POLLOUT) && fd >= 0) {
                pollfd pfd{};
                pfd.fd = fd;
                pfd.events = (rc == TLS_WANT_POLLIN) ? POLLIN : POLLOUT;
                if (::poll(&pfd, 1, 0) > 0) (void) ::tls_close(ctx);
            }
            ::tls_free(ctx);
        }
        if (cfg) ::tls_config_free(cfg);
        if (fd >= 0) {
            // Wake readers / writers parked on the fd. They re-take callMu, find ctx gone and fail without touching
            // the fd. A shutdown makes the descriptor readable and writable, which also wakes a waiter that has not
            // registered yet; and the descriptor is kept open until the last waiter has left, so such a waiter never
            // registers on a number that was closed (or reused by another connection).
            detail::shutdownSocket(fd);
            (void) YarnBall::Reactor::instance()->cancelFd(fd);
            int toClose = -1;
            {
                std::lock_guard<std::mutex> lk(this->impl->callMu);
                if (this->impl->waiters == 0) toClose = fd;
                else this->impl->retiredFd = fd;
            }
            if (toClose >= 0) (void) YarnBall::closeSocket(toClose);
        }
    }

    std::string TlsStream::alpnSelected() const {
        if (!this->impl) return {};
        std::lock_guard<std::mutex> lk(this->impl->callMu);
        if (!this->impl->ctx) return {};
        const char *p = ::tls_conn_alpn_selected(this->impl->ctx);
        return p ? std::string(p) : std::string();
    }

    int TlsStream::fd() const noexcept {
        if (!this->impl) return -1;
        std::lock_guard<std::mutex> lk(this->impl->callMu);
        return this->impl->fd;
    }


    namespace {
        /// Frees the libtls objects of a half-built client on any exit path
        /// (error, or the coroutine being destroyed while parked).
        struct ClientParts {
            ::tls *ctx = nullptr;
            ::tls_config *cfg = nullptr;
            ClientParts() = default;
            ClientParts(const ClientParts &) = delete;
            ClientParts &operator=(const ClientParts &) = delete;
            ~ClientParts() {
                if (ctx) ::tls_free(ctx);
                if (cfg) ::tls_config_free(cfg);
            }
        };
    }

    YarnBall::Task<TlsStream> TlsStream::connect(std::string host,
                                                 std::uint16_t port,
                                                 TlsClientOptions opts) {
        if (opts.clientCertFile.empty() != opts.clientKeyFile.empty()) {
            // Silently skipping half a client identity would connect without client auth.
            throw SocketException("TlsClientOptions: clientCertFile and clientKeyFile must be set together");
        }
        // Resolve (trying every address) and connect; the socket comes back
        // non-blocking and close-on-exec, and is closed again if anything below throws.
        TcpStream tcp = co_await tcpConnect(host, port, opts.connectTimeout);
        const int fd = tcp.fd();

        // Set up the TLS client side.
        ClientParts parts;
        parts.ctx = ::tls_client();
        if (!parts.ctx) {
            throw SocketException("tls_client failed");
        }
        parts.cfg = ::tls_config_new();
        if (!parts.cfg) {
            throw SocketException("tls_config_new failed");
        }
        auto configError = [&parts](const char *what) {
            const char *e = ::tls_config_error(parts.cfg);
            return SocketException(e ? e : what);
        };
        if (opts.insecure_no_verify_cert) {
            ::tls_config_insecure_noverifycert(parts.cfg);
        }
        if (opts.insecure_no_verify_name) {
            ::tls_config_insecure_noverifyname(parts.cfg);
        }
        // Optional production-grade verification config.
        if (!opts.caBundleFile.empty() &&
            ::tls_config_set_ca_file(parts.cfg, opts.caBundleFile.c_str()) != 0) {
            throw configError("tls_config_set_ca_file");
        }
        if (!opts.caBundleDir.empty() &&
            ::tls_config_set_ca_path(parts.cfg, opts.caBundleDir.c_str()) != 0) {
            throw configError("tls_config_set_ca_path");
        }
        if (!opts.clientCertFile.empty() &&
            (::tls_config_set_cert_file(parts.cfg, opts.clientCertFile.c_str()) != 0 ||
             ::tls_config_set_key_file(parts.cfg, opts.clientKeyFile.c_str()) != 0)) {
            throw configError("tls_config_set_(cert|key)_file");
        }
        if (!opts.alpnProtocols.empty() &&
            ::tls_config_set_alpn(parts.cfg, opts.alpnProtocols.c_str()) != 0) {
            throw configError("tls_config_set_alpn");
        }
        if (::tls_configure(parts.ctx, parts.cfg) != 0) {
            throw SocketException(::tls_error(parts.ctx) ? ::tls_error(parts.ctx) : "tls_configure");
        }
        if (::tls_connect_socket(parts.ctx, fd, host.c_str()) != 0) {
            throw SocketException(::tls_error(parts.ctx) ? ::tls_error(parts.ctx) : "tls_connect_socket");
        }

        // Drive the handshake to completion, but not forever: a server that
        // accepts the connection and then never answers would park this forever.
        // (The TCP connect above has its own limit, opts.connectTimeout.)
        IoDeadline deadline(fd, opts.handshakeTimeout);
        while (true) {
            int hs = ::tls_handshake(parts.ctx);
            if (hs == 0) break;
            if (hs == TLS_WANT_POLLIN) {
                co_await YarnBall::io::waitReadable(fd);
                continue;
            }
            if (hs == TLS_WANT_POLLOUT) {
                co_await YarnBall::io::waitWritable(fd);
                continue;
            }
            if (deadline.expired()) throw SocketException("TLS handshake timed out");
            throw SocketException(::tls_error(parts.ctx) ? ::tls_error(parts.ctx) : "tls_handshake");
        }
        deadline.disarm(); // before the fd changes hands

        // The constructor owns the parts from here on, even if it throws.
        ::tls *const ctx = std::exchange(parts.ctx, nullptr);
        ::tls_config *const cfg = std::exchange(parts.cfg, nullptr);
        TlsStream out(ctx, cfg, tcp.release());
        co_return out;
    }

    YarnBall::Task<void> TlsStream::handshake() {
        if (!this->impl) {
            throw SocketException("handshake on closed TlsStream");
        }
        if (!this->impl->hsPending.load(std::memory_order_acquire)) co_return;
        co_await driveHandshake(this->impl);
    }

    YarnBall::Task<void> TlsStream::driveHandshake(std::shared_ptr<Impl> im) {
        auto guard = co_await im->hsMu.lock();
        if (!im->hsPending.load(std::memory_order_acquire)) co_return; // another caller finished it
        if (!im->hsError.empty()) throw SocketException(im->hsError);

        // The ticket keeps the descriptor open for the whole handshake, so the deadline below (declared after it, hence
        // disarmed first) can never shut down a number that close() released and another connection reused.
        int fd = -1;
        std::optional<WaitTicket<Impl>> ticket;
        {
            std::lock_guard<std::mutex> lk(im->callMu);
            if (!im->ctx) throw SocketException("handshake on closed TlsStream");
            fd = im->fd;
            ++im->waiters;
            ticket.emplace(im);
        }
        IoDeadline deadline(fd, im->hsTimeout);
        std::string err;
        while (true) {
            int hs;
            {
                std::lock_guard<std::mutex> lk(im->callMu);
                if (!im->ctx) throw SocketException("handshake on closed TlsStream");
                hs = ::tls_handshake(im->ctx);
                if (hs != 0 && hs != TLS_WANT_POLLIN && hs != TLS_WANT_POLLOUT) {
                    err = ::tls_error(im->ctx) ? ::tls_error(im->ctx) : "tls_handshake";
                }
            }
            if (hs == 0) break;
            if (hs == TLS_WANT_POLLIN) {
                co_await YarnBall::io::waitReadable(fd);
                continue;
            }
            if (hs == TLS_WANT_POLLOUT) {
                co_await YarnBall::io::waitWritable(fd);
                continue;
            }
            if (deadline.expired()) err = "TLS handshake timed out";
            im->hsError = err;
            deadline.disarm();
            TlsStream dead; // close the connection now: it is useless, and the peer should not wait for the owner
            dead.impl = im;
            dead.close();
            throw SocketException(err);
        }
        deadline.disarm();
        im->hsPending.store(false, std::memory_order_release);
        co_return;
    }

    YarnBall::Task<std::size_t> TlsStream::read(std::span<std::byte> buf) {
        if (!this->impl) {
            throw SocketException("read on closed TlsStream");
        }
        // Held across the loop: the shared state outlives this stream object if it is destroyed mid-read.
        const std::shared_ptr<Impl> im = this->impl;
        if (im->hsPending.load(std::memory_order_acquire)) co_await driveHandshake(im);
        // libtls reports end of stream as 0, which an empty buffer must not be mistaken for.
        if (buf.empty()) co_return 0;
        while (true) {
            ssize_t n;
            std::string err;
            int fd;
            std::optional<WaitTicket<Impl>> ticket;
            {
                std::lock_guard<std::mutex> lk(im->callMu);
                if (!im->ctx) throw SocketException("read on closed TlsStream");
                fd = im->fd;
                n = ::tls_read(im->ctx, buf.data(), buf.size());
                if (n < 0 && n != TLS_WANT_POLLIN && n != TLS_WANT_POLLOUT) {
                    err = ::tls_error(im->ctx) ? ::tls_error(im->ctx) : "tls_read";
                } else if (n == TLS_WANT_POLLIN || n == TLS_WANT_POLLOUT) {
                    ++im->waiters; // released by the ticket; keeps `fd` open while we wait on it
                    ticket.emplace(im);
                }
            }
            if (n >= 0) co_return static_cast<std::size_t>(n);
            if (n == TLS_WANT_POLLIN) {
                co_await YarnBall::io::waitReadable(fd);
                continue;
            }
            if (n == TLS_WANT_POLLOUT) {
                co_await YarnBall::io::waitWritable(fd);
                continue;
            }
            throw SocketException(err);
        }
    }

    YarnBall::Task<std::size_t> TlsStream::write(std::span<const std::byte> buf) {
        if (!this->impl) {
            throw SocketException("write on closed TlsStream");
        }
        const std::shared_ptr<Impl> im = this->impl;
        if (im->hsPending.load(std::memory_order_acquire)) co_await driveHandshake(im);
        // One writer at a time for the whole buffer (see Impl::writeMu).
        auto writeGuard = co_await im->writeMu.lock();
        std::size_t total = 0;
        while (total < buf.size()) {
            ssize_t n;
            std::string err;
            int fd;
            std::optional<WaitTicket<Impl>> ticket;
            {
                std::lock_guard<std::mutex> lk(im->callMu);
                if (!im->ctx) throw SocketException("write on closed TlsStream");
                fd = im->fd;
                n = ::tls_write(im->ctx, buf.data() + total, buf.size() - total);
                if (n < 0 && n != TLS_WANT_POLLIN && n != TLS_WANT_POLLOUT) {
                    err = ::tls_error(im->ctx) ? ::tls_error(im->ctx) : "tls_write";
                } else if (n == TLS_WANT_POLLIN || n == TLS_WANT_POLLOUT) {
                    ++im->waiters;
                    ticket.emplace(im);
                }
            }
            if (n > 0) {
                total += static_cast<std::size_t>(n);
                continue;
            }
            if (n == 0) {
                // No progress on a non-empty buffer: a short count would break the "writes it all" contract.
                throw SocketException("tls_write made no progress");
            }
            if (n == TLS_WANT_POLLIN) {
                co_await YarnBall::io::waitReadable(fd);
                continue;
            }
            if (n == TLS_WANT_POLLOUT) {
                co_await YarnBall::io::waitWritable(fd);
                continue;
            }
            throw SocketException(err);
        }
        co_return total;
    }

}

#endif // SOCCER_HAS_TLS
