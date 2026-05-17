//
// Created by Fabrizio Paino on 2022-03-29.
//

#ifndef YARN_DEFINITIONS_HPP
#define YARN_DEFINITIONS_HPP

#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "IScheduler.hpp"
#include "MPMCQueue.h"
#include "WorkStealingDeque.h"

namespace YarnBall {

    class ITask;
    class Fiber;

    /**
     * @brief Co-owning task handle. Retained on the public API for
     *        Waitable-style flows where the caller wants to observe completion.
     *        On the hot path tasks are converted to a raw owning pointer; this
     *        type is not held inside the executor's queues.
     */
    using sITask = std::shared_ptr<ITask>;

    /**
     * @brief Uniquely-owned task handle. Preferred entry point — ownership is
     *        transferred to the executor without touching any reference count.
     */
    using uITask = std::unique_ptr<ITask>;

    /**
     * @brief Raw owning task pointer. The executor passes these through its
     *        per-fiber deques and the global injection queue. The thread that
     *        dequeues a TaskPtr must either run+delete it or hand it off.
     */
    using TaskPtr = ITask *;

    /**
     * @brief Identifier for a fiber slot in the pool (also indexes into the
     *        slot's queue / state).
     */
    using FiberId = unsigned int;

    /**
     * @brief Function alias for plain operations supplied to ad-hoc tasks.
     */
    using Operation = std::function<void()>;

    /**
     * @brief Callback invoked by a temp fiber as it retires. The implementation
     *        is responsible for removing the fiber from the live pool and
     *        keeping the Fiber object alive until the reaper joins it.
     */
    using SignalDone = std::function<void(FiberId)>;

    /**
     * @brief Returns @c true if the global injection queue currently has work.
     *        Used by fibers as a wait-condition input.
     */
    using AnyPendingTasks = std::function<bool()>;

    /**
     * @brief Pulls a burst of tasks from the global injection queue into the
     *        fiber's local deque. Caller-side bounded.
     */
    using GetFromPending = std::function<void(FiberId)>;

    /**
     * @brief Pushes a task into the global injection queue. Ownership of the
     *        raw pointer is transferred.
     */
    using PushPending = std::function<void(TaskPtr)>;

    /**
     * @brief Attempts to steal a task from a peer fiber chosen by the caller.
     *        Returns @c nullptr if no work was obtained.
     */
    using TryStealFromPeers = std::function<TaskPtr(FiberId selfId)>;

    /**
     * @brief Wake-one signal sent by producers when a parked worker may now
     *        have work (either in the injection queue or stealable from a peer).
     */
    using WakeOneIdle = std::function<void()>;

    /**
     * @brief Platform handle to a Fiber's underlying OS thread.
     */
    using OsHandler = std::thread::native_handle_type;

    /**
     * @brief Per-fiber work-stealing deque. Owned by exactly one Fiber; the
     *        bottom is touched only by that Fiber, the top can be stolen by
     *        any other Fiber.
     */
    using Deque = WorkStealingDeque<TaskPtr>;

    /**
     * @brief Global lock-free MPMC injection queue. Used by:
     *          - external (non-worker) submitters
     *          - workers that fail to push to their own full local deque
     *          - retiring temp fibers draining stale local work
     */
    using InjectionQueue = MPMCQueue<TaskPtr>;
    using sInjectionQueue = std::shared_ptr<InjectionQueue>;

    /**
     * @brief Pluggable scheduler interface.
     */
    using sIScheduler = std::shared_ptr<IScheduler>;

    /**
     * @brief Shorthand for the lock used with @c std::condition_variable.
     */
    using Locket = std::unique_lock<std::mutex>;
}

#endif //YARN_DEFINITIONS_HPP
