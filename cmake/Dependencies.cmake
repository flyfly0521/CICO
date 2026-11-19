# Third-party libraries that are too large to vendor.  Versions are the ones
# NeuG (IncGQ) builds with; every tarball is pinned by SHA-256.
#
# Offline builds: run tools/fetch_deps.sh once (or copy the tarballs into
# CICO_DEPS_DIR).  A local source tree can also be used directly:
#   -DFETCHCONTENT_SOURCE_DIR_ABSL=...  -DFETCHCONTENT_SOURCE_DIR_PROTOBUF=...
#   -DFETCHCONTENT_SOURCE_DIR_YAML_CPP=...  -DFETCHCONTENT_SOURCE_DIR_GLOG=...
#   -DCICO_ARROW_SOURCE_DIR=<apache-arrow-18.0.0 root>   (or -DCICO_USE_SYSTEM_ARROW=ON)
include(FetchContent)
include(ExternalProject)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(FETCHCONTENT_QUIET ON)

set(CICO_ABSL_VERSION 20240116.0)
set(CICO_ABSL_SHA256 338420448b140f0dfd1a1ea3c3ce71b3bc172071f24f4d9a57d59b45037da440)
set(CICO_PROTOBUF_VERSION 29.6)
set(CICO_PROTOBUF_SHA256 877bf9f880631aa31daf2c09896276985696728137fcd43cc534a28c5566d9ba)
set(CICO_YAML_CPP_VERSION 0.8.0)
set(CICO_YAML_CPP_SHA256 fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16)
set(CICO_GLOG_VERSION 0.6.0)
set(CICO_GLOG_SHA256 8a83bf982f37bb70825df71a9709fa90ea9f4447fb3c099e1d720a439d88bad6)
set(CICO_ARROW_VERSION 18.0.0)
set(CICO_ARROW_SHA256 abcf1934cd0cdddd33664e9f2d9a251d6c55239d1122ad0ed223b13a583c82a9)

option(CICO_USE_SYSTEM_ARROW "Use an installed Arrow (with Dataset and Acero) instead of building it" OFF)
set(CICO_ARROW_SOURCE_DIR "" CACHE PATH "Existing apache-arrow-${CICO_ARROW_VERSION} source tree (skips the download)")

# Prefer a pre-downloaded tarball in CICO_DEPS_DIR over the network.
function(cico_dep_url out file url)
  if(EXISTS "${CICO_DEPS_DIR}/${file}")
    set(${out} "${CICO_DEPS_DIR}/${file}" PARENT_SCOPE)
  else()
    set(${out} "${url}" PARENT_SCOPE)
  endif()
endfunction()

# Third-party projects are always static and never installed with CICO.
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)

# --- Abseil (required by protobuf) -------------------------------------------
cico_dep_url(_url abseil-cpp-${CICO_ABSL_VERSION}.tar.gz
  https://github.com/abseil/abseil-cpp/archive/refs/tags/${CICO_ABSL_VERSION}.tar.gz)
set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_BUILD_TESTING OFF)
set(ABSL_ENABLE_INSTALL OFF)
set(ABSL_USE_EXTERNAL_GOOGLETEST OFF)
FetchContent_Declare(absl URL ${_url} URL_HASH SHA256=${CICO_ABSL_SHA256})
FetchContent_MakeAvailable(absl)

# --- Protobuf (plan protos + protoc) -------------------------------------------
cico_dep_url(_url protobuf-${CICO_PROTOBUF_VERSION}.tar.gz
  https://github.com/protocolbuffers/protobuf/releases/download/v${CICO_PROTOBUF_VERSION}/protobuf-${CICO_PROTOBUF_VERSION}.tar.gz)
set(protobuf_BUILD_TESTS OFF)
set(protobuf_BUILD_CONFORMANCE OFF)
set(protobuf_BUILD_EXAMPLES OFF)
set(protobuf_BUILD_PROTOC_BINARIES ON)
set(protobuf_BUILD_LIBPROTOC ON)
set(protobuf_BUILD_SHARED_LIBS OFF)
set(protobuf_INSTALL OFF)
set(protobuf_WITH_ZLIB OFF)
set(protobuf_ABSL_PROVIDER package)   # reuse the Abseil targets above
set(utf8_range_ENABLE_INSTALL OFF)
FetchContent_Declare(protobuf URL ${_url} URL_HASH SHA256=${CICO_PROTOBUF_SHA256})
FetchContent_MakeAvailable(protobuf)
set(CICO_PROTOBUF_INCLUDE_DIR ${protobuf_SOURCE_DIR}/src)

# --- yaml-cpp -------------------------------------------------------------------
cico_dep_url(_url yaml-cpp-${CICO_YAML_CPP_VERSION}.tar.gz
  https://github.com/jbeder/yaml-cpp/archive/refs/tags/${CICO_YAML_CPP_VERSION}.tar.gz)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yaml_cpp URL ${_url} URL_HASH SHA256=${CICO_YAML_CPP_SHA256})
FetchContent_MakeAvailable(yaml_cpp)

# --- glog ---------------------------------------------------------------------
cico_dep_url(_url glog-${CICO_GLOG_VERSION}.tar.gz
  https://github.com/google/glog/archive/refs/tags/v${CICO_GLOG_VERSION}.tar.gz)
set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
set(WITH_GTEST OFF CACHE BOOL "" FORCE)
set(WITH_UNWIND OFF CACHE BOOL "" FORCE)
set(WITH_PKGCONFIG OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glog URL ${_url} URL_HASH SHA256=${CICO_GLOG_SHA256})
FetchContent_MakeAvailable(glog)

# --- OpenSSL (system; NeuG uses libcrypto's SHA) -----------------------------------
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

# --- Arrow (CSV reader / file-system types referenced by NeuG's table functions) ---
if(CICO_USE_SYSTEM_ARROW)
  find_package(Arrow ${CICO_ARROW_VERSION} CONFIG REQUIRED)
  find_package(ArrowAcero CONFIG REQUIRED)
  find_package(ArrowDataset CONFIG REQUIRED)
  add_library(arrow_libs INTERFACE)
  foreach(_c Dataset Acero)
    string(TOLOWER ${_c} _l)
    if(TARGET Arrow${_c}::arrow_${_l}_static)
      target_link_libraries(arrow_libs INTERFACE Arrow${_c}::arrow_${_l}_static)
    else()
      target_link_libraries(arrow_libs INTERFACE Arrow${_c}::arrow_${_l}_shared)
    endif()
  endforeach()
  if(TARGET Arrow::arrow_static)
    target_link_libraries(arrow_libs INTERFACE Arrow::arrow_static)
  else()
    target_link_libraries(arrow_libs INTERFACE Arrow::arrow_shared)
  endif()
else()
  set(_arrow_prefix ${CMAKE_BINARY_DIR}/_deps/arrow-install)
  if(CICO_ARROW_SOURCE_DIR)
    set(_arrow_source SOURCE_DIR ${CICO_ARROW_SOURCE_DIR})
  else()
    cico_dep_url(_url apache-arrow-${CICO_ARROW_VERSION}.tar.gz
      https://archive.apache.org/dist/arrow/arrow-${CICO_ARROW_VERSION}/apache-arrow-${CICO_ARROW_VERSION}.tar.gz)
    set(_arrow_source URL ${_url} URL_HASH SHA256=${CICO_ARROW_SHA256})
  endif()
  set(_arrow_libs
    ${_arrow_prefix}/lib/libarrow_dataset.a
    ${_arrow_prefix}/lib/libarrow_acero.a
    ${_arrow_prefix}/lib/libarrow.a)
  # Only the components NeuG's compiler references; no compression codecs, no
  # SIMD dispatch and no bundled downloads beyond Arrow itself.
  ExternalProject_Add(arrow_ep
    ${_arrow_source}
    PREFIX ${CMAKE_BINARY_DIR}/_deps/arrow
    SOURCE_SUBDIR cpp
    INSTALL_DIR ${_arrow_prefix}
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_INSTALL_LIBDIR=lib
      -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
      -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      -DARROW_BUILD_SHARED=OFF -DARROW_BUILD_STATIC=ON
      -DARROW_CSV=ON -DARROW_COMPUTE=ON -DARROW_ACERO=ON -DARROW_DATASET=ON
      -DARROW_FILESYSTEM=ON -DARROW_IPC=ON -DARROW_JSON=OFF -DARROW_PARQUET=OFF
      -DARROW_ORC=OFF -DARROW_FLIGHT=OFF -DARROW_GANDIVA=OFF -DARROW_SUBSTRAIT=OFF
      -DARROW_CUDA=OFF -DARROW_HDFS=OFF -DARROW_S3=OFF -DARROW_GCS=OFF -DARROW_AZURE=OFF
      -DARROW_WITH_UTF8PROC=OFF -DARROW_WITH_RE2=OFF -DARROW_WITH_SNAPPY=OFF
      -DARROW_WITH_ZLIB=OFF -DARROW_WITH_ZSTD=OFF -DARROW_WITH_LZ4=OFF
      -DARROW_WITH_BZ2=OFF -DARROW_WITH_BROTLI=OFF -DARROW_WITH_OPENTELEMETRY=OFF
      -DARROW_MIMALLOC=OFF -DARROW_JEMALLOC=OFF
      -DARROW_SIMD_LEVEL=NONE -DARROW_RUNTIME_SIMD_LEVEL=NONE
      -DARROW_DEPENDENCY_SOURCE=BUNDLED -DARROW_DEPENDENCY_USE_SHARED=OFF
      -DARROW_BUILD_TESTS=OFF -DARROW_BUILD_BENCHMARKS=OFF -DARROW_BUILD_EXAMPLES=OFF
      -DARROW_BUILD_UTILITIES=OFF -DARROW_BUILD_INTEGRATION=OFF -DARROW_FUZZING=OFF
      -DARROW_USE_CCACHE=OFF -DARROW_ENABLE_THREADING=ON
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    BUILD_BYPRODUCTS ${_arrow_libs}
    UPDATE_COMMAND ""
    LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON)
  file(MAKE_DIRECTORY ${_arrow_prefix}/include)
  add_library(arrow_libs INTERFACE)
  add_dependencies(arrow_libs arrow_ep)
  target_include_directories(arrow_libs SYSTEM INTERFACE ${_arrow_prefix}/include)
  target_link_libraries(arrow_libs INTERFACE ${_arrow_libs} Threads::Threads ${CMAKE_DL_LIBS})
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(arrow_libs INTERFACE rt)
  endif()
endif()
