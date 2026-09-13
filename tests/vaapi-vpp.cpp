// SPDX-License-Identifier: LGPL-2.1-or-later
// Exercise the actual driver state machine with private memory only: no DRM
// device, firmware, browser, or CrystalHD device is opened by these tests.
#include "../filters/vaapi/crystalhd_drv_video.cpp"

#include <cerrno>
#include <stdexcept>
#include <string>

extern "C" int __real_close(int fd);

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
      if (fd == timeline && !imported.empty())
        signaled = true;
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

}  // namespace

extern "C" int __real_open(const char *path, int flags, ...);
extern "C" int __real_ioctl(int fd, unsigned long request, ...);
extern "C" int __real_close(int fd);

extern "C" int __wrap_open(const char *path, int flags, ...) {
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
  return fence_io_mock != nullptr
             ? fence_io_mock->Ioctl(fd, request, argument)
             : __real_ioctl(fd, request, argument);
}

extern "C" int __wrap_close(int fd) {
  return fence_io_mock != nullptr ? fence_io_mock->Close(fd)
                                  : __real_close(fd);
}

namespace {

void Require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
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

void PartialFenceImportSignalsBeforeCpuEnd() {
  for (int fail_fd : {101, 102, 103}) {
    FenceIoMock mock;
    MockFenceScope scope(mock);
    Surface surface;
    surface.object_fds = {101, 102, 103};
    mock.fail_import_fd = fail_fd;
    Require(BeginFencedWrite(&surface) == -1,
            "partial fence import must not acknowledge an async writer");
    const auto signal = std::find(mock.events.begin(), mock.events.end(),
                                  "signal");
    const auto end = std::find(mock.events.begin(), mock.events.end(), "end:101");
    Require(signal != mock.events.end() && signal < end,
            "partially imported fence must signal before any CPU END");
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
              !mock.self_wait && !mock.invalid,
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
                                VA_EXPORT_SURFACE_READ_ONLY, &prime) == VA_STATUS_SUCCESS,
            "export private backing before original surface destruction");
    mock.real_files.insert(prime.objects[0].fd);
    fixture.decoder->decoded_frames.at(kTimestampStep)->ready = false;
    fixture.SubmitParameters();
    Require(EndPicture(&fixture.context, 2) == VA_STATUS_SUCCESS,
            "queue fenced VPP on exported backing");
    VASurfaceID owner = 2;
    Require(DestroySurfaces(&fixture.context, &owner, 1) == VA_STATUS_SUCCESS,
            "retire original owner with pending fence");
    unsigned long handle = prime.objects[0].fd;
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
    unsigned long handle = fd;
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
              !mock.invalid,
          "partial import plus failed signal must close the timeline before END");
}

}  // namespace

int main() {
  try {
    RepeatedVppRetainsPicture();
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
    PartialFenceImportSignalsBeforeCpuEnd();
    CompletedFenceSignalsBeforeCpuEnd();
    VppCpuOwnershipFailuresAreNotSuccessfulFrames();
    CpuReadStartUnwindsAcquiredPrefix();
    CpuSyncRetriesInterruptedAccess();
    CpuReadFailuresCannotCompleteCopies();
    ImageReadFailuresAndDerivedRollback();
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
  puts("VA-API VPP identity/lifecycle/fences/images: 30 hardware-free regressions passed");
  return 0;
}
