#!/usr/bin/env bash
#
# Cross-compile the LOTI daemon (lotid) and CLI (loti) for a Raspberry Pi from a dev box.
#
# Usage:  scripts/build-cross.sh <aarch64|armv6> [extra cmake args...]
#   aarch64  Raspberry Pi Zero 2 W (Cortex-A53, 64-bit)  → build/aarch64/{lotid,loti}
#   armv6    Raspberry Pi Zero / Zero W (ARMv6, 32-bit)  → build/armv6/{lotid,loti}
#
# Prerequisites (Debian/Ubuntu host) — see the matching cmake/toolchain-*.cmake and
# doc/embedded.md for the full walkthrough (toolchain, sysroot, target OpenSSL):
#   aarch64: sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
#   armv6:   sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
# plus a target libcrypto for find_package(OpenSSL): either multiarch dev packages
# (libssl-dev:arm64 / :armhf) or a Pi sysroot pointed at with LOTI_SYSROOT=/path.
#
# The host cannot run the cross-built binaries' ctest; copy them to the Pi (or run under
# qemu-user-static — optional, see doc/embedded.md) to test.
set -euo pipefail

arch="${1:-}"
case "$arch" in
  aarch64) toolchain=toolchain-aarch64-linux-gnu.cmake;    cc=aarch64-linux-gnu-gcc ;;
  armv6)   toolchain=toolchain-armv6-linux-gnueabihf.cmake; cc=arm-linux-gnueabihf-gcc ;;
  *) echo "usage: $0 <aarch64|armv6> [extra cmake args...]" >&2; exit 2 ;;
esac
shift

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build/$arch"

if ! command -v "$cc" >/dev/null 2>&1; then
  echo "error: cross compiler '$cc' is not on PATH." >&2
  echo "Install it (Debian/Ubuntu):" >&2
  if [ "$arch" = aarch64 ]; then
    echo "  sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu" >&2
    libssl=libssl-dev:arm64
  else
    echo "  sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf" >&2
    libssl=libssl-dev:armhf
  fi
  echo "and a target libcrypto ($libssl multiarch or a Pi sysroot;" >&2
  echo "see cmake/$toolchain and doc/embedded.md)." >&2
  exit 1
fi

# Release: an embedded node wants an optimized binary. Build only the runtime targets —
# the unit tests can't run on the host under a cross build.
cmake -S "$root" -B "$build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$root/cmake/$toolchain" \
  "$@"
cmake --build "$build" -j --target lotid loti

echo
echo "built for $arch:"
for bin in lotid loti; do
  echo "  $build/$bin"
  file "$build/$bin" 2>/dev/null | sed 's/^/    /' || true
done
