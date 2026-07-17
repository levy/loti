# LOTI CLI — Design Specification

This document specifies the command-line interface for a **deployable LOTI node** — the tool
that turns the [simulation model](implementation.md) into a viable product. It defines the full
command surface, the daemon/client architecture, and the portable proof format.

A **working subset is already implemented**: the [`lotid`](../app/lotid/lotid.cpp) daemon and
[`loti`](../app/loti/loti.cpp) client cover `init`, the control-socket RPC, `peer add`/`peer ls`,
`publish`, `events`/`event show`/`event find`, the three discoveries (`bounds` / `chain` / `order`,
including remote `<creator>:<hash>` addressing), Ed25519 identity and signing, snapshot persistence
with `db stat`/`backup`/`restore`, and portable, offline-verifiable proofs (`prove` / `verify` /
`proof show`). The rest of the surface below — daemon supervision, key rotation, routing/overlay
management, the `clock` subcommands, `db gc`/`db verify`, `config`, `stats`/`metrics`, and
reference-node targeting — remains design/reference (see
[paper-vs-implementation.md](paper-vs-implementation.md) for the remaining gaps). See
[Implementation status](#implementation-status-mvp) below for the exact command table and where the
implementation deviates from this spec.

How one codebase provides both this CLI/daemon and the simulation model is described in
[architecture.md](architecture.md).

The commands are grounded in the operations the system actually needs
([theory.md](theory.md)): run a node, hold a key-based identity, publish events, grow and
share a clock-event DAG, run the three discoveries, and — the product's whole point —
**produce and verify portable proofs of an event's time bounds and of the order of two
events** that a third party can check offline.

## Goals and non-goals of the CLI

**Goals**

- Expose every operation a node operator and an end user need to run a node and use the
  network in production.
- Make **proofs first-class**: generate a self-contained proof artifact, and verify one
  **without needing to be a participant** (offline, no network, no trust in the prover beyond
  the reference node's clock).
- Be **scriptable**: stable exit codes, `--json` everywhere, stdin/stdout friendly, no
  interactive prompts unless a TTY is present and `--yes` is absent.
- Feel familiar: a long-running daemon plus a thin client, in the style of
  `bitcoind`/`bitcoin-cli`, `ipfs`, `dockerd`/`docker`, `wg`/`wg-quick`.

**Non-goals**

- The CLI does not define the wire protocol or storage engine; it assumes the daemon
  implements them (extending the simulation's `LotiHeader` + clock/discovery packets to a
  persistent, signed, dynamic node).
- No GUI. A future web/browser front end would talk to the same daemon RPC.

## From simulation to product

The CLI presupposes a real node, which requires promoting several "not implemented" items
from [paper-vs-implementation.md](paper-vs-implementation.md) to real features. Each is called
out at the relevant command as a **Requires:** note. In summary the daemon must add:

- **Persistence** — a durable store for local events, the local clock-event chain, learned
  neighbor cross-links, and discovery results (the simulation keeps everything in RAM).
- **Key-based identity and signing** — Ed25519 (or similar) keys; `NodeId` becomes a public-key
  fingerprint, and events/clock events are signed so proofs are attributable.
- **Real, dynamic peering** — neighbors join/leave; routes recompute; no global configurator.
- **A control channel** — a local RPC socket so `loti` can drive `lotid`.
- **Proof export/verification** — serialize an event chain into a portable, offline-verifiable
  artifact.

## Implementation status (MVP)

The MVP delivers the operator's core loop — run a signed node, publish, discover, **prove and
verify offline** — plus persistence. All of the daemon promotions above are implemented.

**Implemented commands** (`loti <cmd>`; also accepted on the daemon's stdin):

| Area | Commands |
| --- | --- |
| Node & identity | `init`, `status` (`node status`), `key`, `stop` (`node stop`) |
| Events | `publish <text>`, `events`, `event show <hash\|last>`, `event find <text>` |
| Discovery | `bounds <e>`, `chain <e>`, `order <a> <b>` — `<e>` is a local `hash`/`last` **or** a remote `<creator>:<hash>` |
| Proofs | `prove bounds\|order\|chain … --out <f>`, `verify <f>`, `proof show <f>` |
| Peers | `peer add <id:ip:port>`, `peer ls` |
| Storage | `db stat`, `db backup --out <f>`, `db restore <f>` |
| Global | `--control`, `--json` (pretty, nested), `--quiet`, `--home`, `--out`, `--trust`, `--help` |

**Deviations from this spec** (accepted for the MVP):

- **Proof format is the compact binary wire form**, not the JSON shown under
  [Proof format](#proof-format). It reuses the DAG wire codec (magic `LOTIPROF` + version), so a
  proof's bytes are byte-exact with what the node serializes and no JSON library is pulled in.
  `verify`/`proof show` render a JSON *view* of it under `--json`.
- **`--trust <node>` is advisory**: `verify` proves integrity + attribution (exit `0`/`6`); with
  `--trust` it prints a warning if the reference node is outside the set but does not change the
  exit code.
- **`--reference <node>` (choosing which node anchors a proof) is deferred**: the reference is
  always the node running the discovery. Remote-event addressing `<creator>:<hash>` lets an
  independent node prove a *peer's* event — that node becomes the reference (an independent-notary
  proof), which the acceptance suite exercises over real UDP.
- **RPC is a hand-rolled versioned line protocol** over a Unix socket (not JSON-RPC); `--json` is
  a client-side rendering of the reply fields.
- **`event find <text>`** (local content substring search) is an addition not in the original
  command surface.

**Retention.** The rule *local events and the local clock chain are never dropped* (they back all
future proofs) holds **by construction**: nothing prunes them, and `snapshot`/`restore` round-trip
the whole DAG. `db gc` and `db verify` from [Storage & maintenance](#storage--maintenance) are not
implemented — there is no expendable derived state to garbage-collect at MVP scale.

**Deferred (post-MVP):** `config get/set/list` (+ a config-file loader; `init` writes a flat config
that the operator pastes into the `lotid` command), `node start` (process spawn/daemonize),
`publish --file|--sign|--salt|--wait` flags, `log` tailing, multi-reference proofs, and an
incremental/append or embedded-DB store (the MVP writes a periodic full snapshot).

## Architecture

```
          proof.loti (portable artifact)
                 ▲                 ▲
                 │ export          │ verify (offline, no daemon needed)
   ┌─────────┐   │   ┌──────────────────────────┐        ┌─────────────┐
   │  loti   │───┼──▶│   lotid (node daemon)     │◀──UDP─▶│  neighbor   │
   │  (CLI)  │  RPC  │  DAG · keys · store · net │  P2P   │   lotid     │
   └─────────┘       └──────────────────────────┘        └─────────────┘
```

- **`lotid`** — the long-running node: maintains the local clock-event DAG, holds the signing
  key, persists state, talks to neighbor daemons over the peer-to-peer transport, and answers
  discoveries. It must run continuously to keep issuing clock events and to serve neighbors.
- **`loti`** — the CLI client: connects to the local daemon over a control socket (default
  `$LOTI_HOME/control.sock`) and issues commands. `loti verify` and other pure-crypto
  operations run **without** a daemon.

## Global conventions

### Invocation

```
loti [global-flags] <command> [subcommand] [args] [flags]
lotid [global-flags]                      # the daemon
```

Global flags: `--home <dir>` (state dir, default `$LOTI_HOME` or `~/.loti`), `--json`,
`--quiet`, `-v/--verbose`, `--yes` (assume yes), `--rpc <addr>` (control socket/endpoint),
`--no-color`, `--config <file>`.

### Configuration precedence

`command-line flag` > `LOTI_*` environment variable > `$LOTI_HOME/config.toml` > built-in
default. See [Configuration reference](#configuration-reference).

### Object addressing

Everything in LOTI is content-addressed by a 32-byte SHA-256 digest (64 hex chars). The CLI
accepts:

- **Events / clock events** — full hex hash, or an unambiguous **short prefix** (≥ 8 hex),
  optionally namespaced `event:<hash>` / `clock:<hash>` when disambiguation is needed.
- **Nodes** — a public-key fingerprint (hash of the public key), a short prefix, a configured
  **alias** (`loti peer add … --alias court`), or `@self` for the local node.
- **`-`** — read the object/content from stdin; many commands also take `--file <path>`.

### Output and exit codes

Human-readable tables by default; `--json` emits stable machine JSON. Standard exit codes:

| Code | Meaning |
| --- | --- |
| `0` | success (for `verify`: proof is valid) |
| `1` | generic error |
| `2` | usage error (bad flags/args) |
| `3` | daemon not running / unreachable |
| `4` | not found (unknown event, node, proof) |
| `5` | discovery failed or expired (no chain could be built) |
| `6` | **verification failed** (proof invalid) |
| `7` | timeout |

## Command reference

### Node lifecycle & configuration

| Command | Purpose |
| --- | --- |
| `loti init` | Create `$LOTI_HOME`, generate an identity key, write a default `config.toml`. |
| `loti node start` | Launch/supervise `lotid` (foreground with `--foreground`). |
| `loti node stop` | Gracefully stop the daemon (flush store, close peers). |
| `loti node restart` | Stop then start. |
| `loti status` | Node health: identity, uptime, peer count, DAG size, last clock event, in-flight discoveries, store size. `--json` for monitoring. |
| `loti version` | Client + daemon version, protocol version, build info. |
| `loti config get <key>` / `set <key> <value>` / `list` | Read/update configuration; `set` may require `loti node restart` for some keys (flagged in output). |

```console
$ loti init
identity  node:9f3a…c1   (ed25519)
home      ~/.loti
$ loti node start
lotid started (pid 4821), listening udp/:4666, control ~/.loti/control.sock
$ loti status
node        node:9f3a…c1
peers       7 connected / 9 known
clock       tip clock:8b21…  t=2026-07-17T10:31:04Z  (interval 1s)
events      1,204 local
store       412 MB
discoveries 3 in-flight
```

**Requires:** persistence, control socket, daemon supervision.

### Identity & keys

The paper gives every node a private/public key pair and lets it sign events and clock events.
Signatures are what make a proof *attributable* to a reference node.

| Command | Purpose |
| --- | --- |
| `loti key gen` | Generate a fresh identity key (refuses to overwrite without `--force`). |
| `loti key show` | Show public key, fingerprint (`@self` node id), algorithm. `--secret` reveals the private key (guarded by `--yes`). |
| `loti key export --out <file>` / `key import <file>` | Back up / restore the identity, encrypted with a passphrase. |
| `loti key rotate` | Generate a new key and publish a signed key-rotation clock event linking old→new identity. |

**Requires:** key-based identity, signing.

### Peers & overlay

Replaces the simulation's one-shot `NetworkConfigurator` with live peering and a routing table
that changes over time.

| Command | Purpose |
| --- | --- |
| `loti peer add <addr> [--alias <name>]` | Add/dial a neighbor (address + node id/pubkey). |
| `loti peer rm <node>` | Drop a neighbor. |
| `loti peer ls` | List neighbors: node id, alias, address, RTT, last clock event seen, up/down. |
| `loti peer ping <node>` | Liveness/latency check. |
| `loti route ls [--to <node>]` | Show the overlay routing table (next hop toward each destination). |
| `loti overlay export/import` | Snapshot / seed the neighbor + routing configuration (bootstrap). |

```console
$ loti peer add udp://198.51.100.7:4666 --alias court --id node:1a2b…
peer added: court (node:1a2b…)  handshaking…  connected
$ loti route ls --to node:1a2b…
node:1a2b…  →  next-hop court (node:1a2b…)  1 hop
```

**Requires:** dynamic topology, peer handshake/authentication.

### Events

| Command | Purpose |
| --- | --- |
| `loti publish [--file <path> \| -] [--sign] [--salt] [--wait]` | Create and store a new event with the given content, reference the latest local clock event, and (optionally) sign it. Prints the event hash. `--wait` blocks until a local clock event pins it. |
| `loti event ls [--since <t>] [--mine]` | List known events. |
| `loti event show <event>` | Show an event: creator, hash, size, referenced clock events, signature status, pinned-by clock event. |
| `loti event get <event> [--out <file>]` | Fetch event **content** (only if available locally or the creator shares it). |
| `loti event import <file>` | Import an externally supplied event (e.g. one a web server published) so it can be discovered/verified. |

```console
$ echo "patent draft v3" | loti publish --sign --salt --wait
event:2c7f…9a   pinned by clock:8b21…  (t≈2026-07-17T10:31:05Z, local)
```

**Requires:** persistence; content storage; optional event sharing/import.

### Clock events

Normally automatic (~1/s), but exposed for inspection and control.

| Command | Purpose |
| --- | --- |
| `loti clock ls [--since <t>] [--limit N]` | List the local clock-event chain with timestamps. |
| `loti clock show <clock>` | Show a clock event: timestamp, referenced events (previous local, neighbor tips, pinned events), reverse cross-links, signature. |
| `loti clock tick` | Force-create a clock event now (in addition to the periodic schedule). |
| `loti clock config [--interval <dur>]` | Show/set the clock-event interval. |

**Requires:** persistence.

### Discovery (query) operations

The three read operations from [implementation.md](implementation.md). By default the
**reference clock is your own node** (`@self`), matching the simulation. A production node can
target a **reference node** — e.g. a trusted notary — with `--reference <node>`, asking that
node to anchor the chain in *its* clock events, which is what makes a proof meaningful to a
third party who trusts that node's clock.

| Command | Purpose |
| --- | --- |
| `loti chain <event> [--reference <node>] [--timeout <dur>]` | Run an event-chain discovery; print the enclosing chain (`lowerBound · event · upperBound`). |
| `loti bounds <event> [--reference <node>]` | Find the time bounds: `(lower, upper)` timestamps and the interval width, in the reference node's local clock. |
| `loti order <event1> <event2> [--reference <node>]` | Determine order: `before` (-1), `after` (+1), or `undetermined` (0). |
| `loti discovery ls` / `watch` | List in-flight/recent discoveries; stream state changes. |

```console
$ loti bounds event:2c7f…9a --reference court
event    event:2c7f…9a
lower    2026-07-17T10:31:02Z   (clock:7a10…, node court)
upper    2026-07-17T10:31:39Z   (clock:7a2f…, node court)
width    37s
according-to  court (node:1a2b…)

$ loti order event:2c7f…9a event:44de…10 --reference court
before   (event:2c7f…9a was created before event:44de…10, according to court)
```

**Requires:** the discovery engine (already in the simulation) plus reference-node targeting
and signed results.

### Proofs — the core product feature

A **proof** is a self-contained, portable artifact that a third party can verify **offline**.
It captures the discovered event chain, the reference node's public key, and the signatures on
every clock event, so `loti verify` (or any independent implementation) can confirm the chain
is intact and attributable — no network, no daemon, no trust beyond the reference node's clock.

| Command | Purpose |
| --- | --- |
| `loti prove bounds <event> [--reference <node>] --out <file>` | Run a bounds discovery and serialize a bounds proof. |
| `loti prove order <event1> <event2> [--reference <node>] --out <file>` | Serialize an order proof (two chains + comparison). |
| `loti proof show <file>` | Human-readable summary of a proof file (no verification). |
| `loti verify <file> [--trust <node>…] [--json]` | **Offline** verification. Checks: every clock event's hash recomputes; the chain is linked (each element references its predecessor); the event is included; the endpoints are the reference node's clock events; and (if signed) the signatures are valid under the embedded public key. Exit `0` if valid, `6` if not. Prints the proven bounds/order and *which* node's clock they are relative to, so the verifier can decide whether to trust that clock. |

```console
$ loti prove bounds event:2c7f…9a --reference court --out draft.loti
wrote draft.loti  (bounds proof, chain length 34, signed by court)

# on a completely different machine, no LOTI node, no network:
$ loti verify draft.loti
VALID   event:2c7f…9a  created 2026-07-17T10:31:02Z .. 10:31:39Z (width 37s)
        according to the local clock of court (node:1a2b…), signature OK
```

**Requires:** proof serialization + signing (see [Proof format](#proof-format)).

### Storage & maintenance

| Command | Purpose |
| --- | --- |
| `loti db stat` | Store size, counts (events, clock events, cross-links, discoveries), growth rate. |
| `loti db gc [--keep <dur>]` | Prune expendable data. **Local events and the local clock chain must be retained** (they are the node's only record and back all future proofs); learned neighbor cross-links and expired discovery caches can be trimmed. |
| `loti db verify` | Re-hash and re-check the integrity of the local store. |
| `loti db backup --out <file>` / `db restore <file>` | Cold backup / restore of the entire node state. |
| `loti db export/import` | Interchange subsets (e.g. share a slice of the clock DAG). |

**Requires:** persistence, a defined retention policy.

### Statistics & monitoring

| Command | Purpose |
| --- | --- |
| `loti stats` | The metrics the simulation already collects: clock/event counts, per-type discovery started/aborted/completed counts, chain length/interval, discovery latency, file-length growth. |
| `loti metrics [--prometheus]` | Export metrics for scraping. |
| `loti log [--follow] [--level …]` | Tail the daemon log. |

These map directly onto the signals and result filters in
[implementation.md](implementation.md#statistics).

## Proof format

A proof is a versioned, self-describing document (canonical JSON shown; a compact binary
encoding for single-datagram transport is the wire form). It contains everything
`loti verify` needs offline:

```jsonc
{
  "loti_proof": 1,
  "kind": "bounds",                     // "bounds" | "order" | "chain"
  "reference": {                        // the node whose local clock the bounds are in
    "node": "node:1a2b…",
    "pubkey": "ed25519:…",
    "alias": "court"
  },
  "event": { "creator": "node:…", "hash": "2c7f…9a", "salt": "…",
             "referenced": [ … ], "signature": "…" },
  "lowerBound": [ /* reference node's clock events, oldest→event */
    { "creator":"node:1a2b…","hash":"7a10…","timestamp":"…","salt":"…",
      "referenced":[…], "signature":"…" }, … ],
  "upperBound": [ /* reference node's clock events, event→newest */ … ],
  "result": { "lower":"2026-07-17T10:31:02Z", "upper":"2026-07-17T10:31:39Z" }
  // for "order": two chains + { "order": -1 } instead of a single result
}
```

Verification (offline) checks, in order:

1. every clock event's stored `hash` equals its recomputed SHA-256;
2. the concatenation `lowerBound · event · upperBound` is a hash chain (each element after the
   first references its immediate predecessor; the event references the last lower-bound clock
   event);
3. `lowerBound.front` and `upperBound.back` are created by the `reference` node;
4. if present, every signature is valid under `reference.pubkey` (and the event's signature
   under its creator's key);
5. for `order`, the two chains' intervals are compared and the recorded `order` matches.

Steps 1–3 are exactly what the simulation's `validateEventChainDiscoveryResult` already does;
steps 4–5 and the portable serialization are what productization adds. **The proof's timestamps
are only as trustworthy as the reference node's clock** — verification proves *integrity and
attribution*, and the verifier supplies the *trust* by choosing which reference nodes it
accepts (`--trust`).

## End-to-end workflows

**1. Timestamp your own content.**

```sh
loti init && loti node start
loti peer add udp://bootstrap.example:4666 --id node:…   # join the network
echo "manuscript.pdf sha256 …" | loti publish --sign --salt --wait
```

**2. Get a notarized bounds proof and give it to someone who is not on the network.**

```sh
# You (participant): ask a trusted notary node to anchor the bounds in its clock.
loti prove bounds event:2c7f…9a --reference notary --out proof.loti
# Them (no LOTI install of their own, or an independent one): verify offline.
loti verify proof.loti --trust node:notary…      # exit 0 ⇒ trustworthy
```

**3. Prove which of two events came first.**

```sh
loti prove order event:2c7f…9a event:44de…10 --reference court --out order.loti
loti verify order.loti
```

## Configuration reference

`$LOTI_HOME/config.toml` (every key also settable via `loti config set` and `LOTI_*` env):

| Key | Default | Meaning |
| --- | --- | --- |
| `home` | `~/.loti` | State directory. |
| `identity.key` | `key.pem` | Signing key path. |
| `identity.sign_events` | `true` | Sign published events. |
| `network.listen` | `udp://:4666` | P2P transport bind address. |
| `network.control` | `control.sock` | Local RPC socket for `loti`. |
| `clock.interval` | `1s` | Clock-event creation interval. |
| `discovery.expiry` | `1s`→`30s` | Discovery timeout before abort (raise for real WANs). |
| `discovery.default_reference` | `@self` | Default reference node for discoveries. |
| `store.path` | `db/` | Persistent store location. |
| `store.retain` | `all` | Retention policy for local vs learned data. |
| `peers` | `[]` | Bootstrap neighbors. |

## Security considerations

- **Key custody.** The identity key signs every clock event; its compromise lets an attacker
  impersonate the node. `key export` must be passphrase-encrypted; consider hardware keys.
- **Control socket.** `loti` ↔ `lotid` RPC grants full node control; restrict socket
  permissions and require a token for any non-local `--rpc` endpoint.
- **Trust is explicit.** `verify` proves integrity, not honesty: a proof is only as good as the
  reference node's clock. Encourage `--trust` allow-lists and surface *which* node's clock a
  proof relies on.
- **Forging defenses** (postcomputing/precomputing, [theory.md](theory.md#preventing-attacks))
  depend on fresh clock-event hashes being embedded before content is fixed; the daemon must
  never back-date clock events, and `publish` must reference a *current* clock tip.
- **DoS / query abuse.** Answering discoveries costs CPU and bandwidth; rate-limit per peer.

## Open questions

- How does a verifier discover and trust reputable reference nodes (a notary directory, a web
  of trust)?
- Should proofs be **multi-reference** (bounds according to several independent nodes at once)
  for stronger legal standing?
- What is the exact retention policy that keeps proofs reconstructible years later without
  unbounded storage growth (~3–30 GB/year of clock events per the paper)?
- Transport: stay on UDP (single-datagram chains) or add a reliable/streamed transport for
  large chains and event sharing?
