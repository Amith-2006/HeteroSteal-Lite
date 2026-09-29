cmake_minimum_required(VERSION 3.22)
project(heterosteal
    VERSION 0.2.0
    DESCRIPTION "Research runtime studying io_uring callback scheduling and work-stealing heterogeneity"
    LANGUAGES CXX
)

# -----------------------------------------------------------------------------
# C++ Standard Configuration
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Common compiler warnings
add_compile_options(-Wall -Wextra -Wpedantic)

# -----------------------------------------------------------------------------
# Build Modes & Sanitizer Configuration (per spec §8.6, §11.2)
# -----------------------------------------------------------------------------
option(HS_ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(HS_ENABLE_ASAN_UBSAN "Build with Address/UBSanitizer" OFF)

# Enforce mutual exclusion between sanitizers
if(HS_ENABLE_TSAN AND HS_ENABLE_ASAN_UBSAN)
    message(FATAL_ERROR "HS_ENABLE_TSAN and HS_ENABLE_ASAN_UBSAN are mutually exclusive.")
endif()

# Sanitizers must be excluded in Release builds (per spec §8.6)
string(TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_UPPER)
if(CMAKE_BUILD_TYPE_UPPER STREQUAL "RELEASE")
    if(HS_ENABLE_TSAN OR HS_ENABLE_ASAN_UBSAN)
        message(FATAL_ERROR "Sanitizers must not be enabled in Release builds (per spec §8.6).")
    endif()
endif()

if(HS_ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled: flags -fsanitize=thread -g -O1")
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
    add_compile_definitions(HS_TSAN_BUILD=1)
endif()

if(HS_ENABLE_ASAN_UBSAN)
    message(STATUS "Address/Undefined Sanitizer enabled: flags -fsanitize=address,undefined -g -O1")
    add_compile_options(-fsanitize=address,undefined -g -O1)
    add_link_options(-fsanitize=address,undefined)
endif()

# -----------------------------------------------------------------------------
# Dependency Resolution
# -----------------------------------------------------------------------------
find_package(Threads REQUIRED)

# Check for liburing (spec §11.1, §11.2)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBURING QUIET IMPORTED_TARGET liburing)
endif()

if(NOT TARGET PkgConfig::LIBURING)
    find_package(Liburing QUIET)
endif()

if(TARGET PkgConfig::LIBURING OR TARGET Liburing::uring)
    if(NOT TARGET PkgConfig::LIBURING AND TARGET Liburing::uring)
        add_library(PkgConfig::LIBURING ALIAS Liburing::uring)
    endif()
    message(STATUS "liburing detected (linking to hs_io per spec §11.2)")
    set(HS_HAS_LIBURING ON)
else()
    message(STATUS "liburing not detected on host platform")
    set(HS_HAS_LIBURING OFF)
endif()

# -----------------------------------------------------------------------------
# Public Include Path
# -----------------------------------------------------------------------------
include_directories(include)

# -----------------------------------------------------------------------------
# Core Library: hs_core (spec §2.2, §11.2)
# Modules: WorkStealingDeque (M1), TaskPool & Correlation (M2)
# -----------------------------------------------------------------------------
add_library(hs_core
    src/deque.cpp
    src/object_pool.cpp
)
target_include_directories(hs_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(hs_core PUBLIC Threads::Threads)

# -----------------------------------------------------------------------------
# IO Engine Library: hs_io (spec §2.2, §4.1, §11.2)
# Modules: IoUringEngine, Completion Poller thread, error classification
# -----------------------------------------------------------------------------
if(HS_HAS_LIBURING)
    add_library(hs_io src/iouring_engine.cpp)
    target_include_directories(hs_io PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_link_libraries(hs_io PUBLIC hs_core PkgConfig::LIBURING Threads::Threads)
endif()

# -----------------------------------------------------------------------------
# Target: hs_harness (Driver Binary)
# -----------------------------------------------------------------------------
add_executable(hs_harness bench/harness_main.cpp)
target_include_directories(hs_harness PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(hs_harness PRIVATE hs_core Threads::Threads)
if(HS_HAS_LIBURING)
    target_link_libraries(hs_harness PRIVATE hs_io)
    target_compile_definitions(hs_harness PRIVATE HS_HAS_IOURING=1)
endif()

# -----------------------------------------------------------------------------
# Testing Subsystem (per spec §11.2, §11.3)
# -----------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# HeteroSteal Lite synthetic work-stealing benchmark
add_executable(hs_ws_bench bench/ws_bench.cpp)
target_include_directories(hs_ws_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(hs_ws_bench PRIVATE hs_core Threads::Threads)
