# Implementation

This document describes **how the LOTI protocol works**, as implemented by the `Node` class in
[`src/core`](../src/core) (`node.hpp` / `node.cpp`) — the pure protocol engine that holds the
DAG, runs the three discoveries, and validates chains. The `Node` touches no operating system
and no simulator directly; every effectful operation (time, scheduling, networking, randomness,
signing, telemetry) goes through one of six ports, so the same engine runs unmodified behind two
hosts:

- the OMNeT++ **simulation** modules in [`src/app/sim`](../src/app/sim) (`Daemon`, `Publisher`,
  `Browser`, `NetworkConfigurator`), for experimenting with behavior on large networks and
  collecting statistics; and
- the production **node**, [`lotid`](../src/app/lotid) (a long-running daemon) plus
  [`loti`](../src/app/loti) (its CLI client).

For the theory this realizes see [theory.md](theory.md); for the example network and results see
[simulation.md](simulation.md); for the exact list of what is and is not implemented see
[paper-vs-implementation.md](paper-vs-implementation.md); for how one codebase serves both
hosts see [architecture.md](architecture.md).

All source lives in [`src/`](../src); the runnable simulation example lives in [`sim/`](../sim).

## Module map

Each simulated host (`inet.node.inet.StandardHost`) runs three LOTI application modules on
top of a normal INET UDP/IPv4/Ethernet stack. A single network-wide helper module builds
the peer-to-peer overlay.

| Module | Kind | Role |
| --- | --- | --- |
| [`Daemon`](../src/app/sim/Daemon.cpp) | per-host app (`app[0]`) | Hosts a `Node`: forwards its clock-event timer and inbound UDP datagrams to it, and exposes `publishEvent`/`discoverEvent*` for the co-located Publisher/Browser modules. |
| [`Publisher`](../src/app/sim/Publisher.cpp) | per-host app (`app[1]`) | Periodically creates events with random content via the local daemon. |
| [`Browser`](../src/app/sim/Browser.cpp) | per-host app (`app[2]`) | Periodically picks random event(s) and asks the local daemon to run a discovery. |
| [`NetworkConfigurator`](../src/app/sim/NetworkConfigurator.cpp) | one per network | Builds the overlay neighbor table and next-hop routing table from the physical topology. |

The three apps all declare `like IApp` and connect to the host's UDP layer through
`socketIn`/`socketOut` gates, but only the `Daemon` actually opens a socket. `Publisher` and
`Browser` reach the daemon by a direct C++ method call (`getModuleFromPar<Daemon>`), not over
the network — they are co-located control clients of the daemon.

Discovery statistics are not produced by custom `cResultFilter`s; the telemetry port adapter
([`src/adapters/sim/telemetry.hpp`](../src/adapters/sim/telemetry.hpp)) computes each quantity
itself and emits it as a plain scalar signal (see [Statistics](#statistics)).

## Data model

The wire/stored data structures are plain C++ structs in
[`domain/types.hpp`](../src/core/domain/types.hpp) — the core has no dependency on the OMNeT++
message compiler, `cObject`, or `simtime_t`. The primitive typedefs:

```
NodeId    = uint64_t          // a node's identity: the module's getId() in simulation,
                               // an Ed25519 public-key fingerprint in production
Salt      = uint64_t          // random per-event salt
EventHash = vector<uint8_t>   // a 32-byte SHA-256 digest
Signature = vector<uint8_t>   // empty = unsigned; excluded from the hash
Timestamp = int64_t           // a raw clock tick (simtime_t::raw() in sim, a wall-clock tick in production)
Duration  = int64_t           // a difference of Timestamps
```

| Type | Fields | Notes |
| --- | --- | --- |
| `EventReference` | `creator: NodeId`, `hash: EventHash` | A typed pointer to another event/clock event. |
| `Event` | `creator`, `hash`, `data: Bytes`, `salt`, `referenced_events: [EventReference]`, `signature: Signature` | A published event. `data` is the content; `signature` is optional and excluded from the hash. |
| `ClockEvent` | `creator`, `hash`, `timestamp: Timestamp`, `salt`, `referenced_events`, `signature` | A timestamped event. |
| `LocalClockEvent` : `ClockEvent` | + `referencing_events: [EventReference]` | A clock event **created by this node**; also tracks who references it (reverse edges). |
| `EventChain` | `event: Event`, `lower_bound: deque<ClockEvent>`, `upper_bound: deque<ClockEvent>` | The proof object: a hash chain enclosing `event`. |
| `Neighbor` | `node_id`, `last_clock_event_hash` | An overlay neighbor and the newest clock event of theirs we know. Its transport address is tracked separately by the transport port adapter (see [The overlay](#the-overlay-networkconfigurator)). |
| `EventChainDiscovery` | `start_time`, `end_time`, `originator`, `event`, `chain`, `state` | Originator-side record of a chain discovery. |
| `EventBoundsDiscovery` | `start_time`, `end_time`, `event`, `lower_bound: Timestamp`, `upper_bound: Timestamp`, `state` | Result of a bounds discovery. |
| `EventOrderDiscovery` | `start_time`, `end_time`, `event1`, `event2`, `order`, `state` | Result of an order discovery. |

`DiscoveryState` is one of `in_progress`, `completed`, `aborted`. `Order` is `before` (-1,
event1 before event2), `after` (+1), or `undetermined` (0).

### The clock-event DAG, concretely

An `Event` embeds the hash of the last local clock event (a backward edge into the clock
chain). A `LocalClockEvent` embeds three kinds of backward edges in its `referenced_events`:

1. the **previous local clock event** (extends this node's own chain),
2. the **latest known clock event of each neighbor** (cross-links into neighbor DAGs),
3. **all local events created since the last clock event** (pins those events in time).

`referencing_events` on a `LocalClockEvent` holds the **reverse** cross-links: which neighbor
clock events point back at this one. These reverse edges are learned lazily from neighbor
notifications (see below) and are what makes the *upper* bound walkable.

## Hashing

Hashing uses the bundled header-only [`picosha2.hpp`](../src/core/hash/picosha2.hpp) (SHA-256).
Digests are `picosha2::k_digest_size` = 32 bytes. Serialization for hashing is explicit and
canonical, computed by free functions in
[`hash/hashing.cpp`](../src/core/hash/hashing.cpp) (`hash::calculate_event_hash` /
`hash::calculate_clock_event_hash`):

- **Event**: `data` bytes, then `salt` (big-endian u64), then for each referenced event its
  `creator` (u64) and `hash` bytes.
- **Clock event**: `timestamp` (u64), then `salt`, then each referenced event's
  `creator` and `hash`.

The `signature` field is deliberately excluded from this serialization — a signature signs the
hash, so attaching one never changes an event's or clock event's hash and never invalidates a
proof already issued.

The salt is 64 random bits supplied by the Rng port (`rng_.next_salt()`): the simulation adapter
draws it from the seeded OMNeT++ RNG (so runs replay exactly), production from a CSPRNG. Because
the hash covers the referenced hashes, every reference is a cryptographic commitment to the
referenced object's entire history — this is the property the security argument in
[theory.md](theory.md) relies on.

## Node: local state

`Node` holds (see [`node.hpp`](../src/core/node.hpp)):

- `id_` — this node's identity: an opaque `NodeId`. The simulation host derives it from the
  hosting module's `getId()`; the production node derives it from its Ed25519 public-key
  fingerprint (see [`Ed25519KeyStore`](../src/adapters/os/keystore.hpp)).
- `neighbors_: map<NodeId, Neighbor>` — the overlay neighbor set.
- `all_events_: vector<Event>` — every event this node published.
- `unreferenced_events_: vector<Event>` — events not yet pinned by a clock event.
- `all_clock_events_: vector<LocalClockEvent>` — this node's clock-event chain, in creation
  order (so index order == time order).
- `destination_to_next_hop_: map<NodeId, NodeId>` — overlay routing table.
- `event_hash_to_event_index_`, `event_hash_to_clock_event_index_` — hash → index lookups.
- `event_hash_to_referencing_event_index_: multimap<EventHash, size_t>` — for an event/clock-event
  hash, the index(es) of the **local clock event(s) that reference it** (forward lookup used
  to grow the upper bound).
- `chain_discoveries_`, `bounds_discoveries_`, `order_discoveries_` — in-flight and finished
  discoveries, each paired with the list of callbacks waiting on it. Chain discoveries are keyed
  by event hash; order discoveries by the `(hash1, hash2)` pair.
- the six ports (`clock_`, `scheduler_`, `transport_`, `rng_`, `signer_`, `telemetry_`, bundled
  as `NodePorts`) and `config_` (`NodeConfig{clock_event_interval, discovery_expiry}`), plus two
  timer handles (`clock_timer_`, `purge_timer_`) armed through the scheduler port.

The `Node` owns none of the runtime's resources itself — no socket, no thread, no wall clock. In
the simulation, `Daemon` (the OMNeT++ host) constructs a `Node` at `INITSTAGE_LOCAL` once
`getId()` is available, binds a `UdpSocket` to **port 666** at `INITSTAGE_APPLICATION_LAYER`, and
forwards inbound bytes to `Node::on_packet_received`. The overlay is filled in by
`NetworkConfigurator` at `INITSTAGE_NETWORK_LAYER_3` through `Daemon`'s public
`learnRoute()`/`addNeighbor()` methods, which call `Node::learn_route()`/`Node::add_neighbor()`
(see [The overlay](#the-overlay-networkconfigurator)).

## Growing the local DAG

### Publishing an event

`Publisher::processCreateEventTimer` fires every `createEventInterval` and calls
`Daemon::publishEvent(data)` with `contentLength` random bytes, which delegates to
`Node::publish_event(data)`. The private `insert_event`:

1. references the last local clock event (if any),
2. sets `creator = id_`, a random salt, computes the hash, and signs it via the Signer port,
3. appends to `all_events_`.

`publish_event` then indexes the new event, pushes it onto `unreferenced_events_`, and notifies
the Telemetry port (`on_event_created`, which the simulation adapter turns into the
`eventCreated` signal).

### Creating a clock event

The clock-event timer stays in the `Daemon` module (so it keeps re-sampling the volatile
`createClockEventInterval` parameter); firing it calls `Node::create_clock_event()`. The
private `insert_clock_event` builds `referenced_events` = {last local clock event} ∪ {each
neighbor's `last_clock_event_hash`, where known} ∪ {all `unreferenced_events_`}, stamps
`timestamp = clock_.now()`, salts, hashes, signs, and appends to `all_clock_events_`.
`create_clock_event` then:

1. indexes the new clock event by hash,
2. for each referenced event, records the reverse edge in `event_hash_to_referencing_event_index_`,
3. **clears `unreferenced_events_`** (they are now pinned),
4. notifies the Telemetry port (`on_clock_event_created`),
5. sends a **clock event notification** to every neighbor via the Transport port.

### Clock event notifications

`send_clock_event_notification(neighbor, clock_event)` sends a datagram carrying
`(last_clock_event_hash = my new clock event, neighbor_last_clock_event_hash = the newest clock
event of *that* neighbor I currently know)`.

`process_clock_event_notification` on the receiver:

1. updates `neighbor.last_clock_event_hash` to the sender's new clock event,
2. if `neighbor_last_clock_event_hash` names one of **my** clock events, appends a reverse edge
   to that local clock event's `referencing_events`: `(creator = sender, hash = sender's new
   clock event)`.

Only the **hash** of a clock event is ever broadcast — never its contents, and never events.
This is the "clock event hashes are distributed to neighbors; events are not distributed"
property from the paper.

## The overlay: NetworkConfigurator

INET's `Ipv4NetworkConfigurator` assigns addresses and installs shortest-path IPv4 routes.
LOTI's own `NetworkConfigurator` then builds the **application-level overlay** at
`INITSTAGE_NETWORK_LAYER_3`:

- It extracts the topology of `@networkNode` modules, and for every destination computes
  unweighted single-shortest-paths.
- For each `(source → destination)` pair it calls `sourceDaemon->learnRoute(destination,
  firstHop)` and `sourceDaemon->addNeighbor(firstHop, firstHopIpAddress)` — public `Daemon`
  methods that forward to `Node::learn_route()`/`Node::add_neighbor()` and, for the address, to
  the `SimTransport` adapter's `set_address()` (the transport port's own `NodeId → address`
  table; the domain `Neighbor` struct itself carries no address).

In the bundled example the overlay neighbor graph therefore coincides with the physical
topology (neighbors are directly connected). The paper's intent is that this second layer
could describe a different, non-physical overlay; the code supports that shape but the
example does not exercise it.

## Protocol packets

Three datagram types (see [`wire/packets.hpp`](../src/core/wire/packets.hpp)), each carrying a
one-byte `Type` and the **sender's** `NodeId` in its header (used by the receiver to look the
sender up in its `neighbors_` map):

| `wire::Type` | Payload | Direction |
| --- | --- | --- |
| `clock_notification` | `ClockNotification { last_clock_event_hash, neighbor_last_clock_event_hash }` | broadcast to each neighbor on every clock event |
| `chain_request` | `ChainRequest { originator, event: EventReference }` | routed toward the event's creator |
| `chain_response` | `ChainResponse { originator, chain: EventChain }` | routed back toward the originator |

`Node::on_packet_received` decodes the datagram (`wire::decode`), verifies the sender is a
known neighbor, then dispatches by type. The core owns the exact wire bytes
([`wire/codec.hpp`](../src/core/wire/codec.hpp)); the simulation transport wraps them unchanged
in an INET `BytesChunk` and the production transport puts the same bytes on a real UDP
datagram, so packet-length statistics measure the real protocol overhead. The byte-accounting
helpers used for storage/size statistics — `hash::event_size_bytes` / `hash::clock_event_size_bytes`
in [`hash/hashing.cpp`](../src/core/hash/hashing.cpp) — deliberately **exclude** an event's
`data` content from the accounted size.

## Event chain discovery — the core algorithm

An event chain discovery reconstructs an `EventChain` whose concatenation
`lower_bound · event · upper_bound` is a single hash chain that **begins and ends at the
originator's own clock events** and passes through the requested `event`. This is the proof
object described in [theory.md](theory.md).

> The algorithm below is the **width-1 special case**: one deterministic walk along the routing
> shortest path. How it generalizes to the paper's probabilistic beam search — and how that
> copes with a churning, lazily-linked network — is covered in
> [dynamic-discovery.md](dynamic-discovery.md).

### Starting (originator)

`Node::discover_event_chain(event, callback)`:

- If a discovery for `event.hash` already exists, the call **coalesces**: attach the callback
  (in progress), or immediately replay the aborted/completed result.
- Otherwise create an `EventChainDiscovery` (state `in_progress`) and:
  - **If this node is the creator**, build the chain locally — `add_local_lower_bound` +
    `add_local_upper_bound` — and complete immediately (no packets).
  - **Otherwise** look up `find_next_hop_neighbor(event.creator)` and send a `ChainRequest`
    with `originator = this node`.

Only the **originator** keeps an `EventChainDiscovery` record. Intermediate and creator nodes
handle request/response packets statelessly, building an `EventChain` on the stack and
forwarding it.

### Routing the request

`process_chain_request`:

- **Not the creator** → forward the request to `find_next_hop_neighbor(event.creator)`.
- **Is the creator** → seed `chain.event`, then in order
  `add_local_lower_bound`, `add_local_upper_bound`, `extend_lower_bound_for_neighbor(sender)`,
  `extend_upper_bound_for_neighbor(sender)`; if all succeed, send a `ChainResponse` back to the
  neighbor the request came from. Any failing step drops the discovery without sending a
  response (it will later expire at the originator via purge — see
  [Discovery lifecycle and expiry](#discovery-lifecycle-and-expiry)).

### The four chain-building primitives

These are the heart of the algorithm (all in [`node.cpp`](../src/core/node.cpp)):

- **`add_local_lower_bound(chain)`** — look at the `referenced_events` of the current front of
  `lower_bound` (or of `event` if `lower_bound` is empty); find one created by *this* node,
  resolve it to a local clock event, and `push_front` it. This prepends the local clock event
  that the current front *references* (a step backward/older).

- **`add_local_upper_bound(chain)`** — take the hash of the current back of `upper_bound` (or
  of `event` if empty), look it up in `event_hash_to_referencing_event_index_`, and `push_back`
  the first local clock event that *references* it (a step forward/newer).

- **`extend_lower_bound_for_neighbor(neighbor, chain)`** — starting from the front local clock
  event, walk **backward** through `all_clock_events_` (`index--`, `push_front` each), stopping
  when a clock event whose `referenced_events` include one created by `neighbor` reaches the
  front. Returns `false` if the start of the chain is reached first.

- **`extend_upper_bound_for_neighbor(neighbor, chain)`** — the mirror image: walk **forward**
  (`index++`, `push_back`) until a local clock event whose `referencing_events` include one
  created by `neighbor` reaches the back. Returns `false` if the end of the chain is reached
  first.

The two `extend*` primitives are why a node "takes care of providing enough local clock
events for the next neighbor" (README): it splices its own chain far enough that both
endpoints touch a given neighbor's clock events.

### Routing the response back

`process_chain_response`:

- **Not the originator** (intermediate node) → `add_local_lower_bound`, `add_local_upper_bound`
  (splice this node's clock events onto the ends the previous node left touching it), then
  `extend_lower_bound_for_neighbor(neighbor)`, `extend_upper_bound_for_neighbor(neighbor)` —
  `neighbor` being the node that just sent this response — then forward the updated chain
  toward `find_next_hop_neighbor(originator)`. Any failing step silently drops the response.
- **Is the originator** → `add_local_lower_bound`, `add_local_upper_bound` to attach the
  originator's own clock events at both ends, then `complete_chain_discovery`. A failing step
  aborts the discovery.

The net effect: the request walks hop-by-hop to the creator; the response walks hop-by-hop
back; at each node the chain accretes that node's local clock events; and it arrives at the
originator with the originator's own clock events at both ends.

### Completing and validating

`complete_chain_discovery` sets state `completed`, then **always** calls
`validate_chain_discovery_result` before invoking callbacks:

- `validate_chain_discovery_result` requires `lower_bound.front().creator == id_` and
  `upper_bound.back().creator == id_` — the endpoints must be the originator's clock events
  (so the bounds are in the originator's local time) — then calls `validate_event_chain`.
- `validate_event_chain` verifies, over `lower_bound · event · upper_bound`:
  - every clock event's stored hash equals a freshly recomputed hash,
  - every clock event's `signature` verifies under the Signer port for its `creator` (an empty
    signature is accepted as unsigned),
  - every element after the first embeds a reference to its **immediate predecessor**
    (the tamper-evident hash-chain property),
  - the event references the last lower-bound clock event (unless the lower bound is empty),
    and the event's own signature verifies.

The `extend_*_for_neighbor` primitives additionally `assert` their precondition (e.g.
`chain.lower_bound.front().creator == id_`) as a debug-only sanity check, compiled out under
`NDEBUG`; `validate_chain_discovery_result` is what a release build actually relies on, and it
always runs.

## Event bounds discovery

`discover_event_bounds(event, callback)` coalesces like chain discovery, then simply starts an
event chain discovery with the **Node itself** as the chain callback
(`discover_event_chain(event, *this)` — `Node` privately implements `ChainCallback`). When
that chain completes, `Node::on_chain_completed` fills the matching `EventBoundsDiscovery` with
`lower_bound = chain.lower_bound.front().timestamp` and
`upper_bound = chain.upper_bound.back().timestamp` and completes it. The bounds are thus the
timestamps of the originator's two enclosing clock events. (The paper notes this could be
made more robust by running several chain discoveries; the code runs exactly one.)

## Event order discovery

`discover_event_order(event1, event2, callback)` coalesces on the pair, then starts **two**
independent chain discoveries, one per event. `on_chain_completed` notices when both underlying
chains are complete and calls `compare_event_chains`:

```
if  chain1.upper_bound.back().timestamp < chain2.lower_bound.front().timestamp  ->  before (-1)
elif chain2.upper_bound.back().timestamp < chain1.lower_bound.front().timestamp  ->  after (+1)
else                                                                             ->  undetermined (0)
```

Because both chains are anchored in the **same originator's** clock events, the comparison is
a comparison of that node's local timestamps: if event1's whole interval lies strictly before
event2's (or vice versa) the order is definite; if the intervals overlap the order is
undetermined. A single completed chain discovery can satisfy a bounds discovery and an
order discovery at once, since all three share the same discovery-coalescing machinery.

## Discovery lifecycle and expiry

Each discovery type has `insert*` / `abort*` / `complete*` helpers that flip `state`, stamp
`end_time`, notify every registered callback, and notify the Telemetry port
(`on_*_discovery_started/aborted/completed`).

`Node::purge_discoveries` fires every `discovery_expiry / 100` (armed by
`schedule_purge_timer`, via the Scheduler port). Any discovery whose `start_time` is older than
`discovery_expiry` is removed; if it was still `in_progress` it is first aborted (which fires
the callbacks and the corresponding `*_aborted` telemetry hook). Aborting an event chain
discovery cascades: `Node::on_chain_aborted` (again, `Node`'s own `ChainCallback`
implementation) also aborts the dependent bounds and order discoveries. Expiry is the main
reason discoveries fail; the other is a chain that cannot be closed because the necessary
reverse edges (`referencing_events`) have not yet been learned from neighbor notifications —
which is why the first and last events created near the start of a run often cannot be bounded
(see [simulation.md](simulation.md)).

## Applications

- **`Publisher`** — one timer (`createEventInterval`); each firing publishes an event of
  `contentLength` random bytes through the local daemon.
- **`Browser`** — three independent timers (`discoverEventChainInterval`,
  `discoverEventBoundsInterval`, `discoverEventOrderInterval`); an interval of `0s` disables
  that timer. Each firing picks a random event — via the `NetworkConfigurator`, it selects a
  random network node and a random event from **that node's** daemon (an out-of-band "oracle"
  standing in for the web servers of a real deployment) — and requests the corresponding
  discovery. Results are logged (`EV_INFO`); `Browser` implements all three discovery callback
  interfaces (`ChainCallback`, `BoundsCallback`, `OrderCallback`).

## Statistics

Signals are declared on the `Daemon` in [`Daemon.ned`](../src/app/sim/Daemon.ned) and aggregated
at the network level in [`sim/Network.ned`](../sim/Network.ned):

- **Counts** — clock/event creation counts and started/aborted/completed counts for each of
  the three discovery types.
- **Vectors** — `clockEventsFileLength` / `eventsFileLength` accumulate `clockEventCreated` /
  `eventCreated` (each carrying the object's byte size) over time (per-node storage growth).
- **Vectors + histograms** — chain discovery time / length / interval, bounds discovery time
  / interval, order discovery time / order.

Every value is computed by the core and handed to the runtime as a ready-made scalar through the
Telemetry port — there is no `cResultFilter` turning message objects into statistics. The
simulation adapter, [`SimTelemetry`](../src/adapters/sim/telemetry.hpp), registers one OMNeT++
signal per hook and emits: `hash::clock_event_size_bytes` / `hash::event_size_bytes` on the
"created" signals; `end_time − start_time` for each discovery type's time signal;
`|lower_bound| + |upper_bound| + 1` for chain length; `upper_bound − lower_bound` (in raw ticks,
converted with `SimTime::fromRaw`) for chain/bounds interval; and the numeric `Order` value for
the order-discovery result. INET additionally records its own UDP/packet statistics.
