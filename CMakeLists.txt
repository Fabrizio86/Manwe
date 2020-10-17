cmake_minimum_required(VERSION 3.16)
project(Yarns VERSION 3.1.2)

set(CMAKE_CXX_STANDARD 20)

message("mode: " ${CMAKE_BUILD_TYPE})

if (${CMAKE_BUILD_TYPE} STREQUAL "Release")
    IF (WIN32)
        set(LIB_DIR ${PROJECT_SOURCE_DIR}/cmake-build-release-visualstudio/Yarns)
    else ()
        set(LIB_DIR ${PROJECT_SOURCE_DIR}/cmake-build-release/Yarns)
    endif ()
elseif (${CMAKE_BUILD_TYPE} STREQUAL "MinSizeRel")
    set(LIB_DIR ${PROJECT_SOURCE_DIR}/Yarns)
elseif (${CMAKE_BUILD_TYPE} STREQUAL "Debug")
    IF (WIN32)
        set(LIB_DIR ${PROJECT_SOURCE_DIR}/cmake-build-debug-visualstudio/Yarns)
    else ()
        set(LIB_DIR ${PROJECT_SOURCE_DIR}/cmake-build-debug/Yarns)
    endif ()
endif ()

set(LIB_PATH ${PROJECT_SOURCE_DIR})

add_subdirectory(Yarns)
add_subdirectory(UnitTest)