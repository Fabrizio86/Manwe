//
// Regression tests for the code-audit fixes: async.
//

#include "test_framework.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "../Yarn/includes/AsyncSync.h"
#include "../Yarn/includes/Cancellation.h"
#include "../Yarn/includes/Coroutines.h"
#include "../Yarn/includes/FileIo.h"
#include "../Yarn/includes/IoAwaiters.h"
#include "../Yarn/includes/JoinHandle.h"
#include "../Yarn/includes/Stream.h"
#include "../Yarn/includes/Timeout.h"
#include "../Yarn/includes/Trace.h"
#include "../Yarn/includes/WhenAll.h"
#include "../Yarn/includes/WhenAny.h"
#include "../Yarn/includes/Yarn.hpp"

#if !defined(_WIN32)
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #include "../Yarn/includes/SignalSet.h"
#endif

using namespace std::chrono_literals;

namespace {

    using YarnBall::AsyncNotify;
    using YarnBall::CancelledException;
    using YarnBall::Task;

    template<typename Pred>
    bool async_wait_for(Pred &&p, std::chrono::steady_clock::duration timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!p()) {
            if (std::chrono::steady_clock::now() >= deadline) return p();
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    /// Parks on @p n; counts a cancellation wake-up in @p seen and rethrows it.
    Task<> parkAndCountCancel(AsyncNotify *n, std::atomic<int> *seen) {
        try {
            co_await n->notified();
        } catch (const CancelledException &) {
            seen->fetch_add(1);
            throw;
        }
    }

    Task<> onceWaiter(YarnBall::AsyncOnce *once, std::atomic<int> *ran, std::atomic<bool> *done) {
        co_await once->callOnce([ran]() -> Task<> {
            ran->fetch_add(1);
            co_return;
        });
        done->store(true);
    }

    Task<bool> traceMatches(YarnBall::trace::Context expect) {
        const auto c = co_await YarnBall::trace::currentAsync();
        co_return c.traceId == expect.traceId && c.spanId == expect.spanId;
    }

    /// Clears the thread's submission-failure hook on scope exit, however the test ends.
    struct SubmitFailureGuard {
        SubmitFailureGuard(int skip, int count) { YarnBall::Yarn::failSubmissionsForTest(skip, count); }
        ~SubmitFailureGuard() { YarnBall::Yarn::failSubmissionsForTest(0, 0); }
    };

    /// Body that records it ran; holds @p sentinel so its frame's destruction is observable as a use_count drop.
    Task<> sentinelBody([[maybe_unused]] std::shared_ptr<int> sentinel, std::atomic<int> *ran) {
        ran->fetch_add(1);
        co_return;
    }

    Task<> whenAllOfSentinels(std::shared_ptr<int> sentinel, std::atomic<int> *ran) {
        std::vector<Task<>> tasks;
        for (int i = 0; i < 3; ++i) tasks.push_back(sentinelBody(sentinel, ran));
        co_await YarnBall::whenAll(std::move(tasks));
    }

    Task<std::size_t> whenAnyOfSentinels(std::shared_ptr<int> sentinel, std::atomic<int> *ran) {
        std::vector<Task<>> tasks;
        for (int i = 0; i < 3; ++i) tasks.push_back(sentinelBody(sentinel, ran));
        co_return co_await YarnBall::whenAny(std::move(tasks));
    }

    /// Counts its own destruction, so a test can see when a coroutine frame's parameters are gone.
    struct DtorCounter {
        std::atomic<int> *n;
        explicit DtorCounter(std::atomic<int> *c) : n(c) {}
        DtorCounter(const DtorCounter &) = delete;
        DtorCounter(DtorCounter &&o) noexcept : n(std::exchange(o.n, nullptr)) {}
        ~DtorCounter() { if (n) n->fetch_add(1); }
    };

    Task<int> guardedValue([[maybe_unused]] DtorCounter guard, int v) { co_return v; }

    Task<> joinGuarded(std::atomic<int> *gone, int *failures) {
        auto h = YarnBall::spawnJoinable(guardedValue(DtorCounter{gone}, 1));
        const int v = co_await h.join();
        // The joiner is woken only after the runner has destroyed the spawned task's frame (and its parameters).
        if (v != 1 || gone->load() < 1) ++*failures;
        gone->store(0);
    }

    Task<> allGuarded(std::atomic<int> *gone, int *failures) {
        std::vector<Task<int>> tasks;
        for (int i = 0; i < 4; ++i) tasks.push_back(guardedValue(DtorCounter{gone}, i));
        (void) co_await YarnBall::whenAll(std::move(tasks));
        if (gone->load() != 4) ++*failures;
    }

    Task<int> traceThroughCombinators() {
        const auto ctx = YarnBall::trace::newRoot();
        co_await YarnBall::trace::installCurrent(ctx);
        int ok = 0;

        std::vector<Task<bool>> all;
        all.push_back(traceMatches(ctx));
        all.push_back(traceMatches(ctx));
        for (bool b : co_await YarnBall::whenAll(std::move(all))) ok += b ? 1 : 0;

        std::vector<Task<bool>> any;
        any.push_back(traceMatches(ctx));
        if ((co_await YarnBall::whenAny(std::move(any))).value) ++ok;

        if (co_await YarnBall::withTimeout(traceMatches(ctx), 5s)) ++ok;
        co_return ok;
    }

    YarnBall::Stream<std::string> yieldLvalue(std::string *after) {
        std::string s = "hello";
        co_yield s;
        *after = s; // the consumer moved the yielded value out; the producer's variable must be untouched
    }

    Task<std::string> pullAll(YarnBall::Stream<std::string> s) {
        std::string got;
        while (auto v = co_await s.next()) got += *v;
        co_return got;
    }

    YarnBall::Stream<int> parkedProducer(AsyncNotify *n) {
        co_await n->notified();
        co_yield 1;
    }

    Task<int> pullOne(AsyncNotify *n) {
        auto s = parkedProducer(n);
        auto v = co_await s.next();
        co_return v ? *v : -1;
    }

    /// 1 = got a value, 2 = cancelled.
    Task<> pullUnderScope(std::stop_token tok, AsyncNotify *n, std::atomic<int> *out) {
        try {
            co_await YarnBall::withCancellation(tok, pullOne(n));
            out->store(1);
        } catch (const CancelledException &) {
            out->store(2);
        }
    }

    std::string writeTempFile(const std::string &name, std::size_t size) {
        const auto path = (std::filesystem::temp_directory_path() / name).string();
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << std::string(size, 'x');
        return path;
    }

}

// ---- AsyncOnce -------------------------------------------------------------

TEST(audit_async_once_runner_frame_destroyed_hands_run_to_waiter) {
    YarnBall::AsyncOnce once;
    AsyncNotify never;
    std::atomic<int> ran{0};
    std::atomic<bool> done{false};

    auto runner = once.callOnce([&never]() -> Task<> { co_await never.notified(); });
    runner.getHandle().resume(); // runs until it parks inside the callable
    EXPECT_TRUE(async_wait_for([&] { return never.waiterCount() == 1; }));

    YarnBall::coSpawn(onceWaiter(&once, &ran, &done));
    std::this_thread::sleep_for(50ms); // let the waiter park behind the running initialiser

    { auto drop = std::move(runner); } // frame destroyed while suspended inside the callable

    EXPECT_TRUE(async_wait_for([&] { return done.load(); }));
    EXPECT_EQ(ran.load(), 1);
    EXPECT_TRUE(once.isCompleted());
}

// ---- whenAny / whenAll -----------------------------------------------------

TEST(audit_async_when_any_frame_destroyed_while_parked_stops_branches) {
    AsyncNotify n;
    std::atomic<int> seen{0};
    std::vector<Task<>> branches;
    branches.push_back(parkAndCountCancel(&n, &seen));
    branches.push_back(parkAndCountCancel(&n, &seen));

    auto t = YarnBall::whenAny(std::move(branches));
    t.getHandle().resume(); // spawns the runners and parks
    EXPECT_TRUE(async_wait_for([&] { return n.waiterCount() == 2; }));

    { auto drop = std::move(t); }

    // The branches were stopped when the awaiting frame died, and nothing resumes the dead frame afterwards.
    EXPECT_TRUE(async_wait_for([&] { return seen.load() == 2; }));
    n.notifyAll();
    std::this_thread::sleep_for(20ms);
}

TEST(audit_async_when_all_frame_destroyed_while_parked_stops_branches) {
    AsyncNotify n;
    std::atomic<int> seen{0};
    std::vector<Task<>> branches;
    branches.push_back(parkAndCountCancel(&n, &seen));
    branches.push_back(parkAndCountCancel(&n, &seen));

    auto t = YarnBall::whenAll(std::move(branches));
    t.getHandle().resume();
    EXPECT_TRUE(async_wait_for([&] { return n.waiterCount() == 2; }));

    { auto drop = std::move(t); }

    EXPECT_TRUE(async_wait_for([&] { return seen.load() == 2; }));
    n.notifyAll();
    std::this_thread::sleep_for(20ms);
}

TEST(audit_async_when_any_empty_input_throws) {
    EXPECT_THROWS_AS(YarnBall::syncWait(YarnBall::whenAny(std::vector<Task<>>{})), std::invalid_argument);
    EXPECT_THROWS_AS(YarnBall::syncWait(YarnBall::whenAny(std::vector<Task<int>>{})), std::invalid_argument);
}

TEST(audit_async_combinator_runners_inherit_trace_context) {
    EXPECT_EQ(YarnBall::syncWait(traceThroughCombinators()), 4);
}

// ---- submission failure ------------------------------------------------------

TEST(audit_async_cospawn_frees_the_frame_when_the_pool_refuses_it) {
    auto sentinel = std::make_shared<int>(0);
    std::atomic<int> ran{0};
    {
        SubmitFailureGuard guard(0, 1);
        EXPECT_THROWS_AS(YarnBall::coSpawn(sentinelBody(sentinel, &ran)), std::bad_alloc);
    }
    EXPECT_EQ(sentinel.use_count(), 1); // the never-started frame was destroyed, not leaked
    EXPECT_EQ(ran.load(), 0);

    // The pool is unharmed: the next spawn runs.
    YarnBall::coSpawn(sentinelBody(sentinel, &ran));
    EXPECT_TRUE(async_wait_for([&] { return ran.load() == 1; }));
}

TEST(audit_async_run_destroys_the_task_exactly_once_when_it_throws) {
    struct Probe final : YarnBall::ITask {
        std::atomic<int> *ran;
        std::atomic<int> *deleted;
        Probe(std::atomic<int> *r, std::atomic<int> *d) : ran(r), deleted(d) {}
        ~Probe() override { deleted->fetch_add(1); }
        void run() override { ran->fetch_add(1); }
        void exception(std::exception_ptr) override {}
    };
    std::atomic<int> ran{0}, deleted{0};
    {
        SubmitFailureGuard guard(0, 2);
        EXPECT_THROWS_AS(YarnBall::Yarn::instance()->run(YarnBall::uITask{new Probe(&ran, &deleted)}), std::bad_alloc);
        EXPECT_EQ(deleted.load(), 1);
        EXPECT_THROWS_AS(YarnBall::Yarn::instance()->run(YarnBall::sITask{std::make_shared<Probe>(&ran, &deleted)}),
                         std::bad_alloc);
        EXPECT_EQ(deleted.load(), 2);
    }
    EXPECT_EQ(ran.load(), 0);
    YarnBall::Yarn::instance()->run(YarnBall::uITask{new Probe(&ran, &deleted)});
    EXPECT_TRUE(async_wait_for([&] { return ran.load() == 1 && deleted.load() == 3; }));
}

TEST(audit_async_when_all_settles_and_frees_frames_when_a_spawn_fails) {
    auto sentinel = std::make_shared<int>(0);
    std::atomic<int> ran{0};
    {
        SubmitFailureGuard guard(1, 1); // the second runner cannot be submitted
        EXPECT_THROWS_AS(YarnBall::syncWait(whenAllOfSentinels(sentinel, &ran)), std::bad_alloc);
    }
    // The one runner that did start ran; the failed one and the never-spawned third were destroyed unrun.
    EXPECT_TRUE(async_wait_for([&] { return ran.load() == 1 && sentinel.use_count() == 1; }));
}

TEST(audit_async_when_any_settles_and_frees_frames_when_a_spawn_fails) {
    auto sentinel = std::make_shared<int>(0);
    std::atomic<int> ran{0};
    {
        SubmitFailureGuard guard(1, 1);
        // Whichever branch won (the first runner may finish before the failure is recorded) the call must settle
        // without hanging; if the failure won, it is what is reported.
        try {
            (void) YarnBall::syncWait(whenAnyOfSentinels(sentinel, &ran));
        } catch (const std::bad_alloc &) {
        }
    }
    EXPECT_TRUE(async_wait_for([&] { return sentinel.use_count() == 1; }));
}

TEST(audit_async_joiner_wakes_only_after_the_spawned_frame_is_destroyed) {
    for (int i = 0; i < 200; ++i) {
        std::atomic<int> gone{0};
        int failures = 0;
        YarnBall::syncWait(joinGuarded(&gone, &failures));
        EXPECT_EQ(failures, 0);
        YarnBall::syncWait(allGuarded(&gone, &failures));
        EXPECT_EQ(failures, 0);
    }
}

TEST(audit_async_pool_reports_quiescence_after_work_drains) {
    std::atomic<int> ran{0};
    for (int i = 0; i < 2000; ++i) YarnBall::coSpawn(sentinelBody(std::make_shared<int>(0), &ran));
    EXPECT_TRUE(YarnBall::Yarn::instance()->awaitQuiescence(10s));
    EXPECT_EQ(ran.load(), 2000);
}

// ---- Stream ----------------------------------------------------------------

TEST(audit_async_stream_yield_lvalue_leaves_producer_variable_intact) {
    std::string after;
    const std::string got = YarnBall::syncWait(pullAll(yieldLvalue(&after)));
    EXPECT_EQ(got, std::string("hello"));
    EXPECT_EQ(after, std::string("hello"));
}

TEST(audit_async_stream_producer_parked_await_is_cancellable) {
    AsyncNotify n;
    std::stop_source src;
    std::atomic<int> out{0};
    YarnBall::coSpawn(pullUnderScope(src.get_token(), &n, &out));
    EXPECT_TRUE(async_wait_for([&] { return n.waiterCount() == 1; }));
    src.request_stop();
    EXPECT_TRUE(async_wait_for([&] { return out.load() != 0; }));
    EXPECT_EQ(out.load(), 2);
}

TEST(audit_async_stream_producer_still_yields_when_not_cancelled) {
    AsyncNotify n;
    std::stop_source src;
    std::atomic<int> out{0};
    YarnBall::coSpawn(pullUnderScope(src.get_token(), &n, &out));
    EXPECT_TRUE(async_wait_for([&] { return n.waiterCount() == 1; }));
    n.notifyOne();
    EXPECT_TRUE(async_wait_for([&] { return out.load() != 0; }));
    EXPECT_EQ(out.load(), 1);
}

// ---- FileIo ----------------------------------------------------------------

TEST(audit_async_file_read_respects_max_bytes) {
    const std::string path = writeTempFile("yarn_audit_async_maxbytes.bin", 1000);
    EXPECT_EQ(YarnBall::syncWait(YarnBall::fs::readToString(path)).size(), std::size_t{1000});
    EXPECT_EQ(YarnBall::syncWait(YarnBall::fs::readToString(path, 1000)).size(), std::size_t{1000});
    EXPECT_THROWS_AS(YarnBall::syncWait(YarnBall::fs::readToString(path, 999)), YarnBall::fs::FileIoError);
    EXPECT_EQ(YarnBall::syncWait(YarnBall::fs::readToBytes(path, 1000)).size(), std::size_t{1000});
    EXPECT_THROWS_AS(YarnBall::syncWait(YarnBall::fs::readToBytes(path, 10)), YarnBall::fs::FileIoError);
    std::filesystem::remove(path);
}

TEST(audit_async_file_read_large_file_crosses_chunks) {
    const std::size_t size = 200 * 1024 + 7;
    const std::string path = writeTempFile("yarn_audit_async_large.bin", size);
    EXPECT_EQ(YarnBall::syncWait(YarnBall::fs::readToString(path)).size(), size);
    EXPECT_EQ(YarnBall::syncWait(YarnBall::fs::readToBytes(path, size)).size(), size);
    EXPECT_THROWS_AS(YarnBall::syncWait(YarnBall::fs::readToBytes(path, size - 1)), YarnBall::fs::FileIoError);
    std::filesystem::remove(path);
}

// ---- deadlineToken ---------------------------------------------------------

TEST(audit_async_deadline_token_fires) {
    const std::stop_token tok = YarnBall::deadlineToken(30ms);
    EXPECT_TRUE(async_wait_for([&] { return tok.stop_requested(); }));
}

TEST(audit_async_deadline_token_can_be_cancelled) {
    std::stop_source cancel;
    const std::stop_token tok = YarnBall::deadlineToken(100ms, cancel.get_token());
    cancel.request_stop();
    std::this_thread::sleep_for(300ms);
    EXPECT_FALSE(tok.stop_requested());
}

// ---- SignalSet -------------------------------------------------------------

#if !defined(_WIN32)

TEST(audit_async_signal_set_only_receives_subscribed_signals) {
    YarnBall::SignalSet a({SIGUSR1});
    YarnBall::SignalSet b({SIGUSR2});
    // SIGUSR2 first: a set subscribed to SIGUSR1 only must not see it.
    ::kill(::getpid(), SIGUSR2);
    ::kill(::getpid(), SIGUSR1);
    EXPECT_EQ(YarnBall::syncWait(a.next()), static_cast<int>(SIGUSR1));
    EXPECT_EQ(YarnBall::syncWait(b.next()), static_cast<int>(SIGUSR2));
}

TEST(audit_async_signal_set_next_captures_set_at_call_time) {
    auto sigs = std::make_unique<YarnBall::SignalSet>(std::initializer_list<int>{SIGUSR1});
    auto pending = sigs->next(); // lazy: has not started running yet
    sigs.reset();
    EXPECT_THROWS_AS(YarnBall::syncWait(std::move(pending)), YarnBall::SignalCaptureError);
}

TEST(audit_async_signal_set_destroy_with_waiters_never_reuses_descriptor) {
    constexpr int kRounds = 100;
    std::atomic<int> threw{0};
    auto waiter = [](Task<int> pending, std::atomic<int> *out) -> Task<> {
        try {
            co_await std::move(pending);
        } catch (const YarnBall::SignalCaptureError &) {
            out->fetch_add(1);
        }
    };
    for (int i = 0; i < kRounds; ++i) {
        auto sigs = std::make_unique<YarnBall::SignalSet>(std::initializer_list<int>{SIGUSR1});
        // next() is called here, while the set is alive; the task may start before or after the set is gone.
        YarnBall::coSpawn(waiter(sigs->next(), &threw));
        if (i % 2 == 0) std::this_thread::sleep_for(1ms); // alternate "parked" and "not started yet"
        sigs.reset();
    }
    EXPECT_TRUE(async_wait_for([&] { return threw.load() == kRounds; }));
}

TEST(audit_async_signal_set_rejects_out_of_range_signal) {
    EXPECT_THROWS_AS(YarnBall::SignalSet({0}), YarnBall::SignalCaptureError);
    EXPECT_THROWS_AS(YarnBall::SignalSet({100000}), YarnBall::SignalCaptureError);
}

#endif

namespace {
    Task<int> asyncInstantSeven() { co_return 7; }

    Task<> asyncInstantUnderLongTimeout(std::atomic<int> *ok, std::atomic<int> *timedOut) {
        try {
            if (co_await YarnBall::withTimeout(asyncInstantSeven(), 30s) == 7) ok->fetch_add(1);
        } catch (const YarnBall::TimeoutException &) {
            timedOut->fetch_add(1);
        }
    }
}

TEST(audit_async_with_timeout_of_an_instant_task_never_times_out) {
    // The sleeper loses the race and is cancelled while it may still be registering its timer. A cancel redone
    // against a proxy handle that the first cancel had already woken and freed could hit a different sleeper that
    // reused the address, waking it early so that a 30 s timeout "fired" at once. Zero false timeouts allowed.
    constexpr int kRounds = 10, kPerRound = 20000;
    int falseTimeouts = 0;
    for (int r = 0; r < kRounds; ++r) {
        std::atomic<int> ok{0}, timedOut{0};
        for (int i = 0; i < kPerRound; ++i) YarnBall::coSpawn(asyncInstantUnderLongTimeout(&ok, &timedOut));
        EXPECT_TRUE(async_wait_for([&] { return ok.load() + timedOut.load() == kPerRound; }, 60s));
        falseTimeouts += timedOut.load();
    }
    EXPECT_EQ(falseTimeouts, 0);
}

// ---- Second-pass audit regressions -------------------------------------------

namespace {
    Task<> asyncSleepThirty() { co_await YarnBall::sleepFor(30s); }

    /// A chain with two cancel hooks (the scope's and the sleep's): a requestCancel takes both at once.
    Task<> asyncScopedSleep() { co_await YarnBall::withCancellation(std::stop_token{}, asyncSleepThirty()); }

    /// 1 = finished, 2 = cancelled, 3 = other failure.
    Task<> asyncOutcomeOf(Task<> *t, std::atomic<int> *flag) {
        try {
            co_await std::move(*t);
            flag->store(1);
        } catch (const CancelledException &) {
            flag->store(2);
        } catch (...) {
            flag->store(3);
        }
    }

    YarnBall::Stream<std::vector<std::unique_ptr<int>>> asyncMoveOnlyBatches() {
        std::vector<std::unique_ptr<int>> b;
        b.push_back(std::make_unique<int>(7));
        co_yield b; // an lvalue whose copy constructor is declared but cannot be instantiated: handed over by pointer
    }

    Task<int> asyncPullFirstOfBatch() {
        auto s = asyncMoveOnlyBatches();
        auto v = co_await s.next();
        co_return v ? *(*v)[0] : -1;
    }
}

TEST(audit_async_cancel_hooks_survive_an_inline_resume_that_unwinds_the_chain) {
    // The pool refuses the wake task, so the first hook's callback resumes the chain inline; the unwind frees the
    // second hook's owner while it is still in the list of hooks being run. Used to write to / call through freed memory
    // (reported by AddressSanitizer); the second hook must simply be skipped.
    for (int i = 0; i < 20; ++i) {
        Task<> t = asyncScopedSleep();
        std::atomic<int> flag{0};
        YarnBall::coSpawn(asyncOutcomeOf(&t, &flag));
        std::this_thread::sleep_for(30ms); // let the chain park on the sleep
        {
            SubmitFailureGuard guard(0, 3);
            t.requestCancel();
        }
        EXPECT_TRUE(async_wait_for([&] { return flag.load() != 0; }));
        EXPECT_EQ(flag.load(), 2);
    }
}

TEST(audit_async_stream_yield_of_a_move_only_container_lvalue_compiles_and_hands_it_over) {
    EXPECT_EQ(YarnBall::syncWait(asyncPullFirstOfBatch()), 7);
}

#if !defined(_WIN32)

namespace {
    Task<> asyncRecvSignal(YarnBall::SignalSet *s, std::atomic<int> *got) {
        try {
            got->store(co_await s->next());
        } catch (...) {
            got->store(-1);
        }
    }
}

TEST(audit_async_signal_set_ignores_a_signal_raised_in_a_forked_child) {
    YarnBall::SignalSet s({SIGUSR2});
    std::atomic<int> got{0};
    YarnBall::coSpawn(asyncRecvSignal(&s, &got));
    std::this_thread::sleep_for(50ms);
    const pid_t child = ::fork();
    if (child == 0) {
        // Inherited handler and pipe, but another process: must not reach the parent's set.
        ::kill(::getpid(), SIGUSR2);
        ::usleep(100000);
        ::_exit(0);
    }
    EXPECT_TRUE(child > 0);
    int status = 0;
    ::waitpid(child, &status, 0);
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(got.load(), 0);
    ::kill(::getpid(), SIGUSR2); // the parent's own signal still arrives
    EXPECT_TRUE(async_wait_for([&] { return got.load() != 0; }));
    EXPECT_EQ(got.load(), static_cast<int>(SIGUSR2));
}

TEST(audit_async_signal_set_for_sigchld_leaves_waitpid_working) {
    YarnBall::SignalSet s({SIGCHLD});
    const pid_t child = ::fork();
    if (child == 0) ::_exit(3);
    EXPECT_TRUE(child > 0);
    EXPECT_EQ(YarnBall::syncWait(s.next()), static_cast<int>(SIGCHLD));
    int status = 0;
    // An ignored SIGCHLD auto-reaps and waitpid would fail with ECHILD.
    EXPECT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 3);
}

namespace {
    Task<> asyncWaitPipe(int fd) { co_await YarnBall::io::waitReadable(fd); }

    Task<> asyncInstantNothing() { co_return; }

    /// Waits on a pipe that is never written: completing at all, other than by the scope stopping, is a spurious wake.
    Task<> asyncVictim(int fd, std::stop_token tok, std::atomic<int> *spurious, std::atomic<int> *cancelled) {
        try {
            co_await YarnBall::withCancellation(tok, asyncWaitPipe(fd));
            spurious->fetch_add(1);
        } catch (const CancelledException &) {
            cancelled->fetch_add(1);
        }
    }

    /// An fd wait that loses a race against an instant task: cancelled while it may still be registering.
    Task<> asyncLosingFdWait(int fd, std::atomic<int> *done) {
        std::vector<Task<>> branches;
        branches.push_back(asyncWaitPipe(fd));
        branches.push_back(asyncInstantNothing());
        (void) co_await YarnBall::whenAny(std::move(branches));
        done->fetch_add(1);
    }
}

TEST(audit_async_fd_wait_cancelled_while_registering_never_wakes_another_waiter) {
    // Same hazard as the sleep case above, for fd waits: a cancel redone against a proxy address that the first cancel had
    // already woken and freed could hit a later waiter that reused the address (on the same fd), waking it early.
    constexpr int kVictims = 64, kChurnFds = 4, kRounds = 5, kPerRound = 10000;
    std::vector<std::array<int, 2>> victimPipes(kVictims), churnPipes(kChurnFds);
    for (auto &p : victimPipes) EXPECT_EQ(::pipe(p.data()), 0);
    for (auto &p : churnPipes) EXPECT_EQ(::pipe(p.data()), 0);

    std::stop_source stop;
    std::atomic<int> spurious{0}, cancelled{0}, done{0};
    for (auto &p : victimPipes) YarnBall::coSpawn(asyncVictim(p[0], stop.get_token(), &spurious, &cancelled));
    std::this_thread::sleep_for(100ms);

    int expected = 0;
    for (int r = 0; r < kRounds; ++r) {
        for (int i = 0; i < kPerRound; ++i) {
            YarnBall::coSpawn(asyncLosingFdWait(churnPipes[static_cast<std::size_t>(i % kChurnFds)][0], &done));
        }
        expected += kPerRound;
        EXPECT_TRUE(async_wait_for([&] { return done.load() == expected; }, 60s));
    }
    EXPECT_EQ(spurious.load(), 0);

    stop.request_stop();
    EXPECT_TRUE(async_wait_for([&] { return cancelled.load() == kVictims; }));
    EXPECT_EQ(spurious.load(), 0);
    for (auto &p : victimPipes) { ::close(p[0]); ::close(p[1]); }
    for (auto &p : churnPipes) { ::close(p[0]); ::close(p[1]); }
}

#endif

// ---- withTimeout on a direct Reactor timer ----------------------------------------

namespace {
    Task<int> asyncThrowsAfterYield() {
        co_await YarnBall::sleepFor(1ms);
        throw std::runtime_error("boom");
        co_return 0;
    }

    Task<int> asyncTimedThrower() { co_return co_await YarnBall::withTimeout(asyncThrowsAfterYield(), 30s); }

    Task<> asyncTimedSleeper(std::chrono::milliseconds limit) { co_await YarnBall::withTimeout(asyncSleepThirty(), limit); }

    Task<> asyncTimedSleeperInScope(std::stop_token token) {
        co_await YarnBall::withCancellation(token, asyncTimedSleeper(30s));
    }
}

TEST(audit_async_with_timeout_rethrows_the_exception_of_a_task_that_wins_by_throwing) {
    EXPECT_THROWS_AS(YarnBall::syncWait(asyncTimedThrower()), std::runtime_error);
}

TEST(audit_async_with_timeout_zero_limit_times_out_a_sleeping_task) {
    EXPECT_THROWS_AS(YarnBall::syncWait(asyncTimedSleeper(0ms)), YarnBall::TimeoutException);
}

TEST(audit_async_with_timeout_stops_with_the_scope_it_runs_in) {
    std::stop_source src;
    std::atomic<int> flag{0};
    auto t = asyncTimedSleeperInScope(src.get_token());
    YarnBall::coSpawn(asyncOutcomeOf(&t, &flag));
    std::this_thread::sleep_for(50ms);
    const auto t0 = std::chrono::steady_clock::now();
    src.request_stop();
    EXPECT_TRUE(async_wait_for([&] { return flag.load() != 0; }, 5s));
    EXPECT_EQ(flag.load(), 2); // cancelled, not timed out and not stuck for 30 s
    EXPECT_TRUE(std::chrono::steady_clock::now() - t0 < 5s);
}

TEST(audit_async_with_timeout_many_concurrent_early_wins_and_timeouts_stay_exact) {
    constexpr int kN = 2000;
    std::atomic<int> won{0}, expired{0};
    auto early = [](std::atomic<int> *w) -> Task<> {
        if (co_await YarnBall::withTimeout(asyncInstantSeven(), 60s) == 7) w->fetch_add(1);
    };
    auto late = [](std::atomic<int> *e) -> Task<> {
        try {
            co_await YarnBall::withTimeout(asyncSleepThirty(), 5ms);
        } catch (const YarnBall::TimeoutException &) {
            e->fetch_add(1);
        }
    };
    for (int i = 0; i < kN; ++i) {
        YarnBall::coSpawn(early(&won));
        YarnBall::coSpawn(late(&expired));
    }
    EXPECT_TRUE(async_wait_for([&] { return won.load() == kN && expired.load() == kN; }, 60s));
}

TEST(audit_async_deadline_token_withdraw_racing_with_registration_is_clean) {
    // The withdrawal races the timer registration from another thread: whichever order they land in, the timer must be
    // taken back, the long deadline must never fire, and nothing may touch a freed timer.
    constexpr int kRounds = 3000;
    std::vector<std::stop_token> tokens;
    tokens.reserve(kRounds);
    for (int i = 0; i < kRounds; ++i) {
        std::stop_source cancel;
        std::thread racer([&cancel] { cancel.request_stop(); });
        tokens.push_back(YarnBall::deadlineToken(60s, cancel.get_token()));
        racer.join();
    }
    std::this_thread::sleep_for(50ms);
    for (const auto &t : tokens) EXPECT_FALSE(t.stop_requested());
}

TEST(audit_async_deadline_token_fires_and_a_late_withdraw_is_harmless) {
    std::stop_source cancel;
    const std::stop_token tok = YarnBall::deadlineToken(20ms, cancel.get_token());
    EXPECT_TRUE(async_wait_for([&] { return tok.stop_requested(); }, 5s));
    cancel.request_stop(); // after the timer ran: must neither crash nor un-fire
    std::this_thread::sleep_for(20ms);
    EXPECT_TRUE(tok.stop_requested());
}

// ---- Joined losers ------------------------------------------------------------

namespace {
    /// Sleeps until cancelled, then takes a while to unwind; records when it was done.
    Task<> asyncSlowUnwind(std::shared_ptr<std::atomic<bool>> unwound) {
        struct Mark {
            std::shared_ptr<std::atomic<bool>> f;
            ~Mark() { f->store(true); }
        } mark{unwound};
        try {
            co_await YarnBall::sleepFor(30s);
        } catch (const CancelledException &) {
            std::this_thread::sleep_for(150ms); // cleanup that takes time
            throw;
        }
    }

    Task<> asyncJoinedTimeout(std::shared_ptr<std::atomic<bool>> unwound, std::atomic<bool> *unwoundAtThrow) {
        try {
            co_await YarnBall::withTimeout(asyncSlowUnwind(unwound), 20ms, YarnBall::joinLosers);
        } catch (const YarnBall::TimeoutException &) {
            unwoundAtThrow->store(unwound->load());
        }
    }

    Task<> asyncPlainTimeout(std::shared_ptr<std::atomic<bool>> unwound, std::atomic<bool> *unwoundAtThrow) {
        try {
            co_await YarnBall::withTimeout(asyncSlowUnwind(unwound), 20ms);
        } catch (const YarnBall::TimeoutException &) {
            unwoundAtThrow->store(unwound->load());
        }
    }

    Task<> asyncSleepTen() { co_await YarnBall::sleepFor(10ms); }

    Task<std::size_t> asyncJoinedRace(std::shared_ptr<std::atomic<bool>> unwound, std::atomic<bool> *loserDone) {
        std::vector<Task<>> v;
        v.push_back(asyncSleepTen());
        v.push_back(asyncSlowUnwind(unwound));
        const std::size_t winner = co_await YarnBall::whenAny(std::move(v), YarnBall::joinLosers);
        loserDone->store(unwound->load());
        co_return winner;
    }
}

TEST(audit_async_with_timeout_joined_waits_for_the_task_to_unwind_before_throwing) {
    auto unwound = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> seen{false};
    YarnBall::syncWait(asyncJoinedTimeout(unwound, &seen));
    EXPECT_TRUE(seen.load()); // the task had finished unwinding when TimeoutException arrived
}

TEST(audit_async_with_timeout_default_does_not_wait_for_the_task) {
    auto unwound = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> seen{true};
    YarnBall::syncWait(asyncPlainTimeout(unwound, &seen));
    EXPECT_FALSE(seen.load()); // still unwinding when the timeout was reported
    EXPECT_TRUE(async_wait_for([&] { return unwound->load(); }, 5s));
}

TEST(audit_async_when_any_joined_resumes_after_every_loser_finished) {
    auto unwound = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> loserDone{false};
    EXPECT_EQ(YarnBall::syncWait(asyncJoinedRace(unwound, &loserDone)), std::size_t(0));
    EXPECT_TRUE(loserDone.load());
}

TEST(audit_async_with_timeout_joined_task_win_and_rethrow_are_unchanged) {
    EXPECT_EQ(YarnBall::syncWait(YarnBall::withTimeout(asyncInstantSeven(), 30s, YarnBall::joinLosers)), 7);
    auto thrower = []() -> Task<int> { co_return co_await YarnBall::withTimeout(asyncThrowsAfterYield(), 30s, YarnBall::joinLosers); };
    EXPECT_THROWS_AS(YarnBall::syncWait(thrower()), std::runtime_error);
}
