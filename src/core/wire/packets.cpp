#include "wire/packets.hpp"

#include <stdexcept>

#include "wire/codec.hpp"

namespace loti::wire {

namespace {
void write_header(Writer& w, Type type, domain::NodeId sender) {
  w.u8(static_cast<std::uint8_t>(type));
  w.u64(sender);
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
  w.u64(m.originator);
  w.u64(m.event.creator);
  w.blob(m.event.hash);
  return w.bytes();
}

domain::Bytes encode(domain::NodeId sender, const ChainResponse& m) {
  Writer w;
  write_header(w, Type::chain_response, sender);
  w.u64(m.originator);
  w.chain(m.chain);
  return w.bytes();
}

Datagram decode(const domain::Bytes& datagram) {
  Reader r(datagram);
  const auto type = static_cast<Type>(r.u8());
  Datagram out;
  out.sender = r.u64();
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
      m.originator = r.u64();
      m.event.creator = r.u64();
      m.event.hash = r.blob();
      out.payload = std::move(m);
      break;
    }
    case Type::chain_response: {
      ChainResponse m;
      m.originator = r.u64();
      m.chain = r.chain();
      out.payload = std::move(m);
      break;
    }
    default:
      throw std::runtime_error("wire: unknown datagram type");
  }
  return out;
}

}  // namespace loti::wire
