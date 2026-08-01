cmake_minimum_required(VERSION 3.20)

project(codicis
  VERSION 0.0.1
  DESCRIPTION "Central limit order book, matching and processing engine"
  LANGUAGES CXX)

# ---- Global settings -------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# ---- Options ---------------------------------------------------------------

option(CODICIS_BUILD_TESTS "Build the Catch2 unit tests" ON)
option(CODICIS_WERROR "Treat warnings as errors" ON)

# ---- Warnings (applied per target via codicis_apply_warnings) --------------

add_library(codicis_warnings INTERFACE)
target_compile_options(codicis_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
if(CODICIS_WERROR)
  target_compile_options(codicis_warnings INTERFACE -Werror)
endif()

# ---- Platform detection ----------------------------------------------------

include(Platform)

# ---- Subsystems ------------------------------------------------------------

add_subdirectory(src/util)
add_subdirectory(src/config)
add_subdirectory(src/event)
add_subdirectory(src/net)

# ---- Tests -----------------------------------------------------------------

if(CODICIS_BUILD_TESTS)
  include(Catch2)
  enable_testing()
  add_subdirectory(tests)
endif()
