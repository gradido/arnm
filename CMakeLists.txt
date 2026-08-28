cmake_minimum_required(VERSION 3.18.2)
cmake_policy(VERSION 3.18.2)

# CMP0135 makes FetchContent stamp extracted archives with the extraction time instead of the
# timestamps inside the tarball, which is what keeps the googletest download in tests/ from
# looking older than the build that unpacked it. It only exists from CMake 3.24 on, and setting
# an unknown policy is a hard error -- so it is asked for rather than assumed, and the versions
# between the declared 3.18.2 minimum and 3.24 simply configure without it.
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# build.zig is the master build of this project -- it defines the targets, the options and the
# matrix that gets verified. This file exists for the one case zig cannot serve: the MSVC ABI
# on Windows, whose SDK zig does not ship. Everything here mirrors build.zig rather than
# leading it; when the two disagree, build.zig is right. See AGENTS.md.
#
#   cmake -B build -DENABLE_TESTS=ON -DENABLE_BENCHMARKS=ON
#   cmake --build build --config Release
#   ctest --test-dir build -C Release

set(PROJECT_NAME arnm)
project(${PROJECT_NAME} C)

# CMake only learned to pass /std:c11 to MSVC in 3.20; older versions accept these lines and
# quietly hand MSVC its default C mode instead. That is not worth raising the minimum over --
# MSVC itself needs VS 2019 16.8 for C11, which ships with a newer CMake anyway -- but if a
# Windows build fails on C11 syntax, check the CMake version before the source.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Mirrors -Dtests, -Dbenchmarks and -Dsanitize of build.zig, defaulting the same way.
option(ENABLE_BENCHMARKS "Build the bench_* binaries" OFF)
option(ENABLE_TESTS "Build the googletest binaries" OFF)
option(ENABLE_SANITIZERS "Build with AddressSanitizer, plus UndefinedBehaviorSanitizer on non-MSVC toolchains (MSVC ships no UBSan)" OFF)
option(ENABLE_THREAD_SANITIZER "Build with ThreadSanitizer (mutually exclusive with ENABLE_SANITIZERS)" OFF)

# Set before any target is declared, so library, tests and benchmarks are instrumented the same
# way -- mixing an instrumented library with an uninstrumented test binary produces false reports.
if(ENABLE_SANITIZERS AND ENABLE_THREAD_SANITIZER)
  message(FATAL_ERROR "ENABLE_SANITIZERS and ENABLE_THREAD_SANITIZER cannot be combined; ThreadSanitizer does not run together with AddressSanitizer")
endif()

if(ENABLE_SANITIZERS)
  if(MSVC)
    # MSVC ships AddressSanitizer only, there is no UndefinedBehaviorSanitizer counterpart.
    # This is also the one sanitizer build.zig cannot offer: zig does not ship the asan runtime.
    message(STATUS "Sanitizers enabled: address")
    add_compile_options(/fsanitize=address /Zi)
  else()
    message(STATUS "Sanitizers enabled: address, undefined")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

if(ENABLE_THREAD_SANITIZER)
  if(MSVC)
    message(FATAL_ERROR "ENABLE_THREAD_SANITIZER is not supported by MSVC")
  endif()
  message(STATUS "Sanitizers enabled: thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
endif()

if(MSVC)
  # The sources are ASCII and lint.sh keeps them that way, which already settles the question:
  # every byte means the same in every codepage. This stays as the second line of defence, for
  # the window between someone typing an em dash and lint.sh telling them not to -- without it
  # MSVC would read the file in the system codepage and warn (C4819) or mangle the character.
  add_compile_options(/utf-8)
endif()

# Mirrors c_flags in build.zig. -Wall and -Wextra are the baseline; -Wconversion earns its place
# beside them because this library narrows on purpose and often: element_size into a uint16_t
# field, element counts into bucket counts, bucket counts into index slots. A silent narrowing is
# exactly how those turn into wrong results instead of ARNM_ERROR_ARITHMETIC_OVERFLOW, so every
# remaining one is meant to carry an explicit cast next to the check that bounds it.
#
# MSVC spells the first two /W4. It has no -Wconversion; C4242, C4244 and C4267 are the three
# warnings that together cover the same ground, and they are off even at /W4, so each is raised
# to level 1 explicitly.
#
# /W4 goes in without a /W3 to fight: cmake_policy(VERSION 3.18.2) at the top of this file puts
# CMP0092 (added in 3.15) at NEW, which is what keeps CMake from seeding CMAKE_C_FLAGS with /W3.
# Were it OLD, both would arrive and MSVC would answer D9025 -- so if that warning ever shows
# up, the policy is what to look at, not this list.
#
# Nothing here is -Werror, and this build is the one that actually prints the findings: zig
# hides C warnings on a successful build and turns them into errors on a failing one, so the
# CMake build is where a narrowing gets noticed without the tree being broken already.
if(MSVC)
  set(ARNM_WARNING_FLAGS /W4 /w14242 /w14244 /w14267)
else()
  set(ARNM_WARNING_FLAGS -Wall -Wextra -Wconversion)
endif()

FILE(GLOB ARNM_SRC "src/*.c")
FILE(GLOB ARNM_HEADERS "include/arnm/*.h")

# yyjson is the parser behind arnm/json_reader.h. Its two files are copied into
# third_party/yyjson/ rather than pulled in as a submodule -- see third_party/yyjson/README.md
# for the version and the reason. It is compiled into libarnm itself, so a consumer links one
# library and adds one include path -- mirrors build.zig, which does the same and leads here.
set(YYJSON_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/yyjson/src/yyjson.c)

add_library(${PROJECT_NAME} STATIC ${ARNM_SRC} ${ARNM_HEADERS} ${YYJSON_SOURCE})

# disable features we don't use
add_compile_definitions(YYJSON_DISABLE_INCR_READER=1)
add_compile_definitions(YYJSON_DISABLE_UTILS=1)
add_compile_definitions(YYJSON_DISABLE_NON_STANDARD=1)
add_compile_definitions(YYJSON_DISABLE_UTF8_VALIDATION=1)

target_include_directories(${PROJECT_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# PRIVATE, and not part of the install interface: no installed header names yyjson, so a
# consumer never needs this path and never learns which parser is underneath.
target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/yyjson/src
)

# PRIVATE: a consumer of the installed library picks its own warning level.
target_compile_options(${PROJECT_NAME} PRIVATE ${ARNM_WARNING_FLAGS})

# The narrowing warnings are a rule arnm holds itself to; a third party source is not ours to
# keep clean under it. A source property is appended after the target options, so the last word
# on that translation unit is the one below. Mirrors the flag set build.zig gives yyjson.c.
if(MSVC)
  set_source_files_properties(${YYJSON_SOURCE} PROPERTIES COMPILE_OPTIONS "/wd4242;/wd4244;/wd4267")
else()
  set_source_files_properties(${YYJSON_SOURCE} PROPERTIES COMPILE_OPTIONS "-Wno-conversion")
endif()

if(MSVC)
  source_group("src" FILES ${ARNM_SRC})
  source_group("include" FILES ${ARNM_HEADERS})
  source_group("third_party" FILES ${YYJSON_SOURCE})
  # debug symbols for a parent project stepping into this one
  target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:/JMC>)
endif()

install(TARGETS ${PROJECT_NAME} ARCHIVE DESTINATION lib)
install(FILES ${ARNM_HEADERS} DESTINATION include/arnm)

if(ENABLE_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
