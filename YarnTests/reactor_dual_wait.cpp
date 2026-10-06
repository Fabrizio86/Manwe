// Reproducer: a reader and a writer waiting on the SAME socket at the same time.
// On Linux epoll keeps one interest set per fd, so registering the writer (EPOLL_CTL_MOD) used to drop the reader's registration and
// the reader coroutine was never resumed (kqueue keeps read and write filters apart, so macOS never showed it). Both must resume.
//   exit 0 = both resumed, 1 = the reader (or writer) was lost.
#include "Coroutines.h"
#include "IoAwaiters.h"
#include "Yarn.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

static std::atomic<int> g_read{0}, g_write{0};

static YarnBall::Task<void> reader(int fd) {
    co_await YarnBall::io::WaitReadableAwaiter{fd};
    g_read = 1;
}
static YarnBall::Task<void> writer(int fd) {
    co_await YarnBall::io::WaitWritableAwaiter{fd};
    g_write = 1;
}

int main() {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 2;
    for (int fd : sv) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    // Fill sv[0]'s send buffer so it is not writable: the writer must really wait.
    char junk[4096] = {};
    while (::write(sv[0], junk, sizeof junk) > 0) {}

    YarnBall::coSpawn(reader(sv[0]));                       // waits for data from sv[1]
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    YarnBall::coSpawn(writer(sv[0]));                       // waits for room in sv[0]'s send buffer; registers on the same fd
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    char c = 'x';
    if (::write(sv[1], &c, 1) < 0) return 2;                         // reader's event
    char sink[4096];
    while (::read(sv[1], sink, sizeof sink) > 0) {}         // frees sv[0]'s send buffer: writer's event
    for (int i = 0; i < 200 && !(g_read && g_write); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::printf("reader resumed=%d writer resumed=%d\n", g_read.load(), g_write.load());
    return (g_read && g_write) ? 0 : 1;
}
