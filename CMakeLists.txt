# LOTI — native build for the shared protocol core and (later) the product node.
#
# This CMake build is INDEPENDENT of the OMNeT++ simulation build (opp_makemake +
# the generated root Makefile). A developer runs `make` for the simulation and
# `cmake` for loti-core / the product; the two never touch the same files.
#
# The one rule this build exists to enforce is the dependency rule from
# documentation/architecture.md: `loti_core` compiles with NO OMNeT++/INET/OS
# dependency. Adapters and apps (added in later MVP stages) depend on loti_core,
# never the reverse.
cmake_minimum_required(VERSION 3.20)
project(loti LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# ----------------------------------------------------------------------------
# loti-core — the pure protocol library.
# Currently a placeholder (Stage 0). Domain model, hashing, DAG, discovery, and
# validation land in Stages 1–2; sources are added to this target as they arrive.
# ----------------------------------------------------------------------------
add_library(loti_core STATIC
  core/loti_core.cpp
  core/hash/hashing.cpp
)
target_include_directories(loti_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core)

# ----------------------------------------------------------------------------
# Tests (doctest, vendored under test/doctest).
# ----------------------------------------------------------------------------
enable_testing()
add_subdirectory(test/core)
