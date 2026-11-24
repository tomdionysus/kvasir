#
# Kvasir - The Distributed Relational Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

cmake_minimum_required(VERSION 3.24)

project(kvasir VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Executables at the top of the build tree: ./build/kvasir_tests.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(KVASIR_BUILD_TESTING "Build kvasir_tests (GoogleTest)" OFF)
option(KVASIR_BUILD_FUZZING "Build the libFuzzer targets in kvasir/fuzz; needs a clang that ships libFuzzer" OFF)
option(KVASIR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
# On: Dannan is the installed package (find_package), never a copy; the
# library is linked into kvasir-node, and the install rules are defined, as
# the Debian build has it (docs/packaging.md).
option(KVASIR_SYSTEM_DANNAN "Build against the installed Dannan and define the install rules" OFF)

find_package(Boost 1.83 CONFIG REQUIRED COMPONENTS headers)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto SSL)
find_package(Threads REQUIRED)

# A fuzz build runs under its sanitizers, Dannan included: instrumented and
# plain code sharing containers give ASan false reports. AddressSanitizer
# cannot start on a kernel with a 39-bit address space; there, "undefined".
set(KVASIR_FUZZ_SANITIZERS "address,undefined" CACHE STRING "The -fsanitize= list a fuzz build is compiled with")
if(KVASIR_BUILD_FUZZING)
  add_compile_options(-fsanitize=${KVASIR_FUZZ_SANITIZERS} -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  # Clang's UBSan checks a widening multiply with __muloti4, which only
  # compiler-rt's builtins hold (libclang-rt-19-dev).
  add_link_options(-fsanitize=${KVASIR_FUZZ_SANITIZERS} --rtlib=compiler-rt -unwindlib=libgcc)
endif()

# Dannan, the network layer, at its tag, cloned from the sibling checkout into
# the build tree, so building Kvasir never touches ../dannan.
# STRING, not PATH: a PATH entry mangles a URL passed on the command line.
set(KVASIR_DANNAN_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../dannan" CACHE STRING "Dannan's repository: a path or a URL")
# Not a cache variable: a moved pin must take effect in a build tree that
# already exists.
set(KVASIR_DANNAN_TAG "0.12.0")
include(FetchContent)
include(GNUInstallDirs)
# The installed one is found by minor, at least the pinned patch (Dannan's
# SameMinorVersion).
if(KVASIR_SYSTEM_DANNAN)
  find_package(dannan ${KVASIR_DANNAN_TAG} CONFIG REQUIRED)
elseif(NOT TARGET dannan::dannan)
  FetchContent_Declare(dannan GIT_REPOSITORY "${KVASIR_DANNAN_REPOSITORY}" GIT_TAG "${KVASIR_DANNAN_TAG}")
  set(DANNAN_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  # The library alone: Kvasir runs none of Dannan's tools.
  set(DANNAN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(dannan)
endif()

# Only Kvasir's own code is instrumented for libFuzzer's coverage, so the
# fuzzer explores Kvasir's parsers and not Dannan's internals.
if(KVASIR_BUILD_FUZZING)
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

add_library(kvasir_warnings INTERFACE)
target_compile_options(kvasir_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(kvasir_warnings INTERFACE -Wthread-safety)
endif()
if(KVASIR_WARNINGS_AS_ERRORS)
  target_compile_options(kvasir_warnings INTERFACE -Werror)
endif()

configure_file(kvasir/include/kvasir/version.h.in ${PROJECT_BINARY_DIR}/generated/kvasir/version.h @ONLY)

add_subdirectory(kvasir)
add_subdirectory(tools)

if(KVASIR_BUILD_TESTING)
  find_package(GTest 1.14 CONFIG REQUIRED)
  # Off where nothing may be downloaded (the Debian package's build).
  option(KVASIR_MYSQL_CLIENT_TESTS "Drive a node with a real MySQL client library in kvasir_tests, built from Oracle's source" ON)
  if(KVASIR_MYSQL_CLIENT_TESTS)
    include(cmake/mysql_client.cmake)
  endif()
  add_subdirectory(kvasir/test)
endif()

if(KVASIR_BUILD_FUZZING)
  add_subdirectory(kvasir/fuzz)
endif()
