// Signer port, simulation adapter — the null signer.
//
// The simulation does not model identity: events/clock events go unsigned (empty
// signature) and verification always accepts, exactly matching pre-Stage-4
// behavior. Because the signature is excluded from the hash, this changes no hash
// and no statistic. Production swaps in a real Ed25519 signer (Stage 4).
#pragma once

#include "ports/signer.hpp"

namespace loti::sim {

struct NullSigner final : ports::Signer {
  [[nodiscard]] domain::Signature sign(const domain::Bytes&) override { return {}; }
  [[nodiscard]] bool verify(const domain::Bytes&, const domain::Signature&,
                            domain::NodeId) const override {
    return true;
  }
};

}  // namespace loti::sim
