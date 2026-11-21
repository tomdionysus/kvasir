#
# Kvasir - The Distributed Relational Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# One libFuzzer binary per parser of untrusted bytes, built with clang. The
# same targets run without clang in kvasir_tests, through Dannan's harness's
# smoke run. The sanitizers and coverage instrumentation are the top-level
# CMakeLists.txt's.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "KVASIR_BUILD_FUZZING needs clang")
endif()

# Each target registers with Dannan's fuzz harness (dannan::testing); a binary
# is its one target and libFuzzer's driver. The same targets run in
# kvasir_tests through the harness's smoke run (kvasir/test/CMakeLists.txt).
set(kvasir_fuzz_targets packet lexer parser execute)
foreach(target IN LISTS kvasir_fuzz_targets)
  add_executable(${target}_fuzz ${target}_fuzz.cpp driver.cpp)
  target_link_options(${target}_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_libraries(${target}_fuzz PRIVATE kvasir dannan::testing kvasir_warnings)
endforeach()
