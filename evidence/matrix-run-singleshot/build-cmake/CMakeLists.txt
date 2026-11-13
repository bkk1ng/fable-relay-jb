cmake_minimum_required(VERSION 3.15)

project(mathlib
    VERSION 1.0.0
    LANGUAGES C
)

include(GNUInstallDirs)

# --- C11 standard, no compiler extensions ---
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Default to Release when no build type is given
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- Library target ---
add_library(mathlib src/mathlib.c)
target_include_directories(mathlib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
set_target_properties(mathlib PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON
)

# --- Compiler warnings (treated as errors) ---
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(mathlib PRIVATE -Wall -Wextra -Wpedantic -Werror)
elseif(MSVC)
    target_compile_options(mathlib PRIVATE /W4 /WX)
endif()

# --- Tests ---
# Expects test/test_mathlib.c to exist alongside the project root.
enable_testing()

add_executable(mathlib_test test/test_mathlib.c)
target_link_libraries(mathlib_test PRIVATE mathlib)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(mathlib_test PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
    target_compile_options(mathlib_test PRIVATE /W4)
endif()

add_test(NAME mathlib_test COMMAND mathlib_test)

# --- Install rules ---
install(TARGETS mathlib
    EXPORT mathlibTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT mathlibTargets
    FILE      mathlibTargets.cmake
    NAMESPACE mathlib::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mathlib
)
