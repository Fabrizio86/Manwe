cmake_minimum_required(VERSION 3.21)
project(benchmarks)

set(CMAKE_CXX_STANDARD 23)

add_executable(bench_yarn bench_yarn.cpp)
target_link_libraries(bench_yarn PRIVATE Yarn)

# End-to-end "request"-shaped benchmark: highlights the per-await win
# that the symmetric-transfer Task<T> design buys over poll-loop
# futures. See bench_async_server.cpp for the scenarios.
add_executable(bench_async_server bench_async_server.cpp)
target_link_libraries(bench_async_server PRIVATE Yarn)

# A future bench_soccer can be added the same way and link Soccer.
