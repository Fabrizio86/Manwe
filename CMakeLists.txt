cmake_minimum_required(VERSION 3.19)
project(Manwe VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Build outputs go to bin/ (tracked) ----------------------------------
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")

# ---- LIB_PATH propagated to subdirectory CMakeLists for include lookups --
set(LIB_PATH ${PROJECT_SOURCE_DIR})

# ---- Components ----------------------------------------------------------
add_subdirectory(Yarn)
add_subdirectory(Wire)
add_subdirectory(Soccer)
add_subdirectory(Embedded)
add_subdirectory(YarnTests)
add_subdirectory(examples)
add_subdirectory(benchmarks)

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
        COMPATIBILITY SameMinorVersion)

configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ManweConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/ManweConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Manwe)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ManweConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ManweConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Manwe)
