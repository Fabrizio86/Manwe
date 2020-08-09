cmake_minimum_required(VERSION 3.16)
project(Yarns VERSION 3.1.2)

set(CMAKE_CXX_STANDARD 20)

include_directories(${LIB_PATH}/Yarns/includes)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -pthread -fPIC")
endif ()

add_library(Yarns
        includes/itask.hpp
        includes/iawaitable.hpp
        includes/queue.hpp
        includes/system.hpp
        src/system.cpp
        includes/yarns.hpp
        src/yarns.cpp
        includes/Definitions.h)
