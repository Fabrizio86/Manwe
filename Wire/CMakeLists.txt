cmake_minimum_required(VERSION 3.16)
project(Wire VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 23)

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -pthread -fPIC")

include_directories(${LIB_PATH}/Yarn/includes)
include_directories(${LIB_PATH}/Wire/includes)

add_library(Wire
        includes/Epoc.hpp
        includes/Centraline.hpp
        includes/iEvent.hpp
        ../WireTest/test.hpp
        ../WireTest/main.cpp)

IF (WIN32)
    set(NAME Yarn)
    set(EXT lib)
else ()
    set(NAME libYarn)
    set(EXT a)
endif ()

target_compile_options(Wire PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(Wire INTERFACE ${LIB_PATH}/bin/${NAME}.${EXT})
set_target_properties(Wire PROPERTIES LINKER_LANGUAGE CXX)