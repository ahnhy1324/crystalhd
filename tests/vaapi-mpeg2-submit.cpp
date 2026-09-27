// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual public VA submission, owned buffers and MPEG-2 replay. All decoder
// calls are deterministic mocks; unexpected graphics or ioctl access aborts.
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
struct MockOutput {
  uint64_t timestamp;
  unsigned width, height;
  bool eos;
  std::vector<uint8_t> pixels;
};
struct DecoderMock {
  bool capacity = true, opened = false, leased = false;
  unsigned opens = 0, closes = 0, starts = 0, stops = 0, flushes = 0, releases = 0;
  BC_INPUT_FORMAT format = {};
  std::vector<InputRecord> inputs;
  std::deque<MockOutput> outputs;
  void Queue(uint64_t timestamp, unsigned width, unsigned height) {
    outputs.push_back({timestamp, width, height, false,
                      std::vector<uint8_t>(static_cast<size_t>(width)*height*2, 128)});
  }
  void Eos() { outputs.push_back({0, 0, 0, true, {}}); }
};
DecoderMock *active = nullptr;
void CheckDevice(HANDLE device) {
  Require(active && device == active && active->opened, "private opened decoder handle only");
}
struct MockScope {
  explicit MockScope(DecoderMock *m) { Require(!active, "single fixture"); active = m; }
  ~MockScope() { active = nullptr; }
};
}
extern "C" BC_STATUS DtsDeviceOpen(HANDLE *device, uint32_t) {
  Require(active && !active->opened && device, "mock open has no live predecessor");
  active->opened = true; ++active->opens; *device = active; return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsCrystalHDVersion(HANDLE device, PBC_INFO_CRYSTAL version) {
  CheckDevice(device); *version = {}; version->device = 1; return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsSetInputFormat(HANDLE device, BC_INPUT_FORMAT *format) {
  CheckDevice(device);
  Require(format && format->mSubtype == BC_MSUBTYPE_MPEG2VIDEO && format->Progressive,
          "real OpenDecoder requests progressive MPEG-2, not H264");
  active->format = *format; return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsOpenDecoder(HANDLE device, uint32_t stream) {
  CheckDevice(device); Require(stream == BC_STREAM_TYPE_ES, "elementary stream mode"); return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsSetColorSpace(HANDLE device, BC_OUTPUT_FORMAT mode) {
  CheckDevice(device); Require(mode == OUTPUT_MODE422_YUY2, "YUY2 capture remains unchanged"); return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsStartDecoder(HANDLE device) {
  CheckDevice(device); ++active->starts; return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsStartCapture(HANDLE device) { CheckDevice(device); return BC_STS_SUCCESS; }
extern "C" BC_STATUS DtsStopDecoder(HANDLE device) {
  CheckDevice(device); Require(!active->leased, "stop has no external output lease"); ++active->stops; return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsCloseDecoder(HANDLE device) { CheckDevice(device); return BC_STS_SUCCESS; }
extern "C" BC_STATUS DtsDeviceClose(HANDLE device) {
  CheckDevice(device); ++active->closes; active->opened = false; return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsGetDriverStatus(HANDLE device, BC_DTS_STATUS *status) {
  CheckDevice(device); *status = {}; status->ReadyListCount = active->outputs.size(); return BC_STS_SUCCESS;
}
extern "C" uint32_t DtsTxFreeSize(HANDLE device) {
  CheckDevice(device); return active->capacity ? 1024*1024 : 0;
}
extern "C" BC_STATUS DtsProcInput(HANDLE device, uint8_t *bytes, uint32_t size,
                                  uint64_t timestamp, BOOL encrypted) {
  CheckDevice(device);
  Require(bytes && size && timestamp && !encrypted, "complete owned timestamped input");
  active->inputs.push_back({timestamp, std::vector<uint8_t>(bytes, bytes+size)});
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsProcOutputNoCopy(HANDLE device, uint32_t, BC_DTS_PROC_OUT *output) {
  CheckDevice(device); Require(!active->leased, "one output lease at a time");
  if (active->outputs.empty()) return BC_STS_NO_DATA;
  auto &next = active->outputs.front();
  *output = {}; output->PoutFlags = BC_POUT_FLAGS_PIB_VALID;
  output->PicInfo.timeStamp = next.timestamp;
  output->PicInfo.width = next.width; output->PicInfo.height = next.height;
  output->PicInfo.flags = next.eos ? VDEC_FLAG_EOS : 0;
  output->Ybuff = next.pixels.data(); output->YBuffDoneSz = next.pixels.size()/4;
  active->leased = true; return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsReleaseOutputBuffs(HANDLE device, PVOID, BOOL change) {
  CheckDevice(device); Require(active->leased && !change, "release exactly the returned picture lease");
  active->leased = false; ++active->releases; active->outputs.pop_front(); return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsFlushInput(HANDLE device, uint32_t mode) {
  CheckDevice(device); Require(mode == 0, "finite EOS, never reset-as-drain"); ++active->flushes; return BC_STS_SUCCESS;
}
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
VAPictureParameterBufferMPEG2 Picture(unsigned width = 16, unsigned height = 32,
                                     unsigned kind = 1,
                                     VASurfaceID f = VA_INVALID_SURFACE,
                                     VASurfaceID b = VA_INVALID_SURFACE) {
  VAPictureParameterBufferMPEG2 p = {};
  p.horizontal_size = width; p.vertical_size = height;
  p.forward_reference_picture = f; p.backward_reference_picture = b;
  p.picture_coding_type = kind; p.f_code = 0x1111;
  p.picture_coding_extension.bits.picture_structure = 3;
  p.picture_coding_extension.bits.frame_pred_frame_dct = 1;
  p.picture_coding_extension.bits.progressive_frame = 1;
  return p;
}
VAIQMatrixBufferMPEG2 Custom(unsigned seed = 0) {
  VAIQMatrixBufferMPEG2 q = {};
  q.load_intra_quantiser_matrix = q.load_non_intra_quantiser_matrix = 1;
  q.load_chroma_intra_quantiser_matrix = q.load_chroma_non_intra_quantiser_matrix = 1;
  uint8_t *lists[] = {q.intra_quantiser_matrix, q.non_intra_quantiser_matrix,
                     q.chroma_intra_quantiser_matrix, q.chroma_non_intra_quantiser_matrix};
  for (unsigned list = 0; list < 4; ++list)
    for (unsigned i = 0; i < 64; ++i) lists[list][i] = 1 + (seed + list*39 + i*11) % 255;
  lists[0][0] = lists[2][0] = 8;
  return q;
}
std::vector<uint8_t> SliceBytes(unsigned row, uint8_t tag = 0x95) {
  return {0, 0, 1, static_cast<uint8_t>(row+1), 0x43, tag, 0x77, 0x88};
}
VASliceParameterBufferMPEG2 Slice(unsigned row = 0, unsigned offset = 0) {
  VASliceParameterBufferMPEG2 p = {};
  p.slice_data_size = 8; p.slice_data_offset = offset;
  p.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
  p.macroblock_offset = 38; p.slice_vertical_position = row;
  p.quantiser_scale_code = 8; return p;
}
unsigned ReadBits(const std::vector<uint8_t> &data, size_t *bit, unsigned count) {
  Require(*bit + count <= data.size()*8, "submitted MPEG-2 syntax remains bounded");
  unsigned result = 0;
  while (count--) { result = (result << 1) | ((data[*bit/8] >> (7-*bit%8)) & 1); ++*bit; }
  return result;
}
std::array<std::array<uint8_t,64>,4> MatrixBytes(const std::vector<uint8_t> &data) {
  for (size_t n = 0; n + 5 <= data.size(); ++n) {
    if (data[n] || data[n+1] || data[n+2] != 1 || data[n+3] != 0xb5 || data[n+4] >> 4 != 3) continue;
    size_t bit = (n+4)*8 + 4;
    std::array<std::array<uint8_t,64>,4> result = {};
    for (auto &list : result) {
      Require(ReadBits(data, &bit, 1) == 1, "all effective matrices encoded explicitly");
      for (auto &coefficient : list) coefficient = ReadBits(data, &bit, 8);
    }
    return result;
  }
  throw std::runtime_error("submitted AU is missing its quantization snapshot");
}
void CheckMatrices(const std::vector<uint8_t> &data, const VAIQMatrixBufferMPEG2 &q) {
  const auto decoded = MatrixBytes(data);
  const uint8_t *lists[] = {q.intra_quantiser_matrix, q.non_intra_quantiser_matrix,
      q.chroma_intra_quantiser_matrix, q.chroma_non_intra_quantiser_matrix};
  for (unsigned list = 0; list < 4; ++list)
    Require(!std::memcmp(decoded[list].data(), lists[list], 64), "retained full matrix bytes match caller snapshot");
}
void CheckTail(const std::vector<uint8_t> &actual, const std::vector<uint8_t> &tail) {
  Require(actual.size() >= tail.size() &&
          std::equal(tail.begin(), tail.end(), actual.end()-tail.size()),
          "each slice preserves its own data-buffer-relative offset and exact bytes");
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
  explicit Fixture(VAProfile profile = VAProfileMPEG2Main, unsigned w = 16, unsigned h = 32)
      : width(w), height(h) {
    context.pDriverData = &driver;
    Require(CreateConfig(&context, profile, VAEntrypointVLD, nullptr, 0, &config) == VA_STATUS_SUCCESS,
            "public MPEG-2 profile configuration");
    Require(CreateContext(&context, config, width, height, VA_PROGRESSIVE,
                          nullptr, 0, &id) == VA_STATUS_SUCCESS, "public MPEG-2 context");
    decode = driver.contexts.at(id);
    Require(decode->profile == profile && decode->IsMpeg2(), "created context retains selected profile");
    Surface(1); Surface(2); Surface(3); Surface(4);
  }
  void Surface(VASurfaceID id, unsigned w = 0, unsigned h = 0) {
    auto surface = std::make_shared<::Surface>();
    Require(surface->AllocateInternal(nullptr, -1, w ? w : width, h ? h : height, VA_FOURCC_NV12),
            "private CPU surface allocation");
    driver.surfaces[id] = std::move(surface);
  }
  VABufferID Buffer(VABufferType type, const void *data, unsigned bytes,
                    unsigned elements = 1, VAContextID owner = VA_INVALID_ID) {
    VABufferID b = VA_INVALID_ID;
    Require(CreateBuffer(&context, owner == VA_INVALID_ID ? id : owner, type,
                          bytes, elements, const_cast<void *>(data), &b) == VA_STATUS_SUCCESS,
            "real public buffer allocation");
    return b;
  }
  void Begin(VASurfaceID target = 1) {
    Require(BeginPicture(&context, id, target) == VA_STATUS_SUCCESS, "public BeginPicture");
  }
  VAStatus RenderStatus(VABufferID b) { return RenderPicture(&context, id, &b, 1); }
  void Render(VABufferID b) { Require(RenderStatus(b) == VA_STATUS_SUCCESS, "public RenderPicture"); }
  void Parameters(const VAPictureParameterBufferMPEG2 &p, const VAIQMatrixBufferMPEG2 *q = nullptr) {
    Render(Buffer(VAPictureParameterBufferType, &p, sizeof(p)));
    if (q) Render(Buffer(VAIQMatrixBufferType, q, sizeof(*q)));
    auto s = Slice(); auto bytes = SliceBytes(0);
    Render(Buffer(VASliceParameterBufferType, &s, sizeof(s)));
    Render(Buffer(VASliceDataBufferType, bytes.data(), bytes.size()));
  }
  void End() { Require(EndPicture(&context, id) == VA_STATUS_SUCCESS, "public EndPicture/SubmitPicture"); }
  void Submit(VASurfaceID target, unsigned kind = 1,
              VASurfaceID f = VA_INVALID_SURFACE, VASurfaceID b = VA_INVALID_SURFACE) {
    Begin(target); Parameters(Picture(width, height, kind, f, b)); End();
  }
  void Receive() { Require(ReceiveAvailable(&driver, decode.get()) == VA_STATUS_SUCCESS, "actual receive/output dispatch"); }
};

void ProfilesAndInputFormat() {
  for (VAProfile profile : {VAProfileMPEG2Simple, VAProfileMPEG2Main}) {
    Fixture f(profile);
    VAProfile advertised[16] = {}; int advertised_count = 0;
    Require(QueryConfigProfiles(&f.context, advertised, &advertised_count) == VA_STATUS_SUCCESS &&
            advertised_count <= 16 && std::find(advertised, advertised+advertised_count, profile) != advertised+advertised_count,
            "new profile is actually advertised");
    VAConfigAttrib caps[] = {{VAConfigAttribMaxPictureWidth, 0},
                            {VAConfigAttribMaxPictureHeight, 0},
                            {VAConfigAttribRTFormat, 0}};
    Require(GetConfigAttributes(&f.context, profile, VAEntrypointVLD, caps, 3) == VA_STATUS_SUCCESS,
            "public configuration capability query");
    const unsigned max_width = profile == VAProfileMPEG2Simple ? 720 : 1920;
    const unsigned max_height = profile == VAProfileMPEG2Simple ? 576 : 1088;
    Require(caps[0].value == max_width && caps[1].value == max_height &&
            caps[2].value == VA_RT_FORMAT_YUV420, "profile-specific progressive 420 limits");
    unsigned count = 0;
    Require(QuerySurfaceAttributes(&f.context, f.config, nullptr, &count) == VA_STATUS_SUCCESS,
            "public surface capability count");
    std::vector<VASurfaceAttrib> attributes(count);
    Require(QuerySurfaceAttributes(&f.context, f.config, attributes.data(), &count) == VA_STATUS_SUCCESS,
            "public surface capability values");
    unsigned seen = 0;
    for (const auto &attribute : attributes) {
      if (attribute.type == VASurfaceAttribMaxWidth) {
        Require(attribute.value.value.i == static_cast<int>(max_width), "surface width agrees with config"); ++seen;
      }
      if (attribute.type == VASurfaceAttribMaxHeight) {
        Require(attribute.value.value.i == static_cast<int>(max_height), "surface height agrees with config"); ++seen;
      }
    }
    Require(seen == 2, "both surface bounds reported");
    for (const auto &geometry : {std::pair<int,int>{int(max_width)+16,int(max_height)},
                                std::pair<int,int>{int(max_width),int(max_height)+16}}) {
      VAContextID oversized = VA_INVALID_ID;
      Require(CreateContext(&f.context, f.config, geometry.first, geometry.second,
                            VA_PROGRESSIVE, nullptr, 0, &oversized) != VA_STATUS_SUCCESS,
              "context creation enforces advertised profile bounds");
    }
    f.Submit(1);
    Require(f.mock.opens == 1 && f.mock.inputs.size() == 1, "one accepted first I picture");
    Require(f.mock.format.width == 16 && f.mock.format.height == 32, "configured input dimensions");
    Require(f.decode->replay.cached_pictures() == 0 && f.decode->mpeg2_replay.cached_pictures() == 1,
            "MPEG-2 never enters the H264 IDR transport");
  }
}
void OwnedBuffersAndOrdering() {
  for (bool grouped : {false, true}) {
    std::array<unsigned,4> order{0,1,2,3};
    do {
      Fixture f;
      auto picture = Picture(); auto matrix = Custom(5); const auto expected = matrix;
      auto slice = Slice(); auto bytes = SliceBytes(0);
      VABufferID ids[] = {f.Buffer(VAPictureParameterBufferType, &picture, sizeof(picture)),
        f.Buffer(VAIQMatrixBufferType, &matrix, sizeof(matrix)),
        f.Buffer(VASliceParameterBufferType, &slice, sizeof(slice)),
        f.Buffer(VASliceDataBufferType, bytes.data(), bytes.size())};
      f.Begin();
      if (grouped) {
        VABufferID ordered[] = {ids[order[0]], ids[order[1]], ids[order[2]], ids[order[3]]};
        Require(RenderPicture(&f.context, f.id, ordered, 4) == VA_STATUS_SUCCESS, "arbitrary grouped buffer order");
      } else for (unsigned index : order) f.Render(ids[index]);
      for (VABufferID id : ids) {
        void *mapped = nullptr;
        Require(MapBuffer(&f.context, id, &mapped) == VA_STATUS_SUCCESS, "map original public buffer");
        std::memset(mapped, 0xee, f.driver.buffers.at(id).data.size());
        Require(UnmapBuffer(&f.context, id) == VA_STATUS_SUCCESS &&
                DestroyBuffer(&f.context, id) == VA_STATUS_SUCCESS, "original storage can disappear after Render");
      }
      f.End(); Require(f.mock.inputs.size() == 1, "one immutable accepted AU");
      CheckMatrices(f.mock.inputs.front().bytes, expected);
      CheckTail(f.mock.inputs.front().bytes, bytes);
    } while (std::next_permutation(order.begin(), order.end()));
  }
}
void SliceGroupsAndOffsets() {
  for (bool array : {false, true}) {
    Fixture f; f.Begin(); auto picture = Picture();
    f.Render(f.Buffer(VAPictureParameterBufferType, &picture, sizeof(picture)));
    const auto a = SliceBytes(0, 0x91), b = SliceBytes(1, 0xa9);
    auto sa = Slice(0, 3), sb = Slice(1, array ? 17 : 7);
    std::vector<uint8_t> first(array ? 25 : 11, 0xde), second(15, 0xad);
    std::copy(a.begin(), a.end(), first.begin()+3);
    std::copy(b.begin(), b.end(), array ? first.begin()+17 : second.begin()+7);
    if (array) {
      VASliceParameterBufferMPEG2 parameters[] = {sa, sb};
      f.Render(f.Buffer(VASliceDataBufferType, first.data(), first.size()));
      f.Render(f.Buffer(VASliceParameterBufferType, parameters, sizeof(sa), 2));
    } else {
      f.Render(f.Buffer(VASliceParameterBufferType, &sa, sizeof(sa)));
      f.Render(f.Buffer(VASliceParameterBufferType, &sb, sizeof(sb)));
      f.Render(f.Buffer(VASliceDataBufferType, first.data(), first.size()));
      f.Render(f.Buffer(VASliceDataBufferType, second.data(), second.size()));
    }
    f.End(); auto joined = a; joined.insert(joined.end(), b.begin(), b.end());
    CheckTail(f.mock.inputs.front().bytes, joined);
  }
}
void MatrixCommitAndFailureAtomicity() {
  Fixture f; auto original = Custom(7);
  f.Begin(); f.Parameters(Picture(), &original); f.End();
  const auto matrices = f.decode->mpeg2_matrices.lists;
  const uint64_t next = f.decode->next_timestamp;
  const size_t inputs = f.mock.inputs.size();
  auto invalid = Custom(31); invalid.non_intra_quantiser_matrix[23] = 0;
  f.Begin(2); f.Parameters(Picture(), &invalid);
  Require(EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS, "invalid active coefficient rejected");
  Require(f.decode->mpeg2_matrices.lists == matrices && f.decode->next_timestamp == next &&
          f.mock.inputs.size() == inputs, "failure commits neither IQ state, token nor input");
  f.Begin(2); f.Parameters(Picture()); f.End();
  CheckMatrices(f.mock.inputs.back().bytes, original);
  auto replacement = Custom(43); f.Begin(3); f.Parameters(Picture(), &replacement); f.End();
  CheckMatrices(f.mock.inputs.back().bytes, replacement);
}
void MalformedBuffersPoison() {
  for (VABufferType type : {VAPictureParameterBufferType, VAIQMatrixBufferType, VASliceParameterBufferType}) {
    unsigned size = type == VAPictureParameterBufferType ? sizeof(VAPictureParameterBufferMPEG2) :
        type == VAIQMatrixBufferType ? sizeof(VAIQMatrixBufferMPEG2) : sizeof(VASliceParameterBufferMPEG2);
    for (unsigned wrong : {1U, size-1, size+1}) {
      Fixture f; f.Begin(); std::vector<uint8_t> data(wrong, 0);
      auto id = f.Buffer(type, data.data(), wrong);
      Require(f.RenderStatus(id) != VA_STATUS_SUCCESS, "wrong element size rejected immediately");
      Require(EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS, "ignored Render error poisons End");
      Require(f.mock.inputs.empty() && !f.mock.opens && f.decode->next_timestamp == kTimestampStep,
              "malformed input never opens/submits/advances");
      f.Submit(1); Require(f.mock.inputs.size() == 1, "fresh Begin recovers");
    }
  }
  for (bool picture : {false, true}) {
    Fixture f; f.Begin();
    std::vector<uint8_t> data(2*(picture ? sizeof(VAPictureParameterBufferMPEG2) : sizeof(VAIQMatrixBufferMPEG2)));
    auto id = f.Buffer(picture ? VAPictureParameterBufferType : VAIQMatrixBufferType,
                       data.data(), data.size()/2, 2);
    Require(f.RenderStatus(id) != VA_STATUS_SUCCESS, "picture/IQ requires one element");
    Require(EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS && !f.mock.opens, "bad element count cannot submit");
  }
  {
    Fixture f; auto p = Picture(); auto b = f.Buffer(VAPictureParameterBufferType, &p, sizeof(p));
    Require(BufferSetNumElements(&f.context, b, 0) == VA_STATUS_SUCCESS, "public buffer resized to zero elements");
    f.Begin(); Require(f.RenderStatus(b) != VA_STATUS_SUCCESS &&
                       EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS && !f.mock.opens,
                       "zero-element metadata is not a valid picture");
  }
  {
    Fixture f; f.Begin(); f.Parameters(Picture());
    uint8_t unknown = 0;
    auto b = f.Buffer(VAImageBufferType, &unknown, sizeof(unknown));
    Require(f.RenderStatus(b) != VA_STATUS_SUCCESS &&
            EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS && !f.mock.opens,
            "unknown decode payload poisons otherwise complete parameters");
  }
}
void BadCallsContextsAndMissingPairs() {
  for (int count : {-1, 1}) {
    Fixture f; f.Begin();
    Require(RenderPicture(&f.context, f.id, nullptr, count) != VA_STATUS_SUCCESS, "invalid public array/count rejected");
    Require(EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS && !f.mock.opens, "invalid call poisons transaction");
    f.Submit(1);
  }
  {
    Fixture f; f.Begin(); f.Parameters(Picture());
    Require(RenderPicture(&f.context, f.id, nullptr, 0) == VA_STATUS_SUCCESS,
            "zero-buffer Render remains an innocuous compatibility no-op");
    f.End();
  }
  {
    Fixture f; VAContextID other = VA_INVALID_ID;
    Require(CreateContext(&f.context, f.config, 16, 32, VA_PROGRESSIVE, nullptr, 0, &other) == VA_STATUS_SUCCESS,
            "second public context");
    auto p = Picture(); auto b = f.Buffer(VAPictureParameterBufferType, &p, sizeof(p), 1, other);
    f.Begin(); Require(f.RenderStatus(b) != VA_STATUS_SUCCESS, "cross-context buffer rejected");
    Require(EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS && !f.mock.opens, "foreign buffer never opens hardware");
  }
  for (bool missing_data : {false, true}) {
    Fixture f; f.Begin(); auto p = Picture(); auto s = Slice(); auto bytes = SliceBytes(0);
    f.Render(f.Buffer(VAPictureParameterBufferType, &p, sizeof(p)));
    if (missing_data) f.Render(f.Buffer(VASliceParameterBufferType, &s, sizeof(s)));
    else f.Render(f.Buffer(VASliceDataBufferType, bytes.data(), bytes.size()));
    Require(EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS && !f.mock.opens, "unpaired parameters/data rejected");
  }
}
void UnsupportedPictureAndSlicePoison() {
  for (unsigned variant = 0; variant < 9; ++variant) {
    Fixture f; f.Begin(); auto p = Picture(); auto s = Slice(); auto data = SliceBytes(0);
    if (variant == 0) p.picture_coding_extension.bits.picture_structure = 1;
    if (variant == 1) p.picture_coding_extension.bits.progressive_frame = 0;
    if (variant == 2) p.picture_coding_extension.bits.frame_pred_frame_dct = 0;
    if (variant == 3) p.picture_coding_extension.bits.repeat_first_field = 1;
    if (variant == 4) p.picture_coding_extension.bits.top_field_first = 1;
    if (variant == 5) s.slice_data_flag = VA_SLICE_DATA_FLAG_BEGIN;
    if (variant == 6) s.slice_data_offset = UINT_MAX;
    if (variant == 7) s.slice_data_size = data.size()+1;
    if (variant == 8) s.macroblock_offset = 37;
    auto a = f.Buffer(VAPictureParameterBufferType, &p, sizeof(p));
    auto b = f.Buffer(VASliceParameterBufferType, &s, sizeof(s));
    auto c = f.Buffer(VASliceDataBufferType, data.data(), data.size());
    VABufferID ids[] = {a,b,c};
    (void)RenderPicture(&f.context, f.id, ids, 3);
    Require(EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS && !f.mock.opens && f.mock.inputs.empty(),
            "unsupported field/interlaced/fragmented/range syntax fails before hardware");
    f.Submit(1);
  }
}
void ReferenceValidation() {
  for (unsigned variant = 0; variant < 8; ++variant) {
    Fixture f; f.Submit(1); f.Submit(2, 2, 1);
    const auto before = f.mock.inputs.size(); const auto next = f.decode->next_timestamp;
    auto p = Picture(16, 32, 2, 2);
    if (variant == 0) p.forward_reference_picture = VA_INVALID_SURFACE;
    if (variant == 1) p.forward_reference_picture = 99;
    if (variant == 2) p.forward_reference_picture = 1; // Retained but no longer newest.
    if (variant == 3) p.forward_reference_picture = 3; // Target alias.
    if (variant == 4) f.driver.surfaces.at(2)->expected_timestamp = 0;
    if (variant == 5) f.decode->surface_timestamps.erase(f.driver.surfaces.at(2).get());
    if (variant == 6) f.driver.surfaces.at(2)->backing_owner = 1;
    if (variant == 7) {
      f.driver.surfaces.at(2)->expected_timestamp = 0;
      f.decode->surface_timestamps[f.driver.surfaces.at(2).get()] = 0;
    }
    f.Begin(3); auto s = Slice(); auto bytes = SliceBytes(0);
    VABufferID ids[] = {f.Buffer(VAPictureParameterBufferType, &p, sizeof(p)),
      f.Buffer(VASliceParameterBufferType, &s, sizeof(s)),
      f.Buffer(VASliceDataBufferType, bytes.data(), bytes.size())};
    (void)RenderPicture(&f.context, f.id, ids, 3);
    Require(EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS, "unknown/stale/wrong-anchor/target reference rejected");
    Require(f.mock.inputs.size() == before && f.decode->next_timestamp == next, "invalid references commit no input/token");
  }
  {
    Fixture f; f.Submit(1); f.Submit(2, 2, 1);
    Require(!f.driver.surfaces.at(1)->ready && !f.driver.surfaces.at(2)->ready, "references deliberately await output");
    f.Submit(3, 3, 1, 2);
    const auto *b = f.decode->mpeg2_replay.Find(3*kTimestampStep);
    Require(b && b->forward == kTimestampStep && b->backward == 2*kTimestampStep,
            "valid unready references resolve to immutable accepted tokens");
  }
  {
    Fixture f; f.Submit(1);
    VAContextID foreign = VA_INVALID_ID;
    Require(CreateContext(&f.context, f.config, 16, 32, VA_PROGRESSIVE, nullptr, 0, &foreign) == VA_STATUS_SUCCESS,
            "independent reference owner context");
    // Seed only the foreign ownership map, without opening a second physical
    // decoder. A numeric timestamp collision is not ownership by this context.
    auto other = f.driver.contexts.at(foreign);
    f.driver.surfaces.at(4)->expected_timestamp = kTimestampStep;
    other->surface_timestamps[f.driver.surfaces.at(4).get()] = kTimestampStep;
    const auto count = f.mock.inputs.size();
    f.Begin(3); auto p = Picture(16,32,2,4);
    auto b = f.Buffer(VAPictureParameterBufferType, &p, sizeof(p));
    Require(f.RenderStatus(b) != VA_STATUS_SUCCESS &&
            EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS,
            "cross-context reference cannot borrow a colliding input token");
    Require(f.mock.inputs.size() == count, "foreign reference does not transmit");
  }
}

void LatePoisonAndReferenceSnapshot() {
  {
    Fixture f; auto original = Custom(1);
    f.Begin(); f.Parameters(Picture(), &original); f.End();
    const auto committed = f.decode->mpeg2_matrices.lists;
    const auto next = f.decode->next_timestamp;
    auto replacement = Custom(19);
    f.Begin(2); f.Parameters(Picture(), &replacement);
    VABufferID nonexistent = VA_INVALID_ID;
    Require(RenderPicture(&f.context, f.id, &nonexistent, 1) != VA_STATUS_SUCCESS,
            "late invalid ID poisons already valid pending parameters");
    auto good = f.Buffer(VAIQMatrixBufferType, &replacement, sizeof(replacement));
    Require(f.RenderStatus(good) != VA_STATUS_SUCCESS &&
            EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS,
            "late valid data cannot erase a poisoned transaction");
    Require(f.decode->mpeg2_matrices.lists == committed && f.decode->next_timestamp == next &&
            f.mock.inputs.size() == 1, "late failure leaves committed state unchanged");
    f.Begin(2); f.Parameters(Picture()); f.End();
    CheckMatrices(f.mock.inputs.back().bytes, original);
  }
  {
    Fixture f; f.Submit(1); f.Submit(2,2,1);
    f.Begin(3); f.Parameters(Picture(16,32,3,1,2));
    // Model a public ID's mutable binding changing after Render. The accepted
    // picture already owns reference tokens, not pointers into this map.
    f.driver.surfaces.at(1)->expected_timestamp = 999*kTimestampStep;
    f.decode->surface_timestamps[f.driver.surfaces.at(1).get()] = 999*kTimestampStep;
    f.End();
    const auto *b = f.decode->mpeg2_replay.Find(3*kTimestampStep);
    Require(b && b->forward == kTimestampStep && b->backward == 2*kTimestampStep,
            "Render snapshots immutable references before a public ID can be reused");
  }
}
void VisibleAndAllocatedGeometry() {
  Fixture f(VAProfileMPEG2Main, 1920, 1088);
  f.Begin(); f.Parameters(Picture(1920,1080)); f.End();
  Require(f.mock.format.width == 1920 && f.mock.format.height == 1080 &&
          f.decode->stream_height == 1080 && f.decode->height == 1088,
          "visible picture geometry is distinct from aligned VA allocation");
  f.mock.Queue(kTimestampStep, 1920, 1080); f.Receive();
  Require(f.driver.surfaces.at(1)->ready && !f.driver.surfaces.at(1)->failed,
          "actual output accepts visible 1080 rows in 1088 allocation");
  const auto before = f.mock.inputs.size(); f.Begin(2); f.Parameters(Picture(1920,1088));
  Require(EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS && f.mock.inputs.size() == before,
          "mid-context visible geometry cannot silently change");
}
void SealedReplayOriginalBytes() {
  Fixture f; const auto original = Custom(9);
  f.Begin(1); f.Parameters(Picture(), &original); f.End();
  f.Submit(2, 2, 1);
  const auto first = f.mock.inputs;
  Require(SealDecodeBatch(&f.driver, f.decode.get()) == VA_STATUS_SUCCESS && f.mock.flushes == 1,
          "actual finite-batch seal requests EOS");
  f.Submit(3, 3, 1, 2);
  Require(f.mock.inputs.size() == 2, "new B waits behind the sealed batch");
  const auto *queued = f.decode->mpeg2_replay.Find(3*kTimestampStep);
  Require(queued != nullptr, "queued immutable B exists"); const auto saved_b = queued->bytes;
  f.mock.Queue(kTimestampStep,16,32); f.mock.Queue(2*kTimestampStep,16,32); f.mock.Eos();
  f.Receive();
  Require(f.decode->mpeg2_replay.NeedsRestart() && f.mock.releases == 3, "real output leases and EOS are complete");
  Require(PumpDecodeInput(&f.driver, f.decode.get()) == VA_STATUS_SUCCESS,
          "actual transport performs full close/reopen and replay");
  Require(f.mock.opens == 2 && f.mock.closes == 1 && f.mock.stops == 1 && f.mock.inputs.size() == 5,
          "no channel-only reset or lost queued B");
  for (size_t n = 0; n < first.size(); ++n)
    Require(f.mock.inputs[n+2].timestamp == first[n].timestamp && f.mock.inputs[n+2].bytes == first[n].bytes,
            "original I/P timestamps and full access-unit bytes survive replay");
  Require(f.mock.inputs.back().bytes == saved_b && f.mock.inputs.back().timestamp == 3*kTimestampStep,
          "queued B also preserves original matrix/timestamp/slices");
  CheckMatrices(f.mock.inputs.back().bytes, original);
  for (uint64_t t : {kTimestampStep, 2*kTimestampStep, 3*kTimestampStep}) f.mock.Queue(t,16,32);
  f.Receive();
  Require(f.decode->pending.empty() && f.driver.surfaces.at(3)->ready,
          "duplicates do not substitute for pending B completion");
  Require(f.decode->replay.cached_pictures() == 0, "original H264 helper stays unused");
}
void BackpressureAndSimpleBRejection() {
  {
    Fixture f; f.mock.capacity = false; auto matrix = Custom(61);
    f.Begin(); f.Parameters(Picture(), &matrix); f.End();
    Require(f.mock.inputs.empty() && f.decode->next_timestamp == 2*kTimestampStep &&
            f.decode->mpeg2_replay.cached_pictures() == 1,
            "accepted backpressured picture owns token before transport capacity is available");
    const auto *unit = f.decode->mpeg2_replay.Find(kTimestampStep);
    Require(unit != nullptr, "backpressured bytes retained"); const auto bytes = unit->bytes;
    CheckMatrices(bytes, matrix);
    f.mock.capacity = true;
    Require(PumpDecodeInput(&f.driver, f.decode.get()) == VA_STATUS_SUCCESS &&
            f.mock.inputs.size() == 1 && f.mock.inputs.front().bytes == bytes,
            "later admission uses the same owned AU exactly once");
  }
  {
    Fixture f(VAProfileMPEG2Simple); f.Submit(1); f.Submit(2,2,1);
    const auto next = f.decode->next_timestamp;
    f.Begin(3); f.Parameters(Picture(16,32,3,1,2));
    Require(EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS &&
            f.mock.inputs.size() == 2 && f.decode->next_timestamp == next,
            "Simple profile rejects B pictures even when their references are valid");
  }
}
}

int main() {
  const std::pair<const char *, void (*)()> tests[] = {
    {"profiles and MPEG2 input format", ProfilesAndInputFormat},
    {"owned buffers and all public call orders", OwnedBuffersAndOrdering},
    {"slice arrays, groups and offsets", SliceGroupsAndOffsets},
    {"matrix commit and failure atomicity", MatrixCommitAndFailureAtomicity},
    {"malformed buffers poison and recover", MalformedBuffersPoison},
    {"bad calls, context and pair ownership", BadCallsContextsAndMissingPairs},
    {"unsupported picture/slice syntax", UnsupportedPictureAndSlicePoison},
    {"reference identity and unready anchors", ReferenceValidation},
    {"late poison and immutable reference snapshot", LatePoisonAndReferenceSnapshot},
    {"visible versus allocated geometry", VisibleAndAllocatedGeometry},
    {"actual EOS reopen and original replay", SealedReplayOriginalBytes},
    {"backpressure and Simple profile restriction", BackpressureAndSimpleBRejection},
  };
  unsigned failed = 0;
  for (const auto &test : tests) {
    try { test.second(); std::printf("%s: PASS\n", test.first); }
    catch (const std::exception &error) { ++failed; std::fprintf(stderr, "%s: FAIL: %s\n", test.first, error.what()); }
  }
  std::printf("%zu groups, %u checks, %u failures\n", sizeof(tests)/sizeof(tests[0]), checks, failed);
  return failed ? 1 : 0;
}
