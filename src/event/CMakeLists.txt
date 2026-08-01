# codicis_event: the event-loop abstraction plus the platform backend.
#
# Only the backend matching the host platform is compiled; both backend
# sources also guard their bodies with the same macro for defense in depth.

set(_codicis_event_sources
  event_loop.cc
  event_loop_factory.cc)

if(CODICIS_HAVE_KQUEUE)
  list(APPEND _codicis_event_sources kqueue_loop.cc)
endif()
if(CODICIS_HAVE_EPOLL)
  list(APPEND _codicis_event_sources epoll_loop.cc)
endif()

add_library(codicis_event STATIC ${_codicis_event_sources})

target_include_directories(codicis_event PUBLIC
  ${CMAKE_SOURCE_DIR}/include)

# Propagate the detected backend as a compile definition.
if(CODICIS_HAVE_KQUEUE)
  target_compile_definitions(codicis_event PRIVATE CODICIS_HAVE_KQUEUE=1)
endif()
if(CODICIS_HAVE_EPOLL)
  target_compile_definitions(codicis_event PRIVATE CODICIS_HAVE_EPOLL=1)
endif()

target_link_libraries(codicis_event
  PUBLIC codicis_util
  PRIVATE codicis_warnings)

add_library(codicis::event ALIAS codicis_event)
