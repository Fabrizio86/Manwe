//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef YARN_COROUTINES_H
#define YARN_COROUTINES_H

#include <atomic>
#include <coroutine>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <stop_token>
#include <thread>
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

    /**
     * @brief Handler invoked when a detached (@c coSpawn) task finishes by
     *        throwing. Nobody awaits a detached task, so without a handler
     *        the exception would be lost.
     */
    using UnhandledExceptionHandler = std::function<void(std::exception_ptr)>;

    /**
     * @brief Install the process-wide handler for exceptions escaping
     *        detached tasks. Pass an empty function to restore the
     *        default, which writes an error line through @c log::error.
     *        @c CancelledException (an expected way for a cancelled task
     *        to end) is never reported. The handler may run on any Yarn
     *        worker and must not throw. Thread-safe.
     */
    void setUnhandledExceptionHandler(UnhandledExceptionHandler handler);

    namespace detail {

        /**
         * @brief Route @p e to the active unhandled-exception handler.
         *        Defined in Yarn.cpp.
         */
        void reportDetachedException(std::exception_ptr e) noexcept;

        struct TaskPromiseCore;

        /**
         * @brief Intrusive registration of a parked await (or a scope
         *        forwarder) with the chain-cancel registry of the root of
         *        its @c co_await chain, so @c Task::requestCancel on any task
         *        of that chain can wake it. Lives inside the owning awaiter.
         *        See @ref armCancelHook / @ref requestChainCancel.
         */
        struct CancelHook {
            void (*fn)(void *) = nullptr;
            void *ctx = nullptr;
            TaskPromiseCore *leaf = nullptr;
            std::atomic<TaskPromiseCore *> root{nullptr}; // non-null while linked
            CancelHook *prev = nullptr;
            CancelHook *next = nullptr;
            std::atomic<bool> firing{false};
            std::thread::id firer{};
            bool *abandoned = nullptr;
        };

        /**
         * @brief Lock guarding the hook list of the chain rooted at @p root
         *        (striped: roots hash onto a fixed set of mutexes). Hook
         *        callbacks run after this lock is released (see
         *        @ref runTakenHooks), so the lock is a leaf: code that holds a
         *        primitive's mutex may not arm or disarm a hook, because a
         *        callback running on another thread takes that mutex while a
         *        disarm waits for it.
         */
        inline std::mutex &chainCancelLock(const void *root) noexcept {
            // Immortal: a detached coroutine can still be torn down by a worker while static destructors run.
            static std::mutex *const stripes = new std::mutex[64];
            return stripes[(reinterpret_cast<std::uintptr_t>(root) >> 6) % 64];
        }

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
            std::atomic<TaskPromiseCore *> parent{nullptr};

            /**
             * @brief Head of the chain-cancel hook list; meaningful only on
             *        the root of a chain (a promise with no parent). Guarded
             *        by @ref chainCancelLock.
             */
            CancelHook *hooks{nullptr};

            /**
             * @brief Set when the coroutine reaches its final suspend, i.e.
             *        before its frame can go away together with the chain
             *        above it. @ref pins keeps that from happening while
             *        Task::requestCancel walks the chain.
             */
            std::atomic<bool> finishing{false};
            std::atomic<int> pins{0};

            /**
             * @brief Cancellation scope installed on this coroutine (see
             *        @c Cancellation.h), or @c nullptr. Points at a token
             *        that lives in this coroutine's own frame. Cancellable
             *        awaits below this promise park against the nearest
             *        scope found by walking @c parent.
             */
            const std::stop_token *scope{nullptr};

            /**
             * @brief Nearest cancellation scope at or above this promise,
             *        or @c nullptr when the chain is not inside one.
             */
            const std::stop_token *cancelToken() const noexcept {
                const TaskPromiseCore *p = this;
                while (p) {
                    if (p->scope) return p->scope;
                    p = p->parent.load(std::memory_order_acquire);
                }
                return nullptr;
            }

            /**
             * @brief Top of the @c co_await chain this promise sits in.
             */
            TaskPromiseCore *chainRoot() noexcept {
                TaskPromiseCore *p = this;
                while (TaskPromiseCore *q = p->parent.load(std::memory_order_acquire)) p = q;
                return p;
            }

            /**
             * @brief @c true if @p x is this promise or one of its ancestors.
             */
            bool inChainOf(const TaskPromiseCore *x) const noexcept {
                const TaskPromiseCore *p = this;
                while (p) {
                    if (p == x) return true;
                    p = p->parent.load(std::memory_order_acquire);
                }
                return false;
            }

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
             *        marked cancelled or sits in a stopped cancellation
             *        scope. Walks the parent chain with
             *        acquire loads. O(depth) — bounded by user code's
             *        @c co_await nesting depth.
             */
            bool isCancellationRequested() const noexcept {
                const TaskPromiseCore *p = this;
                while (p) {
                    if (p->cancelled.load(std::memory_order_acquire)) return true;
                    if (p->scope && p->scope->stop_requested()) return true;
                    p = p->parent.load(std::memory_order_acquire);
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
                    p = p->parent.load(std::memory_order_acquire);
                }
                return {};
            }
        };

        /**
         * @brief Link @p h into the registry of the chain @p leaf belongs
         *        to, so that @ref requestChainCancel on @p leaf or any ancestor
         *        calls @p fn(@p ctx) once. If @p leaf's chain is already
         *        cancelled, @p fn runs inline before this returns (a cancel
         *        that raced the registration is therefore never lost: the
         *        flag is stored before the registry is scanned, and read here
         *        after the hook is linked).
         */
        inline void armCancelHook(CancelHook &h, TaskPromiseCore *leaf, void (*fn)(void *), void *ctx) noexcept {
            h.fn = fn;
            h.ctx = ctx;
            h.leaf = leaf;
            TaskPromiseCore *root = leaf->chainRoot();
            {
                std::lock_guard<std::mutex> lk(chainCancelLock(root));
                h.prev = nullptr;
                h.next = root->hooks;
                if (root->hooks) root->hooks->prev = &h;
                root->hooks = &h;
                h.root.store(root, std::memory_order_relaxed);
            }
            if (leaf->isCancellationRequested()) fn(ctx);
        }

        /**
         * @brief Unlink @p h. If a @ref runTakenHooks is running its
         *        callback on another thread this waits for it, so the owner
         *        may free @p h afterwards. Idempotent; a hook that was never
         *        armed is a no-op.
         */
        inline void disarmCancelHook(CancelHook &h) noexcept {
            // Acquire pairs with the release store of root = nullptr in takeChainHooks, which comes after firing = true: a
            // disarm that sees the hook unlinked therefore also sees it firing and waits below instead of returning
            // while the callback is about to run.
            if (TaskPromiseCore *root = h.root.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(chainCancelLock(root));
                if (h.root.load(std::memory_order_relaxed) == root) {
                    if (h.prev) h.prev->next = h.next; else root->hooks = h.next;
                    if (h.next) h.next->prev = h.prev;
                    h.prev = h.next = nullptr;
                    h.root.store(nullptr, std::memory_order_relaxed);
                    return;
                }
            }
            // Unlinked by a fire: its callback may still be running.
            if (h.firing.load(std::memory_order_acquire)) {
                if (h.firer == std::this_thread::get_id()) {
                    // Called from inside the callback itself (an inline resume): the firing thread must not touch h again.
                    if (h.abandoned) *h.abandoned = true;
                    return;
                }
                while (h.firing.load(std::memory_order_acquire)) std::this_thread::yield();
            }
        }

        /**
         * @brief Unlink every hook whose leaf sits at or below @p x and
         *        return them as a list (through @c next) for
         *        @ref runTakenHooks. The caller must have @p x pinned.
         */
        inline CancelHook *takeChainHooks(TaskPromiseCore *x) noexcept {
            TaskPromiseCore *root = x->chainRoot();
            CancelHook *taken = nullptr;
            std::lock_guard<std::mutex> lk(chainCancelLock(root));
            for (CancelHook *h = root->hooks; h;) {
                CancelHook *nx = h->next;
                if (h->leaf->inChainOf(x)) {
                    if (h->prev) h->prev->next = h->next; else root->hooks = h->next;
                    if (h->next) h->next->prev = h->prev;
                    h->firer = std::this_thread::get_id();
                    h->abandoned = nullptr;
                    h->firing.store(true, std::memory_order_release);
                    // Last: publishes the unlinked state only once firing is visible (see disarmCancelHook).
                    h->root.store(nullptr, std::memory_order_release);
                    h->prev = nullptr;
                    h->next = taken;
                    taken = h;
                }
                h = nx;
            }
            return taken;
        }

        /**
         * @brief Run the callbacks of hooks returned by @ref takeChainHooks.
         *        Each hook fires at most once; a hook's owner may be freed
         *        as soon as its callback has run (its disarm waits for it).
         *
         * Two phases. A callback can resume its coroutine inline (the pool
         * refused the task) and unwind a chain that owns the *other* taken
         * hooks, freeing them while they still sit in the list. So first,
         * with no user code running, every hook's fn/ctx is copied into a
         * local slot and its @c abandoned pointer aimed at that slot; only
         * then are the callbacks invoked. A same-thread disarm during a
         * callback marks the victim's slot, and that hook is skipped (its
         * owner is gone) and never touched again.
         */
        inline void runTakenHooks(CancelHook *taken) noexcept {
            struct Slot {
                CancelHook *h;
                void (*fn)(void *);
                void *ctx;
                bool abandoned;
            };
            constexpr std::size_t kInline = 16;
            while (taken) {
                std::size_t n = 0;
                for (CancelHook *h = taken; h; h = h->next) ++n;
                Slot inlineSlots[kInline];
                std::unique_ptr<Slot[]> heapSlots;
                Slot *slots = inlineSlots;
                if (n > kInline) {
                    heapSlots.reset(new (std::nothrow) Slot[n]);
                    if (heapSlots) slots = heapSlots.get(); else n = kInline; // out of memory: handle a chunk at a time
                }
                std::size_t cnt = 0;
                for (; cnt < n; ++cnt) {
                    CancelHook *h = taken;
                    taken = h->next;
                    slots[cnt] = Slot{h, h->fn, h->ctx, false};
                    h->abandoned = &slots[cnt].abandoned;
                }
                for (std::size_t i = 0; i < cnt; ++i) {
                    if (slots[i].abandoned) continue; // owner destroyed by an earlier callback
                    slots[i].fn(slots[i].ctx);
                    if (!slots[i].abandoned) slots[i].h->firing.store(false, std::memory_order_release);
                }
            }
        }

        /**
         * @brief Cancel request on @p x: set its flag and fire the hooks at
         *        or below it. Safe against @p x completing concurrently (the
         *        chain above a finished task may already be gone): the
         *        completion waits for the walk, or the walk is skipped.
         */
        inline void requestChainCancel(TaskPromiseCore *x) noexcept {
            x->cancelled.store(true, std::memory_order_seq_cst);
            x->pins.fetch_add(1, std::memory_order_seq_cst);
            CancelHook *taken = nullptr;
            if (!x->finishing.load(std::memory_order_seq_cst)) taken = takeChainHooks(x);
            x->pins.fetch_sub(1, std::memory_order_seq_cst);
            runTakenHooks(taken);
        }

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
                    // Let a concurrent Task::requestCancel finish walking the chain before it can be torn down.
                    p.finishing.store(true, std::memory_order_seq_cst);
                    while (p.pins.load(std::memory_order_seq_cst) != 0) std::this_thread::yield();
                    std::coroutine_handle<> next =
                        p.continuation ? p.continuation : std::noop_coroutine();
                    if (p.detached) {
                        if (p.exception) reportDetachedException(p.exception);
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

            Task<> get_return_object() noexcept;
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
         * @brief Mark this task (and, via the parent chain, every task it
         *        co_awaits) as cancellation-requested, and wake whatever of
         *        them is parked in a cancellable await (see
         *        @c Cancellation.h): the await throws @c CancelledException.
         *        Work that is not parked observes the request at its next
         *        cancellable await or @ref checkCancel poll.
         *
         * Safe to call from any thread. Idempotent. A no-op if the Task
         * wrapper is empty; if the coroutine has already completed the flag
         * is still set on the promise but nothing is woken. The Task must
         * not be destroyed while this call is running.
         */
        void requestCancel() noexcept {
            if (!this->handle) return;
            detail::requestChainCancel(&this->handle.promise());
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
                    p.parent.store(static_cast<detail::TaskPromiseCore *>(&caller.promise()),
                                   std::memory_order_release);
                }
                return this->handle;
            }

            /// Returns the result by value, not as a reference into the promise: the awaited Task is usually a temporary
            /// that dies at the end of the full-expression, so `for (x : co_await f())` or `auto &&r = co_await f();`
            /// would otherwise keep a reference into a destroyed frame.
            T await_resume() {
                if (!this->handle) throw std::logic_error("co_await on an empty Task");
                if constexpr (std::is_void_v<T>) {
                    this->handle.promise().result();
                } else {
                    return std::move(this->handle.promise().result());
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

        inline Task<> TaskPromise<void>::get_return_object() noexcept {
            return Task<void>{std::coroutine_handle<TaskPromise<void> >::from_promise(*this)};
        }


        /**
         * @brief Tiny manual-reset event used by syncWait to block the caller
         *        until the inner coroutine has set it from final_suspend.
         */
        class ManualResetEvent {
        public:
            /**
             * @brief Notify_all is called WHILE STILL HOLDING @c mu, not
             *        after releasing it. @c event lives on syncWait's
             *        stack frame, with nothing keeping it alive past
             *        wait() returning: if the lock were released before
             *        notify_all(), the waiting thread's cv.wait(lk,
             *        pred) can observe `ready == true` and return via a
             *        plain spurious wakeup -- a real, standards-
             *        permitted occurrence, not a bug -- entirely
             *        independent of this notify_all() call. syncWait()
             *        then returns, its stack frame (including this
             *        ManualResetEvent) is reused by whatever runs next
             *        on that thread, and the still-pending notify_all()
             *        call here executes against memory that has since
             *        been reinitialized as a *different*, currently-
             *        live condition_variable -- corrupting its internal
             *        state and surfacing as an unrelated, later wait()
             *        call throwing "condition_variable wait failed:
             *        Invalid argument" on whatever unlucky syncWait
             *        happens to run next. Holding the lock across
             *        notify_all() closes the race: the waiter cannot
             *        observe `ready == true` and proceed to destroy
             *        this object until AFTER this function has released
             *        the mutex, by which point notify_all() has already
             *        completed.
             */
            void set() {
                std::lock_guard<std::mutex> lk(this->mu);
                this->ready = true;
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

        inline SyncWaitTask<void> makeSyncWaitTask(Task<> task) {
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
         * resume — no double-destroy hazards.
         *
         * The one exception is the initial submission of a detached task
         * (@c coSpawn), which passes @c ownsFrame: that task owns a frame that
         * nobody else can free until it has run once. If it is destroyed
         * without ever running (the pool refused it, or shut down first), the
         * destructor destroys the never-started frame instead of leaking it.
         */
        class CoroutineITask final : public ITask {
        public:
            explicit CoroutineITask(std::coroutine_handle<> h, bool ownsFrame = false) noexcept
                : handle(h), owns(ownsFrame) {
            }

            ~CoroutineITask() override {
                if (this->owns && this->handle) this->handle.destroy();
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
            bool owns{false};
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
        std::unique_ptr<ITask> owned;
        try {
            owned.reset(new detail::CoroutineITask(handle, /*ownsFrame=*/true));
        } catch (...) {
            // The frame is ours alone until the ITask exists: destroy it instead of leaking it.
            handle.destroy();
            throw;
        }
        // From here the ITask owns the frame: if run() throws it destroys the task (and so the never-started frame),
        // and if the pool drops the task unrun at shutdown the same happens.
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

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            try {
                std::unique_ptr<ITask> ct{new detail::CoroutineITask(h)};
                this->pool->run(std::move(ct));
                return true;
            } catch (...) {
                // Last-resort fallback: do not suspend, so the coroutine continues inline on the caller (returning
                // false instead of calling h.resume() here keeps the resumed body out of await_suspend, where it
                // would run on top of this frame and could destroy the awaiter under it). Loses the "bounce to
                // worker" guarantee but does not leak the coroutine.
                return false;
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
     * @section cooperative_cancellation Cancellation with std::stop_token
     *
     * Cancellation uses plain @c std::stop_token. Two complementary ways
     * to observe a request:
     *
     *  - Polling: pass a token to your coroutine and check
     *    @c stop_requested() (or @c co_await checkCancel()) at safe points.
     *  - Waking: run the work with @c withCancellation(token, task)
     *    (Cancellation.h), or under @c whenAny / @c withTimeout, which do it
     *    for their branches. Inside such a scope every parked await
     *    (sleepFor, io::waitReadable/Writable, AsyncMutex & friends,
     *    JoinHandle, Telegraph channels) is taken out of its queue when the
     *    token is stopped and throws @c CancelledException, and
     *    @c checkCancel() sees the scope too.
     *  - Task::requestCancel: wakes the same parked awaits of the task and
     *    of every task it co_awaits (and the scopes whenAny / whenAll /
     *    withCancellation open below it), without any scope or token.
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
     * To interrupt an fd wait without a scope, call
     * @c Reactor::cancelFd(fd) from the code that owns the fd just before
     * closing it; the parked coroutines resume and see the closed fd.
     *
     * Tokens are not propagated implicitly into tasks that are detached with
     * @c coSpawn: pass the token explicitly or wrap the task in
     * @c withCancellation.
     */

}

#endif // YARN_COROUTINES_H
