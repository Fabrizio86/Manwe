cmake_minimum_required(VERSION 3.16)
project(Wire VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 23)

include_directories(${LIB_PATH}/Yarn/includes)

add_library(Wire
        src/Messenger.cpp
        includes/Messenger.h
        includes/IMessageQueue.h
        src/LMessageQueue.cpp
        includes/LMessageQueue.h
        src/MacMessageQueue.cpp
        includes/MacMessageQueue.h)

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