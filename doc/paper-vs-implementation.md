# Paper vs. Implementation

This is the gap analysis: what the paper ([theory.md](theory.md)) describes versus what the
current code — the `Node` engine in [`src/core`](../src/core)
([implementation.md](implementation.md)), hosted by the OMNeT++ simulation
([simulation.md](simulation.md)) and by the production node,
[`lotid`](../src/app/lotid)/[`loti`](../src/app/loti) ([cli.md](cli.md)) — actually does. The
simulation remains a research tool for validating feasibility and gathering statistics on large
networks; the production node is a real, if still-growing, deployable implementation (its exact
command surface and MVP scope are in [cli.md → Implementation status](cli.md#implementation-status-mvp)).
Several ideas from the paper are still deliberately simplified or left out, in both hosts.

## Fully implemented

| Paper concept | In the code |
| --- | --- |
| Events and clock events as hash-linked data structures | `Event`, `ClockEvent`, `LocalClockEvent` in [`domain/types.hpp`](../src/core/domain/types.hpp) |
| Clock events carry a **local** timestamp; no global clock | `LocalClockEvent.timestamp = clock_.now()`; comparisons are always in the originator's local time |
| Clock event references: previous local clock event, latest neighbor clock events, unreferenced local events | `Node::insert_clock_event` |
| Events reference a recent local clock event | `Node::insert_event` (references the last local clock event) |
| Optional random salt | `Rng::next_salt` (64 bits), hashed into every event/clock event |
| SHA-256 hashing over a canonical serialization | `hash::calculate_event_hash` / `hash::calculate_clock_event_hash` using [`picosha2.hpp`](../src/core/hash/picosha2.hpp) |
| Broadcast clock-event **hashes** to neighbors; do not distribute events | `wire::Type::clock_notification`; events never sent on the wire |
| Ever-growing distributed clock-event DAG with cross-links | `referenced_events` (forward) + `referencing_events` (reverse) |
| Proving order by exhibiting a hash chain | `EventChain` and `Node::validate_event_chain` |
| Proving time boundaries via enclosing clock events | event bounds discovery → `(lower_bound, upper_bound)` timestamps |
| Finding order by comparing two events' intervals (before / after / undetermined) | `Node::compare_event_chains` |
| Recursive search along the neighbor hierarchy toward the originator, extending the chain per hop | request/response routing + the four `add_local_*`/`extend_*_for_neighbor` primitives |
| Chain validation at the originator (hashes, linkage, endpoints are local) | `Node::validate_chain_discovery_result` + `Node::validate_event_chain` |
| Discoveries expire after a configurable time | `NodeConfig::discovery_expiry`, `Node::purge_discoveries` |
| Per-node storage and efficiency measured | file-length vectors + discovery statistics ([simulation.md](simulation.md)) |
| **Public/private keys; signing events and clock events** | `Ed25519KeyStore` ([`adapters/os/keystore.hpp`](../src/adapters/os/keystore.hpp)) signs every published/clock event via the Signer port; `Node::validate_event_chain` verifies each signature; `NodeId` in production is the key's fingerprint |
| **Persistent local state** | `lotid` persists **incrementally** to an LMDB embedded store ([`lmdb_store.hpp`](../src/adapters/os/lmdb_store.hpp)) and hydrates its working set from it on restart; `Node::snapshot()` / `Node::restore()` ([`node.hpp`](../src/core/node.hpp)) are the portable blob format behind `db backup` / `db restore` (`db stat` reports the store) |
| **A node daemon + client, with a control channel** | `lotid` (long-running node) + `loti` (CLI) talking over a control socket ([`src/app/lotid`](../src/app/lotid), [`src/app/loti`](../src/app/loti)) |
| **Portable, offline-verifiable proofs** | `loti::proof::Proof` / `serialize` / `verify` ([`core/proof`](../src/core/proof)); `loti prove bounds\|order\|chain`, `loti verify`, `loti proof show` |

## Partially implemented / simplified

| Paper concept | What the code does instead |
| --- | --- |
| **"Probabilistic beam search"** along a hierarchical routing table | A single **deterministic shortest-path** walk in both hosts. Each hop follows one `destination_to_next_hop_` entry (beam width 1); there is no probabilistic branching, no multiple candidate paths, and no redundancy in a single discovery. |
| **Hierarchical routing table maintained over time** | Simulation: a static table computed **once** at init by `NetworkConfigurator` from the physical topology. Production: `loti peer add` can add a direct route to a new neighbor at any time while `lotid` is running (a real improvement over the simulation's one-shot setup), but there is no route *recomputation* — routes only ever reach directly-added peers, and `peer rm`, `route ls`, and `overlay export/import` remain design-only (see [cli.md](cli.md)). |
| **Overlay distinct from the physical network** | Simulation: supported in shape (`neighbors_` + `destination_to_next_hop_` are an app-level overlay), but the bundled example's overlay equals the physical topology, so neighbors are always directly connected. Production: peer addresses are operator-supplied (`peer add <id:ip:port>`), so the overlay is inherently independent of any physical topology, but it is exactly the set of manually-added direct peers — there is no peer discovery. |
| Clock events reference **selected** (~10) neighbor clock events with smaller timestamps | Every neighbor's single latest known clock event is referenced; there is no selection policy and no timestamp filtering (consistent with the paper's "not necessarily for neighbors" caveat, but not the "selected subset" intent). |
| Events reference **selected** local clock events | Only the single most recent local clock event is referenced. |
| Event **bounds** discovery could run several chain discoveries for tighter/redundant/fault-tolerant results | Exactly **one** chain discovery backs each bounds discovery. |
| **Consistency** ("no conflicting orderings") | Emerges from hash-chain integrity and local-clock comparison, and is validated per chain, but there is no explicit cross-node conflict detection or reconciliation. |
| Node identity | Simulation: `NodeId = getId()` — the OMNeT++ module id, a convenient unique id paired with a no-op signer (`NullSigner`), **not** a cryptographic identity. Production: `NodeId = Ed25519KeyStore::fingerprint(pubkey)` — a real cryptographic identity, and every event/clock event is genuinely signed. |
| A trusted **reference node** ("notary") whose clock a proof is expressed in | A node can prove a *peer's* event via remote `<creator>:<hash>` addressing, becoming that proof's de facto reference (exercised over real UDP by `test/acceptance/run.sh`). There is no `--reference <node>` selection, no notary directory or reputation system, and `verify --trust <node>` is advisory only — it warns but does not change the exit code (see [cli.md](cli.md#proofs--the-core-product-feature)). |

## Not implemented

| Paper concept | Status |
| --- | --- |
| **Transfer of value / incentives** to make operators answer queries | Not implemented. No payments, credits, or accounting of who answered what. |
| **A fully dynamic, self-maintaining overlay** (peers leaving, liveness checks, route recomputation) | Not implemented. `peer rm`, `peer ping`, `route ls`, and `overlay export/import` remain design/reference (see [cli.md](cli.md)); peers can only be added, never removed or health-checked, and routing never extends past a direct peer. |
| **Permissionless / trustless participation, sybil resistance** | Out of scope; there is no admission control or misbehavior handling beyond structural chain validation — that catches an *inconsistent* chain, not a *dishonest but internally consistent* one. |
| **Oracles / real-world event relationships** | Not modeled. Event content is arbitrary bytes (random, in the simulation; operator-supplied text, in production) with nothing linking it to an external fact. |
| **Sharing/importing events** between nodes | Not implemented. The simulation's `Browser` reaches into other daemons directly (an out-of-band oracle); production discovers and proves a peer's *already-published* event over the wire, but there is no `event import`/`event get` to pull another node's event content locally. |
| **Attack-prevention analysis** (forging past/future) | The paper's specific adversarial scenarios are discussed only, not simulated or tested. The production acceptance suite does exercise basic tamper detection (flipping a bit in a serialized proof makes `verify` fail with exit `6`), but that is integrity-checking, not an adversarial forging study. |
| **Multiple / redundant / multi-reference chains** per proof | A discovery or proof still yields a single chain anchored at a single reference node. |
| **Daemon lifecycle and operations surface**: process supervision (`node start`/`restart`), key rotation, the `clock` subcommands, `db gc`/`db verify`, `config get/set/list` (+ a config-file loader), `stats`/`metrics`, log tailing | All remain design/reference; see [cli.md → Implementation status](cli.md#implementation-status-mvp) for the exact command table. |

## Known issues and vestigial code

- **Salt space (simulation only).** The simulation's salt is 64 bits of seeded-RNG-derived
  randomness — fine for reproducible runs, but not cryptographically unpredictable. Production
  draws salts from the OS CSPRNG (`getrandom(2)`, [`adapters/os/rng.hpp`](../src/adapters/os/rng.hpp)),
  so this is not a production concern.
- **`findRandomEvent` can pick the same event twice** for an order discovery, and the two
  events may have been created by any node including ones not yet reachable, contributing to
  discoveries that abort/expire in the example run.
- **Hardcoded UDP port 666 and a fixed `app[0]` slot for the daemon** (`NetworkConfigurator::findDaemon`)
  are simulation-only; the production `lotid` takes a configurable `--port`.

## Where the code could go next

Roughly in order of how directly the paper motivates them:

1. Replace the single shortest path with a genuine probabilistic **beam search** (multiple
   concurrent candidate chains, widening on failure) for redundancy and tighter bounds — see
   [dynamic-discovery.md](dynamic-discovery.md) for the full design.
2. Run several chain discoveries per bounds discovery and merge the tightest interval; support
   multi-reference proofs (bounds according to several independent notaries at once).
3. Make peering and routing fully dynamic: peer removal and liveness checks, and route
   computation/recomputation beyond a direct hop as the network changes.
4. Add an incentive/value-transfer layer for answering queries.
5. Model dishonest nodes and the forging-the-past / forging-the-future attacks empirically, to
   confirm the hash-based defenses beyond basic tamper detection.
6. Support importing/sharing event content between nodes (`event import`/`event get`), instead
   of only discovering and proving events a peer already holds.
7. Round out the production operations surface — daemon supervision, key rotation, dynamic
   config, metrics export — tracked in [cli.md](cli.md)'s implementation-status and deferred
   lists.
