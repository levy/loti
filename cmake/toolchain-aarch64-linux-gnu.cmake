# CMake toolchain: cross-compile LOTI for aarch64 (ARMv8-A, 64-bit) — the Raspberry Pi
# Zero 2 W (Cortex-A53) and any arm64 Linux target. This is the recommended long-life
# node target (no 32-bit address-space ceiling; see doc/embedded.md).
#
# Use it through the wrapper:
#     scripts/build-cross.sh aarch64
# or directly:
#     cmake -S . -B build/aarch64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-linux-gnu.cmake
#
# Prerequisites (Debian/Ubuntu host):
#   sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
# plus a target libcrypto so find_package(OpenSSL) resolves — either
#   (a) multiarch cross packages:
#         sudo dpkg --add-architecture arm64
#         sudo apt update && sudo apt install libssl-dev:arm64
#   (b) a Pi sysroot rsynced from the device — point at it with -DLOTI_SYSROOT=/path
#       (or the LOTI_SYSROOT env var). See doc/embedded.md.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(_loti_triplet aarch64-linux-gnu)
set(CMAKE_C_COMPILER   ${_loti_triplet}-gcc)
set(CMAKE_CXX_COMPILER ${_loti_triplet}-g++)

# A sysroot (rsynced from the Pi) takes precedence over host multiarch packages. Accept
# it from -DLOTI_SYSROOT=... or the LOTI_SYSROOT environment variable.
if(NOT LOTI_SYSROOT)
  set(LOTI_SYSROOT "$ENV{LOTI_SYSROOT}")
endif()

if(LOTI_SYSROOT)
  # Pure-sysroot mode: headers and libraries come only from the Pi image.
  set(CMAKE_SYSROOT ${LOTI_SYSROOT})
  set(CMAKE_FIND_ROOT_PATH ${LOTI_SYSROOT})
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
else()
  # Multiarch mode: the arm64 dev packages install under the host's /usr/lib/<triplet>
  # and shared /usr/include, so let find_* also search the host roots but resolve
  # libraries via the target triplet.
  set(CMAKE_LIBRARY_ARCHITECTURE ${_loti_triplet})
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
endif()

# Always find build tools (cmake, etc.) on the host, never in the target root.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
