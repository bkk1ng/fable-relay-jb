cmake_minimum_required(VERSION 3.17)
project(mathlib VERSION 1.0.0 LANGUAGES C)

# ── Language standard ────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ── Compile warnings ─────────────────────────────────────────────────────────
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wdouble-promotion
        -Wmissing-prototypes
    )
elseif(MSVC)
    add_compile_options(/W4 /WX)
endif()

# ── Library target ───────────────────────────────────────────────────────────
add_library(mathlib STATIC src/mathlib.c)

target_include_directories(mathlib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# ── Tests ────────────────────────────────────────────────────────────────────
option(MATHLIB_BUILD_TESTS "Build the mathlib test suite" ON)

if(MATHLIB_BUILD_TESTS)
    enable_testing()

    add_executable(test_mathlib tests/test_mathlib.c)
    target_link_libraries(test_mathlib PRIVATE mathlib)

    add_test(NAME mathlib_suite COMMAND test_mathlib)
    set_tests_properties(mathlib_suite PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
endif()

# ── Install rules ─────────────────────────────────────────────────────────────
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS mathlib
    EXPORT mathlibTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES include/mathlib.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT mathlibTargets
    FILE mathlibTargets.cmake
    NAMESPACE mathlib::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mathlib
)

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
