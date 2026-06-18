cmake_minimum_required(VERSION 3.16)
project(uxnet LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Build type: default to Release (single-config generators only).
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)
endif()

# ---------------------------------------------------------------------------
# Language + global flags.
# -O2 (not -O3) is deliberate: predictable codegen for latency work.
# These are appended after build-type flags, so -O2 wins over Release's -O3.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-O2 -march=native -pthread -Wall -Wextra -Wno-unused-parameter)
add_link_options(-pthread)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(UXNET_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# uxnet_lib — static library of everything under src/.
# Falls back to a header-only INTERFACE target while src/ is empty so the
# rest of the build (tests, benchmarks) can link it uniformly either way.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE UXNET_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

# Standalone src/ apps (have their own main) — build as executables, not lib.
set(UXNET_SRC_APPS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/capture.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/multiqueue_capture.cpp)
list(REMOVE_ITEM UXNET_SOURCES ${UXNET_SRC_APPS})

if(UXNET_SOURCES)
  add_library(uxnet_lib STATIC ${UXNET_SOURCES})
  target_include_directories(uxnet_lib PUBLIC ${UXNET_INCLUDE_DIR})
  target_link_libraries(uxnet_lib PUBLIC Threads::Threads)
else()
  message(STATUS "uxnet: no sources in src/ yet — uxnet_lib is header-only (INTERFACE)")
  add_library(uxnet_lib INTERFACE)
  target_include_directories(uxnet_lib INTERFACE ${UXNET_INCLUDE_DIR})
  target_link_libraries(uxnet_lib INTERFACE Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# GoogleTest — use an installed copy if present, otherwise FetchContent it at
# configure time. Only pulled when there are actual tests to build.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE UXNET_TEST_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)

if(UXNET_TEST_SOURCES)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    message(STATUS "uxnet: GoogleTest not found — fetching via FetchContent")
    include(FetchContent)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    # Keep MSVC-style CRT settings from leaking on other platforms.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  enable_testing()
  include(GoogleTest)

  add_executable(uxnet_tests ${UXNET_TEST_SOURCES})
  target_link_libraries(uxnet_tests PRIVATE uxnet_lib GTest::gtest_main)
  target_include_directories(uxnet_tests PRIVATE ${UXNET_INCLUDE_DIR})
  gtest_discover_tests(uxnet_tests)
else()
  message(STATUS "uxnet: no tests in tests/ yet — skipping uxnet_tests target")
endif()

# ---------------------------------------------------------------------------
# Benchmarks — one standalone executable per benchmarks/*.cpp file.
# The aggregate `bench` target builds them all: `cmake --build build --target bench`.
# ---------------------------------------------------------------------------
add_custom_target(bench COMMENT "Build all uxnet benchmark binaries")

file(GLOB UXNET_BENCH_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.cpp)

foreach(bench_src ${UXNET_BENCH_SOURCES})
  get_filename_component(bench_name ${bench_src} NAME_WE)
  add_executable(${bench_name} ${bench_src})
  target_link_libraries(${bench_name} PRIVATE uxnet_lib)
  target_include_directories(${bench_name} PRIVATE ${UXNET_INCLUDE_DIR})
  set_target_properties(${bench_name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/benchmarks)
  add_dependencies(bench ${bench_name})
endforeach()

# ---------------------------------------------------------------------------
# Standalone src/ apps and tools/ utilities — one executable each.
# ---------------------------------------------------------------------------
foreach(app_src ${UXNET_SRC_APPS})
  get_filename_component(app_name ${app_src} NAME_WE)
  add_executable(${app_name} ${app_src})
  target_link_libraries(${app_name} PRIVATE uxnet_lib)
  target_include_directories(${app_name} PRIVATE ${UXNET_INCLUDE_DIR})
endforeach()

file(GLOB UXNET_TOOL_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/tools/*.cpp)

foreach(tool_src ${UXNET_TOOL_SOURCES})
  get_filename_component(tool_name ${tool_src} NAME_WE)
  add_executable(${tool_name} ${tool_src})
  target_link_libraries(${tool_name} PRIVATE uxnet_lib)
  target_include_directories(${tool_name} PRIVATE ${UXNET_INCLUDE_DIR})
  set_target_properties(${tool_name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)
endforeach()
