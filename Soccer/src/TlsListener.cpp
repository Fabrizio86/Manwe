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
#include "Timers.h"
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
        ::tls *server_ctx = nullptr;
        ::tls_config *cfg = nullptr;
    };

    TlsListener::TlsListener() : impl(std::make_unique<Impl>()) {
    }

    TlsListener::TlsListener(TlsListener &&) noexcept = default;
    TlsListener &TlsListener::operator=(TlsListener &&) noexcept = default;

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
        if (!this->impl) return;
        if (this->impl->server_ctx) {
            ::tls_close(this->impl->server_ctx);
            ::tls_free(this->impl->server_ctx);
            this->impl->server_ctx = nullptr;
        }
        if (this->impl->cfg) {
            ::tls_config_free(this->impl->cfg);
            this->impl->cfg = nullptr;
        }
        this->impl->tcp.close();
    }

    TlsListener TlsListener::bind(const std::string &host,
                                  std::uint16_t port,
                                  const std::string &certPath,
                                  const std::string &keyPath) {
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

    namespace {
        /// Shared between a handshake and its watchdog. The mutex orders "the handshake is over, the fd may be closed or
        /// reused" against "the watchdog is about to shut the fd down", so a late watchdog never touches someone else's fd.
        struct HandshakeGuard {
            std::mutex mu;
            bool over = false;
        };

        YarnBall::Task<void> handshakeWatchdog(std::shared_ptr<HandshakeGuard> g, int fd,
                                               std::chrono::milliseconds limit) {
            co_await YarnBall::sleepFor(limit);
            std::lock_guard lock(g->mu);
            if (!g->over) (void) ::shutdown(fd, SHUT_RDWR); // wakes the parked wait; the handshake then fails
        }
    }

    YarnBall::Task<TlsListener::PendingHandshake> TlsListener::acceptPending() {
        if (!this->impl || !this->impl->server_ctx) {
            throw SocketException("accept on closed TlsListener");
        }
        PendingHandshake p;
        p.tcp = co_await this->impl->tcp.accept();
        // Hand the fd to libtls; the handshake is driven later by completeHandshake.
        if (::tls_accept_socket(this->impl->server_ctx, &p.ctx, p.tcp.fd()) != 0) {
            p.ctx = nullptr;
            const std::string err =
                ::tls_error(this->impl->server_ctx)
                    ? ::tls_error(this->impl->server_ctx)
                    : "tls_accept_socket";
            throw SocketException(err);
        }
        co_return p;
    }

    YarnBall::Task<TlsStream> TlsListener::completeHandshake(PendingHandshake p, std::chrono::milliseconds timeout) {
        ::tls *client_ctx = p.ctx;
        const int client_fd = p.tcp.fd();
        std::shared_ptr<HandshakeGuard> guard;
        if (timeout.count() > 0) {
            guard = std::make_shared<HandshakeGuard>();
            YarnBall::coSpawn(handshakeWatchdog(guard, client_fd, timeout));
        }
        const auto over = [&guard] {
            if (guard) { std::lock_guard lock(guard->mu); guard->over = true; }
        };
        while (true) {
            int hs = ::tls_handshake(client_ctx);
            if (hs == 0) { over(); break; }
            if (hs == TLS_WANT_POLLIN) {
                co_await YarnBall::io::waitReadable(client_fd);
                continue;
            }
            if (hs == TLS_WANT_POLLOUT) {
                co_await YarnBall::io::waitWritable(client_fd);
                continue;
            }
            const std::string err =
                ::tls_error(client_ctx) ? ::tls_error(client_ctx) : "tls_handshake";
            over();
            throw SocketException(err); // p still owns ctx and the socket: both are released with it
        }

        // Transfer ownership of the context and the fd to the TlsStream.
        p.ctx = nullptr;
        const int adopted_fd = p.tcp.release();
        co_return TlsStream(client_ctx, /*cfg=*/nullptr, adopted_fd);
    }

    YarnBall::Task<TlsStream> TlsListener::accept() {
        PendingHandshake p = co_await this->acceptPending();
        co_return co_await completeHandshake(std::move(p), std::chrono::milliseconds{0});
    }


}

#endif // SOCCER_HAS_TLS
