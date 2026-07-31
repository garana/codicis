# codicis_config: configuration from a file and CLI flags (CLI wins).

add_library(codicis_config STATIC
  option.cc
  file_parser.cc
  cli_parser.cc
  config.cc)

target_include_directories(codicis_config PUBLIC
  ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(codicis_config
  PUBLIC codicis_util
  PRIVATE codicis_warnings)

add_library(codicis::config ALIAS codicis_config)
