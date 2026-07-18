#!/usr/bin/env bash
#
# Availability fuzz (hardening plan, Phase 1): a running lotid must survive a flood of
# malformed UDP datagrams from an arbitrary source and a malformed control command, and
# stay responsive. Before the Phase 1 fixes, a single garbage datagram terminated the
# daemon (wire::decode ran before the sender was even checked, and threw straight out of
# the event loop).
#
#   Usage:  test/acceptance/fuzz_udp.sh        (build first: scripts/build-core.sh)
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$ROOT/build/core"
LOTI="$BIN/loti"
LOTID="$BIN/lotid"
if [ ! -x "$LOTI" ] || [ ! -x "$LOTID" ]; then
  echo "fuzz_udp: build first — scripts/build-core.sh (missing $BIN/{loti,lotid})" >&2
  exit 2
fi
command -v python3 >/dev/null || { echo "fuzz_udp: needs python3" >&2; exit 2; }

WORK="$(mktemp -d)"
PORT=28866
SOCK="$WORK/control.sock"
OUT="$WORK/lotid.out"
declare -a PIDS=()
# Kill the daemon and the stdin-keepalive pipeline (pkill -P: this script's children),
# without wait — the `tail -f` keepalive never exits on its own and would hang wait.
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done; pkill -P $$ 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

# tail -f keeps stdin open (no EOF) so lotid does not stop on a closed stdin.
tail -f /dev/null | "$LOTID" --id 1 --port "$PORT" --control "$SOCK" --clock-interval 1 >"$OUT" 2>&1 &
PIDS+=($!)
LOTID_PID=$!

for _ in $(seq 1 50); do "$LOTI" --control "$SOCK" status >/dev/null 2>&1 && break; sleep 0.1; done
if ! "$LOTI" --control "$SOCK" status >/dev/null 2>&1; then
  echo "  ✗ daemon never came up"; cat "$OUT"; exit 1
fi
echo "  ✓ daemon up and responsive"

python3 - "$PORT" <<'PY'
import socket, sys
p = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
pkts = [
    b'\x00', b'\x2a',                          # type-only, truncated
    bytes([9,1,2,3,4,5,6,7,8]),                # valid header, unknown datagram type
    bytes([0,1,2,3,4,5,6,7,8]),                # clock_notification, truncated payload
    bytes([1]+[0]*52+[0xff]*4),                # chain_request with a ~4e9-element count
    b'\xff'*2000, bytes(range(256))*8,         # random / structured garbage
]
for _ in range(300):
    for pk in pkts:
        s.sendto(pk, ('127.0.0.1', p))
print("  ✓ flooded %d malformed datagrams" % (300 * len(pkts)))
PY

# A malformed control command must not kill the daemon either (Phase 1.5).
"$LOTI" --control "$SOCK" peer add "not-a-valid-address" >/dev/null 2>&1
echo "  ✓ sent a malformed control command"

if kill -0 "$LOTID_PID" 2>/dev/null && "$LOTI" --control "$SOCK" status >/dev/null 2>&1; then
  echo "  ✓ daemon survived and is still responsive"
  echo "fuzz_udp: PASS"
  exit 0
fi
echo "  ✗ daemon died or is unresponsive after the flood"; cat "$OUT"; exit 1
