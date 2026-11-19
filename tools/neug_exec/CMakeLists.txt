# neug_exec: test-only executor that runs Cypher on a NeuG *database*, used by
# tests/e2e_neug.py to compare original and rewritten queries when NeuG is the
# target system.  Like a Kuzu or Neo4j server, the target database is external,
# so this links an existing NeuG build:
#   cmake -B build -DCICO_BUILD_NEUG_EXEC=ON -DNEUG_SOURCE_DIR=/path/to/IncGQ
set(NEUG_SOURCE_DIR "" CACHE PATH "NeuG (IncGQ) repository root")
set(NEUG_BUILD_DIR "${NEUG_SOURCE_DIR}/build" CACHE PATH "Existing NeuG build directory")
if(NOT EXISTS "${NEUG_BUILD_DIR}/src/libneug.so")
  message(FATAL_ERROR "CICO_BUILD_NEUG_EXEC: libneug.so not found under ${NEUG_BUILD_DIR}/src")
endif()
set(_deps ${NEUG_BUILD_DIR}/_deps/arrow-build)
add_executable(neug_exec neug_exec.cc)
target_compile_definitions(neug_exec PRIVATE
  BUILD_HTTP_SERVER CPPHTTPLIB_OPENSSL_SUPPORT ENABLE_CHRONO_YMD ENABLE_HTTP_PROTOCOL
  GFLAGS_IS_A_DLL=0 GLOG_CUSTOM_PREFIX_SUPPORT NEUG_CMAKE_VERSION="0.1.0" NEUG_VERSION="0.1.0"
  RAPIDJSON_HAS_STDSTRING=1 __64BIT__)
target_compile_options(neug_exec PRIVATE -fopenmp -w)
target_include_directories(neug_exec PRIVATE
  ${NEUG_BUILD_DIR}/include ${NEUG_SOURCE_DIR}/include
  ${NEUG_BUILD_DIR}/src/utils ${NEUG_BUILD_DIR}/src/compiler ${NEUG_SOURCE_DIR}/third_party
  ${NEUG_SOURCE_DIR}/third_party/glog/src ${NEUG_BUILD_DIR}/third_party/glog
  ${NEUG_SOURCE_DIR}/third_party/yaml-cpp/include ${NEUG_SOURCE_DIR}/third_party/protobuf/src
  ${NEUG_SOURCE_DIR}/third_party/abseil-cpp ${NEUG_BUILD_DIR}/_deps/arrow-src/cpp/src
  ${NEUG_BUILD_DIR}/_deps/arrow-build/src ${NEUG_SOURCE_DIR}/third_party/rapidjson/include
  ${NEUG_SOURCE_DIR}/third_party/HowardHinnant/date/include
  ${NEUG_SOURCE_DIR}/third_party/parallel-hashmap ${NEUG_SOURCE_DIR}/third_party/expected
  ${NEUG_SOURCE_DIR}/third_party/fast_float/include ${NEUG_SOURCE_DIR}/third_party/flat_hash_map)
target_link_options(neug_exec PRIVATE -fopenmp
  "-Wl,-rpath,${NEUG_BUILD_DIR}/src:${NEUG_BUILD_DIR}/third_party/mimalloc")
target_link_libraries(neug_exec PRIVATE ${NEUG_BUILD_DIR}/src/libneug.so OpenSSL::SSL OpenSSL::Crypto
  ${_deps}/release/libarrow_dataset.a ${_deps}/release/libarrow_acero.a ${_deps}/release/libarrow.a
  ${_deps}/snappy_ep/src/snappy_ep-install/lib/libsnappy.a
  ${_deps}/zlib_ep/src/zlib_ep-install/lib/libz.a ${_deps}/zstd_ep-install/lib/libzstd.a
  ${NEUG_BUILD_DIR}/third_party/glog/libglog.a ${NEUG_BUILD_DIR}/third_party/gflags/libgflags_nothreads.a
  ${NEUG_BUILD_DIR}/third_party/mimalloc/libmimalloc.so.2.0 ${CMAKE_DL_LIBS} rt Threads::Threads)
find_library(_unwind unwind)
if(_unwind)
  target_link_libraries(neug_exec PRIVATE ${_unwind})
endif()
