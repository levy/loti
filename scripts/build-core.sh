#!/usr/bin/env bash
#
# Build loti-core and its unit tests in isolation, then run them.
#
# This mechanically proves the dependency rule from documentation/architecture.md:
# loti-core compiles and its tests pass with NO OMNeT++, INET, or product-runtime
# toolchain on the path. The CMake project references none of them; if that ever
# regresses, this build breaks.
#
# Usage:  scripts/build-core.sh [extra cmake args...]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build/core"

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Debug "$@"
cmake --build "$build" -j
ctest --test-dir "$build" --output-on-failure
