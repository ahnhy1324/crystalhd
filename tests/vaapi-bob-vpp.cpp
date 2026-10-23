// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual ProcessVpp on guarded private CPU planes; never opens a device.
// CRYSTALHD_BOB_NV12_ONLY also links on i686 without 32-bit graphics libraries.
#include "../filters/vaapi/crystalhd_drv_video.cpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace allocation_fault {
bool fail_next = false;
void *Allocate(size_t bytes) {
  if (fail_next) {
    fail_next = false;
    throw std::bad_alloc();
  }
  if (void *memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
  throw std::bad_alloc();
}
}
void *operator new(size_t bytes) { return allocation_fault::Allocate(bytes); }
void *operator new[](size_t bytes) { return allocation_fault::Allocate(bytes); }
__attribute__((noinline)) void operator delete(void *p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void *p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void *p, size_t) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void *p, size_t) noexcept { std::free(p); }

extern "C" void gbm_bo_unmap(gbm_bo *, void *) { abort(); }
extern "C" void gbm_bo_destroy(gbm_bo *) { abort(); }
extern "C" drmVersionPtr drmGetVersion(int) { abort(); }
extern "C" void drmFreeVersion(drmVersionPtr) { abort(); }
extern "C" BC_STATUS DtsDeviceClose(HANDLE) { abort(); }
extern "C" BC_STATUS DtsStopDecoder(HANDLE) { abort(); }
extern "C" BC_STATUS DtsCloseDecoder(HANDLE) { abort(); }

#ifndef CRYSTALHD_BOB_NV12_ONLY
namespace scale_fault {
bool guard_invalid_stride = false;
unsigned int rejected_calls = 0;
}
extern "C" int __real_sws_scale(SwsContext *, const uint8_t *const [], const int [],
                                int, int, uint8_t *const [], const int []);
extern "C" int __wrap_sws_scale(SwsContext *context, const uint8_t *const source[],
    const int source_pitch[], int start, int height, uint8_t *const target[],
    const int target_pitch[]) {
  if (scale_fault::guard_invalid_stride &&
      (source_pitch[0] < 0 || source_pitch[1] < 0)) {
    ++scale_fault::rejected_calls;
    return 0;  // Never let the pre-fix path dereference an invalid stride.
  }
  return __real_sws_scale(context, source, source_pitch, start, height, target,
                           target_pitch);
}
#endif

#ifdef CRYSTALHD_BOB_NV12_ONLY
extern "C" gbm_bo *gbm_bo_import(gbm_device *, uint32_t, void *, uint32_t) { abort(); }
extern "C" void *gbm_bo_map(gbm_bo *, uint32_t, uint32_t, uint32_t, uint32_t,
                            uint32_t, uint32_t *, void **) { abort(); }
extern "C" uint32_t gbm_bo_get_stride_for_plane(gbm_bo *, int) { abort(); }
extern "C" uint32_t gbm_bo_get_offset(gbm_bo *, int) { abort(); }
extern "C" SwsContext *sws_getCachedContext(SwsContext *, int, int, AVPixelFormat,
    int, int, AVPixelFormat, int, SwsFilter *, SwsFilter *, const double *) {
  abort();
}
extern "C" int sws_scale(SwsContext *, const uint8_t *const [], const int [],
    int, int, uint8_t *const [], const int []) { abort(); }
extern "C" const int *sws_getCoefficients(int) { abort(); }
extern "C" int sws_setColorspaceDetails(SwsContext *, const int *, int,
    const int *, int, int, int, int) { abort(); }
extern "C" void sws_freeContext(SwsContext *context) { if (context) abort(); }
#endif

namespace {
unsigned int checks = 0, io_calls = 0, read_starts = 0, read_ends = 0;
void Require(bool condition, const char *message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
struct Scaler {
  SwsContext *value = nullptr;
  ~Scaler() { sws_freeContext(value); }
};

void Initialize(Surface *surface, unsigned int width, unsigned int height,
                bool argb = false) {
  surface->width = width;
  surface->height = height;
  surface->fourcc = argb ? VA_FOURCC_ARGB : VA_FOURCC_NV12;
  surface->pitch[0] = width * (argb ? 4 : 1) + 5;
  surface->pitch[1] = argb ? 0 : width + 7;
  const size_t y_bytes = static_cast<size_t>(surface->pitch[0]) * height;
  const size_t uv_bytes = argb ? 0
      : static_cast<size_t>(surface->pitch[1]) * ((height + 1U) / 2);
  surface->storage.assign(32 + y_bytes + 31 + uv_bytes + 64, 0xa5);
  surface->planes[0] = surface->storage.data() + 32;
  surface->planes[1] = argb ? nullptr : surface->planes[0] + y_bytes + 31;
  surface->ready = false;
  surface->failed = true;
  surface->expected_timestamp = surface->frame_timestamp = 777;
}

VARectangle Full(const Surface &surface) {
  return {0, 0, static_cast<uint16_t>(surface.width),
          static_cast<uint16_t>(surface.height)};
}

void Fill(Surface *source) {
  for (unsigned int plane = 0; plane != 2; ++plane) {
    const unsigned int rows = plane == 0 ? source->height : source->height / 2;
    for (unsigned int row = 0; row < rows; ++row) {
      for (unsigned int x = 0; x < source->width; ++x) {
        source->planes[plane][static_cast<size_t>(row) * source->pitch[plane] + x] =
            static_cast<uint8_t>(plane == 0
                ? ((row & 1U) ? 176 : 16) + (row / 2 + x) % 64
                : ((row & 1U) ? 160 : 32) + (x & 1U) * 32 + (row / 2 + x / 2) % 31);
      }
    }
  }
  source->ready = true;
  source->failed = false;
  source->expected_timestamp = source->frame_timestamp = 12345;
}

// Phase accumulator deliberately does not call the production row mapper or
// use its multiplication/division expression.
std::vector<unsigned int> Rows(unsigned int samples, unsigned int count) {
  std::vector<unsigned int> result;
  unsigned int source = 0, phase = 0;
  for (unsigned int target = 0; target < count; ++target) {
    result.push_back(source);
    phase += samples;
    while (phase >= count) { phase -= count; ++source; }
  }
  return result;
}

void ReferenceNv12(const Surface &source, Surface *target, unsigned int parity,
                    const VARectangle &src, const VARectangle &dst,
                    bool full_range = false) {
  if (dst.x != 0 || dst.y != 0 ||
      dst.width != target->width || dst.height != target->height) {
    for (unsigned int row = 0; row < target->height; ++row)
      std::fill_n(target->planes[0] + static_cast<size_t>(row) * target->pitch[0],
                  target->width, full_range ? 0 : 16);
    for (unsigned int row = 0; row < (target->height + 1) / 2; ++row)
      std::fill_n(target->planes[1] + static_cast<size_t>(row) * target->pitch[1],
                  (target->width + 1) & ~1U, 128);
  }
  for (unsigned int plane = 0; plane != 2; ++plane) {
    const unsigned int source_rows = plane == 0 ? src.height : src.height / 2;
    const unsigned int target_rows = plane == 0 ? dst.height : dst.height / 2;
    const unsigned int bytes = plane == 0 ? 1 : 2;
    const auto rows = Rows(source_rows / 2, target_rows);
    const auto columns = Rows(src.width / bytes, dst.width / bytes);
    for (unsigned int y = 0; y < target_rows; ++y)
      for (unsigned int x = 0; x < dst.width / bytes; ++x)
        for (unsigned int byte = 0; byte < bytes; ++byte)
          target->planes[plane][static_cast<size_t>(y + (plane == 0 ? dst.y : dst.y / 2)) *
                                   target->pitch[plane] + dst.x + x * bytes + byte] =
              source.planes[plane][static_cast<size_t>(rows[y] * 2 + parity) * source.pitch[plane] +
                                   columns[x] * bytes + byte];
  }
}

void AlignedOutputRegions(unsigned parity, bool compact, bool full_range) {
  Surface source, field, target, expected;
  Initialize(&source, 32, 1088);
  Initialize(&target, 35, 743);
  Initialize(&expected, 35, 743);
  Fill(&source);
  if (compact) {
    Initialize(&field, source.width, source.height / 2);
    for (unsigned plane = 0; plane != 2; ++plane)
      for (unsigned row = 0; row < field.height / (plane == 0 ? 1 : 2); ++row)
        memcpy(field.planes[plane] + size_t{row} * field.pitch[plane],
               source.planes[plane] + size_t{row * 2 + parity} * source.pitch[plane],
               source.width);
    field.ready = true;
    field.failed = false;
    field.expected_timestamp = field.frame_timestamp = source.frame_timestamp;
  }
  const auto source_bytes = source.storage, field_bytes = field.storage;
  // Retain visible1080 independently of coded1088; odd backing dimensions
  // exercise the last complete UV pair/row and independent plane pitch tails.
  const VARectangle visible = {0, 0, 32, 1080};
  const VARectangle regions[] = {{2, 4, 32, 736}, {6, 12, 16, 720}};
  Scaler scaler;
  std::vector<uint8_t> staging;
  VppDeinterlace request;
  request.bob = true;
  request.field = static_cast<crystalhd_vpp_bob::FieldParity>(parity);
  source.object_fds.push_back(777);
  field.object_fds.push_back(777);
  target.object_fds.push_back(888);
  for (const VARectangle &output : regions) {
    ReferenceNv12(source, &expected, parity, visible, output, full_range);
    const unsigned before_io = io_calls, starts = read_starts, ends = read_ends;
    Require(ProcessVpp(&scaler.value, &staging, compact ? &field : &source, &target,
                       visible, output, false, request, compact,
                       VppColorRange{full_range}) == VA_STATUS_SUCCESS,
            "aligned Bob destination accepts woven/compact and full/limited input");
    Require(target.storage == expected.storage,
            "aligned ROI pixels, four reused borders, odd backing and all guards match independent oracle");
    Require(io_calls == before_io + 4 && read_starts == starts + 1 &&
                read_ends == ends + 1 && target.ready && !target.failed &&
                target.expected_timestamp == 12345 && target.frame_timestamp == 12345 &&
                source.storage == source_bytes && field.storage == field_bytes &&
                scaler.value == nullptr && staging.empty(),
            "ROI placement preserves source/target CPU ownership, source bytes and PTS");
  }
  source.object_fds.clear();
  field.object_fds.clear();
  target.object_fds.clear();
}

#ifndef CRYSTALHD_BOB_NV12_ONLY
void ReferenceArgb(const Surface &source, Surface *target, unsigned int parity,
                   const VARectangle &src, const VARectangle &dst) {
  // Independently compact a physical field for a normal swscale reference.
  // SWS_POINT's resized sampling phase need not equal the NV12 row mapper.
  const size_t y_bytes = static_cast<size_t>(src.width) * (src.height / 2);
  std::vector<uint8_t> field(y_bytes + y_bytes / 2 + 64, 0);
  for (unsigned int plane = 0; plane != 2; ++plane) {
    const unsigned int rows = src.height / (plane == 0 ? 2 : 4);
    for (unsigned int row = 0; row < rows; ++row)
      memcpy(field.data() + (plane == 0 ? 0 : y_bytes) + static_cast<size_t>(row) * src.width,
             source.planes[plane] + static_cast<size_t>(row * 2 + parity) * source.pitch[plane],
             src.width);
  }
  Scaler scaler;
  scaler.value = sws_getContext(src.width, src.height / 2, AV_PIX_FMT_NV12,
      dst.width, dst.height, AV_PIX_FMT_BGRA, SWS_POINT, nullptr, nullptr, nullptr);
  Require(scaler.value != nullptr, "independent ARGB scaler created");
  const uint8_t *input[] = {field.data(), field.data() + y_bytes, nullptr, nullptr};
  const int input_pitch[] = {static_cast<int>(src.width), static_cast<int>(src.width), 0, 0};
  const size_t pitch = static_cast<size_t>(dst.width) * 4;
  std::vector<uint8_t> rgb(pitch * dst.height + 64, 0);
  uint8_t *output[] = {rgb.data(), nullptr, nullptr, nullptr};
  const int output_pitch[] = {static_cast<int>(pitch), 0, 0, 0};
  Require(sws_scale(scaler.value, input, input_pitch, 0, src.height / 2,
                    output, output_pitch) == static_cast<int>(dst.height),
          "independent ARGB reference has all rows");
  if (dst.width != target->width || dst.height != target->height)
    for (unsigned int row = 0; row < target->height; ++row)
      for (unsigned int column = 0; column < target->width; ++column) {
        uint8_t *pixel = target->planes[0] + static_cast<size_t>(row) * target->pitch[0] + column * 4;
        pixel[0] = pixel[1] = pixel[2] = 0;
        pixel[3] = 255;
      }
  for (unsigned int row = 0; row < dst.height; ++row)
    memcpy(target->planes[0] + static_cast<size_t>(row) * target->pitch[0],
           rgb.data() + static_cast<size_t>(row) * pitch, pitch);
}
#endif

struct Geometry { unsigned int width = 0, height = 0; };

void FullRangeBobPixels(unsigned int parity, bool compact, bool argb) {
  Surface source, field, target, expected;
  Initialize(&source, 32, 1088);
  Initialize(&target, 18, 736, argb);
  Initialize(&expected, 18, 736, argb);
  for (unsigned row = 0; row < source.height; ++row)
    memset(source.planes[0] + row * source.pitch[0], row % 2 ? 235 : 16, source.width);
  for (unsigned row = 0; row < source.height / 2; ++row)
    memset(source.planes[1] + row * source.pitch[1], 128, source.width);
  if (compact) {
    Initialize(&field, source.width, source.height / 2);
    for (unsigned plane = 0; plane < 2; ++plane)
      for (unsigned row = 0; row < field.height / (plane == 0 ? 1 : 2); ++row)
        memcpy(field.planes[plane] + row * field.pitch[plane],
               source.planes[plane] + (row * 2 + parity) * source.pitch[plane], source.width);
  }
  const auto original = source.storage;
  const auto original_field = field.storage;
  // Full-range neutral Y maps exactly to RGB=Y. Bob's selected field is
  // constant, so this independent oracle also covers resizing 1080→720.
  const uint8_t luma = parity == 0 ? 16 : 235;
  for (unsigned row = 0; row < expected.height; ++row)
    for (unsigned column = 0; column < expected.width; ++column) {
      const uint8_t value = row < 720 && column < 16 ? luma : 0;
      uint8_t *pixel = expected.planes[0] + row * expected.pitch[0] + column * (argb ? 4 : 1);
      pixel[0] = value;
      if (argb) {
        pixel[1] = pixel[2] = value;
        pixel[3] = 255;
      }
    }
  if (!argb)
    for (unsigned row = 0; row < expected.height / 2; ++row)
      memset(expected.planes[1] + row * expected.pitch[1], 128, expected.width);
  Scaler scaler;
  std::vector<uint8_t> staging;
  VppDeinterlace bob;
  bob.bob = true;
  bob.field = static_cast<crystalhd_vpp_bob::FieldParity>(parity);
  Require(ProcessVpp(&scaler.value, &staging, compact ? &field : &source, &target,
                     {0, 0, 32, 1080}, {0, 0, 16, 720}, false, bob, compact,
                     VppColorRange{true}) == VA_STATUS_SUCCESS,
          "full-range Bob converts either compact or woven source");
  Require(target.storage == expected.storage,
          "full-range coded1088/visible1080→720 pixels, borders and guards match independent oracle");
  Require(source.storage == original && field.storage == original_field,
          "full-range Bob preserves both source layouts");
}

void Pixels(unsigned int source_width, unsigned int source_height,
             unsigned int target_width, unsigned int target_height,
             bool argb, unsigned int parity, bool write_started = false,
             Geometry source_storage = {}, Geometry target_storage = {},
             bool compact = false) {
  Surface source, target, reference, field;
  Initialize(&source, source_storage.width ? source_storage.width : source_width,
              source_storage.height ? source_storage.height : source_height);
  Initialize(&target, target_storage.width ? target_storage.width : target_width,
              target_storage.height ? target_storage.height : target_height, argb);
  Initialize(&reference, target.width, target.height, argb);
  const VARectangle src = {0, 0, static_cast<uint16_t>(source_width),
                                 static_cast<uint16_t>(source_height)};
  const VARectangle dst = {0, 0, static_cast<uint16_t>(target_width),
                                 static_cast<uint16_t>(target_height)};
  Fill(&source);
  const auto original = source.storage;
  if (compact) {
    Initialize(&field, source.width, (source.height + 1U) / 2U);
    for (unsigned int plane = 0; plane != 2; ++plane) {
      const unsigned int rows = source.height / (plane == 0 ? 2 : 4);
      for (unsigned int row = 0; row != rows; ++row)
        memcpy(field.planes[plane] + static_cast<size_t>(row) * field.pitch[plane],
               source.planes[plane] +
                   static_cast<size_t>(row * 2U + parity) * source.pitch[plane],
               source.width);
    }
    field.ready = true;
    field.failed = false;
    field.expected_timestamp = field.frame_timestamp = source.frame_timestamp;
  }
  const auto original_field = field.storage;
  if (!argb) ReferenceNv12(source, &reference, parity, src, dst);
#ifndef CRYSTALHD_BOB_NV12_ONLY
  else ReferenceArgb(source, &reference, parity, src, dst);
#endif
  Scaler scaler;
  std::vector<uint8_t> staging;
  VppDeinterlace request;
  request.bob = true;
  request.field = static_cast<crystalhd_vpp_bob::FieldParity>(parity);
  Require(ProcessVpp(&scaler.value, &staging, compact ? &field : &source,
                     &target, src, dst, write_started, request, compact) == VA_STATUS_SUCCESS,
          "actual Bob VPP succeeds");
  if (target.storage != reference.storage) {
    const size_t offset = static_cast<size_t>(std::mismatch(target.storage.begin(),
        target.storage.end(), reference.storage.begin()).first - target.storage.begin());
    std::fprintf(stderr, "Bob mismatch: %ux%u in %ux%u -> %ux%u in %ux%u "
                  "argb=%u parity=%u compact=%u offset=%zu actual=%u expected=%u\n",
                  source_width, source_height, source.width, source.height,
                  target_width, target_height, target.width, target.height,
                  argb, parity, compact, offset, target.storage[offset], reference.storage[offset]);
  }
  Require(target.storage == reference.storage,
          "actual Bob pixels/padded rows/outer guards match independent field reference");
  Require(source.storage == original, "source field pixels remain immutable");
  Require(field.storage == original_field, "compact field pixels/guards remain immutable");
  Require(target.ready && !target.failed && target.expected_timestamp == 12345 &&
              target.frame_timestamp == 12345, "completed Bob picture identity published");
  Require(argb || staging.empty(), "NV12 Bob does not allocate full-frame staging");
}

void CompactConversion(unsigned int width, unsigned int height,
                       unsigned int parity, Geometry backing = {}) {
  Surface actual, reference;
  Initialize(&actual, backing.width ? backing.width : width,
              backing.height ? backing.height : height / 2U);
  Initialize(&reference, actual.width, actual.height);
  const size_t pitch = static_cast<size_t>(width) * 2U + 11U;
  std::vector<uint8_t> packed(31U + pitch * (height / 2U) + 64U, 0xa7);
  uint8_t *pixels = packed.data() + 31U;
  for (unsigned int row = 0; row != height / 2U; ++row)
    for (unsigned int column = 0; column != width; column += 2U) {
      uint8_t *pair = pixels + static_cast<size_t>(row) * pitch + column * 2U;
      pair[0] = static_cast<uint8_t>(20U + 70U * parity + (row + column) % 97U);
      pair[2] = static_cast<uint8_t>(21U + 70U * parity + (row + column) % 97U);
      pair[1] = static_cast<uint8_t>(40U + 60U * parity + (row + column) % 83U);
      pair[3] = static_cast<uint8_t>(60U + 50U * parity + (row + column) % 71U);
    }
  const auto original = packed;
  const unsigned int rows = std::min(height / 2U, actual.height);
  const unsigned int columns = std::min(width, actual.width);
  for (unsigned int plane = 0; plane != 2; ++plane) {
    const unsigned int output_rows = plane == 0 ? actual.height : (actual.height + 1U) / 2U;
    const unsigned int output_bytes = plane == 0 ? actual.width : (actual.width + 1U) & ~1U;
    for (unsigned int row = 0; row != output_rows; ++row)
      std::fill_n(reference.planes[plane] + static_cast<size_t>(row) * reference.pitch[plane],
                   output_bytes, plane == 0 ? 16 : 128);
  }
  for (unsigned int row = 0; row != rows; ++row)
    for (unsigned int column = 0; column != columns; ++column)
      reference.planes[0][static_cast<size_t>(row) * reference.pitch[0] + column] =
          pixels[static_cast<size_t>(row) * pitch + column * 2U];
  for (unsigned int row = 0; row < (rows + 1U) / 2U; ++row)
    for (unsigned int column = 0; column != columns; ++column) {
      const size_t top = static_cast<size_t>(row * 2U) * pitch + column * 2U + 1U;
      reference.planes[1][static_cast<size_t>(row) * reference.pitch[1] + column] =
          static_cast<uint8_t>((static_cast<unsigned int>(pixels[top]) +
                               pixels[top + pitch] + 1U) / 2U);
    }
  const uint8_t *fields[2] = {};
  fields[parity] = pixels;
  Require(CopyH264InterlacedToSurface(&actual, fields, width, height, pitch,
                                      12345, 1U << parity, true),
          "packed first field converts directly into compact NV12");
  Require(actual.storage == reference.storage,
          "compact conversion matches independent luma/same-field rounded chroma and padding");
  Require(packed == original && actual.ready && actual.frame_timestamp == 12345,
          "compact conversion preserves packed source/guards and exact timestamp");
}

void InvalidCompact() {
  for (unsigned int variant = 0; variant != 7; ++variant) {
    Surface source, target;
    Initialize(&source, 16, 4);
    Initialize(&target, 16, 8);
    Fill(&source);
    VARectangle src = {0, 0, 16, 8};
    const VARectangle dst = Full(target);
    VppDeinterlace request;
    request.bob = true;
    switch (variant) {
      case 0: request.bob = false; break;
      case 1: src.x = 2; src.width -= 2; break;
      case 2: src.y = 2; src.height -= 4; break;
      case 3: src.height = 6; break;
      case 4: src.height = 12; break;
      case 5: request.field = static_cast<crystalhd_vpp_bob::FieldParity>(2); break;
      case 6: source.frame_timestamp = 1; break;
    }
    source.object_fds.push_back(777);
    target.object_fds.push_back(888);
    const auto before_source = source.storage, before_target = target.storage;
    const unsigned int before_io = io_calls;
    Scaler scaler;
    std::vector<uint8_t> staging;
    const VAStatus status = ProcessVpp(&scaler.value, &staging, &source, &target,
                                        src, dst, false, request, true);
    source.object_fds.clear();
    target.object_fds.clear();
    Require(status == (variant == 6 ? VA_STATUS_ERROR_DECODING_ERROR
                                   : VA_STATUS_ERROR_INVALID_PARAMETER),
            "invalid compact-field contract or timestamp is rejected");
    Require(io_calls == before_io && source.storage == before_source &&
                target.storage == before_target && staging.empty(),
            "invalid compact field leaves bytes/guards/leases untouched");
  }
}

void InvalidCompactConversion() {
  for (bool argb : {false, true}) {
    Surface target;
    Initialize(&target, 16, 4, argb);
    std::vector<uint8_t> packed(16U * 4U * 2U, 0x72);
    const uint8_t *fields[2] = {packed.data(), packed.data()};
    const auto before = target.storage;
    const unsigned int before_io = io_calls;
    target.object_fds.push_back(888);
    const bool converted = CopyH264InterlacedToSurface(&target, fields, 16, 8, 32,
                                                       12345, argb ? 1U : 3U, true);
    target.object_fds.clear();
    Require(!converted && target.storage == before && io_calls == before_io,
            "compact conversion rejects both-field and ARGB targets before access");
  }
}

void ZeroFilterOddWidthBackground() {
  Surface source, target, reference;
  Initialize(&source, 16, 16);
  Initialize(&target, 17, 16);
  Initialize(&reference, 17, 16);
  Fill(&source);
  const auto original = source.storage;
  for (unsigned int plane = 0; plane != 2; ++plane) {
    const unsigned int rows = plane == 0 ? 16 : 8;
    const unsigned int row_bytes = plane == 0 ? 17 : 18;
    for (unsigned int row = 0; row < rows; ++row) {
      uint8_t *output = reference.planes[plane] +
          static_cast<size_t>(row) * reference.pitch[plane];
      std::fill_n(output, row_bytes, plane == 0 ? 16 : 128);
      memcpy(output, source.planes[plane] +
             static_cast<size_t>(row) * source.pitch[plane], 16);
    }
  }
  Scaler scaler;
  std::vector<uint8_t> staging;
  const VARectangle visible = {0, 0, 16, 16};
  Require(ProcessVpp(&scaler.value, &staging, &source, &target, Full(source),
                     visible) == VA_STATUS_SUCCESS,
          "zero-filter VPP accepts an even visible extent in an odd-width backing");
  for (unsigned int row = 0; row < 8; ++row)
    Require(target.planes[1][static_cast<size_t>(row) * target.pitch[1] + 16] == 128 &&
                target.planes[1][static_cast<size_t>(row) * target.pitch[1] + 17] == 128,
            "zero-filter background initializes both U and V of the final odd-width pair");
  Require(target.storage == reference.storage,
          "zero-filter visible pixels, neutral border and all pitch/outer guards match");
  Require(source.storage == original && staging.empty(),
          "zero-filter odd-width output preserves source and avoids ARGB staging");
}

void ZeroFilterNv12ExactResize() {
  struct Case {
    unsigned width, height, target_width, target_height;
    VARectangle source, target;
    bool interleaved = false, full_range = false;
  };
  const Case cases[] = {
      {1280, 720, 1920, 1080, {0, 0, 1280, 720}, {0, 0, 1920, 1080}},
      {1920, 1088, 1280, 720, {0, 0, 1920, 1080}, {0, 0, 1280, 720}},
      {1280, 720, 1920, 1080, {0, 0, 1280, 720}, {0, 0, 1920, 1080}, true},
      {1920, 1088, 1280, 720, {0, 0, 1920, 1080}, {0, 0, 1280, 720}, true},
      {1284, 728, 1925, 1091, {2, 3, 1280, 721}, {2, 4, 1920, 1080}},
      {1284, 728, 1925, 1091, {2, 4, 1280, 720}, {2, 4, 1920, 1080}, true},
      {1920, 1080, 1920, 1080, {0, 0, 1920, 1080}, {0, 0, 1920, 1080}},
      {1920, 1080, 1920, 1080, {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, true},
      {36, 28, 31, 25, {3, 3, 29, 23}, {2, 4, 26, 20}},
      {36, 28, 31, 25, {3, 3, 29, 23}, {2, 4, 26, 20}, false, true},
      {36, 28, 31, 25, {3, 3, 26, 21}, {2, 4, 26, 20}},
      {1280, 720, 4, 4, {1, 1, 1279, 719}, {0, 0, 4, 4}},
      {4, 4, 1280, 720, {1, 1, 3, 3}, {0, 0, 1280, 720}},
      {32, 32, 25, 25, {2, 4, 24, 24}, {2, 4, 16, 16}, true, true},
      {65535, 4, 65534, 4, {0, 0, 65535, 4}, {0, 0, 65534, 4}},
      {65535, 4, 2, 4, {0, 0, 65535, 4}, {0, 0, 2, 4}},
      {2, 4, 65534, 4, {0, 0, 2, 4}, {0, 0, 65534, 4}},
      {65534, 4, 65530, 4, {0, 0, 65534, 4}, {0, 0, 65530, 4}, true}};
  const unsigned before_io = io_calls;
  for (const auto &test : cases) {
    Surface source, target, expected;
    Initialize(&source, test.width, test.height);
    Initialize(&target, test.target_width, test.target_height);
    Initialize(&expected, test.target_width, test.target_height);
    Fill(&source);
    const auto original = source.storage;
    const auto &src = test.source;
    const auto &dst = test.target;
    // Independent multiplication/division oracle, including the old generic
    // odd-origin UV phase. Never use the production horizontal accumulator.
    for (unsigned plane = 0; plane < 2; ++plane) {
      const unsigned unit = plane ? 2 : 1;
      for (unsigned row = 0; row < (expected.height + unit - 1) / unit; ++row)
        std::fill_n(expected.planes[plane] + size_t(row) * expected.pitch[plane],
                    plane ? Align(expected.width, 2) : expected.width,
                    plane ? 128 : test.full_range ? 0 : 16);
      for (unsigned row = 0; row < dst.height / unit; ++row) {
        const unsigned source_row = test.interleaved
            ? src.y / unit + uint64_t(row / 2) * (src.height / unit / 2) /
                  (dst.height / unit / 2) * 2 + row % 2
            : (src.y + uint64_t(row * unit) * src.height / dst.height) / unit;
        for (unsigned column = 0; column < dst.width; column += unit) {
          unsigned source_column = src.x + uint64_t(column) * src.width / dst.width;
          if (plane) source_column &= ~1U;
          for (unsigned byte = 0; byte < unit; ++byte)
            expected.planes[plane][size_t(dst.y / unit + row) * expected.pitch[plane] +
                                   dst.x + column + byte] =
                source.planes[plane][size_t(source_row) * source.pitch[plane] +
                                     source_column + byte];
        }
      }
    }
    Scaler scaler;
    std::vector<uint8_t> staging;
    const VppColorRange range{test.full_range};
    Require(ProcessVpp(&scaler.value, &staging, &source, &target, src, dst,
                        false, {}, false, range, test.interleaved) == VA_STATUS_SUCCESS,
            "zero-filter progressive/Weave resize completes");
    Require(target.storage == expected.storage && source.storage == original,
            "resize keeps exact Y/UV samples, all borders, pitch/outer guards and source bytes");
    Require(target.ready && !target.failed && target.expected_timestamp == 12345 &&
                target.frame_timestamp == 12345 && staging.empty() && scaler.value == nullptr,
            "NV12 resize keeps identity and requires no ARGB staging or software scaler");
  }
  Require(io_calls == before_io, "private resize regression performs no device access");
}

#ifndef CRYSTALHD_BOB_NV12_ONLY
void ZeroFilterArgbBorders() {
  for (bool full : {false, true}) {
    Surface source, target, reference;
    Initialize(&source, 16, 16);
    Initialize(&target, full ? 23U : 25U, full ? 9U : 13U, true);
    Initialize(&reference, target.width, target.height, true);
    Fill(&source);
    const auto original = source.storage;
    const VARectangle src = {2, 2, 14, 12};
    const VARectangle dst = full ? Full(target) : VARectangle{1, 2, 22, 8};
    // Render only the visible rectangle into a tight independent reference,
    // then place its logical bytes in an opaque-black guarded destination.
    Scaler oracle;
    oracle.value = sws_getContext(src.width, src.height, AV_PIX_FMT_NV12,
        dst.width, dst.height, AV_PIX_FMT_BGRA, SWS_POINT, nullptr, nullptr, nullptr);
    Require(oracle.value != nullptr, "zero-filter ARGB reference scaler");
    const uint8_t *input[] = {
        source.planes[0] + static_cast<size_t>(src.y) * source.pitch[0] + src.x,
        source.planes[1] + static_cast<size_t>(src.y / 2) * source.pitch[1] + src.x,
        nullptr, nullptr};
    const int input_pitch[] = {static_cast<int>(source.pitch[0]),
                                static_cast<int>(source.pitch[1]), 0, 0};
    const size_t row_bytes = static_cast<size_t>(dst.width) * 4U;
    std::vector<uint8_t> rgb(row_bytes * dst.height + 64U, 0);
    uint8_t *output[] = {rgb.data(), nullptr, nullptr, nullptr};
    const int output_pitch[] = {static_cast<int>(row_bytes), 0, 0, 0};
    Require(sws_scale(oracle.value, input, input_pitch, 0, src.height,
                        output, output_pitch) == dst.height,
            "zero-filter independent reference covers every visible row");
    for (unsigned int row = 0; row != target.height; ++row)
      for (unsigned int column = 0; column != target.width; ++column) {
        uint8_t *pixel = reference.planes[0] +
            static_cast<size_t>(row) * reference.pitch[0] + column * 4U;
        pixel[0] = pixel[1] = pixel[2] = 0;
        pixel[3] = 255;
      }
    for (unsigned int row = 0; row != dst.height; ++row)
      memcpy(reference.planes[0] +
                 static_cast<size_t>(dst.y + row) * reference.pitch[0] + dst.x * 4U,
             rgb.data() + static_cast<size_t>(row) * row_bytes, row_bytes);
    Scaler scaler;
    std::vector<uint8_t> staging;
    Require(ProcessVpp(&scaler.value, &staging, &source, &target, src, dst) ==
                VA_STATUS_SUCCESS,
            "ordinary ARGB conversion accepts offset rectangles and odd full output");
    Require(target.storage == reference.storage && source.storage == original,
            "ordinary ARGB visible bytes, four black borders and every guard are exact");
    Require(target.ready && !target.failed && target.frame_timestamp == 12345 &&
                target.expected_timestamp == 12345,
            "ordinary ARGB rectangle publishes only complete exact-identity output");
  }
}

void ZeroFilterArgbInvalidStrides() {
  for (unsigned int plane = 0; plane != 2; ++plane) {
    Surface source, target;
    Initialize(&source, 16, 16);
    Initialize(&target, 16, 16, true);
    Fill(&source);
    source.pitch[plane] = static_cast<unsigned int>(INT_MAX) + 1U;
    source.object_fds.push_back(777);
    target.object_fds.push_back(888);
    const auto source_bytes = source.storage, target_bytes = target.storage;
    const unsigned int before_io = io_calls, before_scale = scale_fault::rejected_calls;
    Scaler scaler;
    std::vector<uint8_t> staging;
    scale_fault::guard_invalid_stride = true;
    const VAStatus status = ProcessVpp(&scaler.value, &staging, &source, &target,
                                       Full(source), Full(target));
    scale_fault::guard_invalid_stride = false;
    source.object_fds.clear();
    target.object_fds.clear();
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER)
      std::fprintf(stderr, "zero-filter ARGB invalid plane%u stride: status=%d io=%u safe_scale_rejections=%u staging=%zu\n",
                   plane, status, io_calls - before_io,
                   scale_fault::rejected_calls - before_scale, staging.size());
    Require(status == VA_STATUS_ERROR_INVALID_PARAMETER,
            "zero-filter ARGB rejects unsigned strides outside swscale's signed range");
    Require(io_calls == before_io && scale_fault::rejected_calls == before_scale &&
                staging.empty() && source.storage == source_bytes && target.storage == target_bytes,
            "invalid zero-filter ARGB strides reject before CPU access, scaling or pixel writes");
  }
}
#endif

bool HasImportMapping() {
  std::ifstream maps("/proc/self/maps");
  Require(maps.good(), "inspect private imported-mapping lifetime");
  std::string line;
  while (std::getline(maps, line))
    if (line.find("memfd:crystalhd-import-layout-boundary") != std::string::npos)
      return true;
  return false;
}

void ImportLayoutBounds() {
  const unsigned int before_io = io_calls;
  for (bool prime : {false, true}) {
    for (unsigned int variant = 0; variant != 8; ++variant) {
      Require(!HasImportMapping(), "no prior imported mapping remains live");
      Surface client;
      const int fd = memfd_create("crystalhd-import-layout-boundary", MFD_CLOEXEC);
      Require(fd >= 0, "allocate a small private descriptor, never a device");
      client.object_fds.push_back(fd);
      Require(ftruncate(fd, 1024) == 0, "bound the private backing to one KiB");
      unsigned int width = 16, height = 16, size = 384;
      uint32_t pitches[2] = {16, 16}, offsets[2] = {0, 256};
      switch (variant) {
        case 1:
          width = height = 17; size = 534;
          pitches[0] = 20; pitches[1] = 22; offsets[1] = 340;
          break;
        case 2: offsets[0] = 128; offsets[1] = 0; size = 383; break;
        case 3: size = 383; break;
        case 4: pitches[0] = UINT32_MAX / 15U + 1U; break;
        case 5: pitches[1] = UINT32_MAX / 7U + 1U; break;
        case 6: pitches[0] = 15; break;
        case 7: offsets[1] = size + 1; break;
        default: break;
      }
      bool accepted = false;
      std::vector<int> duplicates;
      {
        Surface imported;
        imported.width = width;
        imported.height = height;
        if (prime) {
          VADRMPRIMESurfaceDescriptor descriptor = {};
          descriptor.fourcc = VA_FOURCC_NV12;
          descriptor.width = width; descriptor.height = height;
          descriptor.num_objects = descriptor.num_layers = 1;
          descriptor.objects[0].fd = fd;
          descriptor.objects[0].size = size;
          descriptor.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
          descriptor.layers[0].drm_format = DRM_FORMAT_NV12;
          descriptor.layers[0].num_planes = 2;
          for (unsigned int plane = 0; plane != 2; ++plane) {
            descriptor.layers[0].pitch[plane] = pitches[plane];
            descriptor.layers[0].offset[plane] = offsets[plane];
          }
          accepted = imported.ImportPrime(nullptr, descriptor);
        } else {
          uintptr_t handle = static_cast<uintptr_t>(fd);
          VASurfaceAttribExternalBuffers descriptor = {};
          descriptor.pixel_format = VA_FOURCC_NV12;
          descriptor.width = width; descriptor.height = height;
          descriptor.data_size = size; descriptor.num_planes = 2;
          descriptor.num_buffers = 1; descriptor.buffers = &handle;
          for (unsigned int plane = 0; plane != 2; ++plane) {
            descriptor.pitches[plane] = pitches[plane];
            descriptor.offsets[plane] = offsets[plane];
          }
          accepted = imported.ImportLegacy(descriptor);
        }
        duplicates = imported.object_fds;
        if (std::any_of(imported.object_maps.begin(), imported.object_maps.end(),
                        [](void *mapping) { return mapping != nullptr && mapping != MAP_FAILED; }))
          Require(HasImportMapping(), "the mapping observer sees each owned private file mapping");
        // Intentionally never access any plane, even if an invalid import
        // incorrectly succeeds. Destroy the imported resources before FAIL.
      }
      for (int duplicate : duplicates)
        Require(duplicate != fd && fcntl(duplicate, F_GETFD) == -1 && errno == EBADF,
                "successful or rejected import releases only its duplicated fd");
      Require(fcntl(fd, F_GETFD) >= 0 && !HasImportMapping(),
              "import teardown preserves the caller fd and removes every mapping");
      Require(accepted == (variant < 2),
              "actual import rejects short or overflowing plane spans on this ABI");
    }
  }
  Require(io_calls == before_io, "layout validation never acquires DMA CPU access");
}

void Invalid() {
  for (unsigned int variant = 0; variant != 21; ++variant) {
    Surface source, target;
    Initialize(&source, 16, variant == 4 ? 6 : 8);
    Initialize(&target, variant == 7 ? 15 : 16, variant == 5 ? 6 : 8,
                variant == 16 || variant == 17 || variant == 20);
    Fill(&source);
    auto src = Full(source), dst = Full(target);
    VppDeinterlace request;
    request.bob = true;
    switch (variant) {
      case 0: src.y = 2; src.height -= 4; break;
      case 1: src.width -= 1; break;
      case 2: dst.x = 1; dst.width -= 2; break;
      case 3: dst.height -= 2; break;
      case 6: source.width = src.width = 15; break;
      case 8: request.field = static_cast<crystalhd_vpp_bob::FieldParity>(2); break;
      case 9: source.pitch[0] = 15; break;
      case 10: source.pitch[1] = 15; break;
      case 11: target.pitch[0] = 15; break;
      case 12: target.pitch[1] = 15; break;
      case 13: target.planes[0] = nullptr; break;
      case 14: source.planes[1] = nullptr; break;
      case 16: source.pitch[0] = static_cast<unsigned int>(INT_MAX / 2) + 1; break;
      case 17: target.pitch[0] = 63; break;
      case 18: dst.y = 2; dst.height = 4; break;
      case 19: dst.x = 4; break;
      case 20: dst.x = 2; dst.width -= 2; break;
      default: break;
    }
    source.object_fds.push_back(777);
    target.object_fds.push_back(888);
    const auto source_bytes = source.storage, target_bytes = target.storage;
    const unsigned int before = io_calls;
    Scaler scaler;
    std::vector<uint8_t> staging;
    Surface *destination = variant == 15 ? &source : &target;
    const VAStatus status = ProcessVpp(&scaler.value, &staging, &source,
        destination, src, variant == 15 ? src : dst, false, request);
    source.object_fds.clear();
    target.object_fds.clear();
    Require(status == VA_STATUS_ERROR_INVALID_PARAMETER, "invalid Bob geometry/layout rejected");
    Require(io_calls == before, "invalid Bob rejected before any CPU-access ioctl");
    Require(source.storage == source_bytes && target.storage == target_bytes && staging.empty(),
            "invalid Bob leaves every pixel/guard/staging byte untouched");
  }
}

void AllocationFailure() {
  Surface source, target;
  Initialize(&source, 16, 8);
  Initialize(&target, 16, 8, true);
  Fill(&source);
  source.object_fds.push_back(777);
  const auto before = target.storage;
  Scaler scaler;
  std::vector<uint8_t> staging;
  VppDeinterlace request;
  request.bob = true;
  const unsigned int starts = read_starts, ends = read_ends;
  allocation_fault::fail_next = true;
  const VAStatus status = ProcessVpp(&scaler.value, &staging, &source, &target,
      Full(source), Full(target), false, request);
  allocation_fault::fail_next = false;
  source.object_fds.clear();
  Require(status == VA_STATUS_ERROR_ALLOCATION_FAILED, "ARGB allocation failure is an API error");
  Require(read_starts == starts + 1 && read_ends == ends + 1,
          "ARGB allocation failure balances its source CPU-read lease");
  Require(target.storage == before && !target.ready && staging.empty(),
          "ARGB allocation failure never touches destination");
}
}  // namespace

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
  if ((fd != 777 && fd != 888) || request != DMA_BUF_IOCTL_SYNC) abort();
  va_list arguments;
  va_start(arguments, request);
  const auto *sync = va_arg(arguments, const dma_buf_sync *);
  va_end(arguments);
  ++io_calls;
  if (sync->flags == (DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)) ++read_starts;
  if (sync->flags == (DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ)) ++read_ends;
  return 0;
}

int main(int argc, char **argv) {
  try {
    if (argc == 2 && strcmp(argv[1], "--import-layout") == 0) {
      ImportLayoutBounds();
      std::printf("VA actual import-layout bounds/lifetime: %zu-bit %u checks PASS\n",
                  sizeof(void *) * 8, checks);
      return 0;
    }
#ifndef CRYSTALHD_BOB_NV12_ONLY
    if (argc == 2 && strcmp(argv[1], "--argb-strides") == 0) {
      ZeroFilterArgbInvalidStrides();
      std::printf("VA zero-filter ARGB signed-stride rejection: %u checks PASS\n", checks);
      return 0;
    }
#endif
    Require(argc == 1, "usage: test [--import-layout|--argb-strides (native)]");
    for (unsigned int parity = 0; parity != 2; ++parity) {
      for (bool compact : {false, true}) {
        AlignedOutputRegions(parity, compact, false);
        AlignedOutputRegions(parity, compact, true);
        FullRangeBobPixels(parity, compact, false);
#ifndef CRYSTALHD_BOB_NV12_ONLY
        FullRangeBobPixels(parity, compact, true);
#endif
      }
      for (unsigned int height : {32U, 1088U})
        CompactConversion(32, height, parity);
      CompactConversion(14, 32, parity, {18, 19});
      CompactConversion(14, 32, parity, {18, 13});
      CompactConversion(32, 32, parity, {16, 12});
      // Exercise a vector block plus scalar tail when the final visible
      // luma row still needs its cropped same-field neighbour for chroma.
      CompactConversion(18, 4, parity, {18, 1});
      CompactConversion(34, 8, parity, {18, 3});
      Pixels(1920, 1080, 1280, 720, false, parity, false,
              {1920, 1088}, {1280, 736}, true);
      Pixels(14, 12, 22, 8, false, parity, false, {16, 17}, {23, 9}, true);
      Pixels(16, 32, 16, 32, false, parity, true, {}, {}, true);
#ifndef CRYSTALHD_BOB_NV12_ONLY
      Pixels(1920, 1080, 1280, 720, true, parity, false,
              {1920, 1088}, {1280, 736}, true);
      Pixels(14, 12, 22, 8, true, parity, false, {16, 17}, {23, 9});
      Pixels(14, 12, 22, 8, true, parity, false, {16, 17}, {23, 9}, true);
#endif
      for (unsigned int height : {720U, 1080U, 1088U})
        Pixels(16, height, 16, height, false, parity);
      Pixels(32, 1080, 16, 720, false, parity);
      Pixels(12, 720, 26, 1080, false, parity);
      Pixels(16, 1088, 8, 1080, false, parity);
      Pixels(16, 8, 16, 8, false, parity, true);
      Pixels(16, 12, 16, 12, false, parity, false, {16, 16}, {16, 16});
      Pixels(12, 12, 8, 8, false, parity, false, {16, 16}, {12, 12});
      Pixels(16, 720, 16, 720, false, parity, false, {16, 736}, {16, 736});
      Pixels(32, 1080, 16, 720, false, parity, false, {32, 1088}, {16, 736});
      // Exercise whole=0 and large whole steps at the uint16_t width limit,
      // with independent Y samples / UV pairs and no large frame allocation.
      Pixels(2, 4, 65534, 8, false, parity);
      Pixels(65534, 8, 2, 4, false, parity);
      Pixels(14, 12, 22, 8, false, parity);
      // Odd backing dimensions include the final full neutral UV pair and
      // ceil(height/2) row, but must leave pitch tails / outer guards intact.
      Pixels(16, 12, 16, 12, false, parity, false, {16, 16}, {17, 17});
#ifndef CRYSTALHD_BOB_NV12_ONLY
      Pixels(16, 720, 16, 720, true, parity);
      Pixels(32, 1080, 16, 720, true, parity);
      Pixels(12, 720, 26, 1080, true, parity);
      Pixels(16, 1088, 8, 1080, true, parity);
      Pixels(16, 12, 16, 12, true, parity, false, {16, 16}, {16, 16});
      Pixels(12, 12, 8, 8, true, parity, false, {16, 16}, {12, 12});
      Pixels(16, 720, 16, 720, true, parity, false, {16, 736}, {16, 736});
      Pixels(32, 1080, 16, 720, true, parity, false, {32, 1088}, {16, 736});
#endif
    }
    ZeroFilterOddWidthBackground();
    ZeroFilterNv12ExactResize();
#ifndef CRYSTALHD_BOB_NV12_ONLY
    ZeroFilterArgbInvalidStrides();
    ZeroFilterArgbBorders();
#endif
    Invalid();
    InvalidCompact();
    InvalidCompactConversion();
    AllocationFailure();
    ImportLayoutBounds();
    std::printf("VA Bob actual VPP pixels/guards/CPU leases: %u checks PASS\n", checks);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "VA Bob actual VPP FAIL: %s\n", error.what());
    return 1;
  }
}
