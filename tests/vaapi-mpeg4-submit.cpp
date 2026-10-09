// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual public VA MPEG-4 Part 2 submission and bounded I/P/B replay. Decoder
// calls are deterministic mocks; unexpected graphics or ioctl access aborts.
#include "../filters/vaapi/crystalhd_drv_video.cpp"
#include <atomic>
#include <stdexcept>
#include <string>

namespace {
std::atomic<unsigned> checks{0};
void Require(bool value, const char *message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

struct InputRecord {
  uint64_t timestamp;
  std::vector<uint8_t> bytes;
};
struct MockOutput {
  uint64_t timestamp;
  unsigned width, height;
  uint32_t picture_number;
  uint32_t picture_flags;
  uint32_t pout_flags;
  bool eos;
  std::vector<uint8_t> pixels;
};
struct DecoderMock {
  bool capacity = true, opened = false, leased = false, eos = false;
  bool echo_inputs = false, eos_on_flush = false;
  std::atomic<unsigned> blocked_tx_checks{0};
  unsigned device = 1;
  BC_STATUS eos_status = BC_STS_SUCCESS;
  unsigned opens = 0, closes = 0, starts = 0, stops = 0;
  unsigned flushes = 0, releases = 0, eos_polls = 0;
  BC_INPUT_FORMAT format = {};
  std::vector<uint8_t> metadata;
  std::vector<InputRecord> inputs;
  std::deque<MockOutput> outputs;

  void Queue(uint64_t timestamp, unsigned width, unsigned height,
             uint32_t picture_number = 0, uint32_t picture_flags = 0,
             uint32_t pout_flags = BC_POUT_FLAGS_PIB_VALID) {
    outputs.push_back({timestamp, width, height, picture_number,
                       picture_flags, pout_flags, false,
                       std::vector<uint8_t>(
                           static_cast<size_t>(width) * height * 2, 128)});
  }
  void EosMarker() { outputs.push_back({0, 0, 0, 0, 0, 0, true, {}}); }
};

DecoderMock *active = nullptr;
void CheckDevice(HANDLE device) {
  Require(active && device == active && active->opened,
          "private opened decoder handle only");
}
struct MockScope {
  explicit MockScope(DecoderMock *mock) {
    Require(!active, "one physical decoder fixture at a time");
    active = mock;
  }
  ~MockScope() { active = nullptr; }
};
}  // namespace

extern "C" BC_STATUS DtsDeviceOpen(HANDLE *device, uint32_t) {
  Require(active && !active->opened && device,
          "mock open has no live predecessor");
  active->opened = true;
  ++active->opens;
  *device = active;
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsCrystalHDVersion(HANDLE device,
                                           PBC_INFO_CRYSTAL version) {
  CheckDevice(device);
  *version = {};
  version->device = active->device;
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsSetInputFormat(HANDLE device,
                                         BC_INPUT_FORMAT *format) {
  CheckDevice(device);
  Require(format != nullptr, "input format pointer");
  active->format = *format;
  active->metadata.clear();
  if (format->metaDataSz) {
    Require(format->pMetaData != nullptr, "metadata pointer covers its size");
    active->metadata.assign(format->pMetaData,
                            format->pMetaData + format->metaDataSz);
  }
  active->format.pMetaData = nullptr;
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsOpenDecoder(HANDLE device, uint32_t stream) {
  CheckDevice(device);
  Require(stream == BC_STREAM_TYPE_ES, "elementary stream decoder request");
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsSetColorSpace(HANDLE device,
                                        BC_OUTPUT_FORMAT mode) {
  CheckDevice(device);
  Require(mode == OUTPUT_MODE422_YUY2, "YUY2 capture remains unchanged");
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsStartDecoder(HANDLE device) {
  CheckDevice(device);
  ++active->starts;
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsStartCapture(HANDLE device) {
  CheckDevice(device);
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsStopDecoder(HANDLE device) {
  CheckDevice(device);
  Require(!active->leased, "stop has no external output lease");
  ++active->stops;
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsCloseDecoder(HANDLE device) {
  CheckDevice(device);
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsDeviceClose(HANDLE device) {
  CheckDevice(device);
  ++active->closes;
  active->opened = false;
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsGetDriverStatus(HANDLE device,
                                          BC_DTS_STATUS *status) {
  CheckDevice(device);
  *status = {};
  status->ReadyListCount = active->outputs.size();
  return BC_STS_SUCCESS;
}
extern "C" uint32_t DtsTxFreeSize(HANDLE device) {
  CheckDevice(device);
  if (!active->capacity)
    ++active->blocked_tx_checks;
  return active->capacity ? 1024 * 1024 : 0;
}
extern "C" BC_STATUS DtsProcInput(HANDLE device, uint8_t *bytes,
                                    uint32_t size, uint64_t timestamp,
                                    BOOL encrypted) {
  CheckDevice(device);
  Require(bytes && size && timestamp && !encrypted,
          "complete owned timestamped input");
  active->inputs.push_back(
      {timestamp, std::vector<uint8_t>(bytes, bytes + size)});
  if (active->echo_inputs)
    active->Queue(timestamp, active->format.width, active->format.height);
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsProcOutputNoCopy(HANDLE device, uint32_t,
                                           BC_DTS_PROC_OUT *output) {
  CheckDevice(device);
  Require(!active->leased, "one output lease at a time");
  if (active->outputs.empty()) return BC_STS_NO_DATA;
  auto &next = active->outputs.front();
  *output = {};
  output->PoutFlags = next.pout_flags;
  output->PicInfo.timeStamp = next.timestamp;
  output->PicInfo.width = next.width;
  output->PicInfo.height = next.height;
  output->PicInfo.picture_number = next.picture_number;
  output->PicInfo.flags = next.picture_flags |
      (next.eos ? VDEC_FLAG_EOS : 0);
  output->Ybuff = next.pixels.data();
  output->YBuffDoneSz = next.pixels.size() / 4;
  active->leased = true;
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsReleaseOutputBuffs(HANDLE device, PVOID,
                                             BOOL change) {
  CheckDevice(device);
  Require(active->leased && !change,
          "release exactly the returned picture lease");
  active->leased = false;
  ++active->releases;
  active->outputs.pop_front();
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsFlushInput(HANDLE device, uint32_t mode) {
  CheckDevice(device);
  Require(mode == 0, "finite EOS, never reset-as-drain");
  ++active->flushes;
  if (active->eos_on_flush)
    active->EosMarker();
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsIsEndOfStream(HANDLE device, uint8_t *eos) {
  CheckDevice(device);
  Require(eos != nullptr, "EOS result pointer");
  ++active->eos_polls;
  if (active->eos_status == BC_STS_SUCCESS)
    *eos = active->eos;
  return active->eos_status;
}

extern "C" int ioctl(int, unsigned long, ...) noexcept { abort(); }
extern "C" gbm_device *gbm_create_device(int) { abort(); }
extern "C" void gbm_device_destroy(gbm_device *) { abort(); }
extern "C" gbm_bo *gbm_bo_create(gbm_device *, uint32_t, uint32_t, uint32_t,
                                   uint32_t) { abort(); }
extern "C" void *gbm_bo_map(gbm_bo *, uint32_t, uint32_t, uint32_t,
                              uint32_t, uint32_t, uint32_t *, void **) {
  abort();
}
extern "C" void gbm_bo_unmap(gbm_bo *, void *) { abort(); }
extern "C" void gbm_bo_destroy(gbm_bo *) { abort(); }
extern "C" int gbm_bo_get_plane_count(gbm_bo *) { abort(); }
extern "C" uint32_t gbm_bo_get_stride_for_plane(gbm_bo *, int) { abort(); }
extern "C" uint32_t gbm_bo_get_offset(gbm_bo *, int) { abort(); }
extern "C" int drmGetDevice2(int, uint32_t, drmDevicePtr *) { abort(); }
extern "C" void drmFreeDevice(drmDevicePtr *) { abort(); }
extern "C" int drmPrimeHandleToFD(int, uint32_t, uint32_t, int *) {
  abort();
}
extern "C" void sws_freeContext(SwsContext *) { abort(); }
extern "C" SwsContext *sws_getCachedContext(
    SwsContext *, int, int, AVPixelFormat, int, int, AVPixelFormat, int,
    SwsFilter *, SwsFilter *, const double *) { abort(); }
extern "C" int sws_scale(SwsContext *, const uint8_t *const [], const int [],
                           int, int, uint8_t *const [], const int []) {
  abort();
}

namespace {
VAPictureParameterBufferMPEG4 Picture(
    unsigned width = 16, unsigned height = 16, unsigned kind = 0,
    VASurfaceID forward = VA_INVALID_SURFACE,
    VASurfaceID backward = VA_INVALID_SURFACE, int trb = 0, int trd = 0,
    unsigned backward_kind = 0) {
  VAPictureParameterBufferMPEG4 picture = {};
  picture.vop_width = width;
  picture.vop_height = height;
  picture.forward_reference_picture = forward;
  picture.backward_reference_picture = backward;
  picture.vol_fields.bits.chroma_format = 1;
  picture.vol_fields.bits.obmc_disable = 1;
  picture.vol_fields.bits.resync_marker_disable = 1;
  picture.quant_precision = 5;
  picture.vop_fields.bits.vop_coding_type = kind;
  picture.vop_fields.bits.backward_reference_vop_coding_type = backward_kind;
  picture.vop_fields.bits.vop_rounding_type = kind == 1;
  picture.vop_fields.bits.intra_dc_vlc_thr = 3;
  picture.vop_fcode_forward = 1;
  picture.vop_fcode_backward = 1;
  picture.vop_time_increment_resolution = 30;
  picture.TRB = trb;
  picture.TRD = trd;
  return picture;
}

VASliceParameterBufferMPEG4 Slice(unsigned offset = 0,
                                  unsigned size = 6,
                                  unsigned bit_offset = 3) {
  VASliceParameterBufferMPEG4 slice = {};
  slice.slice_data_size = size;
  slice.slice_data_offset = offset;
  slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
  slice.macroblock_offset = bit_offset;
  slice.macroblock_number = 0;
  slice.quant_scale = 7;
  return slice;
}

std::vector<uint8_t> SliceBytes(uint8_t tag = 0x95) {
  return {static_cast<uint8_t>(0xe0 | (tag & 0x1f)), tag, 0x77,
          0x9a, 0xd5, 0x6b};
}

VAIQMatrixBufferMPEG4 Matrix() {
  VAIQMatrixBufferMPEG4 matrix = {};
  matrix.load_intra_quant_mat = 1;
  matrix.load_non_intra_quant_mat = 1;
  for (unsigned i = 0; i < 64; ++i) {
    matrix.intra_quant_mat[i] = 1 + (i * 13) % 255;
    matrix.non_intra_quant_mat[i] = 1 + (i * 29) % 255;
  }
  return matrix;
}

bool HasStartCode(const std::vector<uint8_t> &bytes, uint8_t code) {
  for (size_t i = 0; i + 3 < bytes.size(); ++i)
    if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1 &&
        bytes[i + 3] == code)
      return true;
  return false;
}
bool HasVol(const std::vector<uint8_t> &bytes) {
  for (unsigned code = 0x20; code <= 0x2f; ++code)
    if (HasStartCode(bytes, code)) return true;
  return false;
}
void CheckI(const std::vector<uint8_t> &bytes) {
  Require(HasVol(bytes) && HasStartCode(bytes, 0xb6),
          "every I AU owns complete VOL and VOP syntax");
  Require(HasStartCode(bytes, 0xb3),
          "every I AU gives firmware an explicit GOV clock origin");
}

struct Fixture {
  DecoderMock mock;
  MockScope scope{&mock};
  Driver driver{-1};
  VADriverContext context = {};
  VAConfigID config = VA_INVALID_ID;
  VAContextID id = VA_INVALID_ID;
  std::shared_ptr<DecodeContext> decode;
  unsigned width, height;

  explicit Fixture(VAProfile profile = VAProfileMPEG4AdvancedSimple,
                   unsigned w = 16, unsigned h = 16)
      : width(w), height(h) {
    context.pDriverData = &driver;
    Require(CreateConfig(&context, profile, VAEntrypointVLD, nullptr, 0,
                         &config) == VA_STATUS_SUCCESS,
            "public MPEG-4 profile configuration");
    Require(CreateContext(&context, config, width, height, VA_PROGRESSIVE,
                          nullptr, 0, &id) == VA_STATUS_SUCCESS,
            "public MPEG-4 context");
    decode = driver.contexts.at(id);
    Require(decode->profile == profile && decode->IsMpeg4(),
            "created context retains selected MPEG-4 profile");
    for (VASurfaceID surface = 1; surface <= 8; ++surface)
      Surface(surface);
  }

  void Surface(VASurfaceID id, unsigned w = 0, unsigned h = 0) {
    auto surface = std::make_shared<::Surface>();
    Require(surface->AllocateInternal(nullptr, -1, w ? w : width,
                                      h ? h : height, VA_FOURCC_NV12),
            "private CPU surface allocation");
    driver.surfaces[id] = std::move(surface);
  }

  VABufferID Buffer(VABufferType type, const void *data, unsigned bytes,
                    unsigned elements = 1,
                    VAContextID owner = VA_INVALID_ID) {
    VABufferID buffer = VA_INVALID_ID;
    Require(CreateBuffer(&context, owner == VA_INVALID_ID ? id : owner, type,
                         bytes, elements, const_cast<void *>(data), &buffer) ==
                VA_STATUS_SUCCESS,
            "real public buffer allocation");
    return buffer;
  }

  void Begin(VASurfaceID target) {
    Require(BeginPicture(&context, id, target) == VA_STATUS_SUCCESS,
            "public BeginPicture");
  }

  VAStatus SubmitStatus(
      VASurfaceID target, const VAPictureParameterBufferMPEG4 &picture,
      const VASliceParameterBufferMPEG4 &slice = Slice(),
      const std::vector<uint8_t> &bytes = SliceBytes(),
      const VAIQMatrixBufferMPEG4 *matrix = nullptr) {
    Begin(target);
    std::vector<VABufferID> buffers;
    buffers.push_back(Buffer(VAPictureParameterBufferType, &picture,
                             sizeof(picture)));
    if (matrix)
      buffers.push_back(Buffer(VAIQMatrixBufferType, matrix, sizeof(*matrix)));
    buffers.push_back(Buffer(VASliceParameterBufferType, &slice,
                             sizeof(slice)));
    buffers.push_back(Buffer(VASliceDataBufferType, bytes.data(), bytes.size()));
    VAStatus first = VA_STATUS_SUCCESS;
    for (VABufferID buffer : buffers) {
      const VAStatus status = RenderPicture(&context, id, &buffer, 1);
      if (first == VA_STATUS_SUCCESS && status != VA_STATUS_SUCCESS)
        first = status;
    }
    const VAStatus end = EndPicture(&context, id);
    return first != VA_STATUS_SUCCESS ? first : end;
  }

  void Submit(VASurfaceID target,
              const VAPictureParameterBufferMPEG4 &picture) {
    Require(SubmitStatus(target, picture) == VA_STATUS_SUCCESS,
            "public MPEG-4 EndPicture/SubmitPicture");
  }

  void I(VASurfaceID target, int trd = 0) {
    Submit(target, Picture(width, height, 0, VA_INVALID_SURFACE,
                           VA_INVALID_SURFACE, 0, trd));
  }
  void P(VASurfaceID target, VASurfaceID forward, int trd = 3) {
    Submit(target, Picture(width, height, 1, forward,
                           VA_INVALID_SURFACE, 0, trd));
  }
  void B(VASurfaceID target, VASurfaceID forward, VASurfaceID backward,
         int trb, int trd, unsigned backward_kind = 1) {
    Submit(target, Picture(width, height, 2, forward, backward, trb, trd,
                           backward_kind));
  }
  void Receive() {
    Require(ReceiveAvailable(&driver, decode.get()) == VA_STATUS_SUCCESS,
            "actual receive/output dispatch");
  }
};

void ProfilesInputFormatAndDeviceGate() {
  for (VAProfile profile : {VAProfileMPEG4Simple,
                            VAProfileMPEG4AdvancedSimple}) {
    Fixture fixture(profile);
    VAProfile profiles[32] = {};
    int count = 0;
    Require(QueryConfigProfiles(&fixture.context, profiles, &count) ==
                    VA_STATUS_SUCCESS &&
                count <= 32 &&
                std::find(profiles, profiles + count, profile) !=
                    profiles + count,
            "MPEG-4 profile is advertised");
    VAEntrypoint entrypoints[4] = {};
    count = 0;
    Require(QueryConfigEntrypoints(&fixture.context, profile, entrypoints,
                                   &count) == VA_STATUS_SUCCESS &&
                count == 1 && entrypoints[0] == VAEntrypointVLD,
            "MPEG-4 exposes only the VLD entrypoint");
    VAConfigAttrib attributes[] = {{VAConfigAttribRTFormat, 0},
                                   {VAConfigAttribDecSliceMode, 0}};
    Require(GetConfigAttributes(&fixture.context, profile, VAEntrypointVLD,
                                attributes, 2) == VA_STATUS_SUCCESS &&
                attributes[0].value == VA_RT_FORMAT_YUV420 &&
                attributes[1].value == VA_DEC_SLICE_MODE_NORMAL,
            "MPEG-4 advertises normal-slice 4:2:0 decode");
    fixture.I(1);
    Require(fixture.mock.opens == 1 && fixture.mock.inputs.size() == 1,
            "one accepted MPEG-4 I picture");
    Require(fixture.mock.format.mSubtype == BC_MSUBTYPE_DIVX &&
                fixture.mock.format.mSubtype != BC_MSUBTYPE_DIVX311 &&
                fixture.mock.format.startCodeSz == 4 &&
                fixture.mock.format.Progressive &&
                fixture.mock.format.width == fixture.width &&
                fixture.mock.format.height == fixture.height,
            "BCM70015 receives progressive start-coded DIVX input");
    Require(fixture.decode->mpeg4_replay.cached_pictures() == 1 &&
                fixture.decode->replay.cached_pictures() == 0 &&
                fixture.decode->mpeg2_replay.cached_pictures() == 0,
            "MPEG-4 uses only its codec-aware I/P/B replay state");
    CheckI(fixture.mock.inputs.front().bytes);
  }

  {
    Fixture fixture;
    VAProfile profiles[32] = {};
    int count = 0;
    Require(QueryConfigProfiles(&fixture.context, profiles, &count) ==
                VA_STATUS_SUCCESS,
            "profile query for unsupported checks");
    Require(std::find(profiles, profiles + count, VAProfileMPEG4Main) ==
                profiles + count,
            "MPEG-4 Main is not advertised");
    VAConfigID config = VA_INVALID_ID;
    Require(CreateConfig(&fixture.context, VAProfileMPEG4Main,
                         VAEntrypointVLD, nullptr, 0, &config) ==
                VA_STATUS_ERROR_UNSUPPORTED_PROFILE,
            "MPEG-4 Main cannot be configured through a hidden path");
  }

  {
    Fixture fixture;
    fixture.mock.device = 0;
    Require(fixture.SubmitStatus(1, Picture()) != VA_STATUS_SUCCESS,
            "BCM70012 rejects MPEG-4 before compressed submission");
    Require(fixture.mock.inputs.empty() && fixture.mock.opens == 1 &&
                fixture.mock.closes == 1 && fixture.mock.starts == 0,
            "BCM70012 gate closes the probe without starting firmware");
  }
}

void OwnedBuffersAndPublicOrdering() {
  std::array<unsigned, 3> order = {0, 1, 2};
  do {
    Fixture fixture;
    auto picture = Picture();
    auto slice = Slice();
    auto bytes = SliceBytes(0xa5);
    VABufferID ids[] = {
        fixture.Buffer(VAPictureParameterBufferType, &picture,
                       sizeof(picture)),
        fixture.Buffer(VASliceParameterBufferType, &slice, sizeof(slice)),
        fixture.Buffer(VASliceDataBufferType, bytes.data(), bytes.size())};
    fixture.Begin(1);
    for (unsigned index : order)
      Require(RenderPicture(&fixture.context, fixture.id, &ids[index], 1) ==
                  VA_STATUS_SUCCESS,
              "public MPEG-4 buffers accept arbitrary call ordering");
    for (VABufferID id : ids) {
      void *mapped = nullptr;
      Require(MapBuffer(&fixture.context, id, &mapped) == VA_STATUS_SUCCESS,
              "map original caller buffer");
      std::memset(mapped, 0xee, fixture.driver.buffers.at(id).data.size());
      Require(UnmapBuffer(&fixture.context, id) == VA_STATUS_SUCCESS &&
                  DestroyBuffer(&fixture.context, id) == VA_STATUS_SUCCESS,
              "caller storage may disappear after RenderPicture");
    }
    Require(EndPicture(&fixture.context, fixture.id) == VA_STATUS_SUCCESS &&
                fixture.mock.inputs.size() == 1,
            "EndPicture uses its immutable owned MPEG-4 snapshot");
    CheckI(fixture.mock.inputs.front().bytes);
  } while (std::next_permutation(order.begin(), order.end()));
}

void ReferencesTimingAndOpenGop() {
  {
    Fixture fixture;
    fixture.I(1);             // tick 0
    fixture.P(2, 1, 3);       // tick 3
    fixture.I(3, 3);          // tick 6, still a self-contained I AU
    fixture.B(4, 2, 3, 1, 3, 0);  // open-GOP B at tick 4

    Require(fixture.mock.inputs.size() == 4,
            "I/P/later-I/open-GOP-B all reach the transport");
    CheckI(fixture.mock.inputs[0].bytes);
    CheckI(fixture.mock.inputs[2].bytes);
    Require(!HasVol(fixture.mock.inputs[1].bytes) &&
                !HasVol(fixture.mock.inputs[3].bytes) &&
                HasStartCode(fixture.mock.inputs[1].bytes, 0xb6) &&
                HasStartCode(fixture.mock.inputs[3].bytes, 0xb6),
            "P/B carry only their complete generated VOP access units");

    const auto *i = fixture.decode->mpeg4_replay.Find(3 * kTimestampStep);
    const auto *b = fixture.decode->mpeg4_replay.Find(4 * kTimestampStep);
    Require(i && i->kind == CrystalHDMpeg4Replay::Kind::I &&
                i->root == 3 * kTimestampStep,
            "later I is a self-contained future replay root");
    Require(b && b->kind == CrystalHDMpeg4Replay::Kind::B &&
                b->forward == 2 * kTimestampStep &&
                b->backward == 3 * kTimestampStep &&
                b->root == kTimestampStep,
            "open-GOP B retains the older root while it crosses the I boundary");

    const size_t before = fixture.mock.inputs.size();
    const uint64_t next = fixture.decode->next_timestamp;
    auto bad_time = Picture(16, 16, 2, 2, 3, 3, 3, 0);
    Require(fixture.SubmitStatus(5, bad_time) != VA_STATUS_SUCCESS &&
                fixture.mock.inputs.size() == before &&
                fixture.decode->next_timestamp == next,
            "B timing outside 0<TRB<TRD commits no AU or token");
  }

  Fixture simple(VAProfileMPEG4Simple);
  simple.I(1);
  simple.P(2, 1);
  Require(simple.SubmitStatus(
              3, Picture(16, 16, 2, 1, 2, 1, 3, 1)) != VA_STATUS_SUCCESS &&
              simple.mock.inputs.size() == 2,
          "Simple profile rejects B pictures with otherwise valid references");
}

void SimpleZeroTrdAnchors() {
  Fixture fixture(VAProfileMPEG4Simple);
  fixture.I(1);
  fixture.P(2, 1, 0);
  fixture.I(3, 0);
  Require(fixture.mock.opens == 1 && fixture.mock.closes == 0 &&
              fixture.mock.inputs.size() == 3 &&
              fixture.decode->mpeg4_timing.have_previous &&
              fixture.decode->mpeg4_timing.previous.tick == 1 &&
              fixture.decode->mpeg4_timing.newest.tick == 2,
          "zero-TRD Simple anchors stay in one monotonic decoder epoch");
}

void SimpleOutputIdentity() {
  {
    Fixture fixture(VAProfileMPEG4Simple);
    fixture.I(1);
    fixture.P(2, 1);
    fixture.P(3, 2);
    fixture.mock.Queue(kTimestampStep, 16, 16, 40);
    fixture.mock.Queue(0, 16, 16, 41);
    fixture.mock.Queue(3 * kTimestampStep, 16, 16, 42);
    fixture.Receive();
    Require(fixture.decode->pending.empty() &&
                fixture.decode->mpeg4_replay.outstanding() == 0 &&
                fixture.driver.surfaces.at(1)->ready &&
                fixture.driver.surfaces.at(2)->ready &&
                fixture.driver.surfaces.at(3)->ready &&
                fixture.driver.surfaces.at(2)->frame_timestamp ==
                    2 * kTimestampStep &&
                fixture.decode->have_mpeg4_simple_picture_number &&
                fixture.decode->mpeg4_simple_picture_number == 42,
            "consecutive Simple ordinal recovers only the oldest token");
  }

  {
    Fixture fixture(VAProfileMPEG4Simple);
    fixture.I(1);
    fixture.P(2, 1);
    fixture.mock.Queue(2 * kTimestampStep, 16, 16, 2);
    Require(ReceiveAvailable(&fixture.driver, fixture.decode.get()) ==
                    VA_STATUS_ERROR_DECODING_ERROR &&
                fixture.decode->mpeg4_replay.failed() &&
                fixture.decode->mpeg4_replay.outstanding() == 2 &&
                fixture.decode->pending.size() == 2,
            "nonzero Simple token cannot skip the oldest submitted picture");
  }

  {
    Fixture fixture(VAProfileMPEG4Simple);
    fixture.I(1);
    fixture.mock.Queue(0, 16, 16, 1);
    fixture.Receive();
    Require(fixture.decode->pending.size() == 1 &&
                !fixture.decode->have_mpeg4_simple_picture_number,
            "zero token without an ordinal baseline is ignored");
    fixture.mock.Queue(kTimestampStep, 16, 16, 1);
    fixture.Receive();
    Require(fixture.driver.surfaces.at(1)->ready &&
                fixture.decode->pending.empty(),
            "exact token remains usable after an unprovable zero token");
  }

  {
    Fixture fixture(VAProfileMPEG4Simple);
    fixture.I(1);
    fixture.P(2, 1);
    fixture.mock.Queue(kTimestampStep, 16, 16, 10);
    fixture.Receive();
    fixture.mock.Queue(0, 16, 16, 10);
    fixture.mock.Queue(0, 16, 16, 12);
    fixture.mock.Queue(0, 16, 16, 11, 0, 0);
    fixture.mock.Queue(0, 16, 16, 11, VDEC_FLAG_INTERLACED_SRC);
    fixture.Receive();
    Require(fixture.decode->pending.size() == 1 &&
                fixture.decode->mpeg4_replay.outstanding() == 1 &&
                fixture.decode->mpeg4_simple_picture_number == 10,
            "repeated, skipped, invalid-PIB and interlaced zero tokens stay unmatched");
    fixture.mock.Queue(2 * kTimestampStep, 16, 16, 11);
    fixture.Receive();
    Require(fixture.driver.surfaces.at(2)->ready &&
                fixture.decode->pending.empty(),
            "ignored zero-token noise cannot retire the pending picture");
  }

  {
    Fixture fixture(VAProfileMPEG4Simple);
    fixture.I(1);
    fixture.P(2, 1);
    fixture.P(3, 2);
    fixture.mock.Queue(kTimestampStep, 16, 16, 7);
    fixture.mock.Queue(2 * kTimestampStep, 16, 16, 0);
    fixture.mock.Queue(0, 16, 16, 8);
    fixture.Receive();
    Require(!fixture.decode->have_mpeg4_simple_picture_number &&
                fixture.decode->pending.size() == 1,
            "an expected picture with ordinal zero breaks the recovery baseline");
    fixture.mock.Queue(3 * kTimestampStep, 16, 16, 8);
    fixture.Receive();
    Require(fixture.driver.surfaces.at(3)->ready,
            "exact token re-establishes identity after a broken baseline");
  }

  {
    Fixture fixture(VAProfileMPEG4Simple);
    fixture.I(1);
    fixture.P(2, 1);
    fixture.mock.Queue(kTimestampStep, 16, 16,
                       std::numeric_limits<uint32_t>::max());
    fixture.mock.Queue(0, 16, 16, 1);
    fixture.Receive();
    Require(fixture.decode->pending.size() == 1 &&
                fixture.decode->mpeg4_replay.outstanding() == 1,
            "Simple ordinal rollover is never inferred from a zero token");
    fixture.mock.Queue(2 * kTimestampStep, 16, 16, 1);
    fixture.Receive();
  }

  {
    Fixture fixture(VAProfileMPEG4AdvancedSimple);
    fixture.I(1);
    fixture.P(2, 1);
    fixture.mock.Queue(0, 16, 16, 1);
    fixture.mock.Queue(2 * kTimestampStep, 16, 16, 2);
    fixture.mock.Queue(kTimestampStep, 16, 16, 1);
    fixture.Receive();
    Require(fixture.decode->pending.empty() &&
                fixture.driver.surfaces.at(1)->ready &&
                fixture.driver.surfaces.at(2)->ready &&
                !fixture.decode->have_mpeg4_simple_picture_number,
            "Advanced Simple keeps its reordered output behavior and no zero recovery");
  }
}

void SimpleRecoveryAcrossReplay() {
  Fixture fixture(VAProfileMPEG4Simple);
  fixture.I(1);
  fixture.P(2, 1);
  Require(SealDecodeBatch(&fixture.driver, fixture.decode.get()) ==
                  VA_STATUS_SUCCESS,
          "Simple replay fixture seals its first batch");
  fixture.P(3, 2);
  fixture.mock.Queue(kTimestampStep, 16, 16, 100);
  fixture.mock.Queue(2 * kTimestampStep, 16, 16, 101);
  fixture.mock.EosMarker();
  fixture.Receive();
  Require(fixture.decode->mpeg4_replay.NeedsRestart() &&
              fixture.decode->have_mpeg4_simple_picture_number,
          "first Simple hardware session establishes an ordinal baseline");
  Require(PumpDecodeInput(&fixture.driver, fixture.decode.get()) ==
                  VA_STATUS_SUCCESS &&
              fixture.mock.opens == 2 && fixture.mock.closes == 1 &&
              !fixture.decode->have_mpeg4_simple_picture_number,
          "hardware reopen clears the old Simple ordinal baseline");
  fixture.mock.Queue(kTimestampStep, 16, 16, 200);
  fixture.mock.Queue(2 * kTimestampStep, 16, 16, 201);
  fixture.mock.Queue(0, 16, 16, 202);
  fixture.Receive();
  Require(fixture.driver.surfaces.at(3)->ready &&
              fixture.driver.surfaces.at(3)->frame_timestamp ==
                  3 * kTimestampStep &&
              fixture.decode->pending.empty() &&
              fixture.decode->mpeg4_replay.outstanding() == 0 &&
              fixture.decode->mpeg4_simple_picture_number == 202,
          "expected replay duplicates rebuild the baseline for a later zero token");
}

void BackwardTimingDiscontinuity() {
  for (VAProfile profile : {VAProfileMPEG4Simple,
                            VAProfileMPEG4AdvancedSimple}) {
    Fixture fixture(profile);
    fixture.I(1);
    Require(SealDecodeBatch(&fixture.driver, fixture.decode.get()) ==
                    VA_STATUS_SUCCESS,
            "pre-backward-seek batch seals");
    fixture.mock.Queue(kTimestampStep, 16, 16);
    fixture.mock.EosMarker();
    fixture.Receive();

    fixture.I(2, -7);
    Require(fixture.mock.opens == 2 && fixture.mock.closes == 1 &&
                fixture.mock.stops == 1 && fixture.mock.inputs.size() == 2 &&
                fixture.decode->mpeg4_replay.cached_pictures() == 1 &&
                fixture.decode->mpeg4_replay.Find(2 * kTimestampStep) !=
                    nullptr &&
                fixture.decode->mpeg4_timing.newest.tick == 0,
            "negative post-seek TRD starts a fresh decoder epoch");
  }
}

void UnsupportedToolsAndConfiguration() {
  for (unsigned variant = 0; variant < 12; ++variant) {
    Fixture fixture;
    auto picture = Picture();
    switch (variant) {
      case 0: picture.vol_fields.bits.short_video_header = 1; break;
      case 1: picture.vol_fields.bits.chroma_format = 2; break;
      case 2: picture.vol_fields.bits.interlaced = 1; break;
      case 3: picture.vol_fields.bits.obmc_disable = 0; break;
      case 4: picture.vol_fields.bits.sprite_enable = 1; break;
      case 5: picture.vol_fields.bits.quarter_sample = 1; break;
      case 6: picture.vol_fields.bits.quant_type = 1; break;
      case 7: picture.vol_fields.bits.data_partitioned = 1; break;
      case 8: picture.vol_fields.bits.reversible_vlc = 1; break;
      case 9: picture.quant_precision = 4; break;
      case 10: picture.vop_fields.bits.vop_coding_type = 3; break;
      case 11: picture.vop_fields.bits.alternate_vertical_scan_flag = 1; break;
    }
    Require(fixture.SubmitStatus(1, picture) != VA_STATUS_SUCCESS &&
                fixture.mock.inputs.empty() && fixture.mock.opens == 0 &&
                fixture.decode->next_timestamp == kTimestampStep,
            "unsupported MPEG-4 tool fails before hardware and token commit");
    fixture.I(1);
  }

  {
    const std::vector<uint8_t> i_bytes =
        {0x1f, 0x00, 0x00, 0xa8, 0x04, 0x7f, 0x55, 0xa5};
    const std::vector<uint8_t> p_bytes =
        {0x01, 0x00, 0x00, 0xa8, 0x04, 0x7f, 0x55, 0xa5};
    Fixture fixture(VAProfileMPEG4Simple, 640, 360);
    auto picture = Picture(640, 360);
    picture.vol_fields.bits.resync_marker_disable = 0;
    auto slice = Slice(0, i_bytes.size(), 3);
    Require(fixture.SubmitStatus(1, picture, slice, i_bytes) ==
                    VA_STATUS_SUCCESS &&
                fixture.mock.inputs.size() == 1 &&
                fixture.decode->mpeg4_timing.have_newest,
            "HEC-free resync-enabled Simple input is submitted");
    const auto first_i = fixture.mock.inputs[0];
    Require(SealDecodeBatch(&fixture.driver, fixture.decode.get()) ==
                    VA_STATUS_SUCCESS,
            "HEC-free Simple batch seals for replay");

    auto p = Picture(640, 360, 1, 1, VA_INVALID_SURFACE, 0, 1);
    p.vol_fields.bits.resync_marker_disable = 0;
    slice = Slice(0, p_bytes.size(), 7);
    Require(fixture.SubmitStatus(2, p, slice, p_bytes) == VA_STATUS_SUCCESS &&
                fixture.mock.inputs.size() == 1,
            "post-seal HEC-free Simple P waits behind the first batch");
    const auto *queued =
        fixture.decode->mpeg4_replay.Find(2 * kTimestampStep);
    Require(queued != nullptr,
            "queued HEC-free Simple P owns an immutable access unit");
    const auto queued_p = queued->bytes;
    fixture.mock.Queue(kTimestampStep, 640, 360, 1);
    fixture.mock.EosMarker();
    fixture.Receive();
    Require(PumpDecodeInput(&fixture.driver, fixture.decode.get()) ==
                    VA_STATUS_SUCCESS &&
                fixture.mock.inputs.size() == 3 &&
                fixture.mock.inputs[1].timestamp == kTimestampStep &&
                fixture.mock.inputs[1].bytes == first_i.bytes &&
                fixture.mock.inputs[2].timestamp == 2 * kTimestampStep &&
                fixture.mock.inputs[2].bytes == queued_p,
            "HEC-free packet headers remain exact across close/reopen replay");
  }

  {
    const std::vector<std::vector<uint8_t>> rejected = {
      {0x1f, 0x00, 0x00, 0xa8, 0x04, 0xff, 0x55, 0xa5},
      {0x1f, 0x00, 0x00, 0xa8, 0x04},
      {0x1f, 0x00, 0x00, 0x80, 0x04, 0x7f, 0x55, 0xa5},
      {0x1f, 0x00, 0x00, 0xf3, 0x04, 0x7f, 0x55, 0xa5},
      {0x1f, 0x00, 0x00, 0xa8, 0x00, 0x7f, 0x55, 0xa5}
    };
    const std::vector<uint8_t> valid =
        {0x1f, 0x00, 0x00, 0xa8, 0x04, 0x7f, 0x55, 0xa5};
    for (const auto &bytes : rejected) {
      Fixture fixture(VAProfileMPEG4Simple, 640, 360);
      auto picture = Picture(640, 360);
      picture.vol_fields.bits.resync_marker_disable = 0;
      auto slice = Slice(0, bytes.size(), 3);
      Require(fixture.SubmitStatus(1, picture, slice, bytes) !=
                      VA_STATUS_SUCCESS &&
                  fixture.mock.inputs.empty() && fixture.mock.opens == 0 &&
                  fixture.decode->next_timestamp == kTimestampStep &&
                  !fixture.decode->mpeg4_timing.have_newest &&
                  fixture.decode->mpeg4_replay.cached_pictures() == 0,
              "unsafe Simple packet header commits no hardware or replay state");
      slice = Slice(0, valid.size(), 3);
      Require(fixture.SubmitStatus(1, picture, slice, valid) ==
                      VA_STATUS_SUCCESS &&
                  fixture.mock.inputs.size() == 1,
              "fresh valid Simple packet recovers after rejected header");
    }
  }

  {
    Fixture fixture(VAProfileMPEG4AdvancedSimple);
    auto picture = Picture();
    picture.vol_fields.bits.resync_marker_disable = 0;
    Require(fixture.SubmitStatus(1, picture) != VA_STATUS_SUCCESS &&
                fixture.mock.inputs.empty() && fixture.mock.opens == 0,
            "resync-enabled Advanced Simple fails before hardware submission");
  }

  {
    Fixture fixture;
    fixture.I(1);
    const size_t before = fixture.mock.inputs.size();
    auto changed = Picture(32, 16, 1, 1, VA_INVALID_SURFACE, 0, 3);
    Require(fixture.SubmitStatus(2, changed) != VA_STATUS_SUCCESS &&
                fixture.mock.inputs.size() == before,
            "mid-context MPEG-4 geometry cannot silently change");
  }
  {
    Fixture fixture;
    fixture.I(1);
    const size_t before = fixture.mock.inputs.size();
    auto changed = Picture(16, 16, 1, 1, VA_INVALID_SURFACE, 0, 3);
    changed.vop_time_increment_resolution = 31;
    Require(fixture.SubmitStatus(2, changed) != VA_STATUS_SUCCESS &&
                fixture.mock.inputs.size() == before,
            "mid-context VOL timing resolution cannot silently change");
  }
}

void MalformedSlicesAndIq() {
  for (unsigned variant = 0; variant < 8; ++variant) {
    Fixture fixture;
    auto slice = Slice();
    auto bytes = SliceBytes();
    switch (variant) {
      case 0: slice.slice_data_flag = VA_SLICE_DATA_FLAG_BEGIN; break;
      case 1: slice.slice_data_offset = bytes.size() + 1; break;
      case 2: slice.slice_data_size = bytes.size() + 1; break;
      case 3: slice.macroblock_offset = bytes.size() * 8; break;
      case 4: slice.macroblock_number = 1; break;
      case 5: slice.quant_scale = 0; break;
      case 6: slice.quant_scale = 32; break;
      case 7: slice.slice_data_size = 0; break;
    }
    Require(fixture.SubmitStatus(1, Picture(), slice, bytes) !=
                    VA_STATUS_SUCCESS &&
                fixture.mock.inputs.empty() && fixture.mock.opens == 0,
            "malformed or fragmented MPEG-4 slice cannot reach hardware");
  }

  {
    Fixture fixture;
    fixture.Begin(1);
    auto picture = Picture();
    auto a = Slice(), b = Slice();
    VASliceParameterBufferMPEG4 slices[] = {a, b};
    auto bytes = SliceBytes();
    VABufferID ids[] = {
        fixture.Buffer(VAPictureParameterBufferType, &picture,
                       sizeof(picture)),
        fixture.Buffer(VASliceParameterBufferType, slices, sizeof(a), 2),
        fixture.Buffer(VASliceDataBufferType, bytes.data(), bytes.size())};
    VAStatus first = RenderPicture(&fixture.context, fixture.id, ids, 3);
    VAStatus end = EndPicture(&fixture.context, fixture.id);
    Require((first != VA_STATUS_SUCCESS || end != VA_STATUS_SUCCESS) &&
                fixture.mock.inputs.empty(),
            "initial backend rejects multi-slice MPEG-4 submissions");
  }

  {
    Fixture fixture;
    fixture.Begin(1);
    auto picture = Picture();
    auto slice = Slice();
    auto bytes = SliceBytes();
    VABufferID ids[] = {
        fixture.Buffer(VAPictureParameterBufferType, &picture,
                       sizeof(picture)),
        fixture.Buffer(VASliceParameterBufferType, &slice, sizeof(slice))};
    Require(RenderPicture(&fixture.context, fixture.id, ids, 2) ==
                VA_STATUS_SUCCESS &&
                EndPicture(&fixture.context, fixture.id) != VA_STATUS_SUCCESS &&
                fixture.mock.inputs.empty(),
            "missing slice-data partner is rejected transactionally");
    (void)bytes;
  }

  {
    Fixture fixture;
    auto picture = Picture();
    picture.vol_fields.bits.quant_type = 1;
    const auto matrix = Matrix();
    Require(fixture.SubmitStatus(1, picture, Slice(), SliceBytes(), &matrix) !=
                    VA_STATUS_SUCCESS &&
                fixture.mock.inputs.empty(),
            "unsupported MPEG quant matrices are rejected, not ignored");
  }

  {
    Fixture fixture;
    const auto matrix = Matrix();
    Require(fixture.SubmitStatus(1, Picture(), Slice(), SliceBytes(),
                                 &matrix) != VA_STATUS_SUCCESS &&
                fixture.mock.inputs.empty(),
            "IQ data is rejected when H.263 quantization is selected");
  }

  {
    Fixture fixture;
    fixture.Begin(1);
    auto picture = Picture();
    auto matrix = Matrix();
    auto slice = Slice();
    auto bytes = SliceBytes();
    VABufferID picture_id = fixture.Buffer(
        VAPictureParameterBufferType, &picture, sizeof(picture));
    std::vector<uint8_t> short_matrix(sizeof(matrix) - 1);
    VABufferID iq_id = fixture.Buffer(VAIQMatrixBufferType,
                                      short_matrix.data(), short_matrix.size());
    VABufferID slice_id = fixture.Buffer(VASliceParameterBufferType, &slice,
                                         sizeof(slice));
    VABufferID data_id = fixture.Buffer(VASliceDataBufferType, bytes.data(),
                                        bytes.size());
    VABufferID ids[] = {picture_id, iq_id, slice_id, data_id};
    VAStatus first = RenderPicture(&fixture.context, fixture.id, ids, 4);
    VAStatus end = EndPicture(&fixture.context, fixture.id);
    Require((first != VA_STATUS_SUCCESS || end != VA_STATUS_SUCCESS) &&
                fixture.mock.inputs.empty(),
            "wrong-sized MPEG-4 IQ buffer poisons the complete picture");
  }
}

void ReferenceIdentity() {
  for (unsigned variant = 0; variant < 6; ++variant) {
    Fixture fixture;
    fixture.I(1);
    fixture.P(2, 1);
    auto picture = Picture(16, 16, 1, 2, VA_INVALID_SURFACE, 0, 3);
    if (variant == 0) picture.forward_reference_picture = VA_INVALID_SURFACE;
    if (variant == 1) picture.forward_reference_picture = 99;
    if (variant == 2) picture.forward_reference_picture = 1;
    if (variant == 3) picture.forward_reference_picture = 3;
    if (variant == 4)
      fixture.driver.surfaces.at(2)->expected_timestamp = 0;
    if (variant == 5)
      fixture.driver.surfaces.at(2)->backing_owner = 1;
    const size_t before = fixture.mock.inputs.size();
    const uint64_t next = fixture.decode->next_timestamp;
    Require(fixture.SubmitStatus(3, picture) != VA_STATUS_SUCCESS &&
                fixture.mock.inputs.size() == before &&
                fixture.decode->next_timestamp == next,
            "missing, stale, target, alias or non-newest P reference is rejected");
  }

  {
    Fixture fixture;
    auto bad_i = Picture();
    bad_i.forward_reference_picture = 1;
    Require(fixture.SubmitStatus(2, bad_i) != VA_STATUS_SUCCESS &&
                fixture.mock.inputs.empty(),
            "I picture cannot carry an incidental reference");
  }

  {
    Fixture fixture;
    fixture.I(1);
    fixture.P(2, 1);
    auto swapped = Picture(16, 16, 2, 2, 1, 1, 3, 0);
    Require(fixture.SubmitStatus(3, swapped) != VA_STATUS_SUCCESS &&
                fixture.mock.inputs.size() == 2,
            "B references must match previous/newest anchor direction");
    fixture.B(3, 1, 2, 1, 3, 1);
    const auto *unit =
        fixture.decode->mpeg4_replay.Find(3 * kTimestampStep);
    Require(unit && unit->forward == kTimestampStep &&
                unit->backward == 2 * kTimestampStep,
            "valid unready B references snapshot exact immutable tokens");
  }

  {
    Fixture fixture;
    fixture.I(1);
    VAContextID foreign = VA_INVALID_ID;
    Require(CreateContext(&fixture.context, fixture.config, 16, 16,
                          VA_PROGRESSIVE, nullptr, 0, &foreign) ==
                VA_STATUS_SUCCESS,
            "independent MPEG-4 reference-owner context");
    auto other = fixture.driver.contexts.at(foreign);
    fixture.driver.surfaces.at(4)->expected_timestamp = kTimestampStep;
    other->surface_timestamps[fixture.driver.surfaces.at(4).get()] =
        kTimestampStep;
    const size_t before = fixture.mock.inputs.size();
    auto picture = Picture(16, 16, 1, 4, VA_INVALID_SURFACE, 0, 3);
    Require(fixture.SubmitStatus(2, picture) != VA_STATUS_SUCCESS &&
                fixture.mock.inputs.size() == before,
            "same numeric token from another context is not a reference");
  }
}

void ReplayReopenAndLaterIRoot() {
  Fixture fixture;
  fixture.I(1);                 // tick 0
  fixture.P(2, 1, 3);           // tick 3
  fixture.I(3, 3);              // tick 6
  fixture.B(4, 2, 3, 1, 3, 0); // tick 4, open GOP
  const auto initial = fixture.mock.inputs;
  Require(initial.size() == 4,
          "initial open-GOP sequence is fully submitted");
  Require(SealDecodeBatch(&fixture.driver, fixture.decode.get()) ==
                    VA_STATUS_SUCCESS &&
                fixture.mock.flushes == 1,
          "real finite-batch seal requests MPEG-4 EOS");

  fixture.P(5, 3, 3);           // tick 9, queued behind sealed batch
  Require(fixture.mock.inputs.size() == 4,
          "new P waits behind the sealed batch");
  const auto *queued =
      fixture.decode->mpeg4_replay.Find(5 * kTimestampStep);
  Require(queued != nullptr, "queued post-EOS P owns an immutable AU");
  const auto queued_bytes = queued->bytes;

  for (uint64_t token : {kTimestampStep, 4 * kTimestampStep,
                         2 * kTimestampStep, 3 * kTimestampStep})
    fixture.mock.Queue(token, 16, 16);
  fixture.mock.EosMarker();
  fixture.Receive();
  Require(fixture.decode->mpeg4_replay.NeedsRestart() &&
              fixture.mock.releases == 5,
          "reordered outputs and actual EOS complete the sealed batch");
  Require(PumpDecodeInput(&fixture.driver, fixture.decode.get()) ==
              VA_STATUS_SUCCESS,
          "transport performs full close/reopen and later-I replay");
  Require(fixture.mock.opens == 2 && fixture.mock.closes == 1 &&
              fixture.mock.stops == 1 && fixture.mock.inputs.size() == 6,
          "completed open-GOP prefix is pruned only after its B is complete");
  Require(fixture.mock.inputs[4].timestamp == 3 * kTimestampStep &&
              fixture.mock.inputs[4].bytes == initial[2].bytes,
          "later I replays as the exact self-contained VOL+VOP AU");
  Require(fixture.mock.inputs[5].timestamp == 5 * kTimestampStep &&
              fixture.mock.inputs[5].bytes == queued_bytes,
          "queued P preserves its original timing bits, bytes and token");
  CheckI(fixture.mock.inputs[4].bytes);

  fixture.mock.Queue(3 * kTimestampStep, 16, 16);
  fixture.mock.Queue(5 * kTimestampStep, 16, 16);
  fixture.Receive();
  Require(fixture.driver.surfaces.at(5)->ready &&
              fixture.decode->pending.empty(),
          "duplicate replay output cannot substitute for the queued P");
}

void RandomAccessTimingDiscontinuity() {
  Fixture fixture;
  fixture.I(1);
  Require(SealDecodeBatch(&fixture.driver, fixture.decode.get()) ==
                  VA_STATUS_SUCCESS,
          "pre-seek MPEG-4 batch seals");
  fixture.mock.Queue(kTimestampStep, 16, 16);
  fixture.mock.EosMarker();
  fixture.Receive();
  Require(fixture.decode->mpeg4_replay.ended() &&
              fixture.decode->mpeg4_replay.outstanding() == 0 &&
              fixture.driver.surfaces.at(1)->ready,
          "pre-seek picture is complete and remains owned");

  fixture.I(2, 0);  // FFmpeg's first post-flush random-access picture.
  Require(fixture.mock.opens == 2 && fixture.mock.closes == 1 &&
              fixture.mock.stops == 1 && fixture.mock.inputs.size() == 2 &&
              fixture.mock.flushes == 1 &&
              fixture.decode->mpeg4_draining_epochs.empty() &&
              !fixture.decode->mpeg4_replay.sealed(),
          "zero-TRD later I starts a fresh hardware/replay epoch");
  const auto *unit = fixture.decode->mpeg4_replay.Find(2 * kTimestampStep);
  Require(unit && unit->kind == CrystalHDMpeg4Replay::Kind::I &&
              fixture.decode->mpeg4_replay.cached_pictures() == 1 &&
              fixture.decode->mpeg4_timing.newest.tick == 0,
          "post-flush I is a self-contained tick-zero replay root");
  Require(fixture.driver.surfaces.at(1)->ready &&
              !fixture.driver.surfaces.at(1)->failed,
          "new timing epoch does not revoke an already completed old frame");
  fixture.P(3, 2);
  Require(fixture.mock.inputs.size() == 3 &&
              fixture.mock.inputs[2].timestamp == 3 * kTimestampStep &&
              fixture.mock.flushes == 1 &&
              !fixture.decode->mpeg4_replay.sealed(),
          "post-seek P is submitted without sealing the new epoch");
}

void PendingRandomAccessTimingDiscontinuity() {
  Fixture fixture;
  fixture.I(1);
  fixture.P(2, 1);
  fixture.mock.Queue(kTimestampStep, 16, 16);
  fixture.mock.Queue(2 * kTimestampStep, 16, 16);
  fixture.mock.EosMarker();

  fixture.I(3, 0);
  Require(fixture.mock.flushes == 1 && fixture.mock.releases == 3 &&
              fixture.mock.opens == 2 && fixture.mock.closes == 1,
          "post-flush I drains old outstanding pictures before restart");
  Require(fixture.driver.surfaces.at(1)->ready &&
              fixture.driver.surfaces.at(2)->ready &&
              !fixture.driver.surfaces.at(1)->failed &&
              !fixture.driver.surfaces.at(2)->failed,
          "old returned/held surfaces remain complete across restart");
  Require(fixture.decode->mpeg4_replay.cached_pictures() == 1 &&
              fixture.decode->mpeg4_replay.Find(3 * kTimestampStep) != nullptr &&
              fixture.decode->mpeg4_timing.newest.tick == 0,
          "pending discontinuity commits only the new random-access root");
}

void QueuedSealedDiscontinuityDrain() {
  Fixture fixture;
  fixture.I(1);
  Require(SealDecodeBatch(&fixture.driver, fixture.decode.get()) ==
                  VA_STATUS_SUCCESS,
          "old epoch seals before accepting more compressed input");
  fixture.P(2, 1);
  Require(fixture.mock.inputs.size() == 1 &&
              fixture.decode->mpeg4_replay.HasQueuedInput() &&
              !fixture.decode->mpeg4_replay.Drained(),
          "post-seal P remains visibly queued behind the old batch");

  fixture.mock.Queue(kTimestampStep, 16, 16);
  fixture.mock.EosMarker();
  fixture.mock.echo_inputs = true;
  fixture.mock.eos_on_flush = true;
  fixture.I(3, 0);

  Require(fixture.mock.inputs.size() == 3 &&
              fixture.decode->mpeg4_draining_epochs.size() == 1 &&
              fixture.decode->pending.count(2 * kTimestampStep) == 1 &&
              fixture.decode->pending.count(3 * kTimestampStep) == 1,
          "EndPicture queues the new epoch behind post-seal replay");
  Require(SyncSurface2(&fixture.context, 3, 1000 * 1000 * 1000ULL) ==
                  VA_STATUS_SUCCESS &&
              fixture.mock.inputs.size() == 4 &&
              fixture.mock.inputs[0].timestamp == kTimestampStep &&
              fixture.mock.inputs[1].timestamp == kTimestampStep &&
              fixture.mock.inputs[2].timestamp == 2 * kTimestampStep &&
              fixture.mock.inputs[3].timestamp == 3 * kTimestampStep &&
              fixture.mock.opens == 3 && fixture.mock.closes == 2 &&
              fixture.mock.flushes == 2,
          "discontinuity finishes the queued replay batch before resetting");
  Require(fixture.driver.surfaces.at(1)->ready &&
              fixture.driver.surfaces.at(2)->ready &&
              fixture.driver.surfaces.at(3)->ready &&
              !fixture.driver.surfaces.at(1)->failed &&
              !fixture.driver.surfaces.at(2)->failed &&
              fixture.decode->pending.empty(),
          "old replay and new epoch leave no stranded surface");
}

void BackpressuredDiscontinuityDrain() {
  Fixture fixture;
  fixture.mock.capacity = false;
  fixture.mock.echo_inputs = true;
  fixture.mock.eos_on_flush = true;
  fixture.I(1);
  Require(fixture.mock.inputs.empty() &&
              fixture.decode->mpeg4_replay.HasQueuedInput() &&
              fixture.decode->mpeg4_replay.outstanding() == 0 &&
              !fixture.decode->mpeg4_replay.Drained(),
          "TX backpressure is queued input, not a drained epoch");

  const auto start = std::chrono::steady_clock::now();
  const VAStatus status = fixture.SubmitStatus(2, Picture());
  const auto elapsed = std::chrono::steady_clock::now() - start;

  Require(status == VA_STATUS_SUCCESS &&
              elapsed < std::chrono::milliseconds(100) &&
              fixture.mock.inputs.empty() && fixture.mock.opens == 1 &&
              fixture.mock.closes == 0 &&
              fixture.decode->mpeg4_draining_epochs.size() == 1 &&
              fixture.decode->mpeg4_replay.Find(2 * kTimestampStep) != nullptr,
          "vaEndPicture queues a backpressured epoch without waiting");
  fixture.P(3, 2, 3);
  Require(fixture.mock.inputs.empty() &&
              fixture.decode->mpeg4_replay.Find(3 * kTimestampStep) != nullptr,
          "the next picture can reference the queued epoch immediately");

  fixture.mock.capacity = true;
  Require(SyncSurface2(&fixture.context, 3, 1000 * 1000 * 1000ULL) ==
                  VA_STATUS_SUCCESS &&
              fixture.mock.inputs.size() == 3 &&
              fixture.mock.inputs[0].timestamp == kTimestampStep &&
              fixture.mock.inputs[1].timestamp == 2 * kTimestampStep &&
              fixture.mock.inputs[2].timestamp == 3 * kTimestampStep &&
              fixture.mock.opens == 2 && fixture.mock.closes == 1 &&
              fixture.mock.flushes == 0 &&
              fixture.decode->mpeg4_draining_epochs.empty() &&
              fixture.decode->pending.empty(),
          "sync advances old and new epochs in exact submission order");
  for (VASurfaceID surface : {1U, 2U, 3U})
    Require(fixture.driver.surfaces.at(surface)->ready &&
                !fixture.driver.surfaces.at(surface)->failed,
            "backpressure recovery preserves every epoch surface");
}

void QueuedDiscontinuityTargetTeardown() {
  Fixture fixture;
  fixture.mock.capacity = false;
  fixture.mock.echo_inputs = true;
  fixture.mock.eos_on_flush = true;
  fixture.I(1);
  fixture.I(2);
  const uint64_t generation = fixture.decode->generation;
  VASurfaceID target = 2;
  Require(DestroySurfaces(&fixture.context, &target, 1) == VA_STATUS_SUCCESS &&
              fixture.driver.surfaces.count(2) == 0 &&
              fixture.driver.surfaces.at(1)->failed &&
              !fixture.driver.surfaces.at(1)->ready &&
              fixture.decode->pending.empty() &&
              fixture.decode->mpeg4_draining_epochs.empty() &&
              fixture.decode->mpeg4_replay.cached_pictures() == 0 &&
              fixture.decode->generation == generation + 1 &&
              fixture.mock.inputs.empty() && fixture.mock.closes == 1,
          "target teardown cancels every queued epoch without stranded state");
  VASurfaceStatus surface_status = VASurfaceReady;
  Require(QuerySurfaceStatus(&fixture.context, 1, &surface_status) ==
                  VA_STATUS_ERROR_DECODING_ERROR &&
              SyncSurface2(&fixture.context, 1, 0) ==
                  VA_STATUS_ERROR_DECODING_ERROR,
          "surviving canceled target reports decode failure, not Rendering");

  fixture.mock.capacity = true;
  fixture.I(1);
  fixture.Receive();
  Require(fixture.driver.surfaces.at(1)->ready &&
              !fixture.driver.surfaces.at(1)->failed,
          "a fresh picture can reuse a surface after teardown reset");
}

void CompletedSurfaceSurvivesTargetTeardown() {
  Fixture fixture(VAProfileMPEG4Simple);
  fixture.I(1);
  fixture.mock.Queue(kTimestampStep, 16, 16, 5);
  fixture.Receive();
  Require(fixture.driver.surfaces.at(1)->ready &&
              !fixture.driver.surfaces.at(1)->failed &&
              fixture.decode->have_mpeg4_simple_picture_number,
          "first target completes before a later teardown");

  fixture.mock.capacity = false;
  fixture.P(2, 1);
  VASurfaceID target = 2;
  Require(DestroySurfaces(&fixture.context, &target, 1) == VA_STATUS_SUCCESS &&
              fixture.driver.surfaces.at(1)->ready &&
              !fixture.driver.surfaces.at(1)->failed &&
              fixture.driver.surfaces.at(1)->frame_timestamp ==
                  kTimestampStep &&
              !fixture.decode->have_mpeg4_simple_picture_number,
          "teardown reset preserves an already completed held target");
  VASurfaceStatus surface_status = VASurfaceRendering;
  Require(QuerySurfaceStatus(&fixture.context, 1, &surface_status) ==
                  VA_STATUS_SUCCESS &&
              surface_status == VASurfaceReady &&
              SyncSurface2(&fixture.context, 1, 0) == VA_STATUS_SUCCESS,
          "completed target stays synchronizable after another target resets");
}

void BackToBackTimingDiscontinuities() {
  Fixture fixture;
  fixture.mock.capacity = false;
  fixture.mock.echo_inputs = true;
  fixture.mock.eos_on_flush = true;
  fixture.I(1);
  fixture.I(2);
  fixture.I(3);
  Require(fixture.decode->mpeg4_draining_epochs.size() == 2 &&
              fixture.decode->mpeg4_replay.Find(3 * kTimestampStep) != nullptr &&
              fixture.mock.inputs.empty(),
          "back-to-back seeks retain three ordered timing epochs");

  fixture.mock.capacity = true;
  Require(SyncSurface2(&fixture.context, 3, 1000 * 1000 * 1000ULL) ==
                  VA_STATUS_SUCCESS &&
              fixture.mock.inputs.size() == 3 &&
              fixture.mock.inputs[0].timestamp == kTimestampStep &&
              fixture.mock.inputs[1].timestamp == 2 * kTimestampStep &&
              fixture.mock.inputs[2].timestamp == 3 * kTimestampStep &&
              fixture.mock.opens == 3 && fixture.mock.closes == 2 &&
              fixture.mock.flushes == 0 &&
              fixture.decode->mpeg4_draining_epochs.empty() &&
              fixture.decode->pending.empty(),
          "each queued timing epoch drains and reopens exactly once");
}

void AutonomousWorkerDrainsBackpressuredDiscontinuities() {
  Fixture fixture;
  fixture.mock.capacity = false;
  fixture.mock.echo_inputs = true;
  fixture.mock.eos_on_flush = true;
  fixture.I(1);
  fixture.I(2);
  fixture.I(3);
  Require(fixture.decode->mpeg4_draining_epochs.size() == 2 &&
              fixture.mock.inputs.empty() && fixture.mock.flushes == 0,
          "worker regression starts with three backpressured timing epochs");

  fixture.mock.capacity = true;
  fixture.driver.StartDecodeWorker();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  bool queries_valid = true;
  bool timed_out = false;
  for (;;) {
    bool ready = true;
    for (VASurfaceID surface : {1U, 2U, 3U}) {
      VASurfaceStatus status = VASurfaceRendering;
      if (QuerySurfaceStatus(&fixture.context, surface, &status) !=
          VA_STATUS_SUCCESS) {
        queries_valid = false;
        break;
      }
      ready &= status == VASurfaceReady;
    }
    if (!queries_valid || ready)
      break;
    if (std::chrono::steady_clock::now() >= deadline) {
      timed_out = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Require(queries_valid, "query-only MPEG-4 progress remains valid");
  Require(!timed_out,
          "worker advances every explicit MPEG-4 timing epoch");

  std::lock_guard<std::mutex> lock(fixture.driver.mutex);
  Require(fixture.mock.inputs.size() == 3 &&
              fixture.mock.inputs[0].timestamp == kTimestampStep &&
              fixture.mock.inputs[1].timestamp == 2 * kTimestampStep &&
              fixture.mock.inputs[2].timestamp == 3 * kTimestampStep &&
              fixture.mock.opens == 3 && fixture.mock.closes == 2 &&
              fixture.mock.flushes == 2 && fixture.mock.releases == 5 &&
              fixture.decode->mpeg4_draining_epochs.empty() &&
              fixture.decode->pending.empty(),
          "worker retries only explicit epoch seals and preserves order");
}

void QueuedMissingMarkerEpochFence() {
  Fixture fixture;
  fixture.I(1);
  fixture.I(2);
  Require(fixture.decode->mpeg4_draining_epochs.size() == 1 &&
              fixture.mock.inputs.size() == 1 && fixture.mock.flushes == 1 &&
              fixture.mock.opens == 1 && fixture.mock.closes == 0,
          "discontinuity seals the submitted old epoch without reopening");

  fixture.mock.Queue(kTimestampStep, 16, 16);
  fixture.Receive();
  const unsigned false_polls = fixture.mock.eos_polls;
  Require(fixture.driver.surfaces.at(1)->ready &&
              fixture.decode->ReplaySealed() && false_polls != 0 &&
              fixture.decode->mpeg4_draining_epochs.size() == 1,
          "completed pictures do not replace a missing old-epoch EOS fence");
  Require(PumpDecodeInput(&fixture.driver, fixture.decode.get()) ==
                  VA_STATUS_SUCCESS &&
              fixture.mock.inputs.size() == 1 && fixture.mock.opens == 1 &&
              fixture.mock.closes == 0,
          "false library EOS keeps the next epoch off the old transport");

  fixture.mock.eos = true;
  fixture.Receive();
  Require(PumpDecodeInput(&fixture.driver, fixture.decode.get()) ==
                  VA_STATUS_SUCCESS &&
              fixture.mock.eos_polls == false_polls + 1 &&
              fixture.decode->mpeg4_draining_epochs.empty() &&
              fixture.mock.inputs.size() == 2 &&
              fixture.mock.inputs[1].timestamp == 2 * kTimestampStep &&
              fixture.mock.opens == 2 && fixture.mock.closes == 1,
          "positive library EOS advances and submits the queued epoch");
}

void CombinedEpochCacheBound() {
  Fixture fixture;
  fixture.mock.capacity = false;
  for (unsigned picture = 0; picture < kMpeg4ReplayPictures; ++picture)
    Require(fixture.SubmitStatus(1 + picture % 8, Picture()) ==
                    VA_STATUS_SUCCESS,
            "each picture within the combined epoch bound is accepted");
  const uint64_t next = fixture.decode->next_timestamp;
  Require(fixture.decode->Mpeg4CachedPictures() == kMpeg4ReplayPictures &&
              fixture.SubmitStatus(1, Picture()) == VA_STATUS_ERROR_HW_BUSY &&
              fixture.decode->next_timestamp == next &&
              fixture.decode->Mpeg4CachedPictures() == kMpeg4ReplayPictures,
          "all queued epochs share one transactional picture bound");
}

void EarlyEndMarkerBeforeFinalPicture() {
  Fixture fixture;
  fixture.I(1);
  fixture.P(2, 1);
  Require(SealDecodeBatch(&fixture.driver, fixture.decode.get()) ==
                  VA_STATUS_SUCCESS,
          "early-marker batch seals");
  fixture.mock.Queue(kTimestampStep, 16, 16);
  fixture.mock.EosMarker();
  fixture.mock.Queue(2 * kTimestampStep, 16, 16);
  fixture.mock.EosMarker();
  fixture.Receive();
  Require(fixture.decode->mpeg4_replay.ended() &&
              fixture.decode->mpeg4_replay.outstanding() == 0 &&
              fixture.decode->pending.empty() &&
              fixture.driver.surfaces.at(1)->ready &&
              fixture.driver.surfaces.at(2)->ready &&
              fixture.mock.releases == 4,
          "early EOS marker cannot discard the final reordered picture");
}

void Mpeg4OnlyEndOfStreamFallback() {
  {
    Fixture fixture;
    fixture.I(1);
    fixture.P(2, 1);
    Require(SealDecodeBatch(&fixture.driver, fixture.decode.get()) ==
                    VA_STATUS_SUCCESS &&
                fixture.decode->ReplaySealed(),
            "MPEG-4 batch is sealed before fallback testing");

    fixture.mock.eos = true;
    Require(ReceiveAvailable(&fixture.driver, fixture.decode.get()) ==
                    VA_STATUS_SUCCESS &&
                fixture.mock.eos_polls == 0 &&
                fixture.decode->ReplaySealed(),
            "library EOS cannot end MPEG-4 while timestamps remain outstanding");

    fixture.mock.eos = false;
    fixture.mock.Queue(kTimestampStep, 16, 16);
    fixture.mock.Queue(2 * kTimestampStep, 16, 16);
    fixture.Receive();
    Require(fixture.decode->ReplaySealed(),
            "missing firmware marker leaves the completed batch sealed");
    const unsigned before = fixture.mock.eos_polls;
    Require(ReceiveAvailable(&fixture.driver, fixture.decode.get()) ==
                    VA_STATUS_SUCCESS &&
                fixture.mock.eos_polls == before + 1 &&
                fixture.decode->ReplaySealed(),
            "a false library EOS poll is not completion evidence");
    fixture.mock.eos = true;
    Require(ReceiveAvailable(&fixture.driver, fixture.decode.get()) ==
                    VA_STATUS_SUCCESS &&
                fixture.mock.eos_polls == before + 2 &&
                !fixture.decode->ReplaySealed() &&
                fixture.mock.releases == 2,
            "fully drained MPEG-4 accepts the fenced library EOS fallback");
  }

  {
    DecoderMock mock;
    MockScope scope(&mock);
    Driver driver(-1);
    DecodeContext decode;
    decode.profile = VAProfileH264Main;
    decode.width = decode.height = 16;
    Require(OpenDecoder(&decode) == VA_STATUS_SUCCESS,
            "mock H264 decoder opens for codec-scope check");
    Require(decode.replay.Append(kTimestampStep, true,
                                 {0, 0, 0, 1, 0x65, 0x80}) &&
                PumpDecodeInput(&driver, &decode) == VA_STATUS_SUCCESS &&
                decode.replay.Seal() &&
                decode.replay.Observe(kTimestampStep) ==
                    CrystalHDDecodeReplay::Output::New,
            "construct completed sealed non-MPEG4 replay state");
    mock.eos = true;
    Require(ReceiveAvailable(&driver, &decode) == VA_STATUS_SUCCESS &&
                mock.eos_polls == 0 && decode.ReplaySealed(),
            "DtsIsEndOfStream fallback is never generalized to H264");
    Require(decode.Close() == BC_STS_SUCCESS,
            "non-MPEG4 scope closes cleanly");
  }
}
}  // namespace

int main() {
  const std::pair<const char *, void (*)()> tests[] = {
      {"profiles, DIVX input and BCM70012 gate",
       ProfilesInputFormatAndDeviceGate},
      {"owned public buffers and render ordering",
       OwnedBuffersAndPublicOrdering},
      {"I/P/B timing and open-GOP root propagation",
       ReferencesTimingAndOpenGop},
      {"Simple zero-TRD anchors", SimpleZeroTrdAnchors},
      {"Simple output identity and zero-token recovery", SimpleOutputIdentity},
      {"Simple recovery across replay reopen", SimpleRecoveryAcrossReplay},
      {"unsupported MPEG-4 tools and stable configuration",
       UnsupportedToolsAndConfiguration},
      {"malformed slices and IQ rejection", MalformedSlicesAndIq},
      {"reference identity, aliases and context ownership",
       ReferenceIdentity},
      {"actual EOS reopen from a later I root",
       ReplayReopenAndLaterIRoot},
      {"random-access timing discontinuity", RandomAccessTimingDiscontinuity},
      {"pending random-access timing discontinuity",
       PendingRandomAccessTimingDiscontinuity},
      {"backward timing discontinuity", BackwardTimingDiscontinuity},
      {"queued sealed-batch discontinuity drain",
       QueuedSealedDiscontinuityDrain},
      {"nonblocking backpressured discontinuity epochs",
       BackpressuredDiscontinuityDrain},
      {"queued discontinuity target teardown",
       QueuedDiscontinuityTargetTeardown},
      {"completed target survives teardown reset",
       CompletedSurfaceSurvivesTargetTeardown},
      {"back-to-back timing discontinuities",
       BackToBackTimingDiscontinuities},
      {"worker drains backpressured timing discontinuities",
       AutonomousWorkerDrainsBackpressuredDiscontinuities},
      {"queued missing-marker epoch fence", QueuedMissingMarkerEpochFence},
      {"combined timing-epoch cache bound", CombinedEpochCacheBound},
      {"early EOS marker before final picture",
       EarlyEndMarkerBeforeFinalPicture},
      {"MPEG-4-only missing-marker EOS fallback",
       Mpeg4OnlyEndOfStreamFallback},
  };
  unsigned failed = 0;
  for (const auto &test : tests) {
    try {
      test.second();
      std::printf("%s: PASS\n", test.first);
    } catch (const std::exception &error) {
      ++failed;
      std::fprintf(stderr, "%s: FAIL: %s\n", test.first, error.what());
    }
  }
  std::printf("%zu groups, %u checks, %u failures\n",
              sizeof(tests) / sizeof(tests[0]), checks.load(), failed);
  return failed ? 1 : 0;
}
