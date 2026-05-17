cmake_minimum_required(VERSION 3.16)
project(Wire VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 23)

# Wire is header-only. Expose includes via an INTERFACE library so other
# components that depend on it (tests, examples) pick up the include path
# automatically.
add_library(Wire INTERFACE)

target_include_directories(Wire INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/includes>
        $<INSTALL_INTERFACE:include/manwe/wire>)

# Wire's coroutine awaiters need the Yarn runtime at link time.
target_link_libraries(Wire INTERFACE Yarn)
