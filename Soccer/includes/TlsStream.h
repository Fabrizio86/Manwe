//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef SOCCER_TLSSTREAM_H
#define SOCCER_TLSSTREAM_H

#ifdef SOCCER_HAS_TLS

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "Coroutines.h"

struct tls;
struct tls_config;

namespace Soccer {

    /**
     * @struct TlsClientOptions
     * @brief Knobs for client-side @c TlsStream::connect.
     *
     * The default values produce a normal verifying client. The
     * @c insecure_* flags turn off the corresponding libtls verification
     * step and are intended for development, testing against self-signed
     * certificates, or interoperating with broken trust stores. Do not
     * use them in production.
     *
     * For production-grade clients (verifying a private CA, doing
     * mutual TLS, advertising ALPN protocols), set the corresponding
     * paths / strings below. Empty strings mean "leave libtls's
     * default" (system trust store, no client cert, no ALPN).
     */
    struct TlsClientOptions {
        bool insecure_no_verify_cert = false;
        bool insecure_no_verify_name = false;

        /**
         * @brief Path to a PEM file of CA certificates to verify the
         *        server with. When empty, libtls uses the system
         *        trust store. Mutually exclusive with @ref caBundleDir
         *        (set whichever your deployment uses).
         */
        std::string caBundleFile;

        /**
         * @brief Path to a directory of CA certificates (one per
         *        file, hashed names a-la OpenSSL c_rehash). Same
         *        semantics as @ref caBundleFile but for hashed dirs.
         */
        std::string caBundleDir;

        /**
         * @brief Client certificate chain (PEM) for mutual TLS. When
         *        set, @ref clientKeyFile must also be set. Empty
         *        disables client auth.
         */
        std::string clientCertFile;

        /**
         * @brief Client private key (PEM) matching @ref clientCertFile.
         */
        std::string clientKeyFile;

        /**
         * @brief ALPN protocol list, comma-separated by application-
         *        layer name (e.g. @c "h2,http/1.1"). When empty, no
         *        ALPN extension is sent. Required by HTTP/2 servers
         *        and by some HTTP/1.1 servers that gate based on
         *        ALPN.
         */
        std::string alpnProtocols;
    };

    /**
     * @struct TlsServerOptions
     * @brief Optional knobs for server-side @c TlsListener. Default-
     *        constructed value matches the previous behaviour (no
     *        client-cert verification). Add @ref clientCaFile to
     *        require client certificates signed by a known CA --
     *        the mutual-TLS server side.
     */
    struct TlsServerOptions {
        /**
         * @brief Path to a PEM file of CA certs that signed acceptable
         *        client certs. When non-empty, the server requests a
         *        client cert during the handshake and verifies it
         *        against this CA bundle; clients with no cert or one
         *        signed by an unknown CA are rejected.
         */
        std::string clientCaFile;

        /**
         * @brief ALPN protocol list to advertise. Same format as the
         *        client option.
         */
        std::string alpnProtocols;
    };

    /**
     * @class TlsStream
     * @brief TLS-wrapped TCP connection (client or server side).
     *
     * Uses libtls's public API (@c tls_handshake / @c tls_read / @c tls_write
     * with non-blocking I/O). The handshake and every byte transfer drive
     * a co_await on @c io::waitReadable or @c io::waitWritable whenever
     * libtls returns @c TLS_WANT_POLLIN / @c TLS_WANT_POLLOUT.
     *
     * Move-only. The destructor disposes the libtls context and closes
     * the underlying fd.
     */
    class TlsStream final {
    public:
        TlsStream();

        TlsStream(const TlsStream &) = delete;
        TlsStream &operator=(const TlsStream &) = delete;

        TlsStream(TlsStream &&) noexcept;
        TlsStream &operator=(TlsStream &&) noexcept;

        ~TlsStream();

        /**
         * @brief Connect to @p host:@p port and complete the client-side
         *        TLS handshake.
         *
         * @param opts Optional client options; defaults verify the server
         *             certificate and hostname against the system trust
         *             store.
         */
        static YarnBall::Task<TlsStream> connect(std::string host,
                                                 std::uint16_t port,
                                                 TlsClientOptions opts = {});

        /**
         * @brief Read up to @p buf.size() decrypted bytes.
         * @return Number of bytes read; 0 indicates the TLS half-close.
         */
        YarnBall::Task<std::size_t> read(std::span<std::byte> buf);

        /**
         * @brief Encrypt and write the entire buffer.
         */
        YarnBall::Task<std::size_t> write(std::span<const std::byte> buf);

        void close() noexcept;

        /**
         * @return Underlying socket fd or -1 if closed.
         */
        int fd() const noexcept;

    private:
        friend class TlsListener;

        /**
         * @brief Adopt an accepted server-side context. Used by
         *        @c TlsListener::accept after @c tls_accept_socket.
         */
        TlsStream(::tls *ctx, ::tls_config *cfg, int fd) noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}

#endif // SOCCER_HAS_TLS
#endif // SOCCER_TLSSTREAM_H
