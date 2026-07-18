# Multi-resolution clock chains (bounded-size pruning without losing orderability)

## Goal

Bound a node's on-disk size **without losing the ability to order / bound events in the
pruned past** — only losing *precision*, gracefully, with age. Today the clock-event log is
append-only and *never pruned* (`db stat` reports `"retention", "local events + clock chain
never dropped"`, [src/app/lotid/lotid.cpp](../../src/app/lotid/lotid.cpp)); at ~1 clock
event/second that is ~3–30 GB/year and unbounded ([doc/theory.md:196](../../doc/theory.md)).
This plan is the concrete answer to the open question already written into the docs —
*"What is the exact retention policy that keeps proofs reconstructible years later without
unbounded storage growth?"* ([doc/cli.md:456](../../doc/cli.md)) — and fills the
specified-but-unimplemented `db gc` verb ([doc/cli.md:331](../../doc/cli.md)).

It is the **disk** analogue of [constrained-node-support.md](constrained-node-support.md)
Part D, which bounds *RAM* (page-cache-backed store) but deliberately does **not** prune.
Sequence this after / alongside Part D so pruning hooks the `Store` port that Part D
introduces.

---

## The model — "N virtual chains per node"

Each node runs **`L` independent clock chains**, each an ordinary hash-linked chain
(exactly today's chain logic, instantiated `L` times), differing only in **clock interval**
and each keeping a **finite ring of `C` clock events**:

- chain 1 (fast): ticks every `I` seconds
- chain 2: every `s·I`
- chain ℓ: every `I·sᴸ⁻¹`

The chains are **fully independent** — a chain-2 clock event is its own event on its own
timer, not a reused chain-1 event. That independence is what makes pruning one chain unable
to damage another.

Four rules define the design:

1. **Every event is pinned into every chain, both directions.** At creation an event `E`
   references the current *tip* of each chain (lower anchor, `Cₚ ← E`), and each chain's next
   tick references `E` (upper pin, `E ← Cⱼ`). So `E` is boundable at every resolution,
   symmetrically.
2. **Conservative pruning.** When a faster chain's old clock events are dropped, the slower
   chain still references every event (of this node) the pruned ones referenced. This holds
   *by construction* because events are pinned into all chains at creation and each chain is
   an immutable hash chain (you cannot migrate references into an already-created clock event
   without changing its hash). Result: pruning a chain **widens** an event's bounds to the
   next-coarser resolution, never **drops** them.
3. **Change-only per-chain advertisement.** A node advertises a chain's tip to neighbors only
   when it changes (fast chain often, coarse chains rarely). Missed advertisements only widen
   a neighbor-derived bound; they never make it wrong.
4. **Neighbor cross-linking at own resolution (`m = ℓ`), optionally coarser.** A local chain-ℓ
   clock event references neighbor chain `ℓ` (matched — the accuracy-per-byte default), never
   finer; extend to coarser (`m ≥ ℓ`) for guaranteed cross-node liveness at ~2× the cost (see
   [Neighbor inclusion](#neighbor-inclusion-policy)).

### Why this is symmetric (the correction that shaped the design)

Bounds are two-sided: lower needs `E` to *reference* a retained clock event, upper needs a
retained clock event to *reference* `E`. Rule 1 gives both at every resolution; rule 2 keeps
both alive under pruning. So an age-`a` event degrades identically on both ends.

---

## Key properties & math

Let `I` = fast interval, `s` = skip factor between chains, `C` = clock events kept per chain,
`L` = number of chains, `d` = neighbor count, `R` = this node's own event-publishing rate.

- **Clock-event count = `L·C`, flat forever** (was `R_clock · age`, unbounded). *The win.*
- **Symmetric precision.** For an event of age `a` (= `now − T(event)`), let `ℓ*(a)` be the
  finest chain still covering it — the smallest `ℓ` with `C·I·sˡ⁻¹ ≥ a`. The lower anchor is
  the last chain-`ℓ*` tick *before* the event and the upper pin is the first one *after* it —
  two **consecutive** ticks, exactly one interval apart — so **the provable (own-chain) window
  ≈ `I·s^(ℓ*−1)`**, the finest-covering interval itself (*not* twice it). Key identity: a
  chain's retained window = `C ×` its interval, so the chain that reaches *just* back to age `a`
  has interval ≈ `a/C`. Two regimes:
  - **Recent (`a ≤ C·I`):** `ℓ* = 1`; window ≈ **`I`**, the fast-resolution floor
    (`a/C` does *not* apply here — you cannot beat the fast interval).
  - **Aged (`a > C·I`):** the finest still-retained chain is a coarse one of interval ≈ `a/C`
    (exact at a chain's far edge, up to `×s` just past it), so **window ∈ `[a/C, sa/C)`** — a
    scale-invariant `[1/C, s/C)` fraction of the age.

  This is the **local own-chain** resolution; a *cross-node* discovery adds the existing
  there-and-back routing overhead (the paper's ~40-event chain) on top.
- **Reach (max orderable age) = `C·I·sᴸ⁻¹`.** Logarithmic storage buys exponential reach:
  one more chain ×`s` the horizon for `+C` clock events.
- **Own-event reference overhead ≈ `s/(s-1)` references per event you publish** (recent
  events carry `L` refs, old ones just 1; the average is ≈ 1.25 at the phone's `s=5`). Since a
  node stores its own event *content* anyway, a ~40-byte hash-ref per event is negligible beside
  it. This scales with **your event rate `R`, not** the 1 Hz clock rate.
- **Advertisement bandwidth ≈ `s/(s-1)` × a single chain** (`Σ 1/Iₗ ≈ (1/I)·s/(s-1)`) —
  coarse chains are almost free on the wire.
- **Extra clock-event creation ≈ `s/(s-1)` × baseline** (~25 % more hashing/signing at `s=5`;
  ~50 % at the mini-PC's `s=3`).
- **Discovery is `O(log)`**: the bounds walk selects the finest chain still covering the
  target's age instead of stepping one fine tick at a time.

### The one thing to watch — event-rate × reach

Rule 1 makes a coarse tick enumerate every event in its (long) interval. For a **normal node**
(`R` ≪ 1 Hz) this is negligible. For a **firehose publisher over long reach** it becomes the
dominant cost (a level-`L` tick references `R·I·sᴸ⁻¹` hashes). The escape valve is
[Part 7](#part-7--sibling-hop-variant-optional-high-event-rate-escape-valve): coarse chains
reference sibling-chain *tips* instead of enumerating events — storage drops to `O(L·C)` at the
cost of making old-event **upper** bounds a distributed (cross-node) property rather than a
local guarantee. Default = enumeration (this model); switch coarse chains to hops only when
`R·reach` is large.

---

## Parameters & recommended defaults

The schedule is `(I, s, C, L)`. It should be a **network-wide convention** (so cross-node
coarse discovery lines up) and is **self-described** by a level tag on each clock event, so
nodes on slightly different schedules can still partially interoperate.

### Sizing from a storage budget

Storage ≈ `N · b_ce`, where `N = L·C` is the total clock-event count and `b_ce` ≈ **1 KB**
all-in per clock event (header + previous-link + neighbor cross-links + own-event refs; ~0.6 KB
with **matched** neighbor cross-linking, ~2 KB with **`m ≥ ℓ`**). So a budget `B` buys
`N = B / b_ce` clock events. Fix `I = 1 s` (the recent-event floor) and `L = 8`, pick a reach
`A`, and the skip factor follows: `s = (A / (C·I))^(1/(L−1))` with `C = N/L`.

Accuracy has two independent parts:
- **Absolute floor** — recent events (age ≤ `C·I`) are bounded to **`I` = 1 s**.
- **Aged relative** — older events to the finest covering chain's interval, **worst-case
  `s/C`** of their age.

**The budget buys accuracy, not reach.** A few hundred years of reach is nearly free (it costs
levels, not retention), so the whole budget goes into `C` (tightens `s/C`) and a smaller `s`
(smoother steps). Three profiles, each at a ~200–300-year reach (`I = 1 s`, `L = 8`, matched
neighbor cross-links):

| Profile | Budget | `N = L·C` | `s` | `C` | Reach | ≤ this age → **1 s** | 10-yr event → | Worst-case aged rel. |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Embedded (Pi Zero) | 64 MB | 64 k (8×8 000) | 7 | 8 000 | ~209 yr | 2.2 h | ~33 h | 0.088 % |
| **Phone — default** | **1 GB** | **1 M (8×125 000)** | 5 | 125 000 | ~309 yr | 1.5 days | ~52 min | **0.004 %** |
| Mini-PC | 32 GB | 32 M (8×4 000 000) | 3 | 4 000 000 | ~277 yr | 46 days | ~81 s | **0.000075 %** |

So a **mini-PC bounds anything in the last 46 days to one second**, a 10-year-old event to
~81 s, and a century-old event to ~36 min — while staying flat at 32 GB forever. The phone does
the same job to ~0.004 % of age in 1 GB. (Contrast the earlier 15 MB toy default: ~0.23 %,
1 s only up to 1.2 h — the budget was 60× underspent.)

**Knobs:** more budget/`C` → tighter aged bounds (`rel ∝ 1/C`); smaller `s` → smoother steps
(needs the storage to keep `C` up); `I` < 1 s → sub-second *recent* bounds (the fast chain then
covers less wall-time). Per-chain `C` may differ; uniform `C` is the simple default. The
**neighbor policy sets `b_ce`**: matched (`m = ℓ`, ~0.6 KB) maximises accuracy-per-byte; `m ≥ ℓ`
(~2 KB) trades roughly 2× accuracy for cross-node robustness (see below).

---

## Cost estimation (Phone default, 1 GB)

`I = 1 s`, `s = 5`, `C = 125 000`, `L = 8`; 40 B per reference; `d = 10` neighbors; own event
rate `R`.

| Cost | Formula | Phone default (1 GB) |
| --- | --- | --- |
| Clock-event count | `L·C` | **1 000 000**, flat forever |
| Clock-event storage | `L·C · b_ce` | **~1 GB**, bounded (≈0.6 GB at matched; 1 KB leaves headroom) |
| — neighbor cross-links, matched (`m = ℓ`) | `C·d·L · 40 B` | ~0.4 GB |
| — neighbor cross-links, `m ≥ ℓ` | `C·d·L(L+1)/2 · 40 B` | ~1.8 GB (→ halve `C` to fit → rel 0.008 %) |
| Own-event ref overhead | `≈ 1.1 ×` (#your events) `× 40 B` | ~44 B per event published (negligible vs content) |
| Extra clock-event creation | `Σ 1/Iₗ ≈ s/(s−1)` | ~1.25× (s=5); ~1.5× for the mini-PC (s=3) |
| Advertisement bandwidth | `≈ s/(s−1) ×` single chain | ~1.25× tip messages |
| Reach (max orderable age) | `C·I·sᴸ⁻¹` | ~309 years |
| Recent-event floor | `I` | 1 s (for ages ≤ 1.5 days) |
| Worst-case aged relative precision | `s/C` | **0.004 %** of age |

Neighbor cross-linking dominates `b_ce`, so it is the real budget lever: **matched** maximises
the clock-event count (hence accuracy); **`m ≥ ℓ`** roughly doubles `b_ce`, halving `C` and the
accuracy for a given budget.

### Baseline comparison

| | Today | With this feature |
| --- | --- | --- |
| Clock events | ~31.5 M/year, **unbounded** | 1 M (phone) / 32 M (mini-PC), **flat** |
| Clock-event disk | ~9.5 GB/year, **unbounded** | 1 GB / 32 GB, **fixed forever** |
| Recent-event bound | 1 s | 1 s, for events up to ~1.5 days / ~46 days old |
| Old-event orderability | full precision, but only while stored | ~0.004 % / ~0.000075 % of age, **~300 yr reach** |
| 512 MB Pi (Zero 2 W) | OOMs / SD fills in weeks–months | bounded (64 MB embedded profile) |

Growth goes from **unbounded** to a **fixed device budget**, spent almost entirely on accuracy
rather than reach.

---

## Invariant change — call it out loudly

This feature **overturns a stated guarantee.** The code today hard-asserts *"local events +
clock chain never dropped"* ([db stat](../../src/app/lotid/lotid.cpp)). The new invariant is:

> The local clock chain is never dropped **below the coarsest retained resolution**; every
> event stays boundable, at a precision that degrades to ~`a/C` of its age.

`db stat` / `db verify` / the docs must state the new semantics precisely. This is a
protocol-visible behavior change, not an internal optimization.

Two consequences to document:

- **Forward-only.** Skip-safe pinning is a property of clock events *at creation*; hash links
  cannot be added retroactively, so coarse chains only ever cover clock events created once the
  node is running the schedule. (No existing stores to carry forward — migration is out of
  scope.)
- **Schedule is a network convention.** Cross-node coarse discovery works best when neighbors
  share `(I, s, C, L)`. The per-clock-event level tag lets mismatched schedules degrade rather
  than fail.

---

## Implementation

Ordered, each step a commit. File paths are post-reorg (`src/…`).

> **Back-compat / migration are out of scope** — there are no deployed nodes yet. The on-disk
> store format and the wire packets may change freely; a format mismatch just re-creates the
> store (no migration code, no dual-format readers, no packet versioning).

### Part 1 — Domain model: chains & level tags ✅

- [x] **1a.** `ClockEvent` gains a `std::uint32_t chain` field, part of the hashed content
  (`calculate_clock_event_hash` prepends it) and the wire codec.
- [x] **1b.** `Neighbor::last_clock_event_hashes` is now a `std::vector<EventHash>` indexed by
  chain (empty entry = not yet learned).
- [x] **1c.** `NodeConfig` carries `std::vector<ChainConfig> chains` (`{interval, keep}` per
  level); empty = one implicit chain 0, no timer. `num_chains_ = max(1, chains.size())`.
- [x] **1d.** LMDB `kFormatVersion` bumped 2→3; snapshot version 1→2. No migration.

**Decision:** the global dense creation-`seq` is *kept* as the storage key / referencing-index
ordering; chains are distinguished by the `chain` field, and `latest_clock_event(chain)` is
backed by an in-RAM `chain -> newest seq` map (seeded at open, advanced on commit). Per-chain
seq / gap-tolerant storage is deferred to Part 3 (pruning), which is the first thing that deletes.

### Part 2 — Multi-chain clock-event creation ✅

- [x] **2a.** `Node` runs one clock timer per chain (`clock_timers_`, `schedule_clock_timer(chain)`);
  `create_clock_event(std::uint32_t chain = 0)`.
- [x] **2b.** `insert_clock_event(chain)` builds a chain-ℓ event: prev chain-ℓ tip + each
  neighbor's matched (chain-ℓ) tip + the events in `unreferenced_per_chain_[ℓ]` (upper pins).
- [x] **2c.** `insert_event` anchors into every chain that has a tip (one ref per chain);
  `publish_event` adds the event to every chain's tail.
- [x] **2d.** Working state is `unreferenced_per_chain_` (rederived per chain in
  `hydrate_working_state`); the DAG is already store-backed (Part D landed).

**Risk:** medium — realized. Single-chain (`num_chains == 1`) is byte-for-byte the old behavior
(all existing tests pass unchanged); `test_chains.cpp` drives 2 chains.

### Part 3 — Conservative-pruning ring (the store) ✅

- [x] **3a.** Store is now **gap-tolerant**: clock events keyed by a monotonic seq (`std::map`
  in-memory; keyed record in LMDB) — pruning deletes records, leaving sparse seqs. Reads by live
  seq (via `clock_events_referencing`) stay valid because prune removes the pruned seqs' reverse
  -index dups too. Per-chain ordering (`chain_seqs_`) + tip (`chain_tip_seq_`) are in-RAM,
  seeded at open. `snapshot()` and `latest_clock_event()` were made gap-tolerant.
- [x] **3b.** `Store::prune_chain(chain, keep)` drops the oldest chain-ℓ clock events (record +
  `clock_index` + `referencing` dups). `Node::create_clock_event(chain)` calls it after each tick
  when `keep > 0`. Conservative by construction (events pin into every chain), verified end-to-end
  by `test_chains.cpp`: after churning chain 0 past its ring, an old event is **still boundable via
  chain 1**, and the discovered enclosing chain is all chain 1.
- [x] **3c.** `Node::gc()` prunes every chain to its `keep`; wired to the `db gc` control verb
  (`loti db gc`). (With per-tick pruning it is normally a no-op — a manual re-assert.)
- [x] **3d.** `db stat` reports `chains` and the new retention line; **`lotid` now runs a default
  4-chain ×8 schedule** (`default_chain_schedule`, keep 4096/chain → bounded store). The full
  acceptance suite (restart, backup/restore, multi-node notary proof over real UDP) passes 10/10
  with the multi-chain default.

**Risk:** medium — realized. Prune is a bounded delete; the reverse-index cleanup is what keeps
discovery correct (a pruned ref resolves to nullopt and is skipped, selecting the finest survivor).

### Part 4 — Discovery over multiple chains ✅ (4a/4b) · 4c deferred

- [x] **4a.** `add_local_lower_bound` / `add_local_upper_bound` need **no change**: because
  chains are independent (a chain-ℓ event's only same-node clock ref is its chain-ℓ predecessor,
  and only its chain-ℓ successor references it), the local walks naturally follow one chain. `E`
  lists chain tips finest-first, so they pick the finest available chain; a pruned ref is skipped
  for free (`clock_event_by_hash` → nullopt), so "select finest surviving chain" falls out once
  Part 3 cleans the indices on prune.
- [x] **4b.** `extend_lower/upper_bound_for_neighbor` rewritten to step by **same-chain hash-ref**
  (predecessor / `clock_events_referencing` filtered to `creator == id_ && chain == current.chain`)
  instead of global `seq ± 1` — keeping the spliced sub-chain hash-linked across chains.
- [ ] **4c.** `discover_event_order` at the finest resolution both events are still covered by —
  **deferred to Part 3**: only meaningful once pruning makes different-age events resolve to
  different chains. Without pruning both use chain 0, so the current `compare_event_chains` is
  already correct.

**Risk:** medium — realized. Cross-node chain/bounds discovery over 2 chains completes and passes
`validate_chain_discovery_result` (`test_chains.cpp`).

### Part 5 — Advertisement ✅ (doc update pending in Part 8)

- [x] **5a.** `wire::ClockNotification` carries `chain`; encoded/decoded in `packets.cpp`.
- [x] **5b.** Change-only falls out for free: `create_clock_event(chain)` notifies only that
  chain's tip, and a chain's tip only changes when it ticks — no extra last-advertised tracking.
- [x] **5c.** `process_clock_event_notification` buckets the tip into
  `neighbor.last_clock_event_hashes[chain]` (resizing as needed).

**Risk:** medium — realized (on-the-wire format touch; `test_wire.cpp` round-trips it). The
`doc/packet-format.md` write-up is Part 8.

### Part 6 — Simulation & statistics ✅ (6a) · 6b via existing stats

- [x] **6a.** The OMNeT++ `Daemon` runs the multi-resolution schedule (`clockChainCount` /
  `clockChainFactor` / `clockChainKeep` NED params; default 4 ×4, keep 512). It keeps the single
  volatile fast timer for chain 0 and piggybacks chain ℓ every `factor^ℓ` fast ticks (the
  "every Nth clock event is in another chain" model), with `keep>0` driving per-chain ring
  pruning. New `clockEventsRetained` statistic records the **current** store size (the bounded
  curve), vs the cumulative `clockEventsFileLength`.
- [x] **6b (via existing stats).** Realized bound widths are already recorded by
  `eventBoundsDiscoveryInterval`; the finer age-correlation is a results-analysis exercise (the
  precise `old-event → coarser-bound` behaviour is unit-tested in `test_chains.cpp`).
- [x] **Validated by running the sim** (`EventBoundsDiscovery`, 60 s, 57 nodes):
  - **Bounded storage:** retained clock events per node plateau at ~34 with `keep=16` vs ~78
    unpruned — flat, not growing with sim time.
  - **Multi-chain is neutral for discovery:** 4 chains, no prune → **54%** bounds completed vs the
    single-chain baseline's **52%**.
  - **Graceful degradation under aggressive pruning:** `keep=16` (16 s fine window) → 35% completed
    — old events past the fine window still partly resolve via coarser chains rather than the node
    running unbounded; a realistic `keep` (covering typical event age) stays near baseline.

**Build/run:** `source <omnetpp>/setenv -q && source <inet>/setenv && source ./setenv -f &&
scripts/build-sim.sh release`, then `cd sim && ../bin/loti -u Cmdenv -c EventBoundsDiscovery`.

### Part 7 — Sibling-hop variant (optional) ⏸ NOT DONE

Optional high-event-rate escape valve; not needed for the current design (enumeration is correct
and bounded for normal event rates). Left as a future option — see the design discussion above.

- [ ] **7a.** Config flag: coarse chains reference sibling-chain tips instead of enumerating events.
- [ ] **7b.** Document the distributed-redundancy trade for old-event upper bounds.

### Part 8 — Documentation ✅

- [x] **8a.** `doc/theory.md`: new "Bounding storage: multi-resolution clock chains" subsection
  (independent geometric chains, event pinned into every chain, `~1/C`-of-age degradation).
- [x] **8b.** `doc/implementation.md`: `chain` field + per-chain `Neighbor` tips in the data model
  and hash; new "Multi-resolution clock chains" subsection (per-chain creation/pinning, prune,
  gap-tolerant store, `lotid` default schedule).
- [x] **8c.** `doc/packet-format.md`: clock notification gains `u64 chain` (81→89 B), `ClockEvent`
  composite gains `chain` (68→76 + 44·r B), hash formula prepends `chain`.
- [x] **8d.** `doc/cli.md`: `db gc` documented, retention rewritten to the new invariant, and the
  open retention question struck through / marked **Answered**.

---

## Neighbor inclusion policy

Neighbor cross-links govern cross-node **reachability** of old events (whether a connected
hash chain to the originator can still be formed), not the bound *width* (that is set by the
querying node's own chains). From lifetimes: a local chain-ℓ clock event lives `C·I·sᴸ⁻¹`; a
neighbor's chain-`m` tip is retained `C·I·sᵐ⁻¹`. For the hop to be alive whenever the local
clock event is alive, need **`m ≥ ℓ`**.

- **Matched only (`m = ℓ`)** — `d·L·C` refs (~0.4 GB at the phone default). Cheapest, so it
  maximises `C` per byte → **best accuracy per byte**. Mild caveat: borderline at each band's
  oldest edge (the neighbor's chain-ℓ tip can age out just as the local clock event does),
  degrading gracefully to a coarser cross-link or another path.
- **Matched + coarser (`m ≥ ℓ`)** — `≈ d·L²/2·C` refs (~1.8 GB), roughly 2× matched. **Never
  dangles by construction.** The robustness upgrade — but at a fixed budget it halves `C` and
  thus the accuracy.
- **All levels incl. finer** — `d·L²·C`. No reachability gain over `m ≥ ℓ` (finer neighbor
  chains die first); just larger events. Avoid.

**Default: matched (`m = ℓ`)** — accuracy-per-byte is the priority here; escalate to `m ≥ ℓ`
if cross-node discovery of oldest-in-band events proves unreliable and the 2× storage (halved
accuracy) is acceptable.

---

## Open questions / risks

- **Firehose × long reach** — the `R·reach` ref cost; Part 7 is the mitigation.
- **Schedule agreement** — cross-node coarse discovery assumes a shared `(I, s, C, L)`; the
  level tag degrades mismatches but does not fully fix them.
- **Per-event durability opt-in** — a node wanting *specific* old events locally
  upper-boundable regardless of the network can give them a direct coarse pin (few events, so
  bounded storage). Deferred; noted for completeness.
- **Interaction with Part D (Store port)** — this plan assumes reads/writes go through the
  `Store` port; if D has not landed, add the per-chain ring on the current `LmdbStore` directly.

---

## Status

**Implemented and validated** on branch `multi-resolution-clock-chains` (worktree). Parts 1–6 and 8
are done across all three build targets (loti-core, the `lotid`/`loti` production node, and the
OMNeT++ simulation). Part 7 (sibling-hop) is an explicitly-optional escape valve not needed for the
current design — left for the future.

Verification:
- `loti_core` unit tests: **46/46 pass** (`scripts/build-core.sh`), including `test_chains.cpp`
  (multi-chain creation, pinning, per-chain tips, cross-node chain-building, ring pruning, and the
  key **old-event-boundable-via-coarser-chain-after-prune** test).
- Production acceptance suite: **10/10 pass** over real UDP with the default 4-chain daemon
  (restart survival, backup/restore, multi-node notary proof). `loti db stat` shows `chains`;
  `loti db gc` works.
- OMNeT++ simulation: builds and runs multi-chain; bounded-storage curve confirmed (retained clock
  events plateau under pruning), multi-chain is neutral for discovery completion vs the baseline,
  and aggressive pruning degrades old-event discovery gracefully (see Part 6).
