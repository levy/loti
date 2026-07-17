// A durable blob store — the production persistence for a node's snapshot.
//
// The Node serializes its whole DAG to an opaque blob (Node::snapshot()); this
// adapter writes it to a file and reads it back, so a restarted lotid resumes with
// all prior events and clock events. Writes go via a temp file + rename() so a
// crash mid-write never corrupts the existing snapshot, and a backup is just a copy
// of the file. Incremental/append-only storage (for the paper's GB/year scale) is a
// later optimization; a periodic full snapshot is enough for MVP restart-survival.
#pragma once

#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
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

  void save(const domain::Bytes& blob) const {
    const std::string tmp = path_ + ".tmp";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char*>(blob.data()),
              static_cast<std::streamsize>(blob.size()));
    }
    std::rename(tmp.c_str(), path_.c_str());  // atomic replace on POSIX
  }

  [[nodiscard]] const std::string& path() const { return path_; }

 private:
  std::string path_;
};

}  // namespace loti::os
