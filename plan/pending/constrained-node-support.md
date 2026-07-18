# Constrained-node support (Raspberry Pi Zero 2 W, incl. 32-bit)

## Goal

Make `lotid` run well as a long-lived node on a **Raspberry Pi Zero 2 W** (quad-core
Cortex-A53, **512 MB RAM**, microSD), and build cleanly for **32-bit** targets too
(Pi Zero / Zero W, ARMv6). Four independent workstreams:

- **A. 32-bit correctness** — the 16 GiB default mapsize overflows `size_t` on 32-bit.
- **B. Cross-compilation** — build for aarch64 (Zero 2 W) and armv6 (Zero/W) from a dev box.
- **C. Reduce syncing** — cut microSD write/fsync load *without* raising the clock interval.
- **D. RAM-bounded node** — stop holding the whole DAG in RAM; 512 MB must not OOM over time.
- **E. Docs & landing page** — document that a node runs on a Pi Zero (how / why / limits), in the
  project documentation and the GitHub Pages landing page.

Recommended sequence: **A → B → C → D → E** (A is a quick correctness fix; B lets us build/run
the 32-bit and aarch64 targets — which also exercises A; C is an independent SD-wear win; D is the
large architectural change and benefits from A–C being in place; E documents the result). D is by
far the biggest.

> Paths are post-reorg (`src/…`). The current live store is the LMDB work in
> [plan/done/lmdb-store.md](../done/lmdb-store.md); this plan builds on it.

---

## Part A — 32-bit correctness

`kDefaultMapSize = std::size_t{16} << 30` ([src/adapters/os/lmdb_store.hpp:36](../../src/adapters/os/lmdb_store.hpp))
is `2^34`, which wraps to **0** in a 32-bit `size_t`. Even a correct 16 GiB can't be `mmap`'d on
32-bit (per-process address space ≈ 2–3 GiB). The `--store-mapsize` parse
([src/app/lotid/lotid.cpp:820](../../src/app/lotid/lotid.cpp), `atof × 2³⁰` → `size_t`) also
overflows for values ≥ 4 on 32-bit.

- [x] **A1.** Make `kDefaultMapSize` word-size-aware: `sizeof(std::size_t) >= 8 ? (16<<30) : (1<<30)`
  (16 GiB on 64-bit, 1 GiB on 32-bit — safely under the 32-bit mmap ceiling). *Done: also added
  `kMaxMapSize` (0 on 64-bit = uncapped, 1.5 GiB on 32-bit) as the single source of truth for the ceiling.*
- [x] **A2.** Validate/clamp `--store-mapsize`: compute in `double`, reject non-positive, and on 32-bit
  clamp to a safe max (~1.5 GiB) with a clear warning; guard the `double → size_t` cast against overflow.
  *Done: `parse_mapsize_gib()` in lotid.cpp; clamps to `LmdbStore::kMaxMapSize`.*
- [x] **A3.** Cap `grow_map()` on 32-bit: don't double past the safe ceiling; when the map cannot grow
  further, surface a clear "store full (32-bit address-space limit)" error instead of a raw LMDB code.
  *Done: `grow_map()` throws `LmdbStoreFull`; on 64-bit (`kMaxMapSize==0`) behavior is unchanged.*
- [x] **A4.** Tests/docs: a doctest asserting the default is non-zero and `≤` a sane bound; document the
  **32-bit lifetime ceiling** — an LMDB env is capped at the mmap size (~2 GiB on 32-bit), so a 32-bit
  node's *total* stored DAG is bounded (~1–2 years at the paper's GB/year). The Zero 2 W (64-bit) has no
  such ceiling. *(Real 32-bit compile-and-run validation comes from Part B.)* *Done: doctest +
  note in doc/architecture.md.*

**Risk:** low. Self-contained; no behavior change on 64-bit.

---

## Part B — Cross-compilation

`loti_core` + LMDB (vendored C) cross-compile cleanly; the fiddly bit is **OpenSSL** (the keystore
links `OpenSSL::Crypto`) needing a target `libcrypto`.

- [x] **B1.** Add CMake toolchain files under `cmake/`: `toolchain-aarch64-linux-gnu.cmake` (Zero 2 W,
  64-bit) and `toolchain-armv6-linux-gnueabihf.cmake` (Zero/W: `-march=armv6 -mfpu=vfp -mfloat-abi=hard`).
  Each sets `CMAKE_SYSTEM_NAME/PROCESSOR`, the cross `CC/CXX`, sysroot, and `CMAKE_FIND_ROOT_PATH`.
  *Done: both files parse cleanly in CMake (only the absent cross-gcc blocks configure).*
- [x] **B2.** Target OpenSSL: point `CMAKE_FIND_ROOT_PATH` at a Pi sysroot (rsynced from the device) or
  multiarch cross packages (`libssl-dev:arm64` / `:armhf`) so `find_package(OpenSSL)` resolves the target
  lib. Document how to obtain the sysroot. *Done: toolchains support both a `LOTI_SYSROOT` pure-sysroot
  mode and a multiarch mode (`CMAKE_LIBRARY_ARCHITECTURE` + FIND_ROOT_PATH BOTH); sysroot acquisition
  walkthrough lives in doc/embedded.md (Part E).*
- [x] **B3.** `scripts/build-cross.sh <aarch64|armv6>` — configure with the toolchain into `build/<arch>/`
  and build `lotid`/`loti`. (Host can't run a cross build's `ctest`.) *Done: Release build of the two
  runtime targets; detects a missing cross compiler with an install hint.*
- [ ] **B4.** *(Optional)* `qemu-user-static` to run the cross-built unit tests on the host under emulation
  (nice for CI); note it as optional. *Documented in doc/embedded.md as an optional path; not scripted.*
- [ ] **B5.** Verify: build both targets; the **armv6** build compiles the 32-bit path from Part A. Run the
  binaries on the Pi (or under qemu) — smoke `lotid --store … --store-mapsize 1` + `loti publish`.
  *DEFERRED — the cross toolchains, multiarch libssl, and a Pi/qemu need root (`apt`), unavailable in
  this sandbox. The toolchain files + script are complete and CMake-validated; run B5 on a dev box with
  the packages installed (steps in doc/embedded.md).*
- [x] **B6.** Capture the concrete build/deploy steps (sysroot acquisition, toolchain invocation, target
  OpenSSL) — these become the "how to build" part of the embedded guide in **Part E**. *Done in
  doc/embedded.md (Part E); inline usage also in each toolchain file and build-cross.sh.*

**Risk:** medium (OpenSSL sysroot friction). No change to on-target runtime behavior.

---

## Part C — Reduce syncing (microSD wear)

Today each change is one durable txn = one `fsync` (~1/s at the default clock interval). The lever
that avoids raising `--clock-interval` is LMDB's `MDB_NOSYNC` + a periodic flush: the OS coalesces
dirty pages and writes them back lazily, so **actual SD writes drop** (less write amplification) and
fsync-forced flushes become one-per-interval. LMDB stays **crash-consistent** either way — the
tradeoff is only a bounded *durability window* (a crash in lazy mode may lose the last `<interval>`
of commits, never corrupt).

- [x] **C1.** `LmdbStore`: add a sync-policy option to the ctor (open with `MDB_NOSYNC` when "lazy").
  Default stays **safe** (fsync per commit). *Done: `SyncPolicy` enum + `sync_policy()` accessor.*
- [x] **C2.** Daemon: `--store-sync-interval <seconds>` — `0` = safe/per-commit; `>0` = `MDB_NOSYNC`
  + a reactor timer calling `store.sync()` every interval. Clean shutdown + the `save` command already
  force a final `sync()`. *Done: `schedule_store_sync()` re-arms a scheduler timer; smoke-tested (lazy
  publish → clean shutdown → reopen persists).*
- [ ] **C3.** *(Optional, complementary)* per-reactor-wakeup **write batching** — group all of one
  wakeup's changes into a single txn (needs a small reactor idle hook). Cuts the commit/fsync count even
  in safe mode. Deferred from the LMDB plan; include if the nosync win isn't enough. *Not done — the
  `MDB_NOSYNC` win is the primary lever; batching stays deferred (would need a reactor idle hook).*
- [x] **C4.** Tests/docs: verify lazy mode survives a clean stop/restart (synced on shutdown) and document
  the durability-window tradeoff. Note `MDB_NOMETASYNC` (fsync data, not meta) as a middle ground.
  *Done: doctest for lazy round-trip after sync; durability note in doc/architecture.md (+ operator
  guidance to land in doc/embedded.md, Part E).*

**Risk:** low–medium. Default behavior unchanged; lazy mode is opt-in with a documented tradeoff.

---

## Part D — RAM-bounded node (the large one)

**Problem.** Stage 1 keeps the *entire* DAG in RAM — `all_events_`, `all_clock_events_`, and the three
index maps ([node.hpp:170-178](../../src/core/node.hpp)) — in addition to persisting it, and the
retention rule never prunes. At the default clock interval that is tens of MB/day → a 512 MB node OOMs
in **weeks**. The user wants true bounded RAM *without* leaning on a longer clock interval.

**Design.** Push the DAG out of the Node and read it through the store, whose LMDB mmap is backed by the
**OS page cache** — so RAM is automatically bounded by available memory (cold pages evict under pressure,
hot pages stay; reads remain zero-copy). This is the deferred "Stage 2" and it **promotes the store to a
real port** and **subsumes the `PersistenceListener`**: the Node reads *and* writes through a `Store`
port — LMDB in production, an in-memory impl in the simulation.

Read sites to reroute (from `node.cpp`): `find_clock_event_index`→`all_clock_events_[idx]` walks
(`add_local_lower_bound`/`upper_bound`, `extend_*_for_neighbor`), `find_event_index`→`all_events_[idx]`,
the `event_hash_to_referencing_event_index_.equal_range` reverse lookup, and `.back()` for the latest.
Access is by-hash, by **dense seq** (seqs are gap-free), and a reverse range — all serviceable by LMDB.

- [ ] **D1.** Persist the **reverse index**: an `MDB_DUPSORT` sub-DB `referencing` (key = referenced hash,
  values = clock-event seqs), maintained in `append_clock_event`. Replaces the in-RAM
  `event_hash_to_referencing_event_index_` multimap. (maxdbs 7 → 8; format-version bump.)
- [ ] **D2.** Define an abstract `Store` **port** in `src/core/ports/store.hpp` (keeps `loti_core` pure):
  writes (`append_event`, `append_clock_event`, `update_clock_event`, `put_neighbor`, `put_route`) + reads
  (`event_by_hash`/`_by_seq`, `clock_event_by_hash`/`_by_seq`, `clock_events_referencing(hash)`,
  `event_count`, `clock_event_count`, latest).
- [ ] **D3.** Implement the port in `LmdbStore` (adapters/os) — most reads exist; add by-seq getters + the
  reverse-range query (D1).
- [ ] **D4.** Implement an `InMemoryStore` (adapters/sim or shared) for the simulation — the DAG vectors/maps
  move here; state still dies with the run.
- [ ] **D5.** Rework the `Node`: add `Store&` to `NodePorts`; drop `all_events_`/`all_clock_events_`/the three
  index maps; reroute every read site through the port; the `referencing_events` update becomes a store
  read-modify-write (`update_clock_event`). Keep only small working state (neighbors/routes/unreferenced
  tail — or move those too). Update all `NodePorts` construction sites (daemon, sim `Daemon`,
  `test/harness/world.hpp`).
- [ ] **D6.** Retire the `PersistenceListener` seam (LMDB-plan Step 4) — writes now flow through the `Store`
  port; the daemon injects `LmdbStore`, the sim injects `InMemoryStore`.
- [ ] **D7.** Migration: an old on-disk store lacks the `referencing` sub-DB → on first open of a pre-D
  format, rebuild the reverse index by scanning `clock_events` (guard by the format-version bump).
- [ ] **D8.** Tests: store read-API + reverse-index unit tests; the existing discovery/chain tests pass
  unchanged (now store-backed); acceptance suite green; a **RAM-bound soak** (large N, assert RSS stays
  bounded while the DB grows) — ideally run on the Pi.

**Risk:** **high** — touches `loti_core` (new port), the simulation (new `InMemoryStore`, `NodePorts`
change), reworks the persistence seam built in the LMDB plan, and changes on-disk format (migration).
Cold reads page-fault from SD (slow for *old*-event discoveries only; hot path stays in page cache).
Consider doing D as its own follow-up plan once A–C land.

**32-bit caveat (ties to A):** even RAM-bounded, a 32-bit node's LMDB env is mmap-capped at ~2 GiB, so
its *total* DAG lifetime is bounded regardless of RAM. The Zero 2 W (64-bit) is the real long-life target.

---

## Part E — Documentation & landing page: "runs on a Raspberry Pi"

Tell users — in the project documentation **and** the GitHub Pages landing page ([index.html](../../index.html))
— that a LOTI node runs on a Raspberry Pi Zero: the how, the why, and the honest limits. Write it to
match what is **actually implemented** at the time — if D is still pending, state the RAM-growth caveat
plainly rather than implying bounded memory.

- [ ] **E1.** New `doc/embedded.md` — the full guide:
  - **Why it fits** — lean C++20 core + vendored LMDB, a single-threaded epoll reactor, no runtime deps
    beyond libc/OpenSSL, a small static-ish binary; a node is cheap, low-power, always-on; proofs are
    portable and verified offline.
  - **How** — cross-compile (Part B) or build on-device; recommended Pi flags (`--store-mapsize`,
    `--store-sync-interval`); put `--store` on a high-endurance SD or a USB SSD; key/config via `loti init`.
  - **Targets** — Zero 2 W (aarch64, the recommended long-life target) vs Zero / Zero W (ARMv6, 32-bit).
  - **Limits (honest)** — 512 MB RAM (bounded once Part D lands; until then, note the growth caveat);
    the 32-bit ~2 GiB mmap/DAG ceiling; microSD wear + the sync tradeoff (Part C); WiFi-only on Zero W;
    cold-read latency paging old events from SD.
- [ ] **E2.** README `## Building and running`: add a short "Running on a Raspberry Pi (embedded)"
  subsection linking to `doc/embedded.md` (the build mechanics from B6 live in the guide, not inline).
- [ ] **E3.** Landing page (`index.html`): a concise callout in the **"What it takes"** part of the
  `#costs` section — "a node runs on a ~$15 Raspberry Pi Zero" as a selling point (cheap / low-power /
  always-on), matching the page's tone, linking to the guide. Keep it self-contained (single static file).
- [ ] **E4.** Cross-check every figure/claim against the acceptance suite and on-Pi measurements (binary
  size, steady-state RSS, DB growth rate) so the guide's numbers are measured, not aspirational.

**Risk:** low. Public-facing — keep claims honest and tied to measured behavior. Finalize after A–D so
the limits are accurate.

## Out of scope / notes

- Raising `--clock-interval` is deliberately **not** the RAM strategy (user constraint); it remains a
  separate operator tuning knob.
- `MDB_WRITEMAP`/`MDB_MAPASYNC` (writable mmap, even less overhead) is a possible further C-style knob with
  a larger crash-loss window — not planned unless C's nosync proves insufficient.
