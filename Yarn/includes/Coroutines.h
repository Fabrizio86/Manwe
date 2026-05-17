//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef YARN_COROUTINES_H
#define YARN_COROUTINES_H

#include <atomic>
#include <coroutine>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

#include "ITask.hpp"
#include "SmallObjectPool.h"
#include "Trace.h"

namespace YarnBall {

    class Yarn; // forward decl; coSpawn pulls Yarn.hpp at the call site

    template<typename T = void>
    class Task;

    /**
     * @class CancelledException
     * @brief Thrown by @c checkCancel when the running coroutine (or any
     *        of its parents in the @c co_await chain) has been cancelled
     *        via @c Task::requestCancel.
     *
     * Cancellation is cooperative and observed only at points where the
     * coroutine explicitly polls — there is no asynchronous unwind. The
     * intended use is: at the top of each long-running loop iteration,
     * write @c co_await checkCancel(); to bail out cleanly.
     */
    class CancelledException : public std::exception {
    public:
        const char *what() const noexcept override {
            return "task cancellation requested";
        }
    };

    namespace detail {

        /**
         * @brief Non-template core base for every Task promise. Holds the
         *        cancellation flag and a parent pointer so cancellation
         *        propagates up the @c co_await chain at most once per
         *        @c checkCancel poll.
         *
         * The chain is set up in @c Task::Awaiter::await_suspend: when X
         * @c co_awaits Y, Y's promise.parent is set to X's promise. The
         * walk in @c isCancellationRequested traverses parent links until
         * it finds a set flag or hits the root (a non-Task caller such as
         * @c SyncWaitTask). Cost on the happy path is one atomic load.
         */
        struct TaskPromiseCore {
            /**
             * @brief Cancellation flag for this promise. Set by
             *        @c Task::requestCancel. Read by @c checkCancel and
             *        by descendants walking up via @c parent.
             */
            std::atomic<bool> cancelled{false};

            /**
             * @brief Parent task's core, if this Task is being awaited by
             *        another Task. @c nullptr at the root or when the
             *        awaiter is a non-Task coroutine (e.g. @c SyncWaitTask).
             *        Set once in @c Task::Awaiter::await_suspend.
             */
            TaskPromiseCore *parent{nullptr};

            /**
             * @brief Trace context carried by this coroutine. Lives in
             *        the coroutine frame, so it survives suspend/resume
             *        across different Yarn workers (unlike a thread-
             *        local). Read via @ref currentAsync, written via
             *        @ref installCurrent. Empty by default; reads walk
             *        the parent chain to find an ancestor that has one.
             */
            YarnBall::trace::Context traceCtx{};

            /**
             * @brief @c true if this promise or any ancestor has been
             *        marked cancelled. Walks the parent chain with
             *        acquire loads. O(depth) — bounded by user code's
             *        @c co_await nesting depth.
             */
            bool isCancellationRequested() const noexcept {
                const TaskPromiseCore *p = this;
                while (p) {
                    if (p->cancelled.load(std::memory_order_acquire)) return true;
                    p = p->parent;
                }
                return false;
            }

            /**
             * @brief Walk the parent chain looking for the nearest
             *        non-empty trace context. Returns an empty Context
             *        if none of this promise or its ancestors carry one.
             */
            YarnBall::trace::Context inheritedTrace() const noexcept {
                const TaskPromiseCore *p = this;
                while (p) {
                    if (!p->traceCtx.empty()) return p->traceCtx;
                    p = p->parent;
                }
                return {};
            }
        };

        /**
         * @brief Common state for every Task promise: the continuation handle
         *        and any captured exception, plus the initial/final suspend
         *        protocol that makes Task lazy and chain-friendly.
         *
         * The final suspend awaiter is the heart of the chaining: if a
         * Task has a continuation set (because someone is awaiting it), we
         * symmetric-transfer to that continuation; otherwise we land on
         * std::noop_coroutine() so the original .resume() returns.
         *
         * Derives from @ref TaskPromiseCore so cancellation chain walks
         * can be done without knowing the value type.
         *
         * @tparam Derived CRTP-ish parameter for the FinalAwaiter to recover
         *                 the concrete promise type. Not strictly needed (we
         *                 use coroutine_handle<Promise>::promise() inside the
         *                 awaiter) but kept for clarity.
         */
        template<typename Derived>
        struct TaskPromiseBase : TaskPromiseCore {
            /**
             * @brief Coroutine waiting on us. Set by Task::Awaiter::await_suspend.
             *        @c nullptr when the task is detached (e.g. fire-and-forget
             *        via @c coSpawn).
             */
            std::coroutine_handle<> continuation{};

            /**
             * @brief Captured exception from @c unhandled_exception.
             */
            std::exception_ptr exception{};

            /**
             * @brief Set by @c coSpawn to request that this coroutine
             *        self-destroys at its own final-suspend. Tasks that are
             *        awaited via @c co_await / @c syncWait leave this false
             *        and let the Task wrapper handle destruction.
             */
            bool detached{false};

            /**
             * @brief Final awaiter executed when the coroutine reaches its end.
             *
             * For non-detached tasks: symmetric-transfers to the stored
             * continuation, or to @c noop_coroutine() if none is set.
             *
             * For detached tasks (@c detached == true): self-destroys the
             * coroutine frame in place. This makes @c coSpawn'd tasks
             * leak-free even when they suspend across reactor I/O — neither
             * the original ITask nor the reactor's resumption ITask needs to
             * know who "owns" the frame; the coroutine cleans itself up.
             *
             * @note After @c h.destroy() the awaiter object (which lives in
             *       the destroyed frame) is invalid. The symmetric-transfer
             *       target is captured into a local before destroy so the
             *       return value is safe to use.
             */
            struct FinalAwaiter {
                bool await_ready() const noexcept { return false; }

                template<typename Promise>
                std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
                    auto &p = h.promise();
                    std::coroutine_handle<> next =
                        p.continuation ? p.continuation : std::noop_coroutine();
                    if (p.detached) {
                        h.destroy();
                        // `p` and `*this` are now invalid; only `next` is safe.
                    }
                    return next;
                }

                void await_resume() noexcept {}
            };

            /**
             * @brief Lazy start: the coroutine body does not run until we are
             *        co_await'd or explicitly resumed (e.g. by coSpawn).
             */
            std::suspend_always initial_suspend() noexcept { return {}; }

            FinalAwaiter final_suspend() noexcept { return {}; }

            void unhandled_exception() noexcept {
                this->exception = std::current_exception();
            }
        };


        /**
         * @brief Promise for Task<T> with a value-bearing result. The result
         *        is stored in aligned storage to avoid requiring T to be
         *        default-constructible.
         */
        template<typename T>
        struct TaskPromise : TaskPromiseBase<TaskPromise<T>> {
            alignas(T) std::byte storage[sizeof(T)];
            bool has_value{false};

            TaskPromise() = default;

            ~TaskPromise() {
                if (this->has_value) {
                    std::launder(reinterpret_cast<T *>(&storage))->~T();
                }
            }

            template<typename U>
            void return_value(U &&value)
                noexcept(std::is_nothrow_constructible_v<T, U>) {
                ::new(static_cast<void *>(&storage)) T(std::forward<U>(value));
                this->has_value = true;
            }

            /**
             * @brief Retrieve the result, rethrowing any captured exception.
             *        Called by the awaiter on resume.
             */
            T &&result() {
                if (this->exception) std::rethrow_exception(this->exception);
                return std::move(*std::launder(reinterpret_cast<T *>(&storage)));
            }

            Task<T> get_return_object() noexcept;
        };

        /**
         * @brief Promise for Task<void>.
         */
        template<>
        struct TaskPromise<void> : TaskPromiseBase<TaskPromise<void> > {
            void return_void() noexcept {}

            void result() {
                if (this->exception) std::rethrow_exception(this->exception);
            }

            Task<void> get_return_object() noexcept;
        };

    } // namespace detail


    /**
     * @class Task
     * @brief Lazily-started C++20 coroutine handle wrapper.
     *
     * Task is the basic asynchronous return type used by the Yarn coroutine
     * layer. It is *lazy*: the coroutine body does not run until the Task is
     * co_await'd by another coroutine or handed to @c coSpawn / @c syncWait.
     *
     * Lifetime: Task is move-only and owns its coroutine frame. Its destructor
     * destroys the frame; if the coroutine is suspended (at initial or final
     * suspend) the destruction is well-defined and frees the frame. When
     * ownership is transferred to the runtime via @ref release, the Task
     * wrapper no longer destroys the frame and the runtime is responsible for
     * the final cleanup.
     *
     * Chaining: when one coroutine writes @c co_await someTask, the awaiter
     * stores the caller's handle as the task's continuation and symmetric-
     * transfers control into the task. When the task reaches its final
     * suspend, it symmetric-transfers control back to the caller. No
     * scheduler bounce happens on the chain — every awaited Task runs on the
     * same worker that initiated the chain.
     *
     * @tparam T Result type (use @c void for fire-and-forget chains).
     */
    template<typename T>
    class Task {
    public:
        using promise_type = detail::TaskPromise<T>;
        using handle_type = std::coroutine_handle<promise_type>;

        Task() noexcept = default;

        explicit Task(handle_type h) noexcept : handle(h) {
        }

        Task(Task &&other) noexcept : handle(std::exchange(other.handle, {})) {
        }

        Task &operator=(Task &&other) noexcept {
            if (this != &other) {
                if (this->handle) this->handle.destroy();
                this->handle = std::exchange(other.handle, {});
            }
            return *this;
        }

        Task(const Task &) = delete;
        Task &operator=(const Task &) = delete;

        ~Task() {
            if (this->handle) this->handle.destroy();
        }

        /**
         * @brief @c true if the coroutine has completed (or this wrapper is
         *        empty).
         */
        bool done() const noexcept { return !this->handle || this->handle.done(); }

        /**
         * @brief Transfer ownership of the coroutine handle to the caller.
         *        The caller becomes responsible for destroying the handle
         *        (typically once the coroutine completes its final-suspend
         *        chain). Used by @c coSpawn.
         */
        handle_type release() noexcept {
            return std::exchange(this->handle, {});
        }

        /**
         * @brief Non-owning read of the underlying handle.
         */
        handle_type getHandle() const noexcept { return this->handle; }

        /**
         * @brief Mark this task (and via the parent chain, any task that
         *        co_awaits it) as cancellation-requested. The task
         *        observes the request only at points where it explicitly
         *        polls — see @ref checkCancel.
         *
         * Safe to call from any thread. Idempotent. A no-op if the Task
         * wrapper is empty or the coroutine has already completed; in
         * the latter case the flag is still set on the promise but the
         * coroutine will not observe it.
         */
        void requestCancel() noexcept {
            if (this->handle) {
                this->handle.promise().cancelled.store(true, std::memory_order_release);
            }
        }

        /**
         * @brief Query whether cancellation has been requested on this
         *        task (does not walk the parent chain — that's done
         *        from inside the running coroutine via @ref checkCancel).
         */
        bool isCancellationRequested() const noexcept {
            return this->handle &&
                   this->handle.promise().cancelled.load(std::memory_order_acquire);
        }

        /**
         * @brief Awaiter returned by @c operator co_await.
         *
         * The whole job of this awaiter is to chain a caller into the
         * awaited task and then symmetric-transfer control:
         *  - await_suspend stores @c caller as the task's continuation.
         *  - await_suspend returns @c handle so the compiler tail-calls
         *    into the task's resume point.
         *  - When the task hits final suspend, it tail-calls back to the
         *    caller (via TaskPromiseBase::FinalAwaiter).
         *  - await_resume retrieves the result (or rethrows the exception).
         */
        struct Awaiter {
            handle_type handle;

            bool await_ready() const noexcept {
                return !this->handle || this->handle.done();
            }

            /**
             * @brief Templated suspend so we can recover the caller's
             *        concrete promise type and hook this task into the
             *        cancellation-propagation chain when the caller is
             *        also a Task. The compiler always instantiates with
             *        the caller's actual promise; non-Task callers (e.g.
             *        @c SyncWaitTask) bypass the parent hookup at compile
             *        time via the @c if constexpr.
             */
            template<typename CallerPromise>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<CallerPromise> caller) noexcept {
                auto &p = this->handle.promise();
                p.continuation = caller;
                if constexpr (std::is_base_of_v<detail::TaskPromiseCore, CallerPromise>) {
                    p.parent = static_cast<detail::TaskPromiseCore *>(&caller.promise());
                }
                return this->handle;
            }

            decltype(auto) await_resume() {
                if constexpr (std::is_void_v<T>) {
                    this->handle.promise().result();
                } else {
                    return this->handle.promise().result();
                }
            }
        };

        /**
         * @brief Make Task awaitable. Rvalue-qualified to discourage
         *        accidental re-awaiting of the same Task (a Task is a
         *        one-shot value).
         */
        Awaiter operator co_await() && noexcept {
            return Awaiter{this->handle};
        }

    private:
        handle_type handle{};
    };


    namespace detail {

        template<typename T>
        inline Task<T> TaskPromise<T>::get_return_object() noexcept {
            return Task<T>{std::coroutine_handle<TaskPromise<T> >::from_promise(*this)};
        }

        inline Task<void> TaskPromise<void>::get_return_object() noexcept {
            return Task<void>{std::coroutine_handle<TaskPromise<void> >::from_promise(*this)};
        }


        /**
         * @brief Tiny manual-reset event used by syncWait to block the caller
         *        until the inner coroutine has set it from final_suspend.
         */
        class ManualResetEvent {
        public:
            void set() {
                {
                    std::lock_guard<std::mutex> lk(this->mu);
                    this->ready = true;
                }
                this->cv.notify_all();
            }

            void wait() {
                std::unique_lock<std::mutex> lk(this->mu);
                this->cv.wait(lk, [this] { return this->ready; });
            }

        private:
            std::mutex mu;
            std::condition_variable cv;
            bool ready{false};
        };


        template<typename T>
        class SyncWaitTask;

        /**
         * @brief Promise type used internally by @c syncWait. Same shape as
         *        TaskPromise, but its final-suspend awaiter signals a
         *        @c ManualResetEvent instead of transferring to a continuation.
         */
        template<typename T>
        struct SyncWaitTaskPromise {
            ManualResetEvent *event{nullptr};
            std::exception_ptr exception{};
            alignas(T) std::byte storage[sizeof(T)];
            bool has_value{false};

            SyncWaitTaskPromise() = default;

            ~SyncWaitTaskPromise() {
                if (this->has_value) {
                    std::launder(reinterpret_cast<T *>(&storage))->~T();
                }
            }

            SyncWaitTask<T> get_return_object() noexcept;

            std::suspend_always initial_suspend() noexcept { return {}; }

            struct FinalAwaiter {
                bool await_ready() noexcept { return false; }

                void await_suspend(std::coroutine_handle<SyncWaitTaskPromise> h) noexcept {
                    h.promise().event->set();
                }

                void await_resume() noexcept {}
            };

            FinalAwaiter final_suspend() noexcept { return {}; }

            template<typename U>
            void return_value(U &&v) {
                ::new(static_cast<void *>(&storage)) T(std::forward<U>(v));
                this->has_value = true;
            }

            void unhandled_exception() noexcept {
                this->exception = std::current_exception();
            }

            T &&result() {
                if (this->exception) std::rethrow_exception(this->exception);
                return std::move(*std::launder(reinterpret_cast<T *>(&storage)));
            }
        };

        template<>
        struct SyncWaitTaskPromise<void> {
            ManualResetEvent *event{nullptr};
            std::exception_ptr exception{};

            SyncWaitTask<void> get_return_object() noexcept;

            std::suspend_always initial_suspend() noexcept { return {}; }

            struct FinalAwaiter {
                bool await_ready() noexcept { return false; }

                void await_suspend(std::coroutine_handle<SyncWaitTaskPromise> h) noexcept {
                    h.promise().event->set();
                }

                void await_resume() noexcept {}
            };

            FinalAwaiter final_suspend() noexcept { return {}; }

            void return_void() noexcept {}

            void unhandled_exception() noexcept {
                this->exception = std::current_exception();
            }

            void result() {
                if (this->exception) std::rethrow_exception(this->exception);
            }
        };

        template<typename T>
        class SyncWaitTask {
        public:
            using promise_type = SyncWaitTaskPromise<T>;
            using handle_type = std::coroutine_handle<promise_type>;

            explicit SyncWaitTask(handle_type h) noexcept : handle(h) {
            }

            SyncWaitTask(SyncWaitTask &&o) noexcept : handle(std::exchange(o.handle, {})) {
            }

            SyncWaitTask(const SyncWaitTask &) = delete;

            ~SyncWaitTask() {
                if (this->handle) this->handle.destroy();
            }

            void startAndWait(ManualResetEvent &event) {
                this->handle.promise().event = &event;
                this->handle.resume();
                event.wait();
            }

            decltype(auto) result() {
                if constexpr (std::is_void_v<T>) {
                    this->handle.promise().result();
                } else {
                    return this->handle.promise().result();
                }
            }

        private:
            handle_type handle;
        };

        template<typename T>
        inline SyncWaitTask<T> SyncWaitTaskPromise<T>::get_return_object() noexcept {
            return SyncWaitTask<T>{std::coroutine_handle<SyncWaitTaskPromise<T> >::from_promise(*this)};
        }

        inline SyncWaitTask<void> SyncWaitTaskPromise<void>::get_return_object() noexcept {
            return SyncWaitTask<void>{std::coroutine_handle<SyncWaitTaskPromise<void> >::from_promise(*this)};
        }


        /**
         * @brief Builds the inner syncWait helper coroutine. Two overloads,
         *        one each for void and non-void T, so the discarded branch
         *        of an @c if constexpr doesn't trip the coroutine machinery's
         *        return_value / return_void choice.
         */
        template<typename T>
        inline SyncWaitTask<T> makeSyncWaitTask(Task<T> task) {
            co_return co_await std::move(task);
        }

        inline SyncWaitTask<void> makeSyncWaitTask(Task<void> task) {
            co_await std::move(task);
        }


        /**
         * @class CoroutineITask
         * @brief ITask adapter that drives a @c coroutine_handle on a worker.
         *
         * Lifetime: this adapter NEVER touches the handle after @c resume().
         * Frame destruction is the coroutine's own responsibility:
         *  - Detached tasks (set by @c coSpawn) self-destroy at their final
         *    suspend point.
         *  - Awaited tasks are kept alive by the Task wrapper higher up the
         *    call chain (e.g. @c syncWait's helper).
         * This keeps the reactor's resumption path identical to the initial
         * resume — no per-call ownership flag, no double-destroy hazards.
         */
        class CoroutineITask final : public ITask {
        public:
            explicit CoroutineITask(std::coroutine_handle<> h) noexcept : handle(h) {
            }

            void run() override {
                if (!this->handle) return;
                auto h = this->handle;
                this->handle = nullptr;
                h.resume();
                // Do not touch h after resume(): the coroutine may have
                // self-destroyed (detached) or suspended (frame still owned
                // elsewhere).
            }

            void exception(std::exception_ptr) override {
                // The coroutine's promise routes thrown exceptions through
                // unhandled_exception(); we should not see one here.
            }

            // Pool the allocation. CoroutineITask is the most-common ITask
            // wrapper (every co_spawn and every reactor resumption goes
            // through one); replacing malloc/free with a freelist hit
            // drops the per-spawn alloc cost into a handful of ns.
            static void *operator new(std::size_t) {
                return detail::poolAlloc<CoroutineITask>();
            }

            static void operator delete(void *p) noexcept {
                detail::poolFree<CoroutineITask>(p);
            }

        private:
            std::coroutine_handle<> handle;
        };

    } // namespace detail


    /**
     * @brief Block the calling thread until @p task completes; return its
     *        result, or rethrow the exception it raised.
     *
     * Internally runs @p task to completion via an inner helper coroutine
     * that signals a condition variable from its final suspend. Safe for
     * test harnesses and top-level entry points. **Do not** call from
     * inside a Yarn worker — that would block the worker indefinitely.
     */
    template<typename T>
    auto syncWait(Task<T> task) {
        detail::ManualResetEvent event;
        auto helper = detail::makeSyncWaitTask(std::move(task));
        helper.startAndWait(event);
        if constexpr (std::is_void_v<T>) {
            helper.result();
        } else {
            return std::move(helper.result());
        }
    }


    /**
     * @struct CheckCancelAwaiter
     * @brief Awaiter returned by @c checkCancel. Polls the running
     *        coroutine's promise (and via the parent chain, all
     *        ancestors) for a cancellation request and throws
     *        @c CancelledException at @c await_resume if any flag is
     *        set.
     *
     * Does NOT actually suspend: @c await_suspend returns @c false so
     * control returns inline to the coroutine. The whole operation is
     * one templated branch + a chain of acquire loads — typically a
     * single load on the happy path (no parent chain unless the task
     * is being awaited).
     */
    struct CheckCancelAwaiter {
        bool willThrow{false};

        bool await_ready() const noexcept { return false; }

        template<typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
            if constexpr (std::is_base_of_v<detail::TaskPromiseCore, Promise>) {
                this->willThrow = h.promise().isCancellationRequested();
            }
            return false;
        }

        void await_resume() {
            if (this->willThrow) throw CancelledException{};
        }
    };

    /**
     * @brief Cooperative cancellation poll. Use inside a coroutine body:
     *        @code co_await checkCancel(); @endcode
     *
     * Throws @c CancelledException if @c Task::requestCancel was called
     * on this task or any task in the @c co_await chain above it. The
     * caller's exception will propagate out through the surrounding
     * @c co_await machinery exactly like any other thrown exception.
     *
     * No suspend, no scheduler hop — this is purely an in-line atomic
     * check, comparable in cost to @c stop_token::stop_requested but
     * with chain propagation built in.
     */
    inline CheckCancelAwaiter checkCancel() noexcept { return {}; }


    namespace trace {

        /**
         * @struct CurrentTraceAwaiter
         * @brief Awaiter returned by @c trace::currentAsync. Walks the
         *        running coroutine's promise chain and returns the
         *        nearest non-empty @ref Context, or an empty one if
         *        no ancestor has a trace installed.
         *
         * Does NOT actually suspend: @c await_suspend returns @c false
         * so control returns inline. Cost is the chain walk only —
         * a single non-empty test on the happy path because
         * @c installCurrent puts the context on the promise that
         * runs @c co_await.
         */
        struct CurrentTraceAwaiter {
            Context result{};

            bool await_ready() const noexcept { return false; }

            template<typename Promise>
            bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
                if constexpr (std::is_base_of_v<YarnBall::detail::TaskPromiseCore, Promise>) {
                    this->result = h.promise().inheritedTrace();
                }
                return false;
            }

            Context await_resume() noexcept { return this->result; }
        };

        /**
         * @brief Coroutine-aware reader of the current trace context.
         *        Unlike the thread-local @c current(), this survives
         *        cross-worker resumption because the context is stored
         *        in the coroutine frame.
         *
         * Use:
         * @code
         *   auto ctx = co_await trace::currentAsync();
         *   if (!ctx.empty()) log::info("...", {log::str("trace_id", hexTraceId(ctx))});
         * @endcode
         */
        inline CurrentTraceAwaiter currentAsync() noexcept { return {}; }

        /**
         * @struct InstallTraceAwaiter
         * @brief Awaiter returned by @c trace::installCurrent. Writes
         *        @p ctx to the running coroutine's promise so all
         *        descendants (and the coroutine itself across
         *        cross-worker resumes) observe it via
         *        @ref currentAsync.
         */
        struct InstallTraceAwaiter {
            Context newCtx;

            bool await_ready() const noexcept { return false; }

            template<typename Promise>
            bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
                if constexpr (std::is_base_of_v<YarnBall::detail::TaskPromiseCore, Promise>) {
                    h.promise().traceCtx = this->newCtx;
                }
                return false;
            }

            void await_resume() noexcept {}
        };

        /**
         * @brief Install @p ctx as the trace context for the running
         *        coroutine. All sub-coroutines awaited from this point
         *        observe it through @ref currentAsync (via the parent
         *        chain). Surviving cross-worker resume is automatic
         *        because the context lives in the coroutine frame.
         *
         * Use at the top of a request handler:
         * @code
         *   auto ctx = trace::parseTraceparent(req.header("traceparent"));
         *   if (ctx.empty()) ctx = trace::newRoot();
         *   co_await trace::installCurrent(ctx);
         *   // ... handler body, sub-tasks all see the trace ...
         * @endcode
         */
        inline InstallTraceAwaiter installCurrent(Context ctx) noexcept {
            return {ctx};
        }

    } // namespace trace


    /**
     * @brief Submit @p task to the Yarn pool for fire-and-forget execution.
     *        Ownership of the coroutine frame is transferred to the runtime;
     *        the wrapper is left empty.
     *
     * Implementation: pulls the coroutine_handle out of the Task, wraps it
     * in a @c detail::CoroutineITask, and routes it through @c Yarn::run.
     * The worker that picks up the ITask resumes the coroutine once; the
     * coroutine itself drives the rest of its lifetime via the symmetric-
     * transfer protocol in its promise's final_suspend.
     *
     * Defined inline so the user only includes @c Coroutines.h.
     */
    template<typename T>
    void coSpawn(Task<T> task);

} // namespace YarnBall


#include "Yarn.hpp"

namespace YarnBall {

    template<typename T>
    inline void coSpawn(Task<T> task) {
        auto handle = task.release();
        if (!handle) return;
        // Mark the coroutine as detached: it will self-destroy at its own
        // final suspend, regardless of whether it goes through the reactor.
        handle.promise().detached = true;
        // Spelled as unique_ptr<ITask> to disambiguate from the sITask
        // overload (unique_ptr<Derived> can implicit-convert to both
        // unique_ptr<ITask> and shared_ptr<ITask>).
        std::unique_ptr<ITask> owned{new detail::CoroutineITask(handle)};
        Yarn::instance()->run(std::move(owned));
    }


    /**
     * @struct ScheduleOnAwaiter
     * @brief Awaiter that hops the current coroutine onto a Yarn pool
     *        worker. Returned by @ref scheduleOn.
     *
     * Semantics:
     *  - If the target pool is @c nullptr, await_ready returns @c true and
     *    the coroutine continues inline on the calling thread (no-op).
     *  - Otherwise the awaiter submits a @c CoroutineITask wrapping the
     *    caller's coroutine_handle to the pool. After @c co_await returns,
     *    the coroutine body runs on a worker thread.
     *
     * Typical use:
     *  - Right after accepting a connection on the listener coroutine, you
     *    can write @c co_await scheduleOn(Yarn::instance()) to make sure
     *    the per-connection logic runs on the pool and the listener
     *    coroutine isn't blocking accept-loop progress.
     *  - To bounce off the reactor thread when an awaiter completes inline
     *    on the reactor (rare in our design, but the primitive is here).
     */
    struct ScheduleOnAwaiter {
        Yarn *pool;

        bool await_ready() const noexcept { return this->pool == nullptr; }

        void await_suspend(std::coroutine_handle<> h) noexcept {
            try {
                std::unique_ptr<ITask> ct{new detail::CoroutineITask(h)};
                this->pool->run(std::move(ct));
            } catch (...) {
                // Last-resort fallback: resume inline on the caller. Loses
                // the "bounce to worker" guarantee but does not leak the
                // coroutine.
                h.resume();
            }
        }

        void await_resume() const noexcept {}
    };

    /**
     * @brief Hop the current coroutine onto a Yarn worker.
     *
     * Returns an awaitable; usage: @code co_await scheduleOn(pool); @endcode
     * @param pool The pool to land on. @c nullptr is a no-op.
     */
    inline ScheduleOnAwaiter scheduleOn(Yarn *pool) noexcept {
        return ScheduleOnAwaiter{pool};
    }


    /**
     * @section cooperative_cancellation Cooperative cancellation with std::stop_token
     *
     * Yarn does not need a custom cancellation primitive: pass a
     * @c std::stop_token as a parameter to your coroutine and check
     * @c stop_token::stop_requested() at safe points. The token integrates
     * naturally with the executor — the coroutine observes the request
     * between awaits and exits via @c co_return.
     *
     * Example:
     * @code
     * Task<int> countUntilStop(std::stop_token tok) {
     *     int n = 0;
     *     while (!tok.stop_requested()) {
     *         co_await scheduleOn(Yarn::instance());
     *         ++n;
     *     }
     *     co_return n;
     * }
     *
     * std::stop_source src;
     * auto handle = countUntilStop(src.get_token());
     * // ... later, on any thread:
     * src.request_stop();
     * @endcode
     *
     * Reactor-suspended coroutines observe stop requests at their next
     * awaited point. If you need stop to interrupt an in-flight syscall
     * (e.g. a long @c waitReadable), close the fd from the stop-requester
     * thread — the awaiter will resume with an error.
     *
     * Auto-propagation of stop tokens to sub-tasks is NOT provided in v1;
     * either pass the token explicitly to children or store it in shared
     * state.
     */

}

#endif // YARN_COROUTINES_H
