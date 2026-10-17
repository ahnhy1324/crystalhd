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
bool Consume() {
  const bool fail = fail_next;
  fail_next = false;
  return fail;
}
void *Allocate(size_t bytes) {
  if (Consume()) throw std::bad_alloc();
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
  Require(active->allow_reopen && version != nullptr,
          "version query belongs to an armed reopen");
  *version = {};
  version->device = 1;
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
             ? 1024 * 1024 : 0;
}
extern "C" BC_STATUS DtsProcInput(HANDLE device, uint8_t *bytes, uint32_t size,
                                    uint64_t timestamp, BOOL encrypted) {
  CheckDevice(device);
  Require(bytes != nullptr && size != 0 && !encrypted &&
              !active->fragment_active,
          "ordinary complete input cannot interleave a fragmented AU");
  ++active->legacy_input_calls;
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
extern "C" int drmGetDevice2(int, uint32_t, drmDevicePtr *) { abort(); }
extern "C" void drmFreeDevice(drmDevicePtr *) { abort(); }
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
  VABufferID Buffer(VABufferType type, const void *data, size_t size) {
    VABufferID id = VA_INVALID_ID;
    Require(CreateBuffer(&context, 1, type, size, 1, const_cast<void *>(data), &id) == VA_STATUS_SUCCESS,
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
                before_public->decode_identity == before_identity &&
                !before_public->failed,
            "rejection preserves earlier pictures and public/replay identities");
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

  std::vector<uint8_t> exact(
      crystalhd_access_unit_limits::kH264Bytes - overhead, 0x55);
  exact[0] = 0x65;
  {
    Fixture boundary;
    Require(SubmitH264(&boundary, exact) == VA_STATUS_SUCCESS &&
                boundary.mock.inputs.size() == 16 &&
                LogicalInputs(boundary.mock.inputs).size() == 1 &&
                LogicalInputs(boundary.mock.inputs)[0].bytes.size() ==
                    crystalhd_access_unit_limits::kH264Bytes &&
                boundary.decode->replay.cached_bytes() ==
                    crystalhd_access_unit_limits::kH264Bytes,
            "exact assembled 8 MiB H.264 boundary is accepted and accounted");
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

VABufferID Vpp(Fixture *fixture) {
  auto processor = std::make_shared<DecodeContext>();
  processor->video_process = true;
  fixture->driver.contexts[2] = processor;
  auto target = std::make_shared<Surface>();
  Require(target->AllocateInternal(nullptr, -1, 16, 16, VA_FOURCC_NV12),
          "allocate CPU VPP destination");
  fixture->driver.surfaces[2] = target;
  VAProcPipelineParameterBuffer pipeline = {};
  pipeline.surface = 1;
  return fixture->Buffer(VAProcPipelineParameterBufferType, &pipeline,
                          sizeof(pipeline));
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
  auto parameters = f.Buffer(VAProcPipelineParameterBufferType, &pipeline, sizeof(pipeline));
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
    {"large H.264 AU boundary and recovery", LargeH264AccessUnitBoundaryAndRecovery},
    {"rejected H.264 references require IDR", H264ReferenceRejectionRequiresIdr},
    {"allocation failure is transactional", AllocationFailureIsTransactional},
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
