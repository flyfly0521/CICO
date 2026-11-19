# cico_engine: compile-only JSON-lines rewrite service (see rewrite_engine.cc).
add_executable(cico_engine rewrite_engine.cc)
target_link_libraries(cico_engine PRIVATE cypher_compiler)
install(TARGETS cico_engine RUNTIME DESTINATION bin)
