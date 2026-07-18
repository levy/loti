#include "hash/hashing.hpp"

#include "hash/picosha2.hpp"
#include "hash/serializer.hpp"

namespace loti::hash {

namespace {

constexpr std::size_t kHashBytes = 32;    // SHA-256 digest
constexpr std::size_t kNodeIdBytes = 8;   // u64
constexpr std::size_t kSaltBytes = 8;     // u64
constexpr std::size_t kTimestampBytes = 8;  // u64
constexpr std::size_t kRefBytes = kNodeIdBytes + kHashBytes;

void write_refs(ByteWriter& w, const std::vector<domain::EventReference>& refs) {
  for (const auto& ref : refs) {
    w.write_u64_be(ref.creator);
    w.write_bytes(ref.hash);
  }
}

}  // namespace

domain::EventHash sha256(const domain::Bytes& data) {
  domain::EventHash digest(picosha2::k_digest_size);
  picosha2::hash256(data.begin(), data.end(), digest.begin(), digest.end());
  return digest;
}

domain::EventHash calculate_event_hash(const domain::Event& event) {
  ByteWriter w;
  w.write_bytes(event.data);
  w.write_u64_be(event.salt);
  write_refs(w, event.referenced_events);
  return sha256(w.data());
}

domain::EventHash calculate_clock_event_hash(const domain::ClockEvent& clock_event) {
  ByteWriter w;
  w.write_u64_be(static_cast<std::uint64_t>(clock_event.timestamp));
  w.write_u64_be(clock_event.salt);
  write_refs(w, clock_event.referenced_events);
  return sha256(w.data());
}

std::size_t event_size_bytes(const domain::Event& event) {
  // NodeId creator + EventHash hash + Salt salt + refs. Content `data` excluded.
  return kNodeIdBytes + kHashBytes + kSaltBytes + kRefBytes * event.referenced_events.size();
}

std::size_t clock_event_size_bytes(const domain::ClockEvent& clock_event) {
  return kNodeIdBytes + kHashBytes + kTimestampBytes + kSaltBytes +
         kRefBytes * clock_event.referenced_events.size();
}

}  // namespace loti::hash
