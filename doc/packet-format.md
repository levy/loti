# Packet Format

This document specifies the **wire format of every packet LOTI nodes send to each other**.
It is the reference for the three application-level messages exchanged between neighboring
`Daemon` modules, their field-by-field byte layout, and the flows in which they travel.

The message classes are declared in [`Packet.msg`](../src/Packet.msg) (wire messages),
[`Data.msg`](../src/Data.msg) (nested payload structures), and [`Type.msg`](../src/Type.msg)
(primitive typedefs). Packet construction, sending and dispatch live in
[`Daemon.cc`](../src/Daemon.cc); the byte sizes below are computed by the `calculate*Size`
helpers in [`Data.cc`](../src/Data.cc).

---

## Transport

- **Protocol:** UDP over the INET IPv4 / Ethernet stack (the `Daemon` is an INET application
  module, `app[0]` on each host).
- **Port:** `666`, on both ends. Each `Daemon` binds a `UdpSocket` to port 666 at init and
  sends every packet to `neighbor.getAddress()` on port 666 (`sendToNeighbor`,
  [`Daemon.cc`](../src/Daemon.cc)).
- **Peers:** packets are only ever sent to **directly-connected neighbors** (the overlay built
  by [`NetworkConfigurator`](../src/NetworkConfigurator.cc)). There is no direct end-to-end
  addressing; multi-hop discovery is achieved by neighbor-to-neighbor forwarding (see
  [Message flows](#message-flows)).

### Packet structure

Every LOTI packet is an INET `Packet` composed of exactly **two chunks**, in order:

```
+------------------------+------------------------------+
|  LotiHeader (10 bytes) |  body chunk (one of three)   |
+------------------------+------------------------------+
```

The sender builds the packet by `insertAtBack(header)` then `insertAtBack(body)`. The receiver
(`socketDataArrived`) does `popAtFront<LotiHeader>()`, reads `header.type`, and pops the
matching body chunk. All chunks extend `inet::FieldsChunk`; their `chunkLength` is set
explicitly from the `calculate*Size` helpers rather than inferred from C++ `sizeof`.

> **Sizes below are the LOTI application payload only.** The UDP header (8 B), IPv4 header
> (20 B) and Ethernet framing are added by the INET stack underneath and are not counted here.

---

## Primitive types and sizes

Defined in [`Type.msg`](../src/Type.msg); wire sizes are hard-coded in the `calculate*Size`
helpers in [`Data.cc`](../src/Data.cc).

| Type | C++ underlying | Wire size | Notes |
| --- | --- | --- | --- |
| `NodeId` | `uint64_t` | 8 B | Node identity. |
| `Salt` | `uint64_t` | 8 B | Random salt mixed into a hash. |
| `EventHash` | `vector<uint8_t>` | 32 B | A SHA-256 digest (see [`picosha.h`](../src/picosha.h)). |
| `simtime_t` | INET simulation time | 8 B | Timestamps. |
| `LotiType` | enum | 2 B | Packet discriminator (see below). |
| `ByteVector` | `vector<uint8_t>` | **0 B** | Event payload — deliberately **excluded** from the wire size. |

`LotiType` values ([`Type.msg`](../src/Type.msg)):

| Name | Value | Body chunk |
| --- | --- | --- |
| `LT_CLOCK_EVENT_NOTIFICATION` | `0` | `ClockEventNotification` |
| `LT_EVENT_CHAIN_DISCOVERY_REQUEST` | `1` | `EventChainDiscoveryRequest` |
| `LT_EVENT_CHAIN_DISCOVERY_RESPONSE` | `2` | `EventChainDiscoveryResponse` |

---

## `LotiHeader` — common prefix

Prefixes **every** packet. Declared in [`Packet.msg`](../src/Packet.msg).

| Field | Type | Size | Meaning |
| --- | --- | --- | --- |
| `type` | `LotiType` | 2 B | Which body chunk follows; selects the dispatch branch. |
| `neighbor` | `NodeId` | 8 B | The **sender's** own `NodeId`. The receiver uses it to look up the sending neighbor in its `neighbors` map; a packet from an unknown `neighbor` is dropped with a warning. |

```
LotiHeader (10 bytes)
 0                   1
 0 1 2 3 4 5 6 7 8 9
+---+---------------+
|typ|   neighbor    |
+---+---------------+
 2 B      8 B
```

> **Naming caveat:** despite the field name `neighbor`, this carries the **sender's** id, not
> the recipient's — `header->setNeighbor(nodeId)` at every send site.

**Total: 10 bytes** (fixed).

---

## Packet 1 — `ClockEventNotification`

**Type:** `LT_CLOCK_EVENT_NOTIFICATION` · **When:** on every locally-created clock event, the
daemon sends one to **each** neighbor (`processCreateClockEventTimer` → `sendClockEventNotification`).
This is the continuous background gossip that grows the distributed clock-event DAG. Only
hashes travel — never the clock event's full contents.

| Field | Type | Size | Meaning |
| --- | --- | --- | --- |
| `lastClockEventHash` | `EventHash` | 32 B | Hash of the sender's newest local clock event. |
| `neighborLastClockEventHash` | `EventHash` | 32 B | Hash of the newest clock event the sender has heard **from this neighbor** — the back-reference that lets the receiver link the two DAGs. |

```
Packet: ClockEventNotification
+------------------+----------------------+----------------------------+
| LotiHeader 10 B  | lastClockEventHash   | neighborLastClockEventHash |
| type=0           |        32 B          |            32 B            |
+------------------+----------------------+----------------------------+
```

**Body: 64 bytes · Total packet: 74 bytes** (fixed).

On receipt (`processClockEventNotification`): the receiver records the neighbor's newest hash,
and — if it recognizes `neighborLastClockEventHash` as one of its own clock events — adds a
back-reference (a "referencing event") so the two nodes' DAGs become hash-linked.

---

## Packet 2 — `EventChainDiscoveryRequest`

**Type:** `LT_EVENT_CHAIN_DISCOVERY_REQUEST` · **When:** an originator starts an event-chain
discovery for an event it did **not** create, or an intermediate node forwards such a request
one hop closer to the event's creator (`sendEventChainDiscoveryRequest`). Routed toward the
creator using the next-hop overlay (`findNextHopNeighbor(event.creator)`).

| Field | Type | Size | Meaning |
| --- | --- | --- | --- |
| `originator` | `NodeId` | 8 B | The node that started the discovery; the response must be routed back to it. Preserved unchanged across every hop. |
| `event` | `EventReference` | 40 B | The target event, as `{creator: NodeId (8 B), hash: EventHash (32 B)}`. Identifies both what to find and (via `creator`) where to route. |

```
Packet: EventChainDiscoveryRequest
+------------------+-------------+---------------------------------+
| LotiHeader 10 B  | originator  |  event: EventReference          |
| type=1           |    8 B      |  creator 8 B | hash 32 B        |
+------------------+-------------+---------------------------------+
                                 \------------ 40 B ---------------/
```

**Body: 48 bytes · Total packet: 58 bytes** (fixed).

Routing (`processEventChainDiscoveryRequest`):
- If `event.creator == this node` → this node is the creator: it builds the enclosing
  `EventChain` locally and replies with an `EventChainDiscoveryResponse`.
- Otherwise → forward the same request to the next hop toward `event.creator`.

Only the **originator** keeps a discovery record; intermediate and creator nodes handle the
request statelessly.

---

## Packet 3 — `EventChainDiscoveryResponse`

**Type:** `LT_EVENT_CHAIN_DISCOVERY_RESPONSE` · **When:** the creator builds the initial
`EventChain` and replies; each intermediate node, on the way back, **extends** the chain with
its own clock-event bounds before forwarding it one hop closer to the originator
(`sendEventChainDiscoveryResponse`). Routed back with `findNextHopNeighbor(originator)`.

| Field | Type | Size | Meaning |
| --- | --- | --- | --- |
| `originator` | `NodeId` | 8 B | Copied from the request; the destination this response is being routed toward. |
| `chain` | `EventChain` | variable | The accreting proof: `lowerBound · event · upperBound`. Grows at each hop. |

```
Packet: EventChainDiscoveryResponse
+------------------+-------------+----------------------------------------+
| LotiHeader 10 B  | originator  |  chain: EventChain (variable)          |
| type=2           |    8 B      |  lowerBound[] · event · upperBound[]   |
+------------------+-------------+----------------------------------------+
```

**Body: 8 + `sizeof(EventChain)` bytes · Total packet: 18 + `sizeof(EventChain)` bytes** (variable).

This is the only variable-length packet, and typically the largest. Its size is dominated by
the number of clock events accreted into the chain — one per hop on each side, each further
inflated by its referenced-event list. See the empirical UDP-length charts
([`UdpPacketLength1.png`](UdpPacketLength1.png), [`UdpPacketLength2.png`](UdpPacketLength2.png)).

---

## Nested payload structures

These appear inside the packets above. Declared in [`Data.msg`](../src/Data.msg); sizes from
`calculateEventReferenceSize`/`calculateClockEventSize`/`calculateEventSize`/`calculateEventChainSize`
in [`Data.cc`](../src/Data.cc).

### `EventReference` — 40 B (fixed)

A compact pointer to any event or clock event.

| Field | Type | Size |
| --- | --- | --- |
| `creator` | `NodeId` | 8 B |
| `hash` | `EventHash` | 32 B |

### `ClockEvent` — 56 B + 40 B × *R* (variable)

A timestamped node in the clock DAG. Carried (many at a time) inside `EventChain`.

| Field | Type | Size |
| --- | --- | --- |
| `creator` | `NodeId` | 8 B |
| `hash` | `EventHash` | 32 B |
| `timestamp` | `simtime_t` | 8 B |
| `salt` | `Salt` | 8 B |
| `referencedEvents` | `EventReferenceVector` | 40 B × *R* (*R* = number of referenced events) |

### `Event` — 48 B + 40 B × *R* (variable)

The target content event at the middle of a chain.

| Field | Type | Size |
| --- | --- | --- |
| `creator` | `NodeId` | 8 B |
| `hash` | `EventHash` | 32 B |
| `data` | `ByteVector` | **0 B — excluded from the wire size** (`// ByteVector data: not included` in [`Data.cc`](../src/Data.cc)) |
| `salt` | `Salt` | 8 B |
| `referencedEvents` | `EventReferenceVector` | 40 B × *R* |

> Only the event's **hash** commits to its `data`; the raw bytes are never sized into the
> chain, so the proof stays compact regardless of payload size.

### `EventChain` — variable

The proof structure: a hash-linked `lowerBound · event · upperBound`.

| Part | Type | Size contribution |
| --- | --- | --- |
| `lowerBound` | `ClockEventDeque` | Σ `ClockEvent` sizes |
| `event` | `Event` | one `Event` size |
| `upperBound` | `ClockEventDeque` | Σ `ClockEvent` sizes |

```
sizeof(EventChain) = Σ_lowerBound sizeof(ClockEvent)
                   + sizeof(Event)
                   + Σ_upperBound sizeof(ClockEvent)
```

---

## Size summary

| Packet | Body size | Total (incl. 10 B header) | Fixed? |
| --- | --- | --- | --- |
| `ClockEventNotification` | 64 B | **74 B** | ✔ fixed |
| `EventChainDiscoveryRequest` | 48 B | **58 B** | ✔ fixed |
| `EventChainDiscoveryResponse` | 8 B + `EventChain` | **18 B + `EventChain`** | ✘ variable |

(UDP + IPv4 + Ethernet overhead from the INET stack is additional and not included.)

---

## Message flows

### A. Clock-event gossip (continuous)

Runs on every clock-event tick, independent of any query — this is what keeps the DAGs linked.

```
Daemon A                              Daemon B (neighbor)
   |  creates local clock event           |
   |  ClockEventNotification              |
   |   { lastClockEventHash,              |
   |     neighborLastClockEventHash }     |
   |------------------------------------->|  processClockEventNotification:
   |                                      |   record A's newest hash;
   |                                      |   link back-reference into DAG
   |                (and symmetrically B -> A on B's own ticks)
```

### B. Event-chain discovery (request out, response back)

Started by a node (the *originator*) that wants the proof chain for an event created by some
other node. The request is forwarded hop-by-hop toward the creator; the response is forwarded
back, accreting each hop's clock-event bounds.

```
Originator O        Intermediate I         Creator C
   |  Request{originator=O, event}        |
   |------------->|                        |     (routed toward event.creator)
   |              |  Request{originator=O} |
   |              |----------------------->|  builds EventChain locally
   |              |                        |  (lowerBound · event · upperBound)
   |              |  Response{originator=O,|
   |              |    chain}              |
   |              |<-----------------------|
   |              | extends chain with     |
   |              | its own bounds         |
   |  Response{originator=O, chain}        |
   |<-------------|                        |
   |  adds local lower/upper bounds,       |
   |  completes discovery + validates      |
```

Notes:
- The request carries only an `EventReference` (fixed 58 B); the response carries the full,
  growing `EventChain` (variable).
- If the originator itself created the event, the whole chain is built locally with **no
  packets** at all.
- `EventBoundsDiscovery` and `EventOrderDiscovery` issue **no new packet types** — they are
  built on top of one (or two) event-chain discoveries and read the resulting chain's
  endpoints. See [implementation.md](implementation.md).

---

## See also

- [implementation.md](implementation.md) — the protocol engine, discovery algorithms, and
  validation around these packets.
- [theory.md](theory.md) — why clock events, chains, and bounds prove partial order.
- [`Packet.msg`](../src/Packet.msg), [`Data.msg`](../src/Data.msg), [`Type.msg`](../src/Type.msg)
  — the authoritative declarations.
- [`Daemon.cc`](../src/Daemon.cc) (`send*` / `process*` / `socketDataArrived`) and
  [`Data.cc`](../src/Data.cc) (`calculate*Size`) — construction, dispatch and sizing.
