//
// Cancellable awaits.
//
// A cancellation scope is a std::stop_token associated with a coroutine
// chain. Every await that parks a coroutine on something that may never
// complete (AsyncMutex/Semaphore/Notify/Event/Barrier/RwLock/Once, JoinHandle,
// Telegraph channels, sleepFor, io::waitReadable/waitWritable) looks for the
// nearest scope above it in the co_await chain. When the scope is stopped the
// parked coroutine is taken out of whatever queue holds it and resumed on the
// Yarn pool; its co_await then throws CancelledException. Outside any scope
// the awaits behave exactly as before.
//
// Scopes are established by:
//  - withCancellation(token, task): runs task inside a scope tied to token;
//  - whenAny / withTimeout / channelSelect / whenAll: every branch runs in a
//    scope that is stopped as soon as one branch wins (whenAll: as soon as
//    the enclosing scope stops), so losers stop waiting instead of outliving
//    the race. A scope also follows the scope it is nested in.
//
// Task::requestCancel takes the same road: every parked await also registers
// with the chain it sits in (see detail::armCancelHook in Coroutines.h), so a
// requestCancel on any Task of the chain wakes it exactly like a stopped
// scope does, and scopes created below (whenAny & co.) follow the request.
//
// Cancellation is still cooperative for work that is not parked: a coroutine
// computing, or waiting on something that is not cancellable, runs on until
// its next cancellable await or checkCancel() (which also observes scopes).
//
// Wake, cancel and frame destruction. A parked await publishes a WaitProxy
// handle instead of the coroutine's own handle: a tiny heap coroutine with an
// atomic state that, when resumed, resumes the real coroutine unless the
// awaiter was destroyed first. Exactly one of "resumed" and "destroyed" wins
// that state, so a frame destroyed at the very moment it is being woken (or
// cancelled, or fired by the Reactor) is never resumed afterwards; the late
// resume lands on the proxy and is dropped. An await that had already been
// handed a resource (an AsyncMutex, a semaphore permit, a read/write lock)
// when its frame died gives it back.
//

#ifndef YARN_CANCELLATION_H
#define YARN_CANCELLATION_H

#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

#include "Coroutines.h"

namespace YarnBall {

    namespace detail {

        /**
         * @brief Resume @p h on the Yarn pool (inline only if the pool
         *        refuses the task, so a coroutine is never stranded).
         */
        inline void resumeOnPool(std::coroutine_handle<> h) noexcept {
            try {
                std::unique_ptr<ITask> ct{new CoroutineITask(h)};
                Yarn::instance()->run(std::move(ct));
            } catch (...) {
                h.resume();
            }
        }

        /**
         * @brief Fixed-type stop callback payload so every awaiter can embed
         *        the same @c std::stop_callback instantiation.
         */
        struct CancelThunk {
            void (*fn)(void *);
            void *ctx;

            void operator()() const noexcept { this->fn(this->ctx); }
        };

        /**
         * @brief Heap coroutine standing in for a parked coroutine wherever
         *        a handle is queued (wait lists, JoinHandle state, Reactor).
         *        Resuming it resumes @c target, unless the awaiter that owns
         *        it was destroyed first. Two references: the awaiter's, and
         *        the resumer's (released when the proxy runs, or reclaimed by
         *        the awaiter when it removed the proxy from its queue itself).
         */
        struct ProxyCoro {
            struct promise_type {
                std::atomic<int> refs{2};
                std::atomic<int> state{0}; // 0 parked, 1 resumed, 2 awaiter destroyed first
                std::coroutine_handle<> target{};

                ProxyCoro get_return_object() noexcept {
                    return ProxyCoro{std::coroutine_handle<promise_type>::from_promise(*this)};
                }

                std::suspend_always initial_suspend() noexcept { return {}; }

                struct Final {
                    bool await_ready() const noexcept { return false; }

                    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                        auto &p = h.promise();
                        std::coroutine_handle<> next = p.target;
                        if (p.state.exchange(1, std::memory_order_acq_rel) == 2) next = std::noop_coroutine();
                        if (p.refs.fetch_sub(1, std::memory_order_acq_rel) == 1) h.destroy();
                        return next;
                    }

                    void await_resume() const noexcept {}
                };

                Final final_suspend() noexcept { return {}; }

                void return_void() noexcept {}

                void unhandled_exception() noexcept { std::terminate(); }
            };

            std::coroutine_handle<promise_type> h;
        };

        inline ProxyCoro makeProxyCoro() { co_return; }

        /**
         * @brief The awaiter's end of a @ref ProxyCoro. Member of every
         *        parking awaiter; see the file header for the protocol.
         */
        class WaitProxy {
        public:
            WaitProxy() = default;
            WaitProxy(const WaitProxy &) = delete;
            WaitProxy &operator=(const WaitProxy &) = delete;
            ~WaitProxy() { this->drop(); }

            explicit operator bool() const noexcept { return static_cast<bool>(this->h); }

            /// The handle to queue / register in place of the coroutine's own.
            std::coroutine_handle<> publish(std::coroutine_handle<> target) {
                this->h = makeProxyCoro().h;
                this->h.promise().target = target;
                return this->h;
            }

            std::coroutine_handle<> handle() const noexcept { return this->h; }

            /// The proxy was removed from its queue by this awaiter: nobody else can resume it. Frees it.
            void reclaim() noexcept {
                if (this->h) {
                    this->h.destroy();
                    this->h = {};
                }
            }

            /**
             * @brief Make a still-pending resume of the proxy a no-op.
             * @return @c true if the proxy had not run (the real coroutine
             *         will never be resumed through it); @c false if it
             *         already resumed it.
             */
            bool kill() noexcept {
                int expected = 0;
                return this->h && this->h.promise().state.compare_exchange_strong(
                                      expected, 2, std::memory_order_acq_rel);
            }

            /// Release the awaiter's reference.
            void drop() noexcept {
                if (!this->h) return;
                auto p = std::exchange(this->h, {});
                if (p.promise().refs.fetch_sub(1, std::memory_order_acq_rel) == 1) p.destroy();
            }

            /**
             * @brief Settle the proxy when its awaiter goes away.
             * @param removed  The awaiter itself took the proxy out of its
             *                 queue (so no resume is pending).
             * @return @c true if a wake that was already in flight was
             *         dropped, i.e. whatever it was handing over (a lock, a
             *         permit) is now unclaimed and must be given back.
             */
            bool retire(bool removed) noexcept {
                if (!this->h) return false;
                if (removed) {
                    this->reclaim();
                    return false;
                }
                const bool dropped = this->kill();
                this->drop();
                return dropped;
            }

        private:
            std::coroutine_handle<ProxyCoro::promise_type> h{};
        };

        /**
         * @brief Cancel registration embedded in a parked awaiter. Lives in
         *        the coroutine frame, so destroying it (when the awaiter
         *        goes away) also waits for a callback that is running on
         *        another thread.
         *
         * Two sources feed it: the nearest scope token above the awaiting
         * coroutine (stop_callback) and Task::requestCancel on any Task of
         * its chain (a hook in the chain's registry). Either may call the
         * armed function, possibly more than once and possibly on another
         * thread, so it must be idempotent.
         */
        class CancelBinding {
        public:
            CancelBinding() = default;
            CancelBinding(const CancelBinding &) = delete;
            CancelBinding &operator=(const CancelBinding &) = delete;
            ~CancelBinding() { this->disarm(); }

            /**
             * @brief The cancellation context of the suspending coroutine, or
             *        @c nullptr for a coroutine that is not a Task.
             */
            template<typename Promise>
            static TaskPromiseCore *ambient(std::coroutine_handle<Promise> h) noexcept {
                if constexpr (std::is_base_of_v<TaskPromiseCore, Promise>) {
                    return static_cast<TaskPromiseCore *>(&h.promise());
                } else {
                    (void) h;
                    return nullptr;
                }
            }

            /**
             * @brief Register @p fn(@p ctx) to run on cancellation of
             *        @p core's chain. If it is already cancelled, @p fn runs
             *        inline before this returns.
             */
            void arm(TaskPromiseCore *core, void (*fn)(void *), void *ctx) {
                this->doArm(core, fn, ctx);
            }

            /**
             * @brief Like @ref arm, and returns a latch that is set just
             *        before @p fn first runs. It outlives the awaiter, so a
             *        suspending coroutine can poll it after handing itself
             *        to a system that may resume it (when touching the
             *        awaiter is no longer safe).
             */
            std::shared_ptr<std::atomic<bool>> armWatched(TaskPromiseCore *core, void (*fn)(void *), void *ctx) {
                this->latch = std::make_shared<std::atomic<bool>>(false);
                auto watch = this->latch;
                this->doArm(core, fn, ctx);
                return watch;
            }

            /**
             * @brief Unregister; blocks while the callback is running on
             *        another thread. Idempotent.
             */
            void disarm() noexcept {
                this->cb.reset();
                disarmCancelHook(this->hook);
            }

        private:
            void doArm(TaskPromiseCore *core, void (*fn)(void *), void *ctx) {
                this->userFn = fn;
                this->userCtx = ctx;
                if (const std::stop_token *tok = core->cancelToken()) {
                    this->cb.emplace(*tok, CancelThunk{&CancelBinding::fire, this});
                }
                armCancelHook(this->hook, core, &CancelBinding::fire, this);
            }

            static void fire(void *p) noexcept {
                auto *self = static_cast<CancelBinding *>(p);
                if (self->latch) self->latch->store(true, std::memory_order_release);
                self->userFn(self->userCtx);
            }

            void (*userFn)(void *) = nullptr;
            void *userCtx = nullptr;
            std::shared_ptr<std::atomic<bool>> latch;
            std::optional<std::stop_callback<CancelThunk>> cb;
            CancelHook hook;
        };

        /**
         * @brief Non-suspending awaiter that installs @c *tok as the
         *        cancellation scope of the running coroutine. @c tok must
         *        outlive the coroutine (use a local of the same frame).
         */
        struct InstallScopeAwaiter {
            const std::stop_token *tok;

            bool await_ready() const noexcept { return false; }

            template<typename Promise>
            bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
                if constexpr (std::is_base_of_v<TaskPromiseCore, Promise>) {
                    h.promise().scope = this->tok;
                }
                return false;
            }

            void await_resume() const noexcept {}
        };

        /**
         * @brief Non-suspending awaiter yielding the cancellation context of
         *        the running coroutine (@c nullptr if it is not a Task).
         */
        struct CurrentScopeAwaiter {
            TaskPromiseCore *result{nullptr};

            bool await_ready() const noexcept { return false; }

            template<typename Promise>
            bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
                this->result = CancelBinding::ambient(h);
                return false;
            }

            TaskPromiseCore *await_resume() const noexcept { return this->result; }
        };

        /**
         * @brief Forwards a stop request from one token to a stop source.
         */
        struct StopForward {
            std::stop_source *target;

            void operator()() const noexcept { this->target->request_stop(); }
        };

        /**
         * @brief Keeps upstream cancellation wired to a stop source for as
         *        long as it lives (declare it after the source): up to two
         *        explicit tokens, plus, through @ref follow, the scope and
         *        Task::requestCancel state of a coroutine chain.
         */
        class ScopeLink {
        public:
            ScopeLink() = default;
            ScopeLink(const ScopeLink &) = delete;
            ScopeLink &operator=(const ScopeLink &) = delete;

            ~ScopeLink() {
                this->a.reset();
                this->b.reset();
                disarmCancelHook(this->chain);
            }

            void add(const std::stop_token &upstream, std::stop_source &target) {
                if (!upstream.stop_possible()) return;
                (this->n == 0 ? this->a : this->b).emplace(upstream, StopForward{&target});
                ++this->n;
            }

            /**
             * @brief Stop @p target when @p core's chain is cancelled: its
             *        nearest scope is stopped, or Task::requestCancel is
             *        called on any Task at or above @p core. A null @p core
             *        (not a Task) is ignored.
             */
            void follow(TaskPromiseCore *core, std::stop_source &target) {
                if (!core) return;
                if (const std::stop_token *tok = core->cancelToken()) this->add(*tok, target);
                armCancelHook(this->chain, core, &ScopeLink::forward, &target);
            }

        private:
            static void forward(void *p) noexcept { static_cast<std::stop_source *>(p)->request_stop(); }

            std::optional<std::stop_callback<StopForward>> a;
            std::optional<std::stop_callback<StopForward>> b;
            int n = 0;
            CancelHook chain;
        };

    } // namespace detail


    /**
     * @brief Run @p task inside a cancellation scope tied to @p token. When
     *        @p token is stopped, cancellable awaits parked anywhere under
     *        @p task wake with @c CancelledException (see file header).
     *        If the caller is itself inside a scope, stopping that scope
     *        also stops this one.
     */
    template<typename T>
    Task<T> withCancellation(std::stop_token token, Task<T> task) {
        std::stop_source own;
        const std::stop_token ownToken = own.get_token();
        detail::TaskPromiseCore *outer = co_await detail::CurrentScopeAwaiter{};
        detail::ScopeLink link;
        link.add(token, own);
        link.follow(outer, own);
        co_await detail::InstallScopeAwaiter{&ownToken};
        if constexpr (std::is_void_v<T>) {
            co_await std::move(task);
            co_return;
        } else {
            co_return co_await std::move(task);
        }
    }

}

#endif // YARN_CANCELLATION_H
