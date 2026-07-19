#include "validate/chain.hpp"

#include <vector>

#include "hash/hashing.hpp"

namespace loti::validate {
namespace {

bool references(const std::vector<domain::EventReference>& refs,
                const domain::EventReference& target) {
  for (const auto& r : refs)
    if (r == target) return true;
  return false;
}

}  // namespace

ChainResult verify_chain(const domain::EventChain& chain, domain::NodeId reference,
                         const ports::Signer& signer) {
  ChainResult res;
  if (chain.lower_bound.empty() || chain.upper_bound.empty()) {
    res.reason = "chain missing an enclosing bound";
    return res;
  }
  if (chain.lower_bound.front().creator != reference) {
    res.reason = "first clock event is not the reference node's";
    return res;
  }
  if (chain.upper_bound.back().creator != reference) {
    res.reason = "last clock event is not the reference node's";
    return res;
  }

  domain::EventReference prev;
  bool have_prev = false;
  for (const auto& ce : chain.lower_bound) {
    if (hash::calculate_clock_event_hash(ce) != ce.hash) {
      res.reason = "lower-bound clock event hash mismatch";
      return res;
    }
    if (!signer.verify(ce.hash, ce.signature, ce.creator)) {
      res.reason = "lower-bound clock event signature invalid";
      return res;
    }
    if (have_prev && !references(ce.referenced_events, prev)) {
      res.reason = "lower bound is not linked";
      return res;
    }
    prev = domain::EventReference{ce.creator, ce.hash};
    have_prev = true;
  }

  if (hash::calculate_event_hash(chain.event) != chain.event.hash) {
    res.reason = "event hash mismatch";
    return res;
  }
  if (!references(chain.event.referenced_events, prev)) {
    res.reason = "event does not reference the lower bound";
    return res;
  }
  if (!signer.verify(chain.event.hash, chain.event.signature, chain.event.creator)) {
    res.reason = "event signature invalid";
    return res;
  }
  prev = domain::EventReference{chain.event.creator, chain.event.hash};

  // `prev` is the event as the loop starts. Every upper-bound clock event — including the
  // first — must reference its predecessor: the first references the event itself, anchoring
  // the upper bound to it. (Skipping this check for the first element let a disconnected
  // upper bound, one that does not enclose the event at all, validate.)
  for (const auto& ce : chain.upper_bound) {
    if (hash::calculate_clock_event_hash(ce) != ce.hash) {
      res.reason = "upper-bound clock event hash mismatch";
      return res;
    }
    if (!signer.verify(ce.hash, ce.signature, ce.creator)) {
      res.reason = "upper-bound clock event signature invalid";
      return res;
    }
    if (!references(ce.referenced_events, prev)) {
      res.reason = "upper bound is not linked";
      return res;
    }
    prev = domain::EventReference{ce.creator, ce.hash};
  }

  res.lower = chain.lower_bound.front().timestamp;
  res.upper = chain.upper_bound.back().timestamp;
  // The interval must not be inverted: the reference node's lower clock event cannot be
  // later than its upper. A non-monotonic reference clock could otherwise present a
  // structurally sound but meaningless (negative-width) bound as valid.
  if (res.upper < res.lower) {
    res.reason = "inverted interval (upper bound earlier than lower bound)";
    return res;  // res.ok stays false
  }
  res.ok = true;
  return res;
}

}  // namespace loti::validate
