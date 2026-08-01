# codicis_ipc: pluggable child-process helper framework and storage client.

add_library(codicis_ipc STATIC
  helper_codec.cc
  helper_client.cc
  storage_client.cc)

target_include_directories(codicis_ipc PUBLIC
  ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(codicis_ipc
  PUBLIC codicis_util codicis_event
  PRIVATE codicis_warnings)

add_library(codicis::ipc ALIAS codicis_ipc)
