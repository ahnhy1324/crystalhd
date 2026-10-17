// SPDX-License-Identifier: LGPL-2.1-or-later
// Private CPU backing only; section GC removes all graphics/decoder APIs.
// Run each allocator benchmark mode in a FRESH process, including native -m32:
//   vaapi-private-backing-test --benchmark fresh|pool 500 8|24
// This measures allocation/zeroing, not decoding throughput or Atom timing.
#include "../filters/vaapi/crystalhd_drv_video.cpp"

#include <stdexcept>
#include <sys/resource.h>

extern "C" void gbm_bo_unmap(gbm_bo *, void *) { abort(); }
extern "C" void gbm_bo_destroy(gbm_bo *) { abort(); }
extern "C" gbm_bo *gbm_bo_create(gbm_device *, uint32_t, uint32_t, uint32_t, uint32_t) { abort(); }
extern "C" int gbm_bo_get_plane_count(gbm_bo *) { abort(); }
extern "C" void *gbm_bo_map(gbm_bo *, uint32_t, uint32_t, uint32_t, uint32_t,
                            uint32_t, uint32_t *, void **) { abort(); }
extern "C" uint32_t gbm_bo_get_stride_for_plane(gbm_bo *, int) { abort(); }
extern "C" uint32_t gbm_bo_get_offset(gbm_bo *, int) { abort(); }
extern "C" int drmGetDevice2(int, uint32_t, drmDevicePtr *) { abort(); }
extern "C" void drmFreeDevice(drmDevicePtr *) { abort(); }
extern "C" int drmPrimeHandleToFD(int, uint32_t, uint32_t, int *) { abort(); }
extern "C" drmVersionPtr drmGetVersion(int) { abort(); }
extern "C" void drmFreeVersion(drmVersionPtr) { abort(); }
extern "C" BC_STATUS DtsDeviceClose(HANDLE) { abort(); }
extern "C" BC_STATUS DtsStopDecoder(HANDLE) { abort(); }
extern "C" BC_STATUS DtsCloseDecoder(HANDLE) { abort(); }

namespace {
void Require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void LifetimeAndInitialization() {
  auto pool = std::make_shared<CrystalHDPrivateBackingPool>();
  auto original = std::make_shared<Surface>();
  Require(original->AllocateCpuNv12(129, 17, pool), "allocate odd private geometry");
  uint8_t *const backing = original->storage.data();
  std::fill(original->storage.begin(), original->storage.end(), 0xa5);
  original->id = 77;
  original->ready = original->failed = true;
  original->expected_timestamp = original->frame_timestamp = 123;
  original->decode_identity = original->trace_decode_identity = 45;
  std::weak_ptr<Surface> old_identity = original;
  auto retained = original;
  original.reset();
  Require(pool->Cached().buffers == 0 && retained->storage[0] == 0xa5,
          "a retained immutable picture cannot return backing");
  std::thread release([picture = std::move(retained)]() mutable { picture.reset(); });
  release.join();
  Require(old_identity.expired() && pool->Cached().buffers == 1,
          "only final release, including on another thread, returns backing");
  Surface replacement;
  Require(replacement.AllocateCpuNv12(32, 52, pool) &&
              replacement.storage.data() == backing && replacement.width == 32 &&
              replacement.height == 52 &&
              replacement.pitch[0] == 64 && replacement.pitch[1] == 64 &&
              replacement.planes[1] == backing + 64 * 52 &&
              !replacement.ready && !replacement.failed &&
              replacement.id == VA_INVALID_SURFACE && replacement.expected_timestamp == 0 &&
              replacement.frame_timestamp == 0 && replacement.decode_identity == 0 &&
              replacement.trace_decode_identity == 0 &&
              std::all_of(replacement.storage.begin(), replacement.storage.end(),
                          [](uint8_t byte) { return byte == 0; }),
          "equal-byte different geometry reuses zeroed padding, never picture metadata");
}

void BoundsAndReset() {
  CrystalHDPrivateBackingPool pool;
  for (unsigned n = 0; n < 12; ++n) {
    std::vector<uint8_t> bytes(64);
    pool.Recycle(std::move(bytes));
  }
  Require(pool.Cached().buffers == 4 && pool.Cached().bytes == 256,
          "idle storage has a fixed slot bound");
  auto different = pool.Acquire(65);
  Require(different.size() == 65 && pool.Cached().buffers == 4,
          "different sizes cannot consume an incompatible buffer");
  CrystalHDPrivateBackingPool capacity;
  for (unsigned n = 0; n < 3; ++n) {
    std::vector<uint8_t> bytes;
    bytes.reserve(CrystalHDPrivateBackingPool::kBytes / 2);
    bytes.resize(1);
    capacity.Recycle(std::move(bytes));
  }
  Require(capacity.Cached().bytes == CrystalHDPrivateBackingPool::kBytes &&
              capacity.Cached().buffers == 2,
          "retention is bounded by actual capacity, not visible size");
  std::vector<uint8_t> oversized(CrystalHDPrivateBackingPool::kBytes + 1);
  CrystalHDPrivateBackingPool empty;
  empty.Recycle(std::move(oversized));
  Require(empty.Cached().bytes == 0, "oversized backing is not cached");
  DecodeContext context;
  context.private_backing_pool = std::make_shared<CrystalHDPrivateBackingPool>();
  const std::weak_ptr<CrystalHDPrivateBackingPool> old_pool = context.private_backing_pool;
  auto held = std::make_shared<Surface>();
  Require(held->AllocateCpuNv12(16, 16, context.private_backing_pool), "allocate old epoch");
  context.decoded_frames[1] = held;
  Require(context.CloseHardware(0) == BC_STS_SUCCESS && !old_pool.expired(),
          "hardware-only reopen preserves the pool");
  Require(context.Reset(0) == BC_STS_SUCCESS && old_pool.expired() &&
              !context.private_backing_pool && held->storage.size() != 0,
          "logical reset expires the pool without invalidating held old pixels");
  context.private_backing_pool = std::make_shared<CrystalHDPrivateBackingPool>();
  held.reset();
  Require(context.private_backing_pool->Cached().buffers == 0,
          "late old-generation release cannot populate the new pool");
  {
    Surface exposed;
    Require(exposed.AllocateCpuNv12(16, 16, context.private_backing_pool),
            "allocate a backing before an exposure eligibility change");
    exposed.layout.valid = true;
  }
  Require(context.private_backing_pool->Cached().buffers == 0,
          "externally exposed backing is never returned even with a pool link");
}

double Seconds(const timeval &time) { return time.tv_sec + time.tv_usec / 1e6; }

void Benchmark(bool pooled, unsigned frames, unsigned window) {
  auto pool = std::make_shared<CrystalHDPrivateBackingPool>();
  std::deque<std::shared_ptr<Surface>> held;
  unsigned fresh = 0, reused = 0;
  struct rusage before = {}, warmed = {}, after = {};
  getrusage(RUSAGE_SELF, &before);
  const auto start = std::chrono::steady_clock::now();
  auto warm_start = start;
  for (unsigned frame = 0; frame < frames; ++frame) {
    // Match SubmitPicture: construct/allocate the NEW picture before retiring
    // the oldest completed picture. Both modes retain the same live window.
    auto picture = std::make_shared<Surface>();
    if (pooled) {
      if (pool->Cached().buffers != 0) ++reused; else ++fresh;
      Require(picture->AllocateCpuNv12(1920, 1080, pool), "allocate pooled frame");
    } else {
      ++fresh;
      Require(picture->AllocateInternal(nullptr, -1, 1920, 1080, VA_FOURCC_NV12),
              "allocate baseline frame");
    }
    Require(std::all_of(picture->storage.begin(), picture->storage.end(),
                        [](uint8_t byte) { return byte == 0; }),
            "every newly assigned byte, including padding, starts zero");
    std::fill(picture->storage.begin(), picture->storage.end(), uint8_t(frame % 251 + 1));
    if (held.size() == window) {
      const uint8_t expected = static_cast<uint8_t>((frame - window) % 251 + 1);
      Require(std::all_of(held.front()->storage.begin(), held.front()->storage.end(),
                          [&](uint8_t byte) { return byte == expected; }),
              "allocating another frame cannot mutate any retained picture");
      held.pop_front();
    }
    held.push_back(std::move(picture));
    if (frame == window) {
      getrusage(RUSAGE_SELF, &warmed);
      warm_start = std::chrono::steady_clock::now();
    }
  }
  const double wall = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - start).count();
  getrusage(RUSAGE_SELF, &after);
  Require(!pooled || (fresh == window + 1 && reused == frames - window - 1),
          "only the initial live window plus one fresh backing is required");
  const double steady_wall = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - warm_start).count();
  printf("{\"mode\":\"%s\",\"pointer_bits\":%zu,\"frames\":%u,\"window\":%u,"
         "\"fresh_backings\":%u,\"reused_backings\":%u,\"minor_faults\":%ld,"
         "\"user_seconds\":%.6f,\"system_seconds\":%.6f,\"wall_seconds\":%.6f,"
         "\"warmup_frames\":%u,\"steady_minor_faults\":%ld,\"steady_user_seconds\":%.6f,"
         "\"steady_system_seconds\":%.6f,\"steady_wall_seconds\":%.6f,"
         "\"peak_rss_kib\":%ld,\"cached_bytes\":%zu,\"pixels_and_padding\":\"PASS\"}\n",
         pooled ? "pool" : "fresh", sizeof(void *) * 8, frames, window, fresh, reused,
         after.ru_minflt - before.ru_minflt,
         Seconds(after.ru_utime) - Seconds(before.ru_utime),
         Seconds(after.ru_stime) - Seconds(before.ru_stime), wall,
         window + 1, after.ru_minflt - warmed.ru_minflt,
         Seconds(after.ru_utime) - Seconds(warmed.ru_utime),
         Seconds(after.ru_stime) - Seconds(warmed.ru_stime), steady_wall, after.ru_maxrss,
         pool->Cached().bytes);
}
}  // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 5 && strcmp(argv[1], "--benchmark") == 0 &&
        (strcmp(argv[2], "fresh") == 0 || strcmp(argv[2], "pool") == 0)) {
      const unsigned frames = std::stoul(argv[3]), window = std::stoul(argv[4]);
      Require(frames > window && frames <= 10000 && window > 0 && window <= 64,
              "bounded benchmark frame/window counts");
      Benchmark(strcmp(argv[2], "pool") == 0, frames, window);
    } else {
      Require(argc == 1, "usage: test [--benchmark fresh|pool FRAMES WINDOW]");
      LifetimeAndInitialization();
      BoundsAndReset();
      puts("VA private CPU backing: lifetime/initialization/bounds/reset PASS");
    }
  } catch (const std::exception &error) {
    fprintf(stderr, "VA private CPU backing FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
