//
// Created by Fabrizio Paino on 2026-05-15.
//
// Stream<T>: coroutine return type that yields zero or more values via
// co_yield, then completes (or throws). Each value is pulled lazily --
// the producer suspends after each co_yield and the consumer resumes
// it via the awaitable returned by Stream::next().
//
// Consume with the YARN_FOREACH_AWAIT macro:
//
//   Stream<int> countTo(int n) {
//       for (int i = 0; i < n; ++i) co_yield i;
//   }
//
//   Task<int> sum_first_n(int n) {
//       int total = 0;
//       YARN_FOREACH_AWAIT(int v, countTo(n)) {
//           total += v;
//       }
//       co_return total;
//   }
//
// Or pull one at a time:
//
//   auto s = countTo(n);
//   auto v = co_await s.next();
//   if (v) ...;
//

#ifndef YARN_STREAM_H
#define YARN_STREAM_H

#include <coroutine>
#include <cstddef>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace YarnBall {

    template<typename T>
    class Stream;

    namespace detail {

        /**
         * @brief Promise type for @ref Stream. Holds a pointer to the
         *        most recently yielded value plus the consumer's
         *        continuation handle.
         *
         * Lifetime: the yielded value lives in the producer coroutine's
         * frame at the @c co_yield point. The promise stores a pointer
         * to it; the consumer reads the pointer in @c await_resume,
         * then resumes the producer to clear the slot.
         */
        template<typename T>
        struct StreamPromise {
            /**
             * @brief Pointer to the most-recent yielded value. Lives in
             *        the producer's @c co_yield expression's storage.
             *        @c nullptr when no value is pending (initial state
             *        and after every consumer pull).
             */
            T *current = nullptr;

            /**
             * @brief Set @c true by @c return_void to signal the
             *        producer has completed; pairs with @c current
             *        being null to mean "stream ended".
             */
            bool done = false;

            /**
             * @brief Captured exception from the producer body.
             *        Rethrown by the consumer's @c next() on the pull
             *        that observes the failure.
             */
            std::exception_ptr exception{};

            /**
             * @brief Consumer's coroutine handle, set by NextAwaiter's
             *        @c await_suspend and resumed at the next
             *        @c co_yield or completion.
             */
            std::coroutine_handle<> consumer{};

            Stream<T> get_return_object() noexcept;

            std::suspend_always initial_suspend() noexcept { return {}; }

            /**
             * @brief Final suspend transfers control back to the
             *        consumer so it can observe end-of-stream on the
             *        same resume that drove the producer to completion.
             */
            struct FinalAwaiter {
                bool await_ready() const noexcept { return false; }

                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<StreamPromise> h) noexcept {
                    auto &p = h.promise();
                    p.done = true;
                    return p.consumer ? p.consumer : std::noop_coroutine();
                }

                void await_resume() noexcept {
                }
            };

            FinalAwaiter final_suspend() noexcept { return {}; }

            void return_void() noexcept { this->done = true; }

            void unhandled_exception() noexcept {
                this->exception = std::current_exception();
            }

            /**
             * @brief @c co_yield value support. Stores a pointer to the
             *        value, transfers control back to the consumer.
             *
             * The yielded value lives in the producer's expression
             * storage at the @c co_yield site; the consumer sees the
             * pointer in @c NextAwaiter::await_resume, takes the value
             * (move or copy as it pleases), then resumes the producer
             * to release the storage.
             */
            struct YieldAwaiter {
                bool await_ready() const noexcept { return false; }

                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<StreamPromise> h) noexcept {
                    auto &p = h.promise();
                    return p.consumer ? p.consumer : std::noop_coroutine();
                }

                void await_resume() noexcept {
                }
            };

            YieldAwaiter yield_value(T &value) noexcept {
                this->current = std::addressof(value);
                return {};
            }

            YieldAwaiter yield_value(T &&value) noexcept {
                this->current = std::addressof(value);
                return {};
            }
        };

    } // namespace detail


    /**
     * @class Stream
     * @brief Lazy asynchronous generator. The producer @c co_yield s
     *        values one at a time; each @c co_await on the consumer's
     *        @c next() drives the producer until it either yields or
     *        completes.
     *
     * Move-only. Single-consumer: consuming via multiple coroutines
     * concurrently is undefined.
     *
     * @tparam T Yielded value type. Must be move-constructible.
     */
    template<typename T>
    class Stream {
    public:
        using promise_type = detail::StreamPromise<T>;
        using handle_type = std::coroutine_handle<promise_type>;

        Stream() noexcept = default;

        explicit Stream(handle_type h) noexcept : handle(h) {
        }

        Stream(const Stream &) = delete;
        Stream &operator=(const Stream &) = delete;

        Stream(Stream &&other) noexcept : handle(std::exchange(other.handle, {})) {
        }

        Stream &operator=(Stream &&other) noexcept {
            if (this != &other) {
                if (this->handle) this->handle.destroy();
                this->handle = std::exchange(other.handle, {});
            }
            return *this;
        }

        ~Stream() {
            if (this->handle) this->handle.destroy();
        }

        /**
         * @struct NextAwaiter
         * @brief Awaitable returned by @c next(). Resumes the producer
         *        coroutine; the producer either runs to the next
         *        @c co_yield (publishing a value) or completes
         *        (publishing end-of-stream).
         *
         * @c await_resume returns @c std::optional<T>: engaged with
         * the yielded value, or empty on end-of-stream.
         */
        struct NextAwaiter {
            handle_type producer;

            bool await_ready() const noexcept {
                return !this->producer || this->producer.done();
            }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> consumer) noexcept {
                this->producer.promise().consumer = consumer;
                return this->producer;
            }

            std::optional<T> await_resume() {
                auto &p = this->producer.promise();
                if (p.exception) std::rethrow_exception(p.exception);
                if (p.done && p.current == nullptr) {
                    return std::nullopt;
                }
                T *raw = p.current;
                p.current = nullptr;
                return std::optional<T>{std::move(*raw)};
            }
        };

        /**
         * @brief Pull the next value. Returns @c std::optional<T>:
         *        engaged with the yielded value, or empty when the
         *        producer has completed.
         */
        NextAwaiter next() noexcept { return NextAwaiter{this->handle}; }

        /**
         * @brief @c true once the producer has finished and no value is
         *        pending. After a true reading, @c next() will resume
         *        immediately with @c nullopt.
         */
        bool done() const noexcept { return !this->handle || this->handle.done(); }

    private:
        handle_type handle{};
    };


    namespace detail {
        template<typename T>
        inline Stream<T> StreamPromise<T>::get_return_object() noexcept {
            return Stream<T>{std::coroutine_handle<StreamPromise<T>>::from_promise(*this)};
        }
    }


    // ---- Stream combinators -----------------------------------------
    //
    // Each combinator wraps an input @c Stream<T> in a fresh stream
    // coroutine that pulls from the input lazily and yields the
    // transformed values. Pure data-flow; no Yarn dispatch.
    //
    // Combinators take the input @c Stream by value (must be moved in)
    // because Stream is move-only and the lazy wrapper coroutine
    // needs sole ownership of the input handle.

    /**
     * @brief Transform each value through @p fn.
     */
    template<typename T, typename Fn>
    auto streamMap(Stream<T> input, Fn fn)
        -> Stream<std::invoke_result_t<Fn, T>> {
        while (auto v = co_await input.next()) {
            co_yield fn(std::move(*v));
        }
    }

    /**
     * @brief Drop values for which @p pred returns @c false.
     */
    template<typename T, typename Pred>
    Stream<T> streamFilter(Stream<T> input, Pred pred) {
        while (auto v = co_await input.next()) {
            if (pred(*v)) co_yield std::move(*v);
        }
    }

    /**
     * @brief Yield at most @p n values, then end the stream.
     */
    template<typename T>
    Stream<T> streamTake(Stream<T> input, std::size_t n) {
        std::size_t taken = 0;
        while (auto v = co_await input.next()) {
            if (taken >= n) co_return;
            co_yield std::move(*v);
            ++taken;
        }
    }

    /**
     * @brief Skip the first @p n values, then yield the rest.
     */
    template<typename T>
    Stream<T> streamDrop(Stream<T> input, std::size_t n) {
        std::size_t dropped = 0;
        while (auto v = co_await input.next()) {
            if (dropped < n) {
                ++dropped;
                continue;
            }
            co_yield std::move(*v);
        }
    }

}

/**
 * @def YARN_FOREACH_AWAIT(decl, stream)
 * @brief @c co_await -based for-loop over a @c YarnBall::Stream. Bind
 *        each yielded value to @p decl and run the loop body once per
 *        value.
 *
 * Implementation as a macro because C++23 does not yet have a
 * standardised @c co_await -aware range-for. The Stream's @c next()
 * returns an awaiter that yields @c std::optional<T>; the loop
 * dissolves the optional and stops on end-of-stream.
 *
 * Usage:
 * @code
 * YARN_FOREACH_AWAIT(int v, countTo(10)) {
 *     total += v;
 * }
 * @endcode
 */
#define YARN_FOREACH_AWAIT(decl, stream)                                         \
    if (auto _yfa_stream = (stream); true)                                       \
        for (auto _yfa_opt = co_await _yfa_stream.next();                        \
             _yfa_opt.has_value();                                               \
             _yfa_opt = co_await _yfa_stream.next())                             \
            for (bool _yfa_once = true; _yfa_once; _yfa_once = false)            \
                for (decl = std::move(*_yfa_opt); _yfa_once; _yfa_once = false)

#endif // YARN_STREAM_H
