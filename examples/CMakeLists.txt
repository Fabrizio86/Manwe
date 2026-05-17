cmake_minimum_required(VERSION 3.21)
project(examples)

set(CMAKE_CXX_STANDARD 23)

function(add_manwe_example name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE Soccer Wire Yarn)
endfunction()

add_manwe_example(echo_server)
add_manwe_example(http_get)
add_manwe_example(http_server)
add_manwe_example(chat_signals)
add_manwe_example(producer_consumer)
add_manwe_example(ws_echo_server)
add_manwe_example(graceful_http)
add_manwe_example(http2_get)

# ping uses POSIX raw-socket helpers (unistd.h, geteuid for the privilege
# hint). Skipped on Windows; raw sockets there require admin and the
# example's helper text would be wrong.
if (NOT WIN32)
    add_manwe_example(ping)
endif ()
