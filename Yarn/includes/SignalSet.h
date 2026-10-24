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
// Implementation:
//  - kqueue platforms (macOS, BSD): each SignalSet owns a kqueue with
//    one @c EVFILT_SIGNAL filter per requested signal. The signal's
//    disposition is set to an empty handler (kqueue reports the signal
//    regardless, and an installed handler keeps the default action from
//    killing the process) and restored when the last set using it is
//    destroyed. SIG_IGN is deliberately not used: it would make SIGCHLD
//    auto-reap children (waitpid fails with ECHILD) and would be
//    inherited across @c exec. The kqueue descriptor is registered with
//    the Reactor via @c YarnBall::io::waitReadable, so the coroutine
//    suspends without spinning. The handler does nothing, so nothing here
//    has to be async-signal-safe. Signals of the same number that arrive
//    before @c next() runs are coalesced into one event.
//  - Other POSIX platforms (Linux): a small async-signal-safe
//    @c sigaction handler writes the signal number to every live set's
//    non-blocking self-pipe, whose read end is registered with the
//    Reactor. (@c signalfd is not used: it needs the signal blocked in
//    every thread, which a library cannot arrange for threads it does
//    not own.) A handler is reset to the default action by @c exec, so
//    children do not inherit anything, and a signal raised in a forked child
//    that has not exec'd is not forwarded to the parent's sets (the handler
//    compares the process id recorded when the set was created); the self-pipe holds a
//    pipe's worth of pending signals (one byte each) and drops the excess,
//    where the kqueue build coalesces repeats of the same signal.
//
// Any number of SignalSets may be alive at once; each one receives
// every signal it asked for.
//

#ifndef YARN_SIGNAL_SET_H
#define YARN_SIGNAL_SET_H

#include <initializer_list>
#include <memory>
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
     * Neither copyable nor movable: the descriptor and the signal
     * references are bound to the instance. Several sets can coexist
     * (each sees every signal it subscribed to); a signal's previous
     * disposition is restored when the last set holding it goes away.
     *
     * Windows: not implemented in this round. Construction throws
     * @c SignalCaptureError on Windows; @c SetConsoleCtrlHandler
     * integration is a future addition.
     */
    class SignalSet final {
    public:
        /**
         * @brief Start capturing each entry in @p signals. Throws
         *        @c SignalCaptureError if any underlying syscall
         *        (kqueue, pipe, sigaction, kevent) fails, e.g. for
         *        SIGKILL / SIGSTOP or an invalid number.
         *
         * The previous @c sigaction for each signal is captured and
         * restored once the last set holding that signal is destroyed,
         * so this composes with caller code that already installs its
         * own handlers (e.g. a debug crash handler).
         */
        explicit SignalSet(std::initializer_list<int> signals);

        SignalSet(const SignalSet &) = delete;
        SignalSet &operator=(const SignalSet &) = delete;
        SignalSet(SignalSet &&) = delete;
        SignalSet &operator=(SignalSet &&) = delete;

        /**
         * @brief Release this set's signals (restoring the original
         *        dispositions once unused) and close its descriptors.
         */
        ~SignalSet();

        /**
         * @brief Await the next captured signal. Returns the signal
         *        number (one of those passed to the constructor).
         *
         * Don't @c co_await the same SignalSet from several coroutines
         * at once: they compete for events and each signal goes to
         * only one of them. For fan-out, give each consumer its own
         * SignalSet, or forward from one dispatcher coroutine (e.g.
         * into a Telegraph::Signal).
         *
         * The set is captured when @c next() is called, not when the returned
         * task first runs. A task that outlives the set throws
         * @c SignalCaptureError and never touches the closed descriptor.
         *
         * @throws SignalCaptureError on a fatal read error or if the set is destroyed while waiting.
         */
        YarnBall::Task<int> next();

    private:
#if !defined(_WIN32)
        /** Descriptor + closed flag shared with every pending @c next(), so one outliving the set cannot touch a recycled fd. */
        struct State;

        /** The coroutine behind @c next(); takes the state by value so it is captured when @c next() is called. */
        static YarnBall::Task<int> wait(std::shared_ptr<State> st);

        /** Releases every reference and descriptor taken so far; safe on a half-built set. */
        void teardown() noexcept;

        /** Owns the descriptor the Reactor waits on: a kqueue (BSD/macOS) or the read end of the self-pipe. */
        std::shared_ptr<State> state;
        /** Write end of the self-pipe (non-kqueue builds only). */
        [[maybe_unused]] int writeFd = -1;
        /** Index of this set's slot in the handler's fan-out table (non-kqueue builds only). */
        [[maybe_unused]] int fanSlot = -1;
        /** Signals this set holds a reference on (in install order). */
        std::vector<int> installedSignals;
#endif
    };

}

#endif // YARN_SIGNAL_SET_H
