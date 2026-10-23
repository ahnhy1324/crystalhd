// SPDX-License-Identifier: LGPL-2.1-or-later
// Production replay state and output processing with private CPU memory only.
// No device, decoder, capture, firmware, or DRM operation is started.
#define CRYSTALHD_VAAPI_TEST_LIBRARY_CAPABILITIES 1
#include "../filters/vaapi/crystalhd_drv_video.cpp"
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>

namespace allocation_fault {
thread_local bool fail_next = false;
thread_local size_t fail_size = 0;
void *Allocate(size_t bytes) {
  if (fail_next) {
    fail_next = false;
    throw std::bad_alloc();
  }
  if (fail_size != 0 && bytes == fail_size) {
    fail_size = 0;
    throw std::bad_alloc();
  }
  if (void *result = std::malloc(bytes == 0 ? 1 : bytes))
    return result;
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
CRYSTALHD_TEST_NOINLINE void operator delete(void *pointer) noexcept {
  std::free(pointer);
}
CRYSTALHD_TEST_NOINLINE void operator delete[](void *pointer) noexcept {
  std::free(pointer);
}
CRYSTALHD_TEST_NOINLINE void operator delete(void *pointer, size_t) noexcept {
  std::free(pointer);
}
CRYSTALHD_TEST_NOINLINE void operator delete[](void *pointer, size_t) noexcept {
  std::free(pointer);
}
CRYSTALHD_TEST_NOINLINE void operator delete(
    void *pointer, const std::nothrow_t &) noexcept {
  std::free(pointer);
}
CRYSTALHD_TEST_NOINLINE void operator delete[](
    void *pointer, const std::nothrow_t &) noexcept {
  std::free(pointer);
}
#undef CRYSTALHD_TEST_NOINLINE

static int mock_sync_fd = -1;
static uint64_t mock_failed_sync_flags = 0;
static std::vector<uint64_t> mock_sync_calls;
static int mock_decoded_domain_fd = -1;
static uint32_t mock_decoded_domain_handle = 0;
static std::vector<std::pair<uint32_t, uint32_t>> mock_decoded_domain_calls;
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
static int control_read_probe_fd = -1;
static unsigned int control_read_probe_calls = 0;

// Hold one real worker poll after it borrowed the descriptor, without holding
// Driver::mutex. The scoped test below joins the worker before removing this
// pointer; no descriptor or device operation is replaced by a fake success.
struct WorkerPollBarrier {
  std::mutex mutex;
  std::condition_variable condition;
  int descriptor = -1;
  bool entered = false, release = false, expired = false;
  bool destroy_done = false, descriptor_closed = false;
  VAStatus destroy_status = VA_STATUS_ERROR_OPERATION_FAILED;
  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      release = true;
    }
    condition.notify_all();
  }
};
static WorkerPollBarrier *worker_poll_barrier = nullptr;

struct TerminalCloseBarrier {
  std::mutex mutex;
  std::condition_variable condition;
  std::string stage;
  bool entered = false, released = false, expired = false;
  explicit TerminalCloseBarrier(const char *name) : stage(name) {}
  void Block(const char *name) {
    if (stage != name) return;
    std::unique_lock<std::mutex> lock(mutex);
    if (entered) return;
    entered = true;
    condition.notify_all();
    expired = !condition.wait_for(lock, std::chrono::seconds(5),
                                  [&] { return released; });
  }
  bool Wait() {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(2),
                               [&] { return entered; });
  }
  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      released = true;
    }
    condition.notify_all();
  }
};

extern "C" ssize_t __real_read(int fd, void *buffer, size_t count);
extern "C" ssize_t __wrap_read(int fd, void *buffer, size_t count) {
  if (fd == control_read_probe_fd)
    ++control_read_probe_calls;
  return __real_read(fd, buffer, count);
}

extern "C" ssize_t __real___read_chk(int, void *, size_t, size_t);
extern "C" ssize_t __wrap___read_chk(int fd, void *buffer, size_t count,
                                     size_t buffer_size) {
  if (fd == control_read_probe_fd)
    ++control_read_probe_calls;
  return __real___read_chk(fd, buffer, count, buffer_size);
}

extern "C" int __real_ioctl(int fd, unsigned long request, ...);
extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
  va_list arguments;
  va_start(arguments, request);
  void *argument = va_arg(arguments, void *);
  va_end(arguments);
  if (mock_decoded_domain_fd >= 0 && fd == mock_decoded_domain_fd &&
      request == DRM_IOCTL_I915_GEM_SET_DOMAIN) {
    const auto *domain = static_cast<drm_i915_gem_set_domain *>(argument);
    if (domain->handle != mock_decoded_domain_handle)
      throw std::runtime_error("decoded materialization used a foreign GEM handle");
    mock_decoded_domain_calls.emplace_back(domain->read_domains, domain->write_domain);
    return 0;
  }
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
  if (worker_poll_barrier != nullptr && count > 1 && fds != nullptr &&
      std::any_of(fds + 1, fds + count, [](const pollfd &fd) {
        return fd.fd == worker_poll_barrier->descriptor && fd.events == POLLIN;
      })) {
    std::unique_lock<std::mutex> lock(worker_poll_barrier->mutex);
    if (!worker_poll_barrier->entered) {
      worker_poll_barrier->entered = true;
      worker_poll_barrier->condition.notify_all();
      worker_poll_barrier->expired = !worker_poll_barrier->condition.wait_for(
          lock, std::chrono::seconds(5),
          [] { return worker_poll_barrier->release; });
    }
    lock.unlock();
    return __real_poll(fds, count, timeout);
  }
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
  std::deque<uint32_t> picture_flags;
  std::vector<uint8_t> pixels;
  std::vector<std::string> events;
  std::vector<std::string> setup_events;
  BC_INPUT_FORMAT input_format = {};
  BC_SCALING_PARAMS scaling = {};
  unsigned int scale_calls = 0;
  BC_STATUS scale_status = BC_STS_SUCCESS;
  std::function<void()> on_sleep;
  std::function<void(const char *)> on_close;
  bool early_eos = false;
  bool zero_timestamp_picture = false;
  bool fail_poll = false;
  bool input_busy = false;
  bool prefeed_busy_once = false;
  bool remainder_busy_once = false;
  bool allow_device_open = false;
  bool exclusive_device_open = false;
  bool device_owned = false;
  BC_STATUS device_open_status = BC_STS_SUCCESS;
  bool report_bcm70012 = false;
  bool fail_set_input_format = false;
  bool invalid = false;
  int readiness_event = -1;
  uint64_t prefed_timestamp = 0;
  uint32_t tx_free_size = 1024 * 1024;
  unsigned int output_width = 16;
  unsigned int output_height = 16;
  unsigned int open_attempts = 0;
  unsigned int decoder_open_calls = 0;
  unsigned int capture_start_calls = 0;
  HANDLE decoder_open_handle = nullptr;
  uint32_t library_capabilities = 0;
  BC_STATUS library_capabilities_status = BC_STS_SUCCESS;
  unsigned int library_capability_queries = 0;
  unsigned int output_polls = 0;
  unsigned int eos_queries = 0;
  unsigned int eos_after_queries = 0;
  unsigned int device_close_busy = 0;
  unsigned int device_close_calls = 0;
  BC_STATUS stop_status = BC_STS_SUCCESS;
  BC_STATUS decoder_close_status = BC_STS_SUCCESS;
  BC_STATUS capture_start_status = BC_STS_SUCCESS;
  BC_STATUS device_close_status = BC_STS_SUCCESS;
  bool override_output = false;
  BC_DTS_PROC_OUT overridden_output = {};
  BC_STATUS overridden_output_status = BC_STS_SUCCESS;
  BC_STATUS output_release_status = BC_STS_SUCCESS;
  std::vector<uint32_t> status_requests;
  std::thread::id output_thread;
  std::atomic<unsigned int> accepted_inputs{0};
  std::chrono::microseconds input_delay{0};
  HANDLE handle() { return static_cast<HANDLE>(this); }
  static uint8_t Luma(uint64_t timestamp) {
    return static_cast<uint8_t>(40 + timestamp / kTimestampStep * 10);
  }
};

// Worker tests publish this pointer before starting the decode thread and
// clear it only after Driver has joined that thread. Production Dts calls are
// serialized by Driver::mutex, so the shared mock observes the same ordering.
static DecodeIoMock *decode_io_mock = nullptr;
static BC_STATUS MockLibraryCapabilities(uint32_t *capabilities);
struct MockDecodeScope {
  CrystalHDLibraryCapabilitiesQuery previous_capabilities;
  explicit MockDecodeScope(DecodeIoMock &mock)
      : previous_capabilities(crystalhd_test_library_capabilities) {
    decode_io_mock = &mock;
    crystalhd_test_library_capabilities = MockLibraryCapabilities;
  }
  ~MockDecodeScope() {
    crystalhd_test_library_capabilities = previous_capabilities;
    decode_io_mock = nullptr;
  }
};

static BC_STATUS MockLibraryCapabilities(uint32_t *capabilities) {
  if (capabilities == nullptr)
    return BC_STS_INV_ARG;
  *capabilities = 0;
  if (decode_io_mock == nullptr)
    return BC_STS_NOT_IMPL;
  ++decode_io_mock->library_capability_queries;
  *capabilities = decode_io_mock->library_capabilities;
  return decode_io_mock->library_capabilities_status;
}

extern "C" BC_STATUS __real_DtsDeviceOpen(HANDLE *, uint32_t);
extern "C" BC_STATUS __wrap_DtsDeviceOpen(HANDLE *device, uint32_t mode) {
  if (decode_io_mock == nullptr)
    return __real_DtsDeviceOpen(device, mode);
  ++decode_io_mock->open_attempts;
  decode_io_mock->invalid |= device == nullptr ||
                             (device != nullptr && *device != nullptr);
  if (decode_io_mock->device_open_status != BC_STS_SUCCESS)
    return decode_io_mock->device_open_status;
  if (decode_io_mock->exclusive_device_open && decode_io_mock->device_owned)
    return BC_STS_DEC_EXIST_OPEN;
  if (decode_io_mock->allow_device_open) {
    *device = decode_io_mock->handle();
    decode_io_mock->device_owned = true;
    return BC_STS_SUCCESS;
  }
  return BC_STS_ERROR;  // An unexpected reopen must never touch real hardware.
}

extern "C" BC_STATUS __real_DtsDupOutputPollFd(HANDLE, int *);
extern "C" BC_STATUS __wrap_DtsDupOutputPollFd(HANDLE device, int *poll_fd) {
  if (decode_io_mock == nullptr)
    return __real_DtsDupOutputPollFd(device, poll_fd);
  decode_io_mock->invalid |=
      device != decode_io_mock->handle() || poll_fd == nullptr;
  if (poll_fd != nullptr)
    *poll_fd = -1;
  return BC_STS_NOT_IMPL;
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
  decode_io_mock->setup_events.push_back("input-format");
  if (format != nullptr)
    decode_io_mock->input_format = *format;
  return decode_io_mock->fail_set_input_format ? BC_STS_ERROR
                                                : BC_STS_SUCCESS;
}

extern "C" BC_STATUS __real_DtsSetScaleParams(HANDLE, BC_SCALING_PARAMS *);
extern "C" BC_STATUS __wrap_DtsSetScaleParams(
    HANDLE device, BC_SCALING_PARAMS *scaling) {
  if (decode_io_mock == nullptr)
    return __real_DtsSetScaleParams(device, scaling);
  decode_io_mock->invalid |= device != decode_io_mock->handle() ||
                             scaling == nullptr;
  decode_io_mock->setup_events.push_back("scale");
  ++decode_io_mock->scale_calls;
  if (scaling != nullptr)
    decode_io_mock->scaling = *scaling;
  return decode_io_mock->scale_status;
}

#define MOCK_OPEN_OPERATION(name, arguments, validation) \
  extern "C" BC_STATUS __real_##name arguments; \
  extern "C" BC_STATUS __wrap_##name arguments { \
    if (decode_io_mock == nullptr) \
      return __real_##name validation; \
    decode_io_mock->invalid |= device != decode_io_mock->handle(); \
    return BC_STS_SUCCESS; \
  }
MOCK_OPEN_OPERATION(DtsSetColorSpace, (HANDLE device, BC_OUTPUT_FORMAT format),
                    (device, format))
MOCK_OPEN_OPERATION(DtsStartDecoder, (HANDLE device), (device))
#undef MOCK_OPEN_OPERATION

extern "C" BC_STATUS __real_DtsOpenDecoder(HANDLE, uint32_t);
extern "C" BC_STATUS __wrap_DtsOpenDecoder(HANDLE device, uint32_t stream_type) {
  if (decode_io_mock == nullptr)
    return __real_DtsOpenDecoder(device, stream_type);
  decode_io_mock->invalid |= device != decode_io_mock->handle();
  ++decode_io_mock->decoder_open_calls;
  decode_io_mock->decoder_open_handle = device;
  decode_io_mock->setup_events.push_back("open-decoder");
  return BC_STS_SUCCESS;
}

extern "C" BC_STATUS __real_DtsStartCapture(HANDLE);
extern "C" BC_STATUS __wrap_DtsStartCapture(HANDLE device) {
  if (decode_io_mock == nullptr)
    return __real_DtsStartCapture(device);
  decode_io_mock->invalid |= device != decode_io_mock->handle();
  ++decode_io_mock->capture_start_calls;
  return decode_io_mock->capture_start_status;
}

#define MOCK_CLOSE_OPERATION(name, event, result) \
  extern "C" BC_STATUS __real_##name(HANDLE); \
  extern "C" BC_STATUS __wrap_##name(HANDLE device) { \
    if (decode_io_mock == nullptr) \
      return __real_##name(device); \
    decode_io_mock->invalid |= device != decode_io_mock->handle(); \
    decode_io_mock->prefed_timestamp = 0; \
    decode_io_mock->events.push_back(event); \
    if (decode_io_mock->on_close) decode_io_mock->on_close(event); \
    return decode_io_mock->result; \
  }
MOCK_CLOSE_OPERATION(DtsStopDecoder, "stop", stop_status)
MOCK_CLOSE_OPERATION(DtsCloseDecoder, "close-decoder", decoder_close_status)
#undef MOCK_CLOSE_OPERATION

extern "C" BC_STATUS __real_DtsDeviceClose(HANDLE);
extern "C" BC_STATUS __wrap_DtsDeviceClose(HANDLE device) {
  if (decode_io_mock == nullptr)
    return __real_DtsDeviceClose(device);
  decode_io_mock->invalid |= device != decode_io_mock->handle();
  decode_io_mock->prefed_timestamp = 0;
  decode_io_mock->events.push_back("close-device");
  ++decode_io_mock->device_close_calls;
  if (decode_io_mock->on_close) decode_io_mock->on_close("close-device");
  if (decode_io_mock->device_close_busy != 0) {
    --decode_io_mock->device_close_busy;
    return BC_STS_BUSY;
  }
  decode_io_mock->device_owned = false;
  return decode_io_mock->device_close_status;
}

extern "C" BC_STATUS __real_DtsGetDriverStatus(HANDLE, BC_DTS_STATUS *);
extern "C" BC_STATUS __wrap_DtsGetDriverStatus(HANDLE device, BC_DTS_STATUS *status) {
  if (decode_io_mock == nullptr)
    return __real_DtsGetDriverStatus(device, status);
  decode_io_mock->invalid |= device != decode_io_mock->handle();
  ++decode_io_mock->output_polls;
  decode_io_mock->output_thread = std::this_thread::get_id();
  decode_io_mock->status_requests.push_back(status->cpbEmptySize);
  decode_io_mock->invalid |= status->cpbEmptySize != 0 &&
                             status->cpbEmptySize != kStatusSkipReadyHeadPeek;
  *status = {};
  status->ReadyListCount = decode_io_mock->outputs.size();
  if (status->ReadyListCount != 0 && decode_io_mock->readiness_event >= 0) {
    uint64_t value;
    ssize_t result;
    do {
      result = read(decode_io_mock->readiness_event, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
    decode_io_mock->invalid |=
        result != static_cast<ssize_t>(sizeof(value));
  }
  return decode_io_mock->fail_poll ? BC_STS_ERROR : BC_STS_SUCCESS;
}

extern "C" uint32_t __real_DtsTxFreeSize(HANDLE);
extern "C" uint32_t __wrap_DtsTxFreeSize(HANDLE device) {
  if (decode_io_mock == nullptr)
    return __real_DtsTxFreeSize(device);
  decode_io_mock->invalid |= device != decode_io_mock->handle();
  return decode_io_mock->tx_free_size;
}

extern "C" BC_STATUS __real_DtsIsEndOfStream(HANDLE, uint8_t *);
extern "C" BC_STATUS __wrap_DtsIsEndOfStream(HANDLE device, uint8_t *eos) {
  if (decode_io_mock == nullptr)
    return __real_DtsIsEndOfStream(device, eos);
  decode_io_mock->invalid |=
      device != decode_io_mock->handle() || eos == nullptr;
  if (eos == nullptr)
    return BC_STS_INV_ARG;
  ++decode_io_mock->eos_queries;
  *eos = decode_io_mock->eos_after_queries != 0 &&
         decode_io_mock->eos_queries >= decode_io_mock->eos_after_queries;
  return BC_STS_SUCCESS;
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
  ++decode_io_mock->accepted_inputs;
  if (decode_io_mock->input_delay.count() != 0)
    std::this_thread::sleep_for(decode_io_mock->input_delay);
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
  if (decode_io_mock->override_output) {
    *output = decode_io_mock->overridden_output;
    decode_io_mock->events.push_back("override-output");
    return decode_io_mock->overridden_output_status;
  }
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
    output->PicInfo.width = decode_io_mock->output_width;
    output->PicInfo.height = decode_io_mock->output_height;
    if (!decode_io_mock->picture_flags.empty()) {
      output->PicInfo.flags = decode_io_mock->picture_flags.front();
      decode_io_mock->picture_flags.pop_front();
    }
    const unsigned int rows =
        (output->PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) != 0
            ? output->PicInfo.height / 2U : output->PicInfo.height;
    decode_io_mock->pixels.resize(
        static_cast<size_t>(output->PicInfo.width) * rows * 2U);
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
  return decode_io_mock->output_release_status;
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

static void PrepareLinkOutput(OutputFixture *fixture) {
  fixture->decoder.is_70012 = true;
  fixture->pixels.assign(720U * fixture->output.PicInfo.height * 2U, 128);
  for (size_t byte = 0; byte < fixture->pixels.size(); byte += 2)
    fixture->pixels[byte] = 40;
  fixture->output.Ybuff = fixture->pixels.data();
  fixture->output.YBuffDoneSz = fixture->pixels.size() / 4;
}

static void EncryptedOutputNeverMaterializesOrCompletes() {
  for (bool link : {false, true}) {
    for (bool direct : {false, true}) {
      for (unsigned int mode = 0; mode != 3; ++mode) {
        OutputFixture fixture;
        if (link) PrepareLinkOutput(&fixture);
        if (direct) fixture.Direct();
        if (mode != 0) fixture.output.PoutFlags |= BC_POUT_FLAGS_ENCRYPTED;
        if (mode == 2) fixture.output.PicInfo.timeStamp = 0;
        const auto public_pixels = fixture.public_frame->storage;
        const auto private_pixels = fixture.private_frame->storage;
        const auto picture = fixture.decoder.decoded_frames.at(1);
        const VAStatus status = fixture.Process();
        if (mode == 0) {
          Require(status == VA_STATUS_SUCCESS && fixture.public_frame->ready &&
                      fixture.public_frame->planes[0][0] == 40 &&
                      (direct || fixture.private_frame->planes[0][0] == 40) &&
                      fixture.decoder.replay.outstanding() == 0,
                  "clear Link/Flea private/direct output still completes actual pixels");
        } else {
          Require(status == VA_STATUS_ERROR_DECODING_ERROR &&
                      fixture.decoder.replay.failed() &&
                      fixture.decoder.replay.outstanding() == 1 &&
                      fixture.decoder.replay.IsOutstanding(1) &&
                      fixture.decoder.decoded_frames.at(1) == picture &&
                      fixture.decoder.pending.count(1) == 1 &&
                      fixture.public_frame->storage == public_pixels &&
                      fixture.private_frame->storage == private_pixels &&
                      fixture.public_frame->frame_timestamp == 0 &&
                      fixture.private_frame->frame_timestamp == 0 &&
                      !fixture.public_frame->ready && fixture.public_frame->failed &&
                      !fixture.private_frame->ready && fixture.private_frame->failed,
                  "ENC-marked output fails before identity, retirement or private/direct pixel copy");
        }
      }
    }
  }
}

// Synthetic status/flag combinations exercise ownership rules, not evidence
// that the hardware or DIL produces encrypted pictures in these conditions.
static void EncryptedReceiveReleasesOnlyAcquiredOutput() {
  const BC_STATUS statuses[] = {BC_STS_SUCCESS, BC_STS_NO_DATA, BC_STS_BUSY,
                               BC_STS_TIMEOUT, BC_STS_ERROR, BC_STS_FMT_CHANGE};
  for (bool direct : {false, true}) {
    for (bool eos : {false, true}) {
      for (bool completed : {false, true}) {
        if (completed && !eos) continue;
        for (BC_STATUS returned : statuses) {
          for (bool release_failure : {false, true}) {
            if (release_failure && returned != BC_STS_SUCCESS) continue;
            DecodeIoMock io;
            MockDecodeScope scope(io);
            OutputFixture fixture;
            if (direct) fixture.Direct();
            if (completed)
              Require(fixture.decoder.replay.Seal() &&
                          fixture.Process() == VA_STATUS_SUCCESS,
                      "prepare completed clear batch before synthetic ENC EOS");
            fixture.decoder.prefed_timestamp = 2;
            fixture.decoder.device = io.handle();
            io.override_output = true;
            io.overridden_output = fixture.output;
            io.overridden_output.PoutFlags |= BC_POUT_FLAGS_ENCRYPTED;
            if (eos) io.overridden_output.PicInfo.flags |= VDEC_FLAG_EOS;
            io.overridden_output_status = returned;
            io.output_release_status = release_failure ? BC_STS_ERROR : BC_STS_SUCCESS;
            const auto public_pixels = fixture.public_frame->storage;
            const auto private_pixels = fixture.private_frame->storage;
            const size_t outstanding = fixture.decoder.replay.outstanding();
            bool activity = false;
            Require(ReceiveOne(&fixture.driver, &fixture.decoder, 0, &activity) ==
                        VA_STATUS_ERROR_DECODING_ERROR &&
                        activity == (returned == BC_STS_SUCCESS) &&
                        fixture.decoder.replay.failed() &&
                        fixture.decoder.prefed_timestamp == 2 &&
                        fixture.decoder.replay.outstanding() == outstanding &&
                        fixture.public_frame->storage == public_pixels &&
                        fixture.private_frame->storage == private_pixels &&
                        std::count(io.events.begin(), io.events.end(), "override-output") == 1 &&
                        std::count(io.events.begin(), io.events.end(), "release") ==
                            (returned == BC_STS_SUCCESS ? 1 : 0) &&
                        !io.invalid,
                    "ENC data/EOS fail closed and release exactly once only after successful acquisition");
          }
        }
      }
    }
    DecodeIoMock io;
    MockDecodeScope scope(io);
    OutputFixture clear;
    if (direct) clear.Direct();
    clear.decoder.device = io.handle();
    io.override_output = true;
    io.overridden_output = clear.output;
    bool activity = false;
    Require(ReceiveOne(&clear.driver, &clear.decoder, 0, &activity) == VA_STATUS_SUCCESS &&
                activity && clear.decoder.replay.outstanding() == 0 &&
                clear.public_frame->planes[0][0] == 40 &&
                std::count(io.events.begin(), io.events.end(), "release") == 1 && !io.invalid,
            "clear receive control retains successful processing and exactly-once release");
  }
}

struct H264FieldFixture {
  Driver driver{-1};
  DecodeContext decoder;
  std::shared_ptr<Surface> private_frame = std::make_shared<Surface>();
  std::shared_ptr<Surface> public_frame = std::make_shared<Surface>();
  std::vector<uint8_t> top;
  std::vector<uint8_t> bottom;

  explicit H264FieldFixture(bool direct = false, bool is_70012 = false,
                            uint32_t fourcc = VA_FOURCC_NV12,
                            unsigned int width = 16, uint64_t token = 1)
      : top((is_70012 ? 720 : width) * 16 * 2),
        bottom((is_70012 ? 720 : width) * 16 * 2) {
    decoder.id = 7;
    decoder.generation = 3;
    decoder.profile = VAProfileH264Main;
    decoder.width = width;
    decoder.height = 32;
    decoder.is_70012 = is_70012;
    decoder.picture.seq_fields.bits.frame_mbs_only_flag = 0;
    if (fourcc == VA_FOURCC_ARGB) {
      // CPU-only stand-in for a mapped ARGB render target; no GBM device.
      public_frame->width = width;
      public_frame->height = 32;
      public_frame->fourcc = fourcc;
      public_frame->pitch[0] = width * 4U;
      public_frame->storage.resize(static_cast<size_t>(width) * 32U * 4U);
      public_frame->planes[0] = public_frame->storage.data();
    } else {
      Require(public_frame->AllocateInternal(nullptr, -1, width, 32, fourcc),
              "allocate H.264 field public surface");
    }
    if (direct) {
      private_frame->width = width;
      private_frame->height = 32;
      private_frame->direct_picture = true;
      private_frame->decode_target = public_frame;
      private_frame->direct_backing = public_frame;
      public_frame->direct_decode_eligible = true;
    } else {
      Require(private_frame->AllocateInternal(nullptr, -1, width, 32, fourcc),
              "allocate H.264 field private surface");
    }
    private_frame->expected_timestamp = token;
    private_frame->decode_identity = private_frame->trace_decode_identity = 41;
    private_frame->trace_context = 7;
    private_frame->trace_generation = 3;
    private_frame->trace_submission_ordinal = 1;
    private_frame->h264_crop_unit_y = 4;
    public_frame->id = 1;
    public_frame->expected_timestamp = token;
    public_frame->decode_identity = public_frame->trace_decode_identity = 41;
    public_frame->trace_context = 7;
    public_frame->trace_generation = 3;
    public_frame->trace_submission_ordinal = 1;
    public_frame->h264_crop_unit_y = 4;
    public_frame->decode_picture = private_frame;
    driver.surfaces[1] = public_frame;
    decoder.decoded_frames[token] = private_frame;
    decoder.surface_timestamps[public_frame.get()] = token;
    decoder.pending[token] = 1;
    Require(decoder.replay.Append(token, true, {1}),
            "append H.264 field fixture input");
    SendNext(&decoder.replay, token);
    const unsigned int source_width = is_70012 ? 720 : width;
    Fill(top, source_width, 20, 40, 60, width);
    Fill(bottom, source_width, 100, 140, 160, width);
  }

  static void Fill(std::vector<uint8_t> &pixels, unsigned int source_width,
                   uint8_t luma,
                   uint8_t u, uint8_t v, unsigned int width = 16) {
    for (unsigned int row = 0; row < 16; ++row) {
      for (unsigned int column = 0; column < width; column += 2) {
        const size_t at =
            (static_cast<size_t>(row) * source_width + column) * 2;
        pixels[at] = pixels[at + 2] = luma + row;
        pixels[at + 1] = u + row;
        pixels[at + 3] = v + row;
      }
    }
  }

  BC_DTS_PROC_OUT Output(bool is_bottom, bool bottom_first,
                         std::vector<uint8_t> *pixels) const {
    BC_DTS_PROC_OUT output = {};
    output.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    output.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC |
        (is_bottom ? VDEC_FLAG_BOTTOMFIELD : VDEC_FLAG_TOPFIELD) |
        (bottom_first ? VDEC_FLAG_BOTTOM_FIRST : 0);
    output.PicInfo.timeStamp = private_frame->expected_timestamp;
    output.PicInfo.picture_number = 71;
    output.PicInfo.sess_num = 9;
    output.PicInfo.picture_meta_payload = 0x1234;
    output.PicInfo.width = decoder.width;
    output.PicInfo.height = 32;
    output.Ybuff = pixels->data();
    output.YBuffDoneSz = pixels->size() / 4;
    return output;
  }

  void CheckPixels(const Surface &surface) const {
    for (unsigned int row = 0; row < 32; ++row) {
      const unsigned int source_row = row / 2;
      const uint8_t expected = (row & 1U ? 100 : 20) + source_row;
      for (unsigned int column = 0; column < decoder.width; ++column)
        Require(surface.planes[0][static_cast<size_t>(row) *
                                      surface.pitch[0] + column] == expected,
                "field luma is woven into alternating display rows");
    }
    for (unsigned int row = 0; row < 16; ++row) {
      const bool bottom_field = (row & 1U) != 0;
      const unsigned int source_row = (row & ~1U);
      const uint8_t expected_u =
          (bottom_field ? 140 : 40) + source_row + 1;
      const uint8_t expected_v =
          (bottom_field ? 160 : 60) + source_row + 1;
      for (unsigned int column = 0; column < decoder.width; column += 2) {
        const size_t at = static_cast<size_t>(row) * surface.pitch[1] + column;
        Require(surface.planes[1][at] == expected_u &&
                    surface.planes[1][at + 1] == expected_v,
                "field chroma is downsampled within each field before weaving");
      }
    }
  }
};

struct H264WeaveFixture : H264FieldFixture {
  std::vector<uint8_t> pixels;
  BC_DTS_PROC_OUT output = {};
  BC_PIC_INFO_BLOCK raw = {};

  explicit H264WeaveFixture(bool direct = false,
                            uint32_t fourcc = VA_FOURCC_NV12,
                            unsigned int width = 16)
      : H264FieldFixture(direct, false, fourcc, width, kTimestampStep),
        pixels(static_cast<size_t>(width) * 32U * 2U + 160U, 0) {
    const size_t pitch = static_cast<size_t>(width) * 2U;
    for (unsigned int row = 0; row < 16; ++row) {
      memcpy(pixels.data() + row * 2U * pitch, top.data() + row * pitch, pitch);
      memcpy(pixels.data() + (row * 2U + 1U) * pitch,
             bottom.data() + row * pitch, pitch);
    }
    output = Output(false, false, &pixels);
    output.b422Mode = OUTPUT_MODE422_YUY2;
    output.PicInfo.flags |= VDEC_FLAG_UNKNOWN_SRC;
    raw = output.PicInfo;
    raw.timeStamp = kTimestampStep / 20000U;
    raw.flags = VDEC_FLAG_FIELDPAIR | VDEC_FLAG_UNKNOWN_SRC;
    WritePib();
    WriteWord(PayloadBytes(), output.PicInfo.picture_number);
    WriteWord(pixels.size() - 8U, width);
    WriteWord(pixels.size() - 4U, 32);
  }

  size_t PayloadBytes() const { return static_cast<size_t>(decoder.width) * 64U; }
  void WriteWord(size_t offset, uint32_t value) {
    memcpy(pixels.data() + offset, &value, sizeof(value));
  }
  void WritePib() {
    memcpy(pixels.data() + PayloadBytes() + sizeof(uint32_t), &raw,
           offsetof(BC_PIC_INFO_BLOCK, other));
  }
  VAStatus Process() { return ProcessDecodedOutput(&driver, &decoder, output); }

  void CheckArgb(const Surface &surface) const {
    for (unsigned int row = 0; row < 32; ++row) {
      const unsigned int source_row = row / 2U;
      const bool bottom_field = (row & 1U) != 0;
      uint8_t expected[4];
      StoreArgb(expected, (bottom_field ? 100 : 20) + source_row,
                (bottom_field ? 140 : 40) + source_row,
                (bottom_field ? 160 : 60) + source_row);
      for (unsigned int column = 0; column < decoder.width; ++column)
        Require(memcmp(surface.planes[0] + row * surface.pitch[0] + column * 4U,
                       expected, sizeof(expected)) == 0,
                "whole interlaced ARGB preserves both fields and row strides");
    }
  }
};

struct Mpeg2FieldFixture : H264FieldFixture {
  explicit Mpeg2FieldFixture(unsigned int width = 16)
      : H264FieldFixture(false, false, VA_FOURCC_NV12, width) {
    decoder.profile = VAProfileMPEG2Main;
    decoder.stream_width = width;
    decoder.stream_height = 32;
    decoder.have_mpeg2_sequence = true;
    decoder.mpeg2_progressive_sequence = false;
    decoder.replay = CrystalHDDecodeReplay();
    Require(decoder.mpeg2_replay.Append(1, CrystalHDMpeg2Replay::Kind::I, 0, 0, {1}) &&
                decoder.mpeg2_replay.InputSent(),
            "submit frame-coded MPEG-2 I picture to its own replay model");
  }
};

static void Mpeg2FieldPairsPreserveIdentityPixelsAndReplay() {
  for (bool bottom_first : {false, true}) {
    for (unsigned width : {16U, 18U}) {
      Mpeg2FieldFixture fixture(width);
      auto &decode = fixture.decoder;
      BC_DTS_PROC_OUT first = fixture.Output(bottom_first, bottom_first,
          bottom_first ? &fixture.bottom : &fixture.top);
      BC_DTS_PROC_OUT second = fixture.Output(!bottom_first, bottom_first,
          bottom_first ? &fixture.top : &fixture.bottom);
      Require(ProcessDecodedOutput(&fixture.driver, &decode, first) == VA_STATUS_SUCCESS &&
                  decode.h264_first_field.valid && decode.mpeg2_replay.outstanding() == 1 &&
                  decode.pending.count(1) == 1 && !fixture.public_frame->ready &&
                  !fixture.private_frame->ready && decode.replay.outstanding() == 0,
              "MPEG-2 first field retains its own replay token without early readiness");
      Require(ProcessDecodedOutput(&fixture.driver, &decode, second) == VA_STATUS_SUCCESS &&
                  !decode.h264_first_field.valid && decode.mpeg2_replay.outstanding() == 0 &&
                  decode.pending.empty() && fixture.public_frame->ready &&
                  fixture.private_frame->ready && fixture.public_frame->frame_timestamp == 1,
              "MPEG-2 complementary field completes exactly one picture");
      fixture.CheckPixels(*fixture.public_frame);
      fixture.CheckPixels(*fixture.private_frame);
      const auto frozen = fixture.public_frame->storage;
      Require(ProcessDecodedOutput(&fixture.driver, &decode, first) == VA_STATUS_SUCCESS &&
                  !decode.h264_first_field.valid && fixture.public_frame->storage == frozen,
              "retired MPEG-2 field cannot start a pair or overwrite pixels");
    }
  }
  Mpeg2FieldFixture fixture;
  auto &decode = fixture.decoder;
  auto top = fixture.Output(false, false, &fixture.top);
  auto bottom = fixture.Output(true, false, &fixture.bottom);
  Require(decode.mpeg2_replay.Seal() &&
              ProcessDecodedOutput(&fixture.driver, &decode, top) == VA_STATUS_SUCCESS &&
              ProcessDecodedOutput(&fixture.driver, &decode, bottom) == VA_STATUS_SUCCESS &&
              decode.mpeg2_replay.EndOfSequence() &&
              decode.mpeg2_replay.Append(2, CrystalHDMpeg2Replay::Kind::P, 1, 0, {2}) &&
              decode.mpeg2_replay.Restarted() && decode.mpeg2_replay.InputSent(),
          "restart retained MPEG-2 anchor after sealed EOS");
  decode.decoded_frames.erase(1);
  const auto frozen = fixture.public_frame->storage;
  Require(ProcessDecodedOutput(&fixture.driver, &decode, top) == VA_STATUS_SUCCESS &&
              decode.h264_first_field.valid && decode.h264_first_field.replay_duplicate &&
              decode.h264_first_field.pixels.empty() && decode.mpeg2_replay.IsOutstanding(1),
          "first replayed MPEG-2 field retains metadata, not duplicate pixels");
  Require(ProcessDecodedOutput(&fixture.driver, &decode, bottom) == VA_STATUS_SUCCESS &&
              !decode.h264_first_field.valid && !decode.mpeg2_replay.IsOutstanding(1) &&
              fixture.public_frame->storage == frozen,
          "validated duplicate MPEG-2 pair retires once without replacing completed pixels");
}

static void Mpeg2FieldPairsRejectMalformedAndPreserveRetainedPictures() {
  for (unsigned fault = 0; fault < 12; ++fault) {
    Mpeg2FieldFixture fixture;
    auto first = fixture.Output(false, false, &fixture.top);
    auto second = fixture.Output(true, false, &fixture.bottom);
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, first) == VA_STATUS_SUCCESS,
            "retain first MPEG-2 field before fault");
    switch (fault) {
      case 0: second.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_TOPFIELD; break;
      case 1: second.PicInfo.timeStamp = 2; break;
      case 2: ++fixture.private_frame->decode_identity; break;
      case 3: ++fixture.decoder.generation; break;
      case 4: second.PicInfo.width -= 2; break;
      case 5: second.PicInfo.flags |= VDEC_FLAG_BOTTOM_FIRST; break;
      case 6: --second.YBuffDoneSz; break;
      case 7: ++second.PicInfo.picture_number; break;
      case 8: ++second.PicInfo.sess_num; break;
      case 9: ++second.PicInfo.picture_meta_payload; break;
      case 10: second.PicInfo.flags = 0; break;
      case 11: second.YBuffDoneSz *= 2; break;
    }
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, second) ==
                VA_STATUS_ERROR_DECODING_ERROR && fixture.decoder.mpeg2_replay.failed() &&
                !fixture.decoder.h264_first_field.valid && !fixture.public_frame->ready &&
                fixture.private_frame->failed,
            "mismatched, short or ambiguous MPEG-2 fields fail without publishing pixels");
  }
  for (bool retained : {false, true}) {
    Mpeg2FieldFixture fixture;
    auto first = fixture.Output(false, false, &fixture.top);
    auto second = fixture.Output(true, false, &fixture.bottom);
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, first) == VA_STATUS_SUCCESS,
            "hold MPEG-2 field before public target reuse");
    std::shared_ptr<Surface> captured = retained ? fixture.private_frame : nullptr;
    std::weak_ptr<Surface> old = fixture.private_frame;
    auto replacement = std::make_shared<Surface>();
    fixture.public_frame->expected_timestamp = 2;
    fixture.public_frame->decode_identity = 42;
    fixture.public_frame->decode_picture = replacement;
    const auto untouched = fixture.public_frame->storage;
    fixture.private_frame.reset();
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, second) == VA_STATUS_SUCCESS &&
                fixture.decoder.mpeg2_replay.outstanding() == 0 &&
                fixture.decoder.decoded_frames.empty() && fixture.decoder.pending.empty() &&
                fixture.public_frame->storage == untouched && !fixture.public_frame->ready &&
                !fixture.public_frame->failed,
            "MPEG-2 pair cannot overwrite a rebound public picture");
    if (retained) {
      Require(captured->ready && captured->frame_timestamp == 1,
              "retained MPEG-2 picture still receives its own fields");
      fixture.CheckPixels(*captured);
    } else {
      Require(old.expired(), "unretained MPEG-2 pair releases its copied source and owner");
    }
  }
}

static void Mpeg2FieldPartialEosCloseAndRelease() {
  {
    Mpeg2FieldFixture fixture;
    auto first = fixture.Output(false, false, &fixture.top);
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, first) == VA_STATUS_SUCCESS &&
                fixture.decoder.CloseHardware() == BC_STS_SUCCESS &&
                !fixture.decoder.h264_first_field.valid &&
                fixture.decoder.have_mpeg2_sequence && !fixture.decoder.mpeg2_progressive_sequence &&
                fixture.decoder.mpeg2_replay.IsOutstanding(1),
            "hardware-only close keeps frozen MPEG-2 sequence and replay, not partial pixels");
    Require(fixture.decoder.Close() == BC_STS_SUCCESS &&
                !fixture.decoder.have_mpeg2_sequence && fixture.decoder.mpeg2_progressive_sequence &&
                fixture.decoder.mpeg2_replay.outstanding() == 0,
            "logical MPEG-2 reset clears sequence and field lifetime");
  }
  for (bool allocation_failure : {false, true}) {
    DecodeIoMock io;
    MockDecodeScope scope(io);
    Mpeg2FieldFixture fixture;
    fixture.decoder.device = io.handle();
    fixture.decoder.decoder_open = fixture.decoder.decoder_started = true;
    io.output_width = 16; io.output_height = 32;
    io.pixels.resize(16 * 16 * 2);
    io.events.reserve(8);
    io.outputs.push_back(1);
    io.picture_flags.push_back(VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_TOPFIELD);
    bool activity = false;
    if (allocation_failure) {
      allocation_fault::fail_next = true;
      Require(ReceiveOne(&fixture.driver, &fixture.decoder, 0, &activity) ==
                  VA_STATUS_ERROR_ALLOCATION_FAILED && activity && !allocation_fault::fail_next,
              "MPEG-2 field allocation failure is contained");
    } else {
      io.outputs.push_back(0);
      Require(ReceiveAvailable(&fixture.driver, &fixture.decoder) ==
                  VA_STATUS_ERROR_DECODING_ERROR,
              "MPEG-2 EOS cannot complete an unmatched field");
    }
    Require(fixture.decoder.mpeg2_replay.failed() && !fixture.decoder.h264_first_field.valid &&
                fixture.public_frame->failed && !fixture.public_frame->ready && !io.invalid &&
                std::count(io.events.begin(), io.events.end(), "release") ==
                    (allocation_failure ? 1 : 2),
            "failed MPEG-2 partial picture releases each acquired hardware buffer exactly once");
  }
}

static void H264WholeWeavePreservesIdentityAndPixels() {
  for (bool direct : {false, true}) {
    for (uint32_t fourcc : {VA_FOURCC_NV12, VA_FOURCC_ARGB}) {
      if (!direct && fourcc == VA_FOURCC_ARGB)
        continue;  // Private decode storage is NV12; ARGB is a direct target.
      for (unsigned int width : {16U, 18U}) {
        H264WeaveFixture fixture(direct, fourcc, width);
        Require(ClassifyH264InterlacedPayload(fixture.output) ==
                    H264InterlacedPayload::Weave,
                "completed PIB and converted timestamp identify whole Weave");
        Require(fixture.Process() == VA_STATUS_SUCCESS &&
                    !fixture.decoder.h264_first_field.valid &&
                    fixture.decoder.pending.empty() &&
                    fixture.decoder.replay.outstanding() == 0 &&
                    fixture.private_frame->ready && fixture.public_frame->ready &&
                    fixture.public_frame->frame_timestamp == kTimestampStep,
                "one complete interlaced transfer retires exactly one identity");
        if (fourcc == VA_FOURCC_NV12)
          fixture.CheckPixels(*fixture.public_frame);
        else
          fixture.CheckArgb(*fixture.public_frame);
        if (!direct)
          Require(fixture.private_frame->storage == fixture.public_frame->storage,
                  "private and public whole Weave pictures agree");
        const auto original = fixture.public_frame->storage;
        std::fill(fixture.pixels.begin(),
                  fixture.pixels.begin() + fixture.PayloadBytes(), 0);
        Require(fixture.Process() == VA_STATUS_SUCCESS &&
                    fixture.public_frame->storage == original &&
                    fixture.decoder.replay.outstanding() == 0,
                "duplicate whole Weave cannot rewrite materialized pixels");
      }
    }
  }

  {
    H264WeaveFixture fixture;
    const uint64_t next = kTimestampStep * 2U;
    auto replacement = std::make_shared<Surface>();
    Require(replacement->AllocateInternal(nullptr, -1, 16, 32, VA_FOURCC_NV12),
            "allocate rebound whole-Weave target");
    replacement->id = 1;
    replacement->expected_timestamp = next;
    replacement->decode_identity = 99;
    fixture.driver.surfaces[1] = replacement;
    const auto untouched = replacement->storage;
    Require(fixture.Process() == VA_STATUS_SUCCESS &&
                fixture.private_frame->ready &&
                replacement->storage == untouched && !replacement->ready &&
                fixture.decoder.replay.outstanding() == 0,
            "whole Weave retains immutable picture without touching rebound owner");
    fixture.CheckPixels(*fixture.private_frame);
  }
}

static void H264WholeWeaveRejectsMalformedOrPartialOutput() {
  for (unsigned int fault = 0; fault < 17; ++fault) {
    H264WeaveFixture fixture;
    switch (fault) {
      case 0: --fixture.output.YBuffDoneSz; break;
      case 1: fixture.WriteWord(fixture.pixels.size() - 8U, 18); break;
      case 2: fixture.WriteWord(fixture.pixels.size() - 8U,
                               16U | 0x80000000U); break;
      case 3: fixture.WriteWord(fixture.pixels.size() - 4U, 16); break;
      case 4: fixture.raw.timeStamp = 6; break;
      case 5: fixture.raw.timeStamp = 0xffffffffULL; break;
      case 6: fixture.raw.timeStamp =
                  std::numeric_limits<uint64_t>::max() / 20000U + 1U; break;
      case 7: ++fixture.raw.picture_number; break;
      case 8: ++fixture.raw.sess_num; break;
      case 9: ++fixture.raw.picture_meta_payload; break;
      case 10: fixture.raw.flags = VDEC_FLAG_TOPFIELD; break;
      case 11: fixture.raw.flags |= VDEC_FLAG_EOS; break;
      case 12: fixture.raw.width = 18; break;
      case 13: fixture.raw.height = 28; break;
      case 14: fixture.WriteWord(fixture.PayloadBytes(), 72); break;
      case 15: fixture.output.PoutFlags |= BC_POUT_FLAGS_ENCRYPTED; break;
      case 16: fixture.raw.flags |= 0x800U; break;
    }
    fixture.WritePib();
    Require(fixture.Process() == VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.decoder.replay.failed() &&
                !fixture.decoder.h264_first_field.valid &&
                !fixture.public_frame->ready && fixture.private_frame->failed,
            "malformed whole payload never becomes a picture or a held field");
  }
  for (unsigned int fault = 0; fault < 4; ++fault) {
    H264WeaveFixture fixture;
    switch (fault) {
      case 0: ++fixture.private_frame->trace_generation; break;
      case 1: fixture.private_frame->decode_identity = 0; break;
      case 2: fixture.decoder.pending.clear(); break;
      case 3: ++fixture.private_frame->expected_timestamp; break;
    }
    Require(fixture.Process() == VA_STATUS_ERROR_DECODING_ERROR &&
                !fixture.public_frame->ready && fixture.decoder.replay.failed(),
            "whole Weave cannot bypass generation or pending decode ownership");
  }
  {
    H264WeaveFixture fixture;
    BC_DTS_PROC_OUT half = fixture.Output(false, false, &fixture.top);
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, half) ==
                    VA_STATUS_SUCCESS && fixture.decoder.h264_first_field.valid &&
                !fixture.public_frame->ready &&
                fixture.decoder.replay.outstanding() == 1,
            "a single half field remains pending and cannot become whole Weave");
    Require(fixture.Process() == VA_STATUS_ERROR_DECODING_ERROR &&
                !fixture.decoder.h264_first_field.valid &&
                !fixture.public_frame->ready && fixture.decoder.replay.failed(),
            "whole Weave cannot silently replace an already held half field");
  }
  {
    // For tiny geometry the metadata tail can exceed the other half's pixel
    // size. Its line trailer must still identify a field, not a whole frame.
    H264WeaveFixture fixture;
    fixture.output.PicInfo.width = fixture.raw.width = 2;
    fixture.output.PicInfo.height = fixture.raw.height = 4;
    const size_t field_bytes = 2U * 2U * 2U;
    fixture.pixels.assign(field_bytes + 160U, 0);
    fixture.output.Ybuff = fixture.pixels.data();
    fixture.output.YBuffDoneSz = fixture.pixels.size() / 4U;
    fixture.WriteWord(field_bytes, fixture.output.PicInfo.picture_number);
    memcpy(fixture.pixels.data() + field_bytes + sizeof(uint32_t), &fixture.raw,
           offsetof(BC_PIC_INFO_BLOCK, other));
    fixture.WriteWord(fixture.pixels.size() - 8U, 2);
    fixture.WriteWord(fixture.pixels.size() - 4U, 2);
    Require(ClassifyH264InterlacedPayload(fixture.output) ==
                H264InterlacedPayload::Field,
            "large field metadata tail does not manufacture a whole picture");
  }
}

static void H264FieldPairsPreserveIdentityAndPixels() {
  for (bool direct : {false, true}) {
    for (bool bottom_first : {false, true}) {
      H264FieldFixture fixture(direct);
      const bool first_bottom = bottom_first;
      BC_DTS_PROC_OUT first = fixture.Output(
          first_bottom, bottom_first,
          first_bottom ? &fixture.bottom : &fixture.top);
      BC_DTS_PROC_OUT second = fixture.Output(
          !first_bottom, bottom_first,
          first_bottom ? &fixture.top : &fixture.bottom);
      Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, first) ==
                      VA_STATUS_SUCCESS &&
                  fixture.decoder.h264_first_field.valid &&
                  fixture.decoder.pending.count(1) == 1 &&
                  fixture.decoder.replay.outstanding() == 1 &&
                  !fixture.private_frame->ready &&
                  !fixture.public_frame->ready,
              "first H.264 field retains pending/replay ownership and is not READY");
      Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, second) ==
                      VA_STATUS_SUCCESS &&
                  !fixture.decoder.h264_first_field.valid &&
                  fixture.decoder.pending.empty() &&
                  fixture.decoder.replay.outstanding() == 0 &&
                  fixture.private_frame->ready &&
                  fixture.public_frame->ready &&
                  fixture.public_frame->frame_timestamp == 1,
              "complementary H.264 field completes one exact picture");
      fixture.CheckPixels(*fixture.public_frame);
      if (!direct) {
        Require(fixture.private_frame->storage == fixture.public_frame->storage,
                "ordinary private and public H.264 field pictures match");
        fixture.CheckPixels(*fixture.private_frame);
      }
    }
  }

  H264FieldFixture replay;
  Require(replay.decoder.replay.Seal(),
          "seal H.264 field replay fixture");
  BC_DTS_PROC_OUT top = replay.Output(false, false, &replay.top);
  BC_DTS_PROC_OUT bottom = replay.Output(true, false, &replay.bottom);
  Require(ProcessDecodedOutput(&replay.driver, &replay.decoder, top) ==
                  VA_STATUS_SUCCESS &&
              ProcessDecodedOutput(&replay.driver, &replay.decoder, bottom) ==
                  VA_STATUS_SUCCESS &&
              replay.decoder.replay.EndOfSequence() &&
              replay.decoder.replay.Append(2, false, {2}) &&
              replay.decoder.replay.Restarted(),
          "complete original field pair and restart its retained IDR prefix");
  SendNext(&replay.decoder.replay, 1);
  replay.decoder.decoded_frames.erase(1);
  const std::vector<uint8_t> pixels = replay.public_frame->storage;
  Require(ProcessDecodedOutput(&replay.driver, &replay.decoder, top) ==
                  VA_STATUS_SUCCESS &&
              replay.decoder.h264_first_field.valid &&
              replay.decoder.replay.IsOutstanding(1) &&
              replay.public_frame->storage == pixels,
          "first replayed duplicate field remains outstanding without mutating pixels");
  Require(ProcessDecodedOutput(&replay.driver, &replay.decoder, bottom) ==
                  VA_STATUS_SUCCESS &&
              !replay.decoder.h264_first_field.valid &&
              !replay.decoder.replay.IsOutstanding(1) &&
              replay.public_frame->storage == pixels,
          "complete replayed duplicate pair retires hardware readiness exactly once");
  Require(ProcessDecodedOutput(&replay.driver, &replay.decoder, top) ==
                  VA_STATUS_SUCCESS &&
              !replay.decoder.h264_first_field.valid,
          "unsolicited retired duplicate field cannot start a partial picture");
}

static std::shared_ptr<Surface> RebindH264PublicSurface(
    H264FieldFixture *fixture) {
  auto replacement = std::make_shared<Surface>();
  fixture->public_frame->expected_timestamp = 2;
  fixture->public_frame->decode_identity = 42;
  fixture->public_frame->decode_picture = replacement;
  return replacement;
}

static void ReplayedH264FieldsRetainMetadataWithoutPixels() {
  for (bool is_70012 : {false, true}) {
    for (bool bottom_first : {false, true}) {
      for (unsigned fault = 0; fault < 13; ++fault) {
        if (is_70012 && fault != 0) continue;
        DecodeIoMock io;
        MockDecodeScope scope(io);
        H264FieldFixture fixture(false, is_70012);
        auto first = fixture.Output(bottom_first, bottom_first,
            bottom_first ? &fixture.bottom : &fixture.top);
        auto second = fixture.Output(!bottom_first, bottom_first,
            bottom_first ? &fixture.top : &fixture.bottom);
        Require(fixture.decoder.replay.Seal() &&
                    ProcessDecodedOutput(&fixture.driver, &fixture.decoder, first) == VA_STATUS_SUCCESS &&
                    ProcessDecodedOutput(&fixture.driver, &fixture.decoder, second) == VA_STATUS_SUCCESS &&
                    fixture.decoder.replay.EndOfSequence() &&
                    fixture.decoder.replay.Append(2, false, {2}) &&
                    fixture.decoder.replay.Restarted(),
                "complete the original pair before replaying its real reference input");
        SendNext(&fixture.decoder.replay, 1);
        fixture.decoder.decoded_frames.erase(1);
        const auto public_pixels = fixture.public_frame->storage;
        const auto private_pixels = fixture.private_frame->storage;
        const auto unchanged = [&] {
          return fixture.public_frame->storage == public_pixels &&
              fixture.private_frame->storage == private_pixels &&
              fixture.public_frame->ready && !fixture.public_frame->failed &&
              fixture.private_frame->ready && !fixture.private_frame->failed &&
              fixture.public_frame->expected_timestamp == 1 &&
              fixture.public_frame->frame_timestamp == 1 &&
              fixture.public_frame->decode_identity == 41 &&
              fixture.private_frame->frame_timestamp == 1 &&
              fixture.decoder.pending.empty() && fixture.decoder.decoded_frames.empty();
        };
        // Deliberately different duplicate pixels must never reach either
        // retained picture, and their first field need not be retained at all.
        std::fill(fixture.top.begin(), fixture.top.end(), 0);
        std::fill(fixture.bottom.begin(), fixture.bottom.end(), 0);
        io.override_output = true;
        io.overridden_output = first;
        io.events.reserve(8);
        fixture.decoder.device = io.handle();
        if (fault == 1) io.output_release_status = BC_STS_ERROR;
        if (fault == 12) allocation_fault::fail_next = true;
        bool activity = false;
        const VAStatus first_status = ReceiveOne(&fixture.driver, &fixture.decoder, 0, &activity);
        const bool no_allocation = allocation_fault::fail_next;
        allocation_fault::fail_next = false;
        Require(activity && std::count(io.events.begin(), io.events.end(), "release") == 1 &&
                    unchanged() && !io.invalid,
                "each acquired duplicate field releases once without changing retained public/private pictures");
        if (fault == 1) {
          Require(first_status == VA_STATUS_ERROR_DECODING_ERROR &&
                      fixture.decoder.replay.failed() && !fixture.decoder.h264_first_field.valid,
                  "failed first-field release cannot leave replay metadata armed");
          continue;
        }
        const auto &held = fixture.decoder.h264_first_field;
        Require(first_status == VA_STATUS_SUCCESS && held.valid && held.replay_duplicate &&
                    held.pixels.empty() && !held.picture && held.decode_identity == 0 &&
                    held.timestamp == 1 && held.generation == fixture.decoder.generation &&
                    held.bottom == bottom_first && held.bottom_first == bottom_first &&
                    held.is_70012 == is_70012 && held.width == fixture.decoder.width &&
                    held.height == 32 && held.source_pitch == (is_70012 ? 720U : 16U) * 2U &&
                    held.picture_number == first.PicInfo.picture_number &&
                    held.sess_num == first.PicInfo.sess_num &&
                    held.picture_meta_payload == first.PicInfo.picture_meta_payload &&
                    fixture.decoder.replay.IsOutstanding(1) &&
                    (fault != 12 || no_allocation),
                "replayed first field retains exact metadata but no pixel allocation or picture owner");
        switch (fault) {
          case 2: second.PicInfo.flags = first.PicInfo.flags; break;
          case 3: second.PicInfo.timeStamp = 2; break;
          case 4: ++fixture.decoder.generation; break;
          case 5: ++second.PicInfo.picture_number; break;
          case 6: ++second.PicInfo.sess_num; break;
          case 7: ++second.PicInfo.picture_meta_payload; break;
          case 8: second.PicInfo.width = 14; break;
          case 9: --second.YBuffDoneSz; break;
          case 10: second.PicInfo.flags ^= VDEC_FLAG_BOTTOM_FIRST; break;
          case 11: io.output_release_status = BC_STS_ERROR; break;
          default: break;
        }
        if (is_70012) second.PicInfo.timeStamp = 0;
        io.overridden_output = second;
        const VAStatus second_status = ReceiveOne(&fixture.driver, &fixture.decoder, 0, &activity);
        const bool valid = fault == 0 || fault == 12;
        Require(second_status == (valid ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_DECODING_ERROR) &&
                    fixture.decoder.replay.failed() == !valid &&
                    !fixture.decoder.h264_first_field.valid &&
                    fixture.decoder.h264_first_field.pixels.empty() &&
                    (!valid || !fixture.decoder.replay.IsOutstanding(1)) &&
                    std::count(io.events.begin(), io.events.end(), "release") == 2 &&
                    unchanged() && !io.invalid,
                "only the matching released duplicate pair retires replay; malformed/release failures stay terminal");
      }
    }
  }
}

static void H264FieldPairsRespectStaleAndRetainedOwnership() {
  {
    H264FieldFixture fixture;
    mock_sync_fd = memfd_create("crystalhd-field-stale-test", MFD_CLOEXEC);
    Require(mock_sync_fd >= 0, "allocate stale field-pair ownership fd");
    fixture.private_frame->object_fds.push_back(mock_sync_fd);
    std::weak_ptr<Surface> picture = fixture.private_frame;
    const std::vector<uint8_t> public_pixels = fixture.public_frame->storage;
    BC_DTS_PROC_OUT first = fixture.Output(false, false, &fixture.top);
    BC_DTS_PROC_OUT second = fixture.Output(true, false, &fixture.bottom);
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, first) ==
                    VA_STATUS_SUCCESS &&
                fixture.decoder.h264_first_field.valid,
            "retain first field before its public surface is rebound");
    const auto replacement = RebindH264PublicSurface(&fixture);
    fixture.private_frame.reset();
    mock_failed_sync_flags = UINT64_MAX;
    mock_sync_calls.clear();
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, second) ==
                    VA_STATUS_SUCCESS &&
                !fixture.decoder.h264_first_field.valid &&
                fixture.decoder.pending.empty() &&
                fixture.decoder.replay.outstanding() == 0 &&
                fixture.decoder.decoded_frames.empty() && picture.expired() &&
                mock_sync_calls.empty() &&
                fixture.public_frame->storage == public_pixels &&
                !fixture.public_frame->ready && !fixture.public_frame->failed &&
                fixture.public_frame->decode_picture.lock() == replacement,
            "a dropped field pair retires transport without CPU materialization");
    mock_sync_fd = -1;
  }

  for (bool queued_vpp : {false, true}) {
    H264FieldFixture fixture;
    mock_sync_fd = memfd_create(queued_vpp ? "crystalhd-field-vpp-test"
                                           : "crystalhd-field-capture-test",
                                MFD_CLOEXEC);
    Require(mock_sync_fd >= 0, "allocate retained field-pair ownership fd");
    fixture.private_frame->object_fds.push_back(mock_sync_fd);
    BC_DTS_PROC_OUT first = fixture.Output(false, false, &fixture.top);
    BC_DTS_PROC_OUT second = fixture.Output(true, false, &fixture.bottom);
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, first) ==
                    VA_STATUS_SUCCESS &&
                fixture.decoder.h264_first_field.valid,
            "retain first field before capture or VPP owns its picture");
    auto retained = fixture.private_frame;
    if (queued_vpp)
      retained->vpp_readers = 1;
    const auto replacement = RebindH264PublicSurface(&fixture);
    fixture.private_frame.reset();
    mock_failed_sync_flags = UINT64_MAX;
    mock_sync_calls.clear();
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, second) ==
                    VA_STATUS_SUCCESS &&
                retained->ready && retained->frame_timestamp == 1 &&
                mock_sync_calls ==
                    std::vector<uint64_t>{DMA_BUF_SYNC_START |
                                              DMA_BUF_SYNC_WRITE,
                                          DMA_BUF_SYNC_END |
                                              DMA_BUF_SYNC_WRITE} &&
                fixture.decoder.pending.empty() &&
                fixture.decoder.replay.outstanding() == 0 &&
                fixture.decoder.decoded_frames.empty() &&
                !fixture.public_frame->ready && !fixture.public_frame->failed &&
                fixture.public_frame->decode_picture.lock() == replacement,
            "capture and queued VPP ownership preserve stale field pixels");
    fixture.CheckPixels(*retained);
    mock_sync_fd = -1;
  }
}

static void H264Bcm70012SecondFieldTokenRecoveryIsNarrow() {
  for (bool direct : {false, true}) {
    for (bool bottom_first : {false, true}) {
      H264FieldFixture fixture(direct, true);
      const bool first_bottom = bottom_first;
      BC_DTS_PROC_OUT first = fixture.Output(
          first_bottom, bottom_first,
          first_bottom ? &fixture.bottom : &fixture.top);
      BC_DTS_PROC_OUT second = fixture.Output(
          !first_bottom, bottom_first,
          first_bottom ? &fixture.top : &fixture.bottom);
      second.PicInfo.timeStamp = 0;
      Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, first) ==
                      VA_STATUS_SUCCESS &&
                  fixture.decoder.h264_first_field.valid &&
                  ProcessDecodedOutput(&fixture.driver, &fixture.decoder,
                                       second) == VA_STATUS_SUCCESS &&
                  !fixture.decoder.h264_first_field.valid &&
                  fixture.decoder.pending.empty() &&
                  fixture.decoder.replay.outstanding() == 0 &&
                  fixture.public_frame->ready &&
                  fixture.public_frame->frame_timestamp == 1,
              "BCM70012 recovers only its complementary second field token");
      fixture.CheckPixels(*fixture.public_frame);
    }
  }

  {
    H264FieldFixture fixture(false, true);
    BC_DTS_PROC_OUT unbound = fixture.Output(false, false, &fixture.top);
    unbound.PicInfo.timeStamp = 0;
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, unbound) ==
                    VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.decoder.replay.failed() &&
                !fixture.decoder.h264_first_field.valid,
            "a zero-token BCM70012 field cannot start an unbound pair");
  }

  const auto reject_zero_second = [](unsigned int fault) {
    H264FieldFixture fixture(false, true);
    BC_DTS_PROC_OUT first = fixture.Output(false, false, &fixture.top);
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, first) ==
                    VA_STATUS_SUCCESS &&
                fixture.decoder.h264_first_field.valid,
            "retain BCM70012 first field before zero-token mismatch");
    BC_DTS_PROC_OUT second = fixture.Output(true, false, &fixture.bottom);
    second.PicInfo.timeStamp = 0;
    switch (fault) {
      case 0: second.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC |
                                      VDEC_FLAG_TOPFIELD; break;
      case 1: second.PicInfo.width = 14; break;
      case 2: ++fixture.decoder.generation; break;
      case 3: second.PicInfo.flags |= VDEC_FLAG_BOTTOM_FIRST; break;
      case 4: fixture.private_frame->decode_identity = 42; break;
      case 5: ++second.PicInfo.picture_number; break;
      case 6: ++second.PicInfo.sess_num; break;
      case 7: ++second.PicInfo.picture_meta_payload; break;
    }
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, second) ==
                    VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.decoder.replay.failed() &&
                !fixture.decoder.h264_first_field.valid &&
                !fixture.public_frame->ready,
            "BCM70012 zero token cannot bypass pair identity or geometry");
  };
  for (unsigned int fault = 0; fault < 8; ++fault)
    reject_zero_second(fault);

  {
    H264FieldFixture fixture;
    BC_DTS_PROC_OUT first = fixture.Output(false, false, &fixture.top);
    BC_DTS_PROC_OUT second = fixture.Output(true, false, &fixture.bottom);
    second.PicInfo.timeStamp = 0;
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, first) ==
                    VA_STATUS_SUCCESS &&
                ProcessDecodedOutput(&fixture.driver, &fixture.decoder,
                                     second) == VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.decoder.replay.failed(),
            "BCM70015 never inherits the BCM70012 metadata workaround");
  }
}

static void H264FieldPairRejectsPartialOrMismatchedOutput() {
  const auto reject_second = [](unsigned int fault) {
    H264FieldFixture fixture;
    BC_DTS_PROC_OUT first = fixture.Output(false, false, &fixture.top);
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, first) ==
                    VA_STATUS_SUCCESS &&
                fixture.decoder.h264_first_field.valid,
            "retain first field before mismatch injection");
    BC_DTS_PROC_OUT second = fixture.Output(true, false, &fixture.bottom);
    switch (fault) {
      case 0: second.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC |
                                     VDEC_FLAG_TOPFIELD; break;
      case 1: second.PicInfo.timeStamp = 2; break;
      case 2: fixture.private_frame->decode_identity = 42; break;
      case 3: ++fixture.decoder.generation; break;
      case 4: second.PicInfo.width = 14; break;
      case 5: second.PicInfo.flags |= VDEC_FLAG_BOTTOM_FIRST; break;
      case 6: --second.YBuffDoneSz; break;
      case 7: ++second.PicInfo.picture_number; break;
      case 8: ++second.PicInfo.sess_num; break;
      case 9: ++second.PicInfo.picture_meta_payload; break;
    }
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, second) ==
                    VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.decoder.replay.failed() &&
                !fixture.decoder.h264_first_field.valid &&
                fixture.private_frame->failed &&
                (fault == 2 ? !fixture.public_frame->failed
                            : fixture.public_frame->failed) &&
                !fixture.public_frame->ready,
            "mismatched or short second field fails closed without poisoning a rebound public identity");
  };
  for (unsigned int fault = 0; fault < 10; ++fault)
    reject_second(fault);

  H264FieldFixture short_first;
  BC_DTS_PROC_OUT first = short_first.Output(false, false, &short_first.top);
  --first.YBuffDoneSz;
  Require(ProcessDecodedOutput(&short_first.driver, &short_first.decoder,
                               first) == VA_STATUS_ERROR_DECODING_ERROR &&
              short_first.decoder.replay.failed() &&
              !short_first.decoder.h264_first_field.valid &&
              short_first.public_frame->failed,
          "short first field cannot create partial decode state");
}

static void H264FieldPartialStateClearsAtTeardownAndEos() {
  {
    H264FieldFixture fixture;
    BC_DTS_PROC_OUT first = fixture.Output(false, false, &fixture.top);
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, first) ==
                    VA_STATUS_SUCCESS &&
                fixture.decoder.h264_first_field.valid &&
                fixture.decoder.CloseHardware() == BC_STS_SUCCESS &&
                !fixture.decoder.h264_first_field.valid &&
                fixture.decoder.pending.count(1) == 1 &&
                fixture.decoder.replay.outstanding() == 1,
            "hardware teardown discards copied field but preserves replay ownership");
    Require(fixture.decoder.Close() == BC_STS_SUCCESS &&
                !fixture.decoder.h264_first_field.valid &&
                fixture.decoder.pending.empty() &&
                fixture.decoder.replay.outstanding() == 0,
            "full context teardown clears partial field and transport state");
  }

  {
    DecodeIoMock io;
    MockDecodeScope scope(io);
    H264FieldFixture fixture;
    BC_DTS_PROC_OUT first = fixture.Output(false, false, &fixture.top);
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, first) ==
                    VA_STATUS_SUCCESS &&
                fixture.decoder.h264_first_field.valid,
            "retain first field before status failure");
    fixture.decoder.device = io.handle();
    fixture.decoder.decoder_open = fixture.decoder.decoder_started = true;
    io.fail_poll = true;
    Require(ReceiveAvailable(&fixture.driver, &fixture.decoder) ==
                    VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.decoder.replay.failed() &&
                !fixture.decoder.h264_first_field.valid &&
                fixture.public_frame->failed && !io.invalid,
            "hardware status failure clears an incomplete H.264 field pair");
  }

  {
    DecodeIoMock io;
    MockDecodeScope scope(io);
    H264FieldFixture fixture;
    fixture.decoder.device = io.handle();
    fixture.decoder.decoder_open = fixture.decoder.decoder_started = true;
    io.output_width = 16;
    io.output_height = 32;
    io.outputs.push_back(1);
    io.picture_flags.push_back(VDEC_FLAG_INTERLACED_SRC |
                               VDEC_FLAG_TOPFIELD);
    io.outputs.push_back(0);
    Require(ReceiveAvailable(&fixture.driver, &fixture.decoder) ==
                    VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.decoder.replay.failed() &&
                !fixture.decoder.h264_first_field.valid &&
                fixture.public_frame->failed &&
                std::count(io.events.begin(), io.events.end(), "release") == 2 &&
                !io.invalid,
            "EOS after one field releases hardware buffers and fails incomplete picture");
  }
}

static void H264FirstFieldAllocationFailureReleasesOutput() {
  DecodeIoMock io;
  H264FieldFixture fixture;
  io.output_width = 16;
  io.output_height = 32;
  io.outputs.push_back(1);
  io.picture_flags.push_back(VDEC_FLAG_INTERLACED_SRC |
                             VDEC_FLAG_TOPFIELD);
  io.pixels.resize(16 * 16 * 2);
  io.events.reserve(4);
  MockDecodeScope scope(io);
  fixture.decoder.device = io.handle();
  bool activity = false;
  allocation_fault::fail_next = true;
  const VAStatus status =
      ReceiveOne(&fixture.driver, &fixture.decoder, 0, &activity);
  Require(status == VA_STATUS_ERROR_ALLOCATION_FAILED && activity &&
              !allocation_fault::fail_next &&
              !fixture.decoder.h264_first_field.valid &&
              fixture.decoder.h264_first_field.pixels.empty() &&
              fixture.decoder.replay.failed() &&
              fixture.private_frame->failed && fixture.public_frame->failed &&
              std::count(io.events.begin(), io.events.end(), "picture") == 1 &&
              std::count(io.events.begin(), io.events.end(), "release") == 1 &&
              !io.invalid,
          "first-field allocation failure fails closed and releases its hardware output exactly once");
}

static void OutputAllocationFailuresReleaseHardwareExactlyOnce() {
  {
    DecodeIoMock io;
    OutputFixture fixture;
    fixture.Direct();
    // Keep the public surface as the exact current target, but force the
    // private picture to promote from a distinct direct backing.
    fixture.private_frame->direct_backing = std::make_shared<Surface>();
    io.outputs.push_back(1);
    io.pixels.resize(16 * 16 * 2);
    io.events.reserve(4);
    MockDecodeScope scope(io);
    fixture.decoder.device = io.handle();
    bool activity = false;
    allocation_fault::fail_next = true;
    const VAStatus status =
        ReceiveOne(&fixture.driver, &fixture.decoder, 0, &activity);
    Require(status == VA_STATUS_ERROR_ALLOCATION_FAILED && activity &&
                !allocation_fault::fail_next && fixture.decoder.replay.failed() &&
                fixture.private_frame->failed && !fixture.private_frame->ready &&
                fixture.public_frame->failed && !fixture.public_frame->ready &&
                std::count(io.events.begin(), io.events.end(), "picture") == 1 &&
                std::count(io.events.begin(), io.events.end(), "release") == 1 &&
                !io.invalid,
            "post-dequeue private promotion failure poisons both exact identities and releases once");
  }

  {
    DecodeIoMock io;
    OutputFixture fixture;
    fixture.decoder.experimental_ready_remap = true;
    fixture.private_frame->trace_submission_ordinal = 1;
    fixture.private_frame->trace_context = 7;
    fixture.private_frame->trace_generation = fixture.decoder.generation;
    const auto dummy = std::make_shared<Surface>();
    bool boundary = false;
    for (size_t attempt = 0; attempt < 4096; ++attempt) {
      allocation_fault::fail_next = true;
      try {
        fixture.decoder.experimental_ready.push_back(dummy);
      } catch (const std::bad_alloc &) {
        boundary = true;
        break;
      }
    }
    Require(boundary && !allocation_fault::fail_next,
            "locate the next real deque allocation boundary");
    const size_t queued_before = fixture.decoder.experimental_ready.size();
    io.outputs.push_back(1);
    io.pixels.resize(16 * 16 * 2);
    io.events.reserve(4);
    MockDecodeScope scope(io);
    fixture.decoder.device = io.handle();
    bool activity = false;
    allocation_fault::fail_next = true;
    const VAStatus status =
        ReceiveOne(&fixture.driver, &fixture.decoder, 0, &activity);
    Require(status == VA_STATUS_ERROR_ALLOCATION_FAILED && activity &&
                !allocation_fault::fail_next && fixture.decoder.replay.failed() &&
                fixture.decoder.experimental_ready.size() == queued_before &&
                !fixture.private_frame->experimental_ready_queued &&
                fixture.private_frame->experimental_ready_at_ns == 0 &&
                fixture.private_frame->failed && !fixture.private_frame->ready &&
                fixture.public_frame->failed && !fixture.public_frame->ready &&
                std::count(io.events.begin(), io.events.end(), "picture") == 1 &&
                std::count(io.events.begin(), io.events.end(), "release") == 1 &&
                !io.invalid,
            "READY queue allocation failure leaves no ghost entry and releases once");
  }
}

static void DeviceCloseRetriesNonConsumingBusy() {
  DecodeIoMock io;
  MockDecodeScope scope(io);
  DecodeContext decoder;
  decoder.device = io.handle();
  const auto poll = std::make_shared<OutputPollLease>(-1);
  decoder.output_poll = poll;
  decoder.armed_output_poll = poll;
  io.events.reserve(8);
  io.device_close_busy = 1;
  Require(decoder.CloseHardware(0) == BC_STS_BUSY &&
              decoder.device == io.handle() && decoder.output_poll == poll &&
              !decoder.armed_output_poll.expired() &&
              !decoder.decoder_open && !decoder.decoder_started &&
              io.device_close_calls == 1 &&
              std::count(io.events.begin(), io.events.end(), "stop") == 0 &&
              std::count(io.events.begin(), io.events.end(),
                         "close-decoder") == 0 &&
              std::count(io.events.begin(), io.events.end(),
                         "close-device") == 1 &&
              !io.invalid,
          "non-consuming device-close BUSY retains the exact handle and poll leases");
  Require(decoder.CloseHardware(0) == BC_STS_SUCCESS &&
              decoder.device == nullptr && !decoder.output_poll &&
              decoder.armed_output_poll.expired() &&
              io.device_close_calls == 2 &&
              std::count(io.events.begin(), io.events.end(), "stop") == 0 &&
              std::count(io.events.begin(), io.events.end(),
                         "close-decoder") == 0 &&
              std::count(io.events.begin(), io.events.end(),
                         "close-device") == 2 &&
              !io.invalid,
          "retry consumes the retained handle without repeating decoder teardown");

  DecodeContext lifecycle;
  lifecycle.device = io.handle();
  lifecycle.decoder_open = true;
  lifecycle.decoder_started = true;
  io.stop_status = BC_STS_BUSY;
  const unsigned int calls = io.device_close_calls;
  io.device_close_busy = 1;
  Require(lifecycle.CloseHardware(0) == BC_STS_BUSY &&
              lifecycle.device == io.handle() && lifecycle.decoder_started &&
              lifecycle.decoder_open && lifecycle.hardware_close_pending &&
              io.device_close_calls == calls + 1,
          "Stop BUSY falls through to terminal DeviceClose and preserves state only when it too is BUSY");
  io.stop_status = BC_STS_SUCCESS;
  io.decoder_close_status = BC_STS_BUSY;
  io.device_close_busy = 1;
  Require(lifecycle.CloseHardware(0) == BC_STS_BUSY &&
              lifecycle.device == io.handle() && !lifecycle.decoder_started &&
              lifecycle.decoder_open && lifecycle.hardware_close_pending &&
              io.device_close_calls == calls + 2,
          "decoder-close BUSY still reaches terminal DeviceClose without losing retry state");
  io.decoder_close_status = BC_STS_SUCCESS;
  Require(lifecycle.CloseHardware(0) == BC_STS_SUCCESS &&
              lifecycle.device == nullptr && !lifecycle.decoder_started &&
              !lifecycle.decoder_open && !lifecycle.hardware_close_pending &&
              io.device_close_calls == calls + 3,
          "lifecycle retries finish before consuming the exact device handle");

  DecodeContext terminal;
  terminal.device = io.handle();
  terminal.decoder_open = true;
  terminal.decoder_started = true;
  io.stop_status = BC_STS_BUSY;
  Require(terminal.CloseHardware(0) == BC_STS_SUCCESS &&
              terminal.device == nullptr && !terminal.decoder_open &&
              !terminal.decoder_started,
          "terminal DeviceClose can consume a recovery-blocked lifecycle without waiting for RESUME");

  DecodeContext consumed;
  consumed.device = io.handle();
  consumed.decoder_open = true;
  consumed.decoder_started = true;
  io.stop_status = BC_STS_ERROR;
  io.decoder_close_status = BC_STS_ERROR;
  io.device_close_busy = 1;
  Require(consumed.CloseHardware(0) == BC_STS_BUSY &&
              consumed.device == io.handle() &&
              consumed.retained_close_status == BC_STS_ERROR,
          "a retained DeviceClose cannot erase an earlier terminal lifecycle error");
  io.stop_status = BC_STS_SUCCESS;
  io.decoder_close_status = BC_STS_SUCCESS;
  Require(consumed.CloseHardware(0) == BC_STS_ERROR &&
              consumed.device == nullptr && !consumed.decoder_open &&
              !consumed.decoder_started &&
              consumed.retained_close_status == BC_STS_SUCCESS,
          "terminal lifecycle errors survive BUSY retries until DeviceClose consumes ownership");

  DecodeContext terminal_error;
  terminal_error.device = io.handle();
  const auto terminal_poll = std::make_shared<OutputPollLease>(-1);
  terminal_error.output_poll = terminal_poll;
  terminal_error.armed_output_poll = terminal_poll;
  io.device_close_status = BC_STS_IO_ERROR;
  Require(terminal_error.CloseHardware(0) == BC_STS_IO_ERROR &&
              terminal_error.device == nullptr &&
              !terminal_error.hardware_close_pending &&
              !terminal_error.output_poll &&
              terminal_error.armed_output_poll.expired() && !io.invalid,
          "same-process terminal DeviceClose error consumes the handle and poll ownership");
  io.device_close_status = BC_STS_SUCCESS;
}

static void RetainedHandleClosesBeforeDecoderReopen() {
  DecodeIoMock io;
  io.events.reserve(8);
  io.allow_device_open = true;
  io.device_close_busy = 1;
  MockDecodeScope scope(io);
  DecodeContext decoder;
  decoder.width = decoder.height = 16;
  decoder.device = io.handle();
  Require(OpenDecoder(&decoder, 16, 16, nullptr, 0) ==
                  VA_STATUS_ERROR_HW_BUSY &&
              decoder.device == io.handle() && io.open_attempts == 0 &&
              io.device_close_calls == 1 && !io.invalid,
          "a retained close blocks DtsDeviceOpen and cannot overwrite its live handle");
  Require(OpenDecoder(&decoder, 16, 16, nullptr, 0) == VA_STATUS_SUCCESS &&
              decoder.device == io.handle() && decoder.decoder_open &&
              decoder.decoder_started && io.open_attempts == 1 &&
              io.device_close_calls == 2 && !io.invalid,
          "only a consumed retained handle permits one clean decoder reopen");
  Require(decoder.CloseHardware(0) == BC_STS_SUCCESS &&
              decoder.device == nullptr && !io.invalid,
          "reopened decoder closes without leaving test ownership live");
}

static void H264ReplayReopenEligibilityAndFailureOwnership() {
  const auto prepare = [](DecodeContext &decode, DecodeIoMock &io) {
    decode.device = io.handle();
    decode.decoder_open = decode.decoder_started = true;
    decode.profile = VAProfileH264Main;
    decode.width = decode.height = 16;
    decode.generation = 7;
    decode.next_timestamp = 3 * kTimestampStep;
    io.library_capabilities = DTS_LIB_CAP_FLEA_H264_DRAINED_REOPEN;
    Require(decode.replay.Append(kTimestampStep, true, {1}) &&
                decode.replay.InputSent() && decode.replay.Seal() &&
                decode.replay.Observe(kTimestampStep) == CrystalHDDecodeReplay::Output::New &&
                decode.replay.EndOfSequence() &&
                decode.replay.Append(2 * kTimestampStep, false, {2}),
            "prepare an actually ended transport with one queued continuation");
  };
  for (unsigned mode = 0; mode < 16; ++mode) {
    DecodeIoMock io;
    MockDecodeScope scope(io);
    DecodeContext decode;
    prepare(decode, io);
    switch (mode) {
      case 1: decode.is_70012 = true; break;
      case 2: decode.profile = VAProfileMPEG2Main; break;
      case 3: decode.live_h264_fast_path = true; break;
      case 4: decode.closing = true; break;
      case 5: decode.retired = true; break;
      case 6: decode.destroy_prepared = true; break;
      case 7: decode.hardware_close_pending = true; break;
      case 8: decode.retained_close_status = BC_STS_IO_ERROR; break;
      case 9: decode.h264_first_field.valid = true; break;
      case 10: decode.partial_input_timestamp = 2 * kTimestampStep; break;
      case 11: decode.partial_input_offset = 1; break;
      case 12: decode.prefed_timestamp = 3 * kTimestampStep; break;
      case 13: decode.pending_prefeed_timestamp = 3 * kTimestampStep; break;
      case 14:
      case 15:
        decode.replay = CrystalHDDecodeReplay();
        Require(decode.replay.Append(kTimestampStep, true, {1}) &&
                    decode.replay.InputSent(), "prepare an unended transport");
        if (mode == 14)
          Require(decode.replay.Observe(kTimestampStep) == CrystalHDDecodeReplay::Output::New,
                  "completed input is not proof of actual EOS");
        break;
      default: break;
    }
    const int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    Require(fd >= 0, "allocate old output-poll lease");
    auto held_poll = std::make_shared<OutputPollLease>(fd);
    decode.output_poll = held_poll;
    decode.armed_output_poll = held_poll;
    const BC_STATUS expected = mode == 8 ? BC_STS_IO_ERROR : BC_STS_SUCCESS;
    Require(decode.CloseHardwareForH264Replay(0) == expected &&
                decode.device == (mode == 0 ? io.handle() : nullptr) &&
                decode.device_preopened == (mode == 0) &&
                io.device_close_calls == (mode == 0 ? 0U : 1U) &&
                io.library_capability_queries == (mode == 0 ? 1U : 0U) &&
                !decode.decoder_open && !decode.decoder_started &&
                !decode.output_poll && decode.armed_output_poll.expired() &&
                decode.prefed_timestamp == 0 && decode.pending_prefeed_timestamp == 0 &&
                !decode.h264_first_field.valid && decode.partial_input_timestamp == 0 &&
                decode.partial_input_offset == 0 && decode.generation == 7 && !io.invalid,
            "only the exact drained replay state may retain a device; all gates retire normally");
    Require(fcntl(fd, F_GETFD) >= 0,
            "an unlocked poll owner keeps the old descriptor alive after current/armed clear");
    held_poll.reset();
    errno = 0;
    Require(fcntl(fd, F_GETFD) == -1 && errno == EBADF,
            "the last old output-poll owner alone closes its descriptor");
    if (mode == 0)
      Require(decode.CloseHardware(0) == BC_STS_SUCCESS && decode.device == nullptr &&
                  !decode.device_preopened && io.device_close_calls == 1 &&
                  io.library_capability_queries == 1,
              "ordinary hardware close remains terminal even for a qualified retained device");
  }
  for (unsigned fault = 0; fault < 5; ++fault) {
    DecodeIoMock io;
    MockDecodeScope scope(io);
    DecodeContext decode;
    prepare(decode, io);
    io.stop_status = fault == 0 || fault == 4 ? BC_STS_ERROR :
                     fault == 1 ? BC_STS_BUSY : BC_STS_SUCCESS;
    io.decoder_close_status = fault == 2 ? BC_STS_ERROR :
                              fault == 3 ? BC_STS_BUSY : BC_STS_SUCCESS;
    io.device_close_busy = fault == 4 ? 1 : 0;
    const auto old_poll = std::make_shared<OutputPollLease>(-1);
    decode.output_poll = old_poll;
    decode.armed_output_poll = old_poll;
    const BC_STATUS expected = fault == 4 ? BC_STS_BUSY :
        fault == 0 || fault == 2 ? BC_STS_ERROR : BC_STS_SUCCESS;
    Require(decode.CloseHardwareForH264Replay(0) == expected &&
                !decode.device_preopened && io.device_close_calls == 1 &&
                io.library_capability_queries == 1 &&
                decode.device == (fault == 4 ? io.handle() : nullptr) &&
                decode.hardware_close_pending == (fault == 4) &&
                decode.output_poll == (fault == 4 ? old_poll : nullptr) && !io.invalid,
            "failed or BUSY decoder retirement can only take the existing terminal-device path");
    if (fault == 4) {
      io.stop_status = io.decoder_close_status = BC_STS_SUCCESS;
      Require(decode.CloseHardware(0) == BC_STS_ERROR && decode.device == nullptr &&
                  !decode.device_preopened && !decode.output_poll &&
                  decode.armed_output_poll.expired() && io.device_close_calls == 2,
              "non-consuming DeviceClose preserves the first error and poll owner until terminal retry");
    }
  }
  for (bool late_failure : {false, true}) {
    DecodeIoMock io;
    MockDecodeScope scope(io);
    DecodeContext decode;
    prepare(decode, io);
    Require(decode.CloseHardwareForH264Replay(0) == BC_STS_SUCCESS &&
                decode.device_preopened && io.device_close_calls == 0,
            "prepare a retained device before fresh-channel setup failure");
    io.fail_set_input_format = !late_failure;
    io.capture_start_status = late_failure ? BC_STS_ERROR : BC_STS_SUCCESS;
    Require(OpenDecoder(&decode) == VA_STATUS_ERROR_OPERATION_FAILED &&
                decode.device == nullptr && !decode.device_preopened &&
                !decode.decoder_open && !decode.decoder_started &&
                !decode.output_poll && decode.armed_output_poll.expired() &&
                io.open_attempts == 0 && io.device_close_calls == 1 &&
                io.decoder_open_calls == (late_failure ? 1U : 0U) &&
                io.capture_start_calls == (late_failure ? 1U : 0U) && !io.invalid,
            "early and late fresh-channel setup failures consume the retained device exactly once");
  }
  std::puts("H264 replay restart: 16 eligibility, 5 retirement-failure and 2 setup-failure cases PASS");
}

static void ContextAdmissionRejectsBusyBeforePicture() {
  DecodeIoMock io;
  io.allow_device_open = true;
  io.exclusive_device_open = true;
  MockDecodeScope scope(io);
  Driver first{-1}, second{-1};
  VADriverContext context = {}, competing = {};
  context.pDriverData = &first;
  competing.pDriverData = &second;
  VAConfigID config = VA_INVALID_ID, vpp_config = VA_INVALID_ID;
  Require(CreateConfig(&context, VAProfileH264Main, VAEntrypointVLD,
                       nullptr, 0, &config) == VA_STATUS_SUCCESS &&
              CreateConfig(&context, VAProfileNone, VAEntrypointVideoProc,
                           nullptr, 0, &vpp_config) == VA_STATUS_SUCCESS &&
              io.open_attempts == 0,
          "configuration capability queries never reserve the decoder");
  second.configs[config] = {VAProfileH264Main, VAEntrypointVLD};
  VAContextID id = VA_INVALID_ID;
  Require(CreateContext(&context, config, 0, 16, 0, nullptr, 0, &id) ==
                  VA_STATUS_ERROR_INVALID_PARAMETER &&
              CreateContext(&context, config, 16, 16, 0, nullptr, 1, &id) ==
                  VA_STATUS_ERROR_INVALID_PARAMETER &&
              id == VA_INVALID_ID && first.contexts.empty() &&
              io.open_attempts == 0,
          "invalid contexts fail before admission without publishing an ID");
  allocation_fault::fail_next = true;
  Require(CreateContext(&context, config, 16, 16, 0, nullptr, 0, &id) ==
                  VA_STATUS_ERROR_ALLOCATION_FAILED &&
              id == VA_INVALID_ID && first.contexts.empty() &&
              io.open_attempts == 0,
          "allocation failure cannot acquire an unpublished session");
  Require(CreateContext(&context, config, 16, 16, 0, nullptr, 0, &id) ==
                  VA_STATUS_SUCCESS && io.open_attempts == 1,
          "context creation acquires a real playback handle before any picture");
  const auto decoder = first.contexts.at(id);
  Require(decoder->device == io.handle() && decoder->device_preopened &&
              !decoder->decoder_open && !decoder->decoder_started,
          "admission retains the exact handle without opening a codec");
  for (VADriverContextP contender : {&context, &competing}) {
    VAContextID rejected = VA_INVALID_ID;
    const unsigned before = io.open_attempts;
    Require(CreateContext(contender, config, 16, 16, 0, nullptr, 0,
                          &rejected) == VA_STATUS_ERROR_HW_BUSY &&
                rejected == VA_INVALID_ID && first.contexts.size() == 1 &&
                second.contexts.empty() && decoder->device_preopened,
            "both same-driver and independent clients fail before picture submission");
    Require(io.open_attempts == before + (contender == &context ? 0 : 1),
            "locked local ownership rejects directly; independent clients use real admission");
  }
  const unsigned attempts = io.open_attempts;
  VAContextID vpp = VA_INVALID_ID;
  Require(CreateContext(&context, vpp_config, 0, 0, 0, nullptr, 0, &vpp) ==
                  VA_STATUS_SUCCESS && !first.contexts.at(vpp)->device &&
              DestroyContext(&context, vpp) == VA_STATUS_SUCCESS &&
              io.open_attempts == attempts && io.device_close_calls == 0,
          "VPP-only contexts coexist without owning or releasing playback");
  Require(OpenDecoder(decoder.get(), 16, 16, nullptr, 0) == VA_STATUS_SUCCESS &&
              !decoder->device_preopened && io.open_attempts == attempts &&
              io.device_close_calls == 0,
          "first submission consumes its exact admitted handle without a release race");
  Require(decoder->CloseHardware(0) == BC_STS_SUCCESS &&
              decoder->device == nullptr && !decoder->device_preopened &&
              io.device_close_calls == 1 && !io.device_owned,
          "later replay keeps the historical full-device close workaround");
  io.device_owned = true;  // Another client wins the existing replay release window.
  Require(OpenDecoder(decoder.get(), 16, 16, nullptr, 0) == VA_STATUS_ERROR_HW_BUSY &&
              decoder->device == nullptr && !decoder->decoder_open &&
              io.device_close_calls == 1,
          "replay reacquisition loses cleanly without replacing or closing another owner");
  io.device_owned = false;
  Require(OpenDecoder(decoder.get(), 16, 16, nullptr, 0) == VA_STATUS_SUCCESS &&
              io.open_attempts == attempts + 2 &&
              decoder->device == io.handle() &&
              decoder->Reset(0) == BC_STS_SUCCESS &&
              decoder->device == nullptr && io.device_close_calls == 2 &&
              DestroyContext(&context, id) == VA_STATUS_SUCCESS &&
              first.contexts.empty() && !io.invalid,
          "clean reacquisition and existing full reset release exactly their own device");
  Require(CreateContext(&context, config, 16, 16, 0, nullptr, 0, &id) ==
                  VA_STATUS_SUCCESS,
          "a later context can acquire after the first retires");
  io.device_close_busy = 1;
  Require(DestroyContextWithTimeout(&context, id, 0) == VA_STATUS_ERROR_HW_BUSY &&
              first.contexts.count(id) == 1 && io.device_owned &&
              DestroyContextWithTimeout(&context, id, 0) == VA_STATUS_SUCCESS &&
              first.contexts.empty() && !io.device_owned && !io.invalid,
          "unused admitted contexts preserve non-consuming close ownership until retry");
}

static void ContextAdmissionFailuresPublishNothing() {
  for (const auto &failure : {
           std::pair<BC_STATUS, VAStatus>{BC_STS_BUSY, VA_STATUS_ERROR_HW_BUSY},
           {BC_STS_DEC_EXIST_OPEN, VA_STATUS_ERROR_HW_BUSY},
           {BC_STS_ERR_USAGE, VA_STATUS_ERROR_OPERATION_FAILED},
           {BC_STS_IO_ERROR, VA_STATUS_ERROR_OPERATION_FAILED}}) {
    DecodeIoMock io;
    io.device_open_status = failure.first;
    MockDecodeScope scope(io);
    Driver driver{-1};
    driver.configs[1] = {VAProfileH264Main, VAEntrypointVLD};
    VADriverContext context = {};
    context.pDriverData = &driver;
    VAContextID id = VA_INVALID_ID;
    Require(CreateContext(&context, 1, 16, 16, 0, nullptr, 0, &id) == failure.second &&
                id == VA_INVALID_ID && driver.contexts.empty() &&
                driver.next_context == 1 && !io.device_owned &&
                io.device_close_calls == 0 && io.open_attempts == 1,
            "failed real admission leaves no handle, context entry or published ID");
  }
  DecodeIoMock io;
  io.allow_device_open = true;
  io.fail_set_input_format = true;
  MockDecodeScope scope(io);
  Driver driver{-1};
  driver.configs[1] = {VAProfileH264Main, VAEntrypointVLD};
  VADriverContext context = {};
  context.pDriverData = &driver;
  VAContextID id = VA_INVALID_ID;
  Require(CreateContext(&context, 1, 16, 16, 0, nullptr, 0, &id) == VA_STATUS_SUCCESS,
          "codec setup failure starts from admitted context");
  const auto decoder = driver.contexts.at(id);
  Require(OpenDecoder(decoder.get(), 16, 16, nullptr, 0) == VA_STATUS_ERROR_OPERATION_FAILED &&
              decoder->device == nullptr && !decoder->device_preopened &&
              io.device_close_calls == 1 && !io.device_owned &&
              DestroyContext(&context, id) == VA_STATUS_SUCCESS &&
              io.device_close_calls == 1 && !io.invalid,
          "failed first codec setup consumes its admitted lease exactly once");
}

static void CardScalingSetupOrderAndFailures() {
  for (unsigned int fault = 0; fault < 4; ++fault) {
    DecodeIoMock io;
    io.allow_device_open = true;
    io.report_bcm70012 = fault == 1;
    io.scale_status = fault == 2 ? BC_STS_ERROR : BC_STS_SUCCESS;
    io.fail_set_input_format = fault == 3;
    MockDecodeScope scope(io);
    DecodeContext decoder;
    decoder.width = 1920;
    decoder.height = 1088;
    decoder.processing_geometry = {true, 1920, 1080, 1280, 720};
    const VAStatus status = OpenDecoder(&decoder);
    if (fault == 0) {
      Require(status == VA_STATUS_SUCCESS &&
                  io.setup_events == std::vector<std::string>{
                      "input-format", "scale", "open-decoder"} &&
                  io.input_format.width == 1920 &&
                  io.input_format.height == 1088 &&
                  io.scaling.sWidth == 1280 && io.scaling.sHeight == 0 &&
                  io.scaling.DNR == 0 &&
                  decoder.CaptureWidth() == 1280 &&
                  decoder.CaptureHeight() == 720 &&
                  decoder.StreamWidth() == 1920 &&
                  decoder.StreamHeight() == 1088,
              "card width is configured after coded input and before decoder open");
      Require(decoder.CloseHardware() == BC_STS_SUCCESS &&
                  decoder.processing_geometry.enabled &&
                  OpenDecoder(&decoder) == VA_STATUS_SUCCESS &&
                  io.scale_calls == 2 &&
                  io.setup_events == std::vector<std::string>{
                      "input-format", "scale", "open-decoder",
                      "input-format", "scale", "open-decoder"},
              "hardware-only reopen reapplies frozen scale before opening");
    } else {
      Require(status == VA_STATUS_ERROR_OPERATION_FAILED &&
                  decoder.device == nullptr && !decoder.decoder_open &&
                  !decoder.decoder_started && io.device_close_calls == 1 &&
                  io.decoder_open_calls == 0 &&
                  io.scale_calls == (fault == 2 ? 1U : 0U),
              "unsupported card or setup failure closes only its own device");
    }
    Require(!io.invalid, "scale setup uses only the admitted mock device");
  }
  DecodeIoMock io;
  io.allow_device_open = true;
  MockDecodeScope scope(io);
  DecodeContext ordinary;
  ordinary.width = 1920;
  ordinary.height = 1088;
  Require(OpenDecoder(&ordinary) == VA_STATUS_SUCCESS &&
              io.scale_calls == 0 &&
              io.setup_events == std::vector<std::string>{
                  "input-format", "open-decoder"},
          "ordinary VLD never enables the card scaler");
}

struct CardProcessingFixture {
  DecodeIoMock io;
  MockDecodeScope scope{io};
  Driver driver{-1};
  VADriverContext context = {};
  VAContextID id = VA_INVALID_ID;
  VAConfigID config = VA_INVALID_ID;
  std::shared_ptr<DecodeContext> decoder;
  bool bottom_first = false;
  bool interlaced = true;

  explicit CardProcessingFixture(bool bff = false, bool fields = true,
                                 bool keep_policy = false)
      : bottom_first(bff), interlaced(fields) {
    // Processing output and first-field views are driven explicitly here.
    driver.StopWorkers();
    driver.stopping = false;
    io.allow_device_open = true;
    context.pDriverData = &driver;
    VAConfigAttrib processing = {VAConfigAttribDecProcessing, VA_DEC_PROCESSING};
    Require(CreateConfig(&context, VAProfileH264Main, VAEntrypointVLD,
                         &processing, 1, &config) == VA_STATUS_SUCCESS &&
                CreateContext(&context, config, 1920, 1088, 0, nullptr, 0,
                              &id) == VA_STATUS_SUCCESS,
            "public decode-processing config admits a coded-size context");
    decoder = driver.contexts.at(id);
    if (!keep_policy) {
      decoder->live_h264 = false;
      decoder->auto_h264_prefeed = false;
    }
    for (VASurfaceID surface_id : {1U, 2U, 11U, 12U}) {
      const bool original = surface_id < 10;
      auto surface = std::make_shared<Surface>();
      Require(surface->AllocateInternal(nullptr, -1,
                  original ? 1920 : 1280, original ? 1088 : 720,
                  VA_FOURCC_NV12), "allocate public processing surfaces");
      surface->id = surface_id;
      surface->ready = true;
      driver.surfaces[surface_id] = surface;
    }
  }

  VAStatus Render(VASurfaceID original, VASurfaceID output,
                  unsigned visible_height = 1080, unsigned syntax_fault = 0,
                  bool idr = true) {
    VAStatus status = BeginPicture(&context, id, original);
    if (status != VA_STATUS_SUCCESS)
      return status;
    VAPictureParameterBufferH264 picture = {};
    picture.picture_width_in_mbs_minus1 = 119;
    picture.picture_height_in_mbs_minus1 = 67;
    picture.num_ref_frames = 1;
    picture.seq_fields.bits.chroma_format_idc = 1;
    picture.seq_fields.bits.frame_mbs_only_flag = !interlaced;
    picture.seq_fields.bits.mb_adaptive_frame_field_flag = interlaced;
    picture.seq_fields.bits.direct_8x8_inference_flag = 1;
    picture.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    switch (syntax_fault) {
      case 1: picture.seq_fields.bits.chroma_format_idc = 0; break;
      case 2: picture.seq_fields.bits.chroma_format_idc = 2; break;
      case 3: picture.seq_fields.bits.residual_colour_transform_flag = 1; break;
      case 4: picture.bit_depth_luma_minus8 = 1; break;
      case 5: picture.bit_depth_chroma_minus8 = 1; break;
      case 6: picture.pic_fields.bits.field_pic_flag = 1; break;
      case 7: picture.seq_fields.bits.mb_adaptive_frame_field_flag = 0; break;
    }
    VASliceParameterBufferH264 slice = {};
    slice.slice_type = idr ? 2 : 0;
    uint8_t bytes[] = {static_cast<uint8_t>(idr ? 0x65 : 0x41), 0x88, 0x84};
    VARectangle source = {0, 0, 1920, static_cast<uint16_t>(visible_height)};
    VAProcPipelineParameterBuffer pipeline = {};
    pipeline.surface = original;
    pipeline.surface_region = &source;
    pipeline.num_additional_outputs = 1;
    pipeline.additional_outputs = &output;
    pipeline.input_surface_flag = pipeline.output_surface_flag =
        !interlaced ? VA_FRAME_PICTURE :
            bottom_first ? VA_BOTTOM_FIELD_FIRST : VA_TOP_FIELD_FIRST;
    VABufferID buffers[4] = {};
    Require(CreateBuffer(&context, id, VAPictureParameterBufferType,
                         sizeof(picture), 1, &picture, &buffers[0]) == VA_STATUS_SUCCESS &&
                CreateBuffer(&context, id, VASliceParameterBufferType,
                             sizeof(slice), 1, &slice, &buffers[1]) == VA_STATUS_SUCCESS &&
                CreateBuffer(&context, id, VASliceDataBufferType,
                             sizeof(bytes), 1, bytes, &buffers[2]) == VA_STATUS_SUCCESS &&
                CreateBuffer(&context, id, VAProcPipelineParameterBufferType,
                             sizeof(pipeline), 1, &pipeline, &buffers[3]) == VA_STATUS_SUCCESS,
            "create public processing parameters");
    status = RenderPicture(&context, id, buffers, 4);
    // All geometry and output IDs must have been copied at Render, not read
    // again when the caller destroys its buffers or mutates pointed-to data.
    source.height = 1088;
    output = VA_INVALID_SURFACE;
    for (const auto buffer : buffers)
      Require(DestroyBuffer(&context, buffer) == VA_STATUS_SUCCESS,
              "Render snapshots survive destruction of all client buffers");
    return status;
  }

  VAStatus Submit(VASurfaceID original, VASurfaceID output) {
    const VAStatus status = Render(original, output);
    return status == VA_STATUS_SUCCESS ? EndPicture(&context, id) : status;
  }

  void Complete(uint64_t token) {
    io.output_width = 1280;
    io.output_height = 720;
    if (!interlaced) {
      io.outputs.push_back(token);
      io.picture_flags.push_back(0);
      Require(ReceiveAvailable(&driver, decoder.get()) == VA_STATUS_SUCCESS,
              "scaled progressive picture completes through real output handling");
      return;
    }
    const uint32_t order = bottom_first ? VDEC_FLAG_BOTTOM_FIRST : 0;
    for (const bool bottom : {bottom_first, !bottom_first}) {
      io.outputs.push_back(token);
      io.picture_flags.push_back(VDEC_FLAG_INTERLACED_SRC | order |
          (bottom ? VDEC_FLAG_BOTTOMFIELD : VDEC_FLAG_TOPFIELD));
    }
    Require(ReceiveAvailable(&driver, decoder.get()) == VA_STATUS_SUCCESS,
            "scaled top/bottom fields complete through real output handling");
  }
};

static void CardScalingProgressiveOutput() {
  CardProcessingFixture fixture(false, false);
  Require(fixture.Submit(1, 11) == VA_STATUS_SUCCESS && fixture.io.scale_calls == 1,
          "progressive processing takes the explicit card-scale path");
  const auto original = fixture.driver.surfaces.at(1);
  const auto output = fixture.driver.surfaces.at(11);
  const auto picture = original->opaque_picture;
  Require(picture && picture->width == 1280 && picture->height == 720 &&
              picture->h264_crop_unit_y == 2,
          "progressive capture retains its crop unit and reduced allocation");
  fixture.Complete(kTimestampStep);
  Require(picture->ready && output->ready &&
              output->frame_timestamp == kTimestampStep &&
              picture->frame_timestamp == kTimestampStep &&
              fixture.decoder->pending.empty() &&
              fixture.decoder->replay.outstanding() == 0 &&
              !fixture.decoder->h264_first_field.valid &&
              SyncSurface2(&fixture.context, 1, 0) == VA_STATUS_SUCCESS &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "release") == 1 && !fixture.io.invalid,
          "one progressive output completes both exact handles and releases once");
  // This is a constant-plane transport oracle, not scaler-quality evidence.
  // Native comparison independently checks nonuniform card-resampled pixels.
  for (const auto &surface : {picture, output}) {
    for (unsigned row = 0; row < 720; ++row)
      for (unsigned column = 0; column < 1280; ++column)
        Require(surface->planes[0][static_cast<size_t>(row) * surface->pitch[0] + column] ==
                    DecodeIoMock::Luma(kTimestampStep),
                "scaled progressive output copies every expected luma sample");
    for (unsigned row = 0; row < 360; ++row)
      for (unsigned column = 0; column < 1280; ++column)
        Require(surface->planes[1][static_cast<size_t>(row) * surface->pitch[1] + column] == 128,
                "scaled progressive output copies every expected chroma sample");
  }
}

static void CardScalingPreAcceptFailuresPreserveOwnership() {
  for (unsigned fault = 1; fault <= 7; ++fault) {
    CardProcessingFixture fixture;
    Require(fixture.Render(1, 11, 1080, fault) == VA_STATUS_SUCCESS &&
                EndPicture(&fixture.context, fixture.id) == VA_STATUS_ERROR_UNSUPPORTED_PROFILE &&
                fixture.io.inputs.empty() && fixture.io.scale_calls == 0 &&
                fixture.decoder->pending.empty() &&
                !fixture.driver.surfaces.at(1)->opaque_reference,
            "processing rejects unsupported chroma/depth or separate-field syntax before input");
  }
  for (const bool crop_phase : {false, true}) {
    CardProcessingFixture fixture;
    auto output = std::make_shared<Surface>();
    Require(output->AllocateInternal(nullptr, -1,
                crop_phase ? 1280 : 150, crop_phase ? 724 : 80,
                VA_FOURCC_NV12), "allocate field-geometry rejection target");
    output->id = 13;
    output->ready = true;
    fixture.driver.surfaces[13] = output;
    Require(fixture.Render(1, 13, crop_phase ? 1086 : 1024) == VA_STATUS_SUCCESS &&
                EndPicture(&fixture.context, fixture.id) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                fixture.io.inputs.empty() && fixture.io.scale_calls == 0 &&
                !fixture.driver.surfaces.at(1)->opaque_reference,
            "picture-aware admission rejects MBAFF crop phase or nonaligned card width");
  }
  {
    CardProcessingFixture fixture;
    Require(fixture.Render(1, 11) == VA_STATUS_SUCCESS,
            "capture processing request before a competing output transaction");
    const auto original = fixture.driver.surfaces.at(1);
    const auto output = fixture.driver.surfaces.at(11);
    VAConfigID vpp_config = VA_INVALID_ID;
    VAContextID vpp_context = VA_INVALID_ID;
    Require(CreateConfig(&fixture.context, VAProfileNone, VAEntrypointVideoProc,
                         nullptr, 0, &vpp_config) == VA_STATUS_SUCCESS &&
                CreateContext(&fixture.context, vpp_config, 1280, 720, 0,
                              nullptr, 0, &vpp_context) == VA_STATUS_SUCCESS &&
                BeginPicture(&fixture.context, vpp_context, 11) == VA_STATUS_SUCCESS,
            "another public context starts a new transaction on the captured output");
    const uint64_t transaction = output->picture_transaction;
    const auto pixels = output->storage;
    Require(transaction != fixture.decoder->processing_target_transaction &&
                EndPicture(&fixture.context, fixture.id) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                output->picture_transaction == transaction && !output->failed &&
                output->storage == pixels && !original->opaque_reference &&
                !original->opaque_picture && fixture.io.inputs.empty() &&
                fixture.io.scale_calls == 0 && fixture.decoder->pending.empty(),
            "stale processing End cannot steal, fail or modify a newer output writer");
  }
  for (const bool private_backing : {false, true}) {
    CardProcessingFixture fixture;
    Require(fixture.Render(1, 11) == VA_STATUS_SUCCESS,
            "capture processing request before allocation failure");
    const auto original = fixture.driver.surfaces.at(1);
    const auto output = fixture.driver.surfaces.at(11);
    const auto pixels = output->storage;
    const uint64_t transaction = output->picture_transaction;
    if (private_backing)
      allocation_fault::fail_size = 1280U * 720U * 3U / 2U;
    else
      allocation_fault::fail_next = true;
    const VAStatus status = EndPicture(&fixture.context, fixture.id);
    const bool fault_consumed = !allocation_fault::fail_next &&
                               allocation_fault::fail_size == 0;
    allocation_fault::fail_next = false;
    allocation_fault::fail_size = 0;
    Require(status == VA_STATUS_ERROR_ALLOCATION_FAILED && fault_consumed &&
                !original->opaque_reference && !original->opaque_picture &&
                !output->decode_picture.lock() && output->storage == pixels &&
                output->picture_transaction == transaction &&
                fixture.decoder->decoded_frames.empty() &&
                fixture.decoder->pending.empty() &&
                fixture.decoder->next_timestamp == kTimestampStep &&
                !fixture.decoder->processing_geometry.enabled &&
                fixture.io.inputs.empty() && fixture.io.scale_calls == 0,
            "SPS or small private-buffer allocation failure publishes no opaque identity or input");
    Require(fixture.Submit(1, 11) == VA_STATUS_SUCCESS,
            "a fresh transaction recovers after pre-accept allocation failure");
    fixture.Complete(kTimestampStep);
  }
}

static void CardScalingPublicIdentityAndGeometry() {
  CardScalingSetupOrderAndFailures();
  CardScalingProgressiveOutput();
  CardScalingPreAcceptFailuresPreserveOwnership();
  for (const bool bottom_first : {false, true}) {
    CardProcessingFixture fixture(bottom_first);
    Require(fixture.Submit(1, 11) == VA_STATUS_SUCCESS &&
                fixture.io.scale_calls == 1 &&
                fixture.io.input_format.height == 1088,
            "public integrated processing submits cropped SPS with coded input geometry");
    const auto original = fixture.driver.surfaces.at(1);
    const auto output = fixture.driver.surfaces.at(11);
    const auto first = original->opaque_picture;
    Require(first && first == output->decode_picture.lock() &&
                first->width == 1280 && first->height == 720 &&
                first->storage.size() == first->pitch[0] * 1080U &&
                original->opaque_reference && !original->ready,
            "only the small private capture is allocated and bound to both handles");
    VASurfaceStatus original_status = VASurfaceReady;
    Require(QuerySurfaceStatus(&fixture.context, 1, &original_status) == VA_STATUS_SUCCESS &&
                original_status == VASurfaceRendering,
            "opaque reference reports real pending decode completion");
    fixture.Complete(kTimestampStep);
    Require(first->ready && output->ready &&
                output->frame_timestamp == kTimestampStep &&
                first->frame_timestamp == kTimestampStep &&
                QuerySurfaceStatus(&fixture.context, 1, &original_status) == VA_STATUS_SUCCESS &&
                original_status == VASurfaceReady &&
                SyncSurface2(&fixture.context, 1, 0) == VA_STATUS_SUCCESS,
            "opaque original and additional output share exact completion, not full-size pixels");
    VAImage image = {};
    Require(DeriveImage(&fixture.context, 1, &image) == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT,
            "opaque original cannot export stale full-size backing");
    const auto first_pixels = first->storage;
    Require(fixture.Submit(2, 11) == VA_STATUS_SUCCESS &&
                original->opaque_picture == first &&
                fixture.driver.surfaces.at(2)->opaque_picture != first,
            "additional-output reuse does not rebind an older opaque original");
    fixture.Complete(2 * kTimestampStep);
    Require(first->storage == first_pixels && first->frame_timestamp == kTimestampStep &&
                SyncSurface2(&fixture.context, 1, 0) == VA_STATUS_SUCCESS,
            "retained original completion and pixels survive output reuse");
    const auto second = fixture.driver.surfaces.at(2)->opaque_picture;
    Require(fixture.Submit(1, 12) == VA_STATUS_SUCCESS &&
                original->opaque_picture != first &&
                fixture.driver.surfaces.at(11)->decode_picture.lock() == second,
            "original-reference reuse does not steal the previous additional output");
    fixture.Complete(3 * kTimestampStep);
    Require(fixture.io.scale_calls == 1 &&
                fixture.decoder->CaptureHeight() == 720 && !fixture.io.invalid,
            "stable geometry does not reconfigure a live decoder");
    auto alternative = std::make_shared<Surface>();
    Require(alternative->AllocateInternal(nullptr, -1, 960, 540, VA_FOURCC_NV12),
            "allocate rejected midstream alternative");
    alternative->id = 13;
    alternative->ready = true;
    fixture.driver.surfaces[13] = alternative;
    const size_t inputs = fixture.io.inputs.size();
    Require(fixture.Render(2, 13) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                fixture.io.inputs.size() == inputs &&
                fixture.decoder->processing_geometry.capture_width == 1280,
            "midstream scaling change is rejected before input or geometry mutation");
  }
  {
    CardProcessingFixture fixture;
    auto wrong_height = std::make_shared<Surface>();
    Require(wrong_height->AllocateInternal(nullptr, -1, 1280, 724, VA_FOURCC_NV12),
            "allocate rounded-height counterexample");
    wrong_height->id = 13;
    wrong_height->ready = true;
    fixture.driver.surfaces[13] = wrong_height;
    Require(fixture.Render(1, 13, 1088) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                fixture.io.inputs.empty() && fixture.io.scale_calls == 0,
            "1088-to-724 rounded geometry cannot impersonate a visible 720 capture");
  }
  for (const unsigned fault : {0U, 1U, 2U}) {
    CardProcessingFixture fixture;
    Require(fixture.Submit(1, 11) == VA_STATUS_SUCCESS,
            "submit before scaled field fault");
    const unsigned height = fault == 2 ? 724 : 720;
    std::vector<uint8_t> pixels(1280U * (height / 2U) * 2U, 128);
    BC_DTS_PROC_OUT first = {};
    first.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    first.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_TOPFIELD;
    first.PicInfo.width = 1280;
    first.PicInfo.height = height;
    first.PicInfo.timeStamp = kTimestampStep;
    first.Ybuff = pixels.data();
    first.YBuffDoneSz = pixels.size() / 4;
    if (fault == 1)
      --first.YBuffDoneSz;
    VAStatus status = ProcessDecodedOutput(&fixture.driver, fixture.decoder.get(), first);
    if (fault == 0) {
      Require(status == VA_STATUS_SUCCESS, "retain valid scaled first field");
      status = ProcessDecodedOutput(&fixture.driver, fixture.decoder.get(), first);
    }
    Require(status == VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.decoder->replay.failed() &&
                !fixture.driver.surfaces.at(11)->ready &&
                fixture.driver.surfaces.at(1)->opaque_picture->failed,
            "duplicate parity, short extent or actual 724-line field fails both identities closed");
  }
}

static void CardScalingLivePolicyAndOwnership() {
  SavedEnvironment saved_live("CRYSTALHD_VAAPI_LIVE_H264");
  SavedEnvironment saved_low("CRYSTALHD_VAAPI_LOW_LATENCY_H264");
  SavedEnvironment saved_ready("CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP");
  const auto set_policy = [](const char *name, int value) {
    if (value < 0) unsetenv(name);
    else setenv(name, value ? "1" : "0", 1);
  };
  for (int live : {-1, 0, 1}) {
    for (int low : {-1, 0, 1}) {
      for (bool ready : {false, true}) {
        set_policy("CRYSTALHD_VAAPI_LIVE_H264", live);
        set_policy("CRYSTALHD_VAAPI_LOW_LATENCY_H264", low);
        set_policy("CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP", ready ? 1 : -1);
        DecodeIoMock io;
        io.allow_device_open = true;
        MockDecodeScope scope(io);
        Driver driver(-1);
        VADriverContext context = {};
        context.pDriverData = &driver;
        VAConfigID config = VA_INVALID_ID;
        VAContextID id = VA_INVALID_ID;
        VAConfigAttrib processing = {VAConfigAttribDecProcessing, VA_DEC_PROCESSING};
        Require(CreateConfig(&context, VAProfileH264Main, VAEntrypointVLD,
                             &processing, 1, &config) == VA_STATUS_SUCCESS,
                "processing policy matrix creates a public config");
        const VAStatus status = CreateContext(&context, config, 1920, 1088, 0,
                                             nullptr, 0, &id);
        if (ready) {
          Require(status == VA_STATUS_ERROR_ATTR_NOT_SUPPORTED &&
                      id == VA_INVALID_ID && driver.contexts.empty() &&
                      io.open_attempts == 0 && io.inputs.empty(),
                  "READY cannot remap exact processing references, before any device open");
        } else {
          Require(status == VA_STATUS_SUCCESS, "LIVE/LOW are independent processing policies");
          const auto decoder = driver.contexts.at(id);
          Require(decoder->decode_processing && decoder->live_h264 == (live == 1) &&
                      decoder->live_h264_gap_tolerant == (live == 1) &&
                      decoder->live_h264_fast_path == (live == 1) &&
                      decoder->low_latency_h264 == (low == 1) &&
                      decoder->auto_h264_prefeed == (live != 1 && low < 0) &&
                      !decoder->experimental_ready_remap && io.inputs.empty() &&
                      io.scale_calls == 0,
                  "unset processing policy stays finite; explicit LIVE and LOW remain separate");
          Require(DestroyContext(&context, id) == VA_STATUS_SUCCESS &&
                      io.device_close_calls == 1 && driver.contexts.empty(),
                  "policy-only contexts release their device without decoding");
        }
        Require(DestroyConfig(&context, config) == VA_STATUS_SUCCESS && !io.invalid,
                "policy matrix owns and releases only its mock resources");
      }
    }
  }

  set_policy("CRYSTALHD_VAAPI_LIVE_H264", 1);
  set_policy("CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP", -1);
  for (bool prefeed : {false, true}) {
    set_policy("CRYSTALHD_VAAPI_LOW_LATENCY_H264", prefeed ? 1 : 0);
    for (bool bottom_first : {false, true}) {
      CardProcessingFixture fixture(bottom_first, true, true);
      auto &decode = *fixture.decoder;
      auto &io = fixture.io;
      io.prefeed_busy_once = prefeed;
      Require(fixture.Submit(1, 11) == VA_STATUS_SUCCESS,
              "live processing accepts the first actual IDR with small output");
      const auto first = fixture.driver.surfaces.at(1)->opaque_picture;
      Require(first && first == fixture.driver.surfaces.at(11)->decode_picture.lock() &&
                  first->width == 1280 && first->height == 720 &&
                  decode.pending.at(kTimestampStep) == 11 &&
                  decode.replay.outstanding() == 1 &&
                  io.inputs == std::vector<uint64_t>{kTimestampStep},
              "only the actual AU owns a small picture and opaque reference");
      if (prefeed) {
        Require(io.input_calls.size() == 2 && !io.input_calls.back().accepted &&
                    io.input_calls.back().bytes == H264LowLatencyPrefeed() &&
                    decode.pending_prefeed_timestamp == 2 * kTimestampStep &&
                    decode.prefed_timestamp == 0,
                "BUSY prefix retains only a bounded transport retry, not a picture");
        Require(PumpDecodeInput(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                    io.input_calls.size() == 3 && io.input_calls.back().accepted &&
                    decode.pending_prefeed_timestamp == 0 &&
                    decode.prefed_timestamp == 2 * kTimestampStep &&
                    decode.replay.outstanding() == 1 && decode.pending.size() == 1,
                "retry accepts the same prefix without new input or output ownership");
      }
      const size_t prior_calls = io.input_calls.size();
      Require(fixture.Render(2, 12, 1080, 0, false) == VA_STATUS_SUCCESS &&
                  EndPicture(&fixture.context, fixture.id) == VA_STATUS_SUCCESS,
              "actual non-IDR continuation preserves integrated scaling geometry");
      const auto second = fixture.driver.surfaces.at(2)->opaque_picture;
      const auto &continuation = io.input_calls.at(prior_calls);
      const std::vector<uint8_t> non_idr_slice = {0x41, 0x88, 0x84};
      Require(second && second != first &&
                  second == fixture.driver.surfaces.at(12)->decode_picture.lock() &&
                  decode.pending.at(2 * kTimestampStep) == 12 &&
                  decode.replay.live_stream_started() &&
                  io.inputs == std::vector<uint64_t>{kTimestampStep, 2 * kTimestampStep} &&
                  continuation.accepted &&
                  continuation.bytes.size() >= non_idr_slice.size() &&
                  std::equal(non_idr_slice.rbegin(), non_idr_slice.rend(),
                             continuation.bytes.rbegin()) &&
                  continuation.timestamp == (prefeed ? 0 : 2 * kTimestampStep) &&
                  io.input_calls.size() == prior_calls + (prefeed ? 2U : 1U) &&
                  decode.prefed_timestamp == (prefeed ? 3 * kTimestampStep : 0) &&
                  decode.pending.size() == 2,
              "non-IDR consumes an accepted prefix with zero transport tag but exact logical identity");
      fixture.Complete(kTimestampStep);
      const auto first_pixels = first->storage;
      io.output_width = 1280;
      io.output_height = 720;
      const auto queue_field = [&](bool bottom) {
        io.outputs.push_back(2 * kTimestampStep);
        io.picture_flags.push_back(VDEC_FLAG_INTERLACED_SRC |
            (bottom_first ? VDEC_FLAG_BOTTOM_FIRST : 0) |
            (bottom ? VDEC_FLAG_BOTTOMFIELD : VDEC_FLAG_TOPFIELD));
      };
      queue_field(bottom_first);
      Require(ReceiveAvailable(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                  decode.h264_first_field.valid && !second->ready &&
                  decode.replay.outstanding() == 1,
              "first scaled field remains paired to the exact outstanding picture");
      PendingVpp operation;
      operation.source = second;
      operation.decoder = fixture.decoder;
      operation.source_timestamp = 2 * kTimestampStep;
      operation.decoder_generation = decode.generation;
      operation.deinterlace.bob = true;
      operation.deinterlace.field = bottom_first ? crystalhd_vpp_bob::FieldParity::kBottom
                                                : crystalhd_vpp_bob::FieldParity::kTop;
      std::shared_ptr<Surface> compact;
      Surface fhd;
      SwsContext *scaler = nullptr;
      Require(PrepareFirstFieldBob(operation, &compact) == VA_STATUS_SUCCESS && compact &&
                  compact->width == 1280 && compact->height == 360 &&
                  fhd.AllocateCpuNv12(1920, 1080) &&
                  ProcessVpp(&scaler, nullptr, compact.get(), &fhd,
                             VARectangle{0, 0, 1280, 720}, VARectangle{0, 0, 1920, 1080},
                             false, operation.deinterlace, true) == VA_STATUS_SUCCESS &&
                  scaler == nullptr && fhd.ready &&
                  fhd.frame_timestamp == 2 * kTimestampStep && !second->ready &&
                  !fixture.driver.surfaces.at(12)->ready && decode.h264_first_field.valid,
              "private scaled first-field Bob can return FHD without completing its original");
      // Constant-plane transport/geometry oracle only; nonuniform field and
      // chroma resampling have independent VPP and native pixel oracles.
      for (unsigned row = 0; row < 1080; ++row)
        Require(fhd.planes[0][static_cast<size_t>(row) * fhd.pitch[0]] ==
                    DecodeIoMock::Luma(2 * kTimestampStep) &&
                    fhd.planes[0][static_cast<size_t>(row) * fhd.pitch[0] + 1919] ==
                    DecodeIoMock::Luma(2 * kTimestampStep),
                "early FHD Bob addresses every logical output row within its allocation");
      const auto gap_start = std::chrono::steady_clock::now();
      Require(SyncSurface2(&fixture.context, 2, 150ULL * 1000 * 1000) ==
                  VA_STATUS_ERROR_TIMEDOUT &&
                  std::chrono::steady_clock::now() - gap_start >= std::chrono::milliseconds(100) &&
                  std::count(io.events.begin(), io.events.end(), "seal") == 0 &&
                  !decode.replay.sealed() && !decode.replay.failed() &&
                  decode.h264_first_field.valid && decode.replay.outstanding() == 1 &&
                  !second->ready && first->storage == first_pixels,
              "explicit LIVE survives an idle gap beyond 100 ms without inferred EOS or remapping");
      queue_field(!bottom_first);
      // Preserve the real queued complement, suppress the generic mock's
      // duplicate full-frame output, then request an explicit finite drain.
      io.early_eos = true;
      Require(SealDecodeBatch(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                  ReceiveAvailable(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                  second->ready && fixture.driver.surfaces.at(12)->ready &&
                  SyncSurface2(&fixture.context, 2, 0) == VA_STATUS_SUCCESS &&
                  SyncSurface2(&fixture.context, 1, 0) == VA_STATUS_SUCCESS &&
                  second->frame_timestamp == 2 * kTimestampStep &&
                  first->storage == first_pixels && !decode.h264_first_field.valid &&
                  decode.replay.outstanding() == 0 && decode.pending.empty() &&
                  decode.prefed_timestamp == 0 && io.prefed_timestamp == 0 &&
                  std::count(io.events.begin(), io.events.end(), "seal") == 1 &&
                  std::count(io.events.begin(), io.events.end(), "eos") == 1,
              "explicit EOS follows both real fields and retires only the orphan prefix");
      Require(DestroyContext(&fixture.context, fixture.id) == VA_STATUS_SUCCESS &&
                  fixture.driver.contexts.empty() && decode.device == nullptr &&
                  io.device_close_calls == 1 && io.outputs.empty() &&
                  io.picture_flags.empty() && io.scale_calls == 1 && !io.invalid,
              "live scaled context closes without leaked hardware, outputs or reconfiguration");
    }
  }
}

static void RealH264SubmitCompletesFieldPairs() {
  for (bool direct : {false, true}) {
    for (bool bottom_first : {false, true}) {
      DecodeIoMock io;
      MockDecodeScope scope(io);
      Driver driver{-1};
      DecodeContext decoder;
      decoder.id = 9;
      decoder.generation = 5;
      decoder.profile = VAProfileH264Main;
      decoder.width = 16;
      decoder.height = 32;
      decoder.device = io.handle();
      decoder.decoder_open = decoder.decoder_started = true;
      const bool retained_reset = direct && bottom_first;
      decoder.hardware_close_pending = retained_reset;
      io.allow_device_open = retained_reset;
      decoder.live_h264 = true;
      decoder.live_h264_gap_tolerant = direct;
      decoder.live_h264_fast_path = direct;
      decoder.auto_h264_prefeed = !direct;
      decoder.target = 1;
      decoder.have_picture = true;
      decoder.picture.picture_width_in_mbs_minus1 = 0;
      decoder.picture.picture_height_in_mbs_minus1 = 1;
      decoder.picture.num_ref_frames = 1;
      decoder.picture.seq_fields.bits.chroma_format_idc = 1;
      decoder.picture.seq_fields.bits.frame_mbs_only_flag = 0;
      decoder.picture.seq_fields.bits.mb_adaptive_frame_field_flag = 1;
      decoder.picture.seq_fields.bits.direct_8x8_inference_flag = 1;
      decoder.picture.seq_fields.bits.pic_order_cnt_type = 0;
      decoder.picture.pic_fields.bits.deblocking_filter_control_present_flag = 1;
      VASliceParameterBufferH264 slice = {};
      slice.slice_type = 2;
      decoder.slices.push_back(slice);
      decoder.slice_data.push_back({0x65, 0x88, 0x84});
      auto target = std::make_shared<Surface>();
      Require(target->AllocateInternal(nullptr, -1, 16, 32,
                                       VA_FOURCC_NV12),
              "allocate real-submit H.264 target");
      target->id = 1;
      target->direct_decode_eligible = direct;
      driver.surfaces[1] = target;

      Require(SubmitPicture(&driver, &decoder, VAProfileH264Main) ==
                      VA_STATUS_SUCCESS &&
                  io.inputs == std::vector<uint64_t>{kTimestampStep} &&
                  decoder.pending.count(kTimestampStep) == 1 &&
                  decoder.replay.outstanding() == 1 &&
                  decoder.prefed_timestamp == (direct ? 0 : 2 * kTimestampStep) &&
                  io.input_calls.size() == (direct ? 1U : 2U) &&
                  io.open_attempts == (retained_reset ? 1U : 0U) &&
                  !decoder.hardware_close_pending,
              "real H.264 submit reopens a retained reset before accepting one interlaced access unit");
      const uint32_t order = bottom_first ? VDEC_FLAG_BOTTOM_FIRST : 0;
      const uint32_t first = bottom_first ? VDEC_FLAG_BOTTOMFIELD
                                          : VDEC_FLAG_TOPFIELD;
      const uint32_t second = bottom_first ? VDEC_FLAG_TOPFIELD
                                           : VDEC_FLAG_BOTTOMFIELD;
      io.output_width = 16;
      io.output_height = 32;
      io.outputs.push_back(kTimestampStep);
      io.picture_flags.push_back(VDEC_FLAG_INTERLACED_SRC | first | order);
      io.outputs.push_back(kTimestampStep);
      io.picture_flags.push_back(VDEC_FLAG_INTERLACED_SRC | second | order);
      Require(ReceiveAvailable(&driver, &decoder) == VA_STATUS_SUCCESS &&
                  !decoder.h264_first_field.valid && decoder.pending.empty() &&
                  decoder.replay.outstanding() == 0 && target->ready &&
                  target->frame_timestamp == kTimestampStep &&
                  std::count(io.events.begin(), io.events.end(), "release") == 2 &&
                  !io.invalid,
              "real submit/output path completes TFF and BFF in private and live-direct modes");
    }
  }
}

static void TraceIdentityUsesImmutableTokenAndLiveAliasOwner() {
  TraceEnvironment environment;
  const int trace_fd = memfd_create("crystalhd-vaapi-trace-test", MFD_CLOEXEC);
  Require(trace_fd >= 0, "allocate production trace fixture");
  {
    Driver driver{-1, trace_fd};
    // This trace fixture processes output and observes ready aliases directly.
    driver.StopWorkers();
    driver.stopping = false;
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
    const int trace_fd = memfd_create("crystalhd-reuse-imported-planes", MFD_CLOEXEC);
    Require(trace_fd >= 0, "allocate mapped-plane reuse trace sink");
    {
      Driver driver{-1, trace_fd};
      auto decode = std::make_shared<DecodeContext>();
      const size_t bytes = 16 * 16 * 3 / 2;
      const int y = TraceBackingFd("crystalhd-reuse-imported-y", bytes);
      const int duplicate_y = dup(y);
      Require(duplicate_y >= 0, "duplicate mapped luma backing identity");
      const int uv = TraceBackingFd("crystalhd-reuse-imported-uv", bytes);
      const int unrelated = TraceBackingFd("crystalhd-reuse-imported-other", bytes);
      const auto surface = InstallTraceReuseSurface(
          &driver, decode, {y, duplicate_y, uv, unrelated});
      VADRMPRIMESurfaceDescriptor imported = {};
      imported.fourcc = VA_FOURCC_NV12;
      imported.width = imported.height = 16;
      imported.num_objects = 2;
      imported.objects[0].fd = y;
      imported.objects[1].fd = uv;
      for (auto &object : imported.objects) {
        object.size = bytes;
        object.drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
      }
      imported.num_layers = 1;
      imported.layers[0].drm_format = DRM_FORMAT_NV12;
      imported.layers[0].num_planes = 2;
      imported.layers[0].object_index[1] = 1;
      imported.layers[0].pitch[0] = imported.layers[0].pitch[1] = 16;
      Require(GetPrimeLayout(imported, &surface->layout),
              "register only the two actual exported mapped objects");
      VADriverContext context = {};
      context.pDriverData = &driver;
      VADRMPRIMESurfaceDescriptor exported = {};
      Require(ExportSurfaceHandle(&context, 1,
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_READ_ONLY, &exported) ==
                  VA_STATUS_SUCCESS && exported.num_objects == 2,
              "arm reuse observation for both mapped plane objects");
      ClosePrimeDescriptor(&exported);
      probe_poll_calls = 0;
      probe_poll_results = {{1, POLLOUT, 0}, {0, 0, 0}};
      Require(BeginPicture(&context, 7, 1) == VA_STATUS_SUCCESS &&
                  probe_poll_calls == 2 && probe_poll_results.empty(),
              "mapped reuse probes chroma too, deduplicates luma and ignores unrelated fds");
    }
    const std::string trace = ReadTraceFd(trace_fd);
    close(trace_fd);
    const std::string probe = FindTraceEvent(
        trace, "read_export_reuse_implicit_fence_probe");
    Require(TraceFieldIs(probe, "identified_exported_object_count", 2) &&
                TraceFieldIs(probe, "pollout_count", 1) &&
                TraceFieldIs(probe, "not_pollout_count", 1) &&
                TraceFieldIs(probe, "probe_error_count", 0) &&
                TraceSignedFieldIs(probe, "outcome", -EAGAIN),
            "an unsignaled chroma reader is not reported as fully idle reuse");
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
    // Remap publication/readback is explicitly sequenced, not worker progress.
    driver.StopWorkers();
    driver.stopping = false;
    auto decode = std::make_shared<DecodeContext>();
    decode->id = 7;
    decode->generation = 3;
    decode->profile = VAProfileH264Main;
    decode->width = decode->height = 16;
    decode->live_h264 = true;
    decode->live_h264_gap_tolerant = true;
    decode->live_h264_fast_path = true;
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
    requested->experimental_submitted_at_ns = MonotonicNowNs();
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
    surface->experimental_submitted_at_ns = 1;
    surface->experimental_presentation_at_ns = 1;
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
    surface->experimental_submitted_at_ns = 1;
    surface->experimental_presentation_at_ns = 1;
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
  SavedEnvironment delay("CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US");
  SavedEnvironment policy("CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY");
  struct GateState {
    bool transport;
    bool gap_tolerant;
    bool fast_path;
    bool low_latency;
    bool auto_prefeed;
    bool enabled;
    bool latest;
    uint64_t delay_ns;
  };
  const auto create = [](const char *live_value, const char *low_value,
                         const char *remap_value, const char *delay_value,
                         const char *policy_value, VAProfile profile) {
    const auto assign = [](const char *name, const char *value) {
      if (value != nullptr)
        setenv(name, value, 1);
      else
        unsetenv(name);
    };
    assign("CRYSTALHD_VAAPI_LIVE_H264", live_value);
    assign("CRYSTALHD_VAAPI_LOW_LATENCY_H264", low_value);
    assign("CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP", remap_value);
    assign("CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US", delay_value);
    assign("CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY", policy_value);
    DecodeIoMock io;
    io.allow_device_open = true;
    MockDecodeScope scope(io);
    Driver driver{-1};
    driver.configs[1] = {profile, VAEntrypointVLD};
    VADriverContext context = {};
    context.pDriverData = &driver;
    VAContextID context_id = VA_INVALID_ID;
    Require(CreateContext(&context, 1, 16, 16, 0, nullptr, 0,
                          &context_id) == VA_STATUS_SUCCESS,
            "create environment-gated remap context");
    const std::shared_ptr<DecodeContext> decode =
        driver.contexts.at(context_id);
    return GateState{decode->live_h264,
                     decode->live_h264_gap_tolerant,
                     decode->live_h264_fast_path,
                     decode->low_latency_h264,
                     decode->auto_h264_prefeed,
                     decode->experimental_ready_remap,
                     decode->experimental_ready_latest,
                     decode->experimental_ready_delay_ns};
  };

  const GateState default_h264 =
      create(nullptr, nullptr, nullptr, nullptr, nullptr, VAProfileH264Main);
  const GateState fast_h264 =
      create("1", nullptr, nullptr, nullptr, nullptr, VAProfileH264Main);
  const GateState low_h264 =
      create("1", "1", nullptr, nullptr, nullptr, VAProfileH264Main);
  const GateState finite_low_h264 =
      create(nullptr, "1", "1", nullptr, nullptr, VAProfileH264Main);
  const GateState strict_h264 =
      create("0", "1", "1", nullptr, nullptr, VAProfileH264Main);
  const GateState invalid_h264 =
      create("yes", "1", "1", nullptr, nullptr, VAProfileH264Main);
  const GateState default_mpeg2 =
      create(nullptr, nullptr, nullptr, nullptr, nullptr, VAProfileMPEG2Main);
  Require(default_h264.transport && !default_h264.gap_tolerant &&
              !default_h264.fast_path &&
              !default_h264.low_latency && !default_h264.enabled &&
              default_h264.auto_prefeed &&
              fast_h264.transport && fast_h264.gap_tolerant &&
              fast_h264.fast_path &&
              !fast_h264.low_latency && !fast_h264.enabled &&
              !fast_h264.auto_prefeed &&
              low_h264.transport && low_h264.gap_tolerant &&
              low_h264.fast_path &&
              low_h264.low_latency && !low_h264.enabled &&
              !low_h264.auto_prefeed &&
              finite_low_h264.transport && !finite_low_h264.gap_tolerant &&
              !finite_low_h264.fast_path && finite_low_h264.low_latency &&
              !finite_low_h264.enabled && !finite_low_h264.auto_prefeed &&
              !strict_h264.transport && !strict_h264.gap_tolerant &&
              !strict_h264.fast_path &&
              strict_h264.low_latency && !strict_h264.enabled &&
              !invalid_h264.transport && !invalid_h264.gap_tolerant &&
              !invalid_h264.fast_path &&
              invalid_h264.low_latency && !invalid_h264.enabled &&
              !default_mpeg2.transport && !default_mpeg2.gap_tolerant &&
              !default_mpeg2.fast_path &&
              !default_mpeg2.low_latency && !default_mpeg2.enabled &&
              !default_mpeg2.auto_prefeed,
          "default H.264 transport is bounded while fast paths remain exact opt-ins");
  Require(!create(nullptr, "0", nullptr, nullptr, nullptr,
                   VAProfileH264Main).low_latency &&
              !create(nullptr, "0", nullptr, nullptr, nullptr,
                      VAProfileH264Main).auto_prefeed &&
              !create(nullptr, "yes", nullptr, nullptr, nullptr,
                      VAProfileH264Main).low_latency &&
              !create(nullptr, "yes", nullptr, nullptr, nullptr,
                      VAProfileH264Main).auto_prefeed &&
              create("0", nullptr, nullptr, nullptr, nullptr,
                     VAProfileH264Main).auto_prefeed &&
              !create(nullptr, "1", nullptr, nullptr, nullptr,
                      VAProfileMPEG2Main).low_latency,
          "explicit prefeed is H.264-only and any override disables automatic eligibility");

  Require(!create(nullptr, nullptr, "1", nullptr, nullptr,
                  VAProfileH264Main).enabled &&
              !create("1", "1", "0", nullptr, nullptr,
                      VAProfileH264Main).enabled &&
              !create("1", "1", "yes", nullptr, nullptr,
                      VAProfileH264Main).enabled &&
              create("1", "1", "1", nullptr, nullptr,
                     VAProfileH264Main).enabled &&
              !create("1", "1", "1", nullptr, nullptr,
                      VAProfileMPEG2Main).enabled,
          "remap is exact opt-in and restricted to live low-latency H.264");

  const GateState delayed = create("1", "1", "1", "16667", "previous",
                                   VAProfileH264Main);
  const GateState latest = create("1", "1", "1", "33333", "latest",
                                  VAProfileH264Main);
  const GateState immediate = create("1", "1", "1", "0", "previous",
                                     VAProfileH264Main);
  const GateState maximum = create("1", "1", "1", "50000", "latest",
                                   VAProfileH264Main);
  Require(immediate.enabled && !immediate.latest &&
              immediate.delay_ns == 0 &&
              delayed.enabled && !delayed.latest &&
              delayed.delay_ns == kExperimentalDelay16Ns &&
              latest.enabled && latest.latest &&
              latest.delay_ns == kExperimentalDelay33Ns &&
              maximum.enabled && maximum.latest &&
              maximum.delay_ns == kExperimentalDelay50Ns &&
              !create("1", "1", "1", "16.67", "previous",
                      VAProfileH264Main).enabled &&
              !create("1", "1", "1", "50001", "previous",
                      VAProfileH264Main).enabled &&
              !create("1", "1", "1", "50000", "newest",
                      VAProfileH264Main).enabled,
          "experimental delay and selection policy are bounded exact opt-ins");
  for (const auto &configured :
       {std::pair<const char *, uint64_t>{"0", 0},
        {"16667", kExperimentalDelay16Ns},
        {"33333", kExperimentalDelay33Ns},
        {"50000", kExperimentalDelay50Ns}}) {
    for (const char *configured_policy : {"previous", "latest"}) {
      const GateState state = create("1", "1", "1", configured.first,
                                     configured_policy, VAProfileH264Main);
      Require(state.enabled &&
                  state.latest ==
                      (strcmp(configured_policy, "latest") == 0) &&
                  state.delay_ns == configured.second,
              "every policy and delay combination reaches its requested experimental state");
    }
  }

  DecodeContext closing;
  closing.experimental_last_delivered_ordinal = 9;
  Require(closing.CloseHardware() == BC_STS_SUCCESS &&
              closing.experimental_last_delivered_ordinal == 9 &&
              closing.Close() == BC_STS_SUCCESS &&
              closing.experimental_last_delivered_ordinal == 0,
          "hardware reopen preserves and full close resets the experimental delivery watermark");

  Driver driver{-1};
  DecodeContext decode;
  decode.experimental_ready_remap = true;
  decode.have_picture = true;
  decode.picture.seq_fields.bits.chroma_format_idc = 1;
  decode.target = 1;
  decode.slices.resize(1);
  decode.slices.front().slice_type = 1;
  decode.slice_data.push_back({1});
  Require(SubmitPicture(&driver, &decode, VAProfileH264Main) ==
              VA_STATUS_ERROR_UNSUPPORTED_PROFILE,
          "experimental remap fails closed before accepting a B slice");
}

static void DefaultH264TransportIsBounded() {
  DecodeIoMock io;
  io.allow_device_open = true;
  MockDecodeScope scope(io);
  SavedEnvironment live("CRYSTALHD_VAAPI_LIVE_H264");
  unsetenv("CRYSTALHD_VAAPI_LIVE_H264");
  Driver driver{-1};
  driver.configs[1] = {VAProfileH264Main, VAEntrypointVLD};
  VADriverContext context = {};
  context.pDriverData = &driver;
  VAContextID context_id = VA_INVALID_ID;
  Require(CreateContext(&context, 1, 16, 16, 0, nullptr, 0,
                        &context_id) == VA_STATUS_SUCCESS,
          "create default bounded H.264 context");
  const std::shared_ptr<DecodeContext> decode = driver.contexts.at(context_id);
  Require(decode->live_h264 && !decode->live_h264_fast_path,
          "default context separates bounded transport from fast paths");
  for (uint64_t picture = 1; picture <= 900; ++picture) {
    Require(decode->replay.Append(picture, picture == 1, {1}),
            "default context accepts long-running H.264 input");
    SendNext(&decode->replay, picture);
    Require(decode->replay.Observe(picture) ==
                CrystalHDDecodeReplay::Output::New &&
                decode->replay.cached_pictures() <= 512 &&
                decode->replay.outstanding() == 0,
            "default context bounds completed H.264 history");
  }

  setenv("CRYSTALHD_VAAPI_LIVE_H264", "0", 1);
  Driver strict_driver{-1};
  strict_driver.configs[1] = {VAProfileH264Main, VAEntrypointVLD};
  VADriverContext strict_context = {};
  strict_context.pDriverData = &strict_driver;
  context_id = VA_INVALID_ID;
  Require(CreateContext(&strict_context, 1, 16, 16, 0, nullptr, 0,
                        &context_id) == VA_STATUS_SUCCESS,
          "create explicit strict H.264 context");
  const std::shared_ptr<DecodeContext> strict =
      strict_driver.contexts.at(context_id);
  for (uint64_t picture = 1; picture <= 512; ++picture) {
    Require(strict->replay.Append(picture, picture == 1, {1}),
            "strict context accepts its bounded replay prefix");
    SendNext(&strict->replay, picture);
    Require(strict->replay.Observe(picture) ==
                CrystalHDDecodeReplay::Output::New,
            "strict context completes accepted input exactly");
  }
  Require(!strict->live_h264 && !strict->live_h264_fast_path &&
              !strict->replay.Append(513, false, {1}) &&
              strict->replay.failed(),
          "explicit strict mode preserves the 512-picture replay guard");
}

static void ExperimentalPolicyDelayMatrix() {
  const std::array<std::pair<uint64_t, const char *>, 4> delays = {{
      {0, "0"},
      {kExperimentalDelay16Ns, "16667"},
      {kExperimentalDelay33Ns, "33333"},
      {kExperimentalDelay50Ns, "50000"},
  }};
  for (bool latest : {false, true}) {
    for (const auto &configured : delays) {
      Driver driver{-1};
      auto decode = std::make_shared<DecodeContext>();
      decode->id = 1;
      decode->generation = 1;
      decode->experimental_ready_remap = true;
      decode->experimental_ready_latest = latest;
      decode->experimental_ready_delay_ns = configured.first;
      driver.contexts[1] = decode;

      auto requested = std::make_shared<Surface>();
      Require(requested->AllocateInternal(nullptr, -1, 16, 16,
                                          VA_FOURCC_NV12),
              "allocate policy-delay request surface");
      requested->id = 1;
      requested->expected_timestamp = 400000;
      requested->decode_identity = requested->trace_decode_identity = 704;
      requested->trace_context = 1;
      requested->trace_generation = 1;
      requested->trace_submission_ordinal = 4;
      requested->experimental_write_lease = true;
      requested->experimental_write_context = 1;
      requested->experimental_write_generation = 1;
      requested->experimental_write_submission_ordinal = 4;
      requested->experimental_submitted_at_ns = MonotonicNowNs();
      driver.surfaces[1] = requested;

      auto candidate = [&](uint64_t ordinal) {
        auto surface = std::make_shared<Surface>();
        Require(surface->AllocateInternal(nullptr, -1, 16, 16,
                                          VA_FOURCC_NV12),
                "allocate policy-delay candidate surface");
        surface->expected_timestamp = surface->frame_timestamp =
            ordinal * 100000;
        surface->ready = true;
        surface->decode_identity = surface->trace_decode_identity =
            700 + ordinal;
        surface->trace_context = 1;
        surface->trace_generation = 1;
        surface->trace_submission_ordinal = ordinal;
        memset(surface->planes[0], static_cast<int>(ordinal),
               surface->storage.size());
        decode->decoded_frames[surface->expected_timestamp] = surface;
        QueueExperimentalReady(&driver, decode.get(), surface);
        surface->experimental_ready_at_ns =
            requested->experimental_submitted_at_ns;
        return surface;
      };
      const auto second = candidate(2);
      const auto first = candidate(1);

      if (configured.first != 0)
        Require(!SelectExperimentalReady(
                    *decode, *requested,
                    requested->experimental_submitted_at_ns +
                        configured.first - 1),
                "every nonzero delay rejects selection one nanosecond before its threshold");
      const auto before_previous = SelectExperimentalReady(
          *decode, *requested,
          requested->experimental_submitted_at_ns + configured.first);
      Require(latest ? before_previous == second : !before_previous,
              "latest selects the highest shuffled older candidate while previous rejects N-2 when N-1 is absent");

      const auto previous = candidate(3);
      Require(SelectExperimentalReady(
                  *decode, *requested,
                  requested->experimental_submitted_at_ns +
                      configured.first) == previous,
              "every policy-delay combination selects N-1 at the threshold");

      const uint64_t commit_start = MonotonicNowNs();
      requested->experimental_submitted_at_ns =
          commit_start - configured.first;
      bool committed = false;
      Require(TryExperimentalPresentation(&driver, requested, &committed) ==
                      VA_STATUS_SUCCESS &&
                  committed &&
                  ExperimentalPresentationReadable(&driver,
                                                    requested.get()) &&
                  requested->presentation_payload_submission_ordinal ==
                      3 &&
                  requested->planes[0][0] == 3 &&
                  requested->experimental_presentation_at_ns >=
                      requested->experimental_submitted_at_ns +
                          configured.first &&
                  decode->experimental_ready.empty() &&
                  decode->decoded_frames.empty() &&
                  !first->experimental_ready_queued &&
                  !second->experimental_ready_queued &&
                  !previous->experimental_ready_queued,
              "every policy-delay combination commits N-1 only after its threshold and retires older candidates");
    }
  }
}

static void ExperimentalRemapCpuOwnershipFailures() {
  const std::array<uint64_t, 4> failures = {
      DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ,
      DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE,
      DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE,
      DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ,
  };
  for (uint64_t failure : failures) {
    Driver driver{-1};
    auto decode = std::make_shared<DecodeContext>();
    decode->id = 1;
    decode->generation = 1;
    decode->experimental_ready_remap = true;
    driver.contexts[1] = decode;

    auto source = std::make_shared<Surface>();
    auto requested = std::make_shared<Surface>();
    for (const auto &surface : {source, requested})
      Require(surface->AllocateInternal(nullptr, -1, 16, 16,
                                        VA_FOURCC_NV12),
              "allocate remap CPU-ownership failure surface");
    source->expected_timestamp = source->frame_timestamp = 100000;
    source->ready = true;
    source->decode_identity = source->trace_decode_identity = 801;
    source->trace_context = 1;
    source->trace_generation = 1;
    source->trace_submission_ordinal = 1;
    memset(source->planes[0], 1, source->storage.size());
    decode->decoded_frames[source->expected_timestamp] = source;
    QueueExperimentalReady(&driver, decode.get(), source);

    requested->id = 1;
    requested->expected_timestamp = 200000;
    requested->decode_identity = requested->trace_decode_identity = 802;
    requested->trace_context = 1;
    requested->trace_generation = 1;
    requested->trace_submission_ordinal = 2;
    requested->experimental_write_lease = true;
    requested->experimental_write_context = 1;
    requested->experimental_write_generation = 1;
    requested->experimental_write_submission_ordinal = 2;
    requested->experimental_submitted_at_ns = MonotonicNowNs();
    driver.surfaces[1] = requested;

    const int fd = memfd_create("crystalhd-remap-cpu-test", MFD_CLOEXEC);
    Require(fd >= 0, "allocate remap CPU-ownership mock fd");
    source->object_fds.push_back(fd);
    requested->object_fds.push_back(fd);
    mock_sync_fd = fd;
    mock_failed_sync_flags = failure;
    mock_sync_calls.clear();

    bool committed = true;
    const VAStatus result =
        TryExperimentalPresentation(&driver, requested, &committed);
    const std::vector<uint64_t> start_read = {
        DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    const std::vector<uint64_t> start_write = {
        DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ,
        DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE,
        DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
    const std::vector<uint64_t> end_access = {
        DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ,
        DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE,
        DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE,
        DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
    const auto &expected_calls =
        failure == (DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)
            ? start_read
            : failure == (DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)
                  ? start_write
                  : end_access;
    Require(result == VA_STATUS_ERROR_OPERATION_FAILED && !committed &&
                !requested->experimental_presentation &&
                requested->experimental_write_lease &&
                decode->experimental_last_delivered_ordinal == 0 &&
                decode->experimental_ready.size() == 1 &&
                decode->experimental_ready.front() == source &&
                source->experimental_ready_queued &&
                decode->decoded_frames.size() == 1 &&
                decode->decoded_frames.at(100000) == source &&
                mock_sync_calls == expected_calls,
            "remap CPU-ownership failure releases acquired access without committing or retiring READY state");

    source->object_fds.clear();
    requested->object_fds.clear();
    close(fd);
    mock_sync_fd = -1;
    mock_failed_sync_flags = UINT64_MAX;
    mock_sync_calls.clear();
  }
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
    requested->experimental_submitted_at_ns = MonotonicNowNs();
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
    decode.live_h264_gap_tolerant = true;
    decode.live_h264_fast_path = true;
    decode.low_latency_h264 = true;
    decode.experimental_ready_remap = true;
    decode.experimental_ready_latest = true;
    decode.experimental_ready_delay_ns = kExperimentalDelay50Ns;
    decode.experimental_last_delivered_ordinal = 7;
    Surface target;
    target.experimental_write_lease = true;
    target.experimental_write_context = 7;
    target.experimental_write_generation = 1;
    target.experimental_write_submission_ordinal = 1;
    Require(OpenDecoderForSubmit(&decode, &target, 16, 16) ==
                    VA_STATUS_ERROR_OPERATION_FAILED &&
                decode.is_70012 && !decode.low_latency_h264 &&
                !decode.experimental_ready_remap &&
                !decode.experimental_ready_latest &&
                decode.experimental_ready_delay_ns == 0 &&
                decode.experimental_last_delivered_ordinal == 0 &&
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
  requested->experimental_write_submission_ordinal = 6;
  Require(!SelectExperimentalReady(*decode, *requested, now),
          "selection rejects drift greater than one submission");
  decode->experimental_ready_latest = true;
  decode->experimental_ready_delay_ns = kExperimentalDelay16Ns;
  requested->experimental_submitted_at_ns = now;
  Require(!SelectExperimentalReady(*decode, *requested,
                                   now + kExperimentalDelay16Ns - 1),
          "latest-ready selection waits for the configured delay threshold");
  Require(SelectExperimentalReady(*decode, *requested,
                                  now + kExperimentalDelay16Ns) ==
              pictures[3],
          "latest-ready selects the newest eligible ordinal at the threshold");
  decode->experimental_last_delivered_ordinal = 4;
  Require(!SelectExperimentalReady(*decode, *requested,
                                   now + kExperimentalDelay16Ns),
          "latest-ready cannot move behind the last delivered identity");
  decode->experimental_last_delivered_ordinal = 0;
  decode->experimental_ready_latest = false;
  decode->experimental_ready_delay_ns = 0;

  requested->id = 9;
  requested->trace_submission_ordinal = 5;
  requested->experimental_write_submission_ordinal = 5;
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

static void ExperimentalLatestReadyCommitOrder() {
  TraceEnvironment environment;
  const int trace_fd =
      memfd_create("crystalhd-latest-ready-trace", MFD_CLOEXEC);
  Require(trace_fd >= 0, "allocate latest-ready trace sink");
  {
    Driver driver{-1, trace_fd};
    auto decode = std::make_shared<DecodeContext>();
    decode->id = 5;
    decode->generation = 2;
    decode->experimental_ready_remap = true;
    decode->experimental_ready_latest = true;
    driver.contexts[5] = decode;
    const auto candidate = [&](uint64_t ordinal) {
      auto picture = std::make_shared<Surface>();
      Require(picture->AllocateInternal(nullptr, -1, 16, 16,
                                        VA_FOURCC_NV12),
              "allocate latest-ready candidate");
      picture->expected_timestamp = picture->frame_timestamp =
          ordinal * 100000;
      picture->ready = true;
      picture->decode_identity = picture->trace_decode_identity =
          100 + ordinal;
      picture->trace_context = 5;
      picture->trace_generation = 2;
      picture->trace_submission_ordinal = ordinal;
      memset(picture->planes[0], static_cast<int>(ordinal),
             picture->storage.size());
      decode->decoded_frames[picture->expected_timestamp] = picture;
      QueueExperimentalReady(&driver, decode.get(), picture);
      return picture;
    };
    const auto first = candidate(1);
    const auto third = candidate(3);
    const auto second = candidate(2);

    auto requested = std::make_shared<Surface>();
    Require(requested->AllocateInternal(nullptr, -1, 16, 16,
                                        VA_FOURCC_NV12),
            "allocate latest-ready request");
    requested->id = 5;
    requested->expected_timestamp = 500000;
    requested->decode_identity = requested->trace_decode_identity = 205;
    requested->trace_context = 5;
    requested->trace_generation = 2;
    requested->trace_submission_ordinal = 5;
    requested->experimental_write_lease = true;
    requested->experimental_write_context = 5;
    requested->experimental_write_generation = 2;
    requested->experimental_write_submission_ordinal = 5;
    requested->experimental_submitted_at_ns = MonotonicNowNs();
    driver.surfaces[5] = requested;

    bool committed = false;
    Require(TryExperimentalPresentation(&driver, requested, &committed) ==
                    VA_STATUS_SUCCESS &&
                committed &&
                ExperimentalPresentationReadable(&driver, requested.get()) &&
                requested->presentation_payload_submission_ordinal == 3 &&
                requested->planes[0][0] == 3 &&
                decode->experimental_last_delivered_ordinal == 3 &&
                decode->experimental_ready.empty() &&
                decode->decoded_frames.empty() &&
                !first->experimental_ready_queued &&
                !second->experimental_ready_queued &&
                !third->experimental_ready_queued,
            "latest-ready commits the newest ordinal and retires older READY state");

    const auto fourth = candidate(4);
    const auto sixth = candidate(6);
    const auto fifth = candidate(5);
    auto exact = std::make_shared<Surface>();
    exact->expected_timestamp = exact->frame_timestamp = 700000;
    exact->ready = true;
    exact->decode_identity = exact->trace_decode_identity = 107;
    exact->trace_context = 5;
    exact->trace_generation = 2;
    exact->trace_submission_ordinal = 7;
    ConsumeExperimentalExact(&driver, exact);
    Require(decode->experimental_last_delivered_ordinal == 7 &&
                decode->experimental_ready.empty() &&
                decode->decoded_frames.empty() &&
                !fourth->experimental_ready_queued &&
                !fifth->experimental_ready_queued &&
                !sixth->experimental_ready_queued,
            "exact delivery advances the watermark and retires every older READY candidate");

    const auto late = candidate(2);
    Require(!late->experimental_ready_queued &&
                decode->experimental_ready.empty() &&
                decode->decoded_frames.empty(),
            "late output behind the delivered watermark cannot re-enter or leak READY state");

    const auto eighth = candidate(8);
    auto out_of_order_exact = std::make_shared<Surface>();
    out_of_order_exact->expected_timestamp =
        out_of_order_exact->frame_timestamp = 600000;
    out_of_order_exact->ready = true;
    out_of_order_exact->decode_identity =
        out_of_order_exact->trace_decode_identity = 106;
    out_of_order_exact->trace_context = 5;
    out_of_order_exact->trace_generation = 2;
    out_of_order_exact->trace_submission_ordinal = 6;
    ConsumeExperimentalExact(&driver, out_of_order_exact);
    Require(decode->experimental_last_delivered_ordinal == 7 &&
                decode->experimental_ready.size() == 1 &&
                decode->experimental_ready.front() == eighth &&
                eighth->experimental_ready_queued &&
                decode->decoded_frames.count(eighth->expected_timestamp) == 1,
            "out-of-order exact sync cannot move the watermark backward or retire newer READY state");
    auto ninth = std::make_shared<Surface>();
    ninth->expected_timestamp = ninth->frame_timestamp = 900000;
    ninth->ready = true;
    ninth->decode_identity = ninth->trace_decode_identity = 109;
    ninth->trace_context = 5;
    ninth->trace_generation = 2;
    ninth->trace_submission_ordinal = 9;
    ConsumeExperimentalExact(&driver, ninth);
    Require(decode->experimental_last_delivered_ordinal == 9 &&
                decode->experimental_ready.empty() &&
                decode->decoded_frames.empty() &&
                !eighth->experimental_ready_queued,
            "a later exact sync advances the watermark and retires intervening READY state");

    auto delayed = std::make_shared<DecodeContext>();
    delayed->id = 6;
    delayed->generation = 1;
    delayed->experimental_ready_remap = true;
    delayed->experimental_ready_latest = true;
    delayed->experimental_ready_delay_ns = kExperimentalDelay16Ns;
    driver.contexts[6] = delayed;
    auto delayed_payload = std::make_shared<Surface>();
    auto delayed_request = std::make_shared<Surface>();
    for (const auto &surface : {delayed_payload, delayed_request})
      Require(surface->AllocateInternal(nullptr, -1, 16, 16,
                                        VA_FOURCC_NV12),
              "allocate delayed latest-ready state");
    delayed_payload->expected_timestamp =
        delayed_payload->frame_timestamp = 100000;
    delayed_payload->ready = true;
    delayed_payload->decode_identity =
        delayed_payload->trace_decode_identity = 301;
    delayed_payload->trace_context = 6;
    delayed_payload->trace_generation = 1;
    delayed_payload->trace_submission_ordinal = 1;
    delayed->decoded_frames[100000] = delayed_payload;
    QueueExperimentalReady(&driver, delayed.get(), delayed_payload);
    delayed_request->id = 6;
    delayed_request->expected_timestamp = 300000;
    delayed_request->decode_identity =
        delayed_request->trace_decode_identity = 303;
    delayed_request->trace_context = 6;
    delayed_request->trace_generation = 1;
    delayed_request->trace_submission_ordinal = 3;
    delayed_request->experimental_write_lease = true;
    delayed_request->experimental_write_context = 6;
    delayed_request->experimental_write_generation = 1;
    delayed_request->experimental_write_submission_ordinal = 3;
    delayed_request->experimental_submitted_at_ns =
        MonotonicNowNs() + kExperimentalDelay16Ns;
    driver.surfaces[6] = delayed_request;
    committed = true;
    Require(TryExperimentalPresentation(&driver, delayed_request,
                                        &committed) == VA_STATUS_SUCCESS &&
                !committed &&
                !ExperimentalPresentationReadable(&driver,
                                                  delayed_request.get()),
            "real commit path rejects a request before its delay threshold");
    delayed_request->experimental_submitted_at_ns =
        MonotonicNowNs() - kExperimentalDelay16Ns;
    Require(TryExperimentalPresentation(&driver, delayed_request,
                                        &committed) == VA_STATUS_SUCCESS &&
                committed &&
                ExperimentalPresentationReadable(&driver,
                                                  delayed_request.get()) &&
                delayed_request->presentation_payload_submission_ordinal == 1,
            "real commit path accepts latest-ready drift after the delay threshold");
  }
  const std::string trace = ReadTraceFd(trace_fd);
  close(trace_fd);
  const std::string commit = FindTraceEvent(trace, "presentation_commit");
  Require(TraceFieldIs(commit, "submission_ordinal", 5) &&
              TraceFieldIs(commit, "payload_submission_ordinal", 3) &&
              TraceSignedFieldIs(commit, "submission_drift", 2),
          "latest-ready trace records the requested-versus-payload drift");
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

static void FiniteOutputUsesDecodedPublicWritePurpose() {
  for (unsigned int mode : {0U, 1U, 2U}) {
    OutputFixture fixture;
    Require(!fixture.decoder.live_h264_fast_path &&
                !fixture.private_frame->direct_backing,
            "the materialization control starts on the finite private-picture path");
    mock_sync_fd = memfd_create("crystalhd-finite-copy-sync", MFD_CLOEXEC);
    Require(mock_sync_fd >= 0, "allocate the isolated public sync descriptor");
    fixture.public_frame->object_fds.push_back(mock_sync_fd);
    mock_failed_sync_flags = UINT64_MAX;
    mock_sync_calls.clear();
    mock_decoded_domain_calls.clear();
    // Mode 1 models allocation-time confirmation of an owned WC mapping.
    // Default-off and imported mappings must retain generic synchronization.
    fixture.public_frame->experimental_i915_wc = mode == 1;
    fixture.public_frame->owned_i915_dumb = mode != 2;
    fixture.public_frame->dumb_drm_fd = mode != 2 ? 700001 : -1;
    fixture.public_frame->dumb_handle = mode != 2 ? 61 : 0;
    mock_decoded_domain_fd = fixture.public_frame->dumb_drm_fd;
    mock_decoded_domain_handle = fixture.public_frame->dumb_handle;
    struct ResetDomainFixture {
      Surface *target;
      ~ResetDomainFixture() {
        // These domain descriptors are protocol sentinels, not OS handles.
        target->dumb_drm_fd = -1;
        target->dumb_handle = 0;
        mock_decoded_domain_fd = -1;
        mock_decoded_domain_handle = 0;
        mock_sync_fd = -1;
      }
    } reset{fixture.public_frame.get()};
    Require(fixture.Process() == VA_STATUS_SUCCESS &&
                fixture.private_frame->ready && fixture.public_frame->ready &&
                !fixture.public_frame->failed &&
                fixture.private_frame->frame_timestamp == 1 &&
                fixture.public_frame->frame_timestamp == 1 &&
                fixture.private_frame->planes[0][0] == 40 &&
                fixture.private_frame->storage == fixture.public_frame->storage &&
                fixture.private_frame->storage.data() != fixture.public_frame->storage.data() &&
                fixture.decoder.pending.empty() &&
                fixture.decoder.replay.outstanding() == 0,
            "finite materialization preserves separate immutable pixels, identity and completion");
    if (mode == 1) {
      Require(mock_decoded_domain_calls ==
                  std::vector<std::pair<uint32_t, uint32_t>>{
                      {I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC},
                      {I915_GEM_DOMAIN_GTT, 0}} && mock_sync_calls.empty(),
              "the finite public materialization call selects decoded WC/GTT ownership");
    } else {
      Require(mock_decoded_domain_calls.empty() &&
                  mock_sync_calls == std::vector<uint64_t>{
                      DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE,
                      DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE},
              "finite default-off and imported public targets keep DMA-BUF ownership");
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

static void ProtectedH264BufferCannotCommitEarlierParameters() {
  const VABufferType types[] = {
      VAProtectedSliceDataBufferType,
#if VA_CHECK_VERSION(1, 11, 0)
      VAProtectedSessionExecuteBufferType, VAEncryptionParameterBufferType,
#endif
  };
  for (VABufferType type : types) {
    TeardownFixture fixture(0);
    fixture.decoder->config = 1;
    fixture.decoder->profile = VAProfileH264Main;
    fixture.decoder->height = 32;
    fixture.decoder->auto_h264_prefeed = false;
    fixture.driver.configs[1] = {VAProfileH264Main, VAEntrypointVLD};
    auto target = std::make_shared<Surface>();
    Require(target->AllocateInternal(nullptr, -1, 16, 32, VA_FOURCC_NV12),
            "allocate private protected-buffer rejection target");
    memset(target->storage.data(), 99, target->storage.size());
    fixture.driver.surfaces[1] = target;
    const auto pixels = target->storage;
    const auto prepare = [&] {
      Require(BeginPicture(&fixture.context, 1, 1) == VA_STATUS_SUCCESS,
              "begin ordinary H.264 transaction");
      auto &decode = *fixture.decoder;
      decode.have_picture = true;
      decode.picture = {};
      decode.picture.picture_height_in_mbs_minus1 = 1;
      decode.picture.num_ref_frames = 1;
      decode.picture.seq_fields.bits.chroma_format_idc = 1;
      decode.picture.seq_fields.bits.mb_adaptive_frame_field_flag = 1;
      decode.picture.seq_fields.bits.direct_8x8_inference_flag = 1;
      decode.picture.pic_fields.bits.deblocking_filter_control_present_flag = 1;
      VASliceParameterBufferH264 slice = {};
      slice.slice_type = 2;
      decode.slices.push_back(slice);
      decode.slice_data.push_back({0x65, 0x88, 0x84});
    };
    prepare();
    Buffer unsupported;
    unsupported.context = 1;
    unsupported.type = type;
    unsupported.element_size = unsupported.elements = 1;
    unsupported.data = {1};
    fixture.driver.buffers[17] = unsupported;  // Bypass CreateBuffer deliberately.
    VABufferID id = 17;
    Require(RenderPicture(&fixture.context, 1, &id, 1) ==
                VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE &&
                fixture.decoder->h264_picture_invalid,
            "already-constructed protected H.264 buffer invalidates the pending picture");
    Require(EndPicture(&fixture.context, 1) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                fixture.decoder->target == VA_INVALID_SURFACE &&
                fixture.io.inputs.empty() && fixture.io.open_attempts == 0 &&
                fixture.decoder->replay.cached_pictures() == 0 && target->storage == pixels,
            "ignored Render error cannot submit or expose previously accepted parameters");
    prepare();
    fixture.driver.buffers.at(17).type = VAEncCodedBufferType;
    Require(RenderPicture(&fixture.context, 1, &id, 1) == VA_STATUS_SUCCESS &&
                !fixture.decoder->h264_picture_invalid &&
                EndPicture(&fixture.context, 1) == VA_STATUS_SUCCESS &&
                fixture.io.inputs == std::vector<uint64_t>{kTimestampStep} && !fixture.io.invalid,
            "fresh clear Begin recovers while unrelated buffer handling remains unchanged");
  }
}

static void RetryableBusySurvivesContextSurfaceAndDriverTeardown() {
  {
    TeardownFixture fixture(1);
    fixture.io.events.reserve(32);
    fixture.io.device_close_busy = 1;
    Require(DestroyContextWithTimeout(&fixture.context, 1, 0) ==
                    VA_STATUS_ERROR_HW_BUSY &&
                fixture.driver.contexts.count(1) == 1 &&
                fixture.decoder->destroy_prepared && fixture.decoder->retired &&
                fixture.decoder->device == fixture.io.handle(),
            "context destroy retains a non-consumed device for an explicit retry");
    const auto seal_count =
        std::count(fixture.io.events.begin(), fixture.io.events.end(), "seal");
    const auto eos_count =
        std::count(fixture.io.events.begin(), fixture.io.events.end(), "eos");
    const auto release_count =
        std::count(fixture.io.events.begin(), fixture.io.events.end(), "release");
    Require(DestroyContextWithTimeout(&fixture.context, 1, 0) ==
                    VA_STATUS_SUCCESS &&
                fixture.driver.contexts.count(1) == 0 &&
                fixture.decoder->device == nullptr &&
                std::count(fixture.io.events.begin(), fixture.io.events.end(),
                           "seal") == seal_count &&
                std::count(fixture.io.events.begin(), fixture.io.events.end(),
                           "eos") == eos_count &&
                std::count(fixture.io.events.begin(), fixture.io.events.end(),
                           "release") == release_count &&
                !fixture.io.invalid,
            "context retry closes once without repeating its completed drain");
  }

  {
    TeardownFixture fixture(1);
    fixture.io.events.reserve(32);
    fixture.io.early_eos = true;
    fixture.io.device_close_busy = 1;
    Require(DestroyContextWithTimeout(&fixture.context, 1, 0) ==
                    VA_STATUS_ERROR_HW_BUSY &&
                fixture.driver.contexts.count(1) == 1 &&
                fixture.decoder->device == fixture.io.handle(),
            "live ownership takes priority while a failed drain still has a BUSY handle");
    Require(DestroyContextWithTimeout(&fixture.context, 1, 0) ==
                    VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.driver.contexts.count(1) == 0 &&
                fixture.decoder->device == nullptr,
            "terminal retry restores the original drain failure after consuming ownership");
  }

  {
    TeardownFixture fixture(1);
    fixture.io.events.reserve(16);
    fixture.io.stop_status = BC_STS_BUSY;
    fixture.io.device_close_busy = 1;
    const uint64_t generation = fixture.decoder->generation;
    VASurfaceID surface = 1;
    Require(DestroySurfacesWithTimeout(&fixture.context, &surface, 1, 0) ==
                    VA_STATUS_ERROR_HW_BUSY &&
                fixture.driver.surfaces.count(1) == 0 &&
                fixture.decoder->generation == generation + 1 &&
                fixture.decoder->device == fixture.io.handle() &&
                fixture.decoder->decoder_started &&
                fixture.decoder->hardware_close_pending &&
                fixture.io.open_attempts == 0,
            "surface-pool reset reports its retained handle after one generation retirement");
    fixture.io.stop_status = BC_STS_SUCCESS;
    fixture.io.allow_device_open = true;
    Require(OpenDecoder(&*fixture.decoder, 16, 16, nullptr, 0) ==
                    VA_STATUS_SUCCESS &&
                fixture.decoder->device == fixture.io.handle() &&
                fixture.io.open_attempts == 1 && !fixture.io.invalid,
            "the next open consumes a reset handle before acquiring a replacement");
    Require(fixture.decoder->CloseHardware(0) == BC_STS_SUCCESS,
            "surface-reset reopen leaves no live test device");
  }

  {
    TeardownFixture fixture(1);
    fixture.io.events.reserve(16);
    fixture.io.device_close_busy = 1;
    VASurfaceID surface = 1;
    Require(DestroySurfacesWithTimeout(&fixture.context, &surface, 1, 0) ==
                    VA_STATUS_ERROR_HW_BUSY &&
                fixture.decoder->hardware_close_pending &&
                fixture.decoder->pending.empty() &&
                !fixture.decoder->ReplaySealed(),
            "surface reset can retain only terminal hardware ownership");
    Require(DestroyContextWithTimeout(&fixture.context, 1, 0) ==
                    VA_STATUS_SUCCESS &&
                fixture.driver.contexts.count(1) == 0 &&
                fixture.decoder->device == nullptr && !fixture.io.invalid,
            "context destroy consumes ownership without inventing a decode failure");
  }

  {
    DecodeIoMock io;
    io.events.reserve(8);
    io.device_close_busy = 1;
    MockDecodeScope scope(io);
    auto *driver = new Driver(-1);
    auto decoder = std::make_shared<DecodeContext>();
    decoder->device = io.handle();
    driver->contexts[1] = decoder;
    VADriverContext context = {};
    context.pDriverData = driver;
    Require(Terminate(&context) == VA_STATUS_SUCCESS &&
                context.pDriverData == nullptr && decoder->device == nullptr &&
                io.device_close_calls == 2 && !io.invalid,
            "one public termination callback joins workers and consumes BUSY ownership before unload");
  }
}

static void ReadyStatusProbeSkipsReadyHeadPeek() {
  {
    TeardownFixture fixture(1);
    Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
                    VA_STATUS_SUCCESS &&
                fixture.io.status_requests ==
                    std::vector<uint32_t>{kStatusSkipReadyHeadPeek} &&
                fixture.decoder->pending.size() == 1 &&
                !fixture.held[0]->ready && fixture.io.events.empty() &&
                !fixture.io.invalid,
            "empty READY probe skips the DMA peek without changing decode state");
  }

  {
    TeardownFixture fixture(1);
    Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                    VA_STATUS_SUCCESS &&
                fixture.decoder->replay.Seal(),
            "prepare one sealed picture for no-peek READY/EOS polling");
    fixture.io.outputs.push_back(kTimestampStep);
    fixture.io.outputs.push_back(0);
    Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
                    VA_STATUS_SUCCESS &&
                fixture.io.status_requests == std::vector<uint32_t>(
                    3, kStatusSkipReadyHeadPeek) &&
                fixture.io.outputs.empty() && fixture.held[0]->ready &&
                fixture.decoder->pending.empty() &&
                fixture.decoder->replay.outstanding() == 0 &&
                !fixture.decoder->ReplaySealed() &&
                std::count(fixture.io.events.begin(), fixture.io.events.end(),
                           "picture") == 1 &&
                std::count(fixture.io.events.begin(), fixture.io.events.end(),
                           "eos") == 1 &&
                std::count(fixture.io.events.begin(), fixture.io.events.end(),
                           "release") == 2 &&
                !fixture.io.invalid,
            "no-peek READY counts still drain the exact picture and EOS marker");
  }

  {
    TeardownFixture fixture(1);
    fixture.io.fail_poll = true;
    Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
                    VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.io.status_requests ==
                    std::vector<uint32_t>{kStatusSkipReadyHeadPeek} &&
                fixture.decoder->pending.size() == 1 &&
                !fixture.held[0]->ready && !fixture.held[0]->failed &&
                fixture.io.events.empty() && !fixture.io.invalid,
            "no-peek status request preserves the poll failure path");
  }

  {
    TeardownFixture fixture(1);
    fixture.decoder->is_70012 = true;
    Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
                    VA_STATUS_SUCCESS &&
                fixture.io.status_requests == std::vector<uint32_t>{0} &&
                fixture.decoder->pending.size() == 1 &&
                !fixture.held[0]->ready && fixture.io.events.empty() &&
                !fixture.io.invalid,
            "BCM70012 READY probe preserves the legacy filtering peek");
  }
}

static void ConcurrentSyncObservesExperimentalPresentation() {
  for (bool callback_commits : {false, true}) {
    TeardownFixture fixture(2);
    fixture.decoder->id = 1;
    fixture.decoder->generation = 1;
    fixture.decoder->experimental_ready_remap = true;
    fixture.decoder->live_h264 = true;
    fixture.decoder->live_h264_gap_tolerant = true;
    fixture.decoder->live_h264_fast_path = true;
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
    requested->experimental_submitted_at_ns = MonotonicNowNs();
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
    fixture.decoder->live_h264_gap_tolerant = true;
    fixture.decoder->live_h264_fast_path = true;
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
    requested->experimental_submitted_at_ns = MonotonicNowNs();

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
  fixture.decoder->experimental_ready_delay_ns = kExperimentalDelay50Ns;
  fixture.decoder->live_h264 = true;
  fixture.decoder->live_h264_gap_tolerant = true;
  fixture.decoder->live_h264_fast_path = true;
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
  requested->experimental_submitted_at_ns = MonotonicNowNs();

  auto replacement = std::make_shared<DecodeContext>();
  replacement->id = 2;
  replacement->generation = 1;
  replacement->width = replacement->height = 16;
  replacement->device = fixture.io.handle();
  replacement->decoder_open = replacement->decoder_started = true;
  replacement->experimental_ready_remap = true;
  replacement->experimental_ready_delay_ns = kExperimentalDelay50Ns;
  replacement->live_h264 = true;
  replacement->live_h264_gap_tolerant = true;
  replacement->live_h264_fast_path = true;
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
    requested->experimental_submitted_at_ns = MonotonicNowNs();
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

static void ExperimentalDelayedSyncThreshold() {
  {
    TeardownFixture fixture(3);
    fixture.decoder->id = 1;
    fixture.decoder->generation = 1;
    fixture.decoder->experimental_ready_remap = true;
    fixture.decoder->experimental_ready_latest = true;
    fixture.decoder->experimental_ready_delay_ns = kExperimentalDelay16Ns;
    fixture.decoder->live_h264 = true;
    fixture.decoder->live_h264_gap_tolerant = true;
    fixture.decoder->live_h264_fast_path = true;
    fixture.decoder->low_latency_h264 = true;

    const auto payload =
        fixture.decoder->decoded_frames.at(kTimestampStep);
    const auto requested_picture =
        fixture.decoder->decoded_frames.at(3 * kTimestampStep);
    const auto requested = fixture.held[2];
    payload->ready = true;
    payload->frame_timestamp = payload->expected_timestamp;
    payload->decode_identity = payload->trace_decode_identity = 401;
    payload->trace_context = 1;
    payload->trace_generation = 1;
    payload->trace_submission_ordinal = 1;
    memset(payload->planes[0], 61, payload->storage.size());
    requested_picture->decode_identity =
        requested_picture->trace_decode_identity = 403;
    requested_picture->trace_context = 1;
    requested_picture->trace_generation = 1;
    requested_picture->trace_submission_ordinal = 3;
    requested->id = 3;
    requested->decode_identity = requested->trace_decode_identity = 403;
    requested->trace_context = 1;
    requested->trace_generation = 1;
    requested->trace_submission_ordinal = 3;
    requested->decode_picture = requested_picture;
    requested->experimental_write_lease = true;
    requested->experimental_write_context = 1;
    requested->experimental_write_generation = 1;
    requested->experimental_write_submission_ordinal = 3;
    requested->experimental_submitted_at_ns = MonotonicNowNs();
    fixture.decoder->pending.erase(kTimestampStep);
    fixture.decoder->surface_timestamps.erase(fixture.held[0].get());
    QueueExperimentalReady(&fixture.driver, fixture.decoder.get(), payload);

    bool committed = true;
    Require(TryExperimentalPresentation(&fixture.driver, requested,
                                        &committed) == VA_STATUS_SUCCESS &&
                !committed && !requested->experimental_presentation,
            "public sync fixture cannot remap before the configured delay");
    bool observed_before_threshold = false;
    fixture.io.on_sleep = [&] {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      observed_before_threshold = !requested->experimental_presentation;
      requested->experimental_submitted_at_ns =
          MonotonicNowNs() - kExperimentalDelay16Ns;
    };
    Require(SyncSurface2(&fixture.context, 3, 100000000) ==
                    VA_STATUS_SUCCESS &&
                observed_before_threshold &&
                ExperimentalPresentationReadable(&fixture.driver,
                                                  requested.get()) &&
                requested->presentation_payload_submission_ordinal == 1 &&
                requested->planes[0][0] == 61,
            "finite public sync rechecks and commits latest READY only after the delay threshold");
  }

  for (bool latest : {false, true}) {
    TeardownFixture fixture(2);
    fixture.decoder->id = 1;
    fixture.decoder->generation = 1;
    fixture.decoder->experimental_ready_remap = true;
    fixture.decoder->experimental_ready_latest = latest;
    fixture.decoder->experimental_ready_delay_ns = 0;

    const auto payload =
        fixture.decoder->decoded_frames.at(kTimestampStep);
    const auto exact_picture =
        fixture.decoder->decoded_frames.at(2 * kTimestampStep);
    const auto exact = fixture.held[1];
    payload->ready = true;
    payload->frame_timestamp = payload->expected_timestamp;
    payload->decode_identity = payload->trace_decode_identity = 501;
    payload->trace_context = 1;
    payload->trace_generation = 1;
    payload->trace_submission_ordinal = 1;
    QueueExperimentalReady(&fixture.driver, fixture.decoder.get(), payload);
    exact_picture->decode_identity =
        exact_picture->trace_decode_identity = 502;
    exact_picture->trace_context = 1;
    exact_picture->trace_generation = 1;
    exact_picture->trace_submission_ordinal = 2;
    exact->frame_timestamp = exact->expected_timestamp;
    exact->decode_identity = exact->trace_decode_identity = 502;
    exact->trace_context = 1;
    exact->trace_generation = 1;
    exact->trace_submission_ordinal = 2;
    exact->experimental_write_lease = true;
    exact->experimental_write_context = 1;
    exact->experimental_write_generation = 1;
    exact->experimental_write_submission_ordinal = 2;
    exact->experimental_submitted_at_ns = MonotonicNowNs();

    Require(SelectExperimentalReady(*fixture.decoder, *exact,
                                    MonotonicNowNs()) == payload,
            "zero-delay remap candidate is eligible before the exact frame completes");
    exact->ready = true;

    Require(SyncSurface2(&fixture.context, 2, 100000000) ==
                    VA_STATUS_SUCCESS &&
                !exact->experimental_presentation &&
                fixture.decoder->experimental_last_delivered_ordinal == 2 &&
                fixture.decoder->experimental_ready.empty() &&
                fixture.decoder->decoded_frames.empty() &&
                !payload->experimental_ready_queued &&
                !exact_picture->experimental_ready_queued,
            "an exact completed frame wins immediately over an eligible zero-delay remap candidate for both policies");
  }

  {
    TeardownFixture fixture(1);
    fixture.decoder->id = 1;
    fixture.decoder->generation = 1;
    fixture.decoder->experimental_ready_remap = true;
    fixture.decoder->experimental_ready_latest = true;
    fixture.decoder->experimental_last_delivered_ordinal = 2;
    const auto picture =
        fixture.decoder->decoded_frames.at(kTimestampStep);
    const auto surface = fixture.held[0];
    picture->decode_identity = picture->trace_decode_identity = 601;
    picture->trace_context = 1;
    picture->trace_generation = 1;
    picture->trace_submission_ordinal = 1;
    surface->decode_identity = surface->trace_decode_identity = 601;
    surface->trace_context = 1;
    surface->trace_generation = 1;
    surface->trace_submission_ordinal = 1;
    surface->decode_picture = picture;
    surface->experimental_write_lease = true;
    surface->experimental_write_context = 1;
    surface->experimental_write_generation = 1;
    surface->experimental_write_submission_ordinal = 1;
    SendNext(&fixture.decoder->replay, kTimestampStep);
    std::vector<uint8_t> pixels(16 * 16 * 2, 128);
    BC_DTS_PROC_OUT output = {};
    output.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    output.PicInfo.timeStamp = kTimestampStep;
    output.PicInfo.width = output.PicInfo.height = 16;
    output.Ybuff = pixels.data();
    output.YBuffDoneSz = pixels.size() / 4;
    Require(ProcessDecodedOutput(&fixture.driver, fixture.decoder.get(),
                                 output) == VA_STATUS_SUCCESS &&
                surface->ready && picture->ready &&
                !picture->experimental_ready_queued &&
                fixture.decoder->experimental_ready.empty() &&
                fixture.decoder->decoded_frames.empty() &&
                fixture.decoder->experimental_last_delivered_ordinal == 2,
            "late hardware completion behind the watermark stays exact-only and releases private READY state");
  }

  {
    TeardownFixture fixture(1);
    fixture.decoder->id = 1;
    fixture.decoder->generation = 1;
    fixture.decoder->experimental_ready_remap = true;
    fixture.decoder->experimental_ready_latest = true;
    fixture.decoder->experimental_last_delivered_ordinal = 2;
    auto &map_only =
        fixture.decoder->decoded_frames.at(kTimestampStep);
    map_only->decode_identity = map_only->trace_decode_identity = 701;
    map_only->trace_context = 1;
    map_only->trace_generation = 1;
    map_only->trace_submission_ordinal = 1;
    fixture.driver.surfaces.erase(1);
    SendNext(&fixture.decoder->replay, kTimestampStep);
    std::vector<uint8_t> pixels(16 * 16 * 2, 128);
    BC_DTS_PROC_OUT output = {};
    output.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    output.PicInfo.timeStamp = kTimestampStep;
    output.PicInfo.width = output.PicInfo.height = 16;
    output.Ybuff = pixels.data();
    output.YBuffDoneSz = pixels.size() / 4;
    Require(ProcessDecodedOutput(&fixture.driver, fixture.decoder.get(),
                                 output) == VA_STATUS_SUCCESS &&
                fixture.decoder->experimental_ready.empty() &&
                fixture.decoder->decoded_frames.empty() &&
                fixture.decoder->pending.empty() &&
                fixture.decoder->experimental_last_delivered_ordinal == 2,
            "map-only late hardware completion is retired without retaining an invalidated map iterator");
  }
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
  fixture.decoder->live_h264_gap_tolerant = true;
  fixture.decoder->live_h264_fast_path = true;
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
                "strict state stays bounded and live state retires history");
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
  low_latency.live_h264_gap_tolerant = true;
  low_latency.live_h264_fast_path = true;
  low_latency.low_latency_h264 = true;
  low_latency.prefed_timestamp = 17 * kTimestampStep;
  Require(low_latency.Close() == BC_STS_SUCCESS &&
              low_latency.live_h264 && low_latency.live_h264_gap_tolerant &&
              low_latency.live_h264_fast_path &&
              low_latency.low_latency_h264 &&
              low_latency.prefed_timestamp == 0,
          "close preserves low-latency policy but discards its runtime prefix");
  low_latency.prefed_timestamp = 19 * kTimestampStep;
  low_latency.Reset();
  Require(low_latency.live_h264 && low_latency.live_h264_gap_tolerant &&
              low_latency.live_h264_fast_path &&
              low_latency.low_latency_h264 &&
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

static void AutomaticProgressivePrefeedUsesSubmissionSnapshot() {
  SavedEnvironment live("CRYSTALHD_VAAPI_LIVE_H264");
  SavedEnvironment low_latency("CRYSTALHD_VAAPI_LOW_LATENCY_H264");
  unsetenv("CRYSTALHD_VAAPI_LIVE_H264");
  for (const auto &policy : {std::pair<const char *, bool>{nullptr, false},
                              {"0", false}, {nullptr, true}}) {
    if (policy.first) setenv("CRYSTALHD_VAAPI_LOW_LATENCY_H264", policy.first, 1);
    else unsetenv("CRYSTALHD_VAAPI_LOW_LATENCY_H264");
    DecodeIoMock io;
    io.allow_device_open = true;
    MockDecodeScope scope(io);
    Driver driver{-1};
    driver.configs[1] = {VAProfileH264Main, VAEntrypointVLD};
    VADriverContext context = {};
    context.pDriverData = &driver;
    VAContextID id = VA_INVALID_ID;
    Require(CreateContext(&context, 1, 16, 32, 0, nullptr, 0, &id) ==
                VA_STATUS_SUCCESS, "create default or explicit-zero H.264 context");
    const auto decode = driver.contexts.at(id);
    const auto submit = [&](VASurfaceID target_id, bool idr, bool paff) {
      auto target = std::make_shared<Surface>();
      Require(target->AllocateInternal(nullptr, -1, 16, 32, VA_FOURCC_NV12),
              "allocate automatic-prefeed submission target");
      target->id = target_id;
      driver.surfaces[target_id] = target;
      Require(BeginPicture(&context, id, target_id) == VA_STATUS_SUCCESS,
              "begin automatic-prefeed production submission");
      decode->picture.picture_height_in_mbs_minus1 = 1;
      decode->picture.num_ref_frames = 1;
      decode->picture.seq_fields.bits.chroma_format_idc = 1;
      decode->picture.seq_fields.bits.frame_mbs_only_flag = !paff;
      decode->picture.seq_fields.bits.direct_8x8_inference_flag = 1;
      decode->have_picture = true;
      VASliceParameterBufferH264 slice = {};
      slice.slice_type = idr ? 2 : 0;
      decode->slices.push_back(slice);
      decode->slice_data.push_back({uint8_t(idr ? 0x65 : 0x41), 0x88, 0x84});
      return EndPicture(&context, id);
    };
    const bool automatic = policy.first == nullptr && !policy.second;
    io.input_busy = true;
    Require(submit(1, true, policy.second) == VA_STATUS_SUCCESS &&
                decode->replay.NextInput() &&
                decode->replay.NextInput()->prefeed == automatic &&
                io.input_calls.size() == 1 && !io.input_calls.front().accepted,
            "real SubmitPicture snapshots unset progressive eligibility before input backpressure");
    const auto original = decode->replay.NextInput()->bytes;
    decode->picture.seq_fields.bits.frame_mbs_only_flag = 0;
    decode->picture.seq_fields.bits.mb_adaptive_frame_field_flag = 0;
    io.input_busy = false;
    Require(!ShouldAutoPrefeedH264(*decode) &&
                PumpDecodeInput(&driver, decode.get()) == VA_STATUS_SUCCESS &&
                io.input_calls.size() == (automatic ? 3U : 2U) &&
                io.input_calls[1].accepted &&
                io.input_calls[1].bytes == original &&
                io.input_calls[1].timestamp == kTimestampStep &&
                decode->prefed_timestamp == (automatic ? 2 * kTimestampStep : 0) &&
                (!automatic || (io.input_calls[2].accepted &&
                    io.input_calls[2].timestamp == 2 * kTimestampStep &&
                    io.input_calls[2].bytes == H264LowLatencyPrefeed())),
            "accepted progressive AU keeps its prefeed snapshot after PAFF metadata arrives");
    Require(submit(2, false, true) == VA_STATUS_SUCCESS &&
                io.input_calls.back().timestamp == (automatic ? 0 : 2 * kTimestampStep) &&
                io.input_calls.back().bytes != H264LowLatencyPrefeed() &&
                io.inputs == std::vector<uint64_t>({kTimestampStep, 2 * kTimestampStep}) &&
                decode->prefed_timestamp == 0 && io.prefed_timestamp == 0 && !io.invalid,
            "actual PAFF successor consumes any existing prefix at timestamp zero without rearming");
    Require(decode->replay.Seal() &&
                decode->replay.Observe(kTimestampStep) == CrystalHDDecodeReplay::Output::New &&
                decode->replay.Observe(2 * kTimestampStep) == CrystalHDDecodeReplay::Output::New &&
                decode->replay.EndOfSequence() &&
                decode->replay.Append(3 * kTimestampStep, false, {3}) &&
                decode->replay.Restarted() &&
                decode->replay.NextInput()->bytes == original &&
                decode->replay.NextInput()->prefeed == automatic,
            "replay retains production-admitted progressive or opt-out eligibility byte-exactly");
    SendNext(&decode->replay, kTimestampStep);
    Require(!decode->replay.NextInput()->prefeed,
            "production-admitted PAFF successor retains its own no-prefeed snapshot");
  }
}

static void AutomaticH264PrefeedSnapshotsAcceptedSequence() {
  enum class Next { Continue, Ghost, Misaligned };
  for (Next next : {Next::Continue, Next::Ghost, Next::Misaligned}) {
    TeardownFixture fixture(2);
    auto &decode = *fixture.decoder;
    decode.profile = VAProfileH264Main;
    decode.live_h264 = decode.auto_h264_prefeed = true;
    decode.picture.seq_fields.bits.mb_adaptive_frame_field_flag = 1;
    decode.replay = CrystalHDDecodeReplay(true);
    Require(ShouldAutoPrefeedH264(decode),
            "finite frame-coded MBAFF on an opened BCM70015 is eligible");
    decode.picture.seq_fields.bits.frame_mbs_only_flag = 1;
    decode.picture.seq_fields.bits.mb_adaptive_frame_field_flag = 0;
    Require(ShouldAutoPrefeedH264(decode), "progressive frame input is automatic");
    decode.picture.seq_fields.bits.frame_mbs_only_flag = 0;
    decode.picture.seq_fields.bits.mb_adaptive_frame_field_flag = 0;
    Require(!ShouldAutoPrefeedH264(decode), "PAFF sequence remains outside automatic eligibility");
    decode.picture.seq_fields.bits.mb_adaptive_frame_field_flag = 1;
    decode.picture.pic_fields.bits.field_pic_flag = 1;
    Require(!ShouldAutoPrefeedH264(decode), "a coded field is not a frame AU");
    decode.picture.pic_fields.bits.field_pic_flag = 0;
    decode.live_h264_fast_path = true;
    Require(!ShouldAutoPrefeedH264(decode), "explicit live prefeed remains opt-in");
    decode.live_h264_fast_path = false;
    decode.is_70012 = true;
    Require(!ShouldAutoPrefeedH264(decode), "BCM70012 is not auto-prefed");
    decode.is_70012 = false;
    decode.decoder_started = false;
    Require(!ShouldAutoPrefeedH264(decode), "unopened hardware is not known eligible");
    decode.decoder_started = true;
    decode.device = nullptr;
    Require(!ShouldAutoPrefeedH264(decode), "a missing device cannot establish identity");
    decode.device = fixture.io.handle();
    decode.profile = VAProfileMPEG2Main;
    Require(!ShouldAutoPrefeedH264(decode), "other codecs cannot auto-prefeed");
    decode.profile = VAProfileH264Main;
    decode.auto_h264_prefeed = false;
    Require(!ShouldAutoPrefeedH264(decode), "explicit policy opt-out is honored");
    decode.auto_h264_prefeed = true;

    const auto first = TestH264AccessUnit(true, 1);
    Require(decode.replay.Append(kTimestampStep, true, first,
                                  ShouldAutoPrefeedH264(decode)),
            "snapshot automatic eligibility with the accepted original AU");
    decode.picture.seq_fields.bits.frame_mbs_only_flag = 0;
    decode.picture.seq_fields.bits.mb_adaptive_frame_field_flag = 0;
    decode.next_timestamp = 2 * kTimestampStep;
    Require(!ShouldAutoPrefeedH264(decode) && !decode.low_latency_h264 &&
                decode.replay.NextInput()->prefeed &&
                PumpDecodeInput(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                fixture.io.input_calls.size() == 2 &&
                fixture.io.input_calls[0].bytes == first &&
                fixture.io.input_calls[1].bytes == H264LowLatencyPrefeed() &&
                decode.prefed_timestamp == 2 * kTimestampStep,
            "later PAFF metadata cannot change an accepted MBAFF AU");
    if (next == Next::Ghost) {
      fixture.io.outputs.push_back(2 * kTimestampStep);
      Require(ReceiveAvailable(&fixture.driver, &decode) ==
                      VA_STATUS_ERROR_DECODING_ERROR && decode.replay.failed() &&
                  !fixture.held[1]->ready && !fixture.io.invalid,
              "ghost-prefix guard remains active after automatic eligibility ends");
      continue;
    }
    const uint64_t token =
        (next == Next::Misaligned ? 3 : 2) * kTimestampStep;
    const auto second = TestH264AccessUnit(false, 2);
    const auto owner = decode.decoded_frames.at(2 * kTimestampStep);
    Require(decode.replay.Append(token, false, second,
                                  ShouldAutoPrefeedH264(decode)),
            "accept a successor under the PAFF transport policy");
    decode.next_timestamp = token + kTimestampStep;
    const VAStatus status = PumpDecodeInput(&fixture.driver, &decode);
    if (next == Next::Misaligned) {
      Require(status == VA_STATUS_ERROR_DECODING_ERROR && decode.replay.failed() &&
                  fixture.io.input_calls.size() == 2 && !fixture.io.invalid,
              "automatic policy change cannot bypass the held-prefix alignment guard");
      continue;
    }
    Require(status == VA_STATUS_SUCCESS && fixture.io.input_calls.size() == 3 &&
                fixture.io.input_calls[2].accepted &&
                fixture.io.input_calls[2].timestamp == 0 &&
                fixture.io.input_calls[2].bytes == second &&
                fixture.io.inputs == std::vector<uint64_t>(
                    {kTimestampStep, 2 * kTimestampStep}) &&
                decode.decoded_frames.at(2 * kTimestampStep) == owner &&
                decode.prefed_timestamp == 0 && fixture.io.prefed_timestamp == 0 &&
                !fixture.io.invalid,
            "PAFF successor consumes existing prefix byte-exactly without rearming");
    Require(decode.replay.Seal() &&
                decode.replay.Observe(kTimestampStep) ==
                    CrystalHDDecodeReplay::Output::New &&
                decode.replay.Observe(2 * kTimestampStep) ==
                    CrystalHDDecodeReplay::Output::New &&
                decode.replay.EndOfSequence() &&
                decode.replay.Append(3 * kTimestampStep, false, {3}) &&
                decode.replay.Restarted() && decode.replay.NextInput()->prefeed,
            "replay retains the accepted MBAFF eligibility after sequence changes");
    SendNext(&decode.replay, kTimestampStep);
    Require(!decode.replay.NextInput()->prefeed,
            "replay also retains the PAFF successor's no-prefeed decision");
  }
}

static void LiveH264SpeculativePrefeedSequence() {
  TeardownFixture fixture(3);
  fixture.decoder->live_h264 = true;
  fixture.decoder->live_h264_gap_tolerant = true;
  fixture.decoder->live_h264_fast_path = true;
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

static void LiveH264InterlacedPrefeedKeepsExactFieldOwnership(
    bool finite_replay_only = false, unsigned capability_mode = 1) {
  enum class Tail { Complete, Ghost, Flush, FiniteReplay, ContextDrain };
  for (bool bottom_first : {false, true}) {
    for (Tail tail : {Tail::Complete, Tail::Ghost, Tail::Flush,
                      Tail::FiniteReplay, Tail::ContextDrain}) {
      if (finite_replay_only && tail != Tail::FiniteReplay)
        continue;
      TeardownFixture fixture(5);
      if (tail == Tail::FiniteReplay) {
        Require(capability_mode < 4, "known library capability test mode");
        fixture.io.library_capabilities = capability_mode == 0 ? 0 :
            DTS_LIB_CAP_FLEA_H264_DRAINED_REOPEN;
        if (capability_mode == 2)
          fixture.io.library_capabilities_status = BC_STS_ERROR;
        if (capability_mode == 3)
          crystalhd_test_library_capabilities = nullptr;
      }
      auto &decode = *fixture.decoder;
      decode.id = 1;
      decode.generation = 3;
      decode.profile = VAProfileH264Main;
      decode.height = fixture.io.output_height = 32;
      decode.live_h264 = decode.live_h264_gap_tolerant = true;
      decode.live_h264_fast_path = decode.low_latency_h264 = true;
      if (tail == Tail::FiniteReplay)
        decode.live_h264_gap_tolerant = decode.live_h264_fast_path = false;
      decode.replay = CrystalHDDecodeReplay(true);
      for (size_t index = 0; index < fixture.held.size(); ++index) {
        const uint64_t token = (index + 1) * kTimestampStep;
        const auto picture = decode.decoded_frames.at(token);
        for (const auto &frame : {fixture.held[index], picture}) {
          Require(frame->AllocateInternal(nullptr, -1, 16, 32,
                                          VA_FOURCC_NV12),
                  "allocate interlaced prefeed picture");
          memset(frame->storage.data(), 99, frame->storage.size());
          frame->expected_timestamp = token;
          frame->decode_identity = frame->trace_decode_identity = index + 1;
          frame->trace_context = decode.id;
          frame->trace_generation = decode.generation;
          frame->trace_submission_ordinal = index + 1;
          frame->h264_crop_unit_y = 4;
        }
        fixture.held[index]->decode_picture = picture;
      }
      const uint64_t fourth = 4 * kTimestampStep;
      const uint64_t fifth = 5 * kTimestampStep;
      const auto fourth_owner = decode.decoded_frames.at(fourth);
      const auto fifth_owner = decode.decoded_frames.at(fifth);
      decode.decoded_frames.erase(fifth);
      decode.pending.erase(fifth);
      decode.surface_timestamps.erase(fixture.held[4].get());
      fixture.held[4]->decode_picture.reset();
      const uint32_t order = bottom_first ? VDEC_FLAG_BOTTOM_FIRST : 0;
      const uint32_t first = bottom_first ? VDEC_FLAG_BOTTOMFIELD
                                          : VDEC_FLAG_TOPFIELD;
      const uint32_t second = bottom_first ? VDEC_FLAG_TOPFIELD
                                           : VDEC_FLAG_BOTTOMFIELD;
      const auto queue_field = [&](uint64_t token, uint32_t parity) {
        fixture.io.outputs.push_back(token);
        fixture.io.picture_flags.push_back(
            VDEC_FLAG_INTERLACED_SRC | parity | order);
      };
      for (unsigned int picture = 1; picture <= 4; ++picture) {
        const uint64_t token = picture * kTimestampStep;
        Require(decode.replay.Append(token, picture == 1,
                                      TestH264AccessUnit(picture == 1, picture)),
                "accept an actual interlaced AU before its successor prefeed");
        decode.next_timestamp = (picture + 1) * kTimestampStep;
        Require(PumpDecodeInput(&fixture.driver, &decode) == VA_STATUS_SUCCESS,
                "submit interlaced AU and prefeed exactly its successor");
        queue_field(token, first);
        if (picture != 4)
          queue_field(token, second);
        Require(ReceiveAvailable(&fixture.driver, &decode) == VA_STATUS_SUCCESS,
                "receive complete prefix and the final first field");
        if (picture != 4)
          fixture.CheckPicture(picture - 1);
      }
      const auto untouched = fixture.held[3]->storage;
      Require(fixture.io.input_calls.size() == 8 &&
                  fixture.io.input_calls.back().timestamp == fifth &&
                  fixture.io.input_calls.back().bytes == H264LowLatencyPrefeed() &&
                  decode.prefed_timestamp == fifth &&
                  fixture.io.prefed_timestamp == fifth &&
                  decode.h264_first_field.valid &&
                  decode.h264_first_field.timestamp == fourth &&
                  decode.h264_first_field.picture == fourth_owner &&
                  decode.pending.count(fourth) == 1 &&
                  decode.pending.count(fifth) == 0 &&
                  decode.decoded_frames.count(fifth) == 0 &&
                  decode.replay.outstanding() == 1 &&
                  !fourth_owner->ready && !fixture.held[3]->ready &&
                  !fixture.held[4]->ready,
              "token-five prefeed cannot complete half of token four or own a picture");
      Require(PumpDecodeInput(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                  ReceiveAvailable(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                  fixture.io.input_calls.size() == 8 &&
                  fixture.held[3]->storage == untouched &&
                  !fixture.held[3]->ready && decode.h264_first_field.valid,
              "empty transport polls cannot turn the held half into READY");

      if (tail == Tail::FiniteReplay || tail == Tail::ContextDrain) {
        if (tail == Tail::ContextDrain) {
          fixture.io.on_sleep = [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(140));
            fixture.io.on_sleep = [&] {
              Require(std::find(fixture.io.events.begin(), fixture.io.events.end(),
                                "seal") == fixture.io.events.end(),
                      "live prefeed gap does not infer EOF after finite grace");
              std::this_thread::sleep_for(std::chrono::milliseconds(20));
            };
          };
          std::unique_lock<std::mutex> lock(fixture.driver.mutex);
          Require(SyncDecodeSurface(&fixture.driver, fixture.held[3], &lock,
                                     155000000) == VA_STATUS_ERROR_TIMEDOUT &&
                      !decode.replay.failed() && decode.h264_first_field.valid &&
                      decode.prefed_timestamp == fifth &&
                      !fixture.held[3]->ready,
                  "finite live sync preserves incomplete pixels and references");
        }
        fixture.io.early_eos = true;  // Supply the complementary field ourselves.
        std::function<void()> finish_after_seal;
        finish_after_seal = [&] {
          if (std::find(fixture.io.events.begin(), fixture.io.events.end(),
                        "seal") == fixture.io.events.end()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            fixture.io.on_sleep = finish_after_seal;
            return;
          }
          Require(fixture.io.outputs == std::deque<uint64_t>{0} &&
                      decode.prefed_timestamp == fifth &&
                      fixture.io.prefed_timestamp == 0 &&
                      decode.h264_first_field.valid && !fixture.held[3]->ready,
                  "seal drops raw prefix but keeps its guard and unfinished owner");
          fixture.io.outputs.push_front(fourth);
          fixture.io.picture_flags.push_back(
              VDEC_FLAG_INTERLACED_SRC | second | order);
        };
        fixture.io.on_sleep = finish_after_seal;
        const VAStatus status = tail == Tail::ContextDrain
            ? DestroyContext(&fixture.context, 1)
            : SyncSurface(&fixture.context, 4);
        Require(status == VA_STATUS_SUCCESS && fourth_owner->ready &&
                    !decode.h264_first_field.valid && decode.pending.empty() &&
                    decode.replay.outstanding() == 0 &&
                    decode.prefed_timestamp == 0 && fixture.io.prefed_timestamp == 0 &&
                    std::count(fixture.io.events.begin(), fixture.io.events.end(),
                               "seal") == 1 &&
                    std::count(fixture.io.events.begin(), fixture.io.events.end(),
                               "eos") == 1 &&
                    std::count(fixture.io.events.begin(), fixture.io.events.end(),
                               "release") == 9 && !fixture.io.invalid,
                "finite sync or explicit live destroy drains matching half and actual EOS");
        fixture.CheckPicture(3);
        const auto completed = fixture.held[3]->storage;
        if (tail == Tail::ContextDrain) {
          Require(fixture.driver.contexts.empty() && decode.device == nullptr &&
                      fixture.held[3]->storage == completed,
                  "destroyed live context leaves exact completed pixels downloadable");
        } else {
          const auto fifth_unit = TestH264AccessUnit(false, 5);
          decode.decoded_frames[fifth] = fifth_owner;
          decode.pending[fifth] = 5;
          decode.surface_timestamps[fixture.held[4].get()] = fifth;
          fixture.held[4]->decode_picture = fifth_owner;
          Require(decode.replay.Append(fifth, false, fifth_unit),
                  "accept a dependent continuation only with intact replay history");
          decode.next_timestamp = 6 * kTimestampStep;
          fixture.io.allow_device_open = true;
          const HANDLE retained_device = decode.device;
          const VAStatus restarted = PumpDecodeInput(&fixture.driver, &decode);
          const unsigned device_reopens = capability_mode == 1 ? 0 : 1;
          if (fixture.io.open_attempts != device_reopens ||
              fixture.io.device_close_calls != device_reopens)
            std::fprintf(stderr,
                "finite replay lifecycle: device-open=%u device-close=%u channel-open=%u stop=%zu channel-close=%zu capture-start=%u\n",
                fixture.io.open_attempts, fixture.io.device_close_calls,
                fixture.io.decoder_open_calls,
                static_cast<size_t>(std::count(fixture.io.events.begin(),
                                               fixture.io.events.end(), "stop")),
                static_cast<size_t>(std::count(fixture.io.events.begin(),
                                               fixture.io.events.end(), "close-decoder")),
                fixture.io.capture_start_calls);
          Require(restarted == VA_STATUS_SUCCESS &&
                      fixture.io.open_attempts == device_reopens &&
                      fixture.io.device_close_calls == device_reopens &&
                      fixture.io.library_capability_queries ==
                          (capability_mode == 3 ? 0U : 1U) &&
                      fixture.io.decoder_open_calls == 1 &&
                      fixture.io.capture_start_calls == 1 &&
                      std::count(fixture.io.events.begin(), fixture.io.events.end(),
                                 "stop") == 1 &&
                      std::count(fixture.io.events.begin(), fixture.io.events.end(),
                                 "close-decoder") == 1 &&
                      decode.device == retained_device &&
                      fixture.io.decoder_open_handle == retained_device &&
                      decode.generation == 3 &&
                      fixture.io.input_calls.size() == 14,
                  "finite continuation reuses only a qualified library device and replays original reference input");
          for (unsigned int picture = 1; picture <= 5; ++picture) {
            const auto &call = fixture.io.input_calls[7 + picture];
            Require(call.accepted && call.timestamp == picture * kTimestampStep &&
                        call.bytes == TestH264AccessUnit(picture == 1, picture),
                    "post-EOS replay restores each byte-exact original timestamp");
            queue_field(picture * kTimestampStep, first);
            queue_field(picture * kTimestampStep, second);
          }
          Require(ReceiveAvailable(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                      fifth_owner->ready && decode.pending.empty() &&
                      decode.replay.outstanding() == 0 &&
                      fixture.held[3]->storage == completed && !fixture.io.invalid,
                  "replayed pairs do not rewrite retained pixels and new pair completes");
          fixture.CheckPicture(4);
        }
      } else if (tail == Tail::Ghost) {
        queue_field(fifth, first);
        Require(ReceiveAvailable(&fixture.driver, &decode) ==
                        VA_STATUS_ERROR_DECODING_ERROR &&
                    decode.replay.failed() && !decode.h264_first_field.valid &&
                    fixture.held[3]->failed && !fixture.held[3]->ready &&
                    !fixture.held[4]->ready &&
                    std::count(fixture.io.events.begin(), fixture.io.events.end(),
                               "release") == 8 && !fixture.io.invalid,
                "ghost prefix output is released and cannot complete either owner");
      } else if (tail == Tail::Flush) {
        const uint64_t generation = decode.generation;
        VASurfaceID surface = 4;
        Require(DestroySurfacesWithTimeout(&fixture.context, &surface, 1, 0) ==
                        VA_STATUS_SUCCESS &&
                    decode.generation == generation + 1 &&
                    !decode.h264_first_field.valid &&
                    decode.h264_first_field.pixels.empty() &&
                    decode.prefed_timestamp == 0 &&
                    fixture.io.prefed_timestamp == 0 && decode.pending.empty() &&
                    decode.replay.outstanding() == 0 &&
                    !fixture.held[3]->ready && !fixture.held[4]->ready &&
                    !fixture.io.invalid,
                "surface-pool flush clears both held field and speculative successor");
      } else {
        queue_field(fourth, second);
        Require(ReceiveAvailable(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                    !decode.h264_first_field.valid && decode.pending.empty() &&
                    decode.replay.outstanding() == 0 && fourth_owner->ready &&
                    decode.decoded_frames.at(fourth) == fourth_owner,
                "only the matching second field completes the exact fourth owner");
        fixture.CheckPicture(3);
        const auto completed = fixture.held[3]->storage;
        queue_field(fourth, first);
        queue_field(fourth, second);
        Require(ReceiveAvailable(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                    !decode.h264_first_field.valid && decode.pending.empty() &&
                    decode.replay.outstanding() == 0 &&
                    fixture.held[3]->storage == completed,
                "duplicate paired output cannot complete or rewrite token four twice");
        const auto fifth_unit = TestH264AccessUnit(false, 5);
        decode.decoded_frames[fifth] = fifth_owner;
        decode.pending[fifth] = 5;
        decode.surface_timestamps[fixture.held[4].get()] = fifth;
        fixture.held[4]->decode_picture = fifth_owner;
        Require(decode.replay.Append(fifth, false, fifth_unit),
                "accept the actual fifth AU with its own immutable owner");
        decode.next_timestamp = 6 * kTimestampStep;
        Require(PumpDecodeInput(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                    fixture.io.input_calls.size() == 10 &&
                    fixture.io.input_calls[8].accepted &&
                    fixture.io.input_calls[8].timestamp == 0 &&
                    fixture.io.input_calls[8].bytes == fifth_unit &&
                    fixture.io.inputs.back() == fifth &&
                    decode.decoded_frames.at(fifth) == fifth_owner &&
                    decode.prefed_timestamp == 6 * kTimestampStep &&
                    fixture.held[3]->storage == completed,
                "actual fifth AU consumes the existing prefix by zero-tag continuation");
        queue_field(fifth, first);
        queue_field(fifth, second);
        Require(ReceiveAvailable(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                    fifth_owner->ready && decode.pending.empty() &&
                    decode.replay.outstanding() == 0 &&
                    std::count(fixture.io.events.begin(), fixture.io.events.end(),
                               "release") == 12 && !fixture.io.invalid,
                "the real fifth field pair completes once and releases every lease");
        fixture.CheckPicture(4);
      }
      for (unsigned int picture = 0; picture < 3; ++picture)
        fixture.CheckPicture(picture);
    }
  }
}

static void DeferredH264PrefeedRetriesWithoutNewInput() {
  for (unsigned mode = 0; mode != 15; ++mode) {
    TeardownFixture fixture(2);
    auto &decode = *fixture.decoder;
    decode.replay = CrystalHDDecodeReplay(true);
    decode.next_timestamp = 2 * kTimestampStep;
    decode.output_poll = std::make_shared<OutputPollLease>(-1);
    decode.armed_output_poll = decode.output_poll;
    Require(!NeedsDecodeControlWake(decode), "already armed RX lease needs no timer wake");
    const auto first = TestH264AccessUnit(true, 1);
    const auto second = TestH264AccessUnit(false, 2);
    // Exercise the captured automatic policy, not mutable newest metadata.
    Require(decode.replay.Append(kTimestampStep, true, first, true), "queue retry AU");
    fixture.io.prefeed_busy_once = mode != 1;
    if (mode == 1) fixture.io.tx_free_size = 1100; // real AU fits, 170-byte prefix does not.
    Require(PumpDecodeInput(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                decode.prefed_timestamp == 0 && decode.transport_progress == 1 &&
                decode.replay.outstanding() == 1 && decode.input_backpressured &&
                NeedsDecodeControlWake(decode) &&
                decode.pending_prefeed_timestamp == 2 * kTimestampStep &&
                decode.pending_prefeed_generation == decode.generation,
            "unsent prefix never owns a timestamp or counts as progress");
    size_t prior_calls = fixture.io.input_calls.size();
    if (mode == 0) {
      fixture.io.prefeed_busy_once = true;
      Require(PumpDecodeInput(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                  fixture.io.input_calls.size() == prior_calls + 1 &&
                  !fixture.io.input_calls.back().accepted &&
                  decode.pending_prefeed_timestamp == 2 * kTimestampStep &&
                  decode.prefed_timestamp == 0 && decode.transport_progress == 1 &&
                  decode.input_backpressured && NeedsDecodeControlWake(decode),
              "repeated BUSY gets one attempt per turn without progress or lost ownership");
      prior_calls = fixture.io.input_calls.size();
    }
    fixture.io.tx_free_size = 1024 * 1024;
    switch (mode) {
      case 2:
        Require(decode.replay.Append(2 * kTimestampStep, false, second), "real AU supersedes retry");
        decode.next_timestamp = 3 * kTimestampStep;
        break;
      case 3: ++decode.generation; break;
      case 4: decode.next_timestamp = 3 * kTimestampStep; break;
      case 5: Require(decode.replay.Seal(), "seal cancels deferred prefix"); break;
      case 6:
        Require(decode.replay.Observe(kTimestampStep) == CrystalHDDecodeReplay::Output::New,
                "completed work needs no speculative retry");
        decode.pending.clear();
        break;
      case 7: decode.closing = true; break;
      case 8: decode.retired = true; break;
      case 9: decode.destroy_prepared = true; break;
      case 10: decode.hardware_close_pending = true; break;
      case 11:
        Require(decode.replay.Seal() &&
                    decode.replay.Observe(kTimestampStep) == CrystalHDDecodeReplay::Output::New &&
                    decode.replay.EndOfSequence(), "EOS revokes unsent retry");
        break;
      case 12:
        Require(decode.CloseHardware() == BC_STS_SUCCESS, "close revokes unsent retry");
        break;
      case 13:
        Require(decode.Reset() == BC_STS_SUCCESS, "reset revokes unsent retry");
        break;
      case 14:
        Require(FailDecode(&fixture.driver, &decode, "test deferred-prefix cancellation") ==
                    VA_STATUS_ERROR_DECODING_ERROR, "failure revokes unsent retry");
        break;
      default: break;
    }
    Require(PumpDecodeInput(&fixture.driver, &decode) ==
                (mode == 14 ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS),
            "retry/cancellation stays within the existing pump contract");
    if (mode < 2) {
      Require(fixture.io.input_calls.size() == prior_calls + 1 &&
                  fixture.io.input_calls.back().accepted &&
                  fixture.io.input_calls.back().timestamp == 2 * kTimestampStep &&
                  fixture.io.input_calls.back().bytes == ExpectedH264LowLatencyPrefeed() &&
                  decode.prefed_timestamp == 2 * kTimestampStep &&
                  decode.transport_progress == 2 && decode.pending_prefeed_timestamp == 0 &&
                  !NeedsDecodeControlWake(decode),
              "BUSY/space retry sends the same prefix once without a new real AU");
      Require(PumpDecodeInput(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                  fixture.io.input_calls.size() == prior_calls + 1 && decode.transport_progress == 2,
              "accepted retry is never duplicated");
      Require(decode.replay.Append(2 * kTimestampStep, false, second), "queue actual prefed successor");
      decode.next_timestamp = 3 * kTimestampStep;
      Require(PumpDecodeInput(&fixture.driver, &decode) == VA_STATUS_SUCCESS &&
                  fixture.io.input_calls.back().timestamp == 0 &&
                  fixture.io.input_calls.back().bytes == second && decode.prefed_timestamp == 0,
              "only an accepted retry makes actual successor timestamp zero");
    } else {
      Require(fixture.io.input_calls.size() == prior_calls + (mode == 2 ? 1U : 0U) &&
                  decode.prefed_timestamp == 0 &&
                  decode.pending_prefeed_timestamp == 0 &&
                  decode.transport_progress == (mode == 2 ? 2U : 1U),
              "obsolete unsent prefix cannot escape or refresh transport progress");
      if (mode == 2)
        Require(fixture.io.input_calls.back().timestamp == 2 * kTimestampStep &&
                    fixture.io.input_calls.back().bytes == second,
                "real input wins with its original nonzero timestamp");
    }
    Require(!fixture.io.invalid, "retry preserves DIL handle and input ownership");
  }
}

static void LiveH264PrefeedBusyFallback() {
  {
    TeardownFixture fixture(2);
    fixture.decoder->live_h264 = true;
    fixture.decoder->live_h264_gap_tolerant = true;
    fixture.decoder->live_h264_fast_path = true;
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
    fixture.decoder->live_h264_gap_tolerant = true;
    fixture.decoder->live_h264_fast_path = true;
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
    fixture.decoder->live_h264_gap_tolerant = true;
    fixture.decoder->live_h264_fast_path = true;
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
    fixture.decoder->live_h264_gap_tolerant = true;
    fixture.decoder->live_h264_fast_path = true;
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
    fixture.decoder->live_h264_gap_tolerant = true;
    fixture.decoder->live_h264_fast_path = true;
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
    fixture.decoder->live_h264_gap_tolerant = true;
    fixture.decoder->live_h264_fast_path = true;
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

static void DefaultBoundedH264StillDrainsFiniteTail() {
  TeardownFixture fixture(3);
  fixture.decoder->live_h264 = true;
  fixture.decoder->replay = CrystalHDDecodeReplay(true);
  for (uint64_t picture = 1; picture <= 3; ++picture)
    Require(fixture.decoder->replay.Append(
                picture * kTimestampStep, picture == 1,
                {0, 0, 1, static_cast<uint8_t>(picture == 1 ? 0x65 : 0x41)}),
            "default bounded H.264 context retains a finite input tail");
  Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
              VA_STATUS_SUCCESS &&
              fixture.decoder->live_h264 &&
              !fixture.decoder->live_h264_gap_tolerant &&
              !fixture.decoder->live_h264_fast_path,
          "default bounded H.264 submits without live gap or fast policy");
  Require(SyncSurface(&fixture.context, 3) == VA_STATUS_SUCCESS &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "seal") == 1 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "eos") == 1 &&
              fixture.decoder->pending.empty() &&
              fixture.decoder->replay.outstanding() == 0 &&
              fixture.io.outputs.empty() && !fixture.io.invalid,
          "default bounded H.264 inactivity drains every finite tail picture");
  for (unsigned int picture = 0; picture < 3; ++picture)
    fixture.CheckPicture(picture);
}

static void PrepareAcceptedH264Au(TeardownFixture &fixture, size_t bytes,
                                  bool bounded_history = true) {
  fixture.decoder->replay = CrystalHDDecodeReplay(bounded_history);
  fixture.decoder->live_h264 = bounded_history;
  Require(fixture.decoder->replay.Append(
              kTimestampStep, true, std::vector<uint8_t>(bytes, 0x65)),
          "retain one complete H.264 AU for the output-wait regression");
  // Model a fully accepted logical AU. Fragment transport itself is covered
  // separately; this fixture must not call an unwrapped fragment API.
  SendNext(&fixture.decoder->replay, kTimestampStep);
  fixture.io.inputs.push_back(kTimestampStep);
  fixture.decoder->RecordTransportProgress();
}

static void LargeH264CompletionDoesNotInferEos() {
  const size_t chunk = crystalhd_access_unit_limits::kDtsInputChunkBytes;
  for (unsigned int control = 0; control < 3; ++control) {
    TeardownFixture fixture(1);
    const bool longer_grace = control != 1;
    PrepareAcceptedH264Au(fixture, control == 1 ? chunk : chunk + 1,
                          control != 2);
    const uint64_t progress = fixture.decoder->transport_progress;
    unsigned int callbacks = 0;
    fixture.io.on_sleep = [&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(140));
      ++callbacks;
      Require(fixture.decoder->transport_progress == progress,
              "large-AU wait crosses 100 ms without synthetic transport progress");
      if (longer_grace) {
        // Return once without output: the production loop must observe the
        // expired historical grace before the next callback supplies pixels.
        fixture.io.on_sleep = [&] {
          std::lock_guard<std::mutex> lock(fixture.driver.mutex);
          ++callbacks;
          Require(!fixture.decoder->replay.sealed() &&
                      std::count(fixture.io.events.begin(),
                                 fixture.io.events.end(), "seal") == 0,
                  "in-flight large H.264 AU must not infer EOS at 100 ms");
          fixture.io.outputs.push_back(kTimestampStep);
        };
      }
    };
    Require(SyncSurface(&fixture.context, 1) == VA_STATUS_SUCCESS,
            "delayed exact picture completes through the normal sync path");
    Require(callbacks == (longer_grace ? 2U : 1U) &&
                std::count(fixture.io.events.begin(), fixture.io.events.end(),
                           "seal") == (longer_grace ? 0 : 1) &&
                std::count(fixture.io.events.begin(), fixture.io.events.end(),
                           "eos") == (longer_grace ? 0 : 1) &&
                fixture.decoder->sync_inactivity_ns == 0 &&
                fixture.io.open_attempts == 0 && !fixture.io.invalid,
            "only AUs strictly above one chunk gain grace; output resets it");
    fixture.CheckPicture(0);
  }
  Require(DecodeWaitStatus(VA_TIMEOUT_INFINITE, kDecodeTimeoutNs - 1) ==
              VA_STATUS_SUCCESS &&
              DecodeWaitStatus(VA_TIMEOUT_INFINITE, kDecodeTimeoutNs) ==
                  VA_STATUS_ERROR_DECODING_ERROR &&
              DecodeWaitStatus(7000000, 7000000) == VA_STATUS_ERROR_TIMEDOUT,
          "large-AU grace never extends finite or exact ten-second deadlines");
}

static void LargeH264InactivitySurvivesFiniteSyncCalls() {
  TeardownFixture fixture(1);
  PrepareAcceptedH264Au(
      fixture, crystalhd_access_unit_limits::kDtsInputChunkBytes + 1);
  fixture.decoder->sync_inactivity_ns =
      kBoundedH264InactivityGraceNs - 300ULL * 1000 * 1000;
  const uint64_t before = fixture.decoder->sync_inactivity_ns;
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  Require(fixture.decoder->sync_inactivity_ns == before &&
              SyncSurface2(&fixture.context, 1, 10ULL * 1000 * 1000) ==
                  VA_STATUS_ERROR_TIMEDOUT &&
              fixture.decoder->sync_inactivity_ns > before &&
              fixture.decoder->sync_inactivity_ns <
                  kBoundedH264InactivityGraceNs &&
              !fixture.held[0]->ready && !fixture.held[0]->failed &&
              fixture.held[0]->planes[0][0] == 99 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "seal") == 0,
          "finite large-AU sync preserves only actual wait time and old pixels");
  Require(SyncSurface2(&fixture.context, 1, 500ULL * 1000 * 1000) ==
                  VA_STATUS_SUCCESS &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "seal") == 1 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "eos") == 1 &&
              fixture.decoder->replay.outstanding() == 0 &&
              fixture.decoder->pending.empty() && !fixture.io.invalid,
          "repeated finite waits still seal and drain a genuinely idle large tail");
  fixture.CheckPicture(0);
}

static void OutstandingLargeInputUsesCurrentHardwareBatch() {
  const size_t chunk = crystalhd_access_unit_limits::kDtsInputChunkBytes;
  for (size_t bytes : {chunk - 1, chunk, chunk + 1}) {
    CrystalHDDecodeReplay replay;
    Require(replay.Append(1, true, std::vector<uint8_t>(bytes, 0x65)) &&
                !replay.HasOutstandingInputLargerThan(chunk),
            "queued large input is not a completed hardware submission");
    SendNext(&replay, 1);
    Require(replay.HasOutstandingInputLargerThan(chunk) == (bytes > chunk),
            "outstanding input uses a strict transfer-size boundary");
    Require(replay.Observe(1) == CrystalHDDecodeReplay::Output::New &&
                !replay.HasOutstandingInputLargerThan(chunk),
            "completed compressed history alone never extends inactivity");
  }
  CrystalHDDecodeReplay replay;
  Require(replay.Append(1, true, {1}) &&
              replay.Append(2, false, std::vector<uint8_t>(chunk + 1, 0x41)),
          "retain small sent and large unsubmitted input");
  SendNext(&replay, 1);
  Require(!replay.HasOutstandingInputLargerThan(chunk),
          "an unadvanced large AU remains excluded through partial transport");
  SendNext(&replay, 2);
  Require(replay.HasOutstandingInputLargerThan(chunk) &&
              replay.Observe(2) == CrystalHDDecodeReplay::Output::New &&
              !replay.HasOutstandingInputLargerThan(chunk),
          "a completed large AU stops qualifying while a small AU stays pending");

  CrystalHDDecodeReplay again;
  Require(again.Append(1, true, std::vector<uint8_t>(chunk + 1, 0x65)),
          "retain large replay root");
  SendNext(&again, 1);
  Require(again.Observe(1) == CrystalHDDecodeReplay::Output::New &&
              again.Append(2, false, {2}),
          "complete large root before sealing a small tail");
  SendNext(&again, 2);
  Require(again.Seal() && again.Append(3, false, {3}) &&
              again.Observe(2) == CrystalHDDecodeReplay::Output::New &&
              again.EndOfSequence() && again.Restarted() &&
              !again.HasOutstandingInputLargerThan(chunk),
          "completed large history is not outstanding before actual replay");
  SendNext(&again, 1);
  Require(again.HasOutstandingInputLargerThan(chunk) &&
              again.Observe(1) == CrystalHDDecodeReplay::Output::Duplicate &&
              !again.HasOutstandingInputLargerThan(chunk),
          "actual replay qualifies until duplicate completion retires this batch");
}

static void CompletedLargeHistoryAndBcm70012KeepOrdinaryWaits() {
  const size_t chunk = crystalhd_access_unit_limits::kDtsInputChunkBytes;
  {
    TeardownFixture fixture(2);
    PrepareAcceptedH264Au(fixture, chunk + 1);
    Require(fixture.decoder->replay.Append(2 * kTimestampStep, false, {2}),
            "queue small tail after a large accepted picture");
    SendNext(&fixture.decoder->replay, 2 * kTimestampStep);
    fixture.io.inputs.push_back(2 * kTimestampStep);
    fixture.io.outputs.push_back(kTimestampStep);
    Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
                VA_STATUS_SUCCESS,
            "finish large picture before waiting on the small tail");
    fixture.io.on_sleep = [] {
      std::this_thread::sleep_for(std::chrono::milliseconds(140));
    };
    Require(SyncSurface(&fixture.context, 2) == VA_STATUS_SUCCESS &&
                std::count(fixture.io.events.begin(), fixture.io.events.end(),
                           "seal") == 1 &&
                fixture.decoder->sync_inactivity_ns == 0 && !fixture.io.invalid,
            "completed large cache does not extend a small pending tail's grace");
    fixture.CheckPicture(0);
    fixture.CheckPicture(1);
  }
  {
    TeardownFixture fixture(1);
    PrepareAcceptedH264Au(fixture, chunk + 1);
    fixture.decoder->is_70012 = true;
    fixture.io.on_sleep = [] {
      std::this_thread::sleep_for(std::chrono::milliseconds(140));
    };
    Require(SyncSurface2(&fixture.context, 1, 160ULL * 1000 * 1000) ==
                VA_STATUS_ERROR_TIMEDOUT &&
                fixture.decoder->sync_inactivity_ns == 0 &&
                std::count(fixture.io.events.begin(), fixture.io.events.end(),
                           "seal") == 0 &&
                !fixture.held[0]->ready && !fixture.held[0]->failed &&
                fixture.held[0]->planes[0][0] == 99 && !fixture.io.invalid,
            "BCM70012 retains its finite no-EOS wait without large-AU accounting");
  }
}

static void DefaultBoundedH264PreservesShortUnreplayableGap() {
  Require(DecodeBatchGraceExpired(kDecodeBatchGraceNs) &&
              !DecodeBatchGraceExpired(kDecodeBatchGraceNs, true) &&
              !DecodeBatchGraceExpired(kBoundedH264InactivityGraceNs - 1,
                                       true) &&
              DecodeBatchGraceExpired(kBoundedH264InactivityGraceNs, true),
          "bounded H.264 history loss selects only the longer finite grace");

  TeardownFixture fixture(4);
  CrystalHDDecodeReplay::Limits limits;
  limits.cache_bytes = 8;
  limits.pictures = 2;
  fixture.decoder->live_h264 = true;
  fixture.decoder->live_h264_gap_tolerant = false;
  fixture.decoder->live_h264_fast_path = false;
  fixture.decoder->replay = CrystalHDDecodeReplay(limits, true);
  for (uint64_t picture = 1; picture <= 3; ++picture) {
    const uint64_t timestamp = picture * kTimestampStep;
    Require(fixture.decoder->replay.Append(timestamp, picture == 1, {1}) &&
                PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                    VA_STATUS_SUCCESS,
            "submit default bounded-gap fixture");
    if (picture < 3) {
      fixture.io.outputs.push_back(timestamp);
      Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
                  VA_STATUS_SUCCESS,
              "retire old default history before the delivery gap");
    }
  }
  Require(!fixture.decoder->replay.replayable() &&
              fixture.decoder->replay.outstanding() == 1,
          "default gap begins after bounded history was retired");

  unsigned int callbacks = 0;
  fixture.io.on_sleep = [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ++callbacks;
    fixture.io.on_sleep = [&] {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      ++callbacks;
      Require(!fixture.decoder->replay.sealed() &&
                  std::count(fixture.io.events.begin(),
                             fixture.io.events.end(), "seal") == 0,
              "short unreplayable gap is not mistaken for finite EOS");
      Require(fixture.decoder->replay.Append(4 * kTimestampStep, false, {4}) &&
                  PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                      VA_STATUS_SUCCESS,
              "non-IDR input resumes the original bounded hardware session");
      fixture.io.outputs.push_back(4 * kTimestampStep);
      fixture.io.outputs.push_back(3 * kTimestampStep);
    };
  };
  Require(SyncSurface(&fixture.context, 3) == VA_STATUS_SUCCESS &&
              callbacks == 2 && fixture.io.inputs.size() == 4 &&
              fixture.io.open_attempts == 0 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "seal") == 0 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "eos") == 0 &&
              !fixture.io.invalid,
          "default bounded mode resumes exactly across a short live gap");
  fixture.CheckPicture(2);
  fixture.CheckPicture(3);
}

static void DefaultBoundedH264InactivitySurvivesFiniteSyncCalls() {
  TeardownFixture fixture(3);
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
            "submit repeated finite-sync fixture");
    if (picture < 3) {
      fixture.io.outputs.push_back(timestamp);
      Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
                  VA_STATUS_SUCCESS,
              "retire repeated finite-sync history");
    }
  }
  Require(!fixture.decoder->replay.replayable(),
          "repeated finite-sync fixture has no replay prefix");

  fixture.decoder->sync_inactivity_ns =
      kBoundedH264InactivityGraceNs - 300ULL * 1000 * 1000;
  const uint64_t inactivity_before_sync =
      fixture.decoder->sync_inactivity_ns;
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  Require(fixture.decoder->sync_inactivity_ns ==
              inactivity_before_sync &&
              SyncSurface2(&fixture.context, 3, 10ULL * 1000 * 1000) ==
                  VA_STATUS_ERROR_TIMEDOUT &&
              fixture.decoder->sync_inactivity_ns > inactivity_before_sync &&
              fixture.decoder->sync_inactivity_ns <
                  kBoundedH264InactivityGraceNs &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "seal") == 0,
          "player pause is excluded while finite sync wait is accumulated");
  const uint64_t inactivity_after_first_sync =
      fixture.decoder->sync_inactivity_ns;
  Require(SyncSurface2(&fixture.context, 3, 10ULL * 1000 * 1000) ==
                  VA_STATUS_ERROR_TIMEDOUT &&
              fixture.decoder->sync_inactivity_ns >
                  inactivity_after_first_sync &&
              fixture.decoder->sync_inactivity_ns <
                  kBoundedH264InactivityGraceNs &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "seal") == 0,
          "sequential finite sync calls retain only their actual wait time");
  Require(SyncSurface2(&fixture.context, 3, 500ULL * 1000 * 1000) ==
                  VA_STATUS_SUCCESS &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "seal") == 1 &&
              std::count(fixture.io.events.begin(), fixture.io.events.end(),
                         "eos") == 1 &&
              fixture.decoder->pending.empty() &&
              fixture.decoder->replay.outstanding() == 0 &&
              !fixture.io.invalid,
          "finite sync crosses the accumulated inactivity threshold and drains tail");
  fixture.CheckPicture(2);
}

static void EarlyLiveGapPreservesIntactReplayHistory() {
  TeardownFixture fixture(3);
  fixture.decoder->live_h264 = true;
  fixture.decoder->live_h264_gap_tolerant = true;
  fixture.decoder->live_h264_fast_path = true;
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
  fixture.decoder->live_h264_gap_tolerant = true;
  fixture.decoder->live_h264_fast_path = true;
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
    fixture.decoder->live_h264_gap_tolerant = true;
    fixture.decoder->live_h264_fast_path = true;
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

static void BindTeardownPicturesForVpp(TeardownFixture &fixture) {
  fixture.decoder->id = 1;
  fixture.driver.next_context = 2;
  fixture.driver.next_surface = fixture.held.size() + 1;
  fixture.driver.next_decode_identity = fixture.held.size() + 1;
  for (unsigned int index = 0; index < fixture.held.size(); ++index) {
    const uint64_t timestamp = (index + 1) * kTimestampStep;
    const auto &surface = fixture.held[index];
    const auto &picture = fixture.decoder->decoded_frames.at(timestamp);
    for (const auto &frame : {surface, picture}) {
      frame->id = index + 1;
      frame->decode_identity = frame->trace_decode_identity = index + 1;
      frame->trace_context = 1;
      frame->trace_generation = fixture.decoder->generation;
      frame->trace_submission_ordinal = index + 1;
    }
    surface->decode_picture = picture;
  }
}

static void CheckNewBobAfterTeardown(TeardownFixture &fixture,
                                    unsigned int index, bool readable,
                                    int replacement_luma = -1) {
  const auto &source = fixture.held.at(index);
  auto target = std::make_shared<Surface>();
  Require(target->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
          "allocate private post-teardown Bob target");
  target->id = fixture.driver.next_surface++;
  fixture.driver.surfaces[target->id] = target;
  VAConfigID config = VA_INVALID_ID;
  VAContextID processor = VA_INVALID_ID;
  Require(CreateConfig(&fixture.context, VAProfileNone, VAEntrypointVideoProc,
                       nullptr, 0, &config) == VA_STATUS_SUCCESS &&
              CreateContext(&fixture.context, config, 16, 16, 0, nullptr, 0,
                            &processor) == VA_STATUS_SUCCESS,
          "create a new VPP client after the decoder close attempt");
  for (uint32_t flags : {uint32_t{0}, uint32_t{VA_DEINTERLACING_BOTTOM_FIELD}}) {
    memset(target->storage.data(), 99, target->storage.size());
    const auto original = target->storage;
    VAProcFilterParameterBufferDeinterlacing filter = {};
    filter.type = VAProcFilterDeinterlacing;
    filter.algorithm = VAProcDeinterlacingBob;
    filter.flags = flags;
    VABufferID filter_id = VA_INVALID_ID, pipeline_id = VA_INVALID_ID;
    Require(CreateBuffer(&fixture.context, processor,
                         VAProcFilterParameterBufferType, sizeof(filter), 1,
                         &filter, &filter_id) == VA_STATUS_SUCCESS,
            "create exact physical-field Bob parameters");
    VAProcPipelineParameterBuffer pipeline = {};
    pipeline.surface = source->id;
    pipeline.filters = &filter_id;
    pipeline.num_filters = 1;
    Require(CreateBuffer(&fixture.context, processor,
                         VAProcPipelineParameterBufferType, sizeof(pipeline), 1,
                         &pipeline, &pipeline_id) == VA_STATUS_SUCCESS &&
                BeginPicture(&fixture.context, processor, target->id) ==
                    VA_STATUS_SUCCESS,
            "begin a fresh Bob request, not a previously captured VPP lease");
    const VAStatus rendered =
        RenderPicture(&fixture.context, processor, &pipeline_id, 1);
    if (readable) {
      Require(rendered == VA_STATUS_SUCCESS &&
                  EndPicture(&fixture.context, processor) == VA_STATUS_SUCCESS &&
                  target->ready && !target->failed &&
                  target->frame_timestamp == source->frame_timestamp &&
                  target->expected_timestamp == source->expected_timestamp &&
                  target->trace_decode_identity == source->trace_decode_identity &&
                  target->trace_context == source->trace_context &&
                  target->trace_generation == source->trace_generation &&
                  target->trace_submission_ordinal == source->trace_submission_ordinal,
              "both newly submitted Bob fields retain the exact successfully drained picture identity");
      for (unsigned int plane = 0; plane != 2; ++plane)
        for (unsigned int row = 0; row != (plane == 0 ? 16U : 8U); ++row)
          for (unsigned int column = 0; column != 16; ++column)
            Require(target->planes[plane][row * target->pitch[plane] + column] ==
                        (plane == 0 ? (replacement_luma < 0
                            ? DecodeIoMock::Luma(source->expected_timestamp)
                            : replacement_luma) : 128),
                    "new Bob output contains the actual drained Y/UV pixels, not neutral or stale data");
    } else {
      Require(rendered != VA_STATUS_SUCCESS &&
                  EndPicture(&fixture.context, processor) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                  !fixture.driver.contexts.at(processor)->vpp_frame &&
                  !target->ready && target->storage == original,
              "unfinished or failed close cannot expose a fresh immutable Bob source");
    }
    Require(target->vpp_writers == 0 && source->vpp_readers == 0 &&
                fixture.driver.pending_vpp.empty() &&
                DestroyBuffer(&fixture.context, pipeline_id) == VA_STATUS_SUCCESS &&
                DestroyBuffer(&fixture.context, filter_id) == VA_STATUS_SUCCESS,
            "each field completes or rejects without leaking queue/buffer ownership");
  }
  Require(DestroyContext(&fixture.context, processor) == VA_STATUS_SUCCESS &&
              DestroyConfig(&fixture.context, config) == VA_STATUS_SUCCESS,
          "release the independent post-teardown VPP client");
  VASurfaceID target_id = target->id;
  Require(DestroySurfaces(&fixture.context, &target_id, 1) == VA_STATUS_SUCCESS,
          "release post-teardown Bob output without retiring held source pictures");
}

static void FirstH264FieldRefreshesTransportGrace() {
  enum class Case { Grace, Pair, Unknown, ReleaseFailure, SameParity, Duplicate, Ghost };
  for (Case test : {Case::Grace, Case::Pair, Case::Unknown, Case::ReleaseFailure,
                    Case::SameParity, Case::Duplicate, Case::Ghost}) {
    TeardownFixture fixture(1);
    auto &decode = *fixture.decoder;
    decode.profile = VAProfileH264Main;
    decode.height = fixture.io.output_height = 32;
    for (const auto &picture : {fixture.held[0], decode.decoded_frames.at(kTimestampStep)}) {
      Require(picture->AllocateInternal(nullptr, -1, 16, 32, VA_FOURCC_NV12),
              "allocate first-field progress fixture");
      memset(picture->storage.data(), 99, picture->storage.size());
      picture->expected_timestamp = kTimestampStep;
      picture->h264_crop_unit_y = 4;
    }
    BindTeardownPicturesForVpp(fixture);
    Require(PumpDecodeInput(&fixture.driver, &decode) == VA_STATUS_SUCCESS,
            "submit the actual finite picture before observing field progress");
    const uint64_t progress = decode.transport_progress;
    const auto queue = [&](uint64_t token, bool bottom) {
      fixture.io.outputs.push_back(token);
      fixture.io.picture_flags.push_back(VDEC_FLAG_INTERLACED_SRC |
          (bottom ? VDEC_FLAG_BOTTOMFIELD : VDEC_FLAG_TOPFIELD));
    };
    const auto receive = [&] {
      bool activity = false;
      return ReceiveOne(&fixture.driver, &decode, 0, &activity);
    };
    if (test == Case::Grace) {
      unsigned waits = 0;
      fixture.io.on_sleep = [&] {
        ++waits;
        // The next poll has fresh hardware work even though the OLD idle
        // deadline passed. Only successful field acquisition may renew it.
        std::this_thread::sleep_for(std::chrono::nanoseconds(kDecodeBatchGraceNs) +
                                    std::chrono::milliseconds(10));
        std::lock_guard<std::mutex> lock(fixture.driver.mutex);
        queue(kTimestampStep, false);
        fixture.io.on_sleep = [&] {
          ++waits;
          std::lock_guard<std::mutex> lock(fixture.driver.mutex);
          Require(decode.h264_first_field.valid && !fixture.held[0]->ready &&
                      decode.replay.outstanding() == 1 && !decode.replay.sealed() &&
                      decode.transport_progress == progress + 1 &&
                      std::count(fixture.io.events.begin(), fixture.io.events.end(), "seal") == 0,
                  "fresh first field renews idle grace without EOS or early picture completion");
          queue(kTimestampStep, true);
        };
      };
      Require(SyncSurface2(&fixture.context, 1, 2ULL * 1000 * 1000 * 1000) ==
                  VA_STATUS_SUCCESS && waits == 2 &&
                  decode.transport_progress == progress + 2 &&
                  decode.replay.outstanding() == 0 && !decode.replay.sealed() &&
                  fixture.io.open_attempts == 0,
              "complement within renewed grace completes without EOS/restart");
      fixture.CheckPicture(0);
    } else {
      if (test == Case::ReleaseFailure) fixture.io.output_release_status = BC_STS_ERROR;
      if (test == Case::Ghost) decode.prefed_timestamp = 2 * kTimestampStep;
      queue(test == Case::Unknown || test == Case::Ghost ? 2 * kTimestampStep : kTimestampStep, false);
      const VAStatus status = receive();
      if (test == Case::Unknown || test == Case::ReleaseFailure || test == Case::Ghost) {
        Require((status == VA_STATUS_SUCCESS) == (test == Case::Unknown) &&
                    decode.transport_progress == progress && !fixture.held[0]->ready,
                "unknown, speculative and failed-release fields are not transport progress");
      } else {
        Require(status == VA_STATUS_SUCCESS && decode.transport_progress == progress + 1 &&
                    decode.h264_first_field.valid && decode.replay.outstanding() == 1 &&
                    !fixture.held[0]->ready && fixture.held[0]->frame_timestamp == 0,
                "one validated released first field is progress, not a completed picture");
        queue(kTimestampStep, test != Case::SameParity);
        const VAStatus second = receive();
        if (test == Case::SameParity) {
          Require(second != VA_STATUS_SUCCESS && decode.transport_progress == progress + 1,
                  "same-parity rejection cannot refresh transport grace");
        } else {
          Require(second == VA_STATUS_SUCCESS && decode.transport_progress == progress + 2 &&
                      decode.replay.outstanding() == 0 && !decode.h264_first_field.valid,
                  "complementary pair preserves the existing single completion increment");
          fixture.CheckPicture(0);
          if (test == Case::Duplicate) {
            queue(kTimestampStep, false);
            Require(receive() == VA_STATUS_SUCCESS && decode.transport_progress == progress + 2 &&
                        !decode.h264_first_field.valid,
                    "unsolicited completed-picture duplicate cannot refresh grace");
          }
        }
      }
    }
    Require(!fixture.io.invalid, "first-field progress uses only mocked owned hardware calls");
  }
}

static void FirstTemporalBobUsesCompactStorage() {
  for (bool bottom : {false, true}) {
    H264FieldFixture fixture;
    DecodeIoMock io;
    MockDecodeScope scope(io);
    fixture.decoder.device = io.handle();
    io.override_output = true;
    io.overridden_output = fixture.Output(bottom, bottom,
                                          bottom ? &fixture.bottom : &fixture.top);
    bool activity = false;
    Require(ReceiveOne(&fixture.driver, &fixture.decoder, 0, &activity) ==
                VA_STATUS_SUCCESS && activity,
            "compact Bob starts with a validated and released first field");
    PendingVpp operation;
    operation.source = fixture.private_frame;
    operation.decoder = std::shared_ptr<DecodeContext>(&fixture.decoder,
                                                       [](DecodeContext *) {});
    operation.source_timestamp = 1;
    operation.decoder_generation = 3;
    operation.deinterlace.bob = true;
    operation.deinterlace.field = bottom ? crystalhd_vpp_bob::FieldParity::kBottom
                                         : crystalhd_vpp_bob::FieldParity::kTop;
    const auto source_pixels = fixture.private_frame->storage;
    const auto public_pixels = fixture.public_frame->storage;
    const auto held_pixels = fixture.decoder.h264_first_field.pixels;
    std::shared_ptr<Surface> view;
    Require(PrepareFirstFieldBob(operation, &view) == VA_STATUS_SUCCESS && view,
            "prepare exact first temporal field");
    const unsigned field_height = (fixture.private_frame->height + 1U) / 2U;
    Require(view->width == fixture.private_frame->width &&
                view->height == field_height &&
                view->storage.size() == static_cast<size_t>(view->pitch[0]) *
                    (field_height + (field_height + 1U) / 2U),
            "first-field Bob stores only compact field Y/UV rows, not a full Weave frame");
    Require(fixture.private_frame->storage == source_pixels &&
                fixture.public_frame->storage == public_pixels &&
                fixture.decoder.h264_first_field.pixels == held_pixels &&
                fixture.decoder.h264_first_field.valid &&
                !fixture.private_frame->ready && !fixture.public_frame->ready &&
                fixture.decoder.replay.outstanding() == 1 && !io.invalid,
            "compaction preserves held field, public pixels and pending decode ownership");
    const auto compact_pixels = view->storage;
    fixture.decoder.h264_first_field = H264FirstField{};
    Require(view->storage == compact_pixels && view->ready &&
                view->frame_timestamp == 1 && !fixture.private_frame->ready &&
                fixture.decoder.replay.outstanding() == 1,
            "private compact view owns pixels independently of later latch release");
    fixture.decoder.device = nullptr;
  }
}

static void FirstTemporalBobDoesNotCompleteDecodePicture() {
  for (bool bottom_first : {false, true}) {
    for (unsigned visible_height : {32U, 28U}) {
     for (unsigned route : {0U, 1U, 2U}) { // pre-latched, arrives while waiting, worker
      H264FieldFixture fixture;
      DecodeIoMock io;
      MockDecodeScope scope(io);
      auto decoder = std::shared_ptr<DecodeContext>(&fixture.decoder,
                                                   [](DecodeContext *) {});
      fixture.driver.contexts[decoder->id] = decoder;
      decoder->device = io.handle();
      decoder->decoder_started = true;
      decoder->live_h264 = decoder->live_h264_gap_tolerant = true;
      VADriverContext context = {};
      context.pDriverData = &fixture.driver;
      auto receive = [&](bool bottom) {
        io.override_output = true;
        io.overridden_output = fixture.Output(bottom, bottom_first,
                                              bottom ? &fixture.bottom : &fixture.top);
        bool activity = false;
        const VAStatus status = ReceiveOne(&fixture.driver, decoder.get(), 0, &activity);
        io.override_output = false;
        Require(status == VA_STATUS_SUCCESS && activity,
                "acquire and release the exact real field before Bob reads it");
      };
      if (route != 1) receive(bottom_first);
      auto target = std::make_shared<Surface>();
      Require(target->AllocateCpuNv12(8, 16), "allocate scaled private Bob target");
      target->id = 2;
      target->layout.valid = route != 2; // Synchronous fallback or private worker.
      fixture.driver.surfaces[2] = target;
      VAConfigID config = VA_INVALID_ID;
      VAContextID processor = VA_INVALID_ID;
      Require(CreateConfig(&context, VAProfileNone, VAEntrypointVideoProc, nullptr, 0,
                           &config) == VA_STATUS_SUCCESS &&
                  CreateContext(&context, config, 8, 16, 0,
                                nullptr, 0, &processor) == VA_STATUS_SUCCESS,
              "create first-field Bob API context");
      VAProcFilterParameterBufferDeinterlacing filter = {};
      filter.type = VAProcFilterDeinterlacing;
      filter.algorithm = VAProcDeinterlacingBob;
      filter.flags = bottom_first ? VA_DEINTERLACING_BOTTOM_FIELD_FIRST |
                                        VA_DEINTERLACING_BOTTOM_FIELD : 0;
      VABufferID filter_id = VA_INVALID_ID, pipeline_id = VA_INVALID_ID;
      VARectangle region = {0, 0, 16, static_cast<uint16_t>(visible_height)};
      Require(CreateBuffer(&context, processor, VAProcFilterParameterBufferType,
                           sizeof(filter), 1, &filter, &filter_id) == VA_STATUS_SUCCESS,
              "create exact first temporal parity filter");
      VAProcPipelineParameterBuffer pipeline = {};
      pipeline.surface = 1;
      pipeline.surface_region = &region;
      pipeline.filters = &filter_id;
      pipeline.num_filters = 1;
      Require(CreateBuffer(&context, processor, VAProcPipelineParameterBufferType,
                           sizeof(pipeline), 1, &pipeline, &pipeline_id) == VA_STATUS_SUCCESS &&
                  BeginPicture(&context, processor, 2) == VA_STATUS_SUCCESS &&
                  RenderPicture(&context, processor, &pipeline_id, 1) == VA_STATUS_SUCCESS,
              "capture first-field Bob without changing the public decode binding");
      unsigned waits = 0;
      io.on_sleep = [&] {
        ++waits;
        std::lock_guard<std::mutex> lock(fixture.driver.mutex);
        receive(route == 1 ? bottom_first : !bottom_first);
        if (route == 1) {
          io.on_sleep = [&] {
            ++waits;
            std::lock_guard<std::mutex> lock(fixture.driver.mutex);
            receive(!bottom_first); // Baseline terminates without a 10s wait.
          };
        }
      };
      const VAStatus ended = EndPicture(&context, processor);
      if (route == 2 && ended == VA_STATUS_SUCCESS) {
        std::thread worker([&] { RunVppWorker(&fixture.driver); });
        {
          std::unique_lock<std::mutex> lock(fixture.driver.mutex);
          fixture.driver.condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return fixture.driver.pending_vpp.empty();
          });
          fixture.driver.stopping = true;
          fixture.driver.condition.notify_all();
        }
        worker.join();
        fixture.driver.stopping = false;
      }
      Require(ended == VA_STATUS_SUCCESS && waits == (route == 1 ? 1U : 0U) &&
                  target->ready && target->frame_timestamp == 1 &&
                  !fixture.private_frame->ready && !fixture.public_frame->ready &&
                  fixture.private_frame->frame_timestamp == 0 &&
                  decoder->h264_first_field.valid && decoder->replay.outstanding() == 1 &&
                  !decoder->replay.sealed() && decoder->generation == 3 &&
                  SyncSurface2(&context, 1, 0) == VA_STATUS_ERROR_TIMEDOUT,
              "first temporal Bob completes before complement without completing public decode");
      io.on_sleep = {};
      const auto early_pixels = target->storage;
      receive(!bottom_first);
      Require(fixture.private_frame->ready && !decoder->h264_first_field.valid &&
                  decoder->replay.outstanding() == 0 && target->storage == early_pixels,
              "complement completes the original picture without mutating early Bob");
      Surface oracle;
      Require(oracle.AllocateCpuNv12(8, 16), "allocate completed-pair Bob oracle");
      VppDeinterlace bob;
      bob.bob = true;
      bob.field = bottom_first ? crystalhd_vpp_bob::FieldParity::kBottom
                               : crystalhd_vpp_bob::FieldParity::kTop;
      const VARectangle output_region = {0, 0, 8, 16};
      Require(ProcessVpp(&fixture.driver.vpp_scaler, &fixture.driver.vpp_argb_staging,
                         fixture.private_frame.get(), &oracle, region, output_region,
                         false, bob) == VA_STATUS_SUCCESS,
              "derive the existing completed-Weave Bob pixel oracle");
      for (unsigned plane = 0; plane != 2; ++plane)
        for (unsigned row = 0; row != (plane ? (oracle.height + 1) / 2 : oracle.height); ++row)
          Require(memcmp(target->planes[plane] + row * target->pitch[plane],
                         oracle.planes[plane] + row * oracle.pitch[plane], 8) == 0,
                  "every active early Bob Y/U/V byte equals completed-Weave Bob");
      Require(fixture.driver.pending_vpp.empty() &&
                  fixture.private_frame->vpp_readers == 0 && target->vpp_writers == 0 &&
                  !io.invalid && io.open_attempts == 0,
              "early Bob balances leases without decoder reopen");
      Require(DestroyBuffer(&context, pipeline_id) == VA_STATUS_SUCCESS &&
                  DestroyBuffer(&context, filter_id) == VA_STATUS_SUCCESS &&
                  DestroyContext(&context, processor) == VA_STATUS_SUCCESS &&
                  DestroyConfig(&context, config) == VA_STATUS_SUCCESS,
              "release the private first-field API fixtures");
      fixture.driver.contexts.erase(decoder->id);
      decoder->device = nullptr;
      decoder->decoder_started = false;
     }
    }
  }
}

static void FirstTemporalBobRejectsMismatchedLatch() {
  for (unsigned fault = 0; fault != 20; ++fault) {
    H264FieldFixture fixture;
    DecodeIoMock io;
    MockDecodeScope scope(io);
    fixture.decoder.device = io.handle();
    io.override_output = true;
    io.overridden_output = fixture.Output(false, false, &fixture.top);
    bool activity = false;
    Require(ReceiveOne(&fixture.driver, &fixture.decoder, 0, &activity) == VA_STATUS_SUCCESS,
            "negative views start with a validated and released first field");
    PendingVpp operation;
    operation.source = fixture.private_frame;
    operation.decoder = std::shared_ptr<DecodeContext>(&fixture.decoder, [](DecodeContext *) {});
    operation.source_timestamp = 1;
    operation.decoder_generation = 3;
    operation.deinterlace.bob = true;
    auto &first = fixture.decoder.h264_first_field;
    switch (fault) {
      case 0: operation.deinterlace.field = crystalhd_vpp_bob::FieldParity::kBottom; break;
      case 1: first.bottom_first = true; break;
      case 2: first.timestamp = 2; break;
      case 3: ++first.decode_identity; break;
      case 4: ++first.generation; break;
      case 5: fixture.private_frame->failed = true; break;
      case 6: fixture.private_frame->destroyed = true; break;
      case 7: fixture.decoder.retired = true; break;
      case 8: first.replay_duplicate = true; break;
      case 9: first.is_70012 = true; break;
      case 10: fixture.decoder.closing = true; break;
      case 11: first.picture = fixture.public_frame; break;
      case 12: allocation_fault::fail_next = true; break;
      case 13: first.pixels.resize(first.pixels.size() - 1); break;
      case 14: first.source_pitch = 0; break;
      case 15: fixture.private_frame->expected_timestamp = 2; break;
      case 16: ++fixture.private_frame->trace_generation; break;
      case 17: fixture.private_frame->image_write_locked = true; break;
      case 18: fixture.decoder.hardware_close_pending = true; break;
      case 19: ++operation.decoder_generation; break;
    }
    std::shared_ptr<Surface> view;
    const VAStatus status = PrepareFirstFieldBob(operation, &view);
    const VAStatus expected = fault == 12 ? VA_STATUS_ERROR_ALLOCATION_FAILED :
        (fault == 13 || fault == 14) ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS;
    Require(status == expected &&
                !view && !allocation_fault::fail_next && first.valid &&
                fixture.decoder.replay.outstanding() == 1 &&
                !fixture.private_frame->ready && fixture.private_frame->frame_timestamp == 0 &&
                !fixture.public_frame->ready && !io.invalid,
            "mismatched/canceled/OOM first-field views cannot publish or retire a picture");
    fixture.decoder.device = nullptr;
  }
}

static VAPictureParameterBufferH264 NeutralPicture(unsigned width, unsigned height);

static void LiveBobTailWithUnfenceableExposedTarget() {
  enum class Tail { Complete, Cancel, Drain, Auxiliary };
  for (bool explicit_live : {false, true})
  for (bool bottom_first : {false, true}) {
    for (Tail tail : {Tail::Complete, Tail::Cancel, Tail::Drain, Tail::Auxiliary}) {
      if (!explicit_live && tail != Tail::Auxiliary)
        continue;
      TeardownFixture fixture(2);
      // This group exercises the inline unfenceable EndPicture fallback.
      fixture.driver.StopWorkers();
      fixture.driver.stopping = false;
      auto &decode = *fixture.decoder;
      decode.profile = VAProfileH264Main;
      decode.height = fixture.io.output_height = 32;
      decode.live_h264 = true;
      decode.live_h264_gap_tolerant = decode.live_h264_fast_path = explicit_live;
      decode.low_latency_h264 = explicit_live;
      decode.auto_h264_prefeed = !explicit_live;
      decode.replay = CrystalHDDecodeReplay(true);
      for (unsigned int index = 0; index != 2; ++index) {
        const uint64_t token = (index + 1) * kTimestampStep;
        for (const auto &picture : {fixture.held[index],
                                   decode.decoded_frames.at(token)}) {
          Require(picture->AllocateInternal(nullptr, -1, 16, 32,
                                            VA_FOURCC_NV12),
                  "allocate the combined live Bob field-pair fixture");
          memset(picture->storage.data(), 99, picture->storage.size());
          picture->expected_timestamp = token;
          picture->h264_crop_unit_y = 4;
        }
      }
      BindTeardownPicturesForVpp(fixture);
      const uint32_t order = bottom_first ? VDEC_FLAG_BOTTOM_FIRST : 0;
      const uint32_t first = bottom_first ? VDEC_FLAG_BOTTOMFIELD
                                          : VDEC_FLAG_TOPFIELD;
      const uint32_t second = bottom_first ? VDEC_FLAG_TOPFIELD
                                           : VDEC_FLAG_BOTTOMFIELD;
      const auto queue_field = [&](uint64_t token, uint32_t parity) {
        fixture.io.outputs.push_back(token);
        fixture.io.picture_flags.push_back(VDEC_FLAG_INTERLACED_SRC | order | parity);
      };
      for (unsigned int picture = 1; picture <= 2; ++picture) {
        const uint64_t token = picture * kTimestampStep;
        Require(decode.replay.Append(token, picture == 1,
                                      TestH264AccessUnit(picture == 1, picture),
                                      !explicit_live),
                "accept each real AU before its independent successor prefix");
        if (tail == Tail::Auxiliary) {
          H264Auxiliary::Syntax syntax;
          syntax.picture = NeutralPicture(16, 32);
          syntax.generation = decode.generation;
          syntax.device = decode.device;
          syntax.prefeed = true;
          decode.auxiliary.syntax.emplace(token, syntax);
        }
        decode.next_timestamp = (picture + 1) * kTimestampStep;
        Require(PumpDecodeInput(&fixture.driver, &decode) == VA_STATUS_SUCCESS,
                "send real live input and its syntactic successor prefix");
        queue_field(token, first);
        if (picture == 1) queue_field(token, second);
        Require(ReceiveAvailable(&fixture.driver, &decode) == VA_STATUS_SUCCESS,
                "complete the first picture but hold the final complementary field");
      }
      const uint64_t token = 2 * kTimestampStep;
      const auto source = decode.decoded_frames.at(token);
      const uint64_t generation = decode.generation;
      Require(source->frame_timestamp == 0 && !source->ready &&
                  decode.h264_first_field.valid && decode.replay.outstanding() == 1 &&
                  decode.prefed_timestamp == 3 * kTimestampStep &&
                  decode.pending.count(3 * kTimestampStep) == 0,
              "a cold final source and a prefed-only successor remain distinct owners");
      const auto public_pixels = fixture.held[1]->storage;
      const auto held_field = decode.h264_first_field.pixels;
      const uint64_t ordinal = decode.next_submission_ordinal;
      const size_t input_calls = fixture.io.input_calls.size();
      std::vector<uint8_t> expected_auxiliary;
      if (tail == Tail::Auxiliary)
        Require(decode.auxiliary.candidate_timestamp == token &&
                    BuildH264NeutralIntra(decode.auxiliary.candidate.picture,
                                          &expected_auxiliary),
                "only the actually submitted final AU supplies auxiliary syntax");

      if (tail == Tail::Drain) {
        fixture.io.early_eos = true;
        fixture.io.on_sleep = [&] {
          Require(fixture.io.outputs == std::deque<uint64_t>{0} &&
                      decode.h264_first_field.valid && !source->ready,
                  "explicit drain seals before the simulated final complementary field");
          fixture.io.outputs.push_front(token);
          fixture.io.picture_flags.push_back(VDEC_FLAG_INTERLACED_SRC | order | second);
        };
        Require(DestroyContext(&fixture.context, 1) == VA_STATUS_SUCCESS &&
                    source->ready && !source->failed && decode.retired &&
                    decode.pending.empty() && decode.replay.outstanding() == 0 &&
                    decode.prefed_timestamp == 0 && decode.device == nullptr &&
                    std::count(fixture.io.events.begin(), fixture.io.events.end(), "seal") == 1 &&
                    std::count(fixture.io.events.begin(), fixture.io.events.end(), "eos") == 1,
                "explicit decoder drain finishes the exact pair before consuming close");
      }

      auto target = std::make_shared<Surface>();
      Require(target->AllocateInternal(nullptr, -1, 16, 32, VA_FOURCC_NV12),
              "allocate the exposed Bob target without accessing a device");
      target->id = fixture.driver.next_surface++;
      target->layout.valid = true;
      // Exercise the established no-fence-transport branch, not a real
      // sw_sync open failure. No DMA-BUF or debugfs operation is attempted.
      Require(target->object_fds.empty(), "the model has no external fence transport");
      memset(target->storage.data(), 99, target->storage.size());
      const auto original = target->storage;
      fixture.driver.surfaces[target->id] = target;
      VAConfigID config = VA_INVALID_ID;
      VAContextID processor = VA_INVALID_ID;
      Require(CreateConfig(&fixture.context, VAProfileNone, VAEntrypointVideoProc,
                           nullptr, 0, &config) == VA_STATUS_SUCCESS &&
                  CreateContext(&fixture.context, config, 16, 32, 0, nullptr, 0,
                                &processor) == VA_STATUS_SUCCESS,
              "create a real VPP API client for the held live picture");
      VAProcFilterParameterBufferDeinterlacing filter = {};
      filter.type = VAProcFilterDeinterlacing;
      filter.algorithm = VAProcDeinterlacingBob;
      // These existing wait/cancel cases exercise the missing complement,
      // not the newly readable first temporal field tested above.
      const bool selected_bottom = tail == Tail::Drain ? bottom_first : !bottom_first;
      filter.flags = (bottom_first ? VA_DEINTERLACING_BOTTOM_FIELD_FIRST : 0) |
                     (selected_bottom ? VA_DEINTERLACING_BOTTOM_FIELD : 0);
      VABufferID filter_id = VA_INVALID_ID, pipeline_id = VA_INVALID_ID;
      Require(CreateBuffer(&fixture.context, processor, VAProcFilterParameterBufferType,
                           sizeof(filter), 1, &filter, &filter_id) == VA_STATUS_SUCCESS,
              "create complementary-field wait or completed-drain Bob parameters");
      VAProcPipelineParameterBuffer pipeline = {};
      pipeline.surface = 2;
      pipeline.filters = &filter_id;
      pipeline.num_filters = 1;
      Require(CreateBuffer(&fixture.context, processor, VAProcPipelineParameterBufferType,
                           sizeof(pipeline), 1, &pipeline, &pipeline_id) == VA_STATUS_SUCCESS &&
                  BeginPicture(&fixture.context, processor, target->id) == VA_STATUS_SUCCESS &&
                  RenderPicture(&fixture.context, processor, &pipeline_id, 1) == VA_STATUS_SUCCESS,
              "capture the exact final live picture through Begin/Render");
      unsigned int waits = 0;
      const auto supply_after_auxiliary = [&] {
        Require(decode.auxiliary.pending && decode.auxiliary.needs_eos &&
                    decode.auxiliary.timestamp == 3 * kTimestampStep &&
                    fixture.io.input_calls.size() == input_calls + 2 &&
                    fixture.io.input_calls[input_calls].accepted &&
                    fixture.io.input_calls[input_calls].timestamp == 0 &&
                    fixture.io.input_calls[input_calls].bytes == expected_auxiliary &&
                    fixture.io.input_calls.back().accepted &&
                    fixture.io.input_calls.back().timestamp == 4 * kTimestampStep &&
                    fixture.io.input_calls.back().bytes == H264LowLatencyPrefeed() &&
                    decode.next_timestamp == 4 * kTimestampStep &&
                    decode.next_submission_ordinal == ordinal &&
                    decode.pending.size() == 1 && decode.pending.count(token) == 1 &&
                    decode.replay.outstanding() == 1 &&
                    !decode.replay.IsOutstanding(3 * kTimestampStep) &&
                    decode.h264_first_field.pixels == held_field && !source->ready &&
                    fixture.held[1]->storage == public_pixels &&
                    target->storage == original && !target->ready &&
                    std::count(fixture.io.events.begin(), fixture.io.events.end(), "seal") == 0,
                "public EndPicture submits private auxiliary before any original complement is available");
        queue_field(token, second);
      };
      if (tail == Tail::Auxiliary) {
        fixture.io.on_sleep = [&] {
          ++waits;
          if (decode.auxiliary.pending) {
            supply_after_auxiliary();
            return;
          }
          Require(fixture.io.input_calls.size() == input_calls &&
                      fixture.io.outputs.empty() && !source->ready,
                  "no mock output or auxiliary is supplied before the real idle grace");
          std::this_thread::sleep_for(std::chrono::nanoseconds(kDecodeBatchGraceNs) +
                                      std::chrono::milliseconds(1));
          fixture.io.on_sleep = [&] {
            ++waits;
            // A missing EndPicture-to-auxiliary hook fails here, not after
            // the ten-second production deadline or a fabricated completion.
            supply_after_auxiliary();
          };
        };
      } else if (tail != Tail::Drain) {
        fixture.io.on_sleep = [&] {
          ++waits;
          Require(fixture.driver.pending_vpp.size() == 1 &&
                      fixture.driver.pending_vpp.front().worker_active &&
                      source->vpp_readers == 1 && target->vpp_writers == 1 &&
                      !target->ready && target->storage == original &&
                      !decode.replay.sealed(),
                  "unfenceable End waits with balanced ownership, never unfinished pixels");
          if (tail == Tail::Complete) {
            queue_field(token, second);
          } else {
            VASurfaceID source_id = 2;
            Require(DestroySurfacesWithTimeout(&fixture.context, &source_id, 1, 0) ==
                        VA_STATUS_SUCCESS,
                    "actual source-pool retirement cancels the waiting final pair");
          }
        };
      }
      const VAStatus ended = EndPicture(&fixture.context, processor);
      Require(fixture.driver.pending_vpp.empty() && source->vpp_readers == 0 &&
                  target->vpp_writers == 0 && !fixture.io.invalid,
              "all combined tail paths release queued readers and writers exactly once");
      if (tail == Tail::Cancel) {
        Require(waits == 1 && ended == VA_STATUS_ERROR_OPERATION_FAILED &&
                    SyncSurface2(&fixture.context, target->id, 0) ==
                        VA_STATUS_ERROR_DECODING_ERROR &&
                    decode.generation == generation + 1 && !decode.h264_first_field.valid &&
                    !target->ready && target->failed && target->storage == original,
                "pool cancellation reproduces End1 then Sync23 without writing fallback pixels");
      } else {
        Require((tail == Tail::Auxiliary ? waits >= 1 && waits <= 2 :
                     waits == (tail == Tail::Complete ? 1U : 0U)) &&
                    ended == VA_STATUS_SUCCESS && target->ready && !target->failed &&
                    target->frame_timestamp == token && target->expected_timestamp == token &&
                    target->trace_decode_identity == source->trace_decode_identity &&
                    target->trace_generation == generation &&
                    SyncSurface2(&fixture.context, target->id, 0) == VA_STATUS_SUCCESS,
                "natural pair completion or successful explicit drain yields exact Bob identity");
        for (unsigned int plane = 0; plane != 2; ++plane)
          for (unsigned int row = 0; row != (plane == 0 ? 32U : 16U); ++row)
            for (unsigned int column = 0; column != 16; ++column)
              Require(target->planes[plane][row * target->pitch[plane] + column] ==
                          (plane == 0 ? DecodeIoMock::Luma(token) : 128),
                      "successful combined Bob output contains every real active Y/UV byte");
      }
      if (tail == Tail::Auxiliary) {
        Require(source->ready && fixture.held[1]->ready &&
                    decode.replay.outstanding() == 0 && decode.pending.empty() &&
                    !decode.h264_first_field.valid && decode.auxiliary.pending &&
                    decode.auxiliary.needs_eos && !decode.replay.sealed() &&
                    decode.next_submission_ordinal == ordinal && fixture.io.open_attempts == 0,
                "only the real complement completes Bob; private output debt remains without EOS/reopen");
        fixture.CheckPicture(1);
        const auto completed_pixels = target->storage;
        H264WeaveFixture auxiliary_output;
        auxiliary_output.output.PicInfo.timeStamp = 3 * kTimestampStep;
        auxiliary_output.output.PicInfo.flags &= ~VDEC_FLAG_UNKNOWN_SRC;
        auxiliary_output.raw.timeStamp = 3 * kTimestampStep / 20000U;
        auxiliary_output.raw.flags = VDEC_FLAG_FIELDPAIR;
        auxiliary_output.WritePib();
        fixture.io.override_output = true;
        fixture.io.overridden_output = auxiliary_output.output;
        bool activity = false;
        Require(ReceiveOne(&fixture.driver, &decode, 0, &activity) == VA_STATUS_SUCCESS &&
                    activity && !decode.auxiliary.pending && decode.auxiliary.needs_eos &&
                    decode.replay.outstanding() == 0 && target->storage == completed_pixels &&
                    target->frame_timestamp == token && decode.next_submission_ordinal == ordinal,
                "validated and released auxiliary retires only private debt, never public pixels or PTS");
        fixture.io.override_output = false;
        fixture.io.early_eos = true; // Every original and auxiliary already retired.
        Require(std::count(fixture.io.events.begin(), fixture.io.events.end(), "seal") == 0 &&
                    DestroyContext(&fixture.context, 1) == VA_STATUS_SUCCESS &&
                    decode.device == nullptr && decode.retired &&
                    target->storage == completed_pixels && source->ready &&
                    std::count(fixture.io.events.begin(), fixture.io.events.end(), "seal") == 1 &&
                    std::count(fixture.io.events.begin(), fixture.io.events.end(), "eos") == 1 &&
                    fixture.io.open_attempts == 0,
                "explicit close alone flushes and consumes the actual auxiliary-tail EOS");
        Require(std::find(fixture.io.events.begin(), fixture.io.events.end(), "eos") <
                    std::find(fixture.io.events.begin(), fixture.io.events.end(), "stop"),
                "actual EOS precedes terminal decoder teardown");
      }
      Require(std::count(fixture.io.events.begin(), fixture.io.events.end(), "release") ==
                  (tail == Tail::Cancel ? 3 : tail == Tail::Drain ? 5 :
                   tail == Tail::Auxiliary ? 6 : 4) &&
                  (tail == Tail::Drain || tail == Tail::Auxiliary ||
                   std::count(fixture.io.events.begin(), fixture.io.events.end(), "seal") == 0) &&
                  DestroyBuffer(&fixture.context, pipeline_id) == VA_STATUS_SUCCESS &&
                  DestroyBuffer(&fixture.context, filter_id) == VA_STATUS_SUCCESS &&
                  DestroyContext(&fixture.context, processor) == VA_STATUS_SUCCESS &&
                  DestroyConfig(&fixture.context, config) == VA_STATUS_SUCCESS,
              "only explicit drain emits EOS and every acquired field/output is released once");
    }
  }
}

static void DrainedContextSupportsNewBobCaptures() {
  TeardownFixture fixture(40);  // Exercise the real queued-input drain budget.
  BindTeardownPicturesForVpp(fixture);
  Require(DestroyContext(&fixture.context, 1) == VA_STATUS_SUCCESS &&
              fixture.driver.contexts.count(1) == 0 &&
              fixture.decoder->device == nullptr && fixture.decoder->pending.empty() &&
              fixture.io.inputs.size() == 40 && fixture.io.outputs.empty(),
          "drain every queued tail picture and consume successful decoder close");
  const auto eos = std::find(fixture.io.events.begin(), fixture.io.events.end(), "eos");
  const auto stop = std::find(fixture.io.events.begin(), fixture.io.events.end(), "stop");
  Require(eos != fixture.io.events.end() && eos < stop,
          "real tail EOS precedes the consuming close");
  for (unsigned int index : {0U, 39U}) {
    fixture.CheckPicture(index);
    CheckNewBobAfterTeardown(fixture, index, true);
  }
  Require(!fixture.io.invalid && fixture.io.open_attempts == 0,
          "post-close Bob requires no decoder reopen or new hardware call");
}

static void NewBobRequiresSuccessfulConsumingClose() {
  {
    TeardownFixture fixture(1);
    BindTeardownPicturesForVpp(fixture);
    fixture.io.device_close_busy = 1;
    Require(DestroyContextWithTimeout(&fixture.context, 1, 0) == VA_STATUS_ERROR_HW_BUSY &&
                fixture.decoder->device == fixture.io.handle() && fixture.held[0]->ready,
            "a fully drained picture still has a non-consuming BUSY close");
    CheckNewBobAfterTeardown(fixture, 0, false);
    Require(DestroyContextWithTimeout(&fixture.context, 1, 0) == VA_STATUS_SUCCESS &&
                fixture.decoder->device == nullptr && fixture.io.device_close_calls == 2,
            "only the successful terminal retry consumes decoder ownership");
    CheckNewBobAfterTeardown(fixture, 0, true);
    Require(!fixture.io.invalid, "BUSY retry leaves no invalid DIL calls");
  }
  for (bool destroy_source : {false, true}) {
    TeardownFixture fixture(1);
    BindTeardownPicturesForVpp(fixture);
    // A retained immutable picture must not authorize a stale public binding.
    const auto old_picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    fixture.io.device_close_busy = 1;
    Require(DestroyContextWithTimeout(&fixture.context, 1, 0) == VA_STATUS_ERROR_HW_BUSY &&
                fixture.decoder->device == fixture.io.handle() && old_picture->ready,
            "hold the fully drained immutable picture across non-consuming close");
    CheckNewBobAfterTeardown(fixture, 0, false);
    if (destroy_source) {
      VASurfaceID id = 1;
      Require(DestroySurfaces(&fixture.context, &id, 1) == VA_STATUS_SUCCESS &&
                  fixture.driver.surfaces.count(1) == 0 && fixture.held[0]->destroyed &&
                  fixture.decoder->device == fixture.io.handle(),
              "destroy the public source between BUSY and terminal close without consuming its handle");
    } else {
      auto replacement = std::make_shared<Surface>();
      Require(replacement->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
              "allocate an ordinary VPP replacement source");
      replacement->id = fixture.driver.next_surface++;
      replacement->ready = true;
      for (unsigned int plane = 0; plane != 2; ++plane)
        for (unsigned int row = 0; row != (plane == 0 ? 16U : 8U); ++row)
          memset(replacement->planes[plane] + row * replacement->pitch[plane],
                 plane == 0 ? 80 : 128, 16);
      fixture.driver.surfaces[replacement->id] = replacement;
      VAConfigID config = VA_INVALID_ID;
      VAContextID writer = VA_INVALID_ID;
      VABufferID parameters = VA_INVALID_ID;
      VAProcPipelineParameterBuffer pipeline = {};
      pipeline.surface = replacement->id;
      Require(CreateConfig(&fixture.context, VAProfileNone, VAEntrypointVideoProc,
                           nullptr, 0, &config) == VA_STATUS_SUCCESS &&
                  CreateContext(&fixture.context, config, 16, 16, 0, nullptr, 0,
                                &writer) == VA_STATUS_SUCCESS &&
                  CreateBuffer(&fixture.context, writer, VAProcPipelineParameterBufferType,
                               sizeof(pipeline), 1, &pipeline, &parameters) == VA_STATUS_SUCCESS &&
                  BeginPicture(&fixture.context, writer, 1) == VA_STATUS_SUCCESS &&
                  RenderPicture(&fixture.context, writer, &parameters, 1) == VA_STATUS_SUCCESS &&
                  EndPicture(&fixture.context, writer) == VA_STATUS_SUCCESS &&
                  fixture.held[0]->ready && fixture.held[0]->frame_timestamp == 0 &&
                  fixture.held[0]->expected_timestamp == 0 &&
                  fixture.held[0]->trace_decode_identity == 0 &&
                  fixture.decoder->device == fixture.io.handle(),
              "a real VPP writer replaces the public binding while decoder close is still BUSY");
      Require(DestroyBuffer(&fixture.context, parameters) == VA_STATUS_SUCCESS &&
                  DestroyContext(&fixture.context, writer) == VA_STATUS_SUCCESS &&
                  DestroyConfig(&fixture.context, config) == VA_STATUS_SUCCESS,
              "release the replacement writer without affecting retained decoder ownership");
      VASurfaceID id = replacement->id;
      Require(DestroySurfaces(&fixture.context, &id, 1) == VA_STATUS_SUCCESS,
              "release the separate ordinary replacement input");
    }
    Require(DestroyContextWithTimeout(&fixture.context, 1, 0) == VA_STATUS_SUCCESS &&
                fixture.decoder->device == nullptr && fixture.io.device_close_calls == 2,
            "consume successful close only after the public binding has changed");
    CheckNewBobAfterTeardown(fixture, 0, !destroy_source, 80);
    Require(old_picture->ready && !old_picture->failed &&
                old_picture->expected_timestamp == kTimestampStep &&
                old_picture->frame_timestamp == kTimestampStep &&
                old_picture->planes[0][0] == DecodeIoMock::Luma(kTimestampStep) &&
                old_picture->planes[1][0] == 128 && !fixture.io.invalid &&
                (destroy_source || fixture.held[0]->decode_picture.lock() != old_picture),
            "terminal close never republishes a destroyed/reused tuple or mutates an independently held picture");
  }
  {
    TeardownFixture fixture(1);
    BindTeardownPicturesForVpp(fixture);
    const auto old_picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    const auto source = fixture.held[0];
    fixture.io.device_close_busy = 1;
    Require(DestroyContextWithTimeout(&fixture.context, 1, 0) == VA_STATUS_ERROR_HW_BUSY &&
                source->ready && old_picture->ready,
            "retain completed pixels while their original close remains BUSY");
    const auto pixels = source->storage;
    const uint64_t identity = source->decode_identity;
    auto next_decoder = std::make_shared<DecodeContext>();
    next_decoder->id = next_decoder->config = 99;
    next_decoder->profile = VAProfileH264Main;
    fixture.driver.configs[99] = {VAProfileH264Main, VAEntrypointVLD};
    fixture.driver.contexts[99] = next_decoder;
    Require(BeginPicture(&fixture.context, 99, 1) == VA_STATUS_SUCCESS &&
                EndPicture(&fixture.context, 99) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                PublicReadRejected(source.get()) && source->ready && !source->failed &&
                source->decode_identity == identity && source->storage == pixels,
            "a real malformed H264 End rejects public reads without replacing accepted identity or pixels");
    const uint64_t rejected_transaction = source->rejected_picture_transaction;
    Require(BeginPicture(&fixture.context, 99, 1) == VA_STATUS_SUCCESS &&
                !PublicReadRejected(source.get()) && rejected_transaction != 0 &&
                source->picture_transaction != rejected_transaction &&
                source->rejected_picture_transaction == rejected_transaction &&
                source->expected_timestamp == kTimestampStep &&
                source->frame_timestamp == kTimestampStep &&
                source->decode_identity == identity && source->trace_decode_identity == identity &&
                source->trace_context == 1 && source->trace_generation == fixture.decoder->generation &&
                source->trace_submission_ordinal == 1 && source->storage == pixels &&
                fixture.io.inputs == std::vector<uint64_t>{kTimestampStep} &&
                fixture.io.open_attempts == 0,
            "a second speculative Begin supersedes the current rejection marker, not its rejection history");
    Require(DestroyContextWithTimeout(&fixture.context, 1, 0) == VA_STATUS_SUCCESS &&
                fixture.decoder->device == nullptr && fixture.io.device_close_calls == 2,
            "consume the old successful close after a rejected then superseded public transaction");
    CheckNewBobAfterTeardown(fixture, 0, false);
    Require(old_picture->ready && !old_picture->failed &&
                old_picture->frame_timestamp == kTimestampStep &&
                old_picture->planes[0][0] == DecodeIoMock::Luma(kTimestampStep) &&
                source->storage == pixels && fixture.io.inputs.size() == 1 &&
                fixture.io.open_attempts == 0 && !fixture.io.invalid &&
                DestroyContext(&fixture.context, 99) == VA_STATUS_SUCCESS &&
                DestroyConfig(&fixture.context, 99) == VA_STATUS_SUCCESS,
            "rejection history prevents stale republication without changing previously held immutable pixels");
  }
  for (unsigned int failure = 0; failure != 5; ++failure) {
    TeardownFixture fixture(1);
    BindTeardownPicturesForVpp(fixture);
    if (failure == 0) fixture.io.device_close_status = BC_STS_ERROR;
    if (failure == 1) fixture.io.stop_status = BC_STS_ERROR;
    if (failure == 2) fixture.io.decoder_close_status = BC_STS_ERROR;
    if (failure == 3) fixture.io.early_eos = true;
    if (failure == 4) fixture.io.fail_poll = true;
    Require(DestroyContextWithTimeout(&fixture.context, 1, 0) ==
                    (failure < 3 ? VA_STATUS_ERROR_OPERATION_FAILED
                                 : VA_STATUS_ERROR_DECODING_ERROR) &&
                fixture.decoder->device == nullptr && fixture.driver.contexts.count(1) == 0,
            "consumed lifecycle/drain failure is not a successful completion lease");
    CheckNewBobAfterTeardown(fixture, 0, false);
    Require(!fixture.io.invalid, "failed close never invents a new decoder handle");
  }
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
                              return PendingVppCanceled(&fixture.driver,
                                                       operation);
                            }) == VA_STATUS_ERROR_OPERATION_FAILED,
          "completed pixels must not revive a canceled VPP epoch");
  Require(operation.source->ready && fixture.decoder->retired,
          "VPP cancellation must win even over actual completed pixels");
}

static void WaitingVppKeepsCompletedRetiredPicture() {
  TeardownFixture fixture(1);
  BindTeardownPicturesForVpp(fixture);
  PendingVpp operation;
  operation.decoder = fixture.decoder;
  operation.decoder_generation = fixture.decoder->generation;
  operation.source_owner = fixture.held[0];
  operation.source = fixture.decoder->decoded_frames.at(kTimestampStep);
  operation.target = fixture.held[0];
  operation.target_owner = fixture.held[0];
  operation.sequence = operation.target_owner->latest_vpp_sequence;
  fixture.io.on_sleep = [&] {
    Require(DestroyContext(&fixture.context, 1) == VA_STATUS_SUCCESS,
            "complete and successfully consume the captured VPP decoder");
    Require(fixture.held[0]->completed_decode_picture.picture ==
                operation.source,
            "successful close publishes the waiting operation's exact picture");
    Require(CapturedDecodePictureMatchesLifetime(
                &fixture.driver, fixture.held[0], operation.source,
                fixture.decoder, operation.decoder_generation),
            "published close lease validates the waiting decoder epoch");
    Require(!PendingVppCanceled(&fixture.driver, operation),
            "exact successful-close lease keeps the waiting VPP alive");
  };
  std::unique_lock<std::mutex> lock(fixture.driver.mutex);
  Require(SyncDecodeSurface(&fixture.driver, operation.source, &lock,
                            VA_TIMEOUT_INFINITE, [&] {
                              return PendingVppCanceled(&fixture.driver,
                                                       operation);
                            }) == VA_STATUS_SUCCESS,
          "waiting VPP accepts the exact lease published by successful close");
  Require(operation.source->ready && !operation.source->failed &&
              fixture.decoder->retired &&
              !PendingVppCanceled(&fixture.driver, operation) &&
              fixture.held[0]->completed_decode_picture.picture ==
                  operation.source,
          "successful close retains the same owner, pixels, identity and epoch");
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

static void OutputEventReplacesPeriodicWorkerPolling() {
  TeardownFixture fixture(1);
  const int readiness = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  Require(readiness >= 0, "allocate mocked decoder readiness event");
  fixture.io.readiness_event = readiness;
  fixture.decoder->output_poll =
      std::make_shared<OutputPollLease>(readiness);
  fixture.driver.StartDecodeWorker();

  const auto input_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  unsigned int idle_polls = 0;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      if (fixture.io.inputs.size() == 1 && fixture.io.output_polls != 0) {
        idle_polls = fixture.io.output_polls;
        break;
      }
    }
    Require(std::chrono::steady_clock::now() < input_deadline,
            "event worker submits initial compressed input");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  {
    std::lock_guard<std::mutex> lock(fixture.driver.mutex);
    Require(fixture.io.output_polls == idle_polls && !fixture.held[0]->ready,
            "event worker performs no timer-driven READY probes while idle");
    fixture.io.outputs.push_back(kTimestampStep);
  }
  const uint64_t one = 1;
  Require(write(readiness, &one, sizeof(one)) ==
              static_cast<ssize_t>(sizeof(one)),
          "signal mocked decoder readiness");

  const auto output_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      if (fixture.held[0]->ready)
        break;
    }
    Require(std::chrono::steady_clock::now() < output_deadline,
            "event worker publishes signaled output");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::lock_guard<std::mutex> lock(fixture.driver.mutex);
  Require(fixture.io.output_polls == idle_polls + 1 &&
              fixture.decoder->pending.empty() &&
              fixture.io.outputs.empty() && !fixture.io.invalid,
          "one readiness signal causes one bounded output harvest");
}

static void DecodeControlWakeUsesExactArmedOutputLease() {
  DecodeContext decode;
  const int first_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  const int second_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  Require(first_fd >= 0 && second_fd >= 0,
          "allocate exact-lease control-wake fixtures");
  const auto first = std::make_shared<OutputPollLease>(first_fd);
  const auto second = std::make_shared<OutputPollLease>(second_fd);

  decode.output_poll = first;
  Require(NeedsDecodeControlWake(decode),
          "an unarmed decoder still needs a control wake");
  decode.armed_output_poll = first;
  Require(!NeedsDecodeControlWake(decode),
          "the exact armed output lease suppresses a redundant control wake");

  decode.input_backpressured = true;
  Require(NeedsDecodeControlWake(decode),
          "TX backpressure retains its bounded control wake");
  decode.input_backpressured = false;
  decode.input_budget_exhausted = true;
  Require(NeedsDecodeControlWake(decode),
          "an exhausted input turn retains its control wake");
  decode.input_budget_exhausted = false;

  decode.output_poll = second;
  Require(NeedsDecodeControlWake(decode),
          "a reopened output descriptor cannot inherit a stale armed lease");
  decode.armed_output_poll = second;
  Require(!NeedsDecodeControlWake(decode),
          "the replacement lease suppresses only after it is armed");

  decode.profile = VAProfileMPEG4AdvancedSimple;
  Require(decode.mpeg4_replay.Append(
              kTimestampStep, CrystalHDMpeg4Replay::Kind::I, 0, 0,
              {0, 0, 1, 0xb6}) &&
              decode.mpeg4_replay.NextInput() != nullptr &&
              decode.mpeg4_replay.InputSent() &&
              decode.mpeg4_replay.Seal() &&
              decode.mpeg4_replay.Observe(kTimestampStep) ==
                  CrystalHDMpeg4Replay::Output::New &&
              NeedsMpeg4EosProbe(decode) && NeedsDecodeControlWake(decode),
          "state-only MPEG-4 EOS retains its bounded control wake");

  Require(decode.CloseHardware() == BC_STS_SUCCESS &&
              !decode.output_poll && decode.armed_output_poll.expired(),
          "hardware close clears both current and armed output leases");
}

static void DecodeControlDrainConsumesOneEventfdRead() {
  const int descriptor = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  Require(descriptor >= 0, "allocate control-drain fixture");
  const uint64_t one = 1;
  Require(write(descriptor, &one, sizeof(one)) ==
              static_cast<ssize_t>(sizeof(one)) &&
              write(descriptor, &one, sizeof(one)) ==
                  static_cast<ssize_t>(sizeof(one)),
          "coalesce two pending control notifications");

  control_read_probe_fd = descriptor;
  control_read_probe_calls = 0;
  DrainDecodeControlFd(descriptor);
  control_read_probe_fd = -1;
  Require(control_read_probe_calls == 1,
          "one eventfd read drains the coalesced control counter");

  uint64_t value = 0;
  errno = 0;
  Require(read(descriptor, &value, sizeof(value)) < 0 && errno == EAGAIN,
          "the single successful read leaves no latched control event");
  Require(close(descriptor) == 0, "close control-drain fixture");
}

static void TerminalCloseDoesNotBlockReadySync() {
  for (const auto &test : {std::pair<const char *, unsigned>{"stop", 0},
                           {"close-decoder", 0}, {"close-device", 0},
                           {"close-device", 1}, {"close-device", 2}}) {
    TeardownFixture fixture(1);
    fixture.driver.StopWorkers();
    fixture.driver.stopping = false;
    fixture.decoder->profile = VAProfileH264Main;
    fixture.decoder->live_h264 = fixture.decoder->auto_h264_prefeed = true;
    fixture.decoder->next_timestamp = 2 * kTimestampStep;
    fixture.decoder->replay = CrystalHDDecodeReplay(true);
    Require(fixture.decoder->replay.Append(kTimestampStep, true,
                {0, 0, 1, 0x65}, true),
            "retain the final ordinary AU with immutable automatic-prefeed eligibility");
    fixture.driver.configs[1] = {VAProfileH264Main, VAEntrypointVLD};
    BindTeardownPicturesForVpp(fixture);
    Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) == VA_STATUS_SUCCESS,
            "submit the exact terminal-close picture");
    fixture.io.outputs.push_back(kTimestampStep);
    Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) == VA_STATUS_SUCCESS &&
                fixture.decoder->pending.empty() && fixture.held[0]->ready &&
                fixture.decoder->prefed_timestamp == 2 * kTimestampStep &&
                fixture.decoder->pending_prefeed_timestamp == 0 &&
                fixture.decoder->replay.outstanding() == 0 &&
                fixture.decoder->replay.NextInput() == nullptr &&
                fixture.decoder->decoded_frames.size() == 1 &&
                !fixture.decoder->auxiliary.ever &&
                fixture.io.input_calls.size() == 2,
            "all real pixels complete while the actual pump's final unbound AUD prefix remains");
    const auto owner = fixture.held[0];
    const auto picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    const auto pixels = owner->storage;
    const uint64_t generation = fixture.decoder->generation;
    if (test.second == 1) fixture.io.device_close_busy = 1;
    if (test.second == 2) fixture.io.device_close_status = BC_STS_IO_ERROR;
    TerminalCloseBarrier barrier(test.first);
    fixture.io.on_close = [&](const char *stage) { barrier.Block(stage); };
    struct Join {
      TerminalCloseBarrier *barrier;
      DecodeIoMock *io;
      std::thread thread;
      ~Join() {
        barrier->Release();
        if (thread.joinable()) thread.join();
        io->on_close = {};
      }
    } cleanup{&barrier, &fixture.io, {}};
    VAStatus status = VA_STATUS_ERROR_OPERATION_FAILED;
    cleanup.thread = std::thread([&] {
      status = DestroyContextWithTimeout(&fixture.context, 1, 0);
    });
    Require(barrier.Wait(), "terminal DIL call reaches its controlled barrier");
    const unsigned polls = fixture.io.output_polls;
    const size_t inputs = fixture.io.input_calls.size();
    Require(SyncSurface(&fixture.context, 1) == VA_STATUS_SUCCESS &&
                SyncSurface2(&fixture.context, 1, 0) == VA_STATUS_SUCCESS,
            "ready blocking and zero-timeout sync both finish while terminal DIL close is blocked");
    {
      std::lock_guard<std::mutex> lock(barrier.mutex);
      Require(!barrier.released && !barrier.expired,
              "ready sync finishes before the DIL close barrier is released");
    }
    {
      std::unique_lock<std::mutex> lock(fixture.driver.mutex);
      Require(fixture.decoder->closing && fixture.decoder->destroy_prepared &&
                  fixture.decoder->destroy_in_progress && !fixture.decoder->retired &&
                  CapturedDecodePictureMatchesLifetime(&fixture.driver, owner, picture,
                                                       fixture.decoder, generation),
              "immutable captures stay live without publishing a post-close lease early");
      // Fault-model a retained/resumed cold waiter after close admission. It
      // must not poll the handle that the other thread is currently closing.
      picture->ready = false;
      const auto resumed = DecodeWaitState(*fixture.decoder, picture.get(), generation,
                                           kTimestampStep, {});
      const auto cold = SyncDecodeSurface(&fixture.driver, picture, &lock,
                                          VA_TIMEOUT_INFINITE);
      picture->ready = true;
      Require(resumed == VA_STATUS_ERROR_INVALID_CONTEXT &&
                  cold == VA_STATUS_ERROR_INVALID_CONTEXT,
              "prepared terminal close excludes new and resumed nonready hardware polling");
    }
    Require(DestroyContextWithTimeout(&fixture.context, 1, 0) == VA_STATUS_ERROR_INVALID_CONTEXT &&
                BeginPicture(&fixture.context, 1, 1) == VA_STATUS_ERROR_INVALID_CONTEXT,
            "a second destroy and new decode work cannot enter the closing context");
    VAContextID another = VA_INVALID_ID;
    Require(CreateContext(&fixture.context, 1, 16, 16, 0, nullptr, 0, &another) ==
                  VA_STATUS_ERROR_HW_BUSY && another == VA_INVALID_ID &&
                fixture.io.open_attempts == 0 && fixture.io.output_polls == polls &&
                fixture.io.input_calls.size() == inputs,
            "the old handle remains reserved and ready/cold sync performs no DIL work");
    barrier.Release();
    cleanup.thread.join();
    const VAStatus expected = test.second == 1 ? VA_STATUS_ERROR_HW_BUSY :
        test.second == 2 ? VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS;
    Require(status == expected && fixture.decoder->retired &&
                owner->storage == pixels && !barrier.expired && !fixture.io.invalid,
            "terminal outcome retains the exact original pixels and old BUSY/error status");
    Require(CapturedDecodePictureMatchesLifetime(&fixture.driver, owner, picture,
                fixture.decoder, generation) == (test.second == 0),
            "only consuming success publishes the exact completed-picture lease");
    if (test.second == 1) {
      Require(fixture.decoder->device == fixture.io.handle() &&
                  fixture.driver.contexts.count(1) == 1 &&
                  !fixture.decoder->CanYieldTerminalClose() &&
                  DestroyContextWithTimeout(&fixture.context, 1, 0) == VA_STATUS_SUCCESS &&
                  fixture.io.device_close_calls == 2 &&
                  CapturedDecodePictureMatchesLifetime(&fixture.driver, owner, picture,
                                                       fixture.decoder, generation),
              "retired non-consuming BUSY keeps the original retry and later exact lease");
    }
  }
}

static void TerminateJoinsAnUnlockedTerminalDestroy() {
  DecodeIoMock io;
  MockDecodeScope scope(io);
  VADriverContext context = {};
  context.pDriverData = new Driver(-1);
  Driver *driver = GetDriver(&context);
  driver->StopWorkers();
  driver->stopping = false;
  auto decode = std::make_shared<DecodeContext>();
  decode->id = 1;
  decode->profile = VAProfileH264Main;
  decode->width = decode->height = 16;
  decode->device = io.handle();
  decode->decoder_open = decode->decoder_started = true;
  auto picture = std::make_shared<Surface>();
  auto owner = std::make_shared<Surface>();
  for (const auto &frame : {picture, owner}) {
    Require(frame->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
            "allocate terminate/destroy concurrency pixels");
    frame->expected_timestamp = kTimestampStep;
    frame->decode_identity = frame->trace_decode_identity = 1;
    frame->trace_context = frame->trace_generation = frame->trace_submission_ordinal = 1;
  }
  owner->id = 1;
  owner->decode_picture = picture;
  driver->surfaces[1] = owner;
  driver->contexts[1] = decode;
  decode->pending[kTimestampStep] = 1;
  decode->decoded_frames[kTimestampStep] = picture;
  decode->surface_timestamps[owner.get()] = kTimestampStep;
  Require(decode->replay.Append(kTimestampStep, true, {1}) &&
              PumpDecodeInput(driver, decode.get()) == VA_STATUS_SUCCESS,
          "admit the exact picture before concurrent terminate");
  io.outputs.push_back(kTimestampStep);
  Require(ReceiveAvailable(driver, decode.get()) == VA_STATUS_SUCCESS && owner->ready,
          "complete all pixels before the held terminal close");
  const auto pixels = owner->storage;
  TerminalCloseBarrier barrier("close-device");
  io.on_close = [&](const char *stage) { barrier.Block(stage); };
  struct Join {
    VADriverContext *context;
    TerminalCloseBarrier *barrier;
    std::thread destroyer, terminator;
    ~Join() {
      barrier->Release();
      if (destroyer.joinable()) destroyer.join();
      if (terminator.joinable()) terminator.join();
      if (context->pDriverData) Terminate(context);
    }
  } cleanup{&context, &barrier, {}, {}};
  VAStatus destroyed = VA_STATUS_ERROR_OPERATION_FAILED;
  VAStatus terminated = VA_STATUS_ERROR_OPERATION_FAILED;
  cleanup.destroyer = std::thread([&] { destroyed = DestroyContext(&context, 1); });
  Require(barrier.Wait(), "active destroy releases the driver mutex only inside DeviceClose");
  std::atomic<bool> terminate_done{false};
  cleanup.terminator = std::thread([&] {
    terminated = Terminate(&context);
    terminate_done = true;
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool terminate_inside = false;
  while (!terminate_inside && std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(driver->mutex);
      terminate_inside = driver->stopping;
    }
    std::this_thread::yield();
  }
  Require(terminate_inside,
          "concurrent Terminate enters StopWorkers while DeviceClose is still blocked");
  const bool returned_early = terminate_done;
  barrier.Release();
  cleanup.destroyer.join();
  cleanup.terminator.join();
  Require(!returned_early && destroyed == VA_STATUS_SUCCESS && terminated == VA_STATUS_SUCCESS &&
              context.pDriverData == nullptr && io.device_close_calls == 1 &&
              owner->storage == pixels && !barrier.expired && !io.invalid,
          "Terminate joins the active destroy before one consuming close and driver destruction");
}

static void ContextDestroyWaitsForBorrowedOutputPoll() {
  for (bool zero_timeout : {false, true}) {
    TeardownFixture fixture(1);
    const auto captured_owner = fixture.held[0];
    const auto captured_picture = fixture.decoder->decoded_frames.at(kTimestampStep);
    const uint64_t captured_generation = fixture.decoder->generation;
    const int descriptor = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    Require(descriptor >= 0, "allocate borrowed output-poll fixture");
    fixture.decoder->output_poll = std::make_shared<OutputPollLease>(descriptor);
    WorkerPollBarrier barrier;
    barrier.descriptor = descriptor;
    struct PollJoin {
      Driver *driver;
      WorkerPollBarrier *barrier;
      std::thread destroyer;
      ~PollJoin() {
        barrier->Release();
        driver->NotifyDecodeWorker();
        if (destroyer.joinable()) destroyer.join();
        driver->StopWorkers();
        worker_poll_barrier = nullptr;
      }
    } cleanup{&fixture.driver, &barrier, {}};
    worker_poll_barrier = &barrier;
    fixture.driver.StartDecodeWorker();
    {
      std::unique_lock<std::mutex> lock(barrier.mutex);
      Require(barrier.condition.wait_for(lock, std::chrono::seconds(2),
                                         [&] { return barrier.entered; }),
              "the actual decode worker borrows its fd before close begins");
    }
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      Require(fixture.decoder->output_poll_inflight == 1 &&
                  fixture.io.inputs.size() == 1 && fixture.io.outputs.empty(),
              "one actual poll borrow and accepted input are controlled");
      // Complete the actual accepted picture while the worker is outside the
      // mutex. Teardown therefore has no unresolved tail or EOS to invent.
      fixture.io.outputs.push_back(kTimestampStep);
      Require(ReceiveAvailable(&fixture.driver, fixture.decoder.get()) ==
                      VA_STATUS_SUCCESS && fixture.held[0]->ready &&
                  fixture.decoder->pending.empty(),
              "exact mock output completes before terminal close admission");
    }
    if (zero_timeout) {
      const VAStatus first = DestroyContextWithTimeout(&fixture.context, 1, 0);
      bool retained;
      {
        std::lock_guard<std::mutex> lock(fixture.driver.mutex);
        retained = fixture.driver.contexts.count(1) == 1 &&
            fixture.decoder->output_poll_inflight == 1 &&
            fixture.decoder->device == fixture.io.handle() &&
            fixture.decoder->output_poll != nullptr &&
            fixture.io.device_close_calls == 0 &&
            !fixture.decoder->retired && captured_picture->ready &&
            CapturedDecodePictureMatchesLifetime(&fixture.driver, captured_owner,
                captured_picture, fixture.decoder, captured_generation) &&
            fcntl(descriptor, F_GETFD) >= 0;
      }
      barrier.Release();
      {
        std::unique_lock<std::mutex> lock(fixture.driver.mutex);
        Require(fixture.driver.condition.wait_for(lock, std::chrono::seconds(2),
                    [&] { return fixture.decoder->output_poll_inflight == 0; }),
                "the released worker acknowledges its exact poll borrow");
      }
      const VAStatus retried = first == VA_STATUS_ERROR_HW_BUSY
          ? DestroyContext(&fixture.context, 1) : VA_STATUS_ERROR_INVALID_CONTEXT;
      errno = 0;
      const bool closed_at_return = fcntl(descriptor, F_GETFD) == -1 && errno == EBADF;
      Require(first == VA_STATUS_ERROR_HW_BUSY && retained &&
                  retried == VA_STATUS_SUCCESS && closed_at_return,
              "a zero-deadline destroy retains poll ownership for a consuming retry");
    } else {
      cleanup.destroyer = std::thread([&] {
        const VAStatus status = DestroyContext(&fixture.context, 1);
        errno = 0;
        const bool closed = fcntl(descriptor, F_GETFD) == -1 && errno == EBADF;
        {
          std::lock_guard<std::mutex> lock(barrier.mutex);
          barrier.destroy_status = status;
          barrier.descriptor_closed = closed;
          barrier.destroy_done = true;
        }
        barrier.condition.notify_all();
      });
      {
        std::unique_lock<std::mutex> lock(fixture.driver.mutex);
        Require(fixture.driver.condition.wait_for(lock, std::chrono::seconds(2),
                    [&] { return fixture.decoder->closing; }),
                "actual context close begins before testing borrower ordering");
        Require(!fixture.decoder->retired && captured_picture->ready &&
                    CapturedDecodePictureMatchesLifetime(&fixture.driver,
                        captured_owner, captured_picture, fixture.decoder,
                        captured_generation),
                "borrower quiescence preserves the actual captured picture lifetime");
      }
      Require(DestroyContextWithTimeout(&fixture.context, 1, 0) ==
                  VA_STATUS_ERROR_INVALID_CONTEXT,
              "a concurrent destroy cannot consume another caller's active close");
      bool premature;
      {
        std::unique_lock<std::mutex> lock(barrier.mutex);
        premature = barrier.condition.wait_for(lock, std::chrono::milliseconds(20),
                                               [&] { return barrier.destroy_done; });
      }
      barrier.Release();
      {
        std::unique_lock<std::mutex> lock(barrier.mutex);
        Require(barrier.condition.wait_for(lock, std::chrono::seconds(2),
                                           [&] { return barrier.destroy_done; }),
                "terminal destroy finishes after the poll borrower releases");
      }
      cleanup.destroyer.join();
      Require(!premature && barrier.destroy_status == VA_STATUS_SUCCESS &&
                  barrier.descriptor_closed,
              "successful public destroy cannot precede the last borrowed fd close");
    }
    fixture.driver.StopWorkers();
    errno = 0;
    Require(!barrier.expired && fcntl(descriptor, F_GETFD) == -1 && errno == EBADF &&
                fixture.decoder->output_poll_inflight == 0 &&
                fixture.decoder->device == nullptr && !fixture.decoder->output_poll &&
                fixture.driver.contexts.count(1) == 0 &&
                fixture.io.device_close_calls == 1 && !fixture.io.invalid,
            "terminal close retires the borrowed descriptor exactly once without hardware");
  }
}

static void BudgetedWorkerReleasesDriverMutex() {
  constexpr unsigned int kPictures = 24;
  TeardownFixture fixture(kPictures);
  fixture.io.input_delay = std::chrono::milliseconds(1);
  const int readiness = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  Require(readiness >= 0, "allocate budget-yield readiness event");
  fixture.io.readiness_event = readiness;
  fixture.decoder->output_poll =
      std::make_shared<OutputPollLease>(readiness);

  unsigned int inputs_seen_with_mutex = kPictures;
  std::thread frontend([&] {
    while (fixture.io.accepted_inputs.load(std::memory_order_acquire) < 4)
      std::this_thread::yield();
    std::lock_guard<std::mutex> lock(fixture.driver.mutex);
    inputs_seen_with_mutex =
        fixture.io.accepted_inputs.load(std::memory_order_relaxed);
  });
  fixture.driver.StartDecodeWorker();
  frontend.join();

  Require(inputs_seen_with_mutex < kPictures,
          "bounded input backlog yields the VA mutex before draining every picture");
}

static void Mpeg4StateOnlyEosStillWakesEventWorker() {
  TeardownFixture fixture(1);
  fixture.decoder->profile = VAProfileMPEG4AdvancedSimple;
  fixture.decoder->stream_width = fixture.decoder->stream_height = 16;
  Require(fixture.decoder->mpeg4_replay.Append(
              kTimestampStep, CrystalHDMpeg4Replay::Kind::I, 0, 0,
              {0, 0, 1, 0xb6}),
          "retain one MPEG-4 epoch for state-only EOS");
  Require(PumpDecodeInput(&fixture.driver, fixture.decoder.get()) ==
                  VA_STATUS_SUCCESS &&
              fixture.decoder->mpeg4_replay.Seal(),
          "submit and seal the MPEG-4 state-only EOS fixture");
  fixture.io.outputs.push_back(kTimestampStep);
  fixture.io.eos_after_queries = 2;
  const int readiness = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  Require(readiness >= 0, "allocate MPEG-4 EOS readiness event");
  fixture.io.readiness_event = readiness;
  fixture.decoder->output_poll =
      std::make_shared<OutputPollLease>(readiness);
  const uint64_t one = 1;
  Require(write(readiness, &one, sizeof(one)) ==
              static_cast<ssize_t>(sizeof(one)),
          "signal final MPEG-4 picture readiness");
  fixture.driver.StartDecodeWorker();

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      if (fixture.decoder->mpeg4_replay.ended()) {
        Require(fixture.held[0]->ready && fixture.io.eos_queries >= 2 &&
                    !fixture.io.invalid,
                "state-only MPEG-4 EOS completes after a bounded event-path probe");
        break;
      }
    }
    Require(std::chrono::steady_clock::now() < deadline,
            "event worker cannot stall on state-only MPEG-4 EOS");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

static void OutputPollHangupFailsWithoutSpinning() {
  TeardownFixture fixture(1);
  int descriptors[2] = {-1, -1};
  Require(pipe(descriptors) == 0, "allocate mocked decoder hangup pipe");
  fixture.decoder->output_poll =
      std::make_shared<OutputPollLease>(descriptors[0]);
  fixture.driver.StartDecodeWorker();

  const auto input_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      if (fixture.io.inputs.size() == 1 && fixture.io.output_polls != 0)
        break;
    }
    Require(std::chrono::steady_clock::now() < input_deadline,
            "hangup worker submits initial compressed input");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Require(close(descriptors[1]) == 0, "signal mocked decoder poll hangup");

  const auto failure_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  unsigned int failed_polls = 0;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(fixture.driver.mutex);
      if (fixture.decoder->ReplayFailed()) {
        failed_polls = fixture.io.output_polls;
        Require(fixture.held[0]->failed,
                "poll hangup fails its pending presentation surface");
        break;
      }
    }
    Require(std::chrono::steady_clock::now() < failure_deadline,
            "poll hangup becomes a bounded decode failure");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::lock_guard<std::mutex> lock(fixture.driver.mutex);
  Require(fixture.io.output_polls == failed_polls,
          "terminal output poll cannot leave a hot-spinning worker");
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

struct NeutralSyntaxReader {
  std::vector<uint8_t> bytes;
  size_t bit = 0;
  uint32_t Bits(unsigned count) {
    Require(count <= 32 && bit + count <= bytes.size() * 8, "neutral syntax bounds");
    uint32_t value = 0;
    while (count--) { value = (value << 1) | ((bytes[bit / 8] >> (7 - bit % 8)) & 1); ++bit; }
    return value;
  }
  uint32_t UE() {
    unsigned zeros = 0;
    while (!Bits(1)) Require(++zeros < 31, "bounded neutral Exp-Golomb");
    return ((1U << zeros) - 1) + Bits(zeros);
  }
  void End() {
    Require(Bits(1) == 1, "RBSP stop bit");
    while (bit < bytes.size() * 8) Require(Bits(1) == 0, "RBSP zero padding");
  }
};

static std::vector<std::pair<uint8_t, NeutralSyntaxReader>> NeutralParseNals(
    const std::vector<uint8_t> &stream) {
  std::vector<size_t> starts;
  for (size_t i = 0; i + 4 < stream.size(); ++i)
    if (!stream[i] && !stream[i+1] && !stream[i+2] && stream[i+3] == 1) starts.push_back(i);
  Require(starts.size() == 3 && starts.front() == 0, "only AUD/PPS/nonref slice");
  std::vector<std::pair<uint8_t, NeutralSyntaxReader>> result;
  for (size_t n = 0; n < starts.size(); ++n) {
    NeutralSyntaxReader reader;
    unsigned zeros = 0;
    const size_t end = n + 1 < starts.size() ? starts[n+1] : stream.size();
    for (size_t i = starts[n] + 5; i < end; ++i) {
      if (zeros == 2 && stream[i] == 3) {
        Require(i + 1 < end && stream[i+1] <= 3, "valid emulation prevention");
        zeros = 0; continue;
      }
      reader.bytes.push_back(stream[i]);
      zeros = stream[i] == 0 ? zeros + 1 : 0;
    }
    result.emplace_back(stream[starts[n]+4], std::move(reader));
  }
  return result;
}

static VAPictureParameterBufferH264 NeutralPicture(unsigned width, unsigned height) {
  VAPictureParameterBufferH264 p{};
  p.picture_width_in_mbs_minus1 = width / 16 - 1;
  p.picture_height_in_mbs_minus1 = height / 16 - 1;
  p.seq_fields.bits.chroma_format_idc = 1;
  p.seq_fields.bits.mb_adaptive_frame_field_flag = 1;
  p.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 2;
  p.frame_num = 7; p.CurrPic.TopFieldOrderCnt = 18;
  return p;
}

static void NeutralAuxSyntaxIsSelfContained() {
  for (const auto &size : {std::make_pair(16U,32U), {1280U,736U}, {1920U,1088U}}) {
    const auto p = NeutralPicture(size.first, size.second);
    std::vector<uint8_t> stream;
    Require(BuildH264NeutralIntra(p, &stream) && stream.size() <= 16384, "bounded variable geometry");
    auto nals = NeutralParseNals(stream);
    Require(nals[0].first == 9 && nals[1].first == 0x68 && nals[2].first == 1,
            "AUD, reserved PPS and nonreference nonIDR I only; no SPS");
    auto &a = nals[0].second; Require(a.Bits(3) == 0, "AUD intra type"); a.End();
    auto &pps = nals[1].second;
    Require(pps.UE() == 254 && pps.UE() == 0 && pps.Bits(2) == 0 &&
                pps.UE() == 0 && pps.UE() == 0 && pps.UE() == 0 &&
                pps.Bits(3) == 0 && pps.UE() == 0 && pps.UE() == 0 && pps.UE() == 0 &&
                pps.Bits(3) == 4, "PPS CAVLC no groups/default QP/no weighted prediction");
    pps.End();
    auto &slice = nals[2].second;
    Require(slice.UE() == 0 && slice.UE() == 2 && slice.UE() == 254 &&
                slice.Bits(4) == 7 && slice.Bits(1) == 0 && slice.Bits(6) == 18 &&
                slice.UE() == 0 && slice.UE() == 1, "exact frame_num/POC frame slice header");
    const unsigned count = size.first / 16 * (size.second / 16);
    for (unsigned mb = 0; mb < count; ++mb) {
      if (!(mb & 1)) Require(slice.Bits(1) == 0, "MBAFF frame-coded pair");
      Require(slice.UE() == 3 && slice.UE() == 0 && slice.UE() == 0 && slice.Bits(1) == 1,
              "I16 DC zero residual for every macroblock");
    }
    slice.End();
  }
  for (unsigned fault = 0; fault < 14; ++fault) {
    auto p = NeutralPicture(1920, 1088);
    switch (fault) {
      case 0: p.picture_width_in_mbs_minus1 = 120; break;
      case 1: p.picture_height_in_mbs_minus1 = 66; break;
      case 2: p.bit_depth_luma_minus8 = 1; break;
      case 3: p.bit_depth_chroma_minus8 = 1; break;
      case 4: p.seq_fields.bits.chroma_format_idc = 2; break;
      case 5: p.seq_fields.bits.residual_colour_transform_flag = 1; break;
      case 6: p.seq_fields.bits.frame_mbs_only_flag = 1; break;
      case 7: p.seq_fields.bits.mb_adaptive_frame_field_flag = 0; break;
      case 8: p.seq_fields.bits.pic_order_cnt_type = 1; break;
      case 9: p.pic_fields.bits.field_pic_flag = 1; break;
      case 10: p.seq_fields.bits.log2_max_frame_num_minus4 = 13; break;
      case 11: p.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 13; break;
      case 12: p.frame_num = 16; break;
      case 13: p.CurrPic.TopFieldOrderCnt = -1; break;
    }
    std::vector<uint8_t> rejected;
    Require(!BuildH264NeutralIntra(p, &rejected), "unsupported neutral syntax rejected");
  }
}
constexpr uint64_t kNeutralAuxToken = 2 * kTimestampStep;

static void NeutralAuxBegin(H264WeaveFixture *fixture, DecodeIoMock *io) {
  auto &decode = fixture->decoder;
  decode.device = io->handle();
  decode.auxiliary.ever = decode.auxiliary.pending = decode.auxiliary.needs_eos = true;
  decode.auxiliary.timestamp = kNeutralAuxToken;
  decode.auxiliary.generation = decode.generation;
  decode.auxiliary.device = decode.device;
  fixture->output.PicInfo.timeStamp = kNeutralAuxToken;
  fixture->output.PicInfo.flags &= ~VDEC_FLAG_UNKNOWN_SRC;
  fixture->raw.timeStamp = kNeutralAuxToken / 20000U;
  fixture->raw.flags = VDEC_FLAG_FIELDPAIR;
  fixture->WritePib();
  Require(ClassifyH264InterlacedPayload(fixture->output) == H264InterlacedPayload::Weave,
          "auxiliary fixture has complete PIB and exact converted timestamp");
  io->override_output = true;
  io->events.reserve(16);
}

static VAStatus NeutralAuxReceive(H264WeaveFixture *fixture, DecodeIoMock *io) {
  io->overridden_output = fixture->output;
  bool activity = false;
  const VAStatus status = ReceiveOne(&fixture->driver, &fixture->decoder, 0, &activity);
  Require(activity && !io->invalid, "one owned mocked auxiliary output is acquired");
  return status;
}

static void NeutralAuxWeavePreservesHeldOriginal() {
  for (bool direct : {false, true}) {
    for (bool bottom_first : {false, true}) {
      DecodeIoMock io;
      MockDecodeScope scope(io);
      H264WeaveFixture fixture(direct);
      auto first = fixture.Output(bottom_first, bottom_first,
          bottom_first ? &fixture.bottom : &fixture.top);
      auto second = fixture.Output(!bottom_first, bottom_first,
          bottom_first ? &fixture.top : &fixture.bottom);
      Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, first) == VA_STATUS_SUCCESS,
              "hold the genuine original field before a full-Weave auxiliary");
      const auto held = fixture.decoder.h264_first_field;
      const auto public_pixels = fixture.public_frame->storage;
      const auto private_pixels = fixture.private_frame->storage;
      const uint64_t next_timestamp = fixture.decoder.next_timestamp;
      const uint64_t next_ordinal = fixture.decoder.next_submission_ordinal;
      NeutralAuxBegin(&fixture, &io);
      Require(NeutralAuxReceive(&fixture, &io) == VA_STATUS_SUCCESS &&
                  !fixture.decoder.auxiliary.pending &&
                  fixture.decoder.auxiliary.needs_eos &&
                  fixture.decoder.h264_first_field.valid &&
                  fixture.decoder.h264_first_field.timestamp == held.timestamp &&
                  fixture.decoder.h264_first_field.decode_identity == held.decode_identity &&
                  fixture.decoder.h264_first_field.generation == held.generation &&
                  fixture.decoder.h264_first_field.picture == held.picture &&
                  fixture.decoder.h264_first_field.pixels == held.pixels &&
                  fixture.decoder.replay.IsOutstanding(kTimestampStep) &&
                  !fixture.decoder.replay.IsOutstanding(kNeutralAuxToken) &&
                  fixture.decoder.pending.size() == 1 &&
                  fixture.decoder.pending.count(kTimestampStep) == 1 &&
                  fixture.decoder.decoded_frames.size() == 1 &&
                  fixture.decoder.decoded_frames.count(kTimestampStep) == 1 &&
                  fixture.public_frame->storage == public_pixels &&
                  fixture.private_frame->storage == private_pixels &&
                  !fixture.public_frame->ready && !fixture.private_frame->ready &&
                  !fixture.public_frame->failed && !fixture.private_frame->failed &&
                  fixture.decoder.next_timestamp == next_timestamp &&
                  fixture.decoder.next_submission_ordinal == next_ordinal &&
                  std::count(io.events.begin(), io.events.end(), "release") == 1,
              "one released auxiliary retires only its ledger; genuine first field and public owner stay unchanged");
      Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, second) == VA_STATUS_SUCCESS &&
                  !fixture.decoder.h264_first_field.valid &&
                  fixture.decoder.replay.outstanding() == 0 &&
                  fixture.public_frame->ready && fixture.private_frame->ready,
              "only the actual original complement completes its picture after auxiliary retirement");
      fixture.CheckPixels(*fixture.public_frame);
      if (!direct) fixture.CheckPixels(*fixture.private_frame);
      const auto completed_pixels = fixture.public_frame->storage;
      Require(NeutralAuxReceive(&fixture, &io) == VA_STATUS_ERROR_DECODING_ERROR &&
                  fixture.decoder.replay.failed() && !fixture.decoder.auxiliary.pending &&
                  fixture.public_frame->storage == completed_pixels &&
                  fixture.public_frame->ready && !fixture.public_frame->failed &&
                  std::count(io.events.begin(), io.events.end(), "release") == 2,
              "repeated accepted auxiliary token cannot rewrite completed public pixels");
    }
  }
}

static void NeutralAuxWeaveFaultsFailClosed() {
  for (unsigned fault = 0; fault < 16; ++fault) {
    DecodeIoMock io;
    MockDecodeScope scope(io);
    H264WeaveFixture fixture;
    auto first = fixture.Output(false, false, &fixture.top);
    auto second = fixture.Output(true, false, &fixture.bottom);
    Require(ProcessDecodedOutput(&fixture.driver, &fixture.decoder, first) == VA_STATUS_SUCCESS &&
                ProcessDecodedOutput(&fixture.driver, &fixture.decoder, second) == VA_STATUS_SUCCESS,
            "complete the real picture before isolated auxiliary faults");
    const auto public_pixels = fixture.public_frame->storage;
    const auto private_pixels = fixture.private_frame->storage;
    const uint64_t next_timestamp = fixture.decoder.next_timestamp;
    const uint64_t next_ordinal = fixture.decoder.next_submission_ordinal;
    NeutralAuxBegin(&fixture, &io);
    switch (fault) {
      case 0: fixture.output.YBuffDoneSz = (fixture.PayloadBytes() - 4U) / 4U; break;
      case 1: ++fixture.decoder.generation; break;
      case 2: fixture.decoder.auxiliary.device = nullptr; break;
      case 3: io.output_release_status = BC_STS_ERROR; break;
      case 4: ++fixture.raw.timeStamp; fixture.WritePib(); break;
      case 5: ++fixture.raw.picture_number; fixture.WritePib(); break;
      case 6: ++fixture.raw.sess_num; fixture.WritePib(); break;
      case 7: ++fixture.raw.picture_meta_payload; fixture.WritePib(); break;
      case 8: fixture.raw.flags |= 0x800U; fixture.WritePib(); break;
      case 9: fixture.output.PoutFlags &= ~BC_POUT_FLAGS_PIB_VALID; break;
      case 10: fixture.output.PicInfo.flags &= ~VDEC_FLAG_INTERLACED_SRC; break;
      case 11: fixture.output.Ybuff = nullptr; break;
      case 12: fixture.decoder.auxiliary.pending = false; break;
      case 13: fixture.WriteWord(fixture.pixels.size() - 8U, 14); break;
      case 14: fixture.WriteWord(fixture.PayloadBytes(), fixture.output.PicInfo.picture_number + 1); break;
      case 15: fixture.output.YBuffDoneSz = (fixture.PayloadBytes() + 4U) / 4U; break;
    }
    Require(NeutralAuxReceive(&fixture, &io) == VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.decoder.replay.failed() && !fixture.decoder.auxiliary.pending &&
                fixture.public_frame->storage == public_pixels &&
                fixture.private_frame->storage == private_pixels &&
                fixture.public_frame->ready && !fixture.public_frame->failed &&
                fixture.private_frame->ready && !fixture.private_frame->failed &&
                fixture.public_frame->frame_timestamp == kTimestampStep &&
                fixture.private_frame->frame_timestamp == kTimestampStep &&
                fixture.decoder.pending.empty() && fixture.decoder.replay.outstanding() == 0 &&
                fixture.decoder.next_timestamp == next_timestamp &&
                fixture.decoder.next_submission_ordinal == next_ordinal &&
                std::count(io.events.begin(), io.events.end(), "release") == 1,
            "short extent, stale owner, release error or malformed full PIB fails closed without public mutation");
  }
}

static void NeutralAuxSequentialRetirementIsBounded() {
  DecodeIoMock io;
  MockDecodeScope scope(io);
  H264WeaveFixture fixture;
  NeutralAuxBegin(&fixture, &io);
  auto &d = fixture.decoder;
  const auto original = fixture.public_frame->storage;
  for (uint64_t i = 2; i < 132; ++i) {
    d.auxiliary.timestamp = i * kTimestampStep;
    d.auxiliary.pending = true;
    fixture.output.PicInfo.timeStamp = d.auxiliary.timestamp;
    fixture.raw.timeStamp = d.auxiliary.timestamp / 20000;
    fixture.WritePib();
    Require(NeutralAuxReceive(&fixture, &io) == VA_STATUS_SUCCESS &&
                !d.auxiliary.pending && d.auxiliary.ever && d.auxiliary.needs_eos &&
                d.auxiliary.syntax.empty() && d.pending.size() == 1 &&
                d.decoded_frames.size() == 1 && d.replay.outstanding() == 1 &&
                fixture.public_frame->storage == original,
            "over64 sequential auxiliary retirements add no public owner/history ledger");
  }
}

static void NeutralAuxOldUnknownKeepsOriginalPolicy() {
  DecodeIoMock io;
  MockDecodeScope scope(io);
  H264FieldFixture fixture(false, false, VA_FOURCC_NV12, 16, 10 * kTimestampStep);
  auto &d = fixture.decoder;
  d.device = io.handle();
  d.auxiliary.ever = true;
  d.auxiliary.timestamp = 20 * kTimestampStep;
  d.auxiliary.generation = d.generation;
  d.auxiliary.device = d.device;
  auto old = fixture.Output(false, false, &fixture.top);
  old.PicInfo.timeStamp = kTimestampStep;
  Require(d.replay.Inspect(old.PicInfo.timeStamp) == CrystalHDDecodeReplay::Output::Unknown,
          "old/pruned token has no current replay owner");
  io.override_output = true;
  io.overridden_output = old;
  bool activity = false;
  Require(ReceiveOne(&fixture.driver, &d, 0, &activity) == VA_STATUS_SUCCESS &&
              activity && !d.replay.failed() && !d.h264_first_field.valid &&
              d.replay.outstanding() == 1 && !fixture.public_frame->ready &&
              std::count(io.events.begin(), io.events.end(), "release") == 1,
          "auxiliary history does not turn harmless unknown old real output into failure");
}

static void NeutralAuxInputSnapshotSurvivesPendingOutput() {
  TeardownFixture fixture(1);
  auto &d = *fixture.decoder;
  d.profile = VAProfileH264Main;
  d.live_h264_fast_path = d.live_h264_gap_tolerant = true;
  d.auxiliary.ever = d.auxiliary.pending = d.auxiliary.needs_eos = true;
  d.auxiliary.timestamp = 99 * kTimestampStep;
  d.auxiliary.generation = d.generation - 1;
  d.auxiliary.device = d.device;
  // Use a newly queued real AU, not mutable latest picture fields.
  d.replay = CrystalHDDecodeReplay(true);
  const uint64_t token = 100 * kTimestampStep;
  Require(d.replay.Append(token, true, std::vector<uint8_t>{0,0,1,0x65,0x80}),
          "admit actual queued input");
  H264Auxiliary::Syntax syntax;
  syntax.picture = NeutralPicture(16, 32);
  syntax.generation = d.generation; syntax.device = d.device;
  d.auxiliary.syntax.emplace(token, syntax);
  d.picture = {}; // Deliberately incompatible mutable newer metadata.
  Require(PumpDecodeInput(&fixture.driver, &d, 1) == VA_STATUS_SUCCESS &&
              d.auxiliary.pending && d.auxiliary.timestamp == 99 * kTimestampStep &&
              d.auxiliary.generation == d.generation - 1 &&
              d.auxiliary.device == d.device &&
              d.auxiliary.candidate_timestamp == token && d.auxiliary.syntax.empty() &&
              d.auxiliary.candidate.picture.frame_num == 7 &&
              d.auxiliary.candidate.picture.CurrPic.TopFieldOrderCnt == 18,
          "real InputSent preserves latest syntax without rebinding the pending auxiliary owner");
}

static void NeutralAuxiliaryTriggerOwnsOnlyTransport() {
  // DEFAULT automatic prefeed, explicit LIVE+LOW, and both historical LIVE
  // modes without ordinary prefeed all retain their own transport contract.
  for (unsigned policy : {0U, 1U, 2U, 3U})
  for (bool prefix_busy : {false, true}) {
    DecodeIoMock io;
    MockDecodeScope scope(io);
    H264FieldFixture fixture(false, false, VA_FOURCC_NV12, 16, kTimestampStep);
    auto &d = fixture.decoder;
    d.device = io.handle();
    d.decoder_open = d.decoder_started = true;
    d.live_h264 = true;
    d.live_h264_fast_path = d.live_h264_gap_tolerant = policy != 0;
    d.auto_h264_prefeed = policy == 0;
    d.low_latency_h264 = policy == 1;
    d.next_timestamp = 2 * kTimestampStep;
    d.prefed_timestamp = io.prefed_timestamp = d.next_timestamp;
    d.auxiliary.candidate.picture = NeutralPicture(16, 32);
    d.auxiliary.candidate.generation = d.generation;
    d.auxiliary.candidate.device = d.device;
    d.auxiliary.candidate.prefeed = policy < 2;
    d.auxiliary.candidate_timestamp = kTimestampStep;
    auto first = fixture.Output(false, false, &fixture.top);
    Require(ProcessDecodedOutput(&fixture.driver, &d, first) == VA_STATUS_SUCCESS &&
                d.h264_first_field.valid && !fixture.private_frame->ready,
            "real top field establishes missing-complement demand");
    const auto held = d.h264_first_field.pixels;
    const auto pixels = fixture.public_frame->storage;
    const auto ordinal = d.next_submission_ordinal;
    VppDeinterlace demand{};
    demand.field = crystalhd_vpp_bob::FieldParity::kBottom;
    Require(TryH264Auxiliary(&fixture.driver, &d, fixture.public_frame,
                            &demand, kDecodeBatchGraceNs) == VA_STATUS_SUCCESS &&
                io.input_calls.empty() && !d.auxiliary.ever &&
                d.next_timestamp == 2 * kTimestampStep,
            "zero-filter demand never injects auxiliary data");
    demand.bob = true;
    io.prefeed_busy_once = prefix_busy;
    std::vector<uint8_t> expected;
    Require(BuildH264NeutralIntra(d.auxiliary.candidate.picture, &expected),
            "construct expected payload for transport byte comparison");
    Require(TryH264Auxiliary(&fixture.driver, &d, fixture.public_frame,
                            &demand, kDecodeBatchGraceNs) == VA_STATUS_SUCCESS &&
                io.input_calls.size() == 2 && io.input_calls[0].accepted &&
                io.input_calls[0].timestamp == 0 && io.input_calls[0].bytes == expected &&
                io.inputs == std::vector<uint64_t>{2 * kTimestampStep} &&
                d.auxiliary.timestamp == 2 * kTimestampStep &&
                d.auxiliary.pending && d.auxiliary.needs_eos && d.auxiliary.ever &&
                d.auxiliary.generation == d.generation && d.auxiliary.device == d.device &&
                !d.auxiliary.candidate_timestamp && d.next_timestamp == 3 * kTimestampStep &&
                d.next_submission_ordinal == ordinal && d.pending.size() == 1 &&
                d.pending.count(kTimestampStep) && d.decoded_frames.size() == 1 &&
                d.replay.outstanding() == 1 && !d.replay.IsOutstanding(2 * kTimestampStep) &&
                d.h264_first_field.pixels == held && fixture.public_frame->storage == pixels &&
                !fixture.public_frame->ready && !fixture.private_frame->ready,
            "auxiliary consumes accepted prefix once without public owner/ordinal/readiness");
    Require(io.input_calls[1].bytes == H264LowLatencyPrefeed() &&
                io.input_calls[1].timestamp == 3 * kTimestampStep &&
                io.input_calls[1].accepted == !prefix_busy,
            "successor delimiter uses ordinary prefix transport");
    if (prefix_busy) {
      Require(d.pending_prefeed_timestamp == 3 * kTimestampStep && !d.prefed_timestamp &&
                  d.input_backpressured, "BUSY prefix remains pending without replaying dummy");
      bool attempted = false;
      Require(RetryPendingH264Prefeed(&fixture.driver, &d, &attempted) == VA_STATUS_SUCCESS &&
                  attempted && io.input_calls.size() == 3 && io.input_calls.back().accepted &&
                  io.input_calls.back().timestamp == 3 * kTimestampStep &&
                  io.inputs.size() == 1,
              "retry sends only delimiter, never auxiliary payload twice");
    }
    Require(d.prefed_timestamp == 3 * kTimestampStep && !d.pending_prefeed_timestamp &&
                !io.invalid, "next real AU retains exactly one accepted speculative prefix");
    const auto calls = io.input_calls.size();
    Require(TryH264Auxiliary(&fixture.driver, &d, fixture.public_frame,
                            &demand, kDecodeBatchGraceNs) == VA_STATUS_SUCCESS &&
                io.input_calls.size() == calls,
            "pending auxiliary serializes a repeated complementary-field request");
  }
}

static void NeutralAuxAdmissionSnapshotsPolicyAndSyntax() {
  SavedEnvironment live("CRYSTALHD_VAAPI_LIVE_H264");
  SavedEnvironment low("CRYSTALHD_VAAPI_LOW_LATENCY_H264");
  const auto set_policy = [](const char *name, int value) {
    if (value < 0) unsetenv(name);
    else setenv(name, value ? "1" : "0", 1);
  };
  for (int live_value : {-1, 0, 1}) {
    for (int low_value : {-1, 0, 1}) {
      set_policy("CRYSTALHD_VAAPI_LIVE_H264", live_value);
      set_policy("CRYSTALHD_VAAPI_LOW_LATENCY_H264", low_value);
      DecodeIoMock io;
      io.allow_device_open = true;
      MockDecodeScope scope(io);
      Driver driver{-1};
      driver.StopWorkers();
      driver.stopping = false;
      driver.configs[1] = {VAProfileH264Main, VAEntrypointVLD};
      VADriverContext context = {};
      context.pDriverData = &driver;
      VAContextID id = VA_INVALID_ID;
      Require(CreateContext(&context, 1, 16, 32, 0, nullptr, 0, &id) == VA_STATUS_SUCCESS,
              "public ordinary context snapshots the LIVE/LOW policy matrix");
      auto decode = driver.contexts.at(id);
      auto target = std::make_shared<Surface>();
      Require(target->AllocateInternal(nullptr, -1, 16, 32, VA_FOURCC_NV12),
              "allocate policy-matrix public target");
      target->id = 1;
      driver.surfaces[1] = target;
      Require(BeginPicture(&context, id, 1) == VA_STATUS_SUCCESS,
              "begin actual policy-matrix picture");
      decode->picture = NeutralPicture(16, 32);
      decode->have_picture = true;
      VASliceParameterBufferH264 slice = {};
      slice.slice_type = 2;
      decode->slices.push_back(slice);
      decode->slice_data.push_back({0x65, 0x88, 0x84});
      io.input_busy = true;
      const bool eligible = live_value == 1 || (live_value < 0 && low_value != 0);
      Require(EndPicture(&context, id) == VA_STATUS_SUCCESS &&
                  decode->auxiliary.syntax.size() == (eligible ? 1U : 0U),
              "actual accepted AU captures syntax only for DEFAULT eligible or historical LIVE policy");
      const auto accepted = decode->replay.NextInput()->bytes;
      decode->picture = {}; // New mutable metadata must never replace that MBAFF AU.
      io.input_busy = false;
      Require(PumpDecodeInput(&driver, decode.get()) == VA_STATUS_SUCCESS &&
                  io.input_calls[1].bytes == accepted &&
                  decode->auxiliary.syntax.empty() &&
                  decode->auxiliary.candidate_timestamp == (eligible ? kTimestampStep : 0),
              "InputSent uses the accepted immutable AU, including strict/default opt-outs");
      if (eligible)
        Require(decode->auxiliary.candidate.picture.frame_num == 7 &&
                    decode->auxiliary.candidate.picture.CurrPic.TopFieldOrderCnt == 18 &&
                    decode->auxiliary.candidate.generation == decode->generation &&
                    decode->auxiliary.candidate.device == decode->device &&
                    decode->auxiliary.candidate.prefeed ==
                        (low_value == 1 || (live_value != 1 && low_value < 0)),
                "candidate keeps exact frame/POC/device/generation and accepted prefeed decision");
      Require(!io.invalid, "public policy admission uses only mocked transport");
    }
  }
}

static void NeutralAuxFiniteActualEosPermitsContinuation() {
  for (bool retained : {false, true}) {
    for (bool closing : {false, true}) {
      DecodeIoMock io;
      io.allow_device_open = true;
      io.library_capabilities = retained ? DTS_LIB_CAP_FLEA_H264_DRAINED_REOPEN : 0;
      MockDecodeScope scope(io);
      H264WeaveFixture fixture;
      auto &d = fixture.decoder;
      d.device = io.handle();
      d.decoder_open = d.decoder_started = true;
      d.live_h264 = d.auto_h264_prefeed = true;
      d.next_timestamp = 2 * kTimestampStep;
      d.prefed_timestamp = io.prefed_timestamp = d.next_timestamp;
      d.auxiliary.candidate.picture = NeutralPicture(16, 32);
      d.auxiliary.candidate.generation = d.generation;
      d.auxiliary.candidate.device = d.device;
      d.auxiliary.candidate.prefeed = true;
      d.auxiliary.candidate_timestamp = kTimestampStep;
      auto first = fixture.Output(false, false, &fixture.top);
      auto complement = fixture.Output(true, false, &fixture.bottom);
      Require(ProcessDecodedOutput(&fixture.driver, &d, first) == VA_STATUS_SUCCESS,
              "finite original owns a real first field");
      VppDeinterlace demand{};
      demand.bob = true;
      demand.field = crystalhd_vpp_bob::FieldParity::kBottom;
      Require(TryH264Auxiliary(&fixture.driver, &d, fixture.public_frame, &demand,
                              kDecodeBatchGraceNs) == VA_STATUS_SUCCESS && d.auxiliary.pending,
              "DEFAULT accepts actual private lookahead before finite sealing");
      const auto syntax = d.auxiliary.candidate;
      io.early_eos = true;
      Require(SealDecodeBatch(&fixture.driver, &d) == VA_STATUS_SUCCESS && d.replay.sealed(),
              "finite batch is sealed while the genuine complement remains outstanding");
      constexpr uint64_t next = 3 * kTimestampStep;
      auto continuation = std::make_shared<Surface>();
      Require(continuation->AllocateInternal(nullptr, -1, 16, 32, VA_FOURCC_NV12),
              "allocate accepted continuation before the old EOS");
      continuation->expected_timestamp = next;
      InheritTraceIdentity(continuation.get(), *fixture.private_frame);
      continuation->decode_identity = continuation->trace_decode_identity = 42;
      continuation->trace_submission_ordinal = 2;
      continuation->h264_crop_unit_y = 4;
      auto next_public = std::make_shared<Surface>();
      Require(next_public->AllocateInternal(nullptr, -1, 16, 32, VA_FOURCC_NV12),
              "allocate the continuation's exact public binding");
      next_public->id = 2;
      next_public->expected_timestamp = next;
      next_public->decode_identity = 42;
      next_public->h264_crop_unit_y = 4;
      InheritTraceIdentity(next_public.get(), *continuation);
      next_public->decode_picture = continuation;
      fixture.driver.surfaces[2] = next_public;
      d.surface_timestamps[next_public.get()] = next;
      d.decoded_frames[next] = continuation;
      d.pending[next] = 2;
      d.auxiliary.syntax.emplace(next, syntax);
      Require(d.replay.Append(next, false, {0,0,1,0x41,0x80}, true),
              "sealed replay retains the exact accepted continuation");
      d.next_timestamp = 4 * kTimestampStep;
      Require(!d.CanRestartAfterH264Auxiliary() &&
                  ProcessDecodedOutput(&fixture.driver, &d, complement) == VA_STATUS_SUCCESS,
              "pending auxiliary and missing actual EOS cannot permit restart");
      const auto pixels = fixture.public_frame->storage;
      const auto identity = fixture.public_frame->decode_identity;
      const auto ordinal = d.next_submission_ordinal;
      // Describe firmware output without repairing the accepted ledger: its
      // identity must still come from the actual auxiliary Try call above.
      fixture.output.PicInfo.timeStamp = kNeutralAuxToken;
      fixture.output.PicInfo.flags &= ~VDEC_FLAG_UNKNOWN_SRC;
      fixture.raw.timeStamp = kNeutralAuxToken / 20000U;
      fixture.raw.flags = VDEC_FLAG_FIELDPAIR;
      fixture.WritePib();
      io.override_output = true;
      Require(NeutralAuxReceive(&fixture, &io) == VA_STATUS_SUCCESS && !d.auxiliary.pending &&
                  d.auxiliary.needs_eos && !d.auxiliary.ended &&
                  !d.CanRestartAfterH264Auxiliary(),
              "validated/released auxiliary pays only its private output debt, not EOS");
      io.override_output = false;
      bool activity = false;
      Require(ReceiveOne(&fixture.driver, &d, 0, &activity) == VA_STATUS_SUCCESS && activity &&
                  d.replay.NeedsRestart() && d.auxiliary.ended && !d.auxiliary.needs_eos &&
                  !d.auxiliary.eos_only && d.prefed_timestamp == 0 &&
                  d.pending_prefeed_timestamp == 0 && d.CanRestartAfterH264Auxiliary(),
              "only the consumed actual firmware EOS pays every finite restart fence");
      d.closing = closing;
      const size_t calls = io.input_calls.size();
      Require(PumpDecodeInput(&fixture.driver, &d, 1) == VA_STATUS_SUCCESS &&
                  io.device_close_calls == (retained && !closing ? 0U : 1U) &&
                  io.decoder_open_calls == 1 &&
                  io.input_calls.size() == calls + 1 &&
                  io.input_calls.back().timestamp == kTimestampStep &&
                  !d.auxiliary.ever && d.auxiliary.syntax.empty() &&
                  !d.auxiliary.candidate_timestamp && d.pending.at(next) == 2 &&
                  d.decoded_frames.at(next) == continuation &&
                  d.next_submission_ordinal == ordinal &&
                  fixture.public_frame->storage == pixels &&
                  fixture.public_frame->decode_identity == identity,
              "drained auxiliary restarts retained/full hardware, including full closing drain, without stale syntax or public mutation");
      Require(ProcessDecodedOutput(&fixture.driver, &d, first) == VA_STATUS_SUCCESS &&
                  ProcessDecodedOutput(&fixture.driver, &d, complement) == VA_STATUS_SUCCESS &&
                  fixture.public_frame->storage == pixels,
              "reference-prefix replay cannot republish or rewrite a completed original");
      Require(PumpDecodeInput(&fixture.driver, &d, 1) == VA_STATUS_SUCCESS &&
                  d.replay.IsOutstanding(next) && d.auxiliary.candidate_timestamp == 0 &&
                  d.next_submission_ordinal == ordinal && !io.invalid,
              "queued continuation submits after replay and missing fresh syntax uses finite fallback");
      first.PicInfo.timeStamp = complement.PicInfo.timeStamp = next;
      ++first.PicInfo.picture_number;
      ++complement.PicInfo.picture_number;
      Require(ProcessDecodedOutput(&fixture.driver, &d, first) == VA_STATUS_SUCCESS &&
                  ProcessDecodedOutput(&fixture.driver, &d, complement) == VA_STATUS_SUCCESS &&
                  next_public->ready && next_public->frame_timestamp == next &&
                  next_public->decode_identity == 42 && d.pending.count(next) == 0 &&
                  d.replay.outstanding() == 0 && fixture.public_frame->storage == pixels,
              "continuation completes its own real fields and exact public identity after replay");
    }
  }
}

static void NeutralAuxRestartRejectsUnpaidDebt() {
  for (unsigned fault = 0; fault < 16; ++fault) {
    DecodeIoMock io;
    MockDecodeScope scope(io);
    Driver driver{-1};
    DecodeContext d;
    d.profile = VAProfileH264Main;
    d.width = 16; d.height = 32;
    d.device = io.handle();
    d.decoder_open = d.decoder_started = true;
    d.next_timestamp = 4 * kTimestampStep;
    Require(d.replay.Append(kTimestampStep, true, {1}) && d.replay.InputSent() &&
                d.replay.Seal() &&
                d.replay.Observe(kTimestampStep) == CrystalHDDecodeReplay::Output::New &&
                d.replay.EndOfSequence() && d.replay.Append(3 * kTimestampStep, false, {3}),
            "prepare an ended replay with an accepted continuation");
    d.auxiliary.ever = d.auxiliary.ended = true;
    d.auxiliary.generation = d.generation;
    d.auxiliary.device = d.device;
    switch (fault) {
      case 0: d.auxiliary.pending = true; break;
      case 1: d.auxiliary.needs_eos = true; break;
      case 2: d.auxiliary.eos_only = true; break;
      case 3: d.auxiliary.ended = false; break;
      case 4: d.auxiliary.device = nullptr; break;
      case 5: ++d.auxiliary.generation; break;
      case 6: d.h264_first_field.valid = true; break;
      case 7: d.partial_input_timestamp = 3 * kTimestampStep; break;
      case 8: d.partial_input_offset = 1; break;
      case 9: d.prefed_timestamp = 4 * kTimestampStep; break;
      case 10: d.pending_prefeed_timestamp = 4 * kTimestampStep;
               d.pending_prefeed_generation = d.generation; break;
      case 11: d.pending_prefeed_generation = d.generation; break;
      case 12: d.live_h264_fast_path = true; break;
      case 13: d.retired = true; break;
      case 14: d.destroy_prepared = true; break;
      case 15: d.hardware_close_pending = true; break;
    }
    Require(!d.CanRestartAfterH264Auxiliary() &&
                PumpDecodeInput(&driver, &d, 1) == VA_STATUS_ERROR_DECODING_ERROR &&
                d.replay.failed() && io.input_calls.empty() && io.events.empty() &&
                io.open_attempts == 0 && !io.invalid,
            "unpaid auxiliary/EOS/field/prefix/generation/lifecycle debt fails before hardware restart or speculative retry");
  }
  for (unsigned debt = 0; debt < 3; ++debt) {
    DecodeIoMock io;
    MockDecodeScope scope(io);
    H264FieldFixture fixture(false, false, VA_FOURCC_NV12, 16, kTimestampStep);
    auto &d = fixture.decoder;
    d.device = io.handle();
    d.decoder_open = d.decoder_started = true;
    d.auxiliary.ever = d.auxiliary.needs_eos = true;
    d.auxiliary.generation = d.generation;
    d.auxiliary.device = d.device;
    d.auxiliary.pending = debt == 0;
    if (debt == 2) d.h264_first_field.valid = true;
    Require(d.replay.Seal(), "seal before injecting a genuinely premature firmware EOS");
    io.outputs.push_back(0);
    bool activity = false;
    Require(ReceiveOne(&fixture.driver, &d, 0, &activity) == VA_STATUS_ERROR_DECODING_ERROR &&
                activity && d.replay.failed() && !d.auxiliary.ended &&
                !d.CanRestartAfterH264Auxiliary() && io.input_calls.empty() &&
                std::count(io.events.begin(), io.events.end(), "release") == 1 && !io.invalid,
            "actual premature EOS with private output, real output or held-field debt cannot authorize restart");
  }
  {
    DecodeIoMock io;
    io.early_eos = true;
    MockDecodeScope scope(io);
    H264FieldFixture fixture(false, false, VA_FOURCC_NV12, 16, kTimestampStep);
    auto &d = fixture.decoder;
    d.device = io.handle();
    d.decoder_open = d.decoder_started = true;
    Require(ProcessDecodedOutput(&fixture.driver, &d,
                                 fixture.Output(false, false, &fixture.top)) == VA_STATUS_SUCCESS &&
                ProcessDecodedOutput(&fixture.driver, &d,
                                     fixture.Output(true, false, &fixture.bottom)) == VA_STATUS_SUCCESS,
            "finish all original fields before an auxiliary-only closing flush");
    d.auxiliary.ever = d.auxiliary.needs_eos = true;
    d.auxiliary.generation = d.generation;
    d.auxiliary.device = d.device;
    d.closing = true;
    Require(SealDecodeBatch(&fixture.driver, &d) == VA_STATUS_SUCCESS && d.auxiliary.eos_only,
            "only explicit close flushes private debt after all real pictures completed");
    bool activity = false;
    Require(ReceiveOne(&fixture.driver, &d, 0, &activity) == VA_STATUS_SUCCESS &&
                d.auxiliary.ended && !d.replay.NeedsRestart() &&
                d.replay.Append(3 * kTimestampStep, false, {3}) &&
                !d.CanRestartAfterH264Auxiliary() &&
                PumpDecodeInput(&fixture.driver, &d, 1) == VA_STATUS_ERROR_DECODING_ERROR &&
                d.replay.failed() && io.input_calls.empty(),
            "auxiliary-only EOS cannot manufacture a real replay EOS or admit input to ended hardware");
  }
}

static void NeutralAuxiliaryContracts() {
  NeutralAuxAdmissionSnapshotsPolicyAndSyntax();
  NeutralAuxiliaryTriggerOwnsOnlyTransport();
  NeutralAuxFiniteActualEosPermitsContinuation();
  NeutralAuxRestartRejectsUnpaidDebt();
  NeutralAuxSyntaxIsSelfContained();
  NeutralAuxWeavePreservesHeldOriginal();
  NeutralAuxWeaveFaultsFailClosed();
  NeutralAuxSequentialRetirementIsBounded();
  NeutralAuxOldUnknownKeepsOriginalPolicy();
  NeutralAuxInputSnapshotSurvivesPendingOutput();
}

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::strcmp(argv[1], "--terminal-close-ready") == 0) {
      TerminalCloseDoesNotBlockReadySync();
      TerminateJoinsAnUnlockedTerminalDestroy();
      std::puts("VA-API terminal close: ready sync, cold exclusion, hardware reservation, BUSY/failure and concurrent Terminate PASS");
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--mpeg2-fields") == 0) {
      Mpeg2FieldPairsPreserveIdentityPixelsAndReplay();
      Mpeg2FieldPairsRejectMalformedAndPreserveRetainedPictures();
      Mpeg2FieldPartialEosCloseAndRelease();
      std::puts("VA-API MPEG-2 frame-coded TFF/BFF: exact pixels, replay, ownership, EOS and release PASS");
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--card-scaling-live") == 0) {
      CardScalingLivePolicyAndOwnership();
      std::puts("VA-API live card scaling: policy matrix, prefix retry, exact TFF/BFF ownership, early FHD Bob, idle gap and explicit EOS PASS");
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--card-scaling") == 0) {
      CardScalingPublicIdentityAndGeometry();
      std::puts("VA-API integrated card scaling: setup, geometry, TFF/BFF, identity reuse and faults PASS");
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--h264-replay-metadata") == 0) {
      ReplayedH264FieldsRetainMetadataWithoutPixels();
      std::puts("VA-API replayed H264 fields: metadata-only pairing, immutable pixels and release failures PASS");
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--h264-replay-reopen") == 0) {
      for (unsigned capability : {1U, 0U, 2U, 3U})
        LiveH264InterlacedPrefeedKeepsExactFieldOwnership(true, capability);
      H264ReplayReopenEligibilityAndFailureOwnership();
      ContextDestroyWaitsForBorrowedOutputPoll();
      std::puts("VA-API H264 finite replay: qualified reuse, legacy/error full reopen, TFF/BFF ownership and pixels PASS");
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--live-bob-tail") == 0) {
      NeutralAuxiliaryContracts();
      DeferredH264PrefeedRetriesWithoutNewInput();
      FirstTemporalBobUsesCompactStorage();
      FirstTemporalBobDoesNotCompleteDecodePicture();
      FirstTemporalBobRejectsMismatchedLatch();
      FirstH264FieldRefreshesTransportGrace();
      LiveBobTailWithUnfenceableExposedTarget();
      std::puts("VA-API private first-field Bob, progress/grace and LIVE tail: TFF/BFF pixels, ownership, cancellation and explicit drain PASS");
      return 0;
    }
    Require(argc == 1, "usage: vaapi-replay-test [--terminal-close-ready|--card-scaling-live|--card-scaling|--mpeg2-fields|--h264-replay-metadata|--live-bob-tail|--h264-replay-reopen]");
    NeutralAuxiliaryContracts();
    DeferredH264PrefeedRetriesWithoutNewInput();
    FirstTemporalBobUsesCompactStorage();
    FirstTemporalBobDoesNotCompleteDecodePicture();
    FirstTemporalBobRejectsMismatchedLatch();
    SealedBatchQueuesAndReplays();
    RejectIncompleteAndUnboundedReplay();
    ReclaimOnlyCompletedIdrPrefixes();
    BoundedLiveStreamRetainsEveryOutstandingPicture();
    LiveHistoryRestoresOnlyAtAnIntactActualIdr();
    LiveEosNeverSkipsAnAcceptedContinuation();
    LiveStreamMarkerSurvivesReplayAndPruning();
    UnknownOutputNeverGuessesAndInvalidOutputFails();
    DuplicateOutputCannotMutateOrRetirePixels();
    EncryptedOutputNeverMaterializesOrCompletes();
    EncryptedReceiveReleasesOnlyAcquiredOutput();
    H264WholeWeavePreservesIdentityAndPixels();
    Mpeg2FieldPairsPreserveIdentityPixelsAndReplay();
    Mpeg2FieldPairsRejectMalformedAndPreserveRetainedPictures();
    Mpeg2FieldPartialEosCloseAndRelease();
    H264WholeWeaveRejectsMalformedOrPartialOutput();
    H264FieldPairsPreserveIdentityAndPixels();
    ReplayedH264FieldsRetainMetadataWithoutPixels();
    H264FieldPairsRespectStaleAndRetainedOwnership();
    H264Bcm70012SecondFieldTokenRecoveryIsNarrow();
    H264FieldPairRejectsPartialOrMismatchedOutput();
    H264FieldPartialStateClearsAtTeardownAndEos();
    H264FirstFieldAllocationFailureReleasesOutput();
    OutputAllocationFailuresReleaseHardwareExactlyOnce();
    DeviceCloseRetriesNonConsumingBusy();
    RetainedHandleClosesBeforeDecoderReopen();
    H264ReplayReopenEligibilityAndFailureOwnership();
    ContextAdmissionRejectsBusyBeforePicture();
    ContextAdmissionFailuresPublishNothing();
    CardScalingPublicIdentityAndGeometry();
    CardScalingLivePolicyAndOwnership();
    RealH264SubmitCompletesFieldPairs();
    TraceIdentityUsesImmutableTokenAndLiveAliasOwner();
    TraceReadExportReuseBoundary();
    ExperimentalReadyRemapGate();
    DefaultH264TransportIsBounded();
    ExperimentalPolicyDelayMatrix();
    ExperimentalRemapCpuOwnershipFailures();
    ExperimentalReadyRemapFailClosedRaces();
    ExperimentalReadyRemapBounds();
    ExperimentalLatestReadyCommitOrder();
    ExperimentalReadyRemapStateMachine();
    ConcurrentSyncObservesExperimentalPresentation();
    ExperimentalDelayedSyncThreshold();
    DirectOutputOwnershipAndExport();
    RejectIncompleteGeometryAndInitializeAllocationPadding();
    FailedCpuOwnershipNeverCompletesDecode();
    FiniteOutputUsesDecodedPublicWritePurpose();
    StaleUnretainedOutputSkipsOnlyPixelMaterialization();
    StaleUnretainedOutputPreservesTransportAndEos();
    PaddingOnlyCopyByteEquivalence();
    ExactBoundaryConversionByteEquivalence();
    CompletedLiveTailPrecedesUnreconstructibleContinuation();
    DecoderResetPreservesItsExplicitLivePolicy();
    AutomaticProgressivePrefeedUsesSubmissionSnapshot();
    AutomaticH264PrefeedSnapshotsAcceptedSequence();
    LiveH264SpeculativePrefeedSequence();
    LiveH264InterlacedPrefeedKeepsExactFieldOwnership();
    FirstH264FieldRefreshesTransportGrace();
    LiveBobTailWithUnfenceableExposedTarget();
    LiveH264PrefeedBusyFallback();
    LiveH264PrefeedEligibilityAndGhostGuard();
    TransportProgressRequiresActualHardwareIo();
    ProgressingSyncDoesNotSealAnActiveStream();
    DefaultBoundedH264StillDrainsFiniteTail();
    LargeH264CompletionDoesNotInferEos();
    LargeH264InactivitySurvivesFiniteSyncCalls();
    OutstandingLargeInputUsesCurrentHardwareBatch();
    CompletedLargeHistoryAndBcm70012KeepOrdinaryWaits();
    DefaultBoundedH264PreservesShortUnreplayableGap();
    DefaultBoundedH264InactivitySurvivesFiniteSyncCalls();
    EarlyLiveGapPreservesIntactReplayHistory();
    InactiveLiveSyncPreservesReferencesUntilActualInputResumes();
    InactiveLiveSyncHonorsFiniteDeadlineWithoutPoisoningPixels();
    ExplicitLiveContextCloseStillDrainsItsExactTail();
    ActualIdrPreservesLivePolicyUntilExplicitDrain();
    InactiveSyncStillSealsItsExactFinalPicture();
    InactiveSyncStillSealsItsExactFinalPicture(true);
    ProgressDoesNotExtendFiniteSyncDeadline();
    RetryableBusySurvivesContextSurfaceAndDriverTeardown();
    ProtectedH264BufferCannotCommitEarlierParameters();
    NewBobRequiresSuccessfulConsumingClose();
    DrainedContextSupportsNewBobCaptures();
    ContextTeardownDrainsAcceptedTailBeforeClose();
    ContextDrainRejectsMutationAndSurvivesSurfaceRelease();
    WaitingSyncKeepsCompletedRetiredPicture();
    WaitingVppCancellationStillWins();
    WaitingVppKeepsCompletedRetiredPicture();
    ContextDrainFailureDoesNotInventTail();
    CompletedWaitNeverAcceptsDifferentOrDestroyedPicture();
    ReadyStatusProbeSkipsReadyHeadPeek();
    ZeroTimeoutNeverPumpsBusyDecodeSurface();
    ExactReadySurfaceDoesNotWaitForBatchEos();
    ReadyInventoryConditionalOpportunityReplay();
    QueryOnlyObservesAutonomousDecodeProgress();
    OutputEventReplacesPeriodicWorkerPolling();
    DecodeControlWakeUsesExactArmedOutputLease();
    DecodeControlDrainConsumesOneEventfdRead();
    ContextDestroyWaitsForBorrowedOutputPoll();
    TerminalCloseDoesNotBlockReadySync();
    TerminateJoinsAnUnlockedTerminalDestroy();
    BudgetedWorkerReleasesDriverMutex();
    Mpeg4StateOnlyEosStillWakesEventWorker();
    OutputPollHangupFailsWithoutSpinning();
    AutonomousWorkerFinishesOnlyAnExistingSealedBatch();
    AutonomousWorkerStopsAfterTransportFailure();
    std::puts("VA-API sealed-batch replay, immutable-output and teardown tests passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "VA-API replay test failed: %s\n", error.what());
    return 1;
  }
}
