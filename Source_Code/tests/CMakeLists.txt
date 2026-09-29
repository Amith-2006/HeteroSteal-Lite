# tests/CMakeLists.txt
# HeteroSteal Test Suite Configuration

# Milestone M0 Smoke Test: Verify that hs_harness runs and exits cleanly
add_test(
    NAME hs_harness_smoke
    COMMAND hs_harness --version
)

# -----------------------------------------------------------------------------
# GoogleTest Dependency Resolution
# -----------------------------------------------------------------------------
find_package(GTest QUIET)

if(NOT GTest_FOUND AND NOT TARGET GTest::gtest_main)
    message(STATUS "GoogleTest not found on system; acquiring via FetchContent...")
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

if(TARGET GTest::gtest_main)
    set(HS_GTEST_LIBS GTest::gtest GTest::gtest_main)
elseif(TARGET gtest_main)
    set(HS_GTEST_LIBS gtest gtest_main)
else()
    set(HS_GTEST_LIBS ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES})
endif()

# -----------------------------------------------------------------------------
# Milestone M1 Unit Tests: WorkStealingDeque vs Sequential Reference Model
# -----------------------------------------------------------------------------
add_executable(unit_deque_test unit/deque_test.cpp)
target_include_directories(unit_deque_test PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
)
target_link_libraries(unit_deque_test PRIVATE
    hs_core
    ${HS_GTEST_LIBS}
    Threads::Threads
)
add_test(
    NAME unit_deque_test
    COMMAND unit_deque_test
)

# -----------------------------------------------------------------------------
# Milestone M1 Stress Tests: Multi-threaded Concurrent Thieves Element Conservation
# -----------------------------------------------------------------------------
add_executable(stress_deque_test stress/deque_stress.cpp)
target_include_directories(stress_deque_test PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
)
target_link_libraries(stress_deque_test PRIVATE
    hs_core
    ${HS_GTEST_LIBS}
    Threads::Threads
)
add_test(
    NAME stress_deque_test
    COMMAND stress_deque_test
)

# -----------------------------------------------------------------------------
# Milestone M2 Unit Tests: TaskPool Slab, Generational Handles & Invariants
# -----------------------------------------------------------------------------
add_executable(unit_object_pool_test unit/object_pool_test.cpp)
target_include_directories(unit_object_pool_test PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
)
target_link_libraries(unit_object_pool_test PRIVATE
    hs_core
    ${HS_GTEST_LIBS}
    Threads::Threads
)
add_test(
    NAME unit_object_pool_test
    COMMAND unit_object_pool_test
)

# -----------------------------------------------------------------------------
# Milestone M2 Integration Tests: IoUringEngine, Short Read, Cancellation & 100k Ops
# -----------------------------------------------------------------------------
if(HS_HAS_LIBURING)
    add_executable(integration_iouring_test integration/iouring_engine_test.cpp)
    target_include_directories(integration_iouring_test PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    )
    target_link_libraries(integration_iouring_test PRIVATE
        hs_io
        hs_core
        ${HS_GTEST_LIBS}
        Threads::Threads
    )
    add_test(
        NAME integration_iouring_test
        COMMAND integration_iouring_test
    )
endif()
