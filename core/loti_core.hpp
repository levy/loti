// loti-core — the pure protocol library.
//
// This header is a Stage-0 placeholder. Its only job right now is to give the
// loti_core target a public surface to compile, link, and test, proving the
// build split and the dependency rule (no OMNeT++/INET/OS includes here).
//
// The real API — the domain model, hashing, the DAG, the three discoveries,
// validation, and the port interfaces — arrives in later MVP stages
// (see plan/pending/mvp-dual-target.md and documentation/architecture.md).
#pragma once

#include <string_view>

namespace loti::core {

// Bumped when the on-the-wire / on-disk formats change incompatibly.
inline constexpr int protocol_version = 1;

// A trivial self-identification used by the smoke test to confirm the library
// builds and links.
[[nodiscard]] std::string_view library_id() noexcept;

}  // namespace loti::core
