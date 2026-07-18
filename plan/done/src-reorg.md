# Reorg — consolidate the C++ source under a top-level `src/`

Move `core/`, `adapters/`, `app/` into a new top-level `src/`, leaving the top level as
`bin/ doc/ plan/ scripts/ sim/ src/ test/`. `sim/` (the OMNeT++ scenario), `scripts/`, and
`plan/` stay put (user's call); only the C++ source moves.

**Why now:** the old pre-refactor `src/` baseline was retired (commit `6bbf1d0`), freeing the
`src/` name. This is a pure relocation — no behavior change; both targets must stay green.

## Target layout

```
bin/     doc/     plan/    scripts/   sim/    test/
src/
  core/        (was core/)      — the pure protocol library
  adapters/    (was adapters/)  — sim/ + os/ port adapters
  app/         (was app/)       — sim/ (OMNeT++ modules) + lotid/ + loti/
```

## Blast radius (surveyed)

Includes are all `-I`-relative (`#include "node.hpp"`, `"adapters/os/…"`, `"domain/…"`,
`"harness/…"`) — **no `#include` line changes**; only the `-I` dirs move. NED packages resolve
via the `-n` source path, not the directory, so **no `.ned` content changes** — only the `-n`
path in `setenv`. `makefrag` (no paths), `build-core.sh` (cmake resolves), and `bin/loti*`
(NED path via `setenv`) are unaffected.

### Steps

- [x] **Move:** `git mv core src/core`, `git mv adapters src/adapters`, `git mv app src/app`.
- [x] **`CMakeLists.txt` (root):** `core/*.cpp → src/core/*.cpp`; `app/{lotid,loti}/*.cpp →
      src/app/…`; `adapters/os/keystore.cpp → src/adapters/os/keystore.cpp`; include dirs
      `…/core → …/src/core` (loti_core PUBLIC) and `${CMAKE_CURRENT_SOURCE_DIR} → …/src`
      (lotid+loti PRIVATE, for `adapters/os/…`).
- [x] **`test/core/CMakeLists.txt`:** `…/adapters/os/keystore.cpp → …/src/adapters/os/…`;
      include `${CMAKE_SOURCE_DIR} → …/src` (test/doctest + test/ includes unchanged).
- [x] **`scripts/build-sim.sh`:** excludes `-X adapters/os -X app/lotid -X app/loti →
      -X src/adapters/os -X src/app/lotid -X src/app/loti`; includes `-I. -Icore → -Isrc
      -Isrc/core`; update the header comment.
- [x] **`setenv`:** `-n $LOTI_ROOT/app/sim → $LOTI_ROOT/src/app/sim`.
- [x] **`.gitignore`:** update the descriptive comment (`core/…`, `app/…` → `src/…`); anchored
      `/loti*` patterns unaffected.
- [x] **`src/core/{node,wire/packets,domain/types}.hpp`:** the deferred "faithful port of
      src/…" comments now dangle (old `src/` gone) — rewrite to say what the code is.
- [x] **Docs:** `README.md` (repo-layout + Simulation-model links), `doc/implementation.md`
      (path refs + the 14 old-src links), `doc/architecture.md` ("Proposed directory layout" +
      path refs), `doc/paper-vs-implementation.md` — retarget `core/`/`adapters/`/`app/` →
      `src/…` and clear the retired-`src/` references. `plan/done/` keeps its history.

**Verify:** ✅ `build-core.sh` green (loti_core + lotid + loti + tests); the OMNeT++ sim
rebuilds `libloti.so` from the same 9 `src/…` TUs (no production files swept in); `acceptance
run.sh` 10/10. Done in two commits: `6d1ef6b` (move + build config + core comments) and the docs
commit (path retargeting across README/cli/packet-format/architecture/implementation/
paper-vs-implementation; layout blocks; removed-file de-linking; implementation.md banner — every
link verified to resolve).

**Caveat (separate follow-up):** `doc/architecture.md` (Migration section), `doc/implementation.md`,
and `doc/paper-vs-implementation.md` carry **pre-refactor prose** from before M1–M4 — dated
framing ("today these are OMNeT++ message classes", "the current `Daemon.cc`"), resolved-issue
claims ("`Callback.h` … can be deleted" — already deleted), and old method names
(`Daemon::insertClockEvent`). The reorg only retargeted their *links*; a proper rewrite/retirement
of those three docs to the post-MVP architecture is its own task, not part of this structural move.

**Commit:** `6d1ef6b` "reorg: consolidate C++ source under src/" + the docs commit.
