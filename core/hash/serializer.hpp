// Canonical byte writer — the OMNeT++/INET-free replacement for the
// `MemoryOutputStream` the original Daemon used to serialize objects for hashing.
// The core OWNS this encoding so the bytes hashed here are identical to the bytes
// both transports (sim and production) put on the wire — that is what keeps the
// simulation's byte accounting predictive of production.
#pragma once

#include <cstdint>

#include "domain/types.hpp"

namespace loti::hash {

class ByteWriter {
 public:
  void write_bytes(const domain::Bytes& b) { buf_.insert(buf_.end(), b.begin(), b.end()); }

  void write_u64_be(std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      buf_.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFF));
    }
  }

  [[nodiscard]] const domain::Bytes& data() const noexcept { return buf_; }

 private:
  domain::Bytes buf_;
};

}  // namespace loti::hash
