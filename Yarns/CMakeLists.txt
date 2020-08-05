cmake_minimum_required(VERSION 3.16)
project(Yarns VERSION 3.1.2)

set(CMAKE_CXX_STANDARD 17)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -pthread -fPIC")
endif ()

add_library(Yarns
        itask.hpp
        iwaitable.hpp
        queue.hpp
        waitable.hpp
        waitable.cpp
        yarns.hpp
        yarns.cpp
        Definitions.h)
