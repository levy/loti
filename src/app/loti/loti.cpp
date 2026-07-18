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
//   event show <hash|last>            show a known event (creator, content, refs)
//   event find <text...>              find local events whose content contains <text>
//   events                            list event hashes
//   bounds <event> | chain <event>    discover time bounds / an event chain
//   order <a> <b>                     discover the order of two events
//   prove bounds|order|chain … --out  discover + serialize a portable proof
//   verify <file> [--trust <node>]    verify a proof offline (exit 0 valid / 6 invalid)
//   proof show <file>                 summarize a proof (no verification)
//   db stat | backup --out | restore  inspect / back up / restore the snapshot
//   peer add <id:ip:port> | peer ls   manage neighbors
//   key [show]                        node id + public key
//   node stop | stop                  stop the daemon
//   init [--home <dir>]               create home dir, key, and config

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// ANSI styling, enabled only for an interactive stdout with NO_COLOR unset.
struct Style {
  bool on = false;
  const char* title() const { return on ? "\033[1;36m" : ""; }  // bold cyan — section
  const char* group() const { return on ? "\033[1;33m" : ""; }  // bold yellow — group
  const char* cmd()   const { return on ? "\033[32m"   : ""; }  // green — command
  const char* opt()   const { return on ? "\033[33m"   : ""; }  // yellow — option
  const char* bold()  const { return on ? "\033[1m"    : ""; }
  const char* dim()   const { return on ? "\033[2m"    : ""; }
  const char* reset() const { return on ? "\033[0m"    : ""; }
};

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

std::string printable(const domain::Bytes& b) {
  std::string s;
  s.reserve(b.size());
  for (unsigned char c : b) s.push_back((c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.');
  return s;
}

// ---- JSON output: a tiny tree writer with pretty-printing --------------------
// Build a value tree, then serialize it with 2-space indentation and newlines.
// This is a WRITER only — no parser is needed (the daemon speaks the flat line
// protocol; the CLI shapes those fields into proper nested JSON per command).
struct Json {
  enum class Type { Null, Bool, Num, Str, Arr, Obj } type = Type::Null;
  bool b = false;
  std::string text;                                   // Num: literal; Str: raw value
  std::vector<Json> items;                            // Arr
  std::vector<std::pair<std::string, Json>> members;  // Obj

  static Json str(std::string v) { Json j; j.type = Type::Str; j.text = std::move(v); return j; }
  static Json num(long long v) { Json j; j.type = Type::Num; j.text = std::to_string(v); return j; }
  static Json num_text(std::string v) { Json j; j.type = Type::Num; j.text = std::move(v); return j; }
  static Json boolean(bool v) { Json j; j.type = Type::Bool; j.b = v; return j; }
  static Json array() { Json j; j.type = Type::Arr; return j; }
  static Json object() { Json j; j.type = Type::Obj; return j; }
  Json& set(std::string k, Json v) { members.emplace_back(std::move(k), std::move(v)); return *this; }
  Json& push(Json v) { items.push_back(std::move(v)); return *this; }
};

void json_emit(const Json& j, std::string& out, int depth) {
  const auto indent = [&](int d) { out.append(static_cast<std::size_t>(d) * 2, ' '); };
  switch (j.type) {
    case Json::Type::Null: out += "null"; break;
    case Json::Type::Bool: out += j.b ? "true" : "false"; break;
    case Json::Type::Num:  out += j.text; break;
    case Json::Type::Str:  out += '"'; out += json_escape(j.text); out += '"'; break;
    case Json::Type::Arr:
      if (j.items.empty()) { out += "[]"; break; }
      out += "[\n";
      for (std::size_t i = 0; i < j.items.size(); ++i) {
        indent(depth + 1);
        json_emit(j.items[i], out, depth + 1);
        out += (i + 1 < j.items.size() ? ",\n" : "\n");
      }
      indent(depth); out += "]";
      break;
    case Json::Type::Obj:
      if (j.members.empty()) { out += "{}"; break; }
      out += "{\n";
      for (std::size_t i = 0; i < j.members.size(); ++i) {
        indent(depth + 1);
        out += '"'; out += json_escape(j.members[i].first); out += "\": ";
        json_emit(j.members[i].second, out, depth + 1);
        out += (i + 1 < j.members.size() ? ",\n" : "\n");
      }
      indent(depth); out += "}";
      break;
  }
}
std::string json_pretty(const Json& j) { std::string out; json_emit(j, out, 0); out += "\n"; return out; }

// Field keys whose values are small integers (safe as JSON numbers). Absolute
// timestamps (clock-event `ts`, bounds `lower`/`upper`) stay strings to preserve
// their full 19-digit precision for consumers that parse numbers as doubles.
bool json_numeric_key(const std::string& k) {
  return k == "size" || k == "references" || k == "clockEvents" || k == "peers" ||
         k == "interval" || k == "order" || k == "events" || k == "snapshotBytes" ||
         k == "diskBytes";
}
Json json_value(const std::string& key, const std::string& val) {
  if (json_numeric_key(key) && !val.empty()) {
    std::size_t i = (val[0] == '-') ? 1 : 0;
    bool digits = i < val.size();
    for (; i < val.size(); ++i)
      if (val[i] < '0' || val[i] > '9') { digits = false; break; }
    if (digits) return Json::num_text(val);
  }
  return Json::str(val);
}

// Parse one chain-path element "creator=<id> hash=<hex> ts=<n>" into an object.
Json json_clock_from_line(const std::string& v) {
  std::string creator, h, ts;
  for (std::size_t p = 0; p < v.size();) {
    const std::size_t sp = v.find(' ', p);
    const std::string tok = v.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
    const std::size_t eq = tok.find('=');
    if (eq != std::string::npos) {
      const std::string key = tok.substr(0, eq), val = tok.substr(eq + 1);
      if (key == "creator") creator = val;
      else if (key == "hash") h = val;
      else if (key == "ts") ts = val;
    }
    if (sp == std::string::npos) break;
    p = sp + 1;
  }
  Json o = Json::object();
  o.set("creator", Json::str(creator));
  o.set("hash", Json::str(h));
  o.set("timestamp", Json::str(ts));
  return o;
}

Json json_base(const os::ControlReply& r) {
  Json o = Json::object();
  o.set("ok", Json::boolean(r.ok));
  o.set("code", Json::num(r.code));
  if (!r.ok) o.set("error", Json::str(r.message));
  return o;
}

// chain reply → { reference, lowerBound[], event, content, upperBound[], clockEvents }
void json_build_chain(Json& o, const os::ControlReply& r) {
  Json lower = Json::array(), upper = Json::array();
  std::string reference, event, content, clock_events;
  for (const auto& [k, v] : r.fields) {
    if (k == "lower") lower.push(json_clock_from_line(v));
    else if (k == "upper") upper.push(json_clock_from_line(v));
    else if (k == "reference") reference = v;
    else if (k == "event") event = v;
    else if (k == "content") content = v;
    else if (k == "clockEvents") clock_events = v;
  }
  o.set("reference", Json::str(reference));
  o.set("lowerBound", std::move(lower));
  o.set("event", Json::str(event));
  o.set("content", Json::str(content));
  o.set("upperBound", std::move(upper));
  if (!clock_events.empty()) o.set("clockEvents", json_value("clockEvents", clock_events));
}

// event-find reply (interleaved event/content) → { matches: [ {event, content}, … ] }
void json_build_find(Json& o, const os::ControlReply& r) {
  Json matches = Json::array();
  for (const auto& [k, v] : r.fields) {
    if (k == "event") { Json m = Json::object(); m.set("event", Json::str(v)); matches.push(std::move(m)); }
    else if (k == "content" && !matches.items.empty()) matches.items.back().set("content", Json::str(v));
  }
  o.set("matches", std::move(matches));
}

// Any other reply → object of typed fields; repeated keys become arrays.
void json_build_generic(Json& o, const os::ControlReply& r) {
  std::vector<std::string> order;
  std::map<std::string, std::vector<std::string>> grouped;
  for (const auto& [k, v] : r.fields) {
    if (!grouped.count(k)) order.push_back(k);
    grouped[k].push_back(v);
  }
  for (const auto& k : order) {
    const auto& vs = grouped[k];
    if (vs.size() == 1) {
      o.set(k, json_value(k, vs[0]));
    } else {
      Json a = Json::array();
      for (const auto& v : vs) a.push(json_value(k, v));
      o.set(k, std::move(a));
    }
  }
}

Json reply_to_json(const std::vector<std::string>& args, const os::ControlReply& r) {
  Json o = json_base(r);
  if (!r.ok) return o;
  if (args[0] == "chain") json_build_chain(o, r);
  else if (args[0] == "event" && args.size() > 1 && args[1] == "find") json_build_find(o, r);
  else json_build_generic(o, r);
  return o;
}

// A typed clock event (used by `proof show --json`, which has the full chain).
Json json_clock(const domain::ClockEvent& ce) {
  Json o = Json::object();
  o.set("creator", Json::str(hex_id(ce.creator)));
  o.set("hash", Json::str(hex(ce.hash)));
  o.set("timestamp", Json::str(std::to_string(ce.timestamp)));
  return o;
}
Json json_chain(const domain::EventChain& c) {
  Json o = Json::object();
  Json lower = Json::array(), upper = Json::array();
  for (const auto& ce : c.lower_bound) lower.push(json_clock(ce));
  for (const auto& ce : c.upper_bound) upper.push(json_clock(ce));
  Json ev = Json::object();
  ev.set("creator", Json::str(hex_id(c.event.creator)));
  ev.set("hash", Json::str(hex(c.event.hash)));
  ev.set("content", Json::str(printable(c.event.data)));
  o.set("lowerBound", std::move(lower));
  o.set("event", std::move(ev));
  o.set("upperBound", std::move(upper));
  return o;
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
  if (c == "event" && a.size() >= 3 && a[1] == "find") return join_from(2, "event-find");
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

// `loti db stat | backup --out <file> | restore <file>` — inspect and move the
// node's on-disk snapshot. Local events + the local clock chain are never dropped
// (the retention invariant enforced by the daemon).
int do_db(const std::vector<std::string>& args, const std::string& control, const std::string& out,
          bool json, bool quiet) {
  if (args.size() < 2) {
    std::fprintf(stderr, "usage: loti db <stat | backup --out <file> | restore <file>>\n");
    return 2;
  }
  const std::string& sub = args[1];
  std::string request;
  if (sub == "stat") {
    request = "db-stat";
  } else if (sub == "backup") {
    if (out.empty()) {
      std::fprintf(stderr, "loti: db backup requires --out <file>\n");
      return 2;
    }
    request = "db-backup " + out;
  } else if (sub == "restore") {
    if (args.size() < 3) {
      std::fprintf(stderr, "usage: loti db restore <file>\n");
      return 2;
    }
    request = "db-restore " + args[2];
  } else {
    std::fprintf(stderr, "loti: unknown db subcommand: %s\n", sub.c_str());
    return 2;
  }

  const auto reply = os::control_request(control, request);
  if (!reply) {
    std::fprintf(stderr, "loti: cannot reach daemon at %s\n", control.c_str());
    return 1;
  }
  if (json) {
    std::fputs(json_pretty(reply_to_json(args, *reply)).c_str(), stdout);
  } else if (!quiet) {
    if (!reply->ok) std::fprintf(stderr, "error(%d): %s\n", reply->code, reply->message.c_str());
    for (const auto& [k, v] : reply->fields) std::printf("%s: %s\n", k.c_str(), v.c_str());
  }
  return reply->code;
}

// `loti proof show <file> [--json]` — summary (human) or the full proof (JSON),
// no verification.
int do_proof_show(const std::vector<std::string>& args, bool json) {
  if (args.size() < 3) {
    std::fprintf(stderr, "usage: loti proof show <file> [--json]\n");
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

  if (json) {
    Json o = Json::object();
    o.set("loti_proof", Json::num(1));
    o.set("kind", Json::str(kind_name(p.kind)));
    Json ref = Json::object();
    ref.set("node", Json::str(hex_id(p.reference.node)));
    if (!p.reference.pubkey.empty()) ref.set("pubkey", Json::str("ed25519:" + hex(p.reference.pubkey)));
    o.set("reference", std::move(ref));
    o.set("chain", json_chain(p.chain));
    if (p.kind == proof::Kind::order) {
      o.set("chain2", json_chain(p.chain2));
      o.set("order", Json::str(order_name(p.order)));
    }
    std::fputs(json_pretty(o).c_str(), stdout);
    return 0;
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
    Json o = Json::object();
    o.set("valid", Json::boolean(res.valid));
    o.set("kind", Json::str(kind_name(p.kind)));
    o.set("reference", Json::str(hex_id(p.reference.node)));
    if (!p.reference.pubkey.empty()) o.set("pubkey", Json::str("ed25519:" + hex(p.reference.pubkey)));
    if (!res.valid) {
      o.set("reason", Json::str(res.reason));
    } else if (p.kind == proof::Kind::order) {
      o.set("order", Json::str(order_name(res.order)));
    } else {
      o.set("lower", Json::str(std::to_string(res.lower)));
      o.set("upper", Json::str(std::to_string(res.upper)));
    }
    if (!trust.empty()) o.set("trusted", Json::boolean(trusted));
    std::fputs(json_pretty(o).c_str(), stdout);
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

// ---- help ---------------------------------------------------------------
// Color codes sit OUTSIDE the padded field so column alignment is exact.
void help_title(const Style& s, const char* t) { std::printf("\n%s%s%s\n", s.title(), t, s.reset()); }
void help_group(const Style& s, const char* t) { std::printf("\n  %s%s%s\n", s.group(), t, s.reset()); }
void help_cmd(const Style& s, const char* name, const char* desc) {
  std::printf("    %s%-24s%s%s\n", s.cmd(), name, s.reset(), desc);
}
void help_opt(const Style& s, const char* name, const char* desc) {
  std::printf("  %s%-20s%s%s\n", s.opt(), name, s.reset(), desc);
}

void print_help(const Style& s) {
  std::printf("%sloti%s — command-line client for a LOTI node\n", s.bold(), s.reset());

  help_title(s, "USAGE");
  std::printf("  loti [global options] <command> [args]\n");

  help_title(s, "GLOBAL OPTIONS");
  help_opt(s, "--control <path>", "control socket (default: $LOTI_CONTROL or ./loti.sock)");
  help_opt(s, "--json", "machine-readable JSON output");
  help_opt(s, "--quiet, -q", "suppress human output (exit code only)");
  help_opt(s, "--home <dir>", "home directory for `init` (default: $HOME/.loti)");
  help_opt(s, "--out <file>", "output file for `prove`");
  help_opt(s, "--trust <node>", "trusted reference node for `verify` (repeatable)");
  help_opt(s, "--help, -h", "show this help");

  help_title(s, "COMMANDS");

  help_group(s, "Node & identity");
  help_cmd(s, "status", "node id, mode, and DAG counts");
  help_cmd(s, "key", "show node id and public key");
  help_cmd(s, "init", "create home dir, key, and config (local; no daemon)");

  help_group(s, "Events");
  help_cmd(s, "publish <text...>", "publish an event; prints its hash");
  help_cmd(s, "events", "list all known event hashes");
  help_cmd(s, "event show <hash|last>", "show one event (creator, content, refs)");
  help_cmd(s, "event find <text>", "find local events whose content contains <text>");

  help_group(s, "Discovery");
  help_cmd(s, "bounds <event>", "discover an event's time bounds");
  help_cmd(s, "chain <event>", "discover the enclosing chain (the complete path)");
  help_cmd(s, "order <a> <b>", "discover the order of two events");

  help_group(s, "Proofs — portable, offline-verifiable");
  help_cmd(s, "prove bounds <event>", "write a bounds proof to --out <file>");
  help_cmd(s, "prove order <a> <b>", "write an order proof to --out <file>");
  help_cmd(s, "prove chain <event>", "write a raw-chain proof to --out <file>");
  help_cmd(s, "verify <file>", "verify a proof offline (exit 0 valid / 6 invalid)");
  help_cmd(s, "proof show <file>", "summarize a proof (no verification)");

  help_group(s, "Peers");
  help_cmd(s, "peer add <id:ip:port>", "add a neighbor");
  help_cmd(s, "peer ls", "list neighbors");

  help_group(s, "Storage");
  help_cmd(s, "db stat", "snapshot store status (paths, counts, sizes)");
  help_cmd(s, "db backup --out <f>", "write a snapshot copy to a file");
  help_cmd(s, "db restore <file>", "load a snapshot into the running node");

  help_group(s, "Daemon");
  help_cmd(s, "stop", "stop the daemon");

  help_title(s, "ENVIRONMENT");
  help_opt(s, "LOTI_CONTROL", "default control socket path");
  help_opt(s, "NO_COLOR", "disable colored output");

  help_title(s, "EXAMPLES");
  std::printf("  %sloti publish \"manuscript sha256 deadbeef\"%s\n", s.dim(), s.reset());
  std::printf("  %sloti event find manuscript --json%s\n", s.dim(), s.reset());
  std::printf("  %sloti chain <hash> --json%s\n", s.dim(), s.reset());
  std::printf("  %sloti prove bounds <hash> --out proof.loti && loti verify proof.loti%s\n",
              s.dim(), s.reset());
  std::printf("  %sloti prove bounds <creator>:<hash> --out notary.loti   # prove a peer's event%s\n",
              s.dim(), s.reset());

  help_title(s, "EXIT CODES");
  std::printf("  %s0%s ok   %s2%s usage   %s4%s not found   %s6%s verification failed   %s1%s no daemon\n",
              s.bold(), s.reset(), s.bold(), s.reset(), s.bold(), s.reset(), s.bold(), s.reset(),
              s.bold(), s.reset());
  std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string control = std::getenv("LOTI_CONTROL") ? std::getenv("LOTI_CONTROL") : "loti.sock";
  std::string home = std::getenv("HOME") ? std::string(std::getenv("HOME")) + "/.loti" : ".loti";
  std::string out;
  std::vector<std::string> trust;
  bool json = false, quiet = false, help = false;
  std::vector<std::string> args;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--json") json = true;
    else if (a == "--quiet" || a == "-q") quiet = true;
    else if (a == "--help" || a == "-h") help = true;
    else if (a == "--control" && i + 1 < argc) control = argv[++i];
    else if (a == "--home" && i + 1 < argc) home = argv[++i];
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--trust" && i + 1 < argc) trust.push_back(argv[++i]);
    else args.push_back(a);
  }

  Style style;
  style.on = ::isatty(STDOUT_FILENO) && std::getenv("NO_COLOR") == nullptr;
  if (help || (!args.empty() && args[0] == "help")) {
    print_help(style);
    return 0;
  }

  if (args.empty()) {
    std::fprintf(stderr, "usage: loti [global options] <command> [args]\n"
                         "run `loti --help` for the full command list\n");
    return 2;
  }
  if (args[0] == "init") return do_init(home);
  if (args[0] == "prove") return do_prove(args, control, out, quiet);
  if (args[0] == "verify") return do_verify(args, trust, json);
  if (args[0] == "proof" && args.size() >= 2 && args[1] == "show") return do_proof_show(args, json);
  if (args[0] == "db") return do_db(args, control, out, json, quiet);

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
    std::fputs(json_pretty(reply_to_json(args, *reply)).c_str(), stdout);
  } else if (!quiet) {
    if (!reply->ok)
      std::fprintf(stderr, "error(%d): %s\n", reply->code, reply->message.c_str());
    for (const auto& [k, v] : reply->fields) std::printf("%s: %s\n", k.c_str(), v.c_str());
  }
  return reply->code;
}
