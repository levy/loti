# Running a LOTI node on a Raspberry Pi (embedded)

A LOTI node is a cheap thing to run. The daemon (`lotid`) is a lean, single-threaded
C++20 program with a vendored embedded database and no runtime dependencies beyond libc
and OpenSSL — so it is happy on a **~$15 Raspberry Pi Zero**, always-on, sipping power.
This guide is the honest version: **why** it fits, **how** to build and run it, which
**targets** to pick, and the **limits** you should know before you leave one running for a
year.

> Status note: this reflects what is implemented today. Part D of
> [plan/done/constrained-node-support.md](../plan/done/constrained-node-support.md) made a
> node's memory **bounded** (the DAG is read through a page-cache-backed store, not held in
> RAM). The cross-compile toolchains and on-device build are implemented; the concrete
> **on-Pi numbers** below (RSS, DB growth) should be re-measured on your hardware — the
> binary sizes were measured on an x86-64 build host and ARM builds will differ.

---

## Why it fits

- **Lean core, no heavy runtime.** The protocol engine (`loti_core`) is plain C++20 with no
  framework. The daemon adds an epoll reactor, a UDP socket, an Ed25519 keystore, and the
  store. A Release, stripped `lotid` is well under a megabyte (≈0.6 MB on the x86-64 build
  host; measure your ARM build on-device). The only shared-library dependencies are libc
  and OpenSSL's `libcrypto`.
- **Embedded database, vendored.** Persistence is [LMDB](../third_party/lmdb) compiled
  straight into the binary (no server, no daemon, no schema migrations tool). One file on
  the SD card holds the whole DAG.
- **Single-threaded, event-driven.** One epoll loop drives everything; the node never
  blocks and never spins. Idle cost is essentially zero — it wakes on a packet or a timer.
- **Bounded memory.** The Node keeps no copy of the event log in RAM. It reads and writes
  the DAG through a `Store` port whose LMDB mmap is backed by the **OS page cache**, so
  resident memory is bounded by available RAM: cold pages evict under pressure, hot pages
  stay, and reads are zero-copy. The node's own working set is tiny (neighbors, routes, the
  handful of not-yet-referenced events, and any in-flight discoveries).
- **Portable, offline-verifiable proofs.** A proof is a self-contained blob; anyone can
  verify it with the `loti` CLI and no network. A Pi node is a perfectly good notary.

## Targets

| Board | CPU | Word size | RAM | Notes |
|---|---|---|---|---|
| **Pi Zero 2 W** | quad Cortex-A53 | **64-bit** (aarch64) | 512 MB | **Recommended long-life node.** No 32-bit address-space ceiling. |
| Pi Zero / Zero W | single ARM1176 | **32-bit** (ARMv6) | 512 MB | Works, but the 32-bit LMDB mmap caps the *lifetime* DAG (see Limits). WiFi only on Zero W; Zero has no radio. |

The **Zero 2 W (aarch64)** is the target to standardise on: 64-bit, four cores, and no
address-space limit on how big the stored DAG can grow over the node's life.

## How to build

Two options: cross-compile from a dev box (fast, recommended), or build on the device.

### Option 1 — cross-compile (recommended)

The toolchain files live in [`cmake/`](../cmake) and the wrapper is
[`scripts/build-cross.sh`](../scripts/build-cross.sh).

**1. Install the cross toolchain (Debian/Ubuntu host).**

```sh
# aarch64 (Zero 2 W)
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
# armv6 (Zero / Zero W)
sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
```

**2. Provide a target `libcrypto`** so `find_package(OpenSSL)` resolves. Two ways:

- **Multiarch dev packages** (simplest for aarch64):

  ```sh
  sudo dpkg --add-architecture arm64        # or: armhf
  sudo apt update
  sudo apt install libssl-dev:arm64         # or: libssl-dev:armhf
  ```

  Caveat for **ARMv6**: the distro `armhf` toolchain and `libssl:armhf` target ARMv7. The
  toolchain file forces our sources to ARMv6 (`-march=armv6 -mfpu=vfp -mfloat-abi=hard`),
  but a distro `libcrypto:armhf` may contain ARMv7 instructions and fault on a real Pi Zero.
  For a binary that truly runs on ARMv6, use a Pi OS sysroot (below).

- **A Pi sysroot** rsynced from the device (the robust way, and required for real ARMv6):

  ```sh
  # on the Pi: make sure libssl-dev is installed, then from the host:
  mkdir -p ~/pi-sysroot
  rsync -aL pi@raspberrypi.local:/lib     ~/pi-sysroot/
  rsync -aL pi@raspberrypi.local:/usr/lib ~/pi-sysroot/usr/
  rsync -aL pi@raspberrypi.local:/usr/include ~/pi-sysroot/usr/
  export LOTI_SYSROOT=~/pi-sysroot
  ```

  The toolchain files pick up `LOTI_SYSROOT` (or `-DLOTI_SYSROOT=…`) and resolve headers
  and libraries only from the image.

**3. Build.**

```sh
scripts/build-cross.sh aarch64      # → build/aarch64/{lotid,loti}
scripts/build-cross.sh armv6        # → build/armv6/{lotid,loti}
```

It configures a Release build with the matching toolchain and builds only `lotid` and
`loti`. (The host can't run a cross build's `ctest`; test on the Pi, or under
`qemu-user-static` — optional, see below.)

**4. Deploy.**

```sh
scp build/aarch64/lotid build/aarch64/loti pi@raspberrypi.local:/usr/local/bin/
```

**Optional — run the unit tests under emulation on the host (nice for CI):**

```sh
sudo apt install qemu-user-static
# configure a native-style build with the cross toolchain, then run the test binary
# through qemu-aarch64-static (or qemu-arm-static for armv6).
```

### Option 2 — build on the device

The Pi can compile the project itself (slower, but no cross-toolchain fuss):

```sh
sudo apt install cmake g++ libssl-dev
scripts/build-core.sh          # builds loti_core, lotid, loti + runs the tests
```

## How to run

```sh
loti init                                   # generate a key + print the start command
lotid --key ~/.loti/key --port 7000 \
      --control ~/.loti/control.sock \
      --store /var/lib/loti/dag.mdb \       # the DAG file — see storage notes
      --store-mapsize 2 \                   # GiB; 64-bit default is 16, 32-bit is capped
      --store-sync-interval 30              # lazy fsync every 30 s (SD-friendly; see below)
```

Recommended Pi flags:

- **`--store <path>`** — put the DAG on durable, high-endurance storage. A high-endurance
  microSD is fine; a small **USB SSD** is better for a long-life node (endurance and
  cold-read latency both improve).
- **`--store-mapsize <GiB>`** — the LMDB map cap (virtual address space, grown on demand).
  Default is 16 GiB on 64-bit; on 32-bit it defaults to 1 GiB and is clamped to ~1.5 GiB.
- **`--store-sync-interval <s>`** — SD-wear knob, below.
- Key and config come from `loti init`; keep the key file backed up.

### The sync knob (microSD wear)

By default every committed change is fsync'd — durable per commit, but that is a lot of
small writes for an SD card. Set **`--store-sync-interval <seconds> > 0`** and the store
opens `MDB_NOSYNC`: per-commit fsyncs are skipped and a timer flushes every interval, so
the OS coalesces dirty pages and **actual SD writes drop** (less write amplification).

The database stays **crash-consistent** either way (LMDB is copy-on-write; without
`MDB_WRITEMAP` a crash never corrupts it). The only tradeoff is a bounded **durability
window**: a crash in lazy mode may lose up to the last `<interval>` of commits — never the
database's integrity. A clean shutdown and the `save` command always force a final flush.

There is a second reason to prefer lazy mode on slow or high-latency storage: the daemon is
single-threaded, so in the default **safe** mode the per-commit `fsync` runs *on the reactor
thread* — while it blocks, no packets or control commands are serviced. On a busy SD card or
network-backed disk that stall is visible. `--store-sync-interval > 0` moves the flush to a
periodic timer, so a storage-latency spike no longer blocks networking. (Moving `fsync` to a
background thread is a possible future improvement.)

| `--store-sync-interval` | Behaviour | Crash risk |
|---|---|---|
| `0` (default) | fsync every commit | none (fully durable) |
| `> 0` | `MDB_NOSYNC` + flush every `<s>` | lose ≤ `<s>` of commits; never corruption |

(`MDB_NOMETASYNC` — fsync the data but not the meta page — is a documented middle ground;
it is not wired to a flag today.)

## Limits (the honest part)

- **512 MB RAM — now bounded.** Since Part D the DAG is not held in RAM; it is read through
  the page-cache-backed store, so steady-state RSS is bounded (a small working set plus
  whatever hot DB pages the cache holds). It will **not** grow without bound over weeks the
  way the earlier in-RAM design did. *Re-measure steady-state RSS on your Pi to confirm for
  your workload.*
- **32-bit store ceiling.** An LMDB environment can never exceed its mapsize, and a 32-bit
  process can only mmap ~2 GiB, so on a **Pi Zero / Zero W** the store is capped (~1.5 GiB). The
  multi-resolution clock chains keep *clock-event* storage flat, so this ceiling is reached only by
  accumulated **published-event content**, which is not pruned: a heavy publisher may fill it in a
  year or two, a light one never. The **Zero 2 W (64-bit)** has no such ceiling — it is the real
  long-life target.
- **microSD wear.** Even with the sync knob, flash wears. Use a high-endurance card or a
  USB SSD, and prefer a larger `--store-sync-interval` if your durability window allows.
- **Cold-read latency.** Discovering an *old* event may page its records back from the SD
  card (a fault), which is slower than RAM. The hot path — recent events and the clock-chain
  tip — stays in the page cache. This only affects discoveries of long-past events.
- **Radio.** The Zero W is WiFi-only (no Ethernet); the original Zero has no radio at all
  (add USB networking). The Zero 2 W is WiFi + BT.

## Numbers to measure on-device (don't trust aspirational figures)

Before quoting figures anywhere, measure them on the actual Pi:

- **Binary size** — `size lotid` / `ls -l lotid` on the ARM build (ARM ≠ the ~0.6 MB x86-64
  figure above).
- **Steady-state RSS** — `ps -o rss= -p $(pidof lotid)` after a day of running.
- **DB growth rate** — `db-stat`'s `diskBytes` over time (and the paper's GB/year model).

---

See also: [architecture.md](architecture.md) (the port model and the store),
[cli.md](cli.md) (the daemon flags and control commands), and
[plan/done/constrained-node-support.md](../plan/done/constrained-node-support.md) (the full
design and rationale for A–E).
