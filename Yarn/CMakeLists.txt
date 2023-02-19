cmake_minimum_required(VERSION 3.16)
project(Yarn VERSION 3.1.2)

set(CMAKE_CXX_STANDARD 23)

include_directories(${LIB_PATH}/Yarn/includes)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -pthread -fPIC")
endif ()

add_library(Yarn
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
        includes/Definitions.hpp ../Wire/includes/Centraline.hpp)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -G MinGW Makefiles")
