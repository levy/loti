// Hashing and byte-accounting for the domain model.
//
// The layouts here are a faithful port of the original Daemon::calculateEventHash /
// calculateClockEventHash and Data.cc's calculate*Size — byte-for-byte, so hashes
// and sizes match the simulation exactly. SHA-256 comes from the vendored picosha2.
#pragma once

#include <cstddef>

#include "domain/types.hpp"

namespace loti::hash {

// SHA-256 of an arbitrary byte buffer (32-byte digest).
[[nodiscard]] domain::EventHash sha256(const domain::Bytes& data);

// hash = SHA256( data || salt(u64be) || for each ref: creator(u64be) || hash bytes ).
// The event's own creator/hash and its signature are NOT hashed.
[[nodiscard]] domain::EventHash calculate_event_hash(const domain::Event& event);

// hash = SHA256( timestamp(u64be) || salt(u64be) || for each ref: creator(u64be) || hash bytes ).
[[nodiscard]] domain::EventHash calculate_clock_event_hash(const domain::ClockEvent& clock_event);

// Accounted on-wire/on-disk size (matches Data.cc). Event content (`data`) is
// deliberately EXCLUDED — it is not distributed and not counted.
[[nodiscard]] std::size_t event_size_bytes(const domain::Event& event);
[[nodiscard]] std::size_t clock_event_size_bytes(const domain::ClockEvent& clock_event);

}  // namespace loti::hash
