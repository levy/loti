# Architecture — One Codebase, Two Runtimes

This document describes how to structure LOTI so that **a single implementation of the
protocol** serves two very different runtimes from the same source:

1. a **discrete-event simulation** (OMNeT++ / INET) for experimenting with behavior on large
   networks and collecting statistics — what the repository is today
   ([implementation.md](implementation.md), [simulation.md](simulation.md)); and
2. a **real, deployable node** (`lotid`) driven by the [product CLI](cli.md) (`loti`).

The two runtimes disagree about almost every *environmental* concern — time, networking,
storage, randomness, concurrency, configuration, and how results are recorded — but they must
agree **exactly** about the protocol: how events and clock events are formed and hashed, how
the DAG grows, how the three discoveries route and build chains, and how chains are validated.
The architecture's whole job is to isolate the first set of concerns so the second can be
written once.

> **Status (MVP, 2026-07):** this architecture is **realized**. The pure core (`core/`, no
> OMNeT++/INET/OS deps) runs behind six ports; the simulation drives it via `adapters/sim/` +
> `app/sim/`, and the production node via `adapters/os/` + `app/lotid` + `app/loti`. Both targets
> are green: the OMNeT++ `SimpleNetwork` example runs on the core with unchanged statistics, and
> the production node passes an end-to-end acceptance suite (restart survival, backup/restore,
> and a multi-node offline-verifiable proof over real UDP —
> [test/acceptance/run.sh](../test/acceptance/run.sh)). The MVP command surface and its deviations
> from the design are tracked in [cli.md → Implementation status](cli.md#implementation-status-mvp).

## The core idea: functional core, imperative shell

Use **ports and adapters** (hexagonal architecture). The protocol lives in a pure **core
library** that depends only on a small set of abstract **ports** (interfaces) for everything
environmental. Two sets of **adapters** implement those ports — one backed by OMNeT++, one
backed by the real OS — and two **frontends** drive the core: the OMNeT++ application modules
in simulation, and the RPC/CLI in production.

```
        ┌──────────────── frontends ────────────────┐
        │  OMNeT++ apps (Publisher/Browser)   loti CLI ⇄ RPC server │
        └───────────────┬───────────────────────┬────┘
                        │        core API        │
                ┌───────▼────────────────────────▼───────┐
                │            loti-core (pure)             │
                │  DAG · events · clock events · the 3    │
                │  discoveries · chain validation ·       │
                │  hashing · canonical serialization      │
                └───────┬───────────────────────┬─────────┘
                        │        ports           │   (abstract interfaces)
        Clock · Scheduler · Transport · Rng · Store · Signer · Config · Telemetry
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
  and the discovery records, as plain structs (today these are OMNeT++ message classes in
  [`Data.msg`](../src/Data.msg); see [Migration](#migration-from-the-current-code)).
- **Hashing & canonical serialization** — SHA-256 over the fixed byte layout in
  `calculateEventHash` / `calculateClockEventHash`, using the already-portable header-only
  [`picosha.h`](../src/picosha.h). The wire encoding of every packet lives here too, so both
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

The core exposes the same surface the current [`Daemon`](../src/Daemon.h) exposes to its
sibling modules, plus the peering hooks — this is what both frontends call:

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

| Concern | Core port (sketch) | Simulation adapter | Production adapter | Coupled today in `Daemon` |
| --- | --- | --- | --- | --- |
| **Time** | `Timestamp Clock::now()` | OMNeT++ `simTime()` | `system_clock::now()` | `simTime()` |
| **Scheduling** | `TimerId Scheduler::after(Duration, cb)`, `cancel(TimerId)` | `scheduleAt` + self-messages | event loop / `timerfd` | `scheduleCreateClockEventTimer`, `purgeDiscoveriesTimer` |
| **Transport** | `void Transport::send(NodeId, Bytes)`; delivers via `onPacketReceived` | `UdpSocket::sendTo` | real UDP / QUIC socket | `socket`, `sendToNeighbor` |
| **Randomness** | `void Rng::fill(span)` (→ `salt()`) | seeded `intuniform` (reproducible) | CSPRNG | `generateSalt` (`intuniform`) |
| **Storage** | `Store` — append/get events, clock chain, indices, discoveries | in-RAM vectors/maps | LMDB / RocksDB / SQLite | the `vector`/`map` members of `Daemon` |
| **Identity/crypto** | `Signer::sign/verify`; `Hasher::sha256` (shared, deterministic) | stub or cheap keys | Ed25519 + keystore | none (hashing only, no signing) |
| **Configuration** | `Duration Config::duration(key)`, `int Config::integer(key)`, … | NED `par()` / `omnetpp.ini` | `config.toml` + flags + env | `par("createClockEventInterval")` etc. |
| **Telemetry** | `Telemetry::record(Signal, const Record&)` hooks | `emit(signal, obj)` → result recorders | structured logs + metrics | `emit(clockEventCreatedSignal, …)` etc. |

Notes:

- **`Hasher` is shared, not swapped.** Hashing must be identical everywhere, so both runtimes
  use the same implementation; only *keys/signatures* differ (stubbed in sim, real in prod).
- **`Rng` has one interface, two guarantees.** In simulation it is the seeded OMNeT++ RNG so
  runs replay exactly; in production it is a CSPRNG because salts (and keys) are
  security-critical. The core just asks for a salt.
- **`Store` is where "ephemeral vs durable" lives.** The simulation uses an in-RAM store (state
  dies with the run, which is fine); production uses a persistent store that survives restarts
  and backs every future proof. The core's access pattern is identical.
- **Neighbor/routing state** is core state fed through `addNeighbor` / `learnRoute`. The
  simulation's [`NetworkConfigurator`](../src/NetworkConfigurator.cc) computes it once globally;
  the production peering subsystem calls the same methods as peers come and go.

## Assembling the two runtimes

### Simulation target

The existing OMNeT++ modules become **thin adapters**:

- [`Daemon`](../src/Daemon.cc) shrinks to: own a `Node`, construct it with the simulation port
  bundle (`SimClock`, `SimScheduler`, `SimTransport`, `SimRng`, `InMemoryStore`, `StubSigner`,
  `NedConfig`, `SignalTelemetry`), forward `handleMessage` self-messages to `Node::onTimer`,
  forward `socketDataArrived` bytes to `Node::onPacketReceived`, and expose `Node`'s API to
  `Publisher`/`Browser`.
- [`Publisher`](../src/Publisher.cc) / [`Browser`](../src/Browser.cc) keep their timers but call
  the `Node` API (`publishEvent`, `discover*`) instead of protocol internals.
- [`NetworkConfigurator`](../src/NetworkConfigurator.cc) calls `addNeighbor`/`learnRoute` on each
  node instead of writing private fields.
- `SignalTelemetry` translates core telemetry hooks into the exact `@signal`/`@statistic`
  declarations already in [`Daemon.ned`](../src/Daemon.ned) — so all existing charts keep
  working unchanged.

Thousands of `Node` instances run in one process under the OMNeT++ scheduler; virtual time and
seeded RNG make every run reproducible.

### Production target

`lotid` is a small program that constructs **one** `Node` with the OS port bundle (`WallClock`,
`ReactorScheduler`, `UdpTransport`, `SecureRng`, `PersistentStore`, `Ed25519Signer`,
`FileConfig`, `LogMetricsTelemetry`) and runs a single-threaded reactor (see
[Execution model](#execution-model)). A **control-socket RPC server** exposes the `Node` API to
the `loti` CLI. `loti verify` links the core's validation/proof code directly and needs no
daemon and no network.

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

Both runtimes present parameters through the `Config` port, so the core reads
`config.duration("clock.interval")` and neither knows nor cares where the value came from:

| Core key | Simulation source | Production source |
| --- | --- | --- |
| `clock.interval` | `par("createClockEventInterval")` | `config.toml` `clock.interval` |
| `discovery.expiry` | `par("discoveryExpiryTime")` | `config.toml` `discovery.expiry` |
| `publish.content_length` | `par("contentLength")` (Publisher) | CLI `loti publish` input |
| `discovery.*_interval` | `par("discover*Interval")` (Browser) | driven by CLI calls |

The simulation's `omnetpp.ini` configurations (`EventBoundsDiscovery`, …) and the production
`config.toml` are two skins over the same key set.

## Telemetry and statistics parity

The core calls `Telemetry::record(...)` at fixed points — event created, clock event created,
each discovery started / aborted / completed, chain length/interval, discovery latency. One set
of hook points, two sinks:

- **Simulation:** `SignalTelemetry` emits the OMNeT++ signals in [`Daemon.ned`](../src/Daemon.ned)
  and the derived quantities computed by the [result filters](../src/ResultFilter.cc); the
  existing `.anf`/charts are unchanged.
- **Production:** `LogMetricsTelemetry` writes structured logs and Prometheus counters/histograms
  (surfaced by `loti stats` / `loti metrics`).

Because both sinks are fed from the same call sites, the statistics you validate at scale in
simulation are the very metrics you monitor in production.

## Build and packaging

One source tree, three link targets:

- **`loti-core`** — a library with **zero** OMNeT++/INET/OS dependencies. Builds standalone;
  unit-testable on its own; cross-compilable.
- **`loti-sim`** — links `loti-core` + the OMNeT++ adapter modules; built with the existing
  OMNeT++ `opp_makemake` flow ([`.oppbuildspec`](../.oppbuildspec), [`makefrag`](../makefrag)).
- **`lotid` / `loti`** — links `loti-core` + the OS adapters + the RPC server / CLI; built with
  CMake (or the project's chosen native build).

The core builds under both `make MODE=debug/release` (for the sim) and the native build (for the
product); it must not require OMNeT++ to compile, which is the concrete test that the dependency
rule is being honored.

## Proposed directory layout

```
core/                    loti-core — pure protocol, no OMNeT++/OS deps
  domain/                Event, ClockEvent, EventChain, … (plain structs)
  hash/                  picosha.h + canonical serialization
  dag/                   insertEvent/insertClockEvent, cross-links, indices
  discovery/             chain/bounds/order + the four chain primitives
  validate/              chain validation + proof build/verify
  ports/                 Clock, Scheduler, Transport, Rng, Store, Signer, Config, Telemetry (abstract)
  node.{h,cpp}           the Node API and orchestration
adapters/
  sim/                   OMNeT++ port implementations
  os/                    wall-clock, reactor, UDP/QUIC, CSPRNG, persistent store, Ed25519, toml
app/
  sim/                   Daemon/Publisher/Browser/NetworkConfigurator (thin cSimpleModules) + .ned
  lotid/                 the daemon main + RPC server
  loti/                  the CLI client (+ offline verify)
sim/                     example networks, omnetpp.ini, Analysis.anf  (unchanged)
test/
  core/                  fast unit tests of loti-core with fake ports
  harness/               in-process multi-node deterministic integration tests
```

## Testing strategy

Three tiers, each using the same core:

1. **Core unit tests** — instantiate a `Node` with **fake ports** (`FakeClock`,
   `FakeScheduler`, `RecordingTransport`, `SeededRng`, `InMemoryStore`) and assert protocol
   behavior directly. Milliseconds, no OMNeT++.
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

## Migration from the current code

The current [`Daemon.cc`](../src/Daemon.cc) already isolates most protocol logic in private
methods; it is coupled to OMNeT++ mainly at a handful of seams. An incremental path:

1. **De-OMNeT++ the domain model.** Replace the `cObject`-derived types in
   [`Data.msg`](../src/Data.msg) with plain structs in `core/domain/`, and move
   `calculateEventHash` / serialization off INET's `MemoryOutputStream` onto a core serializer
   (picosha stays). Keep the `.msg` packet types only as a thin sim-transport wrapper around the
   core's canonical bytes.
2. **Introduce the ports** and route the existing seams through them: `simTime()` → `Clock`,
   `scheduleAt`/self-messages → `Scheduler`, `socket`/`sendToNeighbor` → `Transport`,
   `generateSalt` → `Rng`, the member `vector`/`map`s → `Store`, `par(...)` → `Config`,
   `emit(...)` → `Telemetry`.
3. **Extract `Node`.** Move the protocol methods verbatim into `core/node`, taking ports by
   reference; shrink `Daemon` to the adapter described above. This is mostly mechanical because
   the logic already lives in free-standing helpers.
4. **Add the production adapters and `lotid`/`loti`**, plus persistence and signing (the
   [gap items](paper-vs-implementation.md)); ship the CLI in [cli.md](cli.md).

A concrete before/after of one seam — creating a clock event:

```cpp
// before (Daemon.cc): environment baked in
clockEvent.setTimestamp(simTime());
clockEvent.setSalt(generateSalt());                 // intuniform
...
scheduleAt(simTime() + par("createClockEventInterval"), &createClockEventTimer);
emit(clockEventCreatedSignal, &clockEvent);
for (auto& n : neighbors) sendClockEventNotification(n.second, clockEvent);

// after (core Node): environment injected through ports
clockEvent.timestamp = clock.now();
clockEvent.salt      = rng.salt();
...
scheduler.after(config.duration("clock.interval"), [this]{ onCreateClockEventTimer(); });
telemetry.record(Signal::ClockEventCreated, clockEvent);
for (auto& n : neighbors) transport.send(n.first, encodeClockEventNotification(clockEvent));
```

The protocol statements are unchanged; only the *effectful* calls now go through ports, which is
precisely what lets the same lines run inside a 10,000-node simulation and inside a single
production daemon.

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
