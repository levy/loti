#include "proof/proof.hpp"

#include <stdexcept>

#include "hash/hashing.hpp"
#include "validate/chain.hpp"
#include "wire/codec.hpp"

namespace loti::proof {
namespace {

// NodeId = first 8 bytes (big-endian) of SHA-256(public key). Must match
// Ed25519KeyStore::fingerprint so a proof's reference identity checks out against
// the embedded public key without linking the crypto adapter into the core.
domain::NodeId fingerprint(const domain::Bytes& public_key) {
  const domain::EventHash digest = hash::sha256(public_key);
  domain::NodeId id;  // 128-bit: the first 16 bytes of SHA-256(pubkey) — matches Ed25519KeyStore
  for (std::size_t i = 0; i < 16 && i < digest.size(); ++i) id.bytes[i] = digest[i];
  return id;
}

// The order two chains' intervals imply (mirrors Node::compare_event_chains).
domain::Order compare(const domain::EventChain& a, const domain::EventChain& b) {
  if (a.upper_bound.back().timestamp < b.lower_bound.front().timestamp) return domain::Order::before;
  if (b.upper_bound.back().timestamp < a.lower_bound.front().timestamp) return domain::Order::after;
  return domain::Order::undetermined;
}

}  // namespace

domain::Bytes serialize(const Proof& p) {
  wire::Writer w;
  w.u64(kMagic);
  w.u64(kVersion);
  w.u8(static_cast<std::uint8_t>(p.kind));
  w.node_id(p.reference.node);
  w.blob(p.reference.pubkey);
  w.chain(p.chain);
  if (p.kind == Kind::order) {
    w.chain(p.chain2);
    w.u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(p.order)));
  }
  return w.bytes();
}

Proof deserialize(const domain::Bytes& bytes) {
  wire::Reader r(bytes);
  if (r.u64() != kMagic) throw std::runtime_error("proof: bad magic (not a LOTI proof)");
  if (r.u64() != kVersion) throw std::runtime_error("proof: unsupported version");
  Proof p;
  p.kind = static_cast<Kind>(r.u8());
  p.reference.node = r.node_id();
  p.reference.pubkey = r.blob();
  p.chain = r.chain();
  if (p.kind == Kind::order) {
    p.chain2 = r.chain();
    p.order = static_cast<domain::Order>(static_cast<std::int8_t>(r.u8()));
  }
  return p;
}

VerifyResult verify(const Proof& p, const ports::Signer& signer) {
  VerifyResult res;
  if (!p.reference.pubkey.empty() && fingerprint(p.reference.pubkey) != p.reference.node) {
    res.reason = "reference public key does not match the reference node id";
    return res;
  }
  const validate::ChainResult c1 = validate::verify_chain(p.chain, p.reference.node, signer);
  if (!c1.ok) {
    res.reason = c1.reason;
    return res;
  }
  res.lower = c1.lower;
  res.upper = c1.upper;

  if (p.kind == Kind::order) {
    const validate::ChainResult c2 = validate::verify_chain(p.chain2, p.reference.node, signer);
    if (!c2.ok) {
      res.reason = c2.reason;
      return res;
    }
    const domain::Order computed = compare(p.chain, p.chain2);
    if (computed != p.order) {
      res.reason = "recorded order does not match the chains' intervals";
      return res;
    }
    res.order = p.order;
  }

  res.valid = true;
  return res;
}

}  // namespace loti::proof
