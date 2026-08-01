# codicis_core: order model and the matching engine (no I/O dependencies).

add_library(codicis_core STATIC
  order.cc
  order_book.cc)

target_include_directories(codicis_core PUBLIC
  ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(codicis_core PRIVATE codicis_warnings)

add_library(codicis::core ALIAS codicis_core)
