// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual replay helpers; no hardware, VA backend, or codec decoder is linked.
#include "../filters/vaapi/crystalhd-vc1-replay.h"
#include "../filters/vaapi/crystalhd-mpeg2-replay.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

using Replay = CrystalHDVc1Replay;
using Kind = Replay::Kind;
static size_t checks, failures, groups;
#define CHECK(e) do { ++checks; if (!(e)) { ++failures; \
  std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #e); } } while (0)
#define REQUIRE(e) do { ++checks; if (!(e)) { ++failures; \
  std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #e); return; } } while (0)

static std::vector<uint8_t> Bytes(uint64_t token) {
  std::vector<uint8_t> bytes{0, 0, 1, 0x0f, 0, 0, 1, 0x0d};
  for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(token >> (8*i)));
  return bytes; // Opaque identity data, not a claim of decodable VC-1 syntax.
}
static bool Add(Replay &r, uint64_t t, Kind k, uint64_t f = 0, uint64_t b = 0) {
  return r.Append(t, k, f, b, Bytes(t));
}
static std::vector<uint64_t> Send(Replay &r) {
  std::vector<uint64_t> sent;
  while (const auto *unit = r.NextInput()) {
    CHECK(unit->bytes == Bytes(unit->timestamp));
    sent.push_back(unit->timestamp);
    if (!r.InputSent()) { CHECK(false); break; }
  }
  return sent;
}
static void FinishNew(Replay &r) {
  for (uint64_t t : Send(r)) CHECK(r.Observe(t) == Replay::Output::New);
}
static void Invariants(const Replay &r) {
  size_t bytes = 0;
  uint64_t prior = 0;
  REQUIRE(!r.units().empty());
  CHECK(r.units().front().kind == Kind::I);
  for (const auto &u : r.units()) {
    CHECK(u.timestamp > prior);
    CHECK(u.bytes == Bytes(u.timestamp));
    const auto *root = r.Find(u.root);
    CHECK(root && root->kind == Kind::I && root->timestamp <= u.timestamp);
    if (u.forward) CHECK(r.Find(u.forward) && u.forward < u.timestamp);
    if (u.backward) CHECK(r.Find(u.backward) && u.backward < u.timestamp);
    if (u.kind == Kind::I || u.kind == Kind::BI) CHECK(!u.forward && !u.backward);
    bytes += u.bytes.size(); prior = u.timestamp;
  }
  CHECK(bytes == r.cached_bytes());
  CHECK(r.Find(r.newest_anchor()) != nullptr);
  if (r.previous_anchor()) CHECK(r.Find(r.previous_anchor()) != nullptr);
}

// A tiny independent model of just the documented WMV3 rounding transition,
// not decoded pixels: I/BI reset to one, P toggles, B preserves. Skipped-P is
// an Advanced-profile transport case and is deliberately absent here.
static unsigned Rounding(const std::vector<uint64_t> &order) {
  unsigned rnd = 0;
  for (uint64_t t : order) {
    if (t == 1 || t == 3) rnd = 1; // I1, BI3
    if (t == 2 || t == 4) rnd ^= 1; // P2, P4
  }
  return rnd;
}
static void BiRoundingCounterexample() {
  CrystalHDMpeg2Replay old;
  using OldKind = CrystalHDMpeg2Replay::Kind;
  CHECK(old.Append(1, OldKind::I, 0, 0, Bytes(1)));
  CHECK(old.Append(2, OldKind::P, 1, 0, Bytes(2)));
  // A naive adapter maps non-reference BI to B, with the incidental reference
  // IDs supplied by FFmpeg. The actual MPEG-2 helper then prunes it.
  CHECK(old.Append(3, OldKind::B, 1, 2, Bytes(3)));
  CHECK(old.Append(4, OldKind::P, 2, 0, Bytes(4)));
  while (old.NextInput()) CHECK(old.InputSent());
  for (uint64_t t : {1ULL, 2ULL, 3ULL, 4ULL})
    CHECK(old.Observe(t) == CrystalHDMpeg2Replay::Output::New);
  std::vector<uint64_t> pruned;
  for (const auto &u : old.units()) pruned.push_back(u.timestamp);
  CHECK(pruned == std::vector<uint64_t>({1, 2, 4}));
  CHECK(Rounding(pruned) != Rounding({1, 2, 3, 4}));
  std::puts("COUNTEREXAMPLE: actual MPEG-2 B pruning changes later WMV3 P rounding");

  Replay r;
  CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::P, 1));
  CHECK(Add(r, 3, Kind::BI)); CHECK(Add(r, 4, Kind::P, 2));
  CHECK(r.previous_anchor() == 2 && r.newest_anchor() == 4);
  auto first = Send(r); CHECK(r.Seal());
  CHECK(Add(r, 5, Kind::BI));
  for (uint64_t t : first) CHECK(r.Observe(t) == Replay::Output::New);
  CHECK(r.EndOfSequence()); CHECK(r.Restarted());
  auto repeated = Send(r);
  CHECK(repeated == std::vector<uint64_t>({1, 2, 3, 4, 5}));
  repeated.pop_back(); CHECK(Rounding(repeated) == Rounding(first));
  CHECK(r.Find(3) && r.Find(3)->completed && r.Find(3)->root == 1);
  Invariants(r);
}

static void SkippedPIdentity() {
  Replay r;
  CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::SkippedP, 1));
  CHECK(r.previous_anchor() == 1 && r.newest_anchor() == 2);
  CHECK(Add(r, 3, Kind::BI));
  CHECK(r.previous_anchor() == 1 && r.newest_anchor() == 2);
  CHECK(Add(r, 4, Kind::B, 1, 2)); CHECK(Add(r, 5, Kind::P, 2));
  CHECK(r.Find(2)->kind == Kind::SkippedP && r.Find(2)->root == 1);
  auto sent = Send(r); CHECK(r.Seal());
  for (uint64_t t : {1ULL, 3ULL, 4ULL, 5ULL})
    CHECK(r.Observe(t) == Replay::Output::New);
  CHECK(r.outstanding() == 1); // A repeated image still owes its OWN output.
  CHECK(r.Observe(1) == Replay::Output::Duplicate);
  CHECK(r.outstanding() == 1);
  CHECK(r.Observe(2) == Replay::Output::New);
  CHECK(r.EndOfSequence());
  CHECK(sent == std::vector<uint64_t>({1, 2, 3, 4, 5}));
  Invariants(r);
}

static void CompletedLeadingBClosure() {
  Replay r;
  CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::P, 1));
  CHECK(Add(r, 3, Kind::I)); CHECK(Add(r, 4, Kind::B, 2, 3));
  CHECK(Add(r, 5, Kind::P, 3)); FinishNew(r);
  // Once B4 completed it can be omitted. Its old P2/I1 are no longer needed;
  // I3/P5 remain a complete prefix for future decoding.
  CHECK(r.cached_pictures() == 2 && !r.Find(4));
  CHECK(!r.Find(1) && !r.Find(2) && r.units().front().timestamp == 3);
  CHECK(Add(r, 6, Kind::BI)); FinishNew(r);
  CHECK(Add(r, 7, Kind::I)); FinishNew(r);
  CHECK(Add(r, 8, Kind::SkippedP, 7));
  CHECK(r.units().front().timestamp == 7 && r.cached_pictures() == 2);
  FinishNew(r);
  CHECK(r.Observe(4) == Replay::Output::Unknown);
  Invariants(r);
}

static void TransitiveSuffixClosure() {
  Replay r;
  CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::P, 1));
  CHECK(Add(r, 3, Kind::I)); CHECK(Add(r, 4, Kind::B, 2, 3));
  CHECK(Add(r, 5, Kind::P, 3)); CHECK(Add(r, 6, Kind::I));
  CHECK(Add(r, 7, Kind::B, 5, 6)); CHECK(Add(r, 8, Kind::P, 6));
  FinishNew(r);
  // Both old open-entry dependencies disappear only after their B outputs
  // complete. Completed anchors from those older roots can then retire.
  CHECK(r.cached_pictures() == 2 && r.units().front().timestamp == 6);
  CHECK(!r.Find(4) && !r.Find(7));
  CHECK(Add(r, 9, Kind::I)); CHECK(Add(r, 10, Kind::P, 9));
  CHECK(r.cached_pictures() == 2 && r.units().front().timestamp == 9);
  FinishNew(r); Invariants(r);
}

static void PendingAndInflightHoldPrefix() {
  Replay r;
  CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::P, 1));
  auto first = Send(r); CHECK(r.Seal());
  CHECK(Add(r, 3, Kind::I));
  for (uint64_t t : first) CHECK(r.Observe(t) == Replay::Output::New);
  CHECK(r.EndOfSequence()); CHECK(r.Restarted());
  CHECK(Send(r) == std::vector<uint64_t>({1, 2, 3}));
  CHECK(Add(r, 4, Kind::P, 3));
  CHECK(r.Find(1) && r.Find(2)); // Completed but replaying owners still live.
  CHECK(r.Observe(3) == Replay::Output::New);
  CHECK(r.Observe(1) == Replay::Output::Duplicate); CHECK(r.Find(1));
  CHECK(r.Observe(2) == Replay::Output::Duplicate);
  CHECK(!r.Find(1) && !r.Find(2));
  CHECK(Send(r) == std::vector<uint64_t>({4}));
  CHECK(r.Observe(4) == Replay::Output::New); Invariants(r);

  Replay pending;
  CHECK(Add(pending, 1, Kind::I)); CHECK(Add(pending, 2, Kind::BI));
  CHECK(Add(pending, 3, Kind::I)); CHECK(Add(pending, 4, Kind::P, 3));
  Send(pending);
  for (uint64_t t : {4ULL, 3ULL, 1ULL})
    CHECK(pending.Observe(t) == Replay::Output::New);
  CHECK(pending.Find(1) && pending.Find(2) && !pending.Find(2)->completed);
  CHECK(pending.Observe(2) == Replay::Output::New);
  CHECK(pending.units().front().timestamp == 3); Invariants(pending);
}

static void SealedWatermarkAndLateInput() {
  Replay r;
  CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::P, 1));
  CHECK(Add(r, 3, Kind::BI)); auto first = Send(r); CHECK(r.Seal());
  CHECK(r.sealed()); CHECK(Add(r, 4, Kind::I)); CHECK(Add(r, 5, Kind::P, 4));
  for (uint64_t t : first) CHECK(r.Observe(t) == Replay::Output::New);
  CHECK(r.cached_pictures() == 5); // Never prune an active sealed batch.
  CHECK(r.NextInput() == nullptr); CHECK(!r.NeedsRestart());
  CHECK(r.EndOfSequence()); CHECK(!r.sealed()); CHECK(r.NeedsRestart());
  CHECK(r.Restarted()); CHECK(r.cached_pictures() == 2);
  CHECK(Send(r) == std::vector<uint64_t>({4, 5})); CHECK(r.Seal());
  CHECK(r.Observe(5) == Replay::Output::New); CHECK(r.Observe(4) == Replay::Output::New);
  CHECK(r.EndOfSequence()); CHECK(!r.NeedsRestart());
  // Input may arrive only after EOS was observed; the token watermark, not
  // vector size after pruning, still forces a real reopen.
  CHECK(Add(r, 6, Kind::BI)); CHECK(r.NeedsRestart());
  CHECK(r.Restarted()); CHECK(Send(r) == std::vector<uint64_t>({4, 5, 6}));
  Invariants(r);
}

static void CursorAfterUnsentReplayPruning() {
  Replay r;
  CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::P, 1));
  auto sent = Send(r); CHECK(r.Seal()); CHECK(Add(r, 3, Kind::I));
  for (uint64_t t : sent) CHECK(r.Observe(t) == Replay::Output::New);
  CHECK(r.EndOfSequence()); CHECK(r.Restarted());
  // The old completed prefix was not yet resubmitted in this batch.
  CHECK(Add(r, 4, Kind::P, 3)); CHECK(!r.Find(1));
  CHECK(Send(r) == std::vector<uint64_t>({3, 4})); Invariants(r);
}

static void OwnedBytesAndHeldPayload() {
  Replay r;
  auto bytes = Bytes(1); CHECK(r.Append(1, Kind::I, 0, 0, bytes));
  bytes.assign(200, 0xff); CHECK(r.Find(1)->bytes == Bytes(1)); FinishNew(r);
  // This adapter demonstrates identity ownership only, not VA surface access
  // authorization; real backend context/canonical/BUSY checks remain required.
  std::map<unsigned, uint64_t> surfaces{{10, 1}};
  auto held = std::make_shared<const std::vector<uint8_t>>(Bytes(1));
  CHECK(Add(r, 2, Kind::SkippedP, surfaces[10])); surfaces[20] = 2;
  CHECK(Add(r, 3, Kind::BI)); CHECK(Add(r, 4, Kind::B, surfaces[10], surfaces[20]));
  surfaces[20] = 999;
  CHECK(r.Find(4)->forward == 1 && r.Find(4)->backward == 2);
  auto first = Send(r); CHECK(r.Seal()); CHECK(Add(r, 5, Kind::P, 2));
  for (uint64_t t : first) CHECK(r.Observe(t) == Replay::Output::New);
  CHECK(r.EndOfSequence()); CHECK(r.Restarted());
  auto repeated = Send(r); CHECK(repeated == std::vector<uint64_t>({1, 2, 3, 5}));
  CHECK(!r.Find(4)); // Ordinary B completed in the old batch; BI3 stays.
  for (uint64_t t : repeated)
    CHECK(r.Observe(t) == (t == 5 ? Replay::Output::New : Replay::Output::Duplicate));
  CHECK(Add(r, 6, Kind::I)); CHECK(Add(r, 7, Kind::P, 6)); FinishNew(r);
  CHECK(!r.Find(1)); CHECK(*held == Bytes(1));
  CHECK(r.units().front().timestamp == 6); Invariants(r);
}

static void InvalidReferences() {
  for (Kind kind : {Kind::P, Kind::SkippedP, Kind::B, Kind::BI}) {
    Replay r; CHECK(!Add(r, 1, kind)); CHECK(r.failed());
  }
  { Replay r; CHECK(!Add(r, 1, Kind::I, 1)); }
  for (Kind kind : {Kind::P, Kind::SkippedP}) {
    { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(!Add(r, 2, kind, 1, 1)); }
    { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::BI));
      CHECK(!Add(r, 3, kind, 2)); }
    { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::P, 1));
      CHECK(Add(r, 3, Kind::B, 1, 2)); CHECK(!Add(r, 4, kind, 3)); }
    { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::I));
      CHECK(!Add(r, 3, kind, 1)); }
  }
  { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(!Add(r, 2, Kind::BI, 1)); }
  { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(!Add(r, 2, Kind::BI, 0, 1)); }
  { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(!Add(r, 2, Kind::B, 1, 1)); }
  { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::P, 1));
    CHECK(!Add(r, 3, Kind::B, 2, 1)); }
  { Replay r; CHECK(!Add(r, 0, Kind::I)); }
  { Replay r; CHECK(Add(r, 1, Kind::I)); CHECK(!Add(r, 1, Kind::I)); }
  { Replay r; CHECK(Add(r, 2, Kind::I)); CHECK(!Add(r, 1, Kind::I)); }
  { Replay r; CHECK(!r.Append(1, Kind::I, 0, 0, {})); }
  { Replay r; CHECK(!Add(r, 1, static_cast<Kind>(99))); }
}

static void InvalidTransitionsAndOutput() {
  { Replay r; CHECK(!r.Seal()); }
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
  { Replay r; CHECK(Add(r, 1, Kind::I)); Send(r); CHECK(r.Seal());
    CHECK(r.Observe(1) == Replay::Output::New); CHECK(r.EndOfSequence());
    CHECK(!r.Restarted()); }
  { Replay r; CHECK(!r.Fail("caller teardown failed"));
    CHECK(!std::strcmp(r.failure(), "caller teardown failed"));
    CHECK(r.failed()); CHECK(!Add(r, 1, Kind::I));
    CHECK(r.NextInput() == nullptr); CHECK(r.Observe(1) == Replay::Output::Invalid); }
}

static void CacheBounds() {
  Replay::Limits l;
  l.picture_bytes = 15; { Replay r(l); CHECK(!Add(r, 1, Kind::I)); }
  l.picture_bytes = 16; l.cache_bytes = 31;
  { Replay r(l); CHECK(Add(r, 1, Kind::I)); CHECK(!Add(r, 2, Kind::P, 1)); }
  l.cache_bytes = 4096; l.pictures = 2;
  { Replay r(l); CHECK(Add(r, 1, Kind::I)); FinishNew(r);
    CHECK(Add(r, 2, Kind::BI)); FinishNew(r); CHECK(!Add(r, 3, Kind::BI));
    CHECK(r.Find(2) && r.Find(2)->completed); }
  l.cache_bytes = 0; { Replay r(l); CHECK(!Add(r, 1, Kind::I)); }
  l.cache_bytes = 4096; l.pictures = 0;
  { Replay r(l); CHECK(!Add(r, 1, Kind::I)); }
  l.cache_bytes = SIZE_MAX; l.pictures = SIZE_MAX; l.picture_bytes = SIZE_MAX;
  { Replay r(l); CHECK(Add(r, UINT64_MAX-1, Kind::I));
    CHECK(Add(r, UINT64_MAX, Kind::P, UINT64_MAX-1)); FinishNew(r); Invariants(r);
    CHECK(!Add(r, 1, Kind::I)); }
  { Replay r(l); CHECK(Add(r, UINT64_MAX, Kind::I)); FinishNew(r); Invariants(r); }
}

static void ReplayBudget() {
  Replay::Limits l; l.replay_pictures = 3;
  Replay r(l);
  CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::P, 1));
  auto sent = Send(r); CHECK(r.Seal()); CHECK(Add(r, 3, Kind::BI));
  for (uint64_t t : sent) CHECK(r.Observe(t) == Replay::Output::New);
  CHECK(r.EndOfSequence()); CHECK(r.Restarted());
  sent = Send(r); CHECK(r.replay_work() == 2); CHECK(r.Seal());
  CHECK(Add(r, 4, Kind::SkippedP, 2));
  for (uint64_t t : sent)
    CHECK(r.Observe(t) == (t == 3 ? Replay::Output::New : Replay::Output::Duplicate));
  CHECK(r.EndOfSequence()); CHECK(r.Restarted());
  CHECK(r.Find(3) && r.replay_work() == 2);
  CHECK(r.NextInput() && r.NextInput()->timestamp == 1); CHECK(r.InputSent());
  CHECK(r.NextInput() == nullptr && r.failed()); CHECK(r.replay_work() == 3);
  CHECK(r.Find(4) && !r.Find(4)->completed);
}

static void RootAdvanceResetsWork() {
  Replay::Limits l; l.replay_pictures = 2;
  Replay r(l);
  CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::BI));
  auto sent = Send(r); CHECK(r.Seal()); CHECK(Add(r, 3, Kind::I));
  for (uint64_t t : sent) CHECK(r.Observe(t) == Replay::Output::New);
  CHECK(r.EndOfSequence()); CHECK(r.Restarted()); sent = Send(r);
  CHECK(r.replay_work() == 2); CHECK(r.Seal()); CHECK(Add(r, 4, Kind::SkippedP, 3));
  for (uint64_t t : sent)
    CHECK(r.Observe(t) == (t == 3 ? Replay::Output::New : Replay::Output::Duplicate));
  CHECK(r.EndOfSequence()); CHECK(r.Restarted());
  CHECK(r.replay_work() == 0 && !r.Find(1));
  CHECK(Send(r) == std::vector<uint64_t>({3, 4})); CHECK(!r.failed());
}

static void OutputOrderPermutations() {
  std::array<uint64_t, 7> order{1, 2, 3, 4, 5, 6, 7};
  do {
    Replay r;
    CHECK(Add(r, 1, Kind::I)); CHECK(Add(r, 2, Kind::SkippedP, 1));
    CHECK(Add(r, 3, Kind::BI)); CHECK(Add(r, 4, Kind::I));
    CHECK(Add(r, 5, Kind::B, 2, 4)); CHECK(Add(r, 6, Kind::I));
    CHECK(Add(r, 7, Kind::P, 6)); Send(r);
    std::unordered_set<uint64_t> pending(order.begin(), order.end());
    for (uint64_t token : order) {
      CHECK(r.Observe(token) == Replay::Output::New); pending.erase(token);
      for (uint64_t p : pending) CHECK(r.Find(p) && !r.Find(p)->completed);
      Invariants(r);
    }
    CHECK(r.cached_pictures() == 2 && r.units().front().timestamp == 6);
    CHECK(!r.failed());
  } while (std::next_permutation(order.begin(), order.end()));
}

static void SustainedPrefixPruning() {
  Replay::Limits l; l.pictures = 16; l.cache_bytes = 16*16;
  Replay r(l); uint64_t next = 1;
  CHECK(Add(r, next++, Kind::I)); FinishNew(r);
  CHECK(Add(r, next, Kind::P, next-1)); ++next; FinishNew(r);
  for (unsigned g = 0; g < 1000; ++g) {
    uint64_t prior = r.newest_anchor(), current = next++;
    CHECK(Add(r, current, Kind::I)); CHECK(Add(r, next++, Kind::B, prior, current));
    CHECK(Add(r, next++, Kind::BI));
    uint64_t p = next++; CHECK(Add(r, p, Kind::SkippedP, current));
    auto sent = Send(r);
    for (auto it = sent.rbegin(); it != sent.rend(); ++it)
      CHECK(r.Observe(*it) == Replay::Output::New);
    CHECK(!r.Find(prior) && r.Find(current)); // Completed ordinary B no longer pins old anchors.
    uint64_t clean = next++;
    CHECK(Add(r, clean, Kind::I)); CHECK(Add(r, next, Kind::P, clean)); ++next;
    FinishNew(r);
    CHECK(r.cached_pictures() == 2 && r.units().front().timestamp == clean);
    Invariants(r);
  }
  CHECK(!r.failed() && r.cached_bytes() == 32);
}

static void SealAtEveryPictureBoundary() {
  struct Picture { Kind kind; uint64_t f, b; };
  const Picture pictures[] = {
    {Kind::I, 0, 0}, {Kind::P, 1, 0}, {Kind::BI, 0, 0}, {Kind::B, 1, 2},
    {Kind::SkippedP, 2, 0}, {Kind::I, 0, 0}, {Kind::B, 5, 6}, {Kind::BI, 0, 0},
    {Kind::P, 6, 0}, {Kind::B, 6, 9}, {Kind::I, 0, 0}, {Kind::P, 11, 0},
    {Kind::BI, 0, 0}, {Kind::SkippedP, 12, 0}, {Kind::B, 12, 14}
  };
  constexpr size_t count = sizeof(pictures) / sizeof(pictures[0]);
  Replay r;
  std::map<uint64_t, std::vector<uint8_t>> original;
  std::map<uint64_t, unsigned> completions;
  CHECK(Add(r, 1, pictures[0].kind));
  for (size_t n = 0; n < count; ++n) {
    std::vector<uint64_t> sent;
    while (const auto *u = r.NextInput()) {
      auto found = original.find(u->timestamp);
      if (found == original.end()) original[u->timestamp] = u->bytes;
      else CHECK(found->second == u->bytes);
      CHECK(u->bytes == Bytes(u->timestamp));
      sent.push_back(u->timestamp); CHECK(r.InputSent());
    }
    REQUIRE(!sent.empty()); CHECK(r.Seal());
    if (n + 1 < count) {
      const auto &p = pictures[n+1]; CHECK(Add(r, n+2, p.kind, p.f, p.b));
      CHECK(r.NextInput() == nullptr);
    }
    for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
      auto output = r.Observe(*it);
      CHECK(output == (completions[*it] ? Replay::Output::Duplicate : Replay::Output::New));
      if (output == Replay::Output::New) ++completions[*it];
    }
    CHECK(r.EndOfSequence()); CHECK(r.NeedsRestart() == (n+1 < count));
    if (n+1 < count) CHECK(r.Restarted());
    CHECK(!r.failed()); Invariants(r);
  }
  CHECK(original.size() == count && completions.size() == count);
  for (const auto &entry : completions) CHECK(entry.second == 1);
}

static void CompletedBOnlyRemoval() {
  Replay r;
  REQUIRE(Add(r,1,Kind::I)); REQUIRE(Add(r,2,Kind::P,1));
  REQUIRE(Add(r,3,Kind::BI)); REQUIRE(Add(r,4,Kind::B,1,2));
  REQUIRE(Add(r,5,Kind::P,2));
  CHECK(Send(r)==std::vector<uint64_t>({1,2,3,4,5}));
  CHECK(r.Observe(4)==Replay::Output::New);
  CHECK(!r.Find(4) && r.cached_pictures()==4 && r.cached_bytes()==4*Bytes(1).size());
  CHECK(r.Observe(4)==Replay::Output::Unknown);
  for(uint64_t token:{1ULL,2ULL,3ULL,5ULL}) CHECK(r.Observe(token)==Replay::Output::New);
  REQUIRE(r.Find(3)); CHECK(r.Find(3)->kind==Kind::BI && r.Find(3)->completed);
  REQUIRE(Add(r,6,Kind::B,2,5));
  CHECK(Send(r)==std::vector<uint64_t>({6})); // Erasing a sent hole must adjust cursor.
  CHECK(r.Observe(6)==Replay::Output::New); CHECK(!r.Find(6)); Invariants(r);
}

static void PendingBAndReplayHoldOldAnchors() {
  Replay r;
  REQUIRE(Add(r,1,Kind::I)); REQUIRE(Add(r,2,Kind::P,1));
  REQUIRE(Add(r,3,Kind::I)); REQUIRE(Add(r,4,Kind::B,2,3));
  REQUIRE(Add(r,5,Kind::P,3)); Send(r);
  for(uint64_t token:{1ULL,2ULL,3ULL,5ULL}) CHECK(r.Observe(token)==Replay::Output::New);
  REQUIRE(r.Find(4)); CHECK(!r.Find(4)->completed && r.outstanding()==1);
  CHECK(r.Find(1) && r.Find(2));
  REQUIRE(Add(r,6,Kind::BI)); CHECK(Send(r)==std::vector<uint64_t>({6}));
  CHECK(r.Observe(6)==Replay::Output::New); CHECK(r.Find(1) && r.Find(2));
  CHECK(r.Observe(4)==Replay::Output::New);
  CHECK(!r.Find(1) && !r.Find(2) && !r.Find(4));
  CHECK(r.units().front().timestamp==3 && r.cached_pictures()==3); Invariants(r);

  Replay replay;
  REQUIRE(Add(replay,1,Kind::I)); REQUIRE(Add(replay,2,Kind::P,1));
  Send(replay); REQUIRE(replay.Seal());
  REQUIRE(Add(replay,3,Kind::B,1,2)); // Arrives after seal, not submitted yet.
  CHECK(replay.Observe(1)==Replay::Output::New); CHECK(replay.Observe(2)==Replay::Output::New);
  REQUIRE(replay.EndOfSequence()); REQUIRE(replay.Restarted());
  CHECK(Send(replay)==std::vector<uint64_t>({1,2,3}));
  REQUIRE(Add(replay,4,Kind::I)); REQUIRE(Add(replay,5,Kind::P,4));
  CHECK(Send(replay)==std::vector<uint64_t>({4,5}));
  CHECK(replay.Observe(1)==Replay::Output::Duplicate);
  CHECK(replay.Observe(2)==Replay::Output::Duplicate);
  CHECK(replay.Observe(4)==Replay::Output::New); CHECK(replay.Observe(5)==Replay::Output::New);
  REQUIRE(replay.Find(3)); CHECK(replay.outstanding()==1 && !replay.Find(3)->completed);
  CHECK(replay.Find(1) && replay.Find(2)); // Reopened B still requires both old anchors.
  CHECK(replay.Observe(3)==Replay::Output::New);
  CHECK(!replay.Find(1) && !replay.Find(2) && !replay.Find(3));
  CHECK(replay.cached_pictures()==2 && replay.units().front().timestamp==4); Invariants(replay);
}

static void CursorAcrossCompletedBHole() {
  Replay r;
  REQUIRE(Add(r,1,Kind::I)); REQUIRE(Add(r,2,Kind::P,1));
  REQUIRE(Add(r,3,Kind::B,1,2)); REQUIRE(Add(r,4,Kind::BI));
  REQUIRE(Add(r,5,Kind::P,2));
  for(uint64_t token:{1ULL,2ULL,3ULL}) {
    REQUIRE(r.NextInput()); CHECK(r.NextInput()->timestamp==token); REQUIRE(r.InputSent());
  }
  CHECK(r.Observe(3)==Replay::Output::New); CHECK(!r.Find(3));
  REQUIRE(r.NextInput()); CHECK(r.NextInput()->timestamp==4);
  CHECK(Send(r)==std::vector<uint64_t>({4,5}));
  for(uint64_t token:{1ULL,2ULL,4ULL,5ULL}) CHECK(r.Observe(token)==Replay::Output::New);
  CHECK(r.NextInput()==nullptr); Invariants(r);
}

static void SealedBCompletionAndWatermark() {
  Replay r;
  REQUIRE(Add(r,1,Kind::I)); REQUIRE(Add(r,2,Kind::P,1)); REQUIRE(Add(r,3,Kind::B,1,2));
  Send(r); REQUIRE(r.Seal());
  for(uint64_t token:{1ULL,2ULL,3ULL}) CHECK(r.Observe(token)==Replay::Output::New);
  REQUIRE(r.Find(3)); CHECK(r.Find(3)->completed); // Sealed batch is not reshaped.
  REQUIRE(r.EndOfSequence()); CHECK(!r.NeedsRestart());
  REQUIRE(Add(r,4,Kind::BI)); CHECK(r.NeedsRestart()); REQUIRE(r.Restarted());
  CHECK(!r.Find(3)); CHECK(Send(r)==std::vector<uint64_t>({1,2,4}));
  CHECK(r.replay_work()==2);
  CHECK(r.Observe(1)==Replay::Output::Duplicate); CHECK(r.Observe(2)==Replay::Output::Duplicate);
  CHECK(r.Observe(4)==Replay::Output::New);
  REQUIRE(Add(r,5,Kind::B,1,2)); CHECK(Send(r)==std::vector<uint64_t>({5}));
  CHECK(r.Observe(5)==Replay::Output::New);
  CHECK(!r.Find(5) && r.replay_work()==2); // Removing B is not a root/budget reset.
  Invariants(r);
}

static void SustainedOpenEntriesWithoutCleanWindows() {
  Replay::Limits limits; limits.pictures=12; limits.cache_bytes=12*Bytes(1).size();
  Replay r(limits); uint64_t next=1;
  REQUIRE(Add(r,next++,Kind::I)); FinishNew(r);
  REQUIRE(Add(r,next,Kind::P,next-1)); ++next; FinishNew(r);
  for(unsigned cycle=0;cycle<1100;++cycle) {
    const uint64_t prior=r.newest_anchor(), current=next++, b=next++, bi=next++, p=next++;
    REQUIRE(Add(r,current,Kind::I)); REQUIRE(Add(r,b,Kind::B,prior,current));
    REQUIRE(Add(r,bi,Kind::BI)); REQUIRE(Add(r,p,Kind::SkippedP,current));
    CHECK(Send(r)==std::vector<uint64_t>({current,b,bi,p}));
    CHECK(r.Observe(p)==Replay::Output::New); CHECK(r.Observe(bi)==Replay::Output::New);
    CHECK(r.Observe(current)==Replay::Output::New);
    CHECK(r.Find(prior) && r.Find(b)); // Pending leading B still pins old anchor.
    CHECK(r.Observe(b)==Replay::Output::New);
    CHECK(!r.Find(b) && !r.Find(prior));
    CHECK(r.cached_pictures()==3 && r.units().front().timestamp==current);
    REQUIRE(r.Find(bi)); CHECK(r.Find(bi)->completed && r.Find(bi)->kind==Kind::BI);
    Invariants(r);
    // The next I immediately has another cross-entry B; no clean I/P window
    // is inserted to make the old conservative closure artificially pass.
  }
  CHECK(!r.failed() && r.cached_bytes()==3*Bytes(1).size());
}

int main() {
  const std::pair<const char *, void (*)()> tests[] = {
    {"actual MPEG-2 BI pruning rounding counterexample", BiRoundingCounterexample},
    {"skipped P anchor and independent output token", SkippedPIdentity},
    {"completed leading B releases its old causal prefix", CompletedLeadingBClosure},
    {"completed B releases transitive open-entry dependencies", TransitiveSuffixClosure},
    {"pending BI and replaying owner prefix retention", PendingAndInflightHoldPrefix},
    {"sealed token watermark and post-EOS late input", SealedWatermarkAndLateInput},
    {"prune completed prefix before replay resubmission", CursorAfterUnsentReplayPruning},
    {"immutable bytes, reference tokens and held payload", OwnedBytesAndHeldPayload},
    {"invalid types, tokens and reference windows", InvalidReferences},
    {"output and lifecycle failures stay closed", InvalidTransitionsAndOutput},
    {"picture, byte, count and maximum token bounds", CacheBounds},
    {"completed BI preserves accumulated replay budget", ReplayBudget},
    {"true root advance resets bounded replay work", RootAdvanceResetsWork},
    {"all 5040 mixed-picture completion orders", OutputOrderPermutations},
    {"1000 bounded open-entry/closed-prefix cycles", SustainedPrefixPruning},
    {"seal and restart at every picture-kind boundary", SealAtEveryPictureBoundary},
    {"only completed ordinary B is discarded", CompletedBOnlyRemoval},
    {"pending and reopened B hold old anchors until completion", PendingBAndReplayHoldOldAnchors},
    {"cursor survives a completed B hole before unsent input", CursorAcrossCompletedBHole},
    {"sealed B completion retains EOS watermark and work budget", SealedBCompletionAndWatermark},
    {"1100 consecutive open entries without synthetic clean windows", SustainedOpenEntriesWithoutCleanWindows},
  };
  for (const auto &test : tests) {
    const size_t before = failures; ++groups; test.second();
    std::printf("%s: %s\n", test.first, before == failures ? "PASS" : "FAIL");
  }
  std::printf("%zu groups, %zu checks, %zu failures\n", groups, checks, failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
