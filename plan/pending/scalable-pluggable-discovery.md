# Scalable & pluggable discovery routing (time-dependent forwarding)

## Goal

Generalize discovery forwarding from **one static, time-agnostic next hop** into a
**time-dependent, pluggable, bounded** forwarding decision, so a discovery routes over the
overlay *as it was during the target event's time range* — not the current topology — and
**scales** by routing directionally when the information exists, degrading gracefully to a
bounded undirected walk when it does not. All of this stays under the system's existing
"soundness is free" guarantee: every returned chain is re-validated by `validate_event_chain`
([doc/dynamic-discovery.md](../../doc/dynamic-discovery.md)), so the router may be as heuristic,
lossy, and probabilistic as we like without ever producing a *wrong* answer — only a missing or
looser one.

This is the concrete **routing substrate** underneath the probabilistic beam search described in
[doc/dynamic-discovery.md](../../doc/dynamic-discovery.md) (its `nextHopsToward(dest, b)` and the
`f = g + h` scoring). The in-packet distributed beam, dedup, and fan-in reconciliation are a
later layer that sits *on top* of this substrate.

---

## The seam being changed

Today every forwarding decision goes through a single function,
`find_next_hop_neighbor(dest)` ([src/core/node.cpp:568](../../src/core/node.cpp)): it looks up
`destination_to_next_hop_[dest]` — one `NodeId`, from a **static** table built once by
`NetworkConfigurator` (unweighted single-shortest-path) — and resolves it in `neighbors_`. It is
called at exactly three sites:

1. originator starting a chain → route request toward `event.creator`
   ([src/core/node.cpp:231](../../src/core/node.cpp))
2. intermediate forwarding a request → toward `event.creator`
   ([src/core/node.cpp:160](../../src/core/node.cpp))
3. intermediate forwarding a response → toward `originator` (the reference node `R`)
   ([src/core/node.cpp:188](../../src/core/node.cpp))

The chain can only *close* along edges that were **cross-linked at the event's time**: the
`extend_*_for_neighbor` primitives walk off the end of the local chain and the branch dies
otherwise ([doc/implementation.md](../../doc/implementation.md)). A current-shortest-path next
hop through a node that was not cross-linked back then is a guaranteed dead hop. **Therefore
forwarding must be time-dependent.**

---

## Decisions (locked)

1. **Route over the historical overlay — old neighbors.** A discovery hop follows the overlay as
   it was in the target time range. Whether the hand-off target is a *current* neighbor is
   irrelevant; only whether it was cross-linked with us *then*. Old neighbor entries (id +
   transport address) are **retained** so delivery still works after the peering relationship
   ends. This keeps the design to a **single historical overlay** — there is no second
   "reach a former neighbor over the current network" routing problem.

2. **The time range is a required user input.** A discovery is
   `discover(destination_id, event_hash, [t_lo, t_hi])`. The network **cannot** estimate the
   time range — the querying party supplies it, alongside the destination node id and the event
   hash (the same out-of-band knowledge that already provides the destination and hash: "I have
   the document, I know which node holds the event, I tell an interested third party, who starts
   the discovery for a proof"). No coarse-discovery bootstrap.

3. **Two distinct time-dependent tables** (the correction that shaped this plan). The DAG's
   cross-links are strictly **1-hop** — they record who your neighbors *were*, never a path to a
   node many hops away — so a single "routing table" cannot come from the DAG. Split it:

   | Table | Keyed by | Answers | Source |
   | --- | --- | --- | --- |
   | **Neighbor history** | `time` | *Who could I hand off to then?* (1-hop candidate set + address) | The DAG cross-links already carry this (`referenced_events` / `referencing_events`); store it as a compact index over the DAG, or fill it explicitly. Local, cheap, **always available**. |
   | **Routing table** | `(destination, time)` | *Which candidate is on a path toward a far destination?* (ordered next hop(s), shortest first) | **Not derivable from the DAG** — multi-hop direction is a global property. Filled externally (config today; a future overlay routing protocol later). **Either filled or not.** |

   The neighbor history alone yields only an **undirected** walk. The routing table is what turns
   it into a **directed, scalable** one.

4. **The forwarding decision** at each hop, given `(dest, time_range)`:
   - **Routing-table hit** → its ordered next hop(s); forward shortest-first (beam branching `b`).
   - **Miss** → fall back to the neighbor history: **all** past neighbors in the time range, or a
     **probabilistic subset** (width `k`, temperature `τ`).
   - Next hops are always **within** the neighbor history at that time (you can only hand off to a
     node you were cross-linked with) — the routing table is time-scoped so its entries are past
     neighbors; intersect to be safe.

5. **Soundness stays free.** Routing is best-effort. `validate_event_chain` guards correctness, so
   a wrong turn, a dropped branch, or a random sample can only cost completeness/tightness, never
   correctness.

6. **The response leg always retraces a reverse-path breadcrumb.** The request records its path as
   it floods toward the creator; the creator reflects each copy back along *that copy's* recorded
   reverse path. The response **never floods and never consults a routing table** — flooding the fat
   `EventChain` back would be unaffordable (a storm of large packets), and no routing table toward
   the originator exists (routing protocols come later). This is what makes the request-leg flood
   actually *complete*. Enabled by Decision 1: the recorded path is historical (old neighbors), so
   every retraced hop is guaranteed cross-linked. **Mandatory, not an optimization.**

---

## The pluggable router

Replace the three `find_next_hop_neighbor` call sites with a core strategy object:

```cpp
struct RouteContext { /* originator, creator, leg (request|response), g-so-far, hop_limit, visited, path (breadcrumb) */ };

class DiscoveryRouter {
public:
  // Ordered/weighted candidate next hops toward `destination`, for the given time range.
  virtual std::vector<domain::NodeId>
  next_hops(domain::NodeId destination,
            domain::TimeRange range,
            const RouteContext& ctx) const = 0;
};
```

Strategies, composed as a decorator stack (config chooses the stack):

- **`StaticShortestPathRouter`** — today's exact behavior: the single `destination_to_next_hop_`
  hop, time ignored. The **width-1 sanity anchor** and the `k=1, τ→0` degenerate case; keeps the
  existing sim/tests green through Part 1.
- **`NeighborHistoryRouter`** — candidates = past neighbors within `range` (undirected). The
  correct, always-available fallback (replaces "all *current* neighbors", which is wrong — a
  current neighbor with no cross-link at `T` is a dead hop).
- **`RoutingTableRouter`** — `(dest, time) → next hop(s)`; directed; delegates to
  `NeighborHistoryRouter` on a miss.
- **`ProbabilisticRouter`** — caps the candidate set to width `k` and samples `∝ exp(−f/τ)`; the
  scalability lid. (`f = g + h` from [dynamic-discovery.md](../../doc/dynamic-discovery.md): `g` =
  accumulated cross-link walk distance, carried in the packet; `h` = routing-table hops to the
  destination.)

The router is **pure core logic over local tables** — not a port (it has no environmental
effect). The *table providers*, however, are fed **through the existing API**, consistent with
the hexagonal design ([doc/architecture.md](../../doc/architecture.md), "neighbor/routing state is
core state fed through `add_neighbor` / `learn_route`"):

- `add_neighbor(id, address)` also **stamps the neighbor history** at the current time and keeps
  the address after the neighbor goes away.
- `learn_route(dest, next_hop)` gains a **time bucket** → fills the time-dependent routing table.

So `NetworkConfigurator` (sim) and the `peer` commands (prod) fill the historical tables with no
new port and minimal surface change.

### Flooding is a parameterization, not a separate mode

With the routing table **empty** — the expected state until routing protocols land (Decision 2,
"routing protocols come later") — every hop misses `RoutingTableRouter` and falls through to
`NeighborHistoryRouter`, whose candidate set is *all past neighbors in the time range*: a full
flood. `ProbabilisticRouter` then decides how much of that flood is actually emitted, so the stack
slides continuously along **one axis**:

| Parameters | Behavior |
| --- | --- |
| `k = degree`, `τ → ∞`, hop-limit large | **full flood** — every past neighbor, every hop |
| `k < degree` and/or finite `τ` | **probabilistically reduced flood** — a bounded random subset |
| `k = 1`, `τ → 0` | **single deterministic path** — today's width-1 walk |

Controlled by `k` (fan-out cap), `b` (branching), `τ` (temperature: uniform vs. greedy), the hop
**limit** (flood depth), the **visited-set** (makes the flood terminate on cyclic graphs), and
`time_range` (bounds the candidate neighbors). Two endpoints of the axis are guarantees:

- `k = 1, τ → 0` recovers the width-1 walk exactly — the sanity anchor.
- `k = degree` (full flood) is the guarantee that **a discovery completes with no routing table at
  all.** The request floods toward the creator; the response retraces the recorded **reverse-path
  breadcrumb** (mandatory, Decision 6 — see below), so it needs neither a routing table nor a flood
  on the return leg. The originator's existing coalesce + first-valid/tightest-wins absorbs the
  redundant copies.

**Consequence for sequencing:** because routing protocols come later, the bounded flood is v1's
**primary** path, not a fallback. So `NeighborHistoryRouter` (Part 3) and the flood controls —
visited-set / hop-limit / width (Part 5) — are the load-bearing near-term work; `RoutingTableRouter`
(Part 4) is a later request-leg optimization that only *prunes* the flood.

### Response leg: reverse-path breadcrumb (mandatory)

The response leg **always** retraces a reverse-path breadcrumb — it never floods and never consults
a routing table (Decision 6). Flooding the fat `EventChain` back would be unaffordable (a storm of
large packets), and there is no routing table toward the originator to fall back on (routing
protocols come later). Instead the request **accumulates its path** as it floods toward the creator,
and the creator reflects each copy back along *that copy's* recorded reverse path. This is what lets
the request-leg flood actually *complete*.

It also aligns with chain accretion: retracing means each node does
`extend_*_for_neighbor(the neighbor it forwarded the request to)`, exactly the cross-linked edge the
chain needs; and because the recorded path is historical (old neighbors, Decision 1), every hop is
guaranteed walkable. Multiple flooded copies reach the creator by different paths, so multiple
responses retrace home by different reverse paths — `dynamic-discovery.md`'s reference-coordinated
fan-out (realization A) made concrete — and the originator keeps the first / tightest. The breadcrumb
is a `NodeId` list **bounded by the hop limit**, so it adds bounded packet overhead.

---

## Scalability

- **Neighbor history** is `O(neighbors × time_buckets)` — small, and inherits the hard bound
  already provided by the multi-resolution ring prune
  ([plan/done/multi-resolution-clock-chains.md](../done/multi-resolution-clock-chains.md), done).
- **Routing table** is the pressure point: `O(destinations × time_buckets)`. Bound it by
  **coarse time buckets** (the multi-resolution epoch idea — fine for recent, coarse for old),
  **sparse fill** (only what a protocol actually learns), and the **fallback** (undirected walk
  over the neighbor history) covering every gap. The table is **never required to be complete**.
- **Fan-out lid:** width `k`, branching `b`, a hop **limit** (max hops a copy travels — bounds
  flood *depth*), and a **visited-node set** in the packet (dedup / loop avoidance).

**Two independent bounds — don't conflate them.** The **hop limit** (new, per-packet, hop count)
caps flood *reach/depth*; the existing **`discovery_expiry`** / `purge_discoveries` (originator-side,
wall-clock) caps discovery *lifetime*. A discovery is bounded by both at once, along different axes
— which is why the hop-count bound is deliberately *not* called "TTL" (that name reads as the
time-based one here).

---

## Packet / API changes

- Discovery API gains `time_range` (Decision 2).
- `ChainRequest` / `ChainResponse` ([src/core/wire/packets.hpp](../../src/core/wire/packets.hpp))
  carry the **`time_range`** (so every hop routes over the same historical overlay), a
  **visited-node set** and hop **limit** (loop/depth control), and the **reverse-path breadcrumb**
  (Decision 6 — request accumulates it, response consumes it; a `NodeId` list bounded by the hop
  limit), plus an optional **`g` accumulator**. Version the packet and update
  [doc/packet-format.md](../../doc/packet-format.md).
- Transport `NodeId → address` mapping is **retained beyond current adjacency** (Decision 1).

---

## Relationship to other work

- **Substrate for the beam.** This plan is the routing layer `dynamic-discovery.md` assumes;
  build it first. The distributed in-packet beam (realizations B/C there), dedup, and fan-in
  reconciliation are a follow-on that consumes `DiscoveryRouter::next_hops`.
- **Borrows from multi-resolution.** Coarse-time bucketing of the tables reuses that plan's
  epoch shape; the neighbor history inherits its ring bound.

---

## Implementation

Ordered; each part a commit. Do the work in a worktree, mark parts done here, move to
`plan/done/` when complete.

### Part 1 — `DiscoveryRouter` seam (no behavior change) — **DONE**
- [x] Introduce the `DiscoveryRouter` interface and `RouteContext`
  ([src/core/routing/discovery_router.hpp](../../src/core/routing/discovery_router.hpp)).
- [x] Wrap today's static table as `StaticShortestPathRouter` (holds const refs to
  `neighbors_` / `destination_to_next_hop_`; returns the one static next hop, k=1).
- [x] Route the three call sites through the router. **Design decision:** rather than editing the
  three sites, `find_next_hop_neighbor` was reimplemented to delegate to
  `router_->next_hops(dest, ctx)` and take the first candidate that is a known neighbor — the call
  sites stay textually intact and the change is provably behavior-preserving. Fan-out (Part 5) will
  change the request sites to iterate all candidates; the response site switches to breadcrumb
  retrace in Part 2. Core tests stay green (0 failures).

**Risk:** low (mechanical extraction). **Done** — `RouteContext` is an empty struct that grows in
later parts.

### Part 2 — Time-range + reverse-path plumbing — **DONE**
- [x] Added `domain::TimeRange` ([types.hpp](../../src/core/domain/types.hpp)) with an
  unconstrained `TimeRange::all()`; `discover_event_chain/bounds/order` take a **required** range
  (stored on `EventChainDiscovery`). **Decision:** callers without a real user window (tests,
  `lotid`, sim Browser) pass `TimeRange::all()` — threading a real window through the CLI control
  protocol is deferred (see Open questions).
- [x] `ChainRequest` carries `range`, `hop_limit`, and the **breadcrumb `path`**; `ChainResponse`
  carries the remaining `path`. Codec + round-trip test updated. **Decision:** the breadcrumb
  **subsumes the visited-set** — with stateless intermediates a copy's breadcrumb *is* its visited
  path (loop avoidance = "am I already in it"); cross-branch dedup needs per-node state and is a
  beam-layer feature deferred regardless. So one `path` field, not two. The `g` accumulator is
  omitted (A*/beam layer, beyond these parts). **`hop_limit`** lives on `NodeConfig`
  (`discovery_hop_limit`, default 0 = unlimited → behavior-preserving; Part 5 sets a cap).
- [x] Request leg **accumulates** the breadcrumb (each forwarder appends itself; loop-avoidance
  skips neighbors already in it); the creator and every response hop **retrace** it via
  `send_chain_response_retrace` (next = `path.back()`, pop) — `find_next_hop_neighbor(originator)`
  is gone from the response path (Decision 6). Verified by the existing 2- and 3-hop discovery
  tests (they exercise the multi-hop retrace and stay green).
- [ ] `doc/packet-format.md` update folded into **Part 7** (Documentation).

**Risk:** medium. **Done** — core targets build clean (`-Wall -Wextra -Wpedantic`) and green. The
sim adapter (`Daemon`/`Browser`) was updated for the new arity but is **not compiled here** (no
OMNeT++/INET in this environment) — unverified until an OMNeT++ build (Part 6).

### Part 3 — Neighbor history + `NeighborHistoryRouter`
- [ ] Build the `time → {NodeId, address}` history — as an index over the DAG cross-links, or an
  explicit fill from `add_neighbor`.
- [ ] Retain old neighbor addresses (Decision 1).
- [ ] `NeighborHistoryRouter`: undirected candidate set over a `time_range` (unions the buckets it
  spans). This is the always-available fallback.

**Risk:** medium (the historical-adjacency source; keep it bounded).

### Part 4 — Time-dependent routing table + `RoutingTableRouter`
- [ ] `(destination, time_bucket) → [next hop, shortest first]`; `learn_route` gains a time bucket.
- [ ] `RoutingTableRouter` with fallback to `NeighborHistoryRouter` on a miss; intersect next hops
  with the neighbor history so every hop is past-cross-linked.
- [ ] Coarse time buckets; **provider seam only** — filling it from an overlay routing protocol is
  **out of scope** (config/static fill is enough to exercise the directed path).

**Risk:** medium.

### Part 5 — Bounded probabilistic fan-out (the flood controls)
- [ ] `ProbabilisticRouter`: width `k`, temperature `τ`, hop limit, visited-node set (dedup).
- [ ] Expose `k` / `b` / `τ` / hop-limit as config (mirrors `dynamic-discovery.md`'s knobs). `k = degree`
  = full flood (the no-routing-table completion guarantee); `k = 1, τ → 0` = the width-1 anchor.

**Risk:** low–medium. Load-bearing for v1 (the flood is the primary path until routing lands).
The reverse-path breadcrumb that makes the request-leg flood *complete* is mandatory and lands in
Part 2, not here.

### Part 6 — Simulation & statistics (the tuning ground)
- [ ] `NetworkConfigurator` fills the historical tables (or a churn scenario builds them over
  time so the current overlay diverges from the historical one — the case this plan exists for).
- [ ] Stats: **completion rate vs. routing-table fill fraction**, fan-out width, bandwidth,
  interval tightness — sweep, ship the winners as production defaults.

**Risk:** low. Validates the design end-to-end.

### Part 7 — Documentation
- [ ] [doc/dynamic-discovery.md](../../doc/dynamic-discovery.md): this substrate under the beam.
- [ ] [doc/implementation.md](../../doc/implementation.md): time-dependent forwarding, the two
  tables, old-neighbor routing.
- [ ] [doc/packet-format.md](../../doc/packet-format.md): the `time_range` field.
- [ ] [doc/architecture.md](../../doc/architecture.md): the `DiscoveryRouter` seam and table
  providers.

---

## Open questions / risks

- **Directed scalability depends on *something* filling the routing table.** Out of scope here; the
  plan only guarantees graceful degradation (undirected bounded walk) without it. The undirected
  fallback must stay cheap enough to be viable on its own on realistic topologies — validate in
  Part 6.
- **Delivery to a former neighbor** relies on the retained address. If that node moved / its
  address changed, delivery fails → the branch dies → graceful (soundness intact). Accepted per
  Decision 1.
- **Time-bucket resolution vs. table size** is the core scalability knob; tune in simulation.
- **Table ↔ history consistency** — routing-table next hops must be past-cross-linked; enforced by
  intersection (Part 4), but a stale/over-broad table just costs dead hops, never correctness.

---

## Status

Pending design plan. Decisions above are locked from the design discussion; implementation not
started.
