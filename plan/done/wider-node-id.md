# Widen NodeId to 128 bits (hardening plan, item 2.4)

**Status: DONE** (commits `harden: widen NodeId to 128 bits` + `sim: NodeId operator<<`). All 8
stages landed; unit suite green and `run.sh`/`fuzz_udp.sh`/`config_posture.sh` acceptance all pass
with real 128-bit ids over UDP (the notary id is now 32 hex chars). **The OMNeT++ sim needed one
edit**: the sim's `Browser` streams `event.creator` (a `NodeId`) into `EV_INFO`, which compiled
when `NodeId` was a `u64` but not as a struct — a non-template `operator<<(std::ostream&, const
NodeId&)` (hex) was added to `domain/types.hpp` to restore it. Verified by building the sim under
OMNeT++ 6.4 + INET 4.7 and running the 57-node `EventBoundsDiscovery` example (534,610 events,
reached the time limit and called finish() cleanly). Everything else in the sim (`= 0`,
`static_cast(getId())`, map keys) is covered by the implicit ctor and struct comparison.

A sub-plan split out of `hardening.md` because it is a wire / on-disk / snapshot **format
change**, not a localized fix. No backward compatibility is required (the operator confirmed):
format versions are bumped freely and old stores/proofs/packets are simply incompatible.

## Why

`NodeId` is `std::uint64_t` — the first **8 bytes** of `SHA-256(pubkey)`
([keystore.cpp](../../src/adapters/os/keystore.cpp), [proof.cpp](../../src/core/proof/proof.cpp)).
At the paper's "few billion nodes" (~2³²) target:

- **birthday collisions among honest nodes** ≈ 50% at ~2³² ids on a 64-bit space — two honest
  nodes sharing an id breaks neighbor lookup, routing, and identity;
- **targeted impersonation** is ~2⁶⁴ grinding — expensive but not out of reach.

Widening to **128 bits** gives ~2⁶⁴ birthday safety (negligible collision probability at 2³²
nodes) and 2¹²⁸ second-preimage resistance.

## Design

- **`domain::NodeId`** becomes a struct wrapping `std::array<std::uint8_t, 16>` with a defaulted
  `operator<=>`/`operator==` (so it stays a `std::map` key and stays comparable). It keeps an
  **implicit constructor from `std::uint64_t`** (big-endian into the low 8 bytes, high 8 zero) so
  `NodeId creator = 0;`, `NodeId(1)` (sim/test/small ids), and `== 0` all keep working with no
  churn. Real identities are the full 16 bytes — never a zero-extended `u64`. Add `is_zero()` and
  hex `to_hex()` / `from_hex()`.
- **Fingerprint** = first **16 bytes** of `SHA-256(pubkey)` (keystore + proof, kept in sync).
- **Wire** carries 16 raw bytes where it carried a `u64`: new `Writer::node_id` / `Reader::node_id`;
  update the datagram header `sender`, `ChainRequest.originator`/`event.creator`, the breadcrumb
  `path`, `ChainResponse.originator`, the codec's event/clock-event `creator`, and the proof's
  `reference.node`. `wire::sender_of` reads the 16-byte id at header offset 1.
- **Stores** (LMDB + sim in-memory) key neighbors/routes by the 16-byte id; LMDB `kFormatVersion`
  bumped. **Snapshot** `kSnapshotVersion` bumped; NodeId fields serialized as 16 bytes.
- **CLI**: `hex_id` prints 32 hex chars; a `parse_node_id` accepts a 32-hex id (real) or a decimal
  (small/sim convenience). `--id <n>` stays decimal; `--trust`/`peer add`/route ids accept hex.

## Stages (each builds + tests green before the next where possible)

1. **Type + helpers** — `domain/types.hpp` struct + implicit `u64` ctor + `is_zero`/hex; a
   `hash::node_id_from_digest` (first 16 bytes) helper.
2. **Wire** — `codec.hpp` `node_id` read/write; `packets.cpp` header + fields; `sender_of`.
3. **Fingerprint** — `keystore.cpp` + `proof.cpp` → 16-byte fingerprint (kept identical).
4. **Stores** — LMDB neighbor/route key encoding + `kFormatVersion` bump; sim in-memory map keys.
5. **Snapshot** — `node.cpp` `kSnapshotVersion` bump + 16-byte NodeId encoding.
6. **CLI** — `hex_id` (32 hex), `parse_node_id`, and the `strtoull` id sites in lotid/loti.
7. **Sim app** — `Daemon.cpp/.h`, `sim/transport.hpp`, `sim/in_memory_store.hpp`: getId()→NodeId.
   **Not compile-verified here** (OMNeT++ build); updated for correctness and called out.
8. **Tests** — convert `NodeId(n)` sites; widen the keystore fingerprint test; add a 128-bit
   distinctness/round-trip test.

## Verification

Core unit suite + `test/acceptance/{run,fuzz_udp,config_posture}.sh`. The OMNeT++ sim app is
edited but built only under OMNeT++ (not in CI here) — explicitly flagged in the commit.

## Decision log

- **128 bits, not 256.** 16 bytes is ample (2⁶⁴ birthday / 2¹²⁸ preimage) and half the wire/key
  size of the full digest. The pubkey (32 bytes) still travels in signatures for verification.
- **Implicit `u64` ctor kept.** Minimizes churn and is safe: it zero-extends (never truncates),
  and real ids come from the 16-byte fingerprint. It is a construction convenience for small/sim
  ids, not a claim that a NodeId is an integer.
