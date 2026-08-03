cmake_minimum_required(VERSION 3.20)

# On macOS, pin the SDK sysroot so it lands in compile_commands.json. Apple
# clang finds the SDK implicitly, but standalone tools (clang-tidy, clangd from
# Homebrew LLVM) do not; without this they fail to locate <cstddef> etc.
if(APPLE AND NOT CMAKE_OSX_SYSROOT)
  execute_process(COMMAND xcrun --show-sdk-path
    OUTPUT_VARIABLE _codicis_sdk OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_codicis_sdk)
    set(CMAKE_OSX_SYSROOT "${_codicis_sdk}" CACHE PATH "macOS SDK sysroot"
        FORCE)
  endif()
endif()

project(codicis
  VERSION 0.0.1
  DESCRIPTION "Central limit order book, matching and processing engine"
  LANGUAGES CXX)

# ---- Global settings -------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Always emit compile_commands.json: it drives clang-tidy and fixes clangd's
# include resolution in editors.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# ---- Options ---------------------------------------------------------------

option(CODICIS_BUILD_TESTS "Build the Catch2 unit tests" ON)
option(CODICIS_WERROR "Treat warnings as errors" ON)
option(CODICIS_CLANG_TIDY "Run clang-tidy during the build" OFF)

# ---- clang-tidy (opt-in; off by default so normal builds stay fast) --------

if(CODICIS_CLANG_TIDY)
  find_program(CODICIS_CLANG_TIDY_EXE
    NAMES clang-tidy
    HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
  if(CODICIS_CLANG_TIDY_EXE)
    message(STATUS "codicis: clang-tidy = ${CODICIS_CLANG_TIDY_EXE}")
    set(CMAKE_CXX_CLANG_TIDY ${CODICIS_CLANG_TIDY_EXE})
  else()
    message(FATAL_ERROR "codicis: CODICIS_CLANG_TIDY=ON but clang-tidy not found")
  endif()
endif()

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
add_subdirectory(src/ipc)
add_subdirectory(src/auth)
add_subdirectory(src/core)
add_subdirectory(src/engine)
add_subdirectory(src/app)
add_subdirectory(tools/storage_helper)
add_subdirectory(tools/auth_helper)

# ---- Tests -----------------------------------------------------------------

if(CODICIS_BUILD_TESTS)
  include(Catch2)
  enable_testing()
  add_subdirectory(tests)
endif()
