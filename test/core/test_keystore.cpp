// Ed25519 keystore: signing, verification, tamper detection, and the identity
// binding (NodeId == public-key fingerprint, pubkey embedded in the signature so
// any party can verify offline).
#include "doctest.h"

#include "adapters/os/keystore.hpp"
#include "domain/types.hpp"
#include "hash/hashing.hpp"

using namespace loti;

TEST_CASE("sign/verify round-trip; signature = 64-byte sig + 32-byte pubkey") {
  os::Ed25519KeyStore ks;  // ephemeral key
  const domain::Bytes msg = {1, 2, 3, 4, 5};
  const auto sig = ks.sign(msg);
  CHECK(sig.size() == 96);
  CHECK(ks.verify(msg, sig, ks.node_id()));
}

TEST_CASE("tampering flips verification to fail") {
  os::Ed25519KeyStore ks;
  const domain::Bytes msg = {10, 20, 30};
  const auto sig = ks.sign(msg);

  auto bad_sig = sig;
  bad_sig[0] ^= 0x01;  // flip a bit of the signature
  CHECK_FALSE(ks.verify(msg, bad_sig, ks.node_id()));

  auto bad_pub = sig;
  bad_pub[70] ^= 0x01;  // flip a bit of the embedded public key
  CHECK_FALSE(ks.verify(msg, bad_pub, ks.node_id()));

  auto bad_msg = msg;
  bad_msg[0] ^= 0x01;  // flip a bit of the message
  CHECK_FALSE(ks.verify(bad_msg, sig, ks.node_id()));
}

TEST_CASE("a signature claiming the wrong identity fails") {
  os::Ed25519KeyStore ks;
  const domain::Bytes msg = {7, 7, 7};
  const auto sig = ks.sign(msg);
  domain::NodeId wrong = ks.node_id();
  wrong.bytes[0] ^= 0x01;  // a different 128-bit id
  CHECK_FALSE(ks.verify(msg, sig, wrong));
}

TEST_CASE("empty signature is rejected (a real signature is required)") {
  os::Ed25519KeyStore ks;  // the production verifier must not accept an unsigned message —
  CHECK_FALSE(ks.verify({1, 2, 3}, {}, 12345));  // otherwise a forged all-unsigned proof verifies
}

TEST_CASE("NodeId is the public-key fingerprint (deterministic)") {
  os::Ed25519KeyStore ks;
  CHECK(ks.node_id() == os::Ed25519KeyStore::fingerprint(ks.public_key()));
}

TEST_CASE("NodeId is a full 128-bit fingerprint, not a truncated 64-bit value") {
  os::Ed25519KeyStore a;
  os::Ed25519KeyStore b;                  // a different key
  CHECK(a.node_id() != b.node_id());      // distinct keys → distinct ids
  CHECK(a.node_id().bytes.size() == 16);  // 128 bits, not 64
  // The high 8 bytes are (almost surely) not all zero — the id is a real 16-byte fingerprint,
  // not merely a zero-extended u64 (which is what the old truncated design produced).
  bool high_nonzero = false;
  for (std::size_t i = 0; i < 8; ++i)
    if (a.node_id().bytes[i] != 0) high_nonzero = true;
  CHECK(high_nonzero);
}

TEST_CASE("one node verifies another's signature from the embedded pubkey alone") {
  os::Ed25519KeyStore signer;    // node A
  os::Ed25519KeyStore verifier;  // node B — different key, no prior knowledge of A
  const domain::Bytes msg = {5, 5, 5, 5};
  const auto sig = signer.sign(msg);
  CHECK(verifier.verify(msg, sig, signer.node_id()));       // knows only A's NodeId
  CHECK_FALSE(verifier.verify(msg, sig, verifier.node_id()));
}

TEST_CASE("the signature is excluded from the event hash") {
  domain::Event e;
  e.creator = 1;
  e.data = {1, 2, 3};
  e.salt = 42;
  const auto before = hash::calculate_event_hash(e);
  e.signature = {9, 9, 9, 9};  // adding a signature must not change the hash
  CHECK(hash::calculate_event_hash(e) == before);
}
