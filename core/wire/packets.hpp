// The three LOTI datagram types (a faithful port of src/Packet.msg), each behind
// a header carrying the message type and the sender's NodeId.
#pragma once

#include <cstdint>
#include <variant>

#include "domain/types.hpp"

namespace loti::wire {

enum class Type : std::uint8_t {
  clock_notification = 0,
  chain_request = 1,
  chain_response = 2,
};

struct ClockNotification {
  domain::EventHash last_clock_event_hash;
  domain::EventHash neighbor_last_clock_event_hash;
};

struct ChainRequest {
  domain::NodeId originator = 0;
  domain::EventReference event;
};

struct ChainResponse {
  domain::NodeId originator = 0;
  domain::EventChain chain;
};

using Payload = std::variant<ClockNotification, ChainRequest, ChainResponse>;

struct Datagram {
  domain::NodeId sender = 0;
  Payload payload;
};

[[nodiscard]] domain::Bytes encode(domain::NodeId sender, const ClockNotification&);
[[nodiscard]] domain::Bytes encode(domain::NodeId sender, const ChainRequest&);
[[nodiscard]] domain::Bytes encode(domain::NodeId sender, const ChainResponse&);

[[nodiscard]] Datagram decode(const domain::Bytes& datagram);

}  // namespace loti::wire
