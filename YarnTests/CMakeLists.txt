cmake_minimum_required(VERSION 3.21)
project(YarnTests)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# POSIX toolchains want -pthread / -fPIC; MSVC has C++ threads on by
# default and would reject the unknown flags. We deliberately do NOT
# also pass -std=c++2a here: CMake's CMAKE_CXX_STANDARD=23 above already
# injects the right flag, and overriding with c++2a (= C++20) would
# silently downgrade the standard on AppleClang -- whose libc++ hides
# <stop_token> outside C++23 unless _LIBCPP_ENABLE_EXPERIMENTAL is set.
if (NOT MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -fPIC")
endif ()

add_executable(YarnTests main.cpp soccer_limits_tests.cpp cancellation_tests.cpp
        audit_core_tests.cpp audit_async_tests.cpp audit_wire_tests.cpp audit_soccer_net_tests.cpp audit_soccer_http_tests.cpp audit_embedded_tests.cpp
        test_framework.hpp)

# Link against the CMake targets (not raw .a paths) so that PUBLIC compile
# definitions like SOCCER_HAS_TLS propagate transitively.
# Embedded's regression tests need a pseudo-terminal, so they are POSIX only (the file is empty elsewhere).
if(WIN32)
    target_link_libraries(YarnTests PUBLIC Soccer Wire Yarn)
else()
    target_link_libraries(YarnTests PUBLIC Embedded Soccer Wire Yarn)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(YarnTests PUBLIC util)
    endif()
endif()
add_test(NAME YarnTests COMMAND YarnTests)

# Process-exit reproducer for ~Yarn (not part of YarnTests: it must be run many times, see the file header).
add_executable(exit_race exit_race.cpp)
target_link_libraries(exit_race PUBLIC Yarn)

# Reactor reproducer: a reader and a writer on the same fd (exit 0 = both resumed).
# POSIX only (socketpair/fcntl); the bug it covers is epoll-specific.
if(NOT WIN32)
    add_executable(reactor_dual_wait reactor_dual_wait.cpp)
    target_link_libraries(reactor_dual_wait PUBLIC Yarn)
    add_test(NAME reactor_dual_wait COMMAND reactor_dual_wait)
endif()

# If libtls is linked into Soccer, propagate the link to YarnTests too.
# (Static libraries' PUBLIC dependencies are inherited automatically when
# linked as targets; this is here defensively.)
