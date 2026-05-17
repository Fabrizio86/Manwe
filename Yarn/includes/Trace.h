//
// Created by Fabrizio Paino on 2026-05-16.
//
// Trace -- W3C TraceContext propagation for distributed tracing.
//
// The W3C @c traceparent header layout this code implements:
//
//   traceparent: 00-{32-hex trace-id}-{16-hex span-id}-{2-hex flags}
//
// One verb each:
//   - newRoot()                   : generate fresh ids (origin of a trace)
//   - newChild(parent)            : same trace-id, new span-id
//   - parseTraceparent(header)    : ingest an incoming header
//   - toTraceparent(ctx)          : emit for an outgoing request
//   - Scope(ctx)                  : RAII "current" context on this thread
//   - current()                   : the thread-local current context
//
// Coroutine model: @c Scope is per-thread. While the user is inside
// synchronous @c co_await chains (Task<T> symmetric transfer stays on
// one worker), @c current() returns the active context. Across awaits
// that genuinely suspend and resume on a different worker, the
// thread-local does not follow; for that case pass the @c Context as
// a parameter or store it on the request object. A coroutine-promise-
// scoped propagation is a future round when a real consumer needs it.
//

#ifndef YARN_TRACE_H
#define YARN_TRACE_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace YarnBall::trace {

    /**
     * @struct Context
     * @brief One W3C trace context.
     */
    struct Context {
        std::array<std::uint8_t, 16> traceId{};
        std::array<std::uint8_t, 8> spanId{};
        std::uint8_t flags{0};

        /**
         * @return @c true when @c flags carries the @c sampled bit
         *         (0x01). Used by exporters to decide whether to ship
         *         the span.
         */
        bool sampled() const noexcept { return (this->flags & 0x01) != 0; }

        /**
         * @return @c true when both ids are all-zero, the canonical
         *         "no trace" state.
         */
        bool empty() const noexcept {
            for (auto b : this->traceId) if (b != 0) return false;
            for (auto b : this->spanId)  if (b != 0) return false;
            return true;
        }
    };

    /**
     * @brief Fresh root context. 16-byte trace-id and 8-byte span-id
     *        from a non-cryptographic PRNG -- trace ids only need to
     *        be unique within a service-mesh lifetime, not
     *        unguessable. @c flags is set to @c 0x01 (sampled).
     */
    Context newRoot();

    /**
     * @brief Child of @p parent: same @c traceId, new @c spanId,
     *        inherits @c flags. Used inside a handler when spawning
     *        a downstream call so the child gets its own span.
     */
    Context newChild(const Context &parent);

    /**
     * @brief Parse a @c traceparent header value. Returns an empty
     *        @c Context (see @c empty()) when the header is missing,
     *        malformed, or uses a version we do not support (we
     *        accept only version @c 00 -- the W3C-stable level).
     */
    Context parseTraceparent(std::string_view header);

    /**
     * @brief Emit @p ctx as a @c traceparent header value (no header
     *        name, just the value). Empty contexts produce an empty
     *        string.
     */
    std::string toTraceparent(const Context &ctx);

    /**
     * @brief 32-hex string of the trace id. Convenient for logging
     *        as a JSON field via @c YarnBall::log::str.
     */
    std::string hexTraceId(const Context &ctx);

    /**
     * @brief 16-hex string of the span id. Convenient for logging.
     */
    std::string hexSpanId(const Context &ctx);

    /**
     * @brief Read the thread-local current context. Empty when no
     *        @c Scope is active.
     */
    const Context &current() noexcept;

    /**
     * @class Scope
     * @brief RAII guard that installs @p ctx as the thread-local
     *        @c current context for the lifetime of the guard. The
     *        previous value is restored on destruction.
     *
     * Move-only is unnecessary -- usage is always lexical-scope.
     * Non-copyable to prevent the "two scopes pointing at the same
     * saved value" race.
     */
    class Scope final {
    public:
        explicit Scope(const Context &ctx) noexcept;
        ~Scope() noexcept;

        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
        Scope(Scope &&) = delete;
        Scope &operator=(Scope &&) = delete;

    private:
        Context saved;
    };

}

#endif // YARN_TRACE_H
