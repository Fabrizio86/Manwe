// Reproducer for the exit-time race in ~Yarn: a process that used Yarn returns from main with coroutines still scheduled.
// Before the fix a worker could still be inside stealFromPeers()/currentSnapshot() when the Yarn singleton was destroyed and abort with
// "mutex lock failed: Invalid argument" (about 1 in 600 runs under CPU load). Run it in a loop with busy processes competing:
//   for i in $(seq 3000); do ./exit_race 8 >/dev/null || echo BAD; done
#include "Coroutines.h"
#include "Timers.h"
#include "Yarn.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
static YarnBall::Task<void> ticker(std::atomic<int>& n) {
    for (;;) { co_await YarnBall::sleepFor(std::chrono::milliseconds(1)); n++; }
}
static std::atomic<int> g_n{0};
int main(int argc, char** argv) {
    int tasks = argc > 1 ? atoi(argv[1]) : 8;
    for (int i = 0; i < tasks; ++i) YarnBall::coSpawn(ticker(g_n));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::printf("exiting with %d ticks\n", g_n.load());
    return 0;   // static destruction of the Yarn singleton runs with the tickers still scheduled
}
