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
