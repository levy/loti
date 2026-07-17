#!/usr/bin/env bash
#
# LOTI production acceptance suite (architecture.md testing tier 4: real sockets,
# real disk, real restart). Exercises the lotid/loti binaries end to end and exits
# non-zero if any check fails — this is the multi-node / persistence CI gate for M4.
#
#   1. restart survival   — a node's DAG survives stop + restart (snapshot store)
#   2. backup / restore    — db backup, diverge, db restore reverts the state
#   3. multi-node notary   — B proves A's event over real UDP; verify offline; a
#                            tampered proof is rejected
#
# Usage:  test/acceptance/run.sh            (build first: scripts/build-core.sh)
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"     # repo root is two up
BIN="$ROOT/build/core"
LOTI="$BIN/loti"
LOTID="$BIN/lotid"
if [ ! -x "$LOTI" ] || [ ! -x "$LOTID" ]; then
  echo "acceptance: build first — scripts/build-core.sh (missing $BIN/{loti,lotid})" >&2
  exit 2
fi

WORK="$(mktemp -d)"
PASS=0 FAIL=0
declare -a PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

ok()  { echo "  ✓ $1"; PASS=$((PASS + 1)); }
bad() { echo "  ✗ $1"; FAIL=$((FAIL + 1)); }
assert_eq()   { [ "$2" = "$3" ]     && ok "$1 (= $2)"        || bad "$1: expected '$2' got '$3'"; }
assert_exit() { [ "$2" = "$3" ]     && ok "$1 (exit $2)"     || bad "$1: expected exit $2 got $3"; }
assert_ne()   { [ "$2" != "$3" ]    && ok "$1"               || bad "$1: '$2' should differ from '$3'"; }

wait_sock() { for _ in $(seq 1 50); do [ -S "$1" ] && return 0; sleep 0.1; done; return 1; }
field()     { awk -v k="$1:" '$1==k{print $2}'; }   # field <key>  (parses "key: value")

# ---------------------------------------------------------------------------
echo "── 1. restart survival ──────────────────────────────────────────────"
test_restart() {
  local home="$WORK/r" sock="$WORK/r.sock" store="$WORK/r.snap"
  "$LOTI" init --home "$home" >/dev/null
  "$LOTID" --key "$home/key" --port 7861 --control "$sock" --store "$store" \
     --clock-interval 0.25 >"$WORK/r1.log" 2>&1 &
  local pid=$!; PIDS+=("$pid")
  wait_sock "$sock" || { bad "daemon did not start"; return; }
  sleep 0.4
  "$LOTI" --control "$sock" publish "alpha"  >/dev/null
  "$LOTI" --control "$sock" publish "beta"   >/dev/null
  "$LOTI" --control "$sock" publish "gamma"  >/dev/null
  sleep 0.4
  local before; before=$("$LOTI" --control "$sock" status | field events)
  assert_eq "3 events before restart" "3" "$before"

  "$LOTI" --control "$sock" stop >/dev/null; wait "$pid" 2>/dev/null   # graceful stop → snapshot saved

  "$LOTID" --key "$home/key" --port 7861 --control "$sock" --store "$store" \
     --clock-interval 0.25 >"$WORK/r2.log" 2>&1 &
  pid=$!; PIDS+=("$pid")
  wait_sock "$sock" || { bad "daemon did not restart"; return; }
  local after; after=$("$LOTI" --control "$sock" status | field events)
  assert_eq "3 events survive restart" "3" "$after"

  # and the restored node can still prove one of its events, verifiably. (`last` is
  # daemon-ephemeral and not restored, so prove a concrete restored event hash.)
  sleep 0.6
  local eh; eh=$("$LOTI" --control "$sock" events | field event | head -1)
  "$LOTI" --control "$sock" prove bounds "$eh" --out "$WORK/r.loti" >/dev/null 2>&1
  "$LOTI" verify "$WORK/r.loti" >/dev/null 2>&1
  assert_exit "prove+verify a restored event" 0 $?
  "$LOTI" --control "$sock" stop >/dev/null; wait "$pid" 2>/dev/null
}
test_restart

# ---------------------------------------------------------------------------
echo "── 2. backup / restore ──────────────────────────────────────────────"
test_backup_restore() {
  local home="$WORK/k" sock="$WORK/k.sock" store="$WORK/k.snap"
  "$LOTI" init --home "$home" >/dev/null
  "$LOTID" --key "$home/key" --port 7862 --control "$sock" --store "$store" \
     --clock-interval 0.25 >"$WORK/k.log" 2>&1 &
  local pid=$!; PIDS+=("$pid")
  wait_sock "$sock" || { bad "daemon did not start"; return; }
  sleep 0.4
  "$LOTI" --control "$sock" publish "one" >/dev/null
  "$LOTI" --control "$sock" publish "two" >/dev/null
  local at_backup; at_backup=$("$LOTI" --control "$sock" status | field events)
  "$LOTI" --control "$sock" db backup --out "$WORK/k.backup" >/dev/null
  assert_eq "backup taken at 2 events" "2" "$at_backup"

  "$LOTI" --control "$sock" publish "three" >/dev/null   # diverge past the backup
  local diverged; diverged=$("$LOTI" --control "$sock" status | field events)
  assert_eq "diverged to 3 events" "3" "$diverged"

  "$LOTI" --control "$sock" db restore "$WORK/k.backup" >/dev/null
  local restored; restored=$("$LOTI" --control "$sock" status | field events)
  assert_eq "restore reverts to 2 events" "2" "$restored"
  "$LOTI" --control "$sock" stop >/dev/null; wait "$pid" 2>/dev/null
}
test_backup_restore

# ---------------------------------------------------------------------------
echo "── 3. multi-node notary proof (real UDP) ────────────────────────────"
test_multinode() {
  local aid bid
  aid=$("$LOTI" init --home "$WORK/a" | awk '/^node id:/{print $3}')
  bid=$("$LOTI" init --home "$WORK/b" | awk '/^node id:/{print $3}')
  "$LOTID" --key "$WORK/a/key" --port 7863 --control "$WORK/a.sock" --clock-interval 0.25 \
     --expiry 10 --peer "$bid:127.0.0.1:7864" >"$WORK/a.log" 2>&1 &
  PIDS+=("$!")
  "$LOTID" --key "$WORK/b/key" --port 7864 --control "$WORK/b.sock" --clock-interval 0.25 \
     --expiry 10 --peer "$aid:127.0.0.1:7863" >"$WORK/b.log" 2>&1 &
  PIDS+=("$!")
  wait_sock "$WORK/a.sock" && wait_sock "$WORK/b.sock" || { bad "daemons did not start"; return; }
  sleep 1.5                                          # let gossip form the cross-links
  local h; h=$("$LOTI" --control "$WORK/a.sock" publish "cross-node manuscript" | field event)
  sleep 2.0                                          # propagate; B's back-refs reach A's post-event clocks

  # B (an independent notary) proves A's event, then verifies it offline. A cross-node
  # discovery can abort while the bidirectional cross-links are still catching up, so
  # retry a few times (each attempt is a fresh, bounded discovery).
  local rc=1
  for _ in 1 2 3 4 5 6; do
    "$LOTI" --control "$WORK/b.sock" prove bounds "$aid:$h" --out "$WORK/notary.loti" >/dev/null 2>&1
    rc=$?; [ "$rc" -eq 0 ] && break
    sleep 1.0
  done
  assert_exit "B proves A's event over UDP" 0 "$rc"
  local ref
  ref=$("$LOTI" proof show "$WORK/notary.loti" --json 2>/dev/null | \
        python3 -c 'import sys,json;print(json.load(sys.stdin)["reference"]["node"])' 2>/dev/null)
  assert_eq "proof anchored in B's clock (notary)" "$bid" "$ref"

  "$LOTI" verify "$WORK/notary.loti" >/dev/null 2>&1
  assert_exit "verify notary proof offline" 0 $?

  # a tampered proof must be rejected
  cp "$WORK/notary.loti" "$WORK/bad.loti"
  python3 -c 'import sys;p=sys.argv[1];b=bytearray(open(p,"rb").read());b[len(b)//2]^=0xFF;open(p,"wb").write(b)' "$WORK/bad.loti"
  "$LOTI" verify "$WORK/bad.loti" >/dev/null 2>&1
  assert_exit "tampered proof rejected" 6 $?
}
test_multinode

# ---------------------------------------------------------------------------
echo "─────────────────────────────────────────────────────────────────────"
echo "acceptance: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
