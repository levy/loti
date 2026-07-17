// Signer port, production adapter — the null signer (placeholder until Stage 4).
//
// Events/clock events go out unsigned and verification accepts everything, exactly
// as in the simulation. Stage 4 replaces this with an Ed25519 keystore; because the
// signature is excluded from the hash, that swap changes no hash and invalidates no
// proof produced now.
#pragma once

#include "ports/signer.hpp"

namespace loti::os {

struct NullSigner final : ports::Signer {
  [[nodiscard]] domain::Signature sign(const domain::Bytes&) override { return {}; }
  [[nodiscard]] bool verify(const domain::Bytes&, const domain::Signature&,
                            domain::NodeId) const override {
    return true;
  }
};

}  // namespace loti::os
