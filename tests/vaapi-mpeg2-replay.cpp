// SPDX-License-Identifier: LGPL-2.1-or-later
// Transport/reference model tests. No decoder/device access.
#include "../filters/vaapi/crystalhd-mpeg2-replay.h"
#include "../filters/vaapi/crystalhd-decode-replay.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

using Replay = CrystalHDMpeg2Replay;
using Kind = Replay::Kind;
static size_t checks = 0, failures = 0, groups = 0;
#define CHECK(e) do { ++checks; if (!(e)) { ++failures; \
  std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #e); } } while (0)
#define REQUIRE(e) do { ++checks; if (!(e)) { ++failures; \
  std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #e); return; } } while (0)
static std::vector<uint8_t> Bytes(uint64_t token) {
  std::vector<uint8_t> bytes{0, 0, 1, 0xb3, 0, 0, 1, 0};
  for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(token >> (8*i)));
  return bytes;  // Opaque owned bytes, intentionally not a decodable bitstream.
}
static bool Add(Replay &r, uint64_t token, Kind kind, uint64_t f = 0, uint64_t b = 0) {
  return r.Append(token, kind, f, b, Bytes(token));
}
static std::vector<uint64_t> Send(Replay &r) {
  std::vector<uint64_t> sent;
  while (const auto *u = r.NextInput()) {
    CHECK(u->bytes == Bytes(u->timestamp));
    sent.push_back(u->timestamp);
    CHECK(r.InputSent());
  }
  return sent;
}
static void FinishNew(Replay &r) {
  auto sent = Send(r);
  for (uint64_t token : sent) CHECK(r.Observe(token) == Replay::Output::New);
}
static void Invariants(const Replay &r) {
  size_t total = 0;
  uint64_t prior = 0;
  for (const auto &u : r.units()) {
    CHECK(u.timestamp > prior);
    CHECK(u.bytes == Bytes(u.timestamp));
    CHECK(r.Find(u.root) && r.Find(u.root)->kind == Kind::I);
    if (u.forward) CHECK(r.Find(u.forward) != nullptr);
    if (u.backward) CHECK(r.Find(u.backward) != nullptr);
    total += u.bytes.size();
    prior = u.timestamp;
  }
  CHECK(total == r.cached_bytes());
  CHECK(!r.units().empty() && r.units().front().kind == Kind::I);
}

static void ExistingIdrCounterexample() {
  CrystalHDDecodeReplay old;
  for (uint64_t token = 1; token <= 3; ++token) {
    CHECK(old.Append(token, token != 2, Bytes(token))); // I0, P3, open-GOP I6.
    CHECK(old.InputSent());
    CHECK(old.Observe(token) == CrystalHDDecodeReplay::Output::New);
  }
  // Actual current helper discards the previous anchor chain: there is now no
  // P3 AU with which to rebuild a subsequently accepted leading B4.
  CHECK(old.cached_pictures() == 1);
  CHECK(old.Append(4, false, Bytes(4)));
  CHECK(old.cached_pictures() == 2);
  std::puts("BASELINE COUNTEREXAMPLE: mapping MPEG-2 I to IDR loses P3 before B4");
  Replay r;
  CHECK(Add(r, 1, Kind::I)); FinishNew(r);
  CHECK(Add(r, 2, Kind::P, 1)); FinishNew(r);
  CHECK(Add(r, 3, Kind::I)); FinishNew(r);
  CHECK(r.Find(1) && r.Find(2) && r.Find(3));
  CHECK(Add(r, 4, Kind::B, 2, 3));
  CHECK(r.Find(4)->root == 1);
  Invariants(r);
}

static void OpenGopRetention() {
  Replay r;
  CHECK(Add(r, 1, Kind::I)); FinishNew(r);
  CHECK(Add(r, 2, Kind::P, 1)); FinishNew(r);
  CHECK(Add(r, 3, Kind::B, 1, 2)); FinishNew(r);
  CHECK(!r.Find(3));
  CHECK(Add(r, 4, Kind::I)); FinishNew(r);
  CHECK(Add(r, 5, Kind::B, 2, 4));
  CHECK(Add(r, 6, Kind::B, 2, 4));
  CHECK(Add(r, 7, Kind::P, 4));
  auto sent = Send(r);
  CHECK(sent == std::vector<uint64_t>({5, 6, 7}));
  CHECK(r.Observe(7) == Replay::Output::New);
  CHECK(r.Find(1) && r.Find(2)); // Pending leading B retains its old anchor.
  CHECK(r.Observe(5) == Replay::Output::New);
  CHECK(r.Find(1) && r.Find(2));
  CHECK(r.Observe(6) == Replay::Output::New);
  CHECK(!r.Find(1) && !r.Find(2) && !r.Find(5) && !r.Find(6));
  CHECK(r.units().front().timestamp == 4);
  CHECK(Add(r, 8, Kind::B, 4, 7));
  Invariants(r);
}

static void SealBeforeLeadingB() {
  Replay r;
  CHECK(Add(r, 1, Kind::I));
  CHECK(Add(r, 2, Kind::P, 1));
  CHECK(Add(r, 3, Kind::I));
  auto first = Send(r);
  CHECK(r.Seal());
  CHECK(Add(r, 4, Kind::B, 2, 3));
  CHECK(r.NextInput() == nullptr);
  CHECK(!r.NeedsRestart());
  for (uint64_t token : {3ULL, 1ULL, 2ULL})
    CHECK(r.Observe(token) == Replay::Output::New);
  CHECK(r.EndOfSequence());
  CHECK(r.NeedsRestart());
  CHECK(r.Restarted());
  auto replayed = Send(r);
  CHECK(replayed == std::vector<uint64_t>({1, 2, 3, 4}));
  CHECK(r.Seal());
  CHECK(r.Observe(4) == Replay::Output::New);
  for (uint64_t token : first) CHECK(r.Observe(token) == Replay::Output::Duplicate);
  CHECK(r.EndOfSequence());
  CHECK(!r.NeedsRestart());
  CHECK(r.replay_work() == 3);
}

static void SealedPruneAndQueuedAnchor() {
  Replay r;
  CHECK(Add(r, 1, Kind::I));
  CHECK(Add(r, 2, Kind::P, 1));
  CHECK(Add(r, 3, Kind::I));
  CHECK(Add(r, 4, Kind::B, 2, 3));
  auto first = Send(r);
  CHECK(r.Seal());
  CHECK(Add(r, 5, Kind::P, 3));
  CHECK(Add(r, 6, Kind::B, 3, 5));
  for (uint64_t token : first) CHECK(r.Observe(token) == Replay::Output::New);
  CHECK(r.Find(1) && r.Find(4)); // No active sealed-batch mutation.
  CHECK(r.EndOfSequence());
  CHECK(r.Restarted());
  CHECK(!r.Find(1) && !r.Find(2) && !r.Find(4));
  auto next = Send(r);
  CHECK(next == std::vector<uint64_t>({3, 5, 6}));
  CHECK(r.Observe(3) == Replay::Output::Duplicate);
  CHECK(r.Observe(5) == Replay::Output::New);
  CHECK(r.Observe(6) == Replay::Output::New);
  Invariants(r);
}

static void InflightCompletedAnchorCannotPrune() {
  Replay r;
  CHECK(Add(r, 1, Kind::I));
  CHECK(Add(r, 2, Kind::P, 1));
  auto first = Send(r); CHECK(r.Seal());
  CHECK(Add(r, 3, Kind::I));
  for (uint64_t t : first) CHECK(r.Observe(t) == Replay::Output::New);
  CHECK(r.EndOfSequence()); CHECK(r.Restarted());
  auto active = Send(r);
  CHECK(Add(r, 4, Kind::P, 3));
  CHECK(r.Find(1) && r.Find(2)); // They are completed, but in-flight again.
  CHECK(r.Observe(3) == Replay::Output::New);
  CHECK(r.Observe(1) == Replay::Output::Duplicate);
  CHECK(r.Find(1)); // Replaying P2 still needs root 1 until it retires.
  CHECK(r.Observe(2) == Replay::Output::Duplicate);
  CHECK(!r.Find(1) && !r.Find(2));
  CHECK(active == std::vector<uint64_t>({1, 2, 3}));
  Invariants(r);
}

// A deliberately small VA-like identity adapter. Actual backend surface BUSY,
// context/canonical-owner and decoded allocation checks remain prerequisites.
// Resolving IDs copies tokens; it never stores a mutable surface-map pointer.
struct Bindings {
  std::map<unsigned, uint64_t> tokens;
  std::map<unsigned, std::shared_ptr<const std::vector<uint8_t>>> pixels;
  uint64_t Resolve(unsigned id) const {
    auto it = tokens.find(id); return it == tokens.end() ? 0 : it->second;
  }
  void Bind(unsigned id, uint64_t token) { tokens[id] = token; }
};
static void SurfaceReuseAndHeldOutput() {
  Replay r; Bindings s;
  CHECK(Add(r, 1, Kind::I)); s.Bind(10, 1); FinishNew(r);
  s.pixels[10] = std::make_shared<const std::vector<uint8_t>>(Bytes(1));
  const auto held = s.pixels[10];
  CHECK(Add(r, 2, Kind::P, s.Resolve(10))); s.Bind(20, 2); FinishNew(r);
  CHECK(Add(r, 3, Kind::I)); s.Bind(30, 3); FinishNew(r);
  CHECK(Add(r, 4, Kind::B, s.Resolve(20), s.Resolve(30)));
  // Rebinding a public ID cannot alter the already queued B's dependencies.
  // This operation models identity only, not authorization to overwrite a busy
  // backend surface, which remains independently enforced there.
  s.Bind(20, 999);
  CHECK(r.Find(4)->forward == 2 && r.Find(4)->backward == 3);
  FinishNew(r);
  s.pixels[40] = std::make_shared<const std::vector<uint8_t>>(Bytes(4));
  const auto held_b = s.pixels[40];
  CHECK(!r.Find(4));
  CHECK(Add(r, 5, Kind::P, 3)); FinishNew(r);
  CHECK(!r.Find(1) && !r.Find(2));
  s.Bind(10, 5);
  s.pixels[10] = std::make_shared<const std::vector<uint8_t>>(Bytes(5));
  CHECK(*held == Bytes(1) && *held_b == Bytes(4));
  CHECK(*s.pixels[10] == Bytes(5));
  Invariants(r);
}

static void ImmutableOwnedBytes() {
  Replay r;
  auto original = Bytes(1);
  CHECK(r.Append(1, Kind::I, 0, 0, original));
  original.assign(128, 0xff);
  CHECK(r.Find(1)->bytes == Bytes(1));
  auto sent = Send(r); CHECK(r.Seal());
  CHECK(Add(r, 2, Kind::P, 1));
  CHECK(r.Observe(1) == Replay::Output::New);
  CHECK(r.EndOfSequence()); CHECK(r.Restarted());
  CHECK(Send(r) == std::vector<uint64_t>({1, 2}));
  CHECK(sent == std::vector<uint64_t>({1}));
}

static void InvalidReferences() {
  { Replay r; CHECK(!Add(r, 1, Kind::P, 9)); CHECK(r.failed()); }
  { Replay r; CHECK(!Add(r, 1, Kind::B, 8, 9)); }
  { Replay r; CHECK(!Add(r, 1, Kind::I, 9)); }
  { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(!Add(r, 2, Kind::P, 1, 1)); }
  { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(!Add(r, 2, Kind::B, 1, 1)); }
  { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::P, 1));
    CHECK(!Add(r, 3, Kind::B, 2, 1)); }
  { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::P, 1));
    CHECK(Add(r, 3, Kind::B, 1, 2)); CHECK(!Add(r, 4, Kind::P, 3)); }
  { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::P, 1));
    CHECK(Add(r, 3, Kind::I)); CHECK(!Add(r, 4, Kind::P, 1)); }
  { Replay r; CHECK(!Add(r, 0, Kind::I)); }
  { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(!Add(r, 1, Kind::I)); }
  { Replay r; CHECK(Add(r, 2, Kind::I)); CHECK(!Add(r, 1, Kind::I)); }
  { Replay r; CHECK(!r.Append(1, Kind::I, 0, 0, {})); }
  { Replay r; CHECK(!Add(r, 1, static_cast<Kind>(99))); }
}

static void InvalidTransitionsAndOutput() {
  { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(!r.Seal()); }
  { Replay r; CHECK(Add(r, 1, Kind::I)); Send(r); CHECK(!r.EndOfSequence()); }
  { Replay r; CHECK(Add(r, 1, Kind::I)); Send(r); CHECK(r.Seal());
    CHECK(!r.EndOfSequence()); CHECK(r.outstanding() == 1); }
  { Replay r; CHECK(Add(r, 1, Kind::I));
    CHECK(r.Observe(1) == Replay::Output::Invalid); CHECK(r.failed()); }
  { Replay r; CHECK(Add(r, 1, Kind::I)); Send(r);
    CHECK(r.Observe(999) == Replay::Output::Unknown); CHECK(r.outstanding() == 1); }
  { Replay r; CHECK(Add(r, 1, Kind::I)); Send(r);
    CHECK(r.Observe(1) == Replay::Output::New);
    CHECK(r.Observe(1) == Replay::Output::Duplicate); CHECK(!r.failed()); }
  { Replay r; CHECK(Add(r, 1, Kind::I)); Send(r); CHECK(r.Seal());
    CHECK(Add(r, 2, Kind::P, 1)); CHECK(!r.Restarted()); }
  { Replay r; CHECK(!r.InputSent()); }
}

static void CacheBounds() {
  Replay::Limits l;
  l.picture_bytes = 15; { Replay r(l); CHECK(!Add(r, 1, Kind::I)); }
  l.picture_bytes = 16; l.cache_bytes = 31;
  { Replay r(l); CHECK(Add(r, 1, Kind::I)); CHECK(!Add(r, 2, Kind::P, 1)); }
  l.cache_bytes = 4096; l.pictures = 2;
  { Replay r(l); CHECK(Add(r, 1, Kind::I)); FinishNew(r);
    CHECK(Add(r, 2, Kind::P, 1)); FinishNew(r); CHECK(!Add(r, 3, Kind::P, 2)); }
  l.cache_bytes = 0;
  { Replay r(l); CHECK(!Add(r, 1, Kind::I)); }
  l.cache_bytes = 4096; l.pictures = 0;
  { Replay r(l); CHECK(!Add(r, 1, Kind::I)); }
}

static void ReplayBoundNotResetByBRemoval() {
  Replay::Limits l; l.replay_pictures = 3;
  Replay r(l);
  CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::P, 1));
  auto sent = Send(r); CHECK(r.Seal()); CHECK(Add(r, 3, Kind::B, 1, 2));
  for (uint64_t t : sent) CHECK(r.Observe(t) == Replay::Output::New);
  CHECK(r.EndOfSequence()); CHECK(r.Restarted());
  sent = Send(r); CHECK(r.Seal()); CHECK(Add(r, 4, Kind::B, 1, 2));
  for (uint64_t t : sent)
    CHECK(r.Observe(t) == (t == 3 ? Replay::Output::New : Replay::Output::Duplicate));
  CHECK(r.EndOfSequence()); CHECK(r.Restarted());
  CHECK(!r.Find(3));
  CHECK(r.replay_work() == 2);
  CHECK(r.NextInput() && r.NextInput()->timestamp == 1); CHECK(r.InputSent());
  CHECK(r.replay_work() == 3);
  CHECK(r.NextInput() == nullptr && r.failed());
  CHECK(r.Find(4) && !r.Find(4)->completed); // Fail closed, never drop pending.
}

static void RootAdvanceResetsWork() {
  Replay::Limits l; l.replay_pictures = 2;
  Replay r(l);
  CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::P, 1));
  auto sent = Send(r); CHECK(r.Seal()); CHECK(Add(r, 3, Kind::I));
  for (uint64_t t : sent) CHECK(r.Observe(t) == Replay::Output::New);
  CHECK(r.EndOfSequence()); CHECK(r.Restarted());
  sent = Send(r); CHECK(r.replay_work() == 2); CHECK(r.Seal());
  CHECK(Add(r, 4, Kind::P, 3));
  for (uint64_t t : sent)
    CHECK(r.Observe(t) == (t == 3 ? Replay::Output::New : Replay::Output::Duplicate));
  CHECK(r.EndOfSequence()); CHECK(r.Restarted());
  CHECK(r.replay_work() == 0); CHECK(!r.Find(1));
  CHECK(Send(r) == std::vector<uint64_t>({3, 4}));
  CHECK(!r.failed());
}

static void OutputOrderPermutations() {
  std::array<uint64_t, 5> order{1, 2, 3, 4, 5};
  do {
    Replay r;
    CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::P, 1));
    CHECK(Add(r, 3, Kind::I)); CHECK(Add(r, 4, Kind::B, 2, 3));
    CHECK(Add(r, 5, Kind::P, 3)); Send(r);
    std::unordered_set<uint64_t> pending(order.begin(), order.end());
    for (uint64_t token : order) {
      CHECK(r.Observe(token) == Replay::Output::New);
      pending.erase(token);
      for (uint64_t p : pending) CHECK(r.Find(p) && !r.Find(p)->completed);
      Invariants(r);
    }
    CHECK(r.cached_pictures() == 2 && r.units().front().timestamp == 3);
    CHECK(!r.failed());
  } while (std::next_permutation(order.begin(), order.end()));
}

static void SustainedBoundedGops() {
  Replay::Limits l; l.pictures = 12; l.cache_bytes = 12*16;
  Replay r(l); uint64_t next = 1;
  CHECK(Add(r, next++, Kind::I)); FinishNew(r);
  CHECK(Add(r, next, Kind::P, next-1)); ++next; FinishNew(r);
  for (unsigned g = 0; g < 1000; ++g) {
    uint64_t prior = r.newest_anchor();
    uint64_t current = next++;
    CHECK(Add(r, current, Kind::I)); FinishNew(r);
    CHECK(Add(r, next++, Kind::B, prior, current));
    CHECK(Add(r, next++, Kind::B, prior, current));
    uint64_t p = next++;
    CHECK(Add(r, p, Kind::P, current));
    auto sent = Send(r);
    for (auto it = sent.rbegin(); it != sent.rend(); ++it)
      CHECK(r.Observe(*it) == Replay::Output::New);
    CHECK(r.cached_pictures() == 2);
    CHECK(r.units().front().timestamp == current);
    CHECK(Add(r, next++, Kind::B, current, p)); FinishNew(r);
    Invariants(r);
  }
  CHECK(!r.failed()); CHECK(r.cached_bytes() == 32);
}

static void SealAtEveryPictureBoundary() {
  struct Picture { Kind kind; uint64_t f, b; };
  const Picture pictures[] = {
    {Kind::I, 0, 0}, {Kind::P, 1, 0}, {Kind::B, 1, 2}, {Kind::B, 1, 2},
    {Kind::I, 0, 0}, {Kind::B, 2, 5}, {Kind::B, 2, 5}, {Kind::P, 5, 0},
    {Kind::B, 5, 8}, {Kind::B, 5, 8}, {Kind::I, 0, 0}, {Kind::B, 8, 11},
    {Kind::P, 11, 0}, {Kind::B, 11, 13}, {Kind::I, 0, 0},
    {Kind::B, 13, 15}, {Kind::P, 15, 0}
  };
  constexpr size_t count = sizeof(pictures) / sizeof(pictures[0]);
  Replay r;
  std::map<uint64_t, std::vector<uint8_t>> first_submission;
  std::map<uint64_t, unsigned> public_completions;
  CHECK(Add(r, 1, pictures[0].kind));
  for (size_t n = 0; n < count; ++n) {
    std::vector<uint64_t> sent;
    while (const auto *u = r.NextInput()) {
      auto old = first_submission.find(u->timestamp);
      if (old == first_submission.end()) first_submission[u->timestamp] = u->bytes;
      else CHECK(old->second == u->bytes);
      CHECK(u->bytes == Bytes(u->timestamp));
      sent.push_back(u->timestamp);
      CHECK(r.InputSent());
    }
    REQUIRE(!sent.empty());
    CHECK(r.Seal());
    if (n + 1 < count) {
      const auto &p = pictures[n+1];
      CHECK(Add(r, n+2, p.kind, p.f, p.b));
      CHECK(r.NextInput() == nullptr);
    }
    // Reverse completion order deliberately keeps earlier dependencies active
    // after dependent outputs complete. All pending entries must survive.
    for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
      auto result = r.Observe(*it);
      CHECK(result == (public_completions[*it] ? Replay::Output::Duplicate
                                             : Replay::Output::New));
      if (result == Replay::Output::New) ++public_completions[*it];
    }
    CHECK(r.EndOfSequence());
    CHECK(r.NeedsRestart() == (n + 1 < count));
    if (n + 1 < count) CHECK(r.Restarted());
    CHECK(!r.failed());
    Invariants(r);
  }
  CHECK(first_submission.size() == count);
  CHECK(public_completions.size() == count);
  for (const auto &entry : public_completions) CHECK(entry.second == 1);
}

int main() {
  const std::pair<const char *, void (*)()> tests[] = {
    {"actual H264 IDR counterexample", ExistingIdrCounterexample},
    {"open GOP retention and completed B omission", OpenGopRetention},
    {"seal between anchor and leading B", SealBeforeLeadingB},
    {"sealed watermark and prefix pruning", SealedPruneAndQueuedAnchor},
    {"replaying completed anchor retained", InflightCompletedAnchorCannotPrune},
    {"surface tokens and held decoded payload", SurfaceReuseAndHeldOutput},
    {"immutable owned original bytes", ImmutableOwnedBytes},
    {"invalid/dummy/stale references", InvalidReferences},
    {"output and lifecycle rejection", InvalidTransitionsAndOutput},
    {"cache and AU bounds", CacheBounds},
    {"completed B cannot reset replay budget", ReplayBoundNotResetByBRemoval},
    {"true root advancement resets work", RootAdvanceResetsWork},
    {"all 120 output completion orders", OutputOrderPermutations},
    {"1000 bounded open GOPs", SustainedBoundedGops},
    {"seal/restart at every I/P/B boundary", SealAtEveryPictureBoundary},
  };
  for (const auto &test : tests) {
    size_t before = failures; ++groups; test.second();
    std::printf("%s: %s\n", test.first, before == failures ? "PASS" : "FAIL");
  }
  std::printf("%zu groups, %zu checks, %zu failures\n", groups, checks, failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
