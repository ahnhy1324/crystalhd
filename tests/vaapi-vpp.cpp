// SPDX-License-Identifier: LGPL-2.1-or-later
// Exercise the actual driver state machine with private memory only: no DRM
// device, firmware, browser, or CrystalHD device is opened by these tests.
#include <functional>
#include <mutex>
static thread_local unsigned bob_resampled_rows[2] = {};
static thread_local size_t nv12_cleared_bytes = 0;
static thread_local unsigned nv12_resampled_rows[2] = {};
#define CRYSTALHD_VAAPI_TEST_NV12_CLEAR(bytes) (nv12_cleared_bytes += (bytes))
#define CRYSTALHD_VAAPI_TEST_NV12_RESAMPLE_ROW(plane) (++nv12_resampled_rows[plane])
#define CRYSTALHD_VAAPI_TEST_BOB_RESAMPLE_ROW(plane) (++bob_resampled_rows[plane])
namespace {
struct Driver;
void VppTestWorkerTurn(Driver *, std::unique_lock<std::mutex> &);
void VppTestPixels(Driver *, uint64_t);
void VppTestDestroyWait();
}
#define CRYSTALHD_VAAPI_TEST_VPP_WORKER_TURN VppTestWorkerTurn
#define CRYSTALHD_VAAPI_TEST_VPP_PIXELS VppTestPixels
#define CRYSTALHD_VAAPI_TEST_VPP_DESTROY_WAIT VppTestDestroyWait
#define DtsGetDriverStatus VppTestGetDriverStatus
#define DtsStopDecoder VppTestStopDecoder
#define DtsDeviceOpen VppTestDeviceOpen
#ifdef CRYSTALHD_VPP_CPU_ONLY
// Same-translation-unit abort stubs cannot be intercepted by linker wraps.
// Route the mocked allocation boundaries explicitly; their fallbacks still abort.
#define drmGetDevice2 __wrap_drmGetDevice2
#define drmFreeDevice __wrap_drmFreeDevice
#define drmPrimeHandleToFD __wrap_drmPrimeHandleToFD
#endif
#include "../filters/vaapi/crystalhd_drv_video.cpp"
#ifdef CRYSTALHD_VPP_CPU_ONLY
#undef drmGetDevice2
#undef drmFreeDevice
#undef drmPrimeHandleToFD
#endif
#undef DtsGetDriverStatus
#undef DtsStopDecoder
#undef DtsDeviceOpen
#undef CRYSTALHD_VAAPI_TEST_VPP_WORKER_TURN
#undef CRYSTALHD_VAAPI_TEST_BOB_RESAMPLE_ROW
#undef CRYSTALHD_VAAPI_TEST_NV12_CLEAR
#undef CRYSTALHD_VAAPI_TEST_NV12_RESAMPLE_ROW

#include <atomic>
#include <cmath>
#include <cerrno>
#include <dirent.h>
#include <linux/sync_file.h>
#include <stdexcept>
#include <string>

#ifdef CRYSTALHD_VPP_CPU_ONLY
#include "vaapi-vpp-cpu-stubs.h"
#endif

namespace allocation_fault {
thread_local bool fail_next = false;
thread_local size_t only_size = 0, failed_size = 0;
thread_local unsigned skip_matches = 0;
void *Allocate(size_t bytes) {
  if (fail_next && (only_size == 0 || bytes == only_size)) {
    if (skip_matches != 0) {
      --skip_matches;
    } else {
      fail_next = false;
      failed_size = bytes;
      throw std::bad_alloc();
    }
  }
  if (void *result = std::malloc(bytes == 0 ? 1 : bytes)) return result;
  throw std::bad_alloc();
}
}
void *operator new(size_t bytes) { return allocation_fault::Allocate(bytes); }
void *operator new[](size_t bytes) { return allocation_fault::Allocate(bytes); }
void *operator new(size_t bytes, const std::nothrow_t &) noexcept {
  try { return allocation_fault::Allocate(bytes); } catch (...) { return nullptr; }
}
void *operator new[](size_t bytes, const std::nothrow_t &) noexcept {
  try { return allocation_fault::Allocate(bytes); } catch (...) { return nullptr; }
}
#if defined(__GNUC__) || defined(__clang__)
#define CRYSTALHD_TEST_NOINLINE __attribute__((noinline))
#else
#define CRYSTALHD_TEST_NOINLINE
#endif
CRYSTALHD_TEST_NOINLINE void operator delete(void *p) noexcept { std::free(p); }
CRYSTALHD_TEST_NOINLINE void operator delete[](void *p) noexcept { std::free(p); }
CRYSTALHD_TEST_NOINLINE void operator delete(void *p, size_t) noexcept { std::free(p); }
CRYSTALHD_TEST_NOINLINE void operator delete[](void *p, size_t) noexcept { std::free(p); }
CRYSTALHD_TEST_NOINLINE void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
CRYSTALHD_TEST_NOINLINE void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }
#undef CRYSTALHD_TEST_NOINLINE

extern "C" int __real_close(int fd);
extern "C" int __real_eventfd(unsigned int initial_value, int flags);

namespace {

unsigned int device_open_attempts = 0;

// Only the current test thread's descriptor calls are mocked. Other VPP
// tests still exercise the real driver state machine, and the wrappers never
// affect the dynamically linked decoder library. No real device is opened.
struct FenceIoMock {
  static constexpr int timeline = 7001;
  static constexpr int fence = 7002;
  int fail_start_fd = -1;
  int fail_end_fd = -1;
  int fail_import_fd = -1;
  int fail_read_start_fd = -1;
  int fail_read_end_fd = -1;
  bool fail_signal = false;
  uint64_t transient_flags = UINT64_MAX;
  std::deque<int> transient_errors;
  bool fail_open = false;
  bool fail_create = false;
  bool signaled = false;
  bool fence_created = false;
  int fence_status = 0;  // sync_file: zero pending, one success, negative error
  bool self_wait = false;
  bool invalid = false;
  std::vector<std::string> events;
  std::unordered_set<int> started;
  std::unordered_set<int> read_started;
  std::unordered_set<int> imported;
  std::unordered_set<int> handles;
  std::unordered_set<int> real_files;

  int Error() {
    errno = EIO;
    return -1;
  }

  int Open(const char *path, int flags) {
    events.push_back("open");
    invalid |= strcmp(path, kSwSyncPath) != 0 ||
               flags != (O_RDWR | O_CLOEXEC);
    if (fail_open)
      return Error();
    handles.insert(timeline);
    return timeline;
  }

  int Ioctl(int fd, unsigned long request, void *argument) {
    if (request == DMA_BUF_IOCTL_SYNC) {
      const auto *sync = static_cast<dma_buf_sync *>(argument);
      if (sync->flags == transient_flags && !transient_errors.empty()) {
        events.push_back("retry:" + std::to_string(fd));
        errno = transient_errors.front();
        transient_errors.pop_front();
        return -1;
      }
      if (sync->flags == (DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) {
        events.push_back("start:" + std::to_string(fd));
        if (fd == fail_start_fd)
          return Error();
        invalid |= !started.insert(fd).second;
      } else if (sync->flags == (DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE)) {
        events.push_back("end:" + std::to_string(fd));
        // Real i915 can block here on the writer's own unsignaled fence.
        // Record the violation instead so regressions fail without hanging.
        self_wait |= imported.count(fd) != 0 && !signaled;
        invalid |= started.erase(fd) != 1;
        if (fd == fail_end_fd)
          return Error();
      } else if (sync->flags == (DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)) {
        events.push_back("read-start:" + std::to_string(fd));
        self_wait |= imported.count(fd) != 0 && !signaled;
        if (fd == fail_read_start_fd)
          return Error();
        invalid |= !read_started.insert(fd).second;
      } else if (sync->flags == (DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ)) {
        events.push_back("read-end:" + std::to_string(fd));
        invalid |= read_started.erase(fd) != 1;
        if (fd == fail_read_end_fd)
          return Error();
      } else {
        invalid = true;
        return Error();
      }
      return 0;
    }
    if (request == SW_SYNC_IOC_CREATE_FENCE) {
      events.push_back("create");
      auto *create = static_cast<SwSyncCreateFenceData *>(argument);
      invalid |= fd != timeline || handles.count(timeline) != 1 ||
                 create->value != 1;
      if (fail_create)
        return Error();
      create->fence = fence;
      fence_created = true;
      handles.insert(fence);
      return 0;
    }
    if (request == DMA_BUF_IOCTL_IMPORT_SYNC_FILE) {
      events.push_back("import:" + std::to_string(fd));
      const auto *import = static_cast<dma_buf_import_sync_file *>(argument);
      invalid |= started.count(fd) != 1 || import->flags != DMA_BUF_SYNC_WRITE ||
                 import->fd != fence || handles.count(fence) != 1;
      if (fd == fail_import_fd)
        return Error();
      imported.insert(fd);
      return 0;
    }
    if (request == SW_SYNC_IOC_INC) {
      events.push_back("signal");
      invalid |= fd != timeline || handles.count(timeline) != 1 ||
                 *static_cast<uint32_t *>(argument) != 1 || signaled;
      if (fail_signal)
        return Error();
      signaled = true;
      fence_status = 1;
      return 0;
    }
    invalid = true;
    return Error();
  }

  int Close(int fd) {
    events.push_back("close:" + std::to_string(fd));
    if (real_files.erase(fd) != 0) {
      invalid |= started.count(fd) != 0 || read_started.count(fd) != 0;
      return __real_close(fd);
    }
    if (fd == timeline || fd == fence) {
      invalid |= handles.erase(fd) != 1;
      // Linux sw_sync release signals all pending fences with -ENOENT.
      if (fd == timeline && fence_created && !signaled) {
        signaled = true;
        fence_status = -ENOENT;
      }
    } else {
      invalid |= fd < 101 || fd > 103 || started.count(fd) != 0 ||
                 read_started.count(fd) != 0;
    }
    return 0;
  }
};

thread_local FenceIoMock *fence_io_mock = nullptr;
thread_local unsigned int *synchronous_status_polls = nullptr;
thread_local unsigned int *synchronous_decoder_stops = nullptr;
thread_local std::function<void(Driver *, std::unique_lock<std::mutex> &)>
    vpp_worker_turn;

void VppTestWorkerTurn(Driver *driver, std::unique_lock<std::mutex> &lock) {
  // One fixture-only turn; ordinary VPP tests use the real condition wait.
  // This controls cancellation ordering, not the scheduler's 1 ms latency.
  auto turn = std::move(vpp_worker_turn);
  if (turn) turn(driver, lock);
}

static thread_local std::function<void(Driver *, uint64_t)> vpp_pixels;
static thread_local std::function<void()> vpp_destroy_wait;
void VppTestPixels(Driver *driver, uint64_t sequence) {
  if (vpp_pixels) vpp_pixels(driver, sequence);
}
void VppTestDestroyWait() {
  if (vpp_destroy_wait) vpp_destroy_wait();
}

struct MockFenceScope {
  explicit MockFenceScope(FenceIoMock &mock) { fence_io_mock = &mock; }
  ~MockFenceScope() { fence_io_mock = nullptr; }
};

// A discovery cache must never turn per-surface DRM files into a shared lease.
// Model only the current thread's node files and owned DUMB allocations.
struct NativeNodeMock {
  static constexpr int render_fd = 8100;
  struct File { bool primary; struct stat identity; };
  struct Dumb { int fd; uint64_t size; uint32_t pitch; uint64_t offset; };
  struct stat render = {}, primary = {};
  char primary_path[64] = "/dev/dri/crystalhd-test-primary";
  char *nodes[DRM_NODE_MAX] = {};
  drmDevice device = {};
  bool device_owned = false, no_primary = false, null_device = false;
  bool fail_destroy = false;
  int mmap_interface_version = 4;
  bool fail_mmap_version = false, fail_wc_offset = false;
  bool different_wc_offset = false;
  unsigned interrupt_mmap_version = 0, interrupt_wc_offset = 0;
  unsigned mmap_version_queries = 0, wc_offset_queries = 0;
  unsigned fail_queries = 0, fail_opens = 0;
  unsigned fail_render_stats = 0, fail_primary_stats = 0;
  unsigned queries = 0, frees = 0, opens = 0, closes = 0;
  unsigned render_stats = 0, primary_stats = 0;
  unsigned creates = 0, destroys = 0, maps = 0, unmaps = 0;
  unsigned map_attempts = 0;
  unsigned fail_bookkeeping_map = 0, fail_bookkeeping_allocation = 0;
  uint64_t dumb_offset_base = 0, last_mapped_offset = 0;
  bool override_create_size = false;
  uint64_t create_size_override = 0;
  unsigned invalid = 0;
  int next_fd = 8200;
  uint32_t next_handle = 1;
  std::unordered_map<int, File> files;
  std::unordered_map<uint32_t, Dumb> dumbs;
  std::unordered_map<void *, std::unique_ptr<std::vector<uint8_t>>> mappings;

  NativeNodeMock() {
    render.st_dev = 10; render.st_ino = 20; render.st_rdev = 30;
    primary.st_dev = 40; primary.st_ino = 50; primary.st_rdev = 60;
    render.st_mode = primary.st_mode = S_IFCHR | 0600;
  }
  int Error() { errno = EIO; return -1; }
  int Open(const char *path, int flags) {
    ++opens;
    if (strcmp(path, primary_path) != 0 || flags != (O_RDWR | O_CLOEXEC)) {
      ++invalid;
      return Error();
    }
    if (fail_opens != 0) { --fail_opens; return Error(); }
    const int fd = next_fd++;
    files.emplace(fd, File{true, primary});
    return fd;
  }
  int Stat(int fd, struct stat *info) {
    if (fd == render_fd) {
      ++render_stats;
      if (fail_render_stats != 0) { --fail_render_stats; return Error(); }
      *info = render;
      return 0;
    }
    const auto file = files.find(fd);
    if (file == files.end()) { ++invalid; return Error(); }
    ++primary_stats;
    if (fail_primary_stats != 0) { --fail_primary_stats; return Error(); }
    *info = file->second.identity;
    return 0;
  }
  int Query(int fd, uint32_t flags, drmDevicePtr *result) {
    ++queries;
    *result = nullptr;
    if (fd != render_fd || flags != 0 || device_owned) {
      ++invalid;
      return Error();
    }
    if (fail_queries != 0) { --fail_queries; return Error(); }
    if (null_device)
      return 0;
    nodes[DRM_NODE_PRIMARY] = primary_path;
    device.nodes = nodes;
    device.available_nodes = no_primary ? 0 : (1U << DRM_NODE_PRIMARY);
    device_owned = true;
    *result = &device;
    return 0;
  }
  void Free(drmDevicePtr *value) {
    ++frees;
    invalid += !device_owned || *value != &device;
    device_owned = false;
    *value = nullptr;
  }
  int Close(int fd) {
    ++closes;
    const auto file = files.find(fd);
    if (file == files.end()) { ++invalid; return Error(); }
    // Closing a surface's own DRM file also drops a failed DESTROY_DUMB handle.
    if (file->second.primary)
      for (auto it = dumbs.begin(); it != dumbs.end(); ) {
        if (it->second.fd == fd) it = dumbs.erase(it);
        else ++it;
      }
    files.erase(file);
    return 0;
  }
  int Ioctl(int fd, unsigned long request, void *argument) {
    const auto file = files.find(fd);
    if (file == files.end() || !file->second.primary) {
      ++invalid;
      return Error();
    }
    if (request == DRM_IOCTL_I915_GETPARAM) {
      auto *query = static_cast<drm_i915_getparam_t *>(argument);
      ++mmap_version_queries;
      if (query->param != I915_PARAM_MMAP_GTT_VERSION || query->value == nullptr) {
        ++invalid;
        return Error();
      }
      if (interrupt_mmap_version != 0) {
        --interrupt_mmap_version;
        errno = EINTR;
        return -1;
      }
      if (fail_mmap_version)
        return Error();
      *query->value = mmap_interface_version;
      return 0;
    }
    if (request == DRM_IOCTL_I915_GEM_MMAP_OFFSET) {
      auto *map = static_cast<drm_i915_gem_mmap_offset *>(argument);
      ++wc_offset_queries;
      const auto dumb = dumbs.find(map->handle);
      if (dumb == dumbs.end() || dumb->second.fd != fd || map->pad != 0 ||
          map->flags != I915_MMAP_OFFSET_WC || map->extensions != 0) {
        ++invalid;
        return Error();
      }
      if (interrupt_wc_offset != 0) {
        --interrupt_wc_offset;
        errno = EINTR;
        return -1;
      }
      if (fail_wc_offset)
        return Error();
      map->offset = dumb->second.offset + (different_wc_offset ? 4096U : 0U);
      return 0;
    }
    if (request == DRM_IOCTL_MODE_CREATE_DUMB) {
      auto *create = static_cast<drm_mode_create_dumb *>(argument);
      if (create->bpp != 8) { ++invalid; return Error(); }
      ++creates;
      create->handle = next_handle++;
      create->pitch = Align(create->width, 64);
      create->size = static_cast<uint64_t>(create->pitch) * create->height;
      if (override_create_size) create->size = create_size_override;
      dumbs.emplace(create->handle, Dumb{fd, create->size, create->pitch,
          dumb_offset_base + static_cast<uint64_t>(create->handle) * 4096U});
      return 0;
    }
    if (request == DRM_IOCTL_MODE_MAP_DUMB) {
      auto *map = static_cast<drm_mode_map_dumb *>(argument);
      const auto dumb = dumbs.find(map->handle);
      if (dumb == dumbs.end() || dumb->second.fd != fd) {
        ++invalid;
        return Error();
      }
      map->offset = dumb->second.offset;
      return 0;
    }
    if (request == DRM_IOCTL_MODE_DESTROY_DUMB) {
      ++destroys;
      const auto *destroy = static_cast<drm_mode_destroy_dumb *>(argument);
      const auto dumb = dumbs.find(destroy->handle);
      if (dumb == dumbs.end() || dumb->second.fd != fd) {
        ++invalid;
        return Error();
      }
      if (fail_destroy)
        return Error();
      dumbs.erase(dumb);
      return 0;
    }
    ++invalid;
    return Error();
  }
  int Prime(int fd, uint32_t handle, uint32_t flags, int *result) {
    const auto dumb = dumbs.find(handle);
    if (dumb == dumbs.end() || dumb->second.fd != fd ||
        flags != (DRM_CLOEXEC | DRM_RDWR)) {
      ++invalid;
      return Error();
    }
    *result = next_fd++;
    files.emplace(*result, File{false, primary});
    return 0;
  }
  void *Map(size_t size, int prot, int flags, int fd, off64_t offset) {
    ++map_attempts;
    last_mapped_offset = static_cast<uint64_t>(offset);
    const auto dumb = std::find_if(dumbs.begin(), dumbs.end(),
        [&](const auto &entry) {
          return entry.second.fd == fd && entry.second.offset ==
              static_cast<uint64_t>(offset) && entry.second.size == size;
        });
    if (dumb == dumbs.end() || prot != (PROT_READ | PROT_WRITE) ||
        flags != MAP_SHARED) {
      ++invalid;
      errno = EIO;
      return MAP_FAILED;
    }
    ++maps;
    auto pixels = std::make_unique<std::vector<uint8_t>>(size);
    void *address = pixels->data();
    mappings.emplace(address, std::move(pixels));
    if (maps == fail_bookkeeping_map) {
      allocation_fault::only_size = 0;
      allocation_fault::failed_size = 0;
      allocation_fault::skip_matches = fail_bookkeeping_allocation - 1;
      allocation_fault::fail_next = true;
    }
    return address;
  }
  int Unmap(void *address, size_t size) {
    const auto mapping = mappings.find(address);
    if (mapping == mappings.end() || mapping->second->size() != size) {
      ++invalid;
      return Error();
    }
    ++unmaps;
    mappings.erase(mapping);
    return 0;
  }
};

thread_local NativeNodeMock *native_node_mock = nullptr;
struct MockNativeNodeScope {
  explicit MockNativeNodeScope(NativeNodeMock &mock) { native_node_mock = &mock; }
  ~MockNativeNodeScope() { native_node_mock = nullptr; }
};

}  // namespace

// Only an explicitly armed, device-less cold-source fixture may make progress
// polls. No hardware or extra production API call is introduced by this shim.
extern "C" BC_STATUS VppTestGetDriverStatus(HANDLE device,
                                             BC_DTS_STATUS *status) {
  if (synchronous_status_polls == nullptr || device != nullptr || status == nullptr)
    return BC_STS_INV_ARG;
  ++*synchronous_status_polls;
  *status = {};
  return BC_STS_SUCCESS;
}

extern "C" BC_STATUS VppTestStopDecoder(HANDLE device) {
  if (synchronous_decoder_stops == nullptr || device != nullptr)
    return BC_STS_INV_ARG;
  ++*synchronous_decoder_stops;
  return BC_STS_SUCCESS;
}

extern "C" BC_STATUS VppTestDeviceOpen(HANDLE *device, uint32_t) {
  ++device_open_attempts;
  if (device != nullptr) *device = nullptr;
  return BC_STS_INV_ARG;  // A regressed target guard must never reach hardware.
}

extern "C" int __real_open(const char *path, int flags, ...);
extern "C" int __real_ioctl(int fd, unsigned long request, ...);
extern "C" int __real_close(int fd);
extern "C" int __real_fstat(int fd, struct stat *info);
extern "C" int __real_drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
extern "C" void __real_drmFreeDevice(drmDevicePtr *device);
extern "C" int __real_drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags,
                                         int *prime_fd);
extern "C" void *__real_mmap(void *, size_t, int, int, int, off_t);
extern "C" void *__real_mmap64(void *, size_t, int, int, int, off64_t);
extern "C" int __real_munmap(void *, size_t);

namespace {
// Real private memfds exercise the importer. Fixed-size bookkeeping must not
// allocate while the selected production vector allocation is being failed.
struct ImportIoProbe {
  int sources[2] = {-1, -1};
  struct stat identities[2] = {};
  struct Mapping { void *address = nullptr; size_t size = 0; } mappings[4];
  unsigned maps = 0, unmaps = 0, invalid = 0;
  int arm_fd = -1;
  unsigned stat_calls = 0, fail_allocation = 0;
  bool armed = false;

  void Arm() {
    allocation_fault::only_size = 0;
    allocation_fault::failed_size = 0;
    allocation_fault::skip_matches = fail_allocation - 1;
    allocation_fault::fail_next = true;
    armed = true;
  }
  void Stat(int fd) {
    // One pending-backing preflight, then the NV12 layout's object and two
    // plane identities. The last stat follows layout-vector allocation and
    // immediately precedes the second surface's ImportLegacy/Prime metadata.
    if (fd == arm_fd && ++stat_calls == 4)
      Arm();
  }
  void Map(void *address, size_t size) {
    if (address == MAP_FAILED) return;
    ++maps;
    for (auto &mapping : mappings) {
      if (mapping.address == nullptr) {
        mapping = {address, size};
        return;
      }
    }
    ++invalid;
  }
  void Unmap(void *address, size_t size, int result) {
    for (auto &mapping : mappings) {
      if (mapping.address == address) {
        if (result == 0 && mapping.size == size) {
          mapping = {};
          ++unmaps;
        } else {
          ++invalid;
        }
        return;
      }
    }
    ++invalid;
  }
  int Copies(bool cleanup = false) const {
    DIR *directory = opendir("/proc/self/fd");
    if (directory == nullptr) return -1;
    int count = 0;
    while (const dirent *entry = readdir(directory)) {
      if (entry->d_name[0] == '.') continue;
      const int fd = static_cast<int>(strtol(entry->d_name, nullptr, 10));
      if (fd == sources[0] || fd == sources[1]) continue;
      struct stat info = {};
      if (__real_fstat(fd, &info) != 0) continue;
      for (unsigned source = 0; source < 2; ++source) {
        if (sources[source] >= 0 && info.st_dev == identities[source].st_dev &&
            info.st_ino == identities[source].st_ino) {
          ++count;
          if (cleanup) __real_close(fd);
          break;
        }
      }
    }
    closedir(directory);
    return count;
  }
  ~ImportIoProbe() {
    // Failed baseline controls may have orphaned a duplicate or mmap. Record
    // their imbalance before assertions; clean them only when leaving the test.
    for (const auto &mapping : mappings)
      if (mapping.address != nullptr)
        __real_munmap(mapping.address, mapping.size);
    Copies(true);
    for (int fd : sources)
      if (fd >= 0) __real_close(fd);
  }
};
thread_local ImportIoProbe *import_io_probe = nullptr;
struct ImportIoScope {
  explicit ImportIoScope(ImportIoProbe &probe) { import_io_probe = &probe; }
  ~ImportIoScope() {
    allocation_fault::fail_next = false;
    allocation_fault::only_size = 0;
    allocation_fault::skip_matches = 0;
    import_io_probe = nullptr;
  }
};
}  // namespace

extern "C" int __wrap_open(const char *path, int flags, ...) {
  if (native_node_mock != nullptr)
    return native_node_mock->Open(path, flags);
  if (fence_io_mock != nullptr)
    return fence_io_mock->Open(path, flags);
  if ((flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE) {
    va_list arguments;
    va_start(arguments, flags);
    const mode_t mode = va_arg(arguments, mode_t);
    va_end(arguments);
    return __real_open(path, flags, mode);
  }
  return __real_open(path, flags);
}

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
  va_list arguments;
  va_start(arguments, request);
  void *argument = va_arg(arguments, void *);
  va_end(arguments);
  if (native_node_mock != nullptr)
    return native_node_mock->Ioctl(fd, request, argument);
  return fence_io_mock != nullptr
             ? fence_io_mock->Ioctl(fd, request, argument)
             : __real_ioctl(fd, request, argument);
}

extern "C" int __wrap_close(int fd) {
  if (native_node_mock != nullptr)
    return native_node_mock->Close(fd);
  return fence_io_mock != nullptr ? fence_io_mock->Close(fd)
                                  : __real_close(fd);
}

extern "C" int __wrap_fstat(int fd, struct stat *info) {
  const int result = native_node_mock != nullptr
      ? native_node_mock->Stat(fd, info) : __real_fstat(fd, info);
  if (result == 0 && import_io_probe != nullptr) import_io_probe->Stat(fd);
  return result;
}
extern "C" int __wrap_drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device) {
  return native_node_mock != nullptr ? native_node_mock->Query(fd, flags, device)
                                     : __real_drmGetDevice2(fd, flags, device);
}
extern "C" void __wrap_drmFreeDevice(drmDevicePtr *device) {
  if (native_node_mock != nullptr)
    native_node_mock->Free(device);
  else
    __real_drmFreeDevice(device);
}
extern "C" int __wrap_drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags,
                                         int *prime_fd) {
  return native_node_mock != nullptr
      ? native_node_mock->Prime(fd, handle, flags, prime_fd)
      : __real_drmPrimeHandleToFD(fd, handle, flags, prime_fd);
}
extern "C" void *__wrap_mmap(void *address, size_t size, int prot, int flags,
                              int fd, off_t offset) {
  void *result = native_node_mock != nullptr
      ? native_node_mock->Map(size, prot, flags, fd, offset)
      : __real_mmap(address, size, prot, flags, fd, offset);
  if (import_io_probe != nullptr) import_io_probe->Map(result, size);
  return result;
}
extern "C" void *__wrap_mmap64(void *address, size_t size, int prot, int flags,
                                int fd, off64_t offset) {
  void *result = native_node_mock != nullptr
      ? native_node_mock->Map(size, prot, flags, fd, offset)
      : __real_mmap64(address, size, prot, flags, fd, offset);
  if (import_io_probe != nullptr) import_io_probe->Map(result, size);
  return result;
}
extern "C" int __wrap_munmap(void *address, size_t size) {
  const int result = native_node_mock != nullptr
      ? native_node_mock->Unmap(address, size) : __real_munmap(address, size);
  if (import_io_probe != nullptr) import_io_probe->Unmap(address, size, result);
  return result;
}

extern "C" int __wrap_eventfd(unsigned int initial_value, int flags) {
  const int fd = __real_eventfd(initial_value, flags);
  if (fd >= 0 && fence_io_mock != nullptr)
    fence_io_mock->real_files.insert(fd);
  return fd;
}

namespace {

void Require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

uint64_t ReadDecodeControlNotifications(Driver &driver) {
  uint64_t notifications = 0;
  const ssize_t bytes = read(driver.decode_control_fd, &notifications,
                             sizeof(notifications));
  if (bytes == -1 && errno == EAGAIN)
    return 0;
  Require(bytes == sizeof(notifications), "read decoder control eventfd");
  return notifications;
}

void UnusedVppProbeDoesNotNotifyDecoder() {
  // Kodi 22.0rc1 CVppPostproc::PreInit/Dispose call order, including its
  // unconditional DI query and individual unused surface
  // destruction. Private CPU storage only; no DRM or decoder device opens.
  Driver driver{-1};
  VADriverContext context = {};
  context.pDriverData = &driver;
  const auto decoder = std::make_shared<DecodeContext>();
  driver.contexts[100] = decoder;
  VAConfigID config = VA_INVALID_ID;
  VAContextID processor = VA_INVALID_ID;
  std::array<VASurfaceID, 8> surfaces;
  VASurfaceAttrib attribute = {};
  attribute.type = VASurfaceAttribPixelFormat;
  attribute.flags = VA_SURFACE_ATTRIB_SETTABLE;
  attribute.value.type = VAGenericValueTypeInteger;
  attribute.value.value.i = VA_FOURCC_NV12;
  Require(CreateConfig(&context, VAProfileNone, VAEntrypointVideoProc,
                       nullptr, 0, &config) == VA_STATUS_SUCCESS &&
              CreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 1920, 1088,
                              surfaces.data(), surfaces.size(), &attribute, 1) ==
                  VA_STATUS_SUCCESS &&
              CreateContext(&context, config, 1920, 1088, 0, surfaces.data(),
                            surfaces.size(), &processor) == VA_STATUS_SUCCESS,
          "create stock Kodi VPP probe objects");
  VAProcFilterType filters[VAProcFilterCount] = {};
  unsigned filter_count = VAProcFilterCount;
  VAProcFilterCapDeinterlacing capabilities[VAProcDeinterlacingCount] = {};
  unsigned capability_count = VAProcDeinterlacingCount;
  Require(QueryVideoProcFilters(&context, processor, filters, &filter_count) ==
                  VA_STATUS_SUCCESS && filter_count == 1 &&
              filters[0] == VAProcFilterDeinterlacing &&
              QueryVideoProcFilterCaps(&context, processor,
                                       VAProcFilterDeinterlacing, capabilities,
                                       &capability_count) ==
                  VA_STATUS_SUCCESS && capability_count == 1 &&
              capabilities[0].type == VAProcDeinterlacingBob,
          "unused probe truthfully discovers the implemented Bob filter");
  for (VASurfaceID surface : surfaces)
    Require(SyncSurface(&context, surface) == VA_STATUS_SUCCESS,
            "sync unused probe surface before disposal");
  Require(DestroyContext(&context, processor) == VA_STATUS_SUCCESS,
          "destroy unused VPP probe context");
  for (VASurfaceID surface : surfaces)
    Require(DestroySurfaces(&context, &surface, 1) == VA_STATUS_SUCCESS,
            "destroy individual unused probe surface");
  Require(DestroyConfig(&context, config) == VA_STATUS_SUCCESS &&
              driver.surfaces.empty() && driver.contexts.size() == 1 &&
              driver.contexts.at(100) == decoder && !decoder->retired,
          "probe disposal preserves unrelated decoder and releases resources");
  const uint64_t notifications = ReadDecodeControlNotifications(driver);
  printf("Unused Kodi-order VPP probe: %llu decoder control notifications\n",
         static_cast<unsigned long long>(notifications));
  Require(notifications == 0,
          "unused VPP probe must not wake the unrelated decoder");
}

void DefaultVppCapabilitiesRemainAvailable() {
  Driver driver{-1};
  VADriverContext context = {};
  context.pDriverData = &driver;
  VAProfile profiles[16] = {};
  int count = 0;
  Require(QueryConfigProfiles(&context, profiles, &count) == VA_STATUS_SUCCESS &&
              count == 11 &&
              std::find(profiles, profiles + count, VAProfileNone) != profiles + count,
          "default profile list still advertises video processing");
  const int profile_count = count;
  VAEntrypoint entrypoint = VAEntrypointVLD;
  count = 99;
  Require(QueryConfigEntrypoints(&context, VAProfileNone, &entrypoint, &count) ==
                  VA_STATUS_SUCCESS && count == 1 &&
              entrypoint == VAEntrypointVideoProc,
          "default VPP entrypoint remains available");
  VAConfigAttrib attributes[] = {{VAConfigAttribRTFormat, 0xdead}};
  Require(GetConfigAttributes(&context, VAProfileNone, VAEntrypointVideoProc,
                              attributes, 1) == VA_STATUS_SUCCESS &&
              attributes[0].value == (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32),
          "default VPP keeps NV12 and ARGB render formats");
  VAConfigID config = VA_INVALID_ID;
  VASurfaceID surface = VA_INVALID_SURFACE;
  VAContextID processor = VA_INVALID_ID;
  Require(CreateConfig(&context, VAProfileNone, VAEntrypointVideoProc,
                        nullptr, 0, &config) == VA_STATUS_SUCCESS &&
              CreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 16, 16,
                              &surface, 1, nullptr, 0) == VA_STATUS_SUCCESS &&
              CreateContext(&context, config, 16, 16, 0, &surface, 1,
                             &processor) == VA_STATUS_SUCCESS,
          "ordinary VPP surface and context creation remain available");
  VAProcPipelineCaps caps = {};
  unsigned filters = 99, capabilities = 99;
  Require(QueryVideoProcPipelineCaps(&context, processor, nullptr, 0, &caps) ==
                  VA_STATUS_SUCCESS &&
              caps.num_input_pixel_formats == 1 && caps.input_pixel_format != nullptr &&
              caps.num_output_pixel_formats == 3 && caps.output_pixel_format != nullptr &&
              caps.input_pixel_format[0] == VA_FOURCC_NV12 &&
              caps.output_pixel_format[0] == VA_FOURCC_NV12 &&
              caps.output_pixel_format[1] == VA_FOURCC_ARGB &&
              QueryVideoProcFilters(&context, processor, nullptr, &filters) ==
                  VA_STATUS_SUCCESS && filters == 1 &&
              QueryVideoProcFilterCaps(&context, processor, VAProcFilterDeinterlacing,
                                       nullptr, &capabilities) ==
                  VA_STATUS_SUCCESS && capabilities == 1,
          "scaling/CSC caps remain available alongside CPU Bob");
  Require(DestroyContext(&context, processor) == VA_STATUS_SUCCESS &&
              DestroySurfaces(&context, &surface, 1) == VA_STATUS_SUCCESS &&
              DestroyConfig(&context, config) == VA_STATUS_SUCCESS,
          "default VPP resource teardown remains valid");
  for (int index = 0; index < profile_count; ++index) {
    const VAProfile profile = profiles[index];
    if (!IsDecodeProfile(profile))
      continue;
    count = 0;
    Require(QueryConfigEntrypoints(&context, profile, &entrypoint, &count) ==
                    VA_STATUS_SUCCESS && count == 1 &&
                entrypoint == VAEntrypointVLD &&
                CreateConfig(&context, profile, VAEntrypointVLD, nullptr, 0,
                             &config) == VA_STATUS_SUCCESS &&
                DestroyConfig(&context, config) == VA_STATUS_SUCCESS,
            "default decode capabilities remain unchanged");
  }
}

void ConfigAttributesMatchAdvertisedSupport() {
  Driver driver{-1};
  VADriverContext context = {};
  context.pDriverData = &driver;
  const auto ids = [&] {
    return std::array<uint64_t, 8>{{driver.next_config, driver.next_surface,
        driver.next_context, driver.next_buffer, driver.next_image,
        driver.next_vpp_sequence, driver.next_decode_identity, driver.next_trace_call}};
  };
  const auto rejects = [&](VAProfile profile, VAEntrypoint entrypoint,
                           VAConfigAttrib *attributes, int count,
                           VAStatus expected, bool null_output = false) {
    const auto previous = driver.configs;
    const auto previous_ids = ids();
    constexpr VAConfigID sentinel = 0xdecafbadU;
    VAConfigID output = sentinel;
    const VAStatus status = CreateConfig(&context, profile, entrypoint,
        attributes, count, null_output ? nullptr : &output);
    Require(status == expected, "unsupported config request returns the exact API error");
    Require(output == sentinel && ids() == previous_ids &&
                driver.configs.size() == previous.size() &&
                std::all_of(previous.begin(), previous.end(), [&](const auto &item) {
                  const auto found = driver.configs.find(item.first);
                  return found != driver.configs.end() &&
                      found->second.profile == item.second.profile &&
                      found->second.entrypoint == item.second.entrypoint &&
                      found->second.rt_format == item.second.rt_format &&
                      found->second.decode_processing == item.second.decode_processing;
                }),
            "rejected config leaves every existing config, ID and output untouched");
  };
  VAProfile profiles[32] = {};
  int profile_count = 0;
  Require(QueryConfigProfiles(&context, profiles, &profile_count) == VA_STATUS_SUCCESS &&
              profile_count > 0 && profile_count <= 32,
          "inspect the actual advertised decode and VPP profiles");
  for (int index = 0; index < profile_count; ++index) {
    const VAProfile profile = profiles[index];
    const bool decode = IsDecodeProfile(profile);
    const VAEntrypoint entrypoint = decode ? VAEntrypointVLD : VAEntrypointVideoProc;
    VAConfigAttrib advertised[] = {{VAConfigAttribRTFormat, 0},
        {VAConfigAttribDecSliceMode, 0},
        {VAConfigAttribRateControl, 0},
        {VAConfigAttribDecProcessing, 0}};
    Require(GetConfigAttributes(&context, profile, entrypoint, advertised, 4) ==
                    VA_STATUS_SUCCESS &&
                advertised[0].value == static_cast<uint32_t>(decode ? VA_RT_FORMAT_YUV420
                    : VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32) &&
                advertised[1].value == static_cast<uint32_t>(decode
                    ? VA_DEC_SLICE_MODE_NORMAL : VA_ATTRIB_NOT_SUPPORTED) &&
                advertised[2].value == VA_ATTRIB_NOT_SUPPORTED &&
                advertised[3].value == static_cast<uint32_t>(IsH264Profile(profile)
                    ? VA_DEC_PROCESSING : VA_ATTRIB_NOT_SUPPORTED),
            "config admission is checked against the actual advertised masks");
    const auto accepts = [&](VAConfigAttrib *attributes, int count) {
      VAConfigID output = VA_INVALID_ID;
      const VAConfigID next = driver.next_config;
      const size_t previous = driver.configs.size();
      Require(CreateConfig(&context, profile, entrypoint, attributes, count, &output) ==
                      VA_STATUS_SUCCESS && output == next &&
                  driver.next_config == next + 1 && driver.configs.size() == previous + 1 &&
                  driver.configs.at(output).profile == profile &&
                  driver.configs.at(output).entrypoint == entrypoint,
              "omitted/default or supported explicit config attributes remain accepted");
    };
    accepts(nullptr, 0);
    Require(!driver.configs.at(driver.next_config - 1).decode_processing,
            "ordinary omitted processing keeps the existing decode contract");
    VAConfigAttrib ordinary[] = {{VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420},
        {VAConfigAttribEncryption, 0}, {VAConfigAttribMaxPictureWidth, kMaxWidth}};
    accepts(ordinary, 3);
    VAConfigAttrib no_processing = {VAConfigAttribDecProcessing, VA_DEC_PROCESSING_NONE};
    accepts(&no_processing, 1);
    VAConfigAttrib ordinary_no_processing[] = {ordinary[0], no_processing, ordinary[1]};
    accepts(ordinary_no_processing, 3);
    for (uint32_t processing : {static_cast<uint32_t>(VA_DEC_PROCESSING), 2U,
                                VA_ATTRIB_NOT_SUPPORTED}) {
      VAConfigAttrib unsupported = {VAConfigAttribDecProcessing, processing};
      if (processing == VA_DEC_PROCESSING && IsH264Profile(profile)) {
        accepts(&unsupported, 1);
        const auto id = driver.next_config - 1;
        Require(driver.configs.at(id).decode_processing,
                "only explicit H.264 processing opts into card-scaled output");
        VAProfile queried_profile;
        VAEntrypoint queried_entrypoint;
        VAConfigAttrib selected[2] = {};
        int selected_count = 0;
        Require(QueryConfigAttributes(&context, id, &queried_profile,
                    &queried_entrypoint, selected, &selected_count) == VA_STATUS_SUCCESS &&
                    selected_count == 2 &&
                    selected[1].type == VAConfigAttribDecProcessing &&
                    selected[1].value == VA_DEC_PROCESSING,
                "config query preserves explicit integrated processing");
      } else
        rejects(profile, entrypoint, &unsupported, 1, VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
      VAConfigAttrib enabled_then_none[] = {ordinary[0], unsupported, no_processing};
      rejects(profile, entrypoint, enabled_then_none, 3, VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
      VAConfigAttrib none_then_enabled[] = {no_processing, ordinary[0], unsupported};
      rejects(profile, entrypoint, none_then_enabled, 3, VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
    }
    const auto previous_ids = ids();
    const size_t previous_configs = driver.configs.size();
    VAConfigID untouched = 0xdecafbadU;
    VAConfigAttrib processing = {VAConfigAttribDecProcessing, VA_DEC_PROCESSING};
    allocation_fault::fail_next = true;
    const VAStatus processing_status = CreateConfig(&context, profile, entrypoint,
        &processing, 1, &untouched);
    const bool allocation_unreached = allocation_fault::fail_next;
    allocation_fault::fail_next = false;
    Require(processing_status == (IsH264Profile(profile)
                    ? VA_STATUS_ERROR_ALLOCATION_FAILED
                    : VA_STATUS_ERROR_ATTR_NOT_SUPPORTED) &&
                allocation_unreached == !IsH264Profile(profile) && untouched == 0xdecafbadU &&
                ids() == previous_ids && driver.configs.size() == previous_configs,
            "processing admission and allocation failure preserve existing configs");
    if (decode) {
      VAConfigAttrib normal = {VAConfigAttribDecSliceMode, VA_DEC_SLICE_MODE_NORMAL};
      accepts(&normal, 1);
    } else {
      for (uint32_t format : {static_cast<uint32_t>(VA_RT_FORMAT_RGB32), advertised[0].value}) {
        VAConfigAttrib rgb = {VAConfigAttribRTFormat, format};
        accepts(&rgb, 1);
      }
    }
    for (uint32_t format : {0U, static_cast<uint32_t>(VA_RT_FORMAT_YUV422),
                           advertised[0].value | VA_RT_FORMAT_YUV422,
                           VA_ATTRIB_NOT_SUPPORTED}) {
      VAConfigAttrib bad = {VAConfigAttribRTFormat, format};
      rejects(profile, entrypoint, &bad, 1, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
      VAConfigAttrib mixed[] = {ordinary[0], bad, ordinary[1]};
      rejects(profile, entrypoint, mixed, 3, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    }
    if (decode) {
      VAConfigAttrib rgb = {VAConfigAttribRTFormat, VA_RT_FORMAT_RGB32};
      rejects(profile, entrypoint, &rgb, 1, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    }
    for (uint32_t mode : {0U, static_cast<uint32_t>(VA_DEC_SLICE_MODE_BASE),
                         static_cast<uint32_t>(VA_DEC_SLICE_MODE_NORMAL | VA_DEC_SLICE_MODE_BASE),
                         VA_ATTRIB_NOT_SUPPORTED}) {
      VAConfigAttrib bad = {VAConfigAttribDecSliceMode, mode};
      rejects(profile, entrypoint, &bad, 1, VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
      VAConfigAttrib mixed[] = {ordinary[0], bad, ordinary[1]};
      rejects(profile, entrypoint, mixed, 3, VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
    }
    if (!decode) {
      VAConfigAttrib normal = {VAConfigAttribDecSliceMode, VA_DEC_SLICE_MODE_NORMAL};
      rejects(profile, entrypoint, &normal, 1, VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
    }
    rejects(profile, entrypoint, ordinary, -1, VA_STATUS_ERROR_INVALID_PARAMETER);
    rejects(profile, entrypoint, nullptr, 1, VA_STATUS_ERROR_INVALID_PARAMETER);
    rejects(profile, entrypoint, ordinary, 1, VA_STATUS_ERROR_INVALID_PARAMETER, true);
  }
  Require(GetConfigAttributes(&context, VAProfileH264Main, VAEntrypointVLD,
                              nullptr, 0) == VA_STATUS_SUCCESS,
          "empty config query accepts an unused null array");
  Require(GetConfigAttributes(&context, VAProfileH264Main, VAEntrypointVLD,
                              nullptr, 1) == VA_STATUS_ERROR_INVALID_PARAMETER &&
              GetConfigAttributes(&context, VAProfileH264Main, VAEntrypointVLD,
                                  nullptr, -1) == VA_STATUS_ERROR_INVALID_PARAMETER,
          "malformed config queries reject before accessing their attribute array");
  const VAConfigID existing = driver.configs.begin()->first;
  VAProfile profile = VAProfileNone;
  VAEntrypoint entrypoint = VAEntrypointVLD;
  int count = -1;
  Require(QueryConfigAttributes(&context, existing, &profile, &entrypoint, nullptr, &count) ==
                  VA_STATUS_SUCCESS && count > 0,
          "config query permits a count-only request");
  Require(QueryConfigAttributes(&context, existing, nullptr, &entrypoint, nullptr, &count) ==
                  VA_STATUS_ERROR_INVALID_PARAMETER &&
              QueryConfigAttributes(&context, existing, &profile, nullptr, nullptr, &count) ==
                  VA_STATUS_ERROR_INVALID_PARAMETER &&
              QueryConfigAttributes(&context, existing, &profile, &entrypoint, nullptr, nullptr) ==
                  VA_STATUS_ERROR_INVALID_PARAMETER,
          "config query validates every required output before dereference");
  const auto previous_ids = ids();
  const size_t previous_count = driver.configs.size();
  VAConfigID output = 0xdecafbadU;
  allocation_fault::fail_next = true;
  const VAStatus allocation_status = CreateConfig(&context, VAProfileH264Main,
      VAEntrypointVLD, nullptr, 0, &output);
  allocation_fault::fail_next = false;
  Require(allocation_status == VA_STATUS_ERROR_ALLOCATION_FAILED &&
              output == 0xdecafbadU && ids() == previous_ids &&
              driver.configs.size() == previous_count,
          "config allocation failure preserves the caller output and every ID");
  while (!driver.configs.empty())
    Require(DestroyConfig(&context, driver.configs.begin()->first) == VA_STATUS_SUCCESS,
            "accepted configs remain normally destroyable after negative requests");
}

void UnsupportedDisplayAttributesAreNotSuccessful() {
  Driver driver{-1};
  VADriverContext context = {};
  context.pDriverData = &driver;
  VADisplayAttribute attribute = {};
  attribute.type = VADisplayAttribBrightness;
  attribute.min_value = -100;
  attribute.max_value = 100;
  attribute.value = 17;
  attribute.flags = VA_DISPLAY_ATTRIB_GETTABLE | VA_DISPLAY_ATTRIB_SETTABLE;
  const auto original = attribute;
  int count = -1;
  Require(QueryDisplayAttributes(&context, &attribute, &count) == VA_STATUS_SUCCESS &&
              count == 0 && memcmp(&attribute, &original, sizeof(attribute)) == 0,
          "no display attributes are advertised and the unused output array stays untouched");
  Require(QueryDisplayAttributes(&context, nullptr, &count) == VA_STATUS_SUCCESS && count == 0,
          "empty display capability query accepts a null unused array");
  for (auto operation : {GetDisplayAttributes, SetDisplayAttributes}) {
    Require(operation(&context, nullptr, 0) == VA_STATUS_SUCCESS,
            "zero-count display operations remain successful no-ops");
    for (VADisplayAttribType type : {VADisplayAttribBrightness, VADisplayAttribRotation,
                                    VADisplayAttribHue}) {
      attribute.type = type;
      const auto before = attribute;
      Require(operation(&context, &attribute, 1) == VA_STATUS_ERROR_ATTR_NOT_SUPPORTED,
              "unadvertised display controls return ATTR_NOT_SUPPORTED, never silent success");
      Require(memcmp(&attribute, &before, sizeof(attribute)) == 0 &&
                  driver.configs.empty() && driver.next_config == 1,
              "unsupported display get/set preserves caller bytes and driver resources");
    }
    Require(operation(&context, nullptr, 1) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                operation(&context, &attribute, -1) == VA_STATUS_ERROR_INVALID_PARAMETER,
            "malformed display get/set rejects before processing attributes");
  }
  Require(QueryDisplayAttributes(&context, &attribute, nullptr) == VA_STATUS_ERROR_INVALID_PARAMETER,
          "display capability query requires a count output");
}

void EmptySubpictureFormatsValidateCount() {
  Driver driver{-1};
  VADriverContext context = {};
  context.pDriverData = &driver;
  VAImageFormat format;
  memset(&format, 0xa5, sizeof(format));
  const auto original = format;
  unsigned int flags = 0x5a5a5a5aU;
  const unsigned int original_flags = flags;
  const unsigned int initial_opens = device_open_attempts;
  for (bool with_format : {false, true}) {
    for (bool with_flags : {false, true}) {
      unsigned int count = UINT_MAX;
      Require(QuerySubpictureFormats(&context, with_format ? &format : nullptr,
                                      with_flags ? &flags : nullptr, &count) ==
                  VA_STATUS_SUCCESS && count == 0,
              "empty subpicture query permits unused format and flag arrays");
      Require(QuerySubpictureFormats(&context, with_format ? &format : nullptr,
                                      with_flags ? &flags : nullptr, nullptr) ==
                  VA_STATUS_ERROR_INVALID_PARAMETER,
              "subpicture query rejects a missing count output");
      Require(memcmp(&format, &original, sizeof(format)) == 0 &&
                  flags == original_flags && device_open_attempts == initial_opens &&
                  driver.surfaces.empty() && driver.configs.empty(),
              "subpicture probes preserve caller arrays and acquire no resources");
    }
  }
}

void UnsupportedSurfaceErrorsDoNotInventDetails() {
  Driver driver{-1};
  VADriverContext context = {};
  context.pDriverData = &driver;
  const unsigned int initial_opens = device_open_attempts;
  auto ready = std::make_shared<Surface>();
  auto failed = std::make_shared<Surface>();
  auto destroyed = std::make_shared<Surface>();
  ready->ready = true;
  failed->failed = true;
  destroyed->destroyed = true;
  driver.surfaces[1] = ready;
  driver.surfaces[2] = failed;
  driver.surfaces[3] = destroyed;
  for (VASurfaceID id : {VASurfaceID(1), VASurfaceID(2), VASurfaceID(3),
                          VA_INVALID_SURFACE}) {
    for (VAStatus status : {VA_STATUS_SUCCESS, VA_STATUS_ERROR_DECODING_ERROR,
                            VA_STATUS_ERROR_HW_BUSY,
                            static_cast<VAStatus>(VA_STATUS_ERROR_UNKNOWN)}) {
      int sentinel = 17;
      void *details = &sentinel;
      Require(QuerySurfaceError(&context, id, status, &details) ==
                  VA_STATUS_ERROR_UNIMPLEMENTED && details == nullptr && sentinel == 17 &&
                  QuerySurfaceError(&context, id, status, nullptr) ==
                      VA_STATUS_ERROR_UNIMPLEMENTED,
              "unsupported error queries return no invented macroblock or reset details");
      Require(driver.surfaces.size() == 3 && driver.surfaces.at(1) == ready &&
                  driver.surfaces.at(2) == failed && driver.surfaces.at(3) == destroyed &&
                  ready->ready && !ready->failed && !ready->destroyed &&
                  !failed->ready && failed->failed && !failed->destroyed &&
                  !destroyed->ready && !destroyed->failed && destroyed->destroyed &&
                  device_open_attempts == initial_opens && driver.pending_vpp.empty(),
              "unsupported diagnostics leave surface state and lifetime unchanged");
    }
  }
  driver.surfaces.erase(3);
  void *details = destroyed.get();
  Require(QuerySurfaceError(&context, 3, VA_STATUS_ERROR_DECODING_ERROR, &details) ==
              VA_STATUS_ERROR_UNIMPLEMENTED && details == nullptr && destroyed->destroyed,
          "removed IDs do not expose retained surface storage as diagnostic data");
}

void ConfigRejectsUnsupportedProtectionTransactionally() {
  for (bool vpp : {false, true}) {
    Driver driver{-1};
    VADriverContext context = {};
    context.pDriverData = &driver;
    const VAProfile profile = vpp ? VAProfileNone : VAProfileH264High;
    const VAEntrypoint entrypoint = vpp ? VAEntrypointVideoProc : VAEntrypointVLD;
    const auto accepts = [&](VAConfigAttrib *attributes, int count) {
      const VAConfigID expected = driver.next_config;
      const size_t previous_count = driver.configs.size();
      VAConfigID output = VA_INVALID_ID;
      Require(CreateConfig(&context, profile, entrypoint, attributes, count,
                            &output) == VA_STATUS_SUCCESS &&
                  output == expected && driver.next_config == expected + 1 &&
                  driver.configs.size() == previous_count + 1 &&
                  driver.configs.at(output).profile == profile &&
                  driver.configs.at(output).entrypoint == entrypoint,
              "ordinary or explicit unencrypted configurations remain available");
    };
    const auto rejects = [&](VAProfile requested_profile,
                             VAEntrypoint requested_entrypoint,
                             VAConfigAttrib *attributes, int count,
                             VAStatus expected, bool null_output = false) {
      const auto previous = driver.configs;
      const VAConfigID next = driver.next_config;
      constexpr VAConfigID sentinel = 0xdecafbadU;
      VAConfigID output = sentinel;
      const VAStatus status = CreateConfig(&context, requested_profile,
          requested_entrypoint, attributes, count, null_output ? nullptr : &output);
      Require(status == expected && output == sentinel && driver.next_config == next &&
                  driver.configs.size() == previous.size() &&
                  std::all_of(previous.begin(), previous.end(), [&](const auto &item) {
                    const auto found = driver.configs.find(item.first);
                    return found != driver.configs.end() &&
                        found->second.profile == item.second.profile &&
                        found->second.entrypoint == item.second.entrypoint;
                  }),
              "rejected config preserves every config, the next ID and output sentinel");
    };
    accepts(nullptr, 0);
    VAConfigAttrib clear = {VAConfigAttribEncryption, 0};
    accepts(&clear, 1);
    VAConfigAttrib ordinary[] = {
        {VAConfigAttribRTFormat, static_cast<uint32_t>(
             vpp ? VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32 : VA_RT_FORMAT_YUV420)},
        {VAConfigAttribEncryption, 0},
        {VAConfigAttribMaxPictureWidth, kMaxWidth}};
    accepts(ordinary, sizeof(ordinary) / sizeof(ordinary[0]));
    for (uint32_t value : {1U, UINT32_MAX}) {
      VAConfigAttrib encrypted = {VAConfigAttribEncryption, value};
      VAConfigAttrib query = encrypted;
      Require(GetConfigAttributes(&context, profile, entrypoint, &query, 1) ==
                  VA_STATUS_SUCCESS && query.value == VA_ATTRIB_NOT_SUPPORTED,
              "config rejection agrees with unsupported encryption query");
      rejects(profile, entrypoint, &encrypted, 1, VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
      VAConfigAttrib mixed[] = {ordinary[0], clear, encrypted, ordinary[2], clear};
      rejects(profile, entrypoint, mixed, sizeof(mixed) / sizeof(mixed[0]),
              VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
    }
#if VA_CHECK_VERSION(1, 11, 0)
    for (VAConfigAttribType type : {
             VAConfigAttribProtectedContentCipherAlgorithm,
             VAConfigAttribProtectedContentCipherBlockSize,
             VAConfigAttribProtectedContentCipherMode,
             VAConfigAttribProtectedContentCipherSampleType,
             VAConfigAttribProtectedContentUsage}) {
      for (uint32_t value : {0U, 1U}) {
        VAConfigAttrib protected_content = {type, value};
        rejects(profile, entrypoint, &protected_content, 1,
                VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
      }
    }
#endif
    rejects(profile, entrypoint, &clear, -1, VA_STATUS_ERROR_INVALID_PARAMETER);
    rejects(profile, entrypoint, nullptr, 1, VA_STATUS_ERROR_INVALID_PARAMETER);
    rejects(profile, entrypoint, nullptr, INT_MAX, VA_STATUS_ERROR_INVALID_PARAMETER);
    rejects(profile, entrypoint, &clear, 1, VA_STATUS_ERROR_INVALID_PARAMETER, true);
    rejects(static_cast<VAProfile>(INT_MAX), entrypoint, nullptr, -1,
            VA_STATUS_ERROR_UNSUPPORTED_PROFILE, true);
    rejects(profile, VAEntrypointEncSlice, nullptr, -1,
            VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT, true);
    while (!driver.configs.empty())
      Require(DestroyConfig(&context, driver.configs.begin()->first) == VA_STATUS_SUCCESS,
              "ordinary config cleanup remains available after rejection");
  }
}

void ProtectedBufferCreationIsTransactional() {
  Driver driver{-1};
  VADriverContext context = {};
  context.pDriverData = &driver;
  driver.contexts[1] = std::make_shared<DecodeContext>();
  driver.contexts[2] = std::make_shared<DecodeContext>();
  driver.contexts.at(2)->video_process = true;
  const uint8_t bytes[] = {0x65, 0x88};
  const VABufferType protected_types[] = {
      VAProtectedSliceDataBufferType,
#if VA_CHECK_VERSION(1, 11, 0)
      VAProtectedSessionExecuteBufferType, VAEncryptionParameterBufferType,
#endif
  };
  for (VAContextID owner : {1U, 2U, VA_INVALID_ID}) {
    for (VABufferType type : {VASliceDataBufferType, VAEncCodedBufferType}) {
      VABufferID ordinary = VA_INVALID_ID;
      Require(CreateBuffer(&context, owner, type, sizeof(bytes), 1,
                            const_cast<uint8_t *>(bytes), &ordinary) == VA_STATUS_SUCCESS &&
                  driver.buffers.at(ordinary).context == owner &&
                  driver.buffers.at(ordinary).type == type &&
                  driver.buffers.at(ordinary).data == std::vector<uint8_t>(bytes, bytes + sizeof(bytes)),
              "ordinary and unrelated buffer creation remain unchanged");
    }
    for (VABufferType type : protected_types) {
      for (bool allocation_failure : {false, true}) {
        const auto previous = driver.buffers;
        const VABufferID next = driver.next_buffer;
        constexpr VABufferID sentinel = 0xdecafbadU;
        VABufferID output = sentinel;
        allocation_fault::fail_next = allocation_failure;
        const VAStatus status = CreateBuffer(&context, owner, type, sizeof(bytes), 1,
                                             const_cast<uint8_t *>(bytes), &output);
        const bool allocation_untouched = allocation_fault::fail_next == allocation_failure;
        allocation_fault::fail_next = false;
        Require(status == VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE && allocation_untouched &&
                    output == sentinel && driver.next_buffer == next &&
                    driver.buffers.size() == previous.size() &&
                    std::all_of(previous.begin(), previous.end(), [&](const auto &item) {
                      const auto found = driver.buffers.find(item.first);
                      return found != driver.buffers.end() &&
                          found->second.context == item.second.context &&
                          found->second.type == item.second.type &&
                          found->second.data == item.second.data;
                    }),
                "protected buffer rejection precedes allocation and preserves all IDs/data");
      }
    }
  }
}

struct PipelineCapsFixture {
  Driver driver{-1};
  VADriverContext context = {};
  VAConfigID config = VA_INVALID_ID;
  VAContextID processor = VA_INVALID_ID;
  PipelineCapsFixture() {
    context.pDriverData = &driver;
    Require(CreateConfig(&context, VAProfileNone, VAEntrypointVideoProc,
                          nullptr, 0, &config) == VA_STATUS_SUCCESS &&
                CreateContext(&context, config, 0, 0, VA_PROGRESSIVE, nullptr, 0,
                               &processor) == VA_STATUS_SUCCESS,
            "create VPP context for output-capability queries");
  }
  ~PipelineCapsFixture() {
    DestroyContext(&context, processor);
    DestroyConfig(&context, config);
  }
  void Query(VAProcPipelineCaps *caps) {
    Require(QueryVideoProcPipelineCaps(&context, processor, nullptr, 0, caps) ==
                VA_STATUS_SUCCESS, "query borrowed VPP pipeline capabilities");
    Require(caps->num_input_pixel_formats == 1 && caps->input_pixel_format != nullptr &&
                caps->num_output_pixel_formats == 3 && caps->output_pixel_format != nullptr &&
                caps->input_pixel_format[0] == VA_FOURCC_NV12 &&
                caps->output_pixel_format[0] == VA_FOURCC_NV12 &&
                caps->output_pixel_format[1] == VA_FOURCC_ARGB &&
                caps->rotation_flags == (1U << VA_ROTATION_NONE) &&
                caps->filter_flags == (VA_FILTER_SCALING_FAST |
                                      VA_FILTER_INTERPOLATION_NEAREST_NEIGHBOR) &&
                caps->min_input_width == 16 && caps->min_input_height == 16 &&
                caps->max_input_width == kMaxWidth && caps->max_input_height == kMaxHeight &&
                caps->min_output_width == 16 && caps->min_output_height == 16 &&
                caps->max_output_width == kMaxWidth && caps->max_output_height == kMaxHeight,
            "returned arrays retain supported NV12/ARGB formats and pipeline limits");
  }
};

void PipelineCapsIgnoreIncomingPixelFormatStorage() {
  PipelineCapsFixture fixture;
  VAProcPipelineCaps caps;
  memset(&caps, 0xa5, sizeof(caps));
  fixture.Query(&caps);

  const long page_size = sysconf(_SC_PAGESIZE);
  Require(page_size > 0, "discover guard-page size");
  struct Guard {
    size_t size;
    void *address;
    ~Guard() { if (address != MAP_FAILED) munmap(address, size); }
  } guard{static_cast<size_t>(page_size),
          mmap(nullptr, static_cast<size_t>(page_size), PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)};
  Require(guard.address != MAP_FAILED, "allocate an inaccessible format-pointer guard");
  uint32_t *unmapped = static_cast<uint32_t *>(guard.address);
  for (unsigned phase = 0; phase < 2; ++phase) {
    caps = {};
    caps.input_pixel_format = unmapped;
    caps.output_pixel_format = unmapped;
    caps.num_input_pixel_formats = caps.num_output_pixel_formats = UINT32_MAX;
    fixture.Query(&caps);  // Neither PROT_NONE nor unmapped memory may be touched.
    if (phase == 0) {
      Require(munmap(guard.address, guard.size) == 0, "unmap the guarded input address");
      guard.address = MAP_FAILED;
    }
  }
  for (uint32_t count : {0U, 1U, UINT32_MAX}) {
    std::array<uint32_t, 4> input = {{11, 22, 33, 44}};
    std::array<uint32_t, 4> output = {{55, 66, 77, 88}};
    const auto original_input = input, original_output = output;
    caps = {};
    caps.input_pixel_format = input.data() + 1;
    caps.output_pixel_format = output.data() + 1;
    caps.num_input_pixel_formats = caps.num_output_pixel_formats = count;
    fixture.Query(&caps);
    Require(input == original_input && output == original_output &&
                caps.input_pixel_format != input.data() + 1 &&
                caps.output_pixel_format != output.data() + 1,
            "zero/tiny/huge incoming counts cannot write caller canaries or limit output arrays");
  }
}

void PipelineCapsBorrowedLifetimeAndErrors() {
  PipelineCapsFixture first, second;
  VAProcPipelineCaps original = {}, isolated = {}, repeated = {};
  first.Query(&original);
  second.Query(&isolated);
  Require(original.input_pixel_format != isolated.input_pixel_format &&
              original.output_pixel_format != isolated.output_pixel_format,
          "separate live drivers own independent capability arrays");
  for (unsigned index = 0; index < 3; ++index) {
    memset(&repeated, 0x5a, sizeof(repeated));
    first.Query(&repeated);
    Require(memcmp(&original, &repeated, sizeof(original)) == 0,
            "repeated queries preserve borrowed pointers and all returned metadata");
  }
  Require(DestroyContext(&first.context, first.processor) == VA_STATUS_SUCCESS &&
              original.input_pixel_format[0] == VA_FOURCC_NV12 &&
              original.output_pixel_format[1] == VA_FOURCC_ARGB &&
              CreateContext(&first.context, first.config, 0, 0, VA_PROGRESSIVE,
                             nullptr, 0, &first.processor) == VA_STATUS_SUCCESS,
          "borrowed format arrays survive context retirement within their owning driver");
  first.Query(&repeated);
  Require(memcmp(&original, &repeated, sizeof(original)) == 0,
          "a replacement VPP context sees the same valid driver-owned metadata");
  Require(QueryVideoProcPipelineCaps(&first.context, first.processor, nullptr, 0,
                                     nullptr) == VA_STATUS_ERROR_INVALID_PARAMETER,
          "null pipeline capability output retains its invalid-parameter error");
  std::array<unsigned char, sizeof(VAProcPipelineCaps)> poisoned;
  poisoned.fill(0xa5);
  memcpy(&repeated, poisoned.data(), poisoned.size());
  Require(QueryVideoProcPipelineCaps(&first.context, VA_INVALID_ID, nullptr, 0,
                                     &repeated) == VA_STATUS_ERROR_INVALID_CONTEXT &&
              memcmp(&repeated, poisoned.data(), poisoned.size()) == 0 &&
              QueryVideoProcPipelineCaps(&first.context, first.processor, nullptr, 1,
                                         &repeated) == VA_STATUS_ERROR_INVALID_PARAMETER &&
              memcmp(&repeated, poisoned.data(), poisoned.size()) == 0,
          "invalid context or missing filter array rejects before touching output storage");
}

void NativeNodeCacheOwnsNoFiles() {
  NativeNodeMock mock;
  MockNativeNodeScope scope(mock);
  std::vector<int> files;
  {
    PrimaryDrmNodeCache cache;
    Require(cache.Open(-1) < 0 && mock.queries == 0 && mock.opens == 0,
            "invalid render fd cannot query or open a node");
    for (unsigned index = 0; index < 8; ++index) {
      const int fd = cache.Open(mock.render_fd);
      Require(fd >= 0 && std::find(files.begin(), files.end(), fd) == files.end(),
              "every cache hit returns a fresh caller-owned DRM file");
      files.push_back(fd);
    }
    Require(mock.queries == 1 && mock.frees == 1 && mock.opens == 8 &&
                mock.render_stats == 8 && mock.primary_stats == 8,
            "stable render and primary metadata avoid repeated libdrm discovery");
  }
  Require(mock.closes == 0 && mock.files.size() == 8,
          "destroying the metadata cache does not close or retain caller files");
  for (int fd : files)
    Require(close(fd) == 0, "each caller closes its own node file");
  Require(mock.files.empty() && !mock.device_owned && mock.invalid == 0,
          "metadata cache has no hidden descriptor or device-object ownership");
}

void NativeNodeCacheInvalidatesChangedIdentities() {
  for (bool render_changed : {false, true}) {
    for (unsigned field = 0; field < 3; ++field) {
      NativeNodeMock mock;
      MockNativeNodeScope scope(mock);
      PrimaryDrmNodeCache cache;
      const int first = cache.Open(mock.render_fd);
      struct stat &identity = render_changed ? mock.render : mock.primary;
      if (field == 0) ++identity.st_dev;
      else if (field == 1) ++identity.st_ino;
      else ++identity.st_rdev;
      const int changed = cache.Open(mock.render_fd);
      const int stable = cache.Open(mock.render_fd);
      Require(first >= 0 && changed >= 0 && stable >= 0 &&
                  first != changed && changed != stable &&
                  mock.queries == 2 && mock.frees == 2 &&
                  mock.opens == (render_changed ? 3U : 4U) &&
                  mock.closes == (render_changed ? 0U : 1U) &&
                  mock.files.size() == 3,
              "changed dev/inode/rdev invalidates and re-discovers the node");
      for (int fd : {first, changed, stable}) close(fd);
      Require(mock.files.empty() && !mock.device_owned && mock.invalid == 0,
              "identity replacement closes only its rejected candidate file");
    }
  }
}

void NativeNodeCacheFailuresRetryWithoutLeases() {
  for (unsigned mode = 0; mode < 6; ++mode) {
    NativeNodeMock mock;
    MockNativeNodeScope scope(mock);
    PrimaryDrmNodeCache cache;
    if (mode == 0) mock.fail_queries = 1;
    if (mode == 1) mock.no_primary = true;
    if (mode == 2) mock.null_device = true;
    if (mode == 3) mock.fail_opens = 1;
    if (mode == 4) mock.fail_primary_stats = 1;
    if (mode == 5) mock.fail_render_stats = 1;
    const int first = cache.Open(mock.render_fd);
    Require((first >= 0) == (mode >= 4) && cache.path.empty(),
            "lookup/open failure retries; failed identity never caches a valid file");
    mock.no_primary = mock.null_device = false;
    const int retry = cache.Open(mock.render_fd);
    const int warm = cache.Open(mock.render_fd);
    Require(retry >= 0 && warm >= 0 && retry != warm && mock.queries == 2 &&
                !cache.path.empty(),
            "a later successful lookup recovers and can cache metadata");
    if (first >= 0) close(first);
    close(retry); close(warm);
    Require(mock.files.empty() && !mock.device_owned && mock.invalid == 0,
            "failed lookup/stat does not leak or steal an owned file");
  }
  for (bool fail_stat : {false, true}) {
    NativeNodeMock mock;
    MockNativeNodeScope scope(mock);
    PrimaryDrmNodeCache cache;
    const int first = cache.Open(mock.render_fd);
    if (fail_stat) mock.fail_primary_stats = 1;
    else mock.fail_opens = 1;
    const int recovered = cache.Open(mock.render_fd);
    const int warm = cache.Open(mock.render_fd);
    Require(first >= 0 && recovered >= 0 && warm >= 0 &&
                mock.queries == 2 && mock.opens == 4 &&
                mock.closes == (fail_stat ? 1U : 0U) && mock.files.size() == 3,
            "failed warm open/stat invalidates and retries discovery immediately");
    for (int fd : {first, recovered, warm}) close(fd);
    Require(mock.files.empty() && !mock.device_owned && mock.invalid == 0,
            "warm failure leaves no cached file lease");
  }
  {
    NativeNodeMock mock;
    MockNativeNodeScope scope(mock);
    PrimaryDrmNodeCache cache;
    const int first = cache.Open(mock.render_fd);
    mock.fail_render_stats = 1;
    const int unverified = cache.Open(mock.render_fd);
    Require(unverified >= 0 && mock.queries == 2 && cache.path.empty(),
            "failed warm render identity forces discovery without caching its result");
    const int retry = cache.Open(mock.render_fd);
    const int warm = cache.Open(mock.render_fd);
    Require(first >= 0 && retry >= 0 && warm >= 0 && mock.queries == 3,
            "render-stat recovery revalidates before restoring the metadata hit path");
    for (int fd : {first, unverified, retry, warm}) close(fd);
    Require(mock.files.empty() && !mock.device_owned && mock.invalid == 0,
            "unverified render identity does not retain any primary file");
  }
}

void NativeSurfaceFilesRetainOriginalLifetime() {
  Driver driver{-1};
  VADriverContext context = {};
  context.pDriverData = &driver;
  driver.drm_fd = NativeNodeMock::render_fd;
  NativeNodeMock mock;
  MockNativeNodeScope scope(mock);
  struct CleanupUnderMock {
    Driver &driver;
    ~CleanupUnderMock() {
      driver.contexts.clear();
      driver.surfaces.clear();
    }
  } cleanup{driver};
  for (unsigned cycle = 0; cycle < 2; ++cycle) {
    VAConfigID config = VA_INVALID_ID;
    VAContextID processor = VA_INVALID_ID;
    VASurfaceID surfaces[8] = {};
    Require(CreateConfig(&context, VAProfileNone, VAEntrypointVideoProc, nullptr, 0,
                          &config) == VA_STATUS_SUCCESS &&
                CreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 1920, 1080,
                                surfaces, 8, nullptr, 0) == VA_STATUS_SUCCESS &&
                CreateContext(&context, config, 1920, 1080, 0, surfaces, 8,
                               &processor) == VA_STATUS_SUCCESS,
            "default full-HD VPP probe allocates its ordinary eight surfaces");
    std::unordered_set<int> primary_files;
    for (VASurfaceID id : surfaces) {
      const auto surface = driver.surfaces.at(id);
      Require(primary_files.insert(surface->dumb_drm_fd).second &&
                  surface->planes[0] != nullptr && surface->planes[1] != nullptr &&
                  surface->object_maps.size() == 1 && surface->object_fds.size() == 1 &&
                  surface->pitch[0] == 1920,
              "every surface still owns a distinct eagerly allocated and mapped file");
    }
    unsigned capabilities = 99;
    Require(QueryVideoProcFilterCaps(&context, processor, VAProcFilterNoiseReduction,
                                     nullptr, &capabilities) ==
                    VA_STATUS_ERROR_UNSUPPORTED_FILTER && capabilities == 0 &&
                mock.queries == 1 && mock.frees == 1 && mock.files.size() == 16 &&
                mock.creates == 8 * (cycle + 1) && mock.maps == 8 * (cycle + 1),
            "unsupported filter discovery preserves ordinary surface allocation lifetimes");
    Require(DestroyContext(&context, processor) == VA_STATUS_SUCCESS &&
                DestroySurfaces(&context, surfaces, 7) == VA_STATUS_SUCCESS &&
                mock.files.size() == 2 && mock.dumbs.size() == 1 &&
                mock.mappings.size() == 1,
            "destroying earlier surfaces preserves the last surface's own DRM file");
    Require(DestroySurfaces(&context, surfaces + 7, 1) == VA_STATUS_SUCCESS &&
                DestroyConfig(&context, config) == VA_STATUS_SUCCESS &&
                mock.files.empty() && mock.dumbs.empty() && mock.mappings.empty(),
            "the last surface closes its own file even while metadata stays cached");
  }
  {
    Surface surface;
    Require(surface.AllocateInternal(nullptr, mock.render_fd, 64, 32,
                                      VA_FOURCC_NV12, false,
                                      &driver.primary_node_cache),
            "allocate a surface before a failed DUMB destroy");
    mock.fail_destroy = true;
  }
  Require(mock.files.empty() && mock.dumbs.empty() && mock.mappings.empty() &&
              mock.queries == 1 && mock.creates == 17 && mock.destroys == 17 &&
              mock.maps == 17 && mock.unmaps == 17 && mock.invalid == 0 &&
              driver.surfaces.empty() && driver.contexts.empty(),
          "even failed DESTROY_DUMB retains the original last-file-close cleanup");
}

void NativeDumbBookkeepingFailure(unsigned allocation) {
  Driver driver{-1};
  VADriverContext context = {};
  context.pDriverData = &driver;
  driver.drm_fd = NativeNodeMock::render_fd;
  NativeNodeMock mock;
  MockNativeNodeScope scope(mock);
  struct CleanupUnderMock {
    Driver &driver;
    ~CleanupUnderMock() { driver.surfaces.clear(); }
  } cleanup{driver};
  // The first surface succeeds. Fail one of the next surface's three
  // metadata allocations only after its private mock mmap has completed.
  mock.fail_bookkeeping_map = 2;
  mock.fail_bookkeeping_allocation = allocation;
  VASurfaceID surfaces[2] = {VA_INVALID_SURFACE, VA_INVALID_SURFACE};
  VAStatus status = VA_STATUS_SUCCESS;
  bool escaped = false;
  try {
    status = CreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 64, 32,
                            surfaces, 2, nullptr, 0);
  } catch (const std::bad_alloc &) {
    escaped = true;
  }
  const bool failed = !allocation_fault::fail_next;
  const size_t failed_size = allocation_fault::failed_size;
  allocation_fault::fail_next = false;
  allocation_fault::only_size = 0;
  allocation_fault::skip_matches = 0;
  Require(failed && failed_size == (allocation == 3 ? sizeof(int) : sizeof(void *)),
          "the selected mapped-surface metadata allocation actually fails");
  Require(!escaped && status == VA_STATUS_ERROR_ALLOCATION_FAILED,
          "DUMB bookkeeping OOM returns an allocation error without escaping");
  Require(surfaces[1] == VA_INVALID_SURFACE && driver.surfaces.empty() &&
              driver.backing_owners.empty(),
          "DUMB bookkeeping OOM publishes no failed surface and rolls back earlier surfaces");
  Require(mock.creates == 2 && mock.destroys == 2 && mock.maps == 2 &&
              mock.unmaps == 2 && mock.files.empty() && mock.dumbs.empty() &&
              mock.mappings.empty() && mock.invalid == 0,
          "DUMB bookkeeping OOM retires each mapping, PRIME fd and handle exactly once");
  mock.fail_bookkeeping_map = 0;
  Require(CreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 64, 32,
                          surfaces, 1, nullptr, 0) == VA_STATUS_SUCCESS &&
              DestroySurfaces(&context, surfaces, 1) == VA_STATUS_SUCCESS,
          "surface creation and destruction recover after bookkeeping OOM");
  Require(mock.creates == 3 && mock.destroys == 3 && mock.maps == 3 &&
              mock.unmaps == 3 && mock.files.empty() && mock.dumbs.empty() &&
              mock.mappings.empty() && mock.invalid == 0,
          "recovery after bookkeeping OOM leaves no resources behind");
}

void NativeDumbBookkeepingFailures() {
  for (unsigned allocation = 1; allocation <= 3; ++allocation)
    NativeDumbBookkeepingFailure(allocation);
}

void ImportedBookkeepingFailure(bool prime, unsigned allocation, bool public_api) {
  ImportIoProbe probe;
  uintptr_t handles[2] = {};
  VADRMPRIMESurfaceDescriptor descriptors[2] = {};
  for (unsigned index = 0; index < 2; ++index) {
    probe.sources[index] = memfd_create("crystalhd-private-import-oom", MFD_CLOEXEC);
    Require(probe.sources[index] >= 0 &&
                ftruncate(probe.sources[index], 512) == 0 &&
                __real_fstat(probe.sources[index], &probe.identities[index]) == 0,
            "allocate caller-owned private import backing");
    const uint8_t pixel = static_cast<uint8_t>(40 + index * 30);
    Require(pwrite(probe.sources[index], &pixel, 1, 0) == 1,
            "initialize distinct caller-owned import bytes");
    handles[index] = probe.sources[index];
    auto &desc = descriptors[index];
    desc.fourcc = VA_FOURCC_NV12;
    desc.width = desc.height = 16;
    desc.num_objects = desc.num_layers = 1;
    desc.objects[0].fd = probe.sources[index];
    desc.objects[0].size = 512;
    desc.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    desc.layers[0].drm_format = DRM_FORMAT_NV12;
    desc.layers[0].num_planes = 2;
    desc.layers[0].pitch[0] = desc.layers[0].pitch[1] = 16;
    desc.layers[0].offset[1] = 256;
  }
  VASurfaceAttribExternalBuffers legacy = {};
  legacy.pixel_format = VA_FOURCC_NV12;
  legacy.width = legacy.height = 16;
  legacy.data_size = 512;
  legacy.num_planes = 2;
  legacy.pitches[0] = legacy.pitches[1] = 16;
  legacy.offsets[1] = 256;
  legacy.buffers = handles;
  legacy.num_buffers = public_api ? 2 : 1;
  VASurfaceAttrib attributes[2] = {};
  attributes[0].type = VASurfaceAttribMemoryType;
  attributes[0].value.type = VAGenericValueTypeInteger;
  attributes[0].value.value.i = prime ? VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2
                                      : VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
  attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
  attributes[1].value.type = VAGenericValueTypePointer;
  attributes[1].value.value.p = prime ? static_cast<void *>(descriptors)
                                      : static_cast<void *>(&legacy);

  ImportIoScope scope(probe);
  Driver driver{-1};
  driver.StopWorkers();
  VADriverContext context = {};
  context.pDriverData = &driver;
  VASurfaceID surfaces[2] = {VA_INVALID_SURFACE, VA_INVALID_SURFACE};
  probe.fail_allocation = allocation;
  if (public_api) probe.arm_fd = probe.sources[1];
  VAStatus status = VA_STATUS_SUCCESS;
  bool imported = true, escaped = false;
  try {
    if (public_api) {
      status = CreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 16, 16,
                              surfaces, 2, attributes, 2);
    } else {
      Surface surface;
      surface.width = surface.height = 16;
      probe.Arm();
      imported = prime ? surface.ImportPrime(nullptr, descriptors[0])
                       : surface.ImportLegacy(legacy);
    }
  } catch (const std::bad_alloc &) {
    escaped = true;
  }
  const bool failed = probe.armed && !allocation_fault::fail_next;
  const size_t failed_size = allocation_fault::failed_size;
  allocation_fault::fail_next = false;
  allocation_fault::only_size = 0;
  allocation_fault::skip_matches = 0;
  probe.arm_fd = -1;
  const int copies = probe.Copies();
  const bool rolled_back = driver.surfaces.empty() && driver.backing_owners.empty();
  bool sources_usable = true;
  for (unsigned index = 0; index < 2; ++index) {
    uint8_t pixel = 0;
    const uint8_t marker = static_cast<uint8_t>(90 + index);
    const bool usable = fcntl(probe.sources[index], F_GETFD) >= 0 &&
        pread(probe.sources[index], &pixel, 1, 0) == 1 &&
        pixel == 40 + index * 30 &&
        pwrite(probe.sources[index], &marker, 1, 1) == 1;
    sources_usable = sources_usable && usable;
  }
  if (escaped || !rolled_back || copies != 0 || probe.maps != probe.unmaps) {
    fprintf(stderr, "import OOM prime=%u allocation=%u api=%u escaped=%u "
                    "status=%#x copies=%d maps=%u unmaps=%u rollback=%u inputs_usable=%u\n",
            prime, allocation, public_api, escaped, status, copies,
            probe.maps, probe.unmaps, rolled_back, sources_usable);
  }
  const size_t expected_size = prime
      ? (allocation == 3 ? sizeof(int) : sizeof(void *))
      : (allocation == 1 ? sizeof(int) : sizeof(void *));
  Require(failed && failed_size == expected_size,
          "the selected import metadata allocation actually fails");
  Require(sources_usable,
          "failed import preserves usable caller-owned descriptors and bytes");
  Require(!escaped && (public_api ? status == VA_STATUS_ERROR_ALLOCATION_FAILED
                                  : !imported),
          "import bookkeeping OOM returns false or VA allocation error without escaping");
  Require(rolled_back && (!public_api || surfaces[1] == VA_INVALID_SURFACE),
          "import bookkeeping OOM publishes no failed surface and rolls back earlier surfaces");
  Require(copies == 0 && probe.maps == (public_api ? 1U : 0U) &&
              probe.maps == probe.unmaps && probe.invalid == 0,
          "import bookkeeping OOM leaves no duplicated descriptors or mappings");
  if (public_api) {
    Require(CreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 16, 16,
                            surfaces, 2, attributes, 2) == VA_STATUS_SUCCESS &&
                driver.surfaces.at(surfaces[0])->planes[0][0] == 40 &&
                driver.surfaces.at(surfaces[1])->planes[0][0] == 70 &&
                DestroySurfaces(&context, surfaces, 2) == VA_STATUS_SUCCESS &&
                driver.surfaces.empty() && driver.backing_owners.empty(),
            "the same caller buffers import and destroy correctly after API OOM");
  } else {
    Surface surface;
    surface.width = surface.height = 16;
    Require((prime ? surface.ImportPrime(nullptr, descriptors[0])
                    : surface.ImportLegacy(legacy)) && surface.planes[0][0] == 40,
            "the same caller buffer imports correctly after importer OOM");
  }
  Require(probe.Copies() == 0 && probe.maps == probe.unmaps && probe.invalid == 0,
          "successful retry also retires every imported descriptor and mapping");
}

void ImportedBookkeepingFailures() {
  for (bool prime : {false, true})
    for (unsigned allocation = 1; allocation <= 3; ++allocation)
      for (bool public_api : {false, true})
        ImportedBookkeepingFailure(prime, allocation, public_api);
}

struct SurfaceCreationFailure {
  VAStatus status = VA_STATUS_SUCCESS;
  bool escaped = false;
  bool injected = false;
  size_t bytes = 0;
};

SurfaceCreationFailure FailSurfaceCreation(VADriverContextP context,
    VASurfaceID *surfaces, unsigned count, VASurfaceAttrib *attributes,
    unsigned attribute_count, unsigned skip, size_t only_size = 0) {
  struct ResetFault {
    ~ResetFault() {
      allocation_fault::fail_next = false;
      allocation_fault::only_size = 0;
      allocation_fault::skip_matches = 0;
    }
  } reset;
  allocation_fault::failed_size = 0;
  allocation_fault::only_size = only_size;
  allocation_fault::skip_matches = skip;
  allocation_fault::fail_next = true;
  SurfaceCreationFailure result;
  try {
    result.status = CreateSurfaces2(context, VA_RT_FORMAT_YUV420, 64, 32,
                                    surfaces, count, attributes, attribute_count);
  } catch (const std::bad_alloc &) {
    result.escaped = true;
  }
  result.injected = !allocation_fault::fail_next;
  result.bytes = allocation_fault::failed_size;
  return result;
}

void CpuSurfaceCreationStorageFailureRollsBack() {
  Driver driver{-1};
  driver.StopWorkers();
  VADriverContext context = {};
  context.pDriverData = &driver;
  VASurfaceID retained_id = VA_INVALID_SURFACE;
  Require(CreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 64, 32,
                          &retained_id, 1, nullptr, 0) == VA_STATUS_SUCCESS,
          "create an existing surface before CPU storage OOM");
  const auto retained = driver.surfaces.at(retained_id);
  retained->planes[0][0] = 73;
  const size_t max_ids = std::vector<VASurfaceID>().max_size();
  if (max_ids < UINT_MAX) {
    VASurfaceID impossible = retained_id;
    Require(CreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 64, 32, &impossible,
                            static_cast<unsigned>(max_ids + 1), nullptr, 0) ==
                    VA_STATUS_ERROR_ALLOCATION_FAILED && impossible == retained_id &&
                driver.surfaces.size() == 1,
            "32-bit surface counts exceeding journal capacity fail before output access");
  }
  VASurfaceID surfaces[2] = {retained_id, retained_id};
  const auto failure = FailSurfaceCreation(&context, surfaces, 2, nullptr, 0,
                                          1, 64 * (32 + 16));
  printf("CPU storage OOM: injected=%d escaped=%d status=%#x live=%zu expected=1 "
         "outputs=%#x,%#x retained_pixel=%u\n", failure.injected, failure.escaped,
         failure.status, driver.surfaces.size(), surfaces[0], surfaces[1],
         retained->planes[0][0]);
  Require(failure.injected && failure.bytes == 64 * (32 + 16) &&
              !failure.escaped && failure.status == VA_STATUS_ERROR_ALLOCATION_FAILED,
          "second CPU storage OOM returns allocation failure through the VA API");
  Require(surfaces[0] == VA_INVALID_SURFACE && surfaces[1] == VA_INVALID_SURFACE &&
              driver.surfaces.size() == 1 && driver.backing_owners.empty() &&
              driver.surfaces.at(retained_id) == retained && retained->planes[0][0] == 73,
          "CPU storage OOM removes the successful batch prefix and preserves existing pixels");
  Require(CreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 64, 32,
                          surfaces, 2, nullptr, 0) == VA_STATUS_SUCCESS &&
              surfaces[0] != VA_INVALID_SURFACE && surfaces[1] != VA_INVALID_SURFACE &&
              surfaces[0] != surfaces[1] && surfaces[0] != retained_id &&
              surfaces[1] != retained_id &&
              DestroySurfaces(&context, surfaces, 2) == VA_STATUS_SUCCESS &&
              driver.surfaces.size() == 1,
          "CPU surface creation recovers with distinct live IDs after storage OOM");
}

void CpuSurfaceCreationIdLogFailureRollsBack() {
  Driver driver{-1};
  driver.StopWorkers();
  VADriverContext context = {};
  context.pDriverData = &driver;
  VASurfaceID retained_id = VA_INVALID_SURFACE;
  Require(CreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 64, 32,
                          &retained_id, 1, nullptr, 0) == VA_STATUS_SUCCESS,
          "create an existing surface before ID-log allocation failure");
  const auto retained = driver.surfaces.at(retained_id);
  retained->planes[0][0] = 83;
  VASurfaceID surface = retained_id;
  // The first ID-log element is the only sizeof(VASurfaceID) allocation on
  // this CPU path. Select it independently of earlier storage allocations.
  const auto failure = FailSurfaceCreation(&context, &surface, 1, nullptr, 0,
                                          0, sizeof(VASurfaceID));
  printf("CPU ID-log OOM: injected=%d escaped=%d status=%#x live=%zu expected=1 "
         "output=%#x retained_pixel=%u\n", failure.injected, failure.escaped,
         failure.status, driver.surfaces.size(), surface, retained->planes[0][0]);
  Require(failure.injected && failure.bytes == sizeof(VASurfaceID) &&
              !failure.escaped && failure.status == VA_STATUS_ERROR_ALLOCATION_FAILED,
          "ID-log OOM returns allocation failure through the VA callback");
  Require(surface == VA_INVALID_SURFACE && driver.surfaces.size() == 1 &&
              driver.backing_owners.empty() && driver.surfaces.at(retained_id) == retained &&
              retained->planes[0][0] == 83,
          "ID-log OOM publishes no surface and preserves the existing live pixels");
}

void CpuSurfaceCreationAllocationFailuresRollBack() {
  bool completed = false;
  bool failed_id_log = false;
  for (unsigned skip = 0; skip != 64; ++skip) {
    Driver driver{-1};
    driver.StopWorkers();
    VADriverContext context = {};
    context.pDriverData = &driver;
    VASurfaceID retained_id = VA_INVALID_SURFACE;
    Require(CreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 64, 32,
                            &retained_id, 1, nullptr, 0) == VA_STATUS_SUCCESS,
            "create a live surface before sweeping CPU publication allocations");
    const auto retained = driver.surfaces.at(retained_id);
    retained->planes[0][0] = 91;
    VASurfaceID surfaces[2] = {retained_id, retained_id};
    // Sweep every allocation, including registry nodes and rollback-log
    // growth, without depending on a standard library's hash-node sizes.
    const auto failure = FailSurfaceCreation(&context, surfaces, 2, nullptr, 0, skip);
    Require(!failure.escaped, "CPU surface creation never leaks an allocation exception");
    if (!failure.injected) {
      Require(failure.status == VA_STATUS_SUCCESS &&
                  surfaces[0] != VA_INVALID_SURFACE && surfaces[1] != VA_INVALID_SURFACE &&
                  surfaces[0] != surfaces[1] &&
                  DestroySurfaces(&context, surfaces, 2) == VA_STATUS_SUCCESS,
              "allocation sweep reaches successful CPU batch publication");
      completed = true;
    } else {
      failed_id_log |= failure.bytes == sizeof(VASurfaceID);
      Require(failure.status == VA_STATUS_ERROR_ALLOCATION_FAILED &&
                  surfaces[0] == VA_INVALID_SURFACE && surfaces[1] == VA_INVALID_SURFACE,
              "CPU registry and ID-log OOM returns no published IDs");
    }
    Require(driver.surfaces.size() == 1 && driver.backing_owners.empty() &&
                driver.surfaces.at(retained_id) == retained && retained->planes[0][0] == 91,
            "CPU registry and log failures preserve all previously live resources");
    if (completed) break;
  }
  Require(completed && failed_id_log,
          "CPU allocation sweep covers ID-log growth and reaches its finite end");
}

void ImportedSurfaceCreationAllocationFailuresRollBack() {
  const auto fd_count = [] {
    DIR *directory = opendir("/proc/self/fd");
    Require(directory != nullptr, "inspect imported surface descriptor lifetime");
    size_t count = 0;
    while (const dirent *entry = readdir(directory))
      if (entry->d_name[0] != '.') ++count;
    closedir(directory);
    return count;
  };
  for (bool legacy : {false, true}) {
    bool completed = false;
    bool failed_backing_log = false;
    for (unsigned skip = 0; skip != 96; ++skip) {
      Driver driver{-1};
      driver.StopWorkers();
      VADriverContext context = {};
      context.pDriverData = &driver;
      Surface files;
      for (unsigned index = 0; index != 2; ++index) {
        const int fd = memfd_create("crystalhd-create-transaction", MFD_CLOEXEC);
        Require(fd >= 0, "allocate private surface transaction backing");
        files.object_fds.push_back(fd);
        Require(ftruncate(fd, 3072) == 0, "size private surface transaction backing");
      }
      VADRMPRIMESurfaceDescriptor prime[3] = {};
      for (auto &desc : prime) {
        desc.fourcc = VA_FOURCC_NV12;
        desc.width = 64;
        desc.height = 32;
        desc.num_objects = 1;
        desc.objects[0].fd = files.object_fds[0];
        desc.objects[0].size = 3072;
        desc.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
        desc.num_layers = 1;
        desc.layers[0].drm_format = DRM_FORMAT_NV12;
        desc.layers[0].num_planes = 2;
        desc.layers[0].pitch[0] = desc.layers[0].pitch[1] = 64;
        desc.layers[0].offset[1] = 2048;
      }
      uintptr_t handles[3] = {static_cast<uintptr_t>(files.object_fds[0]),
                              static_cast<uintptr_t>(files.object_fds[0]),
                              static_cast<uintptr_t>(files.object_fds[0])};
      VASurfaceAttribExternalBuffers external = {};
      external.pixel_format = VA_FOURCC_NV12;
      external.width = 64;
      external.height = 32;
      external.data_size = 3072;
      external.num_planes = 2;
      external.pitches[0] = external.pitches[1] = 64;
      external.offsets[1] = 2048;
      external.num_buffers = 3;
      external.buffers = handles;
      VASurfaceAttrib attributes[2] = {};
      attributes[0].type = VASurfaceAttribMemoryType;
      attributes[0].value.type = VAGenericValueTypeInteger;
      attributes[0].value.value.i = legacy ? VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME
                                         : VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
      attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
      attributes[1].value.type = VAGenericValueTypePointer;
      attributes[1].value.value.p = legacy ? static_cast<void *>(&external)
                                         : static_cast<void *>(prime);
      VASurfaceID retained_id = VA_INVALID_SURFACE;
      Require(CreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 64, 32,
                              &retained_id, 1, attributes, 2) == VA_STATUS_SUCCESS,
              "retain an unrelated imported backing before allocation failures");
      const auto retained = driver.surfaces.at(retained_id);
      retained->planes[0][0] = 117;
      const auto owners = driver.backing_owners;
      const size_t descriptors = fd_count();
      // The batch first aliases the live canonical owner, then introduces
      // another backing and aliases that new owner. Rollback must distinguish
      // the preexisting registry key from keys introduced by this batch.
      for (unsigned index = 0; index != 3; ++index) {
        const int fd = files.object_fds[index == 0 ? 0 : 1];
        prime[index].objects[0].fd = fd;
        handles[index] = fd;
      }
      VASurfaceID surfaces[3] = {retained_id, retained_id, retained_id};
      const auto failure = FailSurfaceCreation(&context, surfaces, 3, attributes, 2, skip);
      Require(!failure.escaped, "imported batch OOM never escapes the public VA callback");
      if (!failure.injected) {
        Require(failure.status == VA_STATUS_SUCCESS &&
                    surfaces[0] != VA_INVALID_SURFACE && surfaces[1] != VA_INVALID_SURFACE &&
                    surfaces[2] != VA_INVALID_SURFACE && surfaces[0] != surfaces[1] &&
                    surfaces[1] != surfaces[2] && surfaces[0] != surfaces[2] &&
                    surfaces[0] != retained_id && surfaces[1] != retained_id &&
                    surfaces[2] != retained_id &&
                    driver.surfaces.at(surfaces[0])->backing_owner == retained_id &&
                    driver.surfaces.at(surfaces[2])->backing_owner == surfaces[1] &&
                    DestroySurfaces(&context, surfaces, 3) == VA_STATUS_SUCCESS,
                "import allocation sweep reaches valid aliases of existing and new owners");
        completed = true;
      } else {
        failed_backing_log |= failure.bytes == sizeof(BackingIdentity);
        Require(failure.status == VA_STATUS_ERROR_ALLOCATION_FAILED &&
                    surfaces[0] == VA_INVALID_SURFACE && surfaces[1] == VA_INVALID_SURFACE &&
                    surfaces[2] == VA_INVALID_SURFACE,
                "import registry, layout and rollback-log OOM publishes no IDs");
      }
      Require(driver.surfaces.size() == 1 && driver.surfaces.at(retained_id) == retained &&
                  driver.backing_owners == owners && retained->planes[0][0] == 117 &&
                  fcntl(retained->object_fds[0], F_GETFD) >= 0 && fd_count() == descriptors,
              "failed imported batches retire new backing and retain prior mappings and fds");
      if (completed) break;
    }
    Require(completed && failed_backing_log,
            "both PRIME import allocation sweeps cover backing bookkeeping and finish");
  }
}

void SurfaceCreationAllocationFailures() {
  CpuSurfaceCreationStorageFailureRollsBack();
  CpuSurfaceCreationIdLogFailureRollsBack();
  CpuSurfaceCreationAllocationFailuresRollBack();
  ImportedSurfaceCreationAllocationFailuresRollBack();
}

void NativeDumbMappingPreserves64BitOffsets() {
  constexpr uint64_t offset = UINT64_C(0x100971000);
  {
    NativeNodeMock mock;
    mock.dumb_offset_base = offset - 4096;
    MockNativeNodeScope scope(mock);
    bool native_backing = false;
    {
      Surface surface;
      Require(surface.AllocateInternal(nullptr, mock.render_fd, 64, 32,
                                        VA_FOURCC_NV12),
              "high-offset NV12 allocation succeeds");
      native_backing = surface.dumb_handle != 0 && surface.dumb_drm_fd >= 0 &&
          surface.object_maps.size() == 1 && surface.object_fds.size() == 1 &&
          surface.storage.empty();
      if (native_backing) {
        Require(mock.map_attempts == 1 && mock.maps == 1 &&
                    mock.last_mapped_offset == offset && mock.invalid == 0 &&
                    surface.object_sizes[0] == 64U * 48U &&
                    surface.pitch[0] == 64 && surface.pitch[1] == 64,
                "DUMB mmap receives the complete offset above four GiB");
        const auto &pixels = *mock.mappings.at(surface.object_maps[0]);
        for (unsigned plane = 0; plane != 2; ++plane) {
          const unsigned rows = plane == 0 ? 32 : 16;
          const size_t start = plane == 0 ? 0 : 64U * 32U;
          for (unsigned y = 0; y != rows; ++y)
            for (unsigned x = 0; x != 64; ++x) {
              const uint8_t expected = static_cast<uint8_t>(
                  17U + plane * 83U + y * 7U + x * 3U);
              surface.planes[plane][y * surface.pitch[plane] + x] = expected;
              Require(pixels[start + y * 64U + x] == expected,
                      "high-offset DUMB Y and UV planes address exact backing bytes");
            }
        }
      }
    }
    Require(mock.files.empty() && mock.dumbs.empty() && mock.mappings.empty() &&
                mock.creates == 1 && mock.destroys == 1 && mock.closes == 2 &&
                mock.maps == mock.unmaps && !mock.device_owned,
            "high-offset allocation releases its PRIME fd, DUMB handle and node");
    Require(native_backing,
            "64-bit DRM offset must retain DUMB backing; CPU fallback is not PRIME-capable");
  }
  struct Rejected { uint64_t offset, size; bool override_size, size_overflow; };
  const Rejected rejected[] = {
      {UINT64_C(0x8000000000001000), 0, false, false},
      {offset, 0, true, false},
      {offset, UINT64_C(0x100000000), true, true}};
  for (const Rejected &input : rejected) {
    if (input.size_overflow && input.size <= std::numeric_limits<size_t>::max())
      continue;
    NativeNodeMock mock;
    mock.dumb_offset_base = input.offset - 4096;
    mock.override_create_size = input.override_size;
    mock.create_size_override = input.size;
    MockNativeNodeScope scope(mock);
    {
      Surface surface;
      Require(surface.AllocateInternal(nullptr, mock.render_fd, 64, 32,
                                        VA_FOURCC_NV12) &&
                  surface.dumb_handle == 0 && surface.dumb_drm_fd == -1 &&
                  surface.object_maps.empty() && surface.object_fds.empty() &&
                  !surface.storage.empty() && mock.map_attempts == 0,
              "unrepresentable DRM offset or extent rejects before mmap and uses CPU backing");
    }
    Require(mock.files.empty() && mock.dumbs.empty() && mock.mappings.empty() &&
                mock.creates == 1 && mock.destroys == 1 && mock.closes == 2 &&
                mock.maps == 0 && mock.unmaps == 0 && mock.invalid == 0 &&
                !mock.device_owned,
            "rejected DUMB metadata releases PRIME fd, handle and node");
  }
}

int NativeDumbOffsetTestEntry() {
  try {
    NativeDumbMappingPreserves64BitOffsets();
    puts("VA-API native DUMB 64-bit offsets/pixels/rejection/cleanup PASS");
    return 0;
  } catch (const std::exception &error) {
    fprintf(stderr, "VA-API native DUMB offset regression failed: %s\n", error.what());
    return 1;
  }
}

void I915WcMappingRequiresExactOffsetIdentity() {
  struct Mode {
    int version;
    bool fail_version, fail_offset, different_offset, interrupted, expected;
  };
  const Mode modes[] = {
      {0, false, false, false, false, false},
      {3, false, false, false, false, false},
      {4, false, false, false, false, true},
      {5, false, false, false, false, true},
      {4, true, false, false, false, false},
      {4, false, true, false, false, false},
      {4, false, false, true, false, false},
      {4, false, false, false, true, true},
  };
  for (const Mode &mode : modes) {
    NativeNodeMock mock;
    MockNativeNodeScope scope(mock);
    {
      Surface surface;
      Require(surface.AllocateInternal(nullptr, mock.render_fd, 64, 32,
                                        VA_FOURCC_NV12),
              "mapping eligibility uses an existing owned DUMB mapping");
      Require(!surface.experimental_i915_wc && !surface.owned_i915_dumb &&
                  mock.mmap_version_queries == 0 && mock.wc_offset_queries == 0,
              "opt-in off never queries or enables experimental WC policy");
      const uint64_t offset = mock.dumbs.at(surface.dumb_handle).offset;
      mock.mmap_interface_version = mode.version;
      mock.fail_mmap_version = mode.fail_version;
      mock.fail_wc_offset = mode.fail_offset;
      mock.different_wc_offset = mode.different_offset;
      mock.interrupt_mmap_version = mode.interrupted ? 1U : 0U;
      mock.interrupt_wc_offset = mode.interrupted ? 1U : 0U;
      Require(ConfirmedI915WcMapping(surface.dumb_drm_fd, surface.dumb_handle,
                                     offset) == mode.expected,
              "WC requires version 4 and the same handle's actual mapped offset");
      const unsigned attempts = mode.interrupted ? 2U : 1U;
      Require(mock.mmap_version_queries == attempts &&
                  mock.wc_offset_queries ==
                      (mode.fail_version || mode.version < 4 ? 0U : attempts),
              "old aliases and failed version queries never authorize WC offsets");
      const unsigned queries = mock.mmap_version_queries + mock.wc_offset_queries;
      Require(!ConfirmedI915WcMapping(-1, surface.dumb_handle, offset) &&
                  !ConfirmedI915WcMapping(surface.dumb_drm_fd, 0, offset) &&
                  mock.mmap_version_queries + mock.wc_offset_queries == queries,
              "missing owned descriptor or handle cannot establish WC identity");
      Require(mock.maps == 1 && mock.mappings.size() == 1 &&
                  surface.planes[0] == mock.mappings.begin()->first,
              "eligibility queries never replace or remap the original mapping");
    }
    Require(mock.files.empty() && mock.dumbs.empty() && mock.mappings.empty() &&
                mock.maps == 1 && mock.unmaps == 1 && mock.invalid == 0,
            "mapping query failures preserve ordinary allocation cleanup");
  }
}

std::shared_ptr<Surface> Picture(uint64_t timestamp, uint8_t luma) {
  auto surface = std::make_shared<Surface>();
  Require(surface->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
          "allocate private picture");
  for (unsigned int row = 0; row < surface->height; ++row)
    memset(surface->planes[0] + row * surface->pitch[0], luma, surface->width);
  for (unsigned int row = 0; row < surface->height / 2; ++row)
    memset(surface->planes[1] + row * surface->pitch[1], 128, surface->width);
  surface->ready = true;
  surface->expected_timestamp = timestamp;
  surface->frame_timestamp = timestamp;
  return surface;
}

struct Fixture {
  Driver driver{-1};
  VADriverContext context = {};
  std::shared_ptr<DecodeContext> decoder = std::make_shared<DecodeContext>();
  std::shared_ptr<DecodeContext> processor = std::make_shared<DecodeContext>();
  std::shared_ptr<Surface> source = Picture(kTimestampStep, 40);
  std::shared_ptr<Surface> target = Picture(0, 99);
  VABufferID parameters = VA_INVALID_ID;

  explicit Fixture(int trace_fd = STDERR_FILENO) : driver(-1, trace_fd) {
    context.pDriverData = &driver;
    processor->video_process = true;
    driver.contexts[1] = decoder;
    driver.contexts[2] = processor;
    driver.surfaces[1] = source;
    driver.surfaces[2] = target;
    driver.next_surface = 3;
    decoder->surface_timestamps[source.get()] = kTimestampStep;
    decoder->decoded_frames[kTimestampStep] = Picture(kTimestampStep, 40);
    VAProcPipelineParameterBuffer pipeline = {};
    pipeline.surface = 1;
    Require(CreateBuffer(&context, 2, VAProcPipelineParameterBufferType,
                         sizeof(pipeline), 1, &pipeline, &parameters) ==
                VA_STATUS_SUCCESS,
            "create VPP parameters");
  }

  void SubmitParameters() {
    Require(BeginPicture(&context, 2, 2) == VA_STATUS_SUCCESS, "begin VPP");
    Require(RenderPicture(&context, 2, &parameters, 1) == VA_STATUS_SUCCESS,
            "capture VPP picture");
  }

  void Complete(uint64_t timestamp, uint8_t luma) {
    Require(EndPicture(&context, 2) == VA_STATUS_SUCCESS, "complete VPP");
    Require(target->ready && !target->failed, "successful target status");
    Require(target->frame_timestamp == timestamp, "correct target identity");
    for (unsigned int row = 0; row < target->height; ++row) {
      for (unsigned int column = 0; column < target->width; ++column)
        Require(target->planes[0][row * target->pitch[0] + column] == luma,
                "correct target pixels");
    }
  }
};

static void VppTraceKeepsQueuedIdentity() {
  const char *previous = getenv("CRYSTALHD_VAAPI_TRACE");
  const bool had_value = previous != nullptr;
  const std::string saved = previous ? previous : "";
  struct Restore {
    bool had; std::string value;
    ~Restore() {
      if (had) setenv("CRYSTALHD_VAAPI_TRACE", value.c_str(), 1);
      else unsetenv("CRYSTALHD_VAAPI_TRACE");
    }
  } restore{had_value, saved};
  setenv("CRYSTALHD_VAAPI_TRACE", "1", 1);
  for (unsigned mode = 0; mode != 3; ++mode) {
    FILE *file = tmpfile();
    Require(file != nullptr, "create VPP trace file");
    {
      Fixture f(fileno(file));
      f.driver.StopWorkers();
      f.driver.stopping = false;
      f.decoder->id = 7;
      f.decoder->generation = 3;
      f.target->id = 2;
      auto picture = f.decoder->decoded_frames.at(kTimestampStep);
      picture->trace_decode_identity = 19;
      picture->trace_submission_ordinal = 4;
      picture->ready = mode == 0;
      f.SubmitParameters();
      Require(EndPicture(&f.context, 2) == VA_STATUS_SUCCESS, "trace immediate/queued VPP");
      if (mode != 0) {
        Require(f.driver.pending_vpp.size() == 1, "one traced queued operation");
        if (mode == 2) f.decoder->Reset();
        else picture->ready = true;
        f.driver.vpp_worker = std::thread([&] { RunVppWorker(&f.driver); });
        Require(SyncSurface2(&f.context, 2, 1000000000ULL) ==
                    (mode == 2 ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS),
                "trace leaves queued completion/cancellation unchanged");
      }
      Require(f.driver.pending_vpp.empty() && picture->vpp_readers == 0 &&
                  f.target->vpp_writers == 0, "tracing retains no VPP leases");
    }
    Require(fseek(file, 0, SEEK_END) == 0, "size VPP trace");
    std::string data(static_cast<size_t>(ftell(file)), '\0');
    rewind(file);
    Require(fread(&data[0], 1, data.size(), file) == data.size(), "read VPP trace");
    fclose(file);
    auto event = [&](const char *name) {
      const std::string marker = std::string("\"event\":\"") + name + "\"";
      const size_t at = data.find(marker);
      Require(at != std::string::npos, "expected VPP stage recorded");
      const size_t begin = data.rfind('\n', at);
      return data.substr(begin == std::string::npos ? 0 : begin + 1,
                         data.find('\n', at) - (begin == std::string::npos ? 0 : begin + 1));
    };
    const std::string terminal = event(mode == 2 ? "vpp_cancel" : "vpp_commit");
    for (const char *field : {"\"context\":7,", "\"generation\":3,",
                              "\"decode_identity\":19,", "\"operation\":1,"})
      Require(terminal.find(field) != std::string::npos,
              "terminal VPP trace retains captured identity after reset");
    if (mode != 0) (void)event("vpp_queued");
    if (mode == 1) {
      (void)event("vpp_start");
      (void)event("vpp_run");
      Require(data.find("\"event\":\"vpp_queued\"") < data.find("\"event\":\"vpp_start\"") &&
                  data.find("\"event\":\"vpp_start\"") < data.find("\"event\":\"vpp_run\""),
              "queue, worker selection and conversion have ordered events");
    }
    if (mode == 0) (void)event("vpp_run");
    if (mode == 2) Require(data.find("\"event\":\"vpp_run\"") == std::string::npos,
                           "canceled source has no conversion-start event");
  }
}

struct DecodeProcessingFixture : Fixture {
  VARectangle visible{0, 0, 256, 24};
  VAProcPipelineParameterBuffer pipeline{};
  VASurfaceID output = 2;

  DecodeProcessingFixture() {
    driver.StopWorkers();
    decoder->id = 1;
    decoder->decode_processing = true;
    decoder->width = 256;
    decoder->height = 32;
    decoder->surface_timestamps.clear();
    decoder->decoded_frames.clear();
    source = std::make_shared<Surface>();
    target = std::make_shared<Surface>();
    Require(source->AllocateInternal(nullptr, -1, 256, 32, VA_FOURCC_NV12) &&
                target->AllocateInternal(nullptr, -1, 128, 12, VA_FOURCC_NV12),
            "allocate coded original and scaled processing output");
    source->id = 1;
    target->id = 2;
    source->ready = target->ready = true;
    driver.surfaces[1] = source;
    driver.surfaces[2] = target;
    pipeline.surface = 1;
    pipeline.surface_region = &visible;
    pipeline.num_additional_outputs = 1;
    pipeline.additional_outputs = &output;
    Require(CreateBuffer(&context, 1, VAProcPipelineParameterBufferType,
                          sizeof(pipeline), 1, &pipeline, &parameters) == VA_STATUS_SUCCESS,
            "create integrated processing parameters");
  }

  VAStatus Render() {
    memcpy(driver.buffers.at(parameters).data.data(), &pipeline, sizeof(pipeline));
    return RenderPicture(&context, 1, &parameters, 1);
  }
};

void DecodeProcessingAdmissionIsExplicitAndCaptured() {
  DecodeProcessingFixture fixture;
  unsigned count = 77;
  VAProcPipelineCaps caps{};
  Require(QueryVideoProcFilters(&fixture.context, 1, nullptr, &count) == VA_STATUS_SUCCESS &&
              count == 0 &&
              QueryVideoProcPipelineCaps(&fixture.context, 1, nullptr, 0, &caps) ==
                  VA_STATUS_SUCCESS && caps.num_additional_outputs == 1 &&
              caps.num_output_pixel_formats == 1 &&
              caps.output_pixel_format[0] == VA_FOURCC_NV12 &&
              caps.min_output_width == 128,
          "integrated decoder exposes one scaled NV12 output, not host Bob filters");
  Require(QueryVideoProcFilterCaps(&fixture.context, 1, VAProcFilterDeinterlacing,
                                   nullptr, &count) == VA_STATUS_ERROR_UNSUPPORTED_FILTER &&
              count == 0,
          "card scaling does not advertise a hardware deinterlacer");
  const auto original = fixture.source->storage;
  const auto output = fixture.target->storage;
  Require(BeginPicture(&fixture.context, 1, 1) == VA_STATUS_SUCCESS &&
              fixture.Render() == VA_STATUS_SUCCESS,
          "integrated H.264 processing captures the additional output");
  Require(fixture.decoder->processing_request.enabled &&
              fixture.decoder->processing_request.visible_height == 24 &&
              fixture.decoder->processing_request.capture_width == 128 &&
              fixture.decoder->processing_request.capture_height == 12 &&
              fixture.decoder->processing_target == 2 &&
              fixture.decoder->have_processing_parameters &&
              !fixture.source->opaque_reference &&
              fixture.source->storage == original && fixture.target->storage == output,
          "Render captures geometry without publishing pixels or making the original opaque");
  fixture.visible.height = 16;
  fixture.output = 1;
  Require(fixture.decoder->processing_request.visible_height == 24 &&
              fixture.decoder->processing_target == 2,
          "captured geometry and output ID do not borrow client pointers");
  Require(DestroyBuffer(&fixture.context, fixture.parameters) == VA_STATUS_SUCCESS &&
              fixture.decoder->have_processing_parameters,
          "buffer destruction does not invalidate the captured processing descriptor");

  for (unsigned invalid = 0; invalid != 13; ++invalid) {
    DecodeProcessingFixture rejected;
    Require(BeginPicture(&rejected.context, 1, 1) == VA_STATUS_SUCCESS,
            "begin bounded invalid processing request");
    switch (invalid) {
      case 0: rejected.visible.x = 2; break;
      case 1: rejected.visible.width = 240; break;
      case 2: rejected.visible.height = 22; break;
      case 3: rejected.pipeline.num_additional_outputs = 0; break;
      case 4: rejected.pipeline.num_additional_outputs = 2; break;
      case 5: rejected.output = 1; break;
      case 6: rejected.target->external_accessed = true; break;
      case 7: rejected.target->opaque_reference = true; break;
      case 8: rejected.source->external_accessed = true; break;
      case 9: rejected.pipeline.num_filters = 1; break;
      case 10: rejected.pipeline.mirror_state = VA_MIRROR_HORIZONTAL; break;
      case 11: rejected.pipeline.input_surface_flag = VA_TOP_FIELD; break;
      case 12:
        Require(rejected.Render() == VA_STATUS_SUCCESS, "capture before duplicate rejection");
        break;
    }
    const auto before = rejected.target->storage;
    const unsigned opens = device_open_attempts;
    Require(rejected.Render() != VA_STATUS_SUCCESS &&
                rejected.decoder->h264_picture_invalid &&
                !rejected.decoder->have_processing_parameters &&
                rejected.decoder->processing_target == VA_INVALID_SURFACE &&
                rejected.driver.pending_vpp.empty() &&
                rejected.target->storage == before && device_open_attempts == opens,
            "invalid or duplicate processing cannot commit stale output or touch hardware");
  }
}

void OpaqueDecodeOriginalRejectsPixelAccess() {
  DecodeProcessingFixture fixture;
  fixture.source->opaque_reference = true;
  fixture.source->opaque_context = 1;
  const auto original = fixture.source->storage;
  VAImageFormat format{};
  format.fourcc = VA_FOURCC_NV12;
  format.byte_order = VA_LSB_FIRST;
  format.bits_per_pixel = 12;
  VAImage image{};
  Require(CreateImage(&fixture.context, &format, 256, 32, &image) == VA_STATUS_SUCCESS,
          "create independent readback image");
  VAImage derived{};
  VADRMPRIMESurfaceDescriptor exported{};
  for (uint32_t access : {VA_EXPORT_SURFACE_READ_ONLY, VA_EXPORT_SURFACE_WRITE_ONLY,
                          VA_EXPORT_SURFACE_READ_WRITE})
    Require(ExportSurfaceHandle(&fixture.context, 1,
                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, access, &exported) ==
                    VA_STATUS_ERROR_INVALID_SURFACE,
            "opaque references never export unpopulated original pixels");
  Require(DeriveImage(&fixture.context, 1, &derived) == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT &&
              GetImage(&fixture.context, 1, 0, 0, 256, 32, image.image_id) ==
                  VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT &&
              PutImage(&fixture.context, 1, image.image_id, 0, 0, 256, 32,
                       0, 0, 256, 32) == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT &&
              BeginPicture(&fixture.context, 2, 1) == VA_STATUS_ERROR_INVALID_SURFACE,
          "opaque references reject image read/write and ordinary VPP target access");
  fixture.decoder->decode_processing = false;
  Require(BeginPicture(&fixture.context, 1, 1) == VA_STATUS_ERROR_INVALID_SURFACE,
          "ordinary decode cannot reuse an opaque processing reference");
  fixture.decoder->decode_processing = true;
  Require(BeginPicture(&fixture.context, 1, 1) == VA_STATUS_SUCCESS,
          "owning processing context may reuse its opaque reference handle");
  VAProcPipelineParameterBuffer pipeline{};
  pipeline.surface = 1;
  VABufferID vpp = VA_INVALID_ID;
  Require(CreateBuffer(&fixture.context, 2, VAProcPipelineParameterBufferType,
                        sizeof(pipeline), 1, &pipeline, &vpp) == VA_STATUS_SUCCESS &&
              BeginPicture(&fixture.context, 2, 2) == VA_STATUS_SUCCESS &&
              RenderPicture(&fixture.context, 2, &vpp, 1) ==
                  VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT &&
              fixture.source->storage == original,
          "separate VPP cannot read opaque originals as blank or upscaled pixels");
}

VAProcFilterParameterBufferDeinterlacing BobParameters(uint32_t flags = 0) {
  VAProcFilterParameterBufferDeinterlacing parameters = {};
  parameters.type = VAProcFilterDeinterlacing;
  parameters.algorithm = VAProcDeinterlacingBob;
  parameters.flags = flags;
  return parameters;
}

VABufferID CreateBobFilter(Fixture &fixture, uint32_t flags = 0) {
  auto parameters = BobParameters(flags);
  VABufferID filter = VA_INVALID_ID;
  Require(CreateBuffer(&fixture.context, 2, VAProcFilterParameterBufferType,
                         sizeof(parameters), 1, &parameters, &filter) ==
              VA_STATUS_SUCCESS,
          "create a real Bob filter parameter buffer");
  return filter;
}

void SetPipelineFilters(Fixture &fixture, VABufferID *filters,
                        unsigned count) {
  VAProcPipelineParameterBuffer parameters = {};
  parameters.surface = 1;
  parameters.filters = filters;
  parameters.num_filters = count;
  memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(),
         &parameters, sizeof(parameters));
}

std::shared_ptr<Surface> PrivateArgbOutput() {
  auto surface = Picture(0, 99);
  surface->fourcc = VA_FOURCC_ARGB;
  surface->rt_format = VA_RT_FORMAT_RGB32;
  surface->pitch[0] = surface->width * 4;
  surface->pitch[1] = 0;
  surface->storage.assign(surface->pitch[0] * surface->height, 99);
  surface->planes[0] = surface->storage.data();
  surface->planes[1] = nullptr;
  return surface;
}

void VppInterleavedScalingPreservesFieldRows() {
  struct Geometry {
    unsigned width, height, output_width, output_height;
    VARectangle source, output;
  };
  const Geometry cases[] = {
      {16, 16, 16, 16, {0, 0, 16, 12}, {0, 0, 16, 8}},
      {1920, 1088, 1280, 720, {0, 0, 1920, 1080}, {0, 0, 1280, 720}},
      {16, 16, 16, 16, {0, 0, 16, 8}, {0, 0, 16, 12}},
      {32, 32, 24, 24, {2, 4, 24, 24}, {2, 4, 16, 16}}};
  for (uint32_t field_order : {VA_TOP_FIELD_FIRST, VA_BOTTOM_FIELD_FIRST})
   for (const auto &geometry : cases) {
    Fixture fixture;
    auto picture = std::make_shared<Surface>();
    fixture.source = std::make_shared<Surface>();
    fixture.target = std::make_shared<Surface>();
    for (const auto &surface : {picture, fixture.source}) {
      Require(surface->AllocateInternal(nullptr, -1, geometry.width, geometry.height,
                                        VA_FOURCC_NV12), "allocate field source");
      surface->ready = true;
      surface->expected_timestamp = surface->frame_timestamp = kTimestampStep;
    }
    Require(fixture.target->AllocateInternal(nullptr, -1, geometry.output_width,
                                              geometry.output_height, VA_FOURCC_NV12),
            "allocate scaled field target");
    fixture.driver.surfaces[1] = fixture.source;
    fixture.driver.surfaces[2] = fixture.target;
    fixture.decoder->surface_timestamps.clear();
    fixture.decoder->surface_timestamps[fixture.source.get()] = kTimestampStep;
    fixture.decoder->decoded_frames[kTimestampStep] = picture;
    for (unsigned plane = 0; plane != 2; ++plane)
      for (unsigned row = 0; row < picture->height / (plane == 0 ? 1 : 2); ++row)
        for (unsigned column = 0; column < picture->width; ++column)
          picture->planes[plane][row * picture->pitch[plane] + column] =
              (row % 2 == 0 ? 20 : 150) + (row / 2 + column / 2) % 64;
    const auto original = picture->storage;
    VARectangle input = geometry.source;
    VARectangle output = geometry.output;
    VAProcPipelineParameterBuffer pipeline = {};
    pipeline.surface = 1;
    pipeline.surface_region = &input;
    pipeline.output_region = &output;
    pipeline.input_surface_flag = pipeline.output_surface_flag = field_order;
    memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(),
           &pipeline, sizeof(pipeline));
    fixture.SubmitParameters();
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
            "interleaved scaling succeeds through the public VPP pipeline");
    for (unsigned plane = 0; plane != 2; ++plane) {
      const unsigned divisor = plane == 0 ? 1 : 2;
      const unsigned rows = output.height / divisor;
      for (unsigned row = 0; row < rows; ++row) {
        const unsigned source_row = input.y / divisor +
            (row / 2) * (input.height / divisor / 2) / (rows / 2) * 2 + row % 2;
        for (unsigned column = 0; column < output.width; ++column) {
          unsigned source_column = input.x +
              (plane == 0 ? column : column & ~1U) * input.width / output.width;
          if (plane != 0) source_column = (source_column & ~1U) + column % 2;
          const uint8_t expected = picture->planes[plane][
              source_row * picture->pitch[plane] + source_column];
          Require(fixture.target->planes[plane][
                      (output.y / divisor + row) * fixture.target->pitch[plane] +
                      output.x + column] == expected,
                  "interleaved NV12 scaling must not mix top and bottom row lattices");
        }
      }
    }
    Require(picture->storage == original,
            "field scaling leaves the full public decoded picture intact");
  }
}

void VppInterleavedArgbScalingAndQueuedSnapshot() {
  for (uint32_t field_order : {VA_TOP_FIELD_FIRST, VA_BOTTOM_FIELD_FIRST})
    for (unsigned mode = 0; mode != 3; ++mode) {
      Fixture fixture;
      fixture.driver.StopWorkers();
      fixture.driver.stopping = false;
      fixture.target = PrivateArgbOutput();
      fixture.driver.surfaces[2] = fixture.target;
      auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
      // Neutral full-range fields have independently predictable RGB samples.
      for (unsigned row = 0; row < picture->height; ++row)
        memset(picture->planes[0] + row * picture->pitch[0],
               row % 2 == 0 ? 48 : 192, picture->width);
      const auto original = picture->storage;
      const auto untouched = fixture.target->storage;
      picture->ready = mode == 0;
      VARectangle input = {0, 0, 16, 12};
      VARectangle output = {0, 0, 16, 8};
      VAProcPipelineParameterBuffer pipeline = {};
      pipeline.surface = 1;
      pipeline.surface_region = &input;
      pipeline.output_region = &output;
      pipeline.input_surface_flag = pipeline.output_surface_flag = field_order;
      pipeline.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
      memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(),
             &pipeline, sizeof(pipeline));
      fixture.SubmitParameters();
      // Both field layout and rectangles are value snapshots, not client leases.
      pipeline.input_surface_flag = pipeline.output_surface_flag = VA_FRAME_PICTURE;
      input.height = 16;
      output.height = 16;
      memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(),
             &pipeline, sizeof(pipeline));
      Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
              "interleaved ARGB processing completes or queues");
      if (mode != 0) {
        Require(fixture.driver.pending_vpp.size() == 1,
                "cold interleaved source captures a pending operation");
        Require(DestroyBuffer(&fixture.context, fixture.parameters) == VA_STATUS_SUCCESS,
                "queued field layout does not borrow client parameters");
        if (mode == 2) fixture.decoder->Reset();
        else picture->ready = true;
        fixture.driver.vpp_worker = std::thread([&] { RunVppWorker(&fixture.driver); });
        Require(SyncSurface2(&fixture.context, 2, 1000000000ULL) ==
                    (mode == 2 ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS),
                "interleaved worker handles completion and cancellation");
      }
      Require(picture->storage == original && fixture.driver.pending_vpp.empty() &&
                  picture->vpp_readers == 0 && fixture.target->vpp_writers == 0,
              "interleaved processing preserves source pixels and releases all owners");
      if (mode == 2) {
        Require(fixture.target->storage == untouched,
                "canceled interleaved conversion publishes no pixels");
        continue;
      }
      for (unsigned row = 0; row < fixture.target->height; ++row)
        for (unsigned column = 0; column < fixture.target->width; ++column) {
          const uint8_t *pixel = fixture.target->planes[0] +
              row * fixture.target->pitch[0] + column * 4;
          const uint8_t expected = row >= 8 ? 0 : (row % 2 == 0 ? 48 : 192);
          Require(pixel[0] == expected && pixel[1] == expected &&
                      pixel[2] == expected && pixel[3] == 255,
                  "interleaved ARGB preserves both fields and neutral black borders");
        }
    }
}

void VppInterleavedInvalidGeometryRejectsBeforeCapture() {
  for (unsigned invalid = 0; invalid != 7; ++invalid) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    Fixture fixture;
    fixture.driver.StopWorkers();
    fixture.driver.stopping = false;
    fixture.target->layout.valid = true;
    fixture.target->object_fds = {101};
    auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    picture->ready = false;
    const auto original = fixture.target->storage;
    VARectangle input = {0, 0, 16, 12}, output = {0, 0, 16, 8};
    switch (invalid) {
      case 0: input.y = 2; break;
      case 1: input.height = 10; break;
      case 2: output.y = 2; break;
      case 3: output.height = 6; break;
      case 4: input.x = 1; input.width = 14; break;
      case 5: output.width = 15; break;
      case 6: output.x = 1; output.width = 14; break;
    }
    VAProcPipelineParameterBuffer pipeline = {};
    pipeline.surface = 1;
    pipeline.surface_region = &input;
    pipeline.output_region = &output;
    pipeline.input_surface_flag = pipeline.output_surface_flag = VA_TOP_FIELD_FIRST;
    memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(),
           &pipeline, sizeof(pipeline));
    Require(BeginPicture(&fixture.context, 2, 2) == VA_STATUS_SUCCESS &&
                RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                    VA_STATUS_ERROR_INVALID_PARAMETER,
            "invalid interleaved geometry rejects before capturing a cold source");
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                mock.events.empty() && fixture.driver.pending_vpp.empty() &&
                picture->vpp_readers == 0 && fixture.target->vpp_writers == 0 &&
                fixture.target->storage == original &&
                !fixture.processor->vpp_frame,
            "invalid field geometry creates no fence, lease, queued work or pixel writes");
    fixture.target->object_fds.clear();
  }
}

void VppInterleavedArgbChromaMatchesCompactedFields() {
  Fixture fixture;
  fixture.target = PrivateArgbOutput();
  fixture.driver.surfaces[2] = fixture.target;
  auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
  for (unsigned row = 0; row < 16; ++row)
    for (unsigned x = 0; x < 16; ++x)
      picture->planes[0][row * picture->pitch[0] + x] = 40 + row * 5 + x;
  for (unsigned row = 0; row < 8; ++row)
    for (unsigned x = 0; x < 16; x += 2) {
      picture->planes[1][row * picture->pitch[1] + x] = row % 2 ? 180 : 80;
      picture->planes[1][row * picture->pitch[1] + x + 1] = row % 2 ? 80 : 180;
    }
  const auto original = picture->storage;
  for (uint32_t field_order : {VA_TOP_FIELD_FIRST, VA_BOTTOM_FIELD_FIRST})
    for (uint8_t range : {VA_SOURCE_RANGE_FULL, VA_SOURCE_RANGE_REDUCED}) {
      VARectangle input{0, 0, 16, 12}, output{0, 0, 16, 8};
      VAProcPipelineParameterBuffer pipeline{};
      pipeline.surface = 1;
      pipeline.surface_region = &input; pipeline.output_region = &output;
      pipeline.input_surface_flag = pipeline.output_surface_flag = field_order;
      pipeline.input_color_properties.color_range = range;
      memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(), &pipeline, sizeof(pipeline));
      fixture.SubmitParameters();
      Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
              "colored interleaved ARGB conversion completes");
      for (unsigned parity = 0; parity < 2; ++parity) {
        // Independently materialize each physical Y/UV field contiguously.
        // The production path instead supplies doubled strides to swscale.
        std::vector<uint8_t> compact(16 * 9 + 64), expected(128 * 4 + 64);
        for (unsigned plane = 0; plane < 2; ++plane)
          for (unsigned row = 0; row < (plane ? 3U : 6U); ++row)
            memcpy(compact.data() + (plane ? 16 * 6 : 0) + row * 16,
                   picture->planes[plane] + (row * 2 + parity) * picture->pitch[plane], 16);
        SwsContext *oracle = sws_getContext(16, 6, AV_PIX_FMT_NV12, 16, 4,
            AV_PIX_FMT_BGRA, SWS_POINT, nullptr, nullptr, nullptr);
        Require(oracle != nullptr, "independent compact-field CSC context");
        const int *matrix = sws_getCoefficients(SWS_CS_DEFAULT);
        Require(sws_setColorspaceDetails(oracle, matrix, range == VA_SOURCE_RANGE_FULL,
                    matrix, 1, 0, 1 << 16, 1 << 16) == 0, "independent CSC range");
        const uint8_t *source[] = {compact.data(), compact.data() + 16 * 6, nullptr, nullptr};
        const int pitches[] = {16, 16, 0, 0};
        uint8_t *destination[] = {expected.data(), nullptr, nullptr, nullptr};
        const int destination_pitches[] = {128, 0, 0, 0};
        const int rows = sws_scale(oracle, source, pitches, 0, 6, destination, destination_pitches);
        sws_freeContext(oracle);
        Require(rows == 4, "independent compact-field CSC returns every row");
        for (unsigned row = 0; row < 4; ++row)
          Require(memcmp(fixture.target->planes[0] +
                             (row * 2 + parity) * fixture.target->pitch[0],
                         expected.data() + row * 128, 64) == 0,
                  "ARGB field chroma/row phase matches independently compacted fields exactly");
      }
      Require(picture->storage == original, "colored CSC preserves source fields");
    }
}

void VppFullRangeArgbUsesDeclaredInput() {
  Fixture fixture;
  fixture.target = PrivateArgbOutput();
  fixture.driver.surfaces[2] = fixture.target;
  auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
  constexpr uint8_t levels[] = {0, 16, 128, 235, 255};
  for (unsigned row = 0; row < picture->height; ++row)
    memset(picture->planes[0] + row * picture->pitch[0],
           levels[row % 5], picture->width);
  VAProcPipelineParameterBuffer pipeline = {};
  pipeline.surface = 1;
  pipeline.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
  pipeline.output_color_properties.color_range = VA_SOURCE_RANGE_FULL;
  memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(),
         &pipeline, sizeof(pipeline));
  fixture.SubmitParameters();
  // Render owns a value snapshot: later client buffer changes are irrelevant.
  pipeline.input_color_properties.color_range = VA_SOURCE_RANGE_REDUCED;
  memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(),
         &pipeline, sizeof(pipeline));
  Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
          "full-range ARGB conversion completes");
  for (unsigned row = 0; row < picture->height; ++row)
    for (unsigned column = 0; column < picture->width; ++column) {
      const uint8_t *pixel = fixture.target->planes[0] +
          row * fixture.target->pitch[0] + column * 4;
      // Neutral full-range YCbCr is exactly R=G=B=Y, independently of swscale.
      Require(pixel[0] == levels[row % 5] && pixel[1] == levels[row % 5] &&
                  pixel[2] == levels[row % 5] && pixel[3] == 255,
              "declared full-range neutral Y must remain the same RGB level");
    }
}

void VppColorRangeArgbReuseAndBob() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    Fixture fixture;
    fixture.target = PrivateArgbOutput();
    fixture.driver.surfaces[2] = fixture.target;
    const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    for (unsigned row = 0; row < picture->height; ++row)
      memset(picture->planes[0] + row * picture->pitch[0],
             row % 2 == 0 ? 16 : 235, picture->width);
    const auto original = picture->storage;
    VABufferID filter = mode == 0 ? VA_INVALID_ID : CreateBobFilter(
        fixture, mode == 2 ? VA_DEINTERLACING_BOTTOM_FIELD : 0);
    // Legacy swscale has a different rounding path for its final limited-range
    // row. Preserve that output exactly, rather than widening a pixel tolerance.
    // This fresh context never receives the new range setup or cached state.
    std::vector<uint8_t> legacy(128 * picture->height + 64, 0);
    const unsigned parity = mode == 2 ? 1 : 0;
    const int source_height = mode == 0 ? picture->height : picture->height / 2;
    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> reference(
        sws_getContext(picture->width, source_height, AV_PIX_FMT_NV12,
                       picture->width, picture->height, AV_PIX_FMT_BGRA,
                       SWS_POINT, nullptr, nullptr, nullptr), &sws_freeContext);
    Require(reference != nullptr, "construct independent default-range legacy reference");
    const uint8_t *input[] = {picture->planes[0] + parity * picture->pitch[0],
                             picture->planes[1] + parity * picture->pitch[1], nullptr, nullptr};
    const int input_pitch[] = {static_cast<int>(picture->pitch[0] * (mode == 0 ? 1 : 2)),
                               static_cast<int>(picture->pitch[1] * (mode == 0 ? 1 : 2)), 0, 0};
    uint8_t *output[] = {legacy.data(), nullptr, nullptr, nullptr};
    const int output_pitch[] = {128, 0, 0, 0};
    Require(sws_scale(reference.get(), input, input_pitch, 0, source_height,
                       output, output_pitch) == static_cast<int>(picture->height),
            "legacy range reference produces every row");
    for (uint8_t range : {VA_SOURCE_RANGE_FULL, VA_SOURCE_RANGE_REDUCED,
                         VA_SOURCE_RANGE_UNKNOWN, VA_SOURCE_RANGE_FULL}) {
      VAProcPipelineParameterBuffer pipeline = {};
      pipeline.surface = 1;
      pipeline.input_color_properties.color_range = range;
      pipeline.output_color_properties.color_range = VA_SOURCE_RANGE_UNKNOWN;
      pipeline.filters = mode == 0 ? nullptr : &filter;
      pipeline.num_filters = mode == 0 ? 0 : 1;
      memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(),
             &pipeline, sizeof(pipeline));
      fixture.SubmitParameters();
      Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
              "cached ARGB scaler accepts successive range/Bob requests");
      for (unsigned row = 0; row < picture->height; ++row)
        for (unsigned column = 0; column < picture->width; ++column) {
          const unsigned parity = mode == 0 ? row % 2 : mode - 1;
          const uint8_t expected = parity == 0 ? 16 : 235;
          const uint8_t *pixel = fixture.target->planes[0] +
              row * fixture.target->pitch[0] + column * 4;
          const bool exact = range == VA_SOURCE_RANGE_FULL
              ? pixel[0] == expected && pixel[1] == expected && pixel[2] == expected && pixel[3] == 255
              : memcmp(pixel, legacy.data() + row * 128 + column * 4, 4) == 0;
          Require(exact,
                  "full/limited/default range changes reset cached CSC for both Bob fields");
        }
      Require(picture->storage == original, "range conversion keeps source immutable");
    }
  }
}

void VppColorRangeNv12PreservesSamples() {
  for (bool bob : {false, true})
    for (uint8_t input : {VA_SOURCE_RANGE_UNKNOWN, VA_SOURCE_RANGE_REDUCED,
                          VA_SOURCE_RANGE_FULL})
      for (bool explicit_output : {false, true}) {
        Fixture fixture;
        const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
        const auto original = picture->storage;
        VABufferID filter = bob ? CreateBobFilter(fixture) : VA_INVALID_ID;
        VARectangle output = {0, 0, 8, 8};
        VAProcPipelineParameterBuffer pipeline = {};
        pipeline.surface = 1;
        pipeline.output_region = &output;
        pipeline.input_color_properties.color_range = input;
        pipeline.output_color_properties.color_range = explicit_output
            ? (input == VA_SOURCE_RANGE_FULL ? VA_SOURCE_RANGE_FULL : VA_SOURCE_RANGE_REDUCED)
            : VA_SOURCE_RANGE_UNKNOWN;
        pipeline.filters = bob ? &filter : nullptr;
        pipeline.num_filters = bob ? 1 : 0;
        memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(),
               &pipeline, sizeof(pipeline));
        fixture.SubmitParameters();
        Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
                "same-range NV12 copy/Bob needs no range conversion");
        for (unsigned row = 0; row < fixture.target->height; ++row)
          for (unsigned column = 0; column < fixture.target->width; ++column)
            Require(fixture.target->planes[0][row * fixture.target->pitch[0] + column] ==
                        (row < 8 && column < 8 ? 40 : input == VA_SOURCE_RANGE_FULL ? 0 : 16),
                    "NV12 preserves active samples and uses range-correct black padding");
        for (unsigned row = 0; row < fixture.target->height / 2; ++row)
          for (unsigned column = 0; column < fixture.target->width; ++column)
            Require(fixture.target->planes[1][row * fixture.target->pitch[1] + column] == 128,
                    "NV12 neutral chroma is independent of range");
        Require(picture->storage == original, "NV12 range handling never mutates the source");
      }
}

void VppColorRangeRejectsUnsupportedRequests() {
  for (bool argb : {false, true})
    for (uint8_t input : {0U, 1U, 2U, 3U, 255U})
      for (uint8_t output : {0U, 1U, 2U, 3U, 255U}) {
        const bool supported = input <= VA_SOURCE_RANGE_FULL && output <= VA_SOURCE_RANGE_FULL &&
            (argb ? output != VA_SOURCE_RANGE_REDUCED
                  : output == VA_SOURCE_RANGE_UNKNOWN ||
                    (input == VA_SOURCE_RANGE_FULL) == (output == VA_SOURCE_RANGE_FULL));
        if (supported) continue;
        Fixture fixture;
        if (argb) {
          fixture.target = PrivateArgbOutput();
          fixture.driver.surfaces[2] = fixture.target;
        }
        const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
        const auto original = fixture.target->storage;
        // An invalid replacement cannot retain an earlier valid snapshot.
        fixture.SubmitParameters();
        VAProcPipelineParameterBuffer pipeline = {};
        pipeline.surface = 1;
        pipeline.input_color_properties.color_range = input;
        pipeline.output_color_properties.color_range = output;
        memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(),
               &pipeline, sizeof(pipeline));
        Require(RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                    VA_STATUS_ERROR_INVALID_PARAMETER &&
                    !fixture.processor->have_vpp_parameters && !fixture.processor->vpp_frame &&
                    picture->vpp_readers == 0 && fixture.target->vpp_writers == 0 &&
                    fixture.driver.pending_vpp.empty() && fixture.target->storage == original,
                "unsupported range rejects before capture, queue, CPU ownership or pixels");
        Require(EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                    fixture.target->storage == original,
                "invalid range replacement cannot replay the old accepted request");
      }
}

void VppColorStandardsNv12AndRejections() {
  for (auto standard : {VAProcColorStandardBT601, VAProcColorStandardBT709})
    for (bool bob : {false, true})
      for (bool inherit : {false, true}) {
        Fixture fixture;
        VAProcPipelineCaps caps;
        memset(&caps, 0xff, sizeof(caps));
        Require(QueryVideoProcPipelineCaps(&fixture.context, 2, nullptr, 0, &caps) ==
                    VA_STATUS_SUCCESS && caps.num_input_color_standards == 2 &&
                    caps.num_output_color_standards == 2 &&
                    caps.input_color_standards[0] == VAProcColorStandardBT601 &&
                    caps.input_color_standards[1] == VAProcColorStandardBT709 &&
                    caps.output_color_standards[0] == VAProcColorStandardBT601 &&
                    caps.output_color_standards[1] == VAProcColorStandardBT709,
                "VPP publishes only supported, driver-owned color standards");
        auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
        const auto original = picture->storage;
        VABufferID filter = bob ? CreateBobFilter(fixture) : VA_INVALID_ID;
        VAProcPipelineParameterBuffer pipeline = {};
        pipeline.surface = 1;
        pipeline.surface_color_standard = standard;
        pipeline.output_color_standard = inherit ? VAProcColorStandardNone : standard;
        pipeline.filters = bob ? &filter : nullptr;
        pipeline.num_filters = bob ? 1 : 0;
        memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(), &pipeline, sizeof(pipeline));
        fixture.SubmitParameters();
        fixture.Complete(kTimestampStep, 40);
        Require(picture->storage == original && picture->vpp_readers == 0 &&
                    fixture.target->vpp_writers == 0,
                "same-standard NV12 preserves pixels and retires ownership");
      }
  for (auto input : {VAProcColorStandardNone, VAProcColorStandardBT601,
                     VAProcColorStandardBT709, VAProcColorStandardBT2020,
                     VAProcColorStandardExplicit})
    for (auto output : {VAProcColorStandardNone, VAProcColorStandardBT601,
                        VAProcColorStandardBT709, VAProcColorStandardSRGB}) {
      const bool supported =
          (input == VAProcColorStandardNone || input == VAProcColorStandardBT601 ||
           input == VAProcColorStandardBT709) &&
          (output == VAProcColorStandardNone || output == input);
      if (supported) continue;
      Fixture fixture;
      fixture.SubmitParameters();
      const auto untouched = fixture.target->storage;
      VAProcPipelineParameterBuffer pipeline = {};
      pipeline.surface = 1;
      pipeline.surface_color_standard = input;
      pipeline.output_color_standard = output;
      memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(), &pipeline, sizeof(pipeline));
      Require(RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                  VA_STATUS_ERROR_INVALID_PARAMETER &&
                  fixture.processor->vpp_source == VA_INVALID_SURFACE &&
                  fixture.target->storage == untouched &&
                  fixture.target->vpp_writers == 0 && fixture.driver.pending_vpp.empty(),
              "unimplemented gamut/transfer/explicit color requests reject before capture");
    }
}

// Independent Y'CbCr equations, not swscale or the driver's coefficient table.
// Fixed-point conversion may differ by at most two 8-bit levels from rounding
// the real-valued standard; colored samples distinguish 601 from 709 by much more.
static std::array<int, 3> ReferenceRgb(unsigned y, unsigned u, unsigned v,
                                       bool full, bool bt709) {
  const double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114;
  const double yy = full ? y : (static_cast<double>(y) - 16) * 255 / 219;
  const double cb = (static_cast<double>(u) - 128) * (full ? 1 : 255.0 / 224);
  const double cr = (static_cast<double>(v) - 128) * (full ? 1 : 255.0 / 224);
  const double red = yy + 2 * (1 - kr) * cr;
  const double blue = yy + 2 * (1 - kb) * cb;
  const double green = (yy - kr * red - kb * blue) / (1 - kr - kb);
  const auto level = [](double x) { return std::clamp<int>(std::lround(x), 0, 255); };
  return {level(blue), level(green), level(red)};
}

void VppColorStandardsArgbPixelsAndReuse() {
  Fixture fixture;
  fixture.target = PrivateArgbOutput();
  fixture.driver.surfaces[2] = fixture.target;
  auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
  const unsigned samples[][3] = {{16,128,128}, {235,128,128}, {81,90,240},
      {63,102,240}, {128,96,160}, {80,180,210}};
  const std::pair<VAProcColorStandardType, VAProcColorStandardType> standards[] = {
      {VAProcColorStandardBT709, VAProcColorStandardBT709},
      {VAProcColorStandardBT601, VAProcColorStandardBT601},
      {VAProcColorStandardNone, VAProcColorStandardNone},
      {VAProcColorStandardBT709, VAProcColorStandardNone}};
  for (const auto &selection : standards)
    for (bool full : {false, true})
      for (const auto &sample : samples)
        for (unsigned mode = 0; mode != 3; ++mode) {
          const auto standard = selection.first;
          for (unsigned row = 0; row < picture->height; ++row)
            memset(picture->planes[0] + row * picture->pitch[0],
                   sample[0] + 12 * (row % 2), picture->width);
          for (unsigned row = 0; row < picture->height / 2; ++row)
            for (unsigned x = 0; x < picture->width; x += 2) {
              const int sign = standard == VAProcColorStandardBT709 ?
                  ((x / 2) % 2 ? -1 : 1) : 0;
              picture->planes[1][row * picture->pitch[1] + x] = sample[1] + 7 * sign;
              picture->planes[1][row * picture->pitch[1] + x + 1] = sample[2] + 9 * sign;
            }
          const auto original = picture->storage;
          VABufferID filter = mode == 0 ? VA_INVALID_ID :
              CreateBobFilter(fixture, mode == 2 ? VA_DEINTERLACING_BOTTOM_FIELD : 0);
          VAProcPipelineParameterBuffer pipeline = {};
          pipeline.surface = 1;
          pipeline.surface_color_standard = standard;
          pipeline.output_color_standard = selection.second;
          pipeline.input_color_properties.color_range = full ? VA_SOURCE_RANGE_FULL : VA_SOURCE_RANGE_REDUCED;
          pipeline.filters = mode == 0 ? nullptr : &filter;
          pipeline.num_filters = mode == 0 ? 0 : 1;
          memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(), &pipeline, sizeof(pipeline));
          fixture.SubmitParameters();
          Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
                  "declared matrix completes ordinary/Bob ARGB conversion");
          for (unsigned row = 0; row < picture->height; ++row) {
            for (unsigned x = 0; x < picture->width; ++x) {
              const int sign = standard == VAProcColorStandardBT709 ?
                  ((x / 2) % 2 ? -1 : 1) : 0;
              const auto rgb = ReferenceRgb(sample[0] + 12 * (mode == 0 ? row % 2 : mode - 1),
                  sample[1] + 7 * sign, sample[2] + 9 * sign, full,
                  standard == VAProcColorStandardBT709);
              const auto *pixel = fixture.target->planes[0] + row * fixture.target->pitch[0] + 4 * x;
              for (unsigned c = 0; c != 3; ++c) {
                if (std::abs(static_cast<int>(pixel[c]) - rgb[c]) > 2)
                  fprintf(stderr, "CSC mismatch standard=%u full=%u mode=%u sample=%u,%u,%u row=%u x=%u channel=%u actual=%u expected=%d\n",
                      static_cast<unsigned>(standard), full, mode, sample[0], sample[1], sample[2], row, x, c, pixel[c], rgb[c]);
                Require(std::abs(static_cast<int>(pixel[c]) - rgb[c]) <= 2,
                        "601/709 RGB agrees with independent color equations");
              }
              Require(pixel[3] == 255, "CSC retains opaque alpha");
            }
          }
          Require(picture->storage == original && picture->vpp_readers == 0 &&
                      fixture.target->vpp_writers == 0,
                  "matrix changes preserve source and release owners");
          if (mode != 0)
            Require(DestroyBuffer(&fixture.context, filter) == VA_STATUS_SUCCESS,
                    "retire completed Bob filter");
        }
}

void VppColorStandardQueuedSnapshotAndCancellation() {
  for (bool cancel : {false, true}) {
    Fixture fixture;
    fixture.driver.StopWorkers();
    fixture.driver.stopping = false;
    fixture.target = PrivateArgbOutput();
    fixture.driver.surfaces[2] = fixture.target;
    auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    for (unsigned row = 0; row < picture->height / 2; ++row)
      for (unsigned x = 0; x < picture->width; x += 2) {
        picture->planes[1][row * picture->pitch[1] + x] = 102;
        picture->planes[1][row * picture->pitch[1] + x + 1] = 240;
      }
    const auto original = picture->storage, untouched = fixture.target->storage;
    picture->ready = false;
    VAProcPipelineParameterBuffer pipeline = {};
    pipeline.surface = 1;
    pipeline.surface_color_standard = pipeline.output_color_standard = VAProcColorStandardBT709;
    memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(), &pipeline, sizeof(pipeline));
    fixture.SubmitParameters();
    pipeline.surface_color_standard = pipeline.output_color_standard = VAProcColorStandardBT601;
    memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(), &pipeline, sizeof(pipeline));
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS && fixture.driver.pending_vpp.size() == 1 &&
                DestroyBuffer(&fixture.context, fixture.parameters) == VA_STATUS_SUCCESS,
            "pending color matrix owns its value, not client buffer lifetime");
    if (cancel) fixture.decoder->Reset(); else picture->ready = true;
    fixture.driver.vpp_worker = std::thread([&] { RunVppWorker(&fixture.driver); });
    Require(SyncSurface2(&fixture.context, 2, 1000000000ULL) ==
                (cancel ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS) &&
                fixture.driver.pending_vpp.empty() && picture->vpp_readers == 0 &&
                fixture.target->vpp_writers == 0 && picture->storage == original,
            "queued color conversion retires source/target owners on success and reset");
    if (cancel) Require(fixture.target->storage == untouched, "canceled matrix writes no pixels");
    else {
      const auto rgb = ReferenceRgb(40, 102, 240, false, true);
      for (unsigned row = 0; row < picture->height; ++row)
        for (unsigned x = 0; x < picture->width; ++x) {
          const auto *pixel = fixture.target->planes[0] + row * fixture.target->pitch[0] + 4 * x;
          for (unsigned c = 0; c != 3; ++c)
            Require(std::abs(static_cast<int>(pixel[c]) - rgb[c]) <= 2,
                    "worker uses captured BT709 after context/client reset");
        }
    }
  }
}

void VppColorRangeQueuedSnapshotAndCancellation() {
  for (bool cancel : {false, true})
    for (unsigned mode = 0; mode < 3; ++mode) {
      Fixture fixture;
      fixture.driver.StopWorkers();
      fixture.driver.stopping = false;
      fixture.target = PrivateArgbOutput();
      fixture.driver.surfaces[2] = fixture.target;
      const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
      for (unsigned row = 0; row < picture->height; ++row)
        memset(picture->planes[0] + row * picture->pitch[0], 16, picture->width);
      const auto original = picture->storage;
      const auto untouched = fixture.target->storage;
      picture->ready = false;
      VABufferID filter = mode == 0 ? VA_INVALID_ID : CreateBobFilter(
          fixture, mode == 2 ? VA_DEINTERLACING_BOTTOM_FIELD : 0);
      VAProcPipelineParameterBuffer pipeline = {};
      pipeline.surface = 1;
      pipeline.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
      pipeline.filters = mode == 0 ? nullptr : &filter;
      pipeline.num_filters = mode == 0 ? 0 : 1;
      memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(),
             &pipeline, sizeof(pipeline));
      fixture.SubmitParameters();
      Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                  fixture.driver.pending_vpp.size() == 1,
              "cold full-range source queues a value-owned conversion");
      Require(DestroyBuffer(&fixture.context, fixture.parameters) == VA_STATUS_SUCCESS,
              "pending range does not borrow the client buffer");
      if (cancel) fixture.decoder->Reset();
      else picture->ready = true;
      fixture.driver.vpp_worker = std::thread([&] { RunVppWorker(&fixture.driver); });
      Require(SyncSurface2(&fixture.context, 2, 1000000000ULL) ==
                  (cancel ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS) &&
                  fixture.driver.pending_vpp.empty() && picture->vpp_readers == 0 &&
                  fixture.target->vpp_writers == 0 && picture->storage == original,
              "full-range worker success/cancellation preserves source and releases owners");
      if (cancel) {
        Require(fixture.target->storage == untouched,
                "canceled color conversion publishes no pixels");
      } else {
        for (unsigned row = 0; row < fixture.target->height; ++row)
          for (unsigned column = 0; column < fixture.target->width; ++column) {
            const uint8_t *pixel = fixture.target->planes[0] +
                row * fixture.target->pitch[0] + column * 4;
            Require(pixel[0] == 16 && pixel[1] == 16 && pixel[2] == 16 && pixel[3] == 255,
                    "queued CSC/Bob retains full-range interpretation after End resets context");
          }
      }
    }
}

void SelectedRtFormatSurvivesConfigAndContextLifetime() {
  for (uint32_t format : {static_cast<uint32_t>(VA_RT_FORMAT_YUV420),
                         static_cast<uint32_t>(VA_RT_FORMAT_RGB32),
                         static_cast<uint32_t>(VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32)}) {
    Fixture fixture;
    // This group completes VPP synchronously; keep exact zero-timeout status
    // checks independent of admission by an otherwise idle worker.
    fixture.driver.StopWorkers();
    fixture.driver.stopping = false;
    fixture.driver.next_context = 3;
    auto argb = PrivateArgbOutput();
    fixture.target->rt_format = VA_RT_FORMAT_YUV420;
    fixture.driver.surfaces[3] = argb;
    fixture.driver.next_surface = 4;
    VAConfigAttrib requested = {VAConfigAttribRTFormat, format};
    VAConfigID config = VA_INVALID_ID;
    Require(CreateConfig(&fixture.context, VAProfileNone, VAEntrypointVideoProc,
                          &requested, 1, &config) == VA_STATUS_SUCCESS,
            "create an explicit selected-format VPP configuration");
    VAConfigAttrib attributes[16] = {};
    VAProfile profile = VAProfileH264High;
    VAEntrypoint entrypoint = VAEntrypointVLD;
    int count = 0;
    Require(QueryConfigAttributes(&fixture.context, config, &profile, &entrypoint,
                                  attributes, &count) == VA_STATUS_SUCCESS &&
                profile == VAProfileNone && entrypoint == VAEntrypointVideoProc &&
                count > 0 && count <= 16 &&
                std::any_of(attributes, attributes + count, [&](const auto &attribute) {
                  return attribute.type == VAConfigAttribRTFormat && attribute.value == format;
                }),
            "config query reports the selected mask, not the broader capability mask");
    VASurfaceAttrib surfaces[16] = {};
    unsigned surface_count = 16;
    Require(QuerySurfaceAttributes(&fixture.context, config, surfaces, &surface_count) ==
                    VA_STATUS_SUCCESS && surface_count <= 16,
            "query selected config surface formats");
    std::unordered_set<uint32_t> pixels;
    for (unsigned index = 0; index < surface_count; ++index)
      if (surfaces[index].type == VASurfaceAttribPixelFormat)
        pixels.insert(surfaces[index].value.value.i);
    Require(pixels.count(VA_FOURCC_NV12) == ((format & VA_RT_FORMAT_YUV420) != 0) &&
                pixels.count(VA_FOURCC_ARGB) == ((format & VA_RT_FORMAT_RGB32) != 0) &&
                pixels.count(VA_FOURCC_BGRA) == ((format & VA_RT_FORMAT_RGB32) != 0) &&
                pixels.size() == static_cast<size_t>((format & VA_RT_FORMAT_YUV420 ? 1 : 0) +
                                                     (format & VA_RT_FORMAT_RGB32 ? 2 : 0)),
            "surface attributes expose only selected output pixel formats");
    VASurfaceID valid = (format & VA_RT_FORMAT_YUV420) ? 2 : 3;
    const VASurfaceID wrong = valid == 2 ? 3 : 2;
    VAContextID processor = VA_INVALID_ID;
    if (format != (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32)) {
      VASurfaceID targets[] = {valid, wrong};
      const auto next = fixture.driver.next_context;
      const auto contexts = fixture.driver.contexts.size();
      Require(CreateContext(&fixture.context, config, 16, 16, 0, targets, 2, &processor) ==
                      VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT && processor == VA_INVALID_ID &&
                  fixture.driver.next_context == next && fixture.driver.contexts.size() == contexts,
              "CreateContext rejects an opposite-format target atomically");
    }
    Require(CreateContext(&fixture.context, config, 16, 16, 0, &valid, 1, &processor) ==
                VA_STATUS_SUCCESS,
            "CreateContext accepts the selected output format");
    VAProcPipelineCaps caps = {};
    Require(QueryVideoProcPipelineCaps(&fixture.context, processor, nullptr, 0, &caps) ==
                    VA_STATUS_SUCCESS && caps.num_input_pixel_formats == 1 &&
                caps.input_pixel_format[0] == VA_FOURCC_NV12 &&
                caps.num_output_pixel_formats == pixels.size() &&
                std::all_of(caps.output_pixel_format,
                    caps.output_pixel_format + caps.num_output_pixel_formats,
                    [&](uint32_t pixel) { return pixels.count(pixel) == 1; }),
            "pipeline caps retain NV12 input independently of selected output formats");
    Require(DestroyConfig(&fixture.context, config) == VA_STATUS_SUCCESS,
            "destroy config while its independently configured context remains live");
    VAProcPipelineCaps retained_caps = {};
    Require(QueryVideoProcPipelineCaps(&fixture.context, processor, nullptr, 0, &retained_caps) ==
                    VA_STATUS_SUCCESS && retained_caps.num_output_pixel_formats == pixels.size() &&
                std::all_of(retained_caps.output_pixel_format,
                    retained_caps.output_pixel_format + retained_caps.num_output_pixel_formats,
                    [&](uint32_t pixel) { return pixels.count(pixel) == 1; }),
            "context keeps its selected pipeline caps after the config is destroyed");
    if (format != (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32)) {
      const auto target = fixture.driver.surfaces.at(wrong);
      const auto storage = target->storage;
      const auto transaction = target->picture_transaction;
      const auto identity = fixture.driver.next_decode_identity;
      const auto timestamp = target->expected_timestamp;
      const auto completed = target->frame_timestamp;
      const bool ready = target->ready, failed = target->failed;
      Require(BeginPicture(&fixture.context, processor, wrong) ==
                      VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT &&
                  target->storage == storage && target->picture_transaction == transaction &&
                  target->expected_timestamp == timestamp && target->frame_timestamp == completed &&
                  target->ready == ready && target->failed == failed &&
                  target->vpp_readers == 0 && target->vpp_writers == 0 &&
                  fixture.driver.next_decode_identity == identity &&
                  fixture.driver.contexts.at(processor)->target == VA_INVALID_SURFACE,
              "Begin rejects wrong output before target mutation, even after config destruction");
    }
    VAProcPipelineParameterBuffer pipeline = {};
    pipeline.surface = 1;
    VABufferID buffer = VA_INVALID_ID;
    if (valid == 3) {
      // FFmpeg forwards decoded LEFT chroma metadata even for an RGB output;
      // do not confuse ordinary client metadata with a new CSC capability.
      pipeline.input_color_properties.chroma_sample_location =
          pipeline.output_color_properties.chroma_sample_location =
              VA_CHROMA_SITING_VERTICAL_CENTER | VA_CHROMA_SITING_HORIZONTAL_LEFT;
      pipeline.input_color_properties.color_range = VA_SOURCE_RANGE_REDUCED;
      pipeline.output_color_properties.color_range = VA_SOURCE_RANGE_FULL;
    }
    Require(CreateBuffer(&fixture.context, processor, VAProcPipelineParameterBufferType,
                          sizeof(pipeline), 1, &pipeline, &buffer) == VA_STATUS_SUCCESS &&
                BeginPicture(&fixture.context, processor, valid) == VA_STATUS_SUCCESS &&
                RenderPicture(&fixture.context, processor, &buffer, 1) == VA_STATUS_SUCCESS &&
                EndPicture(&fixture.context, processor) == VA_STATUS_SUCCESS &&
                SyncSurface2(&fixture.context, valid, 0) == VA_STATUS_SUCCESS &&
                fixture.driver.surfaces.at(valid)->ready &&
                !fixture.driver.surfaces.at(valid)->failed,
            "selected output still permits NV12 input and actual VPP completion after config destruction");
    if (valid == 3) {
      // An independent fresh library conversion checks format admission/copy,
      // not a new matrix/range algorithm. Preserve swscale's actual integer
      // rounding/dither rather than assuming one constant RGB value per frame.
      std::array<uint8_t, 16 * 24 + 64> nv12;
      std::fill_n(nv12.data(), 16 * 16, 40);
      std::fill(nv12.begin() + 16 * 16, nv12.end(), 128);
      std::array<uint8_t, 16 * 16 * 4 + 64> expected = {};
      SwsContext *oracle = sws_getContext(16, 16, AV_PIX_FMT_NV12, 16, 16,
          AV_PIX_FMT_BGRA, SWS_POINT, nullptr, nullptr, nullptr);
      Require(oracle != nullptr, "allocate a separate reference conversion context");
      const uint8_t *input[] = {nv12.data(), nv12.data() + 16 * 16, nullptr, nullptr};
      const int input_pitch[] = {16, 16, 0, 0};
      uint8_t *output[] = {expected.data(), nullptr, nullptr, nullptr};
      const int output_pitch[] = {16 * 4, 0, 0, 0};
      const int rows = sws_scale(oracle, input, input_pitch, 0, 16, output, output_pitch);
      sws_freeContext(oracle);
      Require(rows == 16, "independent reference contains every output row");
      for (unsigned row = 0; row < 16; ++row)
        Require(memcmp(argb->planes[0] + row * argb->pitch[0],
                       expected.data() + row * 64, 64) == 0,
                "selected ARGB output matches every active byte of the independent conversion");
    }
    Require(DestroyContext(&fixture.context, processor) == VA_STATUS_SUCCESS,
            "selected-format VPP context closes normally");
    VAConfigAttrib decode_format = {VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420};
    Require(CreateConfig(&fixture.context, VAProfileH264Main, VAEntrypointVLD,
                          &decode_format, 1, &config) == VA_STATUS_SUCCESS,
            "create selected decode config without opening a decoder");
    VASurfaceID wrong_decode = 3;
    processor = VA_INVALID_ID;
    const auto next = fixture.driver.next_context;
    const auto opens = device_open_attempts;
    Require(CreateContext(&fixture.context, config, 16, 16, 0, &wrong_decode, 1, &processor) ==
                    VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT && processor == VA_INVALID_ID &&
                fixture.driver.next_context == next,
            "wrong decode output rejects before attempting any DIL device open");
    Require(device_open_attempts == opens,
            "wrong decode output never calls the device-free open failure stub");
    Require(DestroyConfig(&fixture.context, config) == VA_STATUS_SUCCESS,
            "unused decode config teardown remains ordinary");
  }
}

void FillFieldRows(Surface &picture) {
  for (unsigned plane = 0; plane < 2; ++plane)
    for (unsigned row = 0; row < (plane == 0 ? 16U : 8U); ++row)
      for (unsigned column = 0; column < 16; ++column)
        picture.planes[plane][row * picture.pitch[plane] + column] =
            static_cast<uint8_t>((plane == 0 ? 20 : 80) + row * 7 + column);
}

void RequireFieldRows(const Surface &target, unsigned parity, bool bob) {
  Require(target.ready && !target.failed &&
              target.expected_timestamp == kTimestampStep &&
              target.frame_timestamp == kTimestampStep,
          "Bob preserves the original source identity without synthesizing PTS");
  for (unsigned plane = 0; plane < 2; ++plane)
    for (unsigned row = 0; row < (plane == 0 ? 16U : 8U); ++row)
      for (unsigned column = 0; column < 16; ++column) {
        // Independent oracle: each selected physical field row occurs twice
        // in BOTH planes. UV parity cannot be inferred by halving a Y row.
        const unsigned selected_row = bob ? (row / 2) * 2 + parity : row;
        const auto expected = static_cast<uint8_t>(
            (plane == 0 ? 20 : 80) + selected_row * 7 + column);
        Require(target.planes[plane][row * target.pitch[plane] + column] == expected,
                "Bob copies only the selected physical Y and UV field");
      }
}

std::weak_ptr<Surface> BindCompletedVppSource(Fixture &fixture, bool direct) {
  auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
  if (direct) {
    picture = std::make_shared<Surface>();
    picture->width = picture->height = 16;
    picture->fourcc = VA_FOURCC_NV12;
    picture->direct_picture = true;
    picture->direct_backing = fixture.source;
    picture->decode_target = fixture.source;
    fixture.decoder->decoded_frames[kTimestampStep] = picture;
  } else {
    FillFieldRows(*picture);
  }
  FillFieldRows(*fixture.source);
  fixture.decoder->id = 1;
  fixture.decoder->profile = VAProfileH264Main;
  fixture.decoder->generation = 7;
  fixture.decoder->width = fixture.decoder->height = 16;
  fixture.source->direct_decode_eligible = direct;
  for (const auto &surface : {fixture.source, picture}) {
    surface->ready = true;
    surface->failed = false;
    surface->expected_timestamp = surface->frame_timestamp = kTimestampStep;
    surface->decode_identity = surface->trace_decode_identity = 17;
    surface->trace_context = 1;
    surface->trace_generation = 7;
    surface->trace_submission_ordinal = 9;
  }
  fixture.source->picture_transaction = 41;
  fixture.source->decode_picture = picture;
  return picture;
}

void CompletedDecoderCloseKeepsNewBobCapturesExact() {
  for (bool direct : {false, true}) {
    std::weak_ptr<Surface> public_lifetime, private_lifetime;
    {
      Fixture fixture;
      private_lifetime = BindCompletedVppSource(fixture, direct);
      const auto capture = [&](const char *phase) {
        Require(BeginPicture(&fixture.context, 2, 2) == VA_STATUS_SUCCESS,
                "begin a completed-source Bob request");
        Require(RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                    VA_STATUS_SUCCESS, phase);
      };
      public_lifetime = fixture.source;
      const auto public_pixels = fixture.source->storage;
      Require(DestroyContext(&fixture.context, 1) == VA_STATUS_SUCCESS &&
                  fixture.driver.contexts.count(1) == 0 && fixture.decoder->retired &&
                  fixture.decoder->device == nullptr && !fixture.source->destroyed &&
                  fixture.source->ready && !fixture.source->failed &&
                  fixture.source->storage == public_pixels,
              "successful consuming decoder close retains the live exact public picture");
      {
        const auto retained = private_lifetime.lock();
        Require(retained && !retained->direct_backing,
                "successful close retains an already promoted immutable picture before any new capture");
      }
      for (unsigned parity : {0U, 1U}) {
        VABufferID filter = CreateBobFilter(fixture,
            parity != 0 ? VA_DEINTERLACING_BOTTOM_FIELD : 0);
        SetPipelineFilters(fixture, &filter, 1);
        capture(parity == 0 ? "capture retired top field" : "capture retired bottom field");
        Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
                "a fresh Bob request can read a completed picture after decoder destruction");
        RequireFieldRows(*fixture.target, parity, true);
        Require(fixture.target->trace_context == 1 &&
                    fixture.target->trace_generation == 7 &&
                    fixture.target->trace_decode_identity == 17 &&
                    fixture.target->trace_submission_ordinal == 9 &&
                    fixture.source->storage == public_pixels &&
                    fixture.source->expected_timestamp == kTimestampStep &&
                    fixture.source->frame_timestamp == kTimestampStep &&
                    fixture.source->decode_identity == 17 &&
                    fixture.target->vpp_writers == 0 && fixture.driver.pending_vpp.empty(),
                "both fields preserve source token and identity without inventing a presentation clock");
      }

      auto next_decoder = std::make_shared<DecodeContext>();
      next_decoder->id = 4;
      next_decoder->profile = VAProfileH264Main;
      fixture.driver.contexts[4] = next_decoder;
      const uint64_t accepted_transaction = fixture.source->picture_transaction;
      Require(BeginPicture(&fixture.context, 4, 1) == VA_STATUS_SUCCESS &&
                  fixture.source->picture_transaction != accepted_transaction,
              "a speculative H264 Begin reserves a new transaction without replacing accepted pixels");
      VABufferID begin_filter = CreateBobFilter(fixture);
      SetPipelineFilters(fixture, &begin_filter, 1);
      capture("capture accepted retired binding after speculative Begin");
      Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
              "Begin alone preserves the completed accepted binding for a fresh Bob capture");
      RequireFieldRows(*fixture.target, 0, true);
      Require(DestroyContext(&fixture.context, 4) == VA_STATUS_SUCCESS,
              "discard the unsubmitted new context without changing the accepted picture");

      // Capture before a real newer writer replaces the public source. The
      // earlier immutable read survives, but a fresh read sees the new pixels.
      VABufferID filter = CreateBobFilter(fixture);
      SetPipelineFilters(fixture, &filter, 1);
      capture("capture retired binding before actual public rewrite");
      auto writer = std::make_shared<DecodeContext>();
      writer->video_process = true;
      fixture.driver.contexts[3] = writer;
      fixture.driver.surfaces[3] = Picture(0, 80);
      VAProcPipelineParameterBuffer replacement = {};
      replacement.surface = 3;
      VABufferID buffer = VA_INVALID_ID;
      Require(CreateBuffer(&fixture.context, 3, VAProcPipelineParameterBufferType,
                           sizeof(replacement), 1, &replacement, &buffer) == VA_STATUS_SUCCESS &&
                  BeginPicture(&fixture.context, 3, 1) == VA_STATUS_SUCCESS &&
                  RenderPicture(&fixture.context, 3, &buffer, 1) == VA_STATUS_SUCCESS &&
                  EndPicture(&fixture.context, 3) == VA_STATUS_SUCCESS,
              "a new ordinary VPP writer replaces the completed-close public binding");
      Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
              "the already captured immutable closed-context picture survives public reuse");
      RequireFieldRows(*fixture.target, 0, true);
      capture("capture replacement public binding after actual rewrite");
      Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                  fixture.target->ready && !fixture.target->failed &&
                  fixture.target->expected_timestamp == 0 && fixture.target->frame_timestamp == 0 &&
                  fixture.target->trace_decode_identity == 0,
              "new capture reads the replacement binding rather than resurrecting the closed picture");
      for (unsigned plane = 0; plane != 2; ++plane)
        for (unsigned row = 0; row != (plane == 0 ? 16U : 8U); ++row)
          for (unsigned column = 0; column != 16; ++column)
            Require(fixture.target->planes[plane][row * fixture.target->pitch[plane] + column] ==
                        (plane == 0 ? 80 : 128),
                    "replacement Y and UV bytes, not the retired field, reach a new output");
    }
    Require(public_lifetime.expired() && private_lifetime.expired(),
            "completed-close ownership cannot retain a public/private direct-backing cycle");
  }
}

void CapturedBobSurvivesSuccessfulDecoderClose() {
  for (bool direct : {false, true}) {
    Fixture fixture;
    BindCompletedVppSource(fixture, direct);
    VABufferID filter = CreateBobFilter(fixture);
    SetPipelineFilters(fixture, &filter, 1);
    Require(BeginPicture(&fixture.context, 2, 2) == VA_STATUS_SUCCESS &&
                RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                    VA_STATUS_SUCCESS,
            "capture the exact completed picture before decoder close");
    const auto captured = fixture.processor->vpp_frame;
    Require(captured && captured == fixture.source->decode_picture.lock(),
            "VPP capture owns the exact immutable decode picture");
    Require(DestroyContext(&fixture.context, 1) == VA_STATUS_SUCCESS &&
                fixture.decoder->retired && fixture.decoder->device == nullptr &&
                fixture.source->completed_decode_picture.picture == captured,
            "successful consuming close publishes the captured picture lease");
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
            "a pre-close capture remains valid after successful decoder close");
    RequireFieldRows(*fixture.target, 0, true);
    Require(fixture.target->trace_context == 1 &&
                fixture.target->trace_generation == 7 &&
                fixture.target->trace_decode_identity == 17 &&
                fixture.target->trace_submission_ordinal == 9 &&
                fixture.target->vpp_writers == 0 &&
                fixture.driver.pending_vpp.empty(),
            "pre-close capture preserves exact pixels and decode identity");
  }
}

void CapturedBobRejectsFailedDecoderClose() {
  Fixture fixture;
  BindCompletedVppSource(fixture, false);
  VABufferID filter = CreateBobFilter(fixture);
  SetPipelineFilters(fixture, &filter, 1);
  Require(BeginPicture(&fixture.context, 2, 2) == VA_STATUS_SUCCESS &&
              RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                  VA_STATUS_SUCCESS,
          "capture a completed picture before a failed decoder close");
  fixture.decoder->retained_close_status = BC_STS_ERROR;
  const auto untouched = fixture.target->storage;
  Require(DestroyContext(&fixture.context, 1) ==
              VA_STATUS_ERROR_OPERATION_FAILED &&
              fixture.decoder->retired && fixture.decoder->device == nullptr &&
              !fixture.source->completed_decode_picture.picture,
          "failed consuming close publishes no completed-picture lease");
  Require(EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_DECODING_ERROR &&
              fixture.target->failed && !fixture.target->ready &&
              fixture.target->storage == untouched &&
              fixture.target->vpp_writers == 0 &&
              fixture.driver.pending_vpp.empty(),
          "failed close cannot authorize captured pixels or a successful output");
}

void CapturedBobRejectsReboundOwnerAfterSuccessfulDecoderClose() {
  for (bool direct : {false, true}) {
    Fixture fixture;
    BindCompletedVppSource(fixture, direct);
    VABufferID filter = CreateBobFilter(fixture);
    SetPipelineFilters(fixture, &filter, 1);
    Require(BeginPicture(&fixture.context, 2, 2) == VA_STATUS_SUCCESS &&
                RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                    VA_STATUS_SUCCESS,
            "capture a completed picture before rebinding its public owner");
    const auto captured = fixture.processor->vpp_frame;
    Require(DestroyContext(&fixture.context, 1) == VA_STATUS_SUCCESS &&
                fixture.source->completed_decode_picture.picture == captured,
            "successful close initially publishes the captured picture lease");
    const auto untouched = fixture.target->storage;
    const uint64_t old_identity = fixture.source->decode_identity;

    // Model the ownership transition shared by every real public writer.  An
    // immutable captured picture remains allocated, but it no longer belongs
    // to the retired decoder's exact public binding and must not be committed.
    RevokeDecodeOwner(&fixture.driver, fixture.source.get());
    fixture.source->picture_transaction = NextDecodeIdentity(&fixture.driver);
    Require(!fixture.source->completed_decode_picture.picture &&
                fixture.source->decode_identity != old_identity,
            "a new public writer revokes the successful-close lease");
    Require(EndPicture(&fixture.context, 2) ==
                VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.target->failed && !fixture.target->ready &&
                fixture.target->storage == untouched &&
                fixture.target->vpp_writers == 0 &&
                fixture.driver.pending_vpp.empty(),
            "retired capture cannot cross a public owner rebind");
  }
}

void QueuedBobSurvivesSuccessfulDecoderClose() {
  for (bool direct : {false, true}) {
    Fixture fixture;
    fixture.driver.StopWorkers();
    BindCompletedVppSource(fixture, direct);
    const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    picture->ready = false;
    picture->frame_timestamp = 0;
    VABufferID filter = CreateBobFilter(fixture);
    SetPipelineFilters(fixture, &filter, 1);
    fixture.SubmitParameters();
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                fixture.driver.pending_vpp.size() == 1 &&
                picture->vpp_readers == 1 && fixture.target->vpp_writers == 1,
            "cold pre-close capture queues one exact Bob operation");

    // Model the decoder drain completing the captured picture while context
    // destruction consumes the hardware session.
    FillFieldRows(*picture);
    picture->ready = true;
    picture->frame_timestamp = kTimestampStep;
    Require(DestroyContext(&fixture.context, 1) == VA_STATUS_SUCCESS &&
                fixture.source->completed_decode_picture.picture == picture &&
                !PendingVppCanceled(&fixture.driver,
                                    fixture.driver.pending_vpp.front()),
            "successful close preserves the queued operation's exact lease");
    const PendingVpp operation = fixture.driver.pending_vpp.front();
    const VAStatus status = ProcessVpp(
        &fixture.driver.vpp_scaler, &fixture.driver.vpp_argb_staging,
        operation.source.get(), operation.target.get(), operation.source_region,
        operation.output_region, false, operation.deinterlace, false,
        operation.color_range, operation.preserve_fields);
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      ReleasePendingVpp(&fixture.driver, operation.sequence, status,
                        status == VA_STATUS_SUCCESS);
    }
    Require(status == VA_STATUS_SUCCESS && fixture.driver.pending_vpp.empty() &&
                picture->vpp_readers == 0 && fixture.target->vpp_writers == 0,
            "queued close-race conversion commits and releases both leases");
    RequireFieldRows(*fixture.target, 0, true);
  }
}

void IneligibleDecoderCloseCannotGrantNewBobReads() {
  for (unsigned mode = 0; mode != 9; ++mode) {
    Fixture fixture;
    BindCompletedVppSource(fixture, mode == 6);
    if (mode == 0)
      Require(fixture.decoder->Reset() == BC_STS_SUCCESS,
              "reset discards the old epoch before a later successful empty destroy");
    else if (mode == 1)
      fixture.decoder->retained_close_status = BC_STS_ERROR;
    else if (mode == 2)
      fixture.source->failed = true;
    else if (mode == 3)
      ++fixture.source->decode_identity;
    else if (mode == 4)
      fixture.source->frame_timestamp = 2 * kTimestampStep;
    else if (mode == 5)
      fixture.source->rejected_picture_transaction = fixture.source->picture_transaction;
    else if (mode == 7)
      ++fixture.source->trace_generation;
    else if (mode == 8)
      ++fixture.source->trace_context;
    const auto pixels = fixture.source->storage;
    if (mode == 6) {
      allocation_fault::failed_size = 0;
      allocation_fault::only_size = fixture.source->storage.size();
      allocation_fault::fail_next = true;
    }
    VAStatus closed;
    try {
      closed = DestroyContext(&fixture.context, 1);
    } catch (...) {
      allocation_fault::fail_next = false;
      allocation_fault::only_size = 0;
      throw;
    }
    allocation_fault::fail_next = false;
    allocation_fault::only_size = 0;
    if (mode == 6)
      Require((closed == VA_STATUS_ERROR_ALLOCATION_FAILED ||
               closed == VA_STATUS_ERROR_OPERATION_FAILED) &&
                  allocation_fault::failed_size == pixels.size(),
              "failed private promotion propagates failure instead of publishing a retained read");
    else
      Require(closed == (mode == 1 ? VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS),
              "completed-close controls distinguish reset, failed close and malformed public binding");
    VABufferID filter = CreateBobFilter(fixture);
    SetPipelineFilters(fixture, &filter, 1);
    const auto untouched = fixture.target->storage;
    Require(BeginPicture(&fixture.context, 2, 2) == VA_STATUS_SUCCESS &&
                RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                    VA_STATUS_ERROR_DECODING_ERROR &&
                !fixture.processor->have_vpp_parameters &&
                EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                fixture.target->storage == untouched && !fixture.target->ready &&
                fixture.target->failed && fixture.source->storage == pixels &&
                fixture.target->vpp_writers == 0 && fixture.driver.pending_vpp.empty(),
            "reset, failure, mismatch, rejection and failed promotion never grant a new public read");
  }
}

void BobCapabilityEnumerationAndCapacity() {
  Fixture fixture;
  unsigned count = 0;
  Require(QueryVideoProcFilters(&fixture.context, 2, nullptr, &count) ==
                  VA_STATUS_SUCCESS && count == 1,
          "null filter array queries the one implemented filter count");
  std::array<VAProcFilterType, 3> filters = {{VAProcFilterCount,
                                          VAProcFilterCount, VAProcFilterCount}};
  const auto old_filters = filters;
  count = 0;
  Require(QueryVideoProcFilters(&fixture.context, 2, filters.data() + 1, &count) ==
                  VA_STATUS_ERROR_MAX_NUM_EXCEEDED && count == 1 &&
              filters == old_filters,
          "short filter array returns the required size without writing canaries");
  Require(QueryVideoProcFilters(&fixture.context, 2, filters.data() + 1, &count) ==
                  VA_STATUS_SUCCESS && count == 1 &&
              filters[0] == VAProcFilterCount &&
              filters[1] == VAProcFilterDeinterlacing &&
              filters[2] == VAProcFilterCount,
          "filter enumeration writes exactly one deinterlacing entry");
  Require(QueryVideoProcFilters(&fixture.context, 2, nullptr, nullptr) ==
                  VA_STATUS_ERROR_INVALID_PARAMETER,
          "filter count pointer remains mandatory");

  count = 0;
  Require(QueryVideoProcFilterCaps(&fixture.context, 2,
              VAProcFilterDeinterlacing, nullptr, &count) == VA_STATUS_SUCCESS &&
              count == 1,
          "null deinterlacing output queries the one Bob capability");
  std::array<VAProcFilterCapDeinterlacing, 3> caps;
  memset(caps.data(), 0xa5, sizeof(caps));
  const auto old_caps = caps;
  count = 0;
  Require(QueryVideoProcFilterCaps(&fixture.context, 2,
              VAProcFilterDeinterlacing, caps.data() + 1, &count) ==
                  VA_STATUS_ERROR_MAX_NUM_EXCEEDED && count == 1 &&
              memcmp(caps.data(), old_caps.data(), sizeof(caps)) == 0,
          "short Bob-capability array preserves all caller storage");
  Require(QueryVideoProcFilterCaps(&fixture.context, 2,
              VAProcFilterDeinterlacing, caps.data() + 1, &count) ==
                  VA_STATUS_SUCCESS && count == 1 &&
              caps[1].type == VAProcDeinterlacingBob &&
              memcmp(&caps[0], &old_caps[0], sizeof(caps[0])) == 0 &&
              memcmp(&caps[2], &old_caps[2], sizeof(caps[2])) == 0,
          "Bob capability query writes one exact, isolated record");
  for (uint32_t reserved : caps[1].va_reserved)
    Require(reserved == 0, "Bob capability reserved fields are initialized");
  count = 7;
  Require(QueryVideoProcFilterCaps(&fixture.context, 2,
              VAProcFilterNoiseReduction, caps.data() + 1, &count) ==
                  VA_STATUS_ERROR_UNSUPPORTED_FILTER && count == 0,
          "unimplemented filter types remain unsupported");
}

void BobPipelineFilterValidation() {
  for (uint32_t flags : std::array<uint32_t, 4>{{0U, VA_DEINTERLACING_BOTTOM_FIELD_FIRST,
                         VA_DEINTERLACING_BOTTOM_FIELD,
                         VA_DEINTERLACING_BOTTOM_FIELD_FIRST |
                             VA_DEINTERLACING_BOTTOM_FIELD}}) {
    Fixture fixture;
    VABufferID filter = CreateBobFilter(fixture, flags);
    VAProcPipelineCaps caps = {};
    Require(QueryVideoProcPipelineCaps(&fixture.context, 2, &filter, 1, &caps) ==
                    VA_STATUS_SUCCESS && caps.num_forward_references == 0 &&
                caps.num_backward_references == 0 &&
                caps.input_pixel_format[0] == VA_FOURCC_NV12 &&
                caps.output_pixel_format[0] == VA_FOURCC_NV12,
            "all supported Bob field/order flags need no temporal reference frames");
  }
  {
    // FFmpeg initializes the semantic filter fields, but need not zero this
    // reserved ABI padding. It must not become an extra Bob request flag.
    Fixture fixture;
    FillFieldRows(*fixture.decoder->decoded_frames.at(kTimestampStep));
    VABufferID filter = CreateBobFilter(fixture, VA_DEINTERLACING_BOTTOM_FIELD);
    auto parameters = BobParameters(VA_DEINTERLACING_BOTTOM_FIELD);
    for (uint32_t &reserved : parameters.va_reserved)
      reserved = 0xa5a5a5a5U;
    memcpy(fixture.driver.buffers.at(filter).data.data(), &parameters,
           sizeof(parameters));
    VAProcPipelineCaps caps = {};
    Require(QueryVideoProcPipelineCaps(&fixture.context, 2, &filter, 1, &caps) ==
                VA_STATUS_SUCCESS,
            "FFmpeg reserved-filter-padding compatibility: capabilities remain Bob");
    SetPipelineFilters(fixture, &filter, 1);
    fixture.SubmitParameters();
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
            "FFmpeg reserved-filter-padding compatibility: semantic Bob still completes");
    RequireFieldRows(*fixture.target, 1, true);
  }
  for (unsigned failure = 0; failure < 15; ++failure) {
    Fixture fixture;
    VABufferID filter = CreateBobFilter(fixture);
    Buffer &buffer = fixture.driver.buffers.at(filter);
    auto parameters = BobParameters();
    switch (failure) {
      case 0: parameters.algorithm = VAProcDeinterlacingWeave; break;
      case 1: parameters.algorithm = VAProcDeinterlacingMotionAdaptive; break;
      case 2: parameters.type = VAProcFilterNoiseReduction; break;
      case 3: parameters.flags = VA_DEINTERLACING_ONE_FIELD; break;
      case 4: parameters.flags = VA_DEINTERLACING_FMD_ENABLE; break;
      case 5: parameters.flags = VA_DEINTERLACING_SCD_ENABLE; break;
      case 6: parameters.flags = UINT32_MAX; break;
      case 7:
        ++buffer.element_size;
        buffer.data.resize(buffer.element_size);
        break;
      case 8: buffer.type = VAPictureParameterBufferType; break;
      case 9: buffer.context = 1; break;
      case 10:
        buffer.elements = 2;
        buffer.data.resize(2 * sizeof(parameters));
        break;
      case 11:
        --buffer.element_size;
        buffer.data.resize(buffer.element_size);
        break;
      case 12: filter = VA_INVALID_ID; break;
      case 13: {
        void *mapped = nullptr;
        Require(MapBuffer(&fixture.context, filter, &mapped) == VA_STATUS_SUCCESS,
                "map the Bob buffer for a borrowed-input rejection");
        break;
      }
      case 14: buffer.data_failed = true; break;
    }
    memcpy(buffer.data.data(), &parameters,
           std::min(buffer.data.size(), sizeof(parameters)));
    VAProcPipelineCaps caps;
    memset(&caps, 0xa5, sizeof(caps));
    const VAProcPipelineCaps original = caps;
    Require(QueryVideoProcPipelineCaps(&fixture.context, 2, &filter, 1, &caps) !=
                    VA_STATUS_SUCCESS &&
                memcmp(&caps, &original, sizeof(caps)) == 0,
            "invalid Bob filter rejects before publishing pipeline capabilities");
    SetPipelineFilters(fixture, &filter, 1);
    Require(BeginPicture(&fixture.context, 2, 2) == VA_STATUS_SUCCESS &&
                RenderPicture(&fixture.context, 2, &fixture.parameters, 1) !=
                    VA_STATUS_SUCCESS && !fixture.processor->have_vpp_parameters,
            "the execution path rejects the same invalid or mapped Bob filter");
    if (failure == 13)
      Require(UnmapBuffer(&fixture.context, filter) == VA_STATUS_SUCCESS,
              "release the mapped invalid-filter control");
  }
  Fixture fixture;
  std::array<VABufferID, 2> filters = {{CreateBobFilter(fixture),
                                      CreateBobFilter(fixture)}};
  VAProcPipelineCaps caps = {};
  Require(QueryVideoProcPipelineCaps(&fixture.context, 2, filters.data(), 2, &caps) !=
              VA_STATUS_SUCCESS,
          "more than one Bob filter is not an advertised pipeline");
  SetPipelineFilters(fixture, filters.data(), filters.size());
  Require(BeginPicture(&fixture.context, 2, 2) == VA_STATUS_SUCCESS &&
              RenderPicture(&fixture.context, 2, &fixture.parameters, 1) !=
                  VA_STATUS_SUCCESS,
          "execution rejects an unsupported multi-filter pipeline");
}

void BobImmediatePixelsAndWeaveReset() {
  Fixture fixture;
  const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
  FillFieldRows(*picture);
  const auto original = picture->storage;
  for (uint32_t flags : std::array<uint32_t, 4>{{0U, VA_DEINTERLACING_BOTTOM_FIELD,
                         VA_DEINTERLACING_BOTTOM_FIELD_FIRST,
                         VA_DEINTERLACING_BOTTOM_FIELD_FIRST |
                             VA_DEINTERLACING_BOTTOM_FIELD}}) {
    VABufferID filter = CreateBobFilter(fixture, flags);
    SetPipelineFilters(fixture, &filter, 1);
    VAProcPipelineParameterBuffer pipeline;
    auto &pipeline_buffer = fixture.driver.buffers.at(fixture.parameters);
    memcpy(&pipeline, pipeline_buffer.data.data(), sizeof(pipeline));
    // A deinterlacing filter explicitly chooses the output field. Opposite
    // legacy pipeline hints must not silently replace that captured choice.
    pipeline.filter_flags = (flags & VA_DEINTERLACING_BOTTOM_FIELD) != 0
                                ? VA_TOP_FIELD : VA_BOTTOM_FIELD;
    memcpy(pipeline_buffer.data.data(), &pipeline, sizeof(pipeline));
    fixture.SubmitParameters();
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
            "complete a selected Bob field through the public VPP API");
    RequireFieldRows(*fixture.target,
        (flags & VA_DEINTERLACING_BOTTOM_FIELD) != 0 ? 1 : 0, true);
    Require(picture->storage == original &&
                fixture.decoder->decoded_frames.at(kTimestampStep) == picture,
            "two field presentations never mutate or retire the woven source");
  }
  SetPipelineFilters(fixture, nullptr, 0);
  fixture.SubmitParameters();
  Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
          "zero-filter VPP remains available after a Bob request");
  RequireFieldRows(*fixture.target, 0, false);
}

std::shared_ptr<Surface> PaddedBobOutput(unsigned width, unsigned height) {
  auto target = std::make_shared<Surface>();
  Require(target->AllocateCpuNv12(width, height), "allocate a Bob ROI target");
  target->pitch[0] = target->pitch[1] = width + 32;
  target->storage.assign(size_t{target->pitch[0]} *
                             (height + (height + 1) / 2) + 64, 0xa5);
  target->planes[0] = target->storage.data();
  target->planes[1] = target->planes[0] + size_t{target->pitch[0]} * height;
  return target;
}

void RequireBobOutputRegion(const Surface &source, const Surface &target,
                           const VARectangle &visible, const VARectangle &output,
                           unsigned parity, bool full_range) {
  for (unsigned plane = 0; plane != 2; ++plane) {
    const unsigned rows = plane == 0 ? target.height : (target.height + 1) / 2;
    const unsigned row_bytes = plane == 0 ? target.width : Align(target.width, 2);
    const unsigned top = plane == 0 ? output.y : output.y / 2;
    const unsigned active_rows = plane == 0 ? output.height : output.height / 2;
    const unsigned source_rows = plane == 0 ? visible.height : visible.height / 2;
    const unsigned left = output.x, right = left + output.width;
    const unsigned unit = plane == 0 ? 1 : 2;
    const uint8_t black = plane == 0 ? (full_range ? 0 : 16) : 128;
    const auto neutral = [black](uint8_t value) { return value == black; };
    for (unsigned row = 0; row < rows; ++row) {
      const uint8_t *line = target.planes[plane] + size_t{row} * target.pitch[plane];
      if (row < top || row >= top + active_rows) {
        Require(std::all_of(line, line + row_bytes, neutral),
                "Bob initializes complete top/bottom and coded-padding borders");
      } else {
        Require(std::all_of(line, line + left, neutral) &&
                    std::all_of(line + right, line + row_bytes, neutral),
                "Bob initializes both horizontal borders on every reused output");
        const unsigned source_row =
            uint64_t{row - top} * (source_rows / 2) / active_rows * 2 + parity;
        const uint8_t *input = source.planes[plane] +
                               size_t{source_row} * source.pitch[plane];
        if (visible.width == output.width) {
          Require(memcmp(line + left, input, output.width) == 0,
                  "placed Bob keeps exact independent Y/UV field rows");
        } else {
          for (unsigned column = 0; column < output.width; ++column) {
            const unsigned source_column =
                uint64_t{column / unit} * (visible.width / unit) /
                    (output.width / unit) * unit + column % unit;
            if (line[left + column] != input[source_column])
              Require(false, "placed Bob scales only the requested visible field extent");
          }
        }
      }
      Require(std::all_of(line + row_bytes, line + target.pitch[plane],
                          [](uint8_t value) { return value == 0xa5; }),
              "Bob preserves every pitch-padding byte");
    }
  }
  const size_t used = size_t{target.pitch[0]} * target.height +
                      size_t{target.pitch[1]} * ((target.height + 1) / 2);
  Require(std::all_of(target.storage.begin() + used, target.storage.end(),
                      [](uint8_t value) { return value == 0xa5; }),
          "Bob preserves the outer output guard");
}

// Repeated destination rows may reuse one private horizontal resample. Test
// changing frames, both field parities, compact views, ROI borders and padding.
void BobHorizontalRowsReuseWithoutStalePixels() {
  for (bool compact : {false, true})
    for (unsigned parity : {0U, 1U}) {
      Surface full, source;
      Require(full.AllocateCpuNv12(32, 24) &&
                  source.AllocateCpuNv12(32, compact ? 12 : 24),
              "allocate full and compact Bob inputs");
      const VARectangle visible{0, 0, 32, 24}, output{4, 4, 48, 36};
      auto target = PaddedBobOutput(56, 44);
      SwsContext *scaler = nullptr;
      std::vector<uint8_t> staging;
      VppDeinterlace deinterlace;
      deinterlace.bob = true;
      deinterlace.field = static_cast<crystalhd_vpp_bob::FieldParity>(parity);
      for (unsigned frame = 0; frame != 2; ++frame) {
        for (unsigned plane = 0; plane != 2; ++plane) {
          const unsigned rows = plane == 0 ? 24 : 12;
          for (unsigned row = 0; row < rows; ++row)
            for (unsigned col = 0; col != 32; ++col)
              full.planes[plane][size_t{row} * full.pitch[plane] + col] =
                  static_cast<uint8_t>(frame * 97 + plane * 43 + row * 7 + col * 3);
          for (unsigned row = 0; row < (compact ? rows / 2 : rows); ++row)
            memcpy(source.planes[plane] + size_t{row} * source.pitch[plane],
                   full.planes[plane] + size_t{compact ? row * 2 + parity : row} *
                       full.pitch[plane], 32);
        }
        source.ready = true;
        const auto original = source.storage;
        bob_resampled_rows[0] = bob_resampled_rows[1] = 0;
        Require(ProcessVpp(&scaler, &staging, &source, target.get(), visible,
                           output, false, deinterlace, compact) == VA_STATUS_SUCCESS,
                "Bob resample completes for a changing full/compact field");
        RequireBobOutputRegion(full, *target, visible, output, parity, false);
        Require(source.storage == original, "row reuse never mutates input pixels");
        Require(bob_resampled_rows[0] == 12 && bob_resampled_rows[1] == 6,
                "resample each physical input row once instead of three times");
      }
      sws_freeContext(scaler);
    }
}

void VppFastModeKeepsNearestPixelsAndRejectsOtherFilters() {
  Fixture fixture;
  fixture.driver.StopWorkers();
  fixture.driver.stopping = false;
  const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
  FillFieldRows(*picture);
  std::vector<uint8_t> expected;
  for (uint32_t flags : {VA_FILTER_SCALING_DEFAULT, VA_FILTER_SCALING_FAST,
                        VA_FILTER_INTERPOLATION_NEAREST_NEIGHBOR,
                        VA_FILTER_SCALING_FAST | VA_FILTER_INTERPOLATION_NEAREST_NEIGHBOR}) {
    VAProcPipelineParameterBuffer pipeline = {};
    pipeline.surface = 1;
    VARectangle output{0, 0, 8, 8};
    pipeline.output_region = &output;
    pipeline.filter_flags = flags;
    memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(),
           &pipeline, sizeof(pipeline));
    fixture.SubmitParameters();
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                SyncSurface2(&fixture.context, 2, 0) == VA_STATUS_SUCCESS,
            "standard FAST and nearest requests complete through ordinary VPP");
    if (expected.empty()) expected = fixture.target->storage;
    Require(fixture.target->storage == expected,
            "FAST selects the existing DEFAULT pixels without a quality change");
  }
  for (uint32_t flags : {VA_FILTER_SCALING_HQ, VA_FILTER_SCALING_NL_ANAMORPHIC,
                        VA_FILTER_INTERPOLATION_BILINEAR,
                        VA_FILTER_INTERPOLATION_ADVANCED,
                        VA_FILTER_SCALING_FAST | VA_FILTER_INTERPOLATION_BILINEAR,
                        0x4000, 0x10000}) {
    VAProcPipelineParameterBuffer pipeline = {};
    pipeline.surface = 1;
    pipeline.filter_flags = flags;
    memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(),
           &pipeline, sizeof(pipeline));
    Require(BeginPicture(&fixture.context, 2, 2) == VA_STATUS_SUCCESS,
            "begin unsupported scaling request");
    const auto previous = fixture.target->storage;
    Require(RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                VA_STATUS_ERROR_UNSUPPORTED_FILTER &&
                fixture.driver.pending_vpp.empty() &&
                fixture.target->storage == previous && picture->vpp_readers == 0,
            "unsupported encoded filter values reject before reading or writing");
    // Drop only the test submission; no EndPicture of rejected parameters.
    fixture.processor->target = VA_INVALID_SURFACE;
  }
}

void Nv12ScalingPreservesCropsBordersAndFieldRows() {
  struct Case { VARectangle input, output; bool fields; unsigned width, height; };
  const Case cases[] = {
      {{0,0,16,16}, {0,0,24,24}, false, 24,24},
      {{1,3,16,13}, {2,4,16,20}, false, 25,29},
      {{1,1,21,17}, {2,4,12,8}, false, 25,19},
      {{3,1,17,11}, {4,4,24,20}, false, 33,29},
      {{2,4,16,12}, {4,4,24,20}, true, 33,29},
      {{2,4,16,16}, {2,4,16,20}, true, 25,29},
      {{2,4,24,20}, {2,4,12,8}, true, 25,19}};
  for (const auto &item : cases) {
    Surface source;
    Require(source.AllocateCpuNv12(32,28), "allocate an ordinary VPP crop source");
    auto target = PaddedBobOutput(item.width, item.height);
    for (unsigned frame = 0; frame != 2; ++frame) {
      for (unsigned plane = 0; plane != 2; ++plane)
        for (unsigned y = 0; y < (plane == 0 ? 28 : 14); ++y)
          for (unsigned x = 0; x != 32; ++x)
            source.planes[plane][size_t{y} * source.pitch[plane] + x] =
                static_cast<uint8_t>(frame * 97 + plane * 71 + y * 13 + x * 3);
      source.ready = true;
      source.expected_timestamp = source.frame_timestamp = (frame+1)*kTimestampStep;
      const auto original = source.storage;
      VppColorRange range;
      range.source_full = frame != 0;
      nv12_cleared_bytes = 0;
      nv12_resampled_rows[0] = nv12_resampled_rows[1] = 0;
      SwsContext *scaler = nullptr;
      std::vector<uint8_t> staging;
      Require(ProcessVpp(&scaler, &staging, &source, target.get(), item.input,
                         item.output, false, {}, false, range, item.fields) == VA_STATUS_SUCCESS,
              "ordinary VPP completes progressive/interleaved cropped output");
      Require(source.storage == original && target->ready && !target->failed &&
                  target->frame_timestamp == source.frame_timestamp &&
                  target->expected_timestamp == source.expected_timestamp,
              "ordinary VPP retains immutable source and exact picture timestamps");
      for (unsigned plane = 0; plane != 2; ++plane) {
        const unsigned rows = plane == 0 ? target->height : (target->height+1)/2;
        const unsigned bytes = plane == 0 ? target->width : Align(target->width,2);
        const unsigned top = item.output.y / (plane == 0 ? 1 : 2);
        const unsigned active_rows = item.output.height / (plane == 0 ? 1 : 2);
        const unsigned unit = plane == 0 ? 1 : 2;
        std::set<unsigned> selected_rows;
        for (unsigned y = 0; y != rows; ++y)
          for (unsigned x = 0; x != target->pitch[plane]; ++x) {
            uint8_t expected = 0xa5;
            if (x < bytes) expected = plane == 0 ? (frame ? 0 : 16) : 128;
            if (y >= top && y < top+active_rows &&
                x >= unsigned(item.output.x) && x < unsigned(item.output.x)+item.output.width) {
              const unsigned row = y-top, column = x-item.output.x;
              // Independent closed-form pixel oracle, including odd crop phase.
              const unsigned sy = item.fields
                  ? item.input.y/unit + uint64_t{row/2}*(item.input.height/unit/2)/
                        (active_rows/2)*2+row%2
                  : (item.input.y + uint64_t{row}*unit*item.input.height/item.output.height)/unit;
              unsigned sx = item.input.x + uint64_t{column/unit}*unit*
                  item.input.width/item.output.width;
              if (plane != 0) sx = (sx & ~1U) + column%2;
              selected_rows.insert(sy);
              expected = source.planes[plane][size_t{sy}*source.pitch[plane]+sx];
            }
            Require(target->planes[plane][size_t{y}*target->pitch[plane]+x] == expected,
                    "nearest crop pixels, independent UV/field phase, borders and pitch tails are exact");
          }
        const unsigned expected_rows = item.input.width == item.output.width ? 0
            : item.output.height > item.input.height ? selected_rows.size() : active_rows;
        Require(nv12_resampled_rows[plane] == expected_rows,
                "same-width copies avoid sampling; enlarged field rows resample once");
      }
      const size_t canvas = size_t{target->width}*target->height +
          size_t{Align(target->width,2)}*((target->height+1)/2);
      const size_t roi = size_t{item.output.width}*item.output.height*3/2;
      Require(nv12_cleared_bytes == canvas-roi,
              "ordinary VPP initializes only actual borders, not overwritten ROI");
      const size_t used = size_t{target->pitch[0]}*target->height +
          size_t{target->pitch[1]}*((target->height+1)/2);
      Require(std::all_of(target->storage.begin()+used, target->storage.end(),
                          [](uint8_t c) { return c == 0xa5; }), "ordinary VPP retains outer guard");
      sws_freeContext(scaler);
    }
  }
}

void Nv12InPlaceCannotDestroyInput() {
  Surface source;
  Require(source.AllocateCpuNv12(16,16), "allocate an in-place negative control");
  std::fill(source.storage.begin(), source.storage.end(), 0x73);
  source.ready = true;
  const auto original = source.storage;
  SwsContext *scaler = nullptr;
  std::vector<uint8_t> staging;
  VARectangle region{0,0,16,16};
  Require(ProcessVpp(&scaler, &staging, &source, &source, region, region) ==
              VA_STATUS_ERROR_INVALID_PARAMETER && source.storage == original,
          "internal dispatcher rejects self-overwrite before clearing source pixels");
  // Public VPP already rejects this request because BeginPicture reserves its
  // target before RenderPicture can accept it as a ready raw-upload source.
  Fixture fixture;
  fixture.driver.StopWorkers();
  fixture.target->expected_timestamp = fixture.target->frame_timestamp = 0;
  const auto before = fixture.target->storage;
  VAProcPipelineParameterBuffer pipeline = {};
  pipeline.surface = 2;
  memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(), &pipeline, sizeof(pipeline));
  Require(BeginPicture(&fixture.context,2,2) == VA_STATUS_SUCCESS &&
              RenderPicture(&fixture.context,2,&fixture.parameters,1) == VA_STATUS_ERROR_INVALID_SURFACE &&
              fixture.target->storage == before,
          "public in-place raw upload remains rejected without writing pixels");
}

void BobAlignedOutputRegionsPreservePixelsAndBorders() {
  for (unsigned canvas_height : {1080U, 1088U})
    for (bool full_range : {false, true}) {
      Fixture fixture;
      // Queued ROI/real-worker behavior is covered separately below.
      fixture.driver.StopWorkers();
      fixture.driver.stopping = false;
      fixture.target = PaddedBobOutput(1920, canvas_height);
      fixture.driver.surfaces[2] = fixture.target;
      // Reuse the same target from the larger ROI to the smaller one: stale
      // previous ROI pixels must become range-correct black on all four sides.
      for (unsigned geometry = 0; geometry != 3; ++geometry) {
        const unsigned width = geometry == 0 ? 1536 : 1280;
        const unsigned height = geometry == 0 ? 864 : 720;
        const unsigned source_width = geometry == 2 ? 1920 : width;
        const unsigned source_height = geometry == 2 ? 1088 : height;
        auto picture = std::make_shared<Surface>();
        fixture.source = std::make_shared<Surface>();
        Require(picture->AllocateCpuNv12(source_width, source_height) &&
                    fixture.source->AllocateCpuNv12(source_width, source_height),
                "allocate a small or coded-1088 Bob source");
        picture->ready = fixture.source->ready = true;
        picture->expected_timestamp = picture->frame_timestamp = kTimestampStep;
        fixture.source->expected_timestamp = fixture.source->frame_timestamp = kTimestampStep;
        FillFieldRows(*picture);
        const auto original = picture->storage;
        fixture.driver.surfaces[1] = fixture.source;
        fixture.decoder->surface_timestamps.clear();
        fixture.decoder->surface_timestamps[fixture.source.get()] = kTimestampStep;
        fixture.decoder->decoded_frames[kTimestampStep] = picture;
        VARectangle visible = {0, 0, static_cast<uint16_t>(source_width),
                                static_cast<uint16_t>(geometry == 2 ? 1080 : height)};
        VARectangle output = {
            static_cast<int16_t>(geometry == 2 ? 2 : (1920 - width) / 2),
            static_cast<int16_t>(geometry == 2 ? 4 : (canvas_height - height) / 2),
            static_cast<uint16_t>(width), static_cast<uint16_t>(height)};
        for (uint32_t flags : std::array<uint32_t, 4>{{
                 0U, VA_DEINTERLACING_BOTTOM_FIELD,
                 VA_DEINTERLACING_BOTTOM_FIELD_FIRST,
                 VA_DEINTERLACING_BOTTOM_FIELD_FIRST | VA_DEINTERLACING_BOTTOM_FIELD}}) {
          VABufferID filter = CreateBobFilter(fixture, flags);
          VAProcPipelineParameterBuffer pipeline = {};
          pipeline.surface = 1;
          pipeline.surface_region = &visible;
          pipeline.output_region = &output;
          pipeline.input_surface_flag = flags & VA_DEINTERLACING_BOTTOM_FIELD_FIRST
              ? VA_BOTTOM_FIELD_FIRST : VA_TOP_FIELD_FIRST;
          pipeline.filters = &filter;
          pipeline.num_filters = 1;
          pipeline.input_color_properties.color_range = full_range
              ? VA_SOURCE_RANGE_FULL : VA_SOURCE_RANGE_REDUCED;
          memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(),
                 &pipeline, sizeof(pipeline));
          fixture.SubmitParameters();
          Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                      SyncSurface2(&fixture.context, 2, 0) == VA_STATUS_SUCCESS &&
                      fixture.target->ready && !fixture.target->failed &&
                      fixture.target->expected_timestamp == kTimestampStep &&
                      fixture.target->frame_timestamp == kTimestampStep &&
                      fixture.target->vpp_writers == 0 && picture->vpp_readers == 0,
                  "public Bob completes an aligned destination ROI with exact ownership and PTS");
          RequireBobOutputRegion(*picture, *fixture.target, visible, output,
              flags & VA_DEINTERLACING_BOTTOM_FIELD ? 1 : 0, full_range);
          Require(picture->storage == original,
                  "destination placement never mutates the immutable source");
          Require(DestroyBuffer(&fixture.context, filter) == VA_STATUS_SUCCESS,
                  "release the completed ROI filter");
        }
      }
    }
}

void BobAlignedOutputRegionQueuedSnapshotAndCancellation() {
  for (bool bottom : {false, true})
    for (bool cancel : {false, true}) {
      Fixture fixture;
      fixture.driver.StopWorkers();
      fixture.driver.stopping = false;
      fixture.target = PaddedBobOutput(32, 32);
      fixture.driver.surfaces[2] = fixture.target;
      const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
      FillFieldRows(*picture);
      picture->ready = false;
      const auto original_source = picture->storage;
      const auto original_target = fixture.target->storage;
      VABufferID filter = CreateBobFilter(fixture, bottom
          ? VA_DEINTERLACING_BOTTOM_FIELD_FIRST | VA_DEINTERLACING_BOTTOM_FIELD : 0);
      VARectangle visible = {0, 0, 16, 16}, output = {8, 4, 16, 16};
      const VARectangle captured_output = output;
      VAProcPipelineParameterBuffer pipeline = {};
      pipeline.surface = 1;
      pipeline.surface_region = &visible;
      pipeline.output_region = &output;
      pipeline.input_surface_flag = bottom ? VA_BOTTOM_FIELD_FIRST : VA_TOP_FIELD_FIRST;
      pipeline.filters = &filter;
      pipeline.num_filters = 1;
      pipeline.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
      memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(),
             &pipeline, sizeof(pipeline));
      fixture.SubmitParameters();
      output = {0, 0, 8, 8};
      Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                  fixture.driver.pending_vpp.size() == 1 &&
                  picture->vpp_readers == 1 && fixture.target->vpp_writers == 1,
              "queued Bob owns its captured nonzero destination rectangle");
      Require(DestroyBuffer(&fixture.context, filter) == VA_STATUS_SUCCESS,
              "queued ROI processing does not borrow client filter storage");
      if (cancel)
        fixture.decoder->Reset();
      else
        picture->ready = true;
      fixture.driver.vpp_worker = std::thread([&] { RunVppWorker(&fixture.driver); });
      Require(SyncSurface2(&fixture.context, 2, 1000000000ULL) ==
                  (cancel ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS) &&
                  fixture.driver.pending_vpp.empty() && picture->vpp_readers == 0 &&
                  fixture.target->vpp_writers == 0 && picture->storage == original_source,
              "queued ROI completion or reset releases both exact picture owners");
      if (cancel)
        Require(fixture.target->storage == original_target && !fixture.target->ready &&
                    fixture.target->failed,
                "canceled ROI work never touches output pixels or padding");
      else
        RequireBobOutputRegion(*picture, *fixture.target, visible, captured_output,
                               bottom ? 1 : 0, true);
    }
}

void Bob720ReturnsFhdThroughDirectAndTwoStageVpp() {
  Fixture fixture;
  // These completed private inputs take the synchronous VPP path.
  fixture.driver.StopWorkers();
  fixture.driver.stopping = false;
  auto picture = std::make_shared<Surface>();
  fixture.source = std::make_shared<Surface>();
  fixture.target = std::make_shared<Surface>();
  auto intermediate = std::make_shared<Surface>();
  auto staged = std::make_shared<Surface>();
  for (const auto &surface : {picture, fixture.source, intermediate})
    Require(surface->AllocateInternal(nullptr, -1, 1280, 720, VA_FOURCC_NV12),
            "allocate 720 input/intermediate");
  for (const auto &surface : {fixture.target, staged})
    Require(surface->AllocateInternal(nullptr, -1, 1920, 1080, VA_FOURCC_NV12),
            "allocate FHD return target");
  fixture.driver.surfaces[1] = fixture.source;
  fixture.driver.surfaces[2] = fixture.target;
  fixture.driver.surfaces[3] = intermediate;
  fixture.driver.surfaces[4] = staged;
  fixture.decoder->surface_timestamps.clear();
  fixture.decoder->surface_timestamps[fixture.source.get()] = kTimestampStep;
  fixture.decoder->decoded_frames[kTimestampStep] = picture;
  for (const auto &surface : {fixture.source, picture}) {
    surface->ready = true;
    surface->expected_timestamp = surface->frame_timestamp = kTimestampStep;
  }
  for (unsigned plane = 0; plane < 2; ++plane)
    for (unsigned row = 0; row < (plane ? 360U : 720U); ++row)
      for (unsigned x = 0; x < 1280; ++x)
        picture->planes[plane][row * picture->pitch[plane] + x] =
            static_cast<uint8_t>((plane * 37 + row * 13 + x * 7) % 251);
  const auto original = picture->storage;
  const auto submit = [&](VASurfaceID source, VASurfaceID target,
                           uint32_t order, int parity) {
    VABufferID filter = parity < 0 ? VA_INVALID_ID : CreateBobFilter(fixture,
        (parity ? VA_DEINTERLACING_BOTTOM_FIELD : 0) |
        (order == VA_BOTTOM_FIELD_FIRST ? VA_DEINTERLACING_BOTTOM_FIELD_FIRST : 0));
    VAProcPipelineParameterBuffer pipeline{};
    pipeline.surface = source;
    pipeline.input_surface_flag = order;
    pipeline.output_surface_flag = VA_FRAME_PICTURE;
    pipeline.filters = parity < 0 ? nullptr : &filter;
    pipeline.num_filters = parity < 0 ? 0 : 1;
    memcpy(fixture.driver.buffers.at(fixture.parameters).data.data(), &pipeline, sizeof(pipeline));
    const auto begin = BeginPicture(&fixture.context, 2, target);
    const auto render = begin == VA_STATUS_SUCCESS
        ? RenderPicture(&fixture.context, 2, &fixture.parameters, 1) : begin;
    if (begin != VA_STATUS_SUCCESS || render != VA_STATUS_SUCCESS)
      fprintf(stderr, "FHD chain source=%u target=%u parity=%d begin=%d render=%d\n",
              source, target, parity, begin, render);
    Require(begin == VA_STATUS_SUCCESS && render == VA_STATUS_SUCCESS,
            "capture actual Bob/scale public VPP transaction");
    if (filter != VA_INVALID_ID)
      Require(DestroyBuffer(&fixture.context, filter) == VA_STATUS_SUCCESS,
              "captured Bob filter may be destroyed before End");
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                SyncSurface2(&fixture.context, target, 0) == VA_STATUS_SUCCESS,
            "complete actual Bob/scale public VPP transaction");
  };
  for (uint32_t order : {VA_TOP_FIELD_FIRST, VA_BOTTOM_FIELD_FIRST})
    for (unsigned parity = 0; parity < 2; ++parity) {
      submit(1, 2, order, parity);
      submit(1, 3, order, parity);
      const auto bob_pixels = intermediate->storage;
      submit(3, 4, VA_FRAME_PICTURE, -1);
      for (unsigned plane = 0; plane < 2; ++plane) {
        const unsigned rows = plane ? 540 : 1080;
        const unsigned source_rows = plane ? 360 : 720;
        const unsigned unit = plane ? 2 : 1;
        for (unsigned row = 0; row < rows; ++row) {
          // Direct Bob uses one physical field; two-stage Bob duplicates that
          // field at 720 first, then a progressive nearest-neighbor scale.
          const unsigned direct_row = uint64_t(row) * (source_rows / 2) / rows * 2 + parity;
          const unsigned intermediate_row = uint64_t(row) * source_rows / rows;
          const unsigned staged_row = intermediate_row / 2 * 2 + parity;
          for (unsigned x = 0; x < 1920 / unit; ++x) {
            const unsigned sx = uint64_t(x) * (1280 / unit) / (1920 / unit);
            for (unsigned component = 0; component < unit; ++component) {
              const unsigned column = x * unit + component;
              const unsigned source_column = sx * unit + component;
              Require(fixture.target->planes[plane][row * fixture.target->pitch[plane] + column] ==
                          picture->planes[plane][direct_row * picture->pitch[plane] + source_column] &&
                          staged->planes[plane][row * staged->pitch[plane] + column] ==
                          picture->planes[plane][staged_row * picture->pitch[plane] + source_column],
                      "FHD direct and staged Y/UV outputs follow independent exact sample maps");
            }
          }
        }
      }
      Require(picture->storage == original && intermediate->storage == bob_pixels &&
                  fixture.decoder->decoded_frames.at(kTimestampStep) == picture &&
                  fixture.driver.pending_vpp.empty() && picture->vpp_readers == 0 &&
                  intermediate->vpp_readers == 0 && intermediate->vpp_writers == 0 &&
                  fixture.target->vpp_writers == 0 && staged->vpp_writers == 0,
              "FHD returns preserve both source lifetimes and release VPP leases");
      for (const auto &surface : {fixture.target, intermediate, staged})
        Require(surface->ready && !surface->failed &&
                    surface->expected_timestamp == kTimestampStep &&
                    surface->frame_timestamp == kTimestampStep,
                "FHD return retains original picture identity without fabricated timestamps");
    }
}

void CompletedVppOutputsRetainChainedSnapshots() {
  for (bool queued : {false, true})
    for (unsigned replacement = 0; replacement < 5; ++replacement) {
      Fixture f;
      f.driver.StopWorkers(); f.driver.stopping = false;
      auto picture = f.decoder->decoded_frames.at(kTimestampStep);
      FillFieldRows(*picture);
      picture->ready = !queued;
      VABufferID filter = CreateBobFilter(f);
      SetPipelineFilters(f, &filter, 1);
      auto downstream = std::make_shared<DecodeContext>(); downstream->video_process = true;
      f.driver.contexts[3] = downstream;
      auto result = Picture(0, 99); f.driver.surfaces[3] = result;
      // Also exercise canonical-owner lookup through an ordinary alias ID.
      auto alias = Picture(0, 7); alias->backing_owner = 2;
      f.driver.surfaces[4] = alias;
      VAProcPipelineParameterBuffer next{}; next.surface = replacement == 0 ? 4 : 2;
      VABufferID parameters = VA_INVALID_ID;
      Require(CreateBuffer(&f.context, 3, VAProcPipelineParameterBufferType,
                            sizeof(next), 1, &next, &parameters) == VA_STATUS_SUCCESS,
              "create independent downstream VPP context and parameters");
      const auto capture = [&] {
        Require(BeginPicture(&f.context, 3, 3) == VA_STATUS_SUCCESS, "begin downstream VPP");
        return RenderPicture(&f.context, 3, &parameters, 1);
      };
      f.SubmitParameters();
      Require(EndPicture(&f.context, 2) == VA_STATUS_SUCCESS, "first Bob completes or queues");
      if (queued) {
        Require(f.driver.pending_vpp.size() == 1 && capture() != VA_STATUS_SUCCESS &&
                    !downstream->have_vpp_parameters && f.driver.pending_vpp.size() == 1,
                "pending upstream VPP is not mistaken for a completed chained source");
        Require(EndPicture(&f.context, 3) == VA_STATUS_ERROR_INVALID_PARAMETER,
                "failed pending-source Render cannot commit a downstream frame");
        picture->ready = true;
        f.driver.vpp_worker = std::thread([&] { RunVppWorker(&f.driver); });
        Require(SyncSurface2(&f.context, 2, 1000000000ULL) == VA_STATUS_SUCCESS,
                "queued upstream VPP completes before downstream capture");
        f.driver.StopWorkers(); f.driver.stopping = false;
      }
      const auto original = f.target->storage;
      Require(capture() == VA_STATUS_SUCCESS, "capture immutable completed VPP pixels");
      Require(downstream->vpp_frame && downstream->vpp_frame.get() != f.target.get() &&
                  downstream->vpp_frame->storage == original && !downstream->vpp_decoder,
              "completed VPP capture owns private pixels before upstream reuse");
      if (replacement == 0) {
        VABufferID bottom = CreateBobFilter(f, VA_DEINTERLACING_BOTTOM_FIELD);
        SetPipelineFilters(f, &bottom, 1);
        f.SubmitParameters();
        Require(EndPicture(&f.context, 2) == VA_STATUS_SUCCESS && f.target->storage != original,
                "upstream target may produce a different selected field after capture");
      } else if (replacement == 1) {
        VAImageFormat format{};
        format.fourcc = VA_FOURCC_NV12; format.byte_order = VA_LSB_FIRST; format.bits_per_pixel = 12;
        VAImage image{};
        Require(CreateImage(&f.context, &format, 16, 16, &image) == VA_STATUS_SUCCESS,
                "create upload replacing captured upstream output");
        auto &bytes = f.driver.buffers.at(image.buf).data;
        for (unsigned plane = 0; plane < 2; ++plane)
          for (unsigned row = 0; row < (plane ? 8U : 16U); ++row)
            memset(bytes.data() + image.offsets[plane] + row * image.pitches[plane], plane ? 128 : 200, 16);
        Require(PutImage(&f.context, 2, image.image_id, 0, 0, 16, 16, 0, 0, 16, 16) == VA_STATUS_SUCCESS &&
                    f.target->storage != original && DestroyImage(&f.context, image.image_id) == VA_STATUS_SUCCESS,
                "ordinary upload replaces upstream pixels after private capture");
      } else if (replacement == 2) {
        VABufferID invalid = VA_INVALID_ID;
        Require(BeginPicture(&f.context, 2, 2) == VA_STATUS_SUCCESS &&
                    RenderPicture(&f.context, 2, &invalid, 1) == VA_STATUS_ERROR_INVALID_BUFFER &&
                    EndPicture(&f.context, 2) == VA_STATUS_ERROR_INVALID_PARAMETER,
                "rejected new upstream write revokes old completion provenance");
      } else if (replacement == 3) {
        picture->ready = false;
        f.SubmitParameters();
        Require(EndPicture(&f.context, 2) == VA_STATUS_SUCCESS && f.driver.pending_vpp.size() == 1,
                "new upstream write can queue after downstream snapshot");
        f.decoder->Reset();
        f.driver.vpp_worker = std::thread([&] { RunVppWorker(&f.driver); });
        Require(SyncSurface2(&f.context, 2, 1000000000ULL) == VA_STATUS_ERROR_DECODING_ERROR,
                "upstream epoch cancellation fails its pending output");
        f.driver.StopWorkers(); f.driver.stopping = false;
      } else {
        Require(DestroyContext(&f.context, 2) == VA_STATUS_SUCCESS,
                "completed VPP pixels outlive their producer context");
      }
      Require(EndPicture(&f.context, 3) == VA_STATUS_SUCCESS && result->storage == original &&
                  result->ready && !result->failed && result->expected_timestamp == kTimestampStep &&
                  result->frame_timestamp == kTimestampStep && f.driver.pending_vpp.empty() &&
                  picture->vpp_readers == 0 && f.target->vpp_readers == 0 &&
                  f.target->vpp_writers == 0 && result->vpp_writers == 0,
              "downstream immutable pixels/timestamp survive upstream reuse/upload/reject/cancel");
      if (replacement == 2 || replacement == 3) {
        const auto untouched = result->storage;
        Require(capture() != VA_STATUS_SUCCESS && !downstream->have_vpp_parameters &&
                    EndPicture(&f.context, 3) == VA_STATUS_ERROR_INVALID_PARAMETER && result->storage == untouched,
                "new capture cannot resurrect a failed upstream completion marker");
      }
    }
  Fixture forged;
  // Timestamp equality and ready=true alone do not establish a completed VPP
  // operation; old decode-epoch rejection must remain effective.
  forged.driver.surfaces[3] = Picture(kTimestampStep, 55);
  VAProcPipelineParameterBuffer pipeline{}; pipeline.surface = 3;
  memcpy(forged.driver.buffers.at(forged.parameters).data.data(), &pipeline, sizeof(pipeline));
  Require(BeginPicture(&forged.context, 2, 2) == VA_STATUS_SUCCESS &&
              RenderPicture(&forged.context, 2, &forged.parameters, 1) == VA_STATUS_ERROR_DECODING_ERROR,
          "ready timestamp without decoder/VPP provenance remains rejected");
  Fixture allocation;
  VABufferID filter = CreateBobFilter(allocation);
  SetPipelineFilters(allocation, &filter, 1);
  allocation.SubmitParameters();
  Require(EndPicture(&allocation.context, 2) == VA_STATUS_SUCCESS,
          "prepare completed source for snapshot allocation failure");
  auto destination = Picture(0, 99); allocation.driver.surfaces[3] = destination;
  const auto untouched = destination->storage;
  VAProcPipelineParameterBuffer next{}; next.surface = 2;
  memcpy(allocation.driver.buffers.at(allocation.parameters).data.data(), &next, sizeof(next));
  Require(BeginPicture(&allocation.context, 2, 3) == VA_STATUS_SUCCESS,
          "begin snapshot allocation failure control");
  allocation_fault::fail_next = true;
  const VAStatus failed = RenderPicture(&allocation.context, 2, &allocation.parameters, 1);
  const bool injected = !allocation_fault::fail_next;
  allocation_fault::fail_next = false;
  Require(injected && failed == VA_STATUS_ERROR_ALLOCATION_FAILED &&
              !allocation.processor->vpp_frame && !allocation.processor->have_vpp_parameters &&
              allocation.driver.pending_vpp.empty() && allocation.target->vpp_readers == 0 &&
              destination->vpp_writers == 0 && destination->storage == untouched &&
              CompletedVppPictureMatches(*allocation.target) &&
              EndPicture(&allocation.context, 2) == VA_STATUS_ERROR_INVALID_PARAMETER,
          "failed snapshot allocation preserves source provenance and leaks no capture/lease/pixels");
  Require(BeginPicture(&allocation.context, 2, 3) == VA_STATUS_SUCCESS &&
              RenderPicture(&allocation.context, 2, &allocation.parameters, 1) == VA_STATUS_SUCCESS &&
              EndPicture(&allocation.context, 2) == VA_STATUS_SUCCESS &&
              destination->storage == allocation.target->storage,
          "fresh snapshot retry succeeds after allocation failure");
}

void ExternalWritersCannotAuthorizeCompletedVppSources() {
  for (unsigned exposure = 0; exposure < 3; ++exposure) {
    Fixture f;
    f.driver.StopWorkers(); f.driver.stopping = false;
    auto picture = f.decoder->decoded_frames.at(kTimestampStep);
    FillFieldRows(*picture);
    picture->ready = exposure != 1;
    // Model the sticky provenance of a separately imported/external-writer
    // backing; no real DRM export or device access occurs in this fixture.
    f.target->vpp_external_write_exposed = exposure == 2;
    VABufferID filter = CreateBobFilter(f);
    SetPipelineFilters(f, &filter, 1);
    f.SubmitParameters();
    Require(EndPicture(&f.context, 2) == VA_STATUS_SUCCESS, "upstream exposed-output operation submits");
    std::shared_ptr<Surface> captured;
    if (exposure == 0) {
      Require(CompletedVppPictureMatches(*f.target) &&
                  SnapshotCompletedVppPicture(*f.target, &captured) == VA_STATUS_SUCCESS,
              "capture immutable completed VPP before external writer exposure");
      Require(captured && captured.get() != f.target.get(), "pre-exposure snapshot owns its pixels");
    }
    const auto original = exposure == 0 ? captured->storage : std::vector<uint8_t>{};
    // Leave any old marker in place deliberately: the sticky exposure guard
    // itself must reject it, and a later completion must not recreate it.
    f.target->vpp_external_write_exposed = true;
    if (exposure == 1) {
      Require(f.driver.pending_vpp.size() == 1, "exposure can occur while VPP is pending");
      picture->ready = true;
      f.driver.vpp_worker = std::thread([&] { RunVppWorker(&f.driver); });
      Require(SyncSurface2(&f.context, 2, 1000000000ULL) == VA_STATUS_SUCCESS,
              "pending exposed target may finish without granting chain provenance");
      f.driver.StopWorkers(); f.driver.stopping = false;
    }
    Require(!CompletedVppPictureMatches(*f.target), "external writer invalidates completed-source authority");
    if (exposure != 0)
      Require(f.target->completed_vpp_picture.sequence == 0, "completion cannot reauthorize exposed backing");
    auto output = Picture(0, 99); f.driver.surfaces[3] = output;
    VAProcPipelineParameterBuffer pipeline{}; pipeline.surface = 2;
    memcpy(f.driver.buffers.at(f.parameters).data.data(), &pipeline, sizeof(pipeline));
    Require(BeginPicture(&f.context, 2, 3) == VA_STATUS_SUCCESS &&
                RenderPicture(&f.context, 2, &f.parameters, 1) == VA_STATUS_ERROR_DECODING_ERROR &&
                !f.processor->have_vpp_parameters &&
                EndPicture(&f.context, 2) == VA_STATUS_ERROR_INVALID_PARAMETER,
            "external-writer timestamped VPP output cannot be captured as immutable");
    if (captured) {
      memset(f.target->planes[0], 211, f.target->pitch[0] * f.target->height);
      Require(captured->storage == original && captured->frame_timestamp == kTimestampStep,
              "already captured private pixels survive later external writes");
    }
  }
}

void BobFilterValuesAreCapturedAtRender() {
  for (unsigned after_render = 0; after_render < 3; ++after_render) {
    Fixture fixture;
    FillFieldRows(*fixture.decoder->decoded_frames.at(kTimestampStep));
    VABufferID filter = CreateBobFilter(fixture, VA_DEINTERLACING_BOTTOM_FIELD);
    SetPipelineFilters(fixture, &filter, 1);
    fixture.SubmitParameters();
    if (after_render == 1) {
      Require(DestroyBuffer(&fixture.context, filter) == VA_STATUS_SUCCESS,
              "release a captured filter before VPP EndPicture");
    } else {
      void *mapped = nullptr;
      Require(MapBuffer(&fixture.context, filter, &mapped) == VA_STATUS_SUCCESS,
              "map captured filter for next-request mutation");
      auto replacement = BobParameters();
      replacement.algorithm = VAProcDeinterlacingWeave;
      memcpy(mapped, &replacement, sizeof(replacement));
      if (after_render == 0)
        Require(UnmapBuffer(&fixture.context, filter) == VA_STATUS_SUCCESS,
                "unmap a changed, already captured filter");
    }
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
            "EndPicture uses its immutable Render-time Bob snapshot");
    RequireFieldRows(*fixture.target, 1, true);
    if (after_render == 2)
      Require(UnmapBuffer(&fixture.context, filter) == VA_STATUS_SUCCESS,
              "release a later mapping after captured VPP completes");
  }
}

void BobQueuedSnapshotAndCancellation() {
  for (bool cancel : {false, true}) {
    Fixture fixture;
    fixture.driver.StopWorkers();
    fixture.driver.stopping = false;
    const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    FillFieldRows(*picture);
    picture->ready = false;
    const auto untouched_target = fixture.target->storage;
    VABufferID filter = CreateBobFilter(fixture, VA_DEINTERLACING_BOTTOM_FIELD);
    SetPipelineFilters(fixture, &filter, 1);
    fixture.SubmitParameters();
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                fixture.driver.pending_vpp.size() == 1 &&
                fixture.driver.pending_vpp.front().deinterlace.bob &&
                static_cast<unsigned>(
                    fixture.driver.pending_vpp.front().deinterlace.field) == 1 &&
                picture->vpp_readers == 1 && fixture.target->vpp_writers == 1,
            "queued VPP retains its physical-field snapshot and read/write ownership");
    Require(DestroyBuffer(&fixture.context, filter) == VA_STATUS_SUCCESS,
            "queued Bob does not borrow its mutable filter buffer");
    fixture.processor->vpp_deinterlace = {};
    if (cancel)
      fixture.decoder->Reset();
    else
      picture->ready = true;
    fixture.driver.vpp_worker = std::thread([&] { RunVppWorker(&fixture.driver); });
    const VAStatus status = SyncSurface2(&fixture.context, 2, 1000000000ULL);
    Require(status == (cancel ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS) &&
                fixture.driver.pending_vpp.empty() && picture->vpp_readers == 0 &&
                fixture.target->vpp_writers == 0,
            "the real worker completes or cancels Bob and releases all owners");
    if (cancel)
      Require(fixture.target->failed && !fixture.target->ready &&
                  fixture.target->storage == untouched_target,
              "canceled Bob never publishes replacement pixels");
    else
      RequireFieldRows(*fixture.target, 1, true);
  }
}

void BobRejectedReplacementCannotReplayOldParameters() {
  Fixture fixture;
  FillFieldRows(*fixture.decoder->decoded_frames.at(kTimestampStep));
  const auto untouched_target = fixture.target->storage;
  VABufferID filter = CreateBobFilter(fixture, VA_DEINTERLACING_BOTTOM_FIELD);
  SetPipelineFilters(fixture, &filter, 1);
  fixture.SubmitParameters();
  Require(fixture.processor->have_vpp_parameters,
          "first Render captures a valid Bob transaction");
  auto invalid = BobParameters(VA_DEINTERLACING_ONE_FIELD);
  memcpy(fixture.driver.buffers.at(filter).data.data(), &invalid, sizeof(invalid));
  Require(RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                  VA_STATUS_ERROR_INVALID_PARAMETER &&
              !fixture.processor->have_vpp_parameters &&
              EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_INVALID_PARAMETER,
          "a rejected replacement Render cannot submit the earlier valid Bob snapshot");
  Require(fixture.target->storage == untouched_target && !fixture.target->ready &&
              fixture.target->failed,
          "failed Bob replacement never publishes stale output pixels");
}

void BobRejectsUnsupportedPipelineParameters() {
  for (unsigned failure = 0; failure < 20; ++failure) {
    Fixture fixture;
    VABufferID filter = CreateBobFilter(fixture);
    SetPipelineFilters(fixture, &filter, 1);
    const auto untouched_target = fixture.target->storage;
    fixture.SubmitParameters();
    auto &buffer = fixture.driver.buffers.at(fixture.parameters);
    VAProcPipelineParameterBuffer parameters;
    memcpy(&parameters, buffer.data.data(), sizeof(parameters));
    VABlendState blend = {};
    VAStatus expected = VA_STATUS_ERROR_INVALID_PARAMETER;
    switch (failure) {
      case 0: parameters.input_surface_flag = VA_TOP_FIELD; break;
      case 1: parameters.input_surface_flag = VA_BOTTOM_FIELD; break;
      case 2:
        // libva defines bit 31 of this field as surface encryption.
        parameters.input_surface_flag = uint32_t{1} << 31;
        break;
      case 3:
        parameters.input_surface_flag =
            VA_TOP_FIELD_FIRST | VA_BOTTOM_FIELD_FIRST;
        break;
      case 4: parameters.output_surface_flag = VA_TOP_FIELD; break;
      case 5: parameters.output_surface_flag = VA_TOP_FIELD_FIRST; break;
      case 6: parameters.mirror_state = VA_MIRROR_HORIZONTAL; break;
      case 7: parameters.mirror_state = VA_MIRROR_VERTICAL; break;
      case 8: parameters.blend_state = &blend; break;
      case 9: parameters.num_forward_references = 1; break;
      case 10: parameters.num_backward_references = 1; break;
      case 11: parameters.num_additional_outputs = 1; break;
      case 12: parameters.va_reserved[0] = 1; break;
      case 13: parameters.pipeline_flags = ~uint32_t{VA_PROC_PIPELINE_FAST}; break;
      case 14: parameters.surface_color_standard = VAProcColorStandardBT2020; break;
      case 15: parameters.output_color_standard = VAProcColorStandardBT709; break;
      case 16:
        buffer.context = 1;
        expected = VA_STATUS_ERROR_INVALID_BUFFER;
        break;
      case 17:
        buffer.data_failed = true;
        expected = VA_STATUS_ERROR_INVALID_BUFFER;
        break;
      case 18: {
        void *mapped = nullptr;
        Require(MapBuffer(&fixture.context, fixture.parameters, &mapped) ==
                    VA_STATUS_SUCCESS,
                "map a pipeline buffer after its valid Render snapshot");
        expected = VA_STATUS_ERROR_SURFACE_BUSY;
        break;
      }
      case 19: parameters.output_surface_flag = uint32_t{1} << 31; break;
    }
    memcpy(buffer.data.data(), &parameters, sizeof(parameters));
    Require(RenderPicture(&fixture.context, 2, &fixture.parameters, 1) == expected &&
                !fixture.processor->have_vpp_parameters,
            "unsupported or borrowed Bob pipeline rejects its replacement Render");
    if (failure == 18)
      Require(UnmapBuffer(&fixture.context, fixture.parameters) == VA_STATUS_SUCCESS,
              "release a rejected mapped pipeline before EndPicture");
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                fixture.target->storage == untouched_target &&
                fixture.target->failed && !fixture.target->ready,
            "invalid Bob pipeline cannot publish its earlier valid snapshot");
  }
}

void VppMeaningfulSemanticsAreValidatedBeforeCapture() {
  for (bool bob : {false, true}) {
    for (unsigned variant = 0; variant < 17; ++variant) {
      Fixture fixture;
      VABufferID filter = bob ? CreateBobFilter(fixture) : VA_INVALID_ID;
      SetPipelineFilters(fixture, bob ? &filter : nullptr, bob ? 1 : 0);
      const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
      const long picture_owners = picture.use_count(), decoder_owners = fixture.decoder.use_count();
      fixture.SubmitParameters();
      const auto source = fixture.source->storage, target = fixture.target->storage;
      auto &buffer = fixture.driver.buffers.at(fixture.parameters);
      VAProcPipelineParameterBuffer parameters;
      memcpy(&parameters, buffer.data.data(), sizeof(parameters));
      VABlendState blend = {};
      VAHdrMetaData hdr = {};
      VARectangle partial = {0, 0, 8, 8};
      switch (variant) {
        case 0: parameters.mirror_state = VA_MIRROR_HORIZONTAL; break;
        case 1: parameters.blend_state = &blend; break;
        case 2: parameters.num_forward_references = 1; break;
        case 3: parameters.num_backward_references = 1; break;
        case 4: parameters.num_additional_outputs = 1; break;
        case 5: parameters.pipeline_flags = ~uint32_t{VA_PROC_PIPELINE_FAST}; break;
        case 6: parameters.surface_color_standard = VAProcColorStandardBT2020; break;
        case 7: parameters.output_color_standard = VAProcColorStandardBT709; break;
        case 8: parameters.processing_mode = VAProcPowerSavingMode; break;
        case 9: parameters.output_hdr_metadata = &hdr; break;
        case 10: parameters.input_surface_flag = VA_TOP_FIELD; break;
        case 11: parameters.output_surface_flag = VA_BOTTOM_FIELD; break;
        case 12: parameters.input_surface_flag = uint32_t{1} << 31; break;
        case 13: parameters.output_surface_flag = uint32_t{1} << 31; break;
        case 14:
          parameters.output_region = &partial;
          parameters.output_background_color = 0xff00ff00;
          break;
        case 15: parameters.surface_color_standard = VAProcColorStandardExplicit; break;
        case 16: parameters.output_color_standard = VAProcColorStandardExplicit; break;
      }
      memcpy(buffer.data.data(), &parameters, sizeof(parameters));
      Require(RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                      VA_STATUS_ERROR_INVALID_PARAMETER,
              "meaningful unsupported zero-filter/Bob request returns INVALID_PARAMETER");
      Require(!fixture.processor->have_vpp_parameters && !fixture.processor->vpp_frame &&
                  !fixture.processor->vpp_decoder &&
                  fixture.processor->vpp_source == VA_INVALID_SURFACE &&
                  fixture.processor->vpp_generation == 0 &&
                  picture.use_count() == picture_owners &&
                  fixture.decoder.use_count() == decoder_owners &&
                  fixture.driver.pending_vpp.empty() && fixture.target->vpp_writers == 0,
              "rejected replacement releases its obsolete capture without acquiring a new picture");
      Require(EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                  fixture.source->storage == source && fixture.target->storage == target &&
                  fixture.target->failed && !fixture.target->ready,
              "rejected semantics cannot replay a prior valid snapshot or change pixels");
    }
    for (unsigned variant = 0; variant < 18; ++variant) {
      if (bob && variant >= 12 && variant <= 15) continue;
      Fixture fixture;
      VABufferID filter = bob ? CreateBobFilter(fixture) : VA_INVALID_ID;
      SetPipelineFilters(fixture, bob ? &filter : nullptr, bob ? 1 : 0);
      auto &buffer = fixture.driver.buffers.at(fixture.parameters);
      VAProcPipelineParameterBuffer parameters;
      memcpy(&parameters, buffer.data.data(), sizeof(parameters));
      VARectangle partial = {0, 0, 8, 8};
      VASurfaceID unused = VA_INVALID_SURFACE;
      switch (variant) {
        case 1: parameters.pipeline_flags = VA_PROC_PIPELINE_FAST; break;
        case 2: parameters.input_surface_flag = VA_TOP_FIELD_FIRST; break;
        case 3: parameters.input_surface_flag = VA_BOTTOM_FIELD_FIRST; break;
        case 4:
          parameters.forward_references = parameters.backward_references =
              parameters.additional_outputs = &unused;
          break;
        case 5:
          parameters.input_color_properties.colour_primaries = 9;
          parameters.input_color_properties.transfer_characteristics = 16;
          parameters.output_color_properties.matrix_coefficients = 9;
          break;
        case 6:
          parameters.input_color_properties.reserved[0] = 0xff;
          parameters.output_color_properties.reserved[1] = 0xff;
          break;
        case 7:
          parameters.input_color_properties.color_range =
              parameters.output_color_properties.color_range = VA_SOURCE_RANGE_REDUCED;
          break;
        case 8:
          parameters.input_color_properties.color_range =
              parameters.output_color_properties.color_range = VA_SOURCE_RANGE_FULL;
          break;
        case 9:
          parameters.input_color_properties.color_range =
              parameters.output_color_properties.color_range = VA_SOURCE_RANGE_REDUCED;
          parameters.output_region = &partial;
          break;
        case 10:
          parameters.output_region = &partial;
          parameters.output_background_color = 0xff000000;
          break;
        case 11: parameters.output_background_color = 0xff00ff00; break;
        case 12:
          parameters.input_color_properties.chroma_sample_location =
              parameters.output_color_properties.chroma_sample_location =
                  VA_CHROMA_SITING_VERTICAL_TOP | VA_CHROMA_SITING_HORIZONTAL_LEFT;
          break;
        case 13:
          parameters.input_surface_flag = parameters.output_surface_flag = VA_TOP_FIELD_FIRST;
          break;
        case 14: parameters.va_reserved[0] = 1; break;
        case 15:
          parameters.input_surface_flag = parameters.output_surface_flag = VA_BOTTOM_FIELD_FIRST;
          break;
        case 16: parameters.output_region = &partial; break;
        case 17:
          // Actual FFmpeg8 H.264 LEFT metadata maps to 2|4=6 on both
          // sides of Bob. Range/siting interpretation remains a P2 task.
          for (auto *properties : {&parameters.input_color_properties,
                                    &parameters.output_color_properties}) {
            properties->chroma_sample_location =
                VA_CHROMA_SITING_VERTICAL_CENTER | VA_CHROMA_SITING_HORIZONTAL_LEFT;
            properties->color_range = VA_SOURCE_RANGE_REDUCED;
            properties->colour_primaries = properties->transfer_characteristics =
                properties->matrix_coefficients = 1;
          }
          break;
      }
      const auto source = fixture.source->storage;
      memcpy(buffer.data.data(), &parameters, sizeof(parameters));
      fixture.SubmitParameters();
      Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                  fixture.target->ready && !fixture.target->failed &&
                  fixture.target->planes[0][0] == 40 && fixture.source->storage == source &&
                  fixture.driver.pending_vpp.empty() && fixture.target->vpp_writers == 0,
              "default/no-op semantics keep ordinary pixels and source ownership intact");
      if (variant == 9 || variant == 10 || variant == 16)
        Require(fixture.target->planes[0][15 * fixture.target->pitch[0] + 15] == 16,
                "partial default/reduced-range output retains supported neutral black padding");
    }
  }
}

void BobRejectsCanonicalSourceTargetAlias() {
  for (bool alias_source : {false, true}) {
    Fixture fixture;
    VABufferID filter = CreateBobFilter(fixture);
    SetPipelineFilters(fixture, &filter, 1);
    const auto untouched_target = fixture.target->storage;
    fixture.SubmitParameters();
    auto &buffer = fixture.driver.buffers.at(fixture.parameters);
    VAProcPipelineParameterBuffer parameters;
    memcpy(&parameters, buffer.data.data(), sizeof(parameters));
    parameters.surface = 2;
    if (alias_source) {
      auto alias = Picture(0, 70);
      alias->backing_owner = 2;
      fixture.driver.surfaces[3] = alias;
      parameters.surface = 3;
    }
    memcpy(buffer.data.data(), &parameters, sizeof(parameters));
    Require(RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                    VA_STATUS_ERROR_INVALID_SURFACE &&
                !fixture.processor->have_vpp_parameters &&
                EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_INVALID_PARAMETER,
            "Bob rejects source/target identity even through a distinct alias ID");
    Require(fixture.target->storage == untouched_target &&
                fixture.target->failed && !fixture.target->ready,
            "canonical in-place rejection never publishes old or partial Bob pixels");
  }
}

void BobRejectsCroppedRegions() {
  for (bool crop_source : {false, true}) {
    const std::array<VARectangle, 5> rejected = crop_source
        ? std::array<VARectangle, 5>{{{0, 4, 16, 12}, {2, 0, 14, 16},
                                      {0, 0, 16, 10}, {-2, 0, 16, 16},
                                      {0, 0, 18, 16}}}
        : std::array<VARectangle, 5>{{{0, 2, 16, 12}, {1, 0, 14, 16},
                                      {0, 0, 16, 10}, {2, 4, 16, 12},
                                      {0, 8, 16, 12}}};
    for (VARectangle region : rejected) {
      Fixture fixture;
      VABufferID filter = CreateBobFilter(fixture);
      SetPipelineFilters(fixture, &filter, 1);
      auto &buffer = fixture.driver.buffers.at(fixture.parameters);
      VAProcPipelineParameterBuffer parameters;
      memcpy(&parameters, buffer.data.data(), sizeof(parameters));
      if (crop_source)
        parameters.surface_region = &region;
      else
        parameters.output_region = &region;
      memcpy(buffer.data.data(), &parameters, sizeof(parameters));
      const auto original = fixture.target->storage;
      Require(BeginPicture(&fixture.context, 2, 2) == VA_STATUS_SUCCESS,
              "begin a cropped Bob negative control");
      Require(RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                  VA_STATUS_ERROR_INVALID_PARAMETER &&
                  !fixture.processor->have_vpp_parameters &&
                  fixture.driver.pending_vpp.empty() &&
                  fixture.decoder->decoded_frames.at(kTimestampStep)->vpp_readers == 0 &&
                  fixture.target->vpp_writers == 0 && fixture.target->storage == original,
              "source crops, invalid phase and out-of-bounds Bob fail before capture or writes");
    }
  }
  for (VARectangle output : std::array<VARectangle, 2>{{
           {2, 0, 14, 16}, {0, 4, 16, 12}}}) {
    Fixture fixture;
    fixture.target = PrivateArgbOutput();
    fixture.driver.surfaces[2] = fixture.target;
    VABufferID filter = CreateBobFilter(fixture);
    SetPipelineFilters(fixture, &filter, 1);
    VAProcPipelineParameterBuffer pipeline;
    auto &buffer = fixture.driver.buffers.at(fixture.parameters);
    memcpy(&pipeline, buffer.data.data(), sizeof(pipeline));
    pipeline.output_region = &output;
    memcpy(buffer.data.data(), &pipeline, sizeof(pipeline));
    const auto original = fixture.target->storage;
    Require(BeginPicture(&fixture.context, 2, 2) == VA_STATUS_SUCCESS &&
                RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                    VA_STATUS_ERROR_INVALID_PARAMETER &&
                !fixture.processor->have_vpp_parameters &&
                fixture.target->storage == original,
            "nonzero ARGB Bob output remains unsupported and untouched");
  }
  // Real clients can expose an origin-zero visible extent within coded
  // padding. The mapper must use that extent, not the allocated height.
  for (unsigned mode = 0; mode < 5; ++mode) {
    Fixture fixture;
    const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    FillFieldRows(*picture);
    const auto original_source = picture->storage;
    const uint32_t flags = mode == 0 ? 0 : mode - 1;
    VABufferID filter = CreateBobFilter(fixture, flags);
    SetPipelineFilters(fixture, mode == 0 ? nullptr : &filter, mode == 0 ? 0 : 1);
    auto &buffer = fixture.driver.buffers.at(fixture.parameters);
    VAProcPipelineParameterBuffer parameters;
    memcpy(&parameters, buffer.data.data(), sizeof(parameters));
    VARectangle visible = {0, 0, 16, 12};
    parameters.surface_region = &visible;
    parameters.output_region = &visible;
    memcpy(buffer.data.data(), &parameters, sizeof(parameters));
    fixture.SubmitParameters();
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                fixture.target->ready && !fixture.target->failed &&
                fixture.target->frame_timestamp == kTimestampStep &&
                fixture.target->expected_timestamp == kTimestampStep,
            "origin-zero visible extents preserve complete-picture output and PTS");
    const unsigned parity = (flags & VA_DEINTERLACING_BOTTOM_FIELD) != 0 ? 1 : 0;
    for (unsigned plane = 0; plane < 2; ++plane) {
      const unsigned visible_rows = plane == 0 ? 12 : 6;
      const unsigned coded_rows = plane == 0 ? 16 : 8;
      for (unsigned row = 0; row < coded_rows; ++row)
        for (unsigned column = 0; column < 16; ++column) {
          const unsigned selected_row = mode == 0 ? row : (row / 2) * 2 + parity;
          const uint8_t expected = row < visible_rows
              ? static_cast<uint8_t>((plane == 0 ? 20 : 80) +
                                     selected_row * 7 + column)
              : static_cast<uint8_t>(plane == 0 ? 16 : 128);
          Require(fixture.target->planes[plane][row * fixture.target->pitch[plane] +
                                                column] == expected,
                  "visible Weave/Bob rows are exact and coded borders are neutral");
        }
    }
    Require(picture->storage == original_source,
            "visible-extent processing never changes the coded source picture");
  }
}

void DecodeTeardownStillNotifies() {
  for (bool closing_surface : {false, true}) {
    Fixture fixture;
    const uint64_t generation = fixture.decoder->generation;
    if (closing_surface) {
      fixture.decoder->closing = true;
      VASurfaceID surface = 1;
      Require(DestroySurfaces(&fixture.context, &surface, 1) == VA_STATUS_SUCCESS &&
                  fixture.decoder->generation == generation,
              "closing decoder surface retires without a second reset");
    } else {
      Require(DestroyContext(&fixture.context, 1) == VA_STATUS_SUCCESS &&
                  fixture.decoder->retired,
              "decode context retires normally");
    }
    Require(ReadDecodeControlNotifications(fixture.driver) == 1,
            "decoder teardown must still notify RX poll and condition waiters");
  }
}

void PendingVppTeardownStillNotifies() {
  for (bool destroy_target : {false, true}) {
    Fixture fixture;
    // Keep the queued operation available for deterministic cancellation
    // checks, with no real device or asynchronous completion racing teardown.
    fixture.driver.StopWorkers();
    ReadDecodeControlNotifications(fixture.driver);
    PendingVpp pending;
    pending.decoder = fixture.decoder;
    pending.source = fixture.decoder->decoded_frames.at(kTimestampStep);
    pending.target = fixture.target;
    pending.target_owner = fixture.target;
    pending.sequence = 1;
    pending.decoder_generation = fixture.decoder->generation;
    pending.source->vpp_readers = 1;
    fixture.target->vpp_writers = 1;
    fixture.target->latest_vpp_sequence = 1;
    fixture.driver.pending_vpp.push_back(pending);
    if (destroy_target) {
      VASurfaceID target = 2;
      Require(DestroySurfaces(&fixture.context, &target, 1) == VA_STATUS_SUCCESS &&
                  PendingVppCanceled(&fixture.driver,
                                     fixture.driver.pending_vpp.front()),
              "destroyed pending VPP target cancels queued output");
    } else {
      Require(DestroyContext(&fixture.context, 2) == VA_STATUS_SUCCESS,
              "processing-context teardown retains queued VPP notification");
    }
    Require(ReadDecodeControlNotifications(fixture.driver) == 1,
            "pending VPP teardown must still notify workers and sync waiters");
    std::lock_guard<std::mutex> lock(fixture.driver.mutex);
    ReleasePendingVpp(&fixture.driver, 1, VA_STATUS_ERROR_OPERATION_FAILED, false);
    Require(fixture.driver.pending_vpp.empty() && fixture.target->failed &&
                fixture.target->planes[0][0] == 99 &&
                pending.source->vpp_readers == 0 && fixture.target->vpp_writers == 0,
            "canceled VPP releases ownership without painting a substitute");
  }
}

void VppQueueAllocationFailureIsTransactional() {
  for (bool exposed : {false, true}) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    Fixture fixture;
    fixture.driver.StopWorkers();
    const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    picture->ready = false;
    fixture.SubmitParameters();
    if (exposed) {
      fixture.target->layout.valid = true;
      fixture.target->object_fds = {101};
      mock.events.reserve(64);
    }
    // Find the next real deque-node allocation without assuming an ABI's
    // block size. Workers are stopped; these sentinel entries never execute.
    PendingVpp sentinel;
    bool boundary = false;
    for (size_t attempt = 0; attempt < 4096; ++attempt) {
      allocation_fault::fail_next = true;
      try {
        fixture.driver.pending_vpp.push_back(sentinel);
      } catch (const std::bad_alloc &) {
        boundary = true;
        break;
      }
    }
    allocation_fault::fail_next = false;
    Require(boundary && allocation_fault::failed_size >= sizeof(PendingVpp),
            "locate the next actual VPP deque-node allocation");
    const size_t queued_before = fixture.driver.pending_vpp.size();
    const auto pixels = fixture.target->storage;
    allocation_fault::only_size = allocation_fault::failed_size;
    allocation_fault::fail_next = true;
    VAStatus status = VA_STATUS_ERROR_OPERATION_FAILED;
    bool escaped = false;
    try {
      status = EndPicture(&fixture.context, 2);
    } catch (const std::bad_alloc &) {
      escaped = true;
    }
    const bool injected = !allocation_fault::fail_next;
    allocation_fault::fail_next = false;
    allocation_fault::only_size = 0;
    const bool queue_preserved = fixture.driver.pending_vpp.size() == queued_before;
    const bool owners_released = picture->vpp_readers == 0 && fixture.target->vpp_writers == 0;
    const bool fence_released = mock.started.empty() && mock.handles.empty() &&
        (!mock.fence_created || mock.fence_status < 0) && !mock.self_wait && !mock.invalid &&
        std::find(mock.events.begin(), mock.events.end(), "signal") == mock.events.end();
    // Even an unfixed implementation must leave this CPU-only test safely
    // destructible after recording its original leaked ownership/fence state.
    if (mock.handles.count(FenceIoMock::timeline) != 0)
      AbortFencedWrite(fixture.target.get(), FenceIoMock::timeline);
    if (!queue_preserved)
      ReleasePendingVpp(&fixture.driver, fixture.target->latest_vpp_sequence,
                        VA_STATUS_ERROR_OPERATION_FAILED, false);
    fixture.driver.pending_vpp.clear();
    Require(injected && !escaped && status == VA_STATUS_ERROR_ALLOCATION_FAILED,
            "VPP queue allocation failure returns ALLOCATION_FAILED without escaping the VA ABI");
    Require(queue_preserved && owners_released && fence_released &&
                fixture.target->storage == pixels && fixture.target->failed &&
                !fixture.target->ready && fixture.processor->target == VA_INVALID_SURFACE &&
                !fixture.processor->have_vpp_parameters,
            "failed VPP enqueue preserves pixels and releases every queue/fence/transaction owner");
    picture->ready = true;
    fixture.SubmitParameters();
    fixture.Complete(kTimestampStep, 40);
    Require(picture->vpp_readers == 0 && fixture.target->vpp_writers == 0 &&
                mock.started.empty() && mock.handles.empty() && !mock.self_wait && !mock.invalid,
            "a fresh VPP request recovers after queue allocation failure");
  }
}

void StaleVppFailurePreservesNewerWriter() {
  Fixture fixture;
  Require(BeginPicture(&fixture.context, 2, 2) == VA_STATUS_SUCCESS,
          "begin the older VPP transaction without valid parameters");
  auto newer = std::make_shared<DecodeContext>();
  newer->video_process = true;
  fixture.driver.contexts[3] = newer;
  VAProcPipelineParameterBuffer pipeline = {};
  pipeline.surface = 1;
  VABufferID parameters = VA_INVALID_ID;
  Require(CreateBuffer(&fixture.context, 3, VAProcPipelineParameterBufferType,
                       sizeof(pipeline), 1, &pipeline, &parameters) == VA_STATUS_SUCCESS &&
              BeginPicture(&fixture.context, 3, 2) == VA_STATUS_SUCCESS &&
              RenderPicture(&fixture.context, 3, &parameters, 1) == VA_STATUS_SUCCESS &&
              EndPicture(&fixture.context, 3) == VA_STATUS_SUCCESS,
          "a newer ordinary VPP writer commits the shared target");
  const uint64_t transaction = fixture.target->picture_transaction;
  const auto pixels = fixture.target->storage;
  Require(fixture.target->ready && !fixture.target->failed &&
              fixture.target->frame_timestamp == kTimestampStep,
          "the newer writer publishes its exact completed picture");
  Require(EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_INVALID_PARAMETER,
          "the older unrendered VPP End still rejects and consumes its transaction");
  Require(fixture.target->ready && !fixture.target->failed &&
              fixture.target->picture_transaction == transaction &&
              fixture.target->frame_timestamp == kTimestampStep &&
              fixture.target->storage == pixels && fixture.target->vpp_writers == 0 &&
              fixture.processor->target == VA_INVALID_SURFACE &&
              !fixture.processor->have_vpp_parameters,
          "a stale VPP failure cannot poison a newer ordinary writer's completed output");
}

void RepeatedVppRetainsPicture() {
  Fixture fixture;
  fixture.SubmitParameters();
  fixture.Complete(kTimestampStep, 40);
  Require(fixture.decoder->decoded_frames.count(kTimestampStep) == 1,
          "first VPP must not retire a live decode picture");
  memset(fixture.target->planes[0], 99, fixture.target->storage.size());
  fixture.SubmitParameters();
  fixture.Complete(kTimestampStep, 40);
}

void DirectCaptureKeepsQueuedVppPrivate() {
  for (bool cancel : {false, true}) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    Fixture fixture;
    fixture.decoder->id = 1;  // Real decoder ID, independent of optional TRACE.
    // Drive the queued lifecycle deterministically on the mocking thread.
    // The real worker would race these manual completion/cancellation steps.
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      fixture.driver.stopping = true;
    }
    fixture.driver.condition.notify_all();
    fixture.driver.vpp_worker.join();
    auto picture = std::make_shared<Surface>();
    picture->width = picture->height = 16;
    picture->expected_timestamp = kTimestampStep;
    picture->direct_picture = true;
    picture->decode_target = fixture.source;
    picture->direct_backing = fixture.source;
    fixture.source->decode_picture = picture;
    fixture.source->ready = false;
    fixture.decoder->decoded_frames[kTimestampStep] = picture;
    fixture.SubmitParameters();
    fixture.target->object_fds = {101};
    fixture.target->layout.valid = true;  // Model an externally exposed backing.
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                fixture.driver.pending_vpp.size() == 1 &&
                picture->vpp_readers == 1 && !picture->direct_backing,
            "pending direct capture queues private source with a real write fence");
    // Reuse the public backing and release the decode map while the queued
    // operation alone retains the captured picture.
    fixture.source->expected_timestamp = 2 * kTimestampStep;
    fixture.source->planes[0][0] = 80;
    fixture.decoder->decoded_frames.clear();
    const auto pending = fixture.driver.pending_vpp.front();
    if (cancel) {
      fixture.decoder->Reset();
      Require(PendingVppCanceled(&fixture.driver, pending),
              "decoder retirement cancels direct capture epoch");
      // The source is still cold and its mutable expectation can change;
      // DEBUG must distinguish both from the immutable queued token/epoch.
      picture->expected_timestamp = 3 * kTimestampStep;
      Require(picture->frame_timestamp == 0 && !picture->ready &&
                  pending.source_timestamp == kTimestampStep &&
                  picture->trace_context == 0 &&
                  fixture.decoder->generation != pending.decoder_generation,
              "cold canceled source differs from queued identity and generation");
      const char *old_debug = getenv("CRYSTALHD_VAAPI_DEBUG");
      const bool had_debug = old_debug != nullptr;
      const std::string old_debug_value = had_debug ? old_debug : "";
      struct FileCloser {
        void operator()(FILE *stream) const {
          if (fclose(stream) != 0) std::abort();
        }
      };
      std::unique_ptr<FILE, FileCloser> log(tmpfile());
      Require(log != nullptr && fflush(stderr) == 0, "prepare private DEBUG capture");
      const int saved_stderr = dup(STDERR_FILENO);
      Require(saved_stderr >= 0, "save stderr for scoped DEBUG capture");
      struct RestoreDebug {
        int descriptor;
        bool had_value;
        const std::string &value;
        ~RestoreDebug() {
          bool clean = fflush(stderr) == 0;
          clean = (dup2(descriptor, STDERR_FILENO) >= 0) && clean;
          // This is our private stdio duplicate, not a mocked fence handle.
          clean = (__real_close(descriptor) == 0) && clean;
          clean = ((had_value ? setenv("CRYSTALHD_VAAPI_DEBUG", value.c_str(), 1)
                              : unsetenv("CRYSTALHD_VAAPI_DEBUG")) == 0) && clean;
          if (!clean) std::abort();
        }
      } restore{saved_stderr, had_debug, old_debug_value};
      Require(setenv("CRYSTALHD_VAAPI_DEBUG", "1", 1) == 0 &&
                  dup2(fileno(log.get()), STDERR_FILENO) >= 0,
              "capture only the existing release diagnostic");
      ReleasePendingVpp(&fixture.driver, pending.sequence,
                        VA_STATUS_ERROR_OPERATION_FAILED, false);
      Require(fflush(stderr) == 0 && fseek(log.get(), 0, SEEK_SET) == 0,
              "read completed release diagnostic");
      char bytes[2048];
      const size_t size = fread(bytes, 1, sizeof(bytes), log.get());
      Require(size < sizeof(bytes) && !ferror(log.get()), "bounded DEBUG capture");
      const std::string text(bytes, size);
      Require(text.find("discarded VPP sequence=" + std::to_string(pending.sequence) +
                        " source_timestamp=0 target=") != std::string::npos &&
                  text.find("expected_timestamp=" + std::to_string(pending.source_timestamp) +
                            " decoder_context=1 captured_generation=" +
                            std::to_string(pending.decoder_generation) +
                            " current_generation=" + std::to_string(fixture.decoder->generation)) !=
                      std::string::npos &&
                  text.find("expected_timestamp=" + std::to_string(picture->expected_timestamp)) ==
                      std::string::npos &&
                  text.find("source_ready=0 source_failed=0 decoder_retired=0 "
                            "source_destroyed=0 target_destroyed=0 target_owner_destroyed=0 "
                            "target_superseded=0") != std::string::npos,
              "DEBUG preserves completed0 and exposes immutable queued identity/cancellation state");
      Require(fixture.target->failed && fixture.target->planes[0][0] == 99 &&
                  mock.fence_status < 0,
              "canceled capture preserves destination and publishes error fence");
    } else {
      const auto completed = Picture(kTimestampStep, 40);
      Require(CopyNv12Surface(*completed, picture.get()), "complete retained private output");
      Require(ProcessVpp(&fixture.driver.vpp_scaler, &fixture.driver.vpp_argb_staging,
                          picture.get(), fixture.target.get(), pending.source_region,
                          pending.output_region, true) == VA_STATUS_SUCCESS,
              "convert queued exact private picture");
      ReleasePendingVpp(&fixture.driver, pending.sequence, VA_STATUS_SUCCESS, true);
      Require(fixture.target->ready && fixture.target->planes[0][0] == 40 &&
                  fixture.target->frame_timestamp == kTimestampStep &&
                  mock.fence_status == 1,
              "queued capture signals only committed original pixels");
    }
    Require(picture->vpp_readers == 0 && fixture.target->vpp_writers == 0 &&
                !mock.invalid && !mock.self_wait,
            "direct capture releases ownership without waiting on its own fence");
  }
}

void InternalQueuedVppCompletesWithoutSwSync() {
  // Cover explicit synchronization, read/default export synchronization and
  // cancellation. Only private memfds and the actual VPP worker are used.
  for (unsigned mode = 0; mode < 4; ++mode) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    Fixture fixture;
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      fixture.driver.stopping = true;
    }
    fixture.driver.condition.notify_all();
    fixture.driver.vpp_worker.join();
    fixture.driver.stopping = false;
    const size_t size = fixture.target->storage.size();
    const int fd = memfd_create("crystalhd-private-async-vpp", MFD_CLOEXEC);
    Require(fd >= 0, "allocate private internal VPP backing");
    fixture.target->object_fds = {fd};
    mock.real_files.insert(fd);
    Require(ftruncate(fd, size) == 0, "size private internal VPP backing");
    void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
    Require(mapping != MAP_FAILED, "map private internal VPP backing");
    fixture.target->object_maps = {mapping};
    fixture.target->object_sizes = {size};
    memcpy(mapping, fixture.target->storage.data(), size);
    fixture.target->planes[0] = static_cast<uint8_t *>(mapping);
    fixture.target->planes[1] = fixture.target->planes[0] +
        fixture.target->pitch[0] * fixture.target->height;
    const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    picture->ready = false;
    mock.fail_open = true;  // No sw_sync node is available, even through mocks.
    fixture.SubmitParameters();
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                fixture.driver.pending_vpp.size() == 1 &&
                fixture.driver.pending_vpp.front().write_timeline == -1 &&
                picture->vpp_readers == 1 && fixture.target->vpp_writers == 1 &&
                !fixture.target->ready && !fixture.target->layout.valid &&
                mock.events.empty(),
            "internal decode-pending VPP queues without opening a timeline");
    VASurfaceStatus state = VASurfaceReady;
    VADRMPRIMESurfaceDescriptor prime = {};
    Require(QuerySurfaceStatus(&fixture.context, 2, &state) == VA_STATUS_SUCCESS &&
                state == VASurfaceRendering &&
                SyncSurface2(&fixture.context, 2, 0) == VA_STATUS_ERROR_TIMEDOUT &&
                ExportSurfaceHandle(&fixture.context, 2,
                    VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                    VA_EXPORT_SURFACE_WRITE_ONLY, &prime) == VA_STATUS_ERROR_SURFACE_BUSY &&
                prime.num_objects == 0 && !fixture.target->layout.valid &&
                mock.events.empty(),
            "pending unfenced pixels are neither ready nor writable-exportable");

    // On assertion failure, wake and join a read exporter before Fixture joins
    // its worker. Never leave a joinable thread or a blocked surface wait.
    struct ExportJoin {
      Driver *driver;
      std::thread thread;
      ~ExportJoin() {
        if (!thread.joinable()) return;
        {
          std::lock_guard<std::mutex> lock(driver->mutex);
          driver->stopping = true;
        }
        driver->condition.notify_all();
        thread.join();
      }
    } exporter{&fixture.driver, {}};
    VAStatus export_status = VA_STATUS_ERROR_OPERATION_FAILED;
    bool export_waited = true;
    if (mode == 1 || mode == 2) {
      exporter.thread = std::thread([&] {
        MockFenceScope worker_scope(mock);
        export_status = ExportSurfaceHandle(&fixture.context, 2,
            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            (mode == 1 ? VA_EXPORT_SURFACE_READ_ONLY : 0) |
                VA_EXPORT_SURFACE_COMPOSED_LAYERS, &prime);
      });
      std::unique_lock<std::mutex> lock(fixture.driver.mutex);
      export_waited = fixture.driver.condition.wait_for(
          lock, std::chrono::milliseconds(100),
          [&] { return fixture.target->export_waiters == 1; });
    }
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      if (mode == 3)
        fixture.decoder->Reset();
      else
        picture->ready = true;
    }
    fixture.driver.vpp_worker = std::thread([&] {
      MockFenceScope worker_scope(mock);
      RunVppWorker(&fixture.driver);
    });
    const VAStatus status = SyncSurface2(&fixture.context, 2, 1000000000ULL);
    if (exporter.thread.joinable()) exporter.thread.join();
    Require(status == (mode == 3 ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS) &&
                fixture.driver.pending_vpp.empty() && picture->vpp_readers == 0 &&
                fixture.target->vpp_writers == 0 && export_waited,
            "the real worker completes or cancels internal VPP and releases owners");
    if (mode == 3) {
      Require(fixture.target->failed && !fixture.target->ready &&
                  memcmp(mapping, fixture.target->storage.data(), size) == 0 &&
                  mock.events.empty(),
              "unfenced cancellation preserves pixels without CPU writes");
    } else {
      Require(fixture.target->ready && !fixture.target->failed &&
                  fixture.target->frame_timestamp == kTimestampStep &&
                  mock.events == std::vector<std::string>{
                      "start:" + std::to_string(fd), "end:" + std::to_string(fd)} &&
                  mock.started.empty() && mock.handles.empty() && !mock.invalid &&
                  !mock.self_wait,
              "unfenced worker performs balanced CPU ownership, never fence operations");
      for (unsigned plane = 0; plane < 2; ++plane)
        for (unsigned row = 0; row < (plane == 0 ? 16U : 8U); ++row)
          for (unsigned column = 0; column < 16; ++column)
            Require(fixture.target->planes[plane][row * fixture.target->pitch[plane] + column] ==
                        (plane == 0 ? 40 : 128),
                    "internal asynchronous VPP commits every actual NV12 pixel");
      if (mode == 1 || mode == 2) {
        uint8_t exported_luma = 0;
        const bool exported = export_status == VA_STATUS_SUCCESS &&
            prime.num_objects == 1 && fixture.target->layout.valid &&
            pread(prime.objects[0].fd, &exported_luma, 1, 0) == 1 && exported_luma == 40;
        if (prime.num_objects != 0) {
          mock.real_files.insert(prime.objects[0].fd);
          close(prime.objects[0].fd);
        }
        Require(exported, "read/default export waits for committed real backing pixels");
      }
    }
  }
}

struct ReadOnlyBobFixture : Fixture {
  Surface exported_files;
  VADRMPRIMESurfaceDescriptor prime = {};
  VABufferID filter = VA_INVALID_ID;

  ReadOnlyBobFixture(FenceIoMock &mock, unsigned parity = 0,
                     unsigned width = 16, unsigned height = 16) {
    driver.StopWorkers();
    driver.stopping = false;
    VASurfaceID id = 2;
    Require(DestroySurfaces(&context, &id, 1) == VA_STATUS_SUCCESS,
            "replace the fixture target with an actual driver allocation");
    driver.next_surface = id;
    Require(CreateSurfaces2(&context, VA_RT_FORMAT_YUV420, width, height,
                            &id, 1, nullptr, 0) == VA_STATUS_SUCCESS && id == 2,
            "allocate an ordinary internally owned NV12 target");
    target = driver.surfaces.at(id);
    const size_t size = target->storage.size();
    const int fd = memfd_create("crystalhd-readonly-bob", MFD_CLOEXEC);
    Require(fd >= 0, "allocate private exportable Bob backing");
    target->object_fds = {fd};
    mock.real_files.insert(fd);
    Require(ftruncate(fd, size) == 0, "size private Bob backing");
    void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    Require(mapping != MAP_FAILED, "map private Bob backing");
    target->object_maps = {mapping};
    target->object_sizes = {size};
    memset(mapping, 99, size);
    target->planes[0] = static_cast<uint8_t *>(mapping);
    target->planes[1] = target->planes[0] + target->pitch[0] * target->height;
    Require(ExportSurfaceHandle(&context, id,
                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                &prime) == VA_STATUS_SUCCESS && prime.num_objects == 1 &&
                target->layout.valid && target->direct_decode_eligible &&
                !target->vpp_external_write_exposed &&
                target->backing_owner == VA_INVALID_SURFACE,
            "a real read-only export preserves internal allocation provenance");
    exported_files.object_fds = {prime.objects[0].fd};
    mock.real_files.insert(prime.objects[0].fd);
    auto picture = decoder->decoded_frames.at(kTimestampStep);
    FillFieldRows(*picture);
    picture->ready = false;
    filter = CreateBobFilter(*this,
        parity ? VA_DEINTERLACING_BOTTOM_FIELD : 0);
    SetPipelineFilters(*this, &filter, 1);
  }

  VAStatus ImportAlias(VASurfaceID *alias) {
    VASurfaceAttrib attributes[2] = {};
    attributes[0].type = VASurfaceAttribMemoryType;
    attributes[0].value.type = VAGenericValueTypeInteger;
    attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
    attributes[1].value.type = VAGenericValueTypePointer;
    attributes[1].value.value.p = &prime;
    return CreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 16, 16,
                            alias, 1, attributes, 2);
  }

  void Queue() {
    SubmitParameters();
    Require(EndPicture(&context, 2) == VA_STATUS_SUCCESS &&
                driver.pending_vpp.size() == 1 &&
                driver.pending_vpp.front().write_timeline == -1 &&
                !driver.pending_vpp.front().worker_active &&
                decoder->decoded_frames.at(kTimestampStep)->vpp_readers == 1 &&
                target->vpp_writers == 1 && !target->ready && !target->failed,
            "read-only exported cold Bob queues without a software timeline");
  }

  void StartWorker(FenceIoMock &mock) {
    driver.vpp_worker = std::thread([this, &mock] {
      MockFenceScope worker_scope(mock);
      RunVppWorker(&driver);
    });
  }
};

void ReadOnlyExportedBobPendingPixels() {
  for (unsigned parity : {0U, 1U}) {
    FenceIoMock mock;  // The software timeline mock is available but unused.
    MockFenceScope scope(mock);
    ReadOnlyBobFixture fixture(mock, parity);
    const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    fixture.Queue();
    VASurfaceStatus state = VASurfaceReady;
    VADRMPRIMESurfaceDescriptor writable = {};
    VASurfaceID alias = VA_INVALID_SURFACE;
    const size_t count = fixture.driver.surfaces.size();
    Require(QuerySurfaceStatus(&fixture.context, 2, &state) == VA_STATUS_SUCCESS &&
                state == VASurfaceRendering &&
                SyncSurface2(&fixture.context, 2, 0) == VA_STATUS_ERROR_TIMEDOUT &&
                SyncSurface2(&fixture.context, 2, 1000000ULL) == VA_STATUS_ERROR_TIMEDOUT &&
                ExportSurfaceHandle(&fixture.context, 2,
                    VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                    VA_EXPORT_SURFACE_WRITE_ONLY, &writable) == VA_STATUS_ERROR_SURFACE_BUSY &&
                writable.num_objects == 0 &&
                fixture.ImportAlias(&alias) == VA_STATUS_ERROR_SURFACE_BUSY &&
                alias == VA_INVALID_SURFACE && fixture.driver.surfaces.size() == count &&
                fixture.target->direct_decode_eligible &&
                !fixture.target->vpp_external_write_exposed && mock.events.empty(),
            "pending read-only Bob waits and rejects writable exposure or alias import");
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      picture->ready = true;
    }
    fixture.StartWorker(mock);
    Require(SyncSurface2(&fixture.context, 2, 1000000000ULL) == VA_STATUS_SUCCESS &&
                fixture.driver.pending_vpp.empty() && picture->vpp_readers == 0 &&
                fixture.target->vpp_writers == 0 &&
                mock.events == std::vector<std::string>{
                    "start:" + std::to_string(fixture.target->object_fds[0]),
                    "end:" + std::to_string(fixture.target->object_fds[0])} &&
                mock.handles.empty() && mock.started.empty() && !mock.invalid && !mock.self_wait,
            "the real worker commits with balanced CPU ownership and no fence calls");
    RequireFieldRows(*fixture.target, parity, true);
    uint8_t exported_luma = 0;
    Require(pread(fixture.prime.objects[0].fd, &exported_luma, 1, 0) == 1 &&
                exported_luma == static_cast<uint8_t>(20 + parity * 7),
            "the previously exported descriptor sees the exact completed field");
  }
}

void ReadOnlyExportedBobWritableExposureKeepsOldAdmission() {
  for (unsigned exposure = 0; exposure != 3; ++exposure) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    ReadOnlyBobFixture fixture(mock);
    const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    fixture.Queue();
    picture->ready = true;
    fixture.StartWorker(mock);
    Require(SyncSurface2(&fixture.context, 2, 1000000000ULL) == VA_STATUS_SUCCESS,
            "complete read-only Bob before introducing another possible writer");
    fixture.driver.StopWorkers();
    fixture.driver.stopping = false;
    if (exposure < 2) {
      VADRMPRIMESurfaceDescriptor writable = {};
      Require(ExportSurfaceHandle(&fixture.context, 2,
                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                  exposure == 0 ? VA_EXPORT_SURFACE_WRITE_ONLY : 0,
                  &writable) == VA_STATUS_SUCCESS && writable.num_objects == 1 &&
                  fixture.target->vpp_external_write_exposed,
              "successful write-only or unspecified export records writable exposure");
      mock.real_files.insert(writable.objects[0].fd);
      close(writable.objects[0].fd);
    } else {
      VASurfaceID alias = VA_INVALID_SURFACE;
      Require(fixture.ImportAlias(&alias) == VA_STATUS_SUCCESS &&
                  fixture.driver.surfaces.at(alias)->backing_owner == 2,
              "completed read-only backing may acquire a real imported alias");
      for (int fd : fixture.driver.surfaces.at(alias)->object_fds)
        mock.real_files.insert(fd);
      Require(DestroySurfaces(&fixture.context, &alias, 1) == VA_STATUS_SUCCESS,
              "retire the imported alias before testing sticky provenance");
    }
    Require(!fixture.target->direct_decode_eligible,
            "writable export or import permanently clears direct allocation eligibility");
    mock.events.clear();
    picture->ready = false;
    const bool can_fence = CpuCacheFlush().Available();
    fixture.driver.stopping = !can_fence;
    fixture.SubmitParameters();
    const VAStatus status = EndPicture(&fixture.context, 2);
    if (can_fence) {
      Require(status == VA_STATUS_SUCCESS && fixture.driver.pending_vpp.size() == 1 &&
                  fixture.driver.pending_vpp.front().write_timeline >= 0 &&
                  mock.events.front() == "open",
              "poisoned backing retains the established fenced route when available");
      ReleasePendingVpp(&fixture.driver, fixture.driver.pending_vpp.front().sequence,
                        VA_STATUS_ERROR_OPERATION_FAILED, false);
    } else {
      Require(status != VA_STATUS_SUCCESS && fixture.driver.pending_vpp.empty() &&
                  mock.events.empty(),
              "scalar poisoned backing retains synchronous cancellation without a timeline");
    }
    fixture.driver.stopping = false;
    Require(picture->vpp_readers == 0 && fixture.target->vpp_writers == 0 &&
                mock.handles.empty() && mock.started.empty() && !mock.invalid && !mock.self_wait,
            "excluded cold work releases ownership without a late conversion");
    Require(fixture.target->planes[0][0] == 20,
            "canceling the old admission route preserves the preceding committed pixels");
  }
}

void ReadOnlyExportedBobCancellationReleasesOwners() {
  for (unsigned mode = 0; mode != 3; ++mode) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    ReadOnlyBobFixture fixture(mock);
    const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    fixture.Queue();
    const auto target = fixture.target;
    std::vector<uint8_t> before(target->object_sizes[0]);
    memcpy(before.data(), target->object_maps[0], before.size());
    std::shared_ptr<Surface> replacement;
    if (mode == 0) {
      fixture.decoder->Reset();
    } else {
      VASurfaceID retired = mode == 1 ? 2 : 1;
      Require(DestroySurfaces(&fixture.context, &retired, 1) == VA_STATUS_SUCCESS,
              "retire the pending read-only target or its decode source");
      if (mode == 1) {
        replacement = Picture(0, 77);
        fixture.driver.surfaces[2] = replacement;
      }
    }
    picture->ready = true;  // A late source completion cannot revive canceled work.
    fixture.StartWorker(mock);
    {
      std::unique_lock<std::mutex> lock(fixture.driver.mutex);
      Require(fixture.driver.condition.wait_for(lock, std::chrono::seconds(1), [&] {
                return fixture.driver.pending_vpp.empty();
              }) && picture->vpp_readers == 0 && target->vpp_writers == 0,
              "the real worker retires canceled unfenced ownership");
    }
    fixture.driver.StopWorkers();
    Require(memcmp(before.data(), target->object_maps[0], before.size()) == 0 &&
                (!replacement || replacement->planes[0][0] == 77) &&
                mock.events.empty() && mock.handles.empty() && !mock.invalid,
            "reset or destruction permits no late write to old or reused backing");
  }
}

int ReadOnlyCpuPendingTests() {
  const std::pair<const char *, void (*)()> groups[] = {
      {"surface creation allocation failures roll back CPU and imported batches", SurfaceCreationAllocationFailures},
      {"read-only exported Bob pending pixels", ReadOnlyExportedBobPendingPixels},
      {"writable exposure retains old admission", ReadOnlyExportedBobWritableExposureKeepsOldAdmission},
      {"read-only Bob cancellation releases owners", ReadOnlyExportedBobCancellationReleasesOwners}};
  unsigned failures = 0;
  for (const auto &group : groups) {
    try {
      group.second();
      printf("PASS %s\n", group.first);
    } catch (const std::exception &error) {
      ++failures;
      fprintf(stderr, "FAIL %s: %s\n", group.first, error.what());
    }
  }
  printf("VA-API read-only CPU pending: 3 groups, %u failures\n", failures);
  return failures == 0 ? 0 : 1;
}

void SyncTimeoutAdmissionDoesNotBlockZeroQueries() {
  for (const uint64_t timeout : {uint64_t{0}, uint64_t{1000000000},
                                 uint64_t{VA_TIMEOUT_INFINITE}}) {
    Fixture fixture;
    fixture.driver.StopWorkers();
    fixture.driver.stopping = false;
    const auto pixels = fixture.target->storage;
    const uint64_t generation = fixture.decoder->generation;
    const unsigned opens = device_open_attempts;
    struct Coordination {
      std::mutex mutex;
      std::condition_variable condition;
      bool holding = false, release = false, entering = false, done = false;
      bool returned_while_held = false;
      VAStatus status = VA_STATUS_ERROR_OPERATION_FAILED;
      std::thread holder, query;
      void Release() {
        {
          std::lock_guard<std::mutex> lock(mutex);
          release = true;
        }
        condition.notify_all();
      }
      ~Coordination() {
        Release();
        if (holder.joinable()) holder.join();
        if (query.joinable()) query.join();
      }
    } control;
    control.holder = std::thread([&] {
      std::unique_lock<std::mutex> driver_lock(fixture.driver.mutex);
      std::unique_lock<std::mutex> lock(control.mutex);
      control.holding = true;
      control.condition.notify_all();
      // A generous coordination bound, not an assertion about OS latency.
      control.condition.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return control.release; });
      control.holding = false;
    });
    {
      std::unique_lock<std::mutex> lock(control.mutex);
      Require(control.condition.wait_for(lock, std::chrono::seconds(2),
                                         [&] { return control.holding; }),
              "another thread holds the actual driver mutex before sync");
    }
    control.query = std::thread([&] {
      {
        std::lock_guard<std::mutex> lock(control.mutex);
        control.entering = true;
      }
      control.condition.notify_all();
      const VAStatus status = SyncSurface2(&fixture.context, 2, timeout);
      {
        std::lock_guard<std::mutex> lock(control.mutex);
        control.status = status;
        control.returned_while_held = control.holding;
        control.done = true;
      }
      control.condition.notify_all();
    });
    bool completed_while_held;
    {
      std::unique_lock<std::mutex> lock(control.mutex);
      Require(control.condition.wait_for(lock, std::chrono::seconds(2),
                                         [&] { return control.entering; }),
              "the synchronization query starts under controlled contention");
      completed_while_held = control.condition.wait_for(
          lock, timeout == 0 ? std::chrono::milliseconds(2000)
                             : std::chrono::milliseconds(20),
          [&] { return control.done; });
    }
    control.Release();
    control.holder.join();
    control.query.join();
    if (timeout == 0) {
      Require(completed_while_held && control.returned_while_held &&
                  control.status == VA_STATUS_ERROR_TIMEDOUT,
              "zero-timeout sync returns TIMEDOUT while another thread still holds the mutex");
    } else {
      Require(!completed_while_held && !control.returned_while_held &&
                  control.status == VA_STATUS_SUCCESS,
              "finite and infinite sync retain blocking admission until mutex release");
    }
    Require(SyncSurface2(&fixture.context, 2, 0) == VA_STATUS_SUCCESS &&
                SyncSurface2(&fixture.context, VA_INVALID_SURFACE, 0) ==
                    VA_STATUS_ERROR_INVALID_SURFACE &&
                fixture.target->ready && !fixture.target->failed &&
                fixture.target->storage == pixels &&
                fixture.target->vpp_readers == 0 && fixture.target->vpp_writers == 0 &&
                fixture.driver.pending_vpp.empty() &&
                fixture.decoder->generation == generation && device_open_attempts == opens,
            "released-mutex queries preserve ready/invalid results, pixels and lifecycle without hardware");
  }
}

void VppBacklogTurnPreservesCancellationAndOwnership() {
  // Control an intervening frontend at the new retired-operation boundary.
  // Assert queue/pixel/lifetime ordering, never scheduler latency or <1 ms.
  for (unsigned mode = 0; mode != 3; ++mode) {
    Fixture fixture;
    fixture.driver.StopWorkers();
    fixture.driver.stopping = false;
    const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    FillFieldRows(*picture);
    const auto source_bytes = picture->storage;
    picture->ready = false;
    VABufferID filter = CreateBobFilter(fixture);
    SetPipelineFilters(fixture, &filter, 1);
    fixture.SubmitParameters();
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
            "queue the first private Bob output");
    const auto next = Picture(0, 99);
    const auto untouched = next->storage;
    fixture.driver.surfaces[3] = next;
    Require(BeginPicture(&fixture.context, 2, 3) == VA_STATUS_SUCCESS &&
                RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                    VA_STATUS_SUCCESS &&
                EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                fixture.driver.pending_vpp.size() == 2 &&
                picture->vpp_readers == 2,
            "two queued outputs retain separate target ownership");
    picture->ready = true;
    bool entered = false, admitted = false, action_ok = false;
    fixture.driver.vpp_worker = std::thread([&] {
      vpp_worker_turn = [&](Driver *driver, std::unique_lock<std::mutex> &lock) {
        entered = driver == &fixture.driver && lock.owns_lock();
        lock.unlock();
        std::thread frontend([&] {
          VASurfaceStatus first_state = VASurfaceRendering;
          VASurfaceStatus next_state = VASurfaceReady;
          const bool states =
              QuerySurfaceStatus(&fixture.context, 2, &first_state) == VA_STATUS_SUCCESS &&
              QuerySurfaceStatus(&fixture.context, 3, &next_state) == VA_STATUS_SUCCESS &&
              first_state == VASurfaceReady && next_state == VASurfaceRendering;
          {
            std::lock_guard<std::mutex> frontend_lock(driver->mutex);
            admitted = states && driver->pending_vpp.size() == 1 &&
                !driver->pending_vpp.front().worker_active &&
                fixture.target->vpp_writers == 0 && next->vpp_writers == 1 &&
                picture->vpp_readers == 1 && next->storage == untouched;
          }
          if (mode == 0) {
            // Retire a completed VA ID while the worker's copied operation
            // still owns that object; the remaining output must stay valid.
            VASurfaceID completed = 2;
            action_ok = DestroySurfaces(&fixture.context, &completed, 1) == VA_STATUS_SUCCESS;
          } else if (mode == 1) {
            VASurfaceID canceled = 3;
            action_ok = DestroySurfaces(&fixture.context, &canceled, 1) == VA_STATUS_SUCCESS;
          } else {
            // Source retirement cancels the queued capture. A frontend sync
            // removes it before the worker resumes; stop must not reuse an
            // erased queue iterator or paint a replacement picture.
            VASurfaceID source = 1;
            action_ok = DestroySurfaces(&fixture.context, &source, 1) == VA_STATUS_SUCCESS &&
                SyncSurface2(&fixture.context, 3, 0) == VA_STATUS_ERROR_DECODING_ERROR;
            std::lock_guard<std::mutex> frontend_lock(driver->mutex);
            driver->stopping = true;
          }
        });
        frontend.join();
        lock.lock();
      };
      RunVppWorker(&fixture.driver);
      vpp_worker_turn = {};
    });
    const VAStatus status = SyncSurface2(&fixture.context, 3, 1000000000ULL);
    fixture.driver.StopWorkers();
    Require(entered && admitted && action_ok,
            "a frontend observes the completed first output and untouched queued successor");
    Require(fixture.driver.pending_vpp.empty() && picture->vpp_readers == 0 &&
                fixture.target->vpp_writers == 0 && next->vpp_writers == 0 &&
                picture->storage == source_bytes,
            "intervening retirement releases all owners without modifying source pixels");
    RequireFieldRows(*fixture.target, 0, true);
    if (mode == 0) {
      Require(status == VA_STATUS_SUCCESS && next->ready && !next->failed &&
                  next->frame_timestamp == kTimestampStep,
              "the successor completes with its exact captured identity");
      RequireFieldRows(*next, 0, true);
    } else {
      Require(status != VA_STATUS_SUCCESS && next->failed && !next->ready &&
                  next->storage == untouched,
              "cancellation or stop cannot acknowledge or overwrite the successor");
    }
  }
}

void VppPixelLeaseAdmitsFrontendAndRetainsMappings() {
  for (unsigned mode = 0; mode != 4; ++mode) {
    Fixture f;
    f.driver.StopWorkers();
    f.driver.stopping = false;
    auto picture = f.decoder->decoded_frames.at(kTimestampStep);
    FillFieldRows(*picture);
    const auto source_bytes = picture->storage;
    const auto untouched = f.target->storage;
    picture->ready = false;
    VABufferID filter = CreateBobFilter(f);
    SetPipelineFilters(f, &filter, 1);
    f.SubmitParameters();
    Require(EndPicture(&f.context, 2) == VA_STATUS_SUCCESS,
            "queue a real NV12 operation before its source completes");
    picture->ready = true;
    struct Control {
      Driver &driver;
      std::mutex mutex;
      std::condition_variable condition;
      bool entered = false, released = false, expired = false;
      bool destroy_entered = false;
      std::atomic<bool> action_done{false};
      VAStatus action_status = VA_STATUS_ERROR_OPERATION_FAILED;
      std::thread action;
      explicit Control(Driver &value) : driver(value) {}
      void Release() {
        { std::lock_guard<std::mutex> lock(mutex); released = true; }
        condition.notify_all();
      }
      ~Control() {
        Release();
        if (action.joinable()) action.join();
        driver.StopWorkers();
      }
    } control(f.driver);
    f.driver.vpp_worker = std::thread([&] {
      vpp_pixels = [&](Driver *, uint64_t) {
        std::unique_lock<std::mutex> lock(control.mutex);
        control.entered = true;
        control.condition.notify_all();
        if (!control.condition.wait_for(lock, std::chrono::seconds(3),
                                        [&] { return control.released; }))
          control.expired = true;
      };
      RunVppWorker(&f.driver);
    });
    {
      std::unique_lock<std::mutex> lock(control.mutex);
      Require(control.condition.wait_for(lock, std::chrono::seconds(2),
                                         [&] { return control.entered; }),
              "the worker enters its unlocked pixel lease");
    }
    VASurfaceStatus state = VASurfaceReady;
    Require(QuerySurfaceStatus(&f.context, 2, &state) == VA_STATUS_SUCCESS &&
                state == VASurfaceRendering &&
                BeginPicture(&f.context, 2, 2) == VA_STATUS_ERROR_HW_BUSY &&
                f.target->storage == untouched,
            "frontend admission is free while the target remains reserved and unready");
    if (mode == 1) {
      {
        std::lock_guard<std::mutex> lock(f.driver.mutex);
        f.decoder->Reset();
      }
      Require(SyncSurface2(&f.context, 2, 0) == VA_STATUS_ERROR_DECODING_ERROR,
              "a canceled active conversion rejects the read without retiring its mapping");
    } else if (mode == 2) {
      control.action = std::thread([&] {
        vpp_destroy_wait = [&] {
          { std::lock_guard<std::mutex> lock(control.mutex); control.destroy_entered = true; }
          control.condition.notify_all();
        };
        VASurfaceID target = 2;
        control.action_status = DestroySurfaces(&f.context, &target, 1);
        control.action_done = true;
        control.condition.notify_all();
      });
      {
        std::unique_lock<std::mutex> lock(control.mutex);
        Require(control.condition.wait_for(lock, std::chrono::seconds(2),
                                           [&] { return control.destroy_entered; }),
                "target destruction reaches its real mapping-quiescence boundary");
      }
      Require(!control.action_done && f.driver.surfaces.count(2) == 1,
              "destruction keeps the backing identity discoverable until the writer retires");
    } else if (mode == 3) {
      control.action = std::thread([&] {
        f.driver.StopWorkers();
        control.action_done = true;
      });
      std::unique_lock<std::mutex> lock(f.driver.mutex);
      Require(f.driver.condition.wait_for(lock, std::chrono::seconds(2),
                                          [&] { return f.driver.stopping; }),
              "stop is admitted before the pixel writer finishes");
      Require(!control.action_done, "stop joins the live allocation lease");
    }
    {
      std::lock_guard<std::mutex> lock(f.driver.mutex);
      Require(f.driver.pending_vpp.size() == 1 &&
                  f.driver.pending_vpp.front().pixels_active &&
                  f.target->vpp_writers == 1 && picture->vpp_readers == 1,
              "active work keeps both reader and writer owners across frontend actions");
    }
    control.Release();
    if (control.action.joinable()) control.action.join();
    const VAStatus synced = mode == 2 ? VA_STATUS_ERROR_INVALID_SURFACE
        : SyncSurface2(&f.context, 2, 1000000000ULL);
    f.driver.StopWorkers();
    Require(!control.expired && f.driver.pending_vpp.empty() &&
                f.target->vpp_writers == 0 && picture->vpp_readers == 0 &&
                picture->storage == source_bytes,
            "completion/cancellation joins all pixel owners and preserves input bytes");
    if (mode == 0) {
      Require(synced == VA_STATUS_SUCCESS && f.target->ready && !f.target->failed &&
                  f.target->frame_timestamp == kTimestampStep,
              "the exact queued picture is published only after pixel completion");
      RequireFieldRows(*f.target, 0, true);
    } else if (mode == 2) {
      Require(control.action_status == VA_STATUS_SUCCESS &&
                  f.driver.surfaces.count(2) == 0 && f.target->destroyed,
              "target destruction completes after pixel quiescence");
    } else {
      Require(synced != VA_STATUS_SUCCESS && !f.target->ready && f.target->failed,
              "reset/stop cannot publish a canceled private result");
    }
  }
  {
    Fixture f;
    f.driver.StopWorkers();
    VASurfaceID target = 2;
    allocation_fault::fail_next = true;
    const VAStatus status = DestroySurfaces(&f.context, &target, 1);
    allocation_fault::fail_next = false;
    Require(status == VA_STATUS_ERROR_ALLOCATION_FAILED &&
                f.driver.surfaces.count(2) == 1 && !f.target->destroyed,
            "mapping-quiescence allocation failure preserves the whole destroy request");
    int pipe_fds[2];
    Require(pipe(pipe_fds) == 0, "create an actual borrowed descriptor");
    f.target->object_fds.push_back(pipe_fds[0]);
    {
      PendingVpp operation;
      operation.source_owner = f.source;
      operation.target = operation.target_owner = f.target;
      VppPixelView input, output;
      Require(!CaptureVppPixels(operation, f.source, false, &input, &output),
              "CPU allocation alone does not make a public upload source immutable");
      Require(CaptureVppPixels(operation, f.decoder->decoded_frames.at(kTimestampStep),
                               false, &input, &output),
              "a separate private picture can pin an owned NV12 target");
      VppPixelView borrowed;
      borrowed.Capture(f.target);
    }
    Require(fcntl(pipe_fds[0], F_GETFD) >= 0,
            "a pixel snapshot never closes its allocation's borrowed descriptors");
    f.target->object_fds.clear();
    close(pipe_fds[0]);
    close(pipe_fds[1]);
  }
}

void ExposedQueuedVppStillRequiresFence() {
  FenceIoMock mock;
  MockFenceScope scope(mock);
  Fixture fixture;
  {
    std::lock_guard<std::mutex> lock(fixture.driver.mutex);
    fixture.driver.stopping = true;
  }
  fixture.driver.condition.notify_all();
  fixture.driver.vpp_worker.join();
  fixture.target->object_fds = {101};
  fixture.target->layout.valid = true;
  const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
  picture->ready = false;
  mock.fail_open = true;
  fixture.SubmitParameters();
  Require(EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_OPERATION_FAILED &&
              fixture.driver.pending_vpp.empty() && picture->vpp_readers == 0 &&
              fixture.target->vpp_writers == 0 && fixture.target->planes[0][0] == 99 &&
              mock.events == std::vector<std::string>{"open"} &&
              mock.started.empty() && mock.handles.empty() && !mock.invalid,
          "exposed backing cannot fall back to unfenced writes after fence failure");
}

void ExerciseExposedColdVppFallback(bool cache_ops_available) {
  Require(CpuCacheFlush().Available() == cache_ops_available,
          "exercise the actual CPU cache-operation capability, never a fabricated one");
  const std::vector<std::string> initial_events = cache_ops_available
      ? std::vector<std::string>{"open"}
      : std::vector<std::string>{};
  for (unsigned int mode = 0; mode != 9; ++mode) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    Fixture fixture;
    fixture.driver.StopWorkers();
    fixture.driver.stopping = false;
    fixture.decoder->decoder_started = true;
    fixture.target->object_fds = {101};
    fixture.target->layout.valid = true;
    const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    picture->ready = false;
    const auto original = fixture.target->storage;
    mock.fail_open = true;
    fixture.SubmitParameters();
    std::atomic<bool> finished{false};
    VAStatus status = VA_STATUS_ERROR_OPERATION_FAILED;
    unsigned int polls = 0;
    struct Join {
      Driver *driver;
      std::thread thread;
      ~Join() {
        if (!thread.joinable()) return;
        {
          std::lock_guard<std::mutex> lock(driver->mutex);
          driver->stopping = true;
        }
        driver->condition.notify_all();
        thread.join();
      }
    } end{&fixture.driver, {}};
    end.thread = std::thread([&] {
      MockFenceScope thread_scope(mock);
      synchronous_status_polls = &polls;
      status = EndPicture(&fixture.context, 2);
      synchronous_status_polls = nullptr;
      finished.store(true);
    });
    {
      std::unique_lock<std::mutex> lock(fixture.driver.mutex);
      Require(fixture.driver.condition.wait_for(lock, std::chrono::seconds(1), [&] {
                return !fixture.driver.pending_vpp.empty() || finished.load();
              }) && !finished.load() && fixture.driver.pending_vpp.size() == 1 &&
                  fixture.driver.pending_vpp.front().worker_active &&
                  picture->vpp_readers == 1 && fixture.target->vpp_writers == 1 &&
                  !fixture.target->ready && !fixture.target->failed &&
                  fixture.target->storage == original && mock.events == initial_events,
              "exposed cold fallback retains owners and cannot return unready success");
      if (mode == 1)
        ++fixture.decoder->generation;
      else if (mode == 2)
        picture->failed = true;
      else if (mode == 5)
        mock.fail_start_fd = 101;
      else if (mode == 6)
        mock.fail_end_fd = 101;
      if (mode == 0 || mode == 5 || mode == 6)
        picture->ready = true;
    }
    std::shared_ptr<Surface> replacement;
    if (mode == 3) {
      Require(DestroyContext(&fixture.context, 2) == VA_STATUS_SUCCESS,
              "destroy the waiting request's VPP context");
    } else if (mode == 4) {
      VASurfaceID target = 2;
      Require(DestroySurfaces(&fixture.context, &target, 1) == VA_STATUS_SUCCESS,
              "destroy the waiting request's target");
    } else if (mode == 7) {
      replacement = Picture(0, 77);
      {
        std::lock_guard<std::mutex> lock(fixture.driver.mutex);
        fixture.driver.surfaces[3] = replacement;
      }
      Require(BeginPicture(&fixture.context, 2, 3) == VA_STATUS_SUCCESS &&
                  RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                      VA_STATUS_SUCCESS,
              "a new Begin/Render uses a different target while the old End waits");
    } else if (mode == 8) {
      VASurfaceID source = 1;
      unsigned int stops = 0;
      const uint64_t generation = fixture.decoder->generation;
      synchronous_decoder_stops = &stops;
      const VAStatus destroyed = DestroySurfaces(&fixture.context, &source, 1);
      synchronous_decoder_stops = nullptr;
      Require(destroyed == VA_STATUS_SUCCESS && stops == 1 &&
                  !fixture.decoder->decoder_started &&
                  fixture.decoder->generation == generation + 1,
              "destroy the waiting request's public source surface");
    }
    fixture.driver.condition.notify_all();
    end.thread.join();
    Require(fixture.driver.pending_vpp.empty() && picture->vpp_readers == 0 &&
                fixture.target->vpp_writers == 0 && mock.started.empty() &&
                mock.handles.empty() && !mock.self_wait && !mock.invalid,
            "synchronous fallback completion/cancellation releases all owners exactly once");
    if (mode == 0) {
      auto expected_events = initial_events;
      expected_events.insert(expected_events.end(), {"start:101", "end:101"});
      Require(status == VA_STATUS_SUCCESS && fixture.target->ready &&
                  !fixture.target->failed &&
                  fixture.target->frame_timestamp == kTimestampStep &&
                  fixture.target->planes[0][0] == 40 &&
                  mock.events == expected_events,
              "without sw_sync, exposed output is actually committed before End succeeds");
    } else {
      Require(status != VA_STATUS_SUCCESS && !fixture.target->ready &&
                  fixture.target->failed,
              "failed/canceled synchronous conversion never acknowledges ready pixels");
      auto expected_events = initial_events;
      if (mode == 5)
        expected_events.push_back("start:101");
      else if (mode == 6)
        expected_events.insert(expected_events.end(), {"start:101", "end:101"});
      Require(mock.events == expected_events,
              "cancellation never writes; actual CPU START/END failures retain exact events");
      if (mode != 6)
        Require(fixture.target->storage == original,
                "cancellation or failed CPU START never changes destination pixels");
      if (mode == 7) {
        Require(fixture.processor->target == 3 &&
                    fixture.processor->have_vpp_parameters &&
                    fixture.processor->vpp_frame == picture &&
                    replacement->planes[0][0] == 77,
                "old End cannot erase or poison newer Begin/Render parameters");
        picture->ready = true;
        Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                    replacement->ready && replacement->planes[0][0] == 40,
                "the newer VPP transaction remains independently completable");
      }
    }
  }
}

void ExposedColdVppFallbackCompletesBeforeReturning() {
  ExerciseExposedColdVppFallback(true);
}

#if defined(CRYSTALHD_VPP_CPU_ONLY) && defined(CRYSTALHD_LEGACY_CPU)
void LegacyExposedColdVppFallbackCompletesBeforeReturning() {
  ExerciseExposedColdVppFallback(false);
}

void LegacyExposedStoppedVppRemainsRejected() {
  Require(!CpuCacheFlush().Available(), "legacy CPU has no special cache-operation path");
  FenceIoMock mock;
  MockFenceScope scope(mock);
  Fixture fixture;
  fixture.driver.StopWorkers();
  fixture.target->object_fds = {101};
  fixture.target->layout.valid = true;
  const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
  picture->ready = false;
  fixture.SubmitParameters();
  Require(EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_OPERATION_FAILED &&
              fixture.driver.pending_vpp.empty() && picture->vpp_readers == 0 &&
              fixture.target->vpp_writers == 0 && fixture.target->planes[0][0] == 99 &&
              fixture.target->failed && !fixture.target->ready &&
              mock.events.empty() && mock.started.empty() && mock.handles.empty() &&
              !mock.invalid && !mock.self_wait,
          "stopped legacy source rejects without CPU/fence events or altered pixels/owners");
}
#endif

void UnsafeFenceFailuresCannotUseSynchronousFallback() {
  for (unsigned int mode = 0; mode != 3; ++mode) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    Fixture fixture;
    fixture.driver.StopWorkers();
    fixture.driver.stopping = false;
    fixture.target->object_fds = {101};
    fixture.target->layout.valid = true;
    const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    picture->ready = false;
    if (mode == 0)
      mock.fail_start_fd = 101;
    else if (mode == 1) {
      fixture.target->object_fds = {101, 102};
      mock.fail_start_fd = 102;
      mock.fail_end_fd = 101;
    } else {
      mock.fail_import_fd = 101;
    }
    fixture.SubmitParameters();
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_OPERATION_FAILED &&
                fixture.driver.pending_vpp.empty() && picture->vpp_readers == 0 &&
                fixture.target->vpp_writers == 0 &&
                fixture.target->planes[0][0] == 99 &&
                mock.started.empty() && mock.handles.empty() &&
                !mock.self_wait && !mock.invalid,
            "CPU acquisition/cleanup or attempted-import failure cannot become synchronous success");
  }
}

void ReuseBetweenRenderAndEndKeepsCapturedPicture() {
  Fixture fixture;
  fixture.SubmitParameters();
  // Model SubmitPicture reclaiming a completed old picture and assigning the
  // public VA surface to the next decode, between VPP Render and End calls.
  fixture.decoder->decoded_frames.erase(kTimestampStep);
  fixture.decoder->decoded_frames[2 * kTimestampStep] =
      Picture(2 * kTimestampStep, 80);
  fixture.decoder->surface_timestamps[fixture.source.get()] = 2 * kTimestampStep;
  fixture.source->expected_timestamp = 2 * kTimestampStep;
  fixture.source->frame_timestamp = 2 * kTimestampStep;
  fixture.Complete(kTimestampStep, 40);
  fixture.SubmitParameters();
  fixture.Complete(2 * kTimestampStep, 80);
}

void ResetRejectsCapturedOldEpoch() {
  Fixture fixture;
  fixture.SubmitParameters();
  fixture.decoder->Reset();
  Require(EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_DECODING_ERROR,
          "reject captured picture from an earlier decoder epoch");
  Require(fixture.target->failed && !fixture.target->ready,
          "old-epoch target must report failure");
  Require(fixture.target->planes[0][0] == 99,
          "reset must not paint a substitute frame");
  VASurfaceStatus surface_status = VASurfaceReady;
  Require(QuerySurfaceStatus(&fixture.context, 2, &surface_status) ==
              VA_STATUS_ERROR_DECODING_ERROR,
          "asynchronous surface status must expose failure");
}

void MissingPictureIsNotSuccessfulFallback() {
  Fixture fixture;
  fixture.decoder->Reset();
  Require(BeginPicture(&fixture.context, 2, 2) == VA_STATUS_SUCCESS, "begin VPP");
  Require(RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
              VA_STATUS_ERROR_DECODING_ERROR,
          "missing private decode picture must fail");
  Require(fixture.target->planes[0][0] == 99,
          "missing picture must not paint a substitute frame");
}

void BusyTargetIsNotReplaced() {
  Fixture fixture;
  fixture.target->vpp_writers = 1;
  fixture.target->expected_timestamp = 7 * kTimestampStep;
  Require(BeginPicture(&fixture.context, 2, 2) == VA_STATUS_ERROR_HW_BUSY,
          "reject reuse of an output with an outstanding writer");
  Require(fixture.target->expected_timestamp == 7 * kTimestampStep &&
              fixture.target->planes[0][0] == 99,
          "busy output must retain its owner and pixels");
  fixture.target->vpp_writers = 0;
}

void RetiredContextDoesNotAcknowledgeAFrame() {
  Fixture fixture;
  fixture.SubmitParameters();
  Require(DestroyContext(&fixture.context, 2) == VA_STATUS_SUCCESS,
          "destroy processing context");
  Require(EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_INVALID_CONTEXT,
          "late end on a retired context must not acknowledge a picture");
  Require(fixture.target->planes[0][0] == 99,
          "retired context must leave destination unchanged");
}

void MalformedSurfaceTeardownIsAtomic() {
  Fixture fixture;
  const uint64_t generation = fixture.decoder->generation;
  VASurfaceID invalid[] = {1, VA_INVALID_SURFACE};
  VASurfaceID duplicate[] = {1, 1};
  Require(DestroySurfaces(&fixture.context, invalid, 2) ==
              VA_STATUS_ERROR_INVALID_SURFACE,
          "reject a teardown list ending in an invalid surface");
  Require(DestroySurfaces(&fixture.context, duplicate, 2) ==
              VA_STATUS_ERROR_INVALID_SURFACE,
          "reject duplicate teardown IDs before removing a surface");
  Require(DestroySurfaces(&fixture.context, nullptr, 1) ==
              VA_STATUS_ERROR_INVALID_PARAMETER,
          "reject a null teardown list");
  Require(DestroySurfaces(&fixture.context, invalid, -1) ==
              VA_STATUS_ERROR_INVALID_PARAMETER,
          "reject a negative teardown count");
  Require(fixture.driver.surfaces.size() == 2 &&
              fixture.driver.surfaces.at(1) == fixture.source &&
              !fixture.source->destroyed && !fixture.target->destroyed &&
              fixture.decoder->generation == generation &&
              fixture.decoder->decoded_frames.count(kTimestampStep) == 1,
          "rejected teardown must not erase, poison, or reset any state");
  fixture.SubmitParameters();
  fixture.Complete(kTimestampStep, 40);
}

void ValidSurfaceTeardownRetiresQueuedEpoch() {
  Fixture fixture;
  PendingVpp pending;
  pending.decoder = fixture.decoder;
  pending.source = fixture.decoder->decoded_frames.at(kTimestampStep);
  pending.target = fixture.target;
  pending.target_owner = fixture.target;
  pending.sequence = fixture.target->latest_vpp_sequence;
  pending.decoder_generation = fixture.decoder->generation;
  VASurfaceID source_id = 1;
  Require(!PendingVppCanceled(&fixture.driver, pending),
          "old epoch initially remains valid");
  Require(DestroySurfaces(&fixture.context, &source_id, 1) == VA_STATUS_SUCCESS,
          "destroy a valid decode surface");
  Require(fixture.source->destroyed && fixture.driver.surfaces.count(1) == 0 &&
              fixture.decoder->generation == pending.decoder_generation + 1 &&
              PendingVppCanceled(&fixture.driver, pending),
          "valid teardown must retire private queued pictures with the pool");
  Require(ReadDecodeControlNotifications(fixture.driver) == 1,
          "decode surface reset must still wake the decoder");
}

void CanceledVppFailsWithoutOverwriting() {
  Fixture fixture;
  std::lock_guard<std::mutex> lock(fixture.driver.mutex);
  PendingVpp pending;
  pending.decoder = fixture.decoder;
  pending.source = fixture.decoder->decoded_frames.at(kTimestampStep);
  pending.target = fixture.target;
  pending.target_owner = fixture.target;
  pending.source_timestamp = kTimestampStep;
  pending.sequence = 1;
  pending.decoder_generation = fixture.decoder->generation;
  pending.source->vpp_readers = 1;
  fixture.target->vpp_writers = 1;
  fixture.target->latest_vpp_sequence = 1;
  fixture.driver.pending_vpp.push_back(pending);
  ReleasePendingVpp(&fixture.driver, 1, VA_STATUS_ERROR_OPERATION_FAILED, false);
  Require(fixture.target->failed && !fixture.target->ready,
          "canceled VPP must report failed target");
  Require(fixture.target->planes[0][0] == 99,
          "canceled VPP must not replace pixels with another frame");
  Require(fixture.decoder->decoded_frames.count(kTimestampStep) == 1,
          "cancellation must not retire the live source picture");
  Require(pending.source->vpp_readers == 0 && fixture.target->vpp_writers == 0,
          "cancellation must release reader and writer ownership");
}

void CpuWriteStartUnwindsAcquiredPrefix() {
  FenceIoMock mock;
  MockFenceScope scope(mock);
  Surface surface;
  surface.object_fds = {101, 102, 103};
  mock.fail_start_fd = 102;
  Require(!surface.BeginCpuWrite(), "partial CPU START must fail");
  Require(mock.events == std::vector<std::string>{
              "start:101", "start:102", "end:101"},
          "failed START must unwind only its acquired prefix");
  Require(mock.started.empty() && !mock.invalid,
          "failed CPU acquisition must leave no ownership");
}

void FencedWriteStartFailureClosesUnpublishedFence() {
  FenceIoMock mock;
  MockFenceScope scope(mock);
  Surface surface;
  surface.object_fds = {101, 102, 103};
  mock.fail_start_fd = 102;
  bool fallback = true;
  Require(BeginFencedWrite(&surface, CpuCacheFlush(), &fallback) == -1 && !fallback,
          "fenced CPU START must fail without synchronous fallback");
  Require(mock.events == std::vector<std::string>{
              "open", "create", "start:101", "start:102", "end:101",
              "close:7002", "close:7001"},
          "failed CPU acquisition unwinds its prefix and closes the unpublished fence");
  Require(mock.started.empty() && mock.handles.empty() && !mock.invalid,
          "failed fenced acquisition must not leak ownership or handles");
}

void FenceCreationFailureAvoidsCpuOwnership() {
  for (bool fail_open : {true, false}) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    Surface surface;
    surface.object_fds = {101, 102};
    mock.fail_open = fail_open;
    mock.fail_create = !fail_open;
    bool fallback = false;
    Require(BeginFencedWrite(&surface, CpuCacheFlush(), &fallback) == -1 && fallback,
            "timeline open or fence creation failure must be reported");
    const std::vector<std::string> expected = fail_open
        ? std::vector<std::string>{"open"}
        : std::vector<std::string>{"open", "create", "close:7001"};
    Require(mock.events == expected,
            "unavailable fence must not acquire CPU ownership of an untouched target");
    Require(mock.started.empty() && mock.handles.empty() &&
                mock.imported.empty() && !mock.signaled && !mock.self_wait &&
                !mock.invalid,
            "failed fence creation must release all CPU ownership and handles");
  }
}

void PartialFenceImportAbortsBeforeCpuEnd() {
  for (int fail_fd : {101, 102, 103}) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    Surface surface;
    surface.object_fds = {101, 102, 103};
    mock.fail_import_fd = fail_fd;
    Require(BeginFencedWrite(&surface) == -1,
            "partial fence import must not acknowledge an async writer");
    const auto abort = std::find(mock.events.begin(), mock.events.end(),
                                 "close:" + std::to_string(FenceIoMock::timeline));
    const auto end = std::find(mock.events.begin(), mock.events.end(), "end:101");
    Require(abort != mock.events.end() && abort < end &&
                std::find(mock.events.begin(), mock.events.end(), "signal") ==
                    mock.events.end() && mock.fence_status < 0,
            "partially imported fence must abort, never succeed, before any CPU END");
    Require(mock.started.empty() && mock.handles.empty() && mock.signaled &&
                !mock.self_wait && !mock.invalid,
            "partial import must not leak ownership or wait on its own fence");
  }
}

void CompletedFenceSignalsBeforeCpuEnd() {
  FenceIoMock mock;
  MockFenceScope scope(mock);
  Surface surface;
  surface.object_fds = {101, 102, 103};
  const int timeline = BeginFencedWrite(&surface);
  Require(timeline == FenceIoMock::timeline && mock.started.size() == 3 &&
              mock.imported.size() == 3 && !mock.signaled &&
              mock.handles == std::unordered_set<int>{timeline},
          "successful fence creation must retain CPU ownership until completion");
  EndFencedWrite(&surface, timeline);
  Require(mock.started.empty() && mock.handles.empty() && mock.signaled &&
              mock.fence_status == 1 && !mock.self_wait && !mock.invalid,
          "normal completion must release its fence before CPU ownership");
}

void VppCpuOwnershipFailuresAreNotSuccessfulFrames() {
  for (bool argb : {false, true}) {
    for (bool fail_start : {true, false}) {
      FenceIoMock mock;
      MockFenceScope scope(mock);
      Fixture fixture;
      fixture.target->object_fds = {101, 102, 103};
      if (argb) {
        fixture.target->fourcc = VA_FOURCC_ARGB;
        fixture.target->pitch[0] = fixture.target->width * 4;
        fixture.target->storage.assign(
            fixture.target->pitch[0] * fixture.target->height, 99);
        fixture.target->planes[0] = fixture.target->storage.data();
        fixture.target->planes[1] = nullptr;
      }
      const std::vector<uint8_t> old_pixels = fixture.target->storage;
      mock.fail_start_fd = fail_start ? 102 : -1;
      mock.fail_end_fd = fail_start ? -1 : 102;
      fixture.SubmitParameters();
      Require(EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_OPERATION_FAILED,
              "failed CPU START or END must not acknowledge completed VPP");
      Require(fixture.target->failed && !fixture.target->ready,
              "CPU ownership failure must report a failed output surface");
      if (fail_start)
        Require(fixture.target->storage == old_pixels,
                "failed CPU START must not write any destination pixels");
      Require(mock.started.empty() && !mock.invalid,
              "VPP failure must attempt cleanup of all acquired objects");
    }
  }
}

void CpuReadStartUnwindsAcquiredPrefix() {
  FenceIoMock mock;
  MockFenceScope scope(mock);
  Surface surface;
  surface.object_fds = {101, 102, 103};
  mock.fail_read_start_fd = 102;
  surface.BeginCpuRead();
  Require(mock.events == std::vector<std::string>{
              "read-start:101", "read-start:102", "read-end:101"},
          "failed READ START must unwind only the acquired prefix");
  Require(mock.read_started.empty() && !mock.invalid,
          "failed READ acquisition cannot retain ownership");
}

void CpuSyncRetriesInterruptedAccess() {
  for (uint64_t direction : {uint64_t(DMA_BUF_SYNC_READ),
                             uint64_t(DMA_BUF_SYNC_WRITE)}) {
    for (uint64_t boundary : {uint64_t(DMA_BUF_SYNC_START),
                              uint64_t(DMA_BUF_SYNC_END)}) {
      FenceIoMock mock;
      MockFenceScope scope(mock);
      Surface surface;
      surface.object_fds = {101};
      mock.transient_flags = direction | boundary;
      mock.transient_errors = {EINTR, EAGAIN};
      if (direction == DMA_BUF_SYNC_READ) {
        surface.BeginCpuRead();
        surface.EndCpuRead();
      } else {
        Require(surface.BeginCpuWrite() && surface.EndCpuWrite(),
                "transient WRITE synchronization errors must be retried");
      }
      Require(mock.transient_errors.empty() && mock.started.empty() &&
                  mock.read_started.empty() && !mock.invalid,
              "READ/WRITE START/END must retry EINTR and EAGAIN");
    }
  }
}

void CpuReadFailuresCannotCompleteCopies() {
  for (bool fail_start : {true, false}) {
    for (int operation : {0, 1, 2}) {
      FenceIoMock mock;
      MockFenceScope scope(mock);
      Fixture fixture;
      auto source = fixture.decoder->decoded_frames.at(kTimestampStep);
      source->object_fds = {101, 102, 103};
      mock.fail_read_start_fd = fail_start ? 102 : -1;
      mock.fail_read_end_fd = fail_start ? -1 : 102;
      if (operation == 2) {
        fixture.target->fourcc = VA_FOURCC_ARGB;
        fixture.target->pitch[0] = fixture.target->width * 4;
        fixture.target->storage.assign(
            fixture.target->pitch[0] * fixture.target->height, 99);
        fixture.target->planes[0] = fixture.target->storage.data();
        fixture.target->planes[1] = nullptr;
      }
      const auto previous = fixture.target->storage;
      if (operation != 0) {
        fixture.SubmitParameters();
        Require(EndPicture(&fixture.context, 2) == VA_STATUS_ERROR_OPERATION_FAILED &&
                    fixture.target->failed && !fixture.target->ready,
                "READ failure must fail VPP output status");
      } else {
        fixture.target->ready = false;
        Require(!CopyNv12Surface(*source, fixture.target.get()) &&
                    !fixture.target->ready,
                "READ failure must not complete an NV12 copy");
      }
      if (fail_start)
        Require(previous == fixture.target->storage,
                "failed READ START must not write destination pixels");
      Require(mock.read_started.empty() && !mock.invalid,
              "all acquired READ objects must receive END on failure");
    }
  }
}

VAImage MakeImage(Fixture *fixture, int width = 16, int height = 16) {
  VAImageFormat format = {};
  format.fourcc = VA_FOURCC_NV12;
  VAImage image = {};
  Require(CreateImage(&fixture->context, &format, width, height, &image) ==
              VA_STATUS_SUCCESS,
          "create private NV12 image");
  return image;
}

void CenteredReadOnlyBobPending() {
  for (unsigned parity : {0U, 1U}) {
    for (unsigned mode = 0; mode != 4; ++mode) {
      FenceIoMock mock;
      MockFenceScope scope(mock);
      ReadOnlyBobFixture fixture(mock, parity, 256, 32);
      crystalhd_centered_output::Config config;
      crystalhd_centered_output::Geometry geometry;
      Require(crystalhd_centered_output::ParseConfig("128x16@256x32:limited", &config) &&
                  crystalhd_centered_output::BuildGeometry(config, 256, 32, 4, &geometry) &&
                  geometry.x == 64 && geometry.y == 8,
              "validate a small centered canvas with horizontal and vertical borders");
      const auto picture = std::make_shared<Surface>();
      const auto small = std::make_shared<Surface>();
      fixture.source = std::make_shared<Surface>();
      Require(fixture.source->AllocateCpuNv12(256, 32) &&
                  picture->AllocateCpuNv12(256, 32) && small->AllocateCpuNv12(128, 16),
              "allocate independent public canvas, captured canvas and immutable sidecar");
      fixture.decoder->id = 1;
      fixture.decoder->centered_geometry = geometry;
      fixture.source->id = 1;
      fixture.source->expected_timestamp = kTimestampStep;
      fixture.source->decode_identity = 41;
      picture->expected_timestamp = small->expected_timestamp = kTimestampStep;
      picture->decode_identity = small->decode_identity = 41;
      picture->trace_context = small->trace_context = 1;
      picture->trace_generation = small->trace_generation = fixture.decoder->generation;
      picture->centered_geometry = geometry;
      picture->centered_pixels = small;
      // A full-canvas fallback would read this poison instead of sidecar pixels.
      std::fill(picture->storage.begin(), picture->storage.end(), uint8_t{231});
      for (unsigned plane = 0; plane != 2; ++plane)
        for (unsigned row = 0; row != (plane == 0 ? 16U : 8U); ++row)
          for (unsigned column = 0; column != 128; ++column)
            small->planes[plane][row * small->pitch[plane] + column] =
                static_cast<uint8_t>((plane == 0 ? 20 : 80) + row * 5 + column % 37);
      const auto sidecar_bytes = small->storage;
      const auto canvas_bytes = picture->storage;
      fixture.driver.surfaces[1] = fixture.source;
      fixture.decoder->surface_timestamps.clear();
      fixture.decoder->surface_timestamps[fixture.source.get()] = kTimestampStep;
      fixture.decoder->decoded_frames[kTimestampStep] = picture;
      fixture.Queue();
      Require(fixture.driver.pending_vpp.front().source == picture &&
                  fixture.driver.pending_vpp.front().source->centered_pixels == small,
              "cold read-only Bob retains the exact centered source and sidecar");
      const VAImage image = MakeImage(&fixture, 256, 32);
      VASurfaceStatus state = VASurfaceReady;
      Require(QuerySurfaceStatus(&fixture.context, 2, &state) == VA_STATUS_SUCCESS &&
                  state == VASurfaceRendering &&
                  SyncSurface2(&fixture.context, 2, 0) == VA_STATUS_ERROR_TIMEDOUT &&
                  SyncSurface2(&fixture.context, 2, 1000000ULL) == VA_STATUS_ERROR_TIMEDOUT &&
                  GetImage(&fixture.context, 2, 0, 0, 256, 32, image.image_id) ==
                      VA_STATUS_ERROR_SURFACE_BUSY && mock.events.empty(),
              "pending centered output is neither ready nor readable and opens no timeline");
      const auto target = fixture.target;
      std::vector<uint8_t> before(target->object_sizes[0]);
      memcpy(before.data(), target->object_maps[0], before.size());
      std::shared_ptr<Surface> replacement;
      if (mode == 1) {
        fixture.decoder->Reset();
      } else if (mode >= 2) {
        VASurfaceID retired = mode == 2 ? 2 : 1;
        Require(DestroySurfaces(&fixture.context, &retired, 1) == VA_STATUS_SUCCESS,
                "destroy the centered pending target or its original source owner");
        if (mode == 2) {
          replacement = std::make_shared<Surface>();
          Require(replacement->AllocateCpuNv12(256, 32), "allocate a replacement VA target");
          std::fill(replacement->storage.begin(), replacement->storage.end(), uint8_t{77});
          replacement->id = 2;
          fixture.driver.surfaces[2] = replacement;
        }
      }
      picture->ready = small->ready = true;
      picture->frame_timestamp = small->frame_timestamp = kTimestampStep;
      fixture.StartWorker(mock);
      if (mode == 0) {
        Require(SyncSurface2(&fixture.context, 2, 1000000000ULL) == VA_STATUS_SUCCESS,
                "Sync waits for the actual worker to complete centered read-only Bob");
      } else {
        std::unique_lock<std::mutex> lock(fixture.driver.mutex);
        Require(fixture.driver.condition.wait_for(lock, std::chrono::seconds(1), [&] {
                  return fixture.driver.pending_vpp.empty();
                }), "the actual worker retires canceled centered Bob");
      }
      fixture.driver.StopWorkers();
      Require(fixture.driver.pending_vpp.empty() && picture->vpp_readers == 0 &&
                  target->vpp_writers == 0 && small->storage == sidecar_bytes &&
                  picture->storage == canvas_bytes && mock.handles.empty() &&
                  mock.started.empty() && !mock.invalid && !mock.self_wait,
              "centered ownership retires without modifying immutable source storage");
      if (mode != 0) {
        Require(!target->ready && target->failed &&
                    memcmp(before.data(), target->object_maps[0], before.size()) == 0 &&
                    (!replacement || std::all_of(replacement->storage.begin(),
                         replacement->storage.end(), [](uint8_t value) { return value == 77; })) &&
                    mock.events.empty(),
                "reset and destruction permit no late writes to old or reused centered targets");
        continue;
      }
      Require(target->ready && !target->failed &&
                  target->expected_timestamp == kTimestampStep &&
                  target->frame_timestamp == kTimestampStep &&
                  fixture.driver.vpp_scaler == nullptr &&
                  mock.events == std::vector<std::string>{
                      "start:" + std::to_string(target->object_fds[0]),
                      "end:" + std::to_string(target->object_fds[0])} &&
                  GetImage(&fixture.context, 2, 0, 0, 256, 32, image.image_id) ==
                      VA_STATUS_SUCCESS,
              "completed centered Bob has exact identity and becomes readable without swscale or fences");
      for (unsigned plane = 0; plane != 2; ++plane) {
        const unsigned first = plane == 0 ? 8U : 4U;
        const unsigned rows = plane == 0 ? 32U : 16U;
        const unsigned active = plane == 0 ? 16U : 8U;
        for (unsigned row = 0; row != rows; ++row)
          for (unsigned column = 0; column != 256; ++column) {
            const bool inside = row >= first && row < first + active &&
                                column >= 64 && column < 192;
            const unsigned selected_row = inside ? ((row - first) / 2U) * 2U + parity : 0;
            const uint8_t expected = inside
                ? static_cast<uint8_t>((plane == 0 ? 20 : 80) + selected_row * 5 +
                                       (column - 64) % 37)
                : static_cast<uint8_t>(plane == 0 ? 16 : 128);
            Require(target->planes[plane][row * target->pitch[plane] + column] == expected,
                    "all centered Bob YUV pixels and limited-range borders match an independent oracle");
          }
      }
    }
  }
}

int CenteredReadOnlyPendingTestEntry() {
  try {
    CenteredReadOnlyBobPending();
    puts("VA-API centered read-only pending Bob pixels/readiness/cancellation PASS");
    return 0;
  } catch (const std::exception &error) {
    fprintf(stderr, "VA-API centered read-only pending Bob failed: %s\n", error.what());
    return 1;
  }
}

struct DerivedFixture : Fixture {
  DerivedFixture() { source->id = 1; target->id = 2; }
};

void ImageAllocationFailure(bool derive, bool bgra, unsigned allocation) {
  Driver driver{-1};
  driver.StopWorkers();
  VADriverContext context = {};
  context.pDriverData = &driver;
  const uint32_t fourcc = bgra ? VA_FOURCC_BGRA : VA_FOURCC_NV12;
  auto source = bgra ? PrivateArgbOutput() : Picture(0, 71);
  source->fourcc = fourcc;
  source->id = 1;
  driver.surfaces[source->id] = source;
  const auto source_pixels = source->storage;
  VAImageFormat format = ImageFormatFor(fourcc);
  VAImage retained = {};
  Require(CreateImage(&context, &format, 8, 8, &retained) == VA_STATUS_SUCCESS,
          "create an existing image before image allocation failure");
  auto &retained_bytes = driver.buffers.at(retained.buf).data;
  std::fill(retained_bytes.begin(), retained_bytes.end(), 109);
  const auto retained_pixels = retained_bytes;
  // Keep bucket growth out of this narrow three-allocation sequence:
  // image bytes, the buffer registry node, then the image registry node.
  driver.buffers.reserve(4);
  driver.images.reserve(4);
  const VABufferID next_buffer = driver.next_buffer;
  const VAImageID next_image = driver.next_image;
  VAImage output;
  memset(&output, 0xa5, sizeof(output));
  VAImage untouched;
  memcpy(&untouched, &output, sizeof(output));
  struct ResetFault {
    ~ResetFault() {
      allocation_fault::fail_next = false;
      allocation_fault::only_size = 0;
      allocation_fault::skip_matches = 0;
    }
  } reset;
  allocation_fault::failed_size = 0;
  allocation_fault::only_size = 0;
  allocation_fault::skip_matches = allocation - 1;
  allocation_fault::fail_next = true;
  bool escaped = false;
  VAStatus status = VA_STATUS_SUCCESS;
  try {
    status = derive ? DeriveImage(&context, source->id, &output)
                    : CreateImage(&context, &format, 16, 16, &output);
  } catch (const std::bad_alloc &) {
    escaped = true;
  }
  const bool injected = !allocation_fault::fail_next;
  const size_t failed_bytes = allocation_fault::failed_size;
  allocation_fault::fail_next = false;
  allocation_fault::only_size = 0;
  allocation_fault::skip_matches = 0;
  printf("%s %s allocation %u: injected=%d escaped=%d status=%#x "
         "buffers=%zu images=%zu output_unchanged=%d "
         "next_buffer=%#x expected_buffer=%#x next_image=%#x expected_image=%#x\n",
         derive ? "DeriveImage" : "CreateImage", bgra ? "BGRA" : "NV12",
         allocation, injected, escaped, status, driver.buffers.size(),
         driver.images.size(), memcmp(&output, &untouched, sizeof(output)) == 0,
         driver.next_buffer, next_buffer, driver.next_image, next_image);
  Require(injected && (allocation != 1 || failed_bytes == (bgra ? 1024U : 384U)),
          "inject the requested image bytes or publication-node allocation");
  Require(!escaped && status == VA_STATUS_ERROR_ALLOCATION_FAILED,
          "image allocation failure returns a VA error without escaping the callback");
  Require(memcmp(&output, &untouched, sizeof(output)) == 0 &&
              driver.buffers.size() == 1 && driver.images.size() == 1 &&
              driver.next_buffer == next_buffer && driver.next_image == next_image &&
              driver.images.at(retained.image_id).va.buf == retained.buf &&
              driver.buffers.at(retained.buf).data == retained_pixels,
          "failed image publication preserves caller output, prior images and IDs");
  Require(source->storage == source_pixels && source->ready && !source->failed &&
              !source->image_write_locked && source->vpp_writers == 0 &&
              source->export_waiters == 0 && !source->external_accessed,
          "failed derived-image allocation changes no source pixels or ownership");
  status = derive ? DeriveImage(&context, source->id, &output)
                  : CreateImage(&context, &format, 16, 16, &output);
  Require(status == VA_STATUS_SUCCESS && output.buf == next_buffer &&
              output.image_id == next_image && output.format.fourcc == fourcc &&
              driver.buffers.size() == 2 && driver.images.size() == 2,
          "image creation recovers after every allocation failure");
  if (derive)
    Require(driver.buffers.at(output.buf).derived_source == source &&
                driver.buffers.at(output.buf).derived_owner == source &&
                source->external_accessed && !source->image_write_locked &&
                source->vpp_writers == 0,
            "successful derived image retains its exact source without a write borrow");
  Require(DestroyImage(&context, output.image_id) == VA_STATUS_SUCCESS &&
              driver.buffers.size() == 1 && driver.images.size() == 1 &&
              source->storage == source_pixels &&
              DestroyImage(&context, retained.image_id) == VA_STATUS_SUCCESS &&
              driver.buffers.empty() && driver.images.empty(),
          "recovered image and retained image both retire without orphan buffers");
}

void ImageAllocationFailuresAreTransactional() {
  for (bool derive : {false, true})
    for (bool bgra : {false, true})
      for (unsigned allocation = 1; allocation <= 3; ++allocation)
        ImageAllocationFailure(derive, bgra, allocation);
}

VAStatus MapImageIntent(Fixture *fixture, VABufferID buffer, void **data,
                        uint32_t flags) {
  if (flags == UINT32_MAX) return MapBuffer(&fixture->context, buffer, data);
#if VA_CHECK_VERSION(1, 21, 0)
  return MapBuffer2(&fixture->context, buffer, data, flags);
#else
  return MapBufferWithFlags(&fixture->context, buffer, data, flags);
#endif
}

void BgraImagesPreserveBytesAndDerivedOwnership() {
  DerivedFixture fixture;
  fixture.target = PrivateArgbOutput();
  fixture.target->id = 2;
  fixture.target->fourcc = VA_FOURCC_BGRA;
  fixture.driver.surfaces[2] = fixture.target;
  VAImageFormat formats[2] = {};
  int count = 0;
  Require(QueryImageFormats(&fixture.context, formats, &count) == VA_STATUS_SUCCESS &&
              count == 2 && formats[0].fourcc == VA_FOURCC_NV12 &&
              formats[1].fourcc == VA_FOURCC_BGRA && formats[1].bits_per_pixel == 32 &&
              formats[1].red_mask == 0x00ff0000 && formats[1].alpha_mask == 0xff000000,
          "BGRA byte order is advertised independently of the legacy ARGB alias");
  for (unsigned y = 0; y < 16; ++y)
    for (unsigned x = 0; x < 16; ++x) {
      auto *pixel = fixture.target->planes[0] + y * fixture.target->pitch[0] + 4 * x;
      pixel[0] = x + 3; pixel[1] = y + 41; pixel[2] = x + y + 151; pixel[3] = 255;
    }
  const auto original = fixture.target->storage;
  VAImage image{};
  Require(CreateImage(&fixture.context, &formats[1], 5, 3, &image) == VA_STATUS_SUCCESS &&
              image.num_planes == 1 && image.pitches[0] == 20 && image.data_size == 60 &&
              image.pitches[1] == 0 && image.offsets[1] == 0 &&
              GetImage(&fixture.context, 2, 0, 0, 5, 3, image.image_id) == VA_STATUS_SUCCESS,
          "odd BGRA images use one checked packed plane and four bytes per pixel");
  void *mapped = nullptr;
  Require(MapImageIntent(&fixture, image.buf, &mapped, 1) == VA_STATUS_SUCCESS,
          "map packed BGRA readback");
  for (unsigned row = 0; row < 3; ++row)
    Require(memcmp(static_cast<uint8_t *>(mapped) + row * 20,
                   original.data() + row * fixture.target->pitch[0], 20) == 0,
            "readback preserves B/G/R/A independently, without channel swaps");
  Require(UnmapBuffer(&fixture.context, image.buf) == VA_STATUS_SUCCESS,
          "unmap packed readback");
  fixture.target->ready = false;
  Require(GetImage(&fixture.context, 2, 0, 0, 5, 3, image.image_id) ==
              VA_STATUS_ERROR_SURFACE_BUSY,
          "pending BGRA cannot expose a previous image");
  fixture.target->ready = true;
  fixture.target->failed = true;
  Require(GetImage(&fixture.context, 2, 0, 0, 5, 3, image.image_id) ==
              VA_STATUS_ERROR_DECODING_ERROR,
          "failed BGRA is not successful readback");
  fixture.target->failed = false;
  auto &bytes = fixture.driver.buffers.at(image.buf).data;
  bytes.resize(59);
  Require(GetImage(&fixture.context, 2, 0, 0, 5, 3, image.image_id) ==
              VA_STATUS_ERROR_INVALID_BUFFER && fixture.target->storage == original,
          "truncated packed backing rejects before ownership or pixels change");
  bytes.resize(60);
  VAImage nv12{};
  Require(CreateImage(&fixture.context, &formats[0], 16, 16, &nv12) == VA_STATUS_SUCCESS &&
              GetImage(&fixture.context, 2, 0, 0, 5, 3, nv12.image_id) ==
                  VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT &&
              PutImage(&fixture.context, 2, nv12.image_id, 0, 0, 5, 3, 0, 0, 5, 3) ==
                  VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT && fixture.target->storage == original,
          "image transfer does not silently invent NV12/BGRA conversion");
  VAImage derived{};
  Require(DeriveImage(&fixture.context, 2, &derived) == VA_STATUS_SUCCESS &&
              derived.format.fourcc == VA_FOURCC_BGRA && derived.num_planes == 1 &&
              derived.data_size == 16 * 16 * 4 &&
              MapImageIntent(&fixture, derived.buf, &mapped, 2) == VA_STATUS_SUCCESS &&
              fixture.target->vpp_writers == 1 && fixture.target->image_write_locked,
          "derived BGRA acquires the existing exclusive write lease");
  const uint8_t replacement[] = {7, 23, 211, 129};
  memcpy(mapped, replacement, sizeof(replacement));
  Require(DestroyImage(&fixture.context, derived.image_id) == VA_STATUS_ERROR_SURFACE_BUSY &&
              UnmapBuffer(&fixture.context, derived.buf) == VA_STATUS_SUCCESS &&
              memcmp(fixture.target->planes[0], replacement, 4) == 0 &&
              fixture.target->vpp_writers == 0 && !fixture.target->image_write_locked &&
              fixture.target->ready && !fixture.target->failed,
          "derived write commits all four channels and retires ownership");
  Require(DestroyImage(&fixture.context, derived.image_id) == VA_STATUS_SUCCESS &&
              DestroyImage(&fixture.context, nv12.image_id) == VA_STATUS_SUCCESS &&
              DestroyImage(&fixture.context, image.image_id) == VA_STATUS_SUCCESS,
          "all packed image buffers retire normally");
  auto legacy = PrivateArgbOutput();
  legacy->id = 3;
  fixture.driver.surfaces[3] = legacy;
  VAImage untouched{};
  Require(DeriveImage(&fixture.context, 3, &untouched) == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT,
          "legacy ARGB image admission is not silently broadened");
}

void FillMappedNv12(const VAImage &image, void *data, uint8_t luma) {
  auto *bytes = static_cast<uint8_t *>(data);
  for (unsigned row = 0; row < image.height; ++row)
    memset(bytes + image.offsets[0] + row * image.pitches[0], luma, image.width);
  for (unsigned row = 0; row < (image.height + 1U) / 2; ++row)
    for (unsigned column = 0; column < Align(image.width, 2); ++column)
      bytes[image.offsets[1] + row * image.pitches[1] + column] =
          static_cast<uint8_t>(luma + 1 + (column & 1));
}

void RequireNv12Pixels(const Surface &surface, uint8_t luma) {
  for (unsigned row = 0; row < surface.height; ++row)
    for (unsigned column = 0; column < surface.width; ++column)
      Require(surface.planes[0][row * surface.pitch[0] + column] == luma,
              "derived upload preserves every luma pixel");
  for (unsigned row = 0; row < (surface.height + 1U) / 2; ++row)
    for (unsigned column = 0; column < Align(surface.width, 2); ++column)
      Require(surface.planes[1][row * surface.pitch[1] + column] ==
                  luma + 1 + (column & 1), "derived upload preserves both chroma components");
}

void DerivedWritesCommitActualSurfacePixels() {
  for (uint32_t flags : {2U, 0U, UINT32_MAX}) {
    DerivedFixture fixture;
    VAImage image = {}, readback = MakeImage(&fixture);
    void *data = nullptr;
    Require(DeriveImage(&fixture.context, 2, &image) == VA_STATUS_SUCCESS &&
                MapImageIntent(&fixture, image.buf, &data, flags) == VA_STATUS_SUCCESS,
            "cold NV12 derived image accepts explicit/default/legacy CPU writes");
    FillMappedNv12(image, data, 61);
    Require(!fixture.target->ready && fixture.target->vpp_writers == 1 &&
                UnmapBuffer(&fixture.context, image.buf) == VA_STATUS_SUCCESS &&
                fixture.target->ready && !fixture.target->failed,
            "final unmap commits rather than merely editing an independent snapshot");
    RequireNv12Pixels(*fixture.target, 61);
    Require(GetImage(&fixture.context, 2, 0, 0, 16, 16, readback.image_id) == VA_STATUS_SUCCESS &&
                fixture.driver.buffers.at(readback.buf).data ==
                    fixture.driver.buffers.at(image.buf).data,
            "public image readback sees the uploaded surface pixels");
    auto output = Picture(0, 0); output->id = 3; fixture.driver.surfaces[3] = output;
    VAProcPipelineParameterBuffer pipeline = {}; pipeline.surface = 2;
    VABufferID parameters = VA_INVALID_ID;
    Require(CreateBuffer(&fixture.context, 2, VAProcPipelineParameterBufferType,
                          sizeof(pipeline), 1, &pipeline, &parameters) == VA_STATUS_SUCCESS &&
                BeginPicture(&fixture.context, 2, 3) == VA_STATUS_SUCCESS &&
                RenderPicture(&fixture.context, 2, &parameters, 1) == VA_STATUS_SUCCESS &&
                EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
            "ordinary VPP consumes the uploaded source without a separate PutImage");
    RequireNv12Pixels(*output, 61);
  }
  {
    DerivedFixture fixture; VAImage image = {}; void *data = nullptr;
    const auto original = fixture.target->storage;
    Require(DeriveImage(&fixture.context, 2, &image) == VA_STATUS_SUCCESS &&
                MapImageIntent(&fixture, image.buf, &data, 1) == VA_STATUS_SUCCESS,
            "explicit READ mapping remains an independent image view");
    FillMappedNv12(image, data, 77);
    Require(MapImageIntent(&fixture, image.buf, &data, 2) == VA_STATUS_ERROR_SURFACE_BUSY &&
                UnmapBuffer(&fixture.context, image.buf) == VA_STATUS_SUCCESS &&
                fixture.target->storage == original && fixture.target->vpp_writers == 0,
            "READ cannot be promoted while mapped or silently write back changed snapshot bytes");
  }
  for (bool read_failure : {true, false}) {
    FenceIoMock mock; MockFenceScope scope(mock); DerivedFixture fixture;
    VAImage image = {}; void *data = nullptr;
    Require(DeriveImage(&fixture.context, 2, &image) == VA_STATUS_SUCCESS, "derive before sync fault");
    fixture.target->object_fds = {101, 102, 103};
    if (read_failure) {
      mock.fail_read_end_fd = 102;
      Require(MapImageIntent(&fixture, image.buf, &data, 0) == VA_STATUS_ERROR_OPERATION_FAILED &&
                  data == nullptr && fixture.driver.buffers.at(image.buf).map_count == 0,
              "failed DEFAULT refresh exposes no writable pointer or completed map");
    } else {
      Require(MapImageIntent(&fixture, image.buf, &data, 2) == VA_STATUS_SUCCESS, "map before write fault");
      FillMappedNv12(image, data, 83); mock.fail_end_fd = 102;
      Require(UnmapBuffer(&fixture.context, image.buf) == VA_STATUS_ERROR_OPERATION_FAILED &&
                  fixture.target->failed && !fixture.target->ready,
              "failed write cache completion never acknowledges a ready surface");
    }
    Require(fixture.target->vpp_writers == 0 && !fixture.target->image_write_locked &&
                mock.started.empty() && mock.read_started.empty() && !mock.invalid,
            "derived CPU failures release every reader and writer lease");
  }
}

void DerivedWriteLeaseBlocksReuseAndSurvivesDestruction() {
  DerivedFixture fixture; VAImage image = {}, another = MakeImage(&fixture); void *data = nullptr, *nested = nullptr;
  // Only direct image calls participate in this ownership-state group.
  fixture.driver.StopWorkers();
  fixture.driver.stopping = false;
  Require(DeriveImage(&fixture.context, 2, &image) == VA_STATUS_SUCCESS &&
              MapImageIntent(&fixture, image.buf, &data, 2) == VA_STATUS_SUCCESS &&
              MapImageIntent(&fixture, image.buf, &nested, 2) == VA_STATUS_SUCCESS && nested == data,
          "nested writable maps share one source reservation");
  FillMappedNv12(image, data, 64);
  Require(UnmapBuffer(&fixture.context, image.buf) == VA_STATUS_SUCCESS &&
              fixture.target->vpp_writers == 1 && !fixture.target->ready &&
              fixture.target->planes[0][0] == 99,
          "non-final unmap cannot release ownership or publish partial pixels");
  VAImage busy = {}; VADRMPRIMESurfaceDescriptor descriptor = {};
  Require(BeginPicture(&fixture.context, 2, 2) == VA_STATUS_ERROR_HW_BUSY &&
              PutImage(&fixture.context, 2, another.image_id, 0, 0, 16, 16, 0, 0, 16, 16) == VA_STATUS_ERROR_SURFACE_BUSY &&
              DeriveImage(&fixture.context, 2, &busy) == VA_STATUS_ERROR_SURFACE_BUSY &&
              SyncSurface2(&fixture.context, 2, 0) == VA_STATUS_ERROR_SURFACE_BUSY &&
              DestroyImage(&fixture.context, image.image_id) == VA_STATUS_ERROR_SURFACE_BUSY &&
              ExportSurfaceHandle(&fixture.context, 2, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_WRITE_ONLY, &descriptor) == VA_STATUS_ERROR_SURFACE_BUSY,
          "mapped derived writer blocks picture/image/sync/destroy and write-only export reuse");
  Require(UnmapBuffer(&fixture.context, image.buf) == VA_STATUS_SUCCESS &&
              fixture.target->vpp_writers == 0 && !fixture.target->image_write_locked,
          "only final unmap releases the reservation and commits pixels");
  RequireNv12Pixels(*fixture.target, 64);
  DerivedFixture destroyed; VAImage orphan = {}; void *orphan_data = nullptr;
  const auto old_pixels = destroyed.target->storage;
  Require(DeriveImage(&destroyed.context, 2, &orphan) == VA_STATUS_SUCCESS &&
              MapImageIntent(&destroyed, orphan.buf, &orphan_data, 2) == VA_STATUS_SUCCESS,
          "map a derived writer before source destruction");
  FillMappedNv12(orphan, orphan_data, 92); VASurfaceID id = 2;
  Require(DestroySurfaces(&destroyed.context, &id, 1) == VA_STATUS_SUCCESS,
          "source destruction safely retires a mapped derived writer");
  auto recycled = Picture(0, 17); recycled->id = id; destroyed.driver.surfaces[id] = recycled;
  const auto recycled_pixels = recycled->storage;
  Require(UnmapBuffer(&destroyed.context, orphan.buf) == VA_STATUS_ERROR_INVALID_SURFACE &&
              destroyed.target->storage == old_pixels && recycled->storage == recycled_pixels &&
              destroyed.target->vpp_writers == 0 && !destroyed.target->image_write_locked &&
              destroyed.driver.buffers.at(orphan.buf).data_failed &&
              DestroyImage(&destroyed.context, orphan.image_id) == VA_STATUS_SUCCESS,
          "orphan final unmap fails without writing old/recycled pixels or leaking counters");
  DerivedFixture captured; VAImage upload = {}; void *upload_data = nullptr;
  auto output = Picture(0, 7); output->id = 3; captured.driver.surfaces[3] = output;
  VAProcPipelineParameterBuffer pipeline = {}; pipeline.surface = 2;
  VABufferID parameters = VA_INVALID_ID;
  Require(CreateBuffer(&captured.context, 2, VAProcPipelineParameterBufferType,
                        sizeof(pipeline), 1, &pipeline, &parameters) == VA_STATUS_SUCCESS &&
              BeginPicture(&captured.context, 2, 3) == VA_STATUS_SUCCESS &&
              RenderPicture(&captured.context, 2, &parameters, 1) == VA_STATUS_SUCCESS &&
              DeriveImage(&captured.context, 2, &upload) == VA_STATUS_SUCCESS &&
              MapImageIntent(&captured, upload.buf, &upload_data, 2) == VA_STATUS_SUCCESS,
          "reserve a derived writer after VPP captures the same cold source");
  FillMappedNv12(upload, upload_data, 71);
  Require(EndPicture(&captured.context, 2) == VA_STATUS_ERROR_HW_BUSY &&
              captured.target->vpp_readers == 0 && captured.driver.pending_vpp.empty() &&
              captured.target->vpp_writers == 1 && captured.target->image_write_locked &&
              UnmapBuffer(&captured.context, upload.buf) == VA_STATUS_SUCCESS &&
              captured.target->ready && captured.target->vpp_writers == 0 &&
              !captured.target->image_write_locked,
          "stale VPP cannot acquire a reader that blocks the derived writer's final commit");
  RequireNv12Pixels(*captured.target, 71);
}

void DerivedDefaultRefreshAndDecodedSnapshotsStayIndependent() {
  DerivedFixture fixture; VAImage derived = {}, update = MakeImage(&fixture); void *data = nullptr;
  Require(DeriveImage(&fixture.context, 2, &derived) == VA_STATUS_SUCCESS, "derive a cold image before upload");
  FillMappedNv12(update, fixture.driver.buffers.at(update.buf).data.data(), 25);
  Require(PutImage(&fixture.context, 2, update.image_id, 0, 0, 16, 16, 0, 0, 16, 16) == VA_STATUS_SUCCESS &&
              MapImageIntent(&fixture, derived.buf, &data, 0) == VA_STATUS_SUCCESS &&
              static_cast<uint8_t *>(data)[derived.offsets[0]] == 25 &&
              static_cast<uint8_t *>(data)[derived.offsets[1]] == 26,
          "DEFAULT refreshes stale derived bytes after an intervening PutImage");
  static_cast<uint8_t *>(data)[derived.offsets[0]] = 66;
  Require(UnmapBuffer(&fixture.context, derived.buf) == VA_STATUS_SUCCESS &&
              fixture.target->planes[0][0] == 66 && fixture.target->planes[0][1] == 25 &&
              fixture.target->planes[1][0] == 26 && fixture.target->planes[1][1] == 27,
          "partial DEFAULT edits preserve refreshed luma/chroma rather than stale snapshot bytes");
  VAImage decoded = {}; const auto original = fixture.source->storage;
  const auto private_original = fixture.decoder->decoded_frames.at(kTimestampStep)->storage;
  Require(DeriveImage(&fixture.context, 1, &decoded) == VA_STATUS_SUCCESS &&
              MapImageIntent(&fixture, decoded.buf, &data, 2) == VA_STATUS_ERROR_OPERATION_FAILED,
          "explicit WRITE cannot alter a retained decoded-picture snapshot");
  for (uint32_t flags : {1U, UINT32_MAX}) {
    Require(MapImageIntent(&fixture, decoded.buf, &data, flags) == VA_STATUS_SUCCESS,
            "decoded READ and legacy snapshot maps remain available");
    FillMappedNv12(decoded, data, 98);
    Require(UnmapBuffer(&fixture.context, decoded.buf) == VA_STATUS_SUCCESS &&
                fixture.source->storage == original &&
                fixture.decoder->decoded_frames.at(kTimestampStep)->storage == private_original,
            "decoded public pixels and immutable retained picture remain independent of snapshot edits");
  }
}

void ImageReadFailuresAndDerivedRollback() {
  for (bool derive : {false, true}) {
    for (bool fail_start : {false, true}) {
      FenceIoMock mock;
      MockFenceScope scope(mock);
      Fixture fixture;
      fixture.source->object_fds = {101, 102, 103};
      mock.fail_read_start_fd = fail_start ? 102 : -1;
      mock.fail_read_end_fd = fail_start ? -1 : 102;
      VAImage image = derive ? VAImage{} : MakeImage(&fixture);
      const size_t images = fixture.driver.images.size();
      const size_t buffers = fixture.driver.buffers.size();
      const VAStatus status = derive
          ? DeriveImage(&fixture.context, 1, &image)
          : GetImage(&fixture.context, 1, 0, 0, 16, 16, image.image_id);
      Require(status == VA_STATUS_ERROR_OPERATION_FAILED,
              "GetImage/DeriveImage must report READ START/END errors");
      Require(fixture.driver.images.size() == images &&
                  fixture.driver.buffers.size() == buffers,
              "failed DeriveImage must roll back its image and buffer");
      Require(mock.read_started.empty() && !mock.invalid,
              "failed image read must release acquired READ objects");
      if (!derive && fail_start) {
        const auto &bytes = fixture.driver.buffers.at(image.buf).data;
        Require(std::all_of(bytes.begin(), bytes.end(),
                            [](uint8_t byte) { return byte == 0; }),
                "failed image READ START cannot expose source pixels");
      }
    }
  }
}

void ImageGeometryAndOddNv12Roundtrip() {
  for (unsigned int width : {1U, 2U, 3U, 16U}) {
    for (unsigned int height : {1U, 2U, 3U, 16U}) {
      Fixture fixture;
      const VAImage image = MakeImage(&fixture, width, height);
      const unsigned int uv_width = (width + 1) & ~1U;
      Require(image.pitches[0] >= width && image.pitches[1] >= uv_width &&
                  image.data_size >= image.offsets[1] +
                      image.pitches[1] * ((height + 1) / 2),
              "odd NV12 images need complete UV pairs and ceil chroma rows");
      for (unsigned int row = 0; row < (height + 1) / 2; ++row)
        for (unsigned int column = 0; column < uv_width; ++column)
          fixture.source->planes[1][row * fixture.source->pitch[1] + column] =
              static_cast<uint8_t>(70 + column);
      Require(GetImage(&fixture.context, 1, 0, 0, width, height, image.image_id) ==
                  VA_STATUS_SUCCESS &&
                  PutImage(&fixture.context, 2, image.image_id, 0, 0, width, height,
                           0, 0, width, height) == VA_STATUS_SUCCESS,
              "odd NV12 image roundtrip must succeed");
      for (unsigned int row = 0; row < (height + 1) / 2; ++row)
        for (unsigned int column = 0; column < uv_width; ++column)
          Require(fixture.target->planes[1][row * fixture.target->pitch[1] + column] ==
                      static_cast<uint8_t>(70 + column),
                  "image copy must preserve the last full UV pair");
    }
  }
  Fixture fixture;
  VAImageFormat format = {};
  format.fourcc = VA_FOURCC_NV12;
  VAImage image = {};
  for (const auto &size : {std::pair<int, int>{0, 16}, {-1, 16},
                          {INT_MAX, 16}, {16, INT_MAX}})
    Require(CreateImage(&fixture.context, &format, size.first, size.second, &image) !=
                VA_STATUS_SUCCESS,
            "invalid or overflow image dimensions must fail before allocation");
}

void ImageCopiesRejectInvalidAndBusyRectangles() {
  Fixture fixture;
  VAImage image = MakeImage(&fixture);
  // Extra private capacity keeps the pre-fix oversized-width reproduction
  // memory-safe: only the declared 16x16 image/surface rectangle is invalid.
  fixture.driver.buffers.at(image.buf).data.resize(2048);
  for (unsigned int width : {0U, 17U}) {
    const auto before = fixture.target->storage;
    Require(PutImage(&fixture.context, 2, image.image_id, 0, 0, width, 16,
                     0, 0, width, 16) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                fixture.target->storage == before,
            "PutImage must reject empty/oversized rectangles before writing");
    Require(GetImage(&fixture.context, 1, 0, 0, width, 16, image.image_id) ==
                VA_STATUS_ERROR_INVALID_PARAMETER,
            "GetImage must reject empty/oversized rectangles instead of clipping");
  }
  fixture.target->vpp_writers = 1;
  fixture.target->ready = false;
  const auto before = fixture.target->storage;
  Require(PutImage(&fixture.context, 2, image.image_id, 0, 0, 16, 16,
                   0, 0, 16, 16) == VA_STATUS_ERROR_SURFACE_BUSY &&
              fixture.target->storage == before,
          "PutImage must not wait on or overwrite a queued VPP target");
  Require(GetImage(&fixture.context, 2, 0, 0, 16, 16, image.image_id) ==
              VA_STATUS_ERROR_SURFACE_BUSY,
          "GetImage must not read a queued VPP target");
  fixture.target->vpp_writers = 0;
  fixture.target->ready = true;
  fixture.driver.buffers.at(image.buf).data.resize(1);
  Require(PutImage(&fixture.context, 2, image.image_id, 0, 0, 16, 16,
                   0, 0, 16, 16) == VA_STATUS_ERROR_INVALID_BUFFER &&
              GetImage(&fixture.context, 1, 0, 0, 16, 16, image.image_id) ==
                  VA_STATUS_ERROR_INVALID_BUFFER,
          "resized image storage must be checked before CPU access");
  Require(DestroyBuffer(&fixture.context, image.buf) == VA_STATUS_SUCCESS &&
              PutImage(&fixture.context, 2, image.image_id, 0, 0, 16, 16,
                       0, 0, 16, 16) == VA_STATUS_ERROR_INVALID_BUFFER &&
              GetImage(&fixture.context, 1, 0, 0, 16, 16, image.image_id) ==
                  VA_STATUS_ERROR_INVALID_BUFFER,
          "destroyed image storage cannot be accessed through a live image ID");
}

void PutImageCannotOverwriteRetainedDecodeIdentity() {
  Fixture fixture;
  const VAImage image = MakeImage(&fixture);
  const auto before = fixture.source->storage;
  auto alias = Picture(kTimestampStep, 99);
  alias->backing_owner = 1;
  fixture.driver.surfaces[3] = alias;
  for (VASurfaceID id : {1U, 3U})
    Require(PutImage(&fixture.context, id, image.image_id, 0, 0, 16, 16,
                     0, 0, 16, 16) == VA_STATUS_ERROR_SURFACE_BUSY,
            "retained decode pictures and aliases cannot be overwritten by PutImage");
  Require(fixture.source->storage == before &&
              fixture.decoder->surface_timestamps.at(fixture.source.get()) ==
                  kTimestampStep &&
              fixture.decoder->decoded_frames.at(kTimestampStep)->planes[0][0] == 40,
          "rejected upload must preserve public/private decoded-frame identity");
  // A decoder may also have submitted an alias rather than its canonical
  // owner. Uploading through the owner must still find that retained picture.
  fixture.decoder->surface_timestamps.erase(fixture.source.get());
  fixture.decoder->surface_timestamps[alias.get()] = kTimestampStep;
  Require(PutImage(&fixture.context, 1, image.image_id, 0, 0, 16, 16,
                   0, 0, 16, 16) == VA_STATUS_ERROR_SURFACE_BUSY,
          "PutImage must find decode identity stored under another live alias");
}

void OrphanedAliasesCannotWaitOnPendingFence() {
  FenceIoMock mock;
  MockFenceScope scope(mock);
  Fixture fixture;
  // Keep a real queued production VPP operation deterministic without a
  // background thread accessing mocked descriptors outside this test thread.
  {
    std::lock_guard<std::mutex> lock(fixture.driver.mutex);
    fixture.driver.stopping = true;
  }
  fixture.driver.condition.notify_all();
  fixture.driver.vpp_worker.join();
  auto writer = Picture(0, 99);
  auto reader = Picture(0, 99);
  for (const auto &alias : {writer, reader}) {
    alias->backing_owner = 2;
    alias->object_fds = {101, 102, 103};
  }
  fixture.driver.surfaces[3] = writer;
  fixture.driver.surfaces[4] = reader;
  fixture.decoder->decoded_frames.at(kTimestampStep)->ready = false;
  Require(BeginPicture(&fixture.context, 2, 3) == VA_STATUS_SUCCESS &&
              RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                  VA_STATUS_SUCCESS &&
              EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS &&
              fixture.driver.pending_vpp.size() == 1 && !mock.signaled,
          "queue a production fenced VPP targeting one alias");
  VASurfaceID owner = 2;
  Require(DestroySurfaces(&fixture.context, &owner, 1) == VA_STATUS_SUCCESS &&
              fixture.driver.surfaces.count(2) == 0 &&
              fixture.driver.pending_vpp.front().target_owner->destroyed,
          "destroy original owner while queued VPP retains its fence");
  const VAImage image = MakeImage(&fixture);
  const auto events = mock.events;
  const size_t images = fixture.driver.images.size();
  const size_t buffers = fixture.driver.buffers.size();
  VAImage derived = {};
  Require(GetImage(&fixture.context, 4, 0, 0, 16, 16, image.image_id) ==
              VA_STATUS_ERROR_INVALID_SURFACE &&
              PutImage(&fixture.context, 4, image.image_id, 0, 0, 16, 16,
                       0, 0, 16, 16) == VA_STATUS_ERROR_INVALID_SURFACE &&
              DeriveImage(&fixture.context, 4, &derived) ==
                  VA_STATUS_ERROR_INVALID_SURFACE,
          "orphaned alias image access must fail before waiting on our fence");
  Require(mock.events == events && !mock.self_wait &&
              fixture.driver.images.size() == images &&
              fixture.driver.buffers.size() == buffers,
          "orphaned aliases cannot issue CPU sync ioctls or leak snapshots");
  VASurfaceStatus surface_status = VASurfaceReady;
  Require(QuerySurfaceStatus(&fixture.context, 4, &surface_status) ==
              VA_STATUS_ERROR_INVALID_SURFACE &&
              SyncSurface2(&fixture.context, 4, 0) == VA_STATUS_ERROR_INVALID_SURFACE &&
              BeginPicture(&fixture.context, 2, 4) == VA_STATUS_ERROR_INVALID_SURFACE &&
              mock.events == events,
          "orphaned aliases cannot report ready or become a new VPP target");
  VADRMPRIMESurfaceDescriptor exported = {};
  Require(ExportSurfaceHandle(&fixture.context, 4,
                              VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                              VA_EXPORT_SURFACE_READ_ONLY, &exported) ==
              VA_STATUS_ERROR_INVALID_SURFACE,
          "exporting an orphan cannot restore a stale backing owner mapping");
}

void VppRechecksOwnersBetweenPublicCalls() {
  for (bool remove_source : {true, false}) {
    for (bool after_render : {true, false}) {
      Fixture fixture;
      auto source_alias = Picture(kTimestampStep, 40);
      auto target_alias = Picture(0, 99);
      source_alias->backing_owner = 1;
      target_alias->backing_owner = 2;
      fixture.driver.surfaces[3] = source_alias;
      fixture.driver.surfaces[4] = target_alias;
      VAProcPipelineParameterBuffer pipeline = {};
      pipeline.surface = 3;
      VABufferID parameters;
      Require(CreateBuffer(&fixture.context, 2, VAProcPipelineParameterBufferType,
                           sizeof(pipeline), 1, &pipeline, &parameters) ==
                  VA_STATUS_SUCCESS &&
                  BeginPicture(&fixture.context, 2, 4) == VA_STATUS_SUCCESS,
              "begin VPP through live source and target aliases");
      if (after_render)
        Require(RenderPicture(&fixture.context, 2, &parameters, 1) == VA_STATUS_SUCCESS,
                "capture a VPP picture before original owner destruction");
      VASurfaceID owner = remove_source ? 1 : 2;
      Require(DestroySurfaces(&fixture.context, &owner, 1) == VA_STATUS_SUCCESS,
              "destroy source or target owner between public VPP calls");
      const auto before = target_alias->storage;
      const VAStatus result = after_render
          ? EndPicture(&fixture.context, 2)
          : RenderPicture(&fixture.context, 2, &parameters, 1);
      Require(result == VA_STATUS_ERROR_INVALID_SURFACE &&
                  target_alias->storage == before,
              "Render/EndPicture must recheck both original alias owners");
    }
  }
}

void PendingBackingReimportIsRejectedBeforeMapping() {
  for (bool legacy : {false, true}) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    Fixture fixture;
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      fixture.driver.stopping = true;
    }
    fixture.driver.condition.notify_all();
    fixture.driver.vpp_worker.join();
    const int fd = memfd_create("crystalhd-private-pending-import", MFD_CLOEXEC);
    Require(fd >= 0, "allocate private backing identity for import test");
    mock.real_files.insert(fd);
    Require(ftruncate(fd, fixture.target->storage.size()) == 0,
            "size private imported memory");
    fixture.target->object_fds = {fd};
    fixture.target->object_sizes = {fixture.target->storage.size()};
    VADRMPRIMESurfaceDescriptor prime = {};
    Require(ExportSurfaceHandle(&fixture.context, 2,
                                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                VA_EXPORT_SURFACE_READ_ONLY |
                                    VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                &prime) == VA_STATUS_SUCCESS,
            "export private backing before original surface destruction");
    mock.real_files.insert(prime.objects[0].fd);
    fixture.decoder->decoded_frames.at(kTimestampStep)->ready = false;
    fixture.SubmitParameters();
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
            "queue fenced VPP on exported backing");
    VASurfaceID owner = 2;
    Require(DestroySurfaces(&fixture.context, &owner, 1) == VA_STATUS_SUCCESS,
            "retire original owner with pending fence");
    uintptr_t handle = prime.objects[0].fd;
    VASurfaceAttribExternalBuffers external = {};
    external.pixel_format = VA_FOURCC_NV12;
    external.width = 16;
    external.height = 16;
    external.data_size = prime.objects[0].size;
    external.num_planes = 2;
    external.num_buffers = 1;
    external.buffers = &handle;
    for (unsigned int plane = 0; plane < 2; ++plane) {
      external.pitches[plane] = prime.layers[0].pitch[plane];
      external.offsets[plane] = prime.layers[0].offset[plane];
    }
    VASurfaceAttrib attributes[2] = {};
    attributes[0].type = VASurfaceAttribMemoryType;
    attributes[0].value.type = VAGenericValueTypeInteger;
    attributes[0].value.value.i = legacy ? VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME
                                       : VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
    attributes[1].value.type = VAGenericValueTypePointer;
    attributes[1].value.value.p = legacy ? static_cast<void *>(&external)
                                       : static_cast<void *>(&prime);
    // With no GBM device, this non-linear PRIME import cannot be mapped at
    // all. BUSY must win before ImportPrime can attempt that mapping path.
    if (!legacy)
      prime.objects[0].drm_format_modifier = I915_FORMAT_MOD_X_TILED;
    VASurfaceID imported = VA_INVALID_SURFACE;
    const size_t count = fixture.driver.surfaces.size();
    const VAStatus result = CreateSurfaces2(
        &fixture.context, VA_RT_FORMAT_YUV420, 16, 16, &imported, 1, attributes, 2);
    // Ensure even a pre-fix successful import's duplicate is cleaned up by
    // the mock, so the failing regression itself never leaks test fds.
    if (fixture.driver.surfaces.count(imported) != 0)
      for (int object : fixture.driver.surfaces.at(imported)->object_fds)
        mock.real_files.insert(object);
    Require(result == VA_STATUS_ERROR_SURFACE_BUSY &&
                imported == VA_INVALID_SURFACE && fixture.driver.surfaces.size() == count &&
                !mock.signaled,
            "pending exported backing must be rejected before PRIME/legacy import");
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      ReleasePendingVpp(&fixture.driver, fixture.driver.pending_vpp.front().sequence,
                        VA_STATUS_ERROR_OPERATION_FAILED, false);
    }
    prime.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    Require(CreateSurfaces2(&fixture.context, VA_RT_FORMAT_YUV420, 16, 16,
                            &imported, 1, attributes, 2) == VA_STATUS_SUCCESS,
            "released pending fence must not permanently block future imports");
    for (int object : fixture.driver.surfaces.at(imported)->object_fds)
      mock.real_files.insert(object);
    Require(!mock.self_wait && !mock.invalid, "import lifecycle keeps ownership balanced");
    close(prime.objects[0].fd);
  }
}

void FirstExternalImportOwnsAliasesAndRollsBackAtomically() {
  for (bool legacy : {false, true}) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    Fixture fixture;
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      fixture.driver.stopping = true;
    }
    fixture.driver.condition.notify_all();
    fixture.driver.vpp_worker.join();
    Surface backing_file;
    const int fd = memfd_create("crystalhd-private-first-import", MFD_CLOEXEC);
    Require(fd >= 0, "allocate private external import backing");
    backing_file.object_fds = {fd};
    mock.real_files.insert(fd);
    Require(ftruncate(fd, 512) == 0, "size private 16x16 NV12 import");
    VADRMPRIMESurfaceDescriptor prime[2] = {};
    VASurfaceAttribExternalBuffers external = {};
    uintptr_t handles[2] = {static_cast<uintptr_t>(fd), static_cast<uintptr_t>(fd)};
    for (unsigned int i = 0; i < 2; ++i) {
      prime[i].fourcc = VA_FOURCC_NV12;
      prime[i].width = 16;
      prime[i].height = 16;
      prime[i].num_objects = 1;
      prime[i].objects[0].fd = fd;
      prime[i].objects[0].size = 512;
      prime[i].objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
      prime[i].num_layers = 1;
      prime[i].layers[0].drm_format = DRM_FORMAT_NV12;
      prime[i].layers[0].num_planes = 2;
      prime[i].layers[0].pitch[0] = 16;
      prime[i].layers[0].pitch[1] = 16;
      prime[i].layers[0].offset[1] = 256;
    }
    external.pixel_format = VA_FOURCC_NV12;
    external.width = external.height = 16;
    external.data_size = 512;
    external.num_planes = 2;
    external.pitches[0] = external.pitches[1] = 16;
    external.offsets[1] = 256;
    external.num_buffers = 2;
    external.buffers = handles;
    VASurfaceAttrib attributes[2] = {};
    attributes[0].type = VASurfaceAttribMemoryType;
    attributes[0].value.type = VAGenericValueTypeInteger;
    attributes[0].value.value.i = legacy ? VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME
                                       : VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
    attributes[1].value.type = VAGenericValueTypePointer;
    attributes[1].value.value.p = legacy ? static_cast<void *>(&external)
                                       : static_cast<void *>(prime);
    VASurfaceID surfaces[2] = {VA_INVALID_SURFACE, VA_INVALID_SURFACE};
    auto create = [&] {
      // Import uses ordinary private memfd dup/mmap/close, not mocked DMA
      // ownership. Let rollback close duplicates we never receive publicly.
      fence_io_mock = nullptr;
      const VAStatus status = CreateSurfaces2(&fixture.context, VA_RT_FORMAT_YUV420,
                                              16, 16, surfaces, 2, attributes, 2);
      fence_io_mock = &mock;
      if (status == VA_STATUS_SUCCESS)
        for (VASurfaceID surface : surfaces)
          for (int object : fixture.driver.surfaces.at(surface)->object_fds)
            mock.real_files.insert(object);
      return status;
    };
    // PRIME2 has one descriptor per surface. Legacy PRIME instead has ONE
    // descriptor with a handles array; make only its second handle unmappable.
    Surface invalid_mapping;
    invalid_mapping.object_fds = {eventfd(0, EFD_CLOEXEC)};
    Require(invalid_mapping.object_fds[0] >= 0, "allocate unmappable batch handle");
    prime[1].objects[0].size = 0;
    handles[1] = invalid_mapping.object_fds[0];
    Require(create() == VA_STATUS_ERROR_ALLOCATION_FAILED &&
                fixture.driver.surfaces.size() == 2 &&
                fixture.driver.backing_owners.empty(),
            "failed import batch must roll back surfaces and new backing identities");
    prime[1].objects[0].size = 512;
    handles[1] = fd;
    Require(create() == VA_STATUS_SUCCESS,
            "import the same external backing twice without exporting it first");
    const auto first = fixture.driver.surfaces.at(surfaces[0]);
    const auto alias = fixture.driver.surfaces.at(surfaces[1]);
    Require(first->backing_owner == VA_INVALID_SURFACE &&
                alias->backing_owner == surfaces[0],
            "the first external import must own every later same-backing alias");
    Require(BeginPicture(&fixture.context, 1, surfaces[0]) == VA_STATUS_SUCCESS,
            "canonical external imports remain accepted as decode targets");
    prime[0].layers[0].offset[0] = external.offsets[0] = 16;
    prime[0].layers[0].offset[1] = external.offsets[1] = 272;
    external.num_buffers = 1;
    VASurfaceID incompatible = VA_INVALID_SURFACE;
    const size_t count = fixture.driver.surfaces.size();
    Require(CreateSurfaces2(&fixture.context, VA_RT_FORMAT_YUV420, 16, 16,
                            &incompatible, 1, attributes, 2) ==
                VA_STATUS_ERROR_INVALID_PARAMETER &&
                incompatible == VA_INVALID_SURFACE &&
                fixture.driver.surfaces.size() == count,
            "same-fd nonidentical valid plane views must not share picture identity");
    prime[0].layers[0].offset[0] = external.offsets[0] = 0;
    prime[0].layers[0].offset[1] = external.offsets[1] = 256;
    fixture.decoder->decoded_frames.at(kTimestampStep)->ready = false;
    Require(BeginPicture(&fixture.context, 2, surfaces[0]) == VA_STATUS_SUCCESS &&
                RenderPicture(&fixture.context, 2, &fixture.parameters, 1) ==
                    VA_STATUS_SUCCESS && EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
            "queue a fenced write to the first external import");
    VASurfaceStatus state = VASurfaceReady;
    const VAImage image = MakeImage(&fixture);
    const auto events = mock.events;
    Require(QuerySurfaceStatus(&fixture.context, surfaces[1], &state) == VA_STATUS_SUCCESS &&
                state == VASurfaceRendering &&
                SyncSurface2(&fixture.context, surfaces[1], 0) != VA_STATUS_SUCCESS &&
                GetImage(&fixture.context, surfaces[1], 0, 0, 16, 16, image.image_id) ==
                    VA_STATUS_ERROR_SURFACE_BUSY &&
                PutImage(&fixture.context, surfaces[1], image.image_id, 0, 0, 16, 16,
                         0, 0, 16, 16) == VA_STATUS_ERROR_SURFACE_BUSY &&
                BeginPicture(&fixture.context, 2, surfaces[1]) == VA_STATUS_ERROR_HW_BUSY &&
                mock.events == events,
            "pre-existing aliases must share pending status and reject CPU/VPP access");
  }
}

void DifferentLumaObjectsCannotAliasSharedChroma() {
  FenceIoMock mock;
  MockFenceScope scope(mock);
  Fixture fixture;
  Surface files;
  for (unsigned int object = 0; object < 3; ++object) {
    const int fd = memfd_create("crystalhd-private-multiplane", MFD_CLOEXEC);
    Require(fd >= 0, "allocate private multiplane import backing");
    files.object_fds.push_back(fd);
    mock.real_files.insert(fd);
    Require(ftruncate(fd, 512) == 0, "size private multiplane backing");
  }
  VADRMPRIMESurfaceDescriptor prime = {};
  prime.fourcc = VA_FOURCC_NV12;
  prime.width = 16;
  prime.height = 16;
  prime.num_objects = 2;
  prime.objects[0].fd = files.object_fds[0];
  prime.objects[1].fd = files.object_fds[2];
  for (unsigned int object = 0; object < 2; ++object) {
    prime.objects[object].size = 512;
    prime.objects[object].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
  }
  prime.num_layers = 1;
  prime.layers[0].drm_format = DRM_FORMAT_NV12;
  prime.layers[0].num_planes = 2;
  prime.layers[0].object_index[1] = 1;
  prime.layers[0].pitch[0] = 16;
  prime.layers[0].pitch[1] = 16;
  VASurfaceAttrib attributes[2] = {};
  attributes[0].type = VASurfaceAttribMemoryType;
  attributes[0].value.type = VAGenericValueTypeInteger;
  attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
  attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
  attributes[1].value.type = VAGenericValueTypePointer;
  attributes[1].value.value.p = &prime;
  auto create = [&](VASurfaceID *id) {
    const VAStatus status = CreateSurfaces2(&fixture.context, VA_RT_FORMAT_YUV420,
                                            16, 16, id, 1, attributes, 2);
    if (status == VA_STATUS_SUCCESS)
      for (int fd : fixture.driver.surfaces.at(*id)->object_fds)
        mock.real_files.insert(fd);
    return status;
  };
  VASurfaceID first = VA_INVALID_SURFACE;
  Require(create(&first) == VA_STATUS_SUCCESS, "import independent luma and chroma objects");
  BackingIdentity chroma;
  Require(GetBackingIdentity(files.object_fds[2], &chroma) &&
              fixture.driver.backing_owners.count(chroma) == 1 &&
              fixture.driver.backing_owners.at(chroma) == first,
          "every imported plane object needs canonical ownership, not just luma");
  prime.objects[0].fd = files.object_fds[1];
  VASurfaceID second = VA_INVALID_SURFACE;
  Require(create(&second) == VA_STATUS_ERROR_INVALID_PARAMETER &&
              second == VA_INVALID_SURFACE && fixture.driver.surfaces.size() == 3,
          "different luma with shared chroma must reject incompatible overlapping views");
  Require(DestroySurfaces(&fixture.context, &first, 1) == VA_STATUS_SUCCESS &&
              fixture.driver.backing_owners.empty(),
          "canonical teardown must remove every object identity");
}

void ImportedPrimeExportsPreservePlaneLayouts() {
  for (unsigned representation = 0; representation < 3; ++representation) {
    Fixture fixture;
    Surface original_files, exported_files;
    const bool argb = representation == 2;
    VADRMPRIMESurfaceDescriptor imported = {};
    imported.fourcc = argb ? VA_FOURCC_ARGB : VA_FOURCC_NV12;
    imported.width = imported.height = 16;
    imported.num_objects = representation == 0 ? 2 : 1;
    for (unsigned object = 0; object < imported.num_objects; ++object) {
      const int fd = memfd_create("crystalhd-private-export-layout", MFD_CLOEXEC);
      Require(fd >= 0, "allocate private imported plane backing");
      original_files.object_fds.push_back(fd);
      Require(ftruncate(fd, 2048) == 0, "size private imported plane backing");
      std::array<uint8_t, 2048> pixels;
      for (unsigned byte = 0; byte < pixels.size(); ++byte)
        pixels[byte] = static_cast<uint8_t>(11 + object * 67 + byte % 31);
      Require(pwrite(fd, pixels.data(), pixels.size(), 0) ==
                  static_cast<ssize_t>(pixels.size()),
              "initialize distinguishable private plane bytes");
      imported.objects[object].fd = fd;
      imported.objects[object].size = pixels.size();
      imported.objects[object].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    }
    imported.num_layers = 1;
    imported.layers[0].drm_format = argb ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_NV12;
    imported.layers[0].num_planes = argb ? 1 : 2;
    imported.layers[0].offset[0] = 64;
    imported.layers[0].pitch[0] = argb ? 80 : 20;
    if (!argb) {
      imported.layers[0].object_index[1] = representation == 0 ? 1 : 0;
      imported.layers[0].offset[1] = representation == 0 ? 32 : 512;
      imported.layers[0].pitch[1] = 20;
    }
    VASurfaceAttrib attributes[2] = {};
    attributes[0].type = VASurfaceAttribMemoryType;
    attributes[0].value.type = VAGenericValueTypeInteger;
    attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
    attributes[1].value.type = VAGenericValueTypePointer;
    attributes[1].value.value.p = &imported;
    VASurfaceID surface = VA_INVALID_SURFACE;
    Require(CreateSurfaces2(&fixture.context,
                            argb ? VA_RT_FORMAT_RGB32 : VA_RT_FORMAT_YUV420,
                            16, 16, &surface, 1, attributes, 2) == VA_STATUS_SUCCESS,
            "import valid split-object or nonzero-offset surface layout");
    SurfaceLayout expected;
    Require(GetPrimeLayout(imported, &expected), "capture the original imported layout");
    const auto import_duplicates = fixture.driver.surfaces.at(surface)->object_fds;
    for (uint32_t layers : {uint32_t(VA_EXPORT_SURFACE_SEPARATE_LAYERS),
                            uint32_t(VA_EXPORT_SURFACE_COMPOSED_LAYERS)}) {
      VADRMPRIMESurfaceDescriptor exported = {};
      const VAStatus status = ExportSurfaceHandle(&fixture.context, surface,
          VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
          VA_EXPORT_SURFACE_READ_ONLY | layers, &exported);
      for (unsigned object = 0; object < exported.num_objects; ++object)
        if (exported.objects[object].fd >= 0)
          exported_files.object_fds.push_back(exported.objects[object].fd);
      Require(status == VA_STATUS_SUCCESS,
              "valid imported multi-object/offset layouts remain exportable");
      SurfaceLayout actual;
      Require(exported.num_objects == imported.num_objects &&
                  exported.num_layers == (argb || layers == VA_EXPORT_SURFACE_COMPOSED_LAYERS
                                             ? 1U : 2U) &&
                  GetPrimeLayout(exported, &actual) && actual == expected,
              "export preserves every plane object, offset, stride and modifier");
      for (unsigned plane = 0; plane < imported.layers[0].num_planes; ++plane) {
        const unsigned layer = argb || layers == VA_EXPORT_SURFACE_COMPOSED_LAYERS
            ? 0 : plane;
        const unsigned index = layer == 0 ? plane : 0;
        const unsigned object = exported.layers[layer].object_index[index];
        const int original = imported.objects[imported.layers[0].object_index[plane]].fd;
        const int duplicate = exported.objects[object].fd;
        Require(duplicate != original && exported.objects[object].size == 2048 &&
                    fcntl(original, F_GETFD) >= 0,
                "export returns independent descriptors without consuming client handles");
        const unsigned rows = plane == 0 ? 16 : 8;
        const unsigned row_bytes = argb ? 64 : 16;
        for (unsigned row = 0; row < rows; ++row)
          for (unsigned column = 0; column < row_bytes; ++column) {
            uint8_t input = 0, output = 0;
            Require(pread(original, &input, 1,
                          imported.layers[0].offset[plane] +
                              row * imported.layers[0].pitch[plane] + column) == 1 &&
                        pread(duplicate, &output, 1,
                          exported.layers[layer].offset[index] +
                              row * exported.layers[layer].pitch[index] + column) == 1 &&
                        input == output,
                    "re-exported plane bytes retain the imported pixel view");
          }
      }
    }
    Require(DestroySurfaces(&fixture.context, &surface, 1) == VA_STATUS_SUCCESS,
            "destroy the imported VA surface after exporting both layer layouts");
    for (int fd : import_duplicates)
      Require(fcntl(fd, F_GETFD) == -1 && errno == EBADF,
              "surface destruction closes only its own imported duplicates");
    for (int fd : original_files.object_fds)
      Require(fcntl(fd, F_GETFD) >= 0, "client import handles outlive the VA surface");
    for (int fd : exported_files.object_fds)
      Require(fcntl(fd, F_GETFD) >= 0, "export handles remain caller-owned after VA destruction");
    for (unsigned failure = 0; failure < (argb ? 2U : 4U); ++failure) {
      auto invalid = imported;
      const unsigned plane = failure < 2 ? 0 : 1;
      const unsigned object = invalid.layers[0].object_index[plane];
      const unsigned rows = plane == 0 ? 16 : 8;
      const unsigned row_bytes = argb ? 64 : 16;
      if ((failure & 1U) == 0)
        invalid.layers[0].pitch[plane] = UINT32_MAX / (rows - 1) + 1;
      else
        invalid.objects[object].size = invalid.layers[0].offset[plane] +
            (rows - 1) * invalid.layers[0].pitch[plane] + row_bytes - 1;
      attributes[1].value.value.p = &invalid;
      VASurfaceID rejected = VA_INVALID_SURFACE;
      Require(CreateSurfaces2(&fixture.context,
                              argb ? VA_RT_FORMAT_RGB32 : VA_RT_FORMAT_YUV420,
                              16, 16, &rejected, 1, attributes, 2) != VA_STATUS_SUCCESS &&
                  fixture.driver.surfaces.size() == 2 &&
                  fixture.driver.backing_owners.empty(),
              "PRIME plane spans cannot wrap on i686 or exceed the object by one byte");
      // No access to rejected plane pointers: malformed descriptors are
      // rejected structurally, without a decode, DMA ioctl or device.
    }
  }
}

void LegacyPrimeBatchUsesOneDescriptorAndRollsBack() {
  const auto fd_count = [] {
    DIR *directory = opendir("/proc/self/fd");
    Require(directory != nullptr, "inspect private regression descriptor lifetime");
    size_t count = 0;
    while (const dirent *entry = readdir(directory))
      if (entry->d_name[0] != '.')
        ++count;
    closedir(directory);
    return count;
  };
  for (unsigned scenario = 0; scenario < 4; ++scenario) {
    Fixture fixture;
    Surface files;
    std::array<uintptr_t, 2> handles;
    for (unsigned index = 0; index < handles.size(); ++index) {
      const int fd = memfd_create("crystalhd-private-legacy-batch", MFD_CLOEXEC);
      Require(fd >= 0, "allocate caller-owned legacy surface handle");
      files.object_fds.push_back(fd);
      Require(ftruncate(fd, 512) == 0, "size private legacy surface handle");
      const uint8_t first_pixel = static_cast<uint8_t>(40 + index * 30);
      Require(pwrite(fd, &first_pixel, 1, 0) == 1,
              "give each batch handle a distinct first pixel");
      handles[index] = fd;
    }
    if (scenario == 1)
      handles[1] = handles[0];
    if (scenario == 2) {
      const int unmappable = eventfd(0, EFD_CLOEXEC);
      Require(unmappable >= 0, "allocate valid but unmappable second batch handle");
      files.object_fds.push_back(unmappable);
      handles[1] = unmappable;
    }
    // Unlike PRIME2's descriptor array, legacy PRIME shares one geometry
    // descriptor and supplies one buffers[] entry per requested surface.
    VASurfaceAttribExternalBuffers external = {};
    external.pixel_format = VA_FOURCC_NV12;
    external.width = external.height = 16;
    external.data_size = 512;
    external.num_planes = 2;
    external.pitches[0] = external.pitches[1] = 16;
    external.offsets[1] = 256;
    external.buffers = handles.data();
    external.num_buffers = scenario == 3 ? 1 : handles.size();
    VASurfaceAttrib attributes[2] = {};
    attributes[0].type = VASurfaceAttribMemoryType;
    attributes[0].value.type = VAGenericValueTypeInteger;
    attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
    attributes[1].value.type = VAGenericValueTypePointer;
    attributes[1].value.value.p = &external;
    VASurfaceID surfaces[2] = {VA_INVALID_SURFACE, VA_INVALID_SURFACE};
    const size_t before = fd_count();
    const VAStatus status = CreateSurfaces2(&fixture.context, VA_RT_FORMAT_YUV420,
                                            16, 16, surfaces, 2, attributes, 2);
    if (scenario >= 2) {
      Require(status != VA_STATUS_SUCCESS && fixture.driver.surfaces.size() == 2 &&
                  fixture.driver.backing_owners.empty() && fd_count() == before,
              "invalid second handle or short legacy batch rolls back all ownership and fds");
    } else {
      Require(status == VA_STATUS_SUCCESS && surfaces[0] != surfaces[1],
              "one legacy descriptor imports all requested buffers[] surfaces");
      {
        const auto first = fixture.driver.surfaces.at(surfaces[0]);
        const auto second = fixture.driver.surfaces.at(surfaces[1]);
        Require(first->planes[0][0] == 40 &&
                    second->planes[0][0] == (scenario == 1 ? 40 : 70) &&
                    first->backing_owner == VA_INVALID_SURFACE &&
                    second->backing_owner ==
                        (scenario == 1 ? surfaces[0] : VA_INVALID_SURFACE),
                "legacy batch selects each handle and retains same-backing alias ownership");
      }
      Require(DestroySurfaces(&fixture.context, surfaces, 2) == VA_STATUS_SUCCESS &&
                  fixture.driver.surfaces.size() == 2 &&
                  fixture.driver.backing_owners.empty() && fd_count() == before,
              "destroy both legacy imports without losing independent caller handles");
    }
    for (int fd : files.object_fds)
      Require(fcntl(fd, F_GETFD) >= 0, "batch import never consumes caller-owned descriptors");
  }
  for (unsigned failure = 0; failure < 4; ++failure) {
    Fixture fixture;
    Surface file;
    const int fd = memfd_create("crystalhd-private-legacy-span", MFD_CLOEXEC);
    Require(fd >= 0, "allocate legacy span-rejection backing");
    file.object_fds.push_back(fd);
    Require(ftruncate(fd, 512) == 0, "size private legacy span-rejection backing");
    uintptr_t handle = fd;
    VASurfaceAttribExternalBuffers external = {};
    external.pixel_format = VA_FOURCC_NV12;
    external.width = external.height = 16;
    external.data_size = 512;
    external.num_planes = 2;
    external.pitches[0] = external.pitches[1] = 16;
    external.offsets[1] = 256;
    external.num_buffers = 1;
    external.buffers = &handle;
    const unsigned plane = failure < 2 ? 0 : 1;
    const unsigned rows = plane == 0 ? 16 : 8;
    if ((failure & 1U) == 0)
      external.pitches[plane] = UINT32_MAX / (rows - 1) + 1;
    else
      external.data_size = external.offsets[plane] +
          (rows - 1) * external.pitches[plane] + 16 - 1;
    VASurfaceAttrib attributes[2] = {};
    attributes[0].type = VASurfaceAttribMemoryType;
    attributes[0].value.type = VAGenericValueTypeInteger;
    attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
    attributes[1].value.type = VAGenericValueTypePointer;
    attributes[1].value.value.p = &external;
    VASurfaceID rejected = VA_INVALID_SURFACE;
    const size_t before = fd_count();
    Require(CreateSurfaces2(&fixture.context, VA_RT_FORMAT_YUV420, 16, 16,
                            &rejected, 1, attributes, 2) != VA_STATUS_SUCCESS &&
                fixture.driver.surfaces.size() == 2 &&
                fixture.driver.backing_owners.empty() && fd_count() == before,
            "legacy Y/UV spans reject 32-bit wrap and one-byte-short backing without leaks");
  }
}

void DecodeTargetsRejectAliasesAndBusyWriters() {
  Fixture fixture;
  auto alias = Picture(kTimestampStep, 40);
  alias->backing_owner = 1;
  fixture.driver.surfaces[3] = alias;
  Require(BeginPicture(&fixture.context, 1, 1) == VA_STATUS_SUCCESS,
          "canonical internal surfaces remain accepted as decode targets");
  Require(BeginPicture(&fixture.context, 1, 3) == VA_STATUS_ERROR_INVALID_SURFACE &&
              fixture.decoder->target == 1,
          "alias VLD targets must fail before changing decode-picture state");
  // Check the submission boundary independently, with a deliberately wrong
  // coded width guaranteeing even the pre-fix path cannot open hardware.
  fixture.decoder->target = 3;
  fixture.decoder->have_picture = true;
  fixture.decoder->picture.seq_fields.bits.chroma_format_idc = 1;
  fixture.decoder->slices.resize(1);
  fixture.decoder->slice_data = {{0x65}};
  fixture.decoder->width = 32;
  fixture.decoder->height = 16;
  Require(SubmitPicture(&fixture.driver, fixture.decoder.get(), VAProfileH264High) ==
              VA_STATUS_ERROR_INVALID_SURFACE,
          "submission must independently reject aliases before decoder work");
  fixture.source->vpp_writers = 1;
  Require(BeginPicture(&fixture.context, 1, 1) == VA_STATUS_ERROR_HW_BUSY,
          "canonical decode targets cannot overwrite a pending VPP writer");
  fixture.decoder->target = 1;
  Require(SubmitPicture(&fixture.driver, fixture.decoder.get(), VAProfileH264High) ==
              VA_STATUS_ERROR_HW_BUSY,
          "submission must recheck pending writer ownership");
}

void PutImageOwnershipFailuresCannotComplete() {
  for (bool fail_start : {true, false}) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    Fixture fixture;
    const VAImage image = MakeImage(&fixture);
    fixture.target->object_fds = {101, 102, 103};
    mock.fail_start_fd = fail_start ? 102 : -1;
    mock.fail_end_fd = fail_start ? -1 : 102;
    const auto before = fixture.target->storage;
    Require(PutImage(&fixture.context, 2, image.image_id, 0, 0, 16, 16,
                     0, 0, 16, 16) == VA_STATUS_ERROR_OPERATION_FAILED &&
                fixture.target->failed && !fixture.target->ready,
            "PutImage START/END failure cannot complete a surface");
    if (fail_start)
      Require(fixture.target->storage == before,
              "failed PutImage START cannot change destination pixels");
    Require(mock.started.empty() && !mock.invalid,
            "failed PutImage must END only acquired objects");
  }
}

void FenceCompletionFailuresFailWithoutSelfWait() {
  for (bool fail_signal : {true, false}) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    Fixture fixture;
    std::lock_guard<std::mutex> lock(fixture.driver.mutex);
    fixture.target->object_fds = {101, 102, 103};
    PendingVpp pending;
    pending.source = fixture.source;
    pending.target = fixture.target;
    pending.target_owner = fixture.target;
    pending.sequence = 1;
    pending.write_timeline = BeginFencedWrite(fixture.target.get());
    Require(pending.write_timeline >= 0, "establish mocked pending VPP fence");
    fixture.source->vpp_readers = 1;
    fixture.target->vpp_writers = 1;
    fixture.target->latest_vpp_sequence = 1;
    fixture.driver.pending_vpp.push_back(pending);
    mock.fail_signal = fail_signal;
    mock.fail_end_fd = fail_signal ? -1 : 102;
    ReleasePendingVpp(&fixture.driver, 1, VA_STATUS_SUCCESS, true);
    Require(fixture.target->failed && !fixture.target->ready,
            "fence signal or CPU END failure must fail the published surface");
    Require(mock.fence_status == (fail_signal ? -ENOENT : 1),
            "failed signal aborts; a later CPU END error cannot retract an already signaled fence");
    Require(mock.started.empty() && mock.handles.empty() && !mock.self_wait &&
                !mock.invalid && fixture.driver.pending_vpp.empty(),
            "fence failure cleanup must release timeline before CPU self-wait");
  }
  FenceIoMock mock;
  MockFenceScope scope(mock);
  Surface surface;
  surface.object_fds = {101, 102, 103};
  mock.fail_import_fd = 102;
  mock.fail_signal = true;
  Require(BeginFencedWrite(&surface) == -1 && mock.signaled &&
              mock.started.empty() && mock.handles.empty() && !mock.self_wait &&
              !mock.invalid &&
              std::find(mock.events.begin(), mock.events.end(), "signal") ==
                  mock.events.end(),
          "partial import abort must not depend on a successful increment ioctl");
}

void AbortPathsPublishErrorFences() {
  unsigned failures = 0;
  for (unsigned mode = 0; mode < 6; ++mode) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    auto fixture = std::make_unique<Fixture>();
    const auto target = fixture->target;
    const auto source = fixture->source;
    const auto pixels = target->storage;
    {
      std::unique_lock<std::mutex> lock(fixture->driver.mutex);
      target->object_fds = {101, 102, 103};
      if (mode == 4) {
        mock.fail_import_fd = 102;
        Require(BeginFencedWrite(target.get()) == -1, "reject partial import");
      } else {
        PendingVpp pending;
        pending.source = source;
        pending.target = target;
        pending.target_owner = target;
        pending.sequence = 1;
        pending.write_timeline = BeginFencedWrite(target.get());
        Require(pending.write_timeline >= 0, "establish an actual production fenced write");
        source->vpp_readers = 1;
        target->vpp_writers = 1;
        target->latest_vpp_sequence = 1;
        fixture->driver.pending_vpp.push_back(pending);
        if (mode == 3) {
          // Prevent scheduling this queued item: the real Driver destructor
          // must abort it after joining its worker, not report a completed copy.
          fixture->driver.stopping = true;
        } else {
          VAStatus status = VA_STATUS_SUCCESS;
          if (mode == 1) {
            const VARectangle valid = {0, 0, 16, 16};
            const VARectangle invalid = {0, 0, 17, 16};
            status = ProcessVpp(&fixture->driver.vpp_scaler,
                &fixture->driver.vpp_argb_staging, source.get(), target.get(),
                valid, invalid, true);
            Require(status == VA_STATUS_ERROR_INVALID_PARAMETER,
                    "exercise production conversion rejection");
          } else if (mode == 2) {
            source->failed = true;
            status = SyncDecodeSurface(&fixture->driver, source, &lock, 0);
            Require(status == VA_STATUS_ERROR_DECODING_ERROR,
                    "exercise production decoder failure without hardware");
          } else if (mode == 5) {
            mock.fail_end_fd = 102;
          }
          ReleasePendingVpp(&fixture->driver, 1, status, mode == 1 || mode == 2);
          Require(target->failed && !target->ready && source->vpp_readers == 0 &&
                      target->vpp_writers == 0 && fixture->driver.pending_vpp.empty(),
                  "aborted operation releases bookkeeping and fails the surface");
        }
      }
    }
    fixture.reset();
    Require(target->storage == pixels && mock.started.empty() && mock.handles.empty() &&
                !mock.self_wait && !mock.invalid,
            "abort must preserve pixels and release all ownership without self-wait");
    const auto closed = std::find(mock.events.begin(), mock.events.end(),
                                  "close:" + std::to_string(FenceIoMock::timeline));
    const auto end = std::find(mock.events.begin(), mock.events.end(), "end:101");
    if (mock.fence_status >= 0 || closed >= end ||
        std::find(mock.events.begin(), mock.events.end(), "signal") != mock.events.end()) {
      fprintf(stderr, "Abort fence mode %u reported status %d (expected error before CPU END)\n",
              mode, mock.fence_status);
      ++failures;
    }
  }
  Require(failures == 0,
          "cancel, conversion/decode failures, destruction and partial import must not signal success");
}

// Opt-in kernel contract check. This needs access to the existing debugfs
// sw_sync node but opens no DRM/CrystalHD device and changes no permissions.
// Default checks above remain entirely hardware/privilege independent.
void GenuineSwSyncCompletionStatuses() {
  struct Handles {
    int timeline = -1;
    int fence = -1;
    ~Handles() {
      if (timeline >= 0) close(timeline);
      if (fence >= 0) close(fence);
    }
  };
  for (bool completed : {false, true}) {
    Handles handles;
    handles.timeline = open(kSwSyncPath, O_RDWR | O_CLOEXEC);
    if (handles.timeline < 0)
      throw std::runtime_error(std::string("open existing sw_sync: ") + strerror(errno));
    SwSyncCreateFenceData create = {};
    create.value = 1;
    create.fence = -1;
    snprintf(create.name, sizeof(create.name), "crystalhd-status-test");
    Require(ioctl(handles.timeline, SW_SYNC_IOC_CREATE_FENCE, &create) == 0,
            "create genuine sw_sync fence");
    handles.fence = create.fence;
    sync_file_info before = {};
    Require(ioctl(handles.fence, SYNC_IOC_FILE_INFO, &before) == 0 &&
                before.status == 0,
            "new genuine fence must be pending");
    const int timeline = handles.timeline;
    handles.timeline = -1;  // the production helper consumes this descriptor
    Require(completed ? EndFencedWrite(nullptr, timeline)
                      : AbortFencedWrite(nullptr, timeline),
            "finish genuine timeline through production helper");
    sync_file_info after = {};
    Require(ioctl(handles.fence, SYNC_IOC_FILE_INFO, &after) == 0 &&
                after.status == (completed ? 1 : -ENOENT),
            "genuine fence must distinguish committed success from abort error");
    printf("Genuine sw_sync %s: status %d -> %d PASS\n",
            completed ? "completion" : "abort", before.status, after.status);
  }
}

}  // namespace

// Manually admitted private latch: tests helper/pixels, not RX-release validation.
static void ExerciseFirstFieldCpuView(bool bottom, unsigned visible) {
  auto decoder = std::make_shared<DecodeContext>();
  auto source = std::make_shared<Surface>();
  Require(source->AllocateCpuNv12(16, 32), "source allocation");
  decoder->profile = VAProfileH264Main;
  decoder->generation = 3;
  source->expected_timestamp = 1;
  source->decode_identity = 41;
  source->trace_generation = 3;
  decoder->decoded_frames[1] = source;
  Require(decoder->replay.Append(1, true, {1}), "append actual fixture token");
  const auto *unit = decoder->replay.NextInput();
  Require(unit && unit->timestamp == 1 && decoder->replay.InputSent(), "submit token");
  std::vector<uint8_t> fields[2];
  for (unsigned f = 0; f != 2; ++f) {
    fields[f].resize(16 * 16 * 2);
    for (unsigned y = 0; y != 16; ++y)
      for (unsigned x = 0; x != 16; x += 2) {
        const size_t at = (y * 16 + x) * 2;
        fields[f][at] = static_cast<uint8_t>(20 + 80*f + y + x);
        fields[f][at+2] = static_cast<uint8_t>(21 + 80*f + y + x);
        fields[f][at+1] = static_cast<uint8_t>(40 + 70*f + y + x);
        fields[f][at+3] = static_cast<uint8_t>(60 + 60*f + y + x);
      }
  }
  auto &first = decoder->h264_first_field;
  first.valid = true;
  first.bottom = first.bottom_first = bottom;
  first.timestamp = 1;
  first.decode_identity = 41;
  first.generation = 3;
  first.picture = source;
  first.width = 16;
  first.height = 32;
  first.source_pitch = 32;
  first.pixels = fields[bottom ? 1 : 0];
  PendingVpp operation;
  operation.source = source;
  operation.decoder = decoder;
  operation.source_timestamp = 1;
  operation.decoder_generation = 3;
  operation.deinterlace.bob = true;
  operation.deinterlace.field = bottom ? crystalhd_vpp_bob::FieldParity::kBottom
                                      : crystalhd_vpp_bob::FieldParity::kTop;
  std::shared_ptr<Surface> view;
  Require(PrepareFirstFieldBob(operation, &view) == VA_STATUS_SUCCESS && view,
        "private first temporal parity view");
  Require(!source->ready && source->frame_timestamp == 0 && first.valid &&
        decoder->replay.outstanding() == 1, "no public completion/retirement");
  Surface complete, actual, expected;
  Require(complete.AllocateCpuNv12(16, 32) && actual.AllocateCpuNv12(8, 16) &&
        expected.AllocateCpuNv12(8, 16), "oracle allocations");
  const uint8_t *pair[2] = {fields[0].data(), fields[1].data()};
  Require(CopyH264InterlacedToSurface(&complete, pair, 16, 32, 32, 1), "full pair");
  SwsContext *scaler = nullptr;
  std::vector<uint8_t> staging;
  const VARectangle region = {0, 0, 16, static_cast<uint16_t>(visible)};
  const VARectangle output = {0, 0, 8, 16};
  Require(ProcessVpp(&scaler, &staging, view.get(), &actual, region, output,
                   false, operation.deinterlace, true) == VA_STATUS_SUCCESS &&
        ProcessVpp(&scaler, &staging, &complete, &expected, region, output,
                   false, operation.deinterlace) == VA_STATUS_SUCCESS, "Bob both views");
  Require(scaler == nullptr, "no software-scaler dependency");
  for (unsigned p = 0; p != 2; ++p)
    for (unsigned y = 0; y != (p ? 8U : 16U); ++y)
      Require(!memcmp(actual.planes[p] + y*actual.pitch[p],
                    expected.planes[p] + y*expected.pitch[p], 8), "active YUV equality");
  operation.deinterlace.field = bottom ? crystalhd_vpp_bob::FieldParity::kTop
                                      : crystalhd_vpp_bob::FieldParity::kBottom;
  view.reset();
  Require(PrepareFirstFieldBob(operation, &view) == VA_STATUS_SUCCESS && !view,
        "opposite parity remains unavailable");
  Require(!source->ready && decoder->replay.outstanding() == 1, "negative preserves owner");
}

static void FirstFieldCpuViewPreservesPublicReadiness() {
  for (bool bottom : {false, true})
    for (unsigned visible : {32U, 28U}) ExerciseFirstFieldCpuView(bottom, visible);
}

// Keep the complete entry globally compiled for syntax coverage. CPU-only
// section-GC builds retain only their selected state-machine groups below.
int VppFullTestEntry(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--trace-vpp") == 0) {
    try { VppTraceKeepsQueuedIdentity(); puts("VA-API VPP trace PASS"); return 0; }
    catch (const std::exception &error) { fprintf(stderr, "%s\n", error.what()); return 1; }
  }
  if (argc == 2 && strcmp(argv[1], "--bgra-images") == 0) {
    try {
      BgraImagesPreserveBytesAndDerivedOwnership();
      puts("VA-API BGRA image bytes/ownership PASS");
      return 0;
    } catch (const std::exception &error) {
      fprintf(stderr, "VA-API BGRA image failed: %s\n", error.what());
      return 1;
    }
  }
  if (argc == 2 && strcmp(argv[1], "--color-standards") == 0) {
    try {
      VppColorStandardsNv12AndRejections();
      VppColorStandardsArgbPixelsAndReuse();
      VppColorStandardQueuedSnapshotAndCancellation();
      puts("VA-API color standards pixels/capture/cancellation PASS");
      return 0;
    } catch (const std::exception &error) {
      fprintf(stderr, "VA-API color standards failed: %s\n", error.what());
      return 1;
    }
  }
  if (argc == 2 && strcmp(argv[1], "--native-dumb-offset") == 0)
    return NativeDumbOffsetTestEntry();
  if (argc == 2 && strcmp(argv[1], "--centered-readonly-pending") == 0)
    return CenteredReadOnlyPendingTestEntry();
  if (argc == 2 && strcmp(argv[1], "--readonly-cpu-pending") == 0)
    return ReadOnlyCpuPendingTests();
  if (argc == 2 && strcmp(argv[1], "--interleaved-scaling") == 0) {
    try {
      VppInterleavedScalingPreservesFieldRows();
      VppInterleavedArgbScalingAndQueuedSnapshot();
      VppInterleavedInvalidGeometryRejectsBeforeCapture();
      VppInterleavedArgbChromaMatchesCompactedFields();
      puts("VA-API interleaved scaling pixels/geometry/lifetime PASS");
      return 0;
    } catch (const std::exception &error) {
      fprintf(stderr, "VA-API interleaved scaling failed: %s\n", error.what());
      return 1;
    }
  }
  if (argc == 2 && strcmp(argv[1], "--bob-fhd-return") == 0) {
    try {
      Bob720ReturnsFhdThroughDirectAndTwoStageVpp();
      puts("VA-API 720 Bob to FHD direct/staged pixels/identity/lifetime PASS");
      return 0;
    } catch (const std::exception &error) {
      fprintf(stderr, "VA-API FHD return failed: %s\n", error.what());
      return 1;
    }
  }
  if (argc == 2 && strcmp(argv[1], "--vpp-chaining") == 0) {
    try {
      CompletedVppOutputsRetainChainedSnapshots();
      ExternalWritersCannotAuthorizeCompletedVppSources();
      puts("VA-API completed VPP chaining/snapshot/provenance PASS");
      return 0;
    } catch (const std::exception &error) {
      fprintf(stderr, "VA-API VPP chaining failed: %s\n", error.what());
      return 1;
    }
  }
  if (argc == 2 && (strcmp(argv[1], "--config-attributes") == 0 ||
                    strcmp(argv[1], "--color-range") == 0 ||
                    strcmp(argv[1], "--display-attributes") == 0 ||
                    strcmp(argv[1], "--subpicture-formats") == 0 ||
                    strcmp(argv[1], "--selected-rt-formats") == 0 ||
                    strcmp(argv[1], "--decode-processing") == 0 ||
                    strcmp(argv[1], "--pipeline-semantics") == 0)) {
    try {
      if (strcmp(argv[1], "--decode-processing") == 0) {
        ConfigAttributesMatchAdvertisedSupport();
        DecodeProcessingAdmissionIsExplicitAndCaptured();
        OpaqueDecodeOriginalRejectsPixelAccess();
      } else if (strcmp(argv[1], "--color-range") == 0) {
        VppFullRangeArgbUsesDeclaredInput();
        VppColorRangeArgbReuseAndBob();
        VppColorRangeNv12PreservesSamples();
        VppColorRangeRejectsUnsupportedRequests();
        VppColorRangeQueuedSnapshotAndCancellation();
      } else if (strcmp(argv[1], "--config-attributes") == 0)
        ConfigAttributesMatchAdvertisedSupport();
      else if (strcmp(argv[1], "--display-attributes") == 0)
        UnsupportedDisplayAttributesAreNotSuccessful();
      else if (strcmp(argv[1], "--subpicture-formats") == 0) {
        EmptySubpictureFormatsValidateCount();
        UnsupportedSurfaceErrorsDoNotInventDetails();
      }
      else if (strcmp(argv[1], "--selected-rt-formats") == 0)
        SelectedRtFormatSurvivesConfigAndContextLifetime();
      else
        VppMeaningfulSemanticsAreValidatedBeforeCapture();
      puts("VA-API config/display attribute admission PASS");
      return 0;
    } catch (const std::exception &error) {
      fprintf(stderr, "VA-API attribute admission failed: %s\n", error.what());
      return 1;
    }
  }
  if (argc > 2 || (argc == 2 && strcmp(argv[1], "--sw-sync") != 0 &&
                   strcmp(argv[1], "--lifecycle-failures") != 0 &&
                   strcmp(argv[1], "--import-contracts") != 0 &&
                   strcmp(argv[1], "--completed-source") != 0)) {
    fprintf(stderr, "usage: %s [--sw-sync|--lifecycle-failures|--import-contracts|--completed-source]\n", argv[0]);
    return 2;
  }
  try {
    if (argc == 2 && (strcmp(argv[1], "--lifecycle-failures") == 0 ||
                      strcmp(argv[1], "--import-contracts") == 0 ||
                      strcmp(argv[1], "--completed-source") == 0)) {
      const std::pair<const char *, void (*)()> lifecycle[] = {
          {"VPP queue allocation failure is transactional", VppQueueAllocationFailureIsTransactional},
          {"stale VPP failure preserves newer writer", StaleVppFailurePreservesNewerWriter},
          {"zero-timeout sync does not block on driver admission", SyncTimeoutAdmissionDoesNotBlockZeroQueries},
          {"VPP backlog turn preserves cancellation and ownership", VppBacklogTurnPreservesCancellationAndOwnership}};
      const std::pair<const char *, void (*)()> imports[] = {
          {"import bookkeeping failure returns errors and releases resources", ImportedBookkeepingFailures},
          {"PRIME export preserves imported plane layout", ImportedPrimeExportsPreservePlaneLayouts},
          {"legacy PRIME batch uses one descriptor", LegacyPrimeBatchUsesOneDescriptorAndRollsBack}};
      const std::pair<const char *, void (*)()> completed[] = {
          {"completed decoder close keeps new Bob captures exact", CompletedDecoderCloseKeepsNewBobCapturesExact},
          {"captured Bob survives successful decoder close", CapturedBobSurvivesSuccessfulDecoderClose},
          {"captured Bob rejects failed decoder close", CapturedBobRejectsFailedDecoderClose},
          {"captured Bob rejects rebound owner after close", CapturedBobRejectsReboundOwnerAfterSuccessfulDecoderClose},
          {"queued Bob survives successful decoder close", QueuedBobSurvivesSuccessfulDecoderClose},
          {"ineligible decoder close cannot grant new Bob reads", IneligibleDecoderCloseCannotGrantNewBobReads}};
      const std::pair<const char *, void (*)()> *groups = lifecycle;
      size_t group_count = sizeof(lifecycle) / sizeof(lifecycle[0]);
      if (strcmp(argv[1], "--import-contracts") == 0) {
        groups = imports;
        group_count = sizeof(imports) / sizeof(imports[0]);
      } else if (strcmp(argv[1], "--completed-source") == 0) {
        groups = completed;
        group_count = sizeof(completed) / sizeof(completed[0]);
      }
      unsigned int failures = 0;
      for (size_t i = 0; i != group_count; ++i) {
        const auto &group = groups[i];
        try {
          group.second();
          printf("PASS %s\n", group.first);
        } catch (const std::exception &error) {
          ++failures;
          fprintf(stderr, "FAIL %s: %s\n", group.first, error.what());
        }
      }
      printf("VPP API regressions: %zu groups, %u failures\n",
             group_count, failures);
      return failures == 0 ? 0 : 1;
    }
    if (argc == 2) {
      GenuineSwSyncCompletionStatuses();
      return 0;
    }
    VppTraceKeepsQueuedIdentity();
    UnusedVppProbeDoesNotNotifyDecoder();
    DefaultVppCapabilitiesRemainAvailable();
    ConfigAttributesMatchAdvertisedSupport();
    DecodeProcessingAdmissionIsExplicitAndCaptured();
    OpaqueDecodeOriginalRejectsPixelAccess();
    UnsupportedDisplayAttributesAreNotSuccessful();
    EmptySubpictureFormatsValidateCount();
    UnsupportedSurfaceErrorsDoNotInventDetails();
    SelectedRtFormatSurvivesConfigAndContextLifetime();
    VppMeaningfulSemanticsAreValidatedBeforeCapture();
    VppInterleavedScalingPreservesFieldRows();
    VppInterleavedArgbScalingAndQueuedSnapshot();
    VppInterleavedInvalidGeometryRejectsBeforeCapture();
    VppInterleavedArgbChromaMatchesCompactedFields();
    VppColorStandardsNv12AndRejections();
    VppColorStandardsArgbPixelsAndReuse();
    VppColorStandardQueuedSnapshotAndCancellation();
    VppFullRangeArgbUsesDeclaredInput();
    VppColorRangeArgbReuseAndBob();
    VppColorRangeNv12PreservesSamples();
    VppColorRangeRejectsUnsupportedRequests();
    VppColorRangeQueuedSnapshotAndCancellation();
    ConfigRejectsUnsupportedProtectionTransactionally();
    ProtectedBufferCreationIsTransactional();
    PipelineCapsIgnoreIncomingPixelFormatStorage();
    PipelineCapsBorrowedLifetimeAndErrors();
    NativeNodeCacheOwnsNoFiles();
    NativeNodeCacheInvalidatesChangedIdentities();
    NativeNodeCacheFailuresRetryWithoutLeases();
    NativeSurfaceFilesRetainOriginalLifetime();
    NativeDumbBookkeepingFailures();
    SurfaceCreationAllocationFailures();
    ImportedBookkeepingFailures();
    NativeDumbMappingPreserves64BitOffsets();
    I915WcMappingRequiresExactOffsetIdentity();
    AbortPathsPublishErrorFences();
    DecodeTeardownStillNotifies();
    PendingVppTeardownStillNotifies();
    VppQueueAllocationFailureIsTransactional();
    StaleVppFailurePreservesNewerWriter();
    BobCapabilityEnumerationAndCapacity();
    BobPipelineFilterValidation();
    BobImmediatePixelsAndWeaveReset();
    BobAlignedOutputRegionsPreservePixelsAndBorders();
    Nv12ScalingPreservesCropsBordersAndFieldRows();
    Nv12InPlaceCannotDestroyInput();
    BobHorizontalRowsReuseWithoutStalePixels();
    VppFastModeKeepsNearestPixelsAndRejectsOtherFilters();
    BobAlignedOutputRegionQueuedSnapshotAndCancellation();
    Bob720ReturnsFhdThroughDirectAndTwoStageVpp();
    CompletedVppOutputsRetainChainedSnapshots();
    ExternalWritersCannotAuthorizeCompletedVppSources();
    FirstFieldCpuViewPreservesPublicReadiness();
    CompletedDecoderCloseKeepsNewBobCapturesExact();
    CapturedBobSurvivesSuccessfulDecoderClose();
    CapturedBobRejectsFailedDecoderClose();
    CapturedBobRejectsReboundOwnerAfterSuccessfulDecoderClose();
    QueuedBobSurvivesSuccessfulDecoderClose();
    IneligibleDecoderCloseCannotGrantNewBobReads();
    BobFilterValuesAreCapturedAtRender();
    BobQueuedSnapshotAndCancellation();
    BobRejectedReplacementCannotReplayOldParameters();
    BobRejectsUnsupportedPipelineParameters();
    BobRejectsCanonicalSourceTargetAlias();
    BobRejectsCroppedRegions();
    RepeatedVppRetainsPicture();
    DirectCaptureKeepsQueuedVppPrivate();
    InternalQueuedVppCompletesWithoutSwSync();
    ReadOnlyExportedBobPendingPixels();
    ReadOnlyExportedBobWritableExposureKeepsOldAdmission();
    ReadOnlyExportedBobCancellationReleasesOwners();
    CenteredReadOnlyBobPending();
    SyncTimeoutAdmissionDoesNotBlockZeroQueries();
    VppBacklogTurnPreservesCancellationAndOwnership();
    VppPixelLeaseAdmitsFrontendAndRetainsMappings();
    ExposedQueuedVppStillRequiresFence();
    ExposedColdVppFallbackCompletesBeforeReturning();
    UnsafeFenceFailuresCannotUseSynchronousFallback();
    ReuseBetweenRenderAndEndKeepsCapturedPicture();
    ResetRejectsCapturedOldEpoch();
    MissingPictureIsNotSuccessfulFallback();
    BusyTargetIsNotReplaced();
    CanceledVppFailsWithoutOverwriting();
    RetiredContextDoesNotAcknowledgeAFrame();
    MalformedSurfaceTeardownIsAtomic();
    ValidSurfaceTeardownRetiresQueuedEpoch();
    CpuWriteStartUnwindsAcquiredPrefix();
    FencedWriteStartFailureClosesUnpublishedFence();
    FenceCreationFailureAvoidsCpuOwnership();
    PartialFenceImportAbortsBeforeCpuEnd();
    CompletedFenceSignalsBeforeCpuEnd();
    VppCpuOwnershipFailuresAreNotSuccessfulFrames();
    CpuReadStartUnwindsAcquiredPrefix();
    CpuSyncRetriesInterruptedAccess();
    CpuReadFailuresCannotCompleteCopies();
    ImageAllocationFailuresAreTransactional();
    ImageReadFailuresAndDerivedRollback();
    DerivedWritesCommitActualSurfacePixels();
    DerivedWriteLeaseBlocksReuseAndSurvivesDestruction();
    DerivedDefaultRefreshAndDecodedSnapshotsStayIndependent();
    BgraImagesPreserveBytesAndDerivedOwnership();
    ImageGeometryAndOddNv12Roundtrip();
    ImageCopiesRejectInvalidAndBusyRectangles();
    PutImageCannotOverwriteRetainedDecodeIdentity();
    PutImageOwnershipFailuresCannotComplete();
    FenceCompletionFailuresFailWithoutSelfWait();
    OrphanedAliasesCannotWaitOnPendingFence();
    VppRechecksOwnersBetweenPublicCalls();
    PendingBackingReimportIsRejectedBeforeMapping();
    FirstExternalImportOwnsAliasesAndRollsBackAtomically();
    DifferentLumaObjectsCannotAliasSharedChroma();
    ImportedPrimeExportsPreservePlaneLayouts();
    LegacyPrimeBatchUsesOneDescriptorAndRollsBack();
    DecodeTargetsRejectAliasesAndBusyWriters();
  } catch (const std::exception &error) {
    fprintf(stderr, "VA-API VPP regression failed: %s\n", error.what());
    return 1;
  }
  puts("VA-API VPP capabilities/identity/lifecycle/fences/images: 114 hardware-free regressions passed");
  return 0;
}

int main(int argc, char **argv) {
  if ((argc == 2 || argc == 5) && strcmp(argv[1], "--image-allocation") == 0) {
    try {
      if (argc == 2) {
        ImageAllocationFailuresAreTransactional();
      } else {
        const bool derive = strcmp(argv[2], "derive") == 0;
        const bool bgra = strcmp(argv[3], "bgra") == 0;
        if ((!derive && strcmp(argv[2], "create") != 0) ||
            (!bgra && strcmp(argv[3], "nv12") != 0) ||
            argv[4][0] < '1' || argv[4][0] > '3' || argv[4][1] != '\0')
          return 2;
        ImageAllocationFailure(derive, bgra, static_cast<unsigned>(argv[4][0] - '0'));
      }
      puts("VA-API image allocation rollback PASS");
      return 0;
    } catch (const std::exception &error) {
      fprintf(stderr, "VA-API image allocation rollback failed: %s\n", error.what());
      return 1;
    }
  }
  const std::pair<const char *, void (*)()> surface_cases[] = {
      {"--surface-create-storage", CpuSurfaceCreationStorageFailureRollsBack},
      {"--surface-create-id-log", CpuSurfaceCreationIdLogFailureRollsBack},
      {"--surface-create-cpu-sweep", CpuSurfaceCreationAllocationFailuresRollBack},
      {"--surface-create-import-sweep", ImportedSurfaceCreationAllocationFailuresRollBack},
      {"--surface-create-transaction", SurfaceCreationAllocationFailures}};
  if (argc == 2) {
    for (const auto &selected : surface_cases) {
      if (strcmp(argv[1], selected.first) != 0) continue;
      try {
        selected.second();
        printf("VA-API surface creation allocation rollback %s PASS\n", selected.first);
        return 0;
      } catch (const std::exception &error) {
        fprintf(stderr, "VA-API surface creation allocation rollback %s failed: %s\n",
                selected.first, error.what());
        return 1;
      }
    }
  }
  if (argc == 5 && strcmp(argv[1], "--import-allocation") == 0) {
    const bool prime = strcmp(argv[2], "prime") == 0;
    const bool public_api = strcmp(argv[4], "api") == 0;
    const unsigned allocation = static_cast<unsigned>(strtoul(argv[3], nullptr, 10));
    if ((!prime && strcmp(argv[2], "legacy") != 0) ||
        (!public_api && strcmp(argv[4], "direct") != 0) ||
        allocation < 1 || allocation > 3) return 2;
    try {
      ImportedBookkeepingFailure(prime, allocation, public_api);
      puts("VA-API import allocation failure cleanup PASS");
      return 0;
    } catch (const std::exception &error) {
      fprintf(stderr, "VA-API import allocation failure cleanup failed: %s\n", error.what());
      return 1;
    }
  }
  if (argc == 3 && strcmp(argv[1], "--native-dumb-allocation") == 0) {
    const unsigned allocation = static_cast<unsigned>(strtoul(argv[2], nullptr, 10));
    if (allocation < 1 || allocation > 3) return 2;
    try {
      NativeDumbBookkeepingFailure(allocation);
      puts("VA-API DUMB allocation failure cleanup PASS");
      return 0;
    } catch (const std::exception &error) {
      fprintf(stderr, "VA-API DUMB allocation failure cleanup failed: %s\n", error.what());
      return 1;
    }
  }
  if (argc == 2 && strcmp(argv[1], "--pixel-lease") == 0) {
    try {
      VppPixelLeaseAdmitsFrontendAndRetainsMappings();
      puts("VA-API VPP pixel lease admission/lifetime PASS");
      return 0;
    } catch (const std::exception &error) {
      fprintf(stderr, "VA-API VPP pixel lease failed: %s\n", error.what());
      return 1;
    }
  }
  if (argc == 2 && strcmp(argv[1], "--nv12-scaling") == 0) {
    try {
      Nv12ScalingPreservesCropsBordersAndFieldRows();
      Nv12InPlaceCannotDestroyInput();
      puts("VA-API ordinary NV12 scaling pixels/crops/fields/padding/work PASS");
      return 0;
    } catch (const std::exception &error) {
      fprintf(stderr, "VA-API ordinary NV12 scaling failed: %s\n", error.what());
      return 1;
    }
  }

  if (argc == 2 && strcmp(argv[1], "--bob-resample-reuse") == 0) {
    try {
      BobHorizontalRowsReuseWithoutStalePixels();
      VppFastModeKeepsNearestPixelsAndRejectsOtherFilters();
      puts("VA-API Bob row reuse pixels/fields/padding PASS");
      return 0;
    } catch (const std::exception &error) {
      fprintf(stderr, "VA-API Bob row reuse failed: %s\n", error.what());
      return 1;
    }
  }

  if (argc == 2 && strcmp(argv[1], "--native-dumb-offset") == 0)
    return NativeDumbOffsetTestEntry();
  if (argc == 2 && strcmp(argv[1], "--centered-readonly-pending") == 0)
    return CenteredReadOnlyPendingTestEntry();
  if (argc == 2 && strcmp(argv[1], "--readonly-cpu-pending") == 0)
    return ReadOnlyCpuPendingTests();
#ifndef CRYSTALHD_VPP_CPU_ONLY
  return VppFullTestEntry(argc, argv);
#else
  const bool processing_only = argc == 2 && strcmp(argv[1], "--decode-processing") == 0;
  if (argc != 1 && !processing_only) return 2;
  const std::pair<const char *, void (*)()> groups[] = {
      {"native DUMB bookkeeping failure cleanup", NativeDumbBookkeepingFailures},
      {"import bookkeeping failure returns errors and releases resources", ImportedBookkeepingFailures},
      {"native DUMB 64-bit offsets preserve pixels and cleanup", NativeDumbMappingPreserves64BitOffsets},
      {"configuration matches advertised support", ConfigAttributesMatchAdvertisedSupport},
      {"card scaling captures explicit additional output", DecodeProcessingAdmissionIsExplicitAndCaptured},
      {"opaque decode originals reject pixel access", OpaqueDecodeOriginalRejectsPixelAccess},
      {"empty subpicture formats validate count", EmptySubpictureFormatsValidateCount},
      {"unsupported surface errors disclose no invented details", UnsupportedSurfaceErrorsDoNotInventDetails},
      {"NV12 color standards preserve pixels and reject conversions", VppColorStandardsNv12AndRejections},
      {"NV12 range preserves samples and background", VppColorRangeNv12PreservesSamples},
      {"interleaved NV12 scaling preserves field lattices", VppInterleavedScalingPreservesFieldRows},
      {"720 Bob returns FHD through direct and staged VPP", Bob720ReturnsFhdThroughDirectAndTwoStageVpp},
      {"aligned NV12 Bob destination pixels and borders", BobAlignedOutputRegionsPreservePixelsAndBorders},
      {"ordinary NV12 scaling preserves crops/fields/borders/work", Nv12ScalingPreservesCropsBordersAndFieldRows},
      {"ordinary in-place negative controls preserve input", Nv12InPlaceCannotDestroyInput},
      {"Bob horizontal row reuse preserves changing full/compact fields", BobHorizontalRowsReuseWithoutStalePixels},
      {"standard FAST keeps nearest pixels and filter rejections", VppFastModeKeepsNearestPixelsAndRejectsOtherFilters},
      {"queued aligned Bob destination snapshot and cancellation", BobAlignedOutputRegionQueuedSnapshotAndCancellation},
      {"unsupported color ranges reject before capture", VppColorRangeRejectsUnsupportedRequests},
      {"private first-field Bob pixels preserve public readiness", FirstFieldCpuViewPreservesPublicReadiness},
      {"configuration rejects unsupported protection transactionally", ConfigRejectsUnsupportedProtectionTransactionally},
      {"protected buffer creation is transactional", ProtectedBufferCreationIsTransactional},
      {"completed decoder close keeps new Bob captures exact", CompletedDecoderCloseKeepsNewBobCapturesExact},
      {"captured Bob survives successful decoder close", CapturedBobSurvivesSuccessfulDecoderClose},
      {"captured Bob rejects failed decoder close", CapturedBobRejectsFailedDecoderClose},
      {"captured Bob rejects rebound owner after close", CapturedBobRejectsReboundOwnerAfterSuccessfulDecoderClose},
      {"queued Bob survives successful decoder close", QueuedBobSurvivesSuccessfulDecoderClose},
      {"ineligible decoder close cannot grant new Bob reads", IneligibleDecoderCloseCannotGrantNewBobReads},
      {"VPP pixel lease admission and lifetime", VppPixelLeaseAdmitsFrontendAndRetainsMappings},
#ifdef CRYSTALHD_LEGACY_CPU
      {"legacy exposed stopped-source rejection", LegacyExposedStoppedVppRemainsRejected},
      {"legacy synchronous fallback: nine no-cache-operation modes", LegacyExposedColdVppFallbackCompletesBeforeReturning}};
#else
      {"exposed stopped-source rejection", ExposedQueuedVppStillRequiresFence},
      {"synchronous fallback: nine completion/cancellation modes", ExposedColdVppFallbackCompletesBeforeReturning},
      {"unsafe CPU/import failures: three rejection modes", UnsafeFenceFailuresCannotUseSynchronousFallback}};
#endif
  unsigned failures = 0;
  unsigned selected = 0;
  for (const auto &group : groups) {
    if (processing_only && group.second != ConfigAttributesMatchAdvertisedSupport &&
        group.second != DecodeProcessingAdmissionIsExplicitAndCaptured &&
        group.second != OpaqueDecodeOriginalRejectsPixelAccess)
      continue;
    ++selected;
    try {
      group.second();
      printf("PASS %s\n", group.first);
    } catch (const std::exception &error) {
      ++failures;
      fprintf(stderr, "FAIL %s: %s\n", group.first, error.what());
    }
  }
#ifdef CRYSTALHD_LEGACY_CPU
  if (!processing_only)
    puts("Unsafe fence-import path: NOT RUN; unavailable with real legacy cache capabilities");
#endif
  printf("VA-API CPU VPP fallback: %u groups, %u failures\n", selected, failures);
  return failures == 0 ? 0 : 1;
#endif
}
