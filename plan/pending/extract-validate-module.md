# Extract chain validation into `core/validate/`

Populate the reserved `core/validate/` module with the one concern that genuinely earns its
own home: **event-chain validation**. Today the identical, security-critical "recompute hashes
+ check linkage + verify signatures" walk exists **twice** — once in `Node`
([node.cpp](../../src/core/node.cpp) `validate_event_chain` / `validate_chain_discovery_result`)
and once in `proof` ([proof.cpp](../../src/core/proof/proof.cpp) anonymous `verify_chain`) —
"deliberately kept in sync by hand" per the MVP decisions log. Two hand-synced copies of the
check that decides whether a proof/discovery is trustworthy is a correctness hazard, and **they
have already drifted** (see below). This makes one canonical validator both callers share.

Scope: **validation only.** The `dag/` and `discovery/` folders stay reserved placeholders —
splitting the engine there was judged not to earn its keep at 795 lines (separate decision).

## The drift this fixes (motivation, confirmed)

- `Node::validate_event_chain` recomputes every **clock-event** hash but **never the event's own
  hash**. `proof::verify_chain` recomputes the event hash too
  ([proof.cpp:65](../../src/core/proof/proof.cpp#L65)). So Node's discovery path currently accepts
  a chain whose `event.hash` doesn't match its content. In practice the event always carries a
  correct hash (the core computes it), so no test catches it — but it is a real gap in the
  discovery-side check, closed for free by routing Node through the complete validator.
- Error reporting differs (Node throws typed messages; proof returns a reason string). The unified
  form returns a result; each caller adapts (Node throws `reason`, proof fills `VerifyResult`).

## Design

New pure module `src/core/validate/` (namespace `loti::validate`), depending only on
`domain/`, `hash/`, and the `Signer` **port** — no OpenSSL, so it compiles into `libloti_core`
and gets swept into the sim's `libloti.so` unchanged.

```cpp
// core/validate/chain.hpp
namespace loti::validate {
struct ChainResult {
  bool ok = false;
  std::string reason;               // why it failed (empty when ok)
  domain::Timestamp lower = 0;      // enclosing interval (meaningful when ok)
  domain::Timestamp upper = 0;
};
// Recompute every hash, check the chain is linked, confirm both endpoints are the
// reference node's clock events, and verify every signature via the Signer port.
[[nodiscard]] ChainResult verify_chain(const domain::EventChain& chain,
                                       domain::NodeId reference,
                                       const ports::Signer& signer);
}
```

The body is the **complete (stricter) walk** — the current `proof::verify_chain` logic verbatim,
including the event-hash recompute and the non-empty-bounds guard. This is the superset of the two
existing behaviors, so routing Node through it makes Node's check strictly stronger (the drift fix)
while `proof` is behavior-identical.

### Call-site changes

- **`proof.cpp`** — delete the anonymous `verify_chain` + its `references` helper; `verify()` calls
  `validate::verify_chain(p.chain, p.reference.node, signer)` and maps `ChainResult` →
  `VerifyResult{reason, lower, upper}`. `fingerprint` and `compare` stay in `proof` (proof-specific;
  `compare` is ordering, not validation — out of scope).
- **`node.cpp`** — `validate_chain_discovery_result` calls
  `validate::verify_chain(discovery.chain, id_, signer_)` and `throw std::runtime_error(reason)` on
  `!ok`. This subsumes both the endpoint checks and the walk, so **`validate_event_chain` is deleted**
  (its only caller was `validate_chain_discovery_result`); remove its declaration from
  [node.hpp](../../src/core/node.hpp). `compare_event_chains` stays (ordering, not validation).

### Build wiring

- Root [CMakeLists.txt](../../CMakeLists.txt): add `src/core/validate/chain.cpp` to the `loti_core`
  source list (explicit list, not a glob).
- `test/core` links `loti_core` → no change. `build-sim.sh` uses `opp_makemake --deep` over
  `src/core/` and excludes only test/build/apps/os → the new pure `.cpp` is picked up automatically.
- Remove the now-obsolete `src/core/validate/.gitkeep` (folder has real code).

## Steps

- [ ] **1 — module:** add `src/core/validate/chain.{hpp,cpp}` with `verify_chain` (the complete
      walk); `git rm src/core/validate/.gitkeep`.
- [ ] **2 — proof:** route `proof::verify` through `validate::verify_chain`; delete the duplicate.
- [ ] **3 — node:** route `validate_chain_discovery_result` through `validate::verify_chain`; delete
      `validate_event_chain` (+ its node.hpp decl).
- [ ] **4 — build:** add the source to CMake; `scripts/build-core.sh` green (lib + lotid + loti +
      `ctest`).
- [ ] **5 — verify:** `test/acceptance/run.sh` green (real signed proofs still verify, tamper still
      rejected); confirm the sim builds + is behavior-identical (pure add, NullSigner → no-op).

## Verify

- `scripts/build-core.sh` — `loti_core` + `lotid` + `loti` build; `ctest` green (discovery + proof
  suites — the discovery test reaching `on_chain_completed` proves the rerouted validation still
  passes a valid chain; the proof tests prove tamper is still rejected with the right reason).
- `test/acceptance/run.sh` — 10/10 (restart survival, backup/restore, multi-node notary proof +
  tamper rejection over real UDP).
- Sim: `scripts/build-sim.sh` compiles (the new TU is swept into `libloti.so`); statistics
  unchanged by construction — validation is a no-op-preserving stricter check on valid data and the
  sim's `NullSigner.verify()` returns true.

## Decisions log

*(fill in during implementation)*
