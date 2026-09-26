// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual public driver callbacks; GBM allocations use test-only private memfds.
// DMA synchronization is injected. No DRM or CrystalHD device is opened.
#ifndef CRYSTALHD_VAAPI_SOURCE
#define CRYSTALHD_VAAPI_SOURCE "../filters/vaapi/crystalhd_drv_video.cpp"
#endif
#include CRYSTALHD_VAAPI_SOURCE

#include <poll.h>
#include <stdexcept>
#include <string>

namespace {
struct MockBo { int fd; uint32_t pitch; size_t size; };
struct IoMock {
  bool fail_create = false, fail_fd = false, fail_map = false;
  bool fail_seek = false, short_size = false, fail_poll = false;
  bool device_destroyed = false;
  uint64_t fail_sync = UINT64_MAX;
  uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
  int plane_count = 1;
  uint32_t offset = 0;
  unsigned creates = 0, destroys = 0, sync_calls = 0, polls = 0;
  unsigned invalid = 0;
  std::unordered_set<MockBo *> bos;
  std::unordered_set<int> export_fds;
  std::unordered_map<void *, size_t> mappings;
  std::vector<uint64_t> sync_flags;
  std::function<void()> poll_hook;
  int Fail(int error = EIO) { errno = error; return -1; }
} io;
static unsigned failures;
static gbm_device *FakeGbm() { return reinterpret_cast<gbm_device *>(uintptr_t{0x1234}); }
static void Require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
static void Check(bool value, const char *message) {
  if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
static int MemoryFile(size_t size) {
  const int fd = memfd_create("crystalhd-buffer-test", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, size) != 0) std::abort();
  return fd;
}
} // namespace

extern "C" int __real_close(int);
extern "C" void *__real_mmap(void *, size_t, int, int, int, off_t);
extern "C" int __real_munmap(void *, size_t);
extern "C" off_t __real_lseek(int, off_t, int);
extern "C" int __real_poll(struct pollfd *, nfds_t, int);
extern "C" gbm_device *__wrap_gbm_create_device(int fd) {
  if (fd != 77) std::abort();
  return FakeGbm();
}
extern "C" void __wrap_gbm_device_destroy(gbm_device *device) {
  if (device != FakeGbm()) std::abort();
  io.invalid += !io.bos.empty();
  io.device_destroyed = true;
}
extern "C" gbm_bo *__wrap_gbm_bo_create(gbm_device *device, uint32_t width,
                                          uint32_t height, uint32_t format,
                                          uint32_t usage) {
  ++io.creates;
  if (device != FakeGbm() || !width || !height || format != GBM_FORMAT_R8 ||
      !(usage & GBM_BO_USE_LINEAR)) std::abort();
  if (io.fail_create) return nullptr;
  const uint32_t pitch = (width + 63U) & ~63U;
  auto *bo = new MockBo{MemoryFile(static_cast<size_t>(pitch) * height), pitch,
                         static_cast<size_t>(pitch) * height};
  io.bos.insert(bo);
  return reinterpret_cast<gbm_bo *>(bo);
}
extern "C" void __wrap_gbm_bo_destroy(gbm_bo *handle) {
  auto *bo = reinterpret_cast<MockBo *>(handle);
  io.invalid += io.device_destroyed;
  if (io.bos.erase(bo) != 1) std::abort();
  ++io.destroys;
  __real_close(bo->fd);
  delete bo;
}
extern "C" int __wrap_gbm_bo_get_fd(gbm_bo *handle) {
  if (io.fail_fd) return io.Fail();
  auto *bo = reinterpret_cast<MockBo *>(handle);
  if (!io.bos.count(bo)) std::abort();
  const int fd = fcntl(bo->fd, F_DUPFD_CLOEXEC, 0);
  if (fd >= 0) io.export_fds.insert(fd);
  return fd;
}
extern "C" int __wrap_gbm_bo_get_plane_count(gbm_bo *) { return io.plane_count; }
extern "C" uint64_t __wrap_gbm_bo_get_modifier(gbm_bo *) { return io.modifier; }
extern "C" uint32_t __wrap_gbm_bo_get_offset(gbm_bo *, int) { return io.offset; }
extern "C" uint32_t __wrap_gbm_bo_get_stride(gbm_bo *handle) {
  return reinterpret_cast<MockBo *>(handle)->pitch;
}
extern "C" void *__wrap_mmap(void *address, size_t size, int prot, int flags,
                              int fd, off_t offset) {
  if (io.fail_map && io.export_fds.count(fd)) { io.Fail(ENOMEM); return MAP_FAILED; }
  void *mapping = __real_mmap(address, size, prot, flags, fd, offset);
  if (mapping != MAP_FAILED && io.export_fds.count(fd)) io.mappings[mapping] = size;
  return mapping;
}
extern "C" int __wrap_munmap(void *address, size_t size) {
  auto found = io.mappings.find(address);
  if (found != io.mappings.end()) {
    io.invalid += found->second != size;
    io.mappings.erase(found);
  }
  return __real_munmap(address, size);
}
extern "C" off_t __wrap_lseek(int fd, off_t offset, int whence) {
  if (io.export_fds.count(fd)) {
    if (io.fail_seek) return io.Fail();
    if (io.short_size && whence == SEEK_END) return 1;
  }
  return __real_lseek(fd, offset, whence);
}
extern "C" int __wrap_close(int fd) {
  io.export_fds.erase(fd);
  return __real_close(fd);
}
extern "C" int __wrap_ioctl(int, unsigned long request, ...) {
  va_list arguments;
  va_start(arguments, request);
  void *argument = va_arg(arguments, void *);
  va_end(arguments);
  if (request != DMA_BUF_IOCTL_SYNC) std::abort();
  const auto *sync = static_cast<dma_buf_sync *>(argument);
  ++io.sync_calls;
  io.sync_flags.push_back(sync->flags);
  return sync->flags == io.fail_sync ? io.Fail() : 0;
}
extern "C" int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
  ++io.polls;
  io.invalid += count != 1 || fds[0].events != POLLOUT || timeout <= 0 || timeout > 10001;
  if (io.poll_hook) {
    auto hook = std::move(io.poll_hook);
    io.poll_hook = {};
    hook();
  }
  if (io.fail_poll) return io.Fail();
  return __real_poll(fds, count, timeout);
}
extern "C" int __wrap___poll_chk(struct pollfd *fds, nfds_t count, int timeout,
                                 size_t bytes) {
  // Fortified sanitizer builds can lower the same call to __poll_chk.
  // Preserve the bounds check and use the identical test synchronization seam.
  if (count > bytes / sizeof(*fds)) std::abort();
  return __wrap_poll(fds, count, timeout);
}
extern "C" BC_STATUS __wrap_DtsDeviceOpen(HANDLE *, uint32_t) { std::abort(); }

namespace {
struct Fixture {
  VADriverContext context = {};
  VADriverVTable table = {};
  drm_state drm = {};
  Driver *driver = nullptr;
  Fixture() {
    io = {};
    drm.fd = 77;
    context.vtable = &table;
    context.drm_state = &drm;
    Require(InitializeDriver(&context, VA_MINOR_VERSION) == VA_STATUS_SUCCESS,
            "initialize actual driver vtable with mocked GBM only");
    driver = static_cast<Driver *>(context.pDriverData);
  }
  ~Fixture() {
    table.vaTerminate(&context);
    Check(io.bos.empty() && io.export_fds.empty() && io.mappings.empty() && !io.invalid,
          "fixture cleanup releases every mock BO, exported fd and mapping");
  }
  VAImage Image(unsigned width = 17, unsigned height = 9) {
    VAImageFormat format = {};
    format.fourcc = VA_FOURCC_NV12;
    format.byte_order = VA_LSB_FIRST;
    format.bits_per_pixel = 12;
    VAImage image = {};
    Require(table.vaCreateImage(&context, &format, width, height, &image) == VA_STATUS_SUCCESS,
            "create actual NV12 image");
    auto &data = driver->buffers.at(image.buf).data;
    for (size_t i = 0; i < data.size(); ++i) data[i] = (i * 29U + 17U) & 0xff;
    return image;
  }
  VAStatus Acquire(VABufferID id, VABufferInfo *info) {
    return table.vaAcquireBufferHandle ? table.vaAcquireBufferHandle(&context, id, info)
                                       : VA_STATUS_ERROR_UNIMPLEMENTED;
  }
  VAStatus Release(VABufferID id) {
    return table.vaReleaseBufferHandle ? table.vaReleaseBufferHandle(&context, id)
                                       : VA_STATUS_ERROR_UNIMPLEMENTED;
  }
  std::shared_ptr<Surface> Picture() {
    auto surface = std::make_shared<Surface>();
    Require(surface->AllocateInternal(nullptr, -1, 18, 10, VA_FOURCC_NV12), "allocate CPU test picture");
    std::fill(surface->storage.begin(), surface->storage.end(), 73);
    surface->ready = true;
    const int fd = MemoryFile(surface->storage.size());
    void *map = mmap(nullptr, surface->storage.size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    Require(map != MAP_FAILED, "map private mock surface backing");
    memcpy(map, surface->storage.data(), surface->storage.size());
    surface->object_fds.push_back(fd);
    surface->object_maps.push_back(map);
    surface->object_sizes.push_back(surface->storage.size());
    surface->planes[0] = static_cast<uint8_t *>(map);
    surface->planes[1] = surface->planes[0] + surface->pitch[0] * surface->height;
    driver->surfaces[1] = surface;
    driver->next_surface = 2;
    return surface;
  }
};

static void Registration() {
  Fixture fixture;
  Check(fixture.table.vaAcquireBufferHandle && fixture.table.vaReleaseBufferHandle,
        "public vtable registers AcquireBufferHandle and ReleaseBufferHandle");
  const VAImage image = fixture.Image();
  VABufferInfo info = {};
  const VAStatus result = fixture.Acquire(image.buf, &info);
  Check(result == VA_STATUS_SUCCESS, "default memory hint acquires a standalone image handle");
  if (result == VA_STATUS_SUCCESS)
    Check(fixture.Release(image.buf) == VA_STATUS_SUCCESS, "release acquired image handle");
}

static void BorrowRoundTrip() {
  Fixture f;
  const VAImage image = f.Image();
  const auto original = f.driver->buffers.at(image.buf).data;
  for (uint32_t hint : {0U, uint32_t(VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME),
       uint32_t(VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)}) {
    VABufferInfo info = {};
    info.mem_type = hint;
    Require(f.Acquire(image.buf, &info) == VA_STATUS_SUCCESS, "acquire accepted default/PRIME memory hint");
    const int fd = static_cast<int>(info.handle);
    Check(info.type == VAImageBufferType && info.mem_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME &&
          info.mem_size >= image.data_size && fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC),
          "acquire reports image type, actual PRIME handle, size and close-on-exec lifetime");
    std::vector<uint8_t> exported(image.data_size);
    Require(pread(fd, exported.data(), exported.size(), 0) == static_cast<ssize_t>(exported.size()),
            "read actual exported private fd");
    Check(exported == f.driver->buffers.at(image.buf).data, "exported bytes match the entire packed image");
    std::vector<uint8_t> padding(info.mem_size - image.data_size);
    if (!padding.empty()) {
      Require(pread(fd, padding.data(), padding.size(), image.data_size) == static_cast<ssize_t>(padding.size()),
              "read allocation padding exposed by the exported fd");
      Check(std::all_of(padding.begin(), padding.end(), [](uint8_t byte) { return byte == 0; }),
            "exported allocation padding is initialized");
    }
    VABufferInfo second = {};
    Check(f.Acquire(image.buf, &second) == VA_STATUS_ERROR_SURFACE_BUSY,
          "repeated acquire rejects a second simultaneous borrow");
    void *mapped = reinterpret_cast<void *>(uintptr_t{0x123});
    Check(f.table.vaMapBuffer(&f.context, image.buf, &mapped) == VA_STATUS_ERROR_SURFACE_BUSY &&
          mapped == reinterpret_cast<void *>(uintptr_t{0x123}), "borrowed image cannot be mapped or change output pointer");
    Check(f.table.vaBufferSetNumElements(&f.context, image.buf, 1) == VA_STATUS_ERROR_SURFACE_BUSY &&
          f.driver->buffers.at(image.buf).data.size() == original.size(), "borrowed buffer cannot be resized");
    Check(f.table.vaDestroyBuffer(&f.context, image.buf) == VA_STATUS_ERROR_SURFACE_BUSY,
          "borrowed buffer cannot be destroyed");
    Check(f.table.vaDestroyImage(&f.context, image.image_id) == VA_STATUS_ERROR_SURFACE_BUSY,
          "borrowed parent image cannot be destroyed");
    exported[0] ^= 0x5a;
    Require(pwrite(fd, exported.data(), exported.size(), 0) == static_cast<ssize_t>(exported.size()),
            "external API updates actual exported storage");
    Require(f.Release(image.buf) == VA_STATUS_SUCCESS, "release commits the external image bytes");
    Check(fcntl(fd, F_GETFD) == -1 && errno == EBADF, "release invalidates its borrowed fd");
    mapped = nullptr;
    Require(f.table.vaMapBuffer(&f.context, image.buf, &mapped) == VA_STATUS_SUCCESS, "map image after release");
    Check(!std::memcmp(mapped, exported.data(), exported.size()), "external writes copy back to CPU image storage");
    Require(f.table.vaUnmapBuffer(&f.context, image.buf) == VA_STATUS_SUCCESS, "unmap released image");
    Check(f.Release(image.buf) == VA_STATUS_ERROR_INVALID_BUFFER, "release without a borrow is rejected");
  }
}

static void InvalidAndMapped() {
  Fixture f;
  const VAImage image = f.Image();
  VABufferInfo info = {};
  info.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
  Check(f.Acquire(image.buf, &info) == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE && io.creates == 0,
        "unsupported-only memory hint does not allocate a backing");
  Check(f.Acquire(VA_INVALID_ID, &info) == VA_STATUS_ERROR_INVALID_BUFFER, "reject invalid acquire buffer ID");
  Check(f.Acquire(image.buf, nullptr) != VA_STATUS_SUCCESS, "reject null buffer-info output");
  VABufferID parameter;
  Require(f.table.vaCreateBuffer(&f.context, VA_INVALID_ID, VAPictureParameterBufferType,
                                 4, 1, nullptr, &parameter) == VA_STATUS_SUCCESS, "create non-image test buffer");
  info = {};
  Check(f.Acquire(parameter, &info) == VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE,
        "non-image parameters cannot be exported as pixel storage");
  void *one = nullptr, *two = nullptr;
  Require(f.table.vaMapBuffer(&f.context, image.buf, &one) == VA_STATUS_SUCCESS &&
          f.table.vaMapBuffer(&f.context, image.buf, &two) == VA_STATUS_SUCCESS && one == two,
          "matching nested maps retain one stable CPU pointer");
  Check(f.Acquire(image.buf, &info) == VA_STATUS_ERROR_SURFACE_BUSY, "CPU mapping excludes external acquire");
  Check(f.table.vaBufferSetNumElements(&f.context, image.buf, 1) == VA_STATUS_ERROR_SURFACE_BUSY,
        "mapped buffer cannot invalidate a borrowed CPU pointer by resizing");
  Check(f.table.vaDestroyBuffer(&f.context, image.buf) == VA_STATUS_ERROR_SURFACE_BUSY &&
        f.table.vaDestroyImage(&f.context, image.image_id) == VA_STATUS_ERROR_SURFACE_BUSY,
        "mapped buffer and parent cannot be destroyed");
  Require(f.table.vaUnmapBuffer(&f.context, image.buf) == VA_STATUS_SUCCESS, "unmap first CPU reference");
  Check(f.Acquire(image.buf, &info) == VA_STATUS_ERROR_SURFACE_BUSY, "remaining CPU map still excludes acquire");
  Require(f.table.vaUnmapBuffer(&f.context, image.buf) == VA_STATUS_SUCCESS, "unmap final CPU reference");
  Require(f.Acquire(image.buf, &info) == VA_STATUS_SUCCESS && f.Release(image.buf) == VA_STATUS_SUCCESS,
          "matched unmaps restore handle-acquire availability");
}

static void SnapshotIsolation() {
  Fixture f;
  auto picture = f.Picture();
  const auto old_surface = picture->storage;
  VAImage snapshot = {};
  Require(f.table.vaDeriveImage(&f.context, 1, &snapshot) == VA_STATUS_SUCCESS, "derive the supported independent snapshot");
  VABufferInfo info = {};
  Require(f.Acquire(snapshot.buf, &info) == VA_STATUS_SUCCESS, "acquire derived snapshot handle");
  Check(f.table.vaGetImage(&f.context, 1, 0, 0, picture->width, picture->height, snapshot.image_id) ==
        VA_STATUS_ERROR_SURFACE_BUSY, "GetImage cannot mutate an externally borrowed snapshot");
  Check(f.table.vaPutImage(&f.context, 1, snapshot.image_id, 0, 0, picture->width, picture->height,
                          0, 0, picture->width, picture->height) == VA_STATUS_ERROR_SURFACE_BUSY,
        "PutImage cannot read an externally borrowed snapshot");
  const uint8_t changed = 11;
  Require(pwrite(static_cast<int>(info.handle), &changed, 1, 0) == 1, "external writer changes snapshot pixels");
  Require(f.Release(snapshot.buf) == VA_STATUS_SUCCESS, "release updated snapshot");
  Check(f.driver->buffers.at(snapshot.buf).data[0] == changed &&
        !std::memcmp(picture->planes[0], old_surface.data(), old_surface.size()) && picture->ready,
        "snapshot handle copy-back changes the image but never its source surface");
}

static void AcquireRollback() {
  for (unsigned failure = 0; failure < 10; ++failure) {
    Fixture f;
    const VAImage image = f.Image();
    const auto original = f.driver->buffers.at(image.buf).data;
    switch (failure) {
      case 0: io.fail_create = true; break;
      case 1: io.fail_fd = true; break;
      case 2: io.fail_map = true; break;
      case 3: io.fail_seek = true; break;
      case 4: io.short_size = true; break;
      case 5: io.modifier = 0x12345678; break;
      case 6: io.plane_count = 2; break;
      case 7: io.offset = 128; break;
      case 8: io.fail_sync = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE; break;
      case 9: io.fail_sync = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE; break;
    }
    VABufferInfo info = {};
    Check(f.Acquire(image.buf, &info) != VA_STATUS_SUCCESS, "failed backing preparation never returns a borrowed handle");
    Check(io.bos.empty() && io.export_fds.empty() && io.mappings.empty(),
          "failed acquisition rolls back BO/fd/map resources");
    void *data = nullptr;
    Require(f.table.vaMapBuffer(&f.context, image.buf, &data) == VA_STATUS_SUCCESS,
            "failed acquisition leaves the original image available");
    Check(!std::memcmp(data, original.data(), original.size()), "failed acquisition preserves original image bytes");
    Require(f.table.vaUnmapBuffer(&f.context, image.buf) == VA_STATUS_SUCCESS, "unmap after failed acquisition");
  }
}

static void ReleaseFailureInvalidates() {
  for (unsigned failure = 0; failure < 3; ++failure) {
    Fixture f;
    const VAImage image = f.Image();
    VABufferInfo info = {};
    Require(f.Acquire(image.buf, &info) == VA_STATUS_SUCCESS, "prepare handle for release failure");
    const int fd = static_cast<int>(info.handle);
    const uint8_t changed = 5;
    Require(pwrite(fd, &changed, 1, 0) == 1, "external write precedes injected synchronization failure");
    if (failure == 0) io.fail_poll = true;
    else io.fail_sync = (failure == 1 ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_READ;
    Check(f.Release(image.buf) != VA_STATUS_SUCCESS, "release cannot hide late wait/read-synchronization failure");
    Check(fcntl(fd, F_GETFD) == -1 && errno == EBADF && io.export_fds.empty() && io.bos.empty() && io.mappings.empty(),
          "failed release still consumes the handle and closes all export resources");
    void *data = nullptr;
    Check(f.table.vaMapBuffer(&f.context, image.buf, &data) != VA_STATUS_SUCCESS,
          "failed copy-back cannot later present stale CPU pixels as a valid image");
    VABufferInfo reacquire = {};
    Check(f.Acquire(image.buf, &reacquire) != VA_STATUS_SUCCESS,
          "a poisoned image cannot re-export stale CPU pixels");
    Check(f.table.vaDestroyImage(&f.context, image.image_id) == VA_STATUS_SUCCESS,
          "failed image remains destroyable");
  }
}

static void DeriveFailureRollback() {
  for (uint64_t flags : {uint64_t(DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ),
                         uint64_t(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ)}) {
    Fixture f;
    auto picture = f.Picture();
    const auto original = picture->storage;
    io.fail_sync = flags;
    VAImage image = {};
    image.image_id = VA_INVALID_ID;
    Check(f.table.vaDeriveImage(&f.context, 1, &image) != VA_STATUS_SUCCESS,
          "derive rejects source read synchronization failure");
    Check(f.driver->images.empty() && f.driver->buffers.empty() && image.image_id == VA_INVALID_ID &&
          !std::memcmp(picture->planes[0], original.data(), original.size()),
          "failed snapshot read rolls back image allocation and preserves the source");
  }
}

static void TerminateInvalidatesBorrow() {
  int fd = -1;
  {
    Fixture f;
    const VAImage image = f.Image();
    VABufferInfo info = {};
    Require(f.Acquire(image.buf, &info) == VA_STATUS_SUCCESS, "acquire handle left live until driver termination");
    fd = static_cast<int>(info.handle);
  }
  Check(fcntl(fd, F_GETFD) == -1 && errno == EBADF, "driver termination closes outstanding borrowed handles");
}

static void ReleaseWaitRehash() {
  Fixture f;
  const VAImage image = f.Image();
  VABufferInfo info = {};
  Require(f.Acquire(image.buf, &info) == VA_STATUS_SUCCESS, "prepare a borrow with concurrent release wait");
  bool observed = false;
  io.poll_hook = [&] {
    std::thread client([&] {
      if (!f.driver->mutex.try_lock()) {
        Check(false, "release fence wait must not hold the global driver mutex");
        return;
      }
      f.driver->mutex.unlock();
      observed = true;
      void *mapped = nullptr;
      VABufferInfo other = {};
      Check(f.Release(image.buf) == VA_STATUS_ERROR_SURFACE_BUSY &&
            f.Acquire(image.buf, &other) == VA_STATUS_ERROR_SURFACE_BUSY &&
            f.table.vaMapBuffer(&f.context, image.buf, &mapped) == VA_STATUS_ERROR_SURFACE_BUSY,
            "release-in-progress excludes competing releases, acquires and maps");
      Check(f.table.vaDestroyImage(&f.context, image.image_id) == VA_STATUS_ERROR_SURFACE_BUSY &&
            f.table.vaBufferSetNumElements(&f.context, image.buf, 1) == VA_STATUS_ERROR_SURFACE_BUSY,
            "release-in-progress retains the image allocation and extent");
      // Force unrelated unordered_map growth during the unlocked wait.
      for (unsigned count = 0; count < 256; ++count) f.Image(2, 2);
    });
    client.join();
  };
  Require(f.Release(image.buf) == VA_STATUS_SUCCESS && observed,
          "release re-finds the same borrowed image after concurrent map rehash");
  Check(io.export_fds.empty() && io.mappings.empty(), "concurrent release wait closes its original fd/map");
}

// Keep the baseline source compilable: it predates the readiness waiter field.
template<class T> static auto WaiterCount(const T &surface, int)
    -> decltype(surface.export_waiters) { return surface.export_waiters; }
template<class T> static unsigned WaiterCount(const T &, long) { return 0; }

static void PendingReadIdentity() {
  for (bool derive : {false, true}) for (unsigned completion = 0; completion < 3; ++completion) {
    Fixture f;
    Require(f.table.vaAcquireBufferHandle != nullptr, "pending read test requires the compatibility callbacks");
    auto target = f.Picture();
    auto source = std::make_shared<Surface>();
    Require(source->AllocateInternal(nullptr, -1, target->width, target->height, VA_FOURCC_NV12),
            "allocate queued conversion source");
    std::fill(source->storage.begin(), source->storage.end(), 109);
    source->ready = true;
    source->expected_timestamp = source->frame_timestamp = 7 * kTimestampStep;
    constexpr uint64_t sequence = 19;
    {
      std::lock_guard<std::mutex> lock(f.driver->mutex);
      target->ready = false;
      target->vpp_writers = 1;
      target->latest_vpp_sequence = sequence;
      source->vpp_readers = 1;
      PendingVpp pending;
      pending.source = source;
      pending.target = pending.target_owner = target;
      pending.source_timestamp = source->expected_timestamp;
      pending.sequence = sequence;
      pending.worker_active = true; // This test owns deterministic completion.
      pending.source_region = pending.output_region =
          VARectangle{0, 0, static_cast<uint16_t>(target->width), static_cast<uint16_t>(target->height)};
      f.driver->pending_vpp.push_back(pending);
    }
    VADRMPRIMESurfaceDescriptor writable = {};
    Require(f.table.vaExportSurfaceHandle(&f.context, 1, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
              VA_EXPORT_SURFACE_WRITE_ONLY, &writable) == VA_STATUS_SUCCESS,
            "write-only export remains available without waiting for readable pixels");
    for (unsigned i = 0; i < writable.num_objects; ++i) close(writable.objects[i].fd);
    VAStatus result = VA_STATUS_ERROR_OPERATION_FAILED;
    VAImage image = {};
    VADRMPRIMESurfaceDescriptor desc = {};
    std::thread reader([&] {
      result = derive ? f.table.vaDeriveImage(&f.context, 1, &image) :
          f.table.vaExportSurfaceHandle(&f.context, 1, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &desc);
    });
    bool waiting;
    {
      std::unique_lock<std::mutex> lock(f.driver->mutex);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (WaiterCount(*target, 0) == 0 && std::chrono::steady_clock::now() < deadline)
        f.driver->condition.wait_for(lock, std::chrono::milliseconds(1));
      waiting = WaiterCount(*target, 0) == 1;
      if (completion == 0 && waiting) {
        const VARectangle region = {0, 0, static_cast<uint16_t>(target->width),
                                    static_cast<uint16_t>(target->height)};
        const VAStatus copied = ProcessVpp(&f.driver->vpp_scaler, &f.driver->vpp_argb_staging,
                                           source.get(), target.get(), region, region, true);
        Check(copied == VA_STATUS_SUCCESS, "actual pending VPP copy completes");
        ReleasePendingVpp(f.driver, sequence, copied, true);
      } else {
        if (completion == 1) ++target->latest_vpp_sequence;
        if (completion == 2) { target->destroyed = true; f.driver->surfaces.erase(1); }
        ReleasePendingVpp(f.driver, sequence, VA_STATUS_ERROR_OPERATION_FAILED, false);
      }
      f.driver->condition.notify_all();
    }
    reader.join();
    Check(waiting && WaiterCount(*target, 0) == 0,
          "read/export waits for the pending operation and releases its readiness guard");
    if (completion == 0) {
      Check(result == VA_STATUS_SUCCESS && target->frame_timestamp == source->frame_timestamp,
            "pending read/export returns the completed sequence, not its initially-zero timestamp");
      if (result == VA_STATUS_SUCCESS) {
        if (derive) Check(f.driver->buffers.at(image.buf).data[0] == 109,
                          "pending derive snapshots the newly completed pixels");
        else {
          uint8_t pixel = 0;
          Check(desc.num_layers == 2 && pread(desc.objects[0].fd, &pixel, 1, 0) == 1 && pixel == 109,
                "pending default export exposes newly completed backing bytes");
        }
      }
    } else {
      Check(result != VA_STATUS_SUCCESS, "read/export rejects a replaced or destroyed pending identity");
      Check(f.driver->images.empty() && f.driver->buffers.empty(), "failed pending derive does not leak an image");
    }
    if (!derive && result == VA_STATUS_SUCCESS)
      for (unsigned i = 0; i < desc.num_objects; ++i) close(desc.objects[i].fd);
  }
}

static void ExportLayouts() {
  Fixture fixture;
  auto picture = fixture.Picture();
  for (const uint32_t flags : {VA_EXPORT_SURFACE_READ_ONLY,
       VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
       VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS}) {
    VADRMPRIMESurfaceDescriptor desc = {};
    Require(fixture.table.vaExportSurfaceHandle(&fixture.context, 1,
              VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, flags, &desc) == VA_STATUS_SUCCESS,
            "export existing backing through the public callback");
    const bool composed = flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS;
    Check(desc.num_layers == (composed ? 1U : 2U),
          "default and explicit-separate NV12 exports use two layers; composed uses one");
    Check(desc.num_objects == 1 && desc.objects[0].fd >= 0 &&
          desc.objects[0].fd != picture->object_fds[0], "surface export returns an independently owned fd");
    if (desc.num_layers == (composed ? 1U : 2U)) {
      Check(desc.layers[0].drm_format == (composed ? DRM_FORMAT_NV12 : DRM_FORMAT_R8),
            "export layer format matches its requested composition");
      const auto &uv = desc.layers[composed ? 0 : 1];
      Check(uv.pitch[composed ? 1 : 0] == picture->pitch[1] &&
            uv.offset[composed ? 1 : 0] == picture->pitch[0] * picture->height,
            "NV12 export retains chroma pitch and byte offset");
    }
    for (unsigned object = 0; object < desc.num_objects; ++object) close(desc.objects[object].fd);
  }
  for (uint32_t flags : {uint32_t(VA_EXPORT_SURFACE_SEPARATE_LAYERS | VA_EXPORT_SURFACE_COMPOSED_LAYERS),
                        uint32_t(0x80000000)}) {
    VADRMPRIMESurfaceDescriptor desc = {};
    Check(fixture.table.vaExportSurfaceHandle(&fixture.context, 1,
              VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, flags, &desc) == VA_STATUS_ERROR_INVALID_PARAMETER &&
          desc.num_objects == 0, "invalid export flags fail before creating output descriptors");
  }
}
} // namespace

int main() {
  for (auto test : {Registration, ExportLayouts, BorrowRoundTrip, InvalidAndMapped,
                    SnapshotIsolation, AcquireRollback, ReleaseFailureInvalidates,
                    DeriveFailureRollback, TerminateInvalidatesBorrow,
                    ReleaseWaitRehash, PendingReadIdentity}) {
    try { test(); }
    catch (const std::exception &error) { Check(false, error.what()); }
  }
  std::printf("VAAPI buffer handles: %s (%u failed checks)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
