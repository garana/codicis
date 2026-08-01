# codicis_util: foundational utilities (no dependencies on other subsystems).

add_library(codicis_util STATIC
  logging.cc
  clock.cc
  base64.cc
  sha1.cc)

target_include_directories(codicis_util PUBLIC
  ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(codicis_util PRIVATE codicis_warnings)

add_library(codicis::util ALIAS codicis_util)
