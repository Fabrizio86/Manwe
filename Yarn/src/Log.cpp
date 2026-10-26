//
// Created by Fabrizio Paino on 2026-05-16.
//

#include "Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>

namespace YarnBall::log {

    namespace {
        /**
         * @brief Sink state. Heap-allocated and never freed: the logger is
         *        used from detached tasks and destructors that can run
         *        during static teardown (e.g. ~Yarn), after ordinary
         *        statics would already be gone.
         */
        struct SinkState {
            std::mutex mu;               // guards sink
            std::recursive_mutex callMu; // serialises sink calls; recursive so a sink may log
            Sink sink = [](std::string_view line) {
                (void) std::fwrite(line.data(), 1, line.size(), stderr);
            };
        };

        SinkState &sinkState() {
            static SinkState *st = new SinkState;
            return *st;
        }

        std::atomic<int> g_minLevel{static_cast<int>(Level::Info)};

        const char *levelName(Level l) noexcept {
            switch (l) {
                case Level::Trace: return "trace";
                case Level::Debug: return "debug";
                case Level::Info:  return "info";
                case Level::Warn:  return "warn";
                case Level::Error: return "error";
            }
            return "info";
        }

        /**
         * @brief Format the current wall time as RFC 3339 with
         *        millisecond precision and a trailing @c "Z".
         *        Locale-independent. Avoids @c std::put_time because
         *        that goes through the global locale.
         */
        std::string nowIso8601() {
            using namespace std::chrono;
            const auto now = system_clock::now();
            const auto tt = system_clock::to_time_t(now);
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &tt);
#else
            gmtime_r(&tt, &tm);
#endif
            const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                          tm.tm_hour, tm.tm_min, tm.tm_sec,
                          static_cast<long long>(ms));
            return std::string(buf);
        }
    }

    namespace detail {
        std::string jsonString(std::string_view s) {
            std::string out;
            out.reserve(s.size() + 2);
            out.push_back('"');
            const std::size_t n = s.size();
            for (std::size_t i = 0; i < n;) {
                const unsigned char c = static_cast<unsigned char>(s[i]);
                if (c >= 0x80) {
                    // Validate one UTF-8 sequence (shortest form, no surrogates, <= U+10FFFF); copy it through if it
                    // is well formed, else substitute U+FFFD for the offending byte.
                    std::size_t len = 0;
                    std::uint32_t cp = 0;
                    if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
                    else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
                    else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
                    bool ok = len != 0 && i + len <= n;
                    for (std::size_t k = 1; ok && k < len; ++k) {
                        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                        if ((cc & 0xC0) != 0x80) ok = false;
                        else cp = (cp << 6) | (cc & 0x3F);
                    }
                    if (ok) {
                        if (len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
                        if (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ok = false;
                    }
                    if (ok) {
                        out.append(s.substr(i, len));
                        i += len;
                    } else {
                        out.append("\xEF\xBF\xBD");
                        ++i;
                    }
                    continue;
                }
                switch (c) {
                    case '"':  out.append("\\\""); break;
                    case '\\': out.append("\\\\"); break;
                    case '\n': out.append("\\n");  break;
                    case '\r': out.append("\\r");  break;
                    case '\t': out.append("\\t");  break;
                    default:
                        if (c < 0x20 || c == 0x7F) {
                            char esc[16];
                            std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(c));
                            out.append(esc);
                        } else {
                            out.push_back(static_cast<char>(c));
                        }
                }
                ++i;
            }
            out.push_back('"');
            return out;
        }
    }

    void setSink(Sink s) {
        auto &st = sinkState();
        // callMu first, as emit() does: once this returns, a sink that was being replaced is not called again, so the
        // caller may close the file or destroy the object it captured.
        std::lock_guard<std::recursive_mutex> call(st.callMu);
        std::lock_guard<std::mutex> lk(st.mu);
        st.sink = std::move(s);
    }

    void setMinLevel(Level lvl) {
        g_minLevel.store(static_cast<int>(lvl), std::memory_order_relaxed);
    }

    Level minLevel() noexcept {
        return static_cast<Level>(g_minLevel.load(std::memory_order_relaxed));
    }

    void emit(Level lvl, std::string_view msg,
                std::initializer_list<Field> fields) {
        if (static_cast<int>(lvl) <
            g_minLevel.load(std::memory_order_relaxed)) {
            return;
        }
        std::string line;
        line.reserve(128 + msg.size());
        line.push_back('{');
        line.append("\"ts\":");
        line.append(detail::jsonString(nowIso8601()));
        line.append(",\"level\":\"");
        line.append(levelName(lvl));
        line.append("\",\"msg\":");
        line.append(detail::jsonString(msg));
        for (const auto &f : fields) {
            line.push_back(',');
            line.append(detail::jsonString(f.key));
            line.push_back(':');
            line.append(f.formatted);
        }
        line.append("}\n");

        // Take callMu, then read the sink under the state lock: a sink that itself calls back into emit() or setSink()
        // re-enters the recursive callMu on the same thread (no deadlock), while sink calls from different threads stay
        // serialised, and setSink() waits for a call in flight so the old sink is never invoked after it returns.
        auto &st = sinkState();
        std::lock_guard<std::recursive_mutex> call(st.callMu);
        Sink sinkCopy;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            sinkCopy = st.sink;
        }
        if (sinkCopy) sinkCopy(line);
    }

}
