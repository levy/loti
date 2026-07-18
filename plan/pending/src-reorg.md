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

- [ ] **Move:** `git mv core src/core`, `git mv adapters src/adapters`, `git mv app src/app`.
- [ ] **`CMakeLists.txt` (root):** `core/*.cpp → src/core/*.cpp`; `app/{lotid,loti}/*.cpp →
      src/app/…`; `adapters/os/keystore.cpp → src/adapters/os/keystore.cpp`; include dirs
      `…/core → …/src/core` (loti_core PUBLIC) and `${CMAKE_CURRENT_SOURCE_DIR} → …/src`
      (lotid+loti PRIVATE, for `adapters/os/…`).
- [ ] **`test/core/CMakeLists.txt`:** `…/adapters/os/keystore.cpp → …/src/adapters/os/…`;
      include `${CMAKE_SOURCE_DIR} → …/src` (test/doctest + test/ includes unchanged).
- [ ] **`scripts/build-sim.sh`:** excludes `-X adapters/os -X app/lotid -X app/loti →
      -X src/adapters/os -X src/app/lotid -X src/app/loti`; includes `-I. -Icore → -Isrc
      -Isrc/core`; update the header comment.
- [ ] **`setenv`:** `-n $LOTI_ROOT/app/sim → $LOTI_ROOT/src/app/sim`.
- [ ] **`.gitignore`:** update the descriptive comment (`core/…`, `app/…` → `src/…`); anchored
      `/loti*` patterns unaffected.
- [ ] **`src/core/{node,wire/packets,domain/types}.hpp`:** the deferred "faithful port of
      src/…" comments now dangle (old `src/` gone) — rewrite to say what the code is.
- [ ] **Docs:** `README.md` (repo-layout + Simulation-model links), `doc/implementation.md`
      (path refs + the 14 old-src links), `doc/architecture.md` ("Proposed directory layout" +
      path refs), `doc/paper-vs-implementation.md` — retarget `core/`/`adapters/`/`app/` →
      `src/…` and clear the retired-`src/` references. `plan/done/` keeps its history.

**Verify:** `scripts/build-core.sh` (core+lotid+loti+tests green); the OMNeT++ sim rebuilds
(`build-sim.sh`) and `libloti.so` links; `test/acceptance/run.sh` 10/10.
**Commit:** "reorg: consolidate C++ source under src/". Then offer to ff `master`.
