cmake_minimum_required(VERSION 3.10)

project(mathlib
    VERSION 1.0.0
    DESCRIPTION "Small math helper library"
    LANGUAGES C
)

# --- Build settings ---------------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Warning flags for the compilers we actually support.
set(MATHLIB_WARNING_FLAGS_C "")
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(MATHLIB_WARNING_FLAGS_C
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wdouble-promotion)
elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    set(MATHLIB_WARNING_FLAGS_C /W4 /permissive-)
endif()

# --- Library target ---------------------------------------------------------

add_library(mathlib src/mathlib.c)

target_include_directories(mathlib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_options(mathlib PRIVATE ${MATHLIB_WARNING_FLAGS_C})
set_target_properties(mathlib PROPERTIES
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
)

# --- Tests ------------------------------------------------------------------

enable_testing()

add_executable(test_mathlib tests/test_mathlib.c)
target_link_libraries(test_mathlib PRIVATE mathlib)
target_compile_options(test_mathlib PRIVATE ${MATHLIB_WARNING_FLAGS_C})

add_test(NAME test_mathlib COMMAND test_mathlib)

# --- Install rules ----------------------------------------------------------

include(GNUInstallDirs)

install(TARGETS mathlib
    EXPORT mathlibTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

# Generate and install an import file so consumers can `find_package(mathlib)`.
install(EXPORT mathlibTargets
    FILE mathlibTargets.cmake
    NAMESPACE mathlib::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mathlib
)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/mathlibConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/mathlibConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mathlib
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/mathlibConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/mathlibConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/mathlibConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mathlib
)
