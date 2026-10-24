//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifdef SOCCER_HAS_TLS

#include "TlsListener.h"

#include <cerrno>
#include <memory>
#include <mutex>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <tls.h>

#include "Coroutines.h"
#include "IoAwaiters.h"
#include "IoDeadline.h"
#include "SocketAddress.h"
#include "SocketException.h"
#include "TcpListener.h"

namespace Soccer {

    /**
     * @struct TlsListener::Impl
     * @brief Holds the underlying TcpListener plus the libtls server
     *        context. @c TcpListener handles the bind/listen lifecycle
     *        and the non-blocking accept; we layer the TLS handshake on
     *        top.
     */
    struct TlsListener::Impl {
        TcpListener tcp;
        /// Guards server_ctx and cfg. close() frees them, tls_accept_socket uses
        /// them, and the two can run on different threads (a certificate reload
        /// closes the listener while an accept loop is parked or just resumed).
        /// Every use of server_ctx happens under this lock, together with a check
        /// that it is still set.
        std::mutex mu;
        ::tls *server_ctx = nullptr;
        ::tls_config *cfg = nullptr;
    };

    TlsListener::TlsListener() : impl(std::make_shared<Impl>()) {
    }

    TlsListener::TlsListener(TlsListener &&) noexcept = default;
    TlsListener &TlsListener::operator=(TlsListener &&other) noexcept {
        if (this != &other) {
            this->close(); // release the context, config and socket this listener still owns
            this->impl = std::move(other.impl);
        }
        return *this;
    }

    TlsListener::~TlsListener() {
        this->close();
    }

    SocketAddress TlsListener::localAddress() const {
        if (!this->impl) {
            throw SocketException("localAddress on closed TlsListener");
        }
        return this->impl->tcp.localAddress();
    }

    void TlsListener::close() noexcept {
        const std::shared_ptr<Impl> im = this->impl;
        if (!im) return;
        {
            std::lock_guard<std::mutex> lk(im->mu);
            if (im->server_ctx) {
                ::tls_close(im->server_ctx);
                ::tls_free(im->server_ctx);
                im->server_ctx = nullptr;
            }
            if (im->cfg) {
                ::tls_config_free(im->cfg);
                im->cfg = nullptr;
            }
        }
        im->tcp.close();
    }

    TlsListener TlsListener::bind(const std::string &host,
                                  std::uint16_t port,
                                  const std::string &certPath,
                                  const std::string &keyPath,
                                  const TlsServerOptions &options) {
        TlsListener l;
        l.impl->tcp = TcpListener::bind(host, port);

        l.impl->cfg = ::tls_config_new();
        if (!l.impl->cfg) {
            throw SocketException("tls_config_new failed");
        }
        if (::tls_config_set_cert_file(l.impl->cfg, certPath.c_str()) != 0) {
            const std::string err =
                ::tls_config_error(l.impl->cfg) ? ::tls_config_error(l.impl->cfg) : "tls_config_set_cert_file";
            throw SocketException(err);
        }
        if (::tls_config_set_key_file(l.impl->cfg, keyPath.c_str()) != 0) {
            const std::string err =
                ::tls_config_error(l.impl->cfg) ? ::tls_config_error(l.impl->cfg) : "tls_config_set_key_file";
            throw SocketException(err);
        }

        if (!options.alpnProtocols.empty() &&
            ::tls_config_set_alpn(l.impl->cfg, options.alpnProtocols.c_str()) != 0) {
            const std::string err =
                ::tls_config_error(l.impl->cfg) ? ::tls_config_error(l.impl->cfg) : "tls_config_set_alpn";
            throw SocketException(err);
        }
        if (!options.clientCaFile.empty()) {
            if (::tls_config_set_ca_file(l.impl->cfg, options.clientCaFile.c_str()) != 0) {
                const std::string err =
                    ::tls_config_error(l.impl->cfg) ? ::tls_config_error(l.impl->cfg) : "tls_config_set_ca_file";
                throw SocketException(err);
            }
            ::tls_config_verify_client(l.impl->cfg); // require a certificate signed by that CA
        }

        l.impl->server_ctx = ::tls_server();
        if (!l.impl->server_ctx) {
            throw SocketException("tls_server failed");
        }
        if (::tls_configure(l.impl->server_ctx, l.impl->cfg) != 0) {
            const std::string err =
                ::tls_error(l.impl->server_ctx) ? ::tls_error(l.impl->server_ctx) : "tls_configure";
            throw SocketException(err);
        }
        return l;
    }

    TlsListener::PendingHandshake::PendingHandshake(PendingHandshake &&o) noexcept
        : tcp(std::move(o.tcp)), ctx(o.ctx) {
        o.ctx = nullptr;
    }

    TlsListener::PendingHandshake &TlsListener::PendingHandshake::operator=(PendingHandshake &&o) noexcept {
        if (this != &o) {
            if (ctx) ::tls_free(ctx);
            tcp = std::move(o.tcp);
            ctx = o.ctx;
            o.ctx = nullptr;
        }
        return *this;
    }

    TlsListener::PendingHandshake::~PendingHandshake() {
        if (ctx) ::tls_free(ctx);
    }

    YarnBall::Task<TlsListener::PendingHandshake> TlsListener::acceptPendingOn(
        std::shared_ptr<Impl> im) {
        if (!im) throw SocketException("accept on closed TlsListener");
        {
            std::lock_guard<std::mutex> lk(im->mu);
            if (!im->server_ctx) throw SocketException("accept on closed TlsListener");
        }
        TlsListener::PendingHandshake p;
        p.tcp = co_await im->tcp.accept();
        // The listener may have been closed (certificate reload) while parked in accept().
        // close() frees server_ctx under the same lock, so this either sees it
        // still set and finishes with it before close() can proceed, or sees it gone.
        std::lock_guard<std::mutex> lk(im->mu);
        if (!im->server_ctx) {
            throw SocketException("accept on closed TlsListener");
        }
        // Hand the fd to libtls; the handshake is driven later by completeHandshake.
        if (::tls_accept_socket(im->server_ctx, &p.ctx, p.tcp.fd()) != 0) {
            p.ctx = nullptr;
            const std::string err =
                ::tls_error(im->server_ctx)
                    ? ::tls_error(im->server_ctx)
                    : "tls_accept_socket";
            throw SocketException(err);
        }
        co_return p;
    }

    YarnBall::Task<TlsListener::PendingHandshake> TlsListener::acceptPending() {
        // Not a coroutine itself: the shared state is captured here, on the
        // caller's thread, so the accept does not depend on `this` surviving.
        return acceptPendingOn(this->impl);
    }

    YarnBall::Task<TlsStream> TlsListener::completeHandshake(PendingHandshake p, std::chrono::milliseconds timeout) {
        ::tls *client_ctx = p.ctx;
        const int client_fd = p.tcp.fd();
        // A peer that stalls mid-handshake is cut off when the deadline passes. Declared
        // after `p` (a parameter), so it is disarmed before the fd is closed or reused.
        IoDeadline deadline(client_fd, timeout);
        while (true) {
            int hs = ::tls_handshake(client_ctx);
            if (hs == 0) break;
            if (hs == TLS_WANT_POLLIN) {
                co_await YarnBall::io::waitReadable(client_fd);
                continue;
            }
            if (hs == TLS_WANT_POLLOUT) {
                co_await YarnBall::io::waitWritable(client_fd);
                continue;
            }
            if (deadline.expired()) throw SocketException("TLS handshake timed out");
            const std::string err =
                ::tls_error(client_ctx) ? ::tls_error(client_ctx) : "tls_handshake";
            throw SocketException(err); // p still owns ctx and the socket: both are released with it
        }

        // Transfer ownership of the context and the fd to the TlsStream. The deadline is
        // disarmed first so a late timer cannot shut down an fd that is no longer ours.
        deadline.disarm();
        p.ctx = nullptr;
        const int adopted_fd = p.tcp.release();
        co_return TlsStream(client_ctx, /*cfg=*/nullptr, adopted_fd);
    }

    YarnBall::Task<TlsStream> TlsListener::accept(std::chrono::milliseconds handshakeTimeout) {
        PendingHandshake p = co_await this->acceptPending();
        // Hand back the stream with its handshake still to run: the first handshake() / read /
        // write drives it under handshakeTimeout, so a peer that connects and stalls costs the
        // accept loop nothing.
        ::tls *ctx = p.ctx;
        p.ctx = nullptr;
        const int fd = p.tcp.release();
        co_return TlsStream(ctx, fd, handshakeTimeout);
    }

}

#endif // SOCCER_HAS_TLS
