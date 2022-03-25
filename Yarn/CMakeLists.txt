cmake_minimum_required(VERSION 3.16)
project(Yarn VERSION 3.1.2)

set(CMAKE_CXX_STANDARD 23)

include_directories(${LIB_PATH}/Yarn/includes)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -pthread -fPIC")
endif ()

add_library(Yarn
        includes/Yarn.h
        src/Yarn.cpp
        includes/ITask.h includes/Fiber.h src/Fiber.cpp includes/IScheduler.h includes/RandomScheduler.h src/RandomScheduler.cpp includes/YarnBall.h src/YarnBall.cpp includes/IWaitable.h src/Waitable.cpp)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -G MinGW Makefiles")
