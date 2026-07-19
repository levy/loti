// Ed25519 keystore on OpenSSL EVP. See keystore.hpp for the identity design.

#include "adapters/os/keystore.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "hash/hashing.hpp"

namespace loti::os {

namespace {
constexpr std::size_t kEd25519Raw = 32;  // raw private and public key length
constexpr std::size_t kEd25519Sig = 64;  // signature length
EVP_PKEY* as_pkey(void* p) { return static_cast<EVP_PKEY*>(p); }
}  // namespace

Ed25519KeyStore::Ed25519KeyStore() {
  pkey_ = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
  if (pkey_ == nullptr) throw std::runtime_error("Ed25519 keygen failed");
  adopt_generated();
}

Ed25519KeyStore::~Ed25519KeyStore() {
  if (pkey_ != nullptr) EVP_PKEY_free(as_pkey(pkey_));
}

void Ed25519KeyStore::adopt_generated() {
  public_key_.assign(kEd25519Raw, 0);
  std::size_t len = kEd25519Raw;
  if (EVP_PKEY_get_raw_public_key(as_pkey(pkey_), public_key_.data(), &len) != 1 ||
      len != kEd25519Raw)
    throw std::runtime_error("cannot extract Ed25519 public key");
}

void Ed25519KeyStore::set_private_key(const domain::Bytes& raw32) {
  if (raw32.size() != kEd25519Raw) throw std::runtime_error("bad Ed25519 private key length");
  EVP_PKEY* pk = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, raw32.data(), raw32.size());
  if (pk == nullptr) throw std::runtime_error("cannot load Ed25519 private key");
  if (pkey_ != nullptr) EVP_PKEY_free(as_pkey(pkey_));
  pkey_ = pk;
  adopt_generated();
}

void Ed25519KeyStore::load_or_generate(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (in) {
    domain::Bytes raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    set_private_key(raw);
    OPENSSL_cleanse(raw.data(), raw.size());  // scrub the raw private key from this buffer
    ::chmod(path.c_str(), 0600);  // best-effort: tighten an existing looser (e.g. 0644) key file
    return;
  }
  // No key on disk: keep the ephemeral key generated in the constructor and persist it.
  // Write to a 0600 temp file (never a world-readable window), fsync-free, then atomically
  // rename into place. Every step is checked — a key that failed to persist must not look
  // like it succeeded, or the node silently regenerates a new identity on the next start.
  domain::Bytes raw(kEd25519Raw, 0);
  std::size_t len = kEd25519Raw;
  if (EVP_PKEY_get_raw_private_key(as_pkey(pkey_), raw.data(), &len) != 1 || len != kEd25519Raw)
    throw std::runtime_error("cannot extract Ed25519 private key");
  const std::string tmp = path + ".tmp";
  ::unlink(tmp.c_str());
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) throw std::runtime_error("cannot create key file " + tmp);
  std::size_t off = 0;
  while (off < raw.size()) {
    const ssize_t n = ::write(fd, raw.data() + off, raw.size() - off);
    if (n <= 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      throw std::runtime_error("cannot write key file " + tmp);
    }
    off += static_cast<std::size_t>(n);
  }
  OPENSSL_cleanse(raw.data(), raw.size());  // scrub the raw private key now it is on disk
  if (::close(fd) != 0) {
    ::unlink(tmp.c_str());
    throw std::runtime_error("cannot flush key file " + tmp);
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    throw std::runtime_error("cannot install key file " + path);
  }
}

domain::Signature Ed25519KeyStore::sign(const domain::Bytes& message) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) throw std::runtime_error("EVP_MD_CTX_new failed");
  domain::Signature sig(kEd25519Sig, 0);
  std::size_t siglen = kEd25519Sig;
  const bool ok = EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, as_pkey(pkey_)) == 1 &&
                  EVP_DigestSign(ctx, sig.data(), &siglen, message.data(), message.size()) == 1;
  EVP_MD_CTX_free(ctx);
  if (!ok || siglen != kEd25519Sig) throw std::runtime_error("Ed25519 sign failed");
  sig.insert(sig.end(), public_key_.begin(), public_key_.end());  // append the public key
  return sig;
}

bool Ed25519KeyStore::verify(const domain::Bytes& message, const domain::Signature& signature,
                             domain::NodeId signer) const {
  if (signature.empty()) return false;  // a real signature is required — unsigned is NOT verified
  if (signature.size() != kEd25519Sig + kEd25519Raw) return false;
  const domain::Bytes pub(signature.begin() + kEd25519Sig, signature.end());
  if (fingerprint(pub) != signer) return false;  // pubkey must match the claimed identity
  EVP_PKEY* pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), pub.size());
  if (pk == nullptr) return false;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  bool ok = false;
  if (ctx != nullptr && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pk) == 1)
    ok = EVP_DigestVerify(ctx, signature.data(), kEd25519Sig, message.data(), message.size()) == 1;
  if (ctx != nullptr) EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pk);
  return ok;
}

domain::NodeId Ed25519KeyStore::fingerprint(const domain::Bytes& public_key) {
  const domain::EventHash digest = hash::sha256(public_key);
  domain::NodeId id;  // 128-bit: the first 16 bytes of SHA-256(pubkey)
  for (std::size_t i = 0; i < 16 && i < digest.size(); ++i) id.bytes[i] = digest[i];
  return id;
}

}  // namespace loti::os
