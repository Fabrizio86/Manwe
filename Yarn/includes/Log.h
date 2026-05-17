//
// Created by Fabrizio Paino on 2026-05-16.
//
// Log -- minimal coroutine-friendly structured logger.
//
// Output is one JSON line per emit:
//
//   {"ts":"2026-05-16T12:34:56.789Z","level":"info","msg":"...",
//    "request_id":"abc","status":200}
//
// Designed for the production "ship to fluentd/loki" pattern. Default
// sink is stderr; users override with @ref setSink to push to a file,
// a socket, or wrap with their own writer.
//
// Hot path: a level check + early return when below the configured
// minimum. Above the threshold, one formatted line + one sink call
// under a mutex. Log emit is NOT a microsecond-budget operation; if
// you log on the inner loop you are doing it wrong.
//

#ifndef YARN_LOG_H
#define YARN_LOG_H

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace YarnBall::log {

    /**
     * @enum Level
     * @brief Standard severity levels. Numeric ordering matches the
     *        "is at least as severe as" comparison used in filtering.
     */
    enum class Level : int {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warn = 3,
        Error = 4,
    };

    /**
     * @struct Field
     * @brief One key/value pair. The value is pre-formatted into JSON
     *        at construction so emit() does not need to know the
     *        original type. Use the factory helpers below.
     */
    struct Field {
        std::string key;
        std::string formatted; // JSON-ready: quoted string, raw number, true/false.
    };

    namespace detail {
        /**
         * @brief JSON-escape @p s in place, surround in quotes, return.
         */
        std::string jsonString(std::string_view s);
    }

    /**
     * @brief @c "value" — a JSON string field.
     */
    inline Field str(std::string key, std::string_view value) {
        return Field{std::move(key), detail::jsonString(value)};
    }

    /**
     * @brief A signed-integer numeric field.
     */
    inline Field i64(std::string key, std::int64_t value) {
        return Field{std::move(key), std::to_string(value)};
    }

    /**
     * @brief A floating-point numeric field. Renders via @c std::ostream
     *        so locale-independent decimal notation.
     */
    inline Field f64(std::string key, double value) {
        std::ostringstream os;
        os.imbue(std::locale::classic());
        os << value;
        return Field{std::move(key), os.str()};
    }

    /**
     * @brief A boolean field.
     */
    inline Field b(std::string key, bool value) {
        return Field{std::move(key), value ? "true" : "false"};
    }

    /**
     * @typedef Sink
     * @brief Callable that receives one fully-formatted log line
     *        (including the trailing newline).
     */
    using Sink = std::function<void(std::string_view)>;

    /**
     * @brief Replace the global sink. Default sink writes to @c stderr
     *        with a single fwrite per line. Idempotent; thread-safe.
     */
    void setSink(Sink s);

    /**
     * @brief Set the minimum level. Emissions below this level are
     *        dropped before any formatting work. Default: @c Info.
     */
    void setMinLevel(Level lvl);

    /**
     * @return The currently configured minimum level.
     */
    Level minLevel() noexcept;

    /**
     * @brief Emit one structured log record. Fields are appended to
     *        the standard @c ts / @c level / @c msg envelope.
     */
    void emit(Level lvl, std::string_view msg, std::initializer_list<Field> fields = {});

    inline void trace(std::string_view m, std::initializer_list<Field> f = {}) { emit(Level::Trace, m, f); }
    inline void debug(std::string_view m, std::initializer_list<Field> f = {}) { emit(Level::Debug, m, f); }
    inline void info (std::string_view m, std::initializer_list<Field> f = {}) { emit(Level::Info,  m, f); }
    inline void warn (std::string_view m, std::initializer_list<Field> f = {}) { emit(Level::Warn,  m, f); }
    inline void error(std::string_view m, std::initializer_list<Field> f = {}) { emit(Level::Error, m, f); }

}

#endif // YARN_LOG_H
