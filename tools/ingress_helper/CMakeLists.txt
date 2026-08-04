# The reference client-request ingress helper child process.

add_executable(codicis_ingress_helper main.cc)

target_link_libraries(codicis_ingress_helper
  PRIVATE codicis_ipc codicis_warnings)
