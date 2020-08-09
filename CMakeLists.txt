cmake_minimum_required(VERSION 3.16)
project(Yarns VERSION 3.1.2)

set(CMAKE_CXX_STANDARD 20)

if (${CMAKE_BUILD_TYPE} STREQUAL "Release")
    set(LIB_DIR cmake-build-release)
elseif (${CMAKE_BUILD_TYPE} STREQUAL "Debug")
    set(LIB_DIR cmake-build-debug)
endif ()

set(LIB_PATH ${PROJECT_SOURCE_DIR})

add_subdirectory(Yarns)
add_subdirectory(UnitTest)
