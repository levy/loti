# Replace the full-overwrite snapshot with an incremental LMDB store

## Goal

Make LOTI's persistent storage **efficient and reliable** at the paper's GB/year scale.
Today the daemon re-serializes the entire in-RAM DAG and rewrites the whole file every 5 s
(and on shutdown). Replace that with an **incremental, crash-safe embedded store** so each new
event/clock-event is a small durable write, not a full rewrite.

> Paths below are post-reorg (everything lives under `src/`, `doc/` stays at the top level).

## Why LMDB

The persisted state (see `src/core/node.cpp:594` `snapshot()`) is an **append-mostly, immutable,
hash-linked log** (`all_events_`, `all_clock_events_`) plus **point-lookup-by-hash** indices and a
little **mutable routing state** (`neighbors_`, `destination_to_next_hop_`). That is an embedded
key-value workload, not a relational one.

LMDB fits best:

- **Reliable by design** — shadow-paging / copy-on-write with double-buffered meta pages, no WAL,
  no compaction, no background threads. A crash mid-write never corrupts committed data (on restart
  it falls back to the last valid meta page). Same store under OpenLDAP and Monero.
- **Efficient for this access pattern** — memory-mapped zero-copy reads; get-by-hash returns a
  pointer into the mmap. Single-writer/multi-reader MVCC matches the single daemon on its epoll
  reactor with no locking on our side. No compaction stalls (the thing that bites RocksDB).
- **Lean build** — two C files (`mdb.c`, `midl.c`) + `lmdb.h`, no external deps; vendored exactly
  like `test/doctest/doctest.h` and `src/core/hash/picosha2.hpp`.
- **License** — OpenLDAP Public License (permissive, BSD-like), compatible with this project.

Alternatives rejected: **RocksDB/LevelDB** (heavy dependency, compaction latency, overkill for a
single-writer daemon); **SQLite** (bulletproof, trivial to vendor, but per-row SQL overhead and no
zero-copy reads — the fallback if we later want ad-hoc/relational queries over the DAG); **roll our
own append-only log** (crash-safe fsync/torn-write/index-recovery is exactly where reliability bugs
hide — the user asked for reliable, so lean on a battle-tested store).

## Current state (facts, with references)

- `src/adapters/os/store.hpp:22` — `FileStore`: header-only, no interface, `save()` does
  `trunc` + temp-file + `std::rename` (full overwrite); `load()` reads the whole file.
- `src/app/lotid/lotid.cpp` — six call sites funnel through `FileStore` + `Node::snapshot()/restore()`:
  construct `store_.emplace(path)` (`:229`), `restore_from_store()` (`:243`), periodic
  `save_snapshot()` every 5 s (`kSnapshotIntervalNs`, `:687`; `schedule_snapshot()` `:680`),
  shutdown save (`:266`), `db_stat()` (`:664`), `db-backup`/`db-restore` via ad-hoc `FileStore`
  (`:390`, `:397`).
- `src/core/node.hpp:101` — `snapshot()`/`restore(blob)`; DAG members at `:170-178`; the three index
  maps are **derived** and rebuilt by `restore()` (`src/core/node.cpp:651-658`).
- `src/core/node.hpp` — Node exposes an **observer interface** the daemon already `override`s
  (`on_chain_completed`, `on_bounds_completed`, `on_order_completed` in `src/app/lotid/lotid.cpp:270+`).
  This is where we hang persistence hooks. *(Verify the exact base-class/interface name in Step 4.)*
- `src/core/wire/codec.hpp:15` — `wire::Writer`/`Reader` with `event()`, `clock_event()`, `refs()`,
  `blob()`, `u64()`, `bytes()`. Reuse these for LMDB values — no new encoding.
- `src/core/domain/types.hpp` — `EventHash = vector<u8>` (32-byte SHA-256 by convention),
  `Event{creator,hash,data,salt,referenced_events,signature}`, `LocalClockEvent : ClockEvent` adds
  `referencing_events`, `Neighbor{node_id,last_clock_event_hash}`, `NodeId = u64`.
- Build: no package manager; `find_package(OpenSSL)` linked into `lotid`/`loti`/tests only;
  `loti_core` stays pure. Sim build excludes the OS adapters via `opp_makemake -X src/adapters/os`
  (`scripts/build-sim.sh`), so an LMDB adapter under `src/adapters/os/` is auto-excluded from the sim;
  the vendored C engine under `third_party/` is excluded with `-X third_party`.

## Architecture

`loti_core` stays pure. LMDB is an **adapter** under `src/adapters/os/`. The Node gains a
**persistence listener** (a nullable pointer, set by the daemon, null in the sim), so the daemon
drives the store and the **simulation is completely unaffected** (no LMDB, no persistence — same as
today). See Step 4 for why a new listener is needed rather than an existing interface.

### Data model (one LMDB env, named sub-DBs)

| sub-DB (dbi) | key | value | notes |
| --- | --- | --- | --- |
| `meta` | short string keys (`"version"`, `"node_id"`) | `u64` / bytes | format/versioning + sanity |
| `events` | `u64` seq, **big-endian** | `wire::Writer::event(e)` | append order → sequential B+tree inserts |
| `event_index` | 32-byte hash (raw) | `u64` seq | get-by-hash → seq |
| `clock_events` | `u64` seq, big-endian | `clock_event(c)` + `refs(c.referencing_events)` | mirrors `snapshot()` encoding |
| `clock_index` | 32-byte hash (raw) | `u64` seq | |
| `unreferenced` | 32-byte hash (raw) | (empty / `u8`) | set membership; deleted when an event gets referenced |
| `neighbors` | `NodeId` u64 BE | `blob(last_clock_event_hash)` | mutable upsert |
| `routes` | `NodeId` dst u64 BE | `NodeId` next_hop u64 BE | mutable upsert |

Keying the logs by monotonic seq (not the random hash) keeps inserts sequential → minimal write
amplification. Big-endian keys make LMDB's default lexicographic compare match numeric order for
cursor iteration.

### Write path

Each logical append is **one durable write txn** that atomically writes the record + its hash-index
entry (+ referencing back-links / unreferenced-set edits). LMDB gives cross-sub-DB atomicity for
free. Neighbor/route changes are small upserts.

**Durability policy (recommended):** batch all appends produced within one reactor wakeup into a
single txn (amortizes the fsync), committed durably (default LMDB sync). This is stronger *and*
cheaper than the current 5-s full rewrite. Offer `MDB_NOSYNC` + a periodic `mdb_env_sync` timer as
an opt-in throughput knob; default stays fully durable because the user prioritized reliability.

### Read/startup path

At startup LMDB replays records once (a single sequential cursor scan — cheap, unlike a per-5-s
rewrite) to rebuild the in-RAM DAG. Node keeps its in-RAM model for now (Stage 1); we only fix the
**write** path. Post-load in-RAM state must be **identical** to today's `restore(blob)` result,
including the derived index rebuild.

## Implementation steps (each step = one commit)

Work in the dedicated `lmdb-store` worktree at `../loti-lmdb-store`, not the main checkout.

- [x] **Step 0 — Vendor LMDB + CMake, no behavior change.** Vendored `third_party/lmdb/{mdb.c,midl.c,
  midl.h,lmdb.h,LICENSE,CHANGES}` (v0.9.31, matches system `liblmdb0`). Added a `loti_lmdb` static lib
  (`project(... LANGUAGES C CXX)`, warnings silenced with `-w`), linked into `lotid` only. Added
  `-X third_party` to `scripts/build-sim.sh`. `scripts/build-core.sh` green, existing tests pass.
- [x] **Step 1 — `os::LmdbStore` skeleton + schema.** `src/adapters/os/lmdb_store.{hpp,cpp}`: opens
  the env (`MDB_NOSUBDIR` single file, 16 GiB default mapsize, 8 named sub-DBs), stamps/checks the
  `meta` format version, RAII close. doctest `test_lmdb_store.cpp`: fresh-open stamps v1, reopen reads
  it back. Wired into the test target (links `loti_lmdb`).
- [x] **Step 2 — Write path + read-back.** `LmdbStore::Batch` groups mutations into one durable
  commit: `append_event`/`append_clock_event` (assign a monotonic seq, write record + hash→seq index
  atomically, reusing the wire codec), `mark/clear_unreferenced`, `put_neighbor`, `put_route`. Seq
  counters seed from the logs on open and advance only on commit (aborted batch = no gap). Added the
  bulk readers `load_events`/`load_clock_events`/`load_unreferenced`/`load_neighbors`/`load_routes` +
  `event_count`/`clock_event_count`. doctest: committed round-trip (events, clock events, neighbor,
  route, unreferenced) survives reopen; aborted batch leaves nothing and no seq gap. (`remove_neighbor`
  skipped — the Node never removes a neighbor.)
- [x] **Step 3 — Node bulk-load from the store.** Added `Node::load(events, clock_events,
  unreferenced_hashes, neighbors, routes)` (`src/core/node.{hpp,cpp}`): moves the collections in,
  reconstructs `unreferenced_events_` as the hash-matching suffix of `all_events_`, and rebuilds the
  derived indices exactly as `restore()` did. `restore(blob)` now parses the snapshot and delegates to
  `load()` — blob format unchanged, so existing `state.snap` files still load. doctest: (a) `restore()`
  round-trips byte-identically (now via `load`); (b) a harness-built DAG persisted record-by-record into
  an LMDB store and loaded into a fresh Node yields a **byte-identical** `snapshot()`. Full core suite
  green (111 assertions).
- [ ] **Step 4 — Node persistence listener.** There is **no** general Node observer to reuse:
  `ChainCallback`/`BoundsCallback`/`OrderCallback` (`src/core/node.hpp:27-45`) are per-discovery result
  callbacks the daemon passes into `discover_*`, not lifecycle hooks. So add a dedicated
  `PersistenceListener` interface (`on_event_appended(const Event&)`,
  `on_clock_event_appended(const LocalClockEvent&)`, `on_neighbor_changed`, `on_route_changed`) plus a
  nullable `Node` pointer set via a setter by the daemon (null in the sim → **zero sim change**). Call
  it from the private append choke points `insert_event`/`insert_clock_event`
  (`src/core/node.hpp:153-154`) and the `add_neighbor`/`learn_route` mutators so it catches appends from
  every trigger (app publish, the internal clock timer, network intake).
- [ ] **Step 5 — Swap the daemon onto LmdbStore.** Replace `FileStore store_` with `LmdbStore`;
  `restore_from_store()` → `replay()`; the daemon's hook impls drive the write path;
  **retire the 5-s full-snapshot timer** (replace with a periodic `mdb_env_sync` flush, or drop it if
  committing durably per batch). Rework `db_stat()` (report env size via `mdb_env_stat`/`mdb_stat`
  instead of `snapshot().size()`).
- [ ] **Step 6 — Backup/restore + migration.** `db-backup` → `mdb_env_copy2(..., MDB_CP_COMPACT)`
  (consistent hot copy). `db-restore` → replace the env dir (or import). Keep `snapshot()`/`restore()`
  as a **portable blob export/import** format (this also gives a one-time importer for an existing
  `state.snap`: `loti db restore state.snap` → `restore(blob)` → re-persist into LMDB). Retire
  `FileStore` from the hot path but keep the blob file I/O for export.
- [ ] **Step 7 — Config & robustness.** `--store` now names an **LMDB env directory** (or use
  `MDB_NOSUBDIR` for a single file to keep the flag's file-path shape). Add `--store-mapsize`
  (default e.g. 16 GiB — virtual only, backed as used). Handle `MDB_MAP_FULL` by growing (reopen with
  a larger mapsize). Update the `loti init` suggestion line and the generated `store=` hint.
- [ ] **Step 8 — Tests + docs.** doctest: incremental round-trip, crash-consistency (truncate/kill
  mid-batch → last committed state intact, no corruption), backup→restore equality, map-full growth.
  Update `doc/architecture.md` Store row (`:140`) and the `src/adapters/os/store.hpp` header comment.
  Move this plan to `plan/done/`.

## Testing

- doctest under `test/core` (add LMDB to that target's link line, temp dirs for envs).
- Crash-consistency is the key reliability test: open env, begin a batch, simulate a crash before/at
  commit, reopen → invariant "committed state intact, uncommitted batch absent, no corruption."
- Sim tests must be **unchanged** (no new failures) — the sim links no LMDB and persists nothing.
- Verify end-to-end against a running `lotid`: publish events, kill -9, restart → all events + clock
  chain survive; `loti db stat` counts match.

## Migration / backward compatibility

- Old `state.snap` blobs remain importable via `restore(blob)` (kept for export/import), so
  `loti db restore old.state.snap` migrates a node onto LMDB in one step.
- `db backup`/`db restore` semantics preserved (backup = consistent copy; restore = load), just
  backed by `mdb_env_copy2` instead of a raw file copy.

## Risks & open questions

- **Persistence listener vs. full Store port.** Stage 1 adds a dedicated nullable persistence
  listener (lowest risk, sim untouched — no general Node observer exists to reuse). Promoting `Store`
  to a real 7th entry in `NodePorts` (as `doc/architecture.md:140` sketches) is deferred to Stage 2 —
  only needed if the sim ever wants persistence.
- **Durability window.** Default to per-batch durable commit; `MDB_NOSYNC` + periodic sync is opt-in.
  Decide the batch boundary (per reactor wakeup vs. a short flush timer).
- **mapsize** must be set up front; pick a generous default and implement `MDB_MAP_FULL` growth.
- **`EventHash` is variable-length** (`vector`, 32 by convention) — index keys use the raw bytes;
  keep the daemon's existing `size() != 32` guards in mind.
- Confirmed: no general observer exists (see Step 4); `create_clock_event` (`src/core/node.hpp:88`) is
  public but is also the body of the internal clock timer, so the daemon can't intercept
  timer-created clock events without the listener.

## Out of scope (future / Stage 2)

- Dropping the in-RAM DAG and serving get-by-hash reads straight from LMDB (only if RAM becomes the
  bound at GB/year). The sub-DB schema above already supports it.
- Promoting `Store` to a first-class `NodePorts` port with a sim in-memory adapter.

## Progress log

- **Step 0 done** — LMDB 0.9.31 vendored under `third_party/lmdb/` (fetched from the upstream
  `LMDB_0.9.31` tag; version matches the host's `liblmdb0`). `loti_lmdb` static lib compiles the C
  engine with `-w`; `project()` now enables `C`. Linked into `lotid` only; `-X third_party` keeps it
  out of the sim. `scripts/build-core.sh`: configure + build + ctest all green, no new warnings.
- **Step 1 done** — `LmdbStore` (`src/adapters/os/lmdb_store.{hpp,cpp}`) opens/creates the env and the
  eight named sub-DBs, stamps the `meta` format version on create and verifies it on reopen, throws on
  any LMDB error or version mismatch, and RAII-closes. Env is a single file (`MDB_NOSUBDIR`), 16 GiB
  default mapsize. Tested in `test_lmdb_store.cpp` (open → reopen); test target now links `loti_lmdb`.
- **Design correction (found in Step 1):** no general Node observer exists — the `*Callback` classes
  are per-discovery result callbacks. Step 4 will add a dedicated nullable `PersistenceListener`
  driven from `insert_event`/`insert_clock_event`. Plan Step 4 / risks updated accordingly.
- **Step 2 done** — `LmdbStore::Batch` write txn (append event/clock event with an atomic hash index,
  unreferenced set, neighbor/route upserts), seq counters that seed on open and advance only on
  commit, plus the `load_*` bulk readers and entry counts. Round-trip and abort-no-gap tests pass
  (21 assertions, no warnings). Step 3 narrowed to the Node-side load API since the readers now exist.
- **Step 3 done** — `Node::load(...)` populates the DAG + rebuilds the derived indices; `restore(blob)`
  now parses the blob and delegates to it (format unchanged, backward compatible). `unreferenced_events_`
  reconstructed as the hash-matching suffix of `all_events_`. Two new tests (restore round-trip; harness
  DAG → LMDB → load → byte-identical snapshot) pass; full core suite 111 assertions green, no warnings.
  First change to `loti_core`; every pre-existing core test still passes.
