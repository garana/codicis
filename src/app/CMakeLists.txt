# codicis_app: application wiring, and the codicis executable.

add_library(codicis_app STATIC
  options.cc
  server.cc)

target_include_directories(codicis_app PUBLIC
  ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(codicis_app
  PUBLIC codicis_config codicis_core codicis_event codicis_ipc codicis_net
  PRIVATE codicis_warnings)

add_library(codicis::app ALIAS codicis_app)

add_executable(codicis main.cc)
target_link_libraries(codicis PRIVATE codicis_app codicis_warnings)
