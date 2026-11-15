#
# Kvasir - The Distributed Relational Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# One libFuzzer target per parser of untrusted bytes. Clang only. The SQL
# lexer and parser join the list when they land.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "KVASIR_BUILD_FUZZING needs clang")
endif()

set(kvasir_fuzz_targets packet)
foreach(target IN LISTS kvasir_fuzz_targets)
  add_executable(${target}_fuzz ${target}_fuzz.cpp)
  target_compile_options(${target}_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(${target}_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(${target}_fuzz PRIVATE kvasir kvasir_warnings)
endforeach()
