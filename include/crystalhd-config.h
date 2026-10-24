// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_NATIVE_CONFIG_H
#define CRYSTALHD_NATIVE_CONFIG_H

// Read-only, component-local schema-v1 defaults. No public DIL symbols, Python
// dependency, setenv, or per-frame I/O. See CONFIGURATION-SCHEMA.md.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace crystalhd_config {
namespace {
struct Error { const char *reason; };
static void Require(bool condition, const char *reason) {
  if (!condition) throw Error{reason};
}

struct Value {
  enum Type { Absent, UnsetType, Boolean, Integer, String } type;
  bool boolean;
  uint64_t integer;
  std::string string;
  Value() : type(Absent), boolean(false), integer(0) {}
  explicit Value(bool v) : type(Boolean), boolean(v), integer(0) {}
  explicit Value(uint64_t v) : type(Integer), boolean(false), integer(v) {}
  explicit Value(const char *v) : type(String), boolean(false), integer(0), string(v) {}
  explicit Value(std::string v) : type(String), boolean(false), integer(0), string(std::move(v)) {}
  static Value Unset() { Value v; v.type = UnsetType; return v; }
  bool operator==(const Value &v) const {
    return type == v.type && boolean == v.boolean && integer == v.integer && string == v.string;
  }
};
using Layer = std::map<std::string, Value>;
enum class Kind { Boolean, IntegerEnum, StringEnum, String, NonemptyString, ScaleWidth, OutputSize };
struct Rule { Kind kind; std::vector<Value> choices; Value fallback; };
using Rules = std::map<std::string, Rule>;
struct Preset { Layer settings; bool needs_acknowledgement; };
using Presets = std::map<std::string, Preset>;
#include "crystalhd-config-schema.h"

// Decode before splitting/stripping to match Python's Unicode str grammar,
// independently of the process locale. Escaped JSON surrogates are handled
// separately; surrogate codepoints are never legal in the input UTF-8 stream.
static std::u32string Decode(const std::string &bytes) {
  std::u32string result;
  for (size_t i = 0; i < bytes.size();) {
    uint32_t c = static_cast<unsigned char>(bytes[i++]);
    if (c < 128) { result.push_back(c); continue; }
    unsigned count;
    uint32_t minimum;
    if (c >= 0xc2 && c <= 0xdf) { count = 1; minimum = 128; c &= 31; }
    else if (c >= 0xe0 && c <= 0xef) { count = 2; minimum = 2048; c &= 15; }
    else if (c >= 0xf0 && c <= 0xf4) { count = 3; minimum = 65536; c &= 7; }
    else throw Error{"invalid-utf8"};
    Require(bytes.size() - i >= count, "invalid-utf8");
    while (count--) {
      unsigned char next = static_cast<unsigned char>(bytes[i++]);
      Require((next & 0xc0) == 0x80, "invalid-utf8");
      c = (c << 6) | (next & 63);
    }
    Require(c >= minimum && c <= 0x10ffff && !(c >= 0xd800 && c <= 0xdfff), "invalid-utf8");
    result.push_back(c);
  }
  Require(result.empty() || result[0] != 0xfeff, "utf8-bom");
  return result;
}
static void Append(std::string &out, uint32_t c) {
  if (c < 128) out.push_back(static_cast<char>(c));
  else if (c < 2048) {
    out.push_back(static_cast<char>(0xc0 | (c >> 6)));
    out.push_back(static_cast<char>(0x80 | (c & 63)));
  } else if (c < 65536) {
    out.push_back(static_cast<char>(0xe0 | (c >> 12)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
    out.push_back(static_cast<char>(0x80 | (c & 63)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (c >> 18)));
    out.push_back(static_cast<char>(0x80 | ((c >> 12) & 63)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
    out.push_back(static_cast<char>(0x80 | (c & 63)));
  }
}
static std::string Encode(const std::u32string &text) {
  std::string result;
  for (char32_t c : text) Append(result, c);
  return result;
}
static bool Space(char32_t c) {
  return (c >= 9 && c <= 13) || (c >= 28 && c <= 32) || c == 0x85 || c == 0xa0 ||
         c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 ||
         c == 0x202f || c == 0x205f || c == 0x3000;
}
static bool LineBreak(char32_t c) {
  return (c >= 10 && c <= 13) || (c >= 28 && c <= 30) || c == 0x85 || c == 0x2028 || c == 0x2029;
}
static std::u32string Strip(const std::u32string &text) {
  size_t first = 0, last = text.size();
  while (first < last && Space(text[first])) ++first;
  while (last > first && Space(text[last - 1])) --last;
  return text.substr(first, last - first);
}
static bool Name(const std::string &s, bool key = false) {
  if (s.empty() || (!key && s.size() > 64)) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    bool letter = c >= 'a' && c <= 'z';
    if (!key) letter = letter || (c >= 'A' && c <= 'Z');
    bool digit = c >= '0' && c <= '9';
    if (i == 0 ? !(letter || (!key && digit)) :
        !(letter || digit || c == '_' || c == '.' || (!key && c == '-'))) return false;
  }
  return true;
}
static unsigned Hex(char32_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  throw Error{"invalid-json-escape"};
}
static uint32_t Escape(const std::u32string &s, size_t &i) {
  Require(s.size() - i >= 4, "invalid-json-escape");
  uint32_t value = 0;
  for (unsigned n = 0; n < 4; ++n) value = (value << 4) | Hex(s[i++]);
  return value;
}
static Value ParseValue(const std::u32string &s) {
  if (s == U"unset") return Value::Unset();
  if (s == U"true") return Value(true);
  if (s == U"false") return Value(false);
  if (!s.empty() && s.front() == '"') {
    std::string result;
    size_t i = 1;
    bool closed = false;
    while (i < s.size()) {
      uint32_t c = s[i++];
      if (c == '"') { closed = true; break; }
      Require(c >= 32, "invalid-json-string");
      if (c == '\\') {
        Require(i < s.size(), "invalid-json-escape");
        c = s[i++];
        switch (c) {
          case '"': case '\\': case '/': break;
          case 'b': c = 8; break;
          case 'f': c = 12; break;
          case 'n': c = 10; break;
          case 'r': c = 13; break;
          case 't': c = 9; break;
          case 'u': {
            c = Escape(s, i);
            if (c >= 0xd800 && c <= 0xdbff && s.size() - i >= 6 && s[i] == '\\' && s[i+1] == 'u') {
              size_t next = i + 2;
              uint32_t low = Escape(s, next);
              if (low >= 0xdc00 && low <= 0xdfff) {
                c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
                i = next;
              }
            }
            break;
          }
          default: throw Error{"invalid-json-escape"};
        }
      }
      Require(c != 0, "nul-string");
      Append(result, c);
    }
    Require(closed && i == s.size(), "invalid-json-string");
    return Value(std::move(result));
  }
  Require(!s.empty() && s.size() <= 10 && (s.size() == 1 || s[0] != '0'), "invalid-value");
  uint64_t number = 0;
  for (char32_t c : s) {
    Require(c >= '0' && c <= '9', "invalid-value");
    number = number * 10 + c - '0';
  }
  return Value(number);
}
static bool OutputSize(const std::string &s) {
  size_t x = s.find('x');
  if (x == std::string::npos || x == 0 || x > 4 || s.size() - x - 1 == 0 || s.size() - x - 1 > 4) return false;
  uint32_t dimensions[2] = {0, 0};
  for (unsigned n = 0; n < 2; ++n) {
    const size_t begin = n ? x + 1 : 0, end = n ? s.size() : x;
    if (s[begin] == '0') return false;
    for (size_t i = begin; i < end; ++i) {
      if (s[i] < '0' || s[i] > '9') return false;
      dimensions[n] = dimensions[n] * 10 + s[i] - '0';
    }
    if (dimensions[n] > 4096 || dimensions[n] % 2) return false;
  }
  return dimensions[0] * dimensions[1] <= 4096 * 2160;
}
static bool Valid(const Rule &r, const Value &v) {
  if (v.type == Value::UnsetType) return true;
  switch (r.kind) {
    case Kind::Boolean: return v.type == Value::Boolean;
    case Kind::IntegerEnum: case Kind::StringEnum:
      for (const Value &choice : r.choices) if (v == choice) return true;
      return false;
    case Kind::String: return v.type == Value::String;
    case Kind::NonemptyString: return v.type == Value::String && !v.string.empty();
    case Kind::ScaleWidth: return v.type == Value::Integer &&
        (v.integer == 0 || (v.integer >= 128 && v.integer <= 1918 && !(v.integer % 2)));
    case Kind::OutputSize: return v.type == Value::String && OutputSize(v.string);
  }
  return false;
}
struct Document {
  Layer metadata, settings;
  std::map<std::string, Layer> profiles;
};
static Document Parse(const std::string &bytes, const Rules &rules) {
  Require(bytes.size() <= 65536, "oversize");
  std::u32string text = Decode(bytes);
  Document doc;
  std::set<std::string> sections;
  Layer *layer = nullptr;
  bool metadata = false;
  for (size_t begin = 0; begin < text.size();) {
    size_t end = begin;
    while (end < text.size() && !LineBreak(text[end])) ++end;
    std::u32string line = Strip(text.substr(begin, end - begin));
    begin = end < text.size() ? end + 1 : end;
    if (end < text.size() && text[end] == '\r' && begin < text.size() && text[begin] == '\n') ++begin;
    if (line.empty() || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      std::string name = Encode(line.substr(1, line.size() - 2));
      Require(sections.insert(name).second, "duplicate-section");
      metadata = name == "crystalhd";
      if (metadata) layer = &doc.metadata;
      else if (name == "settings") layer = &doc.settings;
      else {
        Require(name.compare(0, 8, "profile.") == 0 && Name(name.substr(8)), "unknown-section");
        layer = &doc.profiles[name.substr(8)];
      }
      continue;
    }
    size_t equals = line.find('=');
    Require(layer && equals != std::u32string::npos, "key-outside-section");
    std::string key = Encode(Strip(line.substr(0, equals)));
    Require(Name(key, true), "invalid-key");
    Value value = ParseValue(Strip(line.substr(equals + 1)));
    if (metadata) {
      if (key == "schema") Require(value == Value(uint64_t(1)), "unsupported-schema");
      else {
        Require(key == "preset" || key == "profile", "unknown-metadata");
        Require(value.type == Value::String && Name(value.string), "invalid-selector");
      }
    } else {
      auto rule = rules.find(key);
      Require(rule != rules.end(), "unknown-setting");
      Require(Valid(rule->second, value), "invalid-setting");
    }
    Require(layer->emplace(key, std::move(value)).second, "duplicate-key");
  }
  Require(doc.metadata.count("schema") == 1, "missing-schema");
  return doc;
}
static void Apply(Layer &values, const Layer &layer, const Rules &rules) {
  for (const auto &v : layer) values[v.first] = v.second.type == Value::UnsetType ? rules.at(v.first).fallback : v.second;
}
static Layer Resolve(const Document &system, const Document &user, const Rules &rules,
                     const Presets &presets, Layer *requests, bool *needs_ack) {
  Layer selectors = system.metadata;
  for (const auto &v : user.metadata) selectors[v.first] = v.second;
  std::string preset = selectors.count("preset") ? selectors.at("preset").string : "default";
  auto entry = presets.find(preset);
  Require(entry != presets.end(), "unavailable-preset");
  *needs_ack = entry->second.needs_acknowledgement;
  std::vector<const Layer *> layers{&entry->second.settings, &system.settings, &user.settings};
  if (selectors.count("profile")) {
    std::string name = selectors.at("profile").string;
    Require(system.profiles.count(name) || user.profiles.count(name), "unknown-profile");
    auto s = system.profiles.find(name), u = user.profiles.find(name);
    if (s != system.profiles.end()) layers.push_back(&s->second);
    if (u != user.profiles.end()) layers.push_back(&u->second);
  }
  Layer values;
  for (const auto &r : rules) values.emplace(r.first, r.second.fallback);
  for (const Layer *layer : layers) {
    Apply(values, *layer, rules);
    for (const auto &v : *layer) (*requests)[v.first] = v.second;
  }
  Require(!values.at("vaapi.h264.ready_remap").boolean ||
          (values.at("vaapi.h264.mode").string == "live" && values.at("vaapi.h264.low_latency").boolean), "ready-dependency");
  Require(!values.at("chromium.experimental_hw_decode").boolean || values.at("chromium.disable_gpu_sandbox").boolean, "sandbox-dependency");
  const bool hardware = values.at("player.decoder").string == "hardware";
  const uint64_t scale = values.at("gstreamer.crystalhddec.scale_width").integer;
  Require(!scale || hardware, "scale-dependency");
  Require(values.at("player.software_output_size").type == Value::Absent || (hardware && scale), "output-size-dependency");
  return values;
}

struct Fd {
  int value;
  explicit Fd(int v) : value(v) {}
  ~Fd() { if (value >= 0) close(value); }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
};
static std::string ReadFile(const std::string &path, uid_t owner) {
  Require(!path.empty() && path[0] == '/' && path.back() != '/', "nonabsolute-path");
  size_t slash = path.find_last_of('/');
  std::string parent = slash == 0 ? "/" : path.substr(0, slash), name = path.substr(slash + 1);
  Fd directory(open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (directory.value < 0 && errno == ENOENT) return {};
  Require(directory.value >= 0, "unsafe-directory");
  struct stat info;
  Require(!fstat(directory.value, &info) && S_ISDIR(info.st_mode) && info.st_uid == owner && !(info.st_mode & 0022), "unsafe-directory");
  if (fstatat(directory.value, name.c_str(), &info, AT_SYMLINK_NOFOLLOW)) {
    if (errno == ENOENT) return {};
    throw Error{"unreadable-file"};
  }
  Require(S_ISREG(info.st_mode), "nonregular-file");
  Fd file(openat(directory.value, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  if (file.value < 0 && errno == ENOENT) return {};
  Require(file.value >= 0, "unreadable-file");
  Require(!fstat(file.value, &info) && S_ISREG(info.st_mode) && info.st_uid == owner && !(info.st_mode & 0022), "unsafe-file");
  std::string data;
  char block[4096];
  while (data.size() <= 65536) {
    size_t remaining = 65537 - data.size();
    ssize_t n = read(file.value, block, remaining < sizeof(block) ? remaining : sizeof(block));
    if (n < 0 && errno == EINTR) continue;
    Require(n >= 0, "unreadable-file");
    if (!n) break;
    data.append(block, static_cast<size_t>(n));
  }
  Require(data.size() <= 65536, "oversize");
  // Missing is distinct from a present, invalid empty document.
  return data.empty() ? std::string("\n") : data;
}
static std::string UserPath() {
  const char *base = std::getenv("XDG_CONFIG_HOME");
  if (base && *base) {
    Require(base[0] == '/', "nonabsolute-xdg");
    return std::string(base) + "/crystalhd/config.ini";
  }
  base = std::getenv("HOME");
  if (!base || !*base) return {};
  Require(base[0] == '/', "nonabsolute-home");
  return std::string(base) + "/.config/crystalhd/config.ini";
}
using EnvironmentMap = std::map<std::string, std::string>;
static EnvironmentMap Project(const Layer &values, const Layer &requests, bool needs_ack) {
  Require(!needs_ack, "preset-requires-config-run-acknowledgement");
  EnvironmentMap result;
  struct Adapter { const char *key, *variable; };
  const Adapter adapters[] = {
    {"libcrystalhd.rx_pool_count", "CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT"},
    {"vaapi.h264.mode", "CRYSTALHD_VAAPI_LIVE_H264"},
    {"vaapi.h264.low_latency", "CRYSTALHD_VAAPI_LOW_LATENCY_H264"},
    {"vaapi.h264.ready_remap", "CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP"},
    {"vaapi.i915_wc", "CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC"},
    {"vaapi.debug", "CRYSTALHD_VAAPI_DEBUG"},
    {"vaapi.trace", "CRYSTALHD_VAAPI_TRACE"}
  };
  for (const auto &adapter : adapters) {
    auto requested = requests.find(adapter.key);
    if (requested == requests.end()) continue;
    const bool raw_override = std::getenv(adapter.variable) != nullptr;
    if (requested->second.type == Value::UnsetType) continue;
    const Value &v = values.at(adapter.key);
    if (!std::strcmp(adapter.key, "libcrystalhd.rx_pool_count")) {
      if (v.integer == 8) {
        Require(raw_override, "experimental-setting-requires-config-run");
        continue;  // Never cache a managed experimental activation.
      }
      result.emplace(adapter.variable, std::to_string(v.integer));
    } else if (!std::strcmp(adapter.key, "vaapi.h264.mode")) {
      if (v.string == "live") {
        Require(raw_override, "experimental-setting-requires-config-run");
        continue;
      }
      if (v.string == "strict") result.emplace(adapter.variable, "0");
    } else if (v.boolean) {
      if (std::strcmp(adapter.key, "vaapi.debug") && std::strcmp(adapter.key, "vaapi.trace")) {
        Require(raw_override, "experimental-setting-requires-config-run");
        continue;
      }
      result.emplace(adapter.variable, "1");
    }
  }
  return result;
}
struct Snapshot {
  EnvironmentMap environment;
  const char *failure = nullptr;
};
static Snapshot ReadDefaults(const std::string &system_path, const std::string &user_path,
                             uid_t system_owner, uid_t user_owner) noexcept {
  Snapshot result;
  try {
    Rules rules = MakeRules();
    const std::string system = system_path.empty() ? std::string() : ReadFile(system_path, system_owner);
    const std::string user = user_path.empty() ? std::string() : ReadFile(user_path, user_owner);
    if (system.empty() && user.empty()) return result;
    Layer requests;
    bool needs_ack = false;
    Layer values = Resolve(system.empty() ? Document{} : Parse(system, rules),
                           user.empty() ? Document{} : Parse(user, rules),
                           rules, MakePresets(), &requests, &needs_ack);
    result.environment = Project(values, requests, needs_ack);
  } catch (const Error &error) {
    result.environment.clear();
    result.failure = error.reason;
  } catch (...) {
    result.environment.clear();
    result.failure = "reader-resource-failure";
  }
  return result;
}
static Snapshot Load() noexcept {
  Snapshot result;
  try {
    const char *disabled = std::getenv("CRYSTALHD_CONFIG_DISABLE");
    if ((disabled && !std::strcmp(disabled, "1")) || getuid() != geteuid() || getgid() != getegid()) return result;
    result = ReadDefaults("/etc/crystalhd/config.ini", UserPath(), 0, getuid());
  } catch (const Error &error) {
    result.failure = error.reason;
  } catch (...) {
    result.failure = "reader-resource-failure";
  }
  if (result.failure) std::fprintf(stderr, "crystalhd-config: managed defaults ignored (%s); using native environment/defaults\n", result.failure);
  return result;
}
static const Snapshot &Defaults() noexcept {
  static const Snapshot snapshot = Load();
  return snapshot;
}
static const char *Lookup(const Snapshot &snapshot, const char *name) noexcept {
  const char *raw = std::getenv(name);
  if (raw) return raw;  // Presence, including empty/invalid text, is authoritative.
  // Avoid a temporary allocating std::string in this noexcept hot-path lookup.
  for (const auto &value : snapshot.environment)
    if (!std::strcmp(value.first.c_str(), name)) return value.second.c_str();
  return nullptr;
}
static inline const char *Environment(const char *name) noexcept {
  const int saved_errno = errno;
  const char *value = Lookup(Defaults(), name);
  errno = saved_errno;
  return value;
}
}  // component-local implementation
}  // namespace crystalhd_config
#endif
