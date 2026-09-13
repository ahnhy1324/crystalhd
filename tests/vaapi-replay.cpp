// SPDX-License-Identifier: LGPL-2.1-or-later
// Production replay state and output processing with private CPU memory only.
// No device, decoder, capture, firmware, or DRM operation is started.
#include "../filters/vaapi/crystalhd_drv_video.cpp"
#include <stdexcept>
#include <string>

static int mock_sync_fd = -1;
static uint64_t mock_failed_sync_flags = 0;
extern "C" int __real_ioctl(int fd, unsigned long request, ...);
extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
  va_list arguments;
  va_start(arguments, request);
  void *argument = va_arg(arguments, void *);
  va_end(arguments);
  if (fd == mock_sync_fd && request == DMA_BUF_IOCTL_SYNC) {
    const auto *sync = static_cast<dma_buf_sync *>(argument);
    if (sync->flags == mock_failed_sync_flags) {
      errno = EIO;
      return -1;
    }
    return 0;
  }
  return __real_ioctl(fd, request, argument);
}

static void Require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

// These wrappers drive the real submission/drain/teardown state machine with
// deterministic output, without a device or firmware. A sleep callback models
// another API call while the production wait has released the driver mutex.
struct DecodeIoMock {
  std::vector<uint64_t> inputs;
  std::deque<uint64_t> outputs;
  std::vector<uint8_t> pixels;
  std::vector<std::string> events;
  std::function<void()> on_sleep;
  bool early_eos = false;
  bool fail_poll = false;
  bool invalid = false;
  unsigned int open_attempts = 0;
  HANDLE handle() { return static_cast<HANDLE>(this); }
  static uint8_t Luma(uint64_t timestamp) {
    return static_cast<uint8_t>(40 + timestamp / kTimestampStep * 10);
  }
};

static thread_local DecodeIoMock *decode_io_mock = nullptr;
struct MockDecodeScope {
  explicit MockDecodeScope(DecodeIoMock &mock) { decode_io_mock = &mock; }
  ~MockDecodeScope() { decode_io_mock = nullptr; }
};

extern "C" BC_STATUS __real_DtsDeviceOpen(HANDLE *, uint32_t);
extern "C" BC_STATUS __wrap_DtsDeviceOpen(HANDLE *device, uint32_t mode) {
  if (decode_io_mock == nullptr)
    return __real_DtsDeviceOpen(device, mode);
  ++decode_io_mock->open_attempts;
  return BC_STS_ERROR;  // An unexpected reopen must never touch real hardware.
}

#define MOCK_CLOSE_OPERATION(name, event) \
  extern "C" BC_STATUS __real_##name(HANDLE); \
  extern "C" BC_STATUS __wrap_##name(HANDLE device) { \
    if (decode_io_mock == nullptr) \
      return __real_##name(device); \
    decode_io_mock->invalid |= device != decode_io_mock->handle(); \
    decode_io_mock->events.push_back(event); \
    return BC_STS_SUCCESS; \
  }
MOCK_CLOSE_OPERATION(DtsStopDecoder, "stop")
MOCK_CLOSE_OPERATION(DtsCloseDecoder, "close-decoder")
MOCK_CLOSE_OPERATION(DtsDeviceClose, "close-device")
#undef MOCK_CLOSE_OPERATION

extern "C" BC_STATUS __real_DtsGetDriverStatus(HANDLE, BC_DTS_STATUS *);
extern "C" BC_STATUS __wrap_DtsGetDriverStatus(HANDLE device, BC_DTS_STATUS *status) {
  if (decode_io_mock == nullptr)
    return __real_DtsGetDriverStatus(device, status);
  decode_io_mock->invalid |= device != decode_io_mock->handle();
  *status = {};
  status->ReadyListCount = decode_io_mock->outputs.size();
  return decode_io_mock->fail_poll ? BC_STS_ERROR : BC_STS_SUCCESS;
}

extern "C" uint32_t __real_DtsTxFreeSize(HANDLE);
extern "C" uint32_t __wrap_DtsTxFreeSize(HANDLE device) {
  if (decode_io_mock == nullptr)
    return __real_DtsTxFreeSize(device);
  decode_io_mock->invalid |= device != decode_io_mock->handle();
  return 1024 * 1024;
}

extern "C" BC_STATUS __real_DtsProcInput(HANDLE, uint8_t *, uint32_t, uint64_t, BOOL);
extern "C" BC_STATUS __wrap_DtsProcInput(HANDLE device, uint8_t *data,
                                         uint32_t size, uint64_t timestamp,
                                         BOOL encrypted) {
  if (decode_io_mock == nullptr)
    return __real_DtsProcInput(device, data, size, timestamp, encrypted);
  decode_io_mock->invalid |= device != decode_io_mock->handle() ||
                             data == nullptr || size == 0 || encrypted;
  decode_io_mock->inputs.push_back(timestamp);
  decode_io_mock->events.push_back("input");
  return BC_STS_SUCCESS;
}

extern "C" BC_STATUS __real_DtsFlushInput(HANDLE, uint32_t);
extern "C" BC_STATUS __wrap_DtsFlushInput(HANDLE device, uint32_t mode) {
  if (decode_io_mock == nullptr)
    return __real_DtsFlushInput(device, mode);
  decode_io_mock->invalid |= device != decode_io_mock->handle() || mode != 0;
  decode_io_mock->events.push_back("seal");
  if (!decode_io_mock->early_eos)
    for (uint64_t timestamp : decode_io_mock->inputs)
      decode_io_mock->outputs.push_back(timestamp);
  decode_io_mock->outputs.push_back(0);  // Actual simulated firmware EOS marker.
  return BC_STS_SUCCESS;
}

extern "C" BC_STATUS __real_DtsProcOutputNoCopy(HANDLE, uint32_t, BC_DTS_PROC_OUT *);
extern "C" BC_STATUS __wrap_DtsProcOutputNoCopy(HANDLE device, uint32_t timeout,
                                                BC_DTS_PROC_OUT *output) {
  if (decode_io_mock == nullptr)
    return __real_DtsProcOutputNoCopy(device, timeout, output);
  decode_io_mock->invalid |= device != decode_io_mock->handle() || timeout != 0;
  *output = {};
  if (decode_io_mock->outputs.empty())
    return BC_STS_NO_DATA;
  const uint64_t timestamp = decode_io_mock->outputs.front();
  decode_io_mock->outputs.pop_front();
  if (timestamp == 0) {
    output->PicInfo.flags = VDEC_FLAG_EOS;
    decode_io_mock->events.push_back("eos");
  } else {
    output->PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    output->PicInfo.timeStamp = timestamp;
    output->PicInfo.width = 16;
    output->PicInfo.height = 16;
    decode_io_mock->pixels.resize(16 * 16 * 2);
    for (size_t offset = 0; offset < decode_io_mock->pixels.size(); offset += 2) {
      decode_io_mock->pixels[offset] = DecodeIoMock::Luma(timestamp);
      decode_io_mock->pixels[offset + 1] = 128;
    }
    output->Ybuff = decode_io_mock->pixels.data();
    output->YBuffDoneSz = decode_io_mock->pixels.size() / 4;
    decode_io_mock->events.push_back("picture");
  }
  return BC_STS_SUCCESS;
}

extern "C" BC_STATUS __real_DtsReleaseOutputBuffs(HANDLE, PVOID, BOOL);
extern "C" BC_STATUS __wrap_DtsReleaseOutputBuffs(HANDLE device, PVOID reserved,
                                                 BOOL change) {
  if (decode_io_mock == nullptr)
    return __real_DtsReleaseOutputBuffs(device, reserved, change);
  decode_io_mock->invalid |= device != decode_io_mock->handle() ||
                             reserved != nullptr || change;
  decode_io_mock->events.push_back("release");
  return BC_STS_SUCCESS;
}

extern "C" int __real_usleep(useconds_t);
extern "C" int __wrap_usleep(useconds_t duration) {
  if (decode_io_mock != nullptr && decode_io_mock->on_sleep) {
    auto callback = std::move(decode_io_mock->on_sleep);
    decode_io_mock->on_sleep = {};
    callback();
    return 0;
  }
  return __real_usleep(duration);
}

static void SendNext(CrystalHDDecodeReplay *replay, uint64_t timestamp) {
  const auto *unit = replay->NextInput();
  Require(unit != nullptr && unit->timestamp == timestamp, "original input order");
  Require(replay->InputSent(), "record hardware input");
}

static void SealedBatchQueuesAndReplays() {
  CrystalHDDecodeReplay replay;
  const std::vector<uint8_t> idr = {0, 0, 1, 0x65, 0x80};
  const std::vector<uint8_t> predicted = {0, 0, 1, 0x41, 0x80};
  Require(replay.Append(1, true, idr), "append IDR");
  Require(replay.Append(2, false, predicted), "append P picture");
  SendNext(&replay, 1);
  SendNext(&replay, 2);
  Require(replay.Observe(1) == CrystalHDDecodeReplay::Output::New, "first picture");
  Require(replay.Seal(), "seal exact input prefix");
  Require(replay.Append(3, false, predicted), "queue input during EOS");
  Require(replay.NextInput() == nullptr, "never append input after EOS");
  Require(!replay.NeedsRestart(), "cannot restart before sealed outputs drain");
  Require(replay.Observe(2) == CrystalHDDecodeReplay::Output::New, "tail picture");
  Require(replay.EndOfSequence(), "actual EOS completes sealed batch");
  Require(replay.NeedsRestart() && replay.Restarted(), "restart for queued input");
  Require(replay.NextInput()->bytes == idr, "replay original IDR bytes");
  SendNext(&replay, 1);
  Require(replay.Observe(1) == CrystalHDDecodeReplay::Output::Duplicate,
          "replayed IDR is not a second public output");
  Require(replay.NextInput()->bytes == predicted, "replay original P bytes");
  SendNext(&replay, 2);
  Require(replay.Observe(2) == CrystalHDDecodeReplay::Output::Duplicate,
          "replayed reference is not a second public output");
  SendNext(&replay, 3);
  Require(replay.Seal(), "seal second batch");
  Require(replay.Observe(3) == CrystalHDDecodeReplay::Output::New, "queued picture");
  Require(replay.EndOfSequence(), "second batch drains");
  Require(!replay.NeedsRestart(), "no gratuitous replay without new input");
}

static void RejectIncompleteAndUnboundedReplay() {
  CrystalHDDecodeReplay missing_idr;
  Require(!missing_idr.Append(1, false, {1}), "cannot reconstruct missing references");
  CrystalHDDecodeReplay early_eos;
  Require(early_eos.Append(1, true, {1}), "append early-EOS test");
  SendNext(&early_eos, 1);
  Require(early_eos.Seal(), "seal early-EOS test");
  Require(!early_eos.EndOfSequence() && early_eos.failed(),
          "EOS cannot pass with a missing timestamp");
  CrystalHDDecodeReplay unsubmitted;
  Require(unsubmitted.Append(1, true, {1}), "append unsubmitted test");
  Require(unsubmitted.Observe(1) == CrystalHDDecodeReplay::Output::Invalid,
          "never accept output not sent to hardware");

  CrystalHDDecodeReplay::Limits limits;
  limits.picture_bytes = 2;
  limits.cache_bytes = 4;
  limits.pictures = 2;
  limits.replay_pictures = 1;
  CrystalHDDecodeReplay oversize(limits);
  Require(!oversize.Append(1, true, {1, 2, 3}), "bound individual AU bytes");
  CrystalHDDecodeReplay full(limits);
  Require(full.Append(1, true, {1, 2}) && full.Append(2, false, {1, 2}),
          "fill bounded cache");
  Require(!full.Append(3, false, {1}), "bound cache bytes and pictures");

  limits.pictures = 4;
  limits.cache_bytes = 8;
  CrystalHDDecodeReplay work(limits);
  Require(work.Append(1, true, {1}) && work.Append(2, false, {2}), "cache work test");
  SendNext(&work, 1);
  SendNext(&work, 2);
  Require(work.Seal(), "seal work test");
  work.Observe(1);
  work.Observe(2);
  Require(work.EndOfSequence() && work.Append(3, false, {3}) && work.Restarted(),
          "restart work test");
  SendNext(&work, 1);
  Require(work.NextInput() == nullptr && work.failed(), "bound repeated decode work");
}

static void ReclaimOnlyCompletedIdrPrefixes() {
  CrystalHDDecodeReplay replay;
  Require(replay.Append(1, true, {1}) && replay.Append(2, false, {2}) &&
              replay.Append(3, true, {3}), "two IDR epochs");
  SendNext(&replay, 1);
  SendNext(&replay, 2);
  SendNext(&replay, 3);
  replay.Observe(1);
  Require(replay.cached_pictures() == 3, "retain prefix with pending old output");
  replay.Observe(2);
  Require(replay.cached_pictures() == 1 && replay.outstanding() == 1,
          "reclaim completed prefix at an actual IDR only");

  CrystalHDDecodeReplay reordered;
  for (uint64_t timestamp = 1; timestamp <= 4; ++timestamp) {
    Require(reordered.Append(timestamp, timestamp == 1 || timestamp == 4, {1}),
            "append reordered display test");
    SendNext(&reordered, timestamp);
  }
  reordered.Observe(1);
  reordered.Observe(3);  // A B picture is still waiting behind a later P picture.
  reordered.Observe(4);
  Require(reordered.cached_pictures() == 4, "new IDR cannot retire missing B picture");
  reordered.Observe(2);
  Require(reordered.cached_pictures() == 1 && reordered.outstanding() == 0,
          "reclaim reordered prefix only after its last real picture");

  CrystalHDDecodeReplay sealed;
  for (uint64_t timestamp = 1; timestamp <= 3; ++timestamp) {
    Require(sealed.Append(timestamp, timestamp == 1 || timestamp == 3, {1}),
            "append sealed-only completion test");
    SendNext(&sealed, timestamp);
  }
  Require(sealed.Seal(), "seal before all outputs");
  sealed.Observe(1);
  sealed.Observe(2);
  sealed.Observe(3);
  Require(sealed.EndOfSequence() && sealed.Append(4, false, {1}) &&
              sealed.Restarted(), "restart after sealed-only completions");
  Require(sealed.cached_pictures() == 2 && sealed.NextInput()->timestamp == 3,
          "restart reclaims old prefixes and begins at latest actual IDR");
}

static void UnknownOutputNeverGuessesAndInvalidOutputFails() {
  auto exercise = [](bool sealed, bool multiple, bool valid_pixels) {
    Driver driver{-1};
    DecodeContext decoder;
    decoder.width = 16;
    decoder.height = 16;
    for (uint64_t timestamp = 1; timestamp <= (multiple ? 3U : 2U); ++timestamp) {
      auto frame = std::make_shared<Surface>();
      Require(frame->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
              "allocate ordinal-test picture");
      frame->expected_timestamp = timestamp;
      auto public_frame = std::make_shared<Surface>();
      Require(public_frame->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
              "allocate ordinal-test public surface");
      public_frame->expected_timestamp = timestamp;
      driver.surfaces[timestamp] = public_frame;
      decoder.decoded_frames[timestamp] = frame;
      decoder.pending[timestamp] = timestamp;
      Require(decoder.replay.Append(timestamp, timestamp == 1, {1}),
              "append ordinal-test input");
      SendNext(&decoder.replay, timestamp);
    }
    if (sealed)
      Require(decoder.replay.Seal(), "seal ordinal-test batch");
    std::vector<uint8_t> pixels(16 * 16 * 2, 80);
    BC_DTS_PROC_OUT output = {};
    output.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    output.PicInfo.width = 16;
    output.PicInfo.height = 16;
    output.PicInfo.timeStamp = 1;
    output.PicInfo.picture_number = 10;
    output.PicInfo.sess_num = 5;
    output.Ybuff = pixels.data();
    output.YBuffDoneSz = pixels.size() / 4;
    Require(ProcessDecodedOutput(&driver, &decoder, output) == VA_STATUS_SUCCESS,
            "complete timestamped ordinal anchor");
    output.PicInfo.timeStamp = valid_pixels ? 0 : 2;
    output.PicInfo.picture_number = 11;
    if (!valid_pixels)
      output.YBuffDoneSz = 0;
    const VAStatus result = ProcessDecodedOutput(&driver, &decoder, output);
    const auto &tail = driver.surfaces[2];
    if (!valid_pixels) {
      Require(result == VA_STATUS_ERROR_DECODING_ERROR && tail->failed &&
                  !tail->ready && decoder.replay.failed(),
              "invalid genuine output remains a failed picture");
      output.YBuffDoneSz = pixels.size() / 4;
      Require(ProcessDecodedOutput(&driver, &decoder, output) ==
                  VA_STATUS_ERROR_DECODING_ERROR && !tail->ready,
              "later repeated output cannot recover failed decode silently");
    } else {
      Require(result == VA_STATUS_SUCCESS && !tail->ready &&
                  decoder.pending.count(2) == 1,
              "untimestamped output cannot complete a requested picture");
    }
  };
  exercise(true, false, true);  // Even a sole outstanding picture needs identity.
  exercise(false, false, true);
  exercise(true, true, true);
  exercise(true, false, false);
}

static void DuplicateOutputCannotMutateOrRetirePixels() {
  Driver driver{-1};
  DecodeContext decoder;
  decoder.width = 16;
  decoder.height = 16;
  auto private_frame = std::make_shared<Surface>();
  auto public_frame = std::make_shared<Surface>();
  for (const auto &frame : {private_frame, public_frame}) {
    Require(frame->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
            "allocate private test memory");
    frame->expected_timestamp = 1;
  }
  decoder.decoded_frames[1] = private_frame;
  decoder.pending[1] = 1;
  driver.surfaces[1] = public_frame;
  Require(decoder.replay.Append(1, true, {1}), "append pixel test");
  SendNext(&decoder.replay, 1);
  Require(decoder.replay.Seal(), "seal pixel test");
  std::vector<uint8_t> yuy2(16 * 16 * 2, 128);
  for (size_t byte = 0; byte < yuy2.size(); byte += 2)
    yuy2[byte] = 40;
  BC_DTS_PROC_OUT output = {};
  output.Ybuff = yuy2.data();
  output.YBuffDoneSz = yuy2.size() / 4;
  output.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
  output.PicInfo.timeStamp = 1;
  output.PicInfo.width = 16;
  output.PicInfo.height = 16;
  Require(ProcessDecodedOutput(&driver, &decoder, output) == VA_STATUS_SUCCESS,
          "complete first genuine output");
  Require(private_frame->ready && public_frame->ready && decoder.pending.empty(),
          "complete public picture once");
  const uint64_t generation = decoder.generation;
  Require(decoder.CloseHardware() == BC_STS_SUCCESS &&
              decoder.generation == generation && decoder.decoded_frames.count(1) == 1 &&
              decoder.replay.cached_pictures() == 1,
          "hardware-only close preserves client generation and owned pictures");
  Require(decoder.replay.EndOfSequence() &&
              decoder.replay.Append(2, false, {2}) && decoder.replay.Restarted(),
          "replay pixel test");
  SendNext(&decoder.replay, 1);
  for (size_t byte = 0; byte < yuy2.size(); byte += 2)
    yuy2[byte] = 90;  // A deliberately different duplicate must never be copied.
  Require(ProcessDecodedOutput(&driver, &decoder, output) == VA_STATUS_SUCCESS,
          "ignore replay output");
  Require(decoder.decoded_frames.count(1) == 1 && private_frame->planes[0][0] == 40 &&
              public_frame->planes[0][0] == 40 && private_frame->frame_timestamp == 1,
          "duplicate neither mutates nor retires immutable current picture");
}

struct OutputFixture {
  Driver driver{-1};
  DecodeContext decoder;
  std::shared_ptr<Surface> private_frame = std::make_shared<Surface>();
  std::shared_ptr<Surface> public_frame = std::make_shared<Surface>();
  std::vector<uint8_t> pixels;
  BC_DTS_PROC_OUT output = {};

  OutputFixture(unsigned int target_width = 16, unsigned int target_height = 16,
                unsigned int output_width = 16, unsigned int output_height = 16)
      : pixels(output_width * output_height * 2, 128) {
    decoder.width = 16;
    decoder.height = 16;
    for (const auto &frame : {private_frame, public_frame}) {
      Require(frame->AllocateInternal(nullptr, -1, target_width, target_height,
                                      VA_FOURCC_NV12), "allocate output fixture");
      frame->expected_timestamp = 1;
      memset(frame->planes[0], 99, frame->storage.size());
    }
    driver.surfaces[1] = public_frame;
    decoder.decoded_frames[1] = private_frame;
    decoder.pending[1] = 1;
    Require(decoder.replay.Append(1, true, {1}), "append output fixture");
    SendNext(&decoder.replay, 1);
    for (size_t byte = 0; byte < pixels.size(); byte += 2)
      pixels[byte] = 40;
    output.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    output.PicInfo.timeStamp = 1;
    output.PicInfo.width = output_width;
    output.PicInfo.height = output_height;
    output.Ybuff = pixels.data();
    output.YBuffDoneSz = pixels.size() / 4;
  }

  VAStatus Process() { return ProcessDecodedOutput(&driver, &decoder, output); }
};

static void RejectIncompleteGeometryAndInitializeAllocationPadding() {
  for (const auto &size : {std::pair<unsigned int, unsigned int>{32, 16},
                          {16, 32}, {15, 16}, {16, 15}}) {
    OutputFixture fixture(32, 32, size.first, size.second);
    Require(fixture.Process() == VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.public_frame->failed && !fixture.public_frame->ready &&
                fixture.public_frame->planes[0][0] == 99,
            "mismatched coded output cannot copy or complete a surface");
  }
  for (const auto &size : {std::pair<unsigned int, unsigned int>{8, 16}, {16, 8}}) {
    OutputFixture fixture(size.first, size.second);
    Require(fixture.Process() == VA_STATUS_ERROR_DECODING_ERROR &&
                !fixture.public_frame->ready,
            "undersized destination cannot claim a complete picture");
  }
  OutputFixture larger(32, 32);
  Require(larger.Process() == VA_STATUS_SUCCESS && larger.public_frame->ready &&
              larger.public_frame->planes[0][0] == 40 &&
              larger.public_frame->planes[0][16] == 16 &&
              larger.public_frame->planes[0][16 * larger.public_frame->pitch[0]] == 16 &&
              larger.public_frame->planes[1][16] == 128,
          "larger allocation contains exact coded pixels and initialized padding");
}

static void FailedCpuOwnershipNeverCompletesDecode() {
  for (bool private_target : {false, true}) {
    for (uint64_t boundary : {uint64_t(DMA_BUF_SYNC_START), uint64_t(DMA_BUF_SYNC_END)}) {
      OutputFixture fixture;
      const auto &target = private_target ? fixture.private_frame : fixture.public_frame;
      mock_sync_fd = memfd_create("crystalhd-replay-cpu-test", MFD_CLOEXEC);
      Require(mock_sync_fd >= 0, "create private mock DMA ownership fd");
      target->object_fds.push_back(mock_sync_fd);
      mock_failed_sync_flags = boundary | DMA_BUF_SYNC_WRITE;
      Require(fixture.Process() == VA_STATUS_ERROR_DECODING_ERROR &&
                  fixture.public_frame->failed && !fixture.public_frame->ready &&
                  fixture.decoder.replay.failed(),
              "failed CPU ownership cannot complete a public picture");
      if (boundary == DMA_BUF_SYNC_START)
        Require(target->planes[0][0] == 99, "failed START must not write any pixels");
      mock_sync_fd = -1;
    }
  }
}

static void InitializeCopyTestSurface(Surface *surface, unsigned int width,
                                      unsigned int height, unsigned int offset) {
  surface->width = width;
  surface->height = height;
  surface->fourcc = VA_FOURCC_NV12;
  surface->pitch[0] = width + 13;
  surface->pitch[1] = Align(width, 2) + 17;
  const size_t luma_bytes = static_cast<size_t>(surface->pitch[0]) * height;
  const size_t chroma_bytes =
      static_cast<size_t>(surface->pitch[1]) * ((height + 1) / 2);
  // Unequal, unaligned strides and guard gaps expose writes outside the
  // logical rectangle as well as stale bytes inside larger destinations.
  surface->storage.assign(offset + 32 + luma_bytes + 31 + chroma_bytes + 32, 0xa5);
  surface->planes[0] = surface->storage.data() + offset + 32;
  surface->planes[1] = surface->planes[0] + luma_bytes + 31;
}

static void FillCopyTestBytes(std::vector<uint8_t> *bytes) {
  uint32_t state = 0x83abc451U;
  for (uint8_t &byte : *bytes) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    byte = static_cast<uint8_t>(state);
  }
}

static void ReferenceFullNv12Clear(Surface *surface) {
  // Deliberately retain the old full-clear algorithm as the byte oracle.
  for (unsigned int row = 0; row < surface->height; ++row)
    memset(surface->planes[0] + static_cast<size_t>(row) * surface->pitch[0],
           16, surface->width);
  for (unsigned int row = 0; row < (surface->height + 1) / 2; ++row)
    memset(surface->planes[1] + static_cast<size_t>(row) * surface->pitch[1],
           128, Align(surface->width, 2));
}

static void PaddingOnlyCopiesMatchFullClear(unsigned int source_width,
                                            unsigned int source_height,
                                            unsigned int target_width,
                                            unsigned int target_height,
                                            bool is_70012) {
  const unsigned int offset = (source_width + source_height) % 4;
  Surface expected, actual, source;
  InitializeCopyTestSurface(&expected, target_width, target_height, offset);
  InitializeCopyTestSurface(&actual, target_width, target_height, offset);
  const unsigned int width = std::min(source_width, target_width);
  const unsigned int height = std::min(source_height, target_height);
  const unsigned int source_pitch = is_70012
      ? (source_width <= 720 ? 720 : source_width <= 1280 ? 1280 : 1920) * 2
      : source_width * 2;
  std::vector<uint8_t> yuy2(static_cast<size_t>(source_pitch) * source_height + 64);
  FillCopyTestBytes(&yuy2);
  const auto original_yuy2 = yuy2;
  BC_DTS_PROC_OUT output = {};
  output.Ybuff = yuy2.data() + offset;
  output.PicInfo.width = source_width;
  output.PicInfo.height = source_height;
  output.PicInfo.timeStamp = 12345;

  ReferenceFullNv12Clear(&expected);
  for (unsigned int row = 0; row < height; ++row)
    for (unsigned int column = 0; column < width; ++column)
      expected.planes[0][static_cast<size_t>(row) * expected.pitch[0] + column] =
          output.Ybuff[static_cast<size_t>(row) * source_pitch + column * 2];
  for (unsigned int row = 0; row < height; row += 2) {
    const uint8_t *top = output.Ybuff + static_cast<size_t>(row) * source_pitch;
    const uint8_t *bottom = output.Ybuff +
        static_cast<size_t>(std::min(row + 1, height - 1)) * source_pitch;
    for (unsigned int column = 0; column + 1 < width; column += 2) {
      uint8_t *pair = expected.planes[1] +
          static_cast<size_t>(row / 2) * expected.pitch[1] + column;
      pair[0] = (static_cast<unsigned int>(top[column * 2 + 1]) +
                 bottom[column * 2 + 1] + 1) / 2;
      pair[1] = (static_cast<unsigned int>(top[column * 2 + 3]) +
                 bottom[column * 2 + 3] + 1) / 2;
    }
  }
  Require(CopyYuy2ToSurface(&actual, output, is_70012) && actual.ready &&
              actual.frame_timestamp == output.PicInfo.timeStamp &&
              actual.storage == expected.storage && yuy2 == original_yuy2,
          "production YUY2 conversion must match full-clear bytes and guards");

  InitializeCopyTestSurface(&source, source_width, source_height, (offset + 1) % 4);
  FillCopyTestBytes(&source.storage);
  source.frame_timestamp = 54321;
  const auto original_source = source.storage;
  std::fill(actual.storage.begin(), actual.storage.end(), 0xa5);
  std::fill(expected.storage.begin(), expected.storage.end(), 0xa5);
  actual.ready = false;
  actual.failed = true;
  ReferenceFullNv12Clear(&expected);
  for (unsigned int plane = 0; plane < 2; ++plane) {
    const unsigned int rows = plane == 0 ? height : (height + 1) / 2;
    for (unsigned int row = 0; row < rows; ++row)
      memcpy(expected.planes[plane] + static_cast<size_t>(row) * expected.pitch[plane],
             source.planes[plane] + static_cast<size_t>(row) * source.pitch[plane], width);
  }
  Require(CopyNv12Surface(source, &actual) && actual.ready && !actual.failed &&
              actual.frame_timestamp == source.frame_timestamp &&
              actual.storage == expected.storage && source.storage == original_source,
          "production NV12 copy must match full-clear bytes and guards");
}

static void PaddingOnlyCopyByteEquivalence() {
  for (unsigned int width : {1U, 2U, 3U, 7U, 15U, 16U, 17U, 31U, 32U,
                             33U, 63U, 64U, 65U}) {
    for (unsigned int height : {1U, 2U, 3U, 17U}) {
      for (bool link : {false, true}) {
        PaddingOnlyCopiesMatchFullClear(width, height, width, height, link);
        PaddingOnlyCopiesMatchFullClear(width, height, width + 3, height + 3, link);
        PaddingOnlyCopiesMatchFullClear(width, height, std::max(1U, width - 1),
                                        std::max(1U, height - 1), link);
      }
    }
  }
  PaddingOnlyCopiesMatchFullClear(1920, 1088, 1920, 1088, false);
  PaddingOnlyCopiesMatchFullClear(1920, 1088, 1928, 1091, false);
  PaddingOnlyCopiesMatchFullClear(1920, 1088, 1919, 1087, false);
  PaddingOnlyCopiesMatchFullClear(1920, 1088, 1920, 1088, true);
}

struct TeardownFixture {
  DecodeIoMock io;
  MockDecodeScope scope{io};
  Driver driver{-1};
  VADriverContext context = {};
  std::shared_ptr<DecodeContext> decoder = std::make_shared<DecodeContext>();
  std::vector<std::shared_ptr<Surface>> held;

  explicit TeardownFixture(unsigned int pictures = 3) {
    context.pDriverData = &driver;
    driver.contexts[1] = decoder;
    decoder->width = decoder->height = 16;
    decoder->device = io.handle();
    decoder->decoder_open = decoder->decoder_started = true;
    for (unsigned int index = 0; index < pictures; ++index) {
      const uint64_t timestamp = (index + 1) * kTimestampStep;
      auto surface = std::make_shared<Surface>();
      auto private_frame = std::make_shared<Surface>();
      for (const auto &frame : {surface, private_frame}) {
        Require(frame->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
                "allocate teardown picture");
        memset(frame->storage.data(), 99, frame->storage.size());
        frame->expected_timestamp = timestamp;
      }
      held.push_back(surface);
      driver.surfaces[index + 1] = surface;
      decoder->decoded_frames[timestamp] = private_frame;
      decoder->surface_timestamps[surface.get()] = timestamp;
      decoder->pending[timestamp] = index + 1;
      Require(decoder->replay.Append(timestamp, index == 0, {0, 0, 1, 0x65}),
              "retain accepted but not yet submitted compressed input");
    }
  }

  void CheckPicture(unsigned int index) {
    const uint64_t timestamp = (index + 1) * kTimestampStep;
    const auto &surface = held.at(index);
    Require(surface->ready && !surface->failed &&
                surface->expected_timestamp == timestamp &&
                surface->frame_timestamp == timestamp,
            "retained public picture has its exact completed identity");
    for (unsigned int row = 0; row < surface->height; ++row)
      for (unsigned int column = 0; column < surface->width; ++column)
        Require(surface->planes[0][row * surface->pitch[0] + column] ==
                    DecodeIoMock::Luma(timestamp),
                "retained public picture has its own actual pixels");
    Require(SyncSurface(&context, index + 1) == VA_STATUS_SUCCESS,
            "held picture remains downloadable without its decoder context");
  }
};

static void ContextTeardownDrainsAcceptedTailBeforeClose() {
  // More than PumpDecodeInput's per-call budget exercises accepted input still
  // waiting in the CPU queue as well as pictures already sent to the device.
  TeardownFixture fixture(40);
  Require(DestroyContext(&fixture.context, 1) == VA_STATUS_SUCCESS,
          "context teardown must drain live accepted pictures");
  Require(fixture.driver.contexts.count(1) == 0 && fixture.decoder->retired &&
              fixture.decoder->device == nullptr && fixture.decoder->pending.empty(),
          "drained context releases hardware and logical ownership");
  const auto eos = std::find(fixture.io.events.begin(), fixture.io.events.end(), "eos");
  const auto stop = std::find(fixture.io.events.begin(), fixture.io.events.end(), "stop");
  Require(eos != fixture.io.events.end() && eos < stop &&
              fixture.io.inputs.size() == fixture.held.size() &&
              fixture.io.outputs.empty() && !fixture.io.invalid &&
              fixture.io.open_attempts == 0,
          "actual EOS and all accepted outputs must precede hardware close");
  for (unsigned int index = 0; index < fixture.held.size(); ++index)
    fixture.CheckPicture(index);
}

static void ContextDrainRejectsMutationAndSurvivesSurfaceRelease() {
  TeardownFixture fixture;
  const uint64_t generation = fixture.decoder->generation;
  fixture.io.on_sleep = [&] {
    Require(fixture.decoder->closing && !fixture.decoder->retired,
            "draining context remains alive but closed to new input");
    Require(BeginPicture(&fixture.context, 1, 2) == VA_STATUS_ERROR_INVALID_CONTEXT &&
                RenderPicture(&fixture.context, 1, nullptr, 0) ==
                    VA_STATUS_ERROR_INVALID_CONTEXT &&
                EndPicture(&fixture.context, 1) == VA_STATUS_ERROR_INVALID_CONTEXT &&
                DestroyContext(&fixture.context, 1) == VA_STATUS_ERROR_INVALID_CONTEXT,
            "closing context must reject new or duplicate transactions");
    VABufferID buffer = VA_INVALID_ID;
    Require(CreateBuffer(&fixture.context, 1, VASliceDataBufferType, 1, 1,
                         nullptr, &buffer) == VA_STATUS_ERROR_INVALID_CONTEXT,
            "closing context must reject new buffers");
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      for (VAContextID id = 100; id < 300; ++id)
        fixture.driver.contexts[id] = std::make_shared<DecodeContext>();
    }
    VASurfaceID released = 1;
    Require(DestroySurfaces(&fixture.context, &released, 1) == VA_STATUS_SUCCESS &&
                fixture.decoder->generation == generation &&
                fixture.decoder->decoder_started,
            "one released surface must not reset other held outputs mid-drain");
  };
  Require(DestroyContext(&fixture.context, 1) == VA_STATUS_SUCCESS,
          "context teardown survives concurrent surface release and map rehash");
  Require(fixture.driver.contexts.size() == 200 &&
              fixture.driver.contexts.count(1) == 0 && fixture.held[0]->destroyed,
          "teardown erases only its own context after the unlocked interval");
  fixture.CheckPicture(1);
  fixture.CheckPicture(2);
  Require(!fixture.io.invalid && fixture.io.open_attempts == 0,
          "surface release must not force a reset or hardware reopen");
}

static void WaitingSyncKeepsCompletedRetiredPicture() {
  TeardownFixture fixture;
  fixture.io.on_sleep = [&] {
    Require(DestroyContext(&fixture.context, 1) == VA_STATUS_SUCCESS,
            "concurrent teardown completes then retires the waiting picture");
  };
  Require(SyncSurface(&fixture.context, 1) == VA_STATUS_SUCCESS,
          "already-waiting sync accepts exact completion before retirement");
  Require(fixture.decoder->retired && fixture.driver.contexts.count(1) == 0,
          "completion-race test must actually retire its decoder");
  fixture.CheckPicture(0);
}

static void WaitingVppCancellationStillWins() {
  TeardownFixture fixture(1);
  PendingVpp operation;
  operation.decoder = fixture.decoder;
  operation.decoder_generation = fixture.decoder->generation;
  operation.source = fixture.decoder->decoded_frames.at(kTimestampStep);
  operation.target = fixture.held[0];
  operation.target_owner = fixture.held[0];
  operation.sequence = operation.target_owner->latest_vpp_sequence;
  fixture.io.on_sleep = [&] {
    Require(DestroyContext(&fixture.context, 1) == VA_STATUS_SUCCESS,
            "complete actual source while retiring the VPP decoder");
  };
  std::unique_lock<std::mutex> lock(fixture.driver.mutex);
  Require(SyncDecodeSurface(&fixture.driver, operation.source, &lock,
                            VA_TIMEOUT_INFINITE, [&] {
                              return PendingVppCanceled(operation);
                            }) == VA_STATUS_ERROR_OPERATION_FAILED,
          "completed pixels must not revive a canceled VPP epoch");
  Require(operation.source->ready && fixture.decoder->retired,
          "VPP cancellation must win even over actual completed pixels");
}

static void ContextDrainFailureDoesNotInventTail() {
  for (bool early_eos : {true, false}) {
    TeardownFixture fixture;
    fixture.io.early_eos = early_eos;
    fixture.io.fail_poll = !early_eos;
    Require(DestroyContext(&fixture.context, 1) == VA_STATUS_ERROR_DECODING_ERROR,
            "missing exact outputs or hardware failure must fail teardown");
    Require(fixture.decoder->retired && fixture.decoder->device == nullptr &&
                fixture.driver.contexts.count(1) == 0,
            "failed teardown must still release its context and hardware");
    for (const auto &surface : fixture.held)
      Require(surface->failed && !surface->ready && surface->planes[0][0] == 99,
              "failed tail retains old bytes without claiming a picture");
    Require(SyncSurface(&fixture.context, 1) == VA_STATUS_ERROR_DECODING_ERROR,
            "orphaned incomplete output must expose its decode failure");
  }
  TeardownFixture timeout;
  timeout.decoder->closing = true;
  std::unique_lock<std::mutex> lock(timeout.driver.mutex);
  Require(DrainClosingContext(&timeout.driver, timeout.decoder, &lock, 0) ==
              VA_STATUS_ERROR_DECODING_ERROR && timeout.held[0]->failed &&
              timeout.io.inputs.empty(),
          "a spent drain deadline must fail before more hardware work");
}

static void CompletedWaitNeverAcceptsDifferentOrDestroyedPicture() {
  TeardownFixture fixture(1);
  auto &surface = fixture.held[0];
  const uint64_t generation = fixture.decoder->generation;
  fixture.decoder->retired = true;
  surface->ready = true;
  surface->frame_timestamp = 2 * kTimestampStep;
  Require(DecodeWaitState(*fixture.decoder, surface.get(), generation,
                          kTimestampStep, {}) == VA_STATUS_ERROR_DECODING_ERROR,
          "retirement cannot excuse a completed wrong timestamp");
  surface->failed = false;
  surface->frame_timestamp = surface->expected_timestamp = 2 * kTimestampStep;
  Require(DecodeWaitState(*fixture.decoder, surface.get(), generation,
                          kTimestampStep, {}) == VA_STATUS_ERROR_INVALID_SURFACE,
          "retirement cannot excuse reuse of the public surface");
  surface->frame_timestamp = surface->expected_timestamp = kTimestampStep;
  surface->destroyed = true;
  Require(DecodeWaitState(*fixture.decoder, surface.get(), generation,
                          kTimestampStep, {}) == VA_STATUS_ERROR_INVALID_SURFACE,
          "retirement cannot revive a destroyed surface");
}

int main() {
  try {
    SealedBatchQueuesAndReplays();
    RejectIncompleteAndUnboundedReplay();
    ReclaimOnlyCompletedIdrPrefixes();
    UnknownOutputNeverGuessesAndInvalidOutputFails();
    DuplicateOutputCannotMutateOrRetirePixels();
    RejectIncompleteGeometryAndInitializeAllocationPadding();
    FailedCpuOwnershipNeverCompletesDecode();
    PaddingOnlyCopyByteEquivalence();
    ContextTeardownDrainsAcceptedTailBeforeClose();
    ContextDrainRejectsMutationAndSurvivesSurfaceRelease();
    WaitingSyncKeepsCompletedRetiredPicture();
    WaitingVppCancellationStillWins();
    ContextDrainFailureDoesNotInventTail();
    CompletedWaitNeverAcceptsDifferentOrDestroyedPicture();
    std::puts("VA-API sealed-batch replay, immutable-output and teardown tests passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "VA-API replay test failed: %s\n", error.what());
    return 1;
  }
}
