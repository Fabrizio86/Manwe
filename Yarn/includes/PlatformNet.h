//
// Created by Fabrizio Paino on 2026-05-15.
//
// Cross-platform socket portability shim shared between the Yarn Reactor
// and the Soccer networking primitives. Centralises the #ifdef _WIN32
// dance for sockets so the rest of the codebase can issue syscalls without
// per-call platform branches.
//
// Provides:
//   - Platform socket headers (winsock2.h on Windows, POSIX BSD sockets
//     elsewhere) with the conventional WIN32_LEAN_AND_MEAN / NOMINMAX
//     guards in place.
//   - A global @c ssize_t alias on Windows (mirroring @c SSIZE_T) so
//     recv/send/recvfrom return values can be stored uniformly.
//   - Idempotent WSAStartup via @ref ensureWsaStarted.
//   - Tiny inline helpers for the syscalls whose names or error reporting
//     differ between Berkeley sockets and WinSock (close/closesocket,
//     errno/WSAGetLastError, EAGAIN/WSAEWOULDBLOCK, EINTR retry, set
//     non-blocking mode).
//

#ifndef YARN_PLATFORM_NET_H
#define YARN_PLATFORM_NET_H

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <mswsock.h>
    #include <basetsd.h>
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <cerrno>
#endif

#ifdef _WIN32
// MSVC's UCRT does not provide the POSIX @c ssize_t. Soccer + Yarn both
// store recv/send return values in @c ssize_t locals, so alias it once at
// global scope when this shim is included. Guarded so we don't collide
// with mingw / clang-cl headers that already define it.
    #if !defined(_SSIZE_T_DEFINED) && !defined(__ssize_t_defined)
        using ssize_t = SSIZE_T;
        #define _SSIZE_T_DEFINED
    #endif
#endif

namespace YarnBall {

    /**
     * @brief Idempotent WSAStartup. POSIX no-op.
     *
     * Implemented as a Meyer's-singleton: the first call from any thread
     * performs @c WSAStartup once for the process; subsequent calls do
     * nothing. There is intentionally no matching @c WSACleanup --
     * Windows tears the WinSock subsystem down at process exit, which
     * keeps init order independent of static-destructor ordering.
     *
     * Soccer's bind/connect/resolve paths and Yarn's WSAPoll-based
     * Reactor both call this on first use.
     */
    inline void ensureWsaStarted() noexcept {
#ifdef _WIN32
        static const int rc = []() noexcept {
            WSADATA wsa{};
            return ::WSAStartup(MAKEWORD(2, 2), &wsa);
        }();
        (void) rc;
#endif
    }

    /**
     * @brief Last error value from a socket syscall.
     *        @c errno on POSIX, @c WSAGetLastError() on Windows.
     */
    inline int lastSocketError() noexcept {
#ifdef _WIN32
        return ::WSAGetLastError();
#else
        return errno;
#endif
    }

    /**
     * @brief @c true if @p err means "the operation would have blocked".
     *        Maps @c EAGAIN/EWOULDBLOCK on POSIX, @c WSAEWOULDBLOCK on
     *        Windows.
     */
    inline bool isWouldBlock(int err) noexcept {
#ifdef _WIN32
        return err == WSAEWOULDBLOCK;
#else
        return err == EAGAIN || err == EWOULDBLOCK;
#endif
    }

    /**
     * @brief @c true if a non-blocking @c connect is in-flight (i.e. the
     *        caller should wait for writability and then check
     *        @c SO_ERROR).
     */
    inline bool isInProgress(int err) noexcept {
#ifdef _WIN32
        // WinSock returns WSAEWOULDBLOCK for an in-flight non-blocking
        // connect. WSAEINPROGRESS is the lock-held variant from the
        // 16-bit days; modern stacks rarely raise it but accept it
        // defensively.
        return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
        return err == EINPROGRESS;
#endif
    }

    /**
     * @brief @c true if @p err is the EINTR-style "interrupted by signal"
     *        case that justifies a retry. WinSock does not surface
     *        EINTR, so this is always @c false on Windows.
     */
    inline bool isEintr(int err) noexcept {
#ifdef _WIN32
        (void) err;
        return false;
#else
        return err == EINTR;
#endif
    }

    /**
     * @brief Close a socket. @c close on POSIX, @c closesocket on Windows.
     *
     * @p fd is taken as @c int for consistency with the rest of the API,
     * which keeps file/socket descriptors as ints across platforms. The
     * cast to @c SOCKET on Windows is safe in practice because WinSock
     * SOCKET handles allocated by user code fit in the 32-bit positive
     * range; this matches how mio and other portable runtimes plumb
     * sockets internally.
     */
    inline int closeSocket(int fd) noexcept {
#ifdef _WIN32
        return ::closesocket(static_cast<SOCKET>(static_cast<unsigned>(fd)));
#else
        return ::close(fd);
#endif
    }

    /**
     * @brief Put @p fd into non-blocking mode.
     *
     * POSIX: @c fcntl(F_SETFL, O_NONBLOCK).
     * Windows: @c ioctlsocket(FIONBIO, 1).
     *
     * @return 0 on success; -1 on failure (with the platform error code
     *         retrievable via @ref lastSocketError).
     */
    inline int setSocketNonblocking(int fd) noexcept {
#ifdef _WIN32
        u_long mode = 1;
        return ::ioctlsocket(static_cast<SOCKET>(static_cast<unsigned>(fd)),
                             FIONBIO, &mode) == 0 ? 0 : -1;
#else
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0) return -1;
        return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
#endif
    }

}

#endif // YARN_PLATFORM_NET_H
