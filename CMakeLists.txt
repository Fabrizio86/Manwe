cmake_minimum_required(VERSION 3.20)
project(Manwe VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The scheduler and the work-stealing deque rely on seq_cst fences (a store-load handshake no acquire/release pair gives).
# GCC's ThreadSanitizer cannot model a standalone fence and warns at each one; the warning carries no information here.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_FLAGS MATCHES "fsanitize=[a-z,]*thread")
    add_compile_options(-Wno-tsan)
endif()

# Default to an optimised build so benchmarks are meaningful.
# Only when Manwe is the top-level project: a parent that embeds it keeps its own build type.
if (CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif ()

# Position-independent code, so the static libraries can be linked into a consumer's shared object.
# Yarn, Soccer and Embedded are always STATIC (BUILD_SHARED_LIBS is ignored): Yarn holds process-wide singletons
# (Reactor, pools), which a second copy inside a shared library would duplicate.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Build outputs go to bin/ (tracked) ----------------------------------
# Only when Manwe is the top-level project; as a subdirectory it must not
# override the consumer's output layout or write into its own source tree.
if (CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
endif ()

# ---- LIB_PATH propagated to subdirectory CMakeLists for include lookups --
set(LIB_PATH ${PROJECT_SOURCE_DIR})

# ---- Components ----------------------------------------------------------
# Consumers embedding Manwe as a submodule (e.g. via add_subdirectory) rarely
# want its own test/example/benchmark executables in their build graph; these
# default ON so a standalone checkout behaves exactly as before.
option(MANWE_BUILD_TESTS "Build YarnTests" ON)
option(MANWE_BUILD_EXAMPLES "Build the examples/ executables" ON)
option(MANWE_BUILD_BENCHMARKS "Build the benchmarks/ executables" ON)

if (CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    enable_testing()
endif ()
add_subdirectory(Yarn)
add_subdirectory(Wire)
add_subdirectory(Soccer)
add_subdirectory(Embedded)
if (MANWE_BUILD_TESTS)
    add_subdirectory(YarnTests)
endif ()
if (MANWE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()
if (MANWE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif ()

# ---- Install / find_package(Manwe) ---------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Install each public-facing component under the Manwe export set so
# consumers can write target_link_libraries(... PRIVATE Manwe::Yarn) etc.
install(TARGETS Yarn Wire Soccer Embedded
        EXPORT ManweTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Per-component headers under include/manwe/<component>/.
install(DIRECTORY Yarn/includes/     DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/manwe/yarn)
install(DIRECTORY Wire/includes/     DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/manwe/wire)
install(DIRECTORY Soccer/includes/   DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/manwe/soccer)
install(DIRECTORY Embedded/includes/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/manwe/embedded)

# ManweTargets.cmake: usable by find_package(Manwe) downstream.
install(EXPORT ManweTargets
        NAMESPACE Manwe::
        FILE ManweTargets.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Manwe)

# ManweConfig.cmake + ManweConfigVersion.cmake for find_package().
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/ManweConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

# What the installed config must locate on the consumer's machine. Only the directories
# the libraries were found in are recorded, as search hints; the consumer's config
# resolves the libraries itself (see cmake/ManweConfig.cmake.in).
set(MANWE_HAS_TLS 0)
set(MANWE_HAS_NGHTTP2 0)
set(MANWE_HAS_URING 0)
set(MANWE_LIB_HINTS "")
if (LIBTLS_INCLUDE_DIR AND LIBTLS_LIBRARY AND LIBSSL_LIBRARY AND LIBCRYPTO_LIBRARY)
    set(MANWE_HAS_TLS 1)
    foreach (_lib ${LIBTLS_LIBRARY} ${LIBSSL_LIBRARY} ${LIBCRYPTO_LIBRARY})
        get_filename_component(_dir "${_lib}" DIRECTORY)
        list(APPEND MANWE_LIB_HINTS "${_dir}")
    endforeach ()
endif ()
if (SOCCER_USE_NGHTTP2)
    set(MANWE_HAS_NGHTTP2 1)
    get_filename_component(_dir "${LIBNGHTTP2_LIBRARY}" DIRECTORY)
    list(APPEND MANWE_LIB_HINTS "${_dir}")
endif ()
if (YARN_USE_IO_URING OR YARN_IO_URING_SQPOLL)
    set(MANWE_HAS_URING 1)
    get_filename_component(_dir "${LIBURING_LIBRARY}" DIRECTORY)
    list(APPEND MANWE_LIB_HINTS "${_dir}")
endif ()
list(REMOVE_DUPLICATES MANWE_LIB_HINTS)

configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ManweConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/ManweConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Manwe)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ManweConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ManweConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Manwe)
