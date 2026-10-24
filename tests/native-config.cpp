// SPDX-License-Identifier: LGPL-2.1-or-later
// Exercises the production header without loading a module, DIL or VA driver.
#include "crystalhd-config.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

// Test-only redirect of the fixed system directory: never read host defaults.
extern "C" uid_t __real_geteuid();
extern "C" gid_t __real_getegid();
extern "C" uid_t __wrap_geteuid() {
  return __real_geteuid() + (std::getenv("TEST_NATIVE_CONFIG_UID_MISMATCH") ? 1 : 0);
}
extern "C" gid_t __wrap_getegid() {
  return __real_getegid() + (std::getenv("TEST_NATIVE_CONFIG_GID_MISMATCH") ? 1 : 0);
}
extern "C" int __real_open(const char *, int, ...);
extern "C" int __wrap_open(const char *path, int flags, ...) {
  const char *directory = std::getenv("TEST_NATIVE_CONFIG_SYSTEM_DIRECTORY");
  if (directory && !std::strcmp(path, "/etc/crystalhd"))
    return __real_open(directory, flags);
  return __real_open(path, flags);
}

using namespace crystalhd_config;
static std::string Read(const char *path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
static std::string HexBytes(const std::string &s) {
  std::string result;
  for (unsigned char c : s) { result += "0123456789abcdef"[c >> 4]; result += "0123456789abcdef"[c & 15]; }
  return result;
}
static std::string Unhex(const std::string &s) {
  std::string result;
  Require(s.size() % 2 == 0, "invalid-test-input");
  for (size_t i = 0; i < s.size(); i += 2) result += static_cast<char>((Hex(s[i]) << 4) | Hex(s[i+1]));
  return result;
}
static void Print(const std::string &section, const Layer &layer) {
  for (const auto &v : layer) {
    std::cout << section << ':' << v.first << '=';
    switch (v.second.type) {
      case Value::Absent: std::cout << 'a'; break;
      case Value::UnsetType: std::cout << 'u'; break;
      case Value::Boolean: std::cout << 'b' << v.second.boolean; break;
      case Value::Integer: std::cout << 'i' << v.second.integer; break;
      case Value::String: std::cout << 's' << HexBytes(v.second.string); break;
    }
    std::cout << '\t';
  }
}
int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "batch-parse") {
    std::string line;
    while (std::getline(std::cin, line)) {
      try {
        Document d = Parse(Unhex(line), MakeRules());
        std::cout << "OK\t";
        Print("crystalhd", d.metadata); Print("settings", d.settings);
        for (const auto &p : d.profiles) Print("profile." + p.first, p.second);
      } catch (const Error &) { std::cout << "FAIL"; }
      std::cout << '\n';
    }
    return 0;
  }
  if (argc >= 4 && std::string(argv[1]) == "resolve") {
    try {
      Rules rules = MakeRules(); Layer requests; bool ack = false;
      Layer values = Resolve(Parse(Read(argv[2]), rules), Parse(Read(argv[3]), rules), rules, MakePresets(), &requests, &ack);
      std::cout << "OK\tack=" << ack << '\t'; Print("values", values); Print("requests", requests); std::cout << '\n';
    } catch (const Error &) { std::cout << "FAIL\n"; }
    return 0;
  }
  if (argc >= 4 && std::string(argv[1]) == "paths") {
    Snapshot s = ReadDefaults(argv[2], argv[3], getuid(), getuid());
    if (s.failure) std::cout << "FAIL:" << s.failure << '\n';
    for (int i = 4; i < argc; ++i) { const char *v = Lookup(s, argv[i]); std::cout << argv[i] << '=' << (v ? v : "<absent>") << '\n'; }
    return 0;
  }
  if (argc >= 3 && std::string(argv[1]) == "native") {
    for (int i = 2; i < argc; ++i) {
      errno = EDOM;
      const char *v = Environment(argv[i]);
      if (errno != EDOM) return 2;
      std::cout << argv[i] << '=' << (v ? v : "<absent>") << '\n';
    }
    return 0;
  }
  if (argc == 2 && std::string(argv[1]) == "raw-change") {
    const char *before = Environment("CRYSTALHD_VAAPI_DEBUG");
    std::cout << (before ? before : "<absent>") << ':';
    unsetenv("CRYSTALHD_VAAPI_DEBUG");
    const char *after = Environment("CRYSTALHD_VAAPI_DEBUG");
    std::cout << (after ? after : "<absent>") << '\n';
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "snapshot") {
    const char *before = Environment("CRYSTALHD_VAAPI_DEBUG");
    std::ofstream file(argv[2]); file << "[crystalhd]\nschema = 1\n[settings]\nvaapi.debug = false\n"; file.close();
    const char *after = Environment("CRYSTALHD_VAAPI_DEBUG");
    std::cout << (before ? before : "<absent>") << ':' << (after ? after : "<absent>") << '\n';
    return 0;
  }
  return 2;
}
