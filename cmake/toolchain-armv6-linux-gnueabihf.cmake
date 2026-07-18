# CMake toolchain: cross-compile LOTI for ARMv6 hard-float — the Raspberry Pi Zero and
# Zero W (BCM2835, single-core ARM1176, 32-bit). This exercises the 32-bit store path
# from Part A of plan constrained-node-support. Note the 32-bit LMDB mmap ceiling
# (~2 GiB) bounds the node's lifetime DAG — see doc/embedded.md.
#
# Use it through the wrapper:
#     scripts/build-cross.sh armv6
# or directly:
#     cmake -S . -B build/armv6 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-armv6-linux-gnueabihf.cmake
#
# Prerequisites (Debian/Ubuntu host):
#   sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
# plus a target libcrypto so find_package(OpenSSL) resolves — either
#   (a) multiarch cross packages:
#         sudo dpkg --add-architecture armhf
#         sudo apt update && sudo apt install libssl-dev:armhf
#   (b) a Pi sysroot rsynced from the device — point at it with -DLOTI_SYSROOT=/path.
#
# CAVEAT: the Debian `armhf` toolchain and libssl:armhf target ARMv7; the flags below
# recompile our sources for ARMv6, but a distro libcrypto:armhf may contain ARMv7
# instructions and fail on a real Pi Zero. For a binary that truly runs on ARMv6, build
# against a Raspberry Pi OS sysroot (option b) whose libcrypto is ARMv6. See doc/embedded.md.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(_loti_triplet arm-linux-gnueabihf)
set(CMAKE_C_COMPILER   ${_loti_triplet}-gcc)
set(CMAKE_CXX_COMPILER ${_loti_triplet}-g++)

# The Pi Zero / Zero W is ARMv6 + VFPv2, hard-float ABI. The armhf toolchain defaults to
# ARMv7, so force ARMv6 or the binary will SIGILL on the device.
set(_loti_armv6_flags "-march=armv6 -mfpu=vfp -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT   "${_loti_armv6_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_loti_armv6_flags}")

if(NOT LOTI_SYSROOT)
  set(LOTI_SYSROOT "$ENV{LOTI_SYSROOT}")
endif()

if(LOTI_SYSROOT)
  set(CMAKE_SYSROOT ${LOTI_SYSROOT})
  set(CMAKE_FIND_ROOT_PATH ${LOTI_SYSROOT})
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
else()
  set(CMAKE_LIBRARY_ARCHITECTURE ${_loti_triplet})
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
