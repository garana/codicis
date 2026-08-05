# codicis_feed: book-event wire codec and the L1/L2/L3 book replica.

add_library(codicis_feed STATIC
  feed_wire.cc
  book_replica.cc
  feed_publisher.cc)

target_include_directories(codicis_feed PUBLIC
  ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(codicis_feed
  PUBLIC codicis_util codicis_core codicis_event
  PRIVATE codicis_warnings)

add_library(codicis::feed ALIAS codicis_feed)
