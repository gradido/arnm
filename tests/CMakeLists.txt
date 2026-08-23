# One executable per test_*.cpp, the same set build.zig produces, each registered with ctest.
# The tests are C++ while the library is C, so the language is enabled here rather than in the
# root project: a consumer building only the library never pays for a C++ toolchain.

enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Pinned to the release build.zig.zon resolves to, so both builds test against the same
# googletest rather than drifting apart on whatever a branch serves today.
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
)
# Without this googletest links the static CRT while everything else links the dynamic one, and
# MSVC fails at link time with mismatched runtime libraries. No effect off MSVC.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

FILE(GLOB TEST_SOURCES "unit/src/test_*.cpp")

foreach(test_source ${TEST_SOURCES})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE arnm GTest::gtest GTest::gtest_main)
  # memory_limit.h sits beside the sources and every test binary includes it; src/ is on the
  # path as well, for the private memory_intern.h the white box tests read the layout from
  target_include_directories(${test_name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/unit/src
    ${CMAKE_SOURCE_DIR}/src
  )
  set_target_properties(${test_name} PROPERTIES FOLDER tests)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
