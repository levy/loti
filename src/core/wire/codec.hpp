// Length-prefixed binary codec for the domain objects that travel on the wire.
//
// This is the transport encoding (distinct from the fixed canonical layout in
// core/hash used only for hashing). The core owns it so both transports — the
// simulation's BytesChunk and production's real datagram — carry identical bytes.
#pragma once

#include <cstdint>
#include <stdexcept>

#include "domain/types.hpp"

namespace loti::wire {

class Writer {
 public:
  void u8(std::uint8_t v) { buf_.push_back(v); }

  void u64(std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) buf_.push_back(std::uint8_t((v >> shift) & 0xFF));
  }

  void blob(const domain::Bytes& b) {
    u32(static_cast<std::uint32_t>(b.size()));
    buf_.insert(buf_.end(), b.begin(), b.end());
  }

  void refs(const std::vector<domain::EventReference>& rs) {
    u32(static_cast<std::uint32_t>(rs.size()));
    for (const auto& r : rs) { u64(r.creator); blob(r.hash); }
  }

  void event(const domain::Event& e) {
    u64(e.creator); blob(e.hash); blob(e.data); u64(e.salt); refs(e.referenced_events); blob(e.signature);
  }

  void clock_event(const domain::ClockEvent& c) {
    u64(c.creator); blob(c.hash); u64(c.chain);
    u64(static_cast<std::uint64_t>(c.timestamp)); u64(c.salt);
    refs(c.referenced_events); blob(c.signature);
  }

  void chain(const domain::EventChain& ch) {
    event(ch.event);
    u32(static_cast<std::uint32_t>(ch.lower_bound.size()));
    for (const auto& c : ch.lower_bound) clock_event(c);
    u32(static_cast<std::uint32_t>(ch.upper_bound.size()));
    for (const auto& c : ch.upper_bound) clock_event(c);
  }

  void time_range(const domain::TimeRange& tr) {
    u64(static_cast<std::uint64_t>(tr.lo));
    u64(static_cast<std::uint64_t>(tr.hi));
  }

  void node_ids(const std::vector<domain::NodeId>& ids) {
    u32(static_cast<std::uint32_t>(ids.size()));
    for (auto id : ids) u64(id);
  }

  [[nodiscard]] const domain::Bytes& bytes() const noexcept { return buf_; }

 private:
  void u32(std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) buf_.push_back(std::uint8_t((v >> shift) & 0xFF));
  }
  domain::Bytes buf_;
};

class Reader {
 public:
  explicit Reader(const domain::Bytes& b) : buf_(b) {}

  std::uint8_t u8() { need(1); return buf_[pos_++]; }

  std::uint64_t u64() {
    need(8);
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | buf_[pos_++];
    return v;
  }

  domain::Bytes blob() {
    std::uint32_t n = u32();
    need(n);
    domain::Bytes out(buf_.begin() + static_cast<long>(pos_), buf_.begin() + static_cast<long>(pos_ + n));
    pos_ += n;
    return out;
  }

  std::vector<domain::EventReference> refs() {
    std::uint32_t n = u32();
    ensure_count(n, 12);  // each ref is ≥12 wire bytes (u64 creator + u32 blob length)
    std::vector<domain::EventReference> out;
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      domain::EventReference r;
      r.creator = u64();
      r.hash = blob();
      out.push_back(std::move(r));
    }
    return out;
  }

  domain::Event event() {
    domain::Event e;
    e.creator = u64(); e.hash = blob(); e.data = blob(); e.salt = u64();
    e.referenced_events = refs(); e.signature = blob();
    return e;
  }

  domain::ClockEvent clock_event() {
    domain::ClockEvent c;
    c.creator = u64(); c.hash = blob(); c.chain = static_cast<std::uint32_t>(u64());
    c.timestamp = static_cast<domain::Timestamp>(u64()); c.salt = u64();
    c.referenced_events = refs(); c.signature = blob();
    return c;
  }

  domain::EventChain chain() {
    domain::EventChain ch;
    ch.event = event();
    std::uint32_t lo = u32();
    ensure_count(lo, 44);  // each clock event is ≥44 wire bytes
    for (std::uint32_t i = 0; i < lo; ++i) ch.lower_bound.push_back(clock_event());
    std::uint32_t hi = u32();
    ensure_count(hi, 44);
    for (std::uint32_t i = 0; i < hi; ++i) ch.upper_bound.push_back(clock_event());
    return ch;
  }

  domain::TimeRange time_range() {
    domain::TimeRange tr;
    tr.lo = static_cast<domain::Timestamp>(u64());
    tr.hi = static_cast<domain::Timestamp>(u64());
    return tr;
  }

  std::vector<domain::NodeId> node_ids() {
    std::uint32_t n = u32();
    ensure_count(n, 8);  // each id is 8 wire bytes
    std::vector<domain::NodeId> out;
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) out.push_back(u64());
    return out;
  }

  [[nodiscard]] bool at_end() const noexcept { return pos_ == buf_.size(); }

 private:
  std::uint32_t u32() {
    need(4);
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | buf_[pos_++];
    return v;
  }
  void need(std::size_t n) const {
    if (pos_ + n > buf_.size()) throw std::runtime_error("wire: truncated datagram");
  }
  // Reject an element count that could not possibly fit in the remaining bytes, BEFORE
  // reserving for it — an attacker-supplied length prefix must not trigger a huge
  // allocation. Division (not multiplication) avoids overflow on a 32-bit size_t.
  void ensure_count(std::uint32_t n, std::size_t min_elem_bytes) const {
    if (n > (buf_.size() - pos_) / min_elem_bytes)
      throw std::runtime_error("wire: element count exceeds datagram");
  }
  const domain::Bytes& buf_;
  std::size_t pos_ = 0;
};

}  // namespace loti::wire
