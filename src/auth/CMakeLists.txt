# codicis_auth: request authentication (token cache + auth helper client).

add_library(codicis_auth STATIC
  token_cache.cc
  auth_client.cc)

target_include_directories(codicis_auth PUBLIC
  ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(codicis_auth
  PUBLIC codicis_util codicis_event codicis_ipc
  PRIVATE codicis_warnings)

add_library(codicis::auth ALIAS codicis_auth)
