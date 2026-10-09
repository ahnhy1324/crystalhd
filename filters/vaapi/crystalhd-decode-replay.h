// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_DECODE_REPLAY_H
#define CRYSTALHD_DECODE_REPLAY_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_set>
#include <utility>
#include <vector>

// Transport state only: a sealed batch is a real finite H.264 sequence. New
// input is retained until EOS completes, then its original IDR prefix rebuilds
// hardware reference pictures. Public pictures are completed exactly once.
// Optional live transport may retire completed history, but never recreates
// missing references after EOS or discards queued or outstanding pictures.
class CrystalHDDecodeReplay {
 public:
  struct Limits {
    size_t picture_bytes = 512 * 1024;
    size_t cache_bytes = 32 * 1024 * 1024;
    size_t pictures = 512;
    size_t replay_pictures = 8192;
  };
  struct AccessUnit {
    uint64_t timestamp;
    bool idr;
    std::vector<uint8_t> bytes;
    bool completed = false;
  };
  enum class Output { New, Duplicate, Unknown, Invalid };

  CrystalHDDecodeReplay() = default;
  explicit CrystalHDDecodeReplay(bool live) : live_(live) {}
  explicit CrystalHDDecodeReplay(Limits limits, bool live = false)
      : limits_(limits), live_(live) {}

  bool Append(uint64_t timestamp, bool idr, std::vector<uint8_t> bytes) {
    Prune();
    if (failed() || timestamp == 0 || bytes.empty() ||
        (last_timestamp_ == 0 && !idr) || timestamp <= last_timestamp_)
      return Fail("input must begin at an actual IDR with unique timestamps");
    if (bytes.size() > limits_.picture_bytes ||
        bytes.size() > limits_.cache_bytes)
      return Fail("compressed IDR replay cache limit exceeded");
    if (live_)
      MakeRoom(bytes.size());
    if (!HasRoom(bytes.size()))
      return Fail("compressed IDR replay cache limit exceeded");
    cache_bytes_ += bytes.size();
    units_.push_back({timestamp, idr, std::move(bytes), false});
    last_timestamp_ = timestamp;
    return true;
  }

  const AccessUnit *NextInput() {
    if (phase_ != Phase::Running || cursor_ == units_.size())
      return nullptr;
    if (units_[cursor_].completed && replay_work_ >= limits_.replay_pictures) {
      Fail("IDR replay work limit exceeded");
      return nullptr;
    }
    return &units_[cursor_];
  }

  bool InputSent() {
    const AccessUnit *unit = NextInput();
    if (unit == nullptr)
      return Fail("input submitted outside the running batch");
    if (unit->completed)
      ++replay_work_;
    outstanding_.insert(unit->timestamp);
    ++cursor_;
    return true;
  }

  bool CanSeal() const {
    return phase_ == Phase::Running && cursor_ == units_.size() &&
           !outstanding_.empty();
  }
  bool Seal() {
    if (!CanSeal())
      return Fail("cannot seal a batch with unsubmitted input");
    sealed_count_ = cursor_;
    phase_ = Phase::Sealed;
    return true;
  }
  bool EndOfSequence() {
    if (phase_ != Phase::Sealed || !outstanding_.empty())
      return Fail("EOS arrived before every submitted timestamp completed");
    phase_ = Phase::Ended;
    return true;
  }
  bool NeedsRestart() const {
    return phase_ == Phase::Ended && units_.size() > sealed_count_;
  }
  bool Restarted() {
    if (!NeedsRestart())
      return Fail("no retained IDR prefix for decoder restart");
    if (live_ && !replayable_) {
      // Old tail pictures are complete before this call. A retained newer
      // actual IDR can restore replay; otherwise only the first accepted
      // continuation may establish a fresh stream. Never skip queued P input.
      Prune();
      if (!replayable_) {
        if (!units_[sealed_count_].idr)
          return Fail("live continuation after EOS requires an actual IDR");
        RemovePrefix(sealed_count_);
        replayable_ = true;
        replay_work_ = 0;
      }
    }
    if (units_.empty() || !units_.front().idr)
      return Fail("no retained IDR prefix for decoder restart");
    phase_ = Phase::Running;
    // A batch may finish every picture only after it was sealed, when pruning
    // is prohibited. Reclaim its completed old IDR prefixes before replay;
    // otherwise regular IDRs would still eventually exhaust the bounded cache.
    Prune();
    cursor_ = 0;
    return true;
  }

  Output Observe(uint64_t timestamp) {
    if (failed())
      return Output::Invalid;
    for (AccessUnit &unit : units_) {
      if (unit.timestamp != timestamp)
        continue;
      const bool expected = outstanding_.erase(timestamp) != 0;
      if (unit.completed)
        return Output::Duplicate;
      if (!expected) {
        Fail("output timestamp was not submitted to this hardware batch");
        return Output::Invalid;
      }
      unit.completed = true;
      Prune();
      return Output::New;
    }
    return Output::Unknown;  // Firmware can report an untimestamped repeat.
  }

  bool Fail(const char *reason) {
    phase_ = Phase::Failed;
    failure_ = reason;
    return false;
  }
  bool failed() const { return phase_ == Phase::Failed; }
  bool sealed() const { return phase_ == Phase::Sealed; }
  const char *failure() const { return failure_; }
  size_t cached_pictures() const { return units_.size(); }
  size_t outstanding() const { return outstanding_.size(); }
  bool replayable() const { return replayable_; }

 private:
  enum class Phase { Running, Sealed, Ended, Failed };
  bool HasRoom(size_t bytes) const {
    return bytes <= limits_.cache_bytes - cache_bytes_ &&
           units_.size() < limits_.pictures;
  }
  void MakeRoom(size_t bytes) {
    // Completion of another picture is not permission to drop its pending
    // neighbour. Retire only sent, genuinely completed, non-replayed input.
    for (size_t i = 0; !HasRoom(bytes) && i < cursor_;) {
      const auto &unit = units_[i];
      if (!unit.completed || outstanding_.count(unit.timestamp) != 0) {
        ++i;
        continue;
      }
      if (unit.timestamp > lost_history_timestamp_)
        lost_history_timestamp_ = unit.timestamp;
      cache_bytes_ -= unit.bytes.size();
      units_.erase(units_.begin() + i);
      --cursor_;
      if (i < sealed_count_)
        --sealed_count_;
      replayable_ = false;
    }
  }
  void RemovePrefix(size_t prefix) {
    cursor_ -= prefix;
    sealed_count_ = sealed_count_ > prefix ? sealed_count_ - prefix : 0;
    while (prefix-- != 0) {
      cache_bytes_ -= units_.front().bytes.size();
      units_.pop_front();
    }
  }
  void Prune() {
    if (phase_ != Phase::Running && !(live_ && phase_ == Phase::Ended))
      return;
    size_t prefix = 0;
    bool have_prefix = false;
    bool complete = true;
    for (size_t i = 0; i < cursor_; ++i) {
      if (units_[i].idr && complete &&
          (replayable_ || units_[i].timestamp > lost_history_timestamp_)) {
        prefix = i;
        have_prefix = true;
      }
      complete = complete && units_[i].completed &&
                 outstanding_.count(units_[i].timestamp) == 0;
    }
    if (!have_prefix || (prefix == 0 && replayable_))
      return;
    RemovePrefix(prefix);
    replayable_ = true;
    replay_work_ = 0;
  }

  Limits limits_;
  std::deque<AccessUnit> units_;
  std::unordered_set<uint64_t> outstanding_;
  size_t cursor_ = 0;
  size_t sealed_count_ = 0;
  size_t cache_bytes_ = 0;
  size_t replay_work_ = 0;
  uint64_t last_timestamp_ = 0;
  uint64_t lost_history_timestamp_ = 0;
  bool live_ = false;
  bool replayable_ = true;
  Phase phase_ = Phase::Running;
  const char *failure_ = "decoder replay failed";
};
#endif
