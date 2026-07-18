// loti-core — event-chain validation: the trust check shared by discovery and proofs.
//
// One canonical "is this enclosing chain sound?" walk: recompute every clock-event and
// event hash, confirm the chain is linked back-to-front, check both endpoints are the
// reference node's clock events, and verify every signature through the Signer PORT. Both
// the live discovery path (Node::validate_chain_discovery_result) and offline proof
// verification (proof::verify) route through this, so the security-critical check lives in
// exactly one place. Pure: depends only on the domain model, the hasher, and the Signer
// port — never on OpenSSL (the concrete keystore is injected by the caller).
#pragma once

#include <string>

#include "domain/types.hpp"
#include "ports/signer.hpp"

namespace loti::validate {

// The outcome of validating one enclosing chain. `lower`/`upper` are the interval the
// chain proves — the reference node's clock timestamps at the endpoints — and are
// meaningful only when `ok`.
struct ChainResult {
  bool ok = false;
  std::string reason;          // why it failed (empty when ok)
  domain::Timestamp lower = 0;
  domain::Timestamp upper = 0;
};

// Validate that `chain` soundly encloses its event between two clock events of the
// `reference` node: recompute every hash, check the chain is linked, confirm both
// endpoints are the reference's, and verify every signature via `signer` (whose own key is
// irrelevant — each signature embeds its signer's public key).
[[nodiscard]] ChainResult verify_chain(const domain::EventChain& chain,
                                       domain::NodeId reference,
                                       const ports::Signer& signer);

}  // namespace loti::validate
