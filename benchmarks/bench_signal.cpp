//
// benchmarks/bench_signal.cpp -- Telegraph::Signal emit cost.
//
// Prints "name | ops | total_ms | ns/op". Handlers do a relaxed atomic add so the work per slot is
// as small as possible and the numbers show the dispatch overhead itself.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "Wire.h"
#include "Yarn.hpp"

using clk = std::chrono::steady_clock;

namespace {
    std::atomic<long> g_sink{0};

    void report(const char *name, std::size_t ops, clk::duration d) {
        const double ms = std::chrono::duration<double, std::milli>(d).count();
        std::printf("%-40s | %10zu | %9.2f ms | %8.1f ns/op\n", name, ops, ms, ms * 1e6 / double(ops));
    }

    void emitBench(const char *name, int handlers, int threads, std::size_t perThread) {
        Telegraph::Signal<int> sig;
        for (int i = 0; i < handlers; ++i) {
            sig.connect([](int v) { g_sink.fetch_add(v, std::memory_order_relaxed); });
        }
        const auto t0 = clk::now();
        std::vector<std::thread> ts;
        for (int t = 0; t < threads; ++t) {
            ts.emplace_back([&] { for (std::size_t i = 0; i < perThread; ++i) sig.emit(1); });
        }
        for (auto &t : ts) t.join();
        report(name, perThread * std::size_t(threads), clk::now() - t0);
    }
}

int main() {
    constexpr std::size_t N = 1'000'000;
    emitBench("emit, 0 handlers, 1 thread", 0, 1, N);
    emitBench("emit, 1 handler, 1 thread", 1, 1, N);
    emitBench("emit, 8 handlers, 1 thread", 8, 1, N);
    emitBench("emit, 8 handlers, 4 threads", 8, 4, N / 4);
    emitBench("emit, 1 handler, 4 threads", 1, 4, N / 4);
    return g_sink.load() == -1;
}
