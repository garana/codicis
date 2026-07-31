# Platform.cmake
#
# Detects the available event-loop backend (epoll on Linux, kqueue on
# BSD/macOS) and exposes the result as cache variables. Subsystems that
# need a backend (the `event` library) consume these to select the
# platform-specific translation unit and set compile definitions.

include(CheckSymbolExists)

check_symbol_exists(epoll_create1 "sys/epoll.h" CODICIS_HAVE_EPOLL)
check_symbol_exists(kqueue "sys/event.h" CODICIS_HAVE_KQUEUE)

if(NOT CODICIS_HAVE_EPOLL AND NOT CODICIS_HAVE_KQUEUE)
  message(FATAL_ERROR
    "codicis: no supported event backend found (need epoll or kqueue)")
endif()

if(CODICIS_HAVE_KQUEUE)
  message(STATUS "codicis: event backend = kqueue")
endif()
if(CODICIS_HAVE_EPOLL)
  message(STATUS "codicis: event backend = epoll")
endif()
