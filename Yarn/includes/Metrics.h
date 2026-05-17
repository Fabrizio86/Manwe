//
// Created by Fabrizio Paino on 2026-05-16.
//
// Metrics -- counter / gauge / histogram primitives plus a process-
// wide Registry that emits the Prometheus text exposition format.
//
// Designed for the production /metrics endpoint pattern: register
// counters once at startup, increment them lock-free on the hot path,
// scrape the whole set on a periodic HTTP poll. No labels in v1 (a
// metric is uniquely identified by name); add labelled flavours when
// a real consumer needs them.
//
// Hot-path cost: a counter inc is one relaxed atomic fetch-add;
// gauges and histograms are similarly lock-free. The registry's
// shared map is only consulted at registration time (typically once
// during startup) and at scrape time.
//
// Typical use:
//
//     using namespace YarnBall::metrics;
//     static Counter &requests = Registry::instance().counter("http_requests");
//     static Histogram &latency = Registry::instance().histogram(
//         "http_request_latency_ns",
//         {1e3, 1e4, 1e5, 1e6, 1e7, 1e8});  // ns buckets
//
//     // in handler:
//     auto t0 = std::chrono::steady_clock::now();
//     ... do work ...
//     requests.inc();
//     latency.observe(std::chrono::duration_cast<std::chrono::nanoseconds>(
//                         std::chrono::steady_clock::now() - t0).count());
//
// Or, sugar:
//
//     {
//         auto _ = scopeTimer(latency);
//         ... do work ...
//     }
//

#ifndef YARN_METRICS_H
#define YARN_METRICS_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace YarnBall::metrics {

    /**
     * @class Counter
     * @brief Monotonically increasing 64-bit integer. Lock-free
     *        increments via @c std::atomic.
     */
    class Counter final {
    public:
        explicit Counter(std::string help = {}) : helpText(std::move(help)) {}

        Counter(const Counter &) = delete;
        Counter &operator=(const Counter &) = delete;

        /**
         * @brief Add @p n (default 1) to the counter. Relaxed atomic
         *        because counters do not need ordering with respect to
         *        anything except the eventual scrape.
         */
        void inc(std::uint64_t n = 1) noexcept {
            this->v.fetch_add(n, std::memory_order_relaxed);
        }

        /**
         * @brief Current value.
         */
        std::uint64_t value() const noexcept {
            return this->v.load(std::memory_order_relaxed);
        }

        const std::string &help() const noexcept { return this->helpText; }

    private:
        std::atomic<std::uint64_t> v{0};
        std::string helpText;
    };

    /**
     * @class Gauge
     * @brief Signed floating-point value that can go up or down.
     *        Wraps @c std::atomic<double> for lock-free updates.
     */
    class Gauge final {
    public:
        explicit Gauge(std::string help = {}) : helpText(std::move(help)) {}

        Gauge(const Gauge &) = delete;
        Gauge &operator=(const Gauge &) = delete;

        void set(double x) noexcept {
            this->v.store(x, std::memory_order_relaxed);
        }

        void inc(double d = 1.0) noexcept {
            double cur = this->v.load(std::memory_order_relaxed);
            while (!this->v.compare_exchange_weak(cur, cur + d,
                                                    std::memory_order_relaxed)) { }
        }

        void dec(double d = 1.0) noexcept { this->inc(-d); }

        double value() const noexcept {
            return this->v.load(std::memory_order_relaxed);
        }

        const std::string &help() const noexcept { return this->helpText; }

    private:
        std::atomic<double> v{0.0};
        std::string helpText;
    };

    /**
     * @class Histogram
     * @brief Bucketed distribution. Bucket boundaries are fixed at
     *        construction time (typically latency thresholds in ns).
     *        Observations are O(log n) (binary search through buckets)
     *        but n is small (<= ~16 buckets).
     *
     * Cumulative bucket semantics matching Prometheus: a bucket counts
     * every observation @c <= its upper bound. The final @c +Inf bucket
     * counts every observation.
     */
    class Histogram final {
    public:
        /**
         * @brief Construct with the given finite bucket boundaries.
         *        Must be sorted ascending. The implicit @c +Inf bucket
         *        is appended automatically.
         */
        Histogram(std::vector<double> bucketBounds, std::string help = {});

        Histogram(const Histogram &) = delete;
        Histogram &operator=(const Histogram &) = delete;

        /**
         * @brief Record one observation. Increments the count for every
         *        bucket whose upper bound is @c >= @p x (the cumulative
         *        Prometheus convention), and the total sum.
         */
        void observe(double x) noexcept;

        /**
         * @brief Total observation count (== count of the @c +Inf bucket).
         */
        std::uint64_t count() const noexcept;

        /**
         * @brief Sum of all observed values. Useful for computing the
         *        mean (sum / count).
         */
        double sum() const noexcept;

        /**
         * @brief Snapshot the bucket counts and the (bound, count)
         *        pairs in scrape order. Last entry is always
         *        @c (+Inf, totalCount).
         */
        std::vector<std::pair<double, std::uint64_t>> snapshot() const;

        const std::string &help() const noexcept { return this->helpText; }

    private:
        std::vector<double> bounds; // finite, sorted ascending
        // bucketCounts has bounds.size() + 1 entries; the final entry
        // is the +Inf bucket.
        mutable std::vector<std::atomic<std::uint64_t>> bucketCounts;
        std::atomic<std::uint64_t> totalCount{0};
        // sumBits is bit_cast<uint64_t>(double); CAS-loop on it.
        std::atomic<std::uint64_t> sumBits{0};
        std::string helpText;
    };

    /**
     * @brief Scope guard that records elapsed nanoseconds (from
     *        construction to destruction) to a histogram. The most
     *        common per-request latency idiom:
     *
     *            auto _ = scopeTimer(handler_latency_ns);
     *            ... handler body ...
     */
    class ScopeTimer final {
    public:
        explicit ScopeTimer(Histogram &h) noexcept
            : target(&h),
              t0(std::chrono::steady_clock::now()) {
        }

        ScopeTimer(const ScopeTimer &) = delete;
        ScopeTimer &operator=(const ScopeTimer &) = delete;

        ~ScopeTimer() {
            if (this->target) {
                const auto dur = std::chrono::steady_clock::now() - this->t0;
                this->target->observe(static_cast<double>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count()));
            }
        }

    private:
        Histogram *target;
        std::chrono::steady_clock::time_point t0;
    };

    inline ScopeTimer scopeTimer(Histogram &h) noexcept { return ScopeTimer(h); }

    /**
     * @class Registry
     * @brief Process-wide singleton that interns metric objects by
     *        name. Returns a reference; the same name always returns
     *        the same object so a `static` cache in user code is
     *        legitimate.
     *
     * Registration is mutex-protected; updates are not. Hot paths
     * cache the reference in a `static` local and never touch the
     * registry's lock.
     */
    class Registry final {
    public:
        static Registry &instance();

        /**
         * @brief Retrieve (or create) a counter named @p name. The
         *        first call sets the help string; subsequent calls
         *        ignore the help argument and return the same object.
         */
        Counter &counter(const std::string &name, const std::string &help = {});
        Gauge &gauge(const std::string &name, const std::string &help = {});
        Histogram &histogram(const std::string &name,
                              std::vector<double> buckets,
                              const std::string &help = {});

        /**
         * @brief Produce the entire Prometheus text-format exposition
         *        for the registered metrics, in registration order.
         *        Intended to be returned as the body of a @c /metrics
         *        HTTP endpoint.
         */
        std::string scrape() const;

    private:
        Registry() = default;

        mutable std::mutex mu;
        std::unordered_map<std::string, std::unique_ptr<Counter>> counters;
        std::unordered_map<std::string, std::unique_ptr<Gauge>> gauges;
        std::unordered_map<std::string, std::unique_ptr<Histogram>> histograms;
        std::vector<std::string> registrationOrder; // for stable scrape output
    };

}

#endif // YARN_METRICS_H
