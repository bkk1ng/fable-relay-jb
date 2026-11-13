cmake_minimum_required(VERSION 3.17)
project(mathlib VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ── Compile warnings ──────────────────────────────────────────────────────────
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
    -Wundef
    -fno-common
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Release>:-O2>
)

# ── Library ───────────────────────────────────────────────────────────────────
add_library(mathlib src/mathlib.c)

target_include_directories(mathlib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(mathlib PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    PUBLIC_HEADER include/mathlib.h
)

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_mathlib tests/test_mathlib.c)
target_link_libraries(test_mathlib PRIVATE mathlib)

add_test(NAME mathlib_tests COMMAND test_mathlib)

# ── Install rules ─────────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS mathlib
    EXPORT  mathlibTargets
    LIBRARY       DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE       DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME       DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT mathlibTargets
    FILE      mathlibTargets.cmake
    NAMESPACE mathlib::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mathlib
)

include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/mathlibConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mathlibConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/mathlibConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mathlib
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/mathlibConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/mathlibConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mathlib
)
