//
// Created by Fabrizio Paino on 2026-05-15.
//
// tcpServe: accept-loop + per-connection coSpawn helper, the canonical
// "TCP server" idiom in coroutine form. Caller supplies a handler that
// returns Task<void>(TcpStream); tcpServe loops accept, spawns the
// handler for each, and never blocks on a slow handler.
//
// Cancellation: pass a std::stop_token. tcpServe registers a
// std::stop_callback that closes the listener fd on stop request,
// which cancels any in-flight accept (kernel returns EBADF). The
// resulting SocketException is swallowed if stop_requested() is
// true, so the loop exits cleanly even when blocked in accept.
// In-flight handlers keep running until they complete.
//

#ifndef SOCCER_TCP_SERVER_H
#define SOCCER_TCP_SERVER_H

#include <cstring>
#include <functional>
#include <stop_token>
#include <utility>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "Coroutines.h"
#include "SocketException.h"
#include "TcpListener.h"
#include "TcpStream.h"
#include "YarnBall.hpp"

namespace Soccer {

    /**
     * @brief Accept-loop + per-connection spawn. For each accepted
     *        connection, invokes @p handler(std::move(stream)) and
     *        @c coSpawn s the returned task on the Yarn pool.
     *
     * @param listener Bound @c TcpListener. Moved into the coroutine
     *                  frame so its lifetime tracks the loop.
     * @param handler  Per-connection coroutine factory. Called once per
     *                  accepted stream. Returning a @c Task<void> is
     *                  what makes the spawn ergonomic; the handler can
     *                  internally co_await other tasks freely.
     * @param stop     Cooperative stop token. When stopped, a
     *                  @c stop_callback closes the listener fd to
     *                  unblock any pending @c accept; the resulting
     *                  @c SocketException is consumed silently and
     *                  the loop returns.
     *
     * @note In-flight handlers are NOT cancelled when @p stop fires --
     *       C++ coroutines have no forced cancellation. If you want
     *       handlers to shorten, plumb @p stop through to them and
     *       have them check it cooperatively.
     */
    inline YarnBall::Task<void>
    tcpServe(TcpListener listener,
              std::function<YarnBall::Task<void>(TcpStream)> handler,
              std::stop_token stop = {}) {
        // Unblock a pending accept on stop: closing or shutting down
        // the listener fd does NOT reliably wake a kqueue-suspended
        // accept on macOS (the kernel drops the filter silently
        // rather than synthesising an event). The portable way is
        // to "knock on the door" -- briefly open a loopback
        // connection to the listener's own bound address, which
        // forces accept to return one final stream. The loop body
        // then sees stop_requested() and returns; the probe stream
        // is discarded by the handler when it sees an immediate EOF.
        std::stop_callback unblock(stop, [&listener]() noexcept {
            SocketAddress local;
            try {
                local = listener.localAddress();
            } catch (...) {
                return;
            }
            sockaddr_storage dest{};
            const socklen_t destLen = local.length();
            std::memcpy(&dest, local.data(), local.length());

            // Rewrite ANY -> loopback so we hit our own listener.
            if (local.family() == AF_INET) {
                auto *in = reinterpret_cast<sockaddr_in *>(&dest);
                if (in->sin_addr.s_addr == htonl(INADDR_ANY)) {
                    in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                }
            } else if (local.family() == AF_INET6) {
                auto *in6 = reinterpret_cast<sockaddr_in6 *>(&dest);
                static const in6_addr loopback6 = IN6ADDR_LOOPBACK_INIT;
                static const in6_addr any6 = IN6ADDR_ANY_INIT;
                if (std::memcmp(&in6->sin6_addr, &any6, sizeof(any6)) == 0) {
                    in6->sin6_addr = loopback6;
                }
            }

            const int probe = static_cast<int>(::socket(local.family(),
                                                          SOCK_STREAM, 0));
            if (probe < 0) return;
            (void) ::connect(probe,
                              reinterpret_cast<sockaddr *>(&dest), destLen);
            (void) ::close(probe);
        });
        while (!stop.stop_requested()) {
            try {
                TcpStream client = co_await listener.accept();
                YarnBall::coSpawn(handler(std::move(client)));
            } catch (const SocketException &) {
                if (stop.stop_requested()) co_return;
                throw;
            }
        }
        co_return;
    }

}

#endif // SOCCER_TCP_SERVER_H
