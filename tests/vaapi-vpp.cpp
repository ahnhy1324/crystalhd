// SPDX-License-Identifier: LGPL-2.1-or-later
// Exercise the actual driver state machine with private memory only: no DRM
// device, firmware, browser, or CrystalHD device is opened by these tests.
#include "../filters/vaapi/crystalhd_drv_video.cpp"

#include <cerrno>
#include <linux/sync_file.h>
#include <stdexcept>
#include <string>

extern "C" int __real_close(int fd);
extern "C" int __real_eventfd(unsigned int initial_value, int flags);

namespace {

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

struct MockFenceScope {
  explicit MockFenceScope(FenceIoMock &mock) { fence_io_mock = &mock; }
  ~MockFenceScope() { fence_io_mock = nullptr; }
};

// A discovery cache must never turn per-surface DRM files into a shared lease.
// Model only the current thread's node files and owned DUMB allocations.
struct NativeNodeMock {
  static constexpr int render_fd = 8100;
  struct File { bool primary; struct stat identity; };
  struct Dumb { int fd; size_t size; uint32_t pitch; uint64_t offset; };
  struct stat render = {}, primary = {};
  char primary_path[64] = "/dev/dri/crystalhd-test-primary";
  char *nodes[DRM_NODE_MAX] = {};
  drmDevice device = {};
  bool device_owned = false, no_primary = false, null_device = false;
  bool fail_destroy = false;
  unsigned fail_queries = 0, fail_opens = 0;
  unsigned fail_render_stats = 0, fail_primary_stats = 0;
  unsigned queries = 0, frees = 0, opens = 0, closes = 0;
  unsigned render_stats = 0, primary_stats = 0;
  unsigned creates = 0, destroys = 0, maps = 0, unmaps = 0;
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
    if (request == DRM_IOCTL_MODE_CREATE_DUMB) {
      auto *create = static_cast<drm_mode_create_dumb *>(argument);
      if (create->bpp != 8) { ++invalid; return Error(); }
      ++creates;
      create->handle = next_handle++;
      create->pitch = Align(create->width, 64);
      create->size = static_cast<size_t>(create->pitch) * create->height;
      dumbs.emplace(create->handle, Dumb{fd, static_cast<size_t>(create->size),
                                        create->pitch, create->handle * 4096U});
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
  void *Map(size_t size, int prot, int flags, int fd, off_t offset) {
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

extern "C" int __real_open(const char *path, int flags, ...);
extern "C" int __real_ioctl(int fd, unsigned long request, ...);
extern "C" int __real_close(int fd);
extern "C" int __real_fstat(int fd, struct stat *info);
extern "C" int __real_drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
extern "C" void __real_drmFreeDevice(drmDevicePtr *device);
extern "C" int __real_drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags,
                                         int *prime_fd);
extern "C" void *__real_mmap(void *, size_t, int, int, int, off_t);
extern "C" int __real_munmap(void *, size_t);

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
  return native_node_mock != nullptr ? native_node_mock->Stat(fd, info)
                                     : __real_fstat(fd, info);
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
  return native_node_mock != nullptr
      ? native_node_mock->Map(size, prot, flags, fd, offset)
      : __real_mmap(address, size, prot, flags, fd, offset);
}
extern "C" int __wrap_munmap(void *address, size_t size) {
  return native_node_mock != nullptr ? native_node_mock->Unmap(address, size)
                                     : __real_munmap(address, size);
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
              caps.num_output_pixel_formats == 2 && caps.output_pixel_format != nullptr &&
              caps.input_pixel_format[0] == VA_FOURCC_NV12 &&
              caps.output_pixel_format[0] == VA_FOURCC_NV12 &&
              caps.output_pixel_format[1] == VA_FOURCC_ARGB &&
              QueryVideoProcFilters(&context, processor, nullptr, &filters) ==
                  VA_STATUS_SUCCESS && filters == 0 &&
              QueryVideoProcFilterCaps(&context, processor, VAProcFilterDeinterlacing,
                                       nullptr, &capabilities) ==
                  VA_STATUS_ERROR_UNSUPPORTED_FILTER && capabilities == 0,
          "scaling/CSC caps remain truthful without inventing a DI filter");
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
                caps->num_output_pixel_formats == 2 && caps->output_pixel_format != nullptr &&
                caps->input_pixel_format[0] == VA_FOURCC_NV12 &&
                caps->output_pixel_format[0] == VA_FOURCC_NV12 &&
                caps->output_pixel_format[1] == VA_FOURCC_ARGB &&
                caps->rotation_flags == (1U << VA_ROTATION_NONE) &&
                caps->filter_flags == (VA_FILTER_SCALING_DEFAULT |
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
          "invalid context and nonzero filters reject before touching output or incoming pointers");
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
    Require(QueryVideoProcFilterCaps(&context, processor, VAProcFilterDeinterlacing,
                                     nullptr, &capabilities) ==
                    VA_STATUS_ERROR_UNSUPPORTED_FILTER && capabilities == 0 &&
                mock.queries == 1 && mock.frees == 1 && mock.files.size() == 16 &&
                mock.creates == 8 * (cycle + 1) && mock.maps == 8 * (cycle + 1),
            "the same unsupported-DI sequence changes discovery cost, not caps/allocation timing");
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

  Fixture() {
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
      Require(PendingVppCanceled(pending), "decoder retirement cancels direct capture epoch");
      ReleasePendingVpp(&fixture.driver, pending.sequence,
                         VA_STATUS_ERROR_OPERATION_FAILED, false);
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
              mock.events == std::vector<std::string>{"start:101", "open", "end:101"} &&
              mock.started.empty() && mock.handles.empty() && !mock.invalid,
          "exposed backing cannot fall back to unfenced writes after fence failure");
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
  Require(!PendingVppCanceled(pending), "old epoch initially remains valid");
  Require(DestroySurfaces(&fixture.context, &source_id, 1) == VA_STATUS_SUCCESS,
          "destroy a valid decode surface");
  Require(fixture.source->destroyed && fixture.driver.surfaces.count(1) == 0 &&
              fixture.decoder->generation == pending.decoder_generation + 1 &&
              PendingVppCanceled(pending),
          "valid teardown must retire private queued pictures with the pool");
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

void FencedWriteStartFailureDoesNotOpenTimeline() {
  FenceIoMock mock;
  MockFenceScope scope(mock);
  Surface surface;
  surface.object_fds = {101, 102, 103};
  mock.fail_start_fd = 102;
  Require(BeginFencedWrite(&surface) == -1, "fenced CPU START must fail");
  Require(mock.events == std::vector<std::string>{
              "start:101", "start:102", "end:101"},
          "failed CPU acquisition must not create a timeline");
  Require(mock.started.empty() && mock.handles.empty() && !mock.invalid,
          "failed fenced acquisition must not leak ownership or handles");
}

void FenceCreationFailureReleasesOwnership() {
  for (bool fail_open : {true, false}) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    Surface surface;
    surface.object_fds = {101, 102};
    mock.fail_open = fail_open;
    mock.fail_create = !fail_open;
    Require(BeginFencedWrite(&surface) == -1,
            "timeline open or fence creation failure must be reported");
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

struct DerivedFixture : Fixture {
  DerivedFixture() { source->id = 1; target->id = 2; }
};

VAStatus MapImageIntent(Fixture *fixture, VABufferID buffer, void **data,
                        uint32_t flags) {
  if (flags == UINT32_MAX) return MapBuffer(&fixture->context, buffer, data);
#if VA_CHECK_VERSION(1, 21, 0)
  return MapBuffer2(&fixture->context, buffer, data, flags);
#else
  return MapBufferWithFlags(&fixture->context, buffer, data, flags);
#endif
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
    VASurfaceAttribExternalBuffers external[2] = {};
    uintptr_t handle = fd;
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
      external[i].pixel_format = VA_FOURCC_NV12;
      external[i].width = 16;
      external[i].height = 16;
      external[i].data_size = 512;
      external[i].num_planes = 2;
      external[i].pitches[0] = 16;
      external[i].pitches[1] = 16;
      external[i].offsets[1] = 256;
      external[i].num_buffers = 1;
      external[i].buffers = &handle;
    }
    VASurfaceAttrib attributes[2] = {};
    attributes[0].type = VASurfaceAttribMemoryType;
    attributes[0].value.type = VAGenericValueTypeInteger;
    attributes[0].value.value.i = legacy ? VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME
                                       : VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
    attributes[1].value.type = VAGenericValueTypePointer;
    attributes[1].value.value.p = legacy ? static_cast<void *>(external)
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
    // The second descriptor passes fd preflight but fails layout import,
    // after the first descriptor has already created its canonical owner.
    prime[1].objects[0].size = 0;
    external[1].data_size = 0;
    Require(create() == VA_STATUS_ERROR_ALLOCATION_FAILED &&
                fixture.driver.surfaces.size() == 2 &&
                fixture.driver.backing_owners.empty(),
            "failed import batch must roll back surfaces and new backing identities");
    prime[1].objects[0].size = 512;
    external[1].data_size = 512;
    Require(create() == VA_STATUS_SUCCESS,
            "import the same external backing twice without exporting it first");
    const auto first = fixture.driver.surfaces.at(surfaces[0]);
    const auto alias = fixture.driver.surfaces.at(surfaces[1]);
    Require(first->backing_owner == VA_INVALID_SURFACE &&
                alias->backing_owner == surfaces[0],
            "the first external import must own every later same-backing alias");
    Require(BeginPicture(&fixture.context, 1, surfaces[0]) == VA_STATUS_SUCCESS,
            "canonical external imports remain accepted as decode targets");
    prime[0].layers[0].offset[0] = external[0].offsets[0] = 16;
    prime[0].layers[0].offset[1] = external[0].offsets[1] = 272;
    VASurfaceID incompatible = VA_INVALID_SURFACE;
    const size_t count = fixture.driver.surfaces.size();
    Require(CreateSurfaces2(&fixture.context, VA_RT_FORMAT_YUV420, 16, 16,
                            &incompatible, 1, attributes, 2) ==
                VA_STATUS_ERROR_INVALID_PARAMETER &&
                incompatible == VA_INVALID_SURFACE &&
                fixture.driver.surfaces.size() == count,
            "same-fd nonidentical valid plane views must not share picture identity");
    prime[0].layers[0].offset[0] = external[0].offsets[0] = 0;
    prime[0].layers[0].offset[1] = external[0].offsets[1] = 256;
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

int main(int argc, char **argv) {
  if (argc > 2 || (argc == 2 && strcmp(argv[1], "--sw-sync") != 0)) {
    fprintf(stderr, "usage: %s [--sw-sync]\n", argv[0]);
    return 2;
  }
  try {
    if (argc == 2) {
      GenuineSwSyncCompletionStatuses();
      return 0;
    }
    DefaultVppCapabilitiesRemainAvailable();
    PipelineCapsIgnoreIncomingPixelFormatStorage();
    PipelineCapsBorrowedLifetimeAndErrors();
    NativeNodeCacheOwnsNoFiles();
    NativeNodeCacheInvalidatesChangedIdentities();
    NativeNodeCacheFailuresRetryWithoutLeases();
    NativeSurfaceFilesRetainOriginalLifetime();
    AbortPathsPublishErrorFences();
    RepeatedVppRetainsPicture();
    DirectCaptureKeepsQueuedVppPrivate();
    InternalQueuedVppCompletesWithoutSwSync();
    ExposedQueuedVppStillRequiresFence();
    ReuseBetweenRenderAndEndKeepsCapturedPicture();
    ResetRejectsCapturedOldEpoch();
    MissingPictureIsNotSuccessfulFallback();
    BusyTargetIsNotReplaced();
    CanceledVppFailsWithoutOverwriting();
    RetiredContextDoesNotAcknowledgeAFrame();
    MalformedSurfaceTeardownIsAtomic();
    ValidSurfaceTeardownRetiresQueuedEpoch();
    CpuWriteStartUnwindsAcquiredPrefix();
    FencedWriteStartFailureDoesNotOpenTimeline();
    FenceCreationFailureReleasesOwnership();
    PartialFenceImportAbortsBeforeCpuEnd();
    CompletedFenceSignalsBeforeCpuEnd();
    VppCpuOwnershipFailuresAreNotSuccessfulFrames();
    CpuReadStartUnwindsAcquiredPrefix();
    CpuSyncRetriesInterruptedAccess();
    CpuReadFailuresCannotCompleteCopies();
    ImageReadFailuresAndDerivedRollback();
    DerivedWritesCommitActualSurfacePixels();
    DerivedWriteLeaseBlocksReuseAndSurvivesDestruction();
    DerivedDefaultRefreshAndDecodedSnapshotsStayIndependent();
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
    DecodeTargetsRejectAliasesAndBusyWriters();
  } catch (const std::exception &error) {
    fprintf(stderr, "VA-API VPP regression failed: %s\n", error.what());
    return 1;
  }
  puts("VA-API VPP capabilities/identity/lifecycle/fences/images: 44 hardware-free regressions passed");
  return 0;
}
