//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifdef SOCCER_HAS_TLS

#include "TlsStream.h"

#include <mutex>

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <tls.h>

#include "IoAwaiters.h"
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
        /// instead of waiting) is made under this lock; the waits for readiness happen outside it.
        std::mutex callMu;
    };

    TlsStream::TlsStream() : impl(std::make_unique<Impl>()) {
    }

    TlsStream::TlsStream(::tls *ctx, ::tls_config *cfg, int fd) noexcept
        : impl(std::make_unique<Impl>()) {
        this->impl->ctx = ctx;
        this->impl->cfg = cfg;
        this->impl->fd = fd;
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
        // Serialise with in-flight tls_read / tls_write calls so the
        // context is never freed underneath them.
        std::lock_guard<std::mutex> lk(this->impl->callMu);
        if (this->impl->ctx) {
            ::tls_close(this->impl->ctx);
            ::tls_free(this->impl->ctx);
            this->impl->ctx = nullptr;
        }
        if (this->impl->cfg) {
            ::tls_config_free(this->impl->cfg);
            this->impl->cfg = nullptr;
        }
        if (this->impl->fd >= 0) {
            // Wake readers / writers parked on the fd before it is closed (and the number
            // reused). They re-take callMu, find ctx gone and fail without touching the fd.
            const int fd = this->impl->fd;
            this->impl->fd = -1;
            (void) YarnBall::Reactor::instance()->cancelFd(fd);
            ::close(fd);
        }
    }

    std::string TlsStream::alpnSelected() const {
        if (!this->impl || !this->impl->ctx) return {};
        std::lock_guard<std::mutex> lk(this->impl->callMu);
        const char *p = ::tls_conn_alpn_selected(this->impl->ctx);
        return p ? std::string(p) : std::string();
    }

    int TlsStream::fd() const noexcept {
        return this->impl ? this->impl->fd : -1;
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

        TlsStream out(parts.ctx, parts.cfg, tcp.release());
        parts.ctx = nullptr;
        parts.cfg = nullptr;
        co_return out;
    }

    YarnBall::Task<std::size_t> TlsStream::read(std::span<std::byte> buf) {
        if (!this->impl || !this->impl->ctx) {
            throw SocketException("read on closed TlsStream");
        }
        while (true) {
            ssize_t n;
            std::string err;
            int fd;
            {
                std::lock_guard<std::mutex> lk(this->impl->callMu);
                if (!this->impl->ctx) throw SocketException("read on closed TlsStream");
                fd = this->impl->fd; // close() changes it under this lock; waits below use this copy
                n = ::tls_read(this->impl->ctx, buf.data(), buf.size());
                if (n < 0 && n != TLS_WANT_POLLIN && n != TLS_WANT_POLLOUT) {
                    err = ::tls_error(this->impl->ctx) ? ::tls_error(this->impl->ctx) : "tls_read";
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
        if (!this->impl || !this->impl->ctx) {
            throw SocketException("write on closed TlsStream");
        }
        std::size_t total = 0;
        while (total < buf.size()) {
            ssize_t n;
            std::string err;
            int fd;
            {
                std::lock_guard<std::mutex> lk(this->impl->callMu);
                if (!this->impl->ctx) throw SocketException("write on closed TlsStream");
                fd = this->impl->fd; // close() changes it under this lock; waits below use this copy
                n = ::tls_write(this->impl->ctx, buf.data() + total, buf.size() - total);
                if (n < 0 && n != TLS_WANT_POLLIN && n != TLS_WANT_POLLOUT) {
                    err = ::tls_error(this->impl->ctx) ? ::tls_error(this->impl->ctx) : "tls_write";
                }
            }
            if (n >= 0) {
                if (n == 0) break;
                total += static_cast<std::size_t>(n);
                continue;
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
