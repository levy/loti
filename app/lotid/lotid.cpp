// lotid — the LOTI daemon: one protocol node running on the OS port adapters.
//
// This is the production counterpart of the OMNeT++ Daemon module. It constructs a
// single loti::Node, wires it to the wall clock, the epoll reactor (scheduler), a
// real UDP socket (transport), a CSPRNG, a signer (an Ed25519 keystore with --key,
// else a null signer), a snapshot store (--store), and a logging telemetry sink,
// then runs the reactor. Overlay peering is static (from --peer/--route flags); a
// control channel + `loti` CLI arrive in Stage 5. For now the daemon takes line
// commands on stdin so two instances can be scripted:
//
//   publish <text>          publish an event with the given content
//   bounds  <hexprefix|last> discover the time bounds of a known event
//   chain   <hexprefix|last> discover (and validate) an event chain
//   order   <a> <b>          discover the order of two events (hex prefixes/last)
//   events                   list known event hashes
//   key                      show this node's id and public key
//   save                     write a snapshot now (if --store is set)
//   quit                     stop the daemon
//
// Usage:
//   lotid --port <p> {--id <n> | --key <path>} [--peer <id>:<ip>:<port>]...
//         [--route <dst>:<nexthop>]... [--clock-interval <s>] [--expiry <s>]
//         [--store <path>] [--verbose]

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "adapters/os/clock.hpp"
#include "adapters/os/keystore.hpp"
#include "adapters/os/reactor.hpp"
#include "adapters/os/rng.hpp"
#include "adapters/os/signer.hpp"
#include "adapters/os/store.hpp"
#include "adapters/os/telemetry.hpp"
#include "adapters/os/transport.hpp"
#include "node.hpp"

namespace {

using namespace loti;

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

// A daemon hosting one Node, plus the stdin command interface. It is the core's
// discovery callback so results print as they complete.
class Lotid final : public ChainCallback, public BoundsCallback, public OrderCallback {
 public:
  Lotid(domain::NodeId id, std::uint16_t port, domain::Duration clock_ns,
        domain::Duration expiry_ns, bool verbose, const std::string& store_path,
        const std::string& key_path)
      : transport_(port), scheduler_(reactor_, clock_), telemetry_(verbose) {
    if (key_path.empty()) {
      signer_ = std::make_unique<os::NullSigner>();  // unsigned mode: NodeId from --id
      node_id_ = id;
    } else {
      auto keystore = std::make_unique<os::Ed25519KeyStore>();
      keystore->load_or_generate(key_path);
      node_id_ = keystore->node_id();  // signed mode: NodeId is the key fingerprint
      signer_ = std::move(keystore);
    }
    node_ = std::make_unique<Node>(
        node_id_, NodePorts{clock_, scheduler_, transport_, rng_, *signer_, telemetry_},
        NodeConfig{clock_ns, expiry_ns});
    if (!store_path.empty()) store_.emplace(store_path);
  }

  Node& node() { return *node_; }
  domain::NodeId node_id() const { return node_id_; }
  os::UdpTransport& transport() { return transport_; }

  // Load a prior snapshot (if any) BEFORE static peering is applied, so learned
  // neighbor state survives while CLI --peer/--route still set current addresses.
  void restore_from_store() {
    if (!store_) return;
    if (auto blob = store_->load()) {
      node_->restore(*blob);
      std::fprintf(stderr, "[lotid] restored %zu events, %zu clock events from %s\n",
                   node_->event_count(), node_->clock_event_count(), store_->path().c_str());
    }
  }

  void run() {
    node_->start();
    if (store_) schedule_snapshot();
    // Inbound UDP: drain every pending datagram into the core.
    reactor_.add_reader(transport_.fd(), [this] {
      for (;;) {
        domain::Bytes datagram = transport_.receive();
        if (datagram.empty()) break;
        node_->on_packet_received(datagram);
      }
    });
    // Line commands on stdin (EOF stops the daemon).
    reactor_.add_reader(STDIN_FILENO, [this] { on_stdin_readable(); });
    reactor_.run();
    save_snapshot();  // durable final state on graceful shutdown
  }

  // ---- discovery callbacks (logging is done by LogTelemetry; keep these terse) --
  void on_chain_completed(const domain::Event&, const domain::EventChain&) override {}
  void on_chain_aborted(const domain::Event&) override {}
  void on_bounds_completed(const domain::Event&, domain::Timestamp, domain::Timestamp) override {}
  void on_bounds_aborted(const domain::Event&) override {}
  void on_order_completed(const domain::Event&, const domain::Event&, domain::Order) override {}
  void on_order_aborted(const domain::Event&, const domain::Event&) override {}

 private:
  void on_stdin_readable() {
    char buf[4096];
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) {  // EOF or error → shut down
      reactor_.stop();
      return;
    }
    pending_.append(buf, static_cast<std::size_t>(n));
    std::size_t nl;
    while ((nl = pending_.find('\n')) != std::string::npos) {
      handle_command(pending_.substr(0, nl));
      pending_.erase(0, nl + 1);
    }
  }

  void handle_command(const std::string& line) {
    auto tok = split(line, ' ');
    // drop empty tokens (e.g. double spaces)
    std::vector<std::string> args;
    for (auto& t : tok)
      if (!t.empty()) args.push_back(t);
    if (args.empty()) return;
    const std::string& cmd = args[0];

    if (cmd == "publish" && args.size() >= 2) {
      std::string content;
      for (std::size_t k = 1; k < args.size(); ++k) {
        if (k > 1) content += ' ';
        content += args[k];
      }
      domain::Bytes data(content.begin(), content.end());
      const auto& e = node_->publish_event(data);
      last_event_ = e.hash;
      std::printf("published %s\n", hex(e.hash).c_str());
    } else if (cmd == "bounds" && args.size() >= 2) {
      if (auto* e = find_event(args[1])) node_->discover_event_bounds(*e, *this);
      else std::printf("no such event: %s\n", args[1].c_str());
    } else if (cmd == "chain" && args.size() >= 2) {
      if (auto* e = find_event(args[1])) node_->discover_event_chain(*e, *this);
      else std::printf("no such event: %s\n", args[1].c_str());
    } else if (cmd == "order" && args.size() >= 3) {
      auto* a = find_event(args[1]);
      auto* b = find_event(args[2]);
      if (a && b) node_->discover_event_order(*a, *b, *this);
      else std::printf("no such event\n");
    } else if (cmd == "events") {
      for (std::size_t i = 0; i < node_->event_count(); ++i)
        std::printf("  %s\n", hex(node_->event_at(i).hash).c_str());
    } else if (cmd == "key") {
      std::printf("node id: 0x%016llx\n", static_cast<unsigned long long>(node_id_));
      if (auto* ks = dynamic_cast<os::Ed25519KeyStore*>(signer_.get()))
        std::printf("public key: %s\n", hex(ks->public_key()).c_str());
      else
        std::printf("(unsigned mode — no key)\n");
    } else if (cmd == "save") {
      save_snapshot();
    } else if (cmd == "quit" || cmd == "exit") {
      reactor_.stop();
    } else {
      std::printf("unknown command: %s\n", line.c_str());
    }
    std::fflush(stdout);
  }

  // Resolve an event by "last" or a hex-hash prefix over this node's own events.
  const domain::Event* find_event(const std::string& ref) {
    if (ref == "last") {
      if (last_event_.empty()) return nullptr;
      return find_by_hash_prefix(hex(last_event_));
    }
    return find_by_hash_prefix(ref);
  }
  const domain::Event* find_event_ptr(std::size_t i) { return &node_->event_at(i); }
  const domain::Event* find_by_hash_prefix(const std::string& prefix) {
    for (std::size_t i = 0; i < node_->event_count(); ++i)
      if (hex(node_->event_at(i).hash).rfind(prefix, 0) == 0) return find_event_ptr(i);
    return nullptr;
  }

  void save_snapshot() {
    if (!store_) return;
    store_->save(node_->snapshot());
    std::fprintf(stderr, "[lotid] saved snapshot (%zu events, %zu clock events) to %s\n",
                 node_->event_count(), node_->clock_event_count(), store_->path().c_str());
  }
  void schedule_snapshot() {
    reactor_.add_timer(clock_.now() + kSnapshotIntervalNs, [this] {
      save_snapshot();
      schedule_snapshot();
    });
  }

  static constexpr domain::Duration kSnapshotIntervalNs = 5'000'000'000;  // 5 s

  os::WallClock clock_;
  os::Reactor reactor_;
  os::UdpTransport transport_;
  os::ReactorScheduler scheduler_;
  os::SecureRng rng_;
  os::LogTelemetry telemetry_;
  std::unique_ptr<ports::Signer> signer_;  // NullSigner (unsigned) or Ed25519KeyStore
  std::unique_ptr<Node> node_;

  domain::NodeId node_id_ = 0;
  domain::EventHash last_event_;
  std::string pending_;
  std::optional<os::FileStore> store_;
};

double parse_seconds(const char* s) { return std::atof(s); }

}  // namespace

int main(int argc, char** argv) {
  std::optional<domain::NodeId> id;
  std::uint16_t port = 0;
  double clock_interval = 1.0;
  double expiry = 5.0;
  bool verbose = false;
  std::string store_path;           // snapshot file (empty = no persistence)
  std::string key_path;             // Ed25519 key file (empty = unsigned mode)
  std::vector<std::string> peers;   // "id:ip:port"
  std::vector<std::string> routes;  // "dst:nexthop"

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
    else if (a == "--key") key_path = next("--key");
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
                 "[--verbose]\n"
                 "  --key <path>  signed mode: load/generate an Ed25519 key; NodeId = its fingerprint\n"
                 "  --id  <n>     unsigned mode: use n as the NodeId\n");
    return 2;
  }

  const auto to_ns = [](double s) {
    return static_cast<loti::domain::Duration>(s * 1'000'000'000.0);
  };

  try {
    Lotid daemon(id.value_or(0), port, to_ns(clock_interval), to_ns(expiry), verbose, store_path,
                 key_path);
    daemon.restore_from_store();  // load prior state before static peering is applied
    for (const auto& p : peers) {
      auto f = split(p, ':');
      if (f.size() != 3) {
        std::fprintf(stderr, "bad --peer (want id:ip:port): %s\n", p.c_str());
        return 2;
      }
      const auto peer_id = std::strtoull(f[0].c_str(), nullptr, 0);
      daemon.node().add_neighbor(peer_id);
      daemon.node().learn_route(peer_id, peer_id);  // direct route to a neighbor
      daemon.transport().set_peer(peer_id, f[1], static_cast<std::uint16_t>(std::atoi(f[2].c_str())));
    }
    for (const auto& r : routes) {
      auto f = split(r, ':');
      if (f.size() != 2) {
        std::fprintf(stderr, "bad --route (want dst:nexthop): %s\n", r.c_str());
        return 2;
      }
      daemon.node().learn_route(std::strtoull(f[0].c_str(), nullptr, 0),
                                std::strtoull(f[1].c_str(), nullptr, 0));
    }
    std::fprintf(stderr, "[lotid] node 0x%016llx listening on udp/%u%s\n",
                 static_cast<unsigned long long>(daemon.node_id()), port,
                 key_path.empty() ? " (unsigned)" : " (signed)");
    daemon.run();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[lotid] fatal: %s\n", e.what());
    return 1;
  }
  return 0;
}
