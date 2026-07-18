# Vendored LMDB

Lightning Memory-Mapped Database — the embedded store engine for `lotid`'s
persistent DAG (see `src/adapters/os/store.hpp`).

- **Version:** 0.9.31 (matches the system `liblmdb0` on the build host).
- **Upstream:** https://github.com/LMDB/lmdb, tag `LMDB_0.9.31`, from
  `libraries/liblmdb/`.
- **Files:** `lmdb.h`, `mdb.c`, `midl.h`, `midl.c` (the whole engine), plus
  upstream `LICENSE` and `CHANGES`.
- **License:** The OpenLDAP Public License (see `LICENSE`) — permissive, BSD-like.
- **Modifications:** none. Compiled as-is by the `loti_lmdb` target in the top-level
  `CMakeLists.txt`, with warnings silenced (`-w`) because it is unmodified upstream C.

`loti_lmdb` is linked into the `lotid` daemon only. `loti_core` stays pure and the
OMNeT++ simulation excludes this directory (`-X third_party` in `scripts/build-sim.sh`).
