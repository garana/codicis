# The reference auth helper child process.

add_executable(codicis_auth_helper main.cc)

target_link_libraries(codicis_auth_helper
  PRIVATE codicis_ipc codicis_warnings)
