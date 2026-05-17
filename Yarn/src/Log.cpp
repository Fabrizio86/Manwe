//
// Created by Fabrizio Paino on 2026-05-16.
//

#include "Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace YarnBall::log {

    namespace {
        std::mutex g_sinkMu;
        Sink g_sink = [](std::string_view line) {
            (void) std::fwrite(line.data(), 1, line.size(), stderr);
        };
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
            char buf[40];
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
            for (char c : s) {
                switch (c) {
                    case '"':  out.append("\\\""); break;
                    case '\\': out.append("\\\\"); break;
                    case '\n': out.append("\\n");  break;
                    case '\r': out.append("\\r");  break;
                    case '\t': out.append("\\t");  break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20) {
                            char esc[8];
                            std::snprintf(esc, sizeof(esc), "\\u%04x",
                                          static_cast<unsigned>(c) & 0xFF);
                            out.append(esc);
                        } else {
                            out.push_back(c);
                        }
                }
            }
            out.push_back('"');
            return out;
        }
    }

    void setSink(Sink s) {
        std::lock_guard<std::mutex> lk(g_sinkMu);
        g_sink = std::move(s);
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

        std::lock_guard<std::mutex> lk(g_sinkMu);
        if (g_sink) g_sink(line);
    }

}
