cmake_minimum_required(VERSION 3.16)
project(Wire VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 23)

include_directories(${LIB_PATH}/Yarn/includes)

add_library(Wire INTERFACE)

IF (WIN32)
    set(NAME Yarn)
    set(EXT lib)
else()
    set(NAME libYarn)
    set(EXT a)
endif()

message("message: " ${NAME}.${EXT})

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -G MinGW Makefiles")

target_link_libraries(Wire INTERFACE ${LIB_PATH}/bin/${NAME}.${EXT})
set_target_properties(Wire PROPERTIES LINKER_LANGUAGE CXX)