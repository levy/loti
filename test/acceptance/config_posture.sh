#!/usr/bin/env bash
#
# Config / identity posture (hardening plan, Phase 2.5 + the 2.2 init-dir leftover):
#   - running unsigned (--id) prints a LOUD startup warning (no cryptographic identity);
#     running signed (--key) does not.
#   - `loti init` leaves its home directory private (0700), even if it already existed
#     with looser permissions.
#
#   Usage:  test/acceptance/config_posture.sh     (build first: scripts/build-core.sh)
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$ROOT/build/core"
LOTI="$BIN/loti"
LOTID="$BIN/lotid"
if [ ! -x "$LOTI" ] || [ ! -x "$LOTID" ]; then
  echo "config_posture: build first — scripts/build-core.sh (missing $BIN/{loti,lotid})" >&2
  exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
PASS=0 FAIL=0
ok()  { echo "  ✓ $1"; PASS=$((PASS + 1)); }
bad() { echo "  ✗ $1"; FAIL=$((FAIL + 1)); }

# --- unsigned startup warning ---
"$LOTID" --id 1 --port 28931 --control "$WORK/u.sock" </dev/null >"$WORK/u.out" 2>&1 &
UP=$!; sleep 0.5; kill "$UP" 2>/dev/null; wait "$UP" 2>/dev/null
grep -qiE "warning.*unsign|unsigned.*no cryptographic" "$WORK/u.out" \
  && ok "unsigned start prints a loud warning" || { bad "unsigned start has no warning"; cat "$WORK/u.out"; }

# --- signed start does NOT warn ---
"$LOTI" --home "$WORK/h1" init >/dev/null 2>&1
"$LOTID" --key "$WORK/h1/key" --port 28932 --control "$WORK/s.sock" </dev/null >"$WORK/s.out" 2>&1 &
SP=$!; sleep 0.5; kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null
grep -qiE "warning.*unsign" "$WORK/s.out" && bad "signed start wrongly warns" || ok "signed start does not warn"

# --- init dir is 0700 (fresh) ---
mode() { stat -c '%a' "$1" 2>/dev/null; }
[ "$(mode "$WORK/h1")" = 700 ] && ok "fresh init home is 0700" || bad "fresh init home is $(mode "$WORK/h1"), want 700"

# --- init tightens an EXISTING loose dir ---
mkdir -p "$WORK/h2"; chmod 0777 "$WORK/h2"
"$LOTI" --home "$WORK/h2" init >/dev/null 2>&1
[ "$(mode "$WORK/h2")" = 700 ] && ok "init tightens an existing 0777 home to 0700" || bad "existing home stayed $(mode "$WORK/h2"), want 700"

echo "config_posture: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
