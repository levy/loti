// The three LOTI datagram types, each behind a header carrying the message type
// and the sender's NodeId.
#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include "domain/types.hpp"

namespace loti::wire {

enum class Type : std::uint8_t {
  clock_notification = 0,
  chain_request = 1,
  chain_response = 2,
};

struct ClockNotification {
  std::uint32_t chain = 0;  // which of the sender's chains this tip belongs to
  domain::EventHash last_clock_event_hash;
  domain::EventHash neighbor_last_clock_event_hash;
};

struct ChainRequest {
  domain::NodeId originator = 0;
  domain::EventReference event;
  domain::TimeRange range;           // the querying party's estimated window for `event`
  std::uint32_t hop_limit = 0;       // max forward hops (0 = unlimited) — the flood-depth cap
  // Reverse-path breadcrumb: the request's forward path of senders, [originator, …, prev].
  // It doubles as the per-copy visited set (a node forwards only to neighbors not already in
  // it — loop avoidance). The creator reflects the response back along it (Decision 6).
  std::vector<domain::NodeId> path;
};

struct ChainResponse {
  domain::NodeId originator = 0;
  domain::EventChain chain;
  // Remaining reverse path back to the originator: the next hop is path.back(), popped each
  // hop; empty once the response reaches the originator.
  std::vector<domain::NodeId> path;
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
