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
  const size_t uv_bytes = argb ? 0 : static_cast<size_t>(surface->pitch[1]) * (height / 2);
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
                    const VARectangle &src, const VARectangle &dst) {
  if (dst.width != target->width || dst.height != target->height) {
    for (unsigned int row = 0; row < target->height; ++row)
      std::fill_n(target->planes[0] + static_cast<size_t>(row) * target->pitch[0],
                  target->width, 16);
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
          target->planes[plane][static_cast<size_t>(y) * target->pitch[plane] + x * bytes + byte] =
              source.planes[plane][static_cast<size_t>(rows[y] * 2 + parity) * source.pitch[plane] +
                                   columns[x] * bytes + byte];
  }
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

void Pixels(unsigned int source_width, unsigned int source_height,
             unsigned int target_width, unsigned int target_height,
             bool argb, unsigned int parity, bool write_started = false,
             Geometry source_storage = {}, Geometry target_storage = {}) {
  Surface source, target, reference;
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
  if (!argb) ReferenceNv12(source, &reference, parity, src, dst);
#ifndef CRYSTALHD_BOB_NV12_ONLY
  else ReferenceArgb(source, &reference, parity, src, dst);
#endif
  Scaler scaler;
  std::vector<uint8_t> staging;
  VppDeinterlace request;
  request.bob = true;
  request.field = static_cast<crystalhd_vpp_bob::FieldParity>(parity);
  Require(ProcessVpp(&scaler.value, &staging, &source, &target, src,
                     dst, write_started, request) == VA_STATUS_SUCCESS,
          "actual Bob VPP succeeds");
  Require(target.storage == reference.storage,
          "actual Bob pixels/padded rows/outer guards match independent field reference");
  Require(source.storage == original, "source field pixels remain immutable");
  Require(target.ready && !target.failed && target.expected_timestamp == 12345 &&
              target.frame_timestamp == 12345, "completed Bob picture identity published");
  Require(argb || staging.empty(), "NV12 Bob does not allocate full-frame staging");
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

#ifndef CRYSTALHD_BOB_NV12_ONLY
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
  for (unsigned int variant = 0; variant != 18; ++variant) {
    Surface source, target;
    Initialize(&source, 16, variant == 4 ? 6 : 8);
    Initialize(&target, variant == 7 ? 15 : 16, variant == 5 ? 6 : 8, variant >= 16);
    Fill(&source);
    auto src = Full(source), dst = Full(target);
    VppDeinterlace request;
    request.bob = true;
    switch (variant) {
      case 0: src.y = 2; src.height -= 4; break;
      case 1: src.width -= 1; break;
      case 2: dst.x = 2; dst.width -= 2; break;
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
#ifndef CRYSTALHD_BOB_NV12_ONLY
    ZeroFilterArgbInvalidStrides();
#endif
    Invalid();
    AllocationFailure();
    ImportLayoutBounds();
    std::printf("VA Bob actual VPP pixels/guards/CPU leases: %u checks PASS\n", checks);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "VA Bob actual VPP FAIL: %s\n", error.what());
    return 1;
  }
}
