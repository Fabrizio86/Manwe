cmake_minimum_required(VERSION 3.20)
project(Yarn VERSION 3.1.2)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${LIB_PATH}/Yarn/includes)

# Do NOT add -std=c++2a -- CMake's CMAKE_CXX_STANDARD=23 already injects the
# correct flag, and an explicit -std=c++2a (= C++20) would silently downgrade
# the standard, breaking AppleClang's libc++ exposure of <stop_token> et al.
# -pthread comes from Threads::Threads below; -fPIC from CMAKE_POSITION_INDEPENDENT_CODE.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)


# MSVC: keep std::min/max from being macroised by Windows headers, allow
# the standards-conformant preprocessor that the coroutine machinery needs.
if (MSVC)
    set(YARN_MSVC_DEFS NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    set(YARN_MSVC_OPTS /Zc:preprocessor /EHsc /permissive-)
endif ()

# ---- Optional Linux reactor backends -------------------------------------
# YARN_USE_IO_URING: replaces the default epoll backend on Linux with
# io_uring (via liburing). Requires liburing-dev (or equivalent) on the host.
option(YARN_USE_IO_URING "Use io_uring on Linux (requires liburing)" OFF)
# YARN_IO_URING_SQPOLL: when io_uring is enabled, set IORING_SETUP_SQPOLL so
# the kernel polls the submission queue. Older kernels (< 5.11) require
# CAP_SYS_NICE.
option(YARN_IO_URING_SQPOLL "Use SQPOLL with io_uring (subsumes YARN_USE_IO_URING)" OFF)

add_library(Yarn STATIC
        includes/Yarn.hpp
        src/Yarn.cpp
        includes/ITask.hpp
        includes/Fiber.hpp
        src/Fiber.cpp
        includes/IScheduler.hpp
        includes/RandomScheduler.hpp
        src/RandomScheduler.cpp
        includes/YarnBall.hpp
        src/YarnBall.cpp
        includes/IWaitable.hpp
        src/Waitable.cpp
        includes/StopExecutionException.hpp
        src/StopExecutionException.cpp
        includes/Definitions.hpp
        includes/MPMCQueue.h
        includes/WorkStealingDeque.h
        includes/Coroutines.h
        includes/Cancellation.h
        includes/Reactor.h
        src/Reactor.cpp
        includes/AsyncSync.h
        src/AsyncSync.cpp
        includes/IoAwaiters.h
        includes/PlatformNet.h
        includes/SocketHandle.h
        includes/WindowsOverlapped.h
        includes/WhenAll.h
        includes/WhenAny.h
        includes/Timeout.h
        includes/Stream.h
        includes/JoinHandle.h
        includes/SignalSet.h
        src/SignalSet.cpp
        includes/Metrics.h
        src/Metrics.cpp
        includes/Log.h
        src/Log.cpp
        includes/Trace.h
        src/Trace.cpp
        includes/FileIo.h
        includes/Timers.h
        includes/SmallObjectPool.h)

target_include_directories(Yarn PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/includes>
        $<INSTALL_INTERFACE:include/manwe/yarn>)

# Windows: the WSAPoll-based readiness loop and the IOCP completion thread
# both need the WinSock 2 + MSWSock libraries. Linked PUBLIC so transitive
# consumers (Soccer, tests, examples) inherit the link automatically.
if (WIN32)
    target_link_libraries(Yarn PUBLIC ws2_32 mswsock)
endif ()

# Propagate the MSVC definitions/options to every consumer (Soccer, tests, ...),
# not just this directory.
if (MSVC)
    target_compile_definitions(Yarn PUBLIC ${YARN_MSVC_DEFS})
    target_compile_options(Yarn PUBLIC ${YARN_MSVC_OPTS})
endif ()

find_package(Threads REQUIRED)
target_link_libraries(Yarn PUBLIC Threads::Threads)

# AppleClang's libc++ hides several C++20/23 facilities (notably
# <stop_token>, the freestanding bits of <coroutine>, jthread) behind
# _LIBCPP_ENABLE_EXPERIMENTAL / -fexperimental-library, even on Xcode 16
# whose libc++ otherwise carries them. Propagate the opt-in PUBLIC-ly so
# Soccer, YarnTests, and any external consumer linking against Yarn picks
# it up automatically.
if (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    target_compile_options(Yarn PUBLIC -fexperimental-library)
    target_compile_definitions(Yarn PUBLIC _LIBCPP_ENABLE_EXPERIMENTAL)
endif ()

# Apply the io_uring opt-in here so it propagates publicly to every TU
# that includes Yarn headers (the Reactor's storage layout depends on the
# define being consistent across the whole link).
if (YARN_USE_IO_URING OR YARN_IO_URING_SQPOLL)
    if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "YARN_USE_IO_URING is Linux-only (current: ${CMAKE_SYSTEM_NAME})")
    endif ()
    find_library(LIBURING_LIBRARY uring REQUIRED)
    target_compile_definitions(Yarn PUBLIC YARN_USE_IO_URING)
    if (YARN_IO_URING_SQPOLL)
        target_compile_definitions(Yarn PUBLIC YARN_IO_URING_SQPOLL)
    endif ()
    # The installed package names the library through Manwe::UringDeps, which its
    # ManweConfig.cmake locates on the consumer's machine instead of exporting this path.
    target_link_libraries(Yarn PUBLIC
            $<BUILD_INTERFACE:${LIBURING_LIBRARY}>
            $<INSTALL_INTERFACE:Manwe::UringDeps>)
endif ()
