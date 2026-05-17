//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifdef SOCCER_HAS_TLS

#include "TlsStream.h"

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <tls.h>

#include "IoAwaiters.h"
#include "SocketAddress.h"
#include "SocketException.h"

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
    TlsStream &TlsStream::operator=(TlsStream &&) noexcept = default;

    TlsStream::~TlsStream() {
        this->close();
    }

    void TlsStream::close() noexcept {
        if (!this->impl) return;
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
            ::close(this->impl->fd);
            this->impl->fd = -1;
        }
    }

    int TlsStream::fd() const noexcept {
        return this->impl ? this->impl->fd : -1;
    }


    YarnBall::Task<TlsStream> TlsStream::connect(std::string host,
                                                 std::uint16_t port,
                                                 TlsClientOptions opts) {
        // Resolve and create a non-blocking TCP socket.
        auto addr = SocketAddress::resolve(host, port);
        const int fd = ::socket(addr.family(), SOCK_STREAM, 0);
        if (fd < 0) throw SocketException("socket", errno);

        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            int e = errno;
            ::close(fd);
            throw SocketException("fcntl(O_NONBLOCK)", e);
        }

        // Initiate the TCP connect.
        int rc = ::connect(fd, addr.data(), addr.length());
        if (rc < 0 && errno != EINPROGRESS) {
            int e = errno;
            ::close(fd);
            throw SocketException("connect", e);
        }
        if (rc < 0) {
            co_await YarnBall::io::waitWritable(fd);
            int sockerr = 0;
            socklen_t errlen = sizeof(sockerr);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) < 0 || sockerr != 0) {
                ::close(fd);
                throw SocketException("connect (async)", sockerr ? sockerr : errno);
            }
        }

        // Set up the TLS client side.
        ::tls *ctx = ::tls_client();
        if (!ctx) {
            ::close(fd);
            throw SocketException("tls_client failed");
        }
        ::tls_config *cfg = ::tls_config_new();
        if (!cfg) {
            ::tls_free(ctx);
            ::close(fd);
            throw SocketException("tls_config_new failed");
        }
        if (opts.insecure_no_verify_cert) {
            ::tls_config_insecure_noverifycert(cfg);
        }
        if (opts.insecure_no_verify_name) {
            ::tls_config_insecure_noverifyname(cfg);
        }
        // Optional production-grade verification config.
        if (!opts.caBundleFile.empty()) {
            if (::tls_config_set_ca_file(cfg, opts.caBundleFile.c_str()) != 0) {
                const std::string err = ::tls_config_error(cfg)
                    ? ::tls_config_error(cfg) : "tls_config_set_ca_file";
                ::tls_config_free(cfg);
                ::tls_free(ctx);
                ::close(fd);
                throw SocketException(err);
            }
        }
        if (!opts.caBundleDir.empty()) {
            if (::tls_config_set_ca_path(cfg, opts.caBundleDir.c_str()) != 0) {
                const std::string err = ::tls_config_error(cfg)
                    ? ::tls_config_error(cfg) : "tls_config_set_ca_path";
                ::tls_config_free(cfg);
                ::tls_free(ctx);
                ::close(fd);
                throw SocketException(err);
            }
        }
        if (!opts.clientCertFile.empty()) {
            if (::tls_config_set_cert_file(cfg, opts.clientCertFile.c_str()) != 0 ||
                ::tls_config_set_key_file(cfg, opts.clientKeyFile.c_str()) != 0) {
                const std::string err = ::tls_config_error(cfg)
                    ? ::tls_config_error(cfg) : "tls_config_set_(cert|key)_file";
                ::tls_config_free(cfg);
                ::tls_free(ctx);
                ::close(fd);
                throw SocketException(err);
            }
        }
        if (!opts.alpnProtocols.empty()) {
            if (::tls_config_set_alpn(cfg, opts.alpnProtocols.c_str()) != 0) {
                const std::string err = ::tls_config_error(cfg)
                    ? ::tls_config_error(cfg) : "tls_config_set_alpn";
                ::tls_config_free(cfg);
                ::tls_free(ctx);
                ::close(fd);
                throw SocketException(err);
            }
        }
        if (::tls_configure(ctx, cfg) != 0) {
            const std::string err = ::tls_error(ctx) ? ::tls_error(ctx) : "tls_configure";
            ::tls_config_free(cfg);
            ::tls_free(ctx);
            ::close(fd);
            throw SocketException(err);
        }
        if (::tls_connect_socket(ctx, fd, host.c_str()) != 0) {
            const std::string err = ::tls_error(ctx) ? ::tls_error(ctx) : "tls_connect_socket";
            ::tls_config_free(cfg);
            ::tls_free(ctx);
            ::close(fd);
            throw SocketException(err);
        }

        // Drive the handshake to completion.
        while (true) {
            int hs = ::tls_handshake(ctx);
            if (hs == 0) break;
            if (hs == TLS_WANT_POLLIN) {
                co_await YarnBall::io::waitReadable(fd);
                continue;
            }
            if (hs == TLS_WANT_POLLOUT) {
                co_await YarnBall::io::waitWritable(fd);
                continue;
            }
            const std::string err = ::tls_error(ctx) ? ::tls_error(ctx) : "tls_handshake";
            ::tls_config_free(cfg);
            ::tls_free(ctx);
            ::close(fd);
            throw SocketException(err);
        }

        co_return TlsStream(ctx, cfg, fd);
    }

    YarnBall::Task<std::size_t> TlsStream::read(std::span<std::byte> buf) {
        if (!this->impl || !this->impl->ctx) {
            throw SocketException("read on closed TlsStream");
        }
        while (true) {
            ssize_t n = ::tls_read(this->impl->ctx, buf.data(), buf.size());
            if (n >= 0) co_return static_cast<std::size_t>(n);
            if (n == TLS_WANT_POLLIN) {
                co_await YarnBall::io::waitReadable(this->impl->fd);
                continue;
            }
            if (n == TLS_WANT_POLLOUT) {
                co_await YarnBall::io::waitWritable(this->impl->fd);
                continue;
            }
            const std::string err = ::tls_error(this->impl->ctx)
                                        ? ::tls_error(this->impl->ctx)
                                        : "tls_read";
            throw SocketException(err);
        }
    }

    YarnBall::Task<std::size_t> TlsStream::write(std::span<const std::byte> buf) {
        if (!this->impl || !this->impl->ctx) {
            throw SocketException("write on closed TlsStream");
        }
        std::size_t total = 0;
        while (total < buf.size()) {
            ssize_t n = ::tls_write(this->impl->ctx,
                                    buf.data() + total,
                                    buf.size() - total);
            if (n >= 0) {
                if (n == 0) break;
                total += static_cast<std::size_t>(n);
                continue;
            }
            if (n == TLS_WANT_POLLIN) {
                co_await YarnBall::io::waitReadable(this->impl->fd);
                continue;
            }
            if (n == TLS_WANT_POLLOUT) {
                co_await YarnBall::io::waitWritable(this->impl->fd);
                continue;
            }
            const std::string err = ::tls_error(this->impl->ctx)
                                        ? ::tls_error(this->impl->ctx)
                                        : "tls_write";
            throw SocketException(err);
        }
        co_return total;
    }

}

#endif // SOCCER_HAS_TLS
