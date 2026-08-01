# Catch2.cmake
#
# Provides the `Catch2::Catch2WithMain` target used by the test binaries.
# Resolution order (offline-safe by default):
#   1. An installed Catch2 v3 (find_package), if present.
#   2. The vendored amalgamated sources under third_party/catch2 (default).
# The vendored path needs no network, keeping builds reproducible offline.

if(NOT TARGET Catch2::Catch2WithMain)
  find_package(Catch2 3 QUIET)
endif()

if(NOT TARGET Catch2::Catch2WithMain)
  set(_codicis_catch_dir ${CMAKE_SOURCE_DIR}/third_party/catch2)
  if(EXISTS ${_codicis_catch_dir}/catch_amalgamated.cpp)
    add_library(codicis_catch2 STATIC
      ${_codicis_catch_dir}/catch_amalgamated.cpp)
    target_include_directories(codicis_catch2 PUBLIC ${_codicis_catch_dir})
    target_compile_features(codicis_catch2 PUBLIC cxx_std_20)
    # Never lint the vendored third-party amalgamation.
    set_target_properties(codicis_catch2 PROPERTIES CXX_CLANG_TIDY "")
    add_library(Catch2::Catch2WithMain ALIAS codicis_catch2)
    message(STATUS "codicis: using vendored Catch2 (amalgamated)")
  else()
    message(FATAL_ERROR
      "codicis: Catch2 not found and no vendored amalgamated sources at "
      "${_codicis_catch_dir}")
  endif()
endif()
