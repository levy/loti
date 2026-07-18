// An LMDB-backed durable store for a Node's DAG — the production persistence that
// replaces the full-overwrite FileStore (store.hpp). One LMDB environment holds
// several named sub-databases (events, clock events, their hash indices, neighbors,
// routes) plus a `meta` record with the on-disk format version. See
// plan/pending/lmdb-store.md for the schema and rationale.
//
// Stage 1: the store owns the durable records; the Node keeps its in-RAM DAG and is
// fed from here at startup. Linked into the daemon (and tests) only — loti_core and
// the simulation never see LMDB.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <lmdb.h>

#include "domain/types.hpp"
#include "ports/store.hpp"

namespace loti::os {

// Thrown by a write when the environment hits its mapsize cap (MDB_MAP_FULL). The
// caller can grow_map() and retry — the failed transaction leaves no state behind.
struct LmdbMapFull : std::runtime_error {
  LmdbMapFull() : std::runtime_error("lmdb: map full (MDB_MAP_FULL)") {}
};

// Thrown by grow_map() when the environment cannot grow any further — on a 32-bit
// build the mmap has reached the per-process address-space ceiling (kMaxMapSize).
// Unlike LmdbMapFull (recoverable: grow and retry) this is terminal for the build's
// word size; the caller should surface it, not retry.
struct LmdbStoreFull : std::runtime_error {
  LmdbStoreFull()
      : std::runtime_error("lmdb: store full (32-bit address-space limit reached)") {}
};

class LmdbStore final : public ports::Store {
 public:
  // How durably a commit reaches disk. `safe` fsyncs on every commit (each change is
  // durable before the daemon replies). `lazy` opens the env with MDB_NOSYNC: LMDB skips
  // the per-commit fsync and the OS flushes dirty pages on its own schedule, so actual SD
  // writes coalesce (less wear) — sync() (a reactor timer, clean shutdown, or `save`)
  // forces them out. Without MDB_WRITEMAP this stays crash-*consistent*; only durability
  // of the last <flush-window> of commits is at risk (never corruption). See doc/embedded.md.
  enum class SyncPolicy { safe, lazy };

  // On-disk layout version, stamped in the `meta` sub-DB; bump only on a format change.
  // v2 added the `referencing` reverse-index sub-DB (a v1 store is migrated on first open).
  // v3 added the per-clock-event `chain`/level field (multi-resolution chains); back-compat
  // with pre-v3 stores is out of scope — a mismatched store is rejected, not migrated.
  static constexpr std::uint64_t kFormatVersion = 3;

  // Hard ceiling on the env's mapsize. The map is virtual address space, so on 64-bit
  // this is effectively unbounded and left at 0 = "no cap". On 32-bit the mmap must fit
  // in the ~2–3 GiB per-process address space, so it is capped at 1.5 GiB — both
  // --store-mapsize (clamped in lotid) and grow_map() honor it. NB: because an LMDB env
  // can never exceed its mapsize, this also bounds a 32-bit node's *total* stored DAG.
  static constexpr std::size_t kMaxMapSize =
      sizeof(std::size_t) >= 8 ? std::size_t{0} : (std::size_t{3} << 29);  // 0 = none / 1.5 GiB

  // Default env size cap — virtual address space, backed only as used; grown on MDB_MAP_FULL.
  // Word-size-aware: on 32-bit `size_t{16} << 30` (2^34) wraps to 0, and 16 GiB can't be
  // mmap'd anyway, so start at 1 GiB (safely under kMaxMapSize).
  static constexpr std::size_t kDefaultMapSize =
      sizeof(std::size_t) >= 8 ? (std::size_t{16} << 30) : (std::size_t{1} << 30);  // 16 / 1 GiB

  // Opens (creating if absent) the LMDB environment stored at the single file `path`
  // (LMDB keeps its lock file alongside as `<path>-lock`). `sync` picks the durability
  // policy (default safe = fsync per commit). Throws std::runtime_error on any LMDB
  // failure or a format-version mismatch.
  explicit LmdbStore(std::string path, std::size_t map_size = kDefaultMapSize,
                     SyncPolicy sync = SyncPolicy::safe);
  ~LmdbStore();

  LmdbStore(const LmdbStore&) = delete;
  LmdbStore& operator=(const LmdbStore&) = delete;

  // A write transaction grouping several mutations into one durable commit (which
  // amortizes the fsync). LMDB permits a single writer at a time, so at most one Batch
  // may be open per store. A Batch left uncommitted on destruction is aborted, and the
  // store's sequence counters are only advanced by a successful commit() — so an
  // aborted batch leaves no gaps.
  class Batch {
   public:
    ~Batch();
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

    // Append an event / local clock event to its log; returns the assigned sequence
    // number. Also writes the hash→seq index entry, atomically in the same txn.
    std::uint64_t append_event(const domain::Event&);
    std::uint64_t append_clock_event(const domain::LocalClockEvent&);

    // Re-persist an already-appended clock event whose referencing_events changed
    // (located by its hash via the clock index). Throws if it was never appended.
    void update_clock_event(const domain::LocalClockEvent&);

    // Upsert overlay state.
    void put_neighbor(const domain::Neighbor&);
    void put_route(domain::NodeId destination, domain::NodeId next_hop);

    // Durably commit the transaction. The Batch is spent afterwards.
    void commit();

   private:
    friend class LmdbStore;
    Batch(LmdbStore& store, MDB_txn* txn);

    LmdbStore& store_;
    MDB_txn* txn_;
    std::uint64_t event_seq_;
    std::uint64_t clock_seq_;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> pending_chain_appends_;  // (chain, seq)
  };

  // Begin a write transaction. Throws if a batch cannot be started.
  [[nodiscard]] Batch begin();

  // ---- ports::Store: single-record writes (each its own durable txn; grows the map and
  // retries once on MDB_MAP_FULL). The Node writes the DAG exclusively through these.
  std::uint64_t append_event(const domain::Event&) override;
  std::uint64_t append_clock_event(const domain::LocalClockEvent&) override;
  void update_clock_event(const domain::LocalClockEvent&) override;
  void put_neighbor(const domain::Neighbor&) override;
  void put_route(domain::NodeId destination, domain::NodeId next_hop) override;
  void put_timed_routes(const domain::TimedRouteTable&) override;
  void prune_chain(std::uint32_t chain, std::size_t keep) override;

  // ---- ports::Store: reads (each in its own read txn; the mmap keeps them zero-copy) ----
  [[nodiscard]] std::optional<domain::Event> event_by_hash(const domain::EventHash&) const override;
  [[nodiscard]] domain::Event event_by_seq(std::uint64_t seq) const override;
  [[nodiscard]] std::optional<domain::LocalClockEvent> clock_event_by_hash(
      const domain::EventHash&) const override;
  [[nodiscard]] domain::LocalClockEvent clock_event_by_seq(std::uint64_t seq) const override;
  [[nodiscard]] std::optional<std::uint64_t> clock_event_seq(
      const domain::EventHash&) const override;
  [[nodiscard]] std::vector<std::uint64_t> clock_events_referencing(
      const domain::EventHash&) const override;
  [[nodiscard]] std::optional<domain::LocalClockEvent> latest_clock_event() const override;
  [[nodiscard]] std::optional<domain::LocalClockEvent> latest_clock_event(
      std::uint32_t chain) const override;

  // Bulk read-back for startup replay. Events and clock events come back in sequence
  // order; neighbors/routes keyed by node id. (The unreferenced set is not stored — the
  // Node rederives it from the clock events on load.)
  [[nodiscard]] std::vector<domain::Event> load_events() const;
  [[nodiscard]] std::vector<domain::LocalClockEvent> load_clock_events() const override;
  [[nodiscard]] std::map<domain::NodeId, domain::Neighbor> load_neighbors() const override;
  [[nodiscard]] std::map<domain::NodeId, domain::NodeId> load_routes() const override;
  [[nodiscard]] domain::TimedRouteTable load_timed_routes() const override;

  [[nodiscard]] std::size_t event_count() const override;
  [[nodiscard]] std::size_t clock_event_count() const override;

  // Force all committed data to stable storage (backs the `save` control command).
  void sync();

  // Empty every data sub-DB (keeps `meta`) and reset the sequence counters. Used by
  // db-restore before rewriting the store from a snapshot. clear() is the port alias.
  void reset();
  void clear() override { reset(); }

  // Double the environment's mapsize. Call after catching LmdbMapFull, with no live
  // transaction, then retry the write. The map is virtual — only backed as used.
  void grow_map();

  [[nodiscard]] std::size_t map_size() const noexcept { return map_size_; }
  [[nodiscard]] SyncPolicy sync_policy() const noexcept { return sync_policy_; }

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::uint64_t format_version() const noexcept { return format_version_; }

 private:
  // Migration (v1 → v2): populate the `referencing` reverse index by scanning every
  // clock event's referenced_events, within the caller's open write transaction.
  void rebuild_referencing_index(MDB_txn* txn);

  // Rebuild the in-RAM per-chain tip map (chain -> newest seq) by scanning the clock
  // events. Called once at open (after the constructor's txn commits).
  void seed_chain_tips();

  // Fetch a single record's bytes within a read txn; nullopt if the key is absent.
  [[nodiscard]] std::optional<domain::Bytes> get_bytes(MDB_dbi dbi, const void* key,
                                                       std::size_t key_len) const;
  // The seq stored under a hash key in an index sub-DB (event_index / clock_index).
  [[nodiscard]] std::optional<std::uint64_t> seq_of(MDB_dbi index, const domain::EventHash&) const;

  std::string path_;
  std::size_t map_size_ = 0;
  SyncPolicy sync_policy_ = SyncPolicy::safe;
  MDB_env* env_ = nullptr;

  // Named sub-databases (opened once; the handles stay valid for the life of the env).
  MDB_dbi meta_ = 0;
  MDB_dbi events_ = 0;
  MDB_dbi event_index_ = 0;
  MDB_dbi clock_events_ = 0;
  MDB_dbi clock_index_ = 0;
  MDB_dbi referencing_ = 0;  // DUPSORT: referenced hash -> seqs of clock events referencing it
  MDB_dbi neighbors_ = 0;
  MDB_dbi routes_ = 0;
  MDB_dbi timed_routes_ = 0;  // destination -> serialized [TimedRoute] (time-dependent routing)

  std::uint64_t format_version_ = 0;
  std::uint64_t next_event_seq_ = 0;  // next unused key in `events`
  std::uint64_t next_clock_seq_ = 0;  // next unused key in `clock_events`

  // In-RAM per-chain indices, seeded at open and maintained on each durable commit:
  //  - chain_tip_seq_: chain/level -> seq of that chain's newest clock event (backs
  //    latest_clock_event(chain));
  //  - chain_seqs_: chain -> its live seqs oldest-first (backs prune_chain, which pops the
  //    front and deletes the record + its index entries).
  std::map<std::uint32_t, std::uint64_t> chain_tip_seq_;
  std::map<std::uint32_t, std::deque<std::uint64_t>> chain_seqs_;
};

}  // namespace loti::os
