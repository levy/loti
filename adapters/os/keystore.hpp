// Signer port, production adapter — an Ed25519 keystore giving a node an
// attributable cryptographic identity.
//
// Design (self-contained proofs, no key registry): a signature is the 64-byte
// Ed25519 signature over the message (an event/clock-event hash) followed by the
// 32-byte public key. A node's identity — its NodeId — is a fingerprint of that
// public key. So verify() needs nothing but the datagram/proof itself: it splits
// out the embedded public key, checks fingerprint(pubkey) == the claimed creator,
// then does the Ed25519 verification. That is what lets `loti verify` (Stage 6)
// check a proof offline. The signature is excluded from the hash, so signing
// changes no hash and no prior proof.
//
// OpenSSL types are confined to the .cpp (the handle here is an opaque void*), so
// only this adapter + lotid link libcrypto; loti_core stays pure.
#pragma once

#include <string>

#include "domain/types.hpp"
#include "ports/signer.hpp"

namespace loti::os {

class Ed25519KeyStore final : public ports::Signer {
 public:
  Ed25519KeyStore();  // generates an ephemeral key
  ~Ed25519KeyStore() override;

  Ed25519KeyStore(const Ed25519KeyStore&) = delete;
  Ed25519KeyStore& operator=(const Ed25519KeyStore&) = delete;

  // Load the raw private key from `path`, or generate one and persist it there.
  void load_or_generate(const std::string& path);

  // sign = Ed25519(message) [64 bytes] || public key [32 bytes].
  [[nodiscard]] domain::Signature sign(const domain::Bytes& message) override;

  // Verify `signature` over `message` for `signer`. An empty signature is treated
  // as unsigned and accepted (matching NullSigner). Otherwise the embedded public
  // key must fingerprint to `signer` and the Ed25519 check must pass.
  [[nodiscard]] bool verify(const domain::Bytes& message, const domain::Signature& signature,
                            domain::NodeId signer) const override;

  // This node's NodeId, derived from its public key.
  [[nodiscard]] domain::NodeId node_id() const { return fingerprint(public_key_); }
  [[nodiscard]] const domain::Bytes& public_key() const { return public_key_; }

  // NodeId = first 8 bytes (big-endian) of SHA-256(public key).
  [[nodiscard]] static domain::NodeId fingerprint(const domain::Bytes& public_key);

 private:
  void adopt_generated();               // fill public_key_ from pkey_
  void set_private_key(const domain::Bytes& raw32);

  void* pkey_ = nullptr;                // EVP_PKEY* (owns the private key)
  domain::Bytes public_key_;            // raw 32-byte Ed25519 public key
};

}  // namespace loti::os
