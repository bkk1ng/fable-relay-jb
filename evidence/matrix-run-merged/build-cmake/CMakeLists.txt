cmake_minimum_required(VERSION 3.16)
project(mathlib VERSION 1.0.0 LANGUAGES C)

# ── Language standard ──────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ── Compile warnings (project-wide) ───────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Wdouble-promotion
        -Wundef
    )
endif()

# ── Library target ─────────────────────────────────────────────────────────────
add_library(mathlib STATIC src/mathlib.c)

target_include_directories(mathlib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# ── Tests target ───────────────────────────────────────────────────────────────
enable_testing()

add_executable(mathlib_tests tests/test_mathlib.c)
target_link_libraries(mathlib_tests PRIVATE mathlib)

add_test(
    NAME    mathlib_unit
    COMMAND mathlib_tests
)

# ── Install rules ──────────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(
    TARGETS mathlib
    EXPORT  mathlibTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(
    FILES include/mathlib.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    EXPORT    mathlibTargets
    FILE      mathlibTargets.cmake
    NAMESPACE mathlib::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mathlib
)
