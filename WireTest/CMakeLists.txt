cmake_minimum_required(VERSION 3.16)
project(WireTest VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 23)

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -pthread -fPIC")

include_directories(${LIB_PATH}/Yarn/includes)
include_directories(${LIB_PATH}/Wire/includes)

add_executable(WireTest
        main.cpp
        test.hpp
)

IF (WIN32)
    set(NAME Yarn)
    set(EXT lib)
else ()
    set(NAME libYarn)
    set(EXT a)
endif ()

target_compile_options(WireTest PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(WireTest PUBLIC ${LIB_PATH}/bin/${NAME}.${EXT})