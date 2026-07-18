# Architecture — One Codebase, Two Runtimes

This document describes how to structure LOTI so that **a single implementation of the
protocol** serves two very different runtimes from the same source:

1. a **discrete-event simulation** (OMNeT++ / INET) for experimenting with behavior on large
   networks and collecting statistics ([implementation.md](implementation.md),
   [simulation.md](simulation.md)); and
2. a **real, deployable node** (`lotid`) driven by the [product CLI](cli.md) (`loti`).

The two runtimes disagree about almost every *environmental* concern — time, networking,
storage, randomness, concurrency, configuration, and how results are recorded — but they must
agree **exactly** about the protocol: how events and clock events are formed and hashed, how
the DAG grows, how the three discoveries route and build chains, and how chains are validated.
The architecture's whole job is to isolate the first set of concerns so the second can be
written once.

> **The layout in one place:** the pure core lives in `src/core/` (no OMNeT++/INET/OS deps) and
> runs behind six ports; the simulation drives it through `src/adapters/sim/` + `src/app/sim/`, and
> the production node (`lotid`/`loti`) through `src/adapters/os/` + `src/app/lotid` + `src/app/loti`.
> Both targets are covered in CI — the OMNeT++ `SimpleNetwork` example runs on the core, and the
> production node passes the acceptance suite ([test/acceptance/run.sh](../test/acceptance/run.sh)).
> The MVP command surface and where it deviates from this document are in
> [cli.md → Implementation status](cli.md#implementation-status-mvp).

## The core idea: functional core, imperative shell

Use **ports and adapters** (hexagonal architecture). The protocol lives in a pure **core
library** that depends only on a small set of abstract **ports** (interfaces) for everything
environmental. Two sets of **adapters** implement those ports — one backed by OMNeT++, one
backed by the real OS — and two **frontends** drive the core: the OMNeT++ application modules
in simulation, and the RPC/CLI in production.

```
        ┌──────────────── frontends ────────────────┐
        │  OMNeT++ apps (Publisher/Browser)   loti CLI ⇄ control socket │
        └───────────────┬───────────────────────┬────┘
                        │        core API        │
                ┌───────▼────────────────────────▼───────┐
                │            loti-core (pure)             │
                │  DAG · events · clock events · the 3    │
                │  discoveries · chain validation ·       │
                │  hashing · canonical serialization      │
                └───────┬───────────────────────┬─────────┘
                        │        ports           │   (abstract interfaces)
        Clock · Scheduler · Transport · Rng · Signer · Telemetry
                        │                       │
        ┌───────────────▼───────┐   ┌───────────▼──────────────┐
        │  simulation adapters  │   │   production adapters     │
        │  (OMNeT++ / INET)     │   │   (OS: sockets, disk, …)  │
        └───────────────────────┘   └──────────────────────────┘
```

**Rule of dependency:** arrows point inward. `loti-core` includes **no** OMNeT++, INET, or OS
headers, and has **no global mutable state**. Adapters depend on the core; the core depends on
nobody. There are **no `#ifdef SIMULATION` branches inside the core** — the environment is
chosen by *linking* a different adapter set, not by conditional compilation.

## Why the core must be pure

Three properties fall out of "core depends only on ports," and all three are load-bearing:

- **Determinism / reproducibility.** With time, randomness, and network delivery supplied
  through ports, a simulation run is a pure function of its seed and inputs. The same is what
  lets you *replay* a production incident inside the core (see
  [Reproducibility](#reproducibility-and-replay)).
- **Byte-for-byte parity.** The core owns the canonical serialization and hashing, so the bytes
  the real node puts on a UDP socket are the same bytes the simulation accounts for. Storage
  and bandwidth statistics measured at scale in simulation therefore predict production.
- **Fast, honest tests.** The core can be unit-tested and integration-tested with lightweight
  in-process fakes — no OMNeT++, no sockets — while the heavy OMNeT++ harness is reserved for
  large-scale statistics.

## What lives in the core

`loti-core` is a plain C++ library (no `cObject`, no `simtime_t`, no INET chunks). It contains
the logic that is *identical* in both runtimes:

- **Domain model** — `Event`, `ClockEvent`, `LocalClockEvent`, `EventReference`, `EventChain`,
  and the discovery records, as plain structs in [`types.hpp`](../src/core/domain/types.hpp).
- **Hashing & canonical serialization** — SHA-256 over the fixed byte layout in
  `calculateEventHash` / `calculateClockEventHash`, using the already-portable header-only
  [`picosha2.hpp`](../src/core/hash/picosha2.hpp). The wire encoding of every packet lives here too, so both
  transports carry identical bytes.
- **DAG maintenance** — `insertEvent`, `insertClockEvent`, the neighbor cross-link bookkeeping
  (`referencedEvents` / `referencingEvents`), and the hash→index lookups.
- **The three discoveries** — chain request/response routing and the four chain-building
  primitives (`addLocalLowerBound`, `addLocalUpperBound`, `extendLowerBoundForNeighbor`,
  `extendUpperBoundForNeighbor`), plus bounds and order on top.
- **Validation** — `validateEventChain` / `validateEventChainDiscoveryResult`, and (for the
  product) proof serialization/verification.
- **Neighbor table & overlay routing state** — updated *through the API* by whichever frontend
  learns it (the simulation's global configurator, or the production peering subsystem).

The core is a normal instantiable object — you create **one `Node` per participant**. A single
OMNeT++ process holds thousands of `Node` instances; a `lotid` process holds exactly one. No
singletons, no thread-locals, no `static` mutable data.

### Core public API

[`Node`](../src/core/node.hpp) exposes this surface to the frontends (the shape below is
illustrative; the code uses `snake_case`):

```cpp
class Node {
public:
  Node(Ports ports, const Config& cfg, Identity id);

  // ── inbound API (from a frontend) ───────────────────────────────
  EventId publishEvent(Bytes content);
  void discoverEventChain (EventId, ChainCallback);
  void discoverEventBounds(EventId, BoundsCallback);
  void discoverEventOrder (EventId, EventId, OrderCallback);
  void addNeighbor(NodeId, Address);   void removeNeighbor(NodeId);
  void learnRoute(NodeId dest, NodeId nextHop);

  // ── inbound events (from adapters) ──────────────────────────────
  void onPacketReceived(NodeId from, Bytes);   // Transport delivers here
  void onTimer(TimerId);                        // Scheduler fires here
};
```

When the core needs an *effect* — send a packet, arm a timer, read the clock, draw a salt,
persist a record, emit a statistic — it calls a **port**. It never blocks and never touches the
environment directly.

## The ports

Each port is a small abstract interface. The core is constructed with a bundle of port
implementations (`Ports`), so swapping runtimes is swapping that bundle.

| Concern | Core port | Simulation adapter | Production adapter |
| --- | --- | --- | --- |
| **Time** | `Timestamp Clock::now()` | OMNeT++ `simTime()` | `CLOCK_REALTIME` nanoseconds |
| **Scheduling** | `Scheduler::after(Duration, cb)`, `cancel(TimerId)` | `scheduleAt` + self-messages | epoll reactor + a timer queue |
| **Transport** | `Transport::send(NodeId, Bytes)` + `receive()` | INET `UdpSocket` (`BytesChunk`) | non-blocking UDP socket |
| **Randomness** | `Rng::next_salt()` | seeded `intuniform` (reproducible) | `getrandom(2)` CSPRNG |
| **Signing** | `Signer::sign/verify` | `NullSigner` (unsigned) | Ed25519 keystore (OpenSSL) |
| **Telemetry** | `Telemetry::record(...)` hooks | scalar `@statistic` signals | structured stderr log lines |

The DAG state and configuration are **not** ports: the `Node` holds its events, clock chain, and
indices directly, and takes a small config struct. Production makes that state durable by mirroring
every change — event, clock event, neighbor, route — into an LMDB store
([`lmdb_store.hpp`](../src/adapters/os/lmdb_store.hpp)) through a `PersistenceListener`, and reloading
it with `Node::load` at startup; the simulation keeps it in RAM and never persists. (A portable
snapshot through the wire codec — the file-store adapter — remains the `db-backup` / `db-restore` and
migration format.) Hashing is shared, not swapped — it must be identical everywhere; only signing
differs (unsigned in the simulation, Ed25519 in production).

The store's LMDB environment is sized by a mapsize (`--store-mapsize`, in GiB), which is virtual
address space grown on demand. On a **64-bit** build it defaults to 16 GiB and grows without a
practical cap. On a **32-bit** build (e.g. a Pi Zero / Zero W, ARMv6) the mmap must fit in the
~2–3 GiB per-process address space, so the default is 1 GiB and both the flag and auto-grow are
capped at ~1.5 GiB (`LmdbStore::kMaxMapSize`). Because an LMDB env can never exceed its mapsize,
that ceiling also bounds a 32-bit node's **total stored DAG over its lifetime** (~1–2 years at the
paper's GB/year) — the 64-bit Zero 2 W has no such ceiling and is the long-life target. See
[`doc/embedded.md`](embedded.md).

Durability is tunable for SD-card wear. By default each commit is fsync'd (`--store-sync-interval 0`,
safe). With `--store-sync-interval <s> > 0` the store opens `MDB_NOSYNC` (lazy): per-commit fsyncs
are skipped and a reactor timer flushes every `<s>` seconds, so SD writes coalesce. Without
`MDB_WRITEMAP` this stays crash-*consistent* — a crash only risks the last `<s>` of commits, never
integrity. Clean shutdown and `save` always force a final flush. `MDB_NOMETASYNC` (fsync data but not
the meta page) is a documented middle ground, not currently wired to a flag. Full operator guidance:
[`doc/embedded.md`](embedded.md).

Neighbor/routing state is core state fed through `add_neighbor` / `learn_route`: the simulation's
[`NetworkConfigurator`](../src/app/sim/NetworkConfigurator.cpp) computes it once globally; the
production peering commands (`peer add`) call the same methods.

## Assembling the two runtimes

### Simulation target

The OMNeT++ modules are **thin adapters**:

- [`Daemon`](../src/app/sim/Daemon.cpp) owns a `Node`, constructs it with the simulation port
  adapters ([`src/adapters/sim/`](../src/adapters/sim)), forwards `handleMessage` self-messages to
  the `Node`'s timer and received bytes to its packet handler, and exposes the `Node` API to
  `Publisher`/`Browser`.
- [`Publisher`](../src/app/sim/Publisher.cpp) / [`Browser`](../src/app/sim/Browser.cpp) run their
  timers and call the `Node` API (`publish_event`, `discover_*`).
- [`NetworkConfigurator`](../src/app/sim/NetworkConfigurator.cpp) calls `add_neighbor`/`learn_route`
  on each node.
- the simulation telemetry adapter emits the `@signal`/`@statistic` declarations in
  [`Daemon.ned`](../src/app/sim/Daemon.ned), so the `.anf` charts read them by name.

Thousands of `Node` instances run in one process under the OMNeT++ scheduler; virtual time and
seeded RNG make every run reproducible.

### Production target

`lotid` constructs **one** `Node` with the OS port adapters ([`src/adapters/os/`](../src/adapters/os):
wall clock, epoll reactor/scheduler, UDP transport, CSPRNG, file store, Ed25519 keystore, logging
telemetry) and runs a single-threaded reactor (see [Execution model](#execution-model)). A
**control socket** exposes the `Node` API to the `loti` CLI. `loti verify` links the core's
validation/proof code directly and needs no daemon and no network.

Everything below the API line — the DAG, discoveries, validation, hashing, serialization — is
the *same compiled logic* in both targets.

## Execution model

The core is **single-threaded and non-blocking**: every entry point (`publishEvent`,
`onTimer`, `onPacketReceived`, an RPC call) runs to completion without waiting, and any waiting
is expressed by arming a `Scheduler` timer or issuing a `Transport` send. This is exactly how
OMNeT++ already processes events, and it is what lets the two runtimes exercise the identical
code path ordering.

- **Simulation:** the OMNeT++ event scheduler *is* the loop; `Scheduler::after` maps to
  `scheduleAt`, packet delivery to message reception.
- **Production:** one **reactor thread** owns the `Node`. Async I/O — sockets, disk, RPC — runs
  on helper threads or the OS async facility and posts *completed* work as tasks onto the
  reactor queue; the core processes them one at a time. The core needs **no locks** because it
  is only ever touched from the reactor thread. (A node that must scale beyond one core runs
  multiple independent `Node` reactors, sharded by identity — never by sharing one `Node`
  across threads.)

## Data and wire-format sharing

To keep simulation statistics predictive of production, the **core owns the canonical byte
encoding** of every stored object and every packet:

- The domain structs are serialized by core functions (the productized form of
  `calculateEventHash` / `calculate*Size` and the `Packet.msg` layouts).
- The **simulation transport** wraps those exact bytes in an INET chunk (e.g. a `BytesChunk`, or
  a `FieldsChunk` whose `chunkLength` equals the core encoding) so INET's packet-length
  statistics measure the real thing.
- The **production transport** puts the same bytes on a real datagram.

Result: the "chain fits in one UDP datagram," per-event overhead, and file-growth numbers you
measure in simulation are the numbers you get in production, because they are computed from one
serializer.

## Configuration mapping

The core takes a small `NodeConfig` struct (`clock_event_interval`, `discovery_expiry`); each
frontend fills it from its own source, and the core neither knows nor cares where the value came
from:

| Core setting | Simulation source | Production source |
| --- | --- | --- |
| `clock_event_interval` | NED `par("createClockEventInterval")` | `lotid --clock-interval` |
| `discovery_expiry` | NED `par("discoveryExpiryTime")` | `lotid --expiry` |
| event content | `par("contentLength")` (Publisher) | `loti publish` input |
| discovery cadence | `par("discover*Interval")` (Browser) | driven by `loti` calls |

The simulation's `omnetpp.ini` configurations (`EventBoundsDiscovery`, …) and the `lotid` flags
(plus the `loti init` config file) are two skins over the same settings.

## Telemetry and statistics parity

The core calls `Telemetry::record(...)` at fixed points — event created, clock event created,
each discovery started / aborted / completed, chain length/interval, discovery latency. One set
of hook points, two sinks:

- **Simulation:** the telemetry adapter ([`src/adapters/sim/telemetry.hpp`](../src/adapters/sim/telemetry.hpp))
  emits precomputed scalar signals for the `@statistic` declarations in
  [`Daemon.ned`](../src/app/sim/Daemon.ned); the `.anf` charts read them by name.
- **Production:** the logging telemetry adapter writes structured stderr lines (a metrics/Prometheus
  sink is a post-MVP item).

Because both sinks are fed from the same call sites, the statistics you validate at scale in
simulation are the very metrics you monitor in production.

## Build and packaging

One source tree, three link targets:

- **`loti-core`** — a library with **zero** OMNeT++/INET/OS dependencies. Builds standalone;
  unit-testable on its own; cross-compilable.
- **the OMNeT++ simulation** — links `loti-core` + the OMNeT++ adapter modules; built by
  [`scripts/build-sim.sh`](../scripts/build-sim.sh) (`opp_makemake`; [`.oppbuildspec`](../.oppbuildspec),
  [`makefrag`](../makefrag)).
- **`lotid` / `loti`** — link `loti-core` + the OS adapters + the control socket / CLI; built by
  [`scripts/build-core.sh`](../scripts/build-core.sh) (CMake).

The core builds under both `make MODE=debug/release` (for the sim) and the native build (for the
product); it must not require OMNeT++ to compile, which is the concrete test that the dependency
rule is being honored.

## Directory layout

```
src/
  core/                  loti-core — pure protocol, no OMNeT++/OS deps
    domain/                Event, ClockEvent, EventChain, … (plain structs)
    hash/                  picosha2.hpp + canonical serialization (hashing, byte sizes)
    wire/                  length-prefixed codec + the three datagram types
    ports/                 Clock, Scheduler, Transport, Rng, Signer, Telemetry (abstract)
    proof/                 portable proof serialization + offline verification
    node.{hpp,cpp}         the Node — DAG, discoveries, validation, orchestration
  adapters/
    sim/                   OMNeT++ port implementations (header-only)
    os/                    wall clock, epoll reactor, UDP, CSPRNG, file store, Ed25519 keystore, control socket
  app/
    sim/                   Daemon/Publisher/Browser/NetworkConfigurator (thin cSimpleModules) + .ned
    lotid/                 the node daemon (control socket + reactor)
    loti/                  the CLI client (+ offline verify)
sim/                     example network, omnetpp.ini, Analysis.anf
test/
  core/                  fast unit tests of loti-core (doctest, fake ports)
  harness/               in-process multi-node deterministic integration world
  acceptance/            production integration (real sockets, disk, restart)
bin/  doc/  plan/  scripts/     run wrappers · docs · plans · build scripts
```

## Testing strategy

Three tiers, each using the same core:

1. **Core unit tests** — instantiate a `Node` with **fake ports** (`FakeClock`,
   `FakeScheduler`, `FakeTransport`, `SeededRng`, `NullSigner`, `NoopTelemetry`) and assert
   protocol behavior directly. Milliseconds, no OMNeT++.
2. **In-process multi-node harness** — wire N cores together through a `FakeTransport` that
   routes bytes between them with a scriptable delay/loss model, driven by a tiny deterministic
   scheduler. This gives fast, fully reproducible multi-node integration tests (chain discovery
   across several hops, expiry, ordering) without the weight of OMNeT++.
3. **OMNeT++ simulation** — the existing large-network statistical experiments
   ([simulation.md](simulation.md)); the authority for scale, topology, and bandwidth/storage
   numbers.

4. **Production integration tests** of `lotid`/`loti` (real sockets, real disk, restart/backup) —
   implemented as [test/acceptance/run.sh](../test/acceptance/run.sh): restart survival,
   `db backup`/`restore`, and a multi-node notary proof (B proves A's event over real UDP and it
   verifies offline). This is the M4 acceptance gate.

Tiers 1–3 are implemented under `test/core/` (doctest), `test/harness/` (the in-process world),
and the OMNeT++ `sim/` example respectively.

## Reproducibility and replay

Because all nondeterminism enters through ports, the core is a deterministic state machine over
its port inputs. Two payoffs:

- **Simulation** replays exactly from a seed (already true of OMNeT++, preserved by keeping the
  core deterministic and floating-point-free on the hot path).
- **Production** can *record* the sequence of port inputs a `Node` saw (received packets, timer
  firings, clock readings, RNG draws) and later *replay* it against the same core build to
  reproduce a bug offline — turning a live incident into a deterministic test.

## Risks and things to get right

- **Timestamp type.** Use a wide, explicit integer (int64 nanoseconds since epoch) for
  `Timestamp` so `simtime_t` (sim) and `system_clock` (prod) both map in without loss; never
  serialize a platform-specific time type.
- **Keep the core free of ambient state.** No globals, no singletons, no `static` mutable data —
  otherwise the multi-node simulation and the deterministic harness break.
- **No blocking in the core.** A single synchronous disk or socket call inside the core would
  stall every co-located node in the simulation and the whole reactor in production.
- **One serializer.** If the sim and prod encodings ever diverge, byte-accounting parity is lost;
  enforce it by having only `loti-core` encode/decode.
- **RNG discipline.** Deterministic seeded RNG in sim, CSPRNG in prod — but the *core* must never
  assume which; anything security-critical (salts, keys) is only strong because the production
  adapter makes it so.
- **Signing cost at scale.** Real signatures on every clock event are cheap per node but
  significant across a large simulation; keep the sim `Signer` a stub (or a fast cheap key) so
  scale experiments stay fast, while measuring signature size in the byte accounting.
