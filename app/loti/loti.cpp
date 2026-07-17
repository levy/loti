// loti — the command-line client for a running lotid daemon.
//
// It speaks the control-socket line protocol in adapters/os/control.hpp: it maps a
// subcommand to a daemon verb, sends one request, and renders the reply as human
// text or, with --json, as an object (repeated field keys become an array). The
// process exit code is the daemon's reply code (see cli.md). `init` runs locally
// (no daemon): it creates a home directory, generates a key, and writes a config.
//
// Usage:
//   loti [--control <path>] [--json] [--quiet] <command> [args...]
//
//   status | node status              node id, counts, mode
//   publish <text...>                 publish an event
//   event show <hash|last>            show a known event
//   events                            list event hashes
//   bounds <event> | chain <event>    discover time bounds / an event chain
//   order <a> <b>                     discover the order of two events
//   prove bounds|order|chain … --out  discover + serialize a portable proof
//   verify <file> [--trust <node>]    verify a proof offline (exit 0 valid / 6 invalid)
//   proof show <file>                 summarize a proof (no verification)
//   peer add <id:ip:port> | peer ls   manage neighbors
//   key [show]                        node id + public key
//   node stop | stop                  stop the daemon
//   init [--home <dir>]               create home dir, key, and config

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "adapters/os/control.hpp"
#include "adapters/os/keystore.hpp"
#include "proof/proof.hpp"

namespace {

using namespace loti;

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

const char* kind_name(proof::Kind k) {
  switch (k) {
    case proof::Kind::bounds: return "bounds";
    case proof::Kind::order:  return "order";
    case proof::Kind::chain:  return "chain";
  }
  return "unknown";
}
const char* order_name(domain::Order o) {
  switch (o) {
    case domain::Order::before: return "before";
    case domain::Order::after:  return "after";
    case domain::Order::undetermined: return "undetermined";
  }
  return "undetermined";
}

// Render a raw clock tick. Production timestamps are CLOCK_REALTIME nanoseconds, so
// show the wall-clock time alongside the raw value (which is what the hash covers).
std::string format_ts(domain::Timestamp ns) {
  const std::time_t secs = static_cast<std::time_t>(ns / 1'000'000'000);
  std::tm tm{};
  gmtime_r(&secs, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return std::string(buf) + " (raw " + std::to_string(ns) + ")";
}

std::optional<domain::Bytes> unhex(const std::string& s) {
  if (s.size() % 2) return std::nullopt;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  domain::Bytes b;
  b.reserve(s.size() / 2);
  for (std::size_t i = 0; i < s.size(); i += 2) {
    const int hi = nib(s[i]), lo = nib(s[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    b.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return b;
}

std::optional<domain::Bytes> read_file(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return std::nullopt;
  domain::Bytes b;
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
  std::fclose(f);
  return b;
}

bool write_file(const std::string& path, const domain::Bytes& b) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
  std::fclose(f);
  return ok;
}

std::string json_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      default: o.push_back(c);
    }
  }
  return o;
}

void print_json(const os::ControlReply& r) {
  // group values by key (in first-seen order) so repeated keys form arrays
  std::vector<std::string> order;
  std::map<std::string, std::vector<std::string>> grouped;
  for (const auto& [k, v] : r.fields) {
    if (!grouped.count(k)) order.push_back(k);
    grouped[k].push_back(v);
  }
  std::string out = "{";
  out += "\"ok\":" + std::string(r.ok ? "true" : "false") + ",\"code\":" + std::to_string(r.code);
  if (!r.ok) out += ",\"error\":\"" + json_escape(r.message) + "\"";
  for (const auto& k : order) {
    out += ",\"" + json_escape(k) + "\":";
    const auto& vs = grouped[k];
    if (vs.size() == 1) {
      out += "\"" + json_escape(vs[0]) + "\"";
    } else {
      out += "[";
      for (std::size_t i = 0; i < vs.size(); ++i) out += (i ? "," : "") + std::string("\"") + json_escape(vs[i]) + "\"";
      out += "]";
    }
  }
  out += "}\n";
  std::fputs(out.c_str(), stdout);
}

// Map a CLI subcommand (argv after the globals) to a daemon request line.
// Returns "" if the command is unknown/malformed (usage printed by the caller).
std::string to_request(const std::vector<std::string>& a) {
  if (a.empty()) return "";
  auto join_from = [&](std::size_t i, const std::string& verb) {
    std::string r = verb;
    for (; i < a.size(); ++i) r += " " + a[i];
    return r;
  };
  const std::string& c = a[0];
  if (c == "status") return "status";
  if (c == "events") return "events";
  if (c == "publish" && a.size() >= 2) return join_from(1, "publish");
  if (c == "bounds" && a.size() >= 2) return "bounds " + a[1];
  if (c == "chain" && a.size() >= 2) return "chain " + a[1];
  if (c == "order" && a.size() >= 3) return "order " + a[1] + " " + a[2];
  if (c == "key") return "key";
  if (c == "stop") return "quit";
  if (c == "event" && a.size() >= 3 && a[1] == "show") return "event " + a[2];
  if (c == "peer" && a.size() >= 3 && a[1] == "add") return "peer-add " + a[2];
  if (c == "peer" && a.size() >= 2 && a[1] == "ls") return "peer-ls";
  if (c == "node" && a.size() >= 2 && a[1] == "status") return "status";
  if (c == "node" && a.size() >= 2 && a[1] == "stop") return "quit";
  return "";
}

int do_init(const std::string& home) {
  ::mkdir(home.c_str(), 0700);
  const std::string keypath = home + "/key";
  os::Ed25519KeyStore ks;
  ks.load_or_generate(keypath);
  const std::string store = home + "/state.snap";
  const std::string control = home + "/control.sock";
  if (FILE* f = std::fopen((home + "/config").c_str(), "w")) {
    std::fprintf(f, "# lotid config written by `loti init`\nport=7000\nkey=%s\nstore=%s\ncontrol=%s\n",
                 keypath.c_str(), store.c_str(), control.c_str());
    std::fclose(f);
  }
  std::printf("initialized %s\n", home.c_str());
  std::printf("node id: %s\n", hex_id(ks.node_id()).c_str());
  std::printf("public key: %s\n", hex(ks.public_key()).c_str());
  std::printf("start: lotid --key %s --port 7000 --store %s --control %s\n", keypath.c_str(),
              store.c_str(), control.c_str());
  return 0;
}

// `loti prove <bounds|order|chain> <event> [event2] --out <file>` — ask the daemon
// to run the discovery and serialize a proof, then write the artifact to --out.
int do_prove(const std::vector<std::string>& args, const std::string& control,
             const std::string& out, bool quiet) {
  if (out.empty()) {
    std::fprintf(stderr, "loti: prove requires --out <file>\n");
    return 2;
  }
  std::string request;
  for (std::size_t i = 0; i < args.size(); ++i) request += (i ? " " : "") + args[i];
  const auto reply = os::control_request(control, request);
  if (!reply) {
    std::fprintf(stderr, "loti: cannot reach daemon at %s\n", control.c_str());
    return 1;
  }
  if (!reply->ok) {
    std::fprintf(stderr, "error(%d): %s\n", reply->code, reply->message.c_str());
    return reply->code;
  }
  std::string hexproof, kind = "?", reference = "?", chain_length = "?";
  for (const auto& [k, v] : reply->fields) {
    if (k == "proof") hexproof = v;
    else if (k == "kind") kind = v;
    else if (k == "reference") reference = v;
    else if (k == "chainLength") chain_length = v;
  }
  const auto bytes = unhex(hexproof);
  if (!bytes || bytes->empty()) {
    std::fprintf(stderr, "loti: daemon returned no proof\n");
    return 1;
  }
  if (!write_file(out, *bytes)) {
    std::fprintf(stderr, "loti: cannot write %s\n", out.c_str());
    return 1;
  }
  if (!quiet)
    std::printf("wrote %s  (%s proof, chain length %s, reference %s)\n", out.c_str(), kind.c_str(),
                chain_length.c_str(), reference.c_str());
  return 0;
}

// `loti proof show <file>` — human summary, no verification.
int do_proof_show(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    std::fprintf(stderr, "usage: loti proof show <file>\n");
    return 2;
  }
  const auto bytes = read_file(args[2]);
  if (!bytes) {
    std::fprintf(stderr, "loti: cannot read %s\n", args[2].c_str());
    return 4;
  }
  proof::Proof p;
  try {
    p = proof::deserialize(*bytes);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "loti: %s\n", e.what());
    return 4;
  }
  std::printf("kind: %s\n", kind_name(p.kind));
  std::printf("reference: %s\n", hex_id(p.reference.node).c_str());
  if (!p.reference.pubkey.empty()) std::printf("pubkey: ed25519:%s\n", hex(p.reference.pubkey).c_str());
  std::printf("event: %s\n", hex(p.chain.event.hash).c_str());
  std::printf("chainLength: %zu\n", p.chain.lower_bound.size() + p.chain.upper_bound.size());
  if (p.kind == proof::Kind::order) {
    std::printf("event2: %s\n", hex(p.chain2.event.hash).c_str());
    std::printf("order: %s\n", order_name(p.order));
  } else {
    std::printf("lower: %s\n", format_ts(p.chain.lower_bound.front().timestamp).c_str());
    std::printf("upper: %s\n", format_ts(p.chain.upper_bound.back().timestamp).c_str());
  }
  std::printf("(not verified — run `loti verify %s`)\n", args[2].c_str());
  return 0;
}

// `loti verify <file> [--trust <node>]... [--json]` — offline verification.
// Exit 0 if the proof is valid (integrity + attribution), 6 if not. `--trust` is
// advisory for the MVP: verification proves integrity, and a warning is printed if
// the reference node is outside the supplied trust set.
int do_verify(const std::vector<std::string>& args, const std::vector<std::string>& trust, bool json) {
  if (args.size() < 2) {
    std::fprintf(stderr, "usage: loti verify <file> [--trust <node>]... [--json]\n");
    return 2;
  }
  const auto bytes = read_file(args[1]);
  if (!bytes) {
    std::fprintf(stderr, "loti: cannot read %s\n", args[1].c_str());
    return 4;
  }
  proof::Proof p;
  try {
    p = proof::deserialize(*bytes);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "loti: %s\n", e.what());
    return 4;
  }

  os::Ed25519KeyStore verifier;  // verify() uses each signature's embedded public key
  const auto res = proof::verify(p, verifier);

  bool trusted = trust.empty();  // no --trust given → don't gate, just report validity
  for (const auto& t : trust)
    if (std::strtoull(t.c_str(), nullptr, 0) == p.reference.node) trusted = true;

  if (json) {
    std::string o = "{\"valid\":" + std::string(res.valid ? "true" : "false");
    o += ",\"kind\":\"" + std::string(kind_name(p.kind)) + "\"";
    o += ",\"reference\":\"" + hex_id(p.reference.node) + "\"";
    if (!res.valid) o += ",\"reason\":\"" + json_escape(res.reason) + "\"";
    else if (p.kind == proof::Kind::order) o += ",\"order\":\"" + std::string(order_name(res.order)) + "\"";
    else { o += ",\"lower\":" + std::to_string(res.lower) + ",\"upper\":" + std::to_string(res.upper); }
    if (!trust.empty()) o += ",\"trusted\":" + std::string(trusted ? "true" : "false");
    o += "}\n";
    std::fputs(o.c_str(), stdout);
    return res.valid ? 0 : 6;
  }

  if (!res.valid) {
    std::fprintf(stderr, "invalid: %s\n", res.reason.c_str());
    return 6;
  }
  std::printf("valid: %s proof\n", kind_name(p.kind));
  std::printf("reference: %s\n", hex_id(p.reference.node).c_str());
  if (!p.reference.pubkey.empty()) std::printf("pubkey: ed25519:%s\n", hex(p.reference.pubkey).c_str());
  if (p.kind == proof::Kind::order) {
    std::printf("order: %s (relative to the reference node's clock)\n", order_name(res.order));
  } else {
    std::printf("lower: %s\n", format_ts(res.lower).c_str());
    std::printf("upper: %s\n", format_ts(res.upper).c_str());
    std::printf("(bounds are in the reference node's clock)\n");
  }
  if (!trust.empty() && !trusted)
    std::fprintf(stderr, "warning: reference %s is not in the --trust set\n",
                 hex_id(p.reference.node).c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string control = std::getenv("LOTI_CONTROL") ? std::getenv("LOTI_CONTROL") : "loti.sock";
  std::string home = std::getenv("HOME") ? std::string(std::getenv("HOME")) + "/.loti" : ".loti";
  std::string out;
  std::vector<std::string> trust;
  bool json = false, quiet = false;
  std::vector<std::string> args;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--json") json = true;
    else if (a == "--quiet" || a == "-q") quiet = true;
    else if (a == "--control" && i + 1 < argc) control = argv[++i];
    else if (a == "--home" && i + 1 < argc) home = argv[++i];
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--trust" && i + 1 < argc) trust.push_back(argv[++i]);
    else args.push_back(a);
  }

  if (args.empty()) {
    std::fprintf(stderr, "usage: loti [--control <path>] [--json] [--quiet] <command> [args...]\n"
                         "commands: status | publish <text> | event show <h> | events | "
                         "bounds <e> | chain <e> | order <a> <b> | prove <kind> <e> [e2] --out <f> | "
                         "verify <f> | proof show <f> | peer add|ls | key | stop | init\n");
    return 2;
  }
  if (args[0] == "init") return do_init(home);
  if (args[0] == "prove") return do_prove(args, control, out, quiet);
  if (args[0] == "verify") return do_verify(args, trust, json);
  if (args[0] == "proof" && args.size() >= 2 && args[1] == "show") return do_proof_show(args);

  const std::string request = to_request(args);
  if (request.empty()) {
    std::fprintf(stderr, "loti: unknown or malformed command\n");
    return 2;
  }

  const auto reply = os::control_request(control, request);
  if (!reply) {
    std::fprintf(stderr, "loti: cannot reach daemon at %s\n", control.c_str());
    return 1;
  }
  if (json) {
    print_json(*reply);
  } else if (!quiet) {
    if (!reply->ok)
      std::fprintf(stderr, "error(%d): %s\n", reply->code, reply->message.c_str());
    for (const auto& [k, v] : reply->fields) std::printf("%s: %s\n", k.c_str(), v.c_str());
  }
  return reply->code;
}
