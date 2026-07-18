#!/usr/bin/env bash
#
# Regenerate the OMNeT++ Makefile for the core-hosted simulation and build it.
#
# The simulation runs entirely on loti-core: this build compiles core/ +
# adapters/sim/ (the port adapters) + app/sim/ (the thin Node-hosting OMNeT++
# modules). Everything is C++20 (see makefrag). The Makefile is generated, not
# checked in (it is gitignored), so this script is the source of truth for how the
# simulation is built.
#
# Requires the OMNeT++, INET, and loti setenv scripts to have been sourced first:
#   source <omnetpp>/setenv -q && source <inet>/setenv && source ./setenv -f
#
# Usage:  scripts/build-sim.sh [release|debug]   (default: release)
set -euo pipefail

: "${INET_ROOT:?source the INET setenv first (INET_ROOT unset)}"
command -v opp_makemake >/dev/null || { echo "source the OMNeT++ setenv first"; exit 1; }

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
mode="${1:-release}"

# The sim is built ONLY from src/core/ + src/adapters/sim/ + src/app/sim/ (+ sim/).
# Everything else is excluded so --deep doesn't sweep in a .cpp that doesn't belong:
# the production runtime `src/adapters/os` (WallClock, reactor, UDP, the Ed25519
# keystore on OpenSSL) and the production apps `src/app/lotid` (has its own main() +
# links libcrypto) / `src/app/loti` — any would pull an unresolved symbol into libloti.so.
opp_makemake -f --deep -e cpp --make-so -o loti -O out \
  -X test -X build -X scripts -X plan -X doc -X bin \
  -X src/adapters/os -X src/app/lotid -X src/app/loti \
  -KINET_PROJ="$INET_ROOT" -DINET_IMPORT \
  -Isrc -Isrc/core '-I$(INET_PROJ)/src' '-L$(INET_PROJ)/src' '-lINET$(D)'

make MODE="$mode" -j"$(nproc)"
