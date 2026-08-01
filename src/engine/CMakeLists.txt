# codicis_engine: report-before-place sequencing over the matching engine.

add_library(codicis_engine STATIC
  trading_engine.cc)

target_include_directories(codicis_engine PUBLIC
  ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(codicis_engine
  PUBLIC codicis_core codicis_ipc
  PRIVATE codicis_warnings)

add_library(codicis::engine ALIAS codicis_engine)
