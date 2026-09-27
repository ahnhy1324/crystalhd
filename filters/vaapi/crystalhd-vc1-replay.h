// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_VC1_REPLAY_H
#define CRYSTALHD_VC1_REPLAY_H
#include "crystalhd-decode-replay.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

// Progressive VC-1/WMV3 transport only. The caller validates headers and VA
// reference identities, and supplies a complete restartable configuration on
// every I AU. BI is not an anchor: in WMV3 it nevertheless resets rounding
// state (FFmpeg n7.1.1 vc1.c, ff_vc1_parse_frame_header). Retain BI and skipped P
// along with the causal I/P prefix. Only completed, non-outstanding ordinary B
// can be omitted: progressive B carries no reference/rounding state needed by
// later pictures (unlike WMV3 BI), and
// selected Advanced/Main omission streams have passed exact software and card
// pixel comparisons. This is replay-cache pruning, never input frame dropping.
// Firmware/header correctness is outside this transport model.
// All methods require the same caller serialization as the H.264 helper.
class CrystalHDVc1Replay {
 public:
  enum class Kind { I, P, B, BI, SkippedP };
  using Output = CrystalHDDecodeReplay::Output;
  struct Limits {
    size_t picture_bytes = 512 * 1024;
    size_t cache_bytes = 32 * 1024 * 1024;
    size_t pictures = 512;
    size_t replay_pictures = 8192;
  };
  struct AccessUnit {
    uint64_t timestamp;
    Kind kind;
    uint64_t forward, backward;
    // Earliest I required by this picture's reference/state prefix. Original
    // bytes and timestamps never change across a completed EOS/reopen cycle.
    uint64_t root;
    std::vector<uint8_t> bytes;
    bool completed = false;
  };
  CrystalHDVc1Replay() = default;
  explicit CrystalHDVc1Replay(Limits limits) : limits_(limits) {}

  bool Append(uint64_t token, Kind kind, uint64_t forward, uint64_t backward,
              std::vector<uint8_t> bytes) {
    if (failed()) return false;
    Prune();
    if (!token || token <= last_accepted_ || bytes.empty())
      return Fail("nonunique token or empty AU");
    uint64_t root = token;
    const AccessUnit *f = Find(forward), *b = Find(backward);
    switch (kind) {
      case Kind::I:
        if (forward || backward) return Fail("I picture has references");
        break;
      case Kind::P:
      case Kind::SkippedP:
        if (!newest_ || forward != newest_ || backward || !f || !Anchor(f->kind))
          return Fail("P reference is not the current newest anchor");
        root = f->root;
        break;
      case Kind::B:
        if (!previous_ || !newest_ || forward != previous_ ||
            backward != newest_ || !f || !b || !Anchor(f->kind) || !Anchor(b->kind))
          return Fail("B references do not match the implicit anchor window");
        root = std::min(f->root, b->root);
        break;
      case Kind::BI: {
        // The backend normalizes incidental client reference IDs for an intra
        // BI picture. It cannot be a restart root for subsequent P pictures.
        const AccessUnit *anchor = Find(newest_);
        if (forward || backward || !anchor || !Anchor(anchor->kind))
          return Fail("BI requires an established I-root prefix and no references");
        root = anchor->root;
        break;
      }
      default:
        return Fail("unsupported picture type");
    }
    if (bytes.size() > limits_.picture_bytes ||
        cache_bytes_ > limits_.cache_bytes ||
        bytes.size() > limits_.cache_bytes - cache_bytes_ ||
        units_.size() >= limits_.pictures)
      return Fail("bounded VC-1 replay cache exhausted");
    cache_bytes_ += bytes.size();
    units_.push_back({token, kind, forward, backward, root, std::move(bytes), false});
    last_accepted_ = token;
    if (Anchor(kind)) {
      previous_ = newest_;
      newest_ = token;
    }
    Prune();
    return true;
  }

  const AccessUnit *NextInput() {
    if (phase_ != Phase::Running || cursor_ == units_.size()) return nullptr;
    if (units_[cursor_].completed && replay_work_ >= limits_.replay_pictures) {
      Fail("bounded VC-1 replay work exhausted");
      return nullptr;
    }
    return &units_[cursor_];
  }
  bool InputSent() {
    const AccessUnit *unit = NextInput();
    if (!unit) return Fail("no input can be admitted in this phase");
    if (!outstanding_.insert(unit->timestamp).second)
      return Fail("same token submitted twice in one batch");
    if (unit->completed) ++replay_work_;
    ++cursor_;
    return true;
  }
  bool CanSeal() const {
    return phase_ == Phase::Running && cursor_ == units_.size() &&
           !outstanding_.empty();
  }
  bool Seal() {
    if (!CanSeal()) return Fail("seal before all queued input was submitted");
    sealed_token_ = last_accepted_;
    phase_ = Phase::Sealed;
    return true;
  }
  bool EndOfSequence() {
    if (phase_ != Phase::Sealed || !outstanding_.empty())
      return Fail("EOS before all submitted tokens completed");
    phase_ = Phase::Ended;
    return true;
  }
  bool NeedsRestart() const {
    return phase_ == Phase::Ended && last_accepted_ > sealed_token_;
  }
  // Caller must successfully finish full device teardown/reopen first. This
  // models only the transport transition, not a decoder-only reset or timeout.
  bool Restarted() {
    if (!NeedsRestart()) return Fail("restart without completed EOS/new input");
    phase_ = Phase::Running;
    Prune();
    if (units_.empty() || units_.front().kind != Kind::I)
      return Fail("no complete I-root prefix retained");
    cursor_ = 0;
    return true;
  }
  Output Observe(uint64_t token) {
    if (failed()) return Output::Invalid;
    for (AccessUnit &unit : units_) {
      if (unit.timestamp != token) continue;
      const bool expected = outstanding_.erase(token) != 0;
      if (!expected && !unit.completed) {
        Fail("unsubmitted token cannot satisfy a pending picture");
        return Output::Invalid;
      }
      const bool completed = unit.completed;
      unit.completed = true;
      Prune();
      return completed ? Output::Duplicate : Output::New;
    }
    return Output::Unknown;  // Never completes a different accepted picture.
  }
  const AccessUnit *Find(uint64_t token) const {
    if (!token) return nullptr;
    for (const AccessUnit &unit : units_) if (unit.timestamp == token) return &unit;
    return nullptr;
  }
  bool failed() const { return phase_ == Phase::Failed; }
  bool sealed() const { return phase_ == Phase::Sealed; }
  const char *failure() const { return failure_; }
  size_t outstanding() const { return outstanding_.size(); }
  size_t cached_pictures() const { return units_.size(); }
  size_t cached_bytes() const { return cache_bytes_; }
  size_t replay_work() const { return replay_work_; }
  uint64_t previous_anchor() const { return previous_; }
  uint64_t newest_anchor() const { return newest_; }
  const std::vector<AccessUnit> &units() const { return units_; }
  bool Fail(const char *reason) {
    phase_ = Phase::Failed;
    failure_ = reason;
    return false;
  }

 private:
  enum class Phase { Running, Sealed, Ended, Failed };
  static bool Anchor(Kind kind) {
    return kind == Kind::I || kind == Kind::P || kind == Kind::SkippedP;
  }
  void Prune() {
    if (phase_ != Phase::Running || units_.empty()) return;
    // A completed ordinary B is neither an anchor nor WMV3's stateful BI.
    // Do not reshape a sealed batch or remove any outstanding hardware token.
    // Count retained entries before the old cursor, including when erasing a
    // hole in front of still-unsent pictures. Moving the original vectors
    // preserves their bytes, timestamps and reference identities.
    size_t kept = 0, consumed = 0;
    for (size_t n = 0; n < units_.size(); ++n) {
      AccessUnit &unit = units_[n];
      if (unit.kind == Kind::B && unit.completed &&
          !outstanding_.count(unit.timestamp)) {
        cache_bytes_ -= unit.bytes.size();
        continue;
      }
      if (n < cursor_) ++consumed;
      if (kept != n) units_[kept] = std::move(unit);
      ++kept;
    }
    units_.resize(kept);
    cursor_ = consumed;
    uint64_t floor = std::numeric_limits<uint64_t>::max();
    for (const AccessUnit &unit : units_) {
      if (!unit.completed || outstanding_.count(unit.timestamp) ||
          unit.timestamp == previous_ || unit.timestamp == newest_)
        floor = std::min(floor, unit.root);
    }
    // Every remaining picture, including pending B and stateful BI, still
    // needs its causal prefix. Close the retained suffix over those roots.
    // Roots never point forward, so a single reverse scan reaches closure.
    for (auto it = units_.rbegin(); it != units_.rend(); ++it)
      if (it->timestamp >= floor) floor = std::min(floor, it->root);
    if (root_floor_ && floor > root_floor_) replay_work_ = 0;
    root_floor_ = floor;
    size_t remove = 0;
    while (remove < units_.size() && units_[remove].timestamp < floor) {
      cache_bytes_ -= units_[remove].bytes.size();
      ++remove;
    }
    // Every removed entry completed in this or an earlier batch and is not
    // outstanding. Some may not yet have been resubmitted in this batch, so
    // adjust cursor only for removals before it.
    units_.erase(units_.begin(), units_.begin() + remove);
    cursor_ -= std::min(cursor_, remove);
  }
  Limits limits_;
  std::vector<AccessUnit> units_;
  std::unordered_set<uint64_t> outstanding_;
  size_t cursor_ = 0, cache_bytes_ = 0, replay_work_ = 0;
  uint64_t last_accepted_ = 0, sealed_token_ = 0, root_floor_ = 0;
  uint64_t previous_ = 0, newest_ = 0;
  Phase phase_ = Phase::Running;
  const char *failure_ = "none";
};
#endif
