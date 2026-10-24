//
// Regression tests for the code-audit fixes: wire.
//

#include "test_framework.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "../Yarn/includes/Cancellation.h"
#include "../Yarn/includes/Coroutines.h"
#include "../Yarn/includes/JoinHandle.h"
#include "../Yarn/includes/Timeout.h"
#include "../Yarn/includes/Yarn.hpp"
#include "../Wire/includes/Wire.h"

using namespace std::chrono_literals;

namespace {

    template<typename Pred>
    bool wire_wait_for(Pred &&p, std::chrono::steady_clock::duration timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!p()) {
            if (std::chrono::steady_clock::now() >= deadline) return p();
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    YarnBall::Task<> recvInto(std::shared_ptr<Telegraph::Channel<int>> ch, std::atomic<int> *state) {
        auto v = co_await ch->receive();
        state->store(v ? 1 : 2); // 1 = got a value, 2 = nullopt
    }

    YarnBall::Task<> boundedRecvInto(std::shared_ptr<Telegraph::BoundedChannel<int>> ch, std::atomic<int> *state) {
        auto v = co_await ch->receive();
        state->store(v ? 1 : 2);
    }

    YarnBall::Task<> boundedSendInto(std::shared_ptr<Telegraph::BoundedChannel<int>> ch, int value,
                                     std::atomic<int> *state) {
        const bool ok = co_await ch->send(value);
        state->store(ok ? 1 : 2);
    }

    YarnBall::Task<std::optional<int>> boundedRecv(std::shared_ptr<Telegraph::BoundedChannel<int>> ch) {
        co_return co_await ch->receive();
    }

    YarnBall::Task<Telegraph::SelectResult<int>> selectOf(std::vector<std::shared_ptr<Telegraph::Channel<int>>> chans) {
        co_return co_await Telegraph::channelSelect(std::move(chans));
    }

    YarnBall::Task<std::optional<int>> recvFor(std::shared_ptr<Telegraph::Channel<int>> ch,
                                              std::chrono::microseconds d) {
        co_return co_await ch->receiveFor(d);
    }

    YarnBall::Task<bool> boundedSendFor(std::shared_ptr<Telegraph::BoundedChannel<int>> ch, int v,
                                        std::chrono::microseconds d) {
        co_return co_await ch->sendFor(v, d);
    }

    YarnBall::Task<std::optional<int>> boundedRecvFor(std::shared_ptr<Telegraph::BoundedChannel<int>> ch,
                                                      std::chrono::microseconds d) {
        co_return co_await ch->receiveFor(d);
    }

    /// A value whose move constructor can be told to throw once (to test a hand-off that fails midway).
    std::atomic<bool> g_throwOnMove{false};
    struct Fragile {
        int v = 0;
        explicit Fragile(int x) : v(x) {}
        Fragile(const Fragile &) = default;
        Fragile(Fragile &&o) : v(o.v) {
            if (g_throwOnMove.exchange(false)) throw std::runtime_error("move");
        }
    };

    YarnBall::Task<int> fragileRecv(Telegraph::Channel<Fragile> *ch) {
        auto v = co_await ch->receive();
        co_return v ? v->v : -1;
    }

    YarnBall::Task<int> fragileBoundedRecv(Telegraph::BoundedChannel<Fragile> *ch) {
        auto v = co_await ch->receive();
        co_return v ? v->v : -1;
    }

    YarnBall::Task<long> selectConsumer(std::shared_ptr<Telegraph::Channel<int>> a,
                                        std::shared_ptr<Telegraph::Channel<int>> b, bool *reordered) {
        long got = 0;
        int last[2] = {-1, -1};
        for (;;) {
            // Built outside the co_await expression: MinGW GCC 13 has an internal compiler error on a braced list there.
            std::vector<std::shared_ptr<Telegraph::Channel<int>>> sources{a, b};
            auto r = co_await Telegraph::channelSelect<int>(std::move(sources));
            if (!r.value) break;
            ++got;
            if (*r.value < last[r.index]) *reordered = true;
            last[r.index] = *r.value;
        }
        co_return got;
    }

    YarnBall::Task<> selectProducer(std::shared_ptr<Telegraph::Channel<int>> a,
                                    std::shared_ptr<Telegraph::Channel<int>> b, int n) {
        co_await YarnBall::scheduleOn(YarnBall::Yarn::instance());
        for (int i = 0; i < n; ++i) {
            a->send(i);
            b->send(i);
        }
        a->close();
        b->close();
    }

    YarnBall::Task<> awaitNext(Telegraph::Signal<int> *sig, std::atomic<int> *state) {
        try {
            auto [v] = co_await sig->next();
            state->store(1 + v * 0);
        } catch (const YarnBall::CancelledException &) {
            state->store(2);
        }
    }
}

// ---- channels: a parked waiter may outlive the channel -------------------------------------------------------

TEST(audit_wire_channel_freed_with_parked_receiver_wakes_it_with_nullopt) {
    std::atomic<int> state{0};
    auto ch = std::make_unique<Telegraph::Channel<int>>();
    // The coroutine needs a shared_ptr<Channel>; give it a non-owning one so the channel can be freed under it.
    std::shared_ptr<Telegraph::Channel<int>> view(ch.get(), [](Telegraph::Channel<int> *) {});
    YarnBall::coSpawn(recvInto(view, &state));
    EXPECT_TRUE(wire_wait_for([&] { return ch->waiterCount() == 1; }, 5s));
    ch.reset(); // ~Channel closes: the receiver is woken and must not touch the freed channel afterwards
    EXPECT_TRUE(wire_wait_for([&] { return state.load() == 2; }, 5s));
}

TEST(audit_wire_bounded_channel_freed_with_parked_waiters_wakes_them) {
    std::atomic<int> recvState{0};
    std::atomic<int> sendState{0};
    auto rx = std::make_unique<Telegraph::BoundedChannel<int>>(1);
    std::shared_ptr<Telegraph::BoundedChannel<int>> rxView(rx.get(), [](Telegraph::BoundedChannel<int> *) {});
    YarnBall::coSpawn(boundedRecvInto(rxView, &recvState));
    EXPECT_TRUE(wire_wait_for([&] { return rx->receiverCount() == 1; }, 5s));
    rx.reset();
    EXPECT_TRUE(wire_wait_for([&] { return recvState.load() == 2; }, 5s));

    auto tx = std::make_unique<Telegraph::BoundedChannel<int>>(0);
    std::shared_ptr<Telegraph::BoundedChannel<int>> txView(tx.get(), [](Telegraph::BoundedChannel<int> *) {});
    YarnBall::coSpawn(boundedSendInto(txView, 5, &sendState));
    EXPECT_TRUE(wire_wait_for([&] { return tx->senderCount() == 1; }, 5s));
    tx.reset();
    EXPECT_TRUE(wire_wait_for([&] { return sendState.load() == 2; }, 5s)); // send reports failure
}

// ---- channels: pushFront, caps ---------------------------------------------------------------------------

TEST(audit_wire_channel_push_front_on_closed_channel_keeps_value) {
    auto ch = std::make_shared<Telegraph::Channel<int>>();
    ch->close();
    EXPECT_TRUE(ch->pushFront(7));
    EXPECT_EQ(ch->size(), std::size_t{1});
    auto v = YarnBall::syncWait([](std::shared_ptr<Telegraph::Channel<int>> c) -> YarnBall::Task<std::optional<int>> {
        co_return co_await c->receive();
    }(ch));
    EXPECT_TRUE(v.has_value());
    EXPECT_EQ(*v, 7);
}

TEST(audit_wire_bounded_push_front_on_closed_channel_keeps_value) {
    auto ch = std::make_shared<Telegraph::BoundedChannel<int>>(2);
    ch->close();
    EXPECT_TRUE(ch->pushFront(9));
    auto v = YarnBall::syncWait(boundedRecv(ch));
    EXPECT_TRUE(v.has_value());
    EXPECT_EQ(*v, 9);
}

TEST(audit_wire_select_loser_value_survives_a_closed_channel) {
    // a is closed but still holds a value; b is closed and empty. b drops out; a's value is delivered.
    auto a = std::make_shared<Telegraph::Channel<int>>();
    auto b = std::make_shared<Telegraph::Channel<int>>();
    a->send(1);
    a->close();
    b->close();
    auto r = YarnBall::syncWait(selectOf({a, b}));
    EXPECT_EQ(r.index, static_cast<std::size_t>(0));
    EXPECT_TRUE(r.value.has_value());
    EXPECT_EQ(*r.value, 1);
}

TEST(audit_wire_select_without_channels_completes) {
    auto r = YarnBall::syncWait(selectOf({}));
    EXPECT_FALSE(r.value.has_value());
}

TEST(audit_wire_bounded_rendezvous_push_front_does_not_release_a_sender) {
    auto ch = std::make_shared<Telegraph::BoundedChannel<int>>(0);
    std::atomic<int> sendState{0};
    EXPECT_TRUE(ch->pushFront(1)); // a put-back value on a channel that can hold none
    YarnBall::coSpawn(boundedSendInto(ch, 2, &sendState));
    EXPECT_TRUE(wire_wait_for([&] { return ch->senderCount() == 1; }, 5s));

    auto first = YarnBall::syncWait(boundedRecv(ch));
    EXPECT_TRUE(first.has_value());
    EXPECT_EQ(*first, 1);
    // The sender's value must not have been moved into the buffer: it is still parked, not completed.
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(sendState.load(), 0);
    EXPECT_EQ(ch->size(), std::size_t{0});
    EXPECT_EQ(ch->senderCount(), std::size_t{1});

    auto second = YarnBall::syncWait(boundedRecv(ch));
    EXPECT_TRUE(second.has_value());
    EXPECT_EQ(*second, 2);
    EXPECT_TRUE(wire_wait_for([&] { return sendState.load() == 1; }, 5s));
}

TEST(audit_wire_channel_soft_cap_refuses_sends_when_full) {
    Telegraph::Channel<int> ch(2);
    EXPECT_TRUE(ch.send(1));
    EXPECT_TRUE(ch.send(2));
    EXPECT_FALSE(ch.send(3));
    EXPECT_EQ(ch.size(), std::size_t{2});
    Telegraph::Channel<int> unbounded;
    for (int i = 0; i < 1000; ++i) EXPECT_TRUE(unbounded.send(i));
}

// ---- signal ------------------------------------------------------------------------------------------------

TEST(audit_wire_signal_destroyed_with_parked_next_cancels_the_awaiter) {
    std::atomic<int> state{0};
    auto sig = std::make_unique<Telegraph::Signal<int>>();
    YarnBall::coSpawn(awaitNext(sig.get(), &state));
    EXPECT_TRUE(wire_wait_for([&] { return sig->waiterCount() == 1; }, 5s));
    sig.reset();
    EXPECT_TRUE(wire_wait_for([&] { return state.load() == 2; }, 5s)); // CancelledException, not a hang
}

TEST(audit_wire_signal_emit_runs_every_handler_then_rethrows_the_first_exception) {
    Telegraph::Signal<int> sig;
    std::vector<int> ran;
    sig.connect([&](int) { ran.push_back(1); throw std::runtime_error("first"); });
    sig.connect([&](int) { ran.push_back(2); throw std::logic_error("second"); });
    sig.connect([&](int) { ran.push_back(3); });
    bool caught = false;
    try {
        sig.emit(0);
    } catch (const std::runtime_error &e) {
        caught = std::string(e.what()) == "first";
    }
    EXPECT_TRUE(caught);
    EXPECT_EQ(ran.size(), std::size_t{3});
}

TEST(audit_wire_signal_emit_async_reports_handler_exceptions) {
    std::atomic<int> reported{0};
    // Restores the default handler on every exit path: the lambda below captures locals of this test by reference.
    struct HandlerReset {
        ~HandlerReset() { YarnBall::setUnhandledExceptionHandler({}); }
    } handlerReset;
    YarnBall::setUnhandledExceptionHandler([&](std::exception_ptr) { reported.fetch_add(1); });
    Telegraph::Signal<int> sig;
    std::atomic<int> ran{0};
    sig.connect([&](int) { ran.fetch_add(1); throw std::runtime_error("boom"); });
    sig.connect([&](int) { ran.fetch_add(1); });
    sig.emitAsync(0);
    EXPECT_TRUE(wire_wait_for([&] { return ran.load() == 2 && reported.load() == 1; }, 5s));
}

TEST(audit_wire_signal_scoped_connection_disconnects_and_survives_the_signal) {
    std::atomic<int> hits{0};
    {
        Telegraph::Signal<int> sig;
        {
            auto conn = sig.connectScoped([&](int) { hits.fetch_add(1); });
            sig.emit(0);
            EXPECT_EQ(hits.load(), 1);
            EXPECT_EQ(sig.handlerCount(), std::size_t{1});
        }
        EXPECT_EQ(sig.handlerCount(), std::size_t{0});
        sig.emit(0);
        EXPECT_EQ(hits.load(), 1);

        auto kept = sig.connectScoped([&](int) { hits.fetch_add(1); });
        auto moved = std::move(kept);
        EXPECT_FALSE(kept.disconnect());
        EXPECT_EQ(sig.handlerCount(), std::size_t{1});
        auto id = moved.release();
        EXPECT_TRUE(id != 0);
        EXPECT_EQ(sig.handlerCount(), std::size_t{1}); // released: stays connected
    }
    // A connection that outlives its Signal disconnects into nothing.
    auto lateConn = std::make_unique<Telegraph::Signal<int>::ScopedConnection>();
    {
        Telegraph::Signal<int> sig;
        *lateConn = sig.connectScoped([](int) {});
    }
    lateConn.reset();
}

// ---- channelSelect: no lost values, no reordering, no bias ----------------------------------------------

TEST(audit_wire_select_loses_and_reorders_nothing_under_load) {
    // Two producers feeding two channels while one consumer selects over them in a loop. A loser that is handed a
    // value at the instant the select is decided must put it back before the consumer's next select looks.
    int lostRuns = 0;
    int reorderRuns = 0;
    for (int r = 0; r < 60; ++r) {
        auto a = std::make_shared<Telegraph::Channel<int>>();
        auto b = std::make_shared<Telegraph::Channel<int>>();
        bool reordered = false;
        constexpr int kN = 1500;
        YarnBall::coSpawn(selectProducer(a, b, kN));
        const long got = YarnBall::syncWait(selectConsumer(a, b, &reordered));
        if (got != 2L * kN) ++lostRuns;
        if (reordered) ++reorderRuns;
    }
    EXPECT_EQ(lostRuns, 0);
    EXPECT_EQ(reorderRuns, 0);
}

TEST(audit_wire_select_does_not_favour_the_first_channel) {
    std::vector<std::shared_ptr<Telegraph::Channel<int>>> chans;
    for (int i = 0; i < 4; ++i) chans.push_back(std::make_shared<Telegraph::Channel<int>>());
    constexpr int kRounds = 800;
    long wins[4] = {0, 0, 0, 0};
    for (int i = 0; i < kRounds; ++i) {
        for (auto &c : chans) c->send(i);
        const auto r = YarnBall::syncWait(selectOf(chans));
        EXPECT_TRUE(r.value.has_value());
        ++wins[r.index];
    }
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(wins[i] >= kRounds / 20); // each gets at least 5%, not just the first
}

// ---- channels: deadlines that do not lose messages ------------------------------------------------------

TEST(audit_wire_channel_receive_for_times_out_and_leaves_no_waiter) {
    auto ch = std::make_shared<Telegraph::Channel<int>>();
    bool timedOut = false;
    try {
        YarnBall::syncWait(recvFor(ch, 20ms));
    } catch (const YarnBall::TimeoutException &) {
        timedOut = true;
    }
    EXPECT_TRUE(timedOut);
    EXPECT_EQ(ch->waiterCount(), std::size_t{0});
    ch->send(5); // goes to the buffer, not to the abandoned receiver
    auto v = YarnBall::syncWait(recvFor(ch, 1s));
    EXPECT_TRUE(v.has_value());
    EXPECT_EQ(*v, 5);
    ch->close();
    auto closed = YarnBall::syncWait(recvFor(ch, 1s));
    EXPECT_FALSE(closed.has_value());
}

TEST(audit_wire_channel_receive_for_never_drops_a_message) {
    auto ch = std::make_shared<Telegraph::Channel<int>>();
    std::atomic<bool> stop{false};
    std::atomic<long> sent{0};
    std::thread producer([&] {
        int i = 0;
        while (!stop.load()) {
            ch->send(i++);
            sent.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(280 + (i % 40) * 5));
        }
    });
    long expectNext = 0;
    long received = 0;
    bool gap = false;
    for (int i = 0; i < 1500; ++i) {
        try {
            auto v = YarnBall::syncWait(recvFor(ch, 300us));
            if (v) {
                if (*v != expectNext) gap = true;
                expectNext = *v + 1;
                ++received;
            }
        } catch (const YarnBall::TimeoutException &) {
        }
    }
    stop = true;
    producer.join();
    EXPECT_FALSE(gap); // nothing skipped: a timed-out receive never swallowed a value
    EXPECT_EQ(received + static_cast<long>(ch->size()), sent.load());
}

TEST(audit_wire_bounded_send_for_times_out_without_delivering) {
    auto ch = std::make_shared<Telegraph::BoundedChannel<int>>(1);
    EXPECT_TRUE(YarnBall::syncWait(boundedSendFor(ch, 1, 1s)));
    bool timedOut = false;
    try {
        YarnBall::syncWait(boundedSendFor(ch, 2, 20ms)); // full, nobody receiving
    } catch (const YarnBall::TimeoutException &) {
        timedOut = true;
    }
    EXPECT_TRUE(timedOut);
    EXPECT_EQ(ch->senderCount(), std::size_t{0});
    EXPECT_EQ(ch->size(), std::size_t{1});
    auto first = YarnBall::syncWait(boundedRecv(ch));
    EXPECT_EQ(*first, 1);
    EXPECT_EQ(ch->size(), std::size_t{0}); // the timed-out value 2 was not delivered
}

TEST(audit_wire_bounded_send_for_and_receive_for_are_exactly_once) {
    // A sendFor that reports success was received; one that timed out was not: no loss, no duplicate.
    auto ch = std::make_shared<Telegraph::BoundedChannel<int>>(0);
    std::atomic<bool> stop{false};
    std::vector<int> delivered;
    std::thread producer([&] {
        for (int i = 0; i < 400; ++i) {
            try {
                if (YarnBall::syncWait(boundedSendFor(ch, i, 200us))) delivered.push_back(i);
            } catch (const YarnBall::TimeoutException &) {
            }
        }
        stop = true;
    });
    std::vector<int> received;
    while (!stop.load() || ch->senderCount() > 0) {
        try {
            auto v = YarnBall::syncWait(boundedRecvFor(ch, 300us));
            if (v) received.push_back(*v);
        } catch (const YarnBall::TimeoutException &) {
        }
    }
    producer.join();
    EXPECT_TRUE(delivered == received);
}

// ---- channels: a throwing T move must not strand a waiter -----------------------------------------------

TEST(audit_wire_channel_throwing_move_leaves_the_receiver_parked) {
    Telegraph::Channel<Fragile> ch;
    auto h = YarnBall::spawnJoinable(fragileRecv(&ch));
    EXPECT_TRUE(wire_wait_for([&] { return ch.waiterCount() == 1; }, 5s));
    g_throwOnMove = true;
    bool threw = false;
    try {
        ch.send(Fragile(5));
    } catch (const std::runtime_error &) {
        threw = true;
    }
    g_throwOnMove = false;
    EXPECT_TRUE(threw);
    EXPECT_EQ(ch.waiterCount(), std::size_t{1}); // still queued, not orphaned
    EXPECT_TRUE(ch.send(Fragile(6)));
    EXPECT_EQ(YarnBall::syncWait(h.join()), 6);
}

TEST(audit_wire_bounded_throwing_move_leaves_the_receiver_parked) {
    Telegraph::BoundedChannel<Fragile> ch(2);
    auto h = YarnBall::spawnJoinable(fragileBoundedRecv(&ch));
    EXPECT_TRUE(wire_wait_for([&] { return ch.receiverCount() == 1; }, 5s));
    g_throwOnMove = true;
    bool threw = false;
    try {
        YarnBall::syncWait([](Telegraph::BoundedChannel<Fragile> *c) -> YarnBall::Task<bool> {
            co_return co_await c->send(Fragile(5));
        }(&ch));
    } catch (const std::runtime_error &) {
        threw = true;
    }
    g_throwOnMove = false;
    EXPECT_TRUE(threw);
    EXPECT_EQ(ch.receiverCount(), std::size_t{1});
    EXPECT_TRUE(YarnBall::syncWait([](Telegraph::BoundedChannel<Fragile> *c) -> YarnBall::Task<bool> {
        co_return co_await c->send(Fragile(6));
    }(&ch)));
    EXPECT_EQ(YarnBall::syncWait(h.join()), 6);
}

// ---- signal: re-entrant disconnect, cost, destroyed-before-await ---------------------------------------------

TEST(audit_wire_signal_disconnect_of_a_handler_owning_a_connection_does_not_deadlock) {
    using Sig = Telegraph::Signal<int>;
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread t([done] {
        Sig s;
        // Handler A captures the ScopedConnection of handler B (same signal): destroying A destroys B's connection,
        // which locks the signal again. The dying handler must be destroyed outside the lock.
        auto b = s.connectScoped([](int) {});
        auto holder = std::make_shared<Sig::ScopedConnection>(std::move(b));
        auto idA = s.connect([holder](int) {});
        holder.reset();
        s.disconnect(idA);
        EXPECT_EQ(s.handlerCount(), std::size_t{0});
        done->store(true);
    });
    const bool finished = wire_wait_for([&] { return done->load(); }, 5s);
    EXPECT_TRUE(finished);
    if (finished) t.join();
    else t.detach(); // deadlocked: do not hang the whole suite
}

TEST(audit_wire_signal_registering_many_handlers_is_cheap_and_keeps_snapshot_semantics) {
    // Registration must stay linear: a full list copy per connect makes 4x the handlers cost ~16x the time. An
    // absolute bound would depend on the machine's load, so compare two sizes instead (best of three, to ride out
    // a noisy run).
    const auto connectNanos = [](int count) {
        long long best = -1;
        for (int round = 0; round < 3; ++round) {
            Telegraph::Signal<int> probe;
            long sink = 0;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < count; ++i) probe.connect([&sink](int v) { sink += v; });
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            if (best < 0 || ns < best) best = ns;
        }
        return best;
    };
    // A burst of load from other processes can slow one measurement by a large factor, so a failing attempt is
    // repeated; a quadratic registration fails every attempt.
    bool linear = false;
    for (int attempt = 0; attempt < 4 && !linear; ++attempt) {
        const long long small = connectNanos(8000);
        const long long large = connectNanos(32000);
        linear = large < 10 * std::max<long long>(small, 1); // linear ~4x, quadratic ~16x
    }
    EXPECT_TRUE(linear);

    Telegraph::Signal<int> s;
    std::vector<Telegraph::Signal<int>::SlotId> ids;
    long sum = 0;
    for (int i = 0; i < 20000; ++i) ids.push_back(s.connect([&sum](int v) { sum += v; }));
    EXPECT_EQ(s.handlerCount(), std::size_t{20000});
    s.emit(1);
    EXPECT_EQ(sum, 20000L);

    // A handler that connects / disconnects during an emission: the running emission keeps its snapshot.
    Telegraph::Signal<int> t;
    int firstRuns = 0;
    int lateRuns = 0;
    Telegraph::Signal<int>::SlotId victim = 0;
    t.connect([&](int) {
        ++firstRuns;
        t.connect([&](int) { ++lateRuns; });
        t.disconnect(victim);
    });
    victim = t.connect([&](int) { ++firstRuns; });
    t.emit(0);
    EXPECT_EQ(firstRuns, 2);  // the victim still ran in this emission
    EXPECT_EQ(lateRuns, 0);   // the late handler did not
    t.emit(0);
    EXPECT_EQ(firstRuns, 3);  // victim gone
    EXPECT_EQ(lateRuns, 1);
}

TEST(audit_wire_signal_next_awaited_after_the_signal_is_destroyed_is_cancelled) {
    std::atomic<int> state{0};
    std::atomic<bool> destroyed{false};
    auto sig = std::make_unique<Telegraph::Signal<int>>();
    Telegraph::Signal<int> *raw = sig.get();
    YarnBall::coSpawn([](Telegraph::Signal<int> *sg, std::atomic<int> *st, std::atomic<bool> *gone) -> YarnBall::Task<> {
        auto aw = sg->next(); // created while the signal is alive, awaited after it is gone
        st->store(1);
        while (!gone->load()) std::this_thread::sleep_for(1ms);
        try {
            co_await aw;
            st->store(3);
        } catch (const YarnBall::CancelledException &) {
            st->store(2);
        }
    }(raw, &state, &destroyed));
    EXPECT_TRUE(wire_wait_for([&] { return state.load() == 1; }, 5s));
    sig.reset();
    destroyed = true;
    EXPECT_TRUE(wire_wait_for([&] { return state.load() == 2; }, 5s)); // not a hang
}
