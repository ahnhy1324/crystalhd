// SPDX-License-Identifier: LGPL-2.1-or-later
// Exercise the actual driver state machine with private memory only: no DRM
// device, firmware, browser, or CrystalHD device is opened by these tests.
#include "../filters/vaapi/crystalhd_drv_video.cpp"

#include <cerrno>
#include <stdexcept>
#include <string>

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
  bool fail_open = false;
  bool fail_create = false;
  bool signaled = false;
  bool self_wait = false;
  bool invalid = false;
  std::vector<std::string> events;
  std::unordered_set<int> started;
  std::unordered_set<int> imported;
  std::unordered_set<int> handles;

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
      signaled = true;
      return 0;
    }
    invalid = true;
    return Error();
  }

  int Close(int fd) {
    events.push_back("close:" + std::to_string(fd));
    if (fd == timeline || fd == fence)
      invalid |= handles.erase(fd) != 1;
    else
      invalid |= fd < 101 || fd > 103 || started.count(fd) != 0;
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
  } catch (const std::exception &error) {
    fprintf(stderr, "VA-API VPP regression failed: %s\n", error.what());
    return 1;
  }
  puts("VA-API VPP identity/lifecycle/fences: 15 hardware-free regressions passed");
  return 0;
}
