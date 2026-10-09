// SPDX-License-Identifier: LGPL-2.1-or-later
// Real VA submission and replay ownership, private CPU surfaces and deterministic
// Dts calls only. Unexpected device/graphics/ioctl entry points abort.
#include "../filters/vaapi/crystalhd_drv_video.cpp"
#include <stdexcept>
#include <string>

namespace {
unsigned checks = 0;
void Require(bool value, const char *message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
struct InputRecord { uint64_t timestamp; std::vector<uint8_t> bytes; };
struct DecoderMock {
  bool capacity = true;
  std::vector<InputRecord> inputs;
};
DecoderMock *active = nullptr;
void CheckDevice(HANDLE device) {
  Require(active != nullptr && device == active, "only the private decoder handle is used");
}
struct MockScope {
  explicit MockScope(DecoderMock *mock) { Require(active == nullptr, "no nested decoder fixture"); active = mock; }
  ~MockScope() { active = nullptr; }
};
}

extern "C" BC_STATUS DtsDeviceOpen(HANDLE *, uint32_t) { abort(); }
extern "C" BC_STATUS DtsCrystalHDVersion(HANDLE, PBC_INFO_CRYSTAL) { abort(); }
extern "C" BC_STATUS DtsSetInputFormat(HANDLE, BC_INPUT_FORMAT *) { abort(); }
extern "C" BC_STATUS DtsOpenDecoder(HANDLE, uint32_t) { abort(); }
extern "C" BC_STATUS DtsSetColorSpace(HANDLE, BC_OUTPUT_FORMAT) { abort(); }
extern "C" BC_STATUS DtsStartDecoder(HANDLE) { abort(); }
extern "C" BC_STATUS DtsStartCapture(HANDLE) { abort(); }
extern "C" BC_STATUS DtsStopDecoder(HANDLE device) { CheckDevice(device); return BC_STS_SUCCESS; }
extern "C" BC_STATUS DtsCloseDecoder(HANDLE device) { CheckDevice(device); return BC_STS_SUCCESS; }
extern "C" BC_STATUS DtsDeviceClose(HANDLE device) { CheckDevice(device); return BC_STS_SUCCESS; }
extern "C" BC_STATUS DtsGetDriverStatus(HANDLE device, BC_DTS_STATUS *status) {
  CheckDevice(device); *status = {}; return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsIsEndOfStream(HANDLE, uint8_t *) { abort(); }
extern "C" uint32_t DtsTxFreeSize(HANDLE device) {
  CheckDevice(device); return active->capacity ? 1024 * 1024 : 0;
}
extern "C" BC_STATUS DtsProcInput(HANDLE device, uint8_t *bytes, uint32_t size,
                                    uint64_t timestamp, BOOL encrypted) {
  CheckDevice(device);
  Require(bytes != nullptr && size != 0 && timestamp != 0 && !encrypted,
          "original complete timestamped access unit reaches the library");
  active->inputs.push_back({timestamp, std::vector<uint8_t>(bytes, bytes + size)});
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
  for (bool live : {false, true}) {
    Fixture f;
    if (live) Live(&f);
    f.Begin(); f.Parameters(); f.End();
    std::weak_ptr<Surface> previous = f.decode->decoded_frames.at(kTimestampStep);
    Require(f.decode->decoded_frames.at(kTimestampStep)->storage.empty() == live,
            "only explicit live H264 elides private frame allocation");
    Output(&f, kTimestampStep, 40);
    Require(f.driver.surfaces.at(1)->ready &&
                f.driver.surfaces.at(1)->planes[0][0] == 40 &&
                f.decode->decoded_frames.at(kTimestampStep)->storage.empty() == live,
            "direct completion publishes actual pixels without a private copy");
    Output(&f, kTimestampStep, 90);
    Require(f.driver.surfaces.at(1)->planes[0][0] == 40,
            "duplicate cannot replace direct pixels");
    if (live)
      f.driver.surfaces.at(1)->layout.valid = true;
    f.Begin(); f.Parameters(false); f.End();
    Require(f.decode->decoded_frames.at(2 * kTimestampStep)->storage.empty() == live,
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
