//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef SOCCER_TLSLISTENER_H
#define SOCCER_TLSLISTENER_H

#ifdef SOCCER_HAS_TLS

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "Coroutines.h"
#include "SocketAddress.h"
#include "TcpStream.h"
#include "TlsStream.h"

struct tls;
struct tls_config;

namespace Soccer {

    /**
     * @class TlsListener
     * @brief TLS-wrapped TCP listener. Reads server cert + key at bind
     *        time; @c accept performs the TLS handshake on each connection.
     */
    class TlsListener final {
    public:
        TlsListener();

        TlsListener(const TlsListener &) = delete;
        TlsListener &operator=(const TlsListener &) = delete;

        TlsListener(TlsListener &&) noexcept;
        TlsListener &operator=(TlsListener &&) noexcept;

        ~TlsListener();

        /**
         * @brief Bind a listening TLS socket on @p host:@p port using the
         *        certificate at @p certPath and private key at @p keyPath.
         */
        static TlsListener bind(const std::string &host,
                                std::uint16_t port,
                                const std::string &certPath,
                                const std::string &keyPath);

        /**
         * @brief Accept the next TCP connection and perform the server-side
         *        TLS handshake.
         */
        YarnBall::Task<TlsStream> accept();

        /**
         * @class PendingHandshake
         * @brief A TCP connection accepted and handed to libtls, handshake not yet
         *        run. Move-only; owns the connection until @ref completeHandshake
         *        consumes it.
         */
        class PendingHandshake final {
        public:
            PendingHandshake() = default;
            PendingHandshake(const PendingHandshake &) = delete;
            PendingHandshake &operator=(const PendingHandshake &) = delete;
            PendingHandshake(PendingHandshake &&o) noexcept;
            PendingHandshake &operator=(PendingHandshake &&o) noexcept;
            ~PendingHandshake();

            /// The accepted socket (for the peer address); -1 when empty.
            int fd() const noexcept { return tcp.fd(); }

        private:
            friend class TlsListener;
            TcpStream tcp;
            ::tls *ctx = nullptr;
        };

        /**
         * @brief Accept the next TCP connection and create its libtls context, without
         *        running the handshake. Run in the accept loop; the handshake is then
         *        driven by @ref completeHandshake in a task of its own, so one peer that
         *        stalls cannot hold up the others.
         */
        YarnBall::Task<PendingHandshake> acceptPending();

        /**
         * @brief Run the server-side handshake of @p p. A handshake not finished after
         *        @p timeout is abandoned: the connection is shut down and a
         *        SocketException is thrown. Zero or negative means no limit. Does not
         *        touch the listener, so it is safe to run while the listener is closed
         *        and rebound (certificate reload).
         */
        static YarnBall::Task<TlsStream> completeHandshake(PendingHandshake p, std::chrono::milliseconds timeout);

        /**
         * @return The local bound address (useful when bound with port 0).
         */
        SocketAddress localAddress() const;

        void close() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}

#endif // SOCCER_HAS_TLS
#endif // SOCCER_TLSLISTENER_H
