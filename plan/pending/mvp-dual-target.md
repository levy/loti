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

- **M1 — Simulation on the shared core** (end of Stage 2) ✅ **REACHED 2026-07-17**: the OMNeT++
  example runs on `loti-core` with bit-identical protocol statistics. *The simulation half of
  the MVP is done.*
- **M2 — Real node, loopback** (end of Stage 3) ✅ **REACHED 2026-07-17**: two `lotid` processes on
  localhost exchange clock notifications over real UDP and complete a discovery (chain + bounds),
  reproducing sim behavior; node state survives restart (snapshot persistence).
- **M3 — Proofs end-to-end** (end of Stage 6) ✅ **REACHED 2026-07-17**: publish → `loti prove
  bounds/order` → `loti verify` offline (no daemon, no network) works on real Ed25519-signed
  proofs; a mutated proof is rejected with exit 6.
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

## Stage 1 — De-OMNeT++ the domain model & serialization  ✅ **DONE (folded into Stage 2, 2026-07-17)**

**Goal:** protocol data types and hashing live in `loti-core` as plain C++, byte-identical to
today. *Highest-risk stage — guard with golden tests.*

*Landed together with the Stage-2 `Node` extraction (same commit, `d569745`); the two were
inseparable — the core engine couldn't be extracted without the plain-struct model + serializer.*

- [x] Ported `Data.msg` types to plain structs in `core/domain/types.hpp` (`Event`, `ClockEvent`,
      `LocalClockEvent`, `EventReference`, `EventChain`, discovery records, `Neighbor`) — no
      OMNeT++/INET dependency.
- [x] Reserved an **optional `signature` field** on `Event`/`ClockEvent` (excluded from the hash);
      Stage 4 wired real Ed25519 signing into it with **no hash change**, as designed.
- [x] Moved `calculate*Hash`/`calculate*Size` onto a core serializer (`core/hash/serializer.hpp`
      + `hashing.cpp`, byte-for-byte with `Data.cc`), `picosha2.hpp` vendored as-is.
- [x] The sim transport carries the core's canonical bytes in an INET **`BytesChunk`**
      (`adapters/sim/transport.hpp`); the `Packet.msg` chunk types are unused by the core-hosted
      `app/sim` (they remain only in the `src/` baseline).
- [x] Golden tests: `test/core/test_hashing.cpp` (fixed salt → known SHA-256; size == old
      `calculate*Size`) + `test_wire.cpp` (codec round-trip).

**Verify:** ✅ golden hash/size tests pass; the OMNeT++ example produces **byte-identical**
`clockEventsFileLength`/`eventsFileLength` vs the `src/` baseline (M1 A/B check — the core
serializer length equals `Data.cc`'s `calculate*Size`). *(Packet-length histograms differ by
design — the `BytesChunk` carries the core wire encoding, per the Stage-2 parity note.)*
**Commit:** folded into "Extract the Node protocol engine behind ports + in-process harness
(MVP Stage 2)" (`d569745`).

## Stage 2 — Ports + extract the `Node` core  → **M1 ✅ REACHED (2026-07-17)**

**Goal:** the protocol logic moves into `core/node`, driven by ports; the simulation runs on
the shared core.

- [x] Define port interfaces in `core/ports/`: `Clock`, `Scheduler`, `Transport`, `Rng`,
      `Signer`, `Telemetry`. **`Store` and `Config` were intentionally NOT made ports yet** —
      DAG state stays in the `Node` (extracted behind a `Store` port in Stage 3, when the
      persistent implementation gives the abstraction a second implementation); config is a
      plain `NodeConfig` struct for now. Hashing stays a shared free function (`core/hash`),
      not a port.
- [x] Move `Daemon.cc` protocol methods into `core/node` (`Node`), ports by reference:
      `publish_event`, `discover_event_{chain,bounds,order}`, the four `add*/extend*`
      primitives, request/response routing, validation, `on_packet_received`,
      `create_clock_event`, `add_neighbor`/`learn_route`, discovery coalescing + purge. Added a
      real wire codec (`core/wire`) so the transport carries canonical bytes.
- [x] Implement **simulation adapters** (`adapters/sim/`) and shrink the OMNeT++ modules
      (`app/sim/`) to thin `Node` hosts. **DONE (2026-07-17)** on OMNeT++ 6.4 / INET 4.7: six
      header-only port adapters (`SimClock`/`SimScheduler`/`SimTransport`/`SimRng`/`NullSigner`/
      `SimTelemetry` in `adapters/sim/`) plus four thin OMNeT++ modules in `app/sim/` — `Daemon`
      hosts a `loti::Node`; `Publisher`/`Browser`/`NetworkConfigurator` drive it. The build
      (`scripts/build-sim.sh`) now compiles `core/ + adapters/sim/ + app/sim/` and **excludes
      `src/`** (kept as the untouched A/B baseline); NED path is `app/sim:sim`.
- [x] Add `test/core` unit tests (fake ports) and a `test/harness` in-process multi-node
      integration test — chain / bounds / order discovery across several hops.

**Verify:** ✅ **core + harness green** — `build-core.sh` runs 12 cases / 34 assertions under
g++ 15.2 (chain/bounds/order discovery + wire round-trip). ✅ **OMNeT++ sim-parity achieved** —
the `SimpleNetwork` example now runs on `loti-core` (release + debug build clean, C++20). Against
the pre-refactor `src/` baseline (120 s, `NoDiscovery` + `EventBoundsDiscovery`, read back via the
`omnetpp.scave` pandas API) every protocol statistic is **bit-identical**: clockEventCreated
(6806 / 6802), eventCreatedCount (712 / 698), chain & bounds discovery started/completed/aborted
(646 / 400 / 246), and the byte-exact clock/events file lengths. The only delta is
`packetSent`/`Received` (−18…23%), fully explained by dropping a `NetworkConfigurator` bug in the
original — a self-referential, mis-addressed neighbour entry that made every node emit one extra
misdirected notification per tick; with it removed, packet counts match the true topology degree
and discovery completion is unchanged. Debug mode (which re-verifies every clock-event hash) runs
0 asserts, and `EventOrderDiscovery` — which assert-crashed in the old `Daemon.cc` — now runs
clean (the core guards the abort).

**Status:** ✅ **M1 fully reached** — the actual OMNeT++ simulation runs on `loti-core` with
bit-identical protocol statistics. *The simulation half of the MVP is done.*
**Commits:** "Extract the Node protocol engine behind ports + in-process harness (MVP Stage 2)";
"Bind the OMNeT++ simulation onto loti-core (sim adapters + thin app/sim modules) — M1".

## Stage 3 — Production runtime adapters + `lotid` skeleton  → **M2 ✅ REACHED (2026-07-17), incl. restart-survivable persistence**

**Goal:** a real single-node daemon that reproduces protocol behavior over real UDP, with
persistence.

- [x] OS adapters in `adapters/os/` (header-only): `WallClock` (CLOCK_REALTIME ns), a
      single-threaded **epoll `Reactor`** + `ReactorScheduler`, `UdpTransport` (real non-blocking
      socket, port from CLI), `SecureRng` (getrandom CSPRNG), `NullSigner`, `LogTelemetry`, and a
      **`FileStore`** (atomic snapshot blob store). `FileConfig` (TOML) is **still deferred**
      (CLI flags serve as config for now).
- [x] `app/lotid`: constructs one `Node` on the OS ports, runs the reactor, opens the transport,
      drives the clock + purge timers, **loads a snapshot on start and saves one on exit +
      periodically** (`--store <path>`), and takes stdin line commands
      (`publish`/`chain`/`bounds`/`order`/`events`/`save`/`quit`) so two instances can be scripted.
      Built by CMake (`add_executable(lotid …)` links `loti_core` **only — no OMNeT++/INET**), so
      it also builds under `scripts/build-core.sh`.
- [x] Minimal static peering via `--peer id:ip:port` / `--route dst:nexthop` CLI flags (a TOML
      config file is a later nicety). Enough to get two nodes talking; dynamic `peer add` is Stage 5.

**Verify:** ✅ two `lotid` processes on loopback (udp/5001 + udp/5002) exchange clock-event
notifications over **real UDP**; node 1 publishes an event, then a **chain discovery completes
and validates** (endpoints are 2 clock events of node 2, the reference) and a **bounds discovery
completes** with a real wall-clock interval (`[…337904181, …838555560]` ns ≈ 0.50 s wide, matching
the 0.5 s clock cadence, dated 2026). ✅ **State survives restart**: a node publishes an event +
ticks 8 clock events, `quit` saves a 1106-byte snapshot; a fresh process with the same `--store`
restores all 1 event + 8 clock events (same hashes) and continues the clock chain coherently. Core
unit tests stay green (`ctest` 1/1); the OMNeT++ sim still builds and its clock/event density is
unchanged (the snapshot methods are unused there).
**Commits:** "rt: OS adapters + lotid daemon; loopback two-node discovery works — M2";
"rt: snapshot persistence — lotid state survives restart (Stage 3b)".

**Remaining (minor, non-blocking):** `FileConfig` (TOML) — CLI flags cover config for MVP; and
incremental/DB-backed storage (the current `FileStore` writes a full snapshot, fine at MVP scale
but not for the paper's GB/year — a Stage-7 concern).

## Stage 4 — Identity & signing  ✅ **DONE (2026-07-17)**

**Goal:** attributable identity so proofs mean something.

- [x] `Ed25519KeyStore` OS adapter (`adapters/os/keystore.{hpp,cpp}`, OpenSSL EVP): load or
      generate + persist the node key at startup (`lotid --key <path>`). Confined to the adapter —
      only it + `lotid` link `libcrypto`; `loti_core` stays pure.
- [x] `Signer` wired into creation (the core already signed the hash on event/clock-event create)
      **and into `validate`** — `validate_event_chain` now verifies every clock-event and the
      event signature. The sim keeps `NullSigner` (verify → true), so this is a no-op there and
      the OMNeT++ statistics stay **bit-identical** (re-verified: 6802/698/646/400/246).
- [x] `NodeId` derives from the public-key fingerprint in production (first 8 bytes of
      SHA-256(pubkey)); the sim keeps the module id. **Self-contained verification:** a signature
      is `Ed25519(hash) [64] || pubkey [32]`, so `verify` splits out the pubkey, checks
      `fingerprint(pubkey) == creator`, then does the Ed25519 check — any party validates knowing
      only the `NodeId`, no key registry (this is what makes offline `loti verify` possible in Stage 6).
- [~] Commands — `lotid --key` covers key gen/load and the `key` stdin command shows the id +
      public key. `loti init` / `loti key gen/show` as **`loti` CLI** subcommands are **deferred to
      Stage 5** (they need the control-socket client that Stage 5 builds).

**Verify:** ✅ 19 unit cases / 45 assertions (was 12/34) incl. sign/verify, tamper→fail
(signature, embedded pubkey, or message), wrong-claimed-identity→fail, cross-keystore verification
from the embedded pubkey alone, and signature-excluded-from-hash. ✅ end-to-end: two **signed**
`lotid` nodes on loopback (ids `0xc156…`, `0xfdb0…` = their key fingerprints) complete a chain +
bounds discovery — completion means node 1 verified node 2's signed clock events using only node 2's
NodeId + the embedded pubkey. ✅ hashes unchanged; ✅ sim bit-identical with `NullSigner`.
**Commit:** "id: Ed25519 identity + signing; verification wired into validate".

## Stage 5 — Control channel + CLI client  ✅ **DONE (2026-07-17)**

**Goal:** drive the daemon with `loti`; cover the MVP management + query commands.

- [x] Control server in `lotid` over a Unix-domain socket (`--control <path>`), integrated into
      the reactor. **Versioned line protocol** (`adapters/os/control.hpp`): request `verb args…\n`;
      reply `LOTI/1 OK|ERR <code>` + `key\tvalue` field lines + blank terminator. Queries
      (`bounds`/`chain`/`order`) are **asynchronous** — the connection is held until the discovery
      completes or aborts (the purge timer guarantees a callback within `expiry`), then the reply
      is sent. Command dispatch is shared with the stdin interface.
- [x] `app/loti` client: connect → send → render. Globals `--json` (object; repeated field keys
      become an array), `--quiet`, `--control <path>` (or `$LOTI_CONTROL`), and an exit-code table
      (0 ok · 2 usage · 4 not-found · 6 invalid/aborted · 1 unreachable). Object addressing by
      **hash prefix** works (`@self`/aliases deferred).
- [x] Commands: `status`, `publish <text>`, `event show <h>`, `events`, `bounds`/`chain`/`order`
      (with `--wait` semantics — they block on the async reply), `peer add`/`peer ls`, `key`,
      `node status`/`node stop`/`stop`, and **`loti init`** (create home dir + generate key + write
      config) — this delivers the Stage-4-deferred `loti init` / key setup. **Deferred (noted):**
      `config get/set/list` (config is CLI flags + the init file for now), `node start` (process
      spawn/daemonize), `publish --file|-`, and `--reference <node>` on queries (the core picks the
      reference; explicit/multi-reference is a post-MVP item).

**Verify:** ✅ end-to-end against two signed daemons over their control sockets: `loti status`
(id/mode/counts), `loti peer ls`, `loti publish "…"` → hash, `loti event show <h>`, then
`loti bounds <h>` prints a real ~0.5 s interval (async), `loti --json bounds <h>` emits stable
JSON, `loti chain <h>` returns; `loti event show deadbeef` → `error(4)` exit 4; an unreachable
socket → exit 1; `loti init` writes key+config and prints the start command. Core tests green; sim
unaffected (only `adapters/os` + `app/` changed, which the sim build excludes).
**Commit:** "cli: control-socket RPC + loti client (publish/queries/peer/status/key/init)".

## Stage 6 — Proofs: export & offline verify  → **M3 ✅ REACHED (2026-07-17)**

**Goal:** the MVP-defining feature — portable, offline-verifiable proofs.

- [x] Proof serialization (`core/proof/`): a `Proof` = kind + reference (node id + embedded
      Ed25519 pubkey) + the enclosing `EventChain` (event, lowerBound[], upperBound[]); the order
      variant carries two chains + the compared `order`. The portable form reuses the wire codec
      (`core/wire/codec.hpp`) behind a `"LOTIPROF"` magic + version, so a proof's bytes are
      identical to what the node puts on the wire. **Chosen the binary wire form** over the JSON
      shown in cli.md (no JSON lib; byte-exact with the node) — noted as a deviation.
- [x] `loti prove bounds|order|chain <event> [event2] --out <file>` — the daemon runs the chain
      discovery(ies), serializes a proof (reference = the discovering node, whose own clock events
      anchor the chain), and returns it hex over the control socket; the CLI writes the artifact.
      *Deferred:* `--reference <node>` (the core always anchors in the discovering node's clock —
      explicit/multi-reference is post-MVP, as already noted for the queries).
- [x] `loti verify <file>` — pure offline verification in `core/proof::verify(Proof, Signer)`,
      linked directly into the CLI (no daemon, no network): recompute every clock/event hash, check
      the chain is linked, confirm both endpoints are the reference node's, cross-check the
      reference pubkey fingerprints to the reference id, and verify every Ed25519 signature under
      its embedded pubkey. Exit `0` valid / `6` invalid; prints the proven bounds/order and that
      they are in the reference node's clock. `--json` supported; `--trust <node>` is advisory for
      the MVP (a warning if the reference is outside the set) — noted as a deviation from cli.md.
- [x] `loti proof show <file>` — human summary (kind, reference + pubkey, event, chain length,
      bounds/order) with no verification.

**Verify:** ✅ end-to-end against a real signed `lotid`: `loti init` + signed daemon (0.25 s clock),
`publish` → `prove bounds last --out proof.loti` (exit 0, chain length 2), `proof show` renders the
interval + reference pubkey, `verify proof.loti` → exit 0 with the correct bounds, `verify --json`
emits stable JSON. Copied the artifact out, **killed the daemon**, and verified from `/tmp` with no
control socket present → still exit 0 (proves offline). A one-byte flip → `invalid: lower-bound
clock event hash mismatch`, exit 6. `prove order A B` on two clock-separated events → `order: before`,
verify exit 0. Core suite green (26 cases / 86 assertions, +7 proof cases). Sim unaffected: the new
`core/proof/proof.cpp` is pure (wire + picosha2 + the abstract Signer port), swept into `libloti.so`
by `--deep` with no new OpenSSL/OMNeT++ dependency.
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
- **Stage 2 deviations from the doc's port list (accepted):**
  - `Store` and `Config` are **not** ports yet. The `Node` holds its DAG state directly (as the
    original `Daemon` did) and takes a `NodeConfig` struct. Rationale: in Stage 2 the store has
    exactly one (in-RAM) implementation, so the abstraction earns nothing until Stage 3's
    `PersistentStore`; keeping state in the `Node` reduced risk during the delicate extraction.
  - `discovery` / `validate` / `dag` logic lives in `core/node.cpp` (one engine, like the
    original) rather than split across `core/discovery|validate|dag/`; those dirs remain
    placeholders. Split later only if it earns clarity.
  - `Timestamp`/`Duration` are `int64` ticks; the wire header carries the sender id (as the
    original `LotiHeader.neighbor`), so `Transport::send` needs only the destination.
  - The `Node` calls `Signer::sign` on creation (NullSigner → empty signature); signature
    **verification** stays out of validation until Stage 4, so Stage-2 behavior matches the
    original exactly.
- **Toolchain is OMNeT++ 6.4 / INET 4.7** (not the 5.4 / 4.0 the code targeted). Before the
  Stage-2 OMNeT++ binding, ported the existing `src/` + `sim/` to build and run on it, so the
  pre-refactor sim is a working baseline. Changes were **API-only**: `INITSTAGE_NETWORK_LAYER_3`
  → `INITSTAGE_STATIC_ROUTING`; `getInterfaceByNode*GateId` → `findInterfaceByNode*GateId`;
  `ipv4Data()` → `getProtocolData<Ipv4InterfaceData>()`; added the new `socketClosed`
  `UdpSocket::ICallback` override; visualizer import package path; `@castFunction(false)` on the
  aliased `Type.msg` primitives (`Salt`, `ByteVector`) to stop OMNeT++6 emitting duplicate
  `toAnyPtr`/`fromAnyPtr`. **Serialization byte layout unchanged** (`Data.msg`/`Packet.msg`/
  `Data.cc` untouched); builds clean in release + debug; runs with the debug hash-re-verification
  passing (0 asserts). Two pre-existing bugs surfaced: `generateSalt`'s `intuniform(0,0xFFFFFFFF)`
  overflowed a signed int (fixed to `0x7FFFFFFF`; salt is a hashed value, not layout); and an
  `EventOrderDiscovery` double-abort assertion race in the old `Daemon.cc` (out of scope — already
  avoided in the core `Node`, which guards on `in_progress`). Env: source `omnetpp/setenv -q`,
  `inet/setenv`, then loti `setenv -f`.
- **Stage-2 OMNeT++ binding (M1 last mile, 2026-07-17):**
  - **Layout:** the core-hosted OMNeT++ modules live in `app/sim/` (new home, per the target
    layout) and the port adapters in `adapters/sim/`; `src/` is left untouched as the A/B
    baseline. `sim/` (the `SimpleNetwork` scenario + `omnetpp.ini`) is shared and reused
    unchanged except for the statistics sources (below). The build excludes `src/`; the two never
    co-compile, so `app/sim` keeps the same `loti` C++/NED namespace and the ini's `typename`s
    resolve to the core-hosted modules with no changes.
  - **Extension `.cpp`:** `opp_makemake` refuses a deep build mixing `.cc` and `.cpp`; since
    `core/` is `.cpp` (and CMake references those names), the `app/sim/` modules are `.cpp` too
    (`-e cpp`). Adapters are header-only `.hpp`.
  - **C++20 everywhere:** the whole sim build includes core headers (defaulted `operator==`), so
    `makefrag` forces `CXXFLAGS += -std=c++20` over OMNeT++'s default. Build via
    `scripts/build-sim.sh` (the `Makefile` is generated + gitignored).
  - **Telemetry = precomputed scalar signals (no cObjects / no result filters).** The core
    computes the quantities (byte sizes via `core/hash`, discovery times/lengths/intervals from
    the discovery records' raw-tick timestamps); `SimTelemetry` emits them as plain numeric
    signals. This drops the original's `Data.msg` cObjects + `ResultFilter.cc` from the sim
    entirely. `app/sim/Daemon.ned` and `sim/Network.ned` keep the **same `@statistic` names**
    (so results compare by name) but source them from the scalar signals — e.g.
    `sum(clockEventSize(clockEventCreated))` → `sum(clockEventCreated)` (the created signal now
    carries the byte size), and `eventChainDiscoveryTime(...Completed)` → its own
    `eventChainDiscoveryTime` signal. Editing `sim/Network.ned`'s sources means re-running the
    `src/` baseline needs the original `Network.ned` (in git history); the baseline scalars were
    captured first.
  - **Transport = INET `BytesChunk`.** `SimTransport` wraps the core's canonical wire bytes in a
    `BytesChunk` over the existing `UdpSocket` (port 666); no `Packet.msg` chunk types. Packet
    *lengths* therefore differ from the original FieldsChunk sizes (accepted — the parity bar is
    behavioural + byte-exact object sizes, not bit-identical packets).
  - **Clock timer stays in the host** (re-samples the volatile `createClockEventInterval` each
    tick); only the purge timer is core-driven (`NodeConfig.discovery_expiry`, `clock_event_interval=0`).
    `SimScheduler` backs each core timer with a `cMessage` and takes the module's protected
    `scheduleAt`/`cancelEvent` as injected callbacks.
  - **NetworkConfigurator:** replaced the original friend-class field-poking with the Daemon's
    public `learnRoute()`/`addNeighbor()` API, and **dropped the original's buggy "reverse block"**
    (which inserted a self-referential, mis-addressed neighbour per node). This is why
    `packetSent`/`Received` drop ~18–23% vs baseline while every protocol statistic stays
    bit-identical — the removed packets were misdirected duplicates.
  - **Result:** bit-identical protocol statistics vs the `src/` baseline; `EventOrderDiscovery`
    (which assert-crashed in the old `Daemon.cc`) now runs clean because the core `Node` guards
    the double-abort. `src/`'s modules are now dead weight kept only as the baseline — retire them
    once M2+ no longer needs the A/B reference.
- **Stage-3 OMNeT++-free production runtime (M2 core, 2026-07-17):**
  - **OS adapters are header-only** in `adapters/os/` (namespace `loti::os`): `WallClock`
    (CLOCK_REALTIME nanoseconds — a production "tick" is 1 ns, so proof bounds are real dates),
    `Reactor` (epoll + a `multimap`-ordered timer queue) with `ReactorScheduler` expressing the
    core's Scheduler port, `UdpTransport` (non-blocking UDP socket; the core's canonical wire
    bytes go out verbatim — the same encoding the sim's `BytesChunk` carries), `SecureRng`
    (getrandom(2)), `NullSigner`, `LogTelemetry` (stderr lines).
  - **The reactor is the only loop.** It fires due timers (how the core "waits"), then
    `epoll_wait`s until the next timer or a readable fd (UDP socket, stdin), then dispatches —
    single-threaded and non-blocking, matching the sim's DES ordering discipline so both drivers
    exercise the same core code paths. Timers and `WallClock` share CLOCK_REALTIME, so the due
    time the core computes (`clock.now() + delay`) lines up with the reactor's comparisons.
  - **`lotid` builds via CMake and links `loti_core` only** — no OMNeT++/INET —
    so `scripts/build-core.sh` builds it too, mechanically proving the real node needs nothing but
    the pure core + the C++/POSIX runtime.
  - **Peering is CLI flags** (`--peer id:ip:port`, `--route dst:nexthop`), not a config file yet
    (a neighbour gets a direct route by default). **Driving is stdin line commands** — there is no
    control socket yet (that's Stage 5); the stdin interface is a throwaway harness that lets two
    daemons be scripted end to end.
  - **Persistence = snapshot, not a fine-grained `Store` port (Stage 3b, 2026-07-17).** Rather than
    extract a `Store` port threaded through every DAG access + an embedded DB, the `Node` gained
    `snapshot()`/`restore()`: serialize the whole DAG (events, clock events **with their learned
    `referencing_events`**, unreferenced events, neighbors, routes) to an opaque blob via the wire
    codec, and load one back rebuilding the derived indices. `adapters/os/FileStore` writes/reads
    the blob (temp-file + `rename` for atomicity); `lotid --store <path>` restores on start (before
    static peering, so learned neighbor state survives while CLI addresses are re-applied) and
    saves on exit + every 5 s. Rationale: (a) a snapshot captures the **dynamic** `referencing_events`
    that per-op append logging would miss; (b) it is **zero-risk to M1** — `snapshot`/`restore` are
    pure additions the sim never calls, so byte-parity is untouched (verified: sim still builds,
    density unchanged); (c) backup = copy the file. Trade-off: rewrites the whole snapshot each
    save and loses events created since the last save on a hard crash — fine at MVP scale;
    incremental/append or an embedded DB (LMDB/SQLite) is a Stage-7 scaling concern, and the
    fine-grained `Store` port can be introduced then if it earns its keep.
- **Stage-4 identity & signing (2026-07-17):**
  - **Crypto = OpenSSL EVP Ed25519** (real, audited — no hand-rolled crypto), confined to
    `adapters/os/keystore.cpp`; the header exposes an opaque `void*` so only the adapter + `lotid`
    link `libcrypto`. `loti_core` stays pure (it has only the `Signer` **port**).
  - **Self-contained signatures:** a signature is `Ed25519(message) [64] || public key [32]`, and a
    `NodeId` is `first 8 bytes of SHA-256(pubkey)`. `verify(msg, sig, signer)` splits out the
    embedded pubkey, checks `fingerprint(pubkey) == signer`, then does the Ed25519 check — so any
    node verifies any other's signatures with **no key registry / no key exchange** (the pubkey
    rides in the signature, which rides on the wire and in snapshots). This is what makes offline
    `loti verify` (Stage 6) work. Signature stays excluded from the hash → no hash changes, no
    invalidated proofs.
  - **`lotid` dual mode:** `--key <path>` → signed mode, `NodeId = fingerprint(pubkey)` (printed
    hex; `--peer`/`--id`/`--route` now parse base-0 so hex fingerprints round-trip); no `--key` →
    unsigned mode with `NullSigner` + `--id` (keeps the M2/3b tests working). Signer + Node became
    `unique_ptr` so the mode is chosen at construction.
  - **Verification in the core:** `validate_event_chain` now calls `signer_.verify` for each
    clock event and the event. Because the sim's `NullSigner.verify()` returns true, this is a
    no-op there — re-verified the OMNeT++ stats are still bit-identical.
  - **Build-scoping gotcha (fixed):** once `adapters/os/keystore.cpp` and `app/lotid/lotid.cpp`
    existed, `opp_makemake --deep` swept them into the sim's `libloti.so`, which then failed to
    load on an unresolved `libcrypto` / KeyStore-vtable symbol. `scripts/build-sim.sh` now excludes
    `-X adapters/os -X app/lotid -X app/loti` — the sim is built strictly from
    `core/ + adapters/sim/ + app/sim/`. Rule going forward: any new production-only `.cpp` under
    `adapters/os` or `app/*` (non-sim) must be outside the sim's `--deep` sweep.
- **Stage-5 control channel + `loti` CLI (2026-07-17):**
  - **RPC encoding = a hand-rolled versioned line protocol over a Unix-domain socket**, not
    JSON-over-socket or framed binary. Resolves the Stage-1 TBD. Rationale: no JSON library to
    vendor (keeps the minimal-deps stance), trivial to frame (`\n`-terminated request, blank-line
    terminated reply), and `--json` is a pure CLI-side render of the `key\tvalue` fields (repeated
    keys → array). Header `adapters/os/control.hpp` is shared by both binaries.
  - **Queries are async on one connection.** `bounds`/`chain`/`order` register the client fd in a
    per-event pending map and return "deferred"; the discovery completion/abort callback sends the
    reply and closes the fd. No extra timeout needed — the discovery purge (expiry) guarantees a
    completion-or-abort callback, so a waiting client always gets a reply within ~`expiry`.
  - **Dispatch is shared** between the control socket and the stdin interface (one `dispatch()` →
    `Reply`), so the two never drift. Peering also went through one `add_peer()` (used by both the
    `--peer` flag and the `peer-add` command) which also feeds `peer ls`.
  - **`loti init` uses the keystore adapter directly** (local, no daemon) to generate the key and
    writes a flat `key=…`/`port=…`/`store=…`/`control=…` config — this covers the Stage-4-deferred
    `loti init`/key setup. A real config *loader* (`FileConfig`) and `config get/set/list` are still
    deferred; the file is documentation the operator pastes into the `lotid` command for now.
  - Both `lotid` and `loti` link `loti_core` + `libcrypto`; the sim build already excludes
    `app/lotid`/`app/loti`, so adding `app/loti` needed no build-scope change.
- **Stage-6 portable proofs + offline verify (M3, 2026-07-17):**
  - **Proof lives in the pure core** (`core/proof/`, `loti::proof`): a `Proof` = kind + reference
    (node id + embedded pubkey) + the enclosing `EventChain`(s). `serialize`/`deserialize` reuse the
    wire codec behind a `"LOTIPROF"` magic + version; `verify(Proof, Signer&)` runs the offline
    checks and delegates *only* signature verification to the injected `Signer` port — so the core
    stays OpenSSL-free and the same file compiles into `libloti.so` (verified: no new undefined
    crypto symbol). `loti verify` injects an `os::Ed25519KeyStore` whose `verify()` uses each
    signature's *embedded* pubkey (its own key is irrelevant), which is what makes verification work
    with no key registry and no daemon.
  - **Binary wire form, not the JSON in cli.md (accepted deviation).** No JSON library to vendor,
    and the bytes are identical to what the node already serializes. `proof show`/`verify` render a
    human summary (ISO time + raw tick) from it.
  - **`verify` mirrors `Node::validate_event_chain`** (recompute hashes, linkage, endpoints are the
    reference node's) **plus** two productization checks: the reference pubkey must fingerprint to
    the reference id, and every signature must verify. Kept as an independent free function (not a
    call into the private `Node` validator) so the hot protocol path is untouched — the two are
    deliberately kept in sync (noted in both sources' comments).
  - **The reference node is the discovering node.** The core always anchors a chain in the
    discovering node's own clock events, so `prove` sets `reference = self` and embeds *its* pubkey;
    `--reference <node>` (proofs anchored in a *chosen* remote node) stays deferred with the other
    explicit-reference query work. `--trust <node>` on `verify` is advisory for the MVP (a warning
    if the reference is outside the set) — verification proves integrity + attribution, and trust
    is the verifier's call (accepted deviation from cli.md's gating phrasing).
  - **`prove` needs the control socket** (a proof rides back as hex after an async discovery), so it
    is rejected on the stdin harness. Single-chain proofs (`bounds`/`chain`) wait on one event;
    `order` accumulates two chains keyed by the event pair, then compares intervals with the same
    rule as `Node::compare_event_chains`.
- _Store engine (incremental append / LMDB vs SQLite): deferred to Stage 7 (scaling)._
- _RPC encoding (JSON-over-unix-socket vs framed binary): resolved in Stage 5 (hand-rolled line protocol)._
