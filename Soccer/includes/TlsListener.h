//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef SOCCER_TLSLISTENER_H
#define SOCCER_TLSLISTENER_H

#ifdef SOCCER_HAS_TLS

#include <cstdint>
#include <memory>
#include <string>

#include "Coroutines.h"
#include "SocketAddress.h"
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
