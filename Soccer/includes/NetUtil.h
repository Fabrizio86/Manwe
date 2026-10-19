//
// Small socket helpers shared by the Soccer sources: close-on-exec, and a
// portable full shutdown used to wake a coroutine parked on a socket.
//

#ifndef SOCCER_NETUTIL_H
#define SOCCER_NETUTIL_H

#include "PlatformNet.h"

namespace Soccer {

    namespace detail {

        /**
         * @brief Mark @p fd close-on-exec so a fork+exec elsewhere in the process
         *        does not leak the socket into the child. No-op on Windows, where
         *        sockets are not inherited unless a handle-inheriting
         *        CreateProcess asks for it.
         */
        inline void setCloexec(int fd) noexcept {
#ifndef _WIN32
            const int flags = ::fcntl(fd, F_GETFD, 0);
            if (flags >= 0) (void) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
#else
            (void) fd;
#endif
        }

        /**
         * @brief @c ::socket with close-on-exec set atomically where the platform
         *        supports @c SOCK_CLOEXEC (Linux, the BSDs), and right after
         *        creation elsewhere (macOS).
         * @return The new fd, or -1 with the platform error code set.
         */
        inline int openSocket(int family, int type, int protocol = 0) noexcept {
#if !defined(_WIN32) && defined(SOCK_CLOEXEC)
            return static_cast<int>(::socket(family, type | SOCK_CLOEXEC, protocol));
#else
            const int fd = static_cast<int>(::socket(family, type, protocol));
            if (fd >= 0) setCloexec(fd);
            return fd;
#endif
        }

        /**
         * @brief Shut both directions of @p fd down without closing it. Parked
         *        reads and writes on the socket wake up (EOF / error), and the fd
         *        number stays reserved until its owner closes it.
         */
        inline void shutdownSocket(int fd) noexcept {
#ifdef _WIN32
            (void) ::shutdown(static_cast<SOCKET>(static_cast<unsigned>(fd)), SD_BOTH);
#else
            (void) ::shutdown(fd, SHUT_RDWR);
#endif
        }

    }

}

#endif // SOCCER_NETUTIL_H
