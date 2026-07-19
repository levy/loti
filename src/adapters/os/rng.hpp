// Rng port, production adapter — a cryptographically secure salt source.
//
// Salts (and, later, keys) are security-critical: a predictable salt lets an
// attacker grind hash preimages, so production draws from the OS CSPRNG
// (getrandom(2), i.e. the same pool as /dev/urandom) rather than a seeded PRNG.
// The simulation instead uses the seeded OMNeT++ RNG so runs replay exactly.
#pragma once

#include <sys/random.h>

#include <cerrno>
#include <cstddef>
#include <stdexcept>

#include "ports/rng.hpp"

namespace loti::os {

class SecureRng final : public ports::Rng {
 public:
  domain::Salt next_salt() override {
    domain::Salt salt = 0;
    std::size_t got = 0;
    auto* p = reinterpret_cast<unsigned char*>(&salt);
    while (got < sizeof(salt)) {
      const ssize_t n = ::getrandom(p + got, sizeof(salt) - got, 0);
      if (n < 0) {
        if (errno == EINTR) continue;  // interrupted (e.g. early boot, before entropy) — retry
        throw std::runtime_error("getrandom failed");
      }
      got += static_cast<std::size_t>(n);
    }
    return salt;
  }
};

}  // namespace loti::os
