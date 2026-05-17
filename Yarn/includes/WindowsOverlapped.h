//
// Created by Fabrizio Paino on 2026-05-15.
//
// Windows-only completion-port primitives. Defines the OVERLAPPED-derived
// completion record carried by every proactor-style WSARecv / WSASend
// issued through Soccer; the Reactor's IOCP completion thread casts
// completions back to this type, fills in @c bytes / @c err, and resumes
// the awaiting coroutine on the Yarn pool.
//

#ifndef YARN_WINDOWS_OVERLAPPED_H
#define YARN_WINDOWS_OVERLAPPED_H

#ifdef _WIN32

#include <coroutine>

#include "PlatformNet.h"

namespace YarnBall {

    /**
     * @struct OverlappedCompletion
     * @brief Per-operation OVERLAPPED record for a proactor-style
     *        async I/O issued through Soccer.
     *
     * Layout: derives from @c OVERLAPPED so that a pointer to this struct
     * can be passed directly as the @c LPOVERLAPPED argument of
     * @c WSARecv / @c WSASend / @c AcceptEx. The kernel touches only the
     * inherited @c OVERLAPPED fields; the trailing members are private to
     * us, populated by the Reactor's IOCP completion thread.
     *
     * Lifetime: the awaiter creates one of these per operation, holds it
     * by reference until the IOCP wakes it, then drops it. The struct
     * MUST live until the corresponding completion has been dequeued by
     * the IOCP thread -- the kernel writes into the OVERLAPPED fields
     * asynchronously after the syscall returns.
     */
    struct OverlappedCompletion : public OVERLAPPED {
        /**
         * @brief Coroutine to resume on completion. Set by the awaiter
         *        before issuing the syscall; consumed by the IOCP thread
         *        when the completion is dequeued.
         */
        std::coroutine_handle<> handle;

        /**
         * @brief Bytes transferred, copied out by the IOCP thread from
         *        the @c lpNumberOfBytesTransferred output of
         *        @c GetQueuedCompletionStatus.
         */
        DWORD bytes = 0;

        /**
         * @brief Win32 error from the completion (0 on success, otherwise
         *        the value the kernel placed in @c Internal converted via
         *        @c HRESULT_FROM_NT). The awaiter inspects this on resume.
         */
        int err = 0;

        OverlappedCompletion() noexcept {
            // Zero-initialise the inherited OVERLAPPED fields. The kernel
            // requires them clear (specifically Internal, InternalHigh,
            // and Pointer/Offset) before each use.
            OVERLAPPED *base = this;
            *base = OVERLAPPED{};
        }
    };

}

#endif // _WIN32

#endif // YARN_WINDOWS_OVERLAPPED_H
