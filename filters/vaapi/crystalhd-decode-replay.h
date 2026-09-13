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
  explicit CrystalHDDecodeReplay(Limits limits) : limits_(limits) {}

  bool Append(uint64_t timestamp, bool idr, std::vector<uint8_t> bytes) {
    Prune();
    if (failed() || timestamp == 0 || bytes.empty() ||
        (units_.empty() && !idr) ||
        (!units_.empty() && timestamp <= units_.back().timestamp))
      return Fail("input must begin at an actual IDR with unique timestamps");
    if (bytes.size() > limits_.picture_bytes ||
        bytes.size() > limits_.cache_bytes - cache_bytes_ ||
        units_.size() >= limits_.pictures)
      return Fail("compressed IDR replay cache limit exceeded");
    cache_bytes_ += bytes.size();
    units_.push_back({timestamp, idr, std::move(bytes), false});
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
    if (!NeedsRestart() || units_.empty() || !units_.front().idr)
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

 private:
  enum class Phase { Running, Sealed, Ended, Failed };
  void Prune() {
    if (phase_ != Phase::Running)
      return;
    size_t prefix = 0;
    bool complete = true;
    for (size_t i = 0; i < cursor_; ++i) {
      if (i != 0 && units_[i].idr && complete)
        prefix = i;
      complete = complete && units_[i].completed &&
                 outstanding_.count(units_[i].timestamp) == 0;
    }
    if (prefix == 0)
      return;
    cursor_ -= prefix;
    while (prefix-- != 0) {
      cache_bytes_ -= units_.front().bytes.size();
      units_.pop_front();
    }
    replay_work_ = 0;
  }

  Limits limits_;
  std::deque<AccessUnit> units_;
  std::unordered_set<uint64_t> outstanding_;
  size_t cursor_ = 0;
  size_t sealed_count_ = 0;
  size_t cache_bytes_ = 0;
  size_t replay_work_ = 0;
  Phase phase_ = Phase::Running;
  const char *failure_ = "decoder replay failed";
};
#endif
