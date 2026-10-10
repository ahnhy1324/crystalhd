// SPDX-License-Identifier: LGPL-2.1-or-later
// Production replay state and output processing with private CPU memory only.
// No device, decoder, capture, firmware, or DRM operation is started.
#include "../filters/vaapi/crystalhd_drv_video.cpp"
#include <stdexcept>
#include <string>

static int mock_sync_fd = -1;
static uint64_t mock_failed_sync_flags = 0;
static std::vector<uint64_t> mock_sync_calls;
struct ProbePollResult {
  int result = 0;
  short revents = 0;
  int error = 0;
};
static std::deque<ProbePollResult> probe_poll_results;
static unsigned int probe_poll_calls = 0;
static gbm_bo *probe_gbm_bo = nullptr;
static std::vector<int> probe_gbm_plane_fds;
static std::vector<int> probe_gbm_temporary_fds;
extern "C" int __real_ioctl(int fd, unsigned long request, ...);
extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
  va_list arguments;
  va_start(arguments, request);
  void *argument = va_arg(arguments, void *);
  va_end(arguments);
  if (fd == mock_sync_fd && request == DMA_BUF_IOCTL_SYNC) {
    const auto *sync = static_cast<dma_buf_sync *>(argument);
    mock_sync_calls.push_back(sync->flags);
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

extern "C" int __real_poll(struct pollfd *, nfds_t, int);
extern "C" int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
  if (probe_poll_results.empty())
    return __real_poll(fds, count, timeout);
  Require(count == 1 && timeout == 0 && fds != nullptr &&
              fds[0].events == POLLOUT,
          "reuse observation must issue one poll(POLLOUT, timeout=0)");
  const ProbePollResult result = probe_poll_results.front();
  probe_poll_results.pop_front();
  ++probe_poll_calls;
  fds[0].revents = result.revents;
  errno = result.error;
  return result.result;
}

extern "C" int __wrap___poll_chk(struct pollfd *fds, nfds_t count,
                                  int timeout, size_t bytes) {
  Require(fds != nullptr && count <= bytes / sizeof(*fds),
          "fortified poll bounds");
  return __wrap_poll(fds, count, timeout);
}

extern "C" int __real_gbm_bo_get_plane_count(gbm_bo *);
extern "C" int __wrap_gbm_bo_get_plane_count(gbm_bo *bo) {
  if (bo == probe_gbm_bo)
    return static_cast<int>(probe_gbm_plane_fds.size());
  return __real_gbm_bo_get_plane_count(bo);
}

extern "C" int __real_gbm_bo_get_fd_for_plane(gbm_bo *, int);
extern "C" int __wrap_gbm_bo_get_fd_for_plane(gbm_bo *bo, int plane) {
  if (bo != probe_gbm_bo)
    return __real_gbm_bo_get_fd_for_plane(bo, plane);
  Require(plane >= 0 && static_cast<size_t>(plane) < probe_gbm_plane_fds.size(),
          "GBM reuse probe plane bounds");
  const int fd = dup(probe_gbm_plane_fds[plane]);
  if (fd >= 0)
    probe_gbm_temporary_fds.push_back(fd);
  return fd;
}

struct TraceEnvironment {
  bool had_value = false;
  std::string value;
  TraceEnvironment() {
    const char *previous = getenv("CRYSTALHD_VAAPI_TRACE");
    had_value = previous != nullptr;
    if (previous != nullptr)
      value = previous;
    setenv("CRYSTALHD_VAAPI_TRACE", "1", 1);
  }
  ~TraceEnvironment() {
    if (had_value)
      setenv("CRYSTALHD_VAAPI_TRACE", value.c_str(), 1);
    else
      unsetenv("CRYSTALHD_VAAPI_TRACE");
  }
};

struct SavedEnvironment {
  std::string name;
  bool had_value = false;
  std::string value;
  explicit SavedEnvironment(const char *variable) : name(variable) {
    const char *previous = getenv(variable);
    had_value = previous != nullptr;
    if (previous != nullptr)
      value = previous;
  }
  ~SavedEnvironment() {
    if (had_value)
      setenv(name.c_str(), value.c_str(), 1);
    else
      unsetenv(name.c_str());
  }
};

static std::string ReadTraceFd(int fd) {
  struct stat info = {};
  Require(fstat(fd, &info) == 0 && info.st_size >= 0, "inspect trace fixture");
  std::string result(static_cast<size_t>(info.st_size), '\0');
  size_t offset = 0;
  while (offset < result.size()) {
    const ssize_t bytes = pread(fd, result.data() + offset,
                                result.size() - offset, offset);
    Require(bytes > 0, "read trace fixture");
    offset += static_cast<size_t>(bytes);
  }
  return result;
}

static std::string FindTraceEvent(const std::string &trace,
                                  const char *event) {
  const std::string marker = std::string("\"event\":\"") + event + "\"";
  const size_t event_at = trace.find(marker);
  Require(event_at != std::string::npos, "find production trace event");
  const size_t line_start = trace.rfind('\n', event_at);
  const size_t line_end = trace.find('\n', event_at);
  Require(line_end != std::string::npos, "trace event is newline terminated");
  return trace.substr(line_start == std::string::npos ? 0 : line_start + 1,
                      line_end - (line_start == std::string::npos
                                      ? 0 : line_start + 1));
}

static bool TraceFieldIs(const std::string &line, const char *key,
                         uint64_t value) {
  const std::string prefix = std::string("\"") + key + "\":";
  const size_t field = line.find(prefix);
  if (field == std::string::npos)
    return false;
  const size_t number = field + prefix.size();
  const std::string encoded = std::to_string(value);
  const size_t delimiter = number + encoded.size();
  return line.compare(number, encoded.size(), encoded) == 0 &&
         delimiter < line.size() &&
         (line[delimiter] == ',' || line[delimiter] == '}');
}

static bool TraceSignedFieldIs(const std::string &line, const char *key,
                               int64_t value) {
  const std::string prefix = std::string("\"") + key + "\":";
  const size_t field = line.find(prefix);
  if (field == std::string::npos)
    return false;
  const size_t number = field + prefix.size();
  const std::string encoded = std::to_string(value);
  const size_t delimiter = number + encoded.size();
  return line.compare(number, encoded.size(), encoded) == 0 &&
         delimiter < line.size() &&
         (line[delimiter] == ',' || line[delimiter] == '}');
}

static size_t CountTraceEvent(const std::string &trace, const char *event) {
  const std::string marker = std::string("\"event\":\"") + event + "\"";
  size_t count = 0;
  for (size_t at = 0; (at = trace.find(marker, at)) != std::string::npos;
       at += marker.size())
    ++count;
  return count;
}

// These wrappers drive the real submission/drain/teardown state machine with
// deterministic output, without a device or firmware. A sleep callback models
// another API call while the production wait has released the driver mutex.
struct DecodeIoMock {
  struct InputCall {
    uint64_t timestamp = 0;
    std::vector<uint8_t> bytes;
    bool accepted = false;
  };
  std::vector<uint64_t> inputs;
  std::vector<InputCall> input_calls;
  std::deque<uint64_t> outputs;
  std::vector<uint8_t> pixels;
  std::vector<std::string> events;
  std::function<void()> on_sleep;
  bool early_eos = false;
  bool zero_timestamp_picture = false;
  bool fail_poll = false;
  bool input_busy = false;
  bool prefeed_busy_once = false;
  bool remainder_busy_once = false;
  bool allow_device_open = false;
  bool report_bcm70012 = false;
  bool fail_set_input_format = false;
  bool invalid = false;
  uint64_t prefed_timestamp = 0;
  uint32_t tx_free_size = 1024 * 1024;
  unsigned int open_attempts = 0;
  unsigned int output_polls = 0;
  std::thread::id output_thread;
  HANDLE handle() { return static_cast<HANDLE>(this); }
  static uint8_t Luma(uint64_t timestamp) {
    return static_cast<uint8_t>(40 + timestamp / kTimestampStep * 10);
  }
};

// Worker tests publish this pointer before starting the decode thread and
// clear it only after Driver has joined that thread. Production Dts calls are
// serialized by Driver::mutex, so the shared mock observes the same ordering.
static DecodeIoMock *decode_io_mock = nullptr;
struct MockDecodeScope {
  explicit MockDecodeScope(DecodeIoMock &mock) { decode_io_mock = &mock; }
  ~MockDecodeScope() { decode_io_mock = nullptr; }
};

extern "C" BC_STATUS __real_DtsDeviceOpen(HANDLE *, uint32_t);
extern "C" BC_STATUS __wrap_DtsDeviceOpen(HANDLE *device, uint32_t mode) {
  if (decode_io_mock == nullptr)
    return __real_DtsDeviceOpen(device, mode);
  ++decode_io_mock->open_attempts;
  if (decode_io_mock->allow_device_open) {
    *device = decode_io_mock->handle();
    return BC_STS_SUCCESS;
  }
  return BC_STS_ERROR;  // An unexpected reopen must never touch real hardware.
}

extern "C" BC_STATUS __real_DtsCrystalHDVersion(HANDLE, PBC_INFO_CRYSTAL);
extern "C" BC_STATUS __wrap_DtsCrystalHDVersion(
    HANDLE device, PBC_INFO_CRYSTAL version) {
  if (decode_io_mock == nullptr)
    return __real_DtsCrystalHDVersion(device, version);
  decode_io_mock->invalid |= device != decode_io_mock->handle() ||
                             version == nullptr;
  if (version == nullptr)
    return BC_STS_INV_ARG;
  *version = {};
  version->device = decode_io_mock->report_bcm70012 ? 0 : 1;
  return BC_STS_SUCCESS;
}

extern "C" BC_STATUS __real_DtsSetInputFormat(HANDLE, BC_INPUT_FORMAT *);
extern "C" BC_STATUS __wrap_DtsSetInputFormat(
    HANDLE device, BC_INPUT_FORMAT *format) {
  if (decode_io_mock == nullptr)
    return __real_DtsSetInputFormat(device, format);
  decode_io_mock->invalid |= device != decode_io_mock->handle() ||
                             format == nullptr;
  return decode_io_mock->fail_set_input_format ? BC_STS_ERROR
                                                : BC_STS_SUCCESS;
}

#define MOCK_CLOSE_OPERATION(name, event) \
  extern "C" BC_STATUS __real_##name(HANDLE); \
  extern "C" BC_STATUS __wrap_##name(HANDLE device) { \
    if (decode_io_mock == nullptr) \
      return __real_##name(device); \
    decode_io_mock->invalid |= device != decode_io_mock->handle(); \
    decode_io_mock->prefed_timestamp = 0; \
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
  ++decode_io_mock->output_polls;
  decode_io_mock->output_thread = std::this_thread::get_id();
  *status = {};
  status->ReadyListCount = decode_io_mock->outputs.size();
  return decode_io_mock->fail_poll ? BC_STS_ERROR : BC_STS_SUCCESS;
}

extern "C" uint32_t __real_DtsTxFreeSize(HANDLE);
extern "C" uint32_t __wrap_DtsTxFreeSize(HANDLE device) {
  if (decode_io_mock == nullptr)
    return __real_DtsTxFreeSize(device);
  decode_io_mock->invalid |= device != decode_io_mock->handle();
  return decode_io_mock->tx_free_size;
}

extern "C" BC_STATUS __real_DtsProcInput(HANDLE, uint8_t *, uint32_t, uint64_t, BOOL);
extern "C" BC_STATUS __wrap_DtsProcInput(HANDLE device, uint8_t *data,
                                         uint32_t size, uint64_t timestamp,
                                         BOOL encrypted) {
  if (decode_io_mock == nullptr)
    return __real_DtsProcInput(device, data, size, timestamp, encrypted);
  decode_io_mock->invalid |= device != decode_io_mock->handle() ||
                             data == nullptr || size == 0 || encrypted;
  DecodeIoMock::InputCall call;
  call.timestamp = timestamp;
  if (data != nullptr && size != 0)
    call.bytes.assign(data, data + size);
  decode_io_mock->input_calls.push_back(std::move(call));
  auto &record = decode_io_mock->input_calls.back();
  const auto &prefeed = H264LowLatencyPrefeed();
  const bool synthetic = data != nullptr && size == prefeed.size() &&
      std::equal(prefeed.begin(), prefeed.end(), data);
  if (decode_io_mock->input_busy ||
      (synthetic && decode_io_mock->prefeed_busy_once) ||
      (timestamp == 0 && decode_io_mock->prefed_timestamp != 0 &&
       decode_io_mock->remainder_busy_once)) {
    decode_io_mock->prefeed_busy_once = false;
    decode_io_mock->remainder_busy_once = false;
    return BC_STS_BUSY;
  }
  record.accepted = true;
  if (synthetic) {
    decode_io_mock->invalid |= timestamp == 0 ||
                               decode_io_mock->prefed_timestamp != 0;
    decode_io_mock->prefed_timestamp = timestamp;
  } else if (timestamp == 0 && decode_io_mock->prefed_timestamp != 0) {
    decode_io_mock->inputs.push_back(decode_io_mock->prefed_timestamp);
    decode_io_mock->prefed_timestamp = 0;
  } else {
    decode_io_mock->inputs.push_back(timestamp);
  }
  decode_io_mock->events.push_back("input");
  return BC_STS_SUCCESS;
}

extern "C" BC_STATUS __real_DtsFlushInput(HANDLE, uint32_t);
extern "C" BC_STATUS __wrap_DtsFlushInput(HANDLE device, uint32_t mode) {
  if (decode_io_mock == nullptr)
    return __real_DtsFlushInput(device, mode);
  decode_io_mock->invalid |= device != decode_io_mock->handle() || mode != 0;
  decode_io_mock->events.push_back("seal");
  decode_io_mock->prefed_timestamp = 0;
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
  if (timestamp == 0 && !decode_io_mock->zero_timestamp_picture) {
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

static void BoundedLiveStreamRetainsEveryOutstandingPicture() {
  CrystalHDDecodeReplay::Limits limits;
  limits.picture_bytes = 2;
  limits.cache_bytes = 8;
  limits.pictures = 4;
  CrystalHDDecodeReplay live(limits, true);
  for (uint64_t timestamp = 1; timestamp <= 2048; ++timestamp) {
    Require(live.Append(timestamp, timestamp == 1, {1, 2}),
            "live long GOP accepts bounded continuous input");
    SendNext(&live, timestamp);
    if (timestamp > 2)
      Require(live.Observe(timestamp - 2) == CrystalHDDecodeReplay::Output::New,
              "every delayed live picture completes exactly once");
    Require(live.cached_pictures() <= 4 && live.outstanding() <= 3,
            "completed eviction never relaxes queue or in-flight bounds");
  }
  Require(!live.replayable() && !live.failed() && live.Seal(),
          "missing replay history does not poison a finite live drain");
  Require(live.Observe(1) == CrystalHDDecodeReplay::Output::Unknown,
          "evicted duplicate history cannot produce another public picture");
  Require(live.Observe(2047) == CrystalHDDecodeReplay::Output::New &&
              live.Observe(2048) == CrystalHDDecodeReplay::Output::New &&
              live.EndOfSequence() && !live.NeedsRestart(),
          "live final tail requires both exact outputs and genuine EOS");

  limits.cache_bytes = 4;
  limits.pictures = 8;
  CrystalHDDecodeReplay bytes(limits, true);
  for (uint64_t timestamp = 1; timestamp <= 16; ++timestamp) {
    Require(bytes.Append(timestamp, timestamp == 1, {1, 2}),
            "live byte pressure reclaims completed input only");
    SendNext(&bytes, timestamp);
    Require(bytes.Observe(timestamp) == CrystalHDDecodeReplay::Output::New &&
                bytes.cached_pictures() <= 2,
            "live compressed bytes remain bounded independently of count");
  }

  limits.cache_bytes = 8;
  limits.pictures = 4;
  for (bool sent : {false, true}) {
    CrystalHDDecodeReplay pending(limits, true);
    for (uint64_t timestamp = 1; timestamp <= 4; ++timestamp) {
      Require(pending.Append(timestamp, timestamp == 1, {1}),
              "fill live pending bound");
      if (sent)
        SendNext(&pending, timestamp);
    }
    Require(!pending.Append(5, false, {1}) && pending.cached_pictures() == 4 &&
                pending.outstanding() == (sent ? 4U : 0U),
            "live mode cannot evict queued input or outstanding identities");
  }

  limits.cache_bytes = limits.pictures = 1;
  CrystalHDDecodeReplay watermark(limits, true);
  Require(watermark.Append(1, true, {1}), "append live watermark root");
  SendNext(&watermark, 1);
  watermark.Observe(1);
  Require(watermark.Append(2, false, {2}),
          "an empty retired cache is not an empty running decoder");
  SendNext(&watermark, 2);
  watermark.Observe(2);
  Require(!watermark.Append(1, true, {1}),
          "eviction never permits an already accepted timestamp again");
}

static void LiveHistoryRestoresOnlyAtAnIntactActualIdr() {
  CrystalHDDecodeReplay::Limits limits;
  limits.picture_bytes = 1;
  limits.cache_bytes = limits.pictures = 4;
  CrystalHDDecodeReplay live(limits, true);
  for (uint64_t timestamp = 1; timestamp <= 4; ++timestamp) {
    Require(live.Append(timestamp, timestamp == 1, {1}),
            "append live reordered-history test");
    SendNext(&live, timestamp);
    if (timestamp > 1)
      live.Observe(timestamp);
  }
  Require(live.Append(5, false, {1}) && !live.replayable() &&
              live.outstanding() == 1,
          "evict completed interior input without losing the old pending IDR");
  SendNext(&live, 5);
  Require(live.Observe(1) == CrystalHDDecodeReplay::Output::New &&
              !live.replayable(),
          "an IDR older than an evicted P cannot restore incomplete history");
  Require(live.Append(6, true, {6}), "retain a genuine new IDR");
  SendNext(&live, 6);
  Require(live.Observe(6) == CrystalHDDecodeReplay::Output::New &&
              !live.replayable(),
          "new IDR completion cannot retire an earlier outstanding picture");
  Require(live.Observe(5) == CrystalHDDecodeReplay::Output::New &&
              live.replayable() && live.cached_pictures() == 1,
          "completed old tail permits restoring an intact actual IDR prefix");
  Require(live.Append(7, false, {7}), "append restored-history P");
  SendNext(&live, 7);
  Require(live.Seal() && live.Append(8, false, {8}),
          "queue a continuation with restored replay history");
  Require(live.Observe(7) == CrystalHDDecodeReplay::Output::New &&
              live.EndOfSequence() && live.Restarted() &&
              live.NextInput()->timestamp == 6 && live.NextInput()->bytes[0] == 6,
          "restored replay starts at real retained bytes, never a synthetic IDR");
}

static void LiveEosNeverSkipsAnAcceptedContinuation() {
  CrystalHDDecodeReplay::Limits limits;
  limits.picture_bytes = 1;
  limits.cache_bytes = limits.pictures = 3;
  for (bool idr_first : {false, true}) {
    CrystalHDDecodeReplay live(limits, true);
    for (uint64_t timestamp = 1; timestamp <= 4; ++timestamp) {
      Require(live.Append(timestamp, timestamp == 1, {1}),
              "append live EOS history-pressure test");
      SendNext(&live, timestamp);
      if (timestamp < 4)
        live.Observe(timestamp);
    }
    Require(!live.replayable() && live.Seal(),
            "seal live tail after dropping completed history");
    Require(live.Append(5, idr_first, {5}) &&
                live.Append(6, !idr_first, {6}) && !live.failed() &&
                live.NextInput() == nullptr && !live.NeedsRestart(),
            "queued continuation cannot poison or precede the accepted old tail");
    Require(live.Observe(4) == CrystalHDDecodeReplay::Output::New &&
                live.EndOfSequence() && live.NeedsRestart(),
            "old live tail completes before continuation is evaluated");
    if (!idr_first) {
      Require(!live.Restarted() && live.failed(),
              "later IDR cannot excuse skipping the first accepted P");
      continue;
    }
    Require(live.Restarted() && live.replayable() &&
                live.cached_pictures() == 2 && live.outstanding() == 0,
            "first queued actual IDR safely starts a fresh bounded batch");
    SendNext(&live, 5);
    SendNext(&live, 6);
    Require(live.Observe(5) == CrystalHDDecodeReplay::Output::New &&
                live.Observe(6) == CrystalHDDecodeReplay::Output::New,
            "both accepted continuation pictures produce exact new outputs");
  }

  limits.cache_bytes = limits.pictures = 4;
  limits.replay_pictures = 1;
  CrystalHDDecodeReplay replay_work(limits, true);
  Require(replay_work.Append(1, true, {1}) &&
              replay_work.Append(2, false, {2}),
          "append bounded live replay work");
  SendNext(&replay_work, 1);
  SendNext(&replay_work, 2);
  Require(replay_work.Seal() && replay_work.Append(3, false, {3}) &&
              replay_work.Observe(1) == CrystalHDDecodeReplay::Output::New &&
              replay_work.Observe(2) == CrystalHDDecodeReplay::Output::New &&
              replay_work.EndOfSequence() && replay_work.Restarted(),
          "intact live history keeps ordinary strict replay semantics");
  SendNext(&replay_work, 1);
  Require(replay_work.Observe(1) == CrystalHDDecodeReplay::Output::Duplicate,
          "resubmitted live history is not a new public picture");
  Require(replay_work.NextInput() == nullptr && replay_work.failed(),
          "live mode does not relax the finite replay-work bound");
}

static void LiveStreamMarkerSurvivesReplayAndPruning() {
  for (bool live : {false, true}) {
    CrystalHDDecodeReplay replay(live);
    Require(!replay.live_stream_started() && replay.Append(700, true, {1}) &&
                !replay.live_stream_started(),
            "one accepted original AU remains a probe regardless of timestamp");
    SendNext(&replay, 700);
    Require(!replay.live_stream_started() &&
                replay.Observe(700) == CrystalHDDecodeReplay::Output::New &&
                !replay.live_stream_started(),
            "submission and output do not count as additional original input");
    Require(replay.Append(701, true, {2}) && replay.live_stream_started() == live,
            "only live mode starts streaming after a second accepted original AU");
    SendNext(&replay, 701);
    Require(replay.Observe(701) == CrystalHDDecodeReplay::Output::New &&
                replay.cached_pictures() == 1 && replay.live_stream_started() == live,
            "pruning to one intact new IDR cannot turn a stream back into a probe");
    Require(replay.Append(702, false, {3}), "accept pre-seal stream input");
    SendNext(&replay, 702);
    Require(replay.Seal() && replay.Append(703, false, {4}) &&
                replay.Observe(702) == CrystalHDDecodeReplay::Output::New &&
                replay.EndOfSequence() && replay.Restarted() &&
                replay.live_stream_started() == live,
            "explicit EOS and restart preserve the original-input stream marker");
    SendNext(&replay, 701);
    Require(replay.Observe(701) == CrystalHDDecodeReplay::Output::Duplicate &&
                replay.live_stream_started() == live,
            "replayed input and duplicate output preserve stream policy");
  }

  for (unsigned int invalid = 0; invalid < 4; ++invalid) {
    CrystalHDDecodeReplay::Limits limits;
    limits.picture_bytes = limits.cache_bytes = limits.pictures = 1;
    CrystalHDDecodeReplay replay(limits, true);
    Require(replay.Append(700, true, {1}), "accept one AU before rejected continuation");
    const uint64_t timestamp = invalid == 0 ? 700 : 701;
    const std::vector<uint8_t> bytes = invalid == 1 ? std::vector<uint8_t>{}
        : invalid == 2 ? std::vector<uint8_t>{1, 2} : std::vector<uint8_t>{1};
    Require(!replay.Append(timestamp, false, bytes) && !replay.live_stream_started(),
            "duplicate, empty, oversized and over-capacity inputs never start a stream");
  }
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

  void Direct() {
    private_frame = std::make_shared<Surface>();
    private_frame->width = public_frame->width;
    private_frame->height = public_frame->height;
    private_frame->expected_timestamp = 1;
    private_frame->direct_picture = true;
    private_frame->decode_target = public_frame;
    private_frame->direct_backing = public_frame;
    public_frame->direct_decode_eligible = true;
    public_frame->decode_picture = private_frame;
    decoder.decoded_frames[1] = private_frame;
  }
};

static void TraceIdentityUsesImmutableTokenAndLiveAliasOwner() {
  TraceEnvironment environment;
  const int trace_fd = memfd_create("crystalhd-vaapi-trace-test", MFD_CLOEXEC);
  Require(trace_fd >= 0, "allocate production trace fixture");
  {
    Driver driver{-1, trace_fd};
    DecodeContext decoder;
    decoder.id = 7;
    decoder.generation = 3;
    decoder.width = decoder.height = 16;

    const uint64_t old_token = kTimestampStep;
    const uint64_t new_token = 2 * kTimestampStep;
    auto old_picture = std::make_shared<Surface>();
    auto new_picture = std::make_shared<Surface>();
    auto reused_public = std::make_shared<Surface>();
    for (const auto &surface : {old_picture, new_picture, reused_public})
      Require(surface->AllocateInternal(nullptr, -1, 16, 16,
                                        VA_FOURCC_NV12),
              "allocate trace identity surface");

    old_picture->id = 5;
    old_picture->expected_timestamp = old_token;
    old_picture->decode_identity = old_picture->trace_decode_identity = 41;
    old_picture->trace_context = 7;
    old_picture->trace_generation = 3;
    old_picture->trace_submission_ordinal = 1;
    new_picture->id = 5;
    new_picture->expected_timestamp = new_token;
    new_picture->decode_identity = new_picture->trace_decode_identity = 42;
    new_picture->trace_context = 7;
    new_picture->trace_generation = 3;
    new_picture->trace_submission_ordinal = 2;
    reused_public->id = 5;
    reused_public->expected_timestamp = new_token;
    reused_public->decode_identity = reused_public->trace_decode_identity = 42;
    reused_public->trace_context = 7;
    reused_public->trace_generation = 3;
    reused_public->trace_submission_ordinal = 2;
    reused_public->decode_picture = new_picture;
    driver.surfaces[5] = reused_public;
    decoder.decoded_frames[old_token] = old_picture;
    decoder.decoded_frames[new_token] = new_picture;
    decoder.pending[old_token] = 5;
    decoder.pending[new_token] = 5;
    decoder.surface_timestamps[reused_public.get()] = new_token;
    Require(decoder.replay.Append(old_token, true, {1}),
            "append old trace token before public reuse");
    SendNext(&decoder.replay, old_token);

    std::vector<uint8_t> pixels(16 * 16 * 2, 128);
    BC_DTS_PROC_OUT output = {};
    output.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    output.PicInfo.timeStamp = old_token;
    output.PicInfo.width = output.PicInfo.height = 16;
    output.Ybuff = pixels.data();
    output.YBuffDoneSz = pixels.size() / 4;
    Require(ProcessDecodedOutput(&driver, &decoder, output) ==
                VA_STATUS_SUCCESS,
            "process old output after its public surface was rebound");

    auto owner = std::make_shared<Surface>();
    auto alias = std::make_shared<Surface>();
    owner->id = 1;
    owner->ready = true;
    owner->expected_timestamp = owner->frame_timestamp = 300000;
    owner->trace_context = 9;
    owner->trace_generation = 5;
    owner->trace_submission_ordinal = 3;
    owner->trace_decode_identity = 44;
    alias->id = 2;
    alias->backing_owner = 1;
    alias->ready = true;
    alias->expected_timestamp = alias->frame_timestamp = 100000;
    alias->trace_context = 2;
    alias->trace_generation = 1;
    alias->trace_submission_ordinal = 1;
    alias->trace_decode_identity = 12;
    driver.surfaces[1] = owner;
    driver.surfaces[2] = alias;
    VADriverContext va_context = {};
    va_context.pDriverData = &driver;
    Require(SyncSurface2(&va_context, 2, 0) == VA_STATUS_SUCCESS,
            "synchronize ready alias through its current owner");
    VADRMPRIMESurfaceDescriptor descriptor = {};
    Require(ExportSurfaceHandle(
                &va_context, 2, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_ONLY, &descriptor) ==
                VA_STATUS_ERROR_UNIMPLEMENTED,
            "trace an alias export before its backing-type rejection");
  }

  const std::string trace = ReadTraceFd(trace_fd);
  close(trace_fd);
  const std::string output = FindTraceEvent(trace, "output_dequeued");
  Require(TraceFieldIs(output, "context", 7) &&
              TraceFieldIs(output, "generation", 3) &&
              TraceFieldIs(output, "surface", 5) &&
              TraceFieldIs(output, "token", kTimestampStep) &&
              TraceFieldIs(output, "decode_identity", 41) &&
              TraceFieldIs(output, "owner", 5) &&
              TraceFieldIs(output, "submission_ordinal", 1) &&
              !TraceFieldIs(output, "decode_identity", 42),
          "old output trace keeps its immutable pre-reuse identity");
  for (const char *event : {"sync_enter", "export_enter"}) {
    const std::string line = FindTraceEvent(trace, event);
    Require(TraceFieldIs(line, "context", 9) &&
                TraceFieldIs(line, "generation", 5) &&
                TraceFieldIs(line, "surface", 2) &&
                TraceFieldIs(line, "token", 300000) &&
                TraceFieldIs(line, "decode_identity", 44) &&
                TraceFieldIs(line, "owner", 1) &&
                TraceFieldIs(line, "submission_ordinal", 3) &&
                !TraceFieldIs(line, "decode_identity", 12),
            "alias trace takes identity from its current canonical owner");
  }
}

static int TraceBackingFd(const char *name, size_t bytes) {
  const int fd = memfd_create(name, MFD_CLOEXEC);
  Require(fd >= 0 && ftruncate(fd, static_cast<off_t>(bytes)) == 0,
          "allocate trace reuse backing");
  return fd;
}

static std::shared_ptr<Surface> InstallTraceReuseSurface(
    Driver *driver, const std::shared_ptr<DecodeContext> &decode,
    const std::vector<int> &object_fds) {
  auto surface = std::make_shared<Surface>();
  Require(surface->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
          "allocate trace reuse surface");
  surface->id = 1;
  surface->rt_format = VA_RT_FORMAT_YUV420;
  surface->ready = true;
  surface->expected_timestamp = surface->frame_timestamp = 400000;
  surface->decode_identity = surface->trace_decode_identity = 19;
  surface->trace_context = 7;
  surface->trace_generation = 3;
  surface->trace_submission_ordinal = 4;
  surface->object_fds = object_fds;
  surface->object_sizes.assign(object_fds.size(), surface->storage.size());
  driver->surfaces[1] = surface;
  decode->id = 7;
  decode->generation = 3;
  driver->contexts[7] = decode;
  return surface;
}

static void ClosePrimeDescriptor(VADRMPRIMESurfaceDescriptor *descriptor) {
  for (unsigned int object = 0; object < descriptor->num_objects; ++object)
    close(descriptor->objects[object].fd);
  descriptor->num_objects = 0;
}

static void TraceReadExportReuseBoundary() {
  {
    TraceEnvironment environment;
    const int trace_fd = memfd_create("crystalhd-reuse-trace", MFD_CLOEXEC);
    Require(trace_fd >= 0, "allocate read-export reuse trace sink");
    {
      Driver driver{-1, trace_fd};
      auto decode = std::make_shared<DecodeContext>();
      const size_t bytes = 16 * 16 * 3 / 2;
      const int first = TraceBackingFd("crystalhd-reuse-first", bytes);
      const int duplicate = dup(first);
      const int second = TraceBackingFd("crystalhd-reuse-second", bytes);
      Require(duplicate >= 0, "duplicate trace reuse object");
      const auto surface = InstallTraceReuseSurface(
          &driver, decode, {first, duplicate, second});
      VADriverContext context = {};
      context.pDriverData = &driver;
      VADRMPRIMESurfaceDescriptor descriptor = {};
      Require(ExportSurfaceHandle(&context, 1,
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_READ_ONLY, &descriptor) ==
                  VA_STATUS_SUCCESS,
              "successful explicit read-only export arms reuse observation");
      ClosePrimeDescriptor(&descriptor);
      Require(surface->trace_read_export_armed,
              "read-only canonical export remembers the exact binding");
      Require(surface->trace_read_export.operation == 1,
              "first read-only export owns the first trace operation");
      Require(CountTraceEvent(ReadTraceFd(trace_fd),
                              "read_export_reuse_implicit_fence_probe") == 0,
              "export return alone is not reader completion");
      Require(ExportSurfaceHandle(&context, 1,
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_READ_ONLY, &descriptor) ==
                  VA_STATUS_SUCCESS,
              "repeat explicit read-only export of the same binding");
      ClosePrimeDescriptor(&descriptor);
      Require(surface->trace_read_export_armed &&
                  surface->trace_read_export.operation == 2,
              "latest read-only export replaces the correlation operation");
      Require(ExportSurfaceHandle(&context, 1,
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_WRITE_ONLY, &descriptor) ==
                  VA_STATUS_SUCCESS,
              "later write-only export remains behaviorally valid");
      ClosePrimeDescriptor(&descriptor);
      Require(surface->trace_read_export_armed &&
                  surface->trace_read_export.operation == 2,
              "write-only export cannot hide the earlier external reader");

      probe_poll_calls = 0;
      probe_poll_results = {{0, 0, 0}};
      Require(BeginPicture(&context, 7, 1) == VA_STATUS_SUCCESS,
              "same-generation surface reuse remains behaviorally valid");
      Require(probe_poll_results.empty() && probe_poll_calls == 1 &&
                  !surface->trace_read_export_armed,
              "reuse probes only the backing object actually exported");
      Require(BeginPicture(&context, 7, 1) == VA_STATUS_SUCCESS &&
                  probe_poll_calls == 1,
              "one export binding produces only one reuse probe");
    }
    const std::string trace = ReadTraceFd(trace_fd);
    close(trace_fd);
    const std::string probe = FindTraceEvent(trace,
        "read_export_reuse_implicit_fence_probe");
    Require(TraceFieldIs(probe, "context", 7) &&
                TraceFieldIs(probe, "generation", 3) &&
                TraceFieldIs(probe, "surface", 1) &&
                TraceFieldIs(probe, "token", 400000) &&
                TraceFieldIs(probe, "decode_identity", 19) &&
                TraceFieldIs(probe, "owner", 1) &&
                TraceFieldIs(probe, "submission_ordinal", 4) &&
                TraceFieldIs(probe, "operation", 2) &&
                TraceFieldIs(probe,
                             "identified_exported_object_count", 1) &&
                TraceFieldIs(probe, "pollout_count", 0) &&
                TraceFieldIs(probe, "not_pollout_count", 1) &&
                TraceFieldIs(probe, "probe_error_count", 0) &&
                TraceFieldIs(probe, "probe_errno", 0) &&
                TraceSignedFieldIs(probe, "outcome", -EAGAIN),
            "reuse trace preserves export identity and POLLOUT snapshot");
    Require(CountTraceEvent(
                trace, "read_export_reuse_implicit_fence_probe") == 1,
            "same binding reuse trace is exact once");
  }

  {
    TraceEnvironment environment;
    const int trace_fd = memfd_create("crystalhd-reuse-ready", MFD_CLOEXEC);
    Require(trace_fd >= 0, "allocate all-ready reuse trace sink");
    {
      Driver driver{-1, trace_fd};
      auto decode = std::make_shared<DecodeContext>();
      const int object = TraceBackingFd("crystalhd-reuse-ready-object",
                                        16 * 16 * 3 / 2);
      InstallTraceReuseSurface(&driver, decode, {object});
      VADriverContext context = {};
      context.pDriverData = &driver;
      VADRMPRIMESurfaceDescriptor descriptor = {};
      Require(ExportSurfaceHandle(&context, 1,
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_READ_ONLY, &descriptor) ==
                  VA_STATUS_SUCCESS,
              "arm all-ready reuse export");
      ClosePrimeDescriptor(&descriptor);
      probe_poll_calls = 0;
      probe_poll_results = {{1, POLLOUT, 0}};
      Require(BeginPicture(&context, 7, 1) == VA_STATUS_SUCCESS &&
                  probe_poll_results.empty() && probe_poll_calls == 1,
              "all-ready observation cannot alter surface reuse");
    }
    const std::string trace = ReadTraceFd(trace_fd);
    close(trace_fd);
    const std::string probe = FindTraceEvent(trace,
        "read_export_reuse_implicit_fence_probe");
    Require(TraceFieldIs(probe, "identified_exported_object_count", 1) &&
                TraceFieldIs(probe, "pollout_count", 1) &&
                TraceFieldIs(probe, "not_pollout_count", 0) &&
                TraceFieldIs(probe, "probe_error_count", 0) &&
                TraceFieldIs(probe, "probe_errno", 0) &&
                TraceSignedFieldIs(probe, "outcome", 0),
            "all-ready reuse is reported separately from busy and errors");
  }

  {
    TraceEnvironment environment;
    const int trace_fd = memfd_create("crystalhd-reuse-cancel", MFD_CLOEXEC);
    Require(trace_fd >= 0, "allocate generation-cancel trace sink");
    {
      Driver driver{-1, trace_fd};
      auto decode = std::make_shared<DecodeContext>();
      const int object = TraceBackingFd("crystalhd-reuse-cancel-object",
                                        16 * 16 * 3 / 2);
      const auto surface = InstallTraceReuseSurface(&driver, decode, {object});
      VADriverContext context = {};
      context.pDriverData = &driver;
      VADRMPRIMESurfaceDescriptor descriptor = {};
      Require(ExportSurfaceHandle(&context, 1,
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_READ_ONLY, &descriptor) ==
                  VA_STATUS_SUCCESS,
              "arm generation-cancel export");
      ClosePrimeDescriptor(&descriptor);
      ++decode->generation;
      probe_poll_calls = 0;
      probe_poll_results.clear();
      Require(BeginPicture(&context, 7, 1) == VA_STATUS_SUCCESS &&
                  !surface->trace_read_export_armed && probe_poll_calls == 0,
              "generation mismatch cancels without probing old identity");
    }
    const std::string trace = ReadTraceFd(trace_fd);
    close(trace_fd);
    Require(CountTraceEvent(
                trace, "read_export_reuse_implicit_fence_probe") == 0,
            "generation mismatch cannot emit a misattributed reuse result");
  }

  {
    TraceEnvironment environment;
    const int trace_fd = memfd_create("crystalhd-reuse-gbm", MFD_CLOEXEC);
    Require(trace_fd >= 0, "allocate GBM-plane reuse trace sink");
    {
      Driver driver{-1, trace_fd};
      auto decode = std::make_shared<DecodeContext>();
      const size_t bytes = 16 * 16 * 3 / 2;
      const int first = TraceBackingFd("crystalhd-reuse-gbm-first", bytes);
      const int duplicate = dup(first);
      Require(duplicate >= 0, "duplicate GBM-plane backing identity");
      const auto surface = InstallTraceReuseSurface(
          &driver, decode, {first, duplicate});
      probe_gbm_bo = reinterpret_cast<gbm_bo *>(uintptr_t{0x12345});
      probe_gbm_plane_fds = surface->object_fds;
      probe_gbm_temporary_fds.clear();
      surface->bo = probe_gbm_bo;
      surface->trace_read_export = TraceFields(nullptr, surface.get());
      surface->trace_read_export.operation = 77;
      surface->trace_read_export_armed = true;
      VADriverContext context = {};
      context.pDriverData = &driver;
      probe_poll_calls = 0;
      probe_poll_results = {{1, POLLOUT, 0}};
      const VAStatus status = BeginPicture(&context, 7, 1);
      surface->bo = nullptr;
      probe_gbm_bo = nullptr;
      probe_gbm_plane_fds.clear();
      bool all_temporary_fds_closed =
          probe_gbm_temporary_fds.size() == 2;
      for (int fd : probe_gbm_temporary_fds) {
        errno = 0;
        all_temporary_fds_closed &=
            fcntl(fd, F_GETFD) == -1 && errno == EBADF;
      }
      probe_gbm_temporary_fds.clear();
      Require(status == VA_STATUS_SUCCESS && probe_poll_results.empty() &&
                  probe_poll_calls == 1 && all_temporary_fds_closed,
              "GBM planes use closed temporary fds and dedupe shared objects");
    }
    const std::string trace = ReadTraceFd(trace_fd);
    close(trace_fd);
    const std::string probe = FindTraceEvent(trace,
        "read_export_reuse_implicit_fence_probe");
    Require(TraceFieldIs(probe, "operation", 77) &&
                TraceFieldIs(probe,
                             "identified_exported_object_count", 1) &&
                TraceFieldIs(probe, "pollout_count", 1) &&
                TraceFieldIs(probe, "not_pollout_count", 0) &&
                TraceSignedFieldIs(probe, "outcome", 0),
            "GBM shared-plane reuse reports one deduped backing object");
  }

  {
    TraceEnvironment environment;
    const int trace_fd = memfd_create("crystalhd-reuse-gbm-distinct",
                                      MFD_CLOEXEC);
    Require(trace_fd >= 0, "allocate distinct GBM-plane trace sink");
    {
      Driver driver{-1, trace_fd};
      auto decode = std::make_shared<DecodeContext>();
      const size_t bytes = 16 * 16 * 3 / 2;
      const int first = TraceBackingFd("crystalhd-reuse-gbm-y", bytes);
      const int second = TraceBackingFd("crystalhd-reuse-gbm-uv", bytes);
      const int auxiliary = TraceBackingFd("crystalhd-reuse-gbm-aux", bytes);
      const auto surface = InstallTraceReuseSurface(
          &driver, decode, {first, second, auxiliary});
      probe_gbm_bo = reinterpret_cast<gbm_bo *>(uintptr_t{0x12346});
      probe_gbm_plane_fds = surface->object_fds;
      probe_gbm_temporary_fds.clear();
      surface->bo = probe_gbm_bo;
      surface->trace_read_export = TraceFields(nullptr, surface.get());
      surface->trace_read_export.operation = 78;
      surface->trace_read_export_armed = true;
      VADriverContext context = {};
      context.pDriverData = &driver;
      probe_poll_calls = 0;
      probe_poll_results = {{1, POLLOUT, 0}, {1, POLLOUT, 0}};
      const VAStatus status = BeginPicture(&context, 7, 1);
      surface->bo = nullptr;
      probe_gbm_bo = nullptr;
      probe_gbm_plane_fds.clear();
      bool all_temporary_fds_closed =
          probe_gbm_temporary_fds.size() == 2;
      for (int fd : probe_gbm_temporary_fds) {
        errno = 0;
        all_temporary_fds_closed &=
            fcntl(fd, F_GETFD) == -1 && errno == EBADF;
      }
      probe_gbm_temporary_fds.clear();
      Require(status == VA_STATUS_SUCCESS && probe_poll_results.empty() &&
                  probe_poll_calls == 2 && all_temporary_fds_closed,
              "exported GBM planes are probed and auxiliary plane is excluded");
    }
    const std::string trace = ReadTraceFd(trace_fd);
    close(trace_fd);
    const std::string probe = FindTraceEvent(
        trace, "read_export_reuse_implicit_fence_probe");
    Require(TraceFieldIs(probe, "operation", 78) &&
                TraceFieldIs(probe,
                             "identified_exported_object_count", 2) &&
                TraceFieldIs(probe, "pollout_count", 2) &&
                TraceFieldIs(probe, "not_pollout_count", 0) &&
                TraceSignedFieldIs(probe, "outcome", 0),
            "GBM export reports two objects and excludes auxiliary planes");
  }

  {
    TraceEnvironment environment;
    const int trace_fd = memfd_create("crystalhd-reuse-error", MFD_CLOEXEC);
    Require(trace_fd >= 0, "allocate reuse error trace sink");
    {
      Driver driver{-1, trace_fd};
      auto decode = std::make_shared<DecodeContext>();
      const int object = TraceBackingFd("crystalhd-reuse-error-object",
                                        16 * 16 * 3 / 2);
      InstallTraceReuseSurface(&driver, decode, {object});
      VADriverContext context = {};
      context.pDriverData = &driver;
      VADRMPRIMESurfaceDescriptor descriptor = {};
      Require(ExportSurfaceHandle(&context, 1,
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_READ_ONLY, &descriptor) ==
                  VA_STATUS_SUCCESS,
              "arm poll-error export");
      ClosePrimeDescriptor(&descriptor);
      probe_poll_calls = 0;
      probe_poll_results = {{-1, 0, EIO}};
      Require(BeginPicture(&context, 7, 1) == VA_STATUS_SUCCESS &&
                  probe_poll_results.empty() && probe_poll_calls == 1,
              "observation error cannot change valid reuse result");
    }
    const std::string trace = ReadTraceFd(trace_fd);
    close(trace_fd);
    const std::string probe = FindTraceEvent(trace,
        "read_export_reuse_implicit_fence_probe");
    Require(TraceFieldIs(probe, "identified_exported_object_count", 1) &&
                TraceFieldIs(probe, "pollout_count", 0) &&
                TraceFieldIs(probe, "not_pollout_count", 0) &&
                TraceFieldIs(probe, "probe_error_count", 1) &&
                TraceFieldIs(probe, "probe_errno", EIO) &&
                TraceSignedFieldIs(probe, "outcome", -EIO),
            "reuse trace separates poll errors from ready and busy objects");
  }

  {
    const char *previous = getenv("CRYSTALHD_VAAPI_TRACE");
    const bool had_previous = previous != nullptr;
    const std::string previous_value = previous == nullptr ? "" : previous;
    unsetenv("CRYSTALHD_VAAPI_TRACE");
    const int trace_fd = memfd_create("crystalhd-reuse-disabled", MFD_CLOEXEC);
    Require(trace_fd >= 0, "allocate disabled reuse trace sink");
    {
      Driver driver{-1, trace_fd};
      auto decode = std::make_shared<DecodeContext>();
      const int object = TraceBackingFd("crystalhd-reuse-disabled-object",
                                        16 * 16 * 3 / 2);
      const auto surface = InstallTraceReuseSurface(&driver, decode, {object});
      VADriverContext context = {};
      context.pDriverData = &driver;
      VADRMPRIMESurfaceDescriptor descriptor = {};
      Require(ExportSurfaceHandle(&context, 1,
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_READ_ONLY, &descriptor) ==
                  VA_STATUS_SUCCESS,
              "trace-off read-only export behavior is unchanged");
      ClosePrimeDescriptor(&descriptor);
      probe_poll_calls = 0;
      probe_poll_results = {{-1, 0, EIO}};
      const VAStatus begin_status = BeginPicture(&context, 7, 1);
      Require(!surface->trace_read_export_armed &&
                  begin_status == VA_STATUS_SUCCESS &&
                  probe_poll_calls == 0 && probe_poll_results.size() == 1,
              "trace-off reuse performs no observation work");
      probe_poll_results.clear();
    }
    Require(ReadTraceFd(trace_fd).empty(), "trace-off path stays silent");
    close(trace_fd);
    if (had_previous)
      setenv("CRYSTALHD_VAAPI_TRACE", previous_value.c_str(), 1);
    else
      unsetenv("CRYSTALHD_VAAPI_TRACE");
  }
}

static void ExperimentalReadyRemapStateMachine() {
  TraceEnvironment environment;
  const int trace_fd = memfd_create("crystalhd-remap-trace", MFD_CLOEXEC);
  Require(trace_fd >= 0, "allocate experimental remap trace sink");
  {
    Driver driver{-1, trace_fd};
    auto decode = std::make_shared<DecodeContext>();
    decode->id = 7;
    decode->generation = 3;
    decode->profile = VAProfileH264Main;
    decode->width = decode->height = 16;
    decode->live_h264 = true;
    decode->low_latency_h264 = true;
    decode->experimental_ready_remap = true;
    driver.contexts[7] = decode;

    auto payload = std::make_shared<Surface>();
    auto requested_picture = std::make_shared<Surface>();
    auto requested = std::make_shared<Surface>();
    for (const auto &surface : {payload, requested_picture, requested})
      Require(surface->AllocateInternal(nullptr, -1, 16, 16,
                                        VA_FOURCC_NV12),
              "allocate experimental remap pixels");
    payload->id = 1;
    payload->expected_timestamp = payload->frame_timestamp = 100000;
    payload->ready = true;
    payload->decode_identity = payload->trace_decode_identity = 41;
    payload->trace_context = 7;
    payload->trace_generation = 3;
    payload->trace_submission_ordinal = 1;
    payload->experimental_ready_at_ns = MonotonicNowNs();
    payload->experimental_ready_queued = true;
    memset(payload->planes[0], 40, payload->storage.size());

    requested_picture->id = 2;
    requested_picture->expected_timestamp = 200000;
    requested_picture->decode_identity =
        requested_picture->trace_decode_identity = 42;
    requested_picture->trace_context = 7;
    requested_picture->trace_generation = 3;
    requested_picture->trace_submission_ordinal = 2;

    requested->id = 2;
    requested->rt_format = VA_RT_FORMAT_YUV420;
    requested->direct_decode_eligible = true;
    requested->expected_timestamp = 200000;
    requested->decode_identity = requested->trace_decode_identity = 42;
    requested->trace_context = 7;
    requested->trace_generation = 3;
    requested->trace_submission_ordinal = 2;
    requested->experimental_write_lease = true;
    requested->experimental_write_context = 7;
    requested->experimental_write_generation = 3;
    requested->experimental_write_submission_ordinal = 2;
    requested->decode_picture = requested_picture;
    memset(requested->planes[0], 99, requested->storage.size());

    decode->experimental_ready.push_back(payload);
    decode->decoded_frames[payload->expected_timestamp] = payload;
    decode->decoded_frames[requested->expected_timestamp] = requested_picture;
    decode->surface_timestamps[requested.get()] =
        requested->expected_timestamp;
    driver.surfaces[2] = requested;

    bool committed = false;
    Require(TryExperimentalPresentation(&driver, requested, &committed) ==
                    VA_STATUS_SUCCESS &&
                committed &&
                ExperimentalPresentationReadable(&driver, requested.get()) &&
                !requested->ready && requested->frame_timestamp == 0 &&
                requested->planes[0][0] == 40 &&
                requested->presentation_payload_token == 100000 &&
                requested->presentation_payload_decode_identity == 41 &&
                requested->presentation_payload_submission_ordinal == 1 &&
                decode->experimental_ready.empty() &&
                decode->decoded_frames.count(100000) == 0 &&
                decode->decoded_frames.count(200000) == 1,
            "remap commits copied payload without relabeling exact decode");

    auto ordinary = std::make_shared<DecodeContext>();
    ordinary->id = 10;
    driver.contexts[10] = ordinary;
    VADriverContext context = {};
    context.pDriverData = &driver;
    Require(BeginPicture(&context, 10, 2) ==
                VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT,
            "ordinary decode cannot seize an experimental presentation backing");

    auto vpp = std::make_shared<DecodeContext>();
    vpp->id = 8;
    vpp->video_process = true;
    driver.contexts[8] = vpp;
    Require(BeginPicture(&context, 8, 2) ==
                VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT,
            "VPP cannot use an experimental presentation as its write target");
    auto vpp_target = std::make_shared<Surface>();
    Require(vpp_target->AllocateInternal(nullptr, -1, 16, 16,
                                         VA_FOURCC_NV12),
            "allocate excluded VPP target");
    vpp_target->id = 6;
    vpp_target->ready = true;
    vpp_target->direct_decode_eligible = true;
    driver.surfaces[6] = vpp_target;
    Require(BeginPicture(&context, 8, 6) == VA_STATUS_SUCCESS,
            "begin clean VPP target before testing excluded source");
    VAProcPipelineParameterBuffer pipeline = {};
    pipeline.surface = 2;
    pipeline.rotation_state = VA_ROTATION_NONE;
    Buffer pipeline_buffer;
    pipeline_buffer.context = 8;
    pipeline_buffer.type = VAProcPipelineParameterBufferType;
    pipeline_buffer.element_size = sizeof(pipeline);
    pipeline_buffer.elements = 1;
    pipeline_buffer.data.resize(sizeof(pipeline));
    memcpy(pipeline_buffer.data.data(), &pipeline, sizeof(pipeline));
    driver.buffers[1] = std::move(pipeline_buffer);
    VABufferID pipeline_id = 1;
    Require(RenderPicture(&context, 8, &pipeline_id, 1) ==
                VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT,
            "VPP cannot substitute the exact decode behind remapped pixels");

    decode->pending[200000] = 2;
    Require(decode->replay.Append(200000, true, {1}),
            "append late exact experimental output");
    SendNext(&decode->replay, 200000);
    std::vector<uint8_t> yuy2(16 * 16 * 2, 128);
    for (size_t byte = 0; byte < yuy2.size(); byte += 2)
      yuy2[byte] = 90;
    BC_DTS_PROC_OUT output = {};
    output.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    output.PicInfo.timeStamp = 200000;
    output.PicInfo.width = output.PicInfo.height = 16;
    output.Ybuff = yuy2.data();
    output.YBuffDoneSz = yuy2.size() / 4;
    Require(ProcessDecodedOutput(&driver, decode.get(), output) ==
                    VA_STATUS_SUCCESS &&
                requested->planes[0][0] == 40 && !requested->ready &&
                ExperimentalPresentationReadable(&driver, requested.get()) &&
                requested_picture->ready &&
                requested_picture->planes[0][0] == 90 &&
                requested_picture->experimental_ready_queued,
            "late exact output stays private and cannot overwrite presentation");

    VASurfaceStatus surface_status = VASurfaceRendering;
    Require(SyncSurface2(&context, 2, 0) == VA_STATUS_SUCCESS &&
                QuerySurfaceStatus(&context, 2, &surface_status) ==
                    VA_STATUS_SUCCESS &&
                surface_status == VASurfaceReady &&
                requested->planes[0][0] == 40,
            "repeated passive sync observes the latched payload without recopy");

    const int backing = TraceBackingFd("crystalhd-remap-backing",
                                       requested->storage.size());
    requested->object_fds.push_back(backing);
    requested->object_sizes.push_back(requested->storage.size());
    for (unsigned int attempt = 0; attempt < 2; ++attempt) {
      VADRMPRIMESurfaceDescriptor descriptor = {};
      Require(ExportSurfaceHandle(&context, 2,
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_READ_ONLY, &descriptor) ==
                      VA_STATUS_SUCCESS &&
                  requested->experimental_read_export_armed &&
                  ExperimentalPresentationReadable(&driver, requested.get()),
              "read-only export keeps one immutable remapped payload");
      ClosePrimeDescriptor(&descriptor);
    }
    VADRMPRIMESurfaceDescriptor rejected = {};
    Require(ExportSurfaceHandle(&context, 2,
                                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                VA_EXPORT_SURFACE_WRITE_ONLY, &rejected) ==
                VA_STATUS_ERROR_INVALID_PARAMETER,
            "writable export cannot expose an active remapped payload");

    probe_poll_calls = 0;
    probe_poll_results = {{1, POLLOUT, 0}};
    decode->next_submission_ordinal = 3;
    Require(BeginPicture(&context, 7, 2) == VA_STATUS_SUCCESS &&
                probe_poll_results.empty() && probe_poll_calls == 1 &&
                !ExperimentalPresentationReadable(&driver, requested.get()) &&
                requested->experimental_write_lease &&
                ExperimentalWriteReservationPending(requested.get()) &&
                requested->planes[0][0] == 40,
            "all-ready reuse fence releases the frozen public slot once");
    VADRMPRIMESurfaceDescriptor pending_export = {};
    Require(ExportSurfaceHandle(&context, 2,
                                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                VA_EXPORT_SURFACE_READ_ONLY,
                                &pending_export) ==
                VA_STATUS_ERROR_SURFACE_BUSY,
            "Begin-to-submit reservation rejects a concurrent read export");
  }
  {
    Driver driver{-1, trace_fd};
    auto decode = std::make_shared<DecodeContext>();
    decode->id = 12;
    decode->generation = 2;
    decode->profile = VAProfileH264Main;
    decode->width = decode->height = 16;
    decode->experimental_ready_remap = true;
    decode->next_submission_ordinal = 2;
    driver.contexts[12] = decode;
    auto picture = std::make_shared<Surface>();
    auto surface = std::make_shared<Surface>();
    for (const auto &frame : {picture, surface}) {
      Require(frame->AllocateInternal(nullptr, -1, 16, 16,
                                      VA_FOURCC_NV12),
              "allocate Begin-to-submit late-output fixture");
      memset(frame->planes[0], 99, frame->storage.size());
    }
    picture->id = surface->id = 5;
    picture->expected_timestamp = surface->expected_timestamp = 100000;
    picture->decode_identity = surface->decode_identity = 70;
    picture->trace_decode_identity = surface->trace_decode_identity = 70;
    picture->trace_context = surface->trace_context = 12;
    picture->trace_generation = surface->trace_generation = 2;
    picture->trace_submission_ordinal =
        surface->trace_submission_ordinal = 1;
    surface->rt_format = VA_RT_FORMAT_YUV420;
    surface->direct_decode_eligible = true;
    surface->decode_picture = picture;
    decode->decoded_frames[100000] = picture;
    decode->surface_timestamps[surface.get()] = 100000;
    decode->pending[100000] = 5;
    driver.surfaces[5] = surface;
    Require(decode->replay.Append(100000, true, {1}),
            "append Begin-to-submit late output");
    SendNext(&decode->replay, 100000);
    VADriverContext context = {};
    context.pDriverData = &driver;
    Require(BeginPicture(&context, 12, 5) == VA_STATUS_SUCCESS &&
                ExperimentalWriteReservationPending(surface.get()),
            "next submission reserves but does not yet rebind the public backing");
    std::vector<uint8_t> yuy2(16 * 16 * 2, 128);
    for (size_t byte = 0; byte < yuy2.size(); byte += 2)
      yuy2[byte] = 90;
    BC_DTS_PROC_OUT output = {};
    output.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    output.PicInfo.timeStamp = 100000;
    output.PicInfo.width = output.PicInfo.height = 16;
    output.Ybuff = yuy2.data();
    output.YBuffDoneSz = yuy2.size() / 4;
    Require(ProcessDecodedOutput(&driver, decode.get(), output) ==
                    VA_STATUS_SUCCESS &&
                surface->planes[0][0] == 99 && !surface->ready &&
                !surface->failed && picture->ready &&
                picture->planes[0][0] == 90 &&
                picture->experimental_ready_queued &&
                ExperimentalWriteReservationPending(surface.get()),
            "old output arriving after Begin stays private and preserves the next lease");
    surface->expected_timestamp = 200000;
    surface->trace_decode_identity = surface->decode_identity = 71;
    surface->trace_submission_ordinal = 2;
    Require(ExperimentalWriteLeaseMatches(*decode, *surface),
            "accepted next identity activates only its own reserved write lease");
  }
  {
    Driver driver{-1, trace_fd};
    auto decode = std::make_shared<DecodeContext>();
    decode->id = 9;
    decode->generation = 4;
    decode->experimental_ready_remap = true;
    decode->next_submission_ordinal = 4;
    driver.contexts[9] = decode;
    auto surface = std::make_shared<Surface>();
    Require(surface->AllocateInternal(nullptr, -1, 16, 16,
                                      VA_FOURCC_NV12),
            "allocate busy-reader remap surface");
    surface->id = 3;
    surface->direct_decode_eligible = true;
    surface->expected_timestamp = 300000;
    surface->decode_identity = surface->trace_decode_identity = 50;
    surface->trace_context = 9;
    surface->trace_generation = 4;
    surface->trace_submission_ordinal = 3;
    surface->experimental_presentation = true;
    surface->presentation_payload_token = 200000;
    surface->presentation_payload_decode_identity = 49;
    surface->presentation_payload_submission_ordinal = 2;
    surface->object_fds.push_back(
        TraceBackingFd("crystalhd-remap-busy", surface->storage.size()));
    surface->object_sizes.push_back(surface->storage.size());
    surface->experimental_read_export = TraceFields(nullptr, surface.get());
    surface->experimental_read_export.operation = 1;
    surface->experimental_read_export_armed = true;
    driver.surfaces[3] = surface;
    VADriverContext context = {};
    context.pDriverData = &driver;
    probe_poll_calls = 0;
    probe_poll_results = {{-1, 0, EIO}, {0, 0, 0}, {0, 0, 0},
                          {1, POLLOUT, 0}};
    Require(BeginPicture(&context, 9, 3) ==
                    VA_STATUS_ERROR_OPERATION_FAILED &&
                probe_poll_results.size() == 3 && probe_poll_calls == 1 &&
                ExperimentalPresentationReadable(&driver, surface.get()) &&
                surface->experimental_read_export_armed &&
                !surface->experimental_write_lease,
            "reuse probe error retains the frozen ownership arm");
    Require(BeginPicture(&context, 9, 3) == VA_STATUS_ERROR_HW_BUSY &&
                probe_poll_results.size() == 2 && probe_poll_calls == 2 &&
                ExperimentalPresentationReadable(&driver, surface.get()) &&
                surface->experimental_read_export_armed &&
                !surface->experimental_write_lease,
            "busy exported reader keeps the presentation frozen and fails closed");
    Require(BeginPicture(&context, 9, 3) == VA_STATUS_ERROR_HW_BUSY &&
                probe_poll_results.size() == 1 && probe_poll_calls == 3 &&
                ExperimentalPresentationReadable(&driver, surface.get()) &&
                surface->experimental_read_export_armed &&
                !surface->experimental_write_lease,
            "busy reader retries cannot bypass the experimental reuse probe");
    Require(BeginPicture(&context, 9, 3) == VA_STATUS_SUCCESS &&
                probe_poll_results.empty() && probe_poll_calls == 4 &&
                !ExperimentalPresentationReadable(&driver, surface.get()) &&
                !surface->experimental_read_export_armed &&
                surface->experimental_write_lease,
            "writable reader retry releases the frozen presentation exactly once");
  }
  {
    Driver driver{-1, trace_fd};
    auto decode = std::make_shared<DecodeContext>();
    decode->id = 11;
    decode->generation = 5;
    decode->experimental_ready_remap = true;
    driver.contexts[11] = decode;
    auto surface = std::make_shared<Surface>();
    surface->expected_timestamp = 400000;
    surface->trace_context = 11;
    surface->trace_generation = 5;
    surface->trace_decode_identity = 61;
    surface->trace_submission_ordinal = 4;
    surface->experimental_presentation = true;
    surface->presentation_payload_token = 300000;
    surface->presentation_payload_decode_identity = 60;
    surface->presentation_payload_submission_ordinal = 3;
    surface->experimental_read_export = TraceFields(nullptr, surface.get());
    surface->experimental_read_export_armed = true;
    driver.surfaces[4] = surface;
    Require(ExperimentalPresentationReadable(&driver, surface.get()),
            "current experimental generation exposes its latched payload");
    decode->closing = true;
    ClearTraceReadExportsForContext(&driver, 11, 5);
    Require(!ExperimentalPresentationReadable(&driver, surface.get()) &&
                surface->experimental_presentation &&
                surface->experimental_read_export_armed,
            "context retirement keeps an exported presentation frozen");
    auto reserved = std::make_shared<Surface>();
    reserved->experimental_write_lease = true;
    reserved->experimental_write_context = 11;
    reserved->experimental_write_generation = 5;
    reserved->experimental_write_submission_ordinal = 5;
    driver.surfaces[5] = reserved;
    ClearTraceReadExportsForContext(&driver, 11, 5);
    Require(!reserved->experimental_write_lease,
            "context retirement cancels a pre-submit write reservation");
    driver.contexts.erase(11);
    Require(!ExperimentalPresentationReadable(&driver, surface.get()) &&
                surface->experimental_presentation &&
                surface->experimental_read_export_armed,
            "destroyed context cannot expose or overwrite a frozen old payload");
  }
  const std::string trace = ReadTraceFd(trace_fd);
  close(trace_fd);
  const std::string commit = FindTraceEvent(trace, "presentation_commit");
  Require(TraceFieldIs(commit, "context", 7) &&
              TraceFieldIs(commit, "generation", 3) &&
              TraceFieldIs(commit, "surface", 2) &&
              TraceFieldIs(commit, "token", 200000) &&
              TraceFieldIs(commit, "decode_identity", 42) &&
              TraceFieldIs(commit, "submission_ordinal", 2) &&
              TraceFieldIs(commit, "payload_token", 100000) &&
              TraceFieldIs(commit, "payload_decode_identity", 41) &&
              TraceFieldIs(commit, "payload_submission_ordinal", 1) &&
              TraceSignedFieldIs(commit, "submission_drift", 1),
          "remap trace keeps requested and payload identities separate");
}

static void ExperimentalReadyRemapGate() {
  SavedEnvironment live("CRYSTALHD_VAAPI_LIVE_H264");
  SavedEnvironment low_latency("CRYSTALHD_VAAPI_LOW_LATENCY_H264");
  SavedEnvironment remap("CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP");
  const auto create = [](const char *live_value, const char *low_value,
                         const char *remap_value, VAProfile profile) {
    const auto assign = [](const char *name, const char *value) {
      if (value != nullptr)
        setenv(name, value, 1);
      else
        unsetenv(name);
    };
    assign("CRYSTALHD_VAAPI_LIVE_H264", live_value);
    assign("CRYSTALHD_VAAPI_LOW_LATENCY_H264", low_value);
    assign("CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP", remap_value);
    Driver driver{-1};
    driver.configs[1] = {profile, VAEntrypointVLD};
    VADriverContext context = {};
    context.pDriverData = &driver;
    VAContextID context_id = VA_INVALID_ID;
    Require(CreateContext(&context, 1, 16, 16, 0, nullptr, 0,
                          &context_id) == VA_STATUS_SUCCESS,
            "create environment-gated remap context");
    return driver.contexts.at(context_id)->experimental_ready_remap;
  };

  Require(!create(nullptr, nullptr, nullptr, VAProfileH264Main) &&
              !create(nullptr, nullptr, "1", VAProfileH264Main) &&
              !create("1", "1", "0", VAProfileH264Main) &&
              !create("1", "1", "yes", VAProfileH264Main) &&
              create("1", "1", "1", VAProfileH264Main) &&
              !create("1", "1", "1", VAProfileMPEG2Main),
          "remap is exact opt-in and restricted to live low-latency H.264");

  Driver driver{-1};
  DecodeContext decode;
  decode.experimental_ready_remap = true;
  decode.have_picture = true;
  decode.target = 1;
  decode.slices.resize(1);
  decode.slices.front().slice_type = 1;
  decode.slice_data.push_back({1});
  Require(SubmitPicture(&driver, &decode, VAProfileH264Main) ==
              VA_STATUS_ERROR_UNSUPPORTED_PROFILE,
          "experimental remap fails closed before accepting a B slice");
}

static void ExperimentalReadyRemapFailClosedRaces() {
  {
    Driver driver{-1};
    auto decode = std::make_shared<DecodeContext>();
    decode->id = 4;
    decode->generation = 2;
    decode->experimental_ready_remap = true;
    driver.contexts[4] = decode;
    auto payload = std::make_shared<Surface>();
    auto requested = std::make_shared<Surface>();
    for (const auto &surface : {payload, requested})
      Require(surface->AllocateInternal(nullptr, -1, 16, 16,
                                        VA_FOURCC_NV12),
              "allocate SyncSurface presentation-path pixels");
    payload->expected_timestamp = payload->frame_timestamp = 100000;
    payload->ready = true;
    payload->decode_identity = payload->trace_decode_identity = 21;
    payload->trace_context = 4;
    payload->trace_generation = 2;
    payload->trace_submission_ordinal = 1;
    memset(payload->planes[0], 33, payload->storage.size());
    requested->id = 2;
    requested->direct_decode_eligible = true;
    requested->expected_timestamp = 200000;
    requested->decode_identity = requested->trace_decode_identity = 22;
    requested->trace_context = 4;
    requested->trace_generation = 2;
    requested->trace_submission_ordinal = 2;
    requested->experimental_write_lease = true;
    requested->experimental_write_context = 4;
    requested->experimental_write_generation = 2;
    requested->experimental_write_submission_ordinal = 2;
    decode->decoded_frames[payload->expected_timestamp] = payload;
    driver.surfaces[2] = requested;
    QueueExperimentalReady(&driver, decode.get(), payload);
    VADriverContext context = {};
    context.pDriverData = &driver;
    Require(SyncSurface2(&context, 2, 1000000) == VA_STATUS_SUCCESS &&
                ExperimentalPresentationReadable(&driver, requested.get()) &&
                requested->planes[0][0] == 33 &&
                requested->presentation_payload_token == 100000 &&
                !requested->ready && !requested->experimental_write_lease &&
                decode->experimental_ready.empty() &&
                decode->decoded_frames.count(100000) == 0,
            "SyncSurface production path selects, copies, and commits one queued READY payload");
  }

  {
    Driver driver{-1};
    auto experiment = std::make_shared<DecodeContext>();
    experiment->id = 7;
    experiment->generation = 1;
    experiment->experimental_ready_remap = true;
    driver.contexts[7] = experiment;
    auto vpp = std::make_shared<DecodeContext>();
    vpp->id = 8;
    vpp->video_process = true;
    driver.contexts[8] = vpp;

    auto source = std::make_shared<Surface>();
    auto target = std::make_shared<Surface>();
    for (const auto &surface : {source, target})
      Require(surface->AllocateInternal(nullptr, -1, 16, 16,
                                        VA_FOURCC_NV12),
              "allocate VPP ownership-race surfaces");
    source->id = 1;
    source->ready = true;
    source->expected_timestamp = source->frame_timestamp = 100000;
    target->id = 2;
    target->ready = true;
    target->planes[0][0] = 47;
    driver.surfaces[1] = source;
    driver.surfaces[2] = target;
    VADriverContext context = {};
    context.pDriverData = &driver;
    Require(BeginPicture(&context, 8, 2) == VA_STATUS_SUCCESS,
            "VPP begins before an experimental decoder takes the target");

    vpp->vpp_source = 1;
    vpp->vpp_frame = source;
    vpp->have_vpp_parameters = true;
    target->trace_context = 7;
    target->trace_generation = 1;
    target->trace_submission_ordinal = 2;
    target->trace_decode_identity = target->decode_identity = 12;
    target->expected_timestamp = 200000;
    target->experimental_presentation = true;
    target->presentation_payload_token = 100000;
    target->presentation_payload_decode_identity = 11;
    target->presentation_payload_submission_ordinal = 1;
    target->experimental_read_export = TraceFields(nullptr, target.get());
    target->experimental_read_export_armed = true;
    const uint64_t identity = target->decode_identity;
    Require(EndPicture(&context, 8) ==
                    VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT &&
                vpp->target == VA_INVALID_SURFACE &&
                vpp->vpp_source == VA_INVALID_SURFACE &&
                !vpp->vpp_frame && !vpp->have_vpp_parameters &&
                target->experimental_presentation &&
                target->experimental_read_export_armed &&
                target->decode_identity == identity && !target->failed &&
                target->planes[0][0] == 47,
            "stale VPP End cancels without poisoning or overwriting a newer exported presentation");

    auto early_vpp = std::make_shared<DecodeContext>();
    early_vpp->id = 10;
    early_vpp->video_process = true;
    driver.contexts[10] = early_vpp;
    auto early_target = std::make_shared<Surface>();
    Require(early_target->AllocateInternal(nullptr, -1, 16, 16,
                                           VA_FOURCC_NV12),
            "allocate early VPP-failure race target");
    early_target->id = 3;
    early_target->ready = true;
    early_target->planes[0][0] = 53;
    driver.surfaces[3] = early_target;
    Require(BeginPicture(&context, 10, 3) == VA_STATUS_SUCCESS,
            "VPP begins before an early-failure experimental takeover");
    early_target->trace_context = 7;
    early_target->trace_generation = 1;
    early_target->trace_submission_ordinal = 3;
    early_target->trace_decode_identity = early_target->decode_identity = 13;
    early_target->expected_timestamp = 300000;
    early_target->experimental_presentation = true;
    early_target->presentation_payload_token = 200000;
    early_target->presentation_payload_decode_identity = 12;
    early_target->presentation_payload_submission_ordinal = 2;
    early_target->experimental_read_export_armed = true;
    Require(EndPicture(&context, 10) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                early_vpp->target == VA_INVALID_SURFACE &&
                !early_target->failed &&
                early_target->experimental_presentation &&
                early_target->experimental_read_export_armed &&
                early_target->decode_identity == 13 &&
                early_target->planes[0][0] == 53,
            "early stale VPP failure cannot poison a newer experimental owner");
  }

  {
    DecodeIoMock io;
    io.allow_device_open = true;
    io.report_bcm70012 = true;
    io.fail_set_input_format = true;
    MockDecodeScope scope(io);
    DecodeContext decode;
    decode.profile = VAProfileH264Main;
    decode.width = decode.height = 16;
    decode.live_h264 = true;
    decode.low_latency_h264 = true;
    decode.experimental_ready_remap = true;
    Surface target;
    target.experimental_write_lease = true;
    target.experimental_write_context = 7;
    target.experimental_write_generation = 1;
    target.experimental_write_submission_ordinal = 1;
    Require(OpenDecoderForSubmit(&decode, &target, 16, 16) ==
                    VA_STATUS_ERROR_OPERATION_FAILED &&
                decode.is_70012 && !decode.low_latency_h264 &&
                !decode.experimental_ready_remap &&
                !target.experimental_write_lease &&
                io.open_attempts == 1 && !io.invalid &&
                std::count(io.events.begin(), io.events.end(),
                           "close-device") == 1,
            "BCM70012 discovery releases an experimental reservation even when later decoder setup fails");
  }

  {
    Surface exported;
    exported.trace_read_export_armed = true;
    exported.experimental_read_export_armed = true;
    exported.experimental_presentation = true;
    RetireFailedExperimentalState(&exported);
    Require(!exported.trace_read_export_armed &&
                exported.experimental_read_export_armed &&
                exported.experimental_presentation,
            "decode failure preserves an already exported immutable presentation");
    Surface unexported;
    unexported.experimental_write_lease = true;
    unexported.experimental_presentation = true;
    RetireFailedExperimentalState(&unexported);
    Require(!unexported.experimental_write_lease &&
                !unexported.experimental_presentation,
            "decode failure retires unexported experimental state immediately");
  }

  {
    TraceEnvironment environment;
    const int trace_fd =
        memfd_create("crystalhd-retired-reexport-trace", MFD_CLOEXEC);
    Require(trace_fd >= 0, "allocate retired re-export trace sink");
    {
      Driver driver{-1, trace_fd};
      auto surface = std::make_shared<Surface>();
      Require(surface->AllocateInternal(nullptr, -1, 16, 16,
                                        VA_FOURCC_NV12),
              "allocate retired exact export surface");
      surface->id = 3;
      surface->ready = true;
      surface->expected_timestamp = surface->frame_timestamp = 200000;
      surface->trace_context = 7;
      surface->trace_generation = 1;
      surface->trace_submission_ordinal = 2;
      surface->decode_identity = surface->trace_decode_identity = 22;
      surface->object_fds.push_back(
          TraceBackingFd("crystalhd-retired-reexport", surface->storage.size()));
      surface->object_sizes.push_back(surface->storage.size());
      surface->experimental_read_export = TraceFields(nullptr, surface.get());
      surface->experimental_read_export_armed = true;
      driver.surfaces[3] = surface;
      VADriverContext context = {};
      context.pDriverData = &driver;
      VADRMPRIMESurfaceDescriptor descriptor = {};
      Require(ExportSurfaceHandle(
                  &context, 3, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                  VA_EXPORT_SURFACE_READ_ONLY |
                      VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                  &descriptor) == VA_STATUS_SUCCESS &&
                  surface->trace_read_export_armed &&
                  surface->experimental_read_export_armed,
              "trace-only re-export cannot revoke a retired external-reader guard");
      for (unsigned int object = 0; object < descriptor.num_objects; ++object)
        close(descriptor.objects[object].fd);
      auto ordinary = std::make_shared<DecodeContext>();
      ordinary->id = 9;
      driver.contexts[9] = ordinary;
      Require(BeginPicture(&context, 9, 3) ==
                      VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT &&
                  surface->experimental_read_export_armed,
              "ordinary reuse remains fail-closed after retired re-export");
      ordinary->profile = VAProfileMPEG2Main;
      ordinary->target = 3;
      Require(SubmitPicture(&driver, ordinary.get(), VAProfileMPEG2Main) ==
                      VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT &&
                  surface->experimental_read_export_armed &&
                  surface->trace_decode_identity == 22,
              "ordinary End revalidates experimental ownership acquired after Begin");
    }
    close(trace_fd);
  }
}

static void ExperimentalReadyRemapBounds() {
  Driver driver{-1};
  auto decode = std::make_shared<DecodeContext>();
  decode->id = 5;
  decode->generation = 6;
  decode->experimental_ready_remap = true;
  driver.contexts[5] = decode;
  std::vector<std::shared_ptr<Surface>> pictures;
  for (uint64_t ordinal = 1; ordinal <= 4; ++ordinal) {
    auto picture = std::make_shared<Surface>();
    Require(picture->AllocateInternal(nullptr, -1, 16, 16,
                                      VA_FOURCC_NV12),
            "allocate bounded remap picture");
    picture->id = static_cast<VASurfaceID>(ordinal);
    picture->expected_timestamp = picture->frame_timestamp = ordinal * 100000;
    picture->ready = true;
    picture->decode_identity = picture->trace_decode_identity = 100 + ordinal;
    picture->trace_context = 5;
    picture->trace_generation = 6;
    picture->trace_submission_ordinal = ordinal;
    decode->decoded_frames[picture->expected_timestamp] = picture;
    QueueExperimentalReady(&driver, decode.get(), picture);
    pictures.push_back(picture);
  }
  Require(decode->experimental_ready.size() ==
                  kExperimentalPresentationReadyLimit &&
              decode->decoded_frames.size() ==
                  kExperimentalPresentationReadyLimit &&
              !pictures.front()->experimental_ready_queued &&
              decode->decoded_frames.count(100000) == 0,
          "READY storage evicts exactly the oldest completed private picture");

  auto requested = std::make_shared<Surface>();
  Require(requested->AllocateInternal(nullptr, -1, 16, 16,
                                      VA_FOURCC_NV12),
          "allocate bounded remap request");
  requested->expected_timestamp = 500000;
  requested->trace_context = 5;
  requested->trace_generation = 6;
  requested->trace_submission_ordinal = 5;
  requested->experimental_write_lease = true;
  requested->experimental_write_context = 5;
  requested->experimental_write_generation = 6;
  requested->experimental_write_submission_ordinal = 5;
  const uint64_t now = MonotonicNowNs();
  Require(SelectExperimentalReady(*decode, *requested, now) == pictures[3],
          "selection accepts only the immediately preceding READY ordinal");
  pictures[3]->experimental_ready_at_ns =
      now - kExperimentalPresentationMaxAgeNs - 1;
  Require(!SelectExperimentalReady(*decode, *requested, now),
          "selection rejects a candidate beyond the monotonic age bound");
  pictures[3]->experimental_ready_at_ns = now;
  requested->trace_generation = 7;
  Require(!SelectExperimentalReady(*decode, *requested, now),
          "selection rejects a cross-generation READY picture");
  requested->trace_generation = 6;
  requested->trace_submission_ordinal = 6;
  Require(!SelectExperimentalReady(*decode, *requested, now),
          "selection rejects drift greater than one submission");

  requested->id = 9;
  requested->trace_submission_ordinal = 5;
  requested->trace_decode_identity = requested->decode_identity = 209;
  driver.surfaces[9] = requested;
  for (VASurfaceID id : {7U}) {
    auto active = std::make_shared<Surface>();
    active->id = id;
    active->expected_timestamp = static_cast<uint64_t>(id) * 100000;
    active->decode_identity = active->trace_decode_identity = 300 + id;
    active->trace_context = 5;
    active->trace_generation = 6;
    active->trace_submission_ordinal = id;
    active->experimental_presentation = true;
    active->presentation_payload_token = (id - 1) * 100000;
    active->presentation_payload_decode_identity = 299 + id;
    active->presentation_payload_submission_ordinal = id - 1;
    if (id == 7) {
      // A failed presentation can remain frozen for an already exported GPU
      // reader. It is unreadable, but must still consume one bounded slot.
      active->failed = true;
      active->experimental_read_export_armed = true;
    }
    driver.surfaces[id] = active;
  }
  bool committed = false;
  Require(TryExperimentalPresentation(&driver, requested, &committed) ==
                  VA_STATUS_SUCCESS &&
              committed && requested->experimental_presentation &&
              !requested->experimental_write_lease &&
              requested->presentation_payload_token == 400000 &&
              !pictures[3]->experimental_ready_queued &&
              decode->decoded_frames.count(400000) == 0,
          "one active public presentation slot permits a second bounded slot");

  auto next_candidate = std::make_shared<Surface>();
  Require(next_candidate->AllocateInternal(nullptr, -1, 16, 16,
                                            VA_FOURCC_NV12),
          "allocate candidate behind the third active slot");
  next_candidate->expected_timestamp = next_candidate->frame_timestamp =
      500000;
  next_candidate->ready = true;
  next_candidate->decode_identity =
      next_candidate->trace_decode_identity = 105;
  next_candidate->trace_context = 5;
  next_candidate->trace_generation = 6;
  next_candidate->trace_submission_ordinal = 5;
  decode->decoded_frames[500000] = next_candidate;
  QueueExperimentalReady(&driver, decode.get(), next_candidate);

  auto next_requested = std::make_shared<Surface>();
  Require(next_requested->AllocateInternal(nullptr, -1, 16, 16,
                                            VA_FOURCC_NV12),
          "allocate request behind the third active slot");
  next_requested->id = 10;
  next_requested->expected_timestamp = 600000;
  next_requested->trace_context = 5;
  next_requested->trace_generation = 6;
  next_requested->trace_submission_ordinal = 6;
  next_requested->trace_decode_identity =
      next_requested->decode_identity = 210;
  next_requested->experimental_write_lease = true;
  next_requested->experimental_write_context = 5;
  next_requested->experimental_write_generation = 6;
  next_requested->experimental_write_submission_ordinal = 6;
  driver.surfaces[10] = next_requested;
  committed = true;
  const auto unchanged_pixels = next_requested->storage;
  Require(TryExperimentalPresentation(&driver, next_requested, &committed) ==
                  VA_STATUS_SUCCESS &&
              !committed && !next_requested->experimental_presentation &&
              next_requested->experimental_write_lease &&
              next_requested->storage == unchanged_pixels &&
              next_candidate->experimental_ready_queued &&
              decode->decoded_frames.at(500000) == next_candidate &&
              std::find(decode->experimental_ready.begin(),
                        decode->experimental_ready.end(), next_candidate) !=
                  decode->experimental_ready.end(),
          "two active public presentation slots force exact fallback");
}

static void DirectOutputOwnershipAndExport() {
  {
    OutputFixture fixture;
    fixture.Direct();
    fixture.public_frame->vpp_writers = 1;
    Require(fixture.Process() == VA_STATUS_SUCCESS &&
                fixture.private_frame->ready && !fixture.private_frame->direct_backing &&
                fixture.public_frame->planes[0][0] == 99 && !fixture.public_frame->ready,
            "late output cannot write a direct target acquired by fenced VPP");
  }
  for (uint64_t failure : {UINT64_MAX, uint64_t(DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE),
                           uint64_t(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE)}) {
    OutputFixture fixture(32, 32);
    fixture.Direct();
    mock_sync_fd = memfd_create("crystalhd-direct-cpu-test", MFD_CLOEXEC);
    Require(mock_sync_fd >= 0, "allocate mock direct DMA ownership fd");
    fixture.public_frame->object_fds.push_back(mock_sync_fd);
    mock_failed_sync_flags = failure;
    mock_sync_calls.clear();
    const VAStatus result = fixture.Process();
    Require(result == (failure == UINT64_MAX ? VA_STATUS_SUCCESS :
                                             VA_STATUS_ERROR_DECODING_ERROR),
            "direct output propagates CPU ownership failures");
    Require(fixture.private_frame->storage.empty(),
            "direct output never allocates or copies private pixels");
    if (failure == UINT64_MAX) {
      Require(mock_sync_calls == std::vector<uint64_t>{DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE,
                                                       DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE},
              "direct completion performs exactly one public CPU write transaction");
      Require(fixture.public_frame->planes[0][0] == 40 &&
                  fixture.public_frame->planes[0][16] == 16 &&
                  fixture.public_frame->planes[1][16] == 128,
              "direct conversion initializes larger allocation padding");
      mock_failed_sync_flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
      Require(!PromoteDecodedPicture(fixture.private_frame) &&
                  fixture.private_frame->failed && !fixture.private_frame->ready,
              "failed snapshot CPU read cannot publish a captured picture");
    } else {
      Require(fixture.private_frame->failed && fixture.public_frame->failed &&
                  !fixture.private_frame->ready && !fixture.public_frame->ready,
              "failed direct output poisons both picture identities");
    }
    mock_sync_fd = -1;
  }

  {
    OutputFixture fixture;
    fixture.Direct();
    mock_sync_fd = memfd_create("crystalhd-direct-export-test", MFD_CLOEXEC);
    Require(mock_sync_fd >= 0, "allocate read-only export mock fd");
    mock_failed_sync_flags = UINT64_MAX;
    fixture.public_frame->object_fds.push_back(mock_sync_fd);
    fixture.public_frame->object_sizes.push_back(fixture.public_frame->storage.size());
    Require(fixture.Process() == VA_STATUS_SUCCESS, "complete before read-only export");
    VADriverContext context = {};
    context.pDriverData = &fixture.driver;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
      VADRMPRIMESurfaceDescriptor descriptor = {};
      Require(ExportSurfaceHandle(&context, 1,
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_READ_ONLY, &descriptor) ==
                  VA_STATUS_SUCCESS,
              "repeat explicit read-only export of direct backing");
      Require(fixture.private_frame->direct_backing == fixture.public_frame &&
                  fixture.private_frame->storage.empty() &&
                  fixture.public_frame->layout.valid &&
                  fixture.public_frame->direct_decode_eligible,
              "read-only export keeps direct decode ownership eligible");
      for (unsigned i = 0; i < descriptor.num_objects; ++i)
        close(descriptor.objects[i].fd);
    }
    mock_sync_fd = -1;
  }

  {
    OutputFixture fixture;
    fixture.Direct();
    fixture.driver.next_surface = 2;
    mock_sync_fd = memfd_create("crystalhd-imported-alias-test", MFD_CLOEXEC);
    Require(mock_sync_fd >= 0, "allocate imported-alias mock fd");
    mock_failed_sync_flags = UINT64_MAX;
    fixture.public_frame->object_fds.push_back(mock_sync_fd);
    fixture.public_frame->object_sizes.push_back(fixture.public_frame->storage.size());
    Require(ftruncate(mock_sync_fd, fixture.public_frame->storage.size()) == 0 &&
                fixture.Process() == VA_STATUS_SUCCESS,
            "complete direct picture before read-only alias export");
    VADriverContext context = {};
    context.pDriverData = &fixture.driver;
    VADRMPRIMESurfaceDescriptor descriptor = {};
    Require(ExportSurfaceHandle(&context, 1,
                                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                VA_EXPORT_SURFACE_READ_ONLY, &descriptor) ==
                VA_STATUS_SUCCESS,
            "export direct picture read-only before alias import");
    VASurfaceAttrib attributes[3] = {};
    attributes[0].type = VASurfaceAttribMemoryType;
    attributes[0].value.type = VAGenericValueTypeInteger;
    attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attributes[1].type = VASurfaceAttribPixelFormat;
    attributes[1].value.type = VAGenericValueTypeInteger;
    attributes[1].value.value.i = VA_FOURCC_NV12;
    attributes[2].type = VASurfaceAttribExternalBufferDescriptor;
    attributes[2].value.type = VAGenericValueTypePointer;
    attributes[2].value.value.p = &descriptor;
    VASurfaceID alias = VA_INVALID_SURFACE;
    Require(CreateSurfaces2(&context, VA_RT_FORMAT_YUV420,
                            fixture.public_frame->width,
                            fixture.public_frame->height, &alias, 1,
                            attributes, 3) == VA_STATUS_SUCCESS && alias == 2,
            "import read-only export as a write-capable VA alias");
    Require(!fixture.private_frame->direct_backing &&
                !fixture.private_frame->storage.empty() &&
                !fixture.public_frame->direct_decode_eligible &&
                fixture.driver.surfaces.at(alias)->backing_owner == 1 &&
                !fixture.driver.surfaces.at(alias)->direct_decode_eligible,
            "alias import snapshots current picture and disables direct reuse");
    fixture.public_frame->planes[0][0] = 90;
    Require(fixture.private_frame->planes[0][0] == 40,
            "imported alias cannot mutate the retained direct picture");
    for (unsigned i = 0; i < descriptor.num_objects; ++i)
      close(descriptor.objects[i].fd);
    mock_sync_fd = -1;
  }

  struct ExportCase {
    uint32_t flags;
    bool pending;
  };
  for (const ExportCase test : {
           ExportCase{VA_EXPORT_SURFACE_WRITE_ONLY, true},
           ExportCase{VA_EXPORT_SURFACE_WRITE_ONLY, false},
           ExportCase{VA_EXPORT_SURFACE_READ_WRITE, false},
           ExportCase{0, false}}) {
    OutputFixture fixture;
    fixture.Direct();
    mock_sync_fd = memfd_create("crystalhd-writable-export-test", MFD_CLOEXEC);
    Require(mock_sync_fd >= 0, "allocate writable export mock fd");
    mock_failed_sync_flags = UINT64_MAX;
    fixture.public_frame->object_fds.push_back(mock_sync_fd);
    fixture.public_frame->object_sizes.push_back(fixture.public_frame->storage.size());
    if (!test.pending)
      Require(fixture.Process() == VA_STATUS_SUCCESS,
              "complete before writable export");
    VADriverContext context = {};
    context.pDriverData = &fixture.driver;
    VADRMPRIMESurfaceDescriptor descriptor = {};
    Require(ExportSurfaceHandle(&context, 1,
                                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                test.flags, &descriptor) == VA_STATUS_SUCCESS,
            "writable or unspecified export preserves private snapshot");
    Require(!fixture.private_frame->direct_backing &&
                !fixture.private_frame->storage.empty() &&
                !fixture.public_frame->direct_decode_eligible &&
                fixture.public_frame->layout.valid,
            "write exposure permanently disables direct borrowing");
    if (test.pending)
      Require(fixture.Process() == VA_STATUS_SUCCESS,
              "finish privately preserved pending picture");
    fixture.public_frame->planes[0][0] = 90;
    Require(fixture.private_frame->planes[0][0] == 40,
            "external write cannot mutate retained decode snapshot");
    for (unsigned i = 0; i < descriptor.num_objects; ++i)
      close(descriptor.objects[i].fd);
    mock_sync_fd = -1;
  }
}

static void RejectIncompleteGeometryAndInitializeAllocationPadding() {
  for (const auto &size : {std::pair<unsigned int, unsigned int>{32, 16},
                          {16, 32}, {15, 16}, {16, 15}}) {
    OutputFixture fixture(32, 32, size.first, size.second);
    Require(fixture.Process() == VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.public_frame->failed && !fixture.public_frame->ready &&
                fixture.public_frame->planes[0][0] == 99,
            "mismatched coded output cannot copy or complete a surface");
  }
  for (const auto &coded : {
           std::pair<unsigned int, unsigned int>{32, 16}, {16, 32}}) {
    OutputFixture fixture(16, 16, coded.first, coded.second);
    fixture.decoder.width = coded.first;
    fixture.decoder.height = coded.second;
    Require(fixture.Process() == VA_STATUS_ERROR_DECODING_ERROR &&
                !fixture.public_frame->ready,
            "destination missing a full macroblock cannot claim a complete picture");
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

static void StaleUnretainedOutputSkipsOnlyPixelMaterialization() {
  {
    OutputFixture fixture;
    mock_sync_fd = memfd_create("crystalhd-current-map-only-test", MFD_CLOEXEC);
    Require(mock_sync_fd >= 0, "allocate current map-only ownership fd");
    fixture.private_frame->object_fds.push_back(mock_sync_fd);
    std::weak_ptr<Surface> picture = fixture.private_frame;
    fixture.private_frame.reset();
    mock_failed_sync_flags = UINT64_MAX;
    mock_sync_calls.clear();
    Require(fixture.Process() == VA_STATUS_SUCCESS &&
                mock_sync_calls ==
                    std::vector<uint64_t>{DMA_BUF_SYNC_START |
                                              DMA_BUF_SYNC_WRITE,
                                          DMA_BUF_SYNC_END |
                                              DMA_BUF_SYNC_WRITE,
                                          DMA_BUF_SYNC_START |
                                              DMA_BUF_SYNC_READ,
                                          DMA_BUF_SYNC_END |
                                              DMA_BUF_SYNC_READ} &&
                !picture.expired() && picture.lock()->ready &&
                picture.lock()->planes[0][0] == 40 &&
                fixture.public_frame->ready &&
                fixture.public_frame->planes[0][0] == 40,
            "the current public identity converts even when its map is sole owner");
    mock_sync_fd = -1;
  }

  {
    OutputFixture fixture;
    fixture.public_frame->expected_timestamp = 2;
    fixture.public_frame->decode_picture = fixture.private_frame;
    mock_sync_fd =
        memfd_create("crystalhd-stale-weak-owner-test", MFD_CLOEXEC);
    Require(mock_sync_fd >= 0, "allocate stale weak-owner fd");
    fixture.private_frame->object_fds.push_back(mock_sync_fd);
    std::weak_ptr<Surface> picture = fixture.private_frame;
    fixture.private_frame.reset();
    mock_failed_sync_flags = UINT64_MAX;
    mock_sync_calls.clear();
    Require(fixture.Process() == VA_STATUS_SUCCESS &&
                fixture.decoder.pending.empty() &&
                fixture.decoder.replay.outstanding() == 0 &&
                fixture.decoder.decoded_frames.empty() && picture.expired() &&
                mock_sync_calls.empty() &&
                fixture.public_frame->planes[0][0] == 99 &&
                !fixture.public_frame->ready && !fixture.public_frame->failed,
            "a stale weak public owner cannot defeat the map-only discard");
    mock_sync_fd = -1;
  }

  {
    OutputFixture fixture;
    fixture.public_frame->expected_timestamp = 2;
    mock_sync_fd = memfd_create("crystalhd-captured-vpp-test", MFD_CLOEXEC);
    Require(mock_sync_fd >= 0, "allocate captured VPP ownership fd");
    fixture.private_frame->object_fds.push_back(mock_sync_fd);
    auto vpp = std::make_shared<DecodeContext>();
    vpp->video_process = true;
    vpp->vpp_frame = fixture.private_frame;
    Require(vpp->vpp_frame->vpp_readers == 0,
            "captured VPP picture precedes reader accounting");
    fixture.private_frame.reset();
    mock_failed_sync_flags = UINT64_MAX;
    mock_sync_calls.clear();
    Require(fixture.Process() == VA_STATUS_SUCCESS && vpp->vpp_frame->ready &&
                vpp->vpp_frame->frame_timestamp == 1 &&
                vpp->vpp_frame->planes[0][0] == 40 &&
                mock_sync_calls ==
                    std::vector<uint64_t>{DMA_BUF_SYNC_START |
                                              DMA_BUF_SYNC_WRITE,
                                          DMA_BUF_SYNC_END |
                                              DMA_BUF_SYNC_WRITE} &&
                fixture.decoder.pending.empty() &&
                fixture.decoder.replay.outstanding() == 0 &&
                fixture.decoder.decoded_frames.empty() &&
                fixture.public_frame->planes[0][0] == 99 &&
                !fixture.public_frame->ready && !fixture.public_frame->failed,
            "a captured pre-queue VPP picture still receives stale output pixels");
    mock_sync_fd = -1;
  }

  {
    OutputFixture fixture;
    fixture.public_frame->expected_timestamp = 2;
    mock_sync_fd = memfd_create("crystalhd-queued-vpp-test", MFD_CLOEXEC);
    Require(mock_sync_fd >= 0, "allocate queued VPP ownership fd");
    fixture.private_frame->object_fds.push_back(mock_sync_fd);
    PendingVpp queued;
    queued.source = fixture.private_frame;
    queued.source->vpp_readers = 1;
    fixture.private_frame.reset();
    mock_failed_sync_flags = UINT64_MAX;
    mock_sync_calls.clear();
    Require(fixture.Process() == VA_STATUS_SUCCESS && queued.source->ready &&
                queued.source->frame_timestamp == 1 &&
                queued.source->planes[0][0] == 40 &&
                mock_sync_calls ==
                    std::vector<uint64_t>{DMA_BUF_SYNC_START |
                                              DMA_BUF_SYNC_WRITE,
                                          DMA_BUF_SYNC_END |
                                              DMA_BUF_SYNC_WRITE} &&
                fixture.decoder.pending.empty() &&
                fixture.decoder.replay.outstanding() == 0 &&
                fixture.decoder.decoded_frames.empty() &&
                fixture.public_frame->planes[0][0] == 99 &&
                !fixture.public_frame->ready && !fixture.public_frame->failed,
            "a queued VPP picture still receives stale output pixels");
    mock_sync_fd = -1;
  }

  {
    OutputFixture fixture;
    fixture.public_frame->expected_timestamp = 2;
    mock_sync_fd = memfd_create("crystalhd-invalid-stale-test", MFD_CLOEXEC);
    Require(mock_sync_fd >= 0, "allocate invalid stale ownership fd");
    fixture.private_frame->object_fds.push_back(mock_sync_fd);
    std::weak_ptr<Surface> picture = fixture.private_frame;
    fixture.private_frame.reset();
    fixture.output.YBuffDoneSz = 0;
    mock_failed_sync_flags = UINT64_MAX;
    mock_sync_calls.clear();
    Require(fixture.Process() == VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.decoder.replay.failed() &&
                fixture.decoder.pending.empty() &&
                fixture.decoder.decoded_frames.empty() && picture.expired() &&
                mock_sync_calls.empty() &&
                fixture.public_frame->planes[0][0] == 99 &&
                !fixture.public_frame->ready && !fixture.public_frame->failed,
            "invalid stale output fails instead of becoming a silent discard");
    mock_sync_fd = -1;
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

static unsigned int PaddingOnlyCopiesMatchFullClear(unsigned int source_width,
                                                    unsigned int source_height,
                                                    unsigned int target_width,
                                                    unsigned int target_height,
                                                    bool is_70012,
                                                    unsigned int offset = UINT_MAX,
                                                    bool exact_source = false) {
  if (offset == UINT_MAX)
    offset = (source_width + source_height) % 4;
  Surface expected, actual, source;
  InitializeCopyTestSurface(&expected, target_width, target_height, offset);
  InitializeCopyTestSurface(&actual, target_width, target_height, offset);
  const unsigned int width = std::min(source_width, target_width);
  const unsigned int height = std::min(source_height, target_height);
  const unsigned int source_pitch = is_70012
      ? (source_width <= 720 ? 720 : source_width <= 1280 ? 1280 : 1920) * 2
      : source_width * 2;
  // An odd final pixel consumes only its luma byte. Leave no readable suffix
  // after the last byte the scalar oracle needs, even with a padded Link pitch.
  // ASan can then detect a vector load crossing the actual source allocation.
  const size_t source_bytes = exact_source
      ? offset + static_cast<size_t>(source_pitch) * (source_height - 1) +
            source_width * 2 - (source_width & 1U)
      : static_cast<size_t>(source_pitch) * source_height + 64;
  std::vector<uint8_t> yuy2(source_bytes);
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
  return reinterpret_cast<uintptr_t>(output.Ybuff) & 15U;
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

static void ExactBoundaryConversionByteEquivalence() {
  unsigned int source_alignments = 0;
  unsigned int cases = 0;
  for (unsigned int width : {1U, 2U, 3U, 15U, 16U, 17U, 31U, 32U,
                             33U, 63U, 64U, 65U}) {
    for (unsigned int height : {1U, 2U, 3U}) {
      for (bool link : {false, true}) {
        for (unsigned int offset = 0; offset < 16; ++offset) {
          source_alignments |= 1U << PaddingOnlyCopiesMatchFullClear(
              width, height, width, height, link, offset, true);
          ++cases;
        }
      }
    }
  }
  Require(source_alignments == 0xffffU,
          "exact-boundary conversion exercises all 16 actual source alignments");
  // Cover each BCM70012 pitch transition without multiplying full-HD-sized
  // allocations through the alignment matrix above. Existing destination
  // guards, unequal pitches and the unchanged scalar oracle remain in use.
  for (unsigned int width : {719U, 720U, 721U, 1279U, 1280U, 1281U, 1919U, 1920U}) {
    for (unsigned int offset : {0U, 15U}) {
      PaddingOnlyCopiesMatchFullClear(width, 3, width, 3, true, offset, true);
      ++cases;
    }
  }
  PaddingOnlyCopiesMatchFullClear(1920, 1088, 1920, 1088, false, 0, true);
  PaddingOnlyCopiesMatchFullClear(1920, 1088, 1920, 1088, true, 15, true);
  cases += 2;
  std::printf("%u exact-boundary/alignment conversion cases match scalar pixels and guards\n",
              cases);
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

static void ConcurrentSyncObservesExperimentalPresentation() {
  for (bool callback_commits : {false, true}) {
    TeardownFixture fixture(2);
    fixture.decoder->id = 1;
    fixture.decoder->generation = 1;
    fixture.decoder->experimental_ready_remap = true;
    fixture.decoder->live_h264 = true;
    fixture.decoder->low_latency_h264 = true;

    const auto candidate =
        fixture.decoder->decoded_frames.at(kTimestampStep);
    const auto requested_picture =
        fixture.decoder->decoded_frames.at(2 * kTimestampStep);
    const auto requested = fixture.held[1];
    candidate->ready = true;
    candidate->frame_timestamp = candidate->expected_timestamp;
    candidate->decode_identity = candidate->trace_decode_identity = 81;
    candidate->trace_context = 1;
    candidate->trace_generation = 1;
    candidate->trace_submission_ordinal = 1;
    memset(candidate->planes[0], 42, candidate->storage.size());
    requested_picture->decode_identity =
        requested_picture->trace_decode_identity = 82;
    requested_picture->trace_context = 1;
    requested_picture->trace_generation = 1;
    requested_picture->trace_submission_ordinal = 2;
    requested->id = 2;
    requested->decode_identity = requested->trace_decode_identity = 82;
    requested->trace_context = 1;
    requested->trace_generation = 1;
    requested->trace_submission_ordinal = 2;
    requested->decode_picture = requested_picture;
    requested->experimental_write_lease = true;
    requested->experimental_write_context = 1;
    requested->experimental_write_generation = 1;
    requested->experimental_write_submission_ordinal = 2;
    fixture.decoder->pending.erase(kTimestampStep);
    fixture.decoder->surface_timestamps.erase(fixture.held[0].get());

    bool callback_committed = false;
    fixture.io.on_sleep = [&] {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      QueueExperimentalReady(&fixture.driver, fixture.decoder.get(), candidate);
      if (callback_commits)
        Require(TryExperimentalPresentation(&fixture.driver, requested,
                                            &callback_committed) ==
                        VA_STATUS_SUCCESS &&
                    callback_committed,
                "concurrent sync commits one matching older READY payload");
    };
    Require(SyncSurface(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                callback_committed == callback_commits &&
                ExperimentalPresentationReadable(&fixture.driver,
                                                  requested.get()) &&
                !requested->ready && requested->planes[0][0] == 42 &&
                !requested_picture->ready && !requested->failed,
            callback_commits
                ? "exact waiter observes another sync's same-binding presentation commit"
                : "waiting sync commits a READY payload queued after its initial selection");
  }

  {
    TeardownFixture fixture(2);
    fixture.decoder->id = 1;
    fixture.decoder->generation = 1;
    fixture.decoder->experimental_ready_remap = true;
    fixture.decoder->live_h264 = true;
    fixture.decoder->low_latency_h264 = true;

    const auto candidate =
        fixture.decoder->decoded_frames.at(kTimestampStep);
    const auto requested_picture =
        fixture.decoder->decoded_frames.at(2 * kTimestampStep);
    const auto requested = fixture.held[1];
    candidate->decode_identity = candidate->trace_decode_identity = 81;
    candidate->trace_context = 1;
    candidate->trace_generation = 1;
    candidate->trace_submission_ordinal = 1;
    requested_picture->decode_identity =
        requested_picture->trace_decode_identity = 82;
    requested_picture->trace_context = 1;
    requested_picture->trace_generation = 1;
    requested_picture->trace_submission_ordinal = 2;
    requested->id = 2;
    requested->decode_identity = requested->trace_decode_identity = 82;
    requested->trace_context = 1;
    requested->trace_generation = 1;
    requested->trace_submission_ordinal = 2;
    requested->decode_picture = requested_picture;
    requested->experimental_write_lease = true;
    requested->experimental_write_context = 1;
    requested->experimental_write_generation = 1;
    requested->experimental_write_submission_ordinal = 2;

    Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                    VA_STATUS_SUCCESS &&
                fixture.io.inputs.size() == 2,
            "submit pictures for same-call READY harvest");
    // The I/O mock records each split input synchronously but has no firmware
    // parser that consumes the speculative prefix. Model the state after the
    // matching remainder has made the first picture eligible for output.
    fixture.decoder->prefed_timestamp = 0;
    fixture.io.outputs.push_back(kTimestampStep);
    Require(SyncSurface(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                fixture.io.outputs.empty() && candidate->ready &&
                ExperimentalPresentationReadable(&fixture.driver,
                                                  requested.get()) &&
                !requested->ready &&
                requested->planes[0][0] ==
                    DecodeIoMock::Luma(kTimestampStep) &&
                !requested_picture->ready && !requested->failed,
            "same sync selects an older picture harvested by ReceiveAvailable");
  }

  // Rebinding a public surface while an older SyncSurface call has released
  // Driver::mutex must not let that obsolete waiter present into the new
  // context merely because both contexts use the same synthetic token.
  TeardownFixture fixture(2);
  fixture.decoder->id = 1;
  fixture.decoder->generation = 1;
  fixture.decoder->experimental_ready_remap = true;
  fixture.decoder->live_h264 = true;
  fixture.decoder->low_latency_h264 = true;

  const auto old_picture =
      fixture.decoder->decoded_frames.at(2 * kTimestampStep);
  const auto requested = fixture.held[1];
  old_picture->decode_identity = old_picture->trace_decode_identity = 82;
  old_picture->trace_context = 1;
  old_picture->trace_generation = 1;
  old_picture->trace_submission_ordinal = 2;
  requested->id = 2;
  requested->decode_identity = requested->trace_decode_identity = 82;
  requested->trace_context = 1;
  requested->trace_generation = 1;
  requested->trace_submission_ordinal = 2;
  requested->decode_picture = old_picture;
  requested->experimental_write_lease = true;
  requested->experimental_write_context = 1;
  requested->experimental_write_generation = 1;
  requested->experimental_write_submission_ordinal = 2;

  auto replacement = std::make_shared<DecodeContext>();
  replacement->id = 2;
  replacement->generation = 1;
  replacement->width = replacement->height = 16;
  replacement->device = fixture.io.handle();
  replacement->decoder_open = replacement->decoder_started = true;
  replacement->experimental_ready_remap = true;
  replacement->live_h264 = true;
  replacement->low_latency_h264 = true;
  fixture.driver.contexts[2] = replacement;

  auto candidate = std::make_shared<Surface>();
  Require(candidate->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
          "allocate equal-token replacement candidate");
  candidate->expected_timestamp = candidate->frame_timestamp = kTimestampStep;
  candidate->ready = true;
  candidate->decode_identity = candidate->trace_decode_identity = 91;
  candidate->trace_context = 2;
  candidate->trace_generation = 1;
  candidate->trace_submission_ordinal = 1;
  memset(candidate->planes[0], 55, candidate->storage.size());
  replacement->decoded_frames[kTimestampStep] = candidate;

  fixture.io.on_sleep = [&] {
    std::lock_guard<std::mutex> lock(fixture.driver.mutex);
    requested->decode_identity = requested->trace_decode_identity = 92;
    requested->trace_context = 2;
    requested->trace_generation = 1;
    requested->trace_submission_ordinal = 2;
    requested->decode_picture.reset();
    requested->experimental_write_lease = true;
    requested->experimental_write_context = 2;
    requested->experimental_write_generation = 1;
    requested->experimental_write_submission_ordinal = 2;
    QueueExperimentalReady(&fixture.driver, replacement.get(), candidate);
  };

  Require(SyncSurface(&fixture.context, 2) ==
                  VA_STATUS_ERROR_INVALID_SURFACE &&
              requested->trace_context == 2 &&
              requested->trace_decode_identity == 92 &&
              requested->experimental_write_lease &&
              !requested->experimental_presentation &&
              requested->planes[0][0] == 99 &&
              candidate->experimental_ready_queued &&
              replacement->experimental_ready.size() == 1 &&
              replacement->experimental_ready.front() == candidate,
          "obsolete waiter cannot mutate an equal-token replacement binding");
}

static void StaleUnretainedOutputPreservesTransportAndEos() {
  TeardownFixture fixture(2);
  Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                  VA_STATUS_SUCCESS &&
              fixture.decoder->replay.Seal(),
          "submit and seal stale-output transport fixture");
  const uint64_t progress_before_outputs = fixture.decoder->transport_progress;
  fixture.decoder->decoded_frames.at(kTimestampStep)->decode_identity = 1;
  fixture.decoder->decoded_frames.at(2 * kTimestampStep)->decode_identity = 2;
  fixture.held[0]->expected_timestamp = 2 * kTimestampStep;
  fixture.held[0]->decode_identity = 2;
  fixture.held[0]->decode_picture =
      fixture.decoder->decoded_frames.at(2 * kTimestampStep);
  fixture.decoder->pending[2 * kTimestampStep] = 1;
  fixture.decoder->surface_timestamps[fixture.held[0].get()] =
      2 * kTimestampStep;
  mock_sync_fd = memfd_create("crystalhd-stale-output-test", MFD_CLOEXEC);
  Require(mock_sync_fd >= 0, "allocate stale-output CPU ownership fd");
  std::weak_ptr<Surface> stale_picture =
      fixture.decoder->decoded_frames.at(kTimestampStep);
  fixture.decoder->decoded_frames.at(kTimestampStep)->object_fds.push_back(
      mock_sync_fd);
  mock_failed_sync_flags = UINT64_MAX;
  mock_sync_calls.clear();
  fixture.io.outputs.push_back(kTimestampStep);
  fixture.io.outputs.push_back(2 * kTimestampStep);
  fixture.io.outputs.push_back(0);
  Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get(), 1) ==
                  VA_STATUS_SUCCESS &&
              fixture.decoder->pending.count(kTimestampStep) == 0 &&
              fixture.decoder->pending.at(2 * kTimestampStep) == 1 &&
              fixture.decoder->decoded_frames.count(kTimestampStep) == 0 &&
              fixture.decoder->replay.outstanding() == 1 &&
              fixture.decoder->ReplaySealed() &&
              fixture.decoder->transport_progress ==
                  progress_before_outputs + 1 &&
              fixture.io.outputs.size() == 2 && mock_sync_calls.empty() &&
              stale_picture.expired() &&
              fixture.held[0]->planes[0][0] == 99 &&
              !fixture.held[0]->ready && !fixture.held[0]->failed &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "picture") == 1 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "release") == 1 &&
              !fixture.io.invalid,
          "stale pixels are skipped after exact transport retirement and release");
  mock_sync_fd = -1;
  mock_sync_fd = memfd_create("crystalhd-reused-current-test", MFD_CLOEXEC);
  Require(mock_sync_fd >= 0, "allocate reused-current CPU ownership fd");
  fixture.decoder->decoded_frames.at(2 * kTimestampStep)->object_fds.push_back(
      mock_sync_fd);
  mock_sync_calls.clear();
  Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
                  VA_STATUS_SUCCESS &&
              fixture.decoder->pending.empty() &&
              fixture.decoder->replay.outstanding() == 0 &&
              !fixture.decoder->ReplaySealed() &&
              fixture.decoder->transport_progress ==
                  progress_before_outputs + 2 &&
              fixture.io.outputs.empty() &&
              mock_sync_calls ==
                  std::vector<uint64_t>{DMA_BUF_SYNC_START |
                                            DMA_BUF_SYNC_WRITE,
                                        DMA_BUF_SYNC_END |
                                            DMA_BUF_SYNC_WRITE,
                                        DMA_BUF_SYNC_START |
                                            DMA_BUF_SYNC_READ,
                                        DMA_BUF_SYNC_END |
                                            DMA_BUF_SYNC_READ} &&
              fixture.held[0]->ready && !fixture.held[0]->failed &&
              fixture.held[0]->frame_timestamp == 2 * kTimestampStep &&
              fixture.held[0]->planes[0][0] ==
                  DecodeIoMock::Luma(2 * kTimestampStep) &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "picture") == 2 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "release") == 3 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "eos") == 1 &&
              !fixture.io.invalid,
          "the reused current picture still converts before the sealed EOS");
  mock_sync_fd = -1;
}

static void CompletedLiveTailPrecedesUnreconstructibleContinuation() {
  TeardownFixture fixture(4);
  CrystalHDDecodeReplay::Limits limits;
  limits.cache_bytes = limits.pictures = 2;
  fixture.decoder->live_h264 = true;
  fixture.decoder->replay = CrystalHDDecodeReplay(limits, true);
  for (uint64_t picture = 1; picture <= 3; ++picture) {
    const uint64_t timestamp = picture * kTimestampStep;
    Require(fixture.decoder->replay.Append(timestamp, picture == 1, {1}) &&
                PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                    VA_STATUS_SUCCESS,
            "submit live tail fixture without replaying completed input");
    if (picture < 3) {
      fixture.io.outputs.push_back(timestamp);
      Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
                  VA_STATUS_SUCCESS,
              "complete old live prefix with real simulated pixels");
    }
  }
  Require(!fixture.decoder->replay.replayable() &&
              fixture.decoder->replay.Seal() &&
              fixture.decoder->replay.Append(4 * kTimestampStep, false, {4}),
          "accept queued P without poisoning the still-pending old tail");
  fixture.io.outputs.push_back(3 * kTimestampStep);
  fixture.io.outputs.push_back(0);
  {
    std::unique_lock<std::mutex> lock(fixture.driver.mutex);
    Require(SyncDecodeSurface(&fixture.driver, fixture.held[2], &lock,
                              VA_TIMEOUT_INFINITE) == VA_STATUS_SUCCESS,
            "exact old tail plus EOS succeeds before any future pump failure");
  }
  Require(fixture.io.open_attempts == 0 &&
              fixture.decoder->replay.NeedsRestart() &&
              !fixture.decoder->replay.failed(),
          "tail sync does not attempt an unrelated continuation restart");
  fixture.CheckPicture(2);
  const auto pixels = fixture.held[2]->storage;
  Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
              VA_STATUS_ERROR_DECODING_ERROR &&
              fixture.decoder->replay.failed(),
          "future restart failure is reported separately from completed pixels");
  Require(fixture.held[2]->ready && !fixture.held[2]->failed &&
              fixture.held[2]->storage == pixels && !fixture.held[3]->ready,
          "failed future input cannot invalidate or mutate the old exact tail");
}

static void DecoderResetPreservesItsExplicitLivePolicy() {
  for (bool live : {false, true}) {
    DecodeContext decoder;
    decoder.live_h264 = live;
    decoder.replay = CrystalHDDecodeReplay(live);
    for (unsigned int epoch = 0; epoch < 3; ++epoch) {
      if (epoch == 1)
        Require(decoder.Close() == BC_STS_SUCCESS, "close private decoder state");
      if (epoch == 2)
        decoder.Reset();
      Require(decoder.live_h264 == live && !decoder.replay.failed() &&
                  decoder.replay.replayable() && !decoder.replay.live_stream_started(),
              "close/reset retains explicit mode and starts fresh replay state");
      for (uint64_t picture = 1; picture <= 513; ++picture) {
        const uint64_t timestamp = (epoch + 1) * 1000 + picture;
        const bool accepted = decoder.replay.Append(timestamp, picture == 1, {1});
        Require(accepted == (live || picture <= 512),
                "default stays strict and only explicit live mode retires history");
        if (!accepted)
          break;
        Require(decoder.replay.live_stream_started() == (live && picture >= 2),
                "reset stream policy counts new originals, not absolute timestamps");
        SendNext(&decoder.replay, timestamp);
        Require(decoder.replay.Observe(timestamp) == CrystalHDDecodeReplay::Output::New,
                "each reset epoch completes its own exact live/strict picture");
      }
      Require(decoder.replay.failed() != live &&
                  decoder.replay.replayable() != live,
              "reset preserves both strict limit failures and live opt-in behavior");
    }
  }

  DecodeContext low_latency;
  low_latency.live_h264 = true;
  low_latency.low_latency_h264 = true;
  low_latency.prefed_timestamp = 17 * kTimestampStep;
  Require(low_latency.Close() == BC_STS_SUCCESS &&
              low_latency.live_h264 && low_latency.low_latency_h264 &&
              low_latency.prefed_timestamp == 0,
          "close preserves low-latency policy but discards its runtime prefix");
  low_latency.prefed_timestamp = 19 * kTimestampStep;
  low_latency.Reset();
  Require(low_latency.live_h264 && low_latency.low_latency_h264 &&
              low_latency.prefed_timestamp == 0,
          "reset preserves low-latency policy but discards its runtime prefix");
}

static std::vector<uint8_t> TestH264AccessUnit(bool idr, uint8_t payload) {
  return {0x00, 0x00, 0x00, 0x01, 0x09, 0xf0,
          0x00, 0x00, 0x00, 0x01,
          static_cast<uint8_t>(idr ? 0x65 : 0x41), payload, 0x80};
}

static std::vector<uint8_t> ExpectedH264LowLatencyPrefeed() {
  std::vector<uint8_t> bytes(170, 0xff);
  const uint8_t prefix[] = {0x00, 0x00, 0x00, 0x01, 0x09, 0xf0,
                            0x00, 0x00, 0x00, 0x01, 0x0c};
  std::copy(std::begin(prefix), std::end(prefix), bytes.begin());
  bytes.back() = 0x80;
  return bytes;
}

static void LiveH264SpeculativePrefeedSequence() {
  TeardownFixture fixture(3);
  fixture.decoder->live_h264 = true;
  fixture.decoder->low_latency_h264 = true;
  fixture.decoder->is_70012 = false;
  fixture.decoder->replay = CrystalHDDecodeReplay(true);
  fixture.decoder->next_timestamp = kTimestampStep;
  std::vector<std::vector<uint8_t>> units;
  for (unsigned int picture = 1; picture <= 3; ++picture) {
    const uint64_t timestamp = picture * kTimestampStep;
    units.push_back(TestH264AccessUnit(picture == 1,
                                       static_cast<uint8_t>(picture)));
    Require(fixture.decoder->replay.Append(timestamp, picture == 1,
                                            units.back()),
            "append one live H.264 access unit");
    fixture.decoder->next_timestamp = (picture + 1) * kTimestampStep;
    Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                VA_STATUS_SUCCESS,
            "submit one live AU and arm its successor prefix");
  }

  const auto expected_prefeed = ExpectedH264LowLatencyPrefeed();
  Require(H264LowLatencyPrefeed() == expected_prefeed &&
              fixture.io.input_calls.size() == 6 &&
              fixture.io.inputs ==
                  std::vector<uint64_t>({kTimestampStep, 2 * kTimestampStep,
                                         3 * kTimestampStep}),
          "the measured raw170 prefix is exact and is not a logical picture");
  for (size_t picture = 0; picture < units.size(); ++picture) {
    const auto &actual = fixture.io.input_calls[picture * 2];
    const auto &future = fixture.io.input_calls[picture * 2 + 1];
    Require(actual.accepted && actual.bytes == units[picture] &&
                actual.timestamp == (picture == 0
                    ? kTimestampStep : 0) &&
                future.accepted && future.bytes == expected_prefeed &&
                future.timestamp == (picture + 2) * kTimestampStep,
            "each full AU consumes one future tag before the next prefix");
  }
  Require(fixture.decoder->prefed_timestamp == 4 * kTimestampStep &&
              fixture.io.prefed_timestamp == 4 * kTimestampStep &&
              fixture.decoder->replay.outstanding() == 3,
          "the final orphan prefix is not an outstanding public picture");
  Require(SealDecodeBatch(&fixture.driver, fixture.decoder.get()) ==
              VA_STATUS_SUCCESS &&
              fixture.decoder->prefed_timestamp == 4 * kTimestampStep &&
              fixture.io.prefed_timestamp == 0,
          "flush discards the unbound bytes while retaining their guard token");
  Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
              VA_STATUS_SUCCESS && fixture.decoder->replay.outstanding() == 0 &&
              fixture.decoder->prefed_timestamp == 0 &&
              !fixture.decoder->replay.failed() && !fixture.io.invalid,
          "only actual access units complete and EOS retires the guard token");
}

static void LiveH264PrefeedBusyFallback() {
  {
    TeardownFixture fixture(2);
    fixture.decoder->live_h264 = true;
    fixture.decoder->low_latency_h264 = true;
    fixture.decoder->replay = CrystalHDDecodeReplay(true);
    fixture.io.prefeed_busy_once = true;
    const auto first = TestH264AccessUnit(true, 1);
    const auto second = TestH264AccessUnit(false, 2);
    Require(fixture.decoder->replay.Append(kTimestampStep, true, first),
            "append first busy-fallback AU");
    fixture.decoder->next_timestamp = 2 * kTimestampStep;
    Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                VA_STATUS_SUCCESS && fixture.decoder->prefed_timestamp == 0 &&
                fixture.io.input_calls.size() == 2 &&
                fixture.io.input_calls[0].accepted &&
                fixture.io.input_calls[1].timestamp == 2 * kTimestampStep &&
                fixture.io.input_calls[1].bytes ==
                    ExpectedH264LowLatencyPrefeed() &&
                !fixture.io.input_calls[1].accepted,
            "a rejected speculative call was attempted and leaves no prefix");
    Require(fixture.decoder->replay.Append(2 * kTimestampStep, false, second),
            "append fallback AU");
    fixture.decoder->next_timestamp = 3 * kTimestampStep;
    Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                VA_STATUS_SUCCESS && fixture.io.inputs.size() == 2 &&
                fixture.io.input_calls[2].timestamp == 2 * kTimestampStep &&
                fixture.io.input_calls[2].bytes == second,
            "after prefeed busy the next full AU keeps its own timestamp");
  }

  {
    TeardownFixture fixture(2);
    fixture.decoder->live_h264 = true;
    fixture.decoder->low_latency_h264 = true;
    fixture.decoder->replay = CrystalHDDecodeReplay(true);
    const auto first = TestH264AccessUnit(true, 1);
    const auto second = TestH264AccessUnit(false, 2);
    Require(fixture.decoder->replay.Append(kTimestampStep, true, first),
            "append first remainder-retry AU");
    fixture.decoder->next_timestamp = 2 * kTimestampStep;
    Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                VA_STATUS_SUCCESS &&
                fixture.decoder->prefed_timestamp == 2 * kTimestampStep,
            "arm a prefix before testing its busy remainder");
    Require(fixture.decoder->replay.Append(2 * kTimestampStep, false, second),
            "append prefed remainder AU");
    fixture.decoder->next_timestamp = 3 * kTimestampStep;
    fixture.io.remainder_busy_once = true;
    Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                VA_STATUS_SUCCESS &&
                fixture.decoder->prefed_timestamp == 2 * kTimestampStep &&
                fixture.decoder->replay.NextInput() != nullptr &&
                fixture.io.input_calls.back().timestamp == 0 &&
                fixture.io.input_calls.back().bytes == second &&
                !fixture.io.input_calls.back().accepted,
            "busy full AU preserves both prefix ownership and replay cursor");
    Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                VA_STATUS_SUCCESS && fixture.io.inputs.size() == 2 &&
                fixture.decoder->prefed_timestamp == 3 * kTimestampStep &&
                fixture.io.input_calls[fixture.io.input_calls.size() - 2].timestamp == 0 &&
                fixture.io.input_calls[fixture.io.input_calls.size() - 2].bytes == second &&
                fixture.io.input_calls[fixture.io.input_calls.size() - 2].accepted,
            "retry sends the complete AU, consumes its prefix and arms one successor");
    size_t accepted_second_prefixes = 0;
    for (const auto &call : fixture.io.input_calls)
      if (call.accepted && call.timestamp == 2 * kTimestampStep &&
          call.bytes == H264LowLatencyPrefeed())
        ++accepted_second_prefixes;
    Require(accepted_second_prefixes == 1 && !fixture.io.invalid,
            "remainder retry never duplicates the accepted prefix");
  }
}

static void LiveH264PrefeedEligibilityAndGhostGuard() {
  {
    TeardownFixture fixture(2);
    fixture.decoder->live_h264 = true;
    fixture.decoder->low_latency_h264 = true;
    fixture.decoder->replay = CrystalHDDecodeReplay(true);
    const auto first = TestH264AccessUnit(true, 1);
    const auto second = TestH264AccessUnit(false, 2);
    Require(fixture.decoder->replay.Append(kTimestampStep, true, first) &&
                fixture.decoder->replay.Append(2 * kTimestampStep, false,
                                                second),
            "queue two live AUs before one transport pump");
    fixture.decoder->next_timestamp = 3 * kTimestampStep;
    Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                VA_STATUS_SUCCESS && fixture.io.input_calls.size() == 3 &&
                fixture.io.input_calls[0].timestamp == kTimestampStep &&
                fixture.io.input_calls[0].bytes == first &&
                fixture.io.input_calls[1].timestamp == 2 * kTimestampStep &&
                fixture.io.input_calls[1].bytes == second &&
                fixture.io.input_calls[2].timestamp == 3 * kTimestampStep &&
                fixture.io.input_calls[2].bytes ==
                    ExpectedH264LowLatencyPrefeed(),
            "queued AUs keep their timestamps and only the drained tail prefeds");
  }

  {
    TeardownFixture fixture(2);
    fixture.decoder->live_h264 = true;
    fixture.decoder->low_latency_h264 = true;
    fixture.decoder->replay = CrystalHDDecodeReplay(true);
    fixture.io.tx_free_size = 1193;
    Require(fixture.decoder->replay.Append(
                kTimestampStep, true, TestH264AccessUnit(true, 1)),
            "append AU below prefeed free-space boundary");
    fixture.decoder->next_timestamp = 2 * kTimestampStep;
    Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                VA_STATUS_SUCCESS && fixture.io.input_calls.size() == 1 &&
                fixture.decoder->prefed_timestamp == 0,
            "1193 free bytes admit the tiny AU but not raw170 plus reserve");
    fixture.io.tx_free_size = 1194;
    Require(fixture.decoder->replay.Append(
                2 * kTimestampStep, false, TestH264AccessUnit(false, 2)),
            "append AU at prefeed free-space boundary");
    fixture.decoder->next_timestamp = 3 * kTimestampStep;
    Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                VA_STATUS_SUCCESS && fixture.io.input_calls.size() == 3 &&
                fixture.io.input_calls.back().timestamp ==
                    3 * kTimestampStep &&
                fixture.io.input_calls.back().bytes ==
                    ExpectedH264LowLatencyPrefeed(),
            "1194 free bytes admit raw170 plus the conservative reserve");
  }

  for (const bool opt_in : {false, true}) {
    TeardownFixture fixture(1);
    fixture.decoder->live_h264 = true;
    fixture.decoder->low_latency_h264 = opt_in;
    fixture.decoder->is_70012 = opt_in;
    fixture.decoder->replay = CrystalHDDecodeReplay(true);
    const auto unit = TestH264AccessUnit(true, 1);
    Require(fixture.decoder->replay.Append(kTimestampStep, true, unit),
            "append compatibility-path H.264 AU");
    fixture.decoder->next_timestamp = 2 * kTimestampStep;
    Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                VA_STATUS_SUCCESS && fixture.io.input_calls.size() == 1 &&
                fixture.io.input_calls.front().timestamp == kTimestampStep &&
                fixture.io.input_calls.front().bytes == unit &&
                fixture.decoder->prefed_timestamp == 0,
            "opt-out and BCM70012 keep the original byte-exact input path");
  }

  {
    TeardownFixture fixture(1);
    fixture.decoder->live_h264 = true;
    fixture.decoder->low_latency_h264 = true;
    fixture.decoder->replay = CrystalHDDecodeReplay(true);
    Require(fixture.decoder->replay.Append(
                kTimestampStep, true, TestH264AccessUnit(true, 1)),
            "append AU for orphan output guard");
    fixture.decoder->next_timestamp = 2 * kTimestampStep;
    Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                VA_STATUS_SUCCESS &&
                fixture.decoder->prefed_timestamp == 2 * kTimestampStep &&
                SealDecodeBatch(&fixture.driver, fixture.decoder.get()) ==
                    VA_STATUS_SUCCESS,
            "seal while retaining the orphan prefix guard");
    fixture.io.outputs.insert(fixture.io.outputs.end() - 1,
                              2 * kTimestampStep);
    Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
                VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.decoder->replay.failed() &&
                fixture.decoder->prefed_timestamp == 2 * kTimestampStep &&
                !fixture.io.events.empty() &&
                fixture.io.events.back() == "release" &&
                !fixture.io.invalid,
            "an orphan prefix picture before EOS is a released terminal error");
  }
}


static void TransportProgressRequiresActualHardwareIo() {
  TeardownFixture fixture(2);
  std::unique_lock<std::mutex> lock(fixture.driver.mutex);
  Require(fixture.decoder->transport_progress == 0 && fixture.io.inputs.empty(),
          "frontend acceptance alone is not hardware transport progress");
  fixture.io.tx_free_size = 0;
  for (unsigned int attempt = 0; attempt < 8; ++attempt) {
    Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                VA_STATUS_SUCCESS &&
                ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
                VA_STATUS_SUCCESS,
            "empty polls and a full TX ring remain nonblocking");
  }
  Require(fixture.decoder->transport_progress == 0 && fixture.io.inputs.empty(),
          "polls and unsent frontend input must not reset batching grace");
  fixture.io.tx_free_size = 1024 * 1024;
  fixture.io.input_busy = true;
  Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
              VA_STATUS_SUCCESS && fixture.decoder->transport_progress == 0 &&
              fixture.io.inputs.empty(),
          "a busy hardware input call is not accepted transport progress");
  fixture.io.input_busy = false;
  Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
              VA_STATUS_SUCCESS && fixture.decoder->transport_progress == 2,
          "each successfully submitted access unit advances transport progress");
  fixture.io.outputs.push_back(2 * kTimestampStep);
  Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
              VA_STATUS_SUCCESS && fixture.decoder->transport_progress == 3,
          "a real accepted hardware picture advances transport progress");
  const auto pixels = fixture.held[1]->storage;
  fixture.io.outputs.push_back(2 * kTimestampStep);
  Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
              VA_STATUS_SUCCESS && fixture.decoder->transport_progress == 3 &&
              fixture.held[1]->storage == pixels &&
              fixture.held[1]->frame_timestamp == 2 * kTimestampStep,
          "an unsolicited retired duplicate is not transport progress");
  fixture.io.zero_timestamp_picture = true;
  fixture.io.outputs.push_back(0);
  Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
              VA_STATUS_SUCCESS && fixture.decoder->transport_progress == 3 &&
              fixture.held[1]->storage == pixels && !fixture.held[0]->ready,
          "untimestamped firmware output cannot reset batching grace");
  fixture.io.zero_timestamp_picture = false;
  Require(fixture.decoder->replay.Seal(), "seal the exact submitted test batch");
  fixture.io.outputs.push_back(kTimestampStep);
  Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
              VA_STATUS_SUCCESS && fixture.decoder->transport_progress == 4 &&
              fixture.decoder->replay.EndOfSequence() &&
              fixture.decoder->replay.Append(3 * kTimestampStep, false,
                                             {0, 0, 1, 0x41}) &&
              fixture.decoder->replay.Restarted(),
          "completed input can be resubmitted only after the batch drains");
  Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
              VA_STATUS_SUCCESS && fixture.decoder->transport_progress == 7,
          "replayed access units are actual new hardware submissions");
  fixture.io.outputs.push_back(2 * kTimestampStep);
  Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
              VA_STATUS_SUCCESS && fixture.decoder->transport_progress == 8 &&
              fixture.held[1]->storage == pixels &&
              fixture.held[1]->frame_timestamp == 2 * kTimestampStep,
          "expected replay completion is progress without mutating client pixels");
  Require(!fixture.io.invalid && fixture.io.open_attempts == 0,
          "transport progress test never reopens or touches actual hardware");
}

static void ProgressingSyncDoesNotSealAnActiveStream() {
  TeardownFixture fixture(1);
  unsigned int pulses = 0;
  std::function<void()> progress;
  progress = [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    std::lock_guard<std::mutex> lock(fixture.driver.mutex);
    ++pulses;
    if (pulses < 4) {
      Require(!fixture.held[0]->ready && fixture.held[0]->planes[0][0] == 99,
              "later hardware progress cannot invent the requested picture");
      const uint64_t timestamp = (pulses + 1) * kTimestampStep;
      Require(fixture.decoder->replay.Append(timestamp, false, {0, 0, 1, 0x41}) &&
                  PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                      VA_STATUS_SUCCESS,
              "concurrent submission progresses the actual hardware transport");
      fixture.io.outputs.push_back(timestamp);
      fixture.io.on_sleep = progress;
    } else {
      fixture.io.outputs.push_back(kTimestampStep);
    }
  };
  fixture.io.on_sleep = progress;
  const auto start = std::chrono::steady_clock::now();
  Require(SyncSurface(&fixture.context, 1) == VA_STATUS_SUCCESS,
          "requested pixels complete while later input and output keep moving");
  Require(pulses == 4 && std::chrono::steady_clock::now() - start >=
                            std::chrono::milliseconds(160),
          "regression must span more than the original fixed batching grace");
  Require(std::find(fixture.io.events.begin(), fixture.io.events.end(), "seal") ==
              fixture.io.events.end() && fixture.io.inputs.size() == 4 &&
              fixture.decoder->transport_progress == 8 &&
              fixture.decoder->replay.outstanding() == 0,
          "real progress prevents premature EOS and reference-prefix replay");
  fixture.CheckPicture(0);
  Require(!fixture.io.invalid && fixture.io.open_attempts == 0,
          "active stream sync preserves hardware session ownership");
}

static void EarlyLiveGapPreservesIntactReplayHistory() {
  TeardownFixture fixture(3);
  fixture.decoder->live_h264 = true;
  fixture.decoder->replay = CrystalHDDecodeReplay(true);
  for (uint64_t picture = 1; picture <= 2; ++picture)
    Require(fixture.decoder->replay.Append(picture * kTimestampStep,
                                           picture == 1, {1}),
            "accept two original live pictures within the default cache");
  Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) == VA_STATUS_SUCCESS,
          "submit both original live pictures before the early gap");
  fixture.io.outputs.push_back(kTimestampStep);
  Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) == VA_STATUS_SUCCESS &&
              fixture.decoder->replay.replayable() &&
              fixture.decoder->replay.cached_pictures() == 2 &&
              fixture.decoder->replay.outstanding() == 1,
          "early gap has intact replay history and one actual pending picture");
  const uint64_t progress = fixture.decoder->transport_progress;
  const uint64_t generation = fixture.decoder->generation;
  unsigned int callbacks = 0;
  fixture.io.on_sleep = [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    ++callbacks;
    Require(fixture.decoder->transport_progress == progress,
            "early gap returns to sync without input or output progress");
    fixture.io.on_sleep = [&] {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      ++callbacks;
      Require(std::count(fixture.io.events.begin(), fixture.io.events.end(), "seal") == 0 &&
                  fixture.decoder->replay.replayable() &&
                  fixture.decoder->transport_progress == progress &&
                  !fixture.held[1]->ready && fixture.held[1]->planes[0][0] == 99,
              "observed early live gap must not seal even with intact replay history");
      Require(fixture.decoder->replay.Append(3 * kTimestampStep, false, {3}) &&
                  PumpDecodeInput(&fixture.driver, fixture.decoder.get()) == VA_STATUS_SUCCESS,
              "later original input resumes without replaying the retained prefix");
      fixture.io.outputs.push_back(3 * kTimestampStep);
      fixture.io.outputs.push_back(2 * kTimestampStep);
    };
  };
  Require(SyncSurface(&fixture.context, 2) == VA_STATUS_SUCCESS && callbacks == 2 &&
              fixture.io.inputs.size() == 3 && fixture.io.open_attempts == 0 &&
              fixture.decoder->generation == generation &&
              fixture.decoder->replay.replayable() &&
              fixture.decoder->replay.outstanding() == 0 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(), "seal") == 0 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(), "eos") == 0 &&
              !fixture.io.invalid,
          "early live gap preserves the session and exact original input sequence");
  fixture.CheckPicture(0);
  fixture.CheckPicture(1);
  fixture.CheckPicture(2);
}

static void PrepareLiveTailWithoutReplayHistory(TeardownFixture &fixture) {
  CrystalHDDecodeReplay::Limits limits;
  limits.cache_bytes = 8;
  limits.pictures = 2;
  fixture.decoder->live_h264 = true;
  fixture.decoder->replay = CrystalHDDecodeReplay(limits, true);
  for (uint64_t picture = 1; picture <= 3; ++picture) {
    const uint64_t timestamp = picture * kTimestampStep;
    Require(fixture.decoder->replay.Append(timestamp, picture == 1, {1}) &&
                PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                    VA_STATUS_SUCCESS,
            "submit live idle-gap fixture with bounded replay history");
    if (picture < 3) {
      fixture.io.outputs.push_back(timestamp);
      Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
                  VA_STATUS_SUCCESS,
              "complete old live history before retiring its compressed bytes");
    }
  }
  Require(!fixture.decoder->replay.replayable() &&
              fixture.decoder->replay.outstanding() == 1 &&
              !fixture.held[2]->ready && !fixture.held[2]->failed,
          "live gap starts with lost history and one exact pending tail");
}

static void InactiveLiveSyncPreservesReferencesUntilActualInputResumes() {
  TeardownFixture fixture(4);
  PrepareLiveTailWithoutReplayHistory(fixture);
  const uint64_t progress = fixture.decoder->transport_progress;
  const uint64_t generation = fixture.decoder->generation;
  unsigned int callbacks = 0;
  fixture.io.on_sleep = [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    ++callbacks;
    Require(fixture.decoder->transport_progress == progress,
            "first gap callback returns without any hardware input or output");
    // The production loop must observe the expired idle grace between these
    // callbacks; adding input in this first callback would hide the regression.
    fixture.io.on_sleep = [&] {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      ++callbacks;
      Require(std::find(fixture.io.events.begin(), fixture.io.events.end(), "seal") ==
                  fixture.io.events.end() && !fixture.decoder->replay.sealed() &&
                  fixture.decoder->transport_progress == progress,
              "an observed input gap must not destroy sole live references");
      Require(fixture.decoder->replay.Append(4 * kTimestampStep, false, {4}) &&
                  PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                      VA_STATUS_SUCCESS,
              "later input resumes the original live hardware session");
      fixture.io.outputs.push_back(4 * kTimestampStep);
      fixture.io.outputs.push_back(3 * kTimestampStep);
    };
  };
  const auto start = std::chrono::steady_clock::now();
  Require(SyncSurface(&fixture.context, 3) == VA_STATUS_SUCCESS,
          "requested live tail completes after an actually observed delivery gap");
  Require(callbacks == 2 && std::chrono::steady_clock::now() - start >=
                              std::chrono::nanoseconds(kDecodeBatchGraceNs) &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(), "seal") == 0 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(), "eos") == 0 &&
              fixture.io.inputs.size() == 4 && fixture.io.open_attempts == 0 &&
              fixture.decoder->generation == generation && !fixture.io.invalid,
          "live inactivity neither emits EOS nor reopens or resets the decoder");
  fixture.CheckPicture(2);
  fixture.CheckPicture(3);
}

static void InactiveLiveSyncHonorsFiniteDeadlineWithoutPoisoningPixels() {
  TeardownFixture fixture;
  PrepareLiveTailWithoutReplayHistory(fixture);
  const auto pixels = fixture.held[2]->storage;
  const uint64_t progress = fixture.decoder->transport_progress;
  const uint64_t generation = fixture.decoder->generation;
  unsigned int callbacks = 0;
  fixture.io.on_sleep = [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    ++callbacks;
    fixture.io.on_sleep = [&] {
      ++callbacks;
      Require(fixture.decoder->transport_progress == progress &&
                  std::find(fixture.io.events.begin(), fixture.io.events.end(), "seal") ==
                      fixture.io.events.end(),
              "finite wait also observes the inactive grace without sealing");
      std::this_thread::sleep_for(std::chrono::milliseconds(230));
    };
  };
  constexpr uint64_t timeout_ns = 350ULL * 1000 * 1000;
  const auto start = std::chrono::steady_clock::now();
  Require(SyncSurface2(&fixture.context, 3, timeout_ns) == VA_STATUS_ERROR_TIMEDOUT,
          "missing live output still honors the absolute finite caller deadline");
  const auto elapsed = std::chrono::steady_clock::now() - start;
  Require(callbacks >= 1 && callbacks <= 2 &&
              elapsed >= std::chrono::nanoseconds(timeout_ns) &&
              elapsed < std::chrono::seconds(2) &&
              !fixture.held[2]->ready && !fixture.held[2]->failed &&
              fixture.held[2]->storage == pixels && !fixture.decoder->replay.failed() &&
              !fixture.decoder->replay.sealed() &&
              fixture.decoder->generation == generation && fixture.io.open_attempts == 0 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(), "eos") == 0,
          "finite timeout preserves live references and the incomplete pixel identity");
  fixture.io.on_sleep = {};
  fixture.io.outputs.push_back(3 * kTimestampStep);
  Require(SyncSurface(&fixture.context, 3) == VA_STATUS_SUCCESS && !fixture.io.invalid,
          "actual output can complete on the same session after a finite timeout");
  fixture.CheckPicture(2);
}

static void ExplicitLiveContextCloseStillDrainsItsExactTail() {
  TeardownFixture fixture;
  PrepareLiveTailWithoutReplayHistory(fixture);
  Require(DestroyContext(&fixture.context, 1) == VA_STATUS_SUCCESS,
          "explicit live close still emits EOS after replay history retires");
  const auto eos = std::find(fixture.io.events.begin(), fixture.io.events.end(), "eos");
  const auto stop = std::find(fixture.io.events.begin(), fixture.io.events.end(), "stop");
  Require(std::count(fixture.io.events.begin(), fixture.io.events.end(), "seal") == 1 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(), "eos") == 1 &&
              eos < stop && fixture.decoder->retired && fixture.decoder->pending.empty() &&
              fixture.driver.contexts.count(1) == 0 && fixture.io.open_attempts == 0 &&
              !fixture.io.invalid,
          "explicit EOS and actual tail output precede releasing the hardware session");
  fixture.CheckPicture(2);
}

static void ActualIdrPreservesLivePolicyUntilExplicitDrain() {
  TeardownFixture fixture(4);
  PrepareLiveTailWithoutReplayHistory(fixture);
  Require(fixture.decoder->replay.Append(4 * kTimestampStep, true, {0, 0, 1, 0x65}) &&
              PumpDecodeInput(&fixture.driver, fixture.decoder.get()) == VA_STATUS_SUCCESS,
          "submit an actual fresh IDR while preserving the old pending tail");
  fixture.io.outputs.push_back(3 * kTimestampStep);
  Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) == VA_STATUS_SUCCESS &&
              fixture.decoder->replay.replayable() &&
              fixture.decoder->replay.live_stream_started() &&
              fixture.decoder->replay.cached_pictures() == 1,
          "old tail completion restores intact replay from the actual new IDR");
  const auto pixels = fixture.held[3]->storage;
  constexpr uint64_t timeout_ns = 160ULL * 1000 * 1000;
  const auto start = std::chrono::steady_clock::now();
  Require(SyncSurface2(&fixture.context, 4, timeout_ns) == VA_STATUS_ERROR_TIMEDOUT &&
              std::chrono::steady_clock::now() - start >= std::chrono::nanoseconds(timeout_ns) &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(), "seal") == 0 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(), "eos") == 0 &&
              fixture.decoder->replay.live_stream_started() &&
              !fixture.decoder->replay.failed() && !fixture.held[3]->ready &&
              !fixture.held[3]->failed && fixture.held[3]->storage == pixels,
          "restored IDR history does not reinterpret an established live gap as EOF");
  Require(DestroyContext(&fixture.context, 1) == VA_STATUS_SUCCESS &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(), "seal") == 1 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(), "eos") == 1 &&
              fixture.io.open_attempts == 0 && !fixture.io.invalid,
          "explicit context drain still completes the exact retained IDR tail");
  fixture.CheckPicture(2);
  fixture.CheckPicture(3);
}

static void InactiveSyncStillSealsItsExactFinalPicture(bool live = false) {
  TeardownFixture fixture(1);
  if (live) {
    fixture.decoder->live_h264 = true;
    fixture.decoder->replay = CrystalHDDecodeReplay(true);
    Require(fixture.decoder->replay.Append(kTimestampStep, true, {0, 0, 1, 0x65}),
            "live initial decoder probe retains its complete actual IDR history");
  }
  const auto start = std::chrono::steady_clock::now();
  Require(SyncSurface(&fixture.context, 1) == VA_STATUS_SUCCESS,
          "finite input tail still completes through a real sealed batch");
  Require(std::chrono::steady_clock::now() - start >=
              std::chrono::nanoseconds(kDecodeBatchGraceNs) &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "seal") == 1 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "eos") == 1 && fixture.decoder->pending.empty() &&
              fixture.io.outputs.empty(),
          "inactivity seals once and waits for every output plus the EOS fence");
  fixture.CheckPicture(0);
  Require(!fixture.io.invalid && fixture.io.open_attempts == 0,
          "finite tail does not reopen absent continued input");
}

static void ProgressDoesNotExtendFiniteSyncDeadline() {
  TeardownFixture fixture(1);
  unsigned int pulses = 0;
  std::function<void()> progress;
  progress = [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    std::lock_guard<std::mutex> lock(fixture.driver.mutex);
    const uint64_t timestamp = (++pulses + 1) * kTimestampStep;
    Require(fixture.decoder->replay.Append(timestamp, false, {0, 0, 1, 0x41}) &&
                PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                    VA_STATUS_SUCCESS,
            "finite-wait regression maintains genuine transport progress");
    fixture.io.outputs.push_back(timestamp);
    fixture.io.on_sleep = progress;
  };
  fixture.io.on_sleep = progress;
  constexpr uint64_t timeout_ns = 160ULL * 1000 * 1000;
  const auto start = std::chrono::steady_clock::now();
  Require(SyncSurface2(&fixture.context, 1, timeout_ns) ==
              VA_STATUS_ERROR_TIMEDOUT,
          "ongoing transport must not extend the caller's absolute deadline");
  const auto elapsed = std::chrono::steady_clock::now() - start;
  Require(elapsed >= std::chrono::nanoseconds(timeout_ns) &&
              elapsed < std::chrono::seconds(2) && pulses >= 1 && pulses <= 4 &&
              !fixture.held[0]->ready && !fixture.held[0]->failed &&
              fixture.held[0]->planes[0][0] == 99 &&
              fixture.held[0]->frame_timestamp == 0 &&
              std::find(fixture.io.events.begin(), fixture.io.events.end(),
                        "seal") == fixture.io.events.end(),
          "bounded timeout neither seals progressing input nor claims pixels");
  fixture.io.on_sleep = {};
  Require(!fixture.io.invalid && fixture.io.open_attempts == 0,
          "finite timeout does not reset hardware or generation state");
}

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

static void ZeroTimeoutNeverPumpsBusyDecodeSurface() {
  TeardownFixture fixture(1);
  Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                  VA_STATUS_SUCCESS &&
              fixture.io.inputs.size() == 1,
          "submit the zero-timeout picture before synchronization");
  fixture.io.outputs.push_back(kTimestampStep);
  const size_t events = fixture.io.events.size();
  const uint64_t progress = fixture.decoder->transport_progress;
  const auto pixels = fixture.held[0]->storage;
  VASurfaceStatus surface_status = VASurfaceReady;
  Require(QuerySurfaceStatus(&fixture.context, 1, &surface_status) ==
                  VA_STATUS_SUCCESS &&
              surface_status == VASurfaceRendering,
          "pending decode reports Rendering before zero-timeout sync");

  Require(SyncSurface2(&fixture.context, 1, 0) == VA_STATUS_ERROR_TIMEDOUT &&
              fixture.io.events.size() == events &&
              fixture.io.outputs.size() == 1 &&
              fixture.decoder->transport_progress == progress &&
              !fixture.held[0]->ready && !fixture.held[0]->failed &&
              fixture.held[0]->storage == pixels,
          "zero timeout returns immediately without polling, copying or pumping input");

  Require(SyncSurface(&fixture.context, 1) == VA_STATUS_SUCCESS &&
              fixture.io.outputs.empty() && fixture.held[0]->ready &&
              !fixture.io.invalid,
          "a later ordinary sync completes the untouched exact picture");
}

static void ExactReadySurfaceDoesNotWaitForBatchEos() {
  TeardownFixture fixture(2);
  Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                  VA_STATUS_SUCCESS &&
              fixture.io.inputs.size() == 2 && fixture.decoder->replay.Seal(),
          "submit and seal two exact pictures for per-surface synchronization");
  fixture.io.outputs.push_back(kTimestampStep);

  constexpr uint64_t timeout_ns = 5ULL * 1000 * 1000;
  Require(SyncSurface2(&fixture.context, 1, timeout_ns) == VA_STATUS_SUCCESS &&
              fixture.held[0]->ready && !fixture.held[0]->failed &&
              fixture.held[0]->frame_timestamp == kTimestampStep &&
              !fixture.held[1]->ready && fixture.decoder->ReplaySealed() &&
              fixture.decoder->replay.outstanding() == 1 &&
              fixture.io.outputs.empty(),
          "an exact ready target succeeds without waiting for another surface or EOS");

  fixture.io.outputs.push_back(2 * kTimestampStep);
  fixture.io.outputs.push_back(0);
  Require(SyncSurface(&fixture.context, 2) == VA_STATUS_SUCCESS &&
              fixture.held[1]->ready && !fixture.decoder->ReplaySealed() &&
              fixture.decoder->replay.outstanding() == 0 &&
              fixture.io.outputs.empty() && !fixture.io.invalid,
          "later synchronization still drains the remaining picture and EOS");
}

// Hardware-free Phase-B replay of facts represented by the current backend:
// driver_token mirrors Surface::expected_timestamp (never original PTS),
// generation mirrors DecodeContext::generation, and each fixture is scoped to
// one VAContextID. The nonoverlapping owner intervals model the
// decode_identity/decode_picture-protected VASurfaceID binding. Hardware
// completion alone is not READY; pixels must also have been materialized while
// that binding remains valid. This fixture never calls or changes production
// surface selection/synchronization.
class ReadyInventoryReplay {
 public:
  struct Identity {
    uint64_t driver_token = 0;
    uint64_t generation = 0;
    bool operator==(const Identity &other) const {
      return driver_token == other.driver_token &&
             generation == other.generation;
    }
  };

  struct PictureTrace {
    Identity identity;
    VASurfaceID owner = VA_INVALID_SURFACE;
    uint64_t owned_at_ns = 0;
    uint64_t hardware_complete_at_ns = 0;
    uint64_t materialized_at_ns = 0;
    uint64_t released_at_ns = 0;
    uint64_t discarded_at_ns = 0;
    // Trace order, rather than token magnitude, defines decoded submission
    // order and the identity drift used by the simulated latest-ready policy.
    size_t submission_ordinal = 0;
  };

  struct Request {
    Identity identity;
    VASurfaceID owner = VA_INVALID_SURFACE;
    uint64_t entered_at_ns = 0;
    uint64_t exact_return_at_ns = 0;
  };

  struct Policy {
    uint64_t delay_ns = 0;
    uint64_t maximum_owner_age_ns = UINT64_MAX;
    size_t maximum_identity_drift = std::numeric_limits<size_t>::max();
    size_t maximum_older_ready_alternatives_skipped =
        std::numeric_limits<size_t>::max();
  };

  enum class Outcome {
    ConditionalOpportunity,
    NoReadyCandidate,
    ExactRequestCompletedFirst,
    OwnerAgeBoundExceeded,
    IdentityDriftBoundExceeded,
    ReadyAlternativeBoundExceeded,
  };

  struct Result {
    Outcome outcome = Outcome::NoReadyCandidate;
    VAContextID decode_context = VA_INVALID_ID;
    Request request;
    uint64_t decision_at_ns = 0;
    PictureTrace candidate;
    uint64_t owner_age_ns = 0;
    size_t submission_drift = 0;
    size_t older_ready_alternatives_skipped = 0;
  };

  ReadyInventoryReplay(VAContextID decode_context,
                       std::initializer_list<PictureTrace> pictures)
      : decode_context_(decode_context), pictures_(pictures) {
    Require(decode_context != VA_INVALID_ID,
            "READY replay is scoped to one valid decode context");
    for (size_t index = 0; index < pictures_.size(); ++index) {
      PictureTrace &picture = pictures_[index];
      picture.submission_ordinal = index + 1;
      Require(picture.identity.driver_token != 0 &&
                  picture.identity.generation != 0 &&
                  picture.owner != VA_INVALID_SURFACE &&
                  picture.owned_at_ns != 0 &&
                  (picture.hardware_complete_at_ns == 0 ||
                   picture.hardware_complete_at_ns >= picture.owned_at_ns) &&
                  (picture.materialized_at_ns == 0 ||
                   (picture.hardware_complete_at_ns != 0 &&
                    picture.materialized_at_ns >=
                        picture.hardware_complete_at_ns)) &&
                  (picture.released_at_ns == 0 ||
                   picture.released_at_ns >= picture.owned_at_ns) &&
                  (picture.discarded_at_ns == 0 ||
                   (picture.hardware_complete_at_ns != 0 &&
                    picture.materialized_at_ns == 0 &&
                    picture.discarded_at_ns >=
                        picture.hardware_complete_at_ns)),
              "READY inventory has valid identity, ownership and completion times");
      for (size_t previous = 0; previous < index; ++previous) {
        Require(!(pictures_[previous].identity == picture.identity),
                "READY inventory has unique driver identities per generation");
        if (pictures_[previous].owner == picture.owner)
          Require(pictures_[previous].released_at_ns != 0 &&
                      pictures_[previous].released_at_ns <=
                          picture.owned_at_ns,
                  "READY inventory surface reuse ends the old owner binding");
      }
    }
  }

  Result Simulate(const Request &request) const {
    return Simulate(request, Policy{});
  }

  Result Simulate(const Request &request, const Policy &policy) const {
    const PictureTrace *requested = Find(request.identity);
    Require(requested != nullptr && requested->owner == request.owner &&
                request.entered_at_ns < request.exact_return_at_ns &&
                IsOwned(*requested, request.entered_at_ns) &&
                !IsReady(*requested, request.entered_at_ns) &&
                IsOwned(*requested, request.exact_return_at_ns) &&
                IsReady(*requested, request.exact_return_at_ns),
            "READY replay uses an unresolved request and its original exact return");

    Result result;
    result.decode_context = decode_context_;
    result.request = request;
    const uint64_t remaining =
        std::numeric_limits<uint64_t>::max() - request.entered_at_ns;
    result.decision_at_ns =
        policy.delay_ns > remaining
            ? std::numeric_limits<uint64_t>::max()
            : request.entered_at_ns + policy.delay_ns;
    const uint64_t exact_ready_at_ns = ReadyAt(*requested);
    const uint64_t exact_horizon_ns =
        std::min(exact_ready_at_ns, request.exact_return_at_ns);
    if (result.decision_at_ns >= exact_horizon_ns) {
      result.decision_at_ns = exact_horizon_ns;
      result.outcome = Outcome::ExactRequestCompletedFirst;
      return result;
    }

    std::vector<const PictureTrace *> candidates;
    bool rejected_for_age = false;
    bool rejected_for_drift = false;
    for (const PictureTrace &picture : pictures_) {
      // Cross-generation exclusion is an experimental selection boundary. It
      // does not claim that production revokes an already-ready old surface.
      if (picture.identity == request.identity ||
          picture.identity.generation != request.identity.generation ||
          picture.submission_ordinal >= requested->submission_ordinal ||
          !IsOwned(picture, result.decision_at_ns) ||
          !IsReady(picture, result.decision_at_ns))
        continue;
      if (result.decision_at_ns - picture.owned_at_ns >
          policy.maximum_owner_age_ns) {
        rejected_for_age = true;
        continue;
      }
      if (requested->submission_ordinal - picture.submission_ordinal >
          policy.maximum_identity_drift) {
        rejected_for_drift = true;
        continue;
      }
      candidates.push_back(&picture);
    }
    if (candidates.empty()) {
      result.outcome = rejected_for_age
                           ? Outcome::OwnerAgeBoundExceeded
                           : (rejected_for_drift
                                  ? Outcome::IdentityDriftBoundExceeded
                                  : Outcome::NoReadyCandidate);
      return result;
    }

    const PictureTrace *latest = *std::max_element(
        candidates.begin(), candidates.end(),
        [](const PictureTrace *left, const PictureTrace *right) {
          return left->submission_ordinal < right->submission_ordinal;
        });
    result.older_ready_alternatives_skipped = candidates.size() - 1;
    if (result.older_ready_alternatives_skipped >
        policy.maximum_older_ready_alternatives_skipped) {
      result.outcome = Outcome::ReadyAlternativeBoundExceeded;
      return result;
    }
    result.outcome = Outcome::ConditionalOpportunity;
    result.candidate = *latest;
    result.owner_age_ns = result.decision_at_ns - latest->owned_at_ns;
    result.submission_drift =
        requested->submission_ordinal - latest->submission_ordinal;
    return result;
  }

  static std::string Describe(const Result &result) {
    const char *outcome = "invalid";
    switch (result.outcome) {
      case Outcome::ConditionalOpportunity:
        outcome = "conditional-opportunity";
        break;
      case Outcome::NoReadyCandidate:
        outcome = "no-ready-candidate";
        break;
      case Outcome::ExactRequestCompletedFirst:
        outcome = "exact-request-completed-first";
        break;
      case Outcome::OwnerAgeBoundExceeded:
        outcome = "owner-age-bound-exceeded";
        break;
      case Outcome::IdentityDriftBoundExceeded:
        outcome = "identity-drift-bound-exceeded";
        break;
      case Outcome::ReadyAlternativeBoundExceeded:
        outcome = "ready-alternative-bound-exceeded";
        break;
    }
    char description[768];
    std::snprintf(
        description, sizeof(description),
        "READY inventory replay: scope=original-blocking-request-timeline "
        "context=%u request_driver_token=%llu generation=%llu owner=%u "
        "entry_ns=%llu exact_return_ns=%llu result=%s candidate_driver_token=%llu "
        "generation=%llu owner=%u owned_ns=%llu materialized_ns=%llu "
        "hardware_complete_ns=%llu owner_age_ns=%llu submission_drift=%zu "
        "older_ready_alternatives_skipped=%zu actual_client_drops=not-modeled "
        "predicted_fps=not-modeled physical_display=not-observed",
        result.decode_context,
        static_cast<unsigned long long>(result.request.identity.driver_token),
        static_cast<unsigned long long>(result.request.identity.generation),
        result.request.owner,
        static_cast<unsigned long long>(result.request.entered_at_ns),
        static_cast<unsigned long long>(result.request.exact_return_at_ns),
        outcome,
        static_cast<unsigned long long>(result.candidate.identity.driver_token),
        static_cast<unsigned long long>(result.candidate.identity.generation),
        result.candidate.owner,
        static_cast<unsigned long long>(result.candidate.owned_at_ns),
        static_cast<unsigned long long>(result.candidate.materialized_at_ns),
        static_cast<unsigned long long>(
            result.candidate.hardware_complete_at_ns),
        static_cast<unsigned long long>(result.owner_age_ns),
        result.submission_drift,
        result.older_ready_alternatives_skipped);
    return description;
  }

 private:
  const PictureTrace *Find(Identity identity) const {
    for (const PictureTrace &picture : pictures_)
      if (picture.identity == identity)
        return &picture;
    return nullptr;
  }

  static uint64_t ReadyAt(const PictureTrace &picture) {
    return std::max(picture.hardware_complete_at_ns,
                    picture.materialized_at_ns);
  }

  static bool IsOwned(const PictureTrace &picture, uint64_t at_ns) {
    return picture.owned_at_ns <= at_ns &&
           (picture.released_at_ns == 0 || at_ns < picture.released_at_ns);
  }

  static bool IsReady(const PictureTrace &picture, uint64_t at_ns) {
    return picture.hardware_complete_at_ns != 0 &&
           picture.materialized_at_ns != 0 &&
           picture.hardware_complete_at_ns <= at_ns &&
           picture.materialized_at_ns <= at_ns &&
           (picture.discarded_at_ns == 0 || at_ns < picture.discarded_at_ns);
  }

  VAContextID decode_context_ = VA_INVALID_ID;
  std::vector<PictureTrace> pictures_;
};

static ReadyInventoryReplay::PictureTrace ReadyTrace(
    uint64_t driver_token, uint64_t generation, VASurfaceID owner,
    uint64_t owned_at_ns, uint64_t hardware_complete_at_ns,
    uint64_t materialized_at_ns, uint64_t released_at_ns = 0,
    uint64_t discarded_at_ns = 0) {
  return {{driver_token, generation}, owner, owned_at_ns,
          hardware_complete_at_ns, materialized_at_ns, released_at_ns,
          discarded_at_ns, 0};
}

static void ReadyInventoryConditionalOpportunityReplay() {
  using Replay = ReadyInventoryReplay;
  using Identity = Replay::Identity;
  constexpr VAContextID context = 9;
  constexpr uint64_t generation = 7;

  // Opaque synthetic driver tokens intentionally do not resemble media PTS.
  {
    // Deliberately make token magnitude disagree with submission order.
    const Identity older{0x901, generation};
    const Identity requested{0x205, generation};
    Replay replay(context,
                  {ReadyTrace(older.driver_token, generation, 11, 2, 8, 10),
                   ReadyTrace(requested.driver_token, generation, 12, 15, 50,
                              52)});
    const auto result = replay.Simulate({requested, 12, 20, 52});
    Require(result.outcome == Replay::Outcome::ConditionalOpportunity &&
                result.candidate.identity == older &&
                result.candidate.owner == 11 &&
                result.candidate.owned_at_ns == 2 &&
                result.candidate.hardware_complete_at_ns == 8 &&
                result.candidate.materialized_at_ns == 10 &&
                result.owner_age_ns == 18 &&
                result.submission_drift == 1 &&
                result.older_ready_alternatives_skipped == 0,
            "older completed, materialized, owned picture is a conditional opportunity");
    const std::string description = Replay::Describe(result);
    Require(description.find("scope=original-blocking-request-timeline") !=
                    std::string::npos &&
                description.find("predicted_fps=not-modeled") !=
                    std::string::npos &&
                description.find("actual_client_drops=not-modeled") !=
                    std::string::npos &&
                description.find("physical_display=not-observed") !=
                    std::string::npos,
            "READY result cannot be read as predicted FPS or physical display");
    std::puts(description.c_str());
  }

  {
    const Identity requested{0x301, generation};
    Replay replay(
        context,
        {ReadyTrace(requested.driver_token, generation, 21, 2, 30, 32)});
    Require(replay.Simulate({requested, 21, 5, 32}).outcome ==
                Replay::Outcome::NoReadyCandidate,
            "an unresolved request alone provides no READY opportunity");
  }

  {
    const Identity requested{0x311, generation};
    Replay replay(context, {
        ReadyTrace(requested.driver_token, generation, 22, 2, 30, 32),
        ReadyTrace(0x011, generation, 23, 4, 6, 7),
    });
    Require(replay.Simulate({requested, 22, 10, 32}).outcome ==
                Replay::Outcome::NoReadyCandidate,
            "latest-ready cannot substitute a later-submitted identity");
  }

  {
    const Identity requested{0x403, generation};
    Replay replay(context, {
        ReadyTrace(0x401, generation, 31, 1, 4, 0, 0, 5),
        ReadyTrace(0x402, generation, 32, 2, 6, 0),
        ReadyTrace(requested.driver_token, generation, 33, 8, 40, 42),
    });
    Require(replay.Simulate({requested, 33, 10, 42}).outcome ==
                Replay::Outcome::NoReadyCandidate,
            "completed discarded or unmaterialized output is never READY");
  }

  {
    const Identity requested{0x503, generation};
    Replay replay(context, {
        ReadyTrace(0x501, generation, 41, 1, 3, 4, 6),
        ReadyTrace(0x502, generation, 41, 6, 0, 0),
        ReadyTrace(requested.driver_token, generation, 42, 8, 30, 32),
    });
    Require(replay.Simulate({requested, 42, 10, 32}).outcome ==
                Replay::Outcome::NoReadyCandidate,
            "surface reuse revokes the old completed owner binding");
  }

  {
    const Identity requested{0x602, generation + 1};
    Replay replay(context, {
        ReadyTrace(0x601, generation, 51, 1, 3, 4),
        ReadyTrace(requested.driver_token, requested.generation, 52, 8, 30, 32),
    });
    Require(replay.Simulate({requested, 52, 10, 32}).outcome ==
                Replay::Outcome::NoReadyCandidate,
            "experimental selection excludes an old-generation READY candidate");
  }

  {
    const Identity delayed{0x701, generation};
    const Identity requested{0x702, generation};
    Replay replay(context, {
        ReadyTrace(delayed.driver_token, generation, 61, 1, 12, 14),
        ReadyTrace(requested.driver_token, generation, 62, 5, 40, 42),
    });
    const Replay::Request request{requested, 62, 10, 42};
    Require(replay.Simulate(request).outcome ==
                Replay::Outcome::NoReadyCandidate,
            "latest-ready has no candidate at original request entry");
    Replay::Policy policy;
    policy.delay_ns = 8;
    const auto delayed_result = replay.Simulate(request, policy);
    Require(delayed_result.outcome ==
                    Replay::Outcome::ConditionalOpportunity &&
                delayed_result.candidate.identity == delayed &&
                delayed_result.decision_at_ns == 18 &&
                delayed_result.owner_age_ns == 17,
            "bounded delay finds materialized output within the original wait");
    policy.delay_ns = 40;
    Require(replay.Simulate(request, policy).outcome ==
                Replay::Outcome::ExactRequestCompletedFirst,
            "bounded replay never extends past the original exact return");
  }

  {
    const Identity requested{0x804, generation};
    Replay replay(context, {
        ReadyTrace(0x801, generation, 71, 1, 4, 5),
        ReadyTrace(0x802, generation, 72, 2, 6, 7),
        ReadyTrace(0x803, generation, 73, 3, 8, 9),
        ReadyTrace(requested.driver_token, generation, 74, 50, 90, 92),
    });
    const Replay::Request request{requested, 74, 60, 92};
    Replay::Policy policy;
    policy.maximum_owner_age_ns = 40;
    Require(replay.Simulate(request, policy).outcome ==
                Replay::Outcome::OwnerAgeBoundExceeded,
            "READY replay rejects candidates outside the owner-clock age bound");
    policy.maximum_owner_age_ns = UINT64_MAX;
    policy.maximum_identity_drift = 0;
    Require(replay.Simulate(request, policy).outcome ==
                Replay::Outcome::IdentityDriftBoundExceeded,
            "READY replay bounds historical identity drift independently of age");
    policy.maximum_identity_drift = std::numeric_limits<size_t>::max();
    policy.maximum_older_ready_alternatives_skipped = 1;
    Require(replay.Simulate(request, policy).outcome ==
                Replay::Outcome::ReadyAlternativeBoundExceeded,
            "READY replay bounds older alternatives without claiming client drops");
    policy.maximum_older_ready_alternatives_skipped = 2;
    const auto bounded = replay.Simulate(request, policy);
    Require(bounded.outcome == Replay::Outcome::ConditionalOpportunity &&
                bounded.candidate.identity.driver_token == 0x803 &&
                bounded.older_ready_alternatives_skipped == 2 &&
                bounded.submission_drift == 1,
            "bounded latest-ready reports drift and skipped READY alternatives");
  }
}

static void QueryOnlyObservesAutonomousDecodeProgress() {
  TeardownFixture fixture(1);
  const std::thread::id caller = std::this_thread::get_id();
  fixture.driver.StartDecodeWorker();

  const auto input_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      if (fixture.io.inputs.size() == 1 && fixture.io.output_polls >= 3)
        break;
    }
    Require(std::chrono::steady_clock::now() < input_deadline,
            "decode worker submits input and periodically polls output");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  {
    std::lock_guard<std::mutex> lock(fixture.driver.mutex);
    Require(std::find(fixture.io.events.begin(), fixture.io.events.end(),
                      "seal") == fixture.io.events.end() &&
                !fixture.decoder->ReplaySealed(),
            "autonomous progress never infers EOS from an input gap");
    // Hardware completion has no frontend condition-variable notification.
    // The worker must discover it through bounded periodic polling.
    fixture.io.outputs.push_back(kTimestampStep);
  }

  VASurfaceStatus status = VASurfaceRendering;
  const auto output_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  do {
    Require(QuerySurfaceStatus(&fixture.context, 1, &status) ==
                VA_STATUS_SUCCESS,
            "query-only frontend can inspect autonomous decode progress");
    if (status == VASurfaceReady)
      break;
    Require(std::chrono::steady_clock::now() < output_deadline,
            "decode worker publishes output without a sync call");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (true);

  std::lock_guard<std::mutex> lock(fixture.driver.mutex);
  Require(fixture.held[0]->ready && !fixture.held[0]->failed &&
              fixture.held[0]->frame_timestamp == kTimestampStep &&
              fixture.decoder->pending.empty() &&
              fixture.decoder->replay.outstanding() == 0 &&
              fixture.io.outputs.empty() &&
              fixture.io.output_thread != caller && !fixture.io.invalid,
          "worker publishes the exact picture and releases its output once");
  Require(std::count(fixture.io.events.begin(), fixture.io.events.end(),
                     "picture") == 1 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "release") == 1 &&
              std::find(fixture.io.events.begin(), fixture.io.events.end(),
                        "seal") == fixture.io.events.end(),
          "query-only progress neither duplicates output nor seals the stream");
}

static void AutonomousWorkerFinishesOnlyAnExistingSealedBatch() {
  TeardownFixture fixture(2);
  Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                  VA_STATUS_SUCCESS &&
              fixture.io.inputs.size() == 2 && fixture.decoder->replay.Seal(),
          "prepare an already-sealed batch for autonomous draining");
  fixture.io.outputs.push_back(kTimestampStep);
  fixture.io.outputs.push_back(2 * kTimestampStep);
  fixture.io.outputs.push_back(0);
  fixture.driver.StartDecodeWorker();

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      if (fixture.held[0]->ready && fixture.held[1]->ready &&
          !fixture.decoder->ReplaySealed())
        break;
    }
    Require(std::chrono::steady_clock::now() < deadline,
            "decode worker drains exact pictures and an existing EOS marker");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::lock_guard<std::mutex> lock(fixture.driver.mutex);
  Require(fixture.decoder->pending.empty() &&
              fixture.decoder->replay.outstanding() == 0 &&
              fixture.io.outputs.empty() &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "picture") == 2 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "eos") == 1 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "release") == 3 &&
              std::find(fixture.io.events.begin(), fixture.io.events.end(),
                        "seal") == fixture.io.events.end() &&
              !fixture.io.invalid,
          "worker can finish a sealed batch but never creates its EOS fence");
}

static void AutonomousWorkerStopsAfterTransportFailure() {
  TeardownFixture fixture(1);
  fixture.io.fail_poll = true;
  fixture.driver.StartDecodeWorker();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  unsigned int failed_polls = 0;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      if (fixture.held[0]->failed) {
        failed_polls = fixture.io.output_polls;
        break;
      }
    }
    Require(std::chrono::steady_clock::now() < deadline,
            "worker publishes a terminal transport failure");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  {
    std::lock_guard<std::mutex> lock(fixture.driver.mutex);
    Require(fixture.decoder->ReplayFailed() && failed_polls == 1 &&
                fixture.io.output_polls == failed_polls &&
                fixture.io.inputs.empty() && !fixture.io.invalid,
            "terminal failure stops autonomous polling and input submission");
  }
  VASurfaceStatus status = VASurfaceRendering;
  Require(QuerySurfaceStatus(&fixture.context, 1, &status) ==
              VA_STATUS_ERROR_DECODING_ERROR,
          "query-only frontend observes the terminal worker failure");
}

int main() {
  try {
    SealedBatchQueuesAndReplays();
    RejectIncompleteAndUnboundedReplay();
    ReclaimOnlyCompletedIdrPrefixes();
    BoundedLiveStreamRetainsEveryOutstandingPicture();
    LiveHistoryRestoresOnlyAtAnIntactActualIdr();
    LiveEosNeverSkipsAnAcceptedContinuation();
    LiveStreamMarkerSurvivesReplayAndPruning();
    UnknownOutputNeverGuessesAndInvalidOutputFails();
    DuplicateOutputCannotMutateOrRetirePixels();
    TraceIdentityUsesImmutableTokenAndLiveAliasOwner();
    TraceReadExportReuseBoundary();
    ExperimentalReadyRemapGate();
    ExperimentalReadyRemapFailClosedRaces();
    ExperimentalReadyRemapBounds();
    ExperimentalReadyRemapStateMachine();
    ConcurrentSyncObservesExperimentalPresentation();
    DirectOutputOwnershipAndExport();
    RejectIncompleteGeometryAndInitializeAllocationPadding();
    FailedCpuOwnershipNeverCompletesDecode();
    StaleUnretainedOutputSkipsOnlyPixelMaterialization();
    StaleUnretainedOutputPreservesTransportAndEos();
    PaddingOnlyCopyByteEquivalence();
    ExactBoundaryConversionByteEquivalence();
    CompletedLiveTailPrecedesUnreconstructibleContinuation();
    DecoderResetPreservesItsExplicitLivePolicy();
    LiveH264SpeculativePrefeedSequence();
    LiveH264PrefeedBusyFallback();
    LiveH264PrefeedEligibilityAndGhostGuard();
    TransportProgressRequiresActualHardwareIo();
    ProgressingSyncDoesNotSealAnActiveStream();
    EarlyLiveGapPreservesIntactReplayHistory();
    InactiveLiveSyncPreservesReferencesUntilActualInputResumes();
    InactiveLiveSyncHonorsFiniteDeadlineWithoutPoisoningPixels();
    ExplicitLiveContextCloseStillDrainsItsExactTail();
    ActualIdrPreservesLivePolicyUntilExplicitDrain();
    InactiveSyncStillSealsItsExactFinalPicture();
    InactiveSyncStillSealsItsExactFinalPicture(true);
    ProgressDoesNotExtendFiniteSyncDeadline();
    ContextTeardownDrainsAcceptedTailBeforeClose();
    ContextDrainRejectsMutationAndSurvivesSurfaceRelease();
    WaitingSyncKeepsCompletedRetiredPicture();
    WaitingVppCancellationStillWins();
    ContextDrainFailureDoesNotInventTail();
    CompletedWaitNeverAcceptsDifferentOrDestroyedPicture();
    ZeroTimeoutNeverPumpsBusyDecodeSurface();
    ExactReadySurfaceDoesNotWaitForBatchEos();
    ReadyInventoryConditionalOpportunityReplay();
    QueryOnlyObservesAutonomousDecodeProgress();
    AutonomousWorkerFinishesOnlyAnExistingSealedBatch();
    AutonomousWorkerStopsAfterTransportFailure();
    std::puts("VA-API sealed-batch replay, immutable-output and teardown tests passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "VA-API replay test failed: %s\n", error.what());
    return 1;
  }
}
