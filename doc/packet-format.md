# Packet Format

This document specifies the **wire format of every datagram a LOTI node sends to another
node** — the on-the-wire protocol spoken between running `lotid` daemons. It is a self-contained
reference: no knowledge of the simulator (or of OMNeT++) is required or assumed.

A node sends exactly **three kinds of datagram**. Each is a flat, length-prefixed byte string
built by the codec in [`src/core/wire/`](../src/core/wire/): the message grammar is in
[`packets.hpp`](../src/core/wire/packets.hpp) / [`packets.cpp`](../src/core/wire/packets.cpp), and the
primitive read/write rules in [`codec.hpp`](../src/core/wire/codec.hpp). The field types are the
plain domain structs in [`src/core/domain/types.hpp`](../src/core/domain/types.hpp).

> **One serializer, both runtimes.** These exact bytes are produced by `loti-core`. The
> deployable daemon (`lotid`) puts them on a real UDP socket; the research simulator wraps the
> identical bytes so its bandwidth statistics predict production. This document describes those
> bytes — i.e. the real protocol.

---

## Transport

- **Datagram protocol:** UDP over IPv4 (`AF_INET`, `SOCK_DGRAM`). Each LOTI message is carried
  in **exactly one UDP datagram**; a datagram is self-delimiting (all variable fields are
  length-prefixed), so there is no framing, no length header, and no message spans two datagrams.
- **Port:** chosen per node at launch (`lotid --port <p>`); there is no protocol-mandated port.
  A peer is addressed as `id:ip:port` and the node keeps a `NodeId → (IP, port)` table for its
  neighbors.
- **Addressing:** a node only ever sends to **directly-configured neighbors**. Every datagram
  carries the **sender's** `NodeId` in its header (see below); the receiver uses it to identify
  which neighbor sent the packet. Multi-hop discovery is achieved by neighbor-to-neighbor
  forwarding, not end-to-end addressing. An outbound datagram to an unknown next hop is silently
  dropped.
- **Maximum size:** the receive buffer is 65535 bytes (one UDP datagram). Chain responses are
  the only messages that can approach this; the protocol is designed so a chain fits in a single
  datagram.

---

## Encoding conventions

Every field is one of a small set of primitives, concatenated with **no padding or alignment**.
All multi-byte integers are **big-endian (network byte order)**.

| Primitive | Layout | Notes |
| --- | --- | --- |
| `u8` | 1 byte | Used for the message type tag. |
| `u32` | 4 bytes, big-endian | Used only as a length / count prefix. |
| `u64` | 8 bytes, big-endian | All identifiers, salts, timestamps. |
| `blob` | `u32 length` then *length* raw bytes | Any variable-length byte string (hashes, content, signatures). A zero-length blob is 4 bytes (`length = 0`). |
| `array<T>` | `u32 count` then *count* × `T` | Repeated elements, each encoded in turn. |

The decoder ([`Reader`](../src/core/wire/codec.hpp)) validates every length against the remaining
buffer and throws `wire: truncated datagram` on underrun, so a short or malformed datagram is
rejected rather than misread.

### Domain field encodings

How each logical field type maps onto the primitives above:

| Field type | Wire encoding | Typical size | Meaning |
| --- | --- | --- | --- |
| `NodeId` | `u64` | 8 B | A node's identity. |
| `Salt` | `u64` | 8 B | Random salt folded into a hash. |
| `Timestamp` | `u64` | 8 B | A raw clock tick (nanoseconds in production), signed 64-bit reinterpreted as `u64`. |
| `EventHash` | `blob` | 36 B | A SHA-256 digest → `4 (length) + 32`. |
| content `data` | `blob` | 4 + *n* B | Arbitrary event payload bytes. |
| `Signature` | `blob` | 4 B unsigned · 68 B signed | Empty (`length = 0`) when the node is unsigned; an Ed25519 signature → `4 + 64` when signed. |

> **Timestamp is a plain 64-bit integer clock tick** — nanoseconds since the epoch on a real
> node. It is *not* a floating-point or platform-specific time type; it is written with the same
> big-endian `u64` rule as every other integer.

---

## Datagram header

**Every** datagram begins with the same 9-byte header (`write_header` in
[`packets.cpp`](../src/core/wire/packets.cpp)):

| Offset | Size | Field | Encoding | Meaning |
| --- | --- | --- | --- | --- |
| 0 | 1 B | `type` | `u8` | Message type: `0` = clock notification, `1` = chain request, `2` = chain response. Selects how the rest of the datagram is parsed. |
| 1 | 8 B | `sender` | `u64` | The **sender's** own `NodeId`. Lets the receiver identify the neighbor; the payload that follows never repeats it. |

```
 0        1                                   9
 +--------+-----------------------------------+
 |  type  |              sender               |
 |  u8    |               u64                 |
 +--------+-----------------------------------+
    1 B                  8 B
```

**Header size: 9 bytes** (fixed, on every datagram).

The three `type` values and their payloads:

| `type` | Name | Payload | Direction |
| --- | --- | --- | --- |
| `0` | Clock notification | `ClockNotification` | broadcast to every neighbor |
| `1` | Chain request | `ChainRequest` | forwarded toward the event's creator |
| `2` | Chain response | `ChainResponse` | forwarded back toward the originator |

---

## Datagram 1 — Clock notification (`type = 0`)

**When:** a node emits one to **each** neighbor every time it creates a new local clock event,
**on any of its clock chains**. This is the continuous background gossip that hash-links every
node's clock DAG. A node runs several independent, geometrically-spaced clock chains (see
[implementation.md](implementation.md#multi-resolution-clock-chains)), so the notification
carries a `chain` id alongside the two hashes — never the clock event's contents.

Payload after the 9-byte header:

| Field | Encoding | Meaning |
| --- | --- | --- |
| `chain` | `u64` | Which of the sender's clock chains this notification is about (0 = fastest). Advertised change-only: a chain's tip is (re-)sent only when it ticks. |
| `last_clock_event_hash` | `blob` (EventHash) | Hash of the sender's newest local clock event on that chain. |
| `neighbor_last_clock_event_hash` | `blob` (EventHash) | Hash of the newest clock event the sender has heard **from this neighbor on that chain** — the back-reference that stitches the two DAGs together. |

```
 +----------- header 9 B -----------+---- u64 ----+------ blob ------+------ blob ------+
 | type=0 |        sender u64        |    chain    | last_clock_hash  | neighbor_last…   |
 +--------+-------------------------+-------------+------------------+------------------+
                                        8 B         \--- 4 + 32 B ---/ \--- 4 + 32 B ---/
```

**Size (SHA-256 hashes): 9 + 8 + 36 + 36 = 89 bytes** (fixed).

On receipt, the node records the neighbor's newest hash for that chain, and, if it recognizes
`neighbor_last_clock_event_hash` as one of its own clock events, records the reverse cross-link.

---

## Datagram 2 — Chain request (`type = 1`)

**When:** a node (the *originator*) starts an event-chain discovery for an event it did **not**
create, or an intermediate node forwards such a request one hop closer to the event's creator.
Routed toward the creator via each node's next-hop table.

Payload after the 9-byte header:

| Field | Encoding | Meaning |
| --- | --- | --- |
| `originator` | `u64` (NodeId) | The node that started the discovery; the response is routed back to it. Unchanged across every hop. |
| `event.creator` | `u64` (NodeId) | Creator of the target event — the routing destination. |
| `event.hash` | `blob` (EventHash) | Hash of the target event. |

(The last two fields together are an `EventReference`, inlined directly into the datagram.)

```
 +----------- header 9 B -----------+---- u64 ----+---- u64 ----+--- blob ---+
 | type=1 |        sender u64        | originator  | evt.creator | evt.hash   |
 +--------+-------------------------+-------------+-------------+------------+
                                       8 B           8 B          4 + 32 B
```

**Size (SHA-256 hash): 9 + 8 + 8 + 36 = 61 bytes** (fixed).

Routing: if `event.creator` is this node, it builds the enclosing chain locally and replies with
a chain response; otherwise it forwards the same request to the next hop toward `event.creator`.

---

## Datagram 3 — Chain response (`type = 2`)

**When:** the creator builds the initial `EventChain` and replies; each intermediate node, on
the way back, **extends** the chain with its own clock-event bounds before forwarding it one hop
closer to the originator.

Payload after the 9-byte header:

| Field | Encoding | Meaning |
| --- | --- | --- |
| `originator` | `u64` (NodeId) | Copied from the request; the destination this response is routed toward. |
| `chain` | `EventChain` (see below) | The accreting proof: the target event plus a lower- and upper-bound run of clock events. Grows at each hop. |

```
 +----------- header 9 B -----------+---- u64 ----+---- EventChain (variable) ----+
 | type=2 |        sender u64        | originator  | event · lower[] · upper[]     |
 +--------+-------------------------+-------------+-------------------------------+
```

**Size: 9 + 8 + `sizeof(EventChain)` bytes** (variable — the only variable-length datagram, and
the largest). Its size is dominated by the number of clock events accreted into the chain: one
per hop on each side, each further inflated by its referenced-event list and (if present) its
signature.

---

## Composite structures (inside a chain response)

These structures appear only within `EventChain`. Field order below is the exact byte order the
codec writes ([`codec.hpp`](../src/core/wire/codec.hpp)).

### `EventReference` — 44 B (SHA-256, fixed)

A typed pointer to an event or clock event.

| Field | Encoding | Size |
| --- | --- | --- |
| `creator` | `u64` | 8 B |
| `hash` | `blob` (EventHash) | 4 + 32 B |

### `Event` — 64 + *d* + 44·*r* B (unsigned, SHA-256)

The target content event at the center of a chain. *d* = content length, *r* = referenced-event
count.

| Field | Encoding | Size |
| --- | --- | --- |
| `creator` | `u64` | 8 B |
| `hash` | `blob` (EventHash) | 4 + 32 B |
| `data` | `blob` | 4 + *d* B |
| `salt` | `u64` | 8 B |
| `referenced_events` | `array<EventReference>` | 4 + 44·*r* B |
| `signature` | `blob` (Signature) | 4 B unsigned · 68 B signed |

> **The event content `data` is on the wire.** A chain response transmits the target event's
> full payload, not just its hash. (Note: the separate *byte-accounting* helper used for
> bandwidth statistics, `event_size_bytes` in [`src/core/hash/hashing.hpp`](../src/core/hash/hashing.hpp),
> deliberately **excludes** `data` — that is a modeling choice for size statistics, not a claim
> about the encoded bytes. The actual datagram includes it.)

### `ClockEvent` — 76 + 44·*r* B (unsigned, SHA-256)

A timestamped node of the clock DAG; a chain carries many of them. *r* = referenced-event count.
`chain` — which of the node's multi-resolution clock chains this event belongs to (0 = fastest)
— is part of the hashed content, so a clock event cannot lie about its chain.

| Field | Encoding | Size |
| --- | --- | --- |
| `creator` | `u64` | 8 B |
| `hash` | `blob` (EventHash) | 4 + 32 B |
| `chain` | `u64` | 8 B |
| `timestamp` | `u64` | 8 B |
| `salt` | `u64` | 8 B |
| `referenced_events` | `array<EventReference>` | 4 + 44·*r* B |
| `signature` | `blob` (Signature) | 4 B unsigned · 68 B signed |

### `EventChain` — variable

The proof structure. **Note the wire order:** the target `event` is written **first**, then the
lower-bound run, then the upper-bound run (logically the chain reads `lower · event · upper`, but
the bytes lead with the event).

| Order | Field | Encoding |
| --- | --- | --- |
| 1 | `event` | one `Event` |
| 2 | `lower_bound` | `array<ClockEvent>` (`u32` count, then that many clock events) |
| 3 | `upper_bound` | `array<ClockEvent>` (`u32` count, then that many clock events) |

```
sizeof(EventChain) = sizeof(Event)
                   + 4 + Σ_lower sizeof(ClockEvent)
                   + 4 + Σ_upper sizeof(ClockEvent)
```

---

## Size summary

Sizes assume SHA-256 (32-byte) hashes and **unsigned** events (4-byte empty signatures). Signing
adds 64 bytes per signed `Event`/`ClockEvent`.

| Datagram | `type` | Size | Fixed? |
| --- | --- | --- | --- |
| Clock notification | 0 | 89 B | ✔ fixed |
| Chain request | 1 | 61 B | ✔ fixed |
| Chain response | 2 | 17 B + `EventChain` | ✘ variable |

(UDP + IPv4 headers are added by the OS network stack and are not part of these figures.)

---

## What the hashes commit to

The `hash` in every event/clock event is a SHA-256 digest, and it is what the whole scheme
hangs on. Importantly, the hash is computed over a **fixed canonical layout that is separate from
the transport encoding above** (defined in [`src/core/hash/hashing.hpp`](../src/core/hash/hashing.hpp)) —
so a node can recompute and verify any hash it receives:

- **Event hash** = `SHA-256( data ‖ salt(u64 BE) ‖ for each referenced event: creator(u64 BE) ‖ hash bytes )`.
- **Clock-event hash** = `SHA-256( chain(u64 BE) ‖ timestamp(u64 BE) ‖ salt(u64 BE) ‖ for each referenced event: creator(u64 BE) ‖ hash bytes )`.

A node's own `creator`/`hash` fields and its `signature` are **not** part of the preimage — the
signature signs the hash, so signing never changes a hash. This is why the `data` bytes must
travel in the chain response: without them the originator could not recompute the event's hash to
validate the proof.

---

## Message flows

### A. Clock-event gossip (continuous)

Runs on every clock-event tick, independent of any query — this is what keeps the DAGs linked.

```
Node A                                    Node B (neighbor)
   |  creates a local clock event on chain ℓ  |
   |  clock notification (type 0)             |
   |   { chain,                               |
   |     last_clock_event_hash,               |
   |     neighbor_last_clock_event_hash }     |
   |----------------------------------------->|  record A's newest hash for chain ℓ;
   |                                          |  add reverse cross-link into DAG
   |                   (and symmetrically B -> A on B's own ticks, per chain)
```

### B. Event-chain discovery (request out, response back)

Started by a node (the *originator*) that wants the proof chain for an event created by another
node. The request is forwarded hop-by-hop toward the creator; the response is forwarded back,
accreting each hop's clock-event bounds.

```
Originator O          Intermediate I           Creator C
   |  request(type 1){originator=O, event}     |
   |------------->|                             |   (routed toward event.creator)
   |              |  request(type 1)            |
   |              |---------------------------->|  builds EventChain locally
   |              |                             |  (event · lower · upper)
   |              |  response(type 2){orig=O,   |
   |              |    chain}                   |
   |              |<----------------------------|
   |              | extends chain with          |
   |              | its own clock-event bounds  |
   |  response(type 2){originator=O, chain}     |
   |<-------------|                             |
   |  adds its own local bounds,                |
   |  completes discovery + validates the chain |
```

Notes:
- The request carries only an event reference (fixed 61 B); the response carries the full,
  growing chain (variable).
- If the originator itself created the event, the chain is built entirely locally with **no
  datagrams** at all.
- *Event bounds* and *event order* discoveries send **no new datagram types** — they are built on
  top of one (or two) chain discoveries and read the resulting chain's endpoint timestamps.

---

## See also

- [`src/core/wire/packets.hpp`](../src/core/wire/packets.hpp) · [`packets.cpp`](../src/core/wire/packets.cpp)
  — the three datagram definitions and their encode/decode.
- [`src/core/wire/codec.hpp`](../src/core/wire/codec.hpp) — the primitive `Writer`/`Reader` (big-endian
  ints, length-prefixed blobs/arrays).
- [`src/core/domain/types.hpp`](../src/core/domain/types.hpp) — the field types.
- [`src/core/hash/hashing.hpp`](../src/core/hash/hashing.hpp) — the canonical hashing layout and the
  byte-accounting helpers.
- [architecture.md](architecture.md) — how one core serializer feeds both the real daemon and
  the simulator.
- [implementation.md](implementation.md) — the protocol engine, the three discoveries, and chain
  validation around these datagrams.
