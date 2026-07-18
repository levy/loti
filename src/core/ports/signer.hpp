// Signer port — signs event/clock-event hashes and verifies signatures.
//
// Wired but inert until Stage 4: the simulation uses a NullSigner (empty
// signature, verify() always true), production an Ed25519 keystore. Because the
// signature is excluded from the hash (see core/domain/types.hpp), swapping in a
// real signer changes no hash and no proof already produced.
#pragma once

#include "domain/types.hpp"

namespace loti::ports {

class Signer {
 public:
  virtual ~Signer() = default;

  // Sign a message (typically an event/clock-event hash). Empty = unsigned.
  [[nodiscard]] virtual domain::Signature sign(const domain::Bytes& message) = 0;

  // Verify `signature` over `message` as produced by `signer`. An empty signature
  // is treated as "unsigned" and accepted (the current, pre-Stage-4 behavior).
  [[nodiscard]] virtual bool verify(const domain::Bytes& message,
                                    const domain::Signature& signature,
                                    domain::NodeId signer) const = 0;
};

}  // namespace loti::ports
