// A portable snapshot-blob file — the export / backup and migration format.
//
// The Node serializes its whole DAG to an opaque blob (Node::snapshot()); this adapter
// writes it to a file and reads it back. It is NO LONGER the daemon's live store —
// lotid persists incrementally to an LMDB environment (lmdb_store.hpp) — but it remains
// the format for `db-backup` / `db-restore` and for migrating a pre-LMDB `state.snap`
// (start on a fresh --store, then `loti db restore <old.snap>`). Writes go via a temp
// file + rename() so a crash mid-write never corrupts the existing file, and a backup
// is just a copy of the file.
#pragma once

#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "domain/types.hpp"

namespace loti::os {

class FileStore {
 public:
  explicit FileStore(std::string path) : path_(std::move(path)) {}

  [[nodiscard]] std::optional<domain::Bytes> load() const {
    std::ifstream f(path_, std::ios::binary);
    if (!f) return std::nullopt;
    domain::Bytes bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return std::nullopt;
    return bytes;
  }

  // Write via a temp file + atomic rename. Every step is checked and throws on failure —
  // a backup that could not be written must NOT look like it succeeded (its caller, the
  // daemon's db-backup, reports success only if this returns; a throw becomes an ERR reply).
  void save(const domain::Bytes& blob) const {
    const std::string tmp = path_ + ".tmp";
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot open backup file " + tmp);
    f.write(reinterpret_cast<const char*>(blob.data()),
            static_cast<std::streamsize>(blob.size()));
    f.close();  // flush + close; sets failbit on any write/flush error
    if (!f) {
      std::remove(tmp.c_str());
      throw std::runtime_error("cannot write backup file " + tmp);
    }
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {  // atomic replace on POSIX
      std::remove(tmp.c_str());
      throw std::runtime_error("cannot install backup file " + path_);
    }
  }

  [[nodiscard]] const std::string& path() const { return path_; }

 private:
  std::string path_;
};

}  // namespace loti::os
