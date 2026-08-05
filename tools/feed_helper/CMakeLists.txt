# The reference market-data feed-helper child process.

add_executable(codicis_feed_helper main.cc)

target_link_libraries(codicis_feed_helper
  PRIVATE codicis_feed codicis_net codicis_event codicis_warnings)
