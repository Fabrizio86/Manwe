//
// The native socket / descriptor handle type of the platform, shared by the
// Reactor and the PlatformNet helpers. Kept free of platform headers so that
// Reactor.h does not have to pull in <winsock2.h>.
//

#ifndef YARN_SOCKET_HANDLE_H
#define YARN_SOCKET_HANDLE_H

#include <cstdint>

namespace YarnBall {

#ifdef _WIN32
    /**
     * @brief A Windows @c SOCKET (an unsigned pointer-sized integer; 64 bits
     *        on Win64, where it must not be squeezed through an @c int).
     */
    using SocketHandle = std::uintptr_t;

    /// The "no socket" value (@c INVALID_SOCKET).
    inline constexpr SocketHandle kInvalidSocket = ~static_cast<SocketHandle>(0);

    constexpr bool isValidSocket(SocketHandle s) noexcept { return s != kInvalidSocket; }
#else
    /**
     * @brief A POSIX file descriptor.
     */
    using SocketHandle = int;

    /// The "no socket" value.
    inline constexpr SocketHandle kInvalidSocket = -1;

    constexpr bool isValidSocket(SocketHandle s) noexcept { return s >= 0; }
#endif

}

#endif // YARN_SOCKET_HANDLE_H
