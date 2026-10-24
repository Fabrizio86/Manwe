//
// Created by Fabrizio Paino on 2026-05-15.
//

#include "TcpListener.h"

#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#else
#include <afunix.h>
#endif

#include "IoAwaiters.h"
#include "Reactor.h"
#include "NetUtil.h"
#include "PlatformNet.h"
#include "SocketException.h"

namespace Soccer {

    namespace {
        /**
         * @brief Put @p fd into non-blocking mode. Throws on failure.
         */
        void set_nonblocking(int fd) {
            if (YarnBall::setSocketNonblocking(fd) != 0) {
                throw SocketException("set_nonblocking", YarnBall::lastSocketError());
            }
        }
    }

    TcpListener TcpListener::bind(const std::string &host, std::uint16_t port, bool dualStack) {
        YarnBall::ensureWsaStarted();
        const auto addr = SocketAddress::resolve(host, port);

        const int fd = detail::openSocket(addr.family(), SOCK_STREAM, 0);
        if (fd < 0) throw SocketException("socket", YarnBall::lastSocketError());

        // SO_REUSEADDR has different semantics on Windows than on POSIX
        // (BSD-style "rebind same port" rather than "ignore TIME_WAIT"),
        // but for a transient bind during testing this is what we want
        // either way. The cast to char* is required by the WinSock prototype.
#ifdef _WIN32
        // SO_REUSEADDR on Windows lets another process bind the same port.
        const int yes = 1;
        const int reuseOpt = SO_EXCLUSIVEADDRUSE;
#else
        const int yes = 1;
        const int reuseOpt = SO_REUSEADDR;
#endif
        if (::setsockopt(fd, SOL_SOCKET, reuseOpt,
                         reinterpret_cast<const char *>(&yes), sizeof(yes)) < 0) {
            const int e = YarnBall::lastSocketError();
            (void) YarnBall::closeSocket(fd);
            throw SocketException("setsockopt(SO_REUSEADDR)", e);
        }

        if (addr.family() == AF_INET6) {
            // Explicit, so "::" means the same on every OS (the defaults differ).
            const int v6only = dualStack ? 0 : 1;
            if (::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                             reinterpret_cast<const char *>(&v6only), sizeof(v6only)) < 0) {
                const int e = YarnBall::lastSocketError();
                (void) YarnBall::closeSocket(fd);
                throw SocketException("setsockopt(IPV6_V6ONLY)", e);
            }
        }

        if (::bind(fd, addr.data(), addr.length()) < 0) {
            const int e = YarnBall::lastSocketError();
            (void) YarnBall::closeSocket(fd);
            throw SocketException("bind", e);
        }

        if (::listen(fd, SOMAXCONN) < 0) {
            const int e = YarnBall::lastSocketError();
            (void) YarnBall::closeSocket(fd);
            throw SocketException("listen", e);
        }

        try {
            set_nonblocking(fd);
        } catch (...) {
            (void) YarnBall::closeSocket(fd);
            throw;
        }

        return TcpListener(fd);
    }

    namespace {
#ifndef _WIN32
        /**
         * @brief Probe whether a server may still own the Unix socket at @p addr.
         *        A non-blocking connect completes (or is refused) immediately for
         *        this family. Only "no such file" and "refused" mean nobody is there;
         *        EAGAIN / EINPROGRESS (a Linux server with a full backlog) and any
         *        other outcome -- notably EACCES / EPERM, a socket of another user --
         *        count as live, so we never unlink what we cannot prove is dead. On
         *        macOS and the BSDs a server with a full backlog also answers
         *        ECONNREFUSED; @ref lockSidecar is what protects such a server.
         */
        bool unixSocketIsLive(const sockaddr_un &addr, socklen_t addrLen) {
            const int probe = detail::openSocket(AF_UNIX, SOCK_STREAM, 0);
            if (probe < 0) return true; // cannot tell: do not take the path over
            bool live = true;
            if (YarnBall::setSocketNonblocking(probe) == 0) {
                if (::connect(probe, reinterpret_cast<const sockaddr *>(&addr), addrLen) == 0) {
                    live = true;
                } else {
                    const int e = errno;
                    live = !(e == ENOENT || e == ECONNREFUSED);
                }
            }
            (void) ::close(probe);
            return live;
        }

        /**
         * @brief Take an exclusive advisory lock on "<path>.lock" for as long as the
         *        returned descriptor stays open. A listener bound through bindUnix holds
         *        it, so a second bindUnix on the same path can tell a live (even
         *        saturated) server from a stale socket file without relying on connect.
         * @param[out] held Set when another holder has the lock.
         * @return The locked descriptor, or -1 (held, or the lock file cannot be made --
         *         then only the connect probe protects the path).
         */
        int lockSidecar(const std::string &socketPath, bool &held) {
            held = false;
            const std::string lockPath = socketPath + ".lock";
            const int lf = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            if (lf < 0) return -1;
            if (::flock(lf, LOCK_EX | LOCK_NB) != 0) {
                held = (errno == EWOULDBLOCK);
                (void) ::close(lf);
                return -1;
            }
            return lf;
        }

        /// Closes a descriptor on scope exit unless released.
        struct FdGuard {
            int fd;
            explicit FdGuard(int f) noexcept : fd(f) {}
            FdGuard(const FdGuard &) = delete;
            FdGuard &operator=(const FdGuard &) = delete;
            ~FdGuard() {
                if (fd >= 0) (void) ::close(fd);
            }
            int release() noexcept {
                const int f = fd;
                fd = -1;
                return f;
            }
        };

#endif
    }

    TcpListener TcpListener::bindUnix(const std::string &socketPath) {
        YarnBall::ensureWsaStarted();

        // Build (and validate) the address before creating the socket
        // so a bad path cannot leak an fd.
        sockaddr_un addr{};
        const socklen_t addrLen = detail::makeUnixAddr(&addr, socketPath);

        const int fd = detail::openSocket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) throw SocketException("socket(AF_UNIX)", YarnBall::lastSocketError());

#ifndef _WIN32
        int lockFd = -1;
        // A stale socket file from a crashed run is removed so the bind can succeed;
        // a socket a live server still answers on is not. ENOENT is fine (no prior
        // socket); anything that is not a socket (regular file, directory) is left
        // alone and bind() fails with the canonical EADDRINUSE.
        try {
            bool held = false;
            lockFd = lockSidecar(socketPath, held);
            if (held) {
                throw SocketException("bindUnix: a server is already listening on " + socketPath, EADDRINUSE);
            }
            struct stat st{};
            if (::lstat(socketPath.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
                if (unixSocketIsLive(addr, addrLen)) {
                    throw SocketException("bindUnix: a server is already listening on " + socketPath, EADDRINUSE);
                }
                // Remove it only if it is still the node we looked at.
                struct stat again{};
                if (::lstat(socketPath.c_str(), &again) == 0 && again.st_ino == st.st_ino &&
                    again.st_dev == st.st_dev) {
                    (void) ::unlink(socketPath.c_str());
                }
            }
            // Owner-only, without touching the process-wide umask. The node is created with
            // the default mode, but a socket that has not called listen() yet refuses every
            // connect, so nobody can use it before the chmod below.
            if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), addrLen) < 0) {
                throw SocketException("bind(AF_UNIX)", YarnBall::lastSocketError());
            }
            if (::chmod(socketPath.c_str(), 0600) != 0) {
                const int ce = errno;
                (void) ::unlink(socketPath.c_str());
                throw SocketException("chmod(AF_UNIX)", ce);
            }
        } catch (...) {
            const int saved = errno;
            (void) YarnBall::closeSocket(fd);
            if (lockFd >= 0) (void) ::close(lockFd);
            errno = saved;
            throw;
        }
        FdGuard lockGuard(lockFd);
#else
        if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), addrLen) < 0) {
            const int e = YarnBall::lastSocketError();
            (void) YarnBall::closeSocket(fd);
            throw SocketException("bind(AF_UNIX)", e);
        }
#endif

        if (::listen(fd, SOMAXCONN) < 0) {
            const int e = YarnBall::lastSocketError();
            (void) YarnBall::closeSocket(fd);
            throw SocketException("listen(AF_UNIX)", e);
        }

        try {
            set_nonblocking(fd);
        } catch (...) {
            (void) YarnBall::closeSocket(fd);
            throw;
        }

        TcpListener l(fd);
#ifndef _WIN32
        l.lockFd = lockGuard.release();
#endif
        return l;
    }

    namespace {
        /// One accept(2) on @p fd (retried on EINTR). @return the new descriptor, or -1 with @p err set.
        int acceptOnce(int fd, SocketAddress &peer, int &err) noexcept {
            int client;
            do {
                peer.lengthMut() = sizeof(sockaddr_storage);
#if defined(__linux__)
                client = ::accept4(fd, peer.dataMut(), &peer.lengthMut(), SOCK_CLOEXEC);
#else
                client = static_cast<int>(::accept(fd, peer.dataMut(), &peer.lengthMut()));
#endif
            } while (client < 0 && YarnBall::isEintr(YarnBall::lastSocketError()));
            if (client < 0) {
                err = YarnBall::lastSocketError();
                return -1;
            }
#if !defined(__linux__)
            detail::setCloexec(client);
#endif
            return client;
        }
    }

    YarnBall::Task<TcpStream> TcpListener::accept(SocketAddress *out) {
        while (true) {
            // Re-checked on every pass: close() wakes a parked accept, which must then fail.
            // The Use is held across the wait so the descriptor cannot be closed (and its
            // number reused) between the EAGAIN and the registration.
            auto use = this->sock.begin();
            const int fd = use.fd();
            if (fd < 0) {
                throw SocketException("accept on closed TcpListener");
            }
            if (this->sock.interrupted()) throw SocketException("accept interrupted");
            SocketAddress peer;
            int err = 0;
            const int client = acceptOnce(fd, peer, err);

            if (client >= 0) {
                // A blocking client socket would stall a Yarn worker in
                // recv/send, so drop the connection if this fails.
                if (YarnBall::setSocketNonblocking(client) != 0) {
                    (void) YarnBall::closeSocket(client);
                    continue;
                }
                if (out) *out = peer;
                co_return TcpStream(client);
            }

            if (YarnBall::isWouldBlock(err)) {
                co_await detail::waitReadable(use);
                continue;
            }
            throw SocketException("accept", err);
        }
    }

    std::optional<TcpStream> TcpListener::tryAccept(SocketAddress *out) {
        while (true) {
            auto use = this->sock.begin();
            const int fd = use.fd();
            if (fd < 0) {
                throw SocketException("accept on closed TcpListener");
            }
            SocketAddress peer;
            int err = 0;
            const int client = acceptOnce(fd, peer, err);
            if (client < 0) {
                if (YarnBall::isWouldBlock(err)) return std::nullopt;
                throw SocketException("accept", err);
            }
            if (YarnBall::setSocketNonblocking(client) != 0) {
                (void) YarnBall::closeSocket(client);
                continue;
            }
            if (out) *out = peer;
            return std::optional<TcpStream>(std::in_place, client);
        }
    }

    SocketAddress TcpListener::localAddress() const {
        SocketAddress addr;
        addr.lengthMut() = sizeof(sockaddr_storage);
        auto use = this->sock.begin();
        if (use.fd() < 0) {
            throw SocketException("localAddress on closed TcpListener");
        }
        if (::getsockname(use.fd(), addr.dataMut(), &addr.lengthMut()) < 0) {
            throw SocketException("getsockname", YarnBall::lastSocketError());
        }
        return addr;
    }

    bool TcpListener::interruptAccept() noexcept {
#ifndef _WIN32
        auto use = this->sock.begin();
        if (use.fd() < 0) return false;
        // The flag makes a parked accept fail when it is woken, and a wait that registers after this point wakes
        // itself (see SlotWait), so no wakeup is lost; the shutdown on Linux only makes the fd readable as well.
        this->sock.interrupt();
#if defined(__linux__)
        (void) ::shutdown(use.fd(), SHUT_RD);
#endif
        (void) YarnBall::Reactor::instance()->cancelFd(use.fd());
        return true;
#else
        return false;
#endif
    }

    void TcpListener::close() noexcept {
        // Taken out of the slot first, so an accept() woken below re-checks it and fails
        // instead of calling accept on a descriptor number that may be reused.
        const int fd = this->sock.take();
#ifndef _WIN32
        if (this->lockFd >= 0) {
            (void) ::close(this->lockFd); // releases the bindUnix path lock
            this->lockFd = -1;
        }
#endif
        if (fd >= 0) {
#if defined(__linux__)
            // Closing alone leaves an accept() parked on this fd asleep. On Linux a
            // shutdown makes the fd readable, so the pending accept wakes and fails.
            // (This is also why fork() without exec is unsupported: see the class comment.)
            (void) ::shutdown(fd, SHUT_RD);
#endif
#ifndef _WIN32
            // Wake any coroutine still parked on this fd before it is closed (and the number reused).
            (void) YarnBall::Reactor::instance()->cancelFd(fd);
#endif
            this->sock.retire(fd);
        }
    }

}
