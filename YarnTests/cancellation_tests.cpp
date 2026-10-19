//
// Tests for cancellable awaits (Cancellation.h), the Reactor deregistration API,
// and the smaller runtime fixes that landed with them.
//

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "../Yarn/includes/AsyncSync.h"
#include "../Yarn/includes/Cancellation.h"
#include "../Yarn/includes/Coroutines.h"
#include "../Yarn/includes/JoinHandle.h"
#include "../Yarn/includes/Metrics.h"
#include "../Yarn/includes/MPMCQueue.h"
#include "../Yarn/includes/SmallObjectPool.h"
#include "../Yarn/includes/Stream.h"
#include "../Yarn/includes/Timeout.h"
#include "../Yarn/includes/Timers.h"
#include "../Yarn/includes/WhenAll.h"
#include "../Yarn/includes/WhenAny.h"
#include "../Yarn/includes/Yarn.hpp"
#include "../Wire/includes/Wire.h"

#ifndef _WIN32
    #include <fcntl.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #include "../Yarn/includes/IoAwaiters.h"
    #include "../Yarn/includes/Reactor.h"
#endif

using namespace std::chrono_literals;

namespace {

    using YarnBall::CancelledException;
    using YarnBall::Task;

    template<typename Pred>
    bool eventually(Pred &&p, std::chrono::milliseconds limit = 3000ms) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!p()) {
            if (std::chrono::steady_clock::now() >= deadline) return p();
            std::this_thread::sleep_for(2ms);
        }
        return true;
    }

    /// Flips @p flag when the owning coroutine frame is destroyed.
    struct DestroyGuard {
        std::shared_ptr<std::atomic<bool>> flag;

        explicit DestroyGuard(std::shared_ptr<std::atomic<bool>> f) : flag(std::move(f)) {}
        DestroyGuard(const DestroyGuard &) = delete;
        ~DestroyGuard() { flag->store(true); }
    };

    Task<> stopAfter(std::stop_source src, std::chrono::milliseconds d) {
        co_await YarnBall::sleepFor(d);
        src.request_stop();
    }

    /// Runs @p task in a scope stopped after @p d; reports how it ended.
    enum class Outcome { Completed, Cancelled, Failed };

    template<typename T>
    Task<Outcome> runCancelledAfter(Task<T> task, std::chrono::milliseconds d) {
        std::stop_source src;
        YarnBall::coSpawn(stopAfter(src, d));
        try {
            co_await YarnBall::withCancellation(src.get_token(), std::move(task));
        } catch (const CancelledException &) {
            co_return Outcome::Cancelled;
        } catch (...) {
            co_return Outcome::Failed;
        }
        co_return Outcome::Completed;
    }

    /// Records into @p out how a spawned, scoped task ended (so the test can race it against other work).
    template<typename T>
    Task<> runScopedInto(std::stop_token tok, Task<T> task, std::shared_ptr<std::atomic<int>> out) {
        try {
            co_await YarnBall::withCancellation(tok, std::move(task));
            out->store(1);
        } catch (const CancelledException &) {
            out->store(2);
        } catch (...) {
            out->store(3);
        }
    }

    // ---- primitives ----------------------------------------------------

    Task<int> lockAndReturn(YarnBall::AsyncMutex &m) {
        auto g = co_await m.lock();
        co_return 1;
    }

    Task<> acquireOne(YarnBall::AsyncSemaphore &s) { co_await s.acquire(); }
    Task<> awaitNotified(YarnBall::AsyncNotify &n) { co_await n.notified(); }
    Task<> awaitEvent(YarnBall::AsyncEvent &e) { co_await e.wait(); }
    Task<> arriveAt(YarnBall::AsyncBarrier &b) { co_await b.arrive(); }

    Task<> takeExclusive(YarnBall::AsyncRwLock &l) {
        auto g = co_await l.lockExclusive();
    }

    Task<> takeShared(YarnBall::AsyncRwLock &l, std::shared_ptr<std::atomic<bool>> got) {
        auto g = co_await l.lockShared();
        got->store(true);
    }

    Task<> sleepForever() { co_await YarnBall::sleepFor(10min); }

    Task<> sleepForeverGuarded(std::shared_ptr<std::atomic<bool>> destroyed) {
        DestroyGuard guard(destroyed);
        co_await YarnBall::sleepFor(10min);
    }

    Task<int> quickValue() {
        co_await YarnBall::sleepFor(5ms);
        co_return 7;
    }

    Task<int> slowGuardedValue(std::shared_ptr<std::atomic<bool>> destroyed) {
        DestroyGuard guard(destroyed);
        co_await YarnBall::sleepFor(10min);
        co_return 1;
    }

    Task<> slowGuardedVoid(std::shared_ptr<std::atomic<bool>> destroyed) {
        DestroyGuard guard(destroyed);
        co_await YarnBall::sleepFor(10min);
    }

}

TEST(cancel_async_mutex_waiter_leaves_the_queue) {
    YarnBall::AsyncMutex mu;
    auto holder = mu.tryLock();
    EXPECT_TRUE(holder.ownsLock());
    const auto out = YarnBall::syncWait(runCancelledAfter(lockAndReturn(mu), 30ms));
    EXPECT_TRUE(out == Outcome::Cancelled);
    holder.unlock();
    // Nobody inherited the mutex from the cancelled waiter.
    auto again = mu.tryLock();
    EXPECT_TRUE(again.ownsLock());
}

TEST(cancel_async_semaphore_waiter_does_not_consume_a_permit) {
    YarnBall::AsyncSemaphore sem(0);
    EXPECT_TRUE(YarnBall::syncWait(runCancelledAfter(acquireOne(sem), 30ms)) == Outcome::Cancelled);
    sem.release();
    EXPECT_EQ(sem.available(), static_cast<std::size_t>(1));
}

TEST(cancel_async_notify_waiter_is_removed) {
    YarnBall::AsyncNotify n;
    EXPECT_TRUE(YarnBall::syncWait(runCancelledAfter(awaitNotified(n), 30ms)) == Outcome::Cancelled);
    EXPECT_EQ(n.waiterCount(), static_cast<std::size_t>(0));
}

TEST(cancel_async_event_waiter_is_removed_and_event_still_works) {
    YarnBall::AsyncEvent ev;
    EXPECT_TRUE(YarnBall::syncWait(runCancelledAfter(awaitEvent(ev), 30ms)) == Outcome::Cancelled);
    ev.set();
    YarnBall::syncWait(awaitEvent(ev));
}

TEST(cancel_async_barrier_arrival_is_withdrawn) {
    YarnBall::AsyncBarrier barrier(2);
    EXPECT_TRUE(YarnBall::syncWait(runCancelledAfter(arriveAt(barrier), 30ms)) == Outcome::Cancelled);
    EXPECT_EQ(barrier.arrived(), static_cast<std::size_t>(0));
    // The barrier still trips with exactly two fresh arrivals.
    std::vector<Task<>> both;
    both.push_back(arriveAt(barrier));
    both.push_back(arriveAt(barrier));
    YarnBall::syncWait(YarnBall::whenAll(std::move(both)));
}

TEST(cancel_async_rwlock_cancelled_writer_unblocks_readers_behind_it) {
    YarnBall::AsyncRwLock lock;
    auto reader = YarnBall::syncWait(lock.lockShared()); // keeps the lock shared throughout

    std::stop_source src;
    auto writerResult = std::make_shared<std::atomic<int>>(0);
    YarnBall::coSpawn(runScopedInto(src.get_token(), takeExclusive(lock), writerResult));
    std::this_thread::sleep_for(50ms); // the writer is queued first
    auto got = std::make_shared<std::atomic<bool>>(false);
    YarnBall::coSpawn(takeShared(lock, got));
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(got->load()); // writer-preferring: the reader waits behind the writer

    src.request_stop();
    EXPECT_TRUE(eventually([&] { return got->load(); }));
    EXPECT_TRUE(eventually([&] { return writerResult->load() == 2; }));
}

TEST(cancel_async_once_runner_cancel_lets_another_caller_run) {
    YarnBall::AsyncOnce once;
    std::stop_source src;
    auto runnerResult = std::make_shared<std::atomic<int>>(0);
    auto ran = std::make_shared<std::atomic<int>>(0);

    auto slowInit = [](YarnBall::AsyncOnce &o) -> Task<> {
        co_await o.callOnce([]() -> Task<> { co_await YarnBall::sleepFor(10min); });
    };
    YarnBall::coSpawn(runScopedInto(src.get_token(), slowInit(once), runnerResult));
    std::this_thread::sleep_for(50ms);

    auto second = [](YarnBall::AsyncOnce &o, std::shared_ptr<std::atomic<int>> r) -> Task<> {
        co_await o.callOnce([r]() -> Task<> {
            r->fetch_add(1);
            co_return;
        });
    };
    auto secondDone = std::make_shared<std::atomic<bool>>(false);
    YarnBall::coSpawn([](Task<> t, std::shared_ptr<std::atomic<bool>> d) -> Task<> {
        co_await std::move(t);
        d->store(true);
    }(second(once, ran), secondDone));
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(secondDone->load());

    src.request_stop();
    EXPECT_TRUE(eventually([&] { return secondDone->load(); }));
    EXPECT_EQ(ran->load(), 1);
    EXPECT_TRUE(eventually([&] { return runnerResult->load() == 2; }));
    EXPECT_TRUE(once.isCompleted());
}

TEST(cancel_join_handle_wait_is_cancelled_but_join_still_works_later) {
    auto h = YarnBall::spawnJoinable([]() -> Task<int> {
        co_await YarnBall::sleepFor(300ms);
        co_return 5;
    }());
    EXPECT_TRUE(YarnBall::syncWait(runCancelledAfter(h.join(), 20ms)) == Outcome::Cancelled);
    EXPECT_EQ(YarnBall::syncWait(h.join()), 5);
}

TEST(cancel_sleep_wakes_promptly) {
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(YarnBall::syncWait(runCancelledAfter(sleepForever(), 30ms)) == Outcome::Cancelled);
    EXPECT_TRUE(std::chrono::steady_clock::now() - t0 < 3s);
}

TEST(cancel_scope_already_stopped_cancels_immediately) {
    std::stop_source src;
    src.request_stop();
    EXPECT_THROWS_AS(YarnBall::syncWait(YarnBall::withCancellation(src.get_token(), sleepForever())),
                     CancelledException);
}

TEST(cancel_scope_is_inherited_by_nested_tasks_and_check_cancel_sees_it) {
    std::stop_source src;
    auto nested = []() -> Task<int> {
        int polls = 0;
        while (true) {
            co_await YarnBall::checkCancel();
            ++polls;
            co_await YarnBall::sleepFor(1ms);
        }
        co_return polls;
    };
    YarnBall::coSpawn(stopAfter(src, 30ms));
    EXPECT_THROWS_AS(YarnBall::syncWait(YarnBall::withCancellation(src.get_token(), nested())),
                     CancelledException);
}

TEST(cancel_when_any_cancels_losers) {
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    std::vector<Task<>> tasks;
    tasks.push_back(slowGuardedVoid(destroyed));
    tasks.push_back([]() -> Task<> { co_await YarnBall::sleepFor(10ms); }());
    const auto idx = YarnBall::syncWait(YarnBall::whenAny(std::move(tasks)));
    EXPECT_EQ(idx, static_cast<std::size_t>(1));
    // The 10-minute sleeper is woken and its frame freed, not left parked for the whole duration.
    EXPECT_TRUE(eventually([&] { return destroyed->load(); }));
}

TEST(cancel_when_any_value_cancels_losers) {
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    std::vector<Task<int>> tasks;
    tasks.push_back(slowGuardedValue(destroyed));
    tasks.push_back(quickValue());
    const auto r = YarnBall::syncWait(YarnBall::whenAny(std::move(tasks)));
    EXPECT_EQ(r.index, static_cast<std::size_t>(1));
    EXPECT_EQ(r.value, 7);
    EXPECT_TRUE(eventually([&] { return destroyed->load(); }));
}

TEST(cancel_with_timeout_cancels_the_task_that_timed_out) {
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    EXPECT_THROWS_AS(YarnBall::syncWait(YarnBall::withTimeout(slowGuardedValue(destroyed), 30ms)),
                     YarnBall::TimeoutException);
    EXPECT_TRUE(eventually([&] { return destroyed->load(); }));
}

TEST(cancel_with_timeout_returns_promptly_and_value_survives) {
    const auto t0 = std::chrono::steady_clock::now();
    // A 10-minute timeout whose task finishes at once must not hold anything up.
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(YarnBall::syncWait(YarnBall::withTimeout(quickValue(), 10min)), 7);
    }
    EXPECT_TRUE(std::chrono::steady_clock::now() - t0 < 5s);
}

TEST(cancel_outer_scope_stops_a_nested_when_any) {
    std::stop_source src;
    YarnBall::coSpawn(stopAfter(src, 30ms));
    auto raceForever = []() -> Task<std::size_t> {
        std::vector<Task<>> tasks;
        tasks.push_back(sleepForever());
        tasks.push_back(sleepForever());
        co_return co_await YarnBall::whenAny(std::move(tasks));
    };
    EXPECT_THROWS_AS(YarnBall::syncWait(YarnBall::withCancellation(src.get_token(), raceForever())),
                     CancelledException);
}

TEST(cancel_outer_scope_stops_a_nested_when_all) {
    std::stop_source src;
    YarnBall::coSpawn(stopAfter(src, 30ms));
    auto allForever = []() -> Task<> {
        std::vector<Task<>> tasks;
        tasks.push_back(sleepForever());
        tasks.push_back(sleepForever());
        co_await YarnBall::whenAll(std::move(tasks));
    };
    EXPECT_THROWS_AS(YarnBall::syncWait(YarnBall::withCancellation(src.get_token(), allForever())),
                     CancelledException);
}

TEST(cancel_destroying_a_parked_mutex_waiter_removes_it_from_the_queue) {
    YarnBall::AsyncMutex mu;
    auto holder = mu.tryLock();
    {
        auto t = lockAndReturn(mu);
        t.getHandle().resume(); // runs until it parks on the held mutex
        EXPECT_FALSE(t.done());
    } // frame destroyed while parked
    holder.unlock();
    auto again = mu.tryLock(); // would fail if the destroyed waiter had been handed the mutex
    EXPECT_TRUE(again.ownsLock());
}

TEST(cancel_destroying_a_parked_rwlock_writer_unblocks_readers) {
    YarnBall::AsyncRwLock lock;
    auto reader = YarnBall::syncWait(lock.lockShared());
    auto got = std::make_shared<std::atomic<bool>>(false);
    {
        auto w = takeExclusive(lock);
        w.getHandle().resume();
        EXPECT_FALSE(w.done());
        YarnBall::coSpawn(takeShared(lock, got));
        std::this_thread::sleep_for(50ms);
        EXPECT_FALSE(got->load());
    } // writer destroyed while parked
    EXPECT_TRUE(eventually([&] { return got->load(); }));
}

TEST(cancel_destroying_a_parked_sleeper_forgets_its_timer) {
    {
        auto t = sleepForever();
        t.getHandle().resume();
        EXPECT_FALSE(t.done());
    }
    // The reactor no longer names the destroyed frame; the process stays healthy.
    EXPECT_EQ(YarnBall::syncWait(quickValue()), 7);
}

// ---- channels ------------------------------------------------------------

namespace {
    Task<Telegraph::SelectResult<int>> selectTwo(std::shared_ptr<Telegraph::Channel<int>> a,
                                                 std::shared_ptr<Telegraph::Channel<int>> b) {
        std::vector<std::shared_ptr<Telegraph::Channel<int>>> chans = {std::move(a), std::move(b)};
        co_return co_await Telegraph::channelSelect(std::move(chans));
    }

    Task<Telegraph::SelectResult<int>> selectTwoBounded(std::shared_ptr<Telegraph::BoundedChannel<int>> a,
                                                        std::shared_ptr<Telegraph::BoundedChannel<int>> b) {
        std::vector<std::shared_ptr<Telegraph::BoundedChannel<int>>> chans = {std::move(a), std::move(b)};
        co_return co_await Telegraph::channelSelect(std::move(chans));
    }

    Task<std::optional<int>> recvOne(std::shared_ptr<Telegraph::Channel<int>> c) { co_return co_await c->receive(); }
    Task<std::optional<int>> recvOneBounded(std::shared_ptr<Telegraph::BoundedChannel<int>> c) {
        co_return co_await c->receive();
    }
}

TEST(cancel_channel_select_loser_does_not_swallow_a_later_value) {
    auto a = std::make_shared<Telegraph::Channel<int>>();
    auto b = std::make_shared<Telegraph::Channel<int>>();

    auto sel = YarnBall::spawnJoinable(selectTwo(a, b));
    std::this_thread::sleep_for(50ms); // both receives are parked
    a->send(1);
    const auto r = YarnBall::syncWait(sel.join());
    EXPECT_EQ(r.index, static_cast<std::size_t>(0));
    EXPECT_EQ(*r.value, 1);

    // Before: the abandoned receive on b was still parked and ate this value.
    b->send(2);
    const auto v = YarnBall::syncWait(YarnBall::withTimeout(recvOne(b), 3s));
    EXPECT_TRUE(v.has_value());
    EXPECT_EQ(*v, 2);
}

TEST(cancel_channel_select_does_not_lose_values_under_races) {
    for (int round = 0; round < 200; ++round) {
        auto a = std::make_shared<Telegraph::Channel<int>>();
        auto b = std::make_shared<Telegraph::Channel<int>>();
        auto sel = YarnBall::spawnJoinable(selectTwo(a, b));
        // Both channels receive a value at (nearly) the same moment: exactly one is returned, the other must stay.
        std::thread t1([&] { a->send(1); });
        std::thread t2([&] { b->send(2); });
        t1.join();
        t2.join();
        const auto r = YarnBall::syncWait(sel.join());
        EXPECT_TRUE(r.value.has_value());
        auto other = r.index == 0 ? b : a;
        const int expected = r.index == 0 ? 2 : 1;
        const auto left = YarnBall::syncWait(YarnBall::withTimeout(recvOne(other), 3s));
        EXPECT_TRUE(left.has_value());
        EXPECT_EQ(*left, expected);
    }
}

TEST(cancel_bounded_channel_select_loser_does_not_swallow_a_later_value) {
    auto a = std::make_shared<Telegraph::BoundedChannel<int>>(4);
    auto b = std::make_shared<Telegraph::BoundedChannel<int>>(4);
    auto sel = YarnBall::spawnJoinable(selectTwoBounded(a, b));
    std::this_thread::sleep_for(50ms);
    YarnBall::syncWait(a->send(1));
    const auto r = YarnBall::syncWait(sel.join());
    EXPECT_EQ(r.index, static_cast<std::size_t>(0));
    YarnBall::syncWait(b->send(2));
    const auto v = YarnBall::syncWait(YarnBall::withTimeout(recvOneBounded(b), 3s));
    EXPECT_EQ(*v, 2);
}

TEST(cancel_channel_receive_in_stopped_scope_leaves_the_waiter_queue) {
    auto c = std::make_shared<Telegraph::Channel<int>>();
    EXPECT_TRUE(YarnBall::syncWait(runCancelledAfter(recvOne(c), 30ms)) == Outcome::Cancelled);
    c->send(9); // buffered, not handed to the cancelled receiver
    EXPECT_EQ(c->size(), static_cast<std::size_t>(1));
}

TEST(cancel_bounded_channel_blocked_send_is_cancelled) {
    auto c = std::make_shared<Telegraph::BoundedChannel<int>>(1);
    YarnBall::syncWait(c->send(1)); // full
    auto blockedSend = [](std::shared_ptr<Telegraph::BoundedChannel<int>> ch) -> Task<bool> {
        co_return co_await ch->send(2);
    };
    EXPECT_TRUE(YarnBall::syncWait(runCancelledAfter(blockedSend(c), 30ms)) == Outcome::Cancelled);
    // The cancelled send's value never entered the channel.
    EXPECT_EQ(*YarnBall::syncWait(recvOneBounded(c)), 1);
    EXPECT_EQ(c->size(), static_cast<std::size_t>(0));
}

namespace {
    Task<> awaitSignal(Telegraph::Signal<int> &sig) { (void) co_await sig.next(); }
}

TEST(cancel_signal_next_in_stopped_scope_leaves_the_pending_list) {
    Telegraph::Signal<int> sig;
    EXPECT_TRUE(YarnBall::syncWait(runCancelledAfter(awaitSignal(sig), 30ms)) == Outcome::Cancelled);
    sig.emit(1); // would resume the cancelled (finished) frame if it were still pending
    {
        auto t = awaitSignal(sig);
        t.getHandle().resume();
        EXPECT_FALSE(t.done());
    } // destroyed while pending
    sig.emit(2);
    std::this_thread::sleep_for(20ms);
}

// ---- reactor ---------------------------------------------------------------

#ifndef _WIN32
namespace {
    struct Pipe {
        int fds[2]{-1, -1};

        Pipe() {
            if (::pipe(fds) != 0) throw std::runtime_error("pipe failed");
            ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
            ::fcntl(fds[1], F_SETFL, O_NONBLOCK);
        }
        ~Pipe() {
            if (fds[0] >= 0) ::close(fds[0]);
            if (fds[1] >= 0) ::close(fds[1]);
        }
    };

    Task<> waitReadableThenFlag(int fd, std::shared_ptr<std::atomic<int>> done) {
        co_await YarnBall::io::waitReadable(fd);
        done->fetch_add(1);
    }
}

TEST(reactor_cancel_fd_resumes_parked_waiters) {
    Pipe p;
    auto done = std::make_shared<std::atomic<int>>(0);
    YarnBall::coSpawn(waitReadableThenFlag(p.fds[0], done));
    YarnBall::coSpawn(waitReadableThenFlag(p.fds[0], done));
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(done->load(), 0);
    EXPECT_TRUE(YarnBall::Reactor::instance()->cancelFd(p.fds[0]) >= 2);
    EXPECT_TRUE(eventually([&] { return done->load() == 2; }));
}

TEST(reactor_cancel_fd_without_waiters_is_a_noop) {
    Pipe p;
    EXPECT_EQ(YarnBall::Reactor::instance()->cancelFd(p.fds[0]), static_cast<std::size_t>(0));
}

TEST(reactor_two_waiters_on_the_same_fd_and_direction_both_wake) {
    Pipe p;
    auto done = std::make_shared<std::atomic<int>>(0);
    YarnBall::coSpawn(waitReadableThenFlag(p.fds[0], done));
    YarnBall::coSpawn(waitReadableThenFlag(p.fds[0], done));
    YarnBall::coSpawn(waitReadableThenFlag(p.fds[0], done));
    std::this_thread::sleep_for(50ms);
    const char b = 'x';
    EXPECT_EQ(::write(p.fds[1], &b, 1), static_cast<ssize_t>(1));
    EXPECT_TRUE(eventually([&] { return done->load() == 3; }));
}

TEST(reactor_wait_in_stopped_scope_is_cancelled_and_deregistered) {
    Pipe p;
    auto waitIt = [](int fd) -> Task<> { co_await YarnBall::io::waitReadable(fd); };
    EXPECT_TRUE(YarnBall::syncWait(runCancelledAfter(waitIt(p.fds[0]), 30ms)) == Outcome::Cancelled);
    // Nothing is left registered for the fd.
    EXPECT_EQ(YarnBall::Reactor::instance()->cancelFd(p.fds[0]), static_cast<std::size_t>(0));
    // And a fresh wait on the same fd still works.
    const char b = 'y';
    EXPECT_EQ(::write(p.fds[1], &b, 1), static_cast<ssize_t>(1));
    YarnBall::syncWait(YarnBall::withTimeout(waitIt(p.fds[0]), 3s));
}

TEST(reactor_cancel_wait_forgets_a_destroyed_waiter) {
    Pipe p;
    {
        auto t = [](int fd) -> Task<> { co_await YarnBall::io::waitReadable(fd); }(p.fds[0]);
        t.getHandle().resume();
        EXPECT_FALSE(t.done());
    } // destroyed while registered
    const char b = 'z';
    EXPECT_EQ(::write(p.fds[1], &b, 1), static_cast<ssize_t>(1)); // would resume the dead frame if still registered
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(YarnBall::Reactor::instance()->cancelFd(p.fds[0]), static_cast<std::size_t>(0));
}

TEST(reactor_async_write_to_closed_peer_reports_error_not_sigpipe) {
    int sv[2];
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    ::fcntl(sv[0], F_SETFL, O_NONBLOCK);
    ::close(sv[1]);
    const char data[] = "hello";
    const ssize_t n = YarnBall::syncWait(YarnBall::io::asyncWrite(sv[0], data, sizeof(data)));
    EXPECT_EQ(n, static_cast<ssize_t>(-1));
    ::close(sv[0]);
}
#endif

// ---- runtime fixes ---------------------------------------------------------

TEST(detached_task_exception_reaches_the_unhandled_handler) {
    auto seen = std::make_shared<std::atomic<int>>(0);
    auto text = std::make_shared<std::string>();
    auto mu = std::make_shared<std::mutex>();
    YarnBall::setUnhandledExceptionHandler([seen, text, mu](std::exception_ptr e) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception &ex) {
            std::lock_guard<std::mutex> lk(*mu);
            *text = ex.what();
        }
        seen->fetch_add(1);
    });
    YarnBall::coSpawn([]() -> Task<> {
        co_await YarnBall::sleepFor(1ms);
        throw std::runtime_error("detached boom");
    }());
    // A cancelled task ending is not an error and must not be reported.
    YarnBall::coSpawn([]() -> Task<> {
        throw CancelledException{};
        co_return;
    }());
    EXPECT_TRUE(eventually([&] { return seen->load() >= 1; }));
    std::this_thread::sleep_for(50ms);
    YarnBall::setUnhandledExceptionHandler({});
    EXPECT_EQ(seen->load(), 1);
    std::lock_guard<std::mutex> lk(*mu);
    EXPECT_EQ(*text, std::string("detached boom"));
}

namespace {
    YarnBall::Stream<std::string> yieldConstLvalue() {
        const std::string s = "const-lvalue";
        co_yield s;
        co_yield s;
    }

    Task<std::string> drainConstStream() {
        std::string out;
        auto st = yieldConstLvalue();
        while (true) {
            auto v = co_await st.next();
            if (!v) break;
            out += *v;
            out += ';';
        }
        co_return out;
    }
}

TEST(stream_co_yield_const_lvalue_copies) {
    EXPECT_EQ(YarnBall::syncWait(drainConstStream()), std::string("const-lvalue;const-lvalue;"));
}

TEST(metrics_help_text_is_escaped) {
    auto &reg = YarnBall::metrics::Registry::instance();
    reg.counter("escape_test_counter", "line one\nline two \\ backslash").inc();
    const std::string body = reg.scrape();
    EXPECT_TRUE(body.find("# HELP escape_test_counter line one\\nline two \\\\ backslash\n") != std::string::npos);
}

TEST(metrics_histogram_bucket_mismatch_and_bad_bounds_are_rejected) {
    auto &reg = YarnBall::metrics::Registry::instance();
    auto &h = reg.histogram("mismatch_test_hist", {1.0, 2.0});
    EXPECT_TRUE(&reg.histogram("mismatch_test_hist", {1.0, 2.0}) == &h);
    EXPECT_THROWS_AS(reg.histogram("mismatch_test_hist", {1.0, 5.0}), std::logic_error);
    EXPECT_THROWS_AS(YarnBall::metrics::Histogram(std::vector<double>{2.0, 1.0}), std::invalid_argument);
    EXPECT_THROWS_AS(YarnBall::metrics::Histogram(std::vector<double>{1.0, 1.0}), std::invalid_argument);
}

namespace {
    /// Copy construction can be made to throw on demand.
    struct CopyThrows {
        static inline std::atomic<bool> armed{false};
        int v = 0;
        CopyThrows() = default;
        explicit CopyThrows(int x) : v(x) {}
        CopyThrows(const CopyThrows &o) : v(o.v) {
            if (armed.load()) throw std::runtime_error("copy failed");
        }
        CopyThrows(CopyThrows &&) noexcept = default;
        CopyThrows &operator=(CopyThrows &&) noexcept = default;
    };
}

TEST(mpmc_throwing_copy_propagates_and_leaves_the_queue_usable) {
    YarnBall::MPMCQueue<CopyThrows> q(4);
    const CopyThrows item(5);
    CopyThrows::armed = true;
    EXPECT_THROWS_AS(q.enqueue(item), std::runtime_error);
    CopyThrows::armed = false;
    EXPECT_TRUE(q.empty());
    EXPECT_TRUE(q.enqueue(item));
    EXPECT_TRUE(q.enqueue(item));
    EXPECT_EQ(q.pop_front()->v, 5);
    EXPECT_EQ(q.pop_front()->v, 5);
    EXPECT_FALSE(q.pop_front().has_value());
}

TEST(small_object_pool_blocks_cached_by_an_exited_thread_are_reusable) {
    using Pool = YarnBall::detail::SmallObjectPool<48>;
    std::vector<void *> blocks;
    std::thread t([&] {
        for (int i = 0; i < 8; ++i) blocks.push_back(Pool::allocate());
        for (void *p : blocks) Pool::deallocate(p); // cached thread-locally ...
    });                                              // ... and handed back to the shared pool at thread exit
    t.join();
    std::vector<void *> again;
    for (int i = 0; i < 8; ++i) again.push_back(Pool::allocate());
    int reused = 0;
    for (void *p : again) {
        for (void *q : blocks) {
            if (p == q) ++reused;
        }
    }
    EXPECT_EQ(reused, 8);
    for (void *p : again) Pool::deallocate(p);
}

TEST(yarn_idle_temp_workers_retire) {
    auto *pool = YarnBall::Yarn::instance();
    const auto base = pool->stats();
    // Block enough workers for long enough that the backlog makes the pool grow.
    auto started = std::make_shared<std::atomic<int>>(0);
    for (int i = 0; i < 400; ++i) {
        pool->run([started] {
            started->fetch_add(1);
            std::this_thread::sleep_for(30ms);
        });
    }
    EXPECT_TRUE(eventually([&] { return started->load() == 400; }, 20000ms));
    // Grown (best effort: depends on core count), then back to the permanent set once idle.
    EXPECT_TRUE(eventually([&] { return pool->stats().aliveWorkers <= base.permanentWorkers; }, 10000ms));
}

// ---- Task::requestCancel wakes parked awaits --------------------------------

namespace {

    /// Awaits *t (kept alive by the test, which calls t->requestCancel()) and records how it ended: 1 done, 2 cancelled.
    template<typename T>
    Task<> runHeld(Task<T> *t, std::shared_ptr<std::atomic<int>> out) {
        try {
            co_await std::move(*t);
            out->store(1);
        } catch (const CancelledException &) {
            out->store(2);
        } catch (...) {
            out->store(3);
        }
    }

    /// Starts make(), lets it park, requests cancellation on it and expects it to end cancelled.
    template<typename Make>
    void expectRequestCancelWakes(Make make) {
        auto out = std::make_shared<std::atomic<int>>(0);
        auto t = make();
        YarnBall::coSpawn(runHeld(&t, out));
        std::this_thread::sleep_for(60ms);
        EXPECT_EQ(out->load(), 0);
        t.requestCancel();
        EXPECT_TRUE(eventually([&] { return out->load() != 0; }));
        EXPECT_EQ(out->load(), 2);
    }

    Task<> recvDiscard(std::shared_ptr<Telegraph::Channel<int>> c) { (void) co_await c->receive(); }
    Task<> recvDiscardBounded(std::shared_ptr<Telegraph::BoundedChannel<int>> c) { (void) co_await c->receive(); }
    Task<> sendBounded(std::shared_ptr<Telegraph::BoundedChannel<int>> c, int v) { (void) co_await c->send(v); }
    Task<> joinVoid(YarnBall::JoinHandle<void> *h) { co_await h->join(); }
    Task<> sleepShort() { co_await YarnBall::sleepFor(400ms); }

    Task<> middleOf(YarnBall::AsyncNotify &n) { co_await awaitNotified(n); }
    Task<> topOf(YarnBall::AsyncNotify &n) { co_await middleOf(n); }

    Task<> raceNotifyAndSleep(YarnBall::AsyncNotify &n) {
        std::vector<Task<>> branches;
        branches.push_back(awaitNotified(n));
        branches.push_back(sleepForever());
        (void) co_await YarnBall::whenAny(std::move(branches));
    }

    Task<> allTwoNotifies(YarnBall::AsyncNotify &a, YarnBall::AsyncNotify &b) {
        std::vector<Task<>> branches;
        branches.push_back(awaitNotified(a));
        branches.push_back(awaitNotified(b));
        co_await YarnBall::whenAll(std::move(branches));
    }

    Task<> scopedNotified(YarnBall::AsyncNotify &n) {
        std::stop_source never;
        co_await YarnBall::withCancellation(never.get_token(), awaitNotified(n));
    }

    Task<> awaitHeld(Task<> *t) { co_await std::move(*t); }
}

TEST(request_cancel_wakes_async_mutex_waiter) {
    YarnBall::AsyncMutex mu;
    auto holder = mu.tryLock();
    expectRequestCancelWakes([&] { return [](YarnBall::AsyncMutex &m) -> Task<> { auto g = co_await m.lock(); }(mu); });
    holder.unlock();
    EXPECT_TRUE(mu.tryLock().ownsLock());
}

TEST(request_cancel_wakes_semaphore_event_notify_barrier_waiters) {
    YarnBall::AsyncSemaphore sem(0);
    expectRequestCancelWakes([&] { return acquireOne(sem); });
    sem.release();
    EXPECT_EQ(sem.available(), static_cast<std::size_t>(1));

    YarnBall::AsyncEvent ev;
    expectRequestCancelWakes([&] { return awaitEvent(ev); });

    YarnBall::AsyncNotify n;
    expectRequestCancelWakes([&] { return awaitNotified(n); });
    EXPECT_EQ(n.waiterCount(), static_cast<std::size_t>(0));

    YarnBall::AsyncBarrier barrier(2);
    expectRequestCancelWakes([&] { return arriveAt(barrier); });
    EXPECT_EQ(barrier.arrived(), static_cast<std::size_t>(0));
}

TEST(request_cancel_wakes_rwlock_writer) {
    YarnBall::AsyncRwLock lock;
    auto reader = YarnBall::syncWait(lock.lockShared());
    expectRequestCancelWakes([&] { return takeExclusive(lock); });
    reader.unlock();
    auto w = YarnBall::syncWait(lock.lockExclusive()); // the cancelled writer left no trace
}

TEST(request_cancel_wakes_sleep_and_join) {
    expectRequestCancelWakes([] { return sleepForever(); });
    auto h = YarnBall::spawnJoinable(sleepShort());
    expectRequestCancelWakes([&] { return joinVoid(&h); });
    // The joined task was not cancelled: it still completes and can be joined.
    YarnBall::syncWait(h.join());
}

TEST(request_cancel_wakes_channel_and_signal_waiters) {
    auto c = std::make_shared<Telegraph::Channel<int>>();
    expectRequestCancelWakes([&] { return recvDiscard(c); });
    EXPECT_TRUE(c->send(1)); // no parked receiver left to swallow it
    EXPECT_EQ(c->size(), static_cast<std::size_t>(1));

    auto b = std::make_shared<Telegraph::BoundedChannel<int>>(1);
    expectRequestCancelWakes([&] { return recvDiscardBounded(b); });
    YarnBall::syncWait(b->send(1));
    expectRequestCancelWakes([&] { return sendBounded(b, 2); });
    EXPECT_EQ(b->size(), static_cast<std::size_t>(1));

    Telegraph::Signal<int> sig;
    expectRequestCancelWakes([&] { return awaitSignal(sig); });
    sig.emit(3);
}

#ifndef _WIN32
TEST(request_cancel_wakes_fd_wait_and_leaves_nothing_registered) {
    Pipe p;
    expectRequestCancelWakes([&] { return [](int fd) -> Task<> { co_await YarnBall::io::waitReadable(fd); }(p.fds[0]); });
    EXPECT_EQ(YarnBall::Reactor::instance()->cancelFd(p.fds[0]), static_cast<std::size_t>(0));
}
#endif

TEST(request_cancel_on_the_root_wakes_a_leaf_deep_in_the_chain) {
    YarnBall::AsyncNotify n;
    expectRequestCancelWakes([&] { return topOf(n); });
    EXPECT_EQ(n.waiterCount(), static_cast<std::size_t>(0));
}

TEST(request_cancel_on_an_awaited_child_wakes_it_without_cancelling_its_parent) {
    YarnBall::AsyncNotify n;
    Task<> child = awaitNotified(n);
    auto out = std::make_shared<std::atomic<int>>(0);
    auto parent = [](Task<> *c, YarnBall::AsyncNotify &second, std::shared_ptr<std::atomic<int>> o) -> Task<> {
        try {
            co_await std::move(*c);
        } catch (const CancelledException &) {
            o->store(1); // the child was cancelled ...
        }
        co_await awaitNotified(second); // ... the parent itself was not
        o->store(2);
    }(&child, n, out);
    auto parentOut = std::make_shared<std::atomic<int>>(0);
    YarnBall::coSpawn(runHeld(&parent, parentOut));
    EXPECT_TRUE(eventually([&] { return n.waiterCount() == 1; }));
    child.requestCancel();
    EXPECT_TRUE(eventually([&] { return out->load() == 1 && n.waiterCount() == 1; }));
    n.notifyAll();
    EXPECT_TRUE(eventually([&] { return out->load() == 2 && parentOut->load() == 1; }));
}

TEST(request_cancel_leaves_unrelated_chains_alone) {
    YarnBall::AsyncNotify a;
    YarnBall::AsyncNotify b;
    auto outA = std::make_shared<std::atomic<int>>(0);
    auto outB = std::make_shared<std::atomic<int>>(0);
    Task<> ta = awaitNotified(a);
    Task<> tb = awaitNotified(b);
    YarnBall::coSpawn(runHeld(&ta, outA));
    YarnBall::coSpawn(runHeld(&tb, outB));
    EXPECT_TRUE(eventually([&] { return a.waiterCount() == 1 && b.waiterCount() == 1; }));
    ta.requestCancel();
    EXPECT_TRUE(eventually([&] { return outA->load() == 2; }));
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(outB->load(), 0);
    b.notifyOne();
    EXPECT_TRUE(eventually([&] { return outB->load() == 1; }));
}

TEST(request_cancel_before_the_task_parks_is_not_lost) {
    YarnBall::AsyncNotify n;
    Task<> t = awaitNotified(n);
    t.requestCancel();
    auto out = std::make_shared<std::atomic<int>>(0);
    YarnBall::coSpawn(runHeld(&t, out));
    EXPECT_TRUE(eventually([&] { return out->load() == 2; }));
    EXPECT_EQ(n.waiterCount(), static_cast<std::size_t>(0));
}

TEST(request_cancel_reaches_through_when_any_when_all_and_with_cancellation) {
    {
        YarnBall::AsyncNotify n;
        expectRequestCancelWakes([&] { return raceNotifyAndSleep(n); });
        EXPECT_EQ(n.waiterCount(), static_cast<std::size_t>(0));
    }
    {
        YarnBall::AsyncNotify a;
        YarnBall::AsyncNotify b;
        expectRequestCancelWakes([&] { return allTwoNotifies(a, b); });
        EXPECT_TRUE(eventually([&] { return a.waiterCount() == 0 && b.waiterCount() == 0; }));
    }
    {
        YarnBall::AsyncNotify n;
        expectRequestCancelWakes([&] { return scopedNotified(n); });
        EXPECT_EQ(n.waiterCount(), static_cast<std::size_t>(0));
    }
}

TEST(request_cancel_races_with_a_wake_and_ends_exactly_once) {
    for (int i = 0; i < 300; ++i) {
        YarnBall::AsyncNotify n;
        Task<> t = awaitNotified(n);
        auto out = std::make_shared<std::atomic<int>>(0);
        YarnBall::coSpawn(runHeld(&t, out));
        while (n.waiterCount() == 0) std::this_thread::yield();
        std::thread a([&] { n.notifyOne(); });
        std::thread b([&] { t.requestCancel(); });
        a.join();
        b.join();
        EXPECT_TRUE(eventually([&] { return out->load() != 0; }));
        EXPECT_TRUE(out->load() == 1 || out->load() == 2);
        EXPECT_EQ(n.waiterCount(), static_cast<std::size_t>(0));
    }
}

// ---- wake vs cancel vs destroy: exactly one wins ------------------------------

namespace {

    /// Parks like a primitive does: hands out a WaitProxy handle and lets the test play waker.
    struct ManualPark {
        std::coroutine_handle<> *published;
        const std::atomic<bool> *popped;
        bool *dropped;
        YarnBall::detail::WaitProxy proxy;

        ManualPark(std::coroutine_handle<> *pub, const std::atomic<bool> *pop, bool *drop)
            : published(pub), popped(pop), dropped(drop) {}

        ~ManualPark() { *dropped = proxy.retire(!popped->load()); }

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) { *published = proxy.publish(h); }
        void await_resume() const noexcept {}
    };

    Task<> parkManually(std::coroutine_handle<> *pub, const std::atomic<bool> *pop, bool *drop,
                        std::shared_ptr<std::atomic<bool>> ran) {
        co_await ManualPark{pub, pop, drop};
        ran->store(true);
    }
}

TEST(wait_proxy_wake_then_destroy_runs_the_waiter_once) {
    std::coroutine_handle<> proxy;
    std::atomic<bool> popped{false};
    bool dropped = true;
    auto ran = std::make_shared<std::atomic<bool>>(false);
    {
        Task<> t = parkManually(&proxy, &popped, &dropped, ran);
        t.getHandle().resume();
        EXPECT_TRUE(static_cast<bool>(proxy));
        popped = true;
        proxy.resume(); // the wake
        EXPECT_TRUE(ran->load());
        EXPECT_TRUE(t.done());
    }
    EXPECT_FALSE(dropped);
}

TEST(wait_proxy_destroy_beats_an_in_flight_wake) {
    std::coroutine_handle<> proxy;
    std::atomic<bool> popped{false};
    bool dropped = false;
    auto ran = std::make_shared<std::atomic<bool>>(false);
    {
        Task<> t = parkManually(&proxy, &popped, &dropped, ran);
        t.getHandle().resume();
        popped = true; // the waker took the proxy but has not resumed it yet
    }                  // frame destroyed in that window
    EXPECT_TRUE(dropped);
    proxy.resume(); // the late wake lands on the proxy, which must not touch the dead frame
    EXPECT_FALSE(ran->load());
}

TEST(wait_proxy_destroy_while_still_queued_frees_the_proxy) {
    std::coroutine_handle<> proxy;
    std::atomic<bool> popped{false};
    bool dropped = true;
    auto ran = std::make_shared<std::atomic<bool>>(false);
    {
        Task<> t = parkManually(&proxy, &popped, &dropped, ran);
        t.getHandle().resume();
    } // still "queued": the awaiter reclaims the proxy; nobody may resume it
    EXPECT_FALSE(dropped);
    EXPECT_FALSE(ran->load());
}

namespace {

    /// Occupies every Yarn worker, so work posted to the pool stays queued until the plug is released.
    class PoolPlug {
    public:
        PoolPlug() {
            auto *pool = YarnBall::Yarn::instance();
            // A blocker per worker (plus a surplus that any temp worker would also swallow). Tasks posted afterwards sit
            // in the workers' queues: the pool only grows when its shared injection queue backs up, which stays far off.
            this->count = pool->stats().maxWorkers + 16;
            for (int i = 0; i < this->count; ++i) {
                pool->run([this] {
                    this->started.fetch_add(1);
                    while (!this->release.load()) std::this_thread::sleep_for(1ms);
                    this->finished.fetch_add(1);
                });
            }
            this->ok = eventually([&] { return this->started.load() >= pool->stats().aliveWorkers; }, 10000ms);
            std::this_thread::sleep_for(30ms); // let a stray retiring/spawning worker settle
            this->ok = this->ok && this->started.load() >= pool->stats().aliveWorkers;
        }

        PoolPlug(const PoolPlug &) = delete;
        PoolPlug &operator=(const PoolPlug &) = delete;

        ~PoolPlug() {
            this->unplug();
            eventually([&] { return this->finished.load() == this->count; }, 30000ms);
        }

        void unplug() { this->release.store(true); }

        bool ok = false;

    private:
        std::atomic<bool> release{false};
        std::atomic<int> started{0};
        std::atomic<int> finished{0};
        int count = 0;
    };

    Task<> lockThenFlag(YarnBall::AsyncMutex &m, std::shared_ptr<std::atomic<bool>> ran) {
        auto g = co_await m.lock();
        ran->store(true);
    }

    Task<> acquireThenFlag(YarnBall::AsyncSemaphore &s, std::shared_ptr<std::atomic<bool>> ran) {
        co_await s.acquire();
        ran->store(true);
    }

    Task<> writeThenFlag(YarnBall::AsyncRwLock &l, std::shared_ptr<std::atomic<bool>> ran) {
        auto g = co_await l.lockExclusive();
        ran->store(true);
    }

    Task<> notifiedThenFlag(YarnBall::AsyncNotify &n, std::shared_ptr<std::atomic<bool>> ran) {
        co_await n.notified();
        ran->store(true);
    }

    Task<> recvThenFlag(std::shared_ptr<Telegraph::Channel<int>> c, std::shared_ptr<std::atomic<bool>> ran) {
        (void) co_await c->receive();
        ran->store(true);
    }

    Task<> sleepThenFlag(std::shared_ptr<std::atomic<bool>> ran) {
        co_await YarnBall::sleepFor(20ms);
        ran->store(true);
    }

    Task<> scopedLockThenFlag(std::stop_token tok, YarnBall::AsyncMutex &m, std::shared_ptr<std::atomic<bool>> ran) {
        co_await YarnBall::withCancellation(tok, lockThenFlag(m, ran));
    }

    /// Starts @p t inline on this thread (it runs until it parks).
    void startInline(Task<> &t) { t.getHandle().resume(); }
}

#define SKIP_IF_NO_PLUG(plug)                                                             \
    if (!(plug).ok) {                                                                     \
        std::cout << "       (skipped: could not occupy every worker)\n";                  \
        return;                                                                           \
    }

TEST(destroying_a_frame_whose_mutex_grant_is_in_flight_gives_the_mutex_back) {
    YarnBall::AsyncMutex mu;
    auto holder = mu.tryLock();
    auto ran = std::make_shared<std::atomic<bool>>(false);
    {
        PoolPlug plug;
        SKIP_IF_NO_PLUG(plug);
        Task<> t = lockThenFlag(mu, ran);
        startInline(t);          // parked on the mutex
        holder.unlock();         // grants it; the resume is queued behind the plug
        EXPECT_FALSE(t.done());
        t = Task<>{};            // frame destroyed before the queued resume runs
    }
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(ran->load());
    EXPECT_TRUE(mu.tryLock().ownsLock()); // the handed-over ownership was not leaked
}

TEST(destroying_a_frame_whose_semaphore_permit_is_in_flight_gives_the_permit_back) {
    YarnBall::AsyncSemaphore sem(0);
    auto ran = std::make_shared<std::atomic<bool>>(false);
    {
        PoolPlug plug;
        SKIP_IF_NO_PLUG(plug);
        Task<> t = acquireThenFlag(sem, ran);
        startInline(t);
        sem.release();
        t = Task<>{};
    }
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(ran->load());
    EXPECT_EQ(sem.available(), static_cast<std::size_t>(1));
}

TEST(destroying_a_frame_whose_rwlock_grant_is_in_flight_gives_the_lock_back) {
    YarnBall::AsyncRwLock lock;
    auto reader = YarnBall::syncWait(lock.lockShared());
    auto ran = std::make_shared<std::atomic<bool>>(false);
    {
        PoolPlug plug;
        SKIP_IF_NO_PLUG(plug);
        Task<> t = writeThenFlag(lock, ran);
        startInline(t);
        reader.unlock(); // promotes the writer
        t = Task<>{};
    }
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(ran->load());
    auto w = YarnBall::syncWait(lock.lockExclusive()); // would block forever if the grant had leaked
}

TEST(destroying_a_frame_whose_notify_wake_is_in_flight_resumes_nothing) {
    YarnBall::AsyncNotify n;
    auto ran = std::make_shared<std::atomic<bool>>(false);
    {
        PoolPlug plug;
        SKIP_IF_NO_PLUG(plug);
        Task<> t = notifiedThenFlag(n, ran);
        startInline(t);
        n.notifyOne();
        t = Task<>{};
    }
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(ran->load());
    EXPECT_EQ(n.waiterCount(), static_cast<std::size_t>(0));
}

TEST(destroying_a_frame_whose_cancel_wake_is_in_flight_does_not_release_anything) {
    YarnBall::AsyncMutex mu;
    auto holder = mu.tryLock();
    auto ran = std::make_shared<std::atomic<bool>>(false);
    std::stop_source src;
    {
        PoolPlug plug;
        SKIP_IF_NO_PLUG(plug);
        Task<> t = scopedLockThenFlag(src.get_token(), mu, ran);
        startInline(t);
        src.request_stop(); // withdraws the waiter; its resume is queued behind the plug
        t = Task<>{};
    }
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(ran->load());
    EXPECT_FALSE(mu.tryLock().ownsLock()); // still held by `holder`: a cancel hands nothing over
    holder.unlock();
    EXPECT_TRUE(mu.tryLock().ownsLock());
}

TEST(destroying_a_frame_whose_channel_wake_is_in_flight_resumes_nothing) {
    auto c = std::make_shared<Telegraph::Channel<int>>();
    auto ran = std::make_shared<std::atomic<bool>>(false);
    {
        PoolPlug plug;
        SKIP_IF_NO_PLUG(plug);
        Task<> t = recvThenFlag(c, ran);
        startInline(t);
        EXPECT_TRUE(c->send(1));
        t = Task<>{};
    }
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(ran->load());
    // The value that was handed to the destroyed receiver went back to the head of the channel.
    EXPECT_TRUE(c->send(2));
    EXPECT_EQ(*YarnBall::syncWait(recvOne(c)), 1);
    EXPECT_EQ(*YarnBall::syncWait(recvOne(c)), 2);
}

namespace {
    Task<> recvBoundedThenFlag(std::shared_ptr<Telegraph::BoundedChannel<int>> c, std::shared_ptr<std::atomic<bool>> ran) {
        (void) co_await c->receive();
        ran->store(true);
    }
}

TEST(destroying_a_frame_whose_bounded_channel_wake_is_in_flight_returns_the_value) {
    auto c = std::make_shared<Telegraph::BoundedChannel<int>>(2);
    auto ran = std::make_shared<std::atomic<bool>>(false);
    {
        PoolPlug plug;
        SKIP_IF_NO_PLUG(plug);
        Task<> t = recvBoundedThenFlag(c, ran);
        startInline(t);
        YarnBall::syncWait(c->send(1)); // handed straight to the parked receiver
        t = Task<>{};
    }
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(ran->load());
    YarnBall::syncWait(c->send(2));
    EXPECT_EQ(*YarnBall::syncWait(recvOneBounded(c)), 1);
    EXPECT_EQ(*YarnBall::syncWait(recvOneBounded(c)), 2);
}

TEST(destroying_a_frame_whose_timer_has_fired_resumes_nothing) {
    auto ran = std::make_shared<std::atomic<bool>>(false);
    {
        PoolPlug plug;
        SKIP_IF_NO_PLUG(plug);
        Task<> t = sleepThenFlag(ran);
        startInline(t);
        std::this_thread::sleep_for(120ms); // the Reactor fired the timer and queued the resume behind the plug
        t = Task<>{};
    }
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(ran->load());
}

#ifndef _WIN32
TEST(destroying_a_frame_whose_fd_is_ready_resumes_nothing) {
    Pipe p;
    auto ran = std::make_shared<std::atomic<bool>>(false);
    {
        PoolPlug plug;
        SKIP_IF_NO_PLUG(plug);
        Task<> t = [](int fd, std::shared_ptr<std::atomic<bool>> r) -> Task<> {
            co_await YarnBall::io::waitReadable(fd);
            r->store(true);
        }(p.fds[0], ran);
        startInline(t);
        const char b = 'x';
        EXPECT_EQ(::write(p.fds[1], &b, 1), static_cast<ssize_t>(1));
        std::this_thread::sleep_for(120ms); // readiness claimed the waiter; its resume is queued behind the plug
        t = Task<>{};
    }
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(ran->load());
}
#endif

namespace {
    Task<> lockAndRelease(YarnBall::AsyncMutex &m) { auto g = co_await m.lock(); }

    Task<> runCounted(std::stop_token tok, Task<> task, std::shared_ptr<std::atomic<int>> completed,
                      std::shared_ptr<std::atomic<int>> cancelled) {
        try {
            co_await YarnBall::withCancellation(tok, std::move(task));
            completed->fetch_add(1);
        } catch (const CancelledException &) {
            cancelled->fetch_add(1);
        }
    }
}

TEST(stress_semaphore_grants_racing_cancels_never_lose_or_duplicate_a_permit) {
    constexpr int kWaiters = 8;
    constexpr int kPermits = 4;
    for (int iter = 0; iter < 200; ++iter) {
        YarnBall::AsyncSemaphore sem(0);
        std::stop_source srcs[kWaiters];
        auto completed = std::make_shared<std::atomic<int>>(0);
        auto cancelled = std::make_shared<std::atomic<int>>(0);
        for (int i = 0; i < kWaiters; ++i) {
            YarnBall::coSpawn(runCounted(srcs[i].get_token(), acquireOne(sem), completed, cancelled));
        }
        std::thread releaser([&] {
            for (int i = 0; i < kPermits; ++i) {
                sem.release();
                if (i % 2) std::this_thread::yield();
            }
        });
        std::thread stopper([&] {
            for (int i = 0; i < kWaiters; ++i) {
                srcs[i].request_stop();
                if (i % 3 == 0) std::this_thread::yield();
            }
        });
        releaser.join();
        stopper.join();
        EXPECT_TRUE(eventually([&] { return completed->load() + cancelled->load() == kWaiters; }));
        EXPECT_EQ(completed->load() + static_cast<int>(sem.available()), kPermits);
    }
}

TEST(stress_mutex_handoffs_racing_cancels_always_leave_the_mutex_free) {
    constexpr int kWaiters = 8;
    for (int iter = 0; iter < 200; ++iter) {
        YarnBall::AsyncMutex mu;
        auto holder = std::make_shared<YarnBall::AsyncMutexGuard>(mu.tryLock());
        std::stop_source srcs[kWaiters];
        auto completed = std::make_shared<std::atomic<int>>(0);
        auto cancelled = std::make_shared<std::atomic<int>>(0);
        for (int i = 0; i < kWaiters; ++i) {
            YarnBall::coSpawn(runCounted(srcs[i].get_token(), lockAndRelease(mu), completed, cancelled));
        }
        std::thread releaser([&] { holder->unlock(); });
        std::thread stopper([&] {
            for (int i = 0; i < kWaiters; ++i) {
                srcs[i].request_stop();
                if (i % 2 == 0) std::this_thread::yield();
            }
        });
        releaser.join();
        stopper.join();
        EXPECT_TRUE(eventually([&] { return completed->load() + cancelled->load() == kWaiters; }));
        EXPECT_TRUE(mu.tryLock().ownsLock());
    }
}

TEST(stress_timer_expiry_racing_cancel_ends_exactly_once) {
    for (int iter = 0; iter < 300; ++iter) {
        std::stop_source src;
        auto completed = std::make_shared<std::atomic<int>>(0);
        auto cancelled = std::make_shared<std::atomic<int>>(0);
        YarnBall::coSpawn(runCounted(src.get_token(), [](int ms) -> Task<> {
            co_await YarnBall::sleepFor(std::chrono::milliseconds(ms));
        }(1 + iter % 3), completed, cancelled));
        std::this_thread::sleep_for(std::chrono::microseconds(500 + (iter % 7) * 250));
        src.request_stop();
        EXPECT_TRUE(eventually([&] { return completed->load() + cancelled->load() == 1; }));
        std::this_thread::sleep_for(2ms);
        EXPECT_EQ(completed->load() + cancelled->load(), 1);
    }
}

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
TEST(reactor_kqueue_registration_on_a_reused_fd_number_flushes_stale_waiters) {
    int a[2];
    int b[2];
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, a), 0);
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, b), 0);
    const int num = a[0];
    auto stale = std::make_shared<std::atomic<int>>(0);
    auto fresh = std::make_shared<std::atomic<int>>(0);
    YarnBall::coSpawn(waitReadableThenFlag(num, stale));
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(stale->load(), 0);

    // Closed behind the Reactor's back (no cancelFd), then the number is handed out again.
    ::close(num);
    EXPECT_EQ(::dup2(b[0], num), num);
    ::fcntl(num, F_SETFL, O_NONBLOCK);

    YarnBall::coSpawn(waitReadableThenFlag(num, fresh));
    EXPECT_TRUE(eventually([&] { return stale->load() == 1; })); // flushed by the new registration
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(fresh->load(), 0); // the new waiter is still waiting for its own fd

    const char c = 'k';
    EXPECT_EQ(::write(b[1], &c, 1), static_cast<ssize_t>(1));
    EXPECT_TRUE(eventually([&] { return fresh->load() == 1; }));
    ::close(num);
    ::close(a[1]);
    ::close(b[0]);
    ::close(b[1]);
}
#endif
