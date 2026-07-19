# Hardening LOTI for adversarial, always-on deployment

This plan turns the findings of a full design/robustness review into an ordered, checkable
work list. The core protocol and the ports/adapters architecture are sound and are **not**
changed here — every item is localized to an adapter, a validation seam, a config default, or
the docs. Nothing touches the DAG shape, the four chain-building primitives, or the discovery
algorithm.

## Why now — the threat-model shift

The code was written and tested against a **trusted, lossless, single-operator** environment
(the in-process harness uses a lossless FIFO transport; the acceptance test runs two cooperating
nodes on localhost). The docs, however, market a **permissionless, internet-facing, multi-year,
runs-on-a-Pi** node ([README.md](../../README.md), [doc/embedded.md](../../doc/embedded.md)).
The gap between those two worlds is what this plan closes. The recurring root cause, seen across
every adapter, is one sentence:

> **Almost every error path terminates the process, and almost nothing that touches the network
> or disk is defended against a hostile or merely unlucky input.**

## Severity legend

- 🔴 **Critical** — breaks a headline guarantee (availability, proof soundness, or identity)
  and is remotely or locally exploitable today.
- 🟠 **High** — a correctness/durability bug that a multi-year deployment will hit, or a real
  deployability blocker.
- 🟡 **Medium** — hardening, operability, and scale-rot items.
- 📄 **Docs** — reconcile documentation with the code as it actually is.

Each item is a checkbox so progress is visible. Follow the repo convention: do the work in a
worktree, commit per item, tick the box and record any decision inline as you go, and move this
file to `plan/done/` once every 🔴/🟠 item is landed (the 🟡/📄 tail may be split off into a
follow-up plan if it lags).

---

## Phase 1 — Availability: the daemon must not die on bad input 🔴

The single most important phase. Today a **one-byte UDP packet from anyone** — or a Byzantine
chain response from a peer, or a mistyped `peer add` — unwinds out of the event loop and exits
the process ([lotid.cpp:889](../../src/app/lotid/lotid.cpp#L889)).

**Status: DONE** (commits `harden: malformed/dishonest datagrams…` and `harden: decoder length
caps…`). T1/T2 reproduced both crashes before the fix; `test/acceptance/fuzz_udp.sh` confirms a
running daemon now survives a 2100-datagram flood + a bad control command.

- [x] **1.1 — Isolate every reactor callback.** Wrap the timer-callback and fd-readable dispatch
  in [reactor.hpp](../../src/adapters/os/reactor.hpp#L87) (`fire_due_timers`, the `run()` dispatch
  loop) in `try { … } catch (const std::exception& e) { log; }` so one throwing callback is
  logged and dropped, never fatal. This is the structural root fix — every finding below that ends
  in "→ crash" is downgraded to "→ one dropped packet/command" once this lands.
- [x] **1.2 — Guard the UDP receive path.** In the reader lambda at
  [lotid.cpp:305-311](../../src/app/lotid/lotid.cpp#L305), wrap `on_packet_received(datagram)` in a
  per-datagram `try/catch` (drop + optional debug log). Belt-and-suspenders with 1.1; keeps a
  storm of bad packets from spamming the generic reactor logger.
- [x] **1.3 — Decode is validation, not a fatal parse.** `wire::decode`
  ([packets.cpp:80](../../src/core/wire/packets.cpp#L80)) and the `Reader` `need()` guard
  ([codec.hpp:151](../../src/core/wire/codec.hpp#L151)) throw on any malformed field. Confirm the
  only callers are inside the now-guarded paths (transport receive, `proof::deserialize`). Keep
  `decode` throwing (it is the right signal); the fix is that callers must catch.
- [x] **1.4 — Validation on the live path returns, it does not throw.** Change
  `Node::validate_chain_discovery_result` ([node.cpp:580](../../src/core/node.cpp#L580)) so a
  failed `validate::verify_chain` **aborts the discovery** (like an expiry) instead of
  `throw std::runtime_error` at [node.cpp:585](../../src/core/node.cpp#L585). A dishonest peer is
  the *expected* case in a trustless network; it must not be fatal. (The offline `proof::verify`
  path already returns a result — this makes the two symmetric.)
- [x] **1.5 — Guard the control/stdin command path.** Every command in `Lotid::dispatch` is called
  bare except `db-restore`; `peer add <bad-ip>` throws from `UdpTransport::set_peer`
  ([transport.hpp:59](../../src/adapters/os/transport.hpp#L59)) and kills the daemon. Wrap command
  dispatch so a bad command returns an `ERR` reply, never exits. Validate numeric/address args
  (`strtoull`/`atoi`/`parse_range` currently accept garbage silently) and reply `ERR` on bad input.
- [ ] **1.6 — Preserve shutdown cleanup on fatal exit.** If the outer handler at
  [lotid.cpp:889](../../src/app/lotid/lotid.cpp#L889) is ever reached, unlink the control socket and
  `lmdb_->sync()` before returning, so a crash in lazy-sync mode does not skip the final flush.
  **Deferred (low value):** with 1.1 in place the reactor loop no longer throws, so the outer
  handler is only reachable from *startup* (before the socket exists / before any data is
  committed) — run()'s normal-exit cleanup is now reliably reached. Revisit only if a new throwing
  path into `main` is introduced. See decision log.

**Acceptance:** a new test feeds `on_packet_received` a corpus of truncated/garbage/oversized
datagrams and a hostile `ChainResponse` (bad hash, bad linkage, wrong endpoint) and asserts the
node **survives and drops** each one; the acceptance script gains a "fuzz the port" step
(`nc`/`socat` a few random datagrams) and asserts the daemon is still up afterward.

---

## Phase 2 — Proof soundness & cryptographic identity 🔴

These undermine the flagship "portable, offline-verifiable proof" and "attributable identity"
guarantees directly.

**Status: partial.** 2.1 (reject empty signatures) and 2.2 (0600 key file) are DONE, along with
2.x below (an upper-bound linkage soundness bug found while fixing 2.1); 2.3 (fail-loud key + backup writes)
is done, as is 2.5 (warn on unsigned mode). 2.4 (wider NodeId) and 2.6 (key scrub) remain. Full
suite + acceptance green.

- [x] **2.1 — Reject unsigned/empty signatures in validation.** `Ed25519KeyStore::verify` returns
  `true` for an empty signature ([keystore.cpp:84](../../src/adapters/os/keystore.cpp#L84)), and
  `validate::verify_chain` trusts it ([chain.cpp:42](../../src/core/validate/chain.cpp#L42), `:62`,
  `:74`). Combined with `creator` being **excluded from the hash** and `proof::verify` **skipping
  the fingerprint check when `reference.pubkey` is empty**
  ([proof.cpp:64](../../src/core/proof/proof.cpp#L64)), an attacker can hand `loti verify` a
  **fully fabricated, all-unsigned proof** attributing any event/order to any reference node and it
  returns *valid*. Fix: in the production/verify configuration require a non-empty, correctly-signed
  signature on every clock event **and** a present `reference.pubkey` whose fingerprint matches.
  Keep `NullSigner` (empty-accepts) for the **simulation only**, selected by which signer is linked
  — never on the verify path.
  - Design note: the enclosure math is already sound for *real* events (hash-linkage pins the chain
    to real edges), so this does not change any honest proof; it closes the **fabricated-event /
    fabricated-reference** hole specifically.
- [x] **2.2 — Private key file must be `0600`.** DONE: `load_or_generate` now creates the temp key
  file via `open(O_WRONLY|O_CREAT|O_EXCL, 0600)` (no world-readable window), tightens an existing
  looser key with `chmod 0600` on load, and checks every write/rename. `loti init` now also
  tightens its `~/.loti` home dir to `0700` (even if it already existed looser) and checks `mkdir`.

- [x] **2.x — Upper-bound linkage (soundness bug found while fixing 2.1).** `validate::verify_chain`
  skipped the linkage check on the **first** upper-bound clock event (`if (!first && …)`), so an
  upper bound disconnected from the event still validated — it did not actually enclose the event.
  Every upper element (the first must reference the event) is now checked. Honest chains are
  unaffected (upper-front always references the event by construction); regression test added.
- [x] **2.3 — Fail loudly on key/backup write errors.** *DONE — key path with 2.2, backup path here:
  `FileStore::save` now checks open/write/close/rename and throws on failure, composing with the
  Phase 1.5 `dispatch_guarded` so `db-backup` returns `ERR` (verified end to end) instead of a false
  success.* `keystore.cpp`'s key write and
  `FileStore::save` ([adapters/os/store.hpp](../../src/adapters/os/store.hpp) `save`) ignore
  `ofstream`/`rename` failures and return `void`; `db-backup` then reports **success
  unconditionally**. Check every write/rename; propagate failure; make `db-backup` reply `ERR`
  when the blob was not durably written. A backup that can silently write nothing is worse than
  none.
- [ ] **2.4 — Widen `NodeId` to ≥128 bits.** The identity is the first **8 bytes** of
  SHA-256(pubkey) ([keystore.cpp:99](../../src/adapters/os/keystore.cpp#L99),
  [proof.cpp:15](../../src/core/proof/proof.cpp#L15)). At the paper's "few billion nodes" (~2³²)
  target, 64 bits gives a ~50% birthday-collision probability among honest nodes, and ~2⁶⁴ targeted
  impersonation grinding. Move to a wider id (≥16 bytes, or the full 32-byte fingerprint). This is a
  **wire/format change** (`NodeId` is a `u64` in [domain/types.hpp](../../src/core/domain/types.hpp#L21)
  and in every packet/snapshot) — bump the snapshot + on-disk + protocol versions together, or
  scope it as its own sub-plan. Decide and record the chosen width here.
- [x] **2.5 — Gate/annotate unsigned mode.** DONE: `lotid` prints a loud startup WARNING when run
  unsigned (`--id`) — no cryptographic identity, and (since 2.1) its events verify under no signed
  peer; `status` already reports `mode: signed|unsigned`. (Kept `--id` for the simulation/test path
  rather than dropping it.)
- [ ] **2.6 — Scrub key material.** Raw private-key bytes pass through plain `domain::Bytes` in
  `keystore.cpp` with no cleanse. `OPENSSL_cleanse`/`explicit_bzero` before those buffers go out
  of scope. (Lower urgency than 2.1–2.3; include here for completeness.)

**Acceptance:** a test constructs a forged all-unsigned proof and asserts `verify` now **rejects**
it (exit `6`); a test asserts the key file is `0600` after `init`; a test injects an `ofstream`
failure and asserts `db-backup` reports `ERR`.

---

## Phase 3 — Transport authentication & anti-DoS 🔴/🟠

**Status: 3.1, 3.2 (v1), 3.3 DONE.** 3.4 (safe flood defaults) and 3.5 (socket buffers / getrandom
EINTR) remain.

- [x] **3.1 — Cap length prefixes before reserving.** DONE (landed in Phase 1 as the `ensure_count`
  guard in [codec.hpp](../../src/core/wire/codec.hpp): `refs`/`node_ids`/`chain` reject a count
  larger than the remaining buffer could hold before reserving). Test T1b asserts a huge count is
  rejected without a large allocation.
- [x] **3.2 — Authenticate the gossip layer — v1 (source address).** `recvfrom` discarded the source
  ([transport.hpp:74](../../src/adapters/os/transport.hpp#L74)) and the sender is a **self-declared**
  `NodeId` ([node.cpp:166](../../src/core/node.cpp#L166)), so anyone could spoof notifications from
  any neighbor. `UdpTransport::receive` now checks the UDP source against the registered address of
  the claimed sender (via new `wire::sender_of`) and drops a mismatch. **Decision: source-address
  check is the v1** (raises the bar to spoofing the peer's specific source IP:port; not
  cryptographic, and assumes no port-rewriting NAT). **Deferred (v2):** a per-datagram MAC/signature
  — the cryptographically strong version — needs a wire field + the sender's pubkey at the receiver
  and is a larger change. See decision log.
- [x] **3.3 — Dedup and bound `referencing_events`.** `process_clock_event_notification`
  `push_back`s a reverse edge with **no dedup** ([node.cpp:189](../../src/core/node.cpp#L189)) and
  persists it, so a chatty/spoofing neighbor grows a clock event's reverse-edge list without bound
  (memory + disk amplification, and it poisons upper-bound discovery). Dedup on insert and cap the
  per-clock-event reverse-edge count.
  **DONE (dedup):** exact-duplicate reverse edges are now skipped before append. A cap on
  *distinct*-but-spoofed edges (a forger varying the hash each time) is folded into 3.2 — only
  transport authentication stops an unauthenticated sender from minting distinct fake edges.
- [ ] **3.4 — Safe flood defaults.** With `discovery_routing = flood`, `hop_limit`, `fanout`, and
  `forward_cap` all default to `0` = **unlimited** ([node.hpp:69](../../src/core/node.hpp#L69)). Give
  the flood policy non-zero safe defaults (bounded hop-limit, fan-out width, forward cap) so a single
  config flip cannot trigger a network-wide flood. The static (width-1) default stays as is.
- [ ] **3.5 — Socket robustness.** Set `SO_RCVBUF`/`SO_SNDBUF`, and count (telemetry) inbound drops;
  handle `getrandom` `EINTR` with a retry rather than the fatal throw at
  [rng.hpp:26](../../src/adapters/os/rng.hpp#L26).

**Acceptance:** a decode test asserts a huge-count packet is rejected without a large allocation
(cap enforced); a test asserts a spoofed-source notification is dropped; a test asserts duplicate
notifications do not grow `referencing_events`.

---

## Phase 4 — Clock integrity 🟠

The one guarantee LOTI sells (time bounds) rides on `CLOCK_REALTIME` with no protection, on
hardware (Pi Zero) that has no RTC.

**Status: 4.2 + 4.3 DONE.** 4.1 deferred — it needs a monotonic *scheduling* clock decoupled from
the hashed timestamp (a scheduler/clock-wiring change, not unit-testable via the core harness);
tracked in the decision log.

- [ ] **4.1 — Reactor timers on `CLOCK_MONOTONIC`.** `Reactor::now_ns` and `WallClock` both read
  `CLOCK_REALTIME` ([reactor.hpp:95](../../src/adapters/os/reactor.hpp#L95),
  [clock.hpp](../../src/adapters/os/clock.hpp)). A backward NTP step stalls every pending timer; a
  forward step fires them in a burst. Schedule timers against `CLOCK_MONOTONIC` (keep `CLOCK_REALTIME`
  only for the hashed *timestamp*). This decouples "when do I tick" from "what time is it."
- [x] **4.2 — Monotonic-floor + plausibility gate on clock-event timestamps.** Nothing enforces that
  a new clock event's timestamp is ≥ its same-chain predecessor's. Enforce a non-decreasing floor in
  `insert_clock_event` ([node.cpp:607](../../src/core/node.cpp#L607)), and drop/park clock creation
  when `clock.now()` is implausible (e.g. pre-2020 epoch on first boot before NTP sync). Record the
  policy (clamp vs skip) here.
  **DONE — policy is CLAMP:** the timestamp is clamped up to the chain tip (a frozen clock yields
  equal, still hash-ordered, timestamps). The pre-2020 implausibility gate is deferred — it needs a
  wall-clock epoch reference the pure core deliberately lacks; the monotonic floor is the
  load-bearing half.
- [x] **4.3 — Enforce `lower ≤ upper`.** Neither `validate::verify_chain` nor `compare_event_chains`
  ([node.cpp:574](../../src/core/node.cpp#L574)) checks that the proven interval is non-inverted.
  Add `lower ≤ upper` to `ChainResult` validation so a non-monotonic reference clock cannot yield a
  silently-inverted "valid" bound.

**Acceptance:** a test creates clock events across a simulated backward clock step and asserts the
stored timestamps are non-decreasing and no inverted interval validates.

---

## Phase 5 — Durability & operational correctness 🟠

- [x] **5.1 — `prune_chain` must not desync RAM from disk.** DONE: the loop is now read-only and the
  in-RAM `chain_seqs_` tracker is trimmed only AFTER the delete transaction commits (was popped
  before the delete, so an I/O error mid-loop left RAM ahead of the aborted disk state, leaking
  records and degrading the bounded-storage invariant). The I/O-fault path can't be cleanly injected
  against real LMDB, so it is verified by construction + the happy path; added the first real-storage
  prune test (T9) which the earlier sim-store-only ring-prune tests lacked.
- [x] **5.2 — Robust map growth.** DONE: `durable()` now loops grow/retry until the record fits (or
  `grow_map()` throws `LmdbStoreFull` at the 32-bit ceiling). Was a single retry — a record larger
  than one doubling threw uncaught → crash. Test: a 5 MiB single-record write into a 1 MiB map now
  succeeds (grows 1→2→4→8 MiB).
- [ ] **5.3 — Bound or prune published-event storage.** Only the clock chains are ring-pruned; the
  `events`/`event_index` sub-DBs holding user content have **no cap** — a publisher node grows disk
  without bound. Either document this as operator-driven (and surface it in `db stat`) or add an
  event-retention policy. Note this qualifies the "storage stays flat" claim (see 📄).
- [ ] **5.4 — Key pending-discovery maps by a stable token, not a reusable fd.** `lotid`'s
  `pending_bounds_`/`chain_`/`order_`/`proof_` maps are keyed by raw `int fd`; a client that
  disconnects mid-discovery leaves a stale entry, and OS fd-number reuse can misroute a later,
  unrelated client's reply. Key by a monotonic request id (or clear pending entries on
  `close_client`). Confirm the misroute is reachable before fixing; add a regression test.
- [ ] **5.5 — Index-backed event lookup.** `find_by_hash_prefix` and `event find` linear-scan every
  stored event ([lotid.cpp:713](../../src/app/lotid/lotid.cpp#L713)); cost grows with the store over
  years. Use the `clock_index_`/`event_index_` hash indices (range scan on prefix) instead of walking
  every record.
- [ ] **5.6 — fsync off the reactor thread (or document the stall).** Default `safe` sync fsyncs on
  every commit on the single reactor thread, so a storage latency spike blocks all networking and
  control. Either move fsync to a helper thread that posts completion back to the reactor, or
  document that `--store-sync-interval > 0` (lazy) is required for latency-sensitive deployments.

**Acceptance:** a test injects an I/O error mid-`prune_chain` and asserts the retention deque and
on-disk state stay consistent; a test drives a >2× single-record write and asserts it commits.

---

## Phase 6 — Reliability & scale 🟡

- [ ] **6.1 — Reliable clock-notification delivery.** Reverse edges (the upper-bound walkability) are
  learned only from single, change-only UDP notifications ([node.cpp:102](../../src/core/node.cpp#L102));
  a lost datagram loses that reverse edge permanently. Add periodic re-advertisement / anti-entropy of
  per-chain tips so a dropped notification self-heals. This is the main reason real-network discovery
  success will trail the sim's lossless number.
- [ ] **6.2 — Bound chain/datagram size; chunk or reject.** `EventChain` size is unbounded and
  `sendto` failures (incl. `EMSGSIZE`) are swallowed ([transport.hpp:64](../../src/adapters/os/transport.hpp#L64));
  a too-big chain response vanishes silently. Enforce a max chain size (reject/flag oversize), check
  `sendto`, and design an app-layer chunking path (or cap the practical discovery depth) so large
  proofs traverse the real internet rather than relying on 40 KB fragmented UDP.
- [ ] **6.3 — Peer liveness and removal.** Implement `peer rm` / `peer ping`; expire dead peers so a
  years-long node's overlay does not rot into a graveyard of unreachable addresses it keeps notifying.
  (This is also on the paper-vs-implementation "not implemented" list — coordinate.)
- [ ] **6.4 — IPv6 transport.** `set_peer` is `inet_pton(AF_INET, …)` only
  ([transport.hpp:59](../../src/adapters/os/transport.hpp#L59)); add an IPv6/dual-stack path for
  real internet (IPv6-only ISPs, CGNAT).
- [ ] **6.5 — Log timestamps + verbosity.** `LogTelemetry` emits no wall-clock time and floods
  discovery lifecycle lines unconditionally; add timestamps and gate discovery logging on verbosity.

---

## Phase 7 — Documentation reconciliation 📄

Bring the docs in line with the code as it actually is (the review found many doc↔code and
doc↔doc conflicts). No code change; do this alongside the phases that settle each fact.

**Status: 7.1, 7.3, 7.4 DONE** (commit `docs: reconcile store, retention, and socket-path claims`);
`--rpc` (part of 7.3) was already removed by a prior `web:`/`docs:` commit. **7.2** (port numbers —
minor; all ports work, and `loti init` itself prints `--port 7000`, so the 7000 quickstarts match
the code while cli.md's `4666` is the deferred config-file default) and **7.5 / 7.6** (unimplemented
-feature demos; remaining absolute-claim qualifiers) are left as low-risk follow-ups. Note the
retention part of 7.6 (flat-storage vs unpruned events) was folded into the 7.4 rewrite.
The **`index.html` landing-page** claim reconciliation is now DONE too — re-applied on top of the
concurrently-added proof-chain figure once that had landed on master.

- [x] **7.1 — One store story.** [doc/cli.md] and
  [doc/paper-vs-implementation.md](../../doc/paper-vs-implementation.md#L33) call the MVP store "a
  periodic full snapshot," but `lotid` instantiates the **incremental LMDB store**
  ([lotid.cpp:274](../../src/app/lotid/lotid.cpp#L274)). Update cli.md/paper-vs-impl to LMDB; align
  the `--store` file examples (`state.snap` vs `dag.mdb`).
- [ ] **7.2 — One port number.** Sim `666`, CLI default `4666`, quickstarts `7000` appear with no
  reconciliation. Pick a default, use it consistently, and explain the sim's fixed `666`.
- [x] **7.3 — `--control` vs `--rpc` and the default socket path.** cli.md's canonical flag list
  names `--rpc` (used nowhere) while every example uses `--control` (listed nowhere); the default
  path disagrees (`$LOTI_HOME/control.sock` vs README's `./loti.sock`). Make one authoritative.
- [x] **7.4 — Honest retention story.** The default schedule is 4 chains × ×8 × keep 4096
  ([lotid.cpp:234](../../src/app/lotid/lotid.cpp#L234)) → a **sliding ~24-day** orderability horizon,
  not "a decade-old event to about a minute, horizon of centuries" (README) and not "runs
  indefinitely" with old events still orderable. State the real default horizon, the
  chains-needed-for-longer-reach formula, that the horizon *slides* (old events age out of network
  discovery), and the "prove-and-save within the horizon; the saved proof file is eternal" rule.
  Reconcile embedded.md's "~1–2 year 32-bit lifetime" (an old unbounded-growth artifact) with the
  bounded design. Define the undefined term `a` in the precision formula.
- [ ] **7.5 — Stop demoing unimplemented features.** cli.md worked examples use `node start`,
  `--reference <node>`, and `publish --sign/--salt/--wait` that the same doc lists as deferred; and
  packet-format.md keeps a stale "fixed 61 B" chain-request size. Remove or mark clearly.
- [ ] **7.6 — Qualify absolute claims.** "mathematically prove … any digital event" vs discovery
  expiry/failure being first-class (exit code 5; ~83% completion in the sim); "storage stays flat"
  vs unbounded published-event content (5.3); the datagram/notary/"perfectly good" phrasing. Add the
  honest qualifiers this doc set already applies elsewhere.

---

## New tests to add (cross-cutting)

The current suite never feeds hostile input through `on_packet_received`, never exercises LMDB
pruning, and uses a lossless transport. Add:

- [x] **T1 — malformed-packet corpus** through `on_packet_received` → node survives (Phase 1).
- [x] **T2 — Byzantine `ChainResponse`** (bad hash/linkage/endpoint) → discovery aborts, node
  survives (Phase 1.4).
- [x] **T3 — forged all-unsigned proof** → `verify` rejects (Phase 2.1).
- [x] **T4 — key-file mode `0600`** (done) and **backup-write-failure → ERR** (done) (Phase 2.2/2.3).
- [ ] **T5 — huge-count decode** rejected without large allocation (Phase 3.1).
- [x] **T6 — spoofed-source / duplicate notification** dropped (transport source check, 3.2) /
  deduped (3.3).
- [ ] **T7 — backward clock step** → non-decreasing timestamps, no inverted interval (Phase 4).
- [ ] **T8 — lossy-transport discovery** in the harness (add a loss/reorder model to the harness
  `FakeTransport`, which is lossless today) → measure and assert self-healing after 6.1.
- [x] **T9 — LMDB prune/retention** at the real-storage layer (pruning correctness is only tested
  against the in-memory sim store today).

---

## Out of scope (explicitly deferred, tracked elsewhere)

Incentive/value-transfer layer, sybil resistance / admission control, the probabilistic beam
search, multi-reference proofs, oracles/real-world event binding, and event import/sharing — all
already on the paper-vs-implementation "not implemented" list and not required to make the node
robust. This plan is about **hardening what exists**, not extending the feature surface.

## Decision log

Record non-obvious decisions here as items land (chosen `NodeId` width; gossip-auth mechanism;
clock-implausibility policy; fsync threading vs documented-stall; whether `--id`/NullSigner stays
in the production binary).

**Phase 1 (landed):**

- **1.4 — validation returns a verdict, does not throw.** `validate_chain_discovery_result` now
  returns `bool`; `complete_chain_discovery` aborts an unsound discovery (fires `on_chain_aborted`
  + the aborted telemetry hook) instead of throwing. This makes the live discovery path symmetric
  with the offline `proof::verify` path, which already returned a verdict. No honest discovery
  changes — a sound chain still completes exactly as before.
- **1.3 / decoder cap — down payment on 3.1.** Rather than only catching the decode exception, the
  `Reader` now rejects an implausible element count *before* reserving (`ensure_count`, division to
  avoid 32-bit overflow). This forecloses the multi-GB `reserve` DoS at the codec layer; the full
  3.1 (a first-class max-count/max-datagram policy + the T5 assertion that no large allocation is
  attempted) is still open in Phase 3. T1b covers the codec-level rejection today.
- **1.6 — deferred, mooted by 1.1.** See the item above. Not implemented because the reactor loop
  no longer throws; adding a `main`-catch cleanup would be dead code today and risks double
  unlink/sync against run()'s normal-exit cleanup.
- **`on_packet_received` is robust by contract.** The core entry point itself now swallows
  malformed-datagram exceptions (returns/drops), so the protocol engine — not just the daemon
  shell — upholds "a bad datagram is a dropped packet." The reactor/reader catches are
  defense-in-depth for unexpected (non-decode) exceptions.

**Phase 2 (landed):**

- **2.1 — the empty-signature policy lives in the adapter.** The verify path's "require a real
  signature" is enforced by `Ed25519KeyStore::verify` (production/CLI), while `NullSigner` (sim)
  still accepts empty — the policy is chosen by which signer is linked, keeping the core
  signer-agnostic. No honest proof changes; the hole was fabricated/unsigned proofs only.
- **upper-bound linkage (2.x) — a real soundness bug, not just hardening.** The first upper-bound
  element's linkage to the event was unchecked; a disconnected upper bound validated. Fixed by
  requiring every upper element to reference its predecessor (the first references the event).
- **2.4 (wider NodeId) deliberately NOT bundled.** It is a wire/on-disk/snapshot format change
  (version bumps across `domain::NodeId`, packets, snapshot) and must be its own sub-plan — mixing a
  format break into a security patch is how you ship a migration bug.

**Phase 3/4 (landed):**

- **3.2 — source-address check is the v1, not a MAC.** Chosen because it is localized to the
  production transport (core unchanged, sim unaffected — the in-process transport delivers by
  NodeId and can't be spoofed) and cleanly testable with a real localhost-UDP test. It raises the
  bar from "anyone who knows a public NodeId" to "an attacker who can spoof that peer's specific
  source IP:port," which BCP38 egress filtering makes hard across the internet. Two honest caveats:
  it is **not cryptographic** (a same-subnet or on-path attacker can still spoof the source), and it
  **assumes the peer sends from its registered listening port** — a port-rewriting NAT would cause
  false drops (the operator would register the NAT'd address). The strong v2 is a per-datagram
  MAC/signature, deferred because it needs a new wire field + the sender's pubkey at the receiver.
- **3.3 — dedup, not cap.** Exact-duplicate reverse edges are skipped; a cap on distinct spoofed
  edges is left to 3.2 (transport auth), since only authentication stops an unauthenticated forger
  from minting distinct fake edges. Deduping is unconditionally correct (a duplicate carries no
  information); a blind count cap could drop legitimate edges from many neighbors.
- **4.2 — clamp policy.** A backward wall clock clamps the timestamp up to the chain tip (frozen,
  still hash-ordered) rather than skipping the tick. The pre-2020 implausibility gate is deferred —
  it needs a wall-clock epoch reference the pure core intentionally lacks.
- **4.1 — deferred.** Moving reactor *timers* to `CLOCK_MONOTONIC` requires a monotonic scheduling
  clock separate from the `CLOCK_REALTIME` value that gets hashed into timestamps — a
  scheduler/clock-wiring change, and not observable through the core's fake-port harness. Left for a
  daemon-level pass with an integration test.
