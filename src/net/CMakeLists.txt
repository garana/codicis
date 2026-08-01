# codicis_net: hand-rolled HTTP/1.1 (and later WebSocket) on the event loop.

add_library(codicis_net STATIC
  http_message.cc
  http_parser.cc
  router.cc
  listener.cc
  http_server.cc
  ws_frame.cc
  ws_handshake.cc
  websocket.cc)

target_include_directories(codicis_net PUBLIC
  ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(codicis_net
  PUBLIC codicis_util codicis_event
  PRIVATE codicis_warnings)

add_library(codicis::net ALIAS codicis_net)
