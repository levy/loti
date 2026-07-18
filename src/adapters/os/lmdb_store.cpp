#include "adapters/os/lmdb_store.hpp"

#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

#include "wire/codec.hpp"

namespace loti::os {
namespace {

// Number of named sub-DBs the env must accommodate; keep in sync with the opens below.
constexpr unsigned kMaxDbs = 7;

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

// One past the highest sequence key currently in `dbi` (0 if empty). Keys are 8-byte
// big-endian, so MDB_LAST returns the numerically greatest.
std::uint64_t next_seq(MDB_txn* txn, MDB_dbi dbi) {
  MDB_cursor* cur = nullptr;
  check(mdb_cursor_open(txn, dbi, &cur), "cursor_open");
  MDB_val k{}, v{};
  int rc = mdb_cursor_get(cur, &k, &v, MDB_LAST);
  std::uint64_t next = 0;
  if (rc == MDB_SUCCESS) {
    next = get_u64_be(static_cast<const unsigned char*>(k.mv_data)) + 1;
  } else if (rc != MDB_NOTFOUND) {
    mdb_cursor_close(cur);
    fail_rc("cursor_last", rc);
  }
  mdb_cursor_close(cur);
  return next;
}

// Iterate every record of `dbi` in key order within a read-only transaction.
void for_each(MDB_env* env, MDB_dbi dbi,
              const std::function<void(const MDB_val&, const MDB_val&)>& fn) {
  MDB_txn* txn = nullptr;
  check(mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn), "rtxn_begin");
  MDB_cursor* cur = nullptr;
  if (int rc = mdb_cursor_open(txn, dbi, &cur); rc != MDB_SUCCESS) {
    mdb_txn_abort(txn);
    fail_rc("cursor_open", rc);
  }
  MDB_val k{}, v{};
  int rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST);
  while (rc == MDB_SUCCESS) {
    fn(k, v);
    rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
  }
  mdb_cursor_close(cur);
  mdb_txn_abort(txn);
  if (rc != MDB_NOTFOUND) fail_rc("cursor_next", rc);
}

std::size_t entry_count(MDB_env* env, MDB_dbi dbi) {
  MDB_txn* txn = nullptr;
  check(mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn), "rtxn_begin");
  MDB_stat st{};
  int rc = mdb_stat(txn, dbi, &st);
  mdb_txn_abort(txn);
  check(rc, "stat");
  return st.ms_entries;
}

domain::Bytes to_bytes(const MDB_val& v) {
  const auto* p = static_cast<const std::uint8_t*>(v.mv_data);
  return domain::Bytes(p, p + v.mv_size);
}

}  // namespace

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------

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

  // Open (creating) every sub-DB, read-or-initialize the format version, and seed the
  // sequence counters from the existing logs — all in one txn.
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

    next_event_seq_ = next_seq(txn, events_);
    next_clock_seq_ = next_seq(txn, clock_events_);

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

LmdbStore::Batch LmdbStore::begin() {
  MDB_txn* txn = nullptr;
  check(mdb_txn_begin(env_, nullptr, 0, &txn), "txn_begin");
  return Batch(*this, txn);
}

// ---------------------------------------------------------------------------
// Batch — the write path
// ---------------------------------------------------------------------------

LmdbStore::Batch::Batch(LmdbStore& store, MDB_txn* txn)
    : store_(store), txn_(txn), event_seq_(store.next_event_seq_), clock_seq_(store.next_clock_seq_) {}

LmdbStore::Batch::~Batch() {
  if (txn_) mdb_txn_abort(txn_);  // uncommitted → abort; counters were never advanced
}

std::uint64_t LmdbStore::Batch::append_event(const domain::Event& e) {
  const std::uint64_t seq = event_seq_++;

  wire::Writer w;
  w.event(e);
  unsigned char kbuf[8];
  put_u64_be(kbuf, seq);
  MDB_val k{sizeof(kbuf), kbuf};
  MDB_val v{w.bytes().size(), const_cast<std::uint8_t*>(w.bytes().data())};
  check(mdb_put(txn_, store_.events_, &k, &v, 0), "put event");

  unsigned char sbuf[8];
  put_u64_be(sbuf, seq);
  MDB_val ik{e.hash.size(), const_cast<std::uint8_t*>(e.hash.data())};
  MDB_val iv{sizeof(sbuf), sbuf};
  check(mdb_put(txn_, store_.event_index_, &ik, &iv, 0), "put event_index");

  return seq;
}

std::uint64_t LmdbStore::Batch::append_clock_event(const domain::LocalClockEvent& c) {
  const std::uint64_t seq = clock_seq_++;

  wire::Writer w;
  w.clock_event(c);               // the ClockEvent part
  w.refs(c.referencing_events);   // the learned back-references (LocalClockEvent extra)
  unsigned char kbuf[8];
  put_u64_be(kbuf, seq);
  MDB_val k{sizeof(kbuf), kbuf};
  MDB_val v{w.bytes().size(), const_cast<std::uint8_t*>(w.bytes().data())};
  check(mdb_put(txn_, store_.clock_events_, &k, &v, 0), "put clock_event");

  unsigned char sbuf[8];
  put_u64_be(sbuf, seq);
  MDB_val ik{c.hash.size(), const_cast<std::uint8_t*>(c.hash.data())};
  MDB_val iv{sizeof(sbuf), sbuf};
  check(mdb_put(txn_, store_.clock_index_, &ik, &iv, 0), "put clock_index");

  return seq;
}

void LmdbStore::Batch::update_clock_event(const domain::LocalClockEvent& c) {
  MDB_val ik{c.hash.size(), const_cast<std::uint8_t*>(c.hash.data())};
  MDB_val sv{};
  int rc = mdb_get(txn_, store_.clock_index_, &ik, &sv);
  if (rc == MDB_NOTFOUND)
    throw std::runtime_error("lmdb: update_clock_event for an unknown clock event");
  check(rc, "get clock_index");

  unsigned char kbuf[8];
  std::memcpy(kbuf, sv.mv_data, sizeof(kbuf));  // copy the seq out before we write
  wire::Writer w;
  w.clock_event(c);
  w.refs(c.referencing_events);
  MDB_val k{sizeof(kbuf), kbuf};
  MDB_val v{w.bytes().size(), const_cast<std::uint8_t*>(w.bytes().data())};
  check(mdb_put(txn_, store_.clock_events_, &k, &v, 0), "update clock_event");
}

void LmdbStore::Batch::put_neighbor(const domain::Neighbor& n) {
  unsigned char kbuf[8];
  put_u64_be(kbuf, n.node_id);
  MDB_val k{sizeof(kbuf), kbuf};
  MDB_val v{n.last_clock_event_hash.size(),
            const_cast<std::uint8_t*>(n.last_clock_event_hash.data())};
  check(mdb_put(txn_, store_.neighbors_, &k, &v, 0), "put_neighbor");
}

void LmdbStore::Batch::put_route(domain::NodeId destination, domain::NodeId next_hop) {
  unsigned char kbuf[8];
  put_u64_be(kbuf, destination);
  unsigned char vbuf[8];
  put_u64_be(vbuf, next_hop);
  MDB_val k{sizeof(kbuf), kbuf};
  MDB_val v{sizeof(vbuf), vbuf};
  check(mdb_put(txn_, store_.routes_, &k, &v, 0), "put_route");
}

void LmdbStore::Batch::commit() {
  if (!txn_) return;
  MDB_txn* t = txn_;
  txn_ = nullptr;                        // consumed by commit regardless of outcome
  check(mdb_txn_commit(t), "txn_commit");
  store_.next_event_seq_ = event_seq_;   // advance only after a durable commit
  store_.next_clock_seq_ = clock_seq_;
}

// ---------------------------------------------------------------------------
// bulk read-back (startup replay)
// ---------------------------------------------------------------------------

std::vector<domain::Event> LmdbStore::load_events() const {
  std::vector<domain::Event> out;
  for_each(env_, events_, [&](const MDB_val&, const MDB_val& v) {
    domain::Bytes bytes = to_bytes(v);
    wire::Reader r(bytes);
    out.push_back(r.event());
  });
  return out;
}

std::vector<domain::LocalClockEvent> LmdbStore::load_clock_events() const {
  std::vector<domain::LocalClockEvent> out;
  for_each(env_, clock_events_, [&](const MDB_val&, const MDB_val& v) {
    domain::Bytes bytes = to_bytes(v);
    wire::Reader r(bytes);
    domain::LocalClockEvent c;
    static_cast<domain::ClockEvent&>(c) = r.clock_event();
    c.referencing_events = r.refs();
    out.push_back(std::move(c));
  });
  return out;
}

std::map<domain::NodeId, domain::Neighbor> LmdbStore::load_neighbors() const {
  std::map<domain::NodeId, domain::Neighbor> out;
  for_each(env_, neighbors_, [&](const MDB_val& k, const MDB_val& v) {
    domain::Neighbor n;
    n.node_id = get_u64_be(static_cast<const unsigned char*>(k.mv_data));
    n.last_clock_event_hash = to_bytes(v);
    out[n.node_id] = std::move(n);
  });
  return out;
}

std::map<domain::NodeId, domain::NodeId> LmdbStore::load_routes() const {
  std::map<domain::NodeId, domain::NodeId> out;
  for_each(env_, routes_, [&](const MDB_val& k, const MDB_val& v) {
    out[get_u64_be(static_cast<const unsigned char*>(k.mv_data))] =
        get_u64_be(static_cast<const unsigned char*>(v.mv_data));
  });
  return out;
}

std::size_t LmdbStore::event_count() const { return entry_count(env_, events_); }
std::size_t LmdbStore::clock_event_count() const { return entry_count(env_, clock_events_); }

void LmdbStore::sync() { check(mdb_env_sync(env_, 1), "env_sync"); }

void LmdbStore::reset() {
  MDB_txn* txn = nullptr;
  check(mdb_txn_begin(env_, nullptr, 0, &txn), "txn_begin");
  for (MDB_dbi dbi : {events_, event_index_, clock_events_, clock_index_, neighbors_, routes_}) {
    if (int rc = mdb_drop(txn, dbi, 0); rc != MDB_SUCCESS) {  // del=0: empty but keep the DB
      mdb_txn_abort(txn);
      fail_rc("drop", rc);
    }
  }
  check(mdb_txn_commit(txn), "txn_commit");
  next_event_seq_ = 0;
  next_clock_seq_ = 0;
}

}  // namespace loti::os
