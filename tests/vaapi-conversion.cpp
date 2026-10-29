// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual production conversion on private CPU memory; no decoder/DRM device.
// A native -m32 LEGACY_CPU=1 build measures scalar code on the host CPU;
// qemu-i386 -cpu pentium2 is useful for ISA/correctness, never native timing.
// Optional timing: --benchmark progressive|interlaced WIDTH HEIGHT RUNS WARMUP
// Allocations, independent reference, warmup and full checksum are not timed.
#include "../filters/vaapi/crystalhd_drv_video.cpp"

#include <cmath>
#include <stdexcept>
#include <sys/resource.h>

extern "C" void gbm_bo_unmap(gbm_bo *, void *) { abort(); }
extern "C" void gbm_bo_destroy(gbm_bo *) { abort(); }
extern "C" drmVersionPtr drmGetVersion(int) { abort(); }
extern "C" void drmFreeVersion(drmVersionPtr) { abort(); }
extern "C" BC_STATUS DtsDeviceClose(HANDLE) { abort(); }
extern "C" BC_STATUS DtsStopDecoder(HANDLE) { abort(); }
extern "C" BC_STATUS DtsCloseDecoder(HANDLE) { abort(); }

namespace {
unsigned int checks = 0;
volatile uint32_t consumed = 0;

void Require(bool condition, const char *message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}

void Initialize(Surface *surface, unsigned int width, unsigned int height,
                unsigned int offset, bool custom_pitch) {
  surface->width = width;
  surface->height = height;
  surface->fourcc = VA_FOURCC_NV12;
  surface->pitch[0] = custom_pitch ? width + 13 : Align(width, 64);
  surface->pitch[1] = custom_pitch ? Align(width, 2) + 17 : Align(width, 64);
  const size_t y_bytes = static_cast<size_t>(surface->pitch[0]) * height;
  const size_t uv_bytes = static_cast<size_t>(surface->pitch[1]) * ((height + 1) / 2);
  surface->storage.assign(offset + 32 + y_bytes + 31 + uv_bytes + 32, 0xa5);
  surface->planes[0] = surface->storage.data() + offset + 32;
  surface->planes[1] = surface->planes[0] + y_bytes + 31;
}

void Fill(std::vector<uint8_t> *bytes) {
  uint32_t state = 0x83abc451U;
  for (uint8_t &byte : *bytes) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    byte = static_cast<uint8_t>(state);
  }
}

struct Fixture {
  Surface actual, expected;
  std::vector<uint8_t> input;
  BC_DTS_PROC_OUT output = {};
  const uint8_t *fields[2] = {};
  unsigned int source_width;
  unsigned int source_height;
  size_t source_pitch;
  bool interlaced;
  bool link;

  Fixture(unsigned int width, unsigned int height, unsigned int target_width,
          unsigned int target_height, bool fields_mode, bool is_70012,
          unsigned int offset, bool custom_pitch, bool interleaved_fields = false)
      : source_width(width), source_height(height),
        source_pitch(is_70012 ? (width <= 720 ? 720 : width <= 1280 ? 1280 : 1920) * 2U
                             : width * 2U),
        interlaced(fields_mode), link(is_70012) {
    Initialize(&actual, target_width, target_height, offset, custom_pitch);
    Initialize(&expected, target_width, target_height, offset, custom_pitch);
    if (interlaced && custom_pitch) source_pitch += 19;
    input.resize(offset + source_pitch * height);
    Fill(&input);
    output.Ybuff = input.data() + offset;
    output.PicInfo.width = width;
    output.PicInfo.height = height;
    output.PicInfo.timeStamp = 12345;
    fields[0] = output.Ybuff;
    fields[1] = fields[0] + source_pitch * (height / 2U);
    if (interleaved_fields) {
      fields[1] = fields[0] + source_pitch;
      source_pitch *= 2;
    }
    Reference();
  }

  void Reference() {
    // Independent row/pixel oracle, intentionally not a packed-word kernel.
    const unsigned int width = std::min(source_width, expected.width);
    const unsigned int height = std::min(source_height, expected.height);
    for (unsigned int row = 0; row < expected.height; ++row)
      memset(expected.planes[0] + static_cast<size_t>(row) * expected.pitch[0],
             16, expected.width);
    for (unsigned int row = 0; row < (expected.height + 1) / 2; ++row)
      memset(expected.planes[1] + static_cast<size_t>(row) * expected.pitch[1],
             128, Align(expected.width, 2));
    for (unsigned int row = 0; row < height; ++row) {
      const uint8_t *source = interlaced
          ? fields[row % 2] + static_cast<size_t>(row / 2) * source_pitch
          : output.Ybuff + static_cast<size_t>(row) * source_pitch;
      for (unsigned int x = 0; x < width; ++x)
        expected.planes[0][static_cast<size_t>(row) * expected.pitch[0] + x] =
            source[x * 2];
    }
    for (unsigned int row = 0; row < (height + 1) / 2; ++row) {
      const uint8_t *first = interlaced
          ? fields[row % 2] + static_cast<size_t>((row / 2) * 2) * source_pitch
          : output.Ybuff + static_cast<size_t>(row * 2) * source_pitch;
      const uint8_t *second = interlaced
          ? first + source_pitch
          : output.Ybuff + static_cast<size_t>(std::min(row * 2 + 1, height - 1)) * source_pitch;
      for (unsigned int x = 0; x + 1 < width; x += 2) {
        uint8_t *destination = expected.planes[1] +
            static_cast<size_t>(row) * expected.pitch[1] + x;
        destination[0] = (static_cast<unsigned int>(first[x * 2 + 1]) +
                          second[x * 2 + 1] + 1) / 2;
        destination[1] = (static_cast<unsigned int>(first[x * 2 + 3]) +
                          second[x * 2 + 3] + 1) / 2;
      }
    }
  }

  __attribute__((noinline)) bool Convert(uint64_t timestamp) {
    output.PicInfo.timeStamp = timestamp;
    // The barrier exposes the full output to the caller each iteration even
    // when identical deterministic source pixels permit aggressive inlining.
    const bool ok = interlaced
        ? CopyH264InterlacedToSurface(&actual, fields, source_width,
                                     source_height, source_pitch, timestamp)
        : CopyYuy2ToSurface(&actual, output, link);
    asm volatile("" : : "r"(actual.planes[0]), "r"(actual.planes[1]) : "memory");
    return ok;
  }

  void Verify(uint64_t timestamp) {
    Require(actual.storage == expected.storage,
            "conversion bytes, rounded chroma and pitch guards must match");
    Require(actual.ready && actual.frame_timestamp == timestamp,
            "actual production converter must publish the supplied timestamp");
  }
};

struct YuvSample {
  uint8_t y, u, v;
};

std::array<uint8_t, 4> ReferenceLimited601(YuvSample sample) {
  // Independent normalized BT.601 equations, without the production integer
  // coefficients. Preserve signed luma until clipping the final RGB channels.
  const double kr = 0.299, kb = 0.114;
  const double luma = (static_cast<double>(sample.y) - 16) / 219;
  const double cb = (static_cast<double>(sample.u) - 128) / 224;
  const double cr = (static_cast<double>(sample.v) - 128) / 224;
  const double red = luma + 2 * (1 - kr) * cr;
  const double blue = luma + 2 * (1 - kb) * cb;
  const double green = (luma - kr * red - kb * blue) / (1 - kr - kb);
  const auto level = [](double value) {
    return static_cast<uint8_t>(std::clamp<long>(std::lround(value * 255), 0, 255));
  };
  return {level(blue), level(green), level(red), 255};
}

YuvSample PackedRgbSample(unsigned row, unsigned column) {
  static constexpr uint8_t chroma[][2] = {
      {128, 128}, {128, 160}, {160, 128}, {80, 192}, {90, 160},
      {180, 210}, {16, 128}, {240, 16}, {16, 240}};
  const unsigned index = row * 32 + column;
  // Swap column bit 0 with spatial row-parity bit 5. Each chroma block still
  // contains every luma byte once, including colored footroom in both fields.
  const unsigned luma = (index & 0xdeU) | ((index & 1U) << 5) |
                        ((index & 0x20U) >> 5);
  return {static_cast<uint8_t>(luma), chroma[index / 256][0],
          chroma[index / 256][1]};
}

enum class PackedRgbLayout { kProgressive, kLink, kSeparateFields, kInterleavedFields };

void PackedRgbCopy(uint32_t fourcc, PackedRgbLayout layout) {
  constexpr unsigned width = 32, height = 72;
  constexpr size_t source_prefix = 7, source_suffix = 19;
  const bool interlaced = layout == PackedRgbLayout::kSeparateFields ||
                          layout == PackedRgbLayout::kInterleavedFields;
  const size_t row_pitch = layout == PackedRgbLayout::kLink ? 720 * 2 :
                           interlaced ? width * 2 + 11 : width * 2;
  std::vector<uint8_t> input(source_prefix + row_pitch * height + source_suffix, 0x5b);
  for (unsigned row = 0; row < height; ++row) {
    const unsigned stored_row = layout == PackedRgbLayout::kSeparateFields
        ? (row % 2) * (height / 2) + row / 2 : row;
    uint8_t *line = input.data() + source_prefix + stored_row * row_pitch;
    for (unsigned column = 0; column < width; column += 2) {
      const YuvSample first = PackedRgbSample(row, column);
      const YuvSample second = PackedRgbSample(row, column + 1);
      line[column * 2] = first.y;
      line[column * 2 + 1] = first.u;
      line[column * 2 + 2] = second.y;
      line[column * 2 + 3] = first.v;
    }
  }
  const auto original_input = input;
  Surface actual;
  actual.width = width + 4;
  actual.height = height + 4;
  actual.fourcc = fourcc;
  actual.rt_format = VA_RT_FORMAT_RGB32;
  actual.pitch[0] = actual.width * 4 + 13;
  constexpr size_t output_prefix = 31, output_suffix = 29;
  actual.storage.assign(output_prefix + actual.pitch[0] * actual.height + output_suffix, 0xa5);
  actual.planes[0] = actual.storage.data() + output_prefix;
  constexpr uint64_t timestamp = 23456;
  const uint8_t *const source = input.data() + source_prefix;
  if (interlaced) {
    const bool interleaved = layout == PackedRgbLayout::kInterleavedFields;
    const uint8_t *fields[2] = {source, source + (interleaved ? row_pitch : row_pitch * height / 2)};
    Require(CopyH264InterlacedToSurface(&actual, fields, width, height,
                                       row_pitch * (interleaved ? 2 : 1), timestamp),
            "actual interlaced packed-RGB conversion succeeds");
  } else {
    BC_DTS_PROC_OUT output = {};
    output.Ybuff = const_cast<uint8_t *>(source);
    output.PicInfo.width = width;
    output.PicInfo.height = height;
    output.PicInfo.timeStamp = timestamp;
    Require(CopyYuy2ToSurface(&actual, output, layout == PackedRgbLayout::kLink),
            "actual progressive/Link packed-RGB conversion succeeds");
  }
  Require(actual.ready && actual.frame_timestamp == timestamp && input == original_input,
          "packed-RGB conversion publishes the exact timestamp and preserves its source");

  // Frozen pre-fix bytes for every Y=16..255 across all nine chroma pairs.
  // This exact-byte control is distinct from the independent color oracle.
  uint32_t nominal_checksum = 2166136261U;
  for (unsigned row = 0; row < height; ++row)
    for (unsigned column = 0; column < width; ++column)
      if (PackedRgbSample(row, column).y >= 16)
        for (unsigned channel = 0; channel < 4; ++channel)
          nominal_checksum = (nominal_checksum ^ actual.planes[0][row * actual.pitch[0] + column * 4 + channel]) * 16777619U;
  Require(nominal_checksum == 0xd2e5b6a6U,
          "Y>=16 retains the exact historical packed-RGB bytes");

  unsigned colored_footroom[2] = {};
  for (unsigned row = 0; row < actual.height; ++row) {
    for (unsigned column = 0; column < actual.width; ++column) {
      const uint8_t *pixel = actual.planes[0] + row * actual.pitch[0] + column * 4;
      if (row >= height || column >= width) {
        Require(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255,
                "larger RGB surfaces retain exact opaque black padding");
        continue;
      }
      const YuvSample sample = PackedRgbSample(row, column);
      if (sample.y < 16 && (sample.u != 128 || sample.v != 128))
        ++colored_footroom[row % 2];
      const auto expected = ReferenceLimited601(sample);
      for (unsigned channel = 0; channel < 3; ++channel) {
        if (std::abs(static_cast<int>(pixel[channel]) - expected[channel]) > 1)
          fprintf(stderr, "packed RGB mismatch format=%08x layout=%u row=%u column=%u YUV=%u,%u,%u channel=%u actual=%u expected=%u\n",
                  fourcc, static_cast<unsigned>(layout), row, column,
                  sample.y, sample.u, sample.v, channel, pixel[channel], expected[channel]);
        Require(std::abs(static_cast<int>(pixel[channel]) - expected[channel]) <= 1,
                "actual packed-RGB pixels agree with independent signed-luma BT.601");
      }
      Require(pixel[3] == 255, "packed-RGB conversion keeps opaque alpha");
    }
    for (unsigned byte = actual.width * 4; byte < actual.pitch[0]; ++byte)
      Require(actual.planes[0][row * actual.pitch[0] + byte] == 0xa5,
              "RGB conversion preserves row-pitch guards");
  }
  for (size_t byte = 0; byte < output_prefix; ++byte)
    Require(actual.storage[byte] == 0xa5, "RGB conversion preserves prefix guards");
  for (size_t byte = actual.storage.size() - output_suffix; byte < actual.storage.size(); ++byte)
    Require(actual.storage[byte] == 0xa5, "RGB conversion preserves suffix guards");
  Require(colored_footroom[0] != 0 && colored_footroom[1] != 0,
          "colored below-black samples exercise both spatial field parities");
}

void PackedRgbCorrectness() {
  struct HandSample { YuvSample yuv; std::array<uint8_t, 4> bgra; };
  const HandSample samples[] = {
      {{0, 128, 160}, {0, 0, 32, 255}}, {{8, 128, 160}, {0, 0, 42, 255}},
      {{15, 128, 160}, {0, 0, 50, 255}}, {{16, 128, 160}, {0, 0, 51, 255}},
      {{0, 160, 128}, {46, 0, 0, 255}}, {{16, 128, 128}, {0, 0, 0, 255}},
      {{235, 128, 128}, {255, 255, 255, 255}}};
  for (const auto &sample : samples)
    Require(ReferenceLimited601(sample.yuv) == sample.bgra,
            "independent normalized equations reproduce known color samples");
  for (uint32_t fourcc : {VA_FOURCC_BGRA, VA_FOURCC_ARGB})
    for (auto layout : {PackedRgbLayout::kProgressive, PackedRgbLayout::kLink,
                       PackedRgbLayout::kSeparateFields, PackedRgbLayout::kInterleavedFields})
      PackedRgbCopy(fourcc, layout);
}

void Correctness() {
  for (unsigned int width : {1U, 2U, 3U, 7U, 15U, 16U, 17U, 31U, 32U, 33U,
                             63U, 64U, 65U}) {
    for (unsigned int height : {1U, 2U, 3U, 4U, 17U}) {
      for (bool link : {false, true}) {
        for (unsigned int offset : {0U, 1U, 3U}) {
          for (unsigned int variant = 0; variant < 3; ++variant) {
            Fixture f(width, height,
                      variant == 1 ? width + 3 : variant == 2 ? std::max(1U, width - 1) : width,
                      variant == 1 ? height + 3 : variant == 2 ? std::max(1U, height - 1) : height,
                      false, link, offset, true);
            const auto source = f.input;
            Require(f.Convert(12345), "progressive conversion succeeds");
            f.Verify(12345);
            Require(f.input == source, "progressive input remains immutable");
          }
        }
      }
    }
  }
  for (unsigned int width : {2U, 6U, 14U, 16U, 18U, 30U, 32U, 34U, 64U, 66U}) {
    for (unsigned int height : {4U, 8U, 20U, 36U}) {
      for (bool interleaved : {false, true}) {
        for (unsigned int offset : {0U, 1U, 3U}) {
          for (unsigned int variant = 0; variant < 3; ++variant) {
            Fixture f(width, height,
                      variant == 1 ? width + 3 : variant == 2 ? std::max(2U, width - 2) : width,
                      variant == 1 ? height + 3 : variant == 2 ? height - 1 : height,
                      true, false, offset, true, interleaved);
            const auto source = f.input;
            Require(f.Convert(67890), "field-local conversion succeeds");
            f.Verify(67890);
            Require(f.input == source, "interlaced input remains immutable");
          }
        }
      }
    }
  }
  // A cropped final luma row can still be needed by field-local UV averaging.
  // In particular, the lower field's luma must survive a two-row crop even
  // though that field has no UV row in the destination's single chroma row.
  for (unsigned int height : {2U, 3U}) {
    for (bool interleaved : {false, true}) {
      Fixture f(18, 4, 18, height, true, false, 1, true, interleaved);
      const auto source = f.input;
      Require(f.Convert(67890), "short cropped interlaced conversion succeeds");
      f.Verify(67890);
      Require(f.input == source, "cropped interlaced source remains immutable");
    }
  }
  // Explicit half-way values distinguish rounded average from truncation;
  // alternating fields ensure chroma cannot be averaged across time.
  Fixture f(2, 4, 2, 4, true, false, 0, false);
  const uint8_t values[] = {0, 1, 254, 255};
  for (unsigned int row = 0; row < 4; ++row) {
    uint8_t *line = f.input.data() + row * 4;
    line[1] = values[row];
    line[3] = values[row];
  }
  f.Reference();
  Require(f.Convert(7), "rounding fixture converts");
  f.Verify(7);
  Require(f.actual.planes[1][0] == 1 &&
              f.actual.planes[1][f.actual.pitch[1]] == 255,
          "chroma is rounded within each field, never opposite fields");
}

unsigned int Number(const char *text, unsigned int maximum) {
  char *end = nullptr;
  errno = 0;
  const unsigned long value = strtoul(text, &end, 10);
  Require(errno == 0 && end != text && *end == '\0' && value > 0 && value <= maximum,
          "benchmark arguments must be positive bounded integers");
  return static_cast<unsigned int>(value);
}

double Seconds(const timeval &value) {
  return value.tv_sec + static_cast<double>(value.tv_usec) / 1000000.0;
}

void Benchmark(char **argv) {
  const bool interlaced = strcmp(argv[2], "interlaced") == 0;
  Require(interlaced || strcmp(argv[2], "progressive") == 0,
          "benchmark mode is progressive or interlaced");
  const unsigned int width = Number(argv[3], 4096);
  const unsigned int height = Number(argv[4], 2160);
  const unsigned int runs = Number(argv[5], 1000000);
  const unsigned int warmup = Number(argv[6], 100000);
  Require(!interlaced || ((width & 1U) == 0 && height % 4U == 0),
          "interlaced benchmark geometry must satisfy production field contract");
  Fixture f(width, height, width, height, interlaced, false, 0, false);
  Require(f.Convert(1), "benchmark initial conversion succeeds");
  f.Verify(1);
  for (unsigned int i = 0; i < warmup; ++i)
    Require(f.Convert(i + 2), "benchmark warmup succeeds");
  rusage before = {}, after = {};
  timespec start = {}, end = {};
  Require(getrusage(RUSAGE_SELF, &before) == 0 &&
              clock_gettime(CLOCK_MONOTONIC, &start) == 0,
          "benchmark initial clocks succeed");
  for (unsigned int i = 0; i < runs; ++i) {
    if (!f.Convert(i + warmup + 2)) throw std::runtime_error("timed conversion failed");
    consumed = consumed ^ f.actual.planes[0][(i * 97U) % width];
  }
  Require(clock_gettime(CLOCK_MONOTONIC, &end) == 0 &&
              getrusage(RUSAGE_SELF, &after) == 0,
          "benchmark final clocks succeed");
  f.Verify(runs + warmup + 1);
  uint32_t checksum = 2166136261U;
  for (uint8_t byte : f.actual.storage) checksum = (checksum ^ byte) * 16777619U;
  const double wall = end.tv_sec - start.tv_sec +
      static_cast<double>(end.tv_nsec - start.tv_nsec) / 1000000000.0;
  printf("{\"mode\":\"%s\",\"bits\":%zu,\"sse2\":%s,\"width\":%u,\"height\":%u,"
         "\"runs\":%u,\"warmup\":%u,\"wall_s\":%.9f,\"cpu_s\":%.9f,"
         "\"ms_per_frame\":%.6f,\"minor_faults\":%ld,"
         "\"checksum\":%u,\"consumed\":%u}\n",
         interlaced ? "interlaced" : "progressive", sizeof(void *) * 8,
#if defined(__SSE2__)
         "true",
#else
         "false",
#endif
         width, height, runs, warmup, wall,
         Seconds(after.ru_utime) + Seconds(after.ru_stime) -
             Seconds(before.ru_utime) - Seconds(before.ru_stime),
         wall * 1000.0 / runs, after.ru_minflt - before.ru_minflt,
         checksum, consumed);
}
}  // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 7 && strcmp(argv[1], "--benchmark") == 0) {
      Benchmark(argv);
    } else {
      Require(argc == 1, "usage: --benchmark progressive|interlaced WIDTH HEIGHT RUNS WARMUP");
      Correctness();
      PackedRgbCorrectness();
      printf("VA-API actual conversion: %u checks PASS\n", checks);
    }
    return 0;
  } catch (const std::exception &error) {
    fprintf(stderr, "VA-API actual conversion: FAIL: %s\n", error.what());
    return 1;
  }
}
