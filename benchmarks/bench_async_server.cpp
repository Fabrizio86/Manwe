//
// benchmarks/bench_async_server.cpp -- end-to-end latency benchmark for
// realistic await-heavy workloads.
//
// The microbenchmarks in bench_yarn.cpp measure individual primitives
// (dispatch, deque, mpmc, coroutine hop) in isolation. They tend to
// underweight Manwe's design wins because the coroutine hop and the
// fan-out path benefit MORE from each other than each does standalone.
//
// This benchmark mirrors the shape of a typical async server request:
//   parse -> route -> handle (K sub-awaits, e.g. DB lookups) -> respond
// and reports per-request latency end-to-end, plus the implied
// per-await cost.
//
// The K parameter is varied so the trend is visible:
//   K = 1   : single-await "ping" handler        (dispatch dominates)
//   K = 10  : small endpoint                     (typical web)
//   K = 50  : DB-heavy endpoint                  (typical service)
//   K = 200 : deep coroutine pipeline            (worst-case chain)
//
// A spawn+join fan-out variant is also reported -- this is the path
// where the full task lifecycle is exercised (spawn -> run -> join).
//
// Output columns: name | total ms | ns/req | ns/await
//

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "Coroutines.h"
#include "JoinHandle.h"
#include "Yarn.hpp"

using clk = std::chrono::steady_clock;

namespace {
    /**
     * @brief Number of independent "requests" submitted per scenario.
     *        Sized to land each run in ~100-500 ms on a modern laptop
     *        so steady-state dominates startup noise.
     */
    constexpr std::size_t kRequestsSequential = 50'000;
    constexpr std::size_t kRequestsFanOut = 5'000;

    /**
     * @brief Per-request await depths exercised by the sequential
     *        scenario. Mirrors the comment block above.
     */
    constexpr int kDepthPing = 1;
    constexpr int kDepthSmallEndpoint = 10;
    constexpr int kDepthDbHeavy = 50;
    constexpr int kDepthDeepPipeline = 200;

    /**
     * @brief Fan-out width for the spawn+join scenario. Each request
     *        spawns this many child tasks and joins them all.
     */
    constexpr int kFanOutWidth = 16;
}


/**
 * @brief Leaf coroutine -- one symmetric-transfer hop, trivial work
 *        the compiler cannot fold away (the value is summed below).
 */
static YarnBall::Task<int> leaf(int v) {
    co_return v * 2;
}

/**
 * @brief K sequential co_awaits inside one task body. Models the
 *        common handler shape: a single per-request coroutine that
 *        awaits several internal sub-tasks in sequence.
 */
static YarnBall::Task<int> sequentialAwaits(int depth) {
    int sum = 0;
    for (int i = 0; i < depth; ++i) {
        sum += co_await leaf(i);
    }
    co_return sum;
}

/**
 * @brief Spawn @p width child tasks via @c spawnJoinable, then join
 *        them all. Models a parallel fan-out: e.g. parallel DB lookups
 *        gated by a final @c await join.
 */
static YarnBall::Task<int> fanOutAndJoin(int width) {
    std::vector<YarnBall::JoinHandle<int> > handles;
    handles.reserve(static_cast<std::size_t>(width));
    for (int i = 0; i < width; ++i) {
        handles.push_back(YarnBall::spawnJoinable(leaf(i)));
    }
    int sum = 0;
    for (auto &h: handles) {
        sum += co_await h.join();
    }
    co_return sum;
}

/**
 * @brief Per-request driver: invokes the chosen workload, sinks the
 *        result, and bumps the completion counter. Lives outside the
 *        timing loop so spawn cost is what gets measured.
 */
static YarnBall::Task<void> runSequentialRequest(int depth,
                                                  std::atomic<std::size_t> *counter,
                                                  std::atomic<long long> *resultSink) {
    int v = co_await sequentialAwaits(depth);
    // Force the compiler to keep the result by feeding it into an
    // atomic accumulator. relaxed: only the order across requests
    // matters to us, not within.
    resultSink->fetch_add(v, std::memory_order_relaxed);
    counter->fetch_add(1, std::memory_order_release);
    co_return;
}

static YarnBall::Task<void> runFanOutRequest(int width,
                                              std::atomic<std::size_t> *counter,
                                              std::atomic<long long> *resultSink) {
    int v = co_await fanOutAndJoin(width);
    resultSink->fetch_add(v, std::memory_order_relaxed);
    counter->fetch_add(1, std::memory_order_release);
    co_return;
}


/**
 * @brief Pretty-print one result line.
 */
static void reportE2E(const char *name,
                       std::size_t requests,
                       int awaitsPerReq,
                       clk::duration dur) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
    const auto totalAwaits = requests * static_cast<std::size_t>(awaitsPerReq);
    const double perReq = requests
                              ? static_cast<double>(ns) / static_cast<double>(requests)
                              : 0.0;
    const double perAwait = totalAwaits
                                ? static_cast<double>(ns) / static_cast<double>(totalAwaits)
                                : 0.0;
    std::printf("%-46s | %5lld ms | %9.0f ns/req | %6.1f ns/await\n",
                name,
                static_cast<long long>(ms), perReq, perAwait);
}


/**
 * @brief Submit @p requests sequential-await jobs, wait for all to
 *        finish, report the latency.
 */
static void benchSequential(const char *name, int depth, std::size_t requests) {
    std::atomic<std::size_t> done{0};
    std::atomic<long long> sink{0};

    auto start = clk::now();
    for (std::size_t r = 0; r < requests; ++r) {
        YarnBall::coSpawn(runSequentialRequest(depth, &done, &sink));
    }
    while (done.load(std::memory_order_acquire) < requests) {
        std::this_thread::yield();
    }
    auto end = clk::now();

    reportE2E(name, requests, depth, end - start);
    // sink is intentionally not validated; we just need it to escape.
    (void) sink.load();
}

/**
 * @brief Submit @p requests fan-out jobs, each spawning @p width
 *        children + joining. The "awaits per request" reported is
 *        @p width (one join per child).
 */
static void benchFanOut(const char *name, int width, std::size_t requests) {
    std::atomic<std::size_t> done{0};
    std::atomic<long long> sink{0};

    auto start = clk::now();
    for (std::size_t r = 0; r < requests; ++r) {
        YarnBall::coSpawn(runFanOutRequest(width, &done, &sink));
    }
    while (done.load(std::memory_order_acquire) < requests) {
        std::this_thread::yield();
    }
    auto end = clk::now();

    reportE2E(name, requests, width, end - start);
    (void) sink.load();
}


int main() {
    constexpr std::size_t kHeaderRuleWidth = 92;
    std::printf("%-46s | %8s | %12s | %12s\n",
                "scenario", "total", "per-req", "per-await");
    std::printf("%s\n", std::string(kHeaderRuleWidth, '-').c_str());

    // Sequential: one task body, K internal co_awaits. The K=1 case
    // is the spawn-dominated floor; higher K's expose the per-hop
    // win that symmetric transfer gives us over Tokio-style poll loops.
    benchSequential("sequential awaits, K=1  (ping)",
                    kDepthPing, kRequestsSequential);
    benchSequential("sequential awaits, K=10 (small endpoint)",
                    kDepthSmallEndpoint, kRequestsSequential);
    benchSequential("sequential awaits, K=50 (db-heavy)",
                    kDepthDbHeavy, kRequestsSequential);
    benchSequential("sequential awaits, K=200 (deep pipeline)",
                    kDepthDeepPipeline, kRequestsSequential);

    // Fan-out: spawn W children, join them all. Exercises the full
    // task lifecycle (spawn -> run -> join) per child rather than
    // just the symmetric-transfer chain.
    benchFanOut("spawn+join fan-out, W=16",
                kFanOutWidth, kRequestsFanOut);

    // The completion counters above flip when each coroutine bumps
    // them just before its final-suspend; the frame itself is freed
    // a few instructions later, on the worker thread. Sleeping
    // briefly here lets every worker reach a parked state before
    // static destruction begins tearing down the SmallObjectPool's
    // global mutex (which the frame's operator delete would
    // otherwise hit on the way out).
    constexpr auto kPostBenchDrainGrace = std::chrono::milliseconds(100);
    std::this_thread::sleep_for(kPostBenchDrainGrace);

    return 0;
}
