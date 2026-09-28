// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_MPEG2_REPLAY_H
#define CRYSTALHD_MPEG2_REPLAY_H
#include "crystalhd-decode-replay.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

// Progressive, non-droppable I/P/B pictures only. Each I AU must contain a
// complete restartable codec configuration. Actual bitstream construction and
// codec timing are deliberately outside this model. All calls are serialized
// by the caller, as with the existing replay helper.
class CrystalHDMpeg2Replay {
 public:
  enum class Kind { I, P, B };
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
    // Earliest I in the explicit transitive reference closure. Because every
    // intervening I/P is retained in coded order, this is a bounded prefix,
    // not a proposal to decode a sparsely selected reference graph.
    uint64_t root;
    std::vector<uint8_t> bytes;
    bool completed = false;
  };
  CrystalHDMpeg2Replay() = default;
  explicit CrystalHDMpeg2Replay(Limits limits) : limits_(limits) {}

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
        if (!newest_ || forward != newest_ || backward || !f ||
            f->kind == Kind::B)
          return Fail("P reference is not the current newest anchor");
        root = f->root;
        break;
      case Kind::B:
        if (!previous_ || !newest_ || forward != previous_ ||
            backward != newest_ || !f || !b || f->kind == Kind::B ||
            b->kind == Kind::B)
          return Fail("B references do not match the implicit anchor window");
        root = std::min(f->root, b->root);
        break;
      default:
        return Fail("unsupported picture type");
    }
    if (bytes.size() > limits_.picture_bytes ||
        cache_bytes_ > limits_.cache_bytes ||
        bytes.size() > limits_.cache_bytes - cache_bytes_ ||
        units_.size() >= limits_.pictures)
      return Fail("bounded I/P/B replay cache exhausted");
    cache_bytes_ += bytes.size();
    units_.push_back({token, kind, forward, backward, root, std::move(bytes), false});
    last_accepted_ = token;
    if (kind != Kind::B) {
      previous_ = newest_;
      newest_ = token;
    }
    Prune();
    return true;
  }

  const AccessUnit *NextInput() {
    if (phase_ != Phase::Running || cursor_ == units_.size()) return nullptr;
    if (units_[cursor_].completed && replay_work_ >= limits_.replay_pictures) {
      Fail("bounded I/P/B replay work exhausted");
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
  // Caller must have completed full device teardown/reopen successfully. This
  // method models the transport transition, not actual hardware operations.
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
      bool expected = outstanding_.erase(token) != 0;
      if (!expected && !unit.completed) {
        Fail("unsubmitted token cannot satisfy a pending picture");
        return Output::Invalid;
      }
      bool completed = unit.completed;
      unit.completed = true;
      Prune();
      return completed ? Output::Duplicate : Output::New;
    }
    return Output::Unknown;  // Never satisfies any accepted pending picture.
  }
  bool IsOutstanding(uint64_t token) const {
    return token != 0 && outstanding_.count(token) != 0;
  }
  uint64_t OldestOutstanding() const {
    for (const AccessUnit &unit : units_)
      if (outstanding_.count(unit.timestamp) != 0)
        return unit.timestamp;
    return 0;
  }
  const AccessUnit *Find(uint64_t token) const {
    if (!token) return nullptr;
    for (const AccessUnit &unit : units_) if (unit.timestamp == token) return &unit;
    return nullptr;
  }
  bool failed() const { return phase_ == Phase::Failed; }
  bool sealed() const { return phase_ == Phase::Sealed; }
  bool ended() const { return phase_ == Phase::Ended; }
  bool HasQueuedInput() const { return cursor_ < units_.size(); }
  bool Drained() const {
    return !failed() && !HasQueuedInput() && outstanding_.empty() &&
           !sealed() && !NeedsRestart();
  }
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
  void Prune() {
    if (phase_ != Phase::Running || units_.empty()) return;
    uint64_t floor = std::numeric_limits<uint64_t>::max();
    for (const AccessUnit &unit : units_) {
      if (!unit.completed || outstanding_.count(unit.timestamp) ||
          unit.timestamp == previous_ || unit.timestamp == newest_)
        floor = std::min(floor, unit.root);
    }
    if (floor == std::numeric_limits<uint64_t>::max()) return;
    if (root_floor_ && floor > root_floor_) replay_work_ = 0;
    root_floor_ = floor;
    // Stable compaction is O(max pictures), keeps all anchor AUs in coded
    // order, and never removes pending or currently replaying entries.
    size_t keep = 0, removed_before_cursor = 0;
    for (size_t i = 0; i < units_.size(); ++i) {
      const AccessUnit &unit = units_[i];
      bool remove = unit.completed && !outstanding_.count(unit.timestamp) &&
                    (unit.kind == Kind::B || unit.timestamp < floor);
      if (remove) {
        cache_bytes_ -= unit.bytes.size();
        if (i < cursor_) ++removed_before_cursor;
      } else {
        if (keep != i) units_[keep] = std::move(units_[i]);
        ++keep;
      }
    }
    units_.resize(keep);
    cursor_ -= removed_before_cursor;
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

// MPEG-2 and MPEG-4 Part 2 use the same bounded anchor/reference replay model;
// their bitstream assemblers and timing state remain separate.
using CrystalHDMpeg4Replay = CrystalHDMpeg2Replay;
#endif
