//
// Created by Fabrizio Paino on 2026-05-16.
//
// SignalSet -- coroutine-driven capture of POSIX signals (SIGINT,
// SIGTERM, ...). The canonical use case is graceful shutdown:
//
//     YarnBall::SignalSet signals({SIGINT, SIGTERM});
//     int sig = co_await signals.next();
//     // start shutdown ...
//
// Implementation: a process-wide self-pipe handshake.
//  - Constructor installs a small @c sigaction handler for each
//    requested signal. The handler is async-signal-safe: it only
//    issues @c write(2) of one byte (the signal number) to a
//    non-blocking pipe.
//  - The read end of the pipe is registered with the Reactor via
//    @c YarnBall::io::waitReadable, so the coroutine suspends
//    without spinning and resumes on the Yarn pool when a signal
//    arrives.
//  - Destructor restores the original handlers and closes the pipe.
//
// One SignalSet per process: the implementation enforces this with
// a CAS on a global pipe-write file descriptor and throws if a
// second SignalSet is constructed while another is still alive.
// This matches the realistic use case (graceful shutdown
// initialised once at startup) and avoids signal-routing ambiguity.
//

#ifndef YARN_SIGNAL_SET_H
#define YARN_SIGNAL_SET_H

#include <initializer_list>
#include <stdexcept>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#endif

#include "Coroutines.h"

namespace YarnBall {

    /**
     * @class SignalCaptureError
     * @brief Thrown by @c SignalSet on installation or read failure.
     *        Distinct from other Yarn error types so callers can catch
     *        signal-specific failures without swallowing IO errors.
     */
    class SignalCaptureError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /**
     * @class SignalSet
     * @brief Capture POSIX signals as coroutine-awaitable events.
     *
     * Move-only by design — the pipe + global handler state is bound
     * to a single instance. Concurrent construction of two SignalSet
     * objects in the same process throws @c SignalCaptureError on
     * the second one until the first is destroyed.
     *
     * Windows: not implemented in this round. Construction throws
     * @c SignalCaptureError on Windows; @c SetConsoleCtrlHandler
     * integration is a future addition.
     */
    class SignalSet final {
    public:
        /**
         * @brief Install signal handlers for each entry in @p signals.
         *        Throws @c SignalCaptureError if another SignalSet is
         *        already active in this process, or if any underlying
         *        syscall (pipe, sigaction) fails.
         *
         * The previous @c sigaction for each signal is captured and
         * restored by the destructor, so this composes with caller
         * code that already installs its own handlers (e.g. a debug
         * crash handler).
         */
        explicit SignalSet(std::initializer_list<int> signals);

        SignalSet(const SignalSet &) = delete;
        SignalSet &operator=(const SignalSet &) = delete;
        SignalSet(SignalSet &&) = delete;
        SignalSet &operator=(SignalSet &&) = delete;

        /**
         * @brief Restore original signal handlers and close the pipe.
         */
        ~SignalSet();

        /**
         * @brief Await the next captured signal. Returns the signal
         *        number (one of those passed to the constructor).
         *
         * Multiple coroutines may not @c co_await the same SignalSet
         * concurrently — there is one pipe and one byte per signal,
         * so contending readers would split the byte stream. Wrap
         * the await in a single dispatcher coroutine if you need
         * fan-out.
         *
         * @throws SignalCaptureError on a fatal pipe read error.
         */
        YarnBall::Task<int> next();

    private:
#if !defined(_WIN32)
        /** Read end of the self-pipe. Registered with the Reactor. */
        int pipeRead = -1;
        /** Write end of the self-pipe; held only for ordered cleanup. */
        int pipeWrite = -1;
        /** Signals we installed handlers for (in install order). */
        std::vector<int> installedSignals;
        /** Original @c sigaction structs for each installed signal. */
        std::vector<struct sigaction> originalActions;
#endif
    };

}

#endif // YARN_SIGNAL_SET_H
