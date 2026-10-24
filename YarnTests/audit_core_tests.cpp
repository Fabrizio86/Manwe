//
// Regression tests for the code-audit fixes: core.
//

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <stop_token>
#include <filesystem>
#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../Yarn/includes/Cancellation.h"
#include "../Yarn/includes/Coroutines.h"
#include "../Yarn/includes/Fiber.hpp"
#include "../Yarn/includes/ITask.hpp"
#include "../Yarn/includes/Log.h"
#include "../Yarn/includes/Metrics.h"
#include "../Yarn/includes/SmallObjectPool.h"
#include "../Yarn/includes/Timers.h"
#include "../Yarn/includes/Timeout.h"
#include "../Yarn/includes/Trace.h"
#include "../Yarn/includes/Waitable.hpp"
#include "../Yarn/includes/Yarn.hpp"
#include "../Yarn/includes/YarnBall.hpp"

using namespace std::chrono_literals;

namespace {

    template<typename Pred>
    bool audit_wait_for(Pred &&p, std::chrono::steady_clock::duration timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!p()) {
            if (std::chrono::steady_clock::now() >= deadline) return p();
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    /// Sleeps @p d, then records the elapsed time (ns, as a running minimum) and counts itself done.
    YarnBall::Task<> timedSleeper(std::chrono::milliseconds d, std::atomic<long long> *minNs, std::atomic<int> *done) {
        const auto t0 = std::chrono::steady_clock::now();
        co_await YarnBall::sleepFor(d);
        const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        long long cur = minNs->load();
        while (ns < cur && !minNs->compare_exchange_weak(cur, ns)) {
        }
        done->fetch_add(1);
    }

    YarnBall::Task<> sleepTask(std::chrono::milliseconds d) { co_await YarnBall::sleepFor(d); }

    YarnBall::Task<int> instantValue() { co_return 7; }

    YarnBall::Task<> instantUnderTimeout(std::atomic<int> *ok, std::atomic<int> *timedOut) {
        try {
            if (co_await YarnBall::withTimeout(instantValue(), std::chrono::seconds(30)) == 7) ok->fetch_add(1);
        } catch (const YarnBall::TimeoutException &) {
            timedOut->fetch_add(1);
        }
    }

    std::size_t openFdCount() {
#ifdef __linux__
        std::size_t n = 0;
        for (const auto &e : std::filesystem::directory_iterator("/proc/self/fd")) { (void) e; ++n; }
        return n;
#else
        return 0;
#endif
    }

    struct TaskProbe {
        std::atomic<int> ran{0};
        std::atomic<int> failed{0};
        std::atomic<int> deleted{0};
        std::mutex mu;
        std::string message;
    };

    class ProbeTask final : public YarnBall::ITask {
    public:
        explicit ProbeTask(TaskProbe &p) : probe(p) {}
        ~ProbeTask() override { this->probe.deleted.fetch_add(1); }
        void run() override { this->probe.ran.fetch_add(1); }
        void exception(std::exception_ptr e) override {
            try { std::rethrow_exception(e); }
            catch (const std::exception &ex) {
                std::lock_guard<std::mutex> lk(this->probe.mu);
                this->probe.message = ex.what();
            }
            this->probe.failed.fetch_add(1);
        }
    private:
        TaskProbe &probe;
    };

    class BlockerTask final : public YarnBall::ITask {
    public:
        BlockerTask(std::atomic<bool> &started, std::atomic<bool> &release) : started(started), release(release) {}
        void run() override {
            this->started.store(true);
            while (!this->release.load()) std::this_thread::sleep_for(1ms);
        }
        void exception(std::exception_ptr) override {}
    private:
        std::atomic<bool> &started;
        std::atomic<bool> &release;
    };

    // Forwards only exception(): stands in for the pool's shared-owner adapter around a Waitable.
    class ForwardFailure final : public YarnBall::ITask {
    public:
        explicit ForwardFailure(YarnBall::sITask t) : inner(std::move(t)) {}
        void run() override {}
        void exception(std::exception_ptr e) override { this->inner->exception(std::move(e)); }
    private:
        YarnBall::sITask inner;
    };

    class IdleWaitable final : public YarnBall::Waitable {};

    class DerivedTask final : public YarnBall::ITask {
    public:
        explicit DerivedTask(std::atomic<int> &n) : n(n) {}
        void run() override { this->n.fetch_add(1); }
        void exception(std::exception_ptr) override {}
    private:
        std::atomic<int> &n;
    };

} // namespace

// Audit: tasks stranded in a stopped fiber's inbox were deleted silently; they must be failed through exception().
TEST(audit_core_stopped_fiber_fails_stranded_tasks) {
    using namespace YarnBall;
    TaskProbe probe;
    std::atomic<bool> started{false}, release{false};
    auto fiber = std::make_unique<Fiber>(
        FiberId{900}, 64,
        [](FiberId) {}, [](FiberId) {}, [] { return false; }, [](TaskPtr) {},
        [](FiberId) -> TaskPtr { return nullptr; }, [] {}, /*temp=*/false);
    EXPECT_TRUE(fiber->tryPushInbox(new BlockerTask(started, release)));
    EXPECT_TRUE(audit_wait_for([&] { return started.load(); }, 5s));
    // The worker is stuck inside the blocker, so these two can only be stranded.
    EXPECT_TRUE(fiber->tryPushInbox(new ProbeTask(probe)));
    EXPECT_TRUE(fiber->tryPushInbox(new ProbeTask(probe)));
    fiber->stop();
    release.store(true);
    fiber->join();
    fiber->abandonPending();
    EXPECT_EQ(probe.failed.load(), 0);
    fiber.reset();
    EXPECT_EQ(probe.ran.load(), 0);
    EXPECT_EQ(probe.failed.load(), 2);
    EXPECT_EQ(probe.deleted.load(), 2);
    std::lock_guard<std::mutex> lk(probe.mu);
    EXPECT_TRUE(probe.message.find("shut down") != std::string::npos);
}

// Audit: a Waitable whose task is discarded at shutdown blocked wait() forever.
TEST(audit_core_discarded_task_unblocks_waitable) {
    auto w = std::make_shared<IdleWaitable>();
    std::atomic<bool> returned{false};
    std::thread waiter([&] { w->wait(); returned.store(true); });
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(returned.load());
    YarnBall::detail::discardTask(new ForwardFailure(w));
    EXPECT_TRUE(audit_wait_for([&] { return returned.load(); }, 5s));
    waiter.join();
    EXPECT_TRUE(w->hasFailed());
    EXPECT_TRUE(w->errorMessage().find("shut down") != std::string::npos);
}

// Audit: Yarn::run(F&&) must not hijack a unique_ptr<DerivedTask>; it is an ITask owner, not a callable.
TEST(audit_core_run_accepts_unique_ptr_of_derived_task) {
    std::atomic<int> n{0};
    std::unique_ptr<DerivedTask> t = std::make_unique<DerivedTask>(n);
    YarnBall::Yarn::instance()->run(std::move(t));
    EXPECT_TRUE(audit_wait_for([&] { return n.load() == 1; }, 5s));
    YarnBall::run(std::make_unique<DerivedTask>(n));
    EXPECT_TRUE(audit_wait_for([&] { return n.load() == 2; }, 5s));
}

// Audit: exceptions escaping a Yarn::run(callable) were swallowed; they now reach the unhandled-exception handler.
TEST(audit_core_callable_exception_is_reported) {
    std::atomic<int> seen{0};
    YarnBall::setUnhandledExceptionHandler([&](std::exception_ptr e) {
        try { std::rethrow_exception(e); }
        catch (const std::runtime_error &ex) {
            if (std::string(ex.what()) == "audit-callable-boom") seen.fetch_add(1);
        } catch (...) {}
    });
    YarnBall::Yarn::instance()->run([] { throw std::runtime_error("audit-callable-boom"); });
    const bool ok = audit_wait_for([&] { return seen.load() == 1; }, 5s);
    YarnBall::setUnhandledExceptionHandler({});
    EXPECT_TRUE(ok);
}

// Audit: metric names are written verbatim into the exposition text.
TEST(audit_core_metric_names_are_validated) {
    auto &reg = YarnBall::metrics::Registry::instance();
    EXPECT_THROWS_AS(reg.counter("bad name"), std::invalid_argument);
    EXPECT_THROWS_AS(reg.gauge("x\n# TYPE evil counter\nevil"), std::invalid_argument);
    EXPECT_THROWS_AS(reg.histogram("h{le=\"1\"}", {1.0}), std::invalid_argument);
    EXPECT_THROWS_AS(reg.counter("1leading_digit"), std::invalid_argument);
    EXPECT_THROWS_AS(reg.counter(""), std::invalid_argument);
    reg.counter("audit_ok:name_1").inc();
}

TEST(audit_core_histogram_infinity_does_not_poison_sum_and_scrape_is_consistent) {
    auto &reg = YarnBall::metrics::Registry::instance();
    auto &h = reg.histogram("audit_hist_inf", {1.0, 1.0000000000000002, 10.0});
    h.observe(0.5);
    h.observe(std::numeric_limits<double>::infinity());
    h.observe(std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(h.count(), std::uint64_t{2});
    EXPECT_EQ(h.sum(), 0.5);
    const auto snap = h.snapshot();
    for (std::size_t i = 1; i < snap.size(); ++i) EXPECT_TRUE(snap[i].second >= snap[i - 1].second);
    EXPECT_EQ(snap.back().second, std::uint64_t{2});
    const std::string body = reg.scrape();
    EXPECT_TRUE(body.find("audit_hist_inf_count 2\n") != std::string::npos);
    EXPECT_TRUE(body.find("audit_hist_inf_sum 0.5\n") != std::string::npos);
    // Bounds one ulp apart must not collapse into the same label.
    EXPECT_TRUE(body.find("le=\"1\"") != std::string::npos);
    EXPECT_TRUE(body.find("le=\"1.0000000000000002\"") != std::string::npos);
}

TEST(audit_core_log_json_string_sanitizes_utf8_and_del) {
    using YarnBall::log::detail::jsonString;
    EXPECT_EQ(jsonString("a\x7f" "b"), std::string("\"a\\u007fb\""));
    EXPECT_EQ(jsonString("\xff"), std::string("\"\xEF\xBF\xBD\""));
    EXPECT_EQ(jsonString("\xC0\xAF"), std::string("\"\xEF\xBF\xBD\xEF\xBF\xBD\""));   // overlong
    EXPECT_EQ(jsonString("\xED\xA0\x80"), std::string("\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\""));  // surrogate
    EXPECT_EQ(jsonString("\xE2\x82"), std::string("\"\xEF\xBF\xBD\xEF\xBF\xBD\""));   // truncated
    EXPECT_EQ(jsonString("\xE2\x82\xAC"), std::string("\"\xE2\x82\xAC\""));            // valid euro sign
    EXPECT_EQ(jsonString("\xF0\x9F\x98\x80"), std::string("\"\xF0\x9F\x98\x80\""));    // valid emoji
}

TEST(audit_core_log_sink_calls_are_serialised_and_reentrant) {
    using namespace YarnBall;
    std::atomic<int> inSink{0}, maxInSink{0}, lines{0};
    std::atomic<bool> reentered{false};
    const auto prevLevel = log::minLevel();
    log::setMinLevel(log::Level::Info);
    log::setSink([&](std::string_view line) {
        thread_local int depth = 0;  // a nested call on the same thread is the re-entry, not a concurrent sink call
        const bool outermost = depth++ == 0;
        if (outermost) {
            const int now = inSink.fetch_add(1) + 1;
            int prev = maxInSink.load();
            while (now > prev && !maxInSink.compare_exchange_weak(prev, now)) {}
        }
        if (line.find("outer") != std::string_view::npos && !reentered.exchange(true)) {
            log::info("inner");  // a sink that logs must not deadlock
        }
        std::this_thread::sleep_for(100us);
        lines.fetch_add(1);
        if (outermost) inSink.fetch_sub(1);
        --depth;
    });
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i) {
        ts.emplace_back([] { for (int k = 0; k < 20; ++k) log::info("outer"); });
    }
    for (auto &t : ts) t.join();
    log::setSink({});
    log::setMinLevel(prevLevel);
    EXPECT_EQ(maxInSink.load(), 1);
    EXPECT_EQ(lines.load(), 81);
}

TEST(audit_core_trace_child_of_empty_is_root_and_uppercase_hex_rejected) {
    using namespace YarnBall::trace;
    const Context c = newChild(Context{});
    EXPECT_FALSE(c.empty());
    EXPECT_TRUE(c.sampled());
    EXPECT_FALSE(parseTraceparent("00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01").empty());
    EXPECT_TRUE(parseTraceparent("00-0AF7651916CD43DD8448EB211C80319C-b7ad6b7169203331-01").empty());
    EXPECT_TRUE(parseTraceparent("00-0af7651916cd43dd8448eb211c80319c-B7AD6B7169203331-01").empty());
    EXPECT_TRUE(parseTraceparent("00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-0A").empty());
}

TEST(audit_core_trace_scope_destroyed_on_other_thread_leaves_that_thread_alone) {
    using namespace YarnBall::trace;
    const Context a = newRoot();
    const Context b = newRoot();
    std::unique_ptr<Scope> scopeA;
    std::thread owner([&] { scopeA = std::make_unique<Scope>(a); });
    owner.join();
    Scope scopeB(b);
    scopeA.reset();  // destroyed here, not on its owner thread
    EXPECT_TRUE(current().traceId == b.traceId);
}

TEST(audit_core_sleep_durations_saturate_instead_of_overflowing) {
    using namespace YarnBall;
    EXPECT_TRUE(sleepFor(std::chrono::hours::max()).duration == std::chrono::nanoseconds::max());
    EXPECT_TRUE(sleepFor(std::chrono::seconds(-5)).duration == std::chrono::nanoseconds::zero());
    EXPECT_TRUE(sleepFor(std::chrono::milliseconds(3)).duration == std::chrono::milliseconds(3));
    EXPECT_TRUE(sleepUntil(std::chrono::steady_clock::time_point::max()).duration > std::chrono::hours(24 * 365 * 100));
    EXPECT_TRUE(sleepUntil(std::chrono::steady_clock::time_point::min()).duration == std::chrono::nanoseconds::zero());
}

// Audit: the shared freelist is capped and frees the surplus; blocks past the cap must be released without touching
// the ones still cached locally.
TEST(audit_core_small_object_pool_survives_surplus_beyond_global_cap) {
    using Pool = YarnBall::detail::SmallObjectPool<512>;
    constexpr int kBlocks = 30000;  // well past both the per-thread high-water mark and the global cap
    for (int round = 0; round < 2; ++round) {
        std::vector<unsigned char *> blocks;
        blocks.reserve(kBlocks);
        for (int i = 0; i < kBlocks; ++i) {
            auto *p = static_cast<unsigned char *>(Pool::allocate());
            std::memset(p, i & 0xFF, 512);
            blocks.push_back(p);
        }
        for (int i = 0; i < kBlocks; ++i) {
            EXPECT_EQ(blocks[i][0], static_cast<unsigned char>(i & 0xFF));
            EXPECT_EQ(blocks[i][511], static_cast<unsigned char>(i & 0xFF));
        }
        for (auto *p : blocks) Pool::deallocate(p);
    }
    // A thread that exits flushes its cache through the same capped path.
    std::thread t([] {
        std::vector<void *> v;
        for (int i = 0; i < 20000; ++i) v.push_back(Pool::allocate());
        for (void *p : v) Pool::deallocate(p);
    });
    t.join();
}

TEST(audit_core_timers_many_concurrent_sleeps_use_a_fixed_number_of_fds) {
    constexpr int kSleepers = 20000;
    constexpr auto kSleep = 50ms;
    const std::size_t fdsBefore = openFdCount();
    std::atomic<long long> minNs{std::numeric_limits<long long>::max()};
    std::atomic<int> done{0};
    for (int i = 0; i < kSleepers; ++i) YarnBall::coSpawn(timedSleeper(kSleep, &minNs, &done));
    // Sample while they are parked: a timerfd per sleep would blow far past any sane fd limit here.
    EXPECT_TRUE(openFdCount() < fdsBefore + 64);
    EXPECT_TRUE(audit_wait_for([&] { return done.load() == kSleepers; }, 20s));
    EXPECT_EQ(done.load(), kSleepers);
    // Never early: the table fires a sleep only once its own deadline has passed.
    EXPECT_TRUE(minNs.load() >= std::chrono::duration_cast<std::chrono::nanoseconds>(kSleep).count());
}

TEST(audit_core_timers_earlier_deadline_registered_later_fires_first) {
    std::atomic<long long> minLong{std::numeric_limits<long long>::max()};
    std::atomic<long long> minShort{std::numeric_limits<long long>::max()};
    std::atomic<int> doneLong{0}, doneShort{0};
    YarnBall::coSpawn(timedSleeper(400ms, &minLong, &doneLong));
    std::this_thread::sleep_for(20ms); // the long one has armed the shared timer for ~400ms
    YarnBall::coSpawn(timedSleeper(30ms, &minShort, &doneShort));
    EXPECT_TRUE(audit_wait_for([&] { return doneShort.load() == 1; }, 250ms)); // not stuck behind the long deadline
    EXPECT_EQ(doneLong.load(), 0);
    EXPECT_TRUE(audit_wait_for([&] { return doneLong.load() == 1; }, 5s));
}

TEST(audit_core_timers_cancel_storm_leaves_the_timer_service_healthy) {
    constexpr int kTasks = 20000;
    std::atomic<int> ok{0}, timedOut{0};
    for (int i = 0; i < kTasks; ++i) YarnBall::coSpawn(instantUnderTimeout(&ok, &timedOut));
    EXPECT_TRUE(audit_wait_for([&] { return ok.load() + timedOut.load() == kTasks; }, 20s));
    EXPECT_EQ(ok.load(), kTasks);
    EXPECT_EQ(timedOut.load(), 0);
    // Cancelled 30 s timers leave no residue that delays or breaks a fresh sleep.
    std::atomic<long long> minNs{std::numeric_limits<long long>::max()};
    std::atomic<int> done{0};
    YarnBall::coSpawn(timedSleeper(25ms, &minNs, &done));
    EXPECT_TRUE(audit_wait_for([&] { return done.load() == 1; }, 2s));
    EXPECT_TRUE(minNs.load() >= std::chrono::duration_cast<std::chrono::nanoseconds>(25ms).count());
}

TEST(audit_core_timers_sleep_cancelled_while_racing_its_expiry_resumes_exactly_once) {
    // Cancel at roughly the deadline: fire and cancel race for the single resume. Each sleep must finish exactly once.
    constexpr int kRounds = 400;
    std::atomic<int> resumed{0};
    std::vector<std::stop_source> sources(kRounds);
    for (int i = 0; i < kRounds; ++i) {
        YarnBall::coSpawn([](std::stop_token tok, std::atomic<int> *r) -> YarnBall::Task<> {
            try {
                co_await YarnBall::withCancellation(tok, sleepTask(2ms));
            } catch (const YarnBall::CancelledException &) {
            }
            r->fetch_add(1);
        }(sources[i].get_token(), &resumed));
        std::this_thread::sleep_for(std::chrono::microseconds(1900 + (i % 5) * 100));
        sources[i].request_stop();
    }
    EXPECT_TRUE(audit_wait_for([&] { return resumed.load() >= kRounds; }, 10s));
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(resumed.load(), kRounds);
}

// Audit (pass 2): a callable discarded at shutdown reported "unhandled exception ... pool shut down" through the
// user's handler (or the log) once per discarded task.
TEST(audit_core_shutdown_discard_of_callable_is_not_reported_as_unhandled) {
    std::atomic<int> reported{0};
    // The handler captures a local by reference: restore the default on every exit path, including a failed EXPECT.
    struct HandlerReset {
        ~HandlerReset() { YarnBall::setUnhandledExceptionHandler({}); }
    } handlerReset;
    YarnBall::setUnhandledExceptionHandler([&](std::exception_ptr) { reported.fetch_add(1); });
    using Fn = decltype([] {});
    YarnBall::detail::discardTask(new YarnBall::detail::CallableITask<Fn>(Fn{}));
    YarnBall::detail::reportDetachedException(std::make_exception_ptr(YarnBall::detail::PoolShutdownError()));
    EXPECT_EQ(reported.load(), 0);
    // A real failure still gets through.
    YarnBall::detail::reportDetachedException(std::make_exception_ptr(std::runtime_error("real")));
    EXPECT_EQ(reported.load(), 1);
}

// Audit (pass 2): a burst of submissions from one worker poked the same parked fiber, so K tasks ran on ~1 thread.
TEST(audit_core_burst_of_spawns_fans_out_over_parked_workers) {
    const int k = std::min(4, YarnBall::Yarn::instance()->stats().permanentWorkers);
    if (k < 2) return;
    std::this_thread::sleep_for(100ms);  // let the pool park
    std::atomic<int> done{0};
    const auto t0 = std::chrono::steady_clock::now();
    YarnBall::Yarn::instance()->run([&] {
        for (int i = 0; i < k; ++i) {
            YarnBall::Yarn::instance()->run([&] { std::this_thread::sleep_for(100ms); done.fetch_add(1); });
        }
    });
    EXPECT_TRUE(audit_wait_for([&] { return done.load() == k; }, 10s));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_TRUE(ms < 100 * k * 3 / 4);  // serial would be 100 * k
}

// Audit (pass 2): when every permanent worker blocks on a child that sits in its own queue, the pool never grew
// (growth only followed an injection backlog of 64) and deadlocked with temp slots free.
TEST(audit_core_pool_grows_when_every_worker_is_blocked_on_queued_work) {
    auto *y = YarnBall::Yarn::instance();
    const int n = y->stats().permanentWorkers;
    std::atomic<int> arrived{0}, kidsDone{0}, finished{0};
    std::atomic<bool> giveUp{false};
    for (int i = 0; i < n; ++i) {
        y->run([&] {
            y->run([&] { kidsDone.fetch_add(1); });  // lands in this worker's own deque
            arrived.fetch_add(1);
            while (kidsDone.load() == 0 && !giveUp.load()) std::this_thread::sleep_for(1ms);
            finished.fetch_add(1);
        });
    }
    const bool ok = audit_wait_for([&] { return finished.load() == n; }, 15s);
    giveUp.store(true);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(audit_wait_for([&] { return finished.load() == n && kidsDone.load() == n; }, 10s));
}

// Audit (pass 2): a trace Scope destroyed on another thread left the origin thread's context installed.
TEST(audit_core_trace_scope_destroyed_elsewhere_is_unwound_on_its_owner_thread) {
    using namespace YarnBall::trace;
    const Context a = newRoot();
    std::unique_ptr<Scope> scopeA;
    std::atomic<int> phase{0};
    bool ownerSawA = false, ownerSawNothing = false;
    std::thread owner([&] {
        scopeA = std::make_unique<Scope>(a);
        ownerSawA = current().traceId == a.traceId;
        phase.store(1);
        while (phase.load() != 2) std::this_thread::sleep_for(1ms);
        ownerSawNothing = current().empty();  // the other thread destroyed the scope
        // Later work on this thread starts from a clean slate, and nested scopes still restore properly.
        const Context b = newRoot();
        {
            Scope sb(b);
            if (current().traceId != b.traceId) ownerSawNothing = false;
        }
        if (!current().empty()) ownerSawNothing = false;
    });
    while (phase.load() != 1) std::this_thread::sleep_for(1ms);
    scopeA.reset();
    phase.store(2);
    owner.join();
    EXPECT_TRUE(ownerSawA);
    EXPECT_TRUE(ownerSawNothing);
}

// Audit (pass 2): a context with only the trace id zeroed yielded an invalid child / traceparent.
TEST(audit_core_trace_context_with_zero_trace_or_span_id_is_invalid) {
    using namespace YarnBall::trace;
    Context c = newRoot();
    EXPECT_FALSE(c.empty());
    Context noTrace = c;
    noTrace.traceId.fill(0);
    EXPECT_TRUE(noTrace.empty());
    EXPECT_TRUE(toTraceparent(noTrace).empty());
    const Context child = newChild(noTrace);
    EXPECT_FALSE(child.empty());
    EXPECT_FALSE(toTraceparent(child).empty());
    Context noSpan = c;
    noSpan.spanId.fill(0);
    EXPECT_TRUE(noSpan.empty());
    EXPECT_TRUE(toTraceparent(noSpan).empty());
}

// Audit (pass 2): a counter `foo_count` next to a histogram `foo` produced duplicate exposition series.
TEST(audit_core_metric_names_colliding_with_histogram_series_are_rejected) {
    auto &reg = YarnBall::metrics::Registry::instance();
    reg.histogram("audit_dup_h", {1.0});
    EXPECT_THROWS_AS(reg.counter("audit_dup_h_count"), std::logic_error);
    EXPECT_THROWS_AS(reg.gauge("audit_dup_h_sum"), std::logic_error);
    EXPECT_THROWS_AS(reg.counter("audit_dup_h_bucket"), std::logic_error);
    reg.counter("audit_dup_c_count").inc();
    EXPECT_THROWS_AS(reg.histogram("audit_dup_c", {1.0}), std::logic_error);
    // Unrelated names that merely share a prefix are fine.
    reg.counter("audit_dup_h_total").inc();
    reg.counter("audit_dup_counter").inc();
}
