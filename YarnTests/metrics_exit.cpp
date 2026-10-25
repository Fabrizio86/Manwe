// Process-exit check for the metrics Registry: handlers on pool threads keep recording while static destructors run.
// The Registry is constructed after the Yarn singleton, so an ordinary function-local static would be destroyed first
// and the workers still running would touch freed memory. Exit 0 (and a clean sanitizer run) means it outlives them.
#include "Metrics.h"
#include "Yarn.hpp"
#include <chrono>
#include <thread>
using namespace YarnBall;
int main() {
    Yarn::instance();
    metrics::Registry::instance().counter("requests_total", "r");
    for (int t = 0; t < 4; ++t)
        Yarn::instance()->run([] {
            for (int i = 0; i < 2000000; ++i) metrics::Registry::instance().counter("requests_total").inc();
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return 0;
}
