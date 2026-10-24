//
// Regression tests for the third audit pass (async).
//

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../Yarn/includes/Coroutines.h"
#include "../Yarn/includes/FileIo.h"
#include "../Yarn/includes/Timeout.h"
#include "../Yarn/includes/Timers.h"
#include "../Yarn/includes/WhenAny.h"
#include "../Yarn/includes/Yarn.hpp"

#if !defined(_WIN32)
    #include <sys/resource.h>
#endif

using namespace std::chrono_literals;

namespace {

    using YarnBall::Task;

    Task<int> finishesAfter50ms() {
        co_await YarnBall::sleepFor(50ms);
        co_return 7;
    }

    Task<int> withHugeTimeout(int which) {
        switch (which) {
            case 0: co_return co_await YarnBall::withTimeout(finishesAfter50ms(), std::chrono::seconds::max());
            case 1: co_return co_await YarnBall::withTimeout(finishesAfter50ms(), std::chrono::hours::max());
            case 2: co_return co_await YarnBall::withTimeout(finishesAfter50ms(), std::chrono::years(1000));
            default: co_return co_await YarnBall::withTimeout(finishesAfter50ms(), std::chrono::seconds::max(), YarnBall::joinLosers);
        }
    }

    /// Owning-instance counter: a moved-from probe owns nothing, so live() is the number of values really alive.
    struct Probe {
        static std::atomic<int> &live() { static std::atomic<int> n{0}; return n; }
        bool owns = true;
        Probe() { live().fetch_add(1); }
        Probe(Probe &&o) noexcept : owns(std::exchange(o.owns, false)) {}
        Probe(const Probe &) = delete;
        ~Probe() { if (owns) live().fetch_sub(1); }
    };

    Task<Probe> probeNow() { co_return Probe{}; }

    /// A loser that cannot be cancelled: blocks a worker, then produces a value.
    Task<Probe> probeSlow() {
        std::this_thread::sleep_for(60ms);
        co_return Probe{};
    }

    Task<int> whenAnyJoinedLeavesOnlyTheWinner() {
        std::vector<Task<Probe>> tasks;
        tasks.push_back(probeNow());
        tasks.push_back(probeSlow());
        auto r = co_await YarnBall::whenAny(std::move(tasks), YarnBall::joinLosers);
        co_return Probe::live().load(); // only the winner's value (held by r) may be alive
    }

    Task<int> timeoutJoinedDropsTheLoserValue() {
        try {
            (void) co_await YarnBall::withTimeout(probeSlow(), 10ms, YarnBall::joinLosers);
        } catch (const YarnBall::TimeoutException &) {
            co_return Probe::live().load();
        }
        co_return -1;
    }

    Task<> writeUnderLimit(std::string path, std::string data) {
        co_await YarnBall::fs::writeString(path, data);
    }
}

TEST(audit3_async_with_timeout_huge_duration_does_not_fire_at_once) {
    for (int which = 0; which < 4; ++which) {
        EXPECT_EQ(YarnBall::syncWait(withHugeTimeout(which)), 7);
    }
}

TEST(audit3_async_join_losers_destroys_the_loser_value_before_the_caller_resumes) {
    EXPECT_EQ(Probe::live().load(), 0);
    EXPECT_EQ(YarnBall::syncWait(whenAnyJoinedLeavesOnlyTheWinner()), 1);
    EXPECT_EQ(Probe::live().load(), 0);
    EXPECT_EQ(YarnBall::syncWait(timeoutJoinedDropsTheLoserValue()), 0);
    EXPECT_EQ(Probe::live().load(), 0);
}

#if !defined(_WIN32)
namespace {
    /// Caps the size of files this process may write and ignores SIGXFSZ; restores both on scope exit.
    struct FileSizeLimit {
        rlimit old{};
        struct sigaction oldAct{};
        explicit FileSizeLimit(rlim_t bytes) {
            ::getrlimit(RLIMIT_FSIZE, &old);
            struct sigaction ign{};
            ign.sa_handler = SIG_IGN;
            sigemptyset(&ign.sa_mask);
            ::sigaction(SIGXFSZ, &ign, &oldAct);
            rlimit lim = old;
            lim.rlim_cur = bytes;
            ::setrlimit(RLIMIT_FSIZE, &lim);
        }
        ~FileSizeLimit() {
            ::setrlimit(RLIMIT_FSIZE, &old);
            ::sigaction(SIGXFSZ, &oldAct, nullptr);
        }
    };

    Task<> closeAfterBufferedWrite(std::string path) {
        auto f = co_await YarnBall::fs::File::open(path, YarnBall::fs::OpenMode::Write);
        const std::string data(40, 'y');
        (void) co_await f.write(std::as_bytes(std::span<const char>(data)));
        co_await f.close();
    }
}

TEST(audit3_async_file_write_reports_a_failed_final_flush) {
    const auto path = (std::filesystem::temp_directory_path() / "yarn_audit3_fsize.bin").string();
    {
        FileSizeLimit limit(10);
        // 100 bytes sit in the stream buffer; the write(2) that finally flushes them is refused by the size limit.
        EXPECT_THROWS_AS(YarnBall::syncWait(writeUnderLimit(path, std::string(100, 'x'))), YarnBall::fs::FileIoError);
        EXPECT_THROWS_AS(YarnBall::syncWait(closeAfterBufferedWrite(path)), YarnBall::fs::FileIoError);
    }
    std::filesystem::remove(path);
}
#endif
