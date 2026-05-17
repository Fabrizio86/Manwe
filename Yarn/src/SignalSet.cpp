//
// Created by Fabrizio Paino on 2026-05-16.
//

#include "SignalSet.h"

#include <atomic>
#include <cerrno>
#include <cstring>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

#include "IoAwaiters.h"

namespace YarnBall {

#if !defined(_WIN32)

    namespace {
        /**
         * @brief Process-wide write fd for the singleton SignalSet's
         *        self-pipe. -1 when no SignalSet is active.
         *
         * Loaded by the signal handler (async-signal-safe: a plain
         * atomic load), set/cleared by SignalSet's lifecycle.
         */
        std::atomic<int> g_signalWritePipe{-1};

        /**
         * @brief @c sigaction body installed for every captured signal.
         *        Only @c write(2) is allowed in async-signal-safe code;
         *        we emit one byte holding the signal number and return.
         *        The reader side decodes the byte back into a signal
         *        number.
         */
        extern "C" void manwe_signal_handler(int sig) {
            const int fd = g_signalWritePipe.load(std::memory_order_acquire);
            if (fd < 0) return;
            const unsigned char b = static_cast<unsigned char>(sig);
            // Async-signal-safe; ignore partial / EAGAIN — losing one
            // notification under extreme pressure is acceptable.
            (void) ::write(fd, &b, 1);
        }
    }

    SignalSet::SignalSet(std::initializer_list<int> signals) {
        int fds[2];
        if (::pipe(fds) < 0) {
            throw SignalCaptureError(std::string("pipe: ") + std::strerror(errno));
        }
        this->pipeRead = fds[0];
        this->pipeWrite = fds[1];

        // Non-blocking so the reader can poll cleanly and the handler's
        // write does not stall if the pipe fills up.
        for (int fd : {this->pipeRead, this->pipeWrite}) {
            int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                const int e = errno;
                ::close(this->pipeRead);
                ::close(this->pipeWrite);
                this->pipeRead = this->pipeWrite = -1;
                throw SignalCaptureError(std::string("fcntl(O_NONBLOCK): ") +
                                          std::strerror(e));
            }
        }

        // Singleton claim. CAS from -1 to our write fd; failure means
        // another SignalSet is alive.
        int expected = -1;
        if (!g_signalWritePipe.compare_exchange_strong(expected, this->pipeWrite,
                                                       std::memory_order_acq_rel)) {
            ::close(this->pipeRead);
            ::close(this->pipeWrite);
            this->pipeRead = this->pipeWrite = -1;
            throw SignalCaptureError(
                "another SignalSet is already active in this process");
        }

        struct sigaction newAction{};
        newAction.sa_handler = &manwe_signal_handler;
        sigemptyset(&newAction.sa_mask);
        // SA_RESTART so syscalls in user code don't get cut short by
        // our handler returning EINTR.
        newAction.sa_flags = SA_RESTART;

        for (int sig : signals) {
            struct sigaction prev{};
            if (::sigaction(sig, &newAction, &prev) < 0) {
                const int e = errno;
                // Roll back: restore any handlers we already replaced,
                // release the singleton claim, close the pipe.
                for (std::size_t i = 0; i < this->installedSignals.size(); ++i) {
                    (void) ::sigaction(this->installedSignals[i],
                                       &this->originalActions[i], nullptr);
                }
                g_signalWritePipe.store(-1, std::memory_order_release);
                ::close(this->pipeRead);
                ::close(this->pipeWrite);
                this->pipeRead = this->pipeWrite = -1;
                throw SignalCaptureError(std::string("sigaction(")
                                          + std::to_string(sig) + "): "
                                          + std::strerror(e));
            }
            this->installedSignals.push_back(sig);
            this->originalActions.push_back(prev);
        }
    }

    SignalSet::~SignalSet() {
        for (std::size_t i = 0; i < this->installedSignals.size(); ++i) {
            (void) ::sigaction(this->installedSignals[i],
                                &this->originalActions[i], nullptr);
        }
        // Release the singleton claim BEFORE closing pipeWrite so a
        // racing signal handler observing the old fd would write to
        // a still-open descriptor.
        g_signalWritePipe.store(-1, std::memory_order_release);
        if (this->pipeWrite >= 0) ::close(this->pipeWrite);
        if (this->pipeRead >= 0) ::close(this->pipeRead);
    }

    YarnBall::Task<int> SignalSet::next() {
        while (true) {
            unsigned char b = 0;
            ::ssize_t n;
            do {
                n = ::read(this->pipeRead, &b, 1);
            } while (n < 0 && errno == EINTR);

            if (n == 1) co_return static_cast<int>(b);

            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                co_await YarnBall::io::waitReadable(this->pipeRead);
                continue;
            }
            if (n == 0) {
                throw SignalCaptureError("signal pipe closed");
            }
            throw SignalCaptureError(std::string("read(signal pipe): ") +
                                      std::strerror(errno));
        }
    }

#else // _WIN32

    SignalSet::SignalSet(std::initializer_list<int>) {
        throw SignalCaptureError(
            "SignalSet on Windows is not implemented in this round; "
            "use SetConsoleCtrlHandler directly until the IOCP-backed "
            "signal capture path lands.");
    }

    SignalSet::~SignalSet() = default;

    YarnBall::Task<int> SignalSet::next() {
        throw SignalCaptureError("SignalSet on Windows is not implemented");
        co_return 0; // unreachable, satisfies the coroutine machinery
    }

#endif

}
