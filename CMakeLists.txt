# CICO: Cypher In, Cypher Out.
#
# Builds the rewrite engine and the Cypher compiler it is based on entirely
# from this repository (plus pinned third-party tarballs, see
# cmake/Dependencies.cmake).
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   # -> build/bin/cico_engine
cmake_minimum_required(VERSION 3.20)
project(CICO VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(CICO_OPTIMIZE_FOR_HOST "Compile with -march=native (NeuG's default; not portable)" OFF)
option(CICO_BUILD_NEUG_EXEC
  "Also build tools/neug_exec (test-only executor on a NeuG database; links an external NeuG build)" OFF)
set(CICO_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps" CACHE PATH
  "Directory with pre-downloaded dependency tarballs (tools/fetch_deps.sh); used instead of the network")

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(WARNING "CICO is developed and tested on Linux; other platforms are untested")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
  message(FATAL_ERROR "GCC >= 11 is required (C++20)")
endif()

include(cmake/Dependencies.cmake)
add_subdirectory(third_party)
add_subdirectory(compiler)
add_subdirectory(engine)
if(CICO_BUILD_NEUG_EXEC)
  add_subdirectory(tools/neug_exec)
endif()
