# Paper vs. Implementation

This is the gap analysis: what the paper ([theory.md](theory.md)) describes versus what the
current simulation model ([implementation.md](implementation.md)) actually does. The code is
a **research simulation** meant to validate feasibility and gather statistics — it is not a
hardened, deployable node. Several ideas from the paper are deliberately simplified or left
out.

## Fully implemented

| Paper concept | In the code |
| --- | --- |
| Events and clock events as hash-linked data structures | `Event`, `ClockEvent`, `LocalClockEvent` in [`Data.msg`](../src/core/domain/types.hpp) |
| Clock events carry a **local** timestamp; no global clock | `LocalClockEvent.timestamp = simTime()`; comparisons are always in the originator's local time |
| Clock event references: previous local clock event, latest neighbor clock events, unreferenced local events | `Daemon::insertClockEvent` |
| Events reference a recent local clock event | `Daemon::insertEvent` (references the last local clock event) |
| Optional random salt | `generateSalt` (64 bits), hashed into every event/clock event |
| SHA-256 hashing over a canonical serialization | `calculateEventHash` / `calculateClockEventHash` using [`picosha2.hpp`](../src/core/hash/picosha2.hpp) |
| Broadcast clock-event **hashes** to neighbors; do not distribute events | `LT_CLOCK_EVENT_NOTIFICATION`; events never sent on the wire |
| Ever-growing distributed clock-event DAG with cross-links | `referencedEvents` (forward) + `referencingEvents` (reverse) |
| Proving order by exhibiting a hash chain | `EventChain` and `validateEventChain` |
| Proving time boundaries via enclosing clock events | event bounds discovery → `(lowerBound, upperBound)` timestamps |
| Finding order by comparing two events' intervals (before / after / undetermined) | `compareEventChains` → `-1 / +1 / 0` |
| Recursive search along the neighbor hierarchy toward the originator, extending the chain per hop | request/response routing + the four `add*/extend*` primitives |
| Chain validation at the originator (hashes, linkage, endpoints are local) | `validateEventChainDiscoveryResult` + `validateEventChain` |
| Discoveries expire after a configurable time | `discoveryExpiryTime`, `purgeDiscoveriesTimer` |
| Per-node storage and efficiency measured | file-length vectors + discovery statistics ([simulation.md](simulation.md)) |

## Partially implemented / simplified

| Paper concept | What the code does instead |
| --- | --- |
| **"Probabilistic beam search"** along a hierarchical routing table | A single **deterministic shortest-path** walk. Each hop follows one `destinationToNextHop` entry (beam width 1); there is no probabilistic branching, no multiple candidate paths, and no redundancy in a single discovery. |
| **Hierarchical routing table maintained over time** | A static routing/neighbor table computed **once** at init by `NetworkConfigurator` from the physical topology (unweighted shortest paths). It never changes during the run. |
| **Overlay distinct from the physical network** | Supported in shape (`neighbors` + `destinationToNextHop` are an app-level overlay), but in the bundled example the overlay equals the physical topology, so neighbors are always directly connected. |
| Clock events reference **selected** (~10) neighbor clock events with smaller timestamps | Every neighbor's single latest known clock event is referenced; there is no selection policy and no timestamp filtering (consistent with the paper's "not necessarily for neighbors" caveat, but not the "selected subset" intent). |
| Events reference **selected** local clock events | Only the single most recent local clock event is referenced. |
| Event **bounds** discovery could run several chain discoveries for tighter/redundant/fault-tolerant results | Exactly **one** chain discovery backs each bounds discovery. |
| **Consistency** ("no conflicting orderings") | Emerges from hash-chain integrity and local-clock comparison, and is validated per chain, but there is no explicit cross-node conflict detection or reconciliation. |
| Node identity | `NodeId = getId()` — the OMNeT++ module id. It is a convenient unique id, **not** a cryptographic identity. |

## Not implemented

| Paper concept | Status |
| --- | --- |
| **Public/private keys; signing events and clock events** | Not implemented. There are no keys and no signatures anywhere; the "optionally sign" steps are omitted. Identity and authenticity are not cryptographically enforced. |
| **Transfer of value / incentives** to make operators answer queries | Not implemented. No payments, credits, or accounting of who answered what. |
| **Dynamic topology** (neighbors and routes changing over time) | Not implemented. Topology and routing are fixed at initialization. |
| **Permissionless / trustless participation, sybil resistance** | Out of scope for the simulation; the node set is fixed and fully cooperative. There is no admission control, misbehavior handling, or defense against a lying/forging node — validation only catches an *inconsistent* chain, not a *dishonest but internally consistent* one. |
| **Oracles / real-world event relationships** | Not modeled. Event content is random bytes. |
| **Sharing events** between nodes (the optional part) | Not implemented; the browser reaches into other daemons directly (an out-of-band oracle) rather than receiving shared events over the network. |
| **Attack-prevention analysis** (forging past/future) | Discussed in the paper only; the code neither performs nor tests these attacks. The security rests on hashing, which is present, but no adversarial scenario is simulated. |
| **Multiple / redundant chains** per proof | A discovery yields a single chain. |

## Known issues and vestigial code

- **`Callback.h` is dead code.** It declares `IEventChainDiscoveryCallback`
  / `IEventBoundsDiscoveryCallback` / `IEventOrderDiscoveryCallback` with `...Failed` methods,
  but it is **not `#include`d anywhere**, references `Event`/`EventChain` without including
  their definitions (so it would not even compile if used), and is superseded by the
  identically named interfaces (with `...Aborted` methods) defined inline in
  [`node.hpp`](../src/core/node.hpp). It can be deleted.
- **`EventChainDiscoveryParticipation`** (in `Data.msg`) is declared but never used.
- **Salt space.** The salt is 64 bits of `intuniform`-derived randomness — fine for a
  simulation, but a real deployment would want a cryptographically strong, larger nonce.
- **`findRandomEvent` can pick the same event twice** for an order discovery, and the two
  events may have been created by any node including ones not yet reachable, contributing to
  the ~17% of discoveries that abort/expire in the example run.
- **Hardcoded UDP port 666** and a fixed `app[0]` slot for the daemon (`NetworkConfigurator::findDaemon`).

## Where the code could go next

Roughly in order of how directly the paper motivates them:

1. Sign events/clock events and give nodes real key-based identities.
2. Replace the single shortest path with a genuine probabilistic **beam search** (multiple
   concurrent candidate chains, widening on failure) for redundancy and tighter bounds — see
   [dynamic-discovery.md](dynamic-discovery.md) for the full design.
3. Run several chain discoveries per bounds discovery and merge the tightest interval.
4. Make the overlay and routing **dynamic** (neighbors join/leave, routes recompute).
5. Add an incentive/value-transfer layer for answering queries.
6. Model dishonest nodes and the forging-the-past / forging-the-future attacks to
   empirically confirm the hash-based defenses.
7. Distribute events (optionally) instead of using the direct-access oracle in `Browser`.

The [CLI design specification](cli.md) turns this list into a concrete product surface —
a node daemon plus client, key-based signing, live peering, and portable offline-verifiable
proofs.
