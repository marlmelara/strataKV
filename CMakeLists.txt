cmake_minimum_required(VERSION 3.24)

project(stratakv
        VERSION 0.1.0
        DESCRIPTION "Crash-safe LSM-tree key-value storage engine"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language level. C++20 is required, not merely preferred: the engine uses
# std::span, designated initializers, and constexpr std::array table building.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# Always emit compile_commands.json so clang-tidy and editors can find flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(STRATAKV_BUILD_TESTS "Build the C++ GoogleTest suites" ON)
option(STRATAKV_BUILD_PYTHON "Build the pybind11 extension module" OFF)
option(STRATAKV_WERROR "Treat compiler warnings as errors" OFF)

# ---------------------------------------------------------------------------
# The engine library. Every phase adds sources here; P0 ships Status, Options,
# Env, CRC32C, and the version string.
# ---------------------------------------------------------------------------
add_library(stratakv
            src/arena.cc
            src/bloom.cc
            src/crash_point.cc
            src/crc32c.cc
            src/db_impl.cc
            src/env.cc
            src/internal_key.cc
            src/memtable.cc
            src/sstable_builder.cc
            src/sstable_reader.cc
            src/status.cc
            src/version.cc
            src/wal_reader.cc
            src/wal_writer.cc
            src/write_batch.cc)

target_include_directories(stratakv
                           PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(stratakv PRIVATE STRATAKV_VERSION="${PROJECT_VERSION}")

target_compile_options(stratakv PRIVATE -Wall -Wextra -Wpedantic -Wshadow
                                        -Wconversion -Wsign-conversion)
if(STRATAKV_WERROR)
  target_compile_options(stratakv PRIVATE -Werror)
endif()

# The static library is linked into a shared Python extension, so its objects
# must be position independent.
set_target_properties(stratakv PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# C++ tests. GoogleTest is fetched rather than vendored so there is no
# submodule to keep in sync; it is skipped entirely for the Python build,
# which must not need the network.
# ---------------------------------------------------------------------------
if(STRATAKV_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_subdirectory(tests/cpp)
endif()

# ---------------------------------------------------------------------------
# Python extension, built by scikit-build-core during `pip install .`.
# ---------------------------------------------------------------------------
if(STRATAKV_BUILD_PYTHON)
  find_package(pybind11 CONFIG REQUIRED)
  pybind11_add_module(_stratakv python/bindings.cc)
  target_link_libraries(_stratakv PRIVATE stratakv)
  install(TARGETS _stratakv DESTINATION stratakv)
endif()
