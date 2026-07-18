#include "adapters/os/lmdb_store.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace loti::os {
namespace {

// Number of named sub-DBs the env must accommodate; keep in sync with the opens below.
constexpr unsigned kMaxDbs = 8;

constexpr char kVersionKey[] = "version";  // key into the `meta` sub-DB

[[noreturn]] void fail_rc(const char* what, int rc) {
  throw std::runtime_error(std::string("lmdb: ") + what + ": " + mdb_strerror(rc));
}
void check(int rc, const char* what) {
  if (rc != MDB_SUCCESS) fail_rc(what, rc);
}

void put_u64_be(unsigned char out[8], std::uint64_t v) {
  for (int i = 7; i >= 0; --i) { out[i] = static_cast<unsigned char>(v & 0xFF); v >>= 8; }
}
std::uint64_t get_u64_be(const unsigned char* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

}  // namespace

LmdbStore::LmdbStore(std::string path, std::size_t map_size) : path_(std::move(path)) {
  check(mdb_env_create(&env_), "env_create");

  // maxdbs and mapsize must both be set before the env is opened.
  if (int rc = mdb_env_set_maxdbs(env_, kMaxDbs); rc != MDB_SUCCESS) {
    mdb_env_close(env_); env_ = nullptr; fail_rc("set_maxdbs", rc);
  }
  if (int rc = mdb_env_set_mapsize(env_, map_size); rc != MDB_SUCCESS) {
    mdb_env_close(env_); env_ = nullptr; fail_rc("set_mapsize", rc);
  }
  // MDB_NOSUBDIR: `path` is a single file (like the old snapshot), not a directory.
  if (int rc = mdb_env_open(env_, path_.c_str(), MDB_NOSUBDIR, 0644); rc != MDB_SUCCESS) {
    mdb_env_close(env_); env_ = nullptr; fail_rc("env_open", rc);
  }

  // Open (creating) every sub-DB and read-or-initialize the format version in one txn.
  MDB_txn* txn = nullptr;
  check(mdb_txn_begin(env_, nullptr, 0, &txn), "txn_begin");
  try {
    auto open = [&](const char* name, MDB_dbi& dbi) {
      check(mdb_dbi_open(txn, name, MDB_CREATE, &dbi), name);
    };
    open("meta", meta_);
    open("events", events_);
    open("event_index", event_index_);
    open("clock_events", clock_events_);
    open("clock_index", clock_index_);
    open("unreferenced", unreferenced_);
    open("neighbors", neighbors_);
    open("routes", routes_);

    MDB_val key{sizeof(kVersionKey) - 1, const_cast<char*>(kVersionKey)};
    MDB_val val{};
    int rc = mdb_get(txn, meta_, &key, &val);
    if (rc == MDB_NOTFOUND) {
      unsigned char buf[8];
      put_u64_be(buf, kFormatVersion);
      MDB_val v{sizeof(buf), buf};
      check(mdb_put(txn, meta_, &key, &v, 0), "put version");
      format_version_ = kFormatVersion;
    } else {
      check(rc, "get version");
      if (val.mv_size != sizeof(std::uint64_t))
        throw std::runtime_error("lmdb: meta 'version' record has an unexpected size");
      format_version_ = get_u64_be(static_cast<const unsigned char*>(val.mv_data));
      if (format_version_ != kFormatVersion)
        throw std::runtime_error("lmdb: unsupported store format version " +
                                 std::to_string(format_version_));
    }
    check(mdb_txn_commit(txn), "txn_commit");
  } catch (...) {
    mdb_txn_abort(txn);
    mdb_env_close(env_);
    env_ = nullptr;
    throw;
  }
}

LmdbStore::~LmdbStore() {
  if (env_) mdb_env_close(env_);
}

}  // namespace loti::os
