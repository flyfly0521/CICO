# Small libraries vendored from NeuG's third_party/ (see each LICENSE) and
# compiled from source.  The CMakeLists of the four compiled libraries are
# NeuG's own; install_neug_target is a no-op here.
macro(install_neug_target target)
endmacro()
set(AUTO_UPDATE_GRAMMAR OFF)
set(RE2_BUILD_TESTING OFF)

add_subdirectory(antlr4_runtime)
add_subdirectory(antlr4_cypher)
add_subdirectory(re2)
add_subdirectory(utf8proc)
foreach(target antlr4_runtime antlr4_cypher re2 utf8proc)
  target_compile_options(${target} PRIVATE -w)
endforeach()
# NeuG adds these through directory-wide include_directories().
target_include_directories(re2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(antlr4_cypher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/antlr4_cypher/include)
target_include_directories(utf8proc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/utf8proc/include)

# Header-only libraries plus the include roots NeuG uses for vendored code
# (e.g. "re2/include/re2.h", "antlr4_cypher/include/cypher_parser.h",
# "pcg/pcg_random.hpp").
add_library(vendored_headers INTERFACE)
target_include_directories(vendored_headers SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/rapidjson/include
  ${CMAKE_CURRENT_SOURCE_DIR}/date/include
  ${CMAKE_CURRENT_SOURCE_DIR}/expected
  ${CMAKE_CURRENT_SOURCE_DIR}/flat_hash_map
  ${CMAKE_CURRENT_SOURCE_DIR}/fast_float/include
  ${CMAKE_CURRENT_SOURCE_DIR}/parallel_hashmap
  ${CMAKE_CURRENT_SOURCE_DIR}/antlr4_cypher/include
  ${CMAKE_CURRENT_SOURCE_DIR}/antlr4_runtime/src
  ${CMAKE_CURRENT_SOURCE_DIR}/re2/include
  ${CMAKE_CURRENT_SOURCE_DIR}/utf8proc/include)
target_compile_definitions(vendored_headers INTERFACE
  RAPIDJSON_HAS_STDSTRING=1 RAPIDJSON_HAS_CXX11=1 RAPIDJSON_HAS_CXX11_RANGE_FOR=1
  RAPIDJSON_HAS_CXX11_RVALUE_REFS=1)
