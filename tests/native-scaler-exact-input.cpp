// SPDX-License-Identifier: LGPL-2.1-or-later
// OFFLINE ONLY: production I_PCM serializer plus an explicit memory-capture
// DtsProcInput mock. Mock SUCCESS commits serializer state, not device input.
#include "libcrystalhd_raw_frame.h"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr unsigned W = BC_RAW_FRAME_WIDTH, H = BC_RAW_FRAME_HEIGHT;
static_assert(W == 256 && H == 96 && BC_RAW_FRAME_FPS == 30,
              "the fixture profiles require the fixed 256x96/30fps serializer");
constexpr size_t Y = W * H, UV = Y / 4;
enum class Corpus { Initial, Holdout, Support };
using Bytes = std::vector<uint8_t>;
void Need(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
struct File {
  int fd = -1;
  File(int directory, const char *name) {
    fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    Need(fd >= 0, "new output open; partial outputs are retained on failure");
  }
  ~File() { if (fd >= 0) close(fd); }
  void Write(const void *data, size_t size) {
    const auto *p = static_cast<const uint8_t *>(data);
    while (size) {
      const auto n = write(fd, p, size);
      if (n < 0 && errno == EINTR) continue;
      Need(n > 0, "output write"); p += n; size -= static_cast<size_t>(n);
    }
  }
  void Text(const std::string &s) { Write(s.data(), s.size()); }
  void Close() { Need(close(fd) == 0, "output close"); fd = -1; }
};
struct Builder {
  BC_RAW_FRAME_BUILDER *value = nullptr;
  Builder() { Need(DtsRawFrameCreate(&value) == BC_STS_SUCCESS, "builder create"); }
  ~Builder() { if (value) DtsRawFrameDestroy(&value); }
  void Close() { Need(DtsRawFrameDestroy(&value) == BC_STS_SUCCESS, "builder close"); }
};
struct Mock {
  bool armed = false;
  const uint8_t *au = nullptr;
  uint32_t bytes = 0;
  uint64_t token = 0;
  unsigned calls = 0;
  Bytes captured;
} mock;
HANDLE MockHandle() { return reinterpret_cast<HANDLE>(uintptr_t{0x12340}); }

const std::array<const char *, 12> initial_names = {{
  "dc96", "dc160", "ramp64to192", "even-plus16", "even-minus16",
  "odd-plus16", "odd-minus16", "even-plus32", "even-minus32",
  "odd-plus32", "odd-minus32", "asymmetric-step96-160-128"}};
const std::array<const char *, 12> holdout_names = {{
  "dc0", "dc255", "xorshift32-shuffle-seed43524844-full0to255",
  "even-plus4", "even-minus4", "odd-plus12", "odd-minus12",
  "even-plus3", "even-minus3", "odd-plus7", "odd-minus7",
  "asymmetric-step0-255-17"}};
const std::array<const char *, 12> support_names = {{
  "ramp-x", "reverse255-x", "ramp-even-plus32", "ramp-even-minus32",
  "ramp-odd-plus32", "ramp-odd-minus32", "ramp-even-plus64", "ramp-even-minus64",
  "ramp-odd-plus64", "ramp-odd-minus64", "flat128", "flat128-even-plus32"}};
Bytes Profile(unsigned index, Corpus corpus) {
  Bytes row(W, 128);
  if (corpus == Corpus::Support) {
    for (unsigned x = 0; x != W; ++x)
      row[x] = index == 1 ? static_cast<uint8_t>(255 - x) :
               index >= 10 ? 128 : static_cast<uint8_t>(x);
    if (index >= 2 && index <= 9) {
      const unsigned group = (index - 2) / 2;
      const unsigned at = (group & 1U) ? 129 : 128;
      const int amplitude = group < 2 ? 32 : 64;
      const int value = row[at] + ((index & 1U) ? -amplitude : amplitude);
      Need(value >= 0 && value <= 255, "unclipped support stimulus");
      row[at] = static_cast<uint8_t>(value);
    }
    if (index == 11) row[128] = 160;
    return row;
  }
  if (corpus == Corpus::Holdout) {
    for (unsigned x = 0; x != W; ++x) {
      if (index == 0) row[x] = 0;
      else if (index == 1) row[x] = 255;
      else if (index == 2) row[x] = static_cast<uint8_t>(x);
      else if (index == 11) row[x] = x < 127 ? 0 : x < 137 ? 255 : 17;
    }
    if (index == 2) {
      uint32_t state = UINT32_C(0x43524844);
      for (unsigned i = W - 1; i != 0; --i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        const unsigned j = state % (i + 1);
        const auto value = row[i]; row[i] = row[j]; row[j] = value;
      }
    }
    if (index >= 3 && index <= 10) {
      const unsigned group = (index - 3) / 2;
      const unsigned at = (group & 1U) ? 129 : 128;
      const std::array<int, 4> amplitudes = {{4, 12, 3, 7}};
      row[at] = static_cast<uint8_t>(128 + ((index & 1U) ? amplitudes[group] : -amplitudes[group]));
    }
    return row;
  }
  for (unsigned x = 0; x != W; ++x) {
    if (index == 0) row[x] = 96;
    else if (index == 1) row[x] = 160;
    else if (index == 2) row[x] = static_cast<uint8_t>(64 + 128 * x / (W - 1));
    else if (index == 11) row[x] = x < 128 ? 96 : x < 136 ? 160 : 128;
  }
  if (index >= 3 && index <= 10) {
    const unsigned at = ((index - 3) % 4) >= 2 ? 129 : 128;
    const int amplitude = index < 7 ? 16 : 32;
    row[at] = static_cast<uint8_t>(128 + ((index & 1U) ? amplitude : -amplitude));
  }
  return row;
}
}  // namespace

extern "C" BC_STATUS DtsProcInput(HANDLE device, uint8_t *data, uint32_t size,
                                  uint64_t timestamp, BOOL encrypted) {
  if (!mock.armed || device != MockHandle() || data != mock.au || size != mock.bytes ||
      timestamp != mock.token || encrypted != FALSE || !size ||
      size > BC_RAW_FRAME_AU_CAPACITY) return BC_STS_INV_ARG;
  ++mock.calls;
  try { mock.captured.assign(data, data + size); }
  catch (...) { return BC_STS_INSUFF_RES; }
  return BC_STS_SUCCESS;  // OFFLINE capture only; no syscall or device access.
}

int main(int argc, char **argv) {
  int directory = -1;
  try {
    Need(argc == 5 && !std::strcmp(argv[1], "--case") &&
             (!std::strcmp(argv[2], "initial") || !std::strcmp(argv[2], "holdout") ||
              !std::strcmp(argv[2], "support")) &&
             !std::strcmp(argv[3], "--emit"),
         "usage: native-scaler-exact-input --case initial|holdout|support --emit NEW_DIRECTORY");
    const Corpus corpus = !std::strcmp(argv[2], "initial") ? Corpus::Initial :
                          !std::strcmp(argv[2], "holdout") ? Corpus::Holdout : Corpus::Support;
    const auto &names = corpus == Corpus::Initial ? initial_names :
                        corpus == Corpus::Holdout ? holdout_names : support_names;
    Need(mkdir(argv[4], 0700) == 0, "refusing existing output directory");
    directory = open(argv[4], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    Need(directory >= 0, "new directory open");
    File coded(directory, "fixture.h264"), known(directory, "known.yuv420p"),
         profiles(directory, "profiles.json"), index(directory, "au-index.tsv");
    profiles.Text("{\"schema\":1,\"offline_mock_only\":true,\"hardware_access\":false,"
                  "\"width\":256,\"height\":96,\"fps\":30,\"frames\":24,"
                  "\"neutral_chroma\":128,\"profiles\":[\n");
    index.Text("frame\tprofile\tslot\toffset\tbytes\tmock_token\n");
    Builder builder;
    uint64_t offset = 0;
    for (unsigned profile = 0; profile != names.size(); ++profile) {
      const Bytes row = Profile(profile, corpus);
      Bytes raster(Y + 2 * UV, 128);
      for (unsigned y = 0; y != H; ++y) std::memcpy(raster.data() + y * W, row.data(), W);
      profiles.Text(std::string(profile ? "," : "") + "{\"id\":" + std::to_string(profile) +
                    ",\"name\":\"" + names[profile] + "\",\"luma\":[");
      for (unsigned x = 0; x != W; ++x)
        profiles.Text(std::string(x ? "," : "") + std::to_string(row[x]));
      profiles.Text("]}\n");
      const BC_RAW_FRAME_PLANES planes = {{raster.data(), raster.data() + Y,
                                          raster.data() + Y + UV},
                                         {W, W / 2, W / 2}, {Y, UV, UV}};
      for (unsigned duplicate = 0; duplicate != 2; ++duplicate) {
        const unsigned frame = profile * 2 + duplicate, slot = frame & 1U;
        const uint8_t *au = nullptr;
        uint32_t bytes = 0;
        Need(DtsRawFramePrepareUpload(builder.value, slot, &planes, &au, &bytes) ==
                 BC_STS_SUCCESS && au && bytes, "prepare I_PCM upload");
        mock.au = au; mock.bytes = bytes; mock.token = (frame + 1) * UINT64_C(100000);
        mock.captured.clear(); mock.armed = true;
        const auto before = mock.calls;
        const auto status = DtsRawFrameSubmit(builder.value, MockHandle(), mock.token);
        mock.armed = false;
        Need(status == BC_STS_SUCCESS && mock.calls == before + 1 &&
                 mock.captured.size() == bytes, "one exact offline mock capture");
        coded.Write(mock.captured.data(), mock.captured.size());
        known.Write(raster.data(), raster.size());
        index.Text(std::to_string(frame) + "\t" + std::to_string(profile) + "\t" +
                   std::to_string(slot) + "\t" + std::to_string(offset) + "\t" +
                   std::to_string(bytes) + "\t" + std::to_string(mock.token) + "\n");
        offset += bytes;
      }
    }
    profiles.Text("]}\n");
    builder.Close(); coded.Close(); known.Close(); profiles.Close(); index.Close();
    Need(close(directory) == 0, "directory close"); directory = -1;
    std::printf("offline_fixture=PASS case=%s frames=24 profiles=12 width=256 height=96 "
                "fps=30 coded_bytes=%llu mock_calls=%u hardware_access=0 "
                "DtsProcInput=MEMORY_CAPTURE_MOCK native_acceptance=NOT_RUN\n",
                argv[2], static_cast<unsigned long long>(offset), mock.calls);
    return 0;
  } catch (const std::exception &error) {
    if (directory >= 0) close(directory);
    std::fprintf(stderr, "offline fixture FAIL: %s; partial files retained\n", error.what());
    return 1;
  }
}
