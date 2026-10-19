// SPDX-License-Identifier: LGPL-2.1-or-later
// Real VA submission and replay ownership, private CPU surfaces and deterministic
// Dts calls only. Unexpected device/graphics/ioctl entry points abort.
#define CRYSTALHD_VAAPI_TEST_FRAGMENT_INPUT 1
#include "../filters/vaapi/crystalhd_drv_video.cpp"
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>

namespace allocation_fault {
thread_local bool fail_next = false;
thread_local size_t fail_at_least = SIZE_MAX;
bool Consume() {
  const bool fail = fail_next;
  fail_next = false;
  return fail;
}
void *Allocate(size_t bytes) {
  if (Consume()) throw std::bad_alloc();
  if (bytes >= fail_at_least) {
    fail_at_least = SIZE_MAX;
    throw std::bad_alloc();
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
// Keep the malloc-backed fault injector opaque to GCC's
// -Wmismatched-new-delete inliner. These are the matching deallocators for the
// replacement operators above, not production allocation code.
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

namespace {
unsigned checks = 0;
void Require(bool value, const char *message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
struct InputRecord {
  uint64_t timestamp;
  std::vector<uint8_t> bytes;
  uint32_t fragment_flags;
};
struct DecoderMock {
  bool capacity = true;
  uint32_t free_bytes = 1024 * 1024;
  bool legacy_pes_family = false;
  uint8_t version_device = 1;
  uint16_t version_minor = 0;
  BC_STATUS version_status = BC_STS_SUCCESS;
  unsigned version_queries = 0;
  bool legacy_busy_once = false;
  bool allow_reopen = false;
  size_t stall_after_inputs = std::numeric_limits<size_t>::max();
  unsigned int reopen_count = 0;
  unsigned int close_count = 0;
  unsigned int legacy_input_calls = 0;
  unsigned int fragment_input_calls = 0;
  bool fragment_active = false;
  std::vector<InputRecord> inputs;
};
DecoderMock *active = nullptr;
void CheckDevice(HANDLE device) {
  Require(active != nullptr && device == active, "only the private decoder handle is used");
}
struct MockScope {
  explicit MockScope(DecoderMock *mock) {
    Require(active == nullptr, "no nested decoder fixture");
    active = mock;
    crystalhd_test_fragment_input = DtsProcInputFragment;
  }
  ~MockScope() {
    crystalhd_test_fragment_input = nullptr;
    active = nullptr;
  }
};
}

extern "C" BC_STATUS DtsDeviceOpen(HANDLE *device, uint32_t) {
  Require(active != nullptr && active->allow_reopen && device != nullptr,
          "only an explicitly armed decoder can reopen");
  *device = active;
  ++active->reopen_count;
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsDupOutputPollFd(HANDLE, int *fd) {
  if (fd) *fd = -1;
  return BC_STS_NOT_IMPL;
}
extern "C" BC_STATUS DtsCrystalHDVersion(HANDLE device, PBC_INFO_CRYSTAL version) {
  CheckDevice(device);
  Require(version != nullptr, "public device identification has an output");
  ++active->version_queries;
  if (active->version_status != BC_STS_SUCCESS)
    return active->version_status;
  *version = {};
  version->device = active->version_device;
  if (active->legacy_pes_family) {
    version->dilVersion.dilRelease = 3;
    version->dilVersion.dilMajor = 22;
    version->dilVersion.dilMinor = active->version_minor;
  }
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsSetInputFormat(HANDLE device, BC_INPUT_FORMAT *format) {
  CheckDevice(device);
  Require(active->allow_reopen && format != nullptr &&
              format->mSubtype == BC_MSUBTYPE_H264,
          "reopen restores the H.264 input format");
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsOpenDecoder(HANDLE device, uint32_t stream_type) {
  CheckDevice(device);
  Require(active->allow_reopen && stream_type == BC_STREAM_TYPE_ES,
          "reopen restores the ES decoder");
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsSetColorSpace(HANDLE device, BC_OUTPUT_FORMAT format) {
  CheckDevice(device);
  Require(active->allow_reopen && format == OUTPUT_MODE422_YUY2,
          "reopen restores packed output");
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsStartDecoder(HANDLE device) {
  CheckDevice(device);
  Require(active->allow_reopen, "only an armed decoder restarts");
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsStartCapture(HANDLE device) {
  CheckDevice(device);
  Require(active->allow_reopen, "only an armed decoder restarts capture");
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsStopDecoder(HANDLE device) {
  CheckDevice(device);
  active->fragment_active = false;
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsCloseDecoder(HANDLE device) { CheckDevice(device); return BC_STS_SUCCESS; }
extern "C" BC_STATUS DtsDeviceClose(HANDLE device) {
  CheckDevice(device);
  ++active->close_count;
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsGetDriverStatus(HANDLE device, BC_DTS_STATUS *status) {
  CheckDevice(device); *status = {}; return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsIsEndOfStream(HANDLE, uint8_t *) { abort(); }
extern "C" uint32_t DtsTxFreeSize(HANDLE device) {
  CheckDevice(device);
  return active->capacity && active->inputs.size() < active->stall_after_inputs
             ? active->free_bytes : 0;
}
extern "C" BC_STATUS DtsProcInput(HANDLE device, uint8_t *bytes, uint32_t size,
                                    uint64_t timestamp, BOOL encrypted) {
  CheckDevice(device);
  Require(bytes != nullptr && size != 0 && !encrypted &&
              !active->fragment_active,
          "ordinary complete input cannot interleave a fragmented AU");
  ++active->legacy_input_calls;
  Require(active->capacity && active->inputs.size() < active->stall_after_inputs &&
              active->free_bytes >= size + (size / 60000 + 1) * 32 + 1024,
          "legacy input is entered only after reserving its complete wire footprint");
  if (active->legacy_busy_once) {
    active->legacy_busy_once = false;
    return BC_STS_BUSY;
  }
  active->inputs.push_back({timestamp,
                            std::vector<uint8_t>(bytes, bytes + size),
                            DTS_INPUT_FRAGMENT_FIRST |
                                DTS_INPUT_FRAGMENT_LAST});
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsProcInputFragment(
    HANDLE device, uint8_t *bytes, uint32_t size, uint64_t timestamp,
    BOOL encrypted, uint32_t flags) {
  CheckDevice(device);
  if (!active->capacity || active->inputs.size() >= active->stall_after_inputs)
    return BC_STS_BUSY;
  const bool first = (flags & DTS_INPUT_FRAGMENT_FIRST) != 0;
  const bool last = (flags & DTS_INPUT_FRAGMENT_LAST) != 0;
  Require(bytes != nullptr && size != 0 && !encrypted &&
              size <= crystalhd_access_unit_limits::kDtsInputChunkBytes &&
              !(flags & ~(DTS_INPUT_FRAGMENT_FIRST |
                           DTS_INPUT_FRAGMENT_LAST)) &&
              (first ? !active->fragment_active
                     : active->fragment_active && timestamp == 0),
          "bounded H.264 fragments preserve FIRST/continuation ordering");
  ++active->fragment_input_calls;
  active->fragment_active = !last;
  active->inputs.push_back(
      {timestamp, std::vector<uint8_t>(bytes, bytes + size), flags});
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsProcOutputNoCopy(HANDLE, uint32_t, BC_DTS_PROC_OUT *) { abort(); }
extern "C" BC_STATUS DtsReleaseOutputBuffs(HANDLE, PVOID, BOOL) { abort(); }
extern "C" BC_STATUS DtsFlushInput(HANDLE, uint32_t) { abort(); }
extern "C" int ioctl(int, unsigned long, ...) noexcept { abort(); }
extern "C" gbm_device *gbm_create_device(int) { abort(); }
extern "C" void gbm_device_destroy(gbm_device *) { abort(); }
extern "C" gbm_bo *gbm_bo_create(gbm_device *, uint32_t, uint32_t, uint32_t, uint32_t) { abort(); }
extern "C" void *gbm_bo_map(gbm_bo *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, void **) { abort(); }
extern "C" void gbm_bo_unmap(gbm_bo *, void *) { abort(); }
extern "C" void gbm_bo_destroy(gbm_bo *) { abort(); }
extern "C" int gbm_bo_get_plane_count(gbm_bo *) { abort(); }
extern "C" uint32_t gbm_bo_get_stride_for_plane(gbm_bo *, int) { abort(); }
extern "C" uint32_t gbm_bo_get_offset(gbm_bo *, int) { abort(); }
extern "C" int gbm_bo_get_fd_for_plane(gbm_bo *, int) { abort(); }
extern "C" uint64_t gbm_bo_get_modifier(gbm_bo *) { abort(); }
extern "C" int drmGetDevice2(int, uint32_t, drmDevicePtr *) { abort(); }
extern "C" void drmFreeDevice(drmDevicePtr *) { abort(); }
extern "C" drmVersionPtr drmGetVersion(int) { abort(); }
extern "C" void drmFreeVersion(drmVersionPtr) { abort(); }
extern "C" int drmPrimeHandleToFD(int, uint32_t, uint32_t, int *) { abort(); }
extern "C" void sws_freeContext(SwsContext *) { abort(); }
extern "C" SwsContext *sws_getCachedContext(SwsContext *, int, int, AVPixelFormat,
    int, int, AVPixelFormat, int, SwsFilter *, SwsFilter *, const double *) { abort(); }
extern "C" int sws_scale(SwsContext *, const uint8_t *const [], const int [],
                           int, int, uint8_t *const [], const int []) { abort(); }

namespace {
VAIQMatrixBufferH264 Flat() {
  VAIQMatrixBufferH264 iq = {};
  std::memset(iq.ScalingList4x4, 16, sizeof(iq.ScalingList4x4));
  std::memset(iq.ScalingList8x8, 16, sizeof(iq.ScalingList8x8));
  return iq;
}
VAIQMatrixBufferH264 Custom(unsigned seed = 0) {
  VAIQMatrixBufferH264 iq = Flat();
  for (unsigned list = 0; list < 6; ++list)
    for (unsigned i = 0; i < 16; ++i) iq.ScalingList4x4[list][i] = 1 + (seed + list * 31 + i * 7) % 255;
  for (unsigned list = 0; list < 2; ++list)
    for (unsigned i = 0; i < 64; ++i) iq.ScalingList8x8[list][i] = 1 + (seed + list * 43 + i * 11) % 255;
  return iq;
}

struct Bits {
  std::vector<uint8_t> bytes;
  size_t position = 0;
  unsigned Get(unsigned count = 1) {
    Require(count <= 32 && position + count <= bytes.size() * 8, "PPS syntax stays in retained RBSP");
    unsigned value = 0;
    while (count--) { value = (value << 1) | ((bytes[position / 8] >> (7 - position % 8)) & 1); ++position; }
    return value;
  }
  unsigned UE() {
    unsigned zero = 0;
    while (Get() == 0) Require(++zero < 31, "bounded exponential-Golomb value");
    return ((1U << zero) - 1) + Get(zero);
  }
  int SE() { unsigned value = UE(); return (value & 1) ? int((value + 1) / 2) : -int(value / 2); }
};

Bits Pps(const std::vector<uint8_t> &unit) {
  Bits bits;
  for (size_t n = 0; n + 5 <= unit.size(); ++n) {
    if (unit[n] || unit[n + 1] || unit[n + 2] || unit[n + 3] != 1 || (unit[n + 4] & 31) != 8) continue;
    size_t end = n + 5;
    while (end + 4 > unit.size() || unit[end] || unit[end + 1] || unit[end + 2] || unit[end + 3] != 1) {
      if (end == unit.size()) break;
      ++end;
    }
    unsigned zeros = 0;
    for (size_t i = n + 5; i < end; ++i) {
      if (zeros == 2 && unit[i] == 3) { zeros = 0; continue; }
      bits.bytes.push_back(unit[i]); zeros = unit[i] == 0 ? zeros + 1 : 0;
    }
    break;
  }
  Require(!bits.bytes.empty(), "actual submitted access unit contains PPS");
  return bits;
}

// Derive the diagonal order independently instead of borrowing the builder's
// scan tables. libva supplies raster order; the PPS carries scan-order deltas.
std::vector<unsigned> Scan(unsigned width) {
  std::vector<unsigned> order;
  for (unsigned diagonal = 0; diagonal <= 2 * width - 2; ++diagonal) {
    const unsigned lo = diagonal < width ? 0 : diagonal - width + 1;
    const unsigned hi = std::min(diagonal, width - 1);
    for (unsigned step = 0; step <= hi - lo; ++step) {
      const unsigned row = (diagonal & 1) ? lo + step : hi - step;
      order.push_back(row * width + diagonal - row);
    }
  }
  return order;
}
void CheckMatrices(const std::vector<uint8_t> &unit, const VAIQMatrixBufferH264 &expected,
                   bool transform = true) {
  Bits bits = Pps(unit);
  Require(bits.UE() == 0 && bits.UE() == 0, "PPS and SPS IDs preserved");
  bits.Get(2); Require(bits.UE() == 0, "no slice groups in fixture");
  bits.UE(); bits.UE(); bits.Get(3); bits.SE(); bits.SE(); bits.SE(); bits.Get(3);
  Require(bits.Get() == unsigned(transform), "transform8x8 flag preserved");
  const bool present = bits.Get() != 0;
  VAIQMatrixBufferH264 decoded = Flat();
  if (present) {
    for (unsigned list = 0; list < (transform ? 8U : 6U); ++list) {
      Require(bits.Get() == 1, "non-flat PPS explicitly describes every active scaling list");
      uint8_t *values = list < 6 ? decoded.ScalingList4x4[list] : decoded.ScalingList8x8[list - 6];
      int previous = 8, next = 8;
      const auto order = Scan(list < 6 ? 4 : 8);
      for (unsigned raster : order) {
        if (next != 0) next = (previous + bits.SE() + 256) % 256;
        Require(next != 0, "nonzero explicit coefficients, not default-list sentinel");
        values[raster] = next; previous = next;
      }
    }
  }
  bits.SE();
  Require(!std::memcmp(decoded.ScalingList4x4, expected.ScalingList4x4,
                       sizeof(decoded.ScalingList4x4)), "submitted PPS retains all raster4x4 coefficients");
  if (transform) Require(!std::memcmp(decoded.ScalingList8x8, expected.ScalingList8x8,
                                     sizeof(decoded.ScalingList8x8)), "submitted PPS retains all raster8x8 coefficients");
}

struct Fixture {
  DecoderMock mock;
  MockScope scope{&mock};
  Driver driver{-1};
  VADriverContext context = {};
  std::shared_ptr<DecodeContext> decode = std::make_shared<DecodeContext>();
  VAPictureParameterBufferH264 picture = {};
  VASliceParameterBufferH264 slice = {};
  Fixture() {
    context.pDriverData = &driver;
    driver.configs[1] = {VAProfileH264High, VAEntrypointVLD};
    driver.contexts[1] = decode;
    decode->config = 1; decode->width = decode->height = 16;
    decode->device = &mock; decode->decoder_open = decode->decoder_started = true;
    auto surface = std::make_shared<Surface>();
    Require(surface->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12), "private CPU surface allocation");
    driver.surfaces[1] = surface;
    picture.seq_fields.bits.chroma_format_idc = 1;
    picture.seq_fields.bits.frame_mbs_only_flag = 1;
    picture.seq_fields.bits.direct_8x8_inference_flag = 1;
    picture.num_ref_frames = 1;
    picture.pic_fields.bits.transform_8x8_mode_flag = 1;
  }
  VABufferID Buffer(VABufferType type, const void *data, size_t size,
                    VAContextID context_id = 1) {
    VABufferID id = VA_INVALID_ID;
    Require(CreateBuffer(&context, context_id, type, size, 1, const_cast<void *>(data), &id) == VA_STATUS_SUCCESS,
            "real buffer creation");
    return id;
  }
  void Begin() { Require(BeginPicture(&context, 1, 1) == VA_STATUS_SUCCESS, "real BeginPicture"); }
  void Render(VABufferID id) { Require(RenderPicture(&context, 1, &id, 1) == VA_STATUS_SUCCESS, "real RenderPicture"); }
  void Parameters(bool idr = true) {
    const uint8_t bytes[] = {uint8_t(idr ? 0x65 : 0x41), 0x88, 0x84};
    Render(Buffer(VAPictureParameterBufferType, &picture, sizeof(picture)));
    Render(Buffer(VASliceParameterBufferType, &slice, sizeof(slice)));
    Render(Buffer(VASliceDataBufferType, bytes, sizeof(bytes)));
  }
  void End() { Require(EndPicture(&context, 1) == VA_STATUS_SUCCESS, "real EndPicture/SubmitPicture"); }
};

VAStatus SubmitH264(Fixture *fixture, const std::vector<uint8_t> &bytes) {
  fixture->Begin();
  fixture->Render(fixture->Buffer(VAPictureParameterBufferType,
                                  &fixture->picture,
                                  sizeof(fixture->picture)));
  fixture->Render(fixture->Buffer(VASliceParameterBufferType,
                                  &fixture->slice,
                                  sizeof(fixture->slice)));
  fixture->Render(fixture->Buffer(VASliceDataBufferType, bytes.data(),
                                  bytes.size()));
  return EndPicture(&fixture->context, 1);
}

std::vector<InputRecord> LogicalInputs(
    const std::vector<InputRecord> &fragments) {
  std::vector<InputRecord> inputs;
  for (const InputRecord &fragment : fragments) {
    if ((fragment.fragment_flags & DTS_INPUT_FRAGMENT_FIRST) != 0) {
      Require(!inputs.empty() ?
                  (inputs.back().fragment_flags & DTS_INPUT_FRAGMENT_LAST) != 0
                              : true,
              "each logical input starts after the prior LAST");
      inputs.push_back(fragment);
    } else {
      Require(!inputs.empty() &&
                  (inputs.back().fragment_flags & DTS_INPUT_FRAGMENT_LAST) == 0,
              "an ES continuation follows an open FIRST fragment");
      inputs.back().bytes.insert(inputs.back().bytes.end(),
                                 fragment.bytes.begin(),
                                 fragment.bytes.end());
      inputs.back().fragment_flags |= fragment.fragment_flags;
    }
  }
  if (!inputs.empty())
    Require((inputs.back().fragment_flags & DTS_INPUT_FRAGMENT_LAST) != 0,
            "the final logical input ends with LAST");
  return inputs;
}

void LargeH264AccessUnitBoundaryAndRecovery() {
  constexpr size_t issue_bytes = 564322;
  std::vector<uint8_t> issue_au(issue_bytes, 0x55);
  issue_au[0] = 0x65;
  size_t overhead = 0;
  {
    Fixture legacy_library;
    crystalhd_test_fragment_input = nullptr;
    const VAStatus status = SubmitH264(&legacy_library, issue_au);
    Require(status == VA_STATUS_ERROR_INVALID_PARAMETER &&
                legacy_library.mock.inputs.empty() &&
                legacy_library.mock.legacy_input_calls == 0 &&
                legacy_library.mock.fragment_input_calls == 0 &&
                legacy_library.decode->partial_input_timestamp == 0 &&
                legacy_library.decode->partial_input_offset == 0 &&
                !legacy_library.decode->replay.failed() &&
                legacy_library.decode->replay.cached_pictures() == 0 &&
                legacy_library.decode->next_timestamp == kTimestampStep &&
                legacy_library.decode->pending.empty() &&
                legacy_library.decode->decoded_frames.empty() &&
                !legacy_library.decode->sent_parameter_sets,
            "an older library rejects a large AU before consuming bytes or committing replay state");
    Require(SubmitH264(&legacy_library, {0x65, 0x88, 0x84}) ==
                    VA_STATUS_SUCCESS &&
                legacy_library.mock.legacy_input_calls == 1 &&
                legacy_library.mock.fragment_input_calls == 0 &&
                !legacy_library.decode->replay.failed(),
            "the same older-library context accepts the next actual IDR");
  }
  {
    Fixture normal;
    Require(SubmitH264(&normal, std::vector<uint8_t>{0x65, 0x88, 0x84}) ==
                    VA_STATUS_SUCCESS &&
                normal.mock.legacy_input_calls == 1 &&
                normal.mock.fragment_input_calls == 0 &&
                normal.mock.inputs.size() == 1,
            "ordinary sub-512KiB H.264 retains the legacy DtsProcInput path");
  }
  {
    Fixture stalled;
    stalled.mock.stall_after_inputs = 1;
    Require(SubmitH264(&stalled, issue_au) == VA_STATUS_SUCCESS &&
                stalled.mock.inputs.size() == 1 &&
                stalled.mock.inputs[0].bytes.size() ==
                    crystalhd_access_unit_limits::kDtsInputChunkBytes &&
                stalled.decode->partial_input_timestamp == kTimestampStep &&
                stalled.decode->partial_input_offset ==
                    crystalhd_access_unit_limits::kDtsInputChunkBytes &&
                stalled.decode->replay.outstanding() == 0 &&
                stalled.decode->replay.NextInput() != nullptr,
            "first fragment returns under TX stall without completing the AU");
    stalled.mock.stall_after_inputs = std::numeric_limits<size_t>::max();
    Require(PumpDecodeInput(&stalled.driver, stalled.decode.get()) ==
                    VA_STATUS_SUCCESS &&
                stalled.mock.inputs.size() == 2 &&
                stalled.decode->partial_input_timestamp == 0 &&
                stalled.decode->partial_input_offset == 0 &&
                stalled.decode->replay.outstanding() == 1 &&
                LogicalInputs(stalled.mock.inputs).size() == 1,
            "bounded continuation resumes and commits the logical large AU");
  }
  {
    Fixture measured;
    Require(SubmitH264(&measured, issue_au) == VA_STATUS_SUCCESS,
            "the reported 564322-byte H.264 IDR passes actual VA submission");
    const auto original = LogicalInputs(measured.mock.inputs);
    Require(measured.mock.inputs.size() == 2 && original.size() == 1 &&
                original[0].bytes.size() > 512U * 1024U,
            "the reported AU is carried by two bounded ES fragments");
    overhead = original[0].bytes.size() - issue_au.size();
    Require(overhead != 0 && overhead < 4096,
            "bounded reconstructed parameter-set and start-code overhead");

    // Exercise the real retained-input EOS/reopen path with the large original
    // bytes; a following picture must not inherit the former 512 KiB failure.
    Require(measured.decode->replay.Seal(), "seal the large-AU hardware batch");
    std::vector<uint8_t> predicted = {0x41, 0x88, 0x84};
    Require(SubmitH264(&measured, predicted) == VA_STATUS_SUCCESS &&
                measured.mock.inputs.size() == 2,
            "queue continuation while the large-AU batch is sealed");
    measured.mock.allow_reopen = true;
    Require(measured.decode->replay.Observe(kTimestampStep) ==
                    CrystalHDDecodeReplay::Output::New &&
                measured.decode->replay.EndOfSequence() &&
                PumpDecodeInput(&measured.driver, measured.decode.get()) ==
                    VA_STATUS_SUCCESS,
            "EOS causes the production close/open/replay path to complete");
    const auto replayed = LogicalInputs(measured.mock.inputs);
    Require(measured.mock.close_count == 1 && measured.mock.reopen_count == 1 &&
                measured.mock.inputs.size() == 5 && replayed.size() == 3 &&
                replayed[1].bytes == replayed[0].bytes,
            "reopen replays the exact oversized IDR then submits new input");
  }

  std::vector<uint8_t> transport_boundary(
      crystalhd_access_unit_limits::kDtsInputChunkBytes - overhead, 0x55);
  transport_boundary[0] = 0x65;
  for (int delta : {-1, 0, 1}) {
    Fixture legacy_library;
    crystalhd_test_fragment_input = nullptr;
    auto bytes = transport_boundary;
    if (delta < 0)
      bytes.pop_back();
    else if (delta > 0)
      bytes.push_back(0x55);
    const VAStatus expected = delta > 0
        ? VA_STATUS_ERROR_INVALID_PARAMETER : VA_STATUS_SUCCESS;
    Require(SubmitH264(&legacy_library, bytes) == expected &&
                legacy_library.mock.legacy_input_calls == (delta > 0 ? 0U : 1U) &&
                legacy_library.mock.fragment_input_calls == 0 &&
                !legacy_library.decode->replay.failed(),
            "old-library admission checks the exact assembled 512 KiB boundary");
  }
  {
    Fixture pending;
    crystalhd_test_fragment_input = nullptr;
    Require(SubmitH264(&pending, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                SubmitH264(&pending, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "prepare retained pictures before a rejected old-library AU");
    const auto before_pending = pending.decode->pending;
    const auto before_frames = pending.decode->decoded_frames;
    const auto before_public = pending.driver.surfaces.at(1);
    const uint64_t before_expected = before_public->expected_timestamp;
    const uint64_t before_identity = before_public->decode_identity;
    const uint64_t before_timestamp = pending.decode->next_timestamp;
    const uint64_t before_submission = pending.decode->next_submission_ordinal;
    const size_t before_cache = pending.decode->replay.cached_bytes();
    Require(SubmitH264(&pending, issue_au) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                pending.mock.inputs.size() == 2 &&
                pending.decode->pending == before_pending &&
                pending.decode->decoded_frames == before_frames &&
                pending.decode->next_timestamp == before_timestamp &&
                pending.decode->next_submission_ordinal == before_submission &&
                pending.decode->replay.cached_bytes() == before_cache &&
                pending.decode->replay.outstanding() == 2 &&
                !pending.decode->replay.failed() &&
                before_public->expected_timestamp == before_expected &&
                before_public->decode_identity != before_identity &&
                before_public->failed,
            "rejection preserves earlier private/replay identities but revokes the public target");
    for (const auto &frame : before_frames)
      Require(!frame.second->failed &&
                  pending.decode->replay.Observe(frame.first) ==
                      CrystalHDDecodeReplay::Output::New,
              "earlier retained pictures remain completable after rejection");
    Require(SubmitH264(&pending, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                pending.mock.inputs.size() == 3 &&
                pending.mock.inputs.back().timestamp == before_timestamp &&
                !pending.decode->replay.failed(),
            "the next IDR uses the unconsumed timestamp and legacy input path");
  }
  {
    Fixture boundary;
    Require(SubmitH264(&boundary, transport_boundary) == VA_STATUS_SUCCESS &&
                boundary.mock.legacy_input_calls == 1 &&
                boundary.mock.fragment_input_calls == 0 &&
                boundary.mock.inputs.size() == 1 &&
                boundary.mock.inputs[0].bytes.size() ==
                    crystalhd_access_unit_limits::kDtsInputChunkBytes,
            "exact assembled 512 KiB H.264 boundary retains one-shot input");
  }

  transport_boundary.push_back(0x55);
  {
    Fixture fragmented;
    Require(SubmitH264(&fragmented, transport_boundary) == VA_STATUS_SUCCESS &&
                fragmented.mock.legacy_input_calls == 0 &&
                fragmented.mock.fragment_input_calls == 2 &&
                LogicalInputs(fragmented.mock.inputs).size() == 1 &&
                LogicalInputs(fragmented.mock.inputs)[0].bytes.size() ==
                    crystalhd_access_unit_limits::kDtsInputChunkBytes + 1U,
            "one byte above 512 KiB switches atomically to FIRST/LAST fragments");
  }

  std::vector<uint8_t> submission_boundary(
      crystalhd_access_unit_limits::kVaH264SubmissionBytes - overhead, 0x55);
  submission_boundary[0] = 0x65;
  for (int delta : {-1, 0, 1}) {
    Fixture boundary;
    auto bytes = submission_boundary;
    if (delta < 0)
      bytes.pop_back();
    else if (delta > 0)
      bytes.push_back(0x55);
    const VAStatus expected = delta > 0
        ? VA_STATUS_ERROR_INVALID_PARAMETER : VA_STATUS_SUCCESS;
    Require(SubmitH264(&boundary, bytes) == expected &&
                boundary.mock.legacy_input_calls == 0 &&
                boundary.mock.fragment_input_calls == (delta > 0 ? 0U : 4U) &&
                !boundary.decode->replay.failed(),
            "VA submission policy checks the exact assembled 2 MiB boundary");
    if (delta <= 0)
      Require(LogicalInputs(boundary.mock.inputs).size() == 1 &&
                  LogicalInputs(boundary.mock.inputs)[0].bytes.size() ==
                      crystalhd_access_unit_limits::kVaH264SubmissionBytes + delta,
              "admitted boundary preserves the complete logical AU bytes");
    else
      Require(boundary.decode->replay.cached_pictures() == 0 &&
                  boundary.decode->next_timestamp == kTimestampStep &&
                  boundary.decode->next_submission_ordinal == 1 &&
                  boundary.decode->pending.empty() &&
                  boundary.decode->decoded_frames.empty() &&
                  !boundary.decode->sent_parameter_sets,
              "a policy rejection commits no replay or picture binding");
  }
  {
    Fixture pending;
    Require(SubmitH264(&pending, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                SubmitH264(&pending, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "prepare retained pictures before a policy rejection");
    const auto before_pending = pending.decode->pending;
    const auto before_frames = pending.decode->decoded_frames;
    const auto before_public = pending.driver.surfaces.at(1);
    const uint64_t before_expected = before_public->expected_timestamp;
    const uint64_t before_identity = before_public->decode_identity;
    const uint64_t before_timestamp = pending.decode->next_timestamp;
    const uint64_t before_submission = pending.decode->next_submission_ordinal;
    const size_t before_cache = pending.decode->replay.cached_bytes();
    auto rejected = submission_boundary;
    rejected.push_back(0x55);
    Require(SubmitH264(&pending, rejected) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                pending.mock.inputs.size() == 2 &&
                pending.decode->pending == before_pending &&
                pending.decode->decoded_frames == before_frames &&
                pending.decode->next_timestamp == before_timestamp &&
                pending.decode->next_submission_ordinal == before_submission &&
                pending.decode->replay.cached_bytes() == before_cache &&
                pending.decode->replay.outstanding() == 2 &&
                !pending.decode->replay.failed() &&
                before_public->expected_timestamp == before_expected &&
                before_public->decode_identity != before_identity &&
                before_public->failed,
            "policy rejection preserves retained output/replay but revokes the public target");
    Require(SubmitH264(&pending, {0x41, 0x88, 0x84}) ==
                    VA_STATUS_ERROR_DECODING_ERROR &&
                pending.mock.inputs.size() == 2 &&
                pending.decode->next_timestamp == before_timestamp &&
                pending.decode->pending == before_pending &&
                pending.decode->decoded_frames == before_frames &&
                !pending.decode->replay.failed(),
            "dependent pictures wait for a real IDR after a rejected reference AU");
    for (const auto &frame : before_frames)
      Require(!frame.second->failed &&
                  pending.decode->replay.Observe(frame.first) ==
                      CrystalHDDecodeReplay::Output::New,
              "earlier pictures remain completable after a policy rejection");
    Require(SubmitH264(&pending, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                pending.mock.inputs.size() == 3 &&
                pending.mock.inputs.back().timestamp == before_timestamp &&
                !pending.decode->replay.failed(),
            "the next actual IDR recovers at the unconsumed timestamp");
    Require(SubmitH264(&pending, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                pending.mock.inputs.size() == 4,
            "dependent input resumes only after the recovery IDR commits");
  }
  {
    Fixture nonreference;
    nonreference.picture.pic_fields.bits.reference_pic_flag = 1;
    Require(SubmitH264(&nonreference, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "prepare an accepted reference before a non-reference rejection");
    nonreference.picture.pic_fields.bits.reference_pic_flag = 0;
    nonreference.slice.slice_type = 1;  // B slice, with nal_ref_idc zero below.
    // A non-IDR omits SPS, so its reconstructed-header size differs from the
    // IDR used for the exact-boundary cases above. Exceed the policy with the
    // slice alone instead of accidentally admitting a smaller complete AU.
    std::vector<uint8_t> discarded_b(
        crystalhd_access_unit_limits::kVaH264SubmissionBytes + 1, 0x55);
    discarded_b[0] = 0x01;
    Require(SubmitH264(&nonreference, discarded_b) ==
                    VA_STATUS_ERROR_INVALID_PARAMETER &&
                nonreference.mock.inputs.size() == 1 &&
                !nonreference.decode->replay.failed(),
            "an oversized non-reference B picture is rejected transactionally");
    nonreference.picture.pic_fields.bits.reference_pic_flag = 1;
    nonreference.slice.slice_type = 0;
    Require(SubmitH264(&nonreference, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                nonreference.mock.inputs.size() == 2,
            "rejecting a non-reference B picture does not gate valid P input");
  }

  static_assert(crystalhd_access_unit_limits::kH264Bytes == 8U * 1024U * 1024U,
                "the hardware policy must not shrink the software allocation bound");
  std::vector<uint8_t> exact(crystalhd_access_unit_limits::kH264Bytes - overhead,
                             0x55);
  exact[0] = 0x65;
  {
    Fixture software_bound;
    Require(SubmitH264(&software_bound, exact) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                software_bound.mock.inputs.empty() &&
                !software_bound.decode->replay.failed() &&
                software_bound.decode->next_timestamp == kTimestampStep,
            "the 8 MiB software assembly bound is not hardware admission");
    auto raw = exact;
    raw.resize(crystalhd_access_unit_limits::kH264Bytes, 0x55);
    CrystalHDDecodeReplay allocation_bound;
    Require(allocation_bound.Append(kTimestampStep, true, std::move(raw)) &&
                allocation_bound.cached_bytes() == crystalhd_access_unit_limits::kH264Bytes,
            "the independent replay allocation bound still permits 8 MiB");
  }
  exact.push_back(0x55);
  {
    Fixture overflow;
    Require(SubmitH264(&overflow, exact) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                overflow.mock.inputs.empty() &&
                !overflow.decode->replay.failed() &&
                overflow.decode->next_timestamp == kTimestampStep,
            "one byte beyond the assembled boundary is rejected before replay commit");
    Require(SubmitH264(&overflow, issue_au) == VA_STATUS_SUCCESS &&
                overflow.mock.inputs.size() == 2 &&
                LogicalInputs(overflow.mock.inputs).size() == 1 &&
                !overflow.decode->replay.failed(),
            "a rejected oversized picture does not poison the next transaction");
  }
}

void QualifiedFleaProgressiveH264Headroom() {
  constexpr size_t base = crystalhd_access_unit_limits::kVaH264SubmissionBytes;
  constexpr size_t limit =
      crystalhd_access_unit_limits::kVaFleaProgressiveH264SubmissionBytes;
  static_assert(base == 2U * 1024U * 1024U && limit == 3014656U,
                "fallback and qualified assembled policies stay separate");
  size_t overhead = 0;
  {
    Fixture f;
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                f.mock.version_queries == 0,
            "ordinary modern input needs no headroom identification query");
    overhead = f.mock.inputs.at(0).bytes.size() - 3;
  }
  const auto raw_bytes = [overhead](size_t assembled) {
    std::vector<uint8_t> result(assembled - overhead, 0x55);
    result[0] = 0x65;
    return result;
  };
  // This is an escaped transport fixture, not evidence that an equally large
  // unescaped slice is accepted by firmware. Repeated 00 00 03 bytes keep the
  // complete Annex-B deescaped size below the separate headroom budget.
  const auto bytes = [overhead](size_t assembled) {
    std::vector<uint8_t> result(assembled - overhead, 0);
    result[0] = 0x65;
    for (size_t i = 3; i < result.size(); i += 3) result[i] = 3;
    return result;
  };
  {
    Fixture f;
    f.mock.legacy_pes_family = true;
    Require(SubmitH264(&f, raw_bytes(base)) == VA_STATUS_SUCCESS &&
                f.mock.version_queries == 0 &&
                f.mock.fragment_input_calls == 4,
            "even identified modern input at 2 MiB avoids extra version queries");
  }
  {
    Fixture f;
    f.mock.legacy_pes_family = true;
    Require(SubmitH264(&f, raw_bytes(limit)) ==
                    VA_STATUS_ERROR_INVALID_PARAMETER &&
                f.mock.inputs.empty() && f.mock.version_queries == 1 &&
                f.decode->replay.cached_pictures() == 0 &&
                f.decode->next_timestamp == kTimestampStep &&
                f.decode->next_submission_ordinal == 1 &&
                f.decode->pending.empty() && f.decode->decoded_frames.empty() &&
                !f.decode->sent_parameter_sets && f.decode->h264_waiting_for_idr &&
                !f.decode->replay.failed(),
            "qualified EBSP cap alone does not admit a large unescaped AU");
  }
  for (int delta : {-1, 0, 1}) {
    Fixture f;
    f.mock.legacy_pes_family = true;
    const size_t assembled = limit + delta;
    const auto slice = bytes(assembled);
    const bool accepted = delta <= 0;
    const size_t fragments = accepted
        ? (assembled + crystalhd_access_unit_limits::kDtsInputChunkBytes - 1) /
              crystalhd_access_unit_limits::kDtsInputChunkBytes
        : 0;
    Require(SubmitH264(&f, slice) == (accepted ? VA_STATUS_SUCCESS
                                            : VA_STATUS_ERROR_INVALID_PARAMETER) &&
                f.mock.version_queries == 1 && f.mock.legacy_input_calls == 0 &&
                f.mock.fragment_input_calls == fragments &&
                !f.decode->replay.failed(),
            "known Flea progressive admission checks cap-1/cap/cap+1 before input");
    if (accepted) {
      const auto logical = LogicalInputs(f.mock.inputs);
      Require(logical.size() == 1 && logical[0].bytes.size() == assembled &&
                  logical[0].timestamp == kTimestampStep &&
                  std::equal(slice.begin(), slice.end(),
                             logical[0].bytes.end() - slice.size()) &&
                  f.decode->partial_input_timestamp == 0 &&
                  f.decode->partial_input_offset == 0,
              "qualified headroom preserves exact bytes, one PTS and bounded fragments");
    } else {
      Require(f.mock.inputs.empty() && f.decode->replay.cached_pictures() == 0 &&
                  f.decode->next_timestamp == kTimestampStep &&
                  f.decode->next_submission_ordinal == 1 &&
                  f.decode->pending.empty() && f.decode->decoded_frames.empty() &&
                  !f.decode->sent_parameter_sets && f.decode->h264_waiting_for_idr,
              "qualified overflow commits no input, replay or accepted identity");
      Require(SubmitH264(&f, {0x41, 0x88, 0x84}) ==
                      VA_STATUS_ERROR_DECODING_ERROR && f.mock.inputs.empty() &&
                  SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                  f.mock.inputs.size() == 1 &&
                  f.mock.inputs[0].timestamp == kTimestampStep &&
                  !f.decode->h264_waiting_for_idr,
              "overflow waits for a real IDR without consuming the recovery PTS");
    }
  }
  {
    Fixture f;
    f.mock.legacy_pes_family = true;
    crystalhd_test_fragment_input = nullptr;
    Require(SubmitH264(&f, bytes(limit)) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                f.mock.inputs.empty() && f.mock.version_queries == 1 &&
                f.decode->replay.cached_pictures() == 0 &&
                f.decode->next_timestamp == kTimestampStep,
            "qualified hardware headroom cannot bypass the legacy whole-call cap");
  }
  for (unsigned mode = 0; mode < 9; ++mode) {
    Fixture f;
    f.mock.legacy_pes_family = mode != 0;
    if (mode == 1) f.mock.version_device = 0;
    if (mode == 2) f.mock.version_device = 2;
    if (mode == 3) f.mock.version_status = BC_STS_ERROR;
    if (mode == 4) f.mock.version_minor = 1;
    if (mode >= 5 && mode <= 7) {
      f.picture.seq_fields.bits.frame_mbs_only_flag = 0;
      f.picture.seq_fields.bits.mb_adaptive_frame_field_flag = mode == 5;
      f.picture.pic_fields.bits.field_pic_flag = mode == 6;
      f.picture.picture_height_in_mbs_minus1 = 1;
      f.decode->height = 32;
      Require(f.driver.surfaces.at(1)->AllocateInternal(
                  nullptr, -1, 16, 32, VA_FOURCC_NV12),
              "interlaced policy control has a valid two-row coded geometry");
    }
    if (mode == 8) f.picture.pic_fields.bits.field_pic_flag = 1;
    std::vector<uint8_t> oversized(base + 1, 0x55);
    oversized[0] = 0x65;
    Require(SubmitH264(&f, oversized) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                f.mock.inputs.empty() && f.mock.legacy_input_calls == 0 &&
                f.mock.fragment_input_calls == 0 &&
                f.mock.version_queries == (mode < 5 ? 1U : 0U) &&
                f.decode->replay.cached_pictures() == 0 &&
                f.decode->pending.empty() && f.decode->decoded_frames.empty() &&
                f.decode->next_timestamp == kTimestampStep &&
                f.decode->next_submission_ordinal == 1,
            "unknown/Link/version failure/MBAFF/field pictures retain the 2 MiB policy");
  }
}

// Independent reference for these valid, nonterminal escaped fixtures. The
// conservative policy does not subtract a terminal 03 or one after an overlong
// zero run. No fixture EPB here overlaps a NAL prefix; malformed/boundary forms
// are checked separately below rather than borrowed as expected escape bytes.
size_t ReferenceH264DeescapedBytes(const std::vector<uint8_t> &bytes) {
  size_t removed = 0;
  for (size_t i = 2; i < bytes.size(); ++i)
    if (bytes[i - 2] == 0 && bytes[i - 1] == 0 && bytes[i] == 3 &&
        (i == 2 || bytes[i - 3] != 0) &&
        i + 1 < bytes.size() && bytes[i + 1] <= 3)
      ++removed;
  return bytes.size() - removed;
}

void QualifiedH264DeescapedAdmissionAndRecovery() {
  constexpr size_t base = crystalhd_access_unit_limits::kVaH264SubmissionBytes;
  // This is an empirical containment policy, not a claimed hardware limit.
  constexpr size_t budget = 2U * 1024U * 1024U - 4U * 1024U;
  std::vector<uint8_t> prefix;
  {
    Fixture f;
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "measure the complete reconstructed Annex-B prefix");
    prefix = f.mock.inputs.at(0).bytes;
    prefix.resize(prefix.size() - 3);
  }
  const size_t prefix_raw = ReferenceH264DeescapedBytes(prefix);
  const auto bytes = [prefix_raw](size_t deescaped) {
    // Exactly 5000 EPBs make budget +/-1 larger than the ordinary 2 MiB
    // assembled policy without approaching the qualified EBSP cap.
    constexpr size_t epbs = 5000;
    std::vector<uint8_t> result = {0x65, 0x88, 0x84};
    for (size_t i = 0; i < epbs; ++i)
      result.insert(result.end(), {0, 0, 3});
    result.push_back(0);  // Also makes the final EPB's next byte valid.
    const size_t raw = prefix_raw + 3 + epbs * 2 + 1;
    Require(raw < deescaped, "deescaped boundary has room for inert tail bytes");
    result.resize(result.size() + deescaped - raw, 0x55);
    return result;
  };
  for (int delta : {-1, 0, 1}) {
    Fixture f;
    f.mock.legacy_pes_family = true;
    const auto slice = bytes(budget + delta);
    auto assembled = prefix;
    assembled.insert(assembled.end(), slice.begin(), slice.end());
    Require(assembled.size() > base &&
                ReferenceH264DeescapedBytes(assembled) == budget + delta,
            "raw boundary is measured over the complete assembled Annex-B AU");
    const bool accepted = delta <= 0;
    Require(SubmitH264(&f, slice) == (accepted ? VA_STATUS_SUCCESS
                                            : VA_STATUS_ERROR_INVALID_PARAMETER),
            "qualified deescaped admission checks budget-1/budget/budget+1");
    if (accepted) {
      const auto logical = LogicalInputs(f.mock.inputs);
      Require(logical.size() == 1 && logical[0].bytes == assembled &&
                  logical[0].timestamp == kTimestampStep &&
                  f.mock.fragment_input_calls ==
                      (assembled.size() +
                       crystalhd_access_unit_limits::kDtsInputChunkBytes - 1) /
                          crystalhd_access_unit_limits::kDtsInputChunkBytes &&
                  f.mock.legacy_input_calls == 0 &&
                  f.decode->partial_input_timestamp == 0 &&
                  f.decode->partial_input_offset == 0 &&
                  !f.decode->replay.failed(),
              "raw-admitted input preserves all encoded bytes, PTS and fragments");
    } else {
      Require(f.mock.inputs.empty() && f.mock.fragment_input_calls == 0 &&
                  f.mock.legacy_input_calls == 0 &&
                  f.decode->replay.cached_pictures() == 0 &&
                  f.decode->pending.empty() && f.decode->decoded_frames.empty() &&
                  f.decode->next_timestamp == kTimestampStep &&
                  f.decode->next_submission_ordinal == 1 &&
                  !f.decode->sent_parameter_sets &&
                  f.decode->h264_waiting_for_idr && !f.decode->replay.failed(),
              "raw rejection consumes no input, replay, accepted picture or PTS");
    }
  }
  for (unsigned next : {0U, 1U, 2U, 3U, 4U, 255U}) {
    Fixture f;
    f.mock.legacy_pes_family = true;
    auto slice = bytes(budget);
    // The terminator after the last 00 00 03 is followed only by 0x55,
    // so changing it cannot accidentally remove an adjacent second EPB.
    slice[3 + 5000 * 3] = uint8_t(next);
    auto assembled = prefix;
    assembled.insert(assembled.end(), slice.begin(), slice.end());
    const bool accepted = next <= 3;
    Require(ReferenceH264DeescapedBytes(assembled) ==
                    budget + (accepted ? 0U : 1U) &&
                SubmitH264(&f, slice) ==
                    (accepted ? VA_STATUS_SUCCESS
                              : VA_STATUS_ERROR_INVALID_PARAMETER),
            "only an EPB followed by 0..3 reduces qualified deescaped accounting");
    if (accepted)
      Require(LogicalInputs(f.mock.inputs).at(0).bytes == assembled,
              "deescaped accounting never rewrites the encoded device input");
    else
      Require(f.mock.inputs.empty() && f.decode->replay.cached_pictures() == 0,
              "an invalid EPB cannot disguise an over-budget raw AU");
  }
  const std::vector<std::vector<uint8_t>> boundary_tails = {
      {0, 0, 3, 0},                 // Valid nonterminal EPB: subtract one.
      {0, 3},                       // Only one preceding zero: literal data.
      {0, 0, 3},                    // Terminal 03: count conservatively.
      {0, 0, 0, 3, 0},              // Overlong zero run: count conservatively.
      {0, 0, 3, 0, 0, 1, 0x41},     // Cannot borrow the next 3-byte NAL prefix.
      {0, 0, 3, 0, 0, 0, 1, 0x41},  // Nor the next 4-byte NAL prefix.
  };
  for (size_t index = 0; index < boundary_tails.size(); ++index) {
    Fixture f;
    f.mock.legacy_pes_family = true;
    auto slice = bytes(budget + 1);
    const auto &tail = boundary_tails[index];
    std::copy(tail.begin(), tail.end(), slice.end() - tail.size());
    // Replacing inert tail bytes changes no assembled byte count. Only the
    // first, fully contained and unambiguous EPB removes one from budget+1.
    const bool accepted = index == 0;
    Require(SubmitH264(&f, slice) ==
                    (accepted ? VA_STATUS_SUCCESS
                              : VA_STATUS_ERROR_INVALID_PARAMETER),
            "ambiguous, terminal or cross-NAL 03 cannot reduce the raw budget");
    if (accepted) {
      auto assembled = prefix;
      assembled.insert(assembled.end(), slice.begin(), slice.end());
      Require(LogicalInputs(f.mock.inputs).at(0).bytes == assembled,
              "a contained EPB changes accounting but not submitted bytes");
    } else {
      Require(f.mock.inputs.empty() && f.decode->replay.cached_pictures() == 0,
              "ambiguous escape bytes reject before transport or replay");
    }
  }
  for (int delta : {0, 1}) {
    Fixture f;
    f.mock.legacy_pes_family = true;
    // Two slice-data buffers contribute a second start code and NAL header.
    // Each raw NAL is small; admission must count their complete AU together.
    auto first = bytes(budget - 5 + delta);
    std::vector<uint8_t> second(first.begin() + first.size() / 2, first.end());
    first.resize(first.size() / 2);
    second.insert(second.begin(), 0x41);
    auto assembled = prefix;
    assembled.insert(assembled.end(), first.begin(), first.end());
    assembled.insert(assembled.end(), {0, 0, 0, 1});
    assembled.insert(assembled.end(), second.begin(), second.end());
    Require(ReferenceH264DeescapedBytes(assembled) == budget + delta &&
                first.size() < base && second.size() < base,
            "aggregate fixture has two individually small NALs at the raw AU boundary");
    f.Begin();
    f.Render(f.Buffer(VAPictureParameterBufferType, &f.picture, sizeof(f.picture)));
    for (const auto *data : {&first, &second}) {
      f.Render(f.Buffer(VASliceParameterBufferType, &f.slice, sizeof(f.slice)));
      f.Render(f.Buffer(VASliceDataBufferType, data->data(), data->size()));
    }
    Require(EndPicture(&f.context, 1) ==
                    (delta == 0 ? VA_STATUS_SUCCESS
                                : VA_STATUS_ERROR_INVALID_PARAMETER),
            "qualified deescaped budget applies to the aggregate, not each NAL separately");
    if (delta == 0)
      Require(LogicalInputs(f.mock.inputs).at(0).bytes == assembled,
              "multiple NALs retain all original bytes and start-code boundaries");
    else
      Require(f.mock.inputs.empty() && f.decode->replay.cached_pictures() == 0,
              "aggregate raw overflow cannot partially enter transport or replay");
  }
  {
    Fixture f;
    f.mock.legacy_pes_family = true;
    f.picture.pic_fields.bits.reference_pic_flag = 1;
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "retain accepted private pictures before raw headroom rejection");
    const auto before_pending = f.decode->pending;
    const auto before_frames = f.decode->decoded_frames;
    const auto target = f.driver.surfaces.at(1);
    const auto expected = target->expected_timestamp;
    const auto identity = target->decode_identity;
    const auto timestamp = f.decode->next_timestamp;
    const auto ordinal = f.decode->next_submission_ordinal;
    const auto cache = f.decode->replay.cached_bytes();
    Require(SubmitH264(&f, bytes(budget + 1)) ==
                    VA_STATUS_ERROR_INVALID_PARAMETER &&
                f.mock.inputs.size() == 2 &&
                f.decode->pending == before_pending &&
                f.decode->decoded_frames == before_frames &&
                f.decode->next_timestamp == timestamp &&
                f.decode->next_submission_ordinal == ordinal &&
                f.decode->replay.cached_bytes() == cache &&
                f.decode->replay.outstanding() == 2 &&
                !f.decode->replay.failed() &&
                target->expected_timestamp == expected &&
                target->decode_identity != identity && target->failed &&
                target->rejected_picture_transaction == target->picture_transaction,
            "raw rejection preserves prior private pictures/replay but revokes its public transaction");
    Require(SubmitH264(&f, {0x41, 0x88, 0x84}) ==
                    VA_STATUS_ERROR_DECODING_ERROR &&
                f.mock.inputs.size() == 2 &&
                f.decode->next_timestamp == timestamp &&
                f.decode->pending == before_pending &&
                f.decode->decoded_frames == before_frames,
            "rejected reference AU establishes an IDR barrier without consuming PTS");
    for (const auto &frame : before_frames)
      Require(!frame.second->failed &&
                  f.decode->replay.Observe(frame.first) ==
                      CrystalHDDecodeReplay::Output::New,
              "earlier accepted pictures remain completable after raw rejection");
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 3 &&
                f.mock.inputs.back().timestamp == timestamp &&
                !f.decode->h264_waiting_for_idr && !f.decode->replay.failed() &&
                SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 4,
            "real IDR resumes at the unconsumed PTS and releases dependent input");
  }
  {
    Fixture f;
    f.mock.legacy_pes_family = true;
    f.picture.pic_fields.bits.reference_pic_flag = 1;
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "retain an accepted reference before raw non-reference rejection");
    const auto before_pending = f.decode->pending;
    const auto before_frames = f.decode->decoded_frames;
    const auto timestamp = f.decode->next_timestamp;
    const auto ordinal = f.decode->next_submission_ordinal;
    const auto cache = f.decode->replay.cached_bytes();
    f.picture.pic_fields.bits.reference_pic_flag = 0;
    f.slice.slice_type = 1;
    auto nonreference_b = bytes(budget + 128);
    nonreference_b[0] = 0x01;  // Non-IDR B picture with nal_ref_idc zero.
    Require(prefix_raw < 128 && nonreference_b.size() > base &&
                nonreference_b.size() + prefix.size() <
                    crystalhd_access_unit_limits::kVaFleaProgressiveH264SubmissionBytes,
            "non-IDR raw overflow remains above budget despite its shorter prefix");
    Require(SubmitH264(&f, nonreference_b) ==
                    VA_STATUS_ERROR_INVALID_PARAMETER &&
                f.mock.inputs.size() == 1 &&
                f.mock.legacy_input_calls == 1 &&
                f.mock.fragment_input_calls == 0 &&
                f.decode->pending == before_pending &&
                f.decode->decoded_frames == before_frames &&
                f.decode->next_timestamp == timestamp &&
                f.decode->next_submission_ordinal == ordinal &&
                f.decode->replay.cached_bytes() == cache &&
                f.decode->replay.outstanding() == 1 &&
                !f.decode->h264_waiting_for_idr && !f.decode->replay.failed(),
            "raw non-reference rejection preserves retained output without imposing an IDR barrier");
    for (const auto &frame : before_frames)
      Require(!frame.second->failed,
              "raw non-reference rejection leaves accepted private pictures valid");
    f.picture.pic_fields.bits.reference_pic_flag = 1;
    f.slice.slice_type = 0;
    Require(SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 2 &&
                f.mock.inputs.back().timestamp == timestamp &&
                !f.decode->h264_waiting_for_idr && !f.decode->replay.failed(),
            "dependent P input resumes at the unconsumed PTS without another IDR");
  }
}

void LegacyFleaPesAdmission() {
  size_t overhead = 0;
  {
    Fixture f;
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "measure the reconstructed Annex-B header bytes");
    overhead = f.mock.inputs.at(0).bytes.size() - 3;
  }
  const auto bytes = [overhead](size_t assembled) {
    std::vector<uint8_t> result(assembled - overhead, 0x55);
    result[0] = 0x65;
    return result;
  };
  constexpr size_t limit = crystalhd_access_unit_limits::kLegacyH264PesBytes;
  static_assert(limit == 768U * 1024U, "legacy full-call admission stays below its TX ring");
  for (size_t size : {crystalhd_access_unit_limits::kDtsInputChunkBytes + 1,
                      size_t(564367), limit - 1, limit}) {
    Fixture f;
    crystalhd_test_fragment_input = nullptr;
    f.mock.legacy_pes_family = true;
    Require(SubmitH264(&f, bytes(size)) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 1 && f.mock.legacy_input_calls == 1 &&
                f.mock.fragment_input_calls == 0 &&
                f.mock.inputs[0].bytes.size() == size &&
                f.mock.inputs[0].timestamp == kTimestampStep &&
                f.decode->partial_input_timestamp == 0 &&
                f.decode->partial_input_offset == 0 && !f.decode->replay.failed(),
            "identified Flea PES admits one complete bounded AU with one timestamp");
  }
  {
    Fixture f;
    crystalhd_test_fragment_input = nullptr;
    f.mock.legacy_pes_family = true;
    f.mock.allow_reopen = true;
    f.decode->decoder_open = f.decode->decoder_started = false;
    f.decode->device_preopened = true;
    Require(SubmitH264(&f, bytes(564367)) == VA_STATUS_SUCCESS &&
                f.mock.version_queries >= 2 && f.mock.inputs.size() == 1 &&
                f.decode->decoder_started,
            "public identity is checked before first-picture admission and restored format startup");
  }
  for (unsigned mode = 0; mode < 4; ++mode) {
    Fixture f;
    crystalhd_test_fragment_input = nullptr;
    f.mock.legacy_pes_family = mode != 0;
    if (mode == 1) f.mock.version_device = 0;
    if (mode == 2) f.mock.version_device = 2;
    if (mode == 3) f.mock.version_status = BC_STS_ERROR;
    Require(SubmitH264(&f, bytes(524289)) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                f.mock.version_queries != 0 && f.mock.inputs.empty() &&
                f.decode->replay.cached_pictures() == 0 &&
                f.decode->next_timestamp == kTimestampStep,
            "unknown, Link and unidentifiable legacy implementations retain the 512 KiB guard");
  }
  {
    Fixture f;
    crystalhd_test_fragment_input = nullptr;
    f.mock.legacy_pes_family = true;
    f.mock.version_minor = 1;
    Require(SubmitH264(&f, bytes(524289)) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                f.mock.inputs.empty(), "an unreviewed DIL version cannot opt into whole-AU admission");
  }
  {
    Fixture f;
    crystalhd_test_fragment_input = nullptr;
    f.mock.legacy_pes_family = true;
    Require(SubmitH264(&f, bytes(limit + 1)) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                f.mock.inputs.empty() && f.mock.legacy_input_calls == 0 &&
                f.mock.fragment_input_calls == 0 &&
                f.decode->replay.cached_pictures() == 0 && f.decode->pending.empty() &&
                f.decode->decoded_frames.empty() &&
                f.decode->next_timestamp == kTimestampStep &&
                f.decode->next_submission_ordinal == 1 && !f.decode->replay.failed(),
            "one byte above the legacy cap rejects before transport, replay or accepted identity");
  }
  for (bool late_busy : {false, true}) {
    Fixture f;
    crystalhd_test_fragment_input = nullptr;
    f.mock.legacy_pes_family = true;
    const size_t reserve = limit + (limit / 60000 + 1) * 32 + 1024;
    f.mock.free_bytes = late_busy ? reserve : reserve - 1;
    f.mock.legacy_busy_once = late_busy;
    Require(SubmitH264(&f, bytes(limit)) == VA_STATUS_SUCCESS &&
                f.decode->input_backpressured && f.mock.inputs.empty() &&
                f.decode->replay.NextInput() != nullptr &&
                f.decode->partial_input_timestamp == 0 &&
                f.decode->partial_input_offset == 0 &&
                f.mock.legacy_input_calls == (late_busy ? 1U : 0U),
            "whole-call reservation or zero-byte BUSY leaves the complete AU pending");
    f.mock.free_bytes = reserve;
    Require(PumpDecodeInput(&f.driver, f.decode.get()) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 1 && f.mock.inputs[0].bytes.size() == limit &&
                f.mock.inputs[0].timestamp == kTimestampStep &&
                f.mock.fragment_input_calls == 0 &&
                PumpDecodeInput(&f.driver, f.decode.get()) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 1 && f.decode->replay.NextInput() == nullptr,
            "capacity progress admits the original whole AU exactly once without a second PTS");
  }
  {
    Fixture f;
    crystalhd_test_fragment_input = nullptr;
    f.mock.legacy_pes_family = true;
    f.mock.capacity = false;
    Require(SubmitH264(&f, bytes(564367)) == VA_STATUS_SUCCESS,
            "retain complete legacy input before a synthetic fragment-state fault");
    f.decode->partial_input_timestamp = kTimestampStep;
    f.decode->partial_input_offset = 1;
    f.mock.capacity = true;
    Require(PumpDecodeInput(&f.driver, f.decode.get()) == VA_STATUS_ERROR_DECODING_ERROR &&
                f.mock.inputs.empty() && f.mock.legacy_input_calls == 0,
            "legacy compatibility never reinterprets a modern partial AU as a new complete input");
  }
  {
    Fixture f;
    crystalhd_test_fragment_input = nullptr;
    f.mock.legacy_pes_family = true;
    Require(SubmitH264(&f, bytes(564367)) == VA_STATUS_SUCCESS &&
                f.decode->replay.Seal() &&
                SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 1,
            "sealed legacy batch holds the following picture until real EOS");
    const auto original = f.mock.inputs[0].bytes;
    f.mock.allow_reopen = true;
    Require(f.decode->replay.Observe(kTimestampStep) == CrystalHDDecodeReplay::Output::New &&
                f.decode->replay.EndOfSequence() &&
                PumpDecodeInput(&f.driver, f.decode.get()) == VA_STATUS_SUCCESS &&
                f.mock.close_count == 1 && f.mock.reopen_count == 1 &&
                f.mock.legacy_input_calls == 3 && f.mock.fragment_input_calls == 0 &&
                f.mock.inputs.size() == 3 && f.mock.inputs[1].bytes == original &&
                f.mock.inputs[1].timestamp == kTimestampStep &&
                f.mock.inputs[2].timestamp == 2 * kTimestampStep &&
                f.decode->partial_input_timestamp == 0 &&
                f.decode->partial_input_offset == 0 && !f.decode->replay.failed(),
            "EOS/reopen replays the exact whole IDR then advances the following picture once");
  }
}

void H264ReferenceRejectionRequiresIdr() {
  std::vector<uint8_t> large(564322, 0x55);
  large[0] = 0x65;
  for (uint8_t rejected_header :
       {uint8_t(0x65), uint8_t(0x41), uint8_t(0x05)}) {
    Fixture f;
    crystalhd_test_fragment_input = nullptr;
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "establish an accepted IDR before rejecting a reference");
    const auto pending = f.decode->pending;
    const auto frames = f.decode->decoded_frames;
    const uint64_t timestamp = f.decode->next_timestamp;
    const size_t cached = f.decode->replay.cached_bytes();
    large[0] = rejected_header;
    Require(SubmitH264(&f, large) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                f.decode->h264_waiting_for_idr,
            "unsubmitted reference P or actual IDR, even with invalid ref bits, requires a fresh IDR");
    for (uint8_t dependent : {uint8_t(0x41), uint8_t(0x01)}) {
      f.slice.slice_type = dependent == 0x01 ? 1 : 2;
      Require(SubmitH264(&f, {dependent, 0x88, 0x84}) ==
                      VA_STATUS_ERROR_DECODING_ERROR &&
                  f.mock.inputs.size() == 1 && f.decode->pending == pending &&
                  f.decode->decoded_frames == frames &&
                  f.decode->next_timestamp == timestamp &&
                  f.decode->replay.cached_bytes() == cached &&
                  !f.decode->replay.failed(),
              "dependent P/B and non-IDR I cannot mutate replay or picture bindings");
    }
    large[0] = 0x65;
    Require(SubmitH264(&f, large) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                f.decode->h264_waiting_for_idr,
            "another rejected IDR does not release the reference gate");
    f.Begin();
    f.Parameters();
    allocation_fault::fail_next = true;
    Require(EndPicture(&f.context, 1) == VA_STATUS_ERROR_ALLOCATION_FAILED &&
                !allocation_fault::fail_next && f.decode->h264_waiting_for_idr,
            "an IDR assembly allocation failure cannot release the gate");
    Require(f.decode->replay.Observe(kTimestampStep) ==
                    CrystalHDDecodeReplay::Output::New &&
                !frames.at(kTimestampStep)->failed,
            "the earlier accepted picture remains independently completable");
    f.slice.slice_type = 2;
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                !f.decode->h264_waiting_for_idr && f.mock.inputs.size() == 2 &&
                f.mock.inputs.back().timestamp == timestamp &&
                SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "only an accepted actual IDR restores following dependent submission");
  }
  {
    Fixture f;
    crystalhd_test_fragment_input = nullptr;
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "establish the reference chain before dropping a nonreference B");
    large[0] = 0x01;
    f.slice.slice_type = 1;
    Require(SubmitH264(&f, large) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                !f.decode->h264_waiting_for_idr,
            "a size-rejected nonreference B cannot break the hardware reference chain");
    f.slice.slice_type = 0;
    Require(SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 2 && !f.decode->replay.failed(),
            "a following reference P remains admissible after nonreference rejection");
  }
  {
    Fixture f;
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "measure reconstructed headers for the assembler limit");
    const size_t overhead = f.mock.inputs.front().bytes.size() - 3;
    std::vector<uint8_t> overflow(
        crystalhd_access_unit_limits::kH264Bytes - overhead + 1, 0x55);
    overflow[0] = 0x65;
    Require(SubmitH264(&f, overflow) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                f.decode->h264_waiting_for_idr && f.mock.inputs.size() == 1 &&
                SubmitH264(&f, {0x41, 0x88, 0x84}) ==
                    VA_STATUS_ERROR_DECODING_ERROR,
            "assembled size overflow gates dependent input before replay commit");
  }
  for (bool retained_reference : {false, true}) {
    Fixture f;
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "establish a reference before RenderPicture size overflow");
    f.Begin();
    f.Render(f.Buffer(VAPictureParameterBufferType, &f.picture, sizeof(f.picture)));
    f.Render(f.Buffer(VASliceParameterBufferType, &f.slice, sizeof(f.slice)));
    if (retained_reference) {
      const uint8_t retained[] = {0x41, 0x88, 0x84};
      f.Render(f.Buffer(VASliceDataBufferType, retained, sizeof(retained)));
    }
    std::vector<uint8_t> overflow(crystalhd_access_unit_limits::kH264Bytes, 0x55);
    overflow[0] = retained_reference ? 0x01 : 0x65;
    VABufferID id = f.Buffer(VASliceDataBufferType, overflow.data(), overflow.size());
    Require(RenderPicture(&f.context, 1, &id, 1) == VA_STATUS_ERROR_INVALID_BUFFER &&
                f.decode->h264_waiting_for_idr &&
                f.decode->slice_data.size() == (retained_reference ? 1U : 0U) &&
                EndPicture(&f.context, 1) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                SubmitH264(&f, {0x41, 0x88, 0x84}) ==
                    VA_STATUS_ERROR_DECODING_ERROR,
            "Render overflow inspects rejected and retained NAL headers before copying bytes");
  }
  {
    Fixture f;
    crystalhd_test_fragment_input = nullptr;
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                f.decode->replay.Seal(),
            "seal accepted output before rejecting a replacement IDR");
    large[0] = 0x65;
    Require(SubmitH264(&f, large) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                f.decode->h264_waiting_for_idr &&
                f.decode->replay.IsOutstanding(kTimestampStep),
            "reference rejection preserves a sealed batch and its pending output");
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                !f.decode->h264_waiting_for_idr && f.mock.inputs.size() == 1 &&
                SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 1,
            "a queued committed IDR releases the gate without bypassing sealed transport ordering");
    Require(f.decode->replay.Observe(kTimestampStep) ==
                    CrystalHDDecodeReplay::Output::New &&
                f.decode->replay.EndOfSequence(),
            "old sealed output can still reach its genuine EOS boundary");
    f.mock.allow_reopen = true;
    Require(PumpDecodeInput(&f.driver, f.decode.get()) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 4 &&
                f.mock.inputs[1].timestamp == kTimestampStep &&
                f.mock.inputs[2].timestamp == 2 * kTimestampStep &&
                f.mock.inputs[3].timestamp == 3 * kTimestampStep,
            "reopen pumps the accepted IDR before its queued dependent P");
  }
  {
    Fixture f;
    crystalhd_test_fragment_input = nullptr;
    large[0] = 0x65;
    Require(SubmitH264(&f, large) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                f.decode->h264_waiting_for_idr &&
                f.decode->CloseHardware(0) == BC_STS_SUCCESS &&
                f.decode->h264_waiting_for_idr,
            "hardware-only close cannot erase an unsubmitted reference gap");
    Require(SubmitH264(&f, {0x41, 0x88, 0x84}) ==
                    VA_STATUS_ERROR_DECODING_ERROR &&
                f.decode->h264_waiting_for_idr && f.mock.reopen_count == 0,
            "Begin/End preserve the gate and reject before reopening hardware");
    Require(f.decode->Close(0) == BC_STS_SUCCESS &&
                !f.decode->h264_waiting_for_idr,
            "logical context close resets the reference gate");
    f.decode->h264_waiting_for_idr = true;
    Require(f.decode->Reset(0) == BC_STS_SUCCESS &&
                !f.decode->h264_waiting_for_idr,
            "logical context reset starts an independent reference lifetime");
  }
}

void AllocationFailureIsTransactional() {
  Fixture buffers;
  const VABufferID expected = buffers.driver.next_buffer;
  VABufferID returned = VA_INVALID_ID;
  const uint8_t byte = 0x65;
  Require(CreateBuffer(&buffers.context, 1, VASliceDataBufferType, UINT_MAX,
                       UINT_MAX, nullptr, &returned) ==
                  VA_STATUS_ERROR_ALLOCATION_FAILED &&
              returned == VA_INVALID_ID &&
              buffers.driver.next_buffer == expected &&
              buffers.driver.buffers.count(expected) == 0,
          "unrepresentable public buffer extent fails before allocation");
  allocation_fault::fail_next = true;
  Require(CreateBuffer(&buffers.context, 1, VASliceDataBufferType, 1, 1,
                       const_cast<uint8_t *>(&byte), &returned) ==
                  VA_STATUS_ERROR_ALLOCATION_FAILED &&
              !allocation_fault::fail_next && returned == VA_INVALID_ID &&
              buffers.driver.next_buffer == expected &&
              buffers.driver.buffers.count(expected) == 0,
          "public buffer allocation failure preserves ID and ownership state");

  buffers.Begin();
  VABufferID slice_data =
      buffers.Buffer(VASliceDataBufferType, &byte, sizeof(byte));
  allocation_fault::fail_next = true;
  Require(RenderPicture(&buffers.context, 1, &slice_data, 1) ==
                  VA_STATUS_ERROR_ALLOCATION_FAILED &&
              !allocation_fault::fail_next &&
              buffers.decode->h264_picture_invalid &&
              EndPicture(&buffers.context, 1) ==
                  VA_STATUS_ERROR_INVALID_PARAMETER,
          "RenderPicture allocation failure poisons only its current picture");

  buffers.Begin();
  buffers.Parameters();
  allocation_fault::fail_next = true;
  Require(EndPicture(&buffers.context, 1) ==
                  VA_STATUS_ERROR_ALLOCATION_FAILED &&
              !allocation_fault::fail_next && buffers.mock.inputs.empty() &&
              !buffers.decode->replay.failed() &&
              !buffers.decode->h264_waiting_for_idr &&
              buffers.decode->next_timestamp == kTimestampStep,
          "access-unit assembly allocation failure commits no replay state");
  Require(SubmitH264(&buffers, std::vector<uint8_t>{0x65, 0x88, 0x84}) ==
                  VA_STATUS_SUCCESS &&
              buffers.mock.inputs.size() == 1,
          "next picture recovers after an assembly allocation failure");

  // A deque node allocation is implementation-managed. Arm the allocator only
  // around Append and repeat until one node boundary is reached.
  CrystalHDDecodeReplay replay;
  size_t accepted = 0;
  for (uint64_t token = 1; token <= 4096 && !replay.failed(); ++token) {
    std::vector<uint8_t> unit(1, 0x65);
    allocation_fault::fail_next = true;
    const bool appended = replay.Append(token, token == 1,
                                        std::move(unit));
    const bool consumed = !allocation_fault::fail_next;
    allocation_fault::fail_next = false;
    if (appended) {
      ++accepted;
      continue;
    }
    Require(consumed && replay.failed() &&
                std::string(replay.failure()) ==
                    "compressed IDR replay cache allocation failed" &&
                replay.cached_pictures() == accepted &&
                replay.cached_bytes() == accepted,
            "replay allocation failure leaves byte and picture counters exact");
  }
  Require(replay.failed() && accepted != 0,
          "fault injection reaches a bounded replay-container allocation");

  CrystalHDDecodeReplay tracking;
  Require(tracking.Append(1, true, {0x65}),
          "prepare replay tracking allocation failure");
  allocation_fault::fail_next = true;
  const bool sent = tracking.InputSent();
  const bool tracking_failure_consumed = !allocation_fault::fail_next;
  allocation_fault::fail_next = false;
  Require(!sent && tracking_failure_consumed && tracking.failed() &&
              tracking.outstanding() == 0 &&
              tracking.cached_pictures() == 1 &&
              tracking.cached_bytes() == 1 &&
              std::string(tracking.failure()) ==
                  "compressed IDR replay tracking allocation failed",
          "tracking allocation failure advances no cursor or outstanding state");

  CrystalHDDecodeReplay reopened;
  Require(reopened.Append(1, true, {0x65}) && !reopened.failed(),
          "a fresh replay context opens normally after allocation failure");
}

void SetGeometry(Fixture *fixture, unsigned coded_width,
                 unsigned coded_height, unsigned surface_width,
                 unsigned surface_height) {
  Require((coded_width & 15U) == 0 && (coded_height & 15U) == 0,
          "H.264 coded fixture uses complete macroblocks");
  fixture->decode->width = coded_width;
  fixture->decode->height = coded_height;
  fixture->picture.picture_width_in_mbs_minus1 = coded_width / 16 - 1;
  fixture->picture.picture_height_in_mbs_minus1 = coded_height / 16 - 1;
  auto surface = std::make_shared<Surface>();
  Require(surface->AllocateInternal(nullptr, -1, surface_width,
                                    surface_height, VA_FOURCC_NV12),
          "allocate H.264 geometry surface");
  fixture->driver.surfaces[1] = std::move(surface);
}

void VisibleCropSurfaceGeometry() {
  {
    Fixture f;
    SetGeometry(&f, 1280, 544, 1280, 534);
    f.Begin();
    f.Parameters();
    f.End();
    Require(f.mock.inputs.size() == 1 &&
                f.decode->next_timestamp == 2 * kTimestampStep,
            "1280x534 visible target accepts one 1280x544 coded picture");
    const auto decoded = f.decode->decoded_frames.at(kTimestampStep);
    Require(decoded->width == 1280 && decoded->height == 534,
            "private picture retains the target's visible geometry");

    std::vector<uint8_t> pixels(1280 * 544 * 2, 128);
    for (size_t offset = 0; offset < pixels.size(); offset += 2)
      pixels[offset] = 73;
    BC_DTS_PROC_OUT output = {};
    output.Ybuff = pixels.data();
    output.YBuffDoneSz = pixels.size() / 4;
    output.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    output.PicInfo.timeStamp = kTimestampStep;
    output.PicInfo.width = 1280;
    output.PicInfo.height = 544;
    Require(ProcessDecodedOutput(&f.driver, f.decode.get(), output) ==
                VA_STATUS_SUCCESS,
            "coded hardware output is clipped to the visible target");
    const auto target = f.driver.surfaces.at(1);
    Require(target->ready && !target->failed &&
                target->width == 1280 && target->height == 534 &&
                target->planes[0][static_cast<size_t>(533) * target->pitch[0]] ==
                    73,
            "last visible luma row is complete without exposing coded padding");
  }

  {
    Fixture f;
    SetGeometry(&f, 1296, 16, 1282, 16);
    f.Begin();
    f.Parameters();
    f.End();
    std::vector<uint8_t> pixels(1296 * 16 * 2, 128);
    for (size_t offset = 0; offset < pixels.size(); offset += 2)
      pixels[offset] = 61;
    BC_DTS_PROC_OUT output = {};
    output.Ybuff = pixels.data();
    output.YBuffDoneSz = pixels.size() / 4;
    output.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    output.PicInfo.timeStamp = kTimestampStep;
    output.PicInfo.width = 1296;
    output.PicInfo.height = 16;
    Require(ProcessDecodedOutput(&f.driver, f.decode.get(), output) ==
                VA_STATUS_SUCCESS &&
                f.driver.surfaces.at(1)->planes[0][1281] == 61,
            "even visible width in the final coded macroblock is accepted and clipped");
  }

  for (unsigned surface_width : {1280U, 1281U}) {
    Fixture f;
    SetGeometry(&f, 1296, 16, surface_width, 16);
    f.Begin();
    f.Parameters();
    Require(EndPicture(&f.context, 1) == VA_STATUS_ERROR_INVALID_SURFACE &&
                f.mock.inputs.empty() && f.decode->decoded_frames.empty(),
            "a full-macroblock width deficit or odd 4:2:0 crop remains invalid");
  }

  for (unsigned surface_height : {528U, 533U}) {
    Fixture f;
    SetGeometry(&f, 1280, 544, 1280, surface_height);
    f.Begin();
    f.Parameters();
    Require(EndPicture(&f.context, 1) == VA_STATUS_ERROR_INVALID_SURFACE &&
                f.mock.inputs.empty() && f.decode->decoded_frames.empty() &&
                f.decode->next_timestamp == kTimestampStep,
            "a full-macroblock deficit or odd 4:2:0 crop remains invalid");
  }

  {
    Fixture f;
    SetGeometry(&f, 1280, 544, 1280, 534);
    f.picture.seq_fields.bits.frame_mbs_only_flag = 0;
    f.Begin();
    f.Parameters();
    Require(EndPicture(&f.context, 1) == VA_STATUS_ERROR_INVALID_SURFACE &&
                f.mock.inputs.empty(),
            "interlaced 4:2:0 requires a four-row vertical crop unit");
  }

  {
    Fixture f;
    f.Begin();
    f.Parameters();
    auto owner = std::make_shared<Surface>();
    Require(owner->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
            "allocate canonical alias owner");
    f.driver.surfaces[2] = std::move(owner);
    f.driver.surfaces.at(1)->backing_owner = 2;
    Require(EndPicture(&f.context, 1) == VA_STATUS_ERROR_INVALID_SURFACE &&
                f.mock.inputs.empty(),
            "cropped geometry support does not admit an alias decode target");
  }
}

void BufferOrderAndSnapshot() {
  for (bool one_call : {false, true}) {
  std::array<unsigned, 4> order = {0, 1, 2, 3};
  do {
    Fixture f;
    const auto expected = Custom(17);
    auto iq = expected;
    const uint8_t bytes[] = {0x65, 0x88, 0x84};
    VABufferID ids[] = {f.Buffer(VAIQMatrixBufferType, &iq, sizeof(iq)),
      f.Buffer(VAPictureParameterBufferType, &f.picture, sizeof(f.picture)),
      f.Buffer(VASliceParameterBufferType, &f.slice, sizeof(f.slice)),
      f.Buffer(VASliceDataBufferType, bytes, sizeof(bytes))};
    f.Begin();
    if (one_call) {
      VABufferID ordered[] = {ids[order[0]], ids[order[1]], ids[order[2]], ids[order[3]]};
      Require(RenderPicture(&f.context, 1, ordered, 4) == VA_STATUS_SUCCESS,
              "arbitrary buffer order in a single public call");
    } else {
      for (unsigned index : order) f.Render(ids[index]);
    }
    std::memset(&iq, 0, sizeof(iq));
    void *mapped = nullptr;
    Require(MapBuffer(&f.context, ids[0], &mapped) == VA_STATUS_SUCCESS, "map submitted IQ buffer");
    std::memset(mapped, 0, sizeof(iq));
    Require(UnmapBuffer(&f.context, ids[0]) == VA_STATUS_SUCCESS &&
            DestroyBuffer(&f.context, ids[0]) == VA_STATUS_SUCCESS, "release original submitted IQ storage");
    f.End();
    Require(f.mock.inputs.size() == 1, "one original AU submitted");
    CheckMatrices(f.mock.inputs[0].bytes, expected);
  } while (std::next_permutation(order.begin(), order.end()));
  }
}
void MissingResetsFlat() {
  Fixture f;
  auto iq = Custom();
  f.Begin(); f.Parameters(); f.Render(f.Buffer(VAIQMatrixBufferType, &iq, sizeof(iq))); f.End();
  CheckMatrices(f.mock.inputs.back().bytes, iq);
  f.Begin(); f.Parameters(false); f.End();
  Require(f.mock.inputs.size() == 2, "custom then missing submits two AUs");
  CheckMatrices(f.mock.inputs.back().bytes, Flat());
}
void MalformedSizeAndRecovery() {
  for (size_t bytes : {size_t(1), sizeof(VAIQMatrixBufferH264) - 1,
                       sizeof(VAIQMatrixBufferH264) + 1, 2 * sizeof(VAIQMatrixBufferH264)}) {
    Fixture f;
    std::vector<uint8_t> malformed(bytes, 16);
    f.Begin(); f.Parameters();
    const auto previous = Custom(19);
    f.Render(f.Buffer(VAIQMatrixBufferType, &previous, sizeof(previous)));
    VABufferID id = f.Buffer(VAIQMatrixBufferType, malformed.data(), malformed.size());
    Require(RenderPicture(&f.context, 1, &id, 1) == VA_STATUS_ERROR_INVALID_BUFFER,
            "reject nonexact IQ structure size");
    f.decode->decoder_started = false; // Any accidental open hits the abort stub.
    Require(EndPicture(&f.context, 1) != VA_STATUS_SUCCESS && f.mock.inputs.empty() &&
            f.decode->decoded_frames.empty() && f.decode->next_timestamp == kTimestampStep,
            "invalid IQ poisons earlier valid IQ before allocation/open/submission");
    f.decode->decoder_started = true;
    f.Begin(); f.Parameters(); f.End();
    CheckMatrices(f.mock.inputs.back().bytes, Flat());
  }
  Fixture f;
  f.Begin(); f.Parameters();
  uint8_t wrong = 0;
  VABufferID id = f.Buffer(VAIQMatrixBufferType, &wrong, 1);
  Require(RenderPicture(&f.context, 1, &id, 1) == VA_STATUS_ERROR_INVALID_BUFFER, "malformed IQ starts failed transaction");
  auto valid = Custom(41);
  f.Render(f.Buffer(VAIQMatrixBufferType, &valid, sizeof(valid)));
  f.End(); CheckMatrices(f.mock.inputs.back().bytes, valid);
}
void ActiveZeroAndInactive8x8() {
  for (unsigned list = 0; list < 8; ++list) {
    Fixture f;
    auto iq = Flat();
    if (list < 6) iq.ScalingList4x4[list][list + 1] = 0;
    else iq.ScalingList8x8[list - 6][63] = 0;
    f.Begin(); f.Render(f.Buffer(VAIQMatrixBufferType, &iq, sizeof(iq))); f.Parameters();
    f.decode->decoder_started = false;
    Require(EndPicture(&f.context, 1) == VA_STATUS_ERROR_INVALID_PARAMETER && f.mock.inputs.empty() &&
            f.decode->decoded_frames.empty() && f.decode->next_timestamp == kTimestampStep,
            "zero in any active IQ list rejects before library submission");
  }
  Fixture f;
  f.picture.pic_fields.bits.transform_8x8_mode_flag = 0;
  auto iq = Custom(3);
  std::memset(iq.ScalingList8x8, 0, sizeof(iq.ScalingList8x8));
  f.Begin(); f.Render(f.Buffer(VAIQMatrixBufferType, &iq, sizeof(iq))); f.Parameters(); f.End();
  CheckMatrices(f.mock.inputs.back().bytes, iq, false);
}
void LegacyProfilesRejectCustom() {
  for (VAProfile profile : {VAProfileH264ConstrainedBaseline, VAProfileH264Main}) {
    Fixture f; f.driver.configs[1].profile = profile;
    f.picture.pic_fields.bits.transform_8x8_mode_flag = 0;
    auto iq = Flat(); iq.ScalingList4x4[4][3] = 17;
    f.Begin(); f.Parameters(); f.Render(f.Buffer(VAIQMatrixBufferType, &iq, sizeof(iq)));
    f.decode->decoder_started = false;
    Require(EndPicture(&f.context, 1) == VA_STATUS_ERROR_INVALID_PARAMETER && f.mock.inputs.empty(),
            "non-High profiles cannot silently flatten custom active matrices");
    f.decode->decoder_started = true;
    f.Begin(); f.Parameters(); f.End();
    Require(f.mock.inputs.size() == 1, "non-High ordinary flat picture remains supported");
  }
}
void ValidReplacementAndMixedFlat() {
  Fixture f;
  auto old = Custom(33), replacement = Flat();
  replacement.ScalingList4x4[4][7] = 201;
  replacement.ScalingList8x8[1][19] = 249;
  f.Begin(); f.Render(f.Buffer(VAIQMatrixBufferType, &old, sizeof(old)));
  f.Parameters(); f.Render(f.Buffer(VAIQMatrixBufferType, &replacement, sizeof(replacement)));
  f.End();
  CheckMatrices(f.mock.inputs.back().bytes, replacement);
}
void OriginalReplayRetainsMatrices() {
  Fixture f;
  const auto first = Custom(1), second = Custom(91);
  f.mock.capacity = false;
  f.Begin(); f.Parameters(); f.Render(f.Buffer(VAIQMatrixBufferType, &first, sizeof(first))); f.End();
  Require(f.mock.inputs.empty() && f.decode->replay.NextInput() != nullptr,
          "input bytes are owned before TX admission");
  const auto retained = f.decode->replay.NextInput()->bytes;
  CheckMatrices(retained, first);
  f.Begin(); f.Parameters(false); f.Render(f.Buffer(VAIQMatrixBufferType, &second, sizeof(second))); f.End();
  Require(f.decode->replay.NextInput()->bytes == retained, "later transaction does not mutate queued PPS");
  f.mock.capacity = true;
  Require(PumpDecodeInput(&f.driver, f.decode.get()) == VA_STATUS_SUCCESS && f.mock.inputs.size() == 2,
          "actual pump forwards both owned AUs");
  const auto originals = f.mock.inputs;
  CheckMatrices(originals[0].bytes, first); CheckMatrices(originals[1].bytes, second);
  Require(f.decode->replay.Seal(), "seal original mocked input prefix");
  for (const auto &unit : originals)
    Require(f.decode->replay.Observe(unit.timestamp) == CrystalHDDecodeReplay::Output::New,
            "model completed original timestamp");
  f.Begin(); f.Parameters(false); f.End();
  Require(f.mock.inputs.size() == 2 && f.decode->replay.EndOfSequence() && f.decode->replay.Restarted(),
          "retained prefix becomes replayable after modeled EOS/restart");
  Require(PumpDecodeInput(&f.driver, f.decode.get()) == VA_STATUS_SUCCESS && f.mock.inputs.size() == 5,
          "actual pump sends replay prefix and new flat transaction");
  for (unsigned n = 0; n < 2; ++n)
    Require(f.mock.inputs[n + 2].timestamp == originals[n].timestamp &&
            f.mock.inputs[n + 2].bytes == originals[n].bytes,
            "replay carries exact original PPS bytes and timestamp, not latest context IQ");
  CheckMatrices(f.mock.inputs[4].bytes, Flat());
}

void Live(Fixture *fixture) {
  fixture->decode->live_h264 = true;
  fixture->decode->live_h264_gap_tolerant = true;
  fixture->decode->live_h264_fast_path = true;
  fixture->decode->replay = CrystalHDDecodeReplay(true);
  fixture->driver.surfaces.at(1)->direct_decode_eligible = true;
}

void Output(Fixture *fixture, uint64_t timestamp, uint8_t luma) {
  std::vector<uint8_t> pixels(16 * 16 * 2, 128);
  for (size_t i = 0; i < pixels.size(); i += 2) pixels[i] = luma;
  BC_DTS_PROC_OUT output = {};
  output.Ybuff = pixels.data();
  output.YBuffDoneSz = pixels.size() / 4;
  output.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
  output.PicInfo.timeStamp = timestamp;
  output.PicInfo.width = output.PicInfo.height = 16;
  Require(ProcessDecodedOutput(&fixture->driver, fixture->decode.get(), output) ==
              VA_STATUS_SUCCESS, "process exact live hardware picture");
}

void SupersededPendingCpuBackingRetiresAfterCommit() {
  constexpr unsigned dimension = 64;
  constexpr size_t bytes = dimension * (dimension + dimension / 2);
  Require(bytes > sizeof(Surface) + 512,
          "the backing fault threshold excludes Surface/control-block allocations");
  const auto output = [](Fixture *fixture, uint64_t timestamp, uint8_t luma) {
    std::vector<uint8_t> pixels(dimension * dimension * 2, 128);
    for (size_t i = 0; i < pixels.size(); i += 2) pixels[i] = luma;
    BC_DTS_PROC_OUT reported = {};
    reported.Ybuff = pixels.data();
    reported.YBuffDoneSz = pixels.size() / 4;
    reported.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    reported.PicInfo.timeStamp = timestamp;
    reported.PicInfo.width = reported.PicInfo.height = dimension;
    Require(ProcessDecodedOutput(&fixture->driver, fixture->decode.get(), reported) ==
                VA_STATUS_SUCCESS,
            "process the exact CPU-backing regression picture");
  };
  {
    Fixture f;
    SetGeometry(&f, dimension, dimension, dimension, dimension);
    f.mock.capacity = false;
    const HANDLE device = f.decode->device;
    const uint64_t generation = f.decode->generation;
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "queue the first pending private picture without hardware output");
    uint8_t *const first = f.decode->decoded_frames.at(kTimestampStep)->storage.data();
    const uint64_t first_identity =
        f.decode->decoded_frames.at(kTimestampStep)->decode_identity;
    Require(SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "commit the replacement before reclaiming unobservable old pixels");
    f.Begin(); f.Parameters(false);
    allocation_fault::fail_at_least = bytes;
    const VAStatus status = EndPicture(&f.context, 1);
    const bool consumed = allocation_fault::fail_at_least == SIZE_MAX;
    allocation_fault::fail_at_least = SIZE_MAX;
    Require(status == VA_STATUS_SUCCESS && !consumed &&
                f.decode->pending.size() == 3 && f.decode->decoded_frames.size() == 3 &&
                f.decode->replay.cached_pictures() == 3 && !f.decode->replay.failed() &&
                f.decode->next_timestamp == 4 * kTimestampStep && f.mock.inputs.empty(),
            "a third pending submission reuses retired CPU bytes without a fresh backing allocation");
    Require(f.decode->decoded_frames.at(kTimestampStep)->storage.empty() &&
                f.decode->decoded_frames.at(kTimestampStep)->planes[0] == nullptr &&
                f.decode->decoded_frames.at(kTimestampStep)->planes[1] == nullptr &&
                f.decode->decoded_frames.at(kTimestampStep)->width == dimension &&
                f.decode->decoded_frames.at(kTimestampStep)->height == dimension &&
                f.decode->decoded_frames.at(kTimestampStep)->decode_identity == first_identity &&
                f.decode->decoded_frames.at(kTimestampStep)->expected_timestamp == kTimestampStep &&
                !f.decode->decoded_frames.at(kTimestampStep)->ready &&
                !f.decode->decoded_frames.at(kTimestampStep)->failed &&
                f.decode->decoded_frames.at(2 * kTimestampStep)->storage.empty() &&
                f.decode->decoded_frames.at(3 * kTimestampStep)->storage.size() == bytes &&
                f.decode->decoded_frames.at(3 * kTimestampStep)->storage.data() == first &&
                f.decode->private_backing_pool->Cached().buffers == 1 &&
                f.decode->private_backing_pool->Cached().bytes == bytes,
            "only superseded unobservable backing is recycled, not pending identities or geometry");
    f.mock.capacity = true;
    Require(PumpDecodeInput(&f.driver, f.decode.get()) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 3 && f.decode->replay.outstanding() == 3,
            "all three accepted timestamps still reach transport exactly once");
    const auto target = f.driver.surfaces.at(1);
    const auto unchanged = target->storage;
    output(&f, 2 * kTimestampStep, 0x22);
    output(&f, kTimestampStep, 0x11);
    Require(f.decode->pending.size() == 1 &&
                f.decode->pending.count(3 * kTimestampStep) == 1 &&
                f.decode->replay.outstanding() == 1 &&
                target->storage == unchanged && !target->ready,
            "late old outputs retire only their own tokens without touching the new public backing");
    output(&f, 3 * kTimestampStep, 0x33);
    Require(SyncSurface2(&f.context, 1, 0) == VA_STATUS_SUCCESS &&
                target->planes[0][0] == 0x33 && f.decode->pending.empty() &&
                f.decode->replay.outstanding() == 0 && !f.decode->replay.failed() &&
                f.decode->device == device && f.decode->generation == generation &&
                f.mock.close_count == 0 && f.mock.reopen_count == 0,
            "the new binding receives only its real pixels without reset or lost accepted work");
  }
  {
    Fixture f;
    SetGeometry(&f, dimension, dimension, dimension, dimension);
    f.mock.capacity = false;
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "prepare an independently captured pending picture");
    const auto captured = f.decode->decoded_frames.at(kTimestampStep);
    uint8_t *const backing = captured->storage.data();
    Require(SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
                captured->storage.size() == bytes && captured->storage.data() == backing &&
                f.decode->private_backing_pool->Cached().buffers == 0,
            "an existing strong private reader exempts a pending picture from backing retirement");
    const auto pending = f.decode->pending;
    const auto frames = f.decode->decoded_frames;
    const uint64_t timestamp = f.decode->next_timestamp;
    f.Begin(); f.Parameters(false);
    allocation_fault::fail_at_least = bytes;
    const VAStatus status = EndPicture(&f.context, 1);
    const bool consumed = allocation_fault::fail_at_least == SIZE_MAX;
    allocation_fault::fail_at_least = SIZE_MAX;
    Require(status == VA_STATUS_ERROR_ALLOCATION_FAILED && consumed &&
                f.decode->pending == pending && f.decode->decoded_frames == frames &&
                f.decode->next_timestamp == timestamp && f.mock.inputs.empty() &&
                captured->storage.size() == bytes && captured->storage.data() == backing &&
                !captured->failed && !f.decode->replay.failed(),
            "fresh allocation failure still preserves every captured pending picture before binding");
    f.mock.capacity = true;
    Require(PumpDecodeInput(&f.driver, f.decode.get()) == VA_STATUS_SUCCESS,
            "captured accepted input remains transportable after the unrelated allocation error");
    output(&f, kTimestampStep, 0x44);
    Require(captured->ready && !captured->failed && captured->planes[0][0] == 0x44 &&
                captured->frame_timestamp == kTimestampStep,
            "the independently retained pending picture still receives its exact delayed pixels");
    output(&f, 2 * kTimestampStep, 0x55);
    Require(f.decode->pending.empty() && f.decode->replay.outstanding() == 0 &&
                !f.decode->replay.failed() && captured->planes[0][0] == 0x44,
            "allocation-error recovery does not lose unrelated accepted output");
  }
}

void PrivateBackingReusePreservesCapturedPictures() {
  Fixture f;
  Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
          "submit a strict private picture before cache reuse");
  Output(&f, kTimestampStep, 40);
  auto captured = std::make_shared<DecodeContext>();
  captured->video_process = true;
  captured->vpp_frame = f.decode->decoded_frames.at(kTimestampStep);
  PendingVpp queued;
  queued.source = captured->vpp_frame;
  uint8_t *const first_backing = queued.source->storage.data();
  const uint64_t first_identity = queued.source->decode_identity;
  const auto original = queued.source->storage;
  Require(SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS,
          "submit the replacement before retiring its earlier private picture");
  Output(&f, 2 * kTimestampStep, 60);
  Require(f.decode->decoded_frames.size() == 1 &&
              f.decode->decoded_frames.at(2 * kTimestampStep)->storage.data() != first_backing &&
              f.decode->private_backing_pool->Cached().buffers == 0 &&
              queued.source->storage == original && queued.source->decode_identity == first_identity,
          "captured and queued VPP references keep retired immutable pixels out of the pool");
  captured->vpp_frame.reset();
  Require(f.decode->private_backing_pool->Cached().buffers == 0,
          "a queued source still owns its storage after capture releases it");
  queued.source.reset();
  Require(f.decode->private_backing_pool->Cached().buffers == 1,
          "final queued reference returns only its CPU backing");
  Require(SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS,
          "strict submission consumes a safely released backing");
  const auto next = f.decode->decoded_frames.at(3 * kTimestampStep);
  Require(next->storage.data() == first_backing && !next->direct_picture &&
              !next->ready && !next->failed && next->frame_timestamp == 0 &&
              next->expected_timestamp == 3 * kTimestampStep &&
              next->decode_identity != first_identity &&
              next->trace_submission_ordinal == 3 &&
              std::all_of(next->storage.begin(), next->storage.end(),
                          [](uint8_t byte) { return byte == 0; }),
          "recycled storage starts zero with a fresh strict identity and timestamp");
  Output(&f, 3 * kTimestampStep, 80);
  for (unsigned plane = 0; plane < 2; ++plane)
    for (unsigned row = 0; row < (plane == 0 ? 16U : 8U); ++row)
      for (unsigned column = 0; column < next->pitch[plane]; ++column)
        Require(next->planes[plane][row * next->pitch[plane] + column] ==
                    (column >= 16 ? 0 : plane == 0 ? 80 : 128),
                "reused decoded payload and every aligned padding byte are exact");
}

void PrivateBackingAllocationFailureIsTransactional() {
  Fixture f;
  Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
          "establish pending output before a private allocation failure");
  const auto pending = f.decode->pending;
  const auto frames = f.decode->decoded_frames;
  const uint64_t timestamp = f.decode->next_timestamp;
  const uint64_t identity = f.driver.surfaces.at(1)->decode_identity;
  f.Begin();
  f.Parameters(false);
  allocation_fault::fail_at_least = 64 * (16 + 8);
  const VAStatus status = EndPicture(&f.context, 1);
  const bool consumed = allocation_fault::fail_at_least == SIZE_MAX;
  allocation_fault::fail_at_least = SIZE_MAX;
  Require(status == VA_STATUS_ERROR_ALLOCATION_FAILED && consumed &&
              f.decode->pending == pending && f.decode->decoded_frames == frames &&
              f.decode->next_timestamp == timestamp && f.mock.inputs.size() == 1 &&
              f.driver.surfaces.at(1)->decode_identity == identity && !f.decode->replay.failed(),
          "failed private backing allocation cannot retire output or publish replay/binding");
  Output(&f, kTimestampStep, 40);
  Require(SyncSurface2(&f.context, 1, 0) == VA_STATUS_ERROR_DECODING_ERROR,
          "a consumed allocation-failed End cannot expose late old public output");
  Require(SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS,
          "a fresh submission still works after private allocation failure");
}

VABufferID Vpp(Fixture *fixture, VASurfaceID source = 1) {
  auto processor = std::make_shared<DecodeContext>();
  processor->video_process = true;
  fixture->driver.contexts[2] = processor;
  auto target = std::make_shared<Surface>();
  Require(target->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
          "allocate CPU VPP destination");
  fixture->driver.surfaces[2] = target;
  VAProcPipelineParameterBuffer pipeline = {};
  pipeline.surface = source;
  return fixture->Buffer(VAProcPipelineParameterBufferType, &pipeline,
                          sizeof(pipeline), 2);
}

void RequireRejectedPublicReads(Fixture *fixture, VASurfaceID surface = 1) {
  Require(SyncSurface2(&fixture->context, surface, 0) == VA_STATUS_ERROR_DECODING_ERROR,
          "new public sync cannot expose a rejected transaction");
  VASurfaceStatus state = VASurfaceReady;
  Require(QuerySurfaceStatus(&fixture->context, surface, &state) ==
              VA_STATUS_ERROR_DECODING_ERROR,
          "new public query cannot report rejected transaction READY");
  VAImage image = {}, derived = {};
  VAImageFormat format = {};
  format.fourcc = VA_FOURCC_NV12;
  Require(DeriveImage(&fixture->context, surface, &derived) ==
              VA_STATUS_ERROR_DECODING_ERROR &&
              CreateImage(&fixture->context, &format, 16, 16, &image) == VA_STATUS_SUCCESS,
          "new derive rejects without preventing independent image allocation");
  auto &storage = fixture->driver.buffers.at(image.buf).data;
  std::fill(storage.begin(), storage.end(), 0x93);
  Require(GetImage(&fixture->context, surface, 0, 0, 16, 16, image.image_id) ==
              VA_STATUS_ERROR_DECODING_ERROR &&
              std::all_of(storage.begin(), storage.end(), [](uint8_t v) { return v == 0x93; }),
          "new GetImage rejects without changing destination bytes");
  Require(DestroyImage(&fixture->context, image.image_id) == VA_STATUS_SUCCESS,
          "independent destination remains releasable");
  VADRMPRIMESurfaceDescriptor descriptor = {};
  for (uint32_t access : {uint32_t(VA_EXPORT_SURFACE_READ_ONLY),
                          uint32_t(VA_EXPORT_SURFACE_READ_WRITE), uint32_t(0)})
    Require(ExportSurfaceHandle(&fixture->context, surface,
                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, access, &descriptor) ==
                VA_STATUS_ERROR_DECODING_ERROR,
            "new read/default export rejects before graphics or fd access");
  VABufferID parameters = Vpp(fixture, surface);
  Require(BeginPicture(&fixture->context, 2, 2) == VA_STATUS_SUCCESS &&
              RenderPicture(&fixture->context, 2, &parameters, 1) ==
                  VA_STATUS_ERROR_DECODING_ERROR,
          "new VPP capture cannot acquire the old accepted picture through rejected public identity");
}

void RejectedH264TargetReadback() {
  Fixture f;
  Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
          "accept the old picture before public target reuse");
  Output(&f, kTimestampStep, 40);
  Require(SyncSurface2(&f.context, 1, 0) == VA_STATUS_SUCCESS,
          "the old picture really was ready before the failed transaction");
  std::vector<uint8_t> rejected(
      crystalhd_access_unit_limits::kVaH264SubmissionBytes + 1, 0x55);
  rejected[0] = 0x41;
  Require(SubmitH264(&f, rejected) == VA_STATUS_ERROR_INVALID_PARAMETER,
          "reject the new oversized reference picture before submission");
  Require(SyncSurface2(&f.context, 1, 0) == VA_STATUS_ERROR_DECODING_ERROR,
          "failed new picture cannot sync successfully using old READY pixels");
  VASurfaceStatus state = VASurfaceReady;
  Require(QuerySurfaceStatus(&f.context, 1, &state) == VA_STATUS_ERROR_DECODING_ERROR,
          "query cannot advertise the failed public transaction as ready");
  VAImage derived = {};
  Require(DeriveImage(&f.context, 1, &derived) == VA_STATUS_ERROR_DECODING_ERROR,
          "derive cannot snapshot the old pixels as the rejected picture");
  VAImageFormat format = {};
  format.fourcc = VA_FOURCC_NV12;
  VAImage image = {};
  Require(CreateImage(&f.context, &format, 16, 16, &image) == VA_STATUS_SUCCESS,
          "allocate an independent image for failed readback");
  std::fill(f.driver.buffers.at(image.buf).data.begin(),
            f.driver.buffers.at(image.buf).data.end(), 0x93);
  Require(GetImage(&f.context, 1, 0, 0, 16, 16, image.image_id) ==
              VA_STATUS_ERROR_DECODING_ERROR,
          "GetImage cannot copy the failed public transaction");
  void *mapped = nullptr;
  Require(MapBuffer(&f.context, image.buf, &mapped) == VA_STATUS_SUCCESS &&
              static_cast<uint8_t *>(mapped)[0] == 0x93 &&
              UnmapBuffer(&f.context, image.buf) == VA_STATUS_SUCCESS,
          "failed GetImage leaves the mapped destination untouched");
  VADRMPRIMESurfaceDescriptor descriptor = {};
  for (uint32_t access : {uint32_t(VA_EXPORT_SURFACE_READ_ONLY),
                          uint32_t(VA_EXPORT_SURFACE_READ_WRITE), uint32_t(0)})
    Require(ExportSurfaceHandle(&f.context, 1,
                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, access, &descriptor) ==
                VA_STATUS_ERROR_DECODING_ERROR,
            "read/default export rejects before any graphics or fd operation");
  const VABufferID parameters = Vpp(&f);
  Require(BeginPicture(&f.context, 2, 2) == VA_STATUS_SUCCESS &&
              RenderPicture(&f.context, 2, const_cast<VABufferID *>(&parameters), 1) ==
                  VA_STATUS_ERROR_DECODING_ERROR,
          "new VPP capture cannot resurrect the revoked private binding");
  Require(SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_ERROR_DECODING_ERROR &&
              SyncSurface2(&f.context, 1, 0) == VA_STATUS_ERROR_DECODING_ERROR,
          "dependent rejection remains a public decode error");
  Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
          "an admitted IDR starts a fresh public binding");
  Output(&f, 2 * kTimestampStep, 80);
  Require(SyncSurface2(&f.context, 1, 0) == VA_STATUS_SUCCESS &&
              f.driver.surfaces.at(1)->planes[0][0] == 80,
          "accepted recovery exposes its own pixels, not the rejected picture");
}

void RejectedH264RecoveryRetainsDelayedReorderedPictures() {
  // Exercise driver-owned picture identities, not a simulated CABAC/DPB or
  // client error-recovery policy. Old B/P output must remain independently
  // completable after a recovery IDR rebinds their reused public target.
  Fixture f;
  f.mock.legacy_pes_family = true;
  f.picture.num_ref_frames = 5;
  f.picture.pic_fields.bits.reference_pic_flag = 1;
  f.slice.slice_type = 2;
  Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
          "accept an IDR before retaining delayed reordered pictures");
  Output(&f, kTimestampStep, 0x11);
  f.slice.slice_type = 0;
  Require(SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS,
          "accept the future P anchor before its B picture");
  // A real private reader must capture P before its public target is reused.
  const auto old_p = f.decode->decoded_frames.at(2 * kTimestampStep);
  f.slice.slice_type = 1;
  Require(SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS,
          "accept a reference B with genuine output still outstanding");
  const auto old_b = f.decode->decoded_frames.at(3 * kTimestampStep);
  const auto target = f.driver.surfaces.at(1);
  const auto pending = f.decode->pending;
  const auto frames = f.decode->decoded_frames;
  const uint64_t timestamp = f.decode->next_timestamp;
  const uint64_t ordinal = f.decode->next_submission_ordinal;
  const size_t cache = f.decode->replay.cached_bytes();
  const HANDLE device = f.decode->device;
  const uint64_t generation = f.decode->generation;
  std::vector<uint8_t> rejected(
      crystalhd_access_unit_limits::kVaH264SubmissionBytes + 1, 0x55);
  rejected[0] = 0x65;
  f.slice.slice_type = 2;
  // The picture/IQ/slice parameters now match the first IDR, so reuse its
  // measured header bytes to distinguish the extended raw guard from the
  // ordinary assembled cap or missing-fragment-transport rejection.
  auto assembled_rejected = f.mock.inputs.front().bytes;
  Require(assembled_rejected.size() > 3,
          "the accepted first IDR supplies measured reconstructed headers");
  assembled_rejected.resize(assembled_rejected.size() - 3);
  assembled_rejected.insert(assembled_rejected.end(), rejected.begin(), rejected.end());
  Require(assembled_rejected.size() >
                  crystalhd_access_unit_limits::kVaH264SubmissionBytes &&
              assembled_rejected.size() <=
                  H264SubmissionLimit(f.decode.get(), assembled_rejected.size()) &&
              !H264HeadroomDeescapedFits(assembled_rejected) &&
              H264FragmentInput() != nullptr,
          "the extended assembled/transport envelope permits this AU but its deescaped policy rejects it");
  Require(SubmitH264(&f, rejected) == VA_STATUS_ERROR_INVALID_PARAMETER &&
              f.mock.inputs.size() == 3 && f.decode->h264_waiting_for_idr &&
              f.decode->pending == pending && f.decode->decoded_frames == frames &&
              f.decode->next_timestamp == timestamp &&
              f.decode->next_submission_ordinal == ordinal &&
              f.decode->replay.cached_bytes() == cache &&
              f.decode->replay.outstanding() == 2 &&
              !f.decode->replay.failed() && PublicReadRejected(target.get()),
          "reference admission rejection preserves delayed work without consuming its recovery token");
  f.slice.slice_type = 0;
  Require(SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_ERROR_DECODING_ERROR &&
              f.mock.inputs.size() == 3 && f.decode->pending == pending &&
              f.decode->decoded_frames == frames &&
              f.decode->next_timestamp == timestamp &&
              f.decode->next_submission_ordinal == ordinal &&
              SyncSurface2(&f.context, 1, 0) == VA_STATUS_ERROR_DECODING_ERROR,
          "dependent input cannot expose old public pixels or consume the recovery timestamp");

  f.slice.slice_type = 2;
  Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
              f.mock.inputs.size() == 4 &&
              f.mock.inputs.back().timestamp == timestamp &&
              !f.decode->h264_waiting_for_idr &&
              f.decode->replay.outstanding() == 3 &&
              f.decode->pending.count(2 * kTimestampStep) == 1 &&
              f.decode->pending.count(3 * kTimestampStep) == 1 &&
              !PublicReadRejected(target.get()),
          "the real recovery IDR binds the same target before either delayed picture completes");
  const auto recovery = f.decode->decoded_frames.at(timestamp);
  const uint64_t identity = target->decode_identity;
  const auto public_pixels = target->storage;
  Output(&f, 3 * kTimestampStep, 0x33);
  Output(&f, 2 * kTimestampStep, 0x22);
  Require(old_b->ready && !old_b->failed && old_b->planes[0][0] == 0x33 &&
              old_b->frame_timestamp == 3 * kTimestampStep &&
              old_p->ready && !old_p->failed && old_p->planes[0][0] == 0x22 &&
              old_p->frame_timestamp == 2 * kTimestampStep &&
              target->storage == public_pixels && !target->ready && !target->failed &&
              target->expected_timestamp == timestamp &&
              target->decode_identity == identity && target->decode_picture.lock() == recovery &&
              f.decode->pending.size() == 1 && f.decode->pending.count(timestamp) == 1 &&
              f.decode->replay.outstanding() == 1 && !f.decode->replay.failed(),
          "reordered old B/P output completes only its retained private pictures, not the recovery target");
  {
    std::unique_lock<std::mutex> lock(f.driver.mutex);
    Require(SyncDecodeSurface(&f.driver, old_b, &lock, 0) == VA_STATUS_SUCCESS &&
                SyncDecodeSurface(&f.driver, old_p, &lock, 0) == VA_STATUS_SUCCESS,
            "both delayed accepted pictures remain independently readable after recovery rebinding");
  }
  Output(&f, 3 * kTimestampStep, 0xee);
  Require(old_b->planes[0][0] == 0x33 && target->storage == public_pixels &&
              f.decode->replay.outstanding() == 1 &&
              f.decode->pending.count(timestamp) == 1,
          "a repeated old output cannot replace accepted pixels or retire the recovery IDR");
  Output(&f, timestamp, 0x44);
  Require(SyncSurface2(&f.context, 1, 0) == VA_STATUS_SUCCESS &&
              target->planes[0][0] == 0x44 &&
              target->frame_timestamp == timestamp &&
              target->decode_identity == identity &&
              f.decode->pending.empty() && f.decode->replay.outstanding() == 0,
          "only the recovery IDR's own output makes its new public transaction ready");
  f.slice.slice_type = 0;
  Require(SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_SUCCESS &&
              f.mock.inputs.size() == 5 &&
              f.mock.inputs.back().timestamp == timestamp + kTimestampStep,
          "following dependent input resumes after the recovery IDR");
  Output(&f, timestamp + kTimestampStep, 0x55);
  Require(SyncSurface2(&f.context, 1, 0) == VA_STATUS_SUCCESS &&
              target->planes[0][0] == 0x55 &&
              old_b->planes[0][0] == 0x33 && old_p->planes[0][0] == 0x22 &&
              f.decode->pending.empty() && f.decode->replay.outstanding() == 0 &&
              !f.decode->replay.failed() && f.decode->device == device &&
              f.decode->generation == generation &&
              f.mock.close_count == 0 && f.mock.reopen_count == 0,
          "same-context continuation preserves every accepted picture without reset or stale public pixels");
}

void RejectedH264PreservesAcceptedPictures() {
  for (bool direct : {false, true}) {
    for (bool ready : {false, true}) {
      Fixture f;
      if (direct) Live(&f);
      Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
              "accept a picture before rejecting reuse");
      if (ready) Output(&f, kTimestampStep, 40);
      const auto target = f.driver.surfaces.at(1);
      const auto picture = f.decode->decoded_frames.at(kTimestampStep);
      const auto pending = f.decode->pending;
      const auto frames = f.decode->decoded_frames;
      const uint64_t identity = picture->decode_identity;
      const uint64_t timestamp = f.decode->next_timestamp;
      const size_t bytes = f.decode->replay.cached_bytes();
      // Model an already retained private reader, not a new public read after
      // rejection. Its lease and accepted timestamp must remain independent.
      PendingVpp retained;
      retained.source = picture;
      ++picture->vpp_readers;
      f.decode->h264_waiting_for_idr = true;
      Require(SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_ERROR_DECODING_ERROR &&
                  target->failed && !target->ready &&
                  target->decode_identity != identity && target->decode_picture.expired() &&
                  f.decode->pending == pending && f.decode->decoded_frames == frames &&
                  f.decode->next_timestamp == timestamp &&
                  f.decode->replay.cached_bytes() == bytes && !f.decode->replay.failed() &&
                  picture->decode_identity == identity && picture->vpp_readers == 1 &&
                  !picture->failed && !picture->direct_backing,
              "rejection revokes only the public target, retaining accepted private work and lease");
      if (!ready) Output(&f, kTimestampStep, 40);
      Require(target->failed && !target->ready &&
                  SyncSurface2(&f.context, 1, 0) == VA_STATUS_ERROR_DECODING_ERROR &&
                  picture->ready && !picture->failed && picture->planes[0][0] == 40 &&
                  picture->frame_timestamp == kTimestampStep &&
                  picture->expected_timestamp == kTimestampStep,
              "late output completes the old private picture without reviving the rejected public target");
      {
        std::unique_lock<std::mutex> lock(f.driver.mutex);
        Require(SyncDecodeSurface(&f.driver, picture, &lock, 0) == VA_STATUS_SUCCESS,
                "a retained private reader still synchronizes its accepted exact picture");
      }
      --picture->vpp_readers;
    }
  }
  for (bool ready : {false, true}) {
    Fixture f;
    Live(&f);
    VABufferID parameters = Vpp(&f);
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "accept old output for a real VPP capture");
    if (ready) Output(&f, kTimestampStep, 40);
    Require(BeginPicture(&f.context, 2, 2) == VA_STATUS_SUCCESS &&
                RenderPicture(&f.context, 2, &parameters, 1) == VA_STATUS_SUCCESS,
            "capture the exact private picture before failed public reuse");
    f.decode->h264_waiting_for_idr = true;
    Require(SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_ERROR_DECODING_ERROR,
            "reject dependent public reuse after the earlier capture");
    if (!ready) Output(&f, kTimestampStep, 40);
    Require(EndPicture(&f.context, 2) == VA_STATUS_SUCCESS &&
                f.driver.surfaces.at(2)->planes[0][0] == 40 &&
                f.driver.surfaces.at(2)->frame_timestamp == kTimestampStep &&
                f.driver.surfaces.at(1)->failed,
            "already captured VPP keeps its exact accepted pixels and timestamp");
  }
}

void RejectedH264OwnershipGuards() {
  for (unsigned mode = 0; mode < 8; ++mode) {
    Fixture f;
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "establish a ready binding before a competing owner");
    Output(&f, kTimestampStep, 40);
    const auto target = f.driver.surfaces.at(1);
    const auto picture = target->decode_picture.lock();
    const uint64_t identity = target->decode_identity;
    f.Begin(); f.Parameters(false);
    f.decode->h264_waiting_for_idr = true;
    switch (mode) {
      case 0: ++target->vpp_readers; break;
      case 1: ++target->vpp_writers; break;
      case 2: ++target->export_waiters; break;
      case 3:
        target->experimental_write_lease = true;
        target->experimental_write_context = 99;
        target->experimental_write_generation = 8;
        target->experimental_write_submission_ordinal = 9;
        break;
      case 4: target->experimental_read_export_armed = true; break;
      case 5: {
        auto other = std::make_shared<DecodeContext>();
        other->config = 1;
        f.driver.contexts[3] = other;
        Require(BeginPicture(&f.context, 3, 1) == VA_STATUS_SUCCESS,
                "a newer Begin supersedes rejection even before any commit");
        break;
      }
      case 6: {
        auto replacement = std::make_shared<Surface>();
        Require(replacement->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
                "replace the public object without reusing its ownership");
        replacement->ready = true;
        f.driver.surfaces[1] = replacement;
        break;
      }
      case 7: target->backing_owner = 2; break;
    }
    Require(EndPicture(&f.context, 1) == VA_STATUS_ERROR_DECODING_ERROR &&
                target->ready && !target->failed && target->decode_identity == identity &&
                target->decode_picture.lock() == picture && !picture->failed &&
                !f.decode->replay.failed(),
            "a stale or busy rejection cannot revoke another owner or accepted private picture");
    if (mode == 3)
      Require(target->experimental_write_lease &&
                  target->experimental_write_context == 99 &&
                  target->experimental_write_generation == 8 &&
                  target->experimental_write_submission_ordinal == 9,
              "foreign experimental lease is completely unchanged");
    if (mode == 4)
      Require(target->experimental_read_export_armed,
              "an earlier external reader guard is not cleared by rejection");
    Require(PublicReadRejected(target.get()) == (mode < 5),
            "only the still-current canonical transaction receives the read barrier");
    if (mode == 5 || mode == 6)
      Require(SyncSurface2(&f.context, 1, 0) == VA_STATUS_SUCCESS,
              "rejection from an obsolete transaction cannot block a newer writer or public object");
  }
}

void RejectedH264PromotionFailure() {
  for (bool ready : {false, true}) {
    Fixture f;
    Live(&f);
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "accept a direct picture before rejected promotion");
    if (ready) Output(&f, kTimestampStep, 40);
    const auto target = f.driver.surfaces.at(1);
    const auto picture = target->decode_picture.lock();
    const uint64_t identity = target->decode_identity;
    const auto pending = f.decode->pending;
    VAImage old_snapshot = {};
    if (ready)
      Require(DeriveImage(&f.context, 1, &old_snapshot) == VA_STATUS_SUCCESS,
              "capture a detached old image before rejection");
    ++picture->vpp_readers;
    f.Begin(); f.Parameters(false);
    f.decode->h264_waiting_for_idr = true;
    allocation_fault::fail_next = true;
    Require(EndPicture(&f.context, 1) == VA_STATUS_ERROR_ALLOCATION_FAILED &&
                !allocation_fault::fail_next && target->decode_identity == identity &&
                target->decode_picture.lock() == picture && picture->direct_backing == target &&
                !target->failed && !picture->failed && picture->storage.empty() &&
                f.decode->pending == pending && !f.decode->replay.failed() &&
                picture->vpp_readers == 1,
            "promotion allocation failure leaves accepted pixels, ownership and replay intact");
    RequireRejectedPublicReads(&f);
    if (!ready) Output(&f, kTimestampStep, 40);
    Require(SyncSurface2(&f.context, 1, 0) == VA_STATUS_ERROR_DECODING_ERROR &&
                target->planes[0][0] == 40 && picture->frame_timestamp == kTimestampStep,
            "promotion failure preserves private pixels but cannot publish old public success");
    RequireRejectedPublicReads(&f);
    {
      std::unique_lock<std::mutex> lock(f.driver.mutex);
      Require(SyncDecodeSurface(&f.driver, picture, &lock, 0) == VA_STATUS_SUCCESS &&
                  picture->decode_identity == identity && picture->vpp_readers == 1 &&
                  picture->direct_backing == target && !picture->failed,
              "accepted private reads and leases survive both rejection and delayed completion");
    }
    if (ready) {
      void *mapped = nullptr;
      Require(MapBufferWithFlags(&f.context, old_snapshot.buf, &mapped, kImageMapRead) ==
                  VA_STATUS_SUCCESS && static_cast<uint8_t *>(mapped)[0] == 40 &&
                  UnmapBuffer(&f.context, old_snapshot.buf) == VA_STATUS_SUCCESS &&
                  DestroyImage(&f.context, old_snapshot.image_id) == VA_STATUS_SUCCESS,
              "an old derived snapshot remains readable and releasable after rejection");
    }
    auto alias = std::make_shared<Surface>();
    alias->backing_owner = 1;
    f.driver.surfaces[3] = alias;
    RequireRejectedPublicReads(&f, 3);
    const uint64_t rejected_transaction = target->picture_transaction;
    ++target->vpp_writers;
    Require(BeginPicture(&f.context, 1, 1) == VA_STATUS_ERROR_HW_BUSY &&
                target->picture_transaction == rejected_transaction && PublicReadRejected(target.get()),
            "failed Begin cannot supersede the rejected public transaction");
    --target->vpp_writers;
    // A mismatched direct backing cannot be copied into the retained picture.
    // Even this promotion failure must not mark the old private picture failed.
    f.Begin(); f.Parameters(false);
    Require(!PublicReadRejected(target.get()),
            "a successful newer Begin supersedes only the prior public-read barrier");
    target->frame_timestamp = 2 * kTimestampStep;
    Require(EndPicture(&f.context, 1) == VA_STATUS_ERROR_OPERATION_FAILED &&
                target->decode_identity == identity && !picture->failed &&
                picture->direct_backing == target && picture->storage.empty(),
            "failed exact-backing validation commits no partial promotion");
    RequireRejectedPublicReads(&f);
    target->frame_timestamp = kTimestampStep;
    Require(SubmitH264(&f, {0x41, 0x88, 0x84}) == VA_STATUS_ERROR_DECODING_ERROR &&
                target->failed && !picture->failed && !picture->direct_backing &&
                picture->planes[0][0] == 40,
            "a later safe preservation can revoke public access without poisoning old pixels");
    --picture->vpp_readers;
  }
}

void RejectedH264ConsumedEnd() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    Fixture f;
    Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
            "accept a picture before an unsuccessful consumed End");
    Output(&f, kTimestampStep, 40);
    const auto picture = f.decode->decoded_frames.at(kTimestampStep);
    const auto pixels = picture->storage;
    f.Begin();
    if (mode != 0) {
      if (mode == 1) f.picture.seq_fields.bits.frame_mbs_only_flag = 0;
      f.Parameters();
    }
    if (mode == 2) allocation_fault::fail_next = true;
    const VAStatus status = EndPicture(&f.context, 1);
    Require(status == (mode == 0 ? VA_STATUS_ERROR_INVALID_PARAMETER :
                       mode == 1 ? VA_STATUS_ERROR_UNSUPPORTED_PROFILE :
                                   VA_STATUS_ERROR_ALLOCATION_FAILED) &&
                !allocation_fault::fail_next && picture->storage == pixels && !picture->failed,
            "malformed, unsupported and assembly-allocation End failures retain private output");
    RequireRejectedPublicReads(&f);
  }
  Fixture f;
  Require(SubmitH264(&f, {0x65, 0x88, 0x84}) == VA_STATUS_SUCCESS,
          "establish a ready picture before recoverable Render input");
  Output(&f, kTimestampStep, 40);
  f.Begin();
  uint8_t short_matrix = 1;
  VABufferID bad = f.Buffer(VAIQMatrixBufferType, &short_matrix, sizeof(short_matrix));
  Require(RenderPicture(&f.context, 1, &bad, 1) == VA_STATUS_ERROR_INVALID_BUFFER &&
              !PublicReadRejected(f.driver.surfaces.at(1).get()),
          "a replaceable Render error does not prematurely reject its transaction");
  auto flat = Flat();
  f.Render(f.Buffer(VAIQMatrixBufferType, &flat, sizeof(flat)));
  f.Parameters();
  f.End();
  Output(&f, 2 * kTimestampStep, 80);
  Require(SyncSurface2(&f.context, 1, 0) == VA_STATUS_SUCCESS &&
              f.driver.surfaces.at(1)->planes[0][0] == 80,
          "valid Render replacement commits its own readable picture");
}

void ObsoleteSurfaceRetirementPreservesIndependentDecode() {
  // A newer sent IDR has retired the old prefix. Deleting its completed public
  // target is not a seek and must not cancel the independent pending timeline.
  for (bool retained_root : {false, true}) {
    Fixture f;
    for (VASurfaceID id : {2U, 3U}) {
      auto target = std::make_shared<Surface>();
      Require(target->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
              "allocate independent decode target");
      f.driver.surfaces[id] = target;
    }
    const auto submit = [&](VASurfaceID id, bool idr) {
      Require(BeginPicture(&f.context, 1, id) == VA_STATUS_SUCCESS,
              "begin a selected decode target");
      f.Parameters(idr);
      return EndPicture(&f.context, 1);
    };
    Require(submit(1, true) == VA_STATUS_SUCCESS, "accept original IDR");
    Output(&f, kTimestampStep, 40);
    const auto captured = f.decode->decoded_frames.at(kTimestampStep);
    const auto pixels = captured->storage;
    ++captured->vpp_readers;
    Require(submit(2, !retained_root) == VA_STATUS_SUCCESS,
            "accept the next independent IDR or dependent reference");
    Output(&f, 2 * kTimestampStep, 60);
    Require(f.decode->replay.cached_pictures() == (retained_root ? 2U : 1U) &&
                f.decode->replay.replayable(),
            "only an actual newer IDR prunes the complete old prefix");
    Require(submit(3, false) == VA_STATUS_SUCCESS, "accept unrelated pending P");
    const auto pending = f.driver.surfaces.at(3);
    const auto private_pending = f.decode->decoded_frames.at(3 * kTimestampStep);
    const uint64_t generation = f.decode->generation;
    const auto original_inputs = f.mock.inputs.size();
    VASurfaceID old = 1;
    Require(DestroySurfaces(&f.context, &old, 1) == VA_STATUS_SUCCESS &&
                f.driver.surfaces.count(old) == 0,
            "destroy only the completed old public target");
    if (retained_root) {
      Require(f.mock.close_count == 1 && f.decode->generation == generation + 1 &&
                  f.decode->device == nullptr && pending->failed && private_pending->failed,
              "destroying a retained reference still resets and cancels dependent pending work");
    } else {
      Require(f.mock.close_count == 0 && f.decode->generation == generation &&
                  f.decode->device == &f.mock && f.decode->replay.cached_pictures() == 2 &&
                  f.decode->pending.count(3 * kTimestampStep) == 1 &&
                  !pending->failed && !private_pending->failed &&
                  f.decode->surface_timestamps.size() == 2 &&
                  f.decode->decoded_frames.count(kTimestampStep) == 0,
              "obsolete eviction releases only its map owners, not independent replay or pending input");
      Output(&f, 3 * kTimestampStep, 80);
      Require(submit(3, false) == VA_STATUS_SUCCESS &&
                  f.mock.inputs.size() == original_inputs + 1 && f.mock.reopen_count == 0,
              "valid P continuation keeps the original hardware session and IDR history");
      Output(&f, 4 * kTimestampStep, 90);
    }
    {
      std::unique_lock<std::mutex> lock(f.driver.mutex);
      Require(SyncDecodeSurface(&f.driver, captured, &lock, 0) == VA_STATUS_SUCCESS &&
                  captured->storage == pixels && captured->frame_timestamp == kTimestampStep &&
                  !captured->failed,
              "captured completed private pixels remain readable through public retirement");
    }
    --captured->vpp_readers;
  }
  {
    Fixture f;
    CrystalHDDecodeReplay::Limits limits;
    limits.pictures = 2;
    f.decode->live_h264 = true;
    f.decode->replay = CrystalHDDecodeReplay(limits, true);
    for (VASurfaceID id : {1U, 2U, 3U}) {
      if (id != 1) {
        auto target = std::make_shared<Surface>();
        Require(target->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
                "allocate bounded-history target");
        f.driver.surfaces[id] = target;
      }
      Require(BeginPicture(&f.context, 1, id) == VA_STATUS_SUCCESS,
              "begin bounded-history target");
      f.Parameters(id == 1);
      f.End();
      if (id != 3) Output(&f, id * kTimestampStep, 40 + id * 10);
    }
    Require(!f.decode->replay.replayable() && f.decode->replay.cached_pictures() == 2,
            "completed history eviction is not proof of a newer independent IDR");
    const auto pending = f.driver.surfaces.at(3);
    const uint64_t generation = f.decode->generation;
    VASurfaceID old = 1;
    Require(DestroySurfaces(&f.context, &old, 1) == VA_STATUS_SUCCESS &&
                f.mock.close_count == 1 && f.decode->generation == generation + 1 && pending->failed,
            "unreplayable old history retains the conservative surface-pool reset");
  }
}

void DirectLiveOutputAndReuse() {
  for (unsigned int mode = 0; mode < 3; ++mode) {
    Fixture f;
    if (mode == 1) {
      f.decode->live_h264 = true;
      f.decode->replay = CrystalHDDecodeReplay(true);
    } else if (mode == 2) {
      Live(&f);
    }
    const bool direct = mode == 2;
    f.Begin(); f.Parameters(); f.End();
    std::weak_ptr<Surface> previous = f.decode->decoded_frames.at(kTimestampStep);
    Require(f.decode->decoded_frames.at(kTimestampStep)->storage.empty() == direct,
            "only the explicit H.264 fast path elides private frame allocation");
    Output(&f, kTimestampStep, 40);
    Require(f.driver.surfaces.at(1)->ready &&
                f.driver.surfaces.at(1)->planes[0][0] == 40 &&
                f.decode->decoded_frames.at(kTimestampStep)->storage.empty() == direct,
            "direct completion publishes actual pixels without a private copy");
    Output(&f, kTimestampStep, 90);
    Require(f.driver.surfaces.at(1)->planes[0][0] == 40,
            "duplicate cannot replace direct pixels");
    if (direct)
      f.driver.surfaces.at(1)->layout.valid = true;
    f.Begin(); f.Parameters(false); f.End();
    Require(f.decode->decoded_frames.at(2 * kTimestampStep)->storage.empty() == direct,
            "read-only export layout does not disable direct decode reuse");
    Require(previous.expired(), "completed uncaptured reuse releases old backing without promotion");
    Output(&f, 2 * kTimestampStep, 80);
    Require(f.driver.surfaces.at(1)->planes[0][0] == 80,
            "reused target receives only its new picture");
  }
}

void DirectVppCaptureAndPendingReuse() {
  for (bool complete_before_capture : {false, true}) {
    Fixture f;
    Live(&f);
    auto parameters = Vpp(&f);
    f.Begin(); f.Parameters(); f.End();
    if (complete_before_capture) Output(&f, kTimestampStep, 40);
    Require(BeginPicture(&f.context, 2, 2) == VA_STATUS_SUCCESS &&
                RenderPicture(&f.context, 2, &parameters, 1) ==
                    VA_STATUS_SUCCESS, "capture pending or completed direct picture");
    const auto captured = f.driver.contexts.at(2)->vpp_frame;
    Require(!captured->storage.empty() && !captured->direct_backing,
            "capture owns private pixels before releasing driver lock");
    f.Begin(); f.Parameters(false); f.End();
    if (!complete_before_capture) Output(&f, kTimestampStep, 40);
    Output(&f, 2 * kTimestampStep, 80);
    Require(EndPicture(&f.context, 2) == VA_STATUS_SUCCESS &&
                f.driver.surfaces.at(2)->planes[0][0] == 40 &&
                f.driver.surfaces.at(2)->frame_timestamp == kTimestampStep &&
                f.driver.surfaces.at(1)->planes[0][0] == 80,
            "VPP Render/End captures exact old picture across decode reuse");
  }
  Fixture f;
  Live(&f);
  f.Begin(); f.Parameters(); f.End();
  auto old = f.decode->decoded_frames.at(kTimestampStep);
  f.Begin(); f.Parameters(false); f.End();
  Require(!old->direct_backing && !old->storage.empty(),
          "reuse detaches pending output before changing public identity");
  Output(&f, kTimestampStep, 40);
  Require(!f.driver.surfaces.at(1)->ready && old->planes[0][0] == 40,
          "late old output only fills detached picture");
  Output(&f, 2 * kTimestampStep, 80);
}

void DirectContextOwnershipAndSharedFallback() {
  for (bool old_live : {false, true}) {
    for (bool fail_old : {false, true}) {
      for (bool expire_new_owner : {false, true}) {
        Fixture f;
        if (old_live)
          Live(&f);
        auto parameters = Vpp(&f);
        f.Begin(); f.Parameters(); f.End();
        auto original = f.decode;
        f.decode = std::make_shared<DecodeContext>();
        f.decode->config = 1;
        f.decode->width = f.decode->height = 16;
        f.decode->device = &f.mock;
        f.decode->decoder_open = f.decode->decoder_started = true;
        Live(&f);
        f.driver.contexts[3] = original;
        f.driver.contexts[1] = f.decode;
        f.Begin(); f.Parameters(); f.End();
        Output(&f, kTimestampStep, 80);
        Require(BeginPicture(&f.context, 2, 2) == VA_STATUS_SUCCESS &&
                    RenderPicture(&f.context, 2, &parameters, 1) ==
                        VA_STATUS_SUCCESS &&
                    EndPicture(&f.context, 2) == VA_STATUS_SUCCESS &&
                    f.driver.surfaces.at(2)->planes[0][0] == 80,
                "VPP chooses exact decoder ownership despite equal timestamps");
        if (expire_new_owner) {
          f.decode->decoded_frames.clear();
          Require(BeginPicture(&f.context, 2, 2) == VA_STATUS_SUCCESS &&
                      RenderPicture(&f.context, 2, &parameters, 1) ==
                          VA_STATUS_ERROR_DECODING_ERROR &&
                      EndPicture(&f.context, 2) ==
                          VA_STATUS_ERROR_INVALID_PARAMETER,
                  "VPP cannot resurrect an older equal-timestamp picture after "
                  "the current weak owner expires");
        }
        std::swap(f.decode, original);
        if (fail_old)
          Require(FailDecode(&f.driver, f.decode.get(), "old decoder failure") ==
                      VA_STATUS_ERROR_DECODING_ERROR,
                  "fail superseded decoder");
        else
          Output(&f, kTimestampStep, 40);
        Require(f.driver.surfaces.at(1)->planes[0][0] == 80 &&
                    !f.driver.surfaces.at(1)->failed,
                "old output or failure cannot claim a newer context even after "
                "its weak owner expires");
        std::swap(f.decode, original);
        f.driver.surfaces.at(1)->direct_decode_eligible = false;
        f.Begin(); f.Parameters(false); f.End();
        Require(!f.decode->decoded_frames.at(2 * kTimestampStep)->storage.empty(),
                "imported or write-exposed targets retain private output");
      }
    }
  }
}

void DirectBackingReassignedBetweenVppBeginAndEnd() {
  Fixture f;
  Live(&f);
  Vpp(&f);
  auto source = f.driver.surfaces.at(2);
  source->ready = true;
  source->expected_timestamp = source->frame_timestamp = kTimestampStep;
  source->decode_identity = NextDecodeIdentity(&f.driver);
  source->decode_picture = source;
  memset(source->storage.data(), 99, source->storage.size());
  auto source_decoder = std::make_shared<DecodeContext>();
  source_decoder->decoded_frames[kTimestampStep] = source;
  source_decoder->surface_timestamps[source.get()] = kTimestampStep;
  f.driver.contexts[3] = source_decoder;
  VAProcPipelineParameterBuffer pipeline = {};
  pipeline.surface = 2;
  auto parameters = f.Buffer(VAProcPipelineParameterBufferType, &pipeline,
                              sizeof(pipeline), 2);
  Require(BeginPicture(&f.context, 2, 1) == VA_STATUS_SUCCESS, "begin VPP into decode backing");
  f.Begin(); f.Parameters(); f.End();
  auto picture = f.decode->decoded_frames.at(kTimestampStep);
  Require(RenderPicture(&f.context, 2, &parameters, 1) == VA_STATUS_SUCCESS &&
              EndPicture(&f.context, 2) == VA_STATUS_SUCCESS,
          "complete VPP after intervening decode acquired the target");
  Require(!picture->direct_backing && !picture->ready &&
              f.driver.surfaces.at(1)->planes[0][0] == 99,
          "VPP End preserves and revokes a pending decode owner");
  Output(&f, kTimestampStep, 40);
  Require(picture->planes[0][0] == 40 &&
              f.driver.surfaces.at(1)->planes[0][0] == 99,
          "late equal-timestamp decode output cannot overwrite VPP result");
}
}

int main() {
  unsigned failures = 0;
  const std::pair<const char *, void (*)()> groups[] = {
    {"obsolete surface retirement preserves independent decode", ObsoleteSurfaceRetirementPreservesIndependentDecode},
    {"rejected H.264 target readback", RejectedH264TargetReadback},
    {"rejected H.264 recovery retains delayed reordered pictures", RejectedH264RecoveryRetainsDelayedReorderedPictures},
    {"rejected H.264 preserves accepted pictures", RejectedH264PreservesAcceptedPictures},
    {"rejected H.264 ownership guards", RejectedH264OwnershipGuards},
    {"rejected H.264 promotion failure", RejectedH264PromotionFailure},
    {"rejected H.264 consumed End", RejectedH264ConsumedEnd},
    {"large H.264 AU boundary and recovery", LargeH264AccessUnitBoundaryAndRecovery},
    {"qualified Flea progressive H.264 headroom", QualifiedFleaProgressiveH264Headroom},
    {"qualified H.264 deescaped admission and recovery", QualifiedH264DeescapedAdmissionAndRecovery},
    {"legacy Flea PES bounded whole-AU admission", LegacyFleaPesAdmission},
    {"rejected H.264 references require IDR", H264ReferenceRejectionRequiresIdr},
    {"allocation failure is transactional", AllocationFailureIsTransactional},
    {"private backing preserves captured pictures", PrivateBackingReusePreservesCapturedPictures},
    {"superseded pending CPU backing retires after commit", SupersededPendingCpuBackingRetiresAfterCommit},
    {"private backing allocation failure is transactional", PrivateBackingAllocationFailureIsTransactional},
    {"visible crop surface geometry", VisibleCropSurfaceGeometry},
    {"buffer ordering and owned snapshot", BufferOrderAndSnapshot},
    {"missing matrix resets flat", MissingResetsFlat},
    {"malformed size and recovery", MalformedSizeAndRecovery},
    {"active zero and inactive8x8", ActiveZeroAndInactive8x8},
    {"legacy profile rejection", LegacyProfilesRejectCustom},
    {"valid replacement and mixed flat", ValidReplacementAndMixedFlat},
    {"original and replay matrix ownership", OriginalReplayRetainsMatrices},
    {"direct live output and reuse", DirectLiveOutputAndReuse},
    {"direct VPP capture and pending reuse", DirectVppCaptureAndPendingReuse},
    {"direct context ownership and shared fallback", DirectContextOwnershipAndSharedFallback},
    {"direct backing reassigned during VPP", DirectBackingReassignedBetweenVppBeginAndEnd},
  };
  for (const auto &group : groups) {
    try { group.second(); std::printf("PASS %s\n", group.first); }
    catch (const std::exception &error) { ++failures; std::fprintf(stderr, "FAIL %s: %s\n", group.first, error.what()); }
  }
  std::printf("VA scaling lifecycle: %zu groups, %u checks, %u failures\n",
              sizeof(groups) / sizeof(groups[0]), checks, failures);
  return failures ? 1 : 0;
}
