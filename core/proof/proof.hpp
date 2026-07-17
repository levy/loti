// loti-core — the portable proof: a self-contained, offline-verifiable artifact.
//
// A proof is an event chain (`lowerBound · event · upperBound`) enclosing an event
// between two clock events of a *reference* node, packaged with that reference's
// identity so a third party can verify it WITHOUT being a participant and WITHOUT a
// network (doc/cli.md, "Proof format"). Verification recomputes every hash, checks
// the chain is linked, confirms the endpoints are the reference node's, and (via a
// Signer port) verifies the embedded signatures — proving *integrity and
// attribution*. The reference node's clock is what the bounds are relative to; the
// verifier supplies the *trust* by choosing which reference nodes it accepts.
//
// The serialized form reuses the wire codec (core/wire/codec.hpp) so a proof's bytes
// are identical to what the node puts on the wire. This module is pure: it depends
// only on the domain model and the Signer PORT, never on OpenSSL — the concrete
// keystore adapter is injected by `loti verify`.
#pragma once

#include <string>

#include "domain/types.hpp"
#include "ports/signer.hpp"

namespace loti::proof {

// What the proof asserts. `bounds` and `chain` are structurally one chain; they
// differ only in how the result is read (time bounds vs the raw enclosing chain).
// `order` carries two chains plus their compared order.
enum class Kind : std::uint8_t { bounds = 0, order = 1, chain = 2 };

// The node whose local clock the bounds are expressed in. `pubkey` is its raw
// 32-byte Ed25519 public key (empty for an unsigned reference); `node` must be its
// fingerprint. Both are carried so a verifier can display and decide whom to trust.
struct Reference {
  domain::NodeId node = 0;
  domain::Bytes pubkey;
};

struct Proof {
  Kind kind = Kind::bounds;
  Reference reference;
  domain::EventChain chain;            // the (single) enclosing chain
  domain::EventChain chain2;           // order only: the second event's chain
  domain::Order order = domain::Order::undetermined;  // order only
};

constexpr std::uint64_t kMagic = 0x4C4F5449'50524F46ull;  // "LOTIPROF"
constexpr std::uint64_t kVersion = 1;

// Serialize / parse the portable binary form. deserialize() throws std::runtime_error
// on a bad magic, an unsupported version, or a truncated buffer.
[[nodiscard]] domain::Bytes serialize(const Proof&);
[[nodiscard]] Proof deserialize(const domain::Bytes&);

// The outcome of offline verification.
struct VerifyResult {
  bool valid = false;
  std::string reason;                  // why it failed (empty when valid)
  // Interpreted result (meaningful when valid):
  domain::Timestamp lower = 0;         // bounds/chain: enclosing interval
  domain::Timestamp upper = 0;
  domain::Order order = domain::Order::undetermined;  // order
};

// Verify a proof offline against `signer` (which supplies signature checking; the
// signer's own key is irrelevant — each signature embeds its signer's public key).
// Checks, in order: reference.pubkey (if present) fingerprints to reference.node;
// every clock/event hash recomputes; the chain is linked; the endpoints are the
// reference node's clock events; and every signature is valid. For `order`, both
// chains are verified and the recorded order must match the compared intervals.
[[nodiscard]] VerifyResult verify(const Proof&, const ports::Signer& signer);

}  // namespace loti::proof
