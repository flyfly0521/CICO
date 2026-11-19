# cypher_compiler: the Cypher compiler the rewrite engine is built on
# (parser, binder, index rewriters, planner, gopt).  The code originates from
# NeuG; see README.md.  It is an object library so that every compiler object
# is linked into the engine.
include(CheckCXXSourceCompiles)

# Translation units: upstream/sources.txt (paths as in the NeuG tree).
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/upstream/sources.txt _listed REGEX "^[^#]")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/upstream/sources.txt)
set(_sources ${CMAKE_CURRENT_SOURCE_DIR}/stubs/execution_stubs.cc)
foreach(entry IN LISTS _listed)
  if(entry MATCHES "^@build/src/utils/neug/generated/proto/")
    continue()                                   # generated from proto/ below
  elseif(entry STREQUAL "@build/src/compiler/extension/codegen/generated_extension_loader.cpp")
    list(APPEND _sources ${CMAKE_CURRENT_SOURCE_DIR}/generated/src/generated_extension_loader.cpp)
  elseif(entry MATCHES "^@build/")
    message(FATAL_ERROR "Unhandled generated source in upstream/sources.txt: ${entry}")
  else()
    list(APPEND _sources ${CMAKE_CURRENT_SOURCE_DIR}/${entry})
  endif()
endforeach()

# Plan / response protos, generated with the protoc built from source.
set(_proto_root ${CMAKE_CURRENT_BINARY_DIR}/proto)
set(_proto_srcs)
file(GLOB _protos ${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto)
foreach(proto IN LISTS _protos)
  get_filename_component(name ${proto} NAME_WE)
  if(name STREQUAL "response")
    set(out ${_proto_root}/neug/generated/proto/response)
  else()
    set(out ${_proto_root}/neug/generated/proto/plan)
  endif()
  add_custom_command(
    OUTPUT ${out}/${name}.pb.cc ${out}/${name}.pb.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${out}
    COMMAND $<TARGET_FILE:protobuf::protoc> -I ${CMAKE_CURRENT_SOURCE_DIR}/proto
            -I ${CICO_PROTOBUF_INCLUDE_DIR} --cpp_out=${out} ${proto}
    DEPENDS ${proto} protobuf::protoc
    COMMENT "protoc ${name}.proto"
    VERBATIM)
  list(APPEND _proto_srcs ${out}/${name}.pb.cc)
endforeach()

check_cxx_source_compiles("
#include <chrono>
int main() { std::chrono::year_month_day ymd{std::chrono::year{2024}, std::chrono::month{6}, std::chrono::day{9}}; return 0; }
" CICO_HAVE_CHRONO_YMD)

add_library(cypher_compiler OBJECT ${_sources} ${_proto_srcs})
target_include_directories(cypher_compiler PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/generated/include
  ${_proto_root}
  ${_proto_root}/neug/generated/proto/plan
  ${_proto_root}/neug/generated/proto/response)
target_compile_definitions(cypher_compiler PUBLIC
  ANTLR4CPP_STATIC NEUG_VERSION="0.1.0" NEUG_CMAKE_VERSION="0.1.0"
  $<$<BOOL:${CICO_HAVE_CHRONO_YMD}>:ENABLE_CHRONO_YMD>
  $<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,__64BIT__,__32BIT__>)
# The compiler sources are built with -w, as upstream does.
target_compile_options(cypher_compiler PRIVATE -w)
if(CICO_OPTIMIZE_FOR_HOST)
  target_compile_options(cypher_compiler PUBLIC -march=native)
endif()
target_link_libraries(cypher_compiler PUBLIC
  vendored_headers antlr4_cypher antlr4_runtime re2 utf8proc
  protobuf::libprotobuf yaml-cpp glog::glog arrow_libs
  OpenSSL::Crypto Threads::Threads ${CMAKE_DL_LIBS})
