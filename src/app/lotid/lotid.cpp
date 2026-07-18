// lotid — the LOTI daemon: one protocol node running on the OS port adapters.
//
// It hosts a single loti::Node and wires it to the wall clock, the epoll reactor
// (scheduler), a real UDP socket (transport), a CSPRNG, a signer (an Ed25519
// keystore with --key, else a null signer), an incremental LMDB store (--store), and
// a logging telemetry sink, then runs the reactor.
//
// It exposes a local control socket (--control <path>) speaking the versioned line
// protocol in adapters/os/control.hpp; the `loti` CLI is the client. The same
// commands are also accepted on stdin (one per line) for quick scripting. Queries
// (bounds/chain/order) are asynchronous: the control connection is held until the
// discovery completes or aborts, then the reply is sent.
//
//   publish <text>            publish an event; reply carries its hash
//   event   <hexprefix|last>  show a known event (creator, content, references)
//   event-find <text>         list local events whose content contains <text>
//   events                    list known event hashes
//   bounds  <hexprefix|last>  discover time bounds  (async)
//   chain   <hexprefix|last>  discover an event chain (async)
//   order   <a> <b>           discover the order of two events (async)
//   prove   <kind> <e> [e2]   discover + serialize a portable proof (async)
//   peer-add <id:ip:port>     add a neighbor
//   peer-ls                   list neighbors
//   status                    node id, counts, mode
//   key                       node id + public key
//   save                      flush the store to disk now (if --store is set)
//   db-stat                   store status (paths, counts, sizes, retention)
//   db-backup <path>          write a snapshot copy to <path>
//   db-restore <path>         load a snapshot from <path> into the running node
//   quit                      stop the daemon
//
// Usage:
//   lotid --port <p> {--id <n> | --key <path>} [--peer <id>:<ip>:<port>]...
//         [--route <dst>:<nexthop>]... [--clock-interval <s>] [--expiry <s>]
//         [--store <path>] [--store-mapsize <GiB>] [--store-sync-interval <s>]
//         [--control <path>] [--verbose]

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "adapters/os/clock.hpp"
#include "adapters/os/control.hpp"
#include "adapters/os/keystore.hpp"
#include "adapters/os/lmdb_store.hpp"
#include "adapters/os/reactor.hpp"
#include "adapters/os/rng.hpp"
#include "adapters/os/signer.hpp"
#include "adapters/os/store.hpp"
#include "adapters/os/telemetry.hpp"
#include "adapters/os/transport.hpp"
#include "adapters/sim/in_memory_store.hpp"
#include "node.hpp"
#include "ports/store.hpp"
#include "proof/proof.hpp"
#include "wire/codec.hpp"

namespace {

using namespace loti;

std::vector<std::string> tokenize(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\r') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  parts.push_back(cur);
  return parts;
}

std::string hex(const domain::EventHash& h) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (unsigned char b : h) {
    s.push_back(d[b >> 4]);
    s.push_back(d[b & 0xF]);
  }
  return s;
}

std::string hex_id(domain::NodeId id) {
  char buf[19];
  std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(id));
  return buf;
}

std::optional<domain::EventHash> unhex(const std::string& s) {
  if (s.empty() || s.size() % 2) return std::nullopt;
  const auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  domain::EventHash b;
  b.reserve(s.size() / 2);
  for (std::size_t i = 0; i < s.size(); i += 2) {
    const int hi = nib(s[i]), lo = nib(s[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    b.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return b;
}

std::string hex_bytes(const domain::Bytes& b) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(b.size() * 2);
  for (unsigned char c : b) {
    s.push_back(d[c >> 4]);
    s.push_back(d[c & 0xF]);
  }
  return s;
}

const char* kind_name(proof::Kind k) {
  switch (k) {
    case proof::Kind::bounds: return "bounds";
    case proof::Kind::order:  return "order";
    case proof::Kind::chain:  return "chain";
  }
  return "unknown";
}

// The order two enclosing chains imply — the same interval comparison the core's
// order discovery uses (Node::compare_event_chains). Kept in sync deliberately.
domain::Order compare_chains(const domain::EventChain& a, const domain::EventChain& b) {
  if (a.upper_bound.back().timestamp < b.lower_bound.front().timestamp) return domain::Order::before;
  if (b.upper_bound.back().timestamp < a.lower_bound.front().timestamp) return domain::Order::after;
  return domain::Order::undetermined;
}

// Event content rendered safe for the single-line control protocol (framing or
// non-printable bytes → '.').
std::string printable(const domain::Bytes& b) {
  std::string s;
  s.reserve(b.size());
  for (unsigned char c : b) s.push_back((c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.');
  return s;
}

// One clock event of a chain path, as a compact "creator=… hash=… ts=…" line.
std::string clock_line(const domain::ClockEvent& ce) {
  return "creator=" + hex_id(ce.creator) + " hash=" + hex(ce.hash) + " ts=" +
         std::to_string(ce.timestamp);
}

// The complete enclosing chain as control fields: the reference node, then the
// ordered path lower → event → upper (one `lower`/`upper` line per clock event),
// the event (hash + decoded content), and the total clock-event count. Repeated
// lower/upper keys render as JSON arrays on the CLI side.
os::ControlFields chain_path_fields(const domain::EventChain& c) {
  os::ControlFields f;
  const domain::NodeId reference = c.lower_bound.empty() ? 0 : c.lower_bound.front().creator;
  f.emplace_back("reference", hex_id(reference));
  for (const auto& ce : c.lower_bound) f.emplace_back("lower", clock_line(ce));
  f.emplace_back("event", hex(c.event.hash));
  f.emplace_back("content", printable(c.event.data));
  for (const auto& ce : c.upper_bound) f.emplace_back("upper", clock_line(ce));
  f.emplace_back("clockEvents", std::to_string(c.lower_bound.size() + c.upper_bound.size()));
  return f;
}

// Exit codes (shared with the CLI; see cli.md).
constexpr int kOk = 0;
constexpr int kUsage = 2;
constexpr int kNotFound = 4;
constexpr int kInvalidResult = 6;

// A command result. `deferred` means the reply is sent later, by a discovery
// callback, over the control connection whose fd is remembered.
struct Reply {
  int code = kOk;
  bool deferred = false;
  std::string error;
  os::ControlFields fields;
};

class Lotid final : public ChainCallback, public BoundsCallback, public OrderCallback {
 public:
  Lotid(domain::NodeId id, std::uint16_t port, domain::Duration clock_ns,
        domain::Duration expiry_ns, bool verbose, std::string store_path, std::size_t store_map_size,
        domain::Duration store_sync_ns, const std::string& key_path, std::string control_path)
      : transport_(port),
        scheduler_(reactor_, clock_),
        telemetry_(verbose),
        store_sync_interval_ns_(store_sync_ns > 0 ? store_sync_ns : 0),
        control_path_(std::move(control_path)) {
    if (key_path.empty()) {
      signer_ = std::make_unique<os::NullSigner>();
      node_id_ = id;
    } else {
      auto keystore = std::make_unique<os::Ed25519KeyStore>();
      keystore->load_or_generate(key_path);
      node_id_ = keystore->node_id();
      signed_ = true;
      signer_ = std::move(keystore);
    }
    // Build the DAG store first — the Node reads and writes the DAG through it. With
    // --store it is LMDB (durable, page-cache-bounded RAM); without, an in-memory store
    // (state lives only for the run, like the simulation).
    if (store_path.empty()) {
      store_ = std::make_unique<sim::InMemoryStore>();
    } else {
      const auto policy = store_sync_interval_ns_ > 0 ? os::LmdbStore::SyncPolicy::lazy
                                                      : os::LmdbStore::SyncPolicy::safe;
      auto lmdb = std::make_unique<os::LmdbStore>(std::move(store_path), store_map_size, policy);
      lmdb_ = lmdb.get();
      store_ = std::move(lmdb);
    }
    // Single-chain schedule for now (behavior unchanged); the multi-resolution default
    // schedule is adopted once ring pruning lands (see plan multi-resolution-clock-chains).
    node_ = std::make_unique<Node>(
        node_id_, NodePorts{clock_, scheduler_, transport_, rng_, *signer_, telemetry_, *store_},
        NodeConfig{{ChainConfig{clock_ns, 0}}, expiry_ns});
  }

  domain::NodeId node_id() const { return node_id_; }

  // Add a neighbor (used by --peer flags and the peer-add command).
  void add_peer(domain::NodeId id, const std::string& ip, std::uint16_t port) {
    node_->add_neighbor(id);
    node_->learn_route(id, id);  // direct route to a neighbor
    transport_.set_peer(id, ip, port);
    peers_.emplace_back(id, ip, port);
  }
  void learn_route(domain::NodeId dst, domain::NodeId next_hop) { node_->learn_route(dst, next_hop); }

  // The Node hydrated its working set from the store in its constructor, so restart
  // recovery is already done; just report what a persistent store carried over.
  void restore_from_store() {
    if (!lmdb_) return;
    std::fprintf(stderr, "[lotid] loaded %zu events, %zu clock events from %s\n",
                 node_->event_count(), node_->clock_event_count(), lmdb_->path().c_str());
  }

  void run() {
    node_->start();
    schedule_store_sync();  // periodic flush when the store runs in lazy (MDB_NOSYNC) mode
    reactor_.add_reader(transport_.fd(), [this] {
      for (;;) {
        domain::Bytes datagram = transport_.receive();
        if (datagram.empty()) break;
        node_->on_packet_received(datagram);
      }
    });
    reactor_.add_reader(STDIN_FILENO, [this] { on_stdin_readable(); });
    setup_control();
    reactor_.run();
    if (!control_path_.empty()) ::unlink(control_path_.c_str());
    if (lmdb_) lmdb_->sync();  // flush on a clean shutdown (writes are already durable)
  }

  // In lazy sync mode the LMDB store skips per-commit fsyncs, so a repeating reactor timer
  // forces committed pages out every interval. Bounds the crash-loss window to one
  // interval; a no-op (never armed) in safe mode or with the in-memory store.
  void schedule_store_sync() {
    if (store_sync_interval_ns_ == 0 || !lmdb_) return;
    scheduler_.after(store_sync_interval_ns_, [this] {
      if (lmdb_) lmdb_->sync();
      schedule_store_sync();  // re-arm
    });
  }

  // ---- discovery callbacks: answer any control clients waiting on this event ----
  void on_chain_completed(const domain::Event& e, const domain::EventChain& c) override {
    if (pending_chain_.count(e.hash))
      answer(pending_chain_, e.hash, Reply{kOk, false, "", chain_path_fields(c)});
    finish_single_proof(e.hash, c);
    finish_order_proof(e.hash, c);
  }
  void on_chain_aborted(const domain::Event& e) override {
    abort_pending(pending_chain_, e.hash);
    abort_single_proof(e.hash);
    abort_order_proof(e.hash);
  }
  void on_bounds_completed(const domain::Event& e, domain::Timestamp lo, domain::Timestamp hi) override {
    answer(pending_bounds_, e.hash,
           Reply{kOk, false, "", {{"event", hex(e.hash)},
                                  {"lower", std::to_string(lo)},
                                  {"upper", std::to_string(hi)},
                                  {"interval", std::to_string(hi - lo)}}});
  }
  void on_bounds_aborted(const domain::Event& e) override { abort_pending(pending_bounds_, e.hash); }
  void on_order_completed(const domain::Event& a, const domain::Event& b, domain::Order o) override {
    const auto key = std::make_pair(a.hash, b.hash);
    auto it = pending_order_.find(key);
    if (it == pending_order_.end()) return;
    respond(it->second, Reply{kOk, false, "", {{"event1", hex(a.hash)}, {"event2", hex(b.hash)},
                                               {"order", std::to_string(static_cast<int>(o))}}});
    pending_order_.erase(it);
  }
  void on_order_aborted(const domain::Event& a, const domain::Event& b) override {
    const auto key = std::make_pair(a.hash, b.hash);
    auto it = pending_order_.find(key);
    if (it == pending_order_.end()) return;
    respond(it->second, Reply{kInvalidResult, false, "discovery aborted", {}});
    pending_order_.erase(it);
  }

 private:
  // ---- command dispatch (shared by stdin and the control socket) ----------
  // reply_fd >= 0 identifies a control connection that a deferred (async) reply
  // will be sent to; -1 means stdin (async commands just start, logged by telemetry).
  Reply dispatch(const std::vector<std::string>& args, int reply_fd) {
    const std::string& verb = args[0];

    if (verb == "publish") {
      if (args.size() < 2) return {kUsage, false, "usage: publish <text>", {}};
      std::string content;
      for (std::size_t k = 1; k < args.size(); ++k) content += (k > 1 ? " " : "") + args[k];
      domain::Bytes data(content.begin(), content.end());
      const auto& e = node_->publish_event(data);
      last_event_ = e.hash;
      return {kOk, false, "", {{"event", hex(e.hash)}}};
    }
    if (verb == "event") {
      if (args.size() < 2) return {kUsage, false, "usage: event <hash|last>", {}};
      const auto e = find_event(args[1]);
      if (!e) return {kNotFound, false, "no such event", {}};
      return {kOk, false, "", {{"event", hex(e->hash)}, {"creator", hex_id(e->creator)},
                               {"content", printable(e->data)},
                               {"size", std::to_string(e->data.size())},
                               {"references", std::to_string(e->referenced_events.size())}}};
    }
    if (verb == "event-find") {
      if (args.size() < 2) return {kUsage, false, "usage: event-find <text>", {}};
      std::string needle;
      for (std::size_t k = 1; k < args.size(); ++k) needle += (k > 1 ? " " : "") + args[k];
      Reply r;
      for (std::size_t i = 0; i < node_->event_count(); ++i) {
        const auto& e = node_->event_at(i);
        const std::string content(e.data.begin(), e.data.end());
        if (content.find(needle) != std::string::npos) {
          r.fields.emplace_back("event", hex(e.hash));
          r.fields.emplace_back("content", printable(e.data));
        }
      }
      if (r.fields.empty()) return {kNotFound, false, "no event matches", {}};
      return r;
    }
    if (verb == "events") {
      Reply r;
      for (std::size_t i = 0; i < node_->event_count(); ++i)
        r.fields.emplace_back("event", hex(node_->event_at(i).hash));
      return r;
    }
    if (verb == "bounds" || verb == "chain" || verb == "order") return dispatch_query(verb, args, reply_fd);
    if (verb == "prove") return dispatch_prove(args, reply_fd);
    if (verb == "peer-add") {
      if (args.size() < 2) return {kUsage, false, "usage: peer-add <id:ip:port>", {}};
      auto f = split(args[1], ':');
      if (f.size() != 3) return {kUsage, false, "want id:ip:port", {}};
      add_peer(std::strtoull(f[0].c_str(), nullptr, 0), f[1],
               static_cast<std::uint16_t>(std::atoi(f[2].c_str())));
      return {kOk, false, "", {{"peer", args[1]}}};
    }
    if (verb == "peer-ls") {
      Reply r;
      for (const auto& [id, ip, port] : peers_)
        r.fields.emplace_back("peer", hex_id(id) + " " + ip + ":" + std::to_string(port));
      return r;
    }
    if (verb == "status") {
      return {kOk, false, "", {{"node", hex_id(node_id_)},
                               {"mode", signed_ ? "signed" : "unsigned"},
                               {"events", std::to_string(node_->event_count())},
                               {"clockEvents", std::to_string(node_->clock_event_count())},
                               {"peers", std::to_string(peers_.size())}}};
    }
    if (verb == "key") {
      Reply r{kOk, false, "", {{"node", hex_id(node_id_)}}};
      if (auto* ks = dynamic_cast<os::Ed25519KeyStore*>(signer_.get()))
        r.fields.emplace_back("publicKey", hex(ks->public_key()));
      else
        r.fields.emplace_back("publicKey", "(unsigned)");
      return r;
    }
    if (verb == "save") {
      sync_store();
      return {kOk, false, "", {{"store", lmdb_ ? lmdb_->path() : "(in-memory)"}}};
    }
    if (verb == "db-stat") return db_stat();
    if (verb == "db-backup") {
      if (args.size() < 2) return {kUsage, false, "usage: db-backup <path>", {}};
      os::FileStore(args[1]).save(node_->snapshot());
      return {kOk, false, "", {{"backup", args[1]},
                               {"events", std::to_string(node_->event_count())},
                               {"clockEvents", std::to_string(node_->clock_event_count())}}};
    }
    if (verb == "db-restore") {
      if (args.size() < 2) return {kUsage, false, "usage: db-restore <path>", {}};
      const auto blob = os::FileStore(args[1]).load();
      if (!blob) return {kNotFound, false, "cannot read snapshot", {}};
      try {
        node_->restore(*blob);  // clears the store, imports the snapshot, rehydrates
      } catch (const std::exception& e) {
        return {kInvalidResult, false, std::string("invalid snapshot: ") + e.what(), {}};
      }
      return {kOk, false, "", {{"restored", args[1]},
                               {"events", std::to_string(node_->event_count())},
                               {"clockEvents", std::to_string(node_->clock_event_count())}}};
    }
    if (verb == "quit" || verb == "exit") {
      reactor_.stop();
      return {kOk, false, "", {{"status", "stopping"}}};
    }
    return {kUsage, false, "unknown command: " + verb, {}};
  }

  Reply dispatch_query(const std::string& verb, const std::vector<std::string>& args, int reply_fd) {
    if (verb == "order") {
      if (args.size() < 3) return {kUsage, false, "usage: order <a> <b>", {}};
      const auto a = resolve_event(args[1]);
      const auto b = resolve_event(args[2]);
      if (!a || !b) return {kNotFound, false, "no such event", {}};
      if (reply_fd >= 0) pending_order_[std::make_pair(a->hash, b->hash)] = reply_fd;
      node_->discover_event_order(*a, *b, *this);
      return reply_fd >= 0 ? Reply{kOk, true, "", {}} : Reply{kOk, false, "", {{"status", "started"}}};
    }
    if (args.size() < 2) return {kUsage, false, "usage: " + verb + " <event>", {}};
    const auto e = resolve_event(args[1]);
    if (!e) return {kNotFound, false, "no such event", {}};
    if (verb == "bounds") {
      if (reply_fd >= 0) pending_bounds_.insert({e->hash, reply_fd});
      node_->discover_event_bounds(*e, *this);
    } else {  // chain
      if (reply_fd >= 0) pending_chain_.insert({e->hash, reply_fd});
      node_->discover_event_chain(*e, *this);
    }
    return reply_fd >= 0 ? Reply{kOk, true, "", {}} : Reply{kOk, false, "", {{"status", "started"}}};
  }

  // ---- prove: run a chain discovery, then serialize a portable proof ------
  // The reply carries the proof as hex under `proof`; the CLI writes it to --out.
  // Proofs need to hold a connection until the discovery completes, so the control
  // socket (not stdin) is required.
  Reply dispatch_prove(const std::vector<std::string>& args, int reply_fd) {
    if (args.size() < 2)
      return {kUsage, false, "usage: prove <bounds|order|chain> <event> [event2]", {}};
    const std::string& sub = args[1];
    if (reply_fd < 0) return {kUsage, false, "prove requires the control socket", {}};

    if (sub == "order") {
      if (args.size() < 4) return {kUsage, false, "usage: prove order <a> <b>", {}};
      const auto a = resolve_event(args[2]);
      const auto b = resolve_event(args[3]);
      if (!a || !b) return {kNotFound, false, "no such event", {}};
      order_proofs_.push_back(OrderProof{reply_fd, a->hash, b->hash, {}, {}});
      node_->discover_event_chain(*a, *this);
      node_->discover_event_chain(*b, *this);
      return {kOk, true, "", {}};
    }
    if (sub != "bounds" && sub != "chain")
      return {kUsage, false, "prove kind must be bounds|order|chain", {}};
    if (args.size() < 3) return {kUsage, false, "usage: prove " + sub + " <event>", {}};
    const auto e = resolve_event(args[2]);
    if (!e) return {kNotFound, false, "no such event", {}};
    const proof::Kind kind = (sub == "chain") ? proof::Kind::chain : proof::Kind::bounds;
    pending_proof_.insert({e->hash, ProofRequest{reply_fd, kind}});
    node_->discover_event_chain(*e, *this);
    return {kOk, true, "", {}};
  }

  // Assemble a completed proof into a reply: the hex blob plus a human summary.
  Reply proof_reply(const proof::Proof& p) {
    Reply r{kOk, false, "", {}};
    r.fields.emplace_back("proof", hex_bytes(proof::serialize(p)));
    r.fields.emplace_back("kind", kind_name(p.kind));
    r.fields.emplace_back("reference", hex_id(p.reference.node));
    r.fields.emplace_back("chainLength",
                          std::to_string(p.chain.lower_bound.size() + p.chain.upper_bound.size()));
    if (p.kind == proof::Kind::order) {
      r.fields.emplace_back("chain2Length",
                            std::to_string(p.chain2.lower_bound.size() + p.chain2.upper_bound.size()));
      r.fields.emplace_back("order", std::to_string(static_cast<int>(p.order)));
    } else {
      r.fields.emplace_back("lower", std::to_string(p.chain.lower_bound.front().timestamp));
      r.fields.emplace_back("upper", std::to_string(p.chain.upper_bound.back().timestamp));
    }
    return r;
  }

  proof::Reference my_reference() const {
    proof::Reference ref;
    ref.node = node_id_;
    if (auto* ks = dynamic_cast<os::Ed25519KeyStore*>(signer_.get())) ref.pubkey = ks->public_key();
    return ref;
  }

  void finish_single_proof(const domain::EventHash& hash, const domain::EventChain& c) {
    auto range = pending_proof_.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it) {
      proof::Proof p;
      p.kind = it->second.kind;
      p.reference = my_reference();
      p.chain = c;
      respond(it->second.fd, proof_reply(p));
    }
    pending_proof_.erase(range.first, range.second);
  }
  void abort_single_proof(const domain::EventHash& hash) {
    auto range = pending_proof_.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it)
      respond(it->second.fd, Reply{kInvalidResult, false, "discovery aborted", {}});
    pending_proof_.erase(range.first, range.second);
  }

  void finish_order_proof(const domain::EventHash& hash, const domain::EventChain& c) {
    for (auto it = order_proofs_.begin(); it != order_proofs_.end();) {
      if (it->a == hash && !it->chain_a) it->chain_a = c;
      if (it->b == hash && !it->chain_b) it->chain_b = c;
      if (it->chain_a && it->chain_b) {
        proof::Proof p;
        p.kind = proof::Kind::order;
        p.reference = my_reference();
        p.chain = *it->chain_a;
        p.chain2 = *it->chain_b;
        p.order = compare_chains(p.chain, p.chain2);
        respond(it->fd, proof_reply(p));
        it = order_proofs_.erase(it);
      } else {
        ++it;
      }
    }
  }
  void abort_order_proof(const domain::EventHash& hash) {
    for (auto it = order_proofs_.begin(); it != order_proofs_.end();) {
      if (it->a == hash || it->b == hash) {
        respond(it->fd, Reply{kInvalidResult, false, "discovery aborted", {}});
        it = order_proofs_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // ---- stdin: dispatch and print the reply as human text ------------------
  void on_stdin_readable() {
    char buf[4096];
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) {
      reactor_.stop();
      return;
    }
    stdin_buf_.append(buf, static_cast<std::size_t>(n));
    std::size_t nl;
    while ((nl = stdin_buf_.find('\n')) != std::string::npos) {
      const std::string line = stdin_buf_.substr(0, nl);
      stdin_buf_.erase(0, nl + 1);
      const auto args = tokenize(line);
      if (args.empty()) continue;
      const Reply r = dispatch(args, -1);
      if (!r.error.empty())
        std::printf("error(%d): %s\n", r.code, r.error.c_str());
      else
        for (const auto& [k, v] : r.fields) std::printf("%s: %s\n", k.c_str(), v.c_str());
      std::fflush(stdout);
    }
  }

  // ---- control socket -----------------------------------------------------
  void setup_control() {
    if (control_path_.empty()) return;
    control_fd_ = os::control_listen(control_path_);
    if (control_fd_ < 0) {
      std::fprintf(stderr, "[lotid] cannot listen on control socket %s\n", control_path_.c_str());
      return;
    }
    reactor_.add_reader(control_fd_, [this] { on_control_accept(); });
    std::fprintf(stderr, "[lotid] control socket at %s\n", control_path_.c_str());
  }
  void on_control_accept() {
    const int fd = ::accept(control_fd_, nullptr, nullptr);
    if (fd < 0) return;
    client_buf_[fd];
    reactor_.add_reader(fd, [this, fd] { on_control_readable(fd); });
  }
  void on_control_readable(int fd) {
    char buf[4096];
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) {
      close_client(fd);
      return;
    }
    auto& acc = client_buf_[fd];
    acc.append(buf, static_cast<std::size_t>(n));
    const std::size_t nl = acc.find('\n');
    if (nl == std::string::npos) return;  // await the full request line
    const auto args = tokenize(acc.substr(0, nl));
    if (args.empty()) {
      respond(fd, {kUsage, false, "empty request", {}});
      return;
    }
    const Reply r = dispatch(args, fd);
    if (!r.deferred) respond(fd, r);  // deferred replies come from a discovery callback
  }
  void respond(int fd, const Reply& r) {
    if (!client_buf_.count(fd)) return;  // client already gone
    os::write_all(fd, os::format_reply(r.code, r.error, r.fields));
    close_client(fd);
  }
  void close_client(int fd) {
    if (!client_buf_.count(fd)) return;
    reactor_.remove_reader(fd);
    ::close(fd);
    client_buf_.erase(fd);
  }

  using PendingByEvent = std::multimap<domain::EventHash, int>;
  void answer(PendingByEvent& pending, const domain::EventHash& hash, const Reply& reply) {
    auto range = pending.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it) respond(it->second, reply);
    pending.erase(range.first, range.second);
  }
  void abort_pending(PendingByEvent& pending, const domain::EventHash& hash) {
    answer(pending, hash, Reply{kInvalidResult, false, "discovery aborted", {}});
  }

  // ---- events / snapshots -------------------------------------------------
  std::optional<domain::Event> find_event(const std::string& ref) {
    if (ref == "last") return last_event_.empty() ? std::nullopt : find_by_hash_prefix(hex(last_event_));
    return find_by_hash_prefix(ref);
  }
  // Resolve an event argument to an Event to run a discovery on. A local reference
  // ("last" or a hash prefix) returns the stored event; a remote reference
  // "<creatorHex>:<fullHashHex>" builds a synthetic pointer so a discovery can be
  // initiated for an event this node does not hold locally (the neighbor answers the
  // chain request and this node becomes the reference — an independent-notary proof).
  std::optional<domain::Event> resolve_event(const std::string& ref) {
    const auto colon = ref.find(':');
    if (colon != std::string::npos) {
      const auto hash = unhex(ref.substr(colon + 1));
      if (!hash || hash->size() != 32) return std::nullopt;
      domain::Event e;
      e.creator = std::strtoull(ref.substr(0, colon).c_str(), nullptr, 0);
      e.hash = *hash;
      return e;
    }
    return find_event(ref);
  }
  // The DAG lives in the store now, so events come back by value (a hash-prefix scan).
  std::optional<domain::Event> find_by_hash_prefix(const std::string& prefix) {
    for (std::size_t i = 0; i < node_->event_count(); ++i) {
      domain::Event e = node_->event_at(i);
      if (hex(e.hash).rfind(prefix, 0) == 0) return e;
    }
    return std::nullopt;
  }
  void sync_store() {
    if (lmdb_) lmdb_->sync();
  }

  // Store status: the retention invariant is that local events and the local clock
  // chain are NEVER pruned (they are the node's only record and back every future
  // proof), so the counts here only ever grow across a restart/backup/restore. The
  // node keeps no expendable derived state to garbage-collect at MVP scale.
  Reply db_stat() {
    const auto snapshot = node_->snapshot();
    Reply r{kOk, false, "", {}};
    r.fields.emplace_back("store", lmdb_ ? lmdb_->path() : "(in-memory)");
    r.fields.emplace_back("events", std::to_string(node_->event_count()));
    r.fields.emplace_back("clockEvents", std::to_string(node_->clock_event_count()));
    r.fields.emplace_back("peers", std::to_string(peers_.size()));
    r.fields.emplace_back("snapshotBytes", std::to_string(snapshot.size()));
    r.fields.emplace_back("retention", "local events + clock chain never dropped");
    if (lmdb_) {
      struct stat st {};
      r.fields.emplace_back("diskBytes",
                            std::to_string(::stat(lmdb_->path().c_str(), &st) == 0 ? st.st_size : 0));
    }
    return r;
  }
  os::WallClock clock_;
  os::Reactor reactor_;
  os::UdpTransport transport_;
  os::ReactorScheduler scheduler_;
  os::SecureRng rng_;
  os::LogTelemetry telemetry_;
  std::unique_ptr<ports::Signer> signer_;
  std::unique_ptr<Node> node_;

  domain::NodeId node_id_ = 0;
  bool signed_ = false;
  domain::EventHash last_event_;
  std::string stdin_buf_;
  std::unique_ptr<ports::Store> store_;          // the DAG store (LMDB or in-memory)
  os::LmdbStore* lmdb_ = nullptr;                // non-null iff store_ is LMDB-backed
  domain::Duration store_sync_interval_ns_ = 0;  // >0 → lazy store + periodic sync() timer
  std::vector<std::tuple<domain::NodeId, std::string, std::uint16_t>> peers_;

  std::string control_path_;
  int control_fd_ = -1;
  std::map<int, std::string> client_buf_;  // per-connection read buffer
  PendingByEvent pending_bounds_, pending_chain_;
  std::map<std::pair<domain::EventHash, domain::EventHash>, int> pending_order_;

  // prove: a single-chain proof waits on one event; an order proof accumulates two.
  struct ProofRequest {
    int fd;
    proof::Kind kind;
  };
  struct OrderProof {
    int fd;
    domain::EventHash a, b;
    std::optional<domain::EventChain> chain_a, chain_b;
  };
  std::multimap<domain::EventHash, ProofRequest> pending_proof_;
  std::vector<OrderProof> order_proofs_;
};

double parse_seconds(const char* s) { return std::atof(s); }

// Parse --store-mapsize (a value in GiB) into a byte count. Computed in double so the
// GiB→bytes multiply can't overflow a 32-bit size_t the way `atof * 2^30 -> size_t`
// does; a non-positive value is rejected; the result is clamped to what size_t can hold
// and, on a 32-bit build, to the store's address-space ceiling (with a warning).
std::size_t parse_mapsize_gib(const char* s) {
  const double gib = std::atof(s);
  if (!(gib > 0.0)) {
    std::fprintf(stderr, "--store-mapsize must be a positive number of GiB\n");
    std::exit(2);
  }
  double bytes = gib * 1073741824.0;
  const double max_size_t = static_cast<double>(std::numeric_limits<std::size_t>::max());
  if (bytes > max_size_t) bytes = max_size_t;  // never wrap the size_t cast
  auto out = static_cast<std::size_t>(bytes);
  if (os::LmdbStore::kMaxMapSize != 0 && out > os::LmdbStore::kMaxMapSize) {
    std::fprintf(stderr,
                 "[lotid] --store-mapsize %.2f GiB exceeds this build's %.2f GiB "
                 "address-space limit; clamping\n",
                 gib, os::LmdbStore::kMaxMapSize / 1073741824.0);
    out = os::LmdbStore::kMaxMapSize;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::optional<domain::NodeId> id;
  std::uint16_t port = 0;
  double clock_interval = 1.0;
  double expiry = 5.0;
  bool verbose = false;
  std::string store_path, key_path, control_path;
  std::size_t store_map_size = loti::os::LmdbStore::kDefaultMapSize;
  double store_sync_interval = 0.0;  // 0 = safe (fsync per commit); >0 = lazy + periodic sync
  std::vector<std::string> peers, routes;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--id") id = std::strtoull(next("--id"), nullptr, 0);
    else if (a == "--port") port = static_cast<std::uint16_t>(std::atoi(next("--port")));
    else if (a == "--peer") peers.push_back(next("--peer"));
    else if (a == "--route") routes.push_back(next("--route"));
    else if (a == "--clock-interval") clock_interval = parse_seconds(next("--clock-interval"));
    else if (a == "--expiry") expiry = parse_seconds(next("--expiry"));
    else if (a == "--store") store_path = next("--store");
    else if (a == "--store-mapsize") store_map_size = parse_mapsize_gib(next("--store-mapsize"));
    else if (a == "--store-sync-interval")
      store_sync_interval = parse_seconds(next("--store-sync-interval"));
    else if (a == "--key") key_path = next("--key");
    else if (a == "--control") control_path = next("--control");
    else if (a == "--verbose" || a == "-v") verbose = true;
    else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      return 2;
    }
  }
  if (port == 0 || (!id && key_path.empty())) {
    std::fprintf(stderr,
                 "usage: lotid --port <p> {--id <n> | --key <path>} [--peer id:ip:port]... "
                 "[--route dst:nexthop]... [--clock-interval s] [--expiry s] [--store <path>] "
                 "[--store-mapsize <GiB>] [--store-sync-interval <s>] [--control <path>] "
                 "[--verbose]\n");
    return 2;
  }

  const auto to_ns = [](double s) {
    return static_cast<loti::domain::Duration>(s * 1'000'000'000.0);
  };

  try {
    Lotid daemon(id.value_or(0), port, to_ns(clock_interval), to_ns(expiry), verbose, store_path,
                 store_map_size, to_ns(store_sync_interval), key_path, control_path);
    daemon.restore_from_store();
    for (const auto& p : peers) {
      auto f = split(p, ':');
      if (f.size() != 3) {
        std::fprintf(stderr, "bad --peer (want id:ip:port): %s\n", p.c_str());
        return 2;
      }
      daemon.add_peer(std::strtoull(f[0].c_str(), nullptr, 0), f[1],
                      static_cast<std::uint16_t>(std::atoi(f[2].c_str())));
    }
    for (const auto& r : routes) {
      auto f = split(r, ':');
      if (f.size() != 2) {
        std::fprintf(stderr, "bad --route (want dst:nexthop): %s\n", r.c_str());
        return 2;
      }
      daemon.learn_route(std::strtoull(f[0].c_str(), nullptr, 0),
                         std::strtoull(f[1].c_str(), nullptr, 0));
    }
    std::fprintf(stderr, "[lotid] node %s listening on udp/%u (%s)\n", hex_id(daemon.node_id()).c_str(),
                 port, key_path.empty() ? "unsigned" : "signed");
    daemon.run();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[lotid] fatal: %s\n", e.what());
    return 1;
  }
  return 0;
}
