#include "proof/proof.hpp"

#include <stdexcept>

#include "hash/hashing.hpp"
#include "wire/codec.hpp"

namespace loti::proof {
namespace {

// NodeId = first 8 bytes (big-endian) of SHA-256(public key). Must match
// Ed25519KeyStore::fingerprint so a proof's reference identity checks out against
// the embedded public key without linking the crypto adapter into the core.
domain::NodeId fingerprint(const domain::Bytes& public_key) {
  const domain::EventHash digest = hash::sha256(public_key);
  domain::NodeId id = 0;
  for (std::size_t i = 0; i < 8 && i < digest.size(); ++i) id = (id << 8) | digest[i];
  return id;
}

bool references(const std::vector<domain::EventReference>& refs, const domain::EventReference& target) {
  for (const auto& r : refs)
    if (r == target) return true;
  return false;
}

// Recompute hashes, check linkage + endpoints, verify signatures. Mirrors
// Node::validate_event_chain / validate_chain_discovery_result, but returns a reason
// instead of throwing and reports the enclosing interval on success.
bool verify_chain(const domain::EventChain& chain, domain::NodeId reference,
                  const ports::Signer& signer, std::string& reason,
                  domain::Timestamp& lower, domain::Timestamp& upper) {
  if (chain.lower_bound.empty() || chain.upper_bound.empty()) {
    reason = "chain missing an enclosing bound";
    return false;
  }
  if (chain.lower_bound.front().creator != reference) {
    reason = "first clock event is not the reference node's";
    return false;
  }
  if (chain.upper_bound.back().creator != reference) {
    reason = "last clock event is not the reference node's";
    return false;
  }

  domain::EventReference prev;
  bool have_prev = false;
  for (const auto& ce : chain.lower_bound) {
    if (hash::calculate_clock_event_hash(ce) != ce.hash) {
      reason = "lower-bound clock event hash mismatch";
      return false;
    }
    if (!signer.verify(ce.hash, ce.signature, ce.creator)) {
      reason = "lower-bound clock event signature invalid";
      return false;
    }
    if (have_prev && !references(ce.referenced_events, prev)) {
      reason = "lower bound is not linked";
      return false;
    }
    prev = domain::EventReference{ce.creator, ce.hash};
    have_prev = true;
  }

  if (hash::calculate_event_hash(chain.event) != chain.event.hash) {
    reason = "event hash mismatch";
    return false;
  }
  if (!references(chain.event.referenced_events, prev)) {
    reason = "event does not reference the lower bound";
    return false;
  }
  if (!signer.verify(chain.event.hash, chain.event.signature, chain.event.creator)) {
    reason = "event signature invalid";
    return false;
  }
  prev = domain::EventReference{chain.event.creator, chain.event.hash};

  bool first = true;
  for (const auto& ce : chain.upper_bound) {
    if (hash::calculate_clock_event_hash(ce) != ce.hash) {
      reason = "upper-bound clock event hash mismatch";
      return false;
    }
    if (!signer.verify(ce.hash, ce.signature, ce.creator)) {
      reason = "upper-bound clock event signature invalid";
      return false;
    }
    if (!first && !references(ce.referenced_events, prev)) {
      reason = "upper bound is not linked";
      return false;
    }
    prev = domain::EventReference{ce.creator, ce.hash};
    first = false;
  }

  lower = chain.lower_bound.front().timestamp;
  upper = chain.upper_bound.back().timestamp;
  return true;
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
  w.u64(p.reference.node);
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
  p.reference.node = r.u64();
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
  if (!verify_chain(p.chain, p.reference.node, signer, res.reason, res.lower, res.upper)) return res;

  if (p.kind == Kind::order) {
    domain::Timestamp lo2 = 0, hi2 = 0;
    if (!verify_chain(p.chain2, p.reference.node, signer, res.reason, lo2, hi2)) return res;
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
