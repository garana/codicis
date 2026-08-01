# The reference storage helper child process.

add_executable(codicis_storage_helper main.cc)

target_link_libraries(codicis_storage_helper
  PRIVATE codicis_ipc codicis_warnings)
