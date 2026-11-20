#!/bin/sh
#
# Kvasir - The Distributed Relational Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# One CI step on Debian 13, as the nodes have it, in GitHub's runner, a local
# container or a Pi; .github/workflows/ci.yml runs each as its own job.
#
#   tools/ci.sh check      the header block on every source, and clang-format
#   tools/ci.sh release    g++ release build, warnings as errors, and the tests
#   tools/ci.sh clang      the same with clang++ (and -Wthread-safety)
#   tools/ci.sh tidy       clang-tidy's use-after-move check, warnings as errors
#   tools/ci.sh asan       AddressSanitizer and UndefinedBehaviorSanitizer
#   tools/ci.sh tsan       ThreadSanitizer
#   tools/ci.sh coverage   a coverage build's tests; lcov report build-ci/coverage.info
#   tools/ci.sh fuzz       each libFuzzer target for KVASIR_FUZZ_SECONDS (60), side by side
#   tools/ci.sh deb        Dannan's packages from its tag by its own tools/build-deb.sh,
#                          installed; then Kvasir's, its tests inside the build, and lintian
#                          (as root in a clean trixie: it installs packages)
#
# Builds go under build-ci/ (KVASIR_CI_OUT to move it), one job per CPU
# (KVASIR_CI_JOBS to set it: 2 on the shared build nodes). Dannan is fetched at
# the pinned tag from the sibling checkout when there is one, else from its
# repository (KVASIR_DANNAN_REPOSITORY to name another). Needs g++ cmake
# ninja-build libboost-dev libssl-dev libyaml-cpp-dev libgtest-dev, and for its
# step clang, libclang-rt-19-dev, clang-format, clang-tidy or lcov.

set -eu

top=$(cd "$(dirname "$0")/.." && pwd)
out=${KVASIR_CI_OUT:-$top/build-ci}
jobs=${KVASIR_CI_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}
step=${1:-}
# The pin, read where it is set.
dannan_tag=$(sed -n 's/^set(KVASIR_DANNAN_TAG "\([0-9.]*\)")/\1/p' "$top/CMakeLists.txt")
if [ -z "${KVASIR_DANNAN_REPOSITORY:-}" ]; then
  if [ -d "$top/../dannan/.git" ]; then
    KVASIR_DANNAN_REPOSITORY=$top/../dannan
  else
    KVASIR_DANNAN_REPOSITORY=https://github.com/tomdionysus/dannan.git
  fi
fi

# Warnings are errors in every build but the sanitizers': instrumented, g++
# warns inside libstdc++ and Asio, not in Kvasir.
configure() {
  dir="$1"
  shift
  cmake -S "$top" -B "$out/$dir" -G Ninja -DKVASIR_BUILD_TESTING=ON -DKVASIR_WARNINGS_AS_ERRORS=ON -DKVASIR_DANNAN_REPOSITORY="$KVASIR_DANNAN_REPOSITORY" "$@"
  cmake --build "$out/$dir" -j "$jobs"
}

case "$step" in
  check)
    cd "$top"
    missing=0
    for f in $(git ls-files '*.h' '*.cpp' '*.h.in'); do
      if ! head -3 "$f" | grep -qx '// Kvasir - The Distributed Relational Database'; then
        echo "no header block: $f"
        missing=1
      fi
    done
    [ "$missing" = 0 ]
    git ls-files '*.h' '*.cpp' | xargs clang-format --dry-run -Werror
    ;;
  release)
    configure release -DCMAKE_BUILD_TYPE=Release
    "$out/release/kvasir_tests"
    ;;
  clang)
    CXX=clang++ configure clang -DCMAKE_BUILD_TYPE=Release
    "$out/clang/kvasir_tests"
    ;;
  asan)
    flags="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
    configure asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKVASIR_WARNINGS_AS_ERRORS=OFF -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags"
    ASAN_OPTIONS=detect_stack_use_after_return=1 UBSAN_OPTIONS=print_stacktrace=1 "$out/asan/kvasir_tests"
    ;;
  tsan)
    flags="-fsanitize=thread -fno-omit-frame-pointer"
    configure tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKVASIR_WARNINGS_AS_ERRORS=OFF -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags"
    TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" "$out/tsan/kvasir_tests"
    ;;
  coverage)
    configure coverage -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=--coverage -DCMAKE_EXE_LINKER_FLAGS=--coverage -DCMAKE_SHARED_LINKER_FLAGS=--coverage
    find "$out/coverage" -name '*.gcda' -delete
    (cd "$out/coverage" && ./kvasir_tests)
    ignore=inconsistent,inconsistent,mismatch,negative,unused
    # A closing brace after a returned object is a line gcov counts and never
    # runs; it is not code.
    lcov --quiet --capture --directory "$out/coverage" --ignore-errors "$ignore" --filter brace,blank --output-file "$out/all.info"
    lcov --quiet --extract "$out/all.info" "$top/kvasir/*" --ignore-errors "$ignore" --output-file "$out/library.info"
    lcov --quiet --remove "$out/library.info" "$top/kvasir/test/*" "$top/kvasir/fuzz/*" --ignore-errors "$ignore" --output-file "$out/coverage.info"
    rm "$out/all.info" "$out/library.info"
    lcov --summary "$out/coverage.info" --ignore-errors "$ignore"
    ;;
  tidy)
    # Configured, not built: clang-tidy reads the compile commands. Use after
    # move counts the unsequenced kind too, a value moved into one argument
    # and read in another (the stack's rule, 2026-11-20).
    CXX=clang++ cmake -S "$top" -B "$out/tidy" -G Ninja -DKVASIR_BUILD_TESTING=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DKVASIR_DANNAN_REPOSITORY="$KVASIR_DANNAN_REPOSITORY"
    tidy=$(command -v run-clang-tidy || command -v run-clang-tidy-19)
    "$tidy" -p "$out/tidy" -j "$jobs" -quiet -checks='-*,bugprone-use-after-move' -warnings-as-errors='*' -header-filter="$top/kvasir/.*" "$top/kvasir/.*"
    ;;
  fuzz)
    CXX=clang++ configure fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKVASIR_BUILD_TESTING=OFF -DKVASIR_BUILD_FUZZING=ON -DKVASIR_WARNINGS_AS_ERRORS=OFF
    mkdir -p "$out/fuzz/findings"
    # The targets side by side, one process each: libFuzzer's -fork mode
    # leaks in its own driver, which would hide a leak in Kvasir's code.
    targets="packet lexer parser execute"
    pids=""
    for target in $targets; do
      mkdir -p "$out/fuzz/corpus/$target"
      "$out/fuzz/${target}_fuzz" -max_total_time="${KVASIR_FUZZ_SECONDS:-60}" -max_len=4096 -timeout=10 \
        -artifact_prefix="$out/fuzz/findings/$target-" "$out/fuzz/corpus/$target" > "$out/fuzz/$target.log" 2>&1 &
      pids="$pids $!"
    done
    failed=0
    for pid in $pids; do
      wait "$pid" || failed=1
    done
    for target in $targets; do
      echo "== $target"
      tail -n 3 "$out/fuzz/$target.log"
    done
    if [ "$failed" != 0 ]; then
      grep -h -A 30 '^==[0-9]*==ERROR\|deadly signal\|uncaught exception' "$out"/fuzz/*.log || true
      exit 1
    fi
    ;;
  deb)
    rm -rf "$out/deb"
    git clone --quiet --depth 1 --branch "$dannan_tag" "$KVASIR_DANNAN_REPOSITORY" "$out/deb/dannan"
    (cd "$out/deb/dannan" && apt-get build-dep -y ./ && DEB_BUILD_OPTIONS="nocheck parallel=$jobs" tools/build-deb.sh)
    apt-get install -y "$out"/deb/dannan/build-deb/libdannan*.deb
    cd "$top"
    apt-get build-dep -y ./
    DEB_BUILD_OPTIONS="parallel=$jobs" tools/build-deb.sh
    lintian --fail-on error,warning build-deb/*.changes
    ;;
  *)
    echo "usage: tools/ci.sh check|release|clang|tidy|asan|tsan|coverage|fuzz|deb" >&2
    exit 2
    ;;
esac
