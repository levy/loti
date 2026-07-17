# MVP — Shared-Core Simulation + Command-Line Node

A staged plan to reach a **minimum viable product** that runs from **one codebase** both as
the OMNeT++ simulation (for large-network experiments and statistics) and as a real
command-line node (`lotid` daemon + `loti` CLI). It executes the migration path in
[architecture.md](../../doc/architecture.md) and delivers the MVP command set in
[cli.md](../../doc/cli.md).

## Definition of done

- **One `loti-core`** library holds the protocol (DAG, events/clock events, hashing +
  canonical serialization, the three discoveries, chain validation, proof build/verify) with
  **no OMNeT++/INET/OS dependency**.
- **The OMNeT++ simulation runs entirely on `loti-core`**, producing the same statistics as
  today (byte-accurate hashes/sizes, comparable completion rates).
- **A real node works end-to-end**: two or more `lotid` instances peer over UDP, publish
  events, and answer discoveries; a user drives them with `loti`.
- **Proofs are the payoff**: `loti prove bounds/order` emits a portable artifact that
  `loti verify` checks **offline** (no daemon, no network), and the reference node's
  signature validates.
- State **survives restart** (persistent store) and can be backed up.

## In scope (MVP command set, from cli.md)

`init` · `node start/stop/status` · `config` · `key gen/show` + event/clock **signing** ·
`peer add/ls` (live peering) · `publish` · `event show` · `bounds`/`order`/`chain`
`--reference` · `prove bounds/order` + **`verify`** · `db stat/backup` · persistence.

## Out of scope (deferred, not MVP)

- The **probabilistic beam search** — the MVP keeps the current **width-1** deterministic walk
  ([dynamic-discovery.md](../../doc/dynamic-discovery.md)); no multi-path/redundant discovery.
- Incentives / value transfer; key rotation; event sharing/import; multi-chain or
  multi-reference proofs; dynamic overlay beyond basic peer add/remove; `db gc/verify/export`;
  Prometheus metrics; QUIC.
- These map to the "Later" items and roadmap in
  [paper-vs-implementation.md](../../doc/paper-vs-implementation.md).

## Guiding constraints

From [architecture.md](../../doc/architecture.md), non-negotiable throughout:

1. **Dependency rule** — `loti-core` compiles with **no** OMNeT++/INET/OS headers; adapters
   depend on core, never the reverse. No `#ifdef SIMULATION` in the core.
2. **No ambient state** — no globals/singletons/`static` mutable data (thousands of `Node`s
   share one sim process).
3. **No blocking in the core**; single-threaded, event-driven; effects go through ports.
4. **One serializer** — core owns the canonical bytes; both transports carry them, so sim
   byte-accounting equals production.
5. **Soundness stays free** — `validateEventChainDiscoveryResult` runs on every result
   regardless of runtime.

## Milestones

- **M1 — Simulation on the shared core** (end of Stage 2): the OMNeT++ example runs on
  `loti-core` with unchanged statistics. *The simulation half of the MVP is done.*
- **M2 — Real node, loopback** (end of Stage 3): two `lotid` processes on localhost exchange
  clock notifications and complete a discovery, reproducing sim behavior.
- **M3 — Proofs end-to-end** (end of Stage 6): publish → prove → verify offline works; signed.
- **M4 — MVP acceptance** (end of Stage 7): restart-survivable, backed up, both targets green,
  docs updated.

## How to work this plan

Per the project's plan conventions: implement in a dedicated git worktree, **mark each task
done here as you land it**, **commit per step**, record decisions in the
[Decisions log](#decisions-log), and `git mv` this file to `plan/done/` at M4. Keep the
OMNeT++ example building and green after every stage.

---

## Stage 0 — Scaffolding & build split  ✅ DONE (2026-07-17)

**Goal:** a `loti-core` library target that builds with no OMNeT++, a test harness, and the
directory layout — without changing simulation behavior.

- [x] Create the layout from architecture.md: `core/{domain,hash,dag,discovery,validate,ports}`,
      `adapters/{sim,os}`, `app/{sim,lotid,loti}`, `test/{core,harness}`. `sim/` and `src/` left
      untouched. Empty module homes carry a `.gitkeep` so git tracks the tree.
- [x] Add a CMake build producing `loti-core` (static lib) and wire a header-only test
      framework for `test/core`. Chose **doctest** (vendored single header at
      `test/doctest/doctest.h`, v2.4.11); C++20; `add_test`/`ctest` integration.
- [x] Keep the existing `opp_makemake` flow for the sim; added `scripts/build-core.sh` which
      configures + builds + `ctest`s the core with no OMNeT++/INET on the path.
- [x] Added a placeholder core (`core/loti_core.{hpp,cpp}` — `library_id()` /
      `protocol_version`) + a passing smoke test (`test/core/test_smoke.cpp`).

**Verify:** ✅ `scripts/build-core.sh` configures, builds `libloti_core.a`, and runs the test
suite green (`1/1 passed`) under plain g++ 15.2 — no OMNeT++/INET include paths. The sim build
is unaffected because the stage is purely additive: the only modified tracked file is
`.gitignore` (added `/build/`); `src/`, `sim/`, and the OMNeT++ Makefile flow are untouched.
**Commit:** "Added loti-core library target and test harness (MVP Stage 0)".

**Notes discovered while building:**
- The CMake build (`CMakeLists.txt` at repo root) and the OMNeT++ `opp_makemake` flow coexist:
  different files (`CMakeLists.txt` vs the generated root `Makefile`), different build dirs.
  Run `make` for the simulation, `cmake`/`scripts/build-core.sh` for the core/product.
- Toolchain present here: cmake 4.2, g++ 15.2, clang++ 23. OMNeT++/INET are **not** installed,
  which is fine — that is exactly the environment `loti-core` must build in.

## Stage 1 — De-OMNeT++ the domain model & serialization

**Goal:** protocol data types and hashing live in `loti-core` as plain C++, byte-identical to
today. *Highest-risk stage — guard with golden tests.*

- [ ] Port `Data.msg` types to plain structs in `core/domain/` (`Event`, `ClockEvent`,
      `LocalClockEvent`, `EventReference`, `EventChain`, discovery records, `Neighbor`).
- [ ] Reserve an **optional `signature` field** on `Event`/`ClockEvent` now (excluded from the
      hash; it covers the hash) so adding real signing in Stage 4 does **not** change hashes.
- [ ] Move `calculateEventHash`/`calculateClockEventHash` and all `calculate*Size` off INET's
      `MemoryOutputStream` onto a core serializer; keep `picosha.h` as-is.
- [ ] Reduce the `Packet.msg` chunk types to a **thin sim-transport wrapper** carrying the
      core's canonical bytes (e.g. a `BytesChunk`, or a `FieldsChunk` whose length equals the
      core encoding).
- [ ] Golden tests: fixed input (fixed salt) → known SHA-256; core serializer length equals the
      old `calculate*Size`.

**Verify:** golden hash/size tests pass; the OMNeT++ example produces **identical**
`clockEventsFileLength`/`eventsFileLength` and packet-length histograms as before this stage.
**Commit:** "core: plain-struct domain model + canonical serializer (byte-identical)".

## Stage 2 — Ports + extract the `Node` core  → **M1**

**Goal:** the protocol logic moves into `core/node`, driven by ports; the simulation runs on
the shared core.

- [ ] Define port interfaces in `core/ports/`: `Clock`, `Scheduler`, `Transport`, `Rng`,
      `Store`, `Signer` (+ shared `Hasher`), `Config`, `Telemetry`.
- [ ] Move `Daemon.cc` protocol methods into `core/node` (`Node`), taking ports by reference:
      `publishEvent`, `discoverEvent{Chain,Bounds,Order}`, the four `add*/extend*` primitives,
      routing, validation, `onPacketReceived`, `onTimer`, `addNeighbor`/`learnRoute`.
- [ ] Implement **simulation adapters** in `adapters/sim/`: `SimClock`(`simTime`),
      `SimScheduler`(`scheduleAt`/self-msgs), `SimTransport`(`UdpSocket`), `SimRng`(seeded
      `intuniform`), `InMemoryStore`, `StubSigner`, `NedConfig`(`par`), `SignalTelemetry`(the
      existing `@signal`/`ResultFilter`s).
- [ ] Shrink `app/sim/` modules to thin adapters: `Daemon` owns a `Node`; `Publisher`/`Browser`
      call the `Node` API; `NetworkConfigurator` calls `addNeighbor`/`learnRoute`.
- [ ] Add `test/core` unit tests (fake ports) and a `test/harness` in-process multi-node
      integration test (chain discovery across several hops, expiry, ordering).

**Verify:** core unit + harness tests green; the OMNeT++ example completes with the same
started/completed/aborted counts and interval/length histograms (within run-to-run noise).
**Commit:** "core: extract Node behind ports; simulation now runs on loti-core". **(M1)**

## Stage 3 — Production runtime adapters + `lotid` skeleton  → **M2**

**Goal:** a real single-node daemon that reproduces protocol behavior over real UDP, with
persistence.

- [ ] Implement OS adapters in `adapters/os/`: `WallClock`, a single-threaded **reactor**
      `Scheduler`, `UdpTransport` (real socket, port from config), `SecureRng` (CSPRNG),
      `PersistentStore` (LMDB or SQLite), `FileConfig` (TOML), `LogTelemetry`.
- [ ] Build `app/lotid`: construct one `Node` with the OS ports, run the reactor, open the
      transport, load/persist state, drive the clock-event and purge timers.
- [ ] Minimal static peering (config file lists neighbors + routes) to get two nodes talking;
      dynamic `peer add` comes in Stage 5.

**Verify:** two `lotid` processes on localhost exchange clock-event notifications; a scripted
discovery on one for an event published on the other completes and validates; state reloads
after restart.
**Commit:** "rt: OS adapters + lotid daemon; loopback two-node discovery works". **(M2)**

## Stage 4 — Identity & signing

**Goal:** attributable identity so proofs mean something.

- [ ] `KeyStore` OS adapter (Ed25519); load/generate the node key at startup.
- [ ] Wire `Signer` into event/clock-event creation (sign the hash) and into `validate`
      (verify signatures under the creator/reference public key). Sim keeps `StubSigner`.
- [ ] `NodeId` derives from the public-key fingerprint in production (sim keeps module id).
- [ ] Commands: `loti init` (create home, key, default config), `loti key gen/show`.

**Verify:** signed events/clock events verify; tampering flips verification to fail; hashes are
**unchanged** vs Stage 1 (signature excluded from hash). Sim run still green with the stub.
**Commit:** "id: Ed25519 identity + signing; loti init / key".

## Stage 5 — Control channel + CLI client

**Goal:** drive the daemon with `loti`; cover the MVP management + query commands.

- [ ] RPC server in `lotid` over a local control socket; a versioned request/response schema.
- [ ] `app/loti` client with global conventions from cli.md: `--json`, `--quiet`, exit-code
      table, object addressing (hash prefixes, `@self`, aliases).
- [ ] Commands: `node start/stop/status`, `config get/set/list`, `peer add/ls`,
      `publish [--file|-] [--sign] [--wait]`, `event show`, and the queries
      `chain`/`bounds`/`order` with `--reference`.

**Verify:** end-to-end shell session — start, peer up, publish, then `loti bounds <event>`
prints an interval; `--json` output is stable; wrong inputs return the documented exit codes.
**Commit:** "cli: control-socket RPC + loti client (node/config/peer/publish/queries)".

## Stage 6 — Proofs: export & offline verify  → **M3**

**Goal:** the MVP-defining feature — portable, offline-verifiable proofs.

- [ ] Implement the proof serialization from cli.md (`loti_proof` v1: event, lowerBound[],
      upperBound[], reference pubkey, result; order variant = two chains + comparison).
- [ ] `loti prove bounds/order --reference <node> --out <file>` (runs the discovery, serializes).
- [ ] `loti verify <file>` linking `core/validate` **directly** (no daemon/network): recompute
      hashes, check linkage, confirm endpoints are the reference node's, verify signatures;
      exit `0` valid / `6` invalid; print the proven bounds/order and which node's clock.
- [ ] `loti proof show` (summary, no verification).

**Verify:** publish → `prove` on one machine → copy file to a daemon-less environment →
`verify` returns `0` with correct bounds; a mutated proof returns `6`.
**Commit:** "proof: portable proof format + offline loti verify". **(M3)**

## Stage 7 — Persistence hardening + acceptance  → **M4**

**Goal:** durable, backed-up, both targets green, docs current.

- [ ] `loti db stat` and `loti db backup --out` / `db restore`; define + enforce the retention
      rule (local events + local clock chain are never dropped).
- [ ] Restart/backup/restore tests; a multi-node acceptance test (publish, prove, verify
      offline) as a CI job.
- [ ] Confirm the OMNeT++ example statistics are unchanged from `main` baseline (regression
      gate on hashes/sizes/completion).
- [ ] Update docs: mark the CLI MVP commands as implemented; note any deviations from
      [cli.md](../../doc/cli.md)/[architecture.md](../../doc/architecture.md) discovered while
      building.

**Verify:** full acceptance suite green in both targets; a cold restart of `lotid` resumes with
all prior events/clock events and can still prove them.
**Commit:** "mvp: persistence + acceptance; both targets green". **(M4 — move plan to done/)**

---

## Cross-cutting

**Testing tiers** (architecture.md): (1) core unit tests with fake ports; (2) in-process
multi-node harness for deterministic integration; (3) OMNeT++ for scale/statistics; plus (4)
`lotid`/`loti` production integration (real sockets/disk/restart). Every stage adds to the tier
it touches; the OMNeT++ example is the regression gate for protocol/byte parity.

**Dependencies between stages:** 0 → 1 → 2 (M1) → 3 (M2) → {4, 5} → 6 (M3) → 7 (M4). Stages 4
and 5 can proceed in parallel once Stage 3 lands, but signing (4) should be wired before proofs
(6) so proofs are attributable.

## Risks

- **Stage 1 byte-drift.** Any change to hash/serialization silently breaks parity and every
  existing proof. Mitigate with golden hash/size tests and the OMNeT++ statistics regression.
- **Persistence write amplification** at ~1 clock event/s/node — pick an embedded store with
  cheap appends; measure `db stat` growth against the paper's ~3–30 GB/year estimate.
- **Reactor vs. OMNeT++ ordering divergence.** Keep the core strictly non-blocking and
  single-threaded so both drivers exercise the same code path ordering; use the replay trick
  (architecture.md) if a prod-only bug appears.
- **Scope creep toward the beam search.** MVP is width-1; resist adding multi-path discovery
  until after M4.

## Decisions log

*(fill in during implementation — record design changes, chosen libraries, and any accepted
deviations from the docs; per repo convention, decisions live here, not in source comments)*

- **Worktree/branch:** implemented on branch `mvp` in a dedicated worktree
  (`../loti-mvp`); merge to `master` at M4.
- **Native build: CMake** (decided, Stage 0). Root `CMakeLists.txt`, `cmake_minimum_required
  3.20`, out-of-tree build under `build/` (gitignored). Coexists with `opp_makemake`.
- **Language standard: C++20** (g++ 15 / clang 23 both fully support it; gives `std::span`,
  concepts, etc. used in the architecture sketches).
- **Test framework: doctest v2.4.11** (decided, Stage 0), vendored single header at
  `test/doctest/doctest.h`. Chosen over Catch2 for lighter/faster compiles; swap is localized.
- _Store engine (LMDB vs SQLite): TBD — Stage 3._
- _RPC encoding (JSON-over-unix-socket vs framed binary): TBD — Stage 5._
