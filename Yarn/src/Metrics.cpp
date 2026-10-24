//
// Created by Fabrizio Paino on 2026-05-16.
//

#include "Metrics.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace YarnBall::metrics {

    // -----------------------------------------------------------------
    // Histogram
    // -----------------------------------------------------------------

    Histogram::Histogram(std::vector<double> bucketBounds, std::string help)
        : bounds(std::move(bucketBounds)),
          bucketCounts(this->bounds.size() + 1),
          helpText(std::move(help)) {
        for (std::size_t i = 0; i < this->bounds.size(); ++i) {
            const double b = this->bounds[i];
            // observe() and the exposition format assume finite, strictly increasing upper bounds.
            if (!std::isfinite(b) || (i > 0 && !(b > this->bounds[i - 1]))) {
                throw std::invalid_argument(
                    "metrics::Histogram: bucket bounds must be finite and strictly ascending");
            }
        }
    }

    void Histogram::observe(double x) noexcept {
        // A NaN would poison the running sum permanently; drop it.
        if (std::isnan(x)) return;
        // Linear scan -- bounds.size() is small (typically <= 16).
        // Binary search would barely pay back the branch prediction
        // win at this scale.
        const std::size_t n = this->bounds.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (x <= this->bounds[i]) {
                this->bucketCounts[i].fetch_add(1, std::memory_order_relaxed);
            }
        }
        this->bucketCounts[n].fetch_add(1, std::memory_order_relaxed);
        this->totalCount.fetch_add(1, std::memory_order_relaxed);

        // Atomic CAS-loop add for sum. std::atomic<double>::fetch_add
        // is C++20 but is missing on some older libc++ shims.
        // A non-finite value still counts in the buckets above (+Inf lands in the +Inf bucket only) but must not enter
        // the sum: one +Inf would make it +Inf (or NaN with a later -Inf) for the life of the process.
        if (!std::isfinite(x)) return;
        std::uint64_t curBits = this->sumBits.load(std::memory_order_relaxed);
        while (true) {
            double cur = std::bit_cast<double>(curBits);
            double next = cur + x;
            std::uint64_t nextBits = std::bit_cast<std::uint64_t>(next);
            if (this->sumBits.compare_exchange_weak(
                    curBits, nextBits, std::memory_order_relaxed)) {
                break;
            }
        }
    }

    std::uint64_t Histogram::count() const noexcept {
        return this->totalCount.load(std::memory_order_relaxed);
    }

    double Histogram::sum() const noexcept {
        const auto bits = this->sumBits.load(std::memory_order_relaxed);
        return std::bit_cast<double>(bits);
    }

    std::vector<std::pair<double, std::uint64_t>> Histogram::snapshot() const {
        std::vector<std::pair<double, std::uint64_t>> out;
        out.reserve(this->bounds.size() + 1);
        for (std::size_t i = 0; i < this->bounds.size(); ++i) {
            out.emplace_back(this->bounds[i],
                              this->bucketCounts[i].load(std::memory_order_relaxed));
        }
        out.emplace_back(std::numeric_limits<double>::infinity(),
                          this->bucketCounts[this->bounds.size()].load(
                              std::memory_order_relaxed));
        // The buckets are read one by one while observers keep adding, so a later bucket can be read before an
        // increment that an earlier read already included. Cumulative counts must never decrease with the bound.
        for (std::size_t i = 1; i < out.size(); ++i) {
            out[i].second = std::max(out[i].second, out[i - 1].second);
        }
        return out;
    }

    // -----------------------------------------------------------------
    // Registry
    // -----------------------------------------------------------------

    Registry &Registry::instance() {
        static Registry r;
        return r;
    }

    namespace {
        /**
         * @brief Prometheus metric names are [a-zA-Z_:][a-zA-Z0-9_:]*. The name is written verbatim into the exposition
         *        text, so anything else (a space, newline or brace) would let a caller inject extra lines or labels.
         */
        void validateName(const std::string &name) {
            bool ok = !name.empty();
            for (std::size_t i = 0; ok && i < name.size(); ++i) {
                const char c = name[i];
                const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
                const bool digit = c >= '0' && c <= '9';
                ok = alpha || (i > 0 && digit);
            }
            if (!ok) {
                throw std::invalid_argument(
                    "metrics::Registry: invalid metric name (must match [a-zA-Z_:][a-zA-Z0-9_:]*)");
            }
        }

        /**
         * @brief The registry's own doc comment promises "a metric is
         *        uniquely identified by name" across all three metric
         *        kinds, but counters/gauges/histograms live in three
         *        separate maps -- nothing enforced that promise. A
         *        name collision across kinds left one object silently
         *        unreachable from scrape() and made scrape() emit the
         *        other kind's TYPE/value block twice (once per
         *        registrationOrder entry), which is invalid Prometheus
         *        exposition format and can get the whole /metrics
         *        response rejected by a scraper. Failing loud at the
         *        actual registration call site turns that into an
         *        immediately attributable error instead of a deferred,
         *        silent observability corruption.
         */
        [[noreturn]] void throwNameCollision(const std::string &name) {
            throw std::logic_error(
                "metrics::Registry: \"" + name +
                "\" already registered as a different metric type");
        }

        constexpr const char *kHistogramSuffixes[] = {"_bucket", "_count", "_sum"};

        bool endsWith(const std::string &s, const std::string &suffix) {
            return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        [[noreturn]] void throwSuffixCollision(const std::string &name, const std::string &other) {
            throw std::logic_error(
                "metrics::Registry: \"" + name + "\" collides with the series of histogram \"" + other +
                "\" (a histogram exposes <name>_bucket, <name>_count and <name>_sum)");
        }
    }

    void Registry::checkSuffixCollisionLocked(const std::string &name, bool isHistogram) const {
        const auto registered = [this](const std::string &n) {
            return this->counters.count(n) || this->gauges.count(n) || this->histograms.count(n);
        };
        for (const char *suffix : kHistogramSuffixes) {
            const std::string sfx(suffix);
            // A new series named <histogram><suffix> would duplicate one the histogram already exposes...
            if (endsWith(name, sfx)) {
                const std::string base = name.substr(0, name.size() - sfx.size());
                if (this->histograms.count(base)) throwSuffixCollision(name, base);
            }
            // ...and a new histogram would expose series that an existing metric already owns.
            if (isHistogram && registered(name + sfx)) throwSuffixCollision(name + sfx, name);
        }
    }

    Counter &Registry::counter(const std::string &name, const std::string &help) {
        validateName(name);
        std::lock_guard<std::mutex> lk(this->mu);
        auto it = this->counters.find(name);
        if (it != this->counters.end()) return *it->second;
        if (this->gauges.count(name) || this->histograms.count(name)) {
            throwNameCollision(name);
        }
        this->checkSuffixCollisionLocked(name, /*isHistogram=*/false);
        auto [ins, _] = this->counters.emplace(name, std::make_unique<Counter>(help));
        this->registrationOrder.push_back(name);
        return *ins->second;
    }

    Gauge &Registry::gauge(const std::string &name, const std::string &help) {
        validateName(name);
        std::lock_guard<std::mutex> lk(this->mu);
        auto it = this->gauges.find(name);
        if (it != this->gauges.end()) return *it->second;
        if (this->counters.count(name) || this->histograms.count(name)) {
            throwNameCollision(name);
        }
        this->checkSuffixCollisionLocked(name, /*isHistogram=*/false);
        auto [ins, _] = this->gauges.emplace(name, std::make_unique<Gauge>(help));
        this->registrationOrder.push_back(name);
        return *ins->second;
    }

    Histogram &Registry::histogram(const std::string &name,
                                      std::vector<double> buckets,
                                      const std::string &help) {
        validateName(name);
        std::lock_guard<std::mutex> lk(this->mu);
        auto it = this->histograms.find(name);
        if (it != this->histograms.end()) {
            if (it->second->bucketBounds() != buckets) {
                throw std::logic_error(
                    "metrics::Registry: histogram \"" + name +
                    "\" already registered with different bucket bounds");
            }
            return *it->second;
        }
        if (this->counters.count(name) || this->gauges.count(name)) {
            throwNameCollision(name);
        }
        this->checkSuffixCollisionLocked(name, /*isHistogram=*/true);
        auto [ins, _] = this->histograms.emplace(
            name, std::make_unique<Histogram>(std::move(buckets), help));
        this->registrationOrder.push_back(name);
        return *ins->second;
    }

    namespace {
        /**
         * @brief Emit a single floating-point value in the Prometheus-
         *        compatible format. +Inf goes out as the literal
         *        @c "+Inf", which Prometheus parses but some
         *        toolchains' @c std::to_string would not.
         */
        std::string fmtDouble(double x) {
            if (std::isinf(x)) return x > 0 ? "+Inf" : "-Inf";
            if (std::isnan(x)) return "NaN";
            // Shortest representation that round-trips: close bucket bounds stay distinct, and 0.1 stays "0.1".
#if defined(__APPLE__) && defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && \
    __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 130300
            // std::to_chars(double) needs macOS 13.3 at run time; with a lower deployment target fall back to the
            // shortest of 15..17 significant digits that parses back to the same value, in the classic locale.
            for (int precision = 15; precision <= 17; ++precision) {
                std::ostringstream os;
                os.imbue(std::locale::classic());
                os.precision(precision);
                os << x;
                std::istringstream is(os.str());
                is.imbue(std::locale::classic());
                double back = 0;
                is >> back;
                if (back == x || precision == 17) return os.str();
            }
            return "NaN";
#else
            char buf[64];
            const auto res = std::to_chars(buf, buf + sizeof(buf), x);
            return std::string(buf, res.ptr);
#endif
        }
    }

    namespace {
        /**
         * @brief Escape a HELP docstring per the Prometheus text format:
         *        backslash and line feed are the only escaped characters,
         *        and an unescaped newline would end the comment line and
         *        turn the rest of the text into a bogus sample.
         */
        std::string escapeHelp(const std::string &s) {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                if (c == '\\') out += "\\\\";
                else if (c == '\n') out += "\\n";
                else out += c;
            }
            return out;
        }
    }

    std::string Registry::scrape() const {
        std::lock_guard<std::mutex> lk(this->mu);
        std::ostringstream os;
        os.imbue(std::locale::classic());
        for (const auto &name : this->registrationOrder) {
            if (auto it = this->counters.find(name); it != this->counters.end()) {
                const auto &c = *it->second;
                if (!c.help().empty()) os << "# HELP " << name << " " << escapeHelp(c.help()) << "\n";
                os << "# TYPE " << name << " counter\n";
                os << name << " " << c.value() << "\n";
                continue;
            }
            if (auto it = this->gauges.find(name); it != this->gauges.end()) {
                const auto &g = *it->second;
                if (!g.help().empty()) os << "# HELP " << name << " " << escapeHelp(g.help()) << "\n";
                os << "# TYPE " << name << " gauge\n";
                os << name << " " << fmtDouble(g.value()) << "\n";
                continue;
            }
            if (auto it = this->histograms.find(name); it != this->histograms.end()) {
                const auto &h = *it->second;
                if (!h.help().empty()) os << "# HELP " << name << " " << escapeHelp(h.help()) << "\n";
                os << "# TYPE " << name << " histogram\n";
                auto snap = h.snapshot();
                for (const auto &[bound, count] : snap) {
                    os << name << "_bucket{le=\"" << fmtDouble(bound) << "\"} " << count << "\n";
                }
                // _count must equal the +Inf bucket of this same snapshot, not a separately sampled counter.
                os << name << "_count " << snap.back().second << "\n";
                os << name << "_sum " << fmtDouble(h.sum()) << "\n";
                continue;
            }
        }
        return os.str();
    }

}
