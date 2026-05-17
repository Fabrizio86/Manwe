//
// benchmarks/bench_yarn.cpp -- microbenchmarks for the core Yarn primitives.
//
// Each benchmark prints "name | ops | total_ms | ns/op". Run on a quiet
// system. Numbers are noisy at small magnitudes; trends across runs are
// the useful signal.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Coroutines.h"
#include "ITask.hpp"
#include "MPMCQueue.h"
#include "WorkStealingDeque.h"
#include "Yarn.hpp"
#include "YarnBall.hpp"

using clk = std::chrono::steady_clock;

namespace {
    /**
     * @brief Per-benchmark workload sizes. Tuned to land in a few hundred
     *        milliseconds on a modern laptop while still being big enough
     *        for the steady-state mean to dominate startup noise.
     */
    constexpr std::size_t kDequeOwnerOps = 1'000'000;
    constexpr std::size_t kDequeConcurrentOps = 200'000;
    constexpr std::size_t kMpmcPerProducer = 200'000;
    constexpr std::size_t kYarnDispatchOps = 200'000;
    constexpr std::size_t kTaskChainIters = 20'000;

    /**
     * @brief Deque capacities used by the benchmark, expressed as
     *        power-of-two literals so the requirement is self-evident.
     */
    constexpr std::size_t kBenchDequeCapacityOwner = std::size_t{1} << 13; // 8192
    constexpr std::size_t kBenchDequeCapacityConcurrent = std::size_t{1} << 14; // 16384
    constexpr std::size_t kBenchMpmcCapacity = std::size_t{1} << 14; // 16384

    /**
     * @brief Depth of the synthetic Task<int> chain used to measure
     *        symmetric-transfer overhead per hop.
     */
    constexpr int kTaskChainDepth = 10;
}

/**
 * @brief Pretty-print a result line.
 */
static void report(const char *name, std::size_t ops, clk::duration dur) {
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
    auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
    double ns_per = ops ? static_cast<double>(total_ns) / static_cast<double>(ops) : 0.0;
    std::printf("%-40s | %10zu ops | %5lld ms | %7.1f ns/op\n",
                name, ops, static_cast<long long>(total_ms), ns_per);
}


/**
 * @brief A trivial ITask that just bumps a counter. Used to measure raw
 *        Yarn::run dispatch throughput.
 */
class BumpTask : public YarnBall::ITask {
public:
    explicit BumpTask(std::atomic<int> *c) : counter(c) {
    }

    void run() override { this->counter->fetch_add(1, std::memory_order_relaxed); }
    void exception(std::exception_ptr) override {
    }

private:
    std::atomic<int> *counter;
};

/**
 * @brief Submit @p N trivial tasks and measure the time until all of them
 *        have run.
 */
static void bench_yarn_dispatch(std::size_t N) {
    std::atomic<int> count{0};
    auto start = clk::now();
    for (std::size_t i = 0; i < N; ++i) {
        std::unique_ptr<YarnBall::ITask> t = std::make_unique<BumpTask>(&count);
        YarnBall::run(std::move(t));
    }
    auto submitEnd = clk::now();
    while (count.load(std::memory_order_acquire) < static_cast<int>(N)) {
        std::this_thread::yield();
    }
    auto drainEnd = clk::now();
    report("submit only  (uITask, no wait)", N, submitEnd - start);
    report("end-to-end   (uITask, submit+drain)", N, drainEnd - start);
}

/**
 * @brief Submit @p N trivial lambdas via the SBO-callable @c run<F>
 *        overload (pool-backed wrapper, no malloc, no vtable for the
 *        outer call). Measures the new fast path.
 */
static void bench_yarn_dispatch_callable(std::size_t N) {
    std::atomic<int> count{0};
    std::atomic<int> *counterPtr = &count;
    auto start = clk::now();
    for (std::size_t i = 0; i < N; ++i) {
        YarnBall::run([counterPtr] {
            counterPtr->fetch_add(1, std::memory_order_relaxed);
        });
    }
    auto submitEnd = clk::now();
    while (count.load(std::memory_order_acquire) < static_cast<int>(N)) {
        std::this_thread::yield();
    }
    auto drainEnd = clk::now();
    report("submit only  (SBO callable)", N, submitEnd - start);
    report("end-to-end   (SBO callable)", N, drainEnd - start);
}


/**
 * @brief Push then pop @p N items through a Chase-Lev deque on a single
 *        thread (owner side only). Measures the LIFO fast path.
 */
static void bench_deque_owner(std::size_t N) {
    YarnBall::WorkStealingDeque<int *> d(kBenchDequeCapacityOwner);
    int sentinel = 0;

    auto start = clk::now();
    for (std::size_t i = 0; i < N; ++i) d.push(&sentinel);
    int *out;
    for (std::size_t i = 0; i < N; ++i) (void) d.pop(out);
    auto end = clk::now();

    report("deque owner push+pop", N * 2, end - start);
}

/**
 * @brief Concurrent owner + 2 stealers on the deque. Owner pushes N
 *        items, thieves drain them. Measures the steal path.
 */
static void bench_deque_concurrent(std::size_t N) {
    YarnBall::WorkStealingDeque<int *> d(kBenchDequeCapacityConcurrent);
    std::atomic<std::size_t> consumed{0};
    int sentinel = 0;

    auto thief = [&] {
        int *out;
        while (consumed.load(std::memory_order_acquire) < N) {
            if (d.steal(out)) consumed.fetch_add(1, std::memory_order_release);
            else std::this_thread::yield();
        }
    };

    std::thread t1(thief), t2(thief);
    auto start = clk::now();
    for (std::size_t i = 0; i < N; ++i) {
        while (!d.push(&sentinel)) {
            int *out;
            if (d.pop(out)) consumed.fetch_add(1, std::memory_order_release);
        }
    }
    while (consumed.load() < N) std::this_thread::yield();
    auto end = clk::now();

    t1.join();
    t2.join();
    report("deque owner+2 thieves", N, end - start);
}

/**
 * @brief MPMC queue: 2 producers + 2 consumers, each producer enqueues
 *        @p per_producer items.
 */
static void bench_mpmc(std::size_t per_producer) {
    constexpr int kP = 2;
    constexpr int kC = 2;
    YarnBall::MPMCQueue<int> q(kBenchMpmcCapacity);
    std::atomic<std::size_t> produced{0};
    std::atomic<std::size_t> consumed{0};
    const std::size_t total = per_producer * kP;

    auto start = clk::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < kP; ++i) {
        threads.emplace_back([&, per_producer] {
            for (std::size_t j = 0; j < per_producer; ++j) {
                while (!q.enqueue(static_cast<int>(j))) std::this_thread::yield();
                produced.fetch_add(1);
            }
        });
    }
    for (int i = 0; i < kC; ++i) {
        threads.emplace_back([&, total] {
            int out;
            while (consumed.load() < total) {
                if (q.dequeue(out)) consumed.fetch_add(1);
                else std::this_thread::yield();
            }
        });
    }
    for (auto &t : threads) t.join();
    auto end = clk::now();

    report("mpmc 2P/2C", total, end - start);
}


/**
 * @brief A 10-deep co_await chain that returns an int. Measures the
 *        symmetric-transfer overhead Task<T> imposes per hop.
 */
static YarnBall::Task<int> chain_step(int n) {
    if (n == 0) co_return 0;
    int x = co_await chain_step(n - 1);
    co_return x + 1;
}

/**
 * @brief Run @p iters chains of depth @ref kTaskChainDepth sequentially.
 */
static void bench_task_chain(std::size_t iters) {
    auto start = clk::now();
    for (std::size_t i = 0; i < iters; ++i) {
        (void) YarnBall::syncWait(chain_step(kTaskChainDepth));
    }
    auto end = clk::now();
    report("Task<int> 10-deep syncWait", iters, end - start);
}


int main() {
    constexpr std::size_t kHeaderRuleWidth = 80;
    std::printf("%-40s | %14s | %8s | %12s\n", "benchmark", "ops", "ms", "ns/op");
    std::printf("%s\n", std::string(kHeaderRuleWidth, '-').c_str());
    bench_deque_owner(kDequeOwnerOps);
    bench_deque_concurrent(kDequeConcurrentOps);
    bench_mpmc(kMpmcPerProducer);
    bench_yarn_dispatch(kYarnDispatchOps);
    bench_yarn_dispatch_callable(kYarnDispatchOps);
    bench_task_chain(kTaskChainIters);
    return 0;
}
