cmake_minimum_required(VERSION 3.21)
project(YarnTests)

set(CMAKE_CXX_STANDARD 23)

include_directories(${LIB_PATH}/Yarns/includes)

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -pthread -fPIC")

add_executable(YarnTests main.cpp)

IF (WIN32)
    set(NAME Yarn)
    set(EXT lib)
else()
    set(NAME libYarn)
    set(EXT a)
endif()

target_link_libraries(YarnTests PUBLIC ${LIB_PATH}/bin/${NAME}.${EXT})
