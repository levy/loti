# Discovery on a Dynamic Network — The Beam Search

Discovery is the one operation that must succeed against a network that never holds still.
Nodes join and leave, routes change, and — most subtly — the cross-links that make a chain
*walkable* only form gradually as clock-event notifications propagate. This document explains
what "dynamic" means for LOTI, why it threatens discovery, and how a **probabilistic beam
search** (the paper's phrase) turns a fragile single-path walk into a robust, tight, redundant
search.

It builds on the width-1 mechanism already documented in
[implementation.md](implementation.md#event-chain-discovery--the-core-algorithm): the four
chain-building primitives (`addLocalLowerBound`, `addLocalUpperBound`,
`extendLowerBoundForNeighbor`, `extendUpperBoundForNeighbor`), request/response routing, and
`validateEventChain`. Read that first; here we generalize it.

> **The routing substrate below is now implemented.** Discovery forwarding is a pluggable
> `routing::DiscoveryRouter`
> ([`routing/discovery_router.hpp`](../src/core/routing/discovery_router.hpp);
> [plan/pending/scalable-pluggable-discovery.md](../plan/pending/scalable-pluggable-discovery.md)):
> a single static next hop (`StaticShortestPathRouter`, still the default), or a bounded flood
> over the neighbors cross-linked within the discovery's required `domain::TimeRange` window
> (`NeighborHistoryRouter`), optionally narrowed by a time-scoped `RoutingTableRouter` and capped
> in width by a `ProbabilisticRouter`. The response leg always retraces the request's recorded
> reverse-path breadcrumb rather than routing, so a flood completes even with an empty routing
> table (see [implementation.md § Forwarding & routing policy](implementation.md#forwarding--routing-policy)).
> What is **not** built yet is the score-weighted layer this document describes: candidates are
> chosen uninformed (uniform sampling under the width cap), not ranked by `f = g + h` and pruned
> to a beam, and the beam is not carried in the packet. The routing substrate is the *inner loop*
> the beam search below still needs to be built on top of — see
> [What changes in the code](#what-changes-in-the-code).

## The one invariant that makes everything else easy

**Soundness is not the search's job.** Every candidate chain a discovery returns is run through
`validateEventChainDiscoveryResult` — hashes recomputed, linkage checked, endpoints confirmed
to be the reference node's own clock events. A chain that does not validate is discarded, full
stop. Therefore:

> The search can be **as heuristic, probabilistic, lossy, and best-effort as we like**. It can
> take wrong turns, drop branches, sample randomly, and race timeouts. None of that can produce
> a *wrong* answer — only a *missing* one or a *looser* one.

The search affects **completeness** (did we find a chain at all?) and **quality** (how tight is
the interval?), never **correctness**. This is what licenses an aggressive beam search: we are
optimizing found-rate and tightness under a hard soundness guarantee we get for free. It is also
why the system stays **consistent** (the paper's requirement): two nodes may find *different*
chains, but they cannot find *contradictory* ones — a chain that reversed a true order would be
a hash cycle, which is infeasible ("it would break the hash by forming a full circle").

## What is dynamic, and how it hurts discovery

| Dynamic phenomenon | Effect on a discovery |
| --- | --- |
| **Lazy cross-link formation.** A `LocalClockEvent`'s reverse links (`referencingEvents`) appear only when a neighbor's notification arrives; forward links only when a neighbor's tip was known at clock-event creation. | Around the event's time a node may have **no cross-link** to the neighbor it needs. The extend primitive then walks far to find a stale one (**loose bounds**) or hits the end of its chain and **fails**. This is exactly why the first/last events of a run so often can't be bounded (~17% aborts in the [example](simulation.md)). |
| **Node churn.** A node on the path is offline or slow. | The single request/response path stalls; the discovery **expires and aborts**. |
| **Topology / route change mid-discovery.** `destinationToNextHop` is different when the response returns than when the request left. | A remembered path would break; routing-by-destination re-routes, but the new path may have worse cross-links. |
| **Partition.** Reference node and creator are in different components. | No chain exists; the discovery must **fail gracefully** (expire → undetermined), not hang. |
| **Latency variance.** Each hop is a round trip. | Long chains (~40 events, "there and back again") accumulate delay; a single slow hop dominates completion time. |
| **The DAG only grows.** More cross-links exist later than earlier. | A discovery that fails or is loose **now** may succeed or tighten **later** — bounds have a *maturity*. |

Every one of these degrades an *uninformed* walk — whether that is the single deterministic
shortest-path walk of the static policy, or the bounded flood the routing substrate (see the note
above) can already be configured to run instead. A beam search — which scores and prunes
candidates instead of walking or flooding blindly — is the general tool that absorbs them.

## The search space, precisely

Fix a **reference node** `R` (the discovery originator; in the product, possibly a notary named
with `--reference`) and a target **event `E`** created by node `K`. A discovery is a search for
an **enclosing chain**

```
  lowerBound · E · upperBound      (a single valid hash chain)
```

whose two endpoints are both clock events **of `R`**, so that the proven interval
`[T_R(lower), T_R(upper)]` is expressed in `R`'s clock.

Model the search as growing a **partial chain**:

- **Seed** (at `K`): `K`'s own clock events that immediately enclose `E`
  (`addLocalLowerBound` + `addLocalUpperBound`). The partial chain's *lower endpoint* and
  *upper endpoint* are clock events currently belonging to `K`.
- **Hop / expansion** (hand-off to a neighbor `M`): the node `N` currently holding an endpoint
  splices its own clock events in and **extends its local chain until that endpoint cross-links
  to `M`** (`extend*ForNeighbor`), then hands the endpoint to `M`. The endpoint now belongs to
  `M`. Lower and upper endpoints hop (possibly through the same node sequence) toward `R`.
- **Goal**: both endpoints belong to `R`. The partial chain is now a complete, validatable
  enclosing chain, and `T_R(upper) − T_R(lower)` is a candidate interval.

So a discovery is a **graph search** whose states are partial chains and whose moves are hops
toward `R` along cross-links. Under the default routing policy the code explores exactly one
state at a time along the static shortest path — **beam width 1, deterministic**. The flood
policy already explores every cross-linked neighbor at each hop, but *uninformed*: every
candidate is treated equally, none scored or pruned by tightness.

### The local looseness signal

The objective is the **tightest valid** enclosing chain. We cannot measure the final interval
until both endpoints reach `R` (intermediate clock events live in different, incomparable local
clocks). But dynamism's damage is *locally observable* at every hop:

> When `extend*ForNeighbor(M)` searches for a cross-link to `M`, **the number of local clock
> events it must walk past is the looseness that stale/missing links forced on this hop.**

Fresh links ⇒ walk ≈ 0 (tight). Stale links ⇒ long walk (loose). Missing links ⇒ the walk runs
off the end of the chain (this hop is **dead** from here). Accumulated walk distance is thus a
local, additive proxy for the spread the final interval will suffer — the exact quantity network
dynamism inflates. The beam search minimizes it.

## The beam search

A **beam** is the set of the `k` best partial chains kept alive at the current frontier;
everything else is pruned. Each round expands the beam and re-prunes.

**Branching.** From a partial chain sitting at node `N`, the children are the choices `N` has:

- **which neighbor `M` to hand off to** — the `b` best next hops toward `R` (a *k-shortest-paths*
  routing table instead of a single `destinationToNextHop`), and/or any neighbor with a usable
  cross-link near the endpoints; and
- **which cross-link point to use** — `extend*ForNeighbor` today stops at the *first* cross-link;
  a beam considers the nearest few, trading a slightly longer chain for a tighter reconnection.

Each viable child costs its measured lower+upper walk distance and one hop.

**Score.** Rank partial chains by an A*-style `f = g + h`:

```
  g(P) = α · (accumulated walk distance)          // looseness so far  (local, additive)
       + β · (chain length in clock events)        // shorter ⇒ tighter & cheaper
  h(P) = γ · (routing hops from the endpoints to R)  // admissible-ish distance-to-goal
```

`g` is carried in the packet and accumulated hop by hop; `h` is read from the local routing
table at each node. Lower `f` is better.

**Prune.** Keep the `k` lowest-`f` partial chains; drop the rest. Cap the branching factor `b`
per node and the beam width `k` so the frontier — and the packets that carry it — stay bounded.

**Probabilistic variant.** Instead of taking the top-`k` deterministically, sample survivors with
probability `∝ exp(−f/τ)` (a *stochastic beam*, temperature `τ`). Why randomize:

- **Diversity / redundancy** — survivors spread across *different* node paths, so one node's churn
  doesn't kill the whole beam (the paper's "redundant" and "fault tolerant").
- **Escape dead ends** — a greedy top-`k` can pile onto one locally-tight branch that later hits a
  missing link; sampling keeps alternatives alive.
- **Load spreading** — different discoveries take different paths, avoiding hot spots.
- **Cost control** — `τ` and `k` cap work regardless of graph size.

Width `k = 1`, `τ → 0` recovers **exactly the `StaticShortestPathRouter` default** — today's
deterministic single-path walk — as the degenerate case, which is the right sanity anchor.

## Realizing the beam across the network

The hard part — and the "quite complex" the paper hints at — is that the frontier is **not
local**: it is spread across nodes and lives inside in-flight packets. Three realizations, in
increasing sophistication:

### A. Reference-coordinated fan-out (redundant, simple)

`R` starts `b` **independent** width-1 discoveries toward `K` along its `b` best next hops (and
`K` may answer each with a different seed / cross-link choice). Each returns one candidate chain;
`R` validates all, keeps the tightest. Pruning happens only at `R`, on return.

- **+** Trivial change (loop the existing algorithm), naturally disjoint paths ⇒ strong churn
  tolerance, matches the README's "several event chain discoveries could be started."
- **−** No in-flight pruning; `b` full walks' worth of bandwidth; no per-hop tightening.

### B. In-packet distributed beam (tight, complex)

The request/response packets carry the beam itself: a small set of partial chains plus their `g`
and a **visited-node set** (to forbid cycles/rework). Each node expands, re-scores, prunes to `k`,
and forwards. Branches that reconverge are deduplicated; branches that reach `R` complete.

- **+** Early pruning ⇒ bandwidth-efficient; per-hop cross-link optimization ⇒ tightest bounds.
- **−** Genuinely complex: per-packet beams, dedup, loop avoidance, fan-in reconciliation, and a
  hard **one-datagram budget** (`k × chain-size ≤ MTU`, per the paper's 40 KB / single-UDP
  target) that bounds `k`.

### C. Hybrid (recommended)

`R` fans out `b` independent requests on **disjoint** first hops (redundancy vs. churn), and
**within each**, nodes carry a *small* local beam (a few cross-link/next-hop choices, pruned to
fit one datagram) to tighten each hop. `R` collects all completions, validates, and returns the
tightest; it may stop early on a "good enough" interval or on expiry.

This puts redundancy where churn lives (independent paths) and tightening where looseness lives
(per-hop cross-link choice), while keeping each packet within the single-datagram budget.

Whichever realization: **completion is a race** — the discovery finishes when the *first*
acceptable chain arrives (or the tightest, if `R` waits for the beam to drain before the expiry).

## How the beam absorbs each dynamic

| Challenge | Beam-search response |
| --- | --- |
| Missing cross-link on the "obvious" path | Other beam members route through neighbors that *do* have a fresh link; dead branches (walk ran off the chain) are pruned, not fatal. |
| Loose cross-link (stale link) | The looseness enters `g`; tighter branches out-score it and survive pruning. Multiple cross-link points per hop pick the nearest. |
| Node churn / a slow hop | Independent branches (fan-out) mean the discovery completes via a live path; the stalled branch simply times out and is dropped. |
| Route change mid-discovery | Each hop reads the *current* routing table for its `h` and next hops, so the beam re-routes continuously; no stale remembered path. |
| Partition | No branch can reach `R`; the beam drains, the discovery **expires and aborts** cleanly → order "undetermined," never a hang or a wrong answer. |
| DAG maturity | Re-running later (or a retry with backoff) explores newly-formed links → higher completion rate and tighter bounds over time. |
| Order discovery's "undetermined" (overlapping intervals) | Tighter bounds from the beam shrink both intervals, so fewer pairs overlap ⇒ **more definite orderings** (`compareEventChains` returns 0 less often). |

## Termination, expiry, retry, maturity

- **Budget.** A discovery is bounded by `discoveryExpiryTime` (and by `k`, `b`, and a hop TTL).
  The `purgeDiscoveriesTimer` already aborts anything overdue; the beam simply explores within
  that window.
- **Early stop.** `R` may accept the first chain under a target interval width instead of waiting
  for the whole beam — a latency/tightness knob.
- **Retry with backoff.** On abort, retry later (links may have matured), optionally widening `k`
  or `b`. Expose as CLI/config policy (`discovery.retry`, `discovery.beam_width`).
- **Maturity.** Because the DAG only grows, a node can *defer* a discovery for freshly-created
  events by a grace period so the enclosing links have time to form — trading latency for a much
  higher completion rate on recent events.

## What changes in the code

The width-1 code is the beam's inner loop; generalizing is mostly additive:

| Today (routing substrate, implemented) | Beam generalization (not yet built) |
| --- | --- |
| `DiscoveryRouter::next_hops(dest, ctx)` → an unranked candidate list — one hop under the static policy, or every cross-linked neighbor under the flood policy, uniformly width-capped by `ProbabilisticRouter` | score those candidates by `f = g + h` and prune to the beam width, instead of capping uniformly (`nextHopsToward` becomes score-aware) |
| `extend*ForNeighbor` stops at the **first** cross-link, returns `bool` | return the nearest **few** cross-link points with their walk distances (the `g` contribution), or "dead" |
| request/response carry **one** `EventChain` plus a reverse-path breadcrumb | carry a pruned **beam** of partial chains + accumulated `g` (realization B/C) |
| originator keeps one `EventChainDiscovery`, first response wins | keep the in-flight beam; **collect multiple completions, validate each, keep the tightest** |
| `discoveryExpiryTime`, `discovery_hop_limit`, and a **uniform** fan-out cap `discovery_fanout` | + score-weighted `beam_width k` (`∝ exp(−f/τ)`), `branching b`, `temperature τ`, `retry` |

Crucially, `validateEventChainDiscoveryResult`, the hashing, and the four primitives are
**unchanged** — the beam orchestrates them; it does not alter the proof.

## The simulation is the tuning ground

This is where the dual-target design ([architecture.md](architecture.md)) pays off directly. The
beam's parameters — `k`, `b`, `τ`, expiry, retry, maturity grace — trade **completion rate**
against **tightness**, **latency**, and **bandwidth**, and the right settings depend on churn
rate, topology depth, and clock-event interval. Those are precisely the quantities the OMNeT++
model varies, and the statistics it already records
([simulation.md](simulation.md#what-statistics-does-the-simulation-collect)) are exactly the
objective:

- started/aborted/**completed** counts → completion rate vs. `k`, `b`, churn;
- event-bounds **interval** histogram → tightness vs. `k` and cross-link choice;
- discovery **time** histogram → latency vs. beam width and early-stop;
- UDP **packet length / sent count** → the one-datagram budget and bandwidth cost of the beam;
- event-order **result** balance → the drop in "undetermined" as bounds tighten.

Sweep the parameters on large, churning networks in simulation; ship the winners as the
production defaults in `config.toml`. Because the same core runs both
([architecture.md](architecture.md)), a beam tuned in simulation is the beam that runs in
`lotid`.

## Open questions

- **Admissible `h`.** Is routing-hop distance a safe heuristic, or can it overshoot and prune the
  truly-tightest chain? A conservative (never-overestimate) `h` keeps beam search from discarding
  the optimum; quantify the gap.
- **Joint vs. independent bounds.** Today the lower and upper bounds ride one node path with
  different cross-link points. Searching them independently (as order discovery already does per
  event) could tighten each — at double the traffic. Worth it?
- **Beam width budget.** What `k` maximizes completion-rate-per-byte while staying within a single
  datagram across realistic MTUs?
- **Adaptive parameters.** Should `k`/`b`/grace adapt per-discovery to observed churn and the
  event's age, rather than being static config?
- **Multi-reference proofs.** Running the beam against several reference nodes at once (for
  stronger legal standing, per [cli.md](cli.md)) — shared search effort, or fully independent?
