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

add_executable(YarnTests main.cpp test_framework.hpp)

# Link against the CMake targets (not raw .a paths) so that PUBLIC compile
# definitions like SOCCER_HAS_TLS propagate transitively.
target_link_libraries(YarnTests PUBLIC Soccer Wire Yarn)

# If libtls is linked into Soccer, propagate the link to YarnTests too.
# (Static libraries' PUBLIC dependencies are inherited automatically when
# linked as targets; this is here defensively.)
