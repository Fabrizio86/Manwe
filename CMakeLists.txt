cmake_minimum_required(VERSION 3.16)
project(Yarns VERSION 3.1.2)

set(CMAKE_CXX_STANDARD 20)

if (${CMAKE_BUILD_TYPE} STREQUAL "Release")
    set(LIB_DIR ${PROJECT_SOURCE_DIR}/cmake-build-release/Yarns)
elseif (${CMAKE_BUILD_TYPE} STREQUAL "MinSizeRel")
    set(LIB_DIR ${PROJECT_SOURCE_DIR}/Yarns)
elseif (${CMAKE_BUILD_TYPE} STREQUAL "Debug")
    set(LIB_DIR ${PROJECT_SOURCE_DIR}/cmake-build-debug/Yarns)
endif ()

set(LIB_PATH ${PROJECT_SOURCE_DIR})

add_subdirectory(Yarns)
add_subdirectory(UnitTest)