// SPDX-License-Identifier: LGPL-2.1-or-later
// Shared transport-lifecycle contract only. Codec-specific replay/reference
// policy remains covered by each codec's existing test suite.

#include "../filters/vaapi/crystalhd-decode-replay.h"
#include "../filters/vaapi/crystalhd-mpeg2-replay.h"
#include "../filters/vaapi/crystalhd-vc1-replay.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using Output = CrystalHDDecodeReplay::Output;

struct ContractLimits {
  size_t picture_bytes = 4096;
  size_t cache_bytes = 64 * 4096;
  size_t pictures = 64;
  size_t replay_pictures = 64;
};

std::vector<uint8_t> Bytes(uint64_t token) {
  std::vector<uint8_t> bytes = {0x00, 0x00, 0x01, 0x00,
                                0x00, 0x00, 0x01, 0x01};
  for (unsigned int byte = 0; byte < 8; ++byte)
    bytes.push_back(static_cast<uint8_t>(token >> (byte * 8)));
  return bytes;
}

class H264Adapter {
 public:
  H264Adapter() : H264Adapter(ContractLimits{}) {}
  explicit H264Adapter(const ContractLimits &limits)
      : replay_(NativeLimits(limits)) {}

  bool AppendRoot(uint64_t token, const std::vector<uint8_t> &bytes) {
    return replay_.Append(token, true, bytes);
  }
  bool AppendPredicted(uint64_t token, uint64_t,
                       const std::vector<uint8_t> &bytes) {
    return replay_.Append(token, false, bytes);
  }
  bool Peek(uint64_t *token, const std::vector<uint8_t> **bytes) {
    const auto *unit = replay_.NextInput();
    if (unit == nullptr)
      return false;
    *token = unit->timestamp;
    *bytes = &unit->bytes;
    return true;
  }
  bool InputSent() { return replay_.InputSent(); }
  bool CanSeal() const { return replay_.CanSeal(); }
  bool Seal() { return replay_.Seal(); }
  bool EndOfSequence() { return replay_.EndOfSequence(); }
  bool NeedsRestart() const { return replay_.NeedsRestart(); }
  bool Restarted() { return replay_.Restarted(); }
  Output Observe(uint64_t token) { return replay_.Observe(token); }
  bool Fail(const char *reason) { return replay_.Fail(reason); }
  bool failed() const { return replay_.failed(); }
  bool sealed() const { return replay_.sealed(); }
  const char *failure() const { return replay_.failure(); }
  size_t cached_pictures() const { return replay_.cached_pictures(); }
  size_t cached_bytes() const { return replay_.cached_bytes(); }
  size_t outstanding() const { return replay_.outstanding(); }

 private:
  static CrystalHDDecodeReplay::Limits NativeLimits(
      const ContractLimits &limits) {
    CrystalHDDecodeReplay::Limits native;
    native.picture_bytes = limits.picture_bytes;
    native.cache_bytes = limits.cache_bytes;
    native.pictures = limits.pictures;
    native.replay_pictures = limits.replay_pictures;
    return native;
  }

  CrystalHDDecodeReplay replay_;
};

template <typename Replay>
class AnchorAdapter {
 public:
  using Kind = typename Replay::Kind;

  AnchorAdapter() : AnchorAdapter(ContractLimits{}) {}
  explicit AnchorAdapter(const ContractLimits &limits)
      : replay_(NativeLimits(limits)) {}

  bool AppendRoot(uint64_t token, const std::vector<uint8_t> &bytes) {
    return replay_.Append(token, Kind::I, 0, 0, bytes);
  }
  bool AppendPredicted(uint64_t token, uint64_t forward,
                       const std::vector<uint8_t> &bytes) {
    return replay_.Append(token, Kind::P, forward, 0, bytes);
  }
  bool Peek(uint64_t *token, const std::vector<uint8_t> **bytes) {
    const auto *unit = replay_.NextInput();
    if (unit == nullptr)
      return false;
    *token = unit->timestamp;
    *bytes = &unit->bytes;
    return true;
  }
  bool InputSent() { return replay_.InputSent(); }
  bool CanSeal() const { return replay_.CanSeal(); }
  bool Seal() { return replay_.Seal(); }
  bool EndOfSequence() { return replay_.EndOfSequence(); }
  bool NeedsRestart() const { return replay_.NeedsRestart(); }
  bool Restarted() { return replay_.Restarted(); }
  Output Observe(uint64_t token) { return replay_.Observe(token); }
  bool Fail(const char *reason) { return replay_.Fail(reason); }
  bool failed() const { return replay_.failed(); }
  bool sealed() const { return replay_.sealed(); }
  const char *failure() const { return replay_.failure(); }
  size_t cached_pictures() const { return replay_.cached_pictures(); }
  size_t cached_bytes() const { return replay_.cached_bytes(); }
  size_t outstanding() const { return replay_.outstanding(); }

 private:
  static typename Replay::Limits NativeLimits(const ContractLimits &limits) {
    typename Replay::Limits native;
    native.picture_bytes = limits.picture_bytes;
    native.cache_bytes = limits.cache_bytes;
    native.pictures = limits.pictures;
    native.replay_pictures = limits.replay_pictures;
    return native;
  }

  Replay replay_;
};

using Mpeg2Adapter = AnchorAdapter<CrystalHDMpeg2Replay>;
using Vc1Adapter = AnchorAdapter<CrystalHDVc1Replay>;

struct Checks {
  size_t checks = 0;
  size_t failures = 0;
  size_t groups = 0;

  bool Check(bool result, const char *expression, const char *function,
             int line) {
    ++checks;
    if (!result) {
      ++failures;
      std::fprintf(stderr, "%s:%d: %s\n", function, line, expression);
    }
    return result;
  }
};

#define CHECK(checks, expression) \
  (checks).Check(static_cast<bool>(expression), #expression, __func__, __LINE__)
#define REQUIRE(checks, expression) \
  do { if (!CHECK(checks, expression)) return; } while (0)

template <typename Adapter>
std::vector<uint64_t> SendAll(Adapter *replay, Checks *checks) {
  constexpr size_t kMaxContractInputs = 1024;
  std::vector<uint64_t> sent;
  uint64_t token = 0;
  const std::vector<uint8_t> *bytes = nullptr;
  while (replay->Peek(&token, &bytes)) {
    if (!CHECK(*checks, sent.size() < kMaxContractInputs))
      break;
    CHECK(*checks, bytes != nullptr && *bytes == Bytes(token));
    sent.push_back(token);
    if (!CHECK(*checks, replay->InputSent()))
      break;
  }
  return sent;
}

template <typename Adapter>
void OwnedInputAndValidation(Checks &checks) {
  {
    Adapter replay;
    auto original = Bytes(1);
    REQUIRE(checks, replay.AppendRoot(1, original));
    original.assign(64, 0xff);
    uint64_t token = 0;
    const std::vector<uint8_t> *bytes = nullptr;
    REQUIRE(checks, replay.Peek(&token, &bytes));
    CHECK(checks, token == 1);
    CHECK(checks, bytes != nullptr && *bytes == Bytes(1));
    CHECK(checks, replay.cached_pictures() == 1);
    CHECK(checks, replay.cached_bytes() == Bytes(1).size());
    REQUIRE(checks, replay.InputSent());
    CHECK(checks, replay.Observe(1) == Output::New);
    CHECK(checks, replay.Observe(1) == Output::Duplicate);
    CHECK(checks, !replay.failed());
  }
  {
    Adapter replay;
    CHECK(checks, !replay.AppendRoot(0, Bytes(0)));
    CHECK(checks, replay.failed());
    CHECK(checks, replay.cached_pictures() == 0 && replay.cached_bytes() == 0);
  }
  {
    Adapter replay;
    CHECK(checks, !replay.AppendRoot(1, {}));
    CHECK(checks, replay.failed());
    CHECK(checks, replay.cached_pictures() == 0 && replay.cached_bytes() == 0);
  }
  {
    Adapter replay;
    CHECK(checks, !replay.AppendPredicted(1, 0, Bytes(1)));
    CHECK(checks, replay.failed());
    CHECK(checks, replay.cached_pictures() == 0 && replay.cached_bytes() == 0);
  }
  {
    Adapter replay;
    REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
    const size_t pictures = replay.cached_pictures();
    const size_t bytes = replay.cached_bytes();
    CHECK(checks, !replay.AppendPredicted(1, 1, Bytes(1)));
    CHECK(checks, replay.failed());
    CHECK(checks, replay.cached_pictures() == pictures);
    CHECK(checks, replay.cached_bytes() == bytes);
  }
  {
    Adapter replay;
    REQUIRE(checks, replay.AppendRoot(2, Bytes(2)));
    const size_t pictures = replay.cached_pictures();
    const size_t bytes = replay.cached_bytes();
    CHECK(checks, !replay.AppendRoot(1, Bytes(1)));
    CHECK(checks, replay.failed());
    CHECK(checks, replay.cached_pictures() == pictures);
    CHECK(checks, replay.cached_bytes() == bytes);
  }
}

template <typename Adapter>
void OutputAndPhaseFailures(Checks &checks) {
  {
    Adapter replay;
    CHECK(checks, !replay.Seal());
    CHECK(checks, replay.failed());
  }
  {
    Adapter replay;
    REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
    CHECK(checks, !replay.Seal());
    CHECK(checks, replay.failed());
  }
  {
    Adapter replay;
    REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
    REQUIRE(checks, replay.AppendPredicted(2, 1, Bytes(2)));
    uint64_t token = 0;
    const std::vector<uint8_t> *bytes = nullptr;
    REQUIRE(checks, replay.Peek(&token, &bytes));
    CHECK(checks, token == 1);
    REQUIRE(checks, replay.InputSent());
    CHECK(checks, !replay.CanSeal());
    CHECK(checks, !replay.Seal());
    CHECK(checks, replay.failed());
    CHECK(checks, replay.outstanding() == 1);
  }
  {
    Adapter replay;
    REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
    REQUIRE(checks, SendAll(&replay, &checks) == std::vector<uint64_t>({1}));
    CHECK(checks, !replay.EndOfSequence());
    CHECK(checks, replay.failed());
  }
  {
    Adapter replay;
    REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
    REQUIRE(checks, SendAll(&replay, &checks) == std::vector<uint64_t>({1}));
    REQUIRE(checks, replay.Seal());
    CHECK(checks, !replay.EndOfSequence());
    CHECK(checks, replay.failed());
    CHECK(checks, replay.outstanding() == 1);
  }
  {
    Adapter replay;
    REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
    CHECK(checks, replay.Observe(1) == Output::Invalid);
    CHECK(checks, replay.failed());
  }
  {
    Adapter replay;
    REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
    REQUIRE(checks, SendAll(&replay, &checks) == std::vector<uint64_t>({1}));
    CHECK(checks, replay.Observe(999) == Output::Unknown);
    CHECK(checks, replay.outstanding() == 1);
    CHECK(checks, replay.Observe(1) == Output::New);
    CHECK(checks, replay.Observe(1) == Output::Duplicate);
    CHECK(checks, !replay.failed());
  }
  {
    Adapter replay;
    REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
    REQUIRE(checks, SendAll(&replay, &checks) == std::vector<uint64_t>({1}));
    REQUIRE(checks, replay.Seal());
    REQUIRE(checks, replay.AppendPredicted(2, 1, Bytes(2)));
    CHECK(checks, !replay.Restarted());
    CHECK(checks, replay.failed());
  }
  {
    Adapter replay;
    CHECK(checks, !replay.InputSent());
    CHECK(checks, replay.failed());
  }
  {
    Adapter replay;
    REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
    REQUIRE(checks, SendAll(&replay, &checks) == std::vector<uint64_t>({1}));
    CHECK(checks, !replay.InputSent());
    CHECK(checks, replay.failed());
  }
  {
    Adapter replay;
    REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
    REQUIRE(checks, SendAll(&replay, &checks) == std::vector<uint64_t>({1}));
    REQUIRE(checks, replay.Seal());
    CHECK(checks, replay.Observe(1) == Output::New);
    REQUIRE(checks, replay.EndOfSequence());
    CHECK(checks, !replay.NeedsRestart());
    CHECK(checks, !replay.Restarted());
    CHECK(checks, replay.failed());
  }
  {
    Adapter replay;
    REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
    CHECK(checks, !replay.Fail("caller teardown failed"));
    CHECK(checks, replay.failed());
    CHECK(checks, std::strcmp(replay.failure(), "caller teardown failed") == 0);
    const size_t pictures = replay.cached_pictures();
    const size_t cached = replay.cached_bytes();
    CHECK(checks, !replay.AppendRoot(2, Bytes(2)));
    CHECK(checks, replay.failed());
    CHECK(checks, replay.cached_pictures() == pictures);
    CHECK(checks, replay.cached_bytes() == cached);
    uint64_t token = 0;
    const std::vector<uint8_t> *bytes = nullptr;
    CHECK(checks, !replay.Peek(&token, &bytes));
    CHECK(checks, replay.Observe(1) == Output::Invalid);
    CHECK(checks, !replay.Restarted());
    CHECK(checks, replay.failed());
  }
}

template <typename Adapter>
void SealedContinuationAndRestart(Checks &checks) {
  Adapter replay;
  REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
  REQUIRE(checks, replay.AppendPredicted(2, 1, Bytes(2)));
  REQUIRE(checks, SendAll(&replay, &checks) ==
                      std::vector<uint64_t>({1, 2}));
  CHECK(checks, replay.CanSeal());
  REQUIRE(checks, replay.Seal());
  CHECK(checks, replay.sealed());
  REQUIRE(checks, replay.AppendPredicted(3, 2, Bytes(3)));
  uint64_t token = 0;
  const std::vector<uint8_t> *bytes = nullptr;
  CHECK(checks, !replay.Peek(&token, &bytes));
  CHECK(checks, !replay.NeedsRestart());
  CHECK(checks, replay.Observe(999) == Output::Unknown);
  CHECK(checks, replay.outstanding() == 2);
  CHECK(checks, replay.Observe(2) == Output::New);
  CHECK(checks, replay.Observe(1) == Output::New);
  REQUIRE(checks, replay.EndOfSequence());
  CHECK(checks, replay.NeedsRestart());
  REQUIRE(checks, replay.Restarted());
  REQUIRE(checks, SendAll(&replay, &checks) ==
                      std::vector<uint64_t>({1, 2, 3}));
  REQUIRE(checks, replay.Seal());
  CHECK(checks, replay.Observe(2) == Output::Duplicate);
  CHECK(checks, replay.Observe(3) == Output::New);
  CHECK(checks, replay.Observe(1) == Output::Duplicate);
  REQUIRE(checks, replay.EndOfSequence());
  CHECK(checks, !replay.NeedsRestart());
  CHECK(checks, !replay.failed());
}

template <typename Adapter>
void LateInputAfterCompletedEos(Checks &checks) {
  Adapter replay;
  REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
  REQUIRE(checks, SendAll(&replay, &checks) == std::vector<uint64_t>({1}));
  REQUIRE(checks, replay.Seal());
  CHECK(checks, replay.Observe(1) == Output::New);
  REQUIRE(checks, replay.EndOfSequence());
  CHECK(checks, !replay.NeedsRestart());
  REQUIRE(checks, replay.AppendPredicted(2, 1, Bytes(2)));
  CHECK(checks, replay.NeedsRestart());
  uint64_t token = 0;
  const std::vector<uint8_t> *bytes = nullptr;
  CHECK(checks, !replay.Peek(&token, &bytes));
  REQUIRE(checks, replay.Restarted());
  REQUIRE(checks, SendAll(&replay, &checks) ==
                      std::vector<uint64_t>({1, 2}));
  REQUIRE(checks, replay.Seal());
  CHECK(checks, replay.Observe(1) == Output::Duplicate);
  CHECK(checks, replay.Observe(2) == Output::New);
  REQUIRE(checks, replay.EndOfSequence());
  CHECK(checks, !replay.NeedsRestart());
}

template <typename Adapter>
void BoundsAndReplayWork(Checks &checks) {
  const size_t bytes = Bytes(1).size();
  {
    ContractLimits limits;
    limits.picture_bytes = bytes - 1;
    Adapter replay(limits);
    CHECK(checks, !replay.AppendRoot(1, Bytes(1)));
    CHECK(checks, replay.failed());
    CHECK(checks, replay.cached_pictures() == 0 && replay.cached_bytes() == 0);
  }
  {
    ContractLimits limits;
    limits.picture_bytes = bytes;
    limits.cache_bytes = bytes * 2 - 1;
    Adapter replay(limits);
    REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
    const size_t pictures = replay.cached_pictures();
    const size_t cached = replay.cached_bytes();
    CHECK(checks, !replay.AppendPredicted(2, 1, Bytes(2)));
    CHECK(checks, replay.failed());
    CHECK(checks, replay.cached_pictures() == pictures);
    CHECK(checks, replay.cached_bytes() == cached);
  }
  {
    ContractLimits limits;
    limits.pictures = 1;
    Adapter replay(limits);
    REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
    const size_t cached = replay.cached_bytes();
    CHECK(checks, !replay.AppendPredicted(2, 1, Bytes(2)));
    CHECK(checks, replay.failed());
    CHECK(checks, replay.cached_pictures() == 1);
    CHECK(checks, replay.cached_bytes() == cached);
  }
  {
    ContractLimits limits;
    limits.cache_bytes = 0;
    Adapter replay(limits);
    CHECK(checks, !replay.AppendRoot(1, Bytes(1)));
    CHECK(checks, replay.failed());
    CHECK(checks, replay.cached_pictures() == 0 && replay.cached_bytes() == 0);
  }
  {
    ContractLimits limits;
    limits.replay_pictures = 1;
    Adapter replay(limits);
    REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
    REQUIRE(checks, replay.AppendPredicted(2, 1, Bytes(2)));
    REQUIRE(checks, SendAll(&replay, &checks) ==
                        std::vector<uint64_t>({1, 2}));
    REQUIRE(checks, replay.Seal());
    REQUIRE(checks, replay.AppendPredicted(3, 2, Bytes(3)));
    CHECK(checks, replay.Observe(1) == Output::New);
    CHECK(checks, replay.Observe(2) == Output::New);
    REQUIRE(checks, replay.EndOfSequence());
    REQUIRE(checks, replay.Restarted());
    uint64_t token = 0;
    const std::vector<uint8_t> *payload = nullptr;
    REQUIRE(checks, replay.Peek(&token, &payload));
    CHECK(checks, token == 1 && payload != nullptr && *payload == Bytes(1));
    REQUIRE(checks, replay.InputSent());
    const size_t pictures = replay.cached_pictures();
    const size_t cached = replay.cached_bytes();
    CHECK(checks, !replay.Peek(&token, &payload));
    CHECK(checks, replay.failed());
    CHECK(checks, replay.cached_pictures() == pictures);
    CHECK(checks, replay.cached_bytes() == cached);
  }
  {
    // Reaching the replay budget does not block the new continuation, but a
    // second restart over the same root must not reset accumulated work.
    ContractLimits limits;
    limits.replay_pictures = 2;
    Adapter replay(limits);
    REQUIRE(checks, replay.AppendRoot(1, Bytes(1)));
    REQUIRE(checks, replay.AppendPredicted(2, 1, Bytes(2)));
    REQUIRE(checks, SendAll(&replay, &checks) ==
                        std::vector<uint64_t>({1, 2}));
    REQUIRE(checks, replay.Seal());
    REQUIRE(checks, replay.AppendPredicted(3, 2, Bytes(3)));
    CHECK(checks, replay.Observe(1) == Output::New);
    CHECK(checks, replay.Observe(2) == Output::New);
    REQUIRE(checks, replay.EndOfSequence());
    REQUIRE(checks, replay.Restarted());
    CHECK(checks, SendAll(&replay, &checks) ==
                      std::vector<uint64_t>({1, 2, 3}));
    CHECK(checks, !replay.failed());
    REQUIRE(checks, replay.Seal());
    REQUIRE(checks, replay.AppendPredicted(4, 3, Bytes(4)));
    CHECK(checks, replay.Observe(1) == Output::Duplicate);
    CHECK(checks, replay.Observe(2) == Output::Duplicate);
    CHECK(checks, replay.Observe(3) == Output::New);
    REQUIRE(checks, replay.EndOfSequence());
    REQUIRE(checks, replay.Restarted());
    const size_t pictures = replay.cached_pictures();
    const size_t cached = replay.cached_bytes();
    uint64_t token = 0;
    const std::vector<uint8_t> *payload = nullptr;
    CHECK(checks, !replay.Peek(&token, &payload));
    CHECK(checks, replay.failed());
    CHECK(checks, replay.cached_pictures() == pictures);
    CHECK(checks, replay.cached_bytes() == cached);
  }
}

template <typename Adapter, typename Function>
void RunGroup(const char *codec, const char *name, Checks *checks,
              Function function) {
  const size_t before = checks->failures;
  ++checks->groups;
  function(*checks);
  std::printf("%s %s: %s\n", codec, name,
              before == checks->failures ? "PASS" : "FAIL");
}

template <typename Adapter>
void RunContract(const char *codec, Checks *checks) {
  RunGroup<Adapter>(codec, "owned input and validation", checks,
                    OwnedInputAndValidation<Adapter>);
  RunGroup<Adapter>(codec, "output and phase failures", checks,
                    OutputAndPhaseFailures<Adapter>);
  RunGroup<Adapter>(codec, "sealed continuation and restart", checks,
                    SealedContinuationAndRestart<Adapter>);
  RunGroup<Adapter>(codec, "late input after completed EOS", checks,
                    LateInputAfterCompletedEos<Adapter>);
  RunGroup<Adapter>(codec, "bounds and replay work", checks,
                    BoundsAndReplayWork<Adapter>);
}

#undef REQUIRE
#undef CHECK

}  // namespace

int main() {
  Checks checks;
  RunContract<H264Adapter>("H.264", &checks);
  RunContract<Mpeg2Adapter>("MPEG-2", &checks);
  RunContract<Vc1Adapter>("VC-1", &checks);
  std::printf("%zu groups, %zu checks, %zu failures\n",
              checks.groups, checks.checks, checks.failures);
  return checks.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
