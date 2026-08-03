# Unit tests. One binary per subsystem; each registered with ctest.

# codicis_test_support: shared test settings (warnings + Catch2 main).
add_library(codicis_test_support INTERFACE)
target_link_libraries(codicis_test_support INTERFACE
  Catch2::Catch2WithMain codicis_warnings)

# Helper: define a Catch2 test binary and register it with ctest.
function(codicis_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE codicis_test_support)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

codicis_add_test(test_util
  unit/test_util.cc)
target_link_libraries(test_util PRIVATE codicis_util)

codicis_add_test(test_config
  unit/test_config.cc)
target_link_libraries(test_config PRIVATE codicis_config)

codicis_add_test(test_event
  unit/test_event.cc)
target_link_libraries(test_event PRIVATE codicis_event)
target_include_directories(test_event PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

codicis_add_test(test_http_parser
  unit/test_http_parser.cc)
target_link_libraries(test_http_parser PRIVATE codicis_net)

codicis_add_test(test_http_server
  unit/test_http_server.cc)
target_link_libraries(test_http_server PRIVATE codicis_net)

codicis_add_test(test_ws
  unit/test_ws.cc)
target_link_libraries(test_ws PRIVATE codicis_net)

codicis_add_test(test_net_epoll
  unit/test_net_epoll.cc)
target_link_libraries(test_net_epoll PRIVATE codicis_net)

codicis_add_test(test_order_book
  unit/test_order_book.cc)
target_link_libraries(test_order_book PRIVATE codicis_core)

codicis_add_test(test_matrix
  unit/test_matrix.cc)
target_link_libraries(test_matrix PRIVATE codicis_core)

codicis_add_test(test_matching_engine
  unit/test_matching_engine.cc)
target_link_libraries(test_matching_engine PRIVATE codicis_core)

codicis_add_test(test_ipc
  unit/test_ipc.cc)
target_link_libraries(test_ipc PRIVATE codicis_ipc)
# Give the spawn test the path to the freshly built reference helper.
add_dependencies(test_ipc codicis_storage_helper)
target_compile_definitions(test_ipc PRIVATE
  CODICIS_STORAGE_HELPER_PATH="$<TARGET_FILE:codicis_storage_helper>")

codicis_add_test(test_token_cache
  unit/test_token_cache.cc)
target_link_libraries(test_token_cache PRIVATE codicis_auth)

codicis_add_test(test_auth
  unit/test_auth.cc)
target_link_libraries(test_auth PRIVATE codicis_auth)
add_dependencies(test_auth codicis_auth_helper)
target_compile_definitions(test_auth PRIVATE
  CODICIS_AUTH_HELPER_PATH="$<TARGET_FILE:codicis_auth_helper>")

codicis_add_test(test_engine
  unit/test_engine.cc)
target_link_libraries(test_engine PRIVATE codicis_engine)

codicis_add_test(test_app
  unit/test_app.cc)
target_link_libraries(test_app PRIVATE codicis_app)
add_dependencies(test_app codicis_storage_helper codicis_auth_helper)
target_compile_definitions(test_app PRIVATE
  CODICIS_STORAGE_HELPER_PATH="$<TARGET_FILE:codicis_storage_helper>"
  CODICIS_AUTH_HELPER_PATH="$<TARGET_FILE:codicis_auth_helper>")
