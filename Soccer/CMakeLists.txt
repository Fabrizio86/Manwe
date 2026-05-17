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
        includes/HttpClient.h
        includes/HttpServer.h
        includes/WebSocket.h
        includes/Http2.h
)

if (WIN32)
    list(APPEND SOCCER_HEADERS includes/Overlapped.h)
endif ()

if (LIBTLS_INCLUDE_DIR AND LIBTLS_LIBRARY)
    list(APPEND SOCCER_SOURCES src/TlsStream.cpp src/TlsListener.cpp)
    list(APPEND SOCCER_HEADERS includes/TlsStream.h includes/TlsListener.h)
    include_directories(${LIBTLS_INCLUDE_DIR})
endif ()

if (LIBNGHTTP2_INCLUDE_DIR AND LIBNGHTTP2_LIBRARY)
    list(APPEND SOCCER_SOURCES src/Http2.cpp)
    include_directories(${LIBNGHTTP2_INCLUDE_DIR})
endif ()

add_library(Soccer ${SOCCER_SOURCES} ${SOCCER_HEADERS})

target_include_directories(Soccer PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/includes>
        $<INSTALL_INTERFACE:include/manwe/soccer>)

# Soccer's awaiters live on top of Yarn (Coroutines, Reactor, IoAwaiters).
target_link_libraries(Soccer PUBLIC Yarn)

if (LIBTLS_INCLUDE_DIR AND LIBTLS_LIBRARY)
    target_link_libraries(Soccer PUBLIC
            ${LIBTLS_LIBRARY}
            ${LIBSSL_LIBRARY}
            ${LIBCRYPTO_LIBRARY})
    target_compile_definitions(Soccer PUBLIC SOCCER_HAS_TLS=1)
else ()
    message(STATUS "Soccer: libtls not found; TLS support disabled. "
            "Install LibreSSL (brew install libressl) to enable.")
endif ()

if (LIBNGHTTP2_INCLUDE_DIR AND LIBNGHTTP2_LIBRARY)
    target_link_libraries(Soccer PUBLIC ${LIBNGHTTP2_LIBRARY})
    target_include_directories(Soccer PUBLIC ${LIBNGHTTP2_INCLUDE_DIR})
    target_compile_definitions(Soccer PUBLIC SOCCER_HAS_HTTP2=1)
else ()
    message(STATUS "Soccer: nghttp2 not found; HTTP/2 stays a stub. "
            "Install nghttp2 (brew install nghttp2) to enable.")
endif ()
