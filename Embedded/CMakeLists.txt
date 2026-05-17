cmake_minimum_required(VERSION 3.19)
project(Embedded VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Manwe::Embedded -- Pi / IoT / hardware-adjacent primitives that
# compose on top of Yarn's Reactor. Currently:
#  - SerialPort (POSIX termios; Windows COM-port path is a stub).
#  - GpioChip / GpioLine (Linux /dev/gpiochip* ioctl; non-Linux is a
#    stub).
#
# UNTESTED on the dev machine in the introducing session (no USB
# serial dongle, no Pi attached). The code follows the canonical
# POSIX patterns and shares the Reactor-driven suspend path with
# Soccer's sockets. Validate before depending in production.

set(EMBEDDED_SOURCES
        src/SerialPort.cpp
        src/Gpio.cpp
)

set(EMBEDDED_HEADERS
        includes/SerialPort.h
        includes/Gpio.h
)

add_library(Embedded ${EMBEDDED_SOURCES} ${EMBEDDED_HEADERS})

target_include_directories(Embedded PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/includes>
        $<INSTALL_INTERFACE:include/manwe/embedded>)

target_link_libraries(Embedded PUBLIC Yarn)
