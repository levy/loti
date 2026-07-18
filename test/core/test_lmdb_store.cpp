// Tests for the LMDB-backed store (src/adapters/os/lmdb_store).
//
// Step 1 covers the lifecycle only: an env opens, stamps the format version, and
// reopens cleanly. The write/replay round-trips arrive with later steps.
#include "adapters/os/lmdb_store.hpp"

#include <filesystem>
#include <string>
#include <system_error>

#include <unistd.h>

#include "doctest.h"

namespace fs = std::filesystem;
using loti::os::LmdbStore;

namespace {

// A unique temp path for an LMDB env, removed (with its `-lock` sibling) on scope exit.
struct TempEnv {
  fs::path path;
  explicit TempEnv(const std::string& tag)
      : path(fs::temp_directory_path() /
             ("loti_lmdb_test_" + tag + "_" + std::to_string(::getpid()))) {
    remove();
  }
  ~TempEnv() { remove(); }
  void remove() const {
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(fs::path(path.string() + "-lock"), ec);
  }
  std::string str() const { return path.string(); }
};

}  // namespace

TEST_CASE("LmdbStore opens, stamps a version, and reopens") {
  TempEnv env("open");

  SUBCASE("a fresh env is created and stamped with the current format version") {
    LmdbStore store(env.str());
    CHECK(store.path() == env.str());
    CHECK(store.format_version() == LmdbStore::kFormatVersion);
    CHECK(fs::exists(env.path));
  }

  SUBCASE("reopening an existing env reads back the same version") {
    { LmdbStore first(env.str()); }
    LmdbStore second(env.str());
    CHECK(second.format_version() == LmdbStore::kFormatVersion);
  }
}
