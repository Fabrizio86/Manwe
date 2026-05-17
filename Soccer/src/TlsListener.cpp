//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifdef SOCCER_HAS_TLS

#include "TlsListener.h"

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <tls.h>

#include "IoAwaiters.h"
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

    YarnBall::Task<TlsStream> TlsListener::accept() {
        if (!this->impl || !this->impl->server_ctx) {
            throw SocketException("accept on closed TlsListener");
        }

        // Accept the underlying TCP connection.
        TcpStream tcp_client = co_await this->impl->tcp.accept();
        const int client_fd = tcp_client.fd();

        // Hand the fd to libtls; it will drive the server-side handshake.
        ::tls *client_ctx = nullptr;
        if (::tls_accept_socket(this->impl->server_ctx, &client_ctx, client_fd) != 0) {
            const std::string err =
                ::tls_error(this->impl->server_ctx)
                    ? ::tls_error(this->impl->server_ctx)
                    : "tls_accept_socket";
            throw SocketException(err);
        }

        // Drive the handshake.
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
            const std::string err =
                ::tls_error(client_ctx) ? ::tls_error(client_ctx) : "tls_handshake";
            ::tls_free(client_ctx);
            throw SocketException(err);
        }

        // Transfer fd ownership from TcpStream to TlsStream. release() leaves
        // tcp_client empty so its destructor won't close the fd.
        const int adopted_fd = tcp_client.release();
        co_return TlsStream(client_ctx, /*cfg=*/nullptr, adopted_fd);
    }

}

#endif // SOCCER_HAS_TLS
