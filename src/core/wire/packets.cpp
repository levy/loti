#include "wire/packets.hpp"

#include <stdexcept>

#include "wire/codec.hpp"

namespace loti::wire {

namespace {
void write_header(Writer& w, Type type, const domain::NodeId& sender) {
  w.u8(static_cast<std::uint8_t>(type));
  w.node_id(sender);
}
}  // namespace

domain::Bytes encode(domain::NodeId sender, const ClockNotification& m) {
  Writer w;
  write_header(w, Type::clock_notification, sender);
  w.u64(m.chain);
  w.blob(m.last_clock_event_hash);
  w.blob(m.neighbor_last_clock_event_hash);
  return w.bytes();
}

domain::Bytes encode(domain::NodeId sender, const ChainRequest& m) {
  Writer w;
  write_header(w, Type::chain_request, sender);
  w.node_id(m.originator);
  w.node_id(m.event.creator);
  w.blob(m.event.hash);
  w.time_range(m.range);
  w.u64(m.hop_limit);
  w.node_ids(m.path);
  return w.bytes();
}

domain::Bytes encode(domain::NodeId sender, const ChainResponse& m) {
  Writer w;
  write_header(w, Type::chain_response, sender);
  w.node_id(m.originator);
  w.chain(m.chain);
  w.node_ids(m.path);
  return w.bytes();
}

std::optional<domain::NodeId> sender_of(const domain::Bytes& datagram) {
  // Header = type (1 byte) + sender NodeId (16 bytes), matching write_header.
  if (datagram.size() < 1 + 16) return std::nullopt;
  domain::NodeId id;
  for (std::size_t i = 0; i < 16; ++i) id.bytes[i] = datagram[1 + i];
  return id;
}

Datagram decode(const domain::Bytes& datagram) {
  Reader r(datagram);
  const auto type = static_cast<Type>(r.u8());
  Datagram out;
  out.sender = r.node_id();
  switch (type) {
    case Type::clock_notification: {
      ClockNotification m;
      m.chain = static_cast<std::uint32_t>(r.u64());
      m.last_clock_event_hash = r.blob();
      m.neighbor_last_clock_event_hash = r.blob();
      out.payload = std::move(m);
      break;
    }
    case Type::chain_request: {
      ChainRequest m;
      m.originator = r.node_id();
      m.event.creator = r.node_id();
      m.event.hash = r.blob();
      m.range = r.time_range();
      m.hop_limit = static_cast<std::uint32_t>(r.u64());
      m.path = r.node_ids();
      out.payload = std::move(m);
      break;
    }
    case Type::chain_response: {
      ChainResponse m;
      m.originator = r.node_id();
      m.chain = r.chain();
      m.path = r.node_ids();
      out.payload = std::move(m);
      break;
    }
    default:
      throw std::runtime_error("wire: unknown datagram type");
  }
  return out;
}

}  // namespace loti::wire
