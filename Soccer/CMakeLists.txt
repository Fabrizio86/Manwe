cmake_minimum_required(VERSION 3.19)
project(Soccer VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 23)

include_directories(${LIB_PATH}/Soccer/includes)
include_directories(${LIB_PATH}/Yarn/includes)

# ---- LibreSSL (libtls) ---------------------------------------------------
# TLS is opt-in. If libtls is present on the host we compile TlsStream and
# TlsListener and expose them through SOCCER_HAS_TLS; otherwise Soccer
# still builds with the TCP/UDP/Raw surface.
#
# Windows: skipped by default because libtls isn't shipped through the
# usual MSVC channels. Building TLS on Windows would need a libressl
# package surfaced via vcpkg / a custom CMAKE_PREFIX_PATH; until that
# story is in place the Win build runs without TLS.
if (NOT WIN32)
    find_path(LIBTLS_INCLUDE_DIR tls.h
            HINTS /opt/homebrew/include /usr/local/include
                  /opt/homebrew/Cellar/libressl/*/include)
    find_library(LIBTLS_LIBRARY tls
            HINTS /opt/homebrew/lib /usr/local/lib
                  /opt/homebrew/Cellar/libressl/*/lib)
    find_library(LIBSSL_LIBRARY ssl
            HINTS /opt/homebrew/lib /usr/local/lib
                  /opt/homebrew/Cellar/libressl/*/lib)
    find_library(LIBCRYPTO_LIBRARY crypto
            HINTS /opt/homebrew/lib /usr/local/lib
                  /opt/homebrew/Cellar/libressl/*/lib)
endif ()

# ---- nghttp2 (HTTP/2) ----------------------------------------------------
# nghttp2 is the reference HTTP/2 implementation (BSD-licensed, packaged
# on every distro and Homebrew). When present, Soccer compiles the
# Http2Connection coroutine wrapper and exposes SOCCER_HAS_HTTP2;
# otherwise Http2.h continues to compile with stub bodies that throw
# Http2NotImplemented. See docs/http2.md for the rationale.
find_path(LIBNGHTTP2_INCLUDE_DIR nghttp2/nghttp2.h
        HINTS /opt/homebrew/include /usr/local/include /usr/include)
find_library(LIBNGHTTP2_LIBRARY nghttp2
        HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)

# Each hardening option of the HTTP/2 sessions is probed for by symbol, not by
# version number: distributions backport CVE fixes (Ubuntu 24.04 ships 1.59 with
# nghttp2_option_set_max_continuations), so the version says little. A library
# lacking an option still gets HTTP/2; that option is just not set.
set(SOCCER_USE_NGHTTP2 FALSE)
set(SOCCER_NGHTTP2_DEFS "")
if (LIBNGHTTP2_INCLUDE_DIR AND LIBNGHTTP2_LIBRARY)
    set(SOCCER_USE_NGHTTP2 TRUE)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES ${LIBNGHTTP2_INCLUDE_DIR})
    set(CMAKE_REQUIRED_LIBRARIES ${LIBNGHTTP2_LIBRARY})
    foreach (_opt max_continuations max_settings max_outbound_ack)
        string(TOUPPER "${_opt}" _OPT)
        # check_cxx_source_compiles caches its answer: after nghttp2 is upgraded a cached
        # "No" would silently keep the hardening option off. Probe afresh every configure.
        unset(SOCCER_NGHTTP2_HAS_${_OPT} CACHE)
        check_cxx_source_compiles("
            #include <nghttp2/nghttp2.h>
            int main() { nghttp2_option *o = nullptr; nghttp2_option_set_${_opt}(o, 8); return 0; }"
                SOCCER_NGHTTP2_HAS_${_OPT})
        if (SOCCER_NGHTTP2_HAS_${_OPT})
            list(APPEND SOCCER_NGHTTP2_DEFS SOCCER_NGHTTP2_HAS_${_OPT}=1)
        else ()
            message(STATUS "Soccer: nghttp2 lacks nghttp2_option_set_${_opt}; that bound is not applied.")
        endif ()
    endforeach ()
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)
endif ()

set(SOCCER_SOURCES
        src/SocketAddress.cpp
        src/TcpStream.cpp
        src/TcpListener.cpp
        src/UdpSocket.cpp
        src/RawSocket.cpp
        src/WebSocket.cpp
)

set(SOCCER_HEADERS
        includes/Soccer.h
        includes/SocketException.h
        includes/SocketAddress.h
        includes/TcpStream.h
        includes/TcpListener.h
        includes/TcpServer.h
        includes/UdpSocket.h
        includes/RawSocket.h
        includes/IcmpEcho.h
        includes/BufferedReader.h
        includes/ConnectionLimiter.h
        includes/IoDeadline.h
        includes/NetUtil.h
        includes/HttpClient.h
        includes/HttpServer.h
        includes/WebSocket.h
        includes/Http2.h
)

if (WIN32)
    list(APPEND SOCCER_HEADERS includes/Overlapped.h)
endif ()

if (LIBTLS_INCLUDE_DIR AND LIBTLS_LIBRARY AND LIBSSL_LIBRARY AND LIBCRYPTO_LIBRARY)
    list(APPEND SOCCER_SOURCES src/TlsStream.cpp src/TlsListener.cpp)
    list(APPEND SOCCER_HEADERS includes/TlsStream.h includes/TlsListener.h)
    include_directories(${LIBTLS_INCLUDE_DIR})
endif ()

if (SOCCER_USE_NGHTTP2)
    list(APPEND SOCCER_SOURCES src/Http2.cpp)
    include_directories(${LIBNGHTTP2_INCLUDE_DIR})
endif ()

add_library(Soccer STATIC ${SOCCER_SOURCES} ${SOCCER_HEADERS})

target_include_directories(Soccer PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/includes>
        $<INSTALL_INTERFACE:include/manwe/soccer>)

# Soccer's awaiters live on top of Yarn (Coroutines, Reactor, IoAwaiters).
target_link_libraries(Soccer PUBLIC Yarn)

# The libraries are linked with absolute paths while building in-tree, but the
# EXPORTED interface must not bake this machine's paths into an install: it names
# the imported targets Manwe::TlsDeps / Manwe::Nghttp2Deps, which the installed
# ManweConfig.cmake defines by locating the libraries on the consumer's machine
# (keg-only prefixes included), which is what keeps the package relocatable. The same goes for
# the include directories: Soccer's public headers only forward-declare the
# libtls / nghttp2 types, so those directories stay private to this target.
if (LIBTLS_INCLUDE_DIR AND LIBTLS_LIBRARY AND LIBSSL_LIBRARY AND LIBCRYPTO_LIBRARY)
    target_link_libraries(Soccer PUBLIC
            $<BUILD_INTERFACE:${LIBTLS_LIBRARY}>
            $<BUILD_INTERFACE:${LIBSSL_LIBRARY}>
            $<BUILD_INTERFACE:${LIBCRYPTO_LIBRARY}>
            $<INSTALL_INTERFACE:Manwe::TlsDeps>)
    target_include_directories(Soccer PRIVATE ${LIBTLS_INCLUDE_DIR})
    target_compile_definitions(Soccer PUBLIC SOCCER_HAS_TLS=1)
else ()
    message(STATUS "Soccer: libtls not found; TLS support disabled. "
            "Install LibreSSL (brew install libressl) to enable.")
endif ()

if (SOCCER_USE_NGHTTP2)
    target_link_libraries(Soccer PUBLIC
            $<BUILD_INTERFACE:${LIBNGHTTP2_LIBRARY}>
            $<INSTALL_INTERFACE:Manwe::Nghttp2Deps>)
    target_include_directories(Soccer PRIVATE ${LIBNGHTTP2_INCLUDE_DIR})
    target_compile_definitions(Soccer PUBLIC SOCCER_HAS_HTTP2=1)
    target_compile_definitions(Soccer PRIVATE ${SOCCER_NGHTTP2_DEFS})
else ()
    message(STATUS "Soccer: nghttp2 not found; HTTP/2 stays a stub. "
            "Install nghttp2 (brew install nghttp2) to enable.")
endif ()
