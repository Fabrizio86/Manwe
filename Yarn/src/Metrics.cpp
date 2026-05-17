//
// Created by Fabrizio Paino on 2026-05-16.
//

#include "Metrics.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <sstream>
#include <utility>

namespace YarnBall::metrics {

    // -----------------------------------------------------------------
    // Histogram
    // -----------------------------------------------------------------

    Histogram::Histogram(std::vector<double> bucketBounds, std::string help)
        : bounds(std::move(bucketBounds)),
          bucketCounts(this->bounds.size() + 1),
          helpText(std::move(help)) {
    }

    void Histogram::observe(double x) noexcept {
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
        return out;
    }

    // -----------------------------------------------------------------
    // Registry
    // -----------------------------------------------------------------

    Registry &Registry::instance() {
        static Registry r;
        return r;
    }

    Counter &Registry::counter(const std::string &name, const std::string &help) {
        std::lock_guard<std::mutex> lk(this->mu);
        auto it = this->counters.find(name);
        if (it != this->counters.end()) return *it->second;
        auto [ins, _] = this->counters.emplace(name, std::make_unique<Counter>(help));
        this->registrationOrder.push_back(name);
        return *ins->second;
    }

    Gauge &Registry::gauge(const std::string &name, const std::string &help) {
        std::lock_guard<std::mutex> lk(this->mu);
        auto it = this->gauges.find(name);
        if (it != this->gauges.end()) return *it->second;
        auto [ins, _] = this->gauges.emplace(name, std::make_unique<Gauge>(help));
        this->registrationOrder.push_back(name);
        return *ins->second;
    }

    Histogram &Registry::histogram(const std::string &name,
                                      std::vector<double> buckets,
                                      const std::string &help) {
        std::lock_guard<std::mutex> lk(this->mu);
        auto it = this->histograms.find(name);
        if (it != this->histograms.end()) return *it->second;
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
            std::ostringstream os;
            os << x;
            return os.str();
        }
    }

    std::string Registry::scrape() const {
        std::lock_guard<std::mutex> lk(this->mu);
        std::ostringstream os;
        for (const auto &name : this->registrationOrder) {
            if (auto it = this->counters.find(name); it != this->counters.end()) {
                const auto &c = *it->second;
                if (!c.help().empty()) os << "# HELP " << name << " " << c.help() << "\n";
                os << "# TYPE " << name << " counter\n";
                os << name << " " << c.value() << "\n";
                continue;
            }
            if (auto it = this->gauges.find(name); it != this->gauges.end()) {
                const auto &g = *it->second;
                if (!g.help().empty()) os << "# HELP " << name << " " << g.help() << "\n";
                os << "# TYPE " << name << " gauge\n";
                os << name << " " << fmtDouble(g.value()) << "\n";
                continue;
            }
            if (auto it = this->histograms.find(name); it != this->histograms.end()) {
                const auto &h = *it->second;
                if (!h.help().empty()) os << "# HELP " << name << " " << h.help() << "\n";
                os << "# TYPE " << name << " histogram\n";
                auto snap = h.snapshot();
                for (const auto &[bound, count] : snap) {
                    os << name << "_bucket{le=\"" << fmtDouble(bound) << "\"} " << count << "\n";
                }
                os << name << "_count " << h.count() << "\n";
                os << name << "_sum " << fmtDouble(h.sum()) << "\n";
                continue;
            }
        }
        return os.str();
    }

}
