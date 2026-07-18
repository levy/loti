# LOTI — Peer-to-peer Non-Consensus Partial Event Ordering

LOTI is a simulation model of a peer-to-peer network in which every participant can
**mathematically prove lower and upper time bounds for any digital event** — expressed in
terms of its own local clock — **without any global consensus, blockchain, or global clock**.
Nodes continuously exchange the hashes of periodic *clock events*, building an ever-growing,
hash-linked distributed DAG. From that DAG a node can reconstruct a tamper-evident *event
chain* that encloses a target event between two of its own clock events, and can thereby
order any two events (before / after / undetermined) according to its local time.

The system is described in the paper *Peer-to-peer Non-Consensus Partial Event Ordering
Network* by Levente Mészáros
([Google Doc](https://docs.google.com/document/d/1KmdknFYkGlxhLoKOQb9g14LW4Jg7zIReaa_GsB5aJio),
local PDF in [doc/](doc/Peer-to-peer%20Non-Consensus%20Partial%20Event%20Ordering%20Network.pdf)).
This repository implements it as a discrete-event simulation on
[OMNeT++ 5.4](https://omnetpp.org) with the [INET 4.0](https://inet.omnetpp.org) framework.

> **What this is and isn't.** This is a research simulation to demonstrate feasibility and
> collect statistics — not a production node. It has **no cryptographic signatures, no key-based
> identities, no incentive layer, and a static topology**, and it approximates the paper's
> probabilistic beam search with a single deterministic shortest-path walk. See
> [doc/paper-vs-implementation.md](doc/paper-vs-implementation.md) for the full list.

## Documentation

Start here depending on what you want:

| If you want to understand… | Read |
| --- | --- |
| **The idea and the math** — goals, events/clock events, proving order and bounds, attack prevention, efficiency | [doc/theory.md](doc/theory.md) |
| **How the code works** — modules, data model, packets, the three discovery algorithms, validation, statistics | [doc/implementation.md](doc/implementation.md) |
| **The on-the-wire protocol** — the three datagrams deployed nodes exchange: field encodings, byte layout, sizes, message flows | [doc/packet-format.md](doc/packet-format.md) |
| **The example network and results** — topology, configuration, charts (the original project README) | [doc/simulation.md](doc/simulation.md) |
| **What is / isn't implemented** — the gap between paper and code, simplifications, known issues | [doc/paper-vs-implementation.md](doc/paper-vs-implementation.md) |
| **The product CLI** — command surface, daemon/client architecture, portable proof format | [doc/cli.md](doc/cli.md) |
| **Dual-target architecture** — how one codebase runs both as the simulation and as the real node | [doc/architecture.md](doc/architecture.md) |
| **Dynamic discovery & the beam search** — coping with churn, lazy links, and topology change | [doc/dynamic-discovery.md](doc/dynamic-discovery.md) |

## Core concepts in 60 seconds

- **Event** — a piece of published content; embeds the hash of a recent local clock event.
- **Clock event** — a timestamped event created ~once per second; embeds the previous local
  clock event, the latest known clock events of neighbors, and recent local events. Only its
  **hash** is broadcast to neighbors.
- **Event chain** — a hash-linked sequence `lowerBound · event · upperBound` that starts and
  ends at the querying node's own clock events and passes through the target event. It is a
  proof of that node's time bounds for the event.
- **Discoveries** — three queries a node's *daemon* can run:
  - **event chain discovery** — reconstruct the enclosing chain for an event (routed to the
    event's creator and back, accreting each hop's clock events);
  - **event bounds discovery** — the enclosing chain's endpoint timestamps `(lower, upper)`;
  - **event order discovery** — compare two events' intervals → `-1`, `+1`, or `0`.

## Simulation model

Each host runs three co-located application modules; one helper module builds the overlay.

- **[Daemon](src/core/node.cpp)** — the protocol engine (grows the local DAG, talks UDP to
  neighbors on port 666, runs the discoveries and validation).
- **[Publisher](src/app/sim/Publisher.cpp)** — periodically creates random-content events.
- **[Browser](src/app/sim/Browser.cpp)** — periodically picks random event(s) and requests a discovery.
- **[NetworkConfigurator](src/app/sim/NetworkConfigurator.cpp)** — builds the neighbor + next-hop
  overlay from the topology once at startup.

The bundled example ([sim/](sim)) is a 57-node network on 1 Gbps / 1 µs links, simulated for
one hour. See [doc/simulation.md](doc/simulation.md) for the topology and result charts.

## Repository layout

```
src/            LOTI source — see doc/architecture.md
  core/         loti-core: pure protocol logic (DAG, discoveries, hashing, wire codec)
  adapters/     port implementations — sim/ (OMNeT++) and os/ (real sockets/disk/crypto)
  app/          frontends — sim/ (Daemon/Publisher/Browser/NetworkConfigurator + .ned),
                lotid/ (node daemon), loti/ (CLI client)
sim/            runnable example
  Network.ned   57-node topology + network-level statistics
  omnetpp.ini   parameters and per-discovery configurations
  Analysis.anf  result analysis skeleton
doc/            documentation (this set) + the paper PDF + result charts
bin/            wrapper scripts (loti, loti_dbg, loti_release)
scripts/        build scripts (build-core.sh, build-sim.sh)
test/           core/harness/acceptance test suites
plan/           design/implementation plans
```

## Building and running

One source tree has **two independent builds** (see [doc/architecture.md](doc/architecture.md)):
the **OMNeT++ simulation** (`opp_makemake`/`make`, needs OMNeT++ + INET) and the **real node** —
the `lotid` daemon and `loti` CLI — built with **CMake**, needing only a C++20 compiler and
OpenSSL. The two never touch the same files.

### The simulation

Prerequisites: OMNeT++ 5.4 and INET 4.0, with `INET_ROOT` set. Then, from the repository
root:

```sh
. setenv                    # sets PATH and LOTI_OMNETPP_OPTIONS (needs INET_ROOT)
make MODE=release           # and/or: make MODE=debug
cd sim
loti                        # convenience wrapper in bin/ (auto-picks release/debug)
```

`loti` (see [bin/loti](bin/loti)) forwards the right `-n` NED paths and INET library to
`opp_run`. You can pass normal OMNeT++ arguments through it, e.g. a configuration:

```sh
loti -u Cmdenv -c EventBoundsDiscovery
```

[sim/omnetpp.ini](sim/omnetpp.ini) defines the base setup (all discovery intervals `0s`,
i.e. off) plus four named configurations that each enable one behavior:
`NoDiscovery`, `EventChainDiscovery`, `EventBoundsDiscovery`, `EventOrderDiscovery`.

### The real node — `lotid` daemon and `loti` CLI

The deployable node ([doc/cli.md](doc/cli.md)) is a separate CMake build with **no OMNeT++/INET
dependency** — just a C++20 compiler, CMake ≥ 3.20, and OpenSSL (libcrypto, for the Ed25519
keystore). It produces two binaries:

- **`lotid`** — the long-running node daemon (grows the DAG, holds the signing key, persists
  state, talks to neighbors over UDP, answers discoveries);
- **`loti`** — the CLI client that drives `lotid` over a local control socket, and verifies
  proofs offline with no daemon.

Build them (and run the `loti-core` unit tests) with the convenience script:

```sh
scripts/build-core.sh                        # CMake Debug build + ctest → build/core/{lotid,loti}
```

or drive CMake directly, e.g. for a release build:

```sh
cmake -S . -B build/core -DCMAKE_BUILD_TYPE=Release
cmake --build build/core -j
```

Both binaries land in `build/core/`. There is no `make install` target yet — to **install**,
copy the two binaries onto your `PATH`:

```sh
sudo install build/core/lotid build/core/loti /usr/local/bin
```

> **Two programs named `loti`.** `bin/loti` is the *simulation* launcher (on your `PATH` after
> `. setenv`); `build/core/loti` is the *real* CLI client. They are different programs — install
> the CLI as above, or invoke it by full path, so the sim wrapper does not shadow it.

**Quickstart** — create an identity, run the daemon, publish, prove, and verify offline:

```sh
loti init                                    # writes ~/.loti/{key,config}; prints your node id
                                             # and the exact `lotid …` line to start
lotid --key ~/.loti/key --port 7000 \
      --store ~/.loti/state.snap --control ~/.loti/control.sock &
loti --control ~/.loti/control.sock publish "hello, world"
loti --control ~/.loti/control.sock status
loti --control ~/.loti/control.sock prove bounds last --out proof.loti
loti verify proof.loti                       # offline — no daemon, no network
```

The CLI locates the daemon via `--control <sock>` (or `$LOTI_CONTROL`, default `./loti.sock`);
`--home` defaults to `~/.loti`. See [doc/cli.md](doc/cli.md) for the full command surface, and
`lotid` / `loti --help` for the flags each accepts. Production integration is exercised
end-to-end by [test/acceptance/run.sh](test/acceptance/run.sh) (restart survival, backup/restore,
a multi-node notary proof over real UDP).

## License

GNU General Public License v3 — see [LICENSE](LICENSE). Copyright (c) 2018 Levente Mészáros.
