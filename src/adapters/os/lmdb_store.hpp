// An LMDB-backed durable store for a Node's DAG — the production persistence that
// replaces the full-overwrite FileStore (store.hpp). One LMDB environment holds
// several named sub-databases (events, clock events, their hash indices, the
// unreferenced-event set, neighbors, routes) plus a `meta` record with the on-disk
// format version. See plan/pending/lmdb-store.md for the schema and rationale.
//
// Stage 1: the store owns the durable records; the Node keeps its in-RAM DAG and is
// fed from here at startup. Linked into the daemon (and tests) only — loti_core and
// the simulation never see LMDB.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <lmdb.h>

#include "domain/types.hpp"

namespace loti::os {

class LmdbStore {
 public:
  // On-disk layout version, stamped in the `meta` sub-DB; bump only on a format change.
  static constexpr std::uint64_t kFormatVersion = 1;
  // Default env size cap — virtual address space, backed only as used; grown on MDB_MAP_FULL.
  static constexpr std::size_t kDefaultMapSize = std::size_t{16} << 30;  // 16 GiB

  // Opens (creating if absent) the LMDB environment stored at the single file `path`
  // (LMDB keeps its lock file alongside as `<path>-lock`). Throws std::runtime_error on
  // any LMDB failure or a format-version mismatch.
  explicit LmdbStore(std::string path, std::size_t map_size = kDefaultMapSize);
  ~LmdbStore();

  LmdbStore(const LmdbStore&) = delete;
  LmdbStore& operator=(const LmdbStore&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::uint64_t format_version() const noexcept { return format_version_; }

 private:
  std::string path_;
  MDB_env* env_ = nullptr;

  // Named sub-databases (opened once; the handles stay valid for the life of the env).
  MDB_dbi meta_ = 0;
  MDB_dbi events_ = 0;
  MDB_dbi event_index_ = 0;
  MDB_dbi clock_events_ = 0;
  MDB_dbi clock_index_ = 0;
  MDB_dbi unreferenced_ = 0;
  MDB_dbi neighbors_ = 0;
  MDB_dbi routes_ = 0;

  std::uint64_t format_version_ = 0;
};

}  // namespace loti::os
