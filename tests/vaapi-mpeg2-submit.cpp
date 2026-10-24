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
  uint32_t flags = 0, number = 0, session = 0, metadata = 0;
};
struct DecoderMock {
  bool capacity = true, opened = false, leased = false;
  bool release_error = false, close_busy = false;
  unsigned input_calls = 0, fail_input_call = 0, complete_after_input = 0;
  uint64_t complete_token = 0;
  unsigned opens = 0, closes = 0, starts = 0, stops = 0, flushes = 0, releases = 0;
  BC_INPUT_FORMAT format = {};
  std::vector<InputRecord> inputs;
  std::deque<MockOutput> outputs;
  void Queue(uint64_t timestamp, unsigned width, unsigned height) {
    outputs.push_back({timestamp, width, height, false,
                      std::vector<uint8_t>(static_cast<size_t>(width)*height*2, 128)});
  }
  void Eos() { outputs.push_back({0, 0, 0, true, {}}); }
  void Field(uint64_t timestamp, bool bottom, bool bottom_first,
             unsigned number = 90) {
    Queue(timestamp, 16, 32);
    auto &out = outputs.back();
    out.pixels.resize(16 * 32);
    out.flags = VDEC_FLAG_INTERLACED_SRC |
        (bottom ? VDEC_FLAG_BOTTOMFIELD : VDEC_FLAG_TOPFIELD) |
        (bottom_first ? VDEC_FLAG_BOTTOM_FIRST : 0);
    out.number = number;
    out.session = 7;
    out.metadata = 123;
  }
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
extern "C" BC_STATUS DtsDupOutputPollFd(HANDLE, int *fd) {
  if (fd) *fd = -1;
  return BC_STS_NOT_IMPL;
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
extern "C" BC_STATUS DtsSetScaleParams(HANDLE, BC_SCALING_PARAMS *) {
  Require(false, "ordinary MPEG-2 never requests card scaling");
  return BC_STS_ERROR;
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
  CheckDevice(device);
  if (active->close_busy) return BC_STS_BUSY;
  ++active->closes; active->opened = false; return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsGetDriverStatus(HANDLE device, BC_DTS_STATUS *status) {
  CheckDevice(device); *status = {}; status->ReadyListCount = active->outputs.size(); return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsIsEndOfStream(HANDLE device, uint8_t *eos) {
  CheckDevice(device); Require(eos, "EOS query output"); *eos = 0; return BC_STS_SUCCESS;
}
extern "C" uint32_t DtsTxFreeSize(HANDLE device) {
  CheckDevice(device); return active->capacity ? 1024*1024 : 0;
}
extern "C" BC_STATUS DtsProcInput(HANDLE device, uint8_t *bytes, uint32_t size,
                                  uint64_t timestamp, BOOL encrypted) {
  CheckDevice(device);
  Require(bytes && size && timestamp && !encrypted, "complete owned timestamped input");
  if (++active->input_calls == active->fail_input_call) return BC_STS_ERROR;
  active->inputs.push_back({timestamp, std::vector<uint8_t>(bytes, bytes+size)});
  if (active->input_calls == active->complete_after_input)
    active->Queue(active->complete_token, active->format.width, active->format.height);
  return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsProcOutputNoCopy(HANDLE device, uint32_t, BC_DTS_PROC_OUT *output) {
  CheckDevice(device); Require(!active->leased, "one output lease at a time");
  if (active->outputs.empty()) return BC_STS_NO_DATA;
  auto &next = active->outputs.front();
  *output = {}; output->PoutFlags = BC_POUT_FLAGS_PIB_VALID;
  output->PicInfo.timeStamp = next.timestamp;
  output->PicInfo.width = next.width; output->PicInfo.height = next.height;
  output->PicInfo.flags = next.eos ? VDEC_FLAG_EOS : next.flags;
  output->PicInfo.picture_number = next.number;
  output->PicInfo.sess_num = next.session;
  output->PicInfo.picture_meta_payload = next.metadata;
  output->b422Mode = OUTPUT_MODE422_YUY2;
  output->Ybuff = next.pixels.data(); output->YBuffDoneSz = next.pixels.size()/4;
  active->leased = true; return BC_STS_SUCCESS;
}
extern "C" BC_STATUS DtsReleaseOutputBuffs(HANDLE device, PVOID, BOOL change) {
  CheckDevice(device); Require(active->leased && !change, "release exactly the returned picture lease");
  if (active->release_error) return BC_STS_ERROR;
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
extern "C" int gbm_bo_get_fd_for_plane(gbm_bo *, int) { abort(); }
extern "C" uint64_t gbm_bo_get_modifier(gbm_bo *) { abort(); }
extern "C" uint32_t gbm_bo_get_stride_for_plane(gbm_bo *, int) { abort(); }
extern "C" uint32_t gbm_bo_get_offset(gbm_bo *, int) { abort(); }
extern "C" int drmGetDevice2(int, uint32_t, drmDevicePtr *) { abort(); }
extern "C" void drmFreeDevice(drmDevicePtr *) { abort(); }
extern "C" int drmPrimeHandleToFD(int, uint32_t, uint32_t, int *) { abort(); }
extern "C" void sws_freeContext(SwsContext *) { abort(); }
extern "C" const int *sws_getCoefficients(int) { abort(); }
extern "C" int sws_setColorspaceDetails(SwsContext *, const int *, int,
    const int *, int, int, int, int) { abort(); }
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
bool SequenceProgressive(const std::vector<uint8_t> &bytes) {
  for (size_t n = 0; n + 6 <= bytes.size(); ++n) {
    if (bytes[n] || bytes[n+1] || bytes[n+2] != 1 ||
        bytes[n+3] != 0xb5 || bytes[n+4] >> 4 != 1) continue;
    size_t bit = (n + 4) * 8 + 4 + 8;
    return ReadBits(bytes, &bit, 1) != 0;
  }
  throw std::runtime_error("submitted I picture has no sequence extension");
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
      Require(f.mock.inputs.empty() && !f.mock.starts && f.decode->next_timestamp == kTimestampStep,
              "malformed input never starts/submits/advances");
      f.Submit(1); Require(f.mock.inputs.size() == 1, "fresh Begin recovers");
    }
  }
  for (bool picture : {false, true}) {
    Fixture f; f.Begin();
    std::vector<uint8_t> data(2*(picture ? sizeof(VAPictureParameterBufferMPEG2) : sizeof(VAIQMatrixBufferMPEG2)));
    auto id = f.Buffer(picture ? VAPictureParameterBufferType : VAIQMatrixBufferType,
                       data.data(), data.size()/2, 2);
    Require(f.RenderStatus(id) != VA_STATUS_SUCCESS, "picture/IQ requires one element");
    Require(EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS && !f.mock.starts, "bad element count cannot submit");
  }
  {
    Fixture f; auto p = Picture(); auto b = f.Buffer(VAPictureParameterBufferType, &p, sizeof(p));
    Require(BufferSetNumElements(&f.context, b, 0) == VA_STATUS_SUCCESS, "public buffer resized to zero elements");
    f.Begin(); Require(f.RenderStatus(b) != VA_STATUS_SUCCESS &&
                       EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS && !f.mock.starts,
                       "zero-element metadata is not a valid picture");
  }
  {
    Fixture f; f.Begin(); f.Parameters(Picture());
    uint8_t unknown = 0;
    auto b = f.Buffer(VAImageBufferType, &unknown, sizeof(unknown));
    Require(f.RenderStatus(b) != VA_STATUS_SUCCESS &&
            EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS && !f.mock.starts,
            "unknown decode payload poisons otherwise complete parameters");
  }
}
void BadCallsContextsAndMissingPairs() {
  for (int count : {-1, 1}) {
    Fixture f; f.Begin();
    Require(RenderPicture(&f.context, f.id, nullptr, count) != VA_STATUS_SUCCESS, "invalid public array/count rejected");
    Require(EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS && !f.mock.starts, "invalid call poisons transaction");
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
    Require(f.decode->CloseHardware() == BC_STS_SUCCESS,
            "release the first session before creating a foreign decode context");
    Require(CreateContext(&f.context, f.config, 16, 32, VA_PROGRESSIVE, nullptr, 0, &other) == VA_STATUS_SUCCESS,
            "second public context");
    auto p = Picture(); auto b = f.Buffer(VAPictureParameterBufferType, &p, sizeof(p), 1, other);
    f.Begin(); Require(f.RenderStatus(b) != VA_STATUS_SUCCESS, "cross-context buffer rejected");
    Require(EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS && !f.mock.starts, "foreign buffer never starts decoding");
  }
  for (bool missing_data : {false, true}) {
    Fixture f; f.Begin(); auto p = Picture(); auto s = Slice(); auto bytes = SliceBytes(0);
    f.Render(f.Buffer(VAPictureParameterBufferType, &p, sizeof(p)));
    if (missing_data) f.Render(f.Buffer(VASliceParameterBufferType, &s, sizeof(s)));
    else f.Render(f.Buffer(VASliceDataBufferType, bytes.data(), bytes.size()));
    Require(EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS && !f.mock.starts, "unpaired parameters/data rejected");
  }
}
void UnsupportedPictureAndSlicePoison() {
  for (unsigned variant = 0; variant < 9; ++variant) {
    // A 16-line progressive grid and paired-field grid differ. Display flags
    // remain unsupported there even when they are safe for an equal-row grid.
    Fixture f(VAProfileMPEG2Main, 16, variant == 3 || variant == 4 ? 16 : 32);
    f.Begin(); auto p = Picture(f.width, f.height); auto s = Slice(); auto data = SliceBytes(0);
    if (variant == 0) p.picture_coding_extension.bits.picture_structure = 1;
    if (variant == 1) p.picture_coding_extension.bits.picture_structure = 2;
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
    Require(EndPicture(&f.context, f.id) != VA_STATUS_SUCCESS && !f.mock.starts && f.mock.inputs.empty(),
            "unsupported field/repeat/fragmented/range syntax fails before hardware");
    f.Submit(1);
  }
}
void ProgressiveDisplayFlags() {
  for (unsigned flags = 0; flags < 4; ++flags) {
    Fixture f(VAProfileMPEG2Main, 720, 480);
    const auto iq = Custom(flags);
    for (unsigned kind : {1U, 2U, 3U}) {
      auto picture = Picture(720, 480, kind,
          kind == 1 ? VA_INVALID_SURFACE : 1, kind == 3 ? 2 : VA_INVALID_SURFACE);
      picture.picture_coding_extension.bits.top_field_first = flags & 1;
      picture.picture_coding_extension.bits.repeat_first_field = (flags >> 1) & 1;
      const auto original = picture;
      f.Begin(kind);
      const auto buffer = f.Buffer(VAPictureParameterBufferType, &picture, sizeof(picture));
      f.Render(buffer);
      f.Render(f.Buffer(VAIQMatrixBufferType, &iq, sizeof(iq)));
      const auto slice = Slice(29); const auto data = SliceBytes(29);
      f.Render(f.Buffer(VASliceParameterBufferType, &slice, sizeof(slice)));
      f.Render(f.Buffer(VASliceDataBufferType, data.data(), data.size()));
      Require(!std::memcmp(&f.decode->mpeg2_picture.picture, &original, sizeof(original)),
              "owned pending picture retains original display flags and references");
      f.End();
      Require(!std::memcmp(f.driver.buffers.at(buffer).data.data(), &original, sizeof(original)) &&
              !std::memcmp(&picture, &original, sizeof(original)),
              "public and caller picture buffers are never canonicalized in place");
      Require(f.mock.inputs.size() == kind && f.decode->mpeg2_progressive_sequence,
              "each original I/P/B produces one progressive transport AU");
      const auto *unit = f.decode->mpeg2_replay.Find(kind * kTimestampStep);
      Require(unit && unit->forward == (kind == 1 ? 0 : kTimestampStep) &&
              unit->backward == (kind == 3 ? 2 * kTimestampStep : 0),
              "normalization retains original reference identity");
      auto canonical = original;
      canonical.picture_coding_extension.bits.top_field_first = 0;
      canonical.picture_coding_extension.bits.repeat_first_field = 0;
      const crystalhd_mpeg2::SequenceOptions sequence = {
          VAProfileMPEG2Main, 4, true, false, 1, 5, 0, 0, 0x3ffff, 112};
      auto matrices = crystalhd_mpeg2::DefaultQuantMatrices(), next = matrices;
      std::vector<uint8_t> expected;
      Require(crystalhd_mpeg2::Assemble(canonical, &iq, matrices,
          {{slice, data.data(), data.size()}}, sequence, {0, kind == 1}, &expected, &next),
          "independent canonical request assembles");
      Require(unit->bytes == expected && f.mock.inputs.back().bytes == expected,
              "public transport and replay retain exact canonical pixels request");
      CheckMatrices(unit->bytes, iq); CheckTail(unit->bytes, data);
      if (kind == 1) Require(SequenceProgressive(unit->bytes), "normalized sequence is progressive");
    }
  }
  for (unsigned variant = 0; variant < 9; ++variant) {
    const unsigned height = variant == 5 ? 360 : variant == 6 ? 720 : 480;
    Fixture f(variant == 0 ? VAProfileMPEG2Simple : VAProfileMPEG2Main, 720, height);
    f.Submit(1);
    const auto before = f.mock.inputs.size(), cached = f.decode->mpeg2_replay.cached_pictures();
    const auto timestamp = f.decode->next_timestamp;
    const auto matrices = f.decode->mpeg2_matrices.lists;
    const auto opens = f.mock.opens, starts = f.mock.starts;
    auto picture = Picture(720, height, 2, 1);
    auto &p = picture.picture_coding_extension.bits;
    p.top_field_first = p.repeat_first_field = 1;
    auto slice = Slice(); auto data = SliceBytes(0); auto iq = Custom(17);
    if (variant == 1) p.picture_structure = 1;
    if (variant == 2) p.picture_structure = 2;
    if (variant == 3) p.frame_pred_frame_dct = 0;
    if (variant == 4) p.progressive_frame = 0;
    if (variant == 7) { slice = Slice(30); data = SliceBytes(30); }
    if (variant == 8) iq.intra_quantiser_matrix[1] = 0;
    f.Begin(2);
    f.Render(f.Buffer(VAPictureParameterBufferType, &picture, sizeof(picture)));
    f.Render(f.Buffer(VAIQMatrixBufferType, &iq, sizeof(iq)));
    f.Render(f.Buffer(VASliceParameterBufferType, &slice, sizeof(slice)));
    f.Render(f.Buffer(VASliceDataBufferType, data.data(), data.size()));
    Require(EndPicture(&f.context, f.id) == VA_STATUS_ERROR_INVALID_PARAMETER &&
            f.mock.inputs.size() == before && f.mock.opens == opens && f.mock.starts == starts &&
            f.decode->next_timestamp == timestamp && f.decode->mpeg2_matrices.lists == matrices &&
            f.decode->mpeg2_replay.cached_pictures() == cached &&
            f.decode->have_mpeg2_sequence && f.decode->mpeg2_progressive_sequence,
            "out-of-scope repeat syntax fails atomically before transport or state mutation");
  }
}
void InterlacedSequenceAdmission() {
  for (unsigned top_first : {0U, 1U}) {
    Fixture f(VAProfileMPEG2Main, 640, 368);
    // The interlaced 360-line visible image has a 24-row coded grid. The
    // decoder context/allocation may retain its usual 16-line padding.
    auto picture = Picture(640, 360);
    picture.picture_coding_extension.bits.progressive_frame = 0;
    picture.picture_coding_extension.bits.frame_pred_frame_dct = 0;
    picture.picture_coding_extension.bits.top_field_first = top_first;
    const auto slice = Slice(23); const auto data = SliceBytes(23);
    f.Begin(1);
    f.Render(f.Buffer(VAPictureParameterBufferType, &picture, sizeof(picture)));
    f.Render(f.Buffer(VASliceParameterBufferType, &slice, sizeof(slice)));
    f.Render(f.Buffer(VASliceDataBufferType, data.data(), data.size()));
    f.End();
    Require(f.mock.inputs.size() == 1 && f.decode->have_mpeg2_sequence &&
            !f.decode->mpeg2_progressive_sequence &&
            !SequenceProgressive(f.mock.inputs.front().bytes),
            "public Main TFF/BFF submit emits an interlaced sequence");
    CheckTail(f.mock.inputs.front().bytes, data);
    for (unsigned type : {2U, 3U}) {
      picture.picture_coding_type = type;
      picture.forward_reference_picture = 1;
      picture.backward_reference_picture = type == 3 ? 2 : VA_INVALID_SURFACE;
      f.Begin(type); f.Parameters(picture); f.End();
    }
    Require(f.mock.inputs.size() == 3 && f.decode->mpeg2_replay.cached_pictures() == 3,
            "public interlaced I/P/B uses the existing MPEG-2 reference transport");
  }
  for (unsigned variant = 0; variant < 3; ++variant) {
    Fixture f(variant == 0 ? VAProfileMPEG2Simple : VAProfileMPEG2Main,
              16, variant == 2 ? 368 : 32);
    auto picture = Picture(16, variant == 2 ? 362 : 32);
    picture.picture_coding_extension.bits.progressive_frame = 0;
    if (variant == 1) picture.picture_coding_extension.bits.repeat_first_field = 1;
    f.Begin(); f.Parameters(picture);
    Require(EndPicture(&f.context, f.id) == VA_STATUS_ERROR_INVALID_PARAMETER &&
            f.mock.opens == 1 && !f.mock.starts && f.mock.format.width == 0 &&
            !f.decode->have_mpeg2_sequence &&
            f.decode->mpeg2_replay.cached_pictures() == 0,
            "Simple interlace, RFF and incomplete chroma rows fail before decoder configuration");
    f.Submit(2);
    Require(f.decode->have_mpeg2_sequence && f.decode->mpeg2_progressive_sequence &&
            SequenceProgressive(f.mock.inputs.front().bytes),
            "a rejected first interlaced request does not latch sequence mode");
  }
}
void SequenceModeLifetime() {
  for (bool display_flags : {false, true})
  for (bool progressive : {false, true}) {
    // Different row grids still make a later progressive frame ambiguous.
    const unsigned height = !progressive && !display_flags ? 360 : 32;
    Fixture f(VAProfileMPEG2Main, 16, (height + 15U) & ~15U);
    f.mock.capacity = false;
    auto picture = Picture(16, height);
    picture.picture_coding_extension.bits.progressive_frame = progressive;
    picture.picture_coding_extension.bits.top_field_first = progressive && display_flags;
    picture.picture_coding_extension.bits.repeat_first_field = progressive && display_flags;
    f.Begin(1); f.Parameters(picture); f.End();
    Require(f.decode->have_mpeg2_sequence &&
            f.decode->mpeg2_progressive_sequence == progressive &&
            f.mock.inputs.empty(),
            "accepted backpressured AU freezes sequence mode before transport");
    const auto *accepted = f.decode->mpeg2_replay.Find(kTimestampStep);
    Require(accepted && SequenceProgressive(accepted->bytes) == progressive,
            "frozen sequence bit matches the owned first AU");
    const auto token = f.decode->next_timestamp;
    const auto count = f.decode->mpeg2_replay.cached_pictures();
    picture.picture_coding_extension.bits.progressive_frame = !progressive;
    picture.picture_coding_extension.bits.top_field_first = !progressive && display_flags;
    picture.picture_coding_extension.bits.repeat_first_field = !progressive && display_flags;
    for (bool close_hardware : {false, true}) {
      if (close_hardware)
        Require(f.decode->CloseHardware() == BC_STS_SUCCESS,
                "transport-only close succeeds without a lease");
      f.Begin(2); f.Parameters(picture);
      Require(EndPicture(&f.context, f.id) == VA_STATUS_ERROR_INVALID_PARAMETER &&
              f.decode->have_mpeg2_sequence &&
              f.decode->mpeg2_progressive_sequence == progressive &&
              f.decode->next_timestamp == token &&
              f.decode->mpeg2_replay.cached_pictures() == count &&
              f.mock.inputs.empty() && f.mock.opens == 1,
              "mode changes fail atomically even after transport-only close");
    }
    Require(f.decode->Close() == BC_STS_SUCCESS && !f.decode->have_mpeg2_sequence,
            "logical context reset clears the sequence contract");
    f.Begin(3); f.Parameters(picture); f.End();
    Require(f.decode->have_mpeg2_sequence &&
            f.decode->mpeg2_progressive_sequence != progressive &&
            f.decode->mpeg2_replay.cached_pictures() == 1,
            "a fresh logical epoch may accept the other sequence mode");
  }
}
void MixedFrameSequenceLifetime() {
  Fixture f;
  f.mock.capacity = false;
  auto picture = Picture();
  picture.picture_coding_extension.bits.progressive_frame = 0;
  f.Begin(1); f.Parameters(picture); f.End();
  const uint64_t first = kTimestampStep;
  picture = Picture(16, 32, 2, 1);
  const auto original = picture;
  f.Begin(2); f.Parameters(picture); f.End();
  Require(!std::memcmp(&picture, &original, sizeof(picture)),
          "mixed frame leaves caller picture flags unchanged");
  picture = Picture(16, 32, 3, 1, 2);
  picture.picture_coding_extension.bits.progressive_frame = 0;
  f.Begin(3); f.Parameters(picture); f.End();
  picture = Picture();
  f.Begin(4); f.Parameters(picture); f.End();
  Require(f.decode->have_mpeg2_sequence && !f.decode->mpeg2_progressive_sequence &&
              f.decode->mpeg2_replay.cached_pictures() == 4 && f.mock.inputs.empty(),
          "interlaced/progressive frame flags never change the frozen sequence");
  Require(!SequenceProgressive(f.decode->mpeg2_replay.Find(first)->bytes) &&
              !SequenceProgressive(f.decode->mpeg2_replay.Find(4 * first)->bytes),
          "later progressive I picture keeps the original interlaced sequence header");
  for (const auto &entry : f.decode->mpeg2_auxiliary.syntax)
    Require(!entry.second.sequence.progressive_sequence && entry.second.eligible,
            "mixed sequence snapshot retains its mode for owned field completion");
  Require(f.decode->CloseHardware() == BC_STS_SUCCESS,
          "transport-only close retains the logical mixed sequence");
  f.Surface(5);
  picture.picture_coding_extension.bits.progressive_frame = 0;
  f.Begin(5); f.Parameters(picture); f.End();
  Require(!f.decode->mpeg2_progressive_sequence &&
              f.decode->mpeg2_replay.cached_pictures() == 5,
          "hardware reopen cannot replace the frozen sequence with last-picture flags");
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
    Require(f.decode->CloseHardware() == BC_STS_SUCCESS,
            "retire the first hardware session before assigning foreign reference ownership");
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

void PrivateMpeg2Completion() {
  Fixture f;
  auto matrix = Custom(19);
  f.Begin(1); f.Parameters(Picture(), &matrix); f.End();
  f.Submit(2, 2, 1);
  const auto first = f.driver.surfaces.at(1);
  const auto ordinal = f.decode->next_submission_ordinal;
  const auto pending = f.decode->pending;
  const auto real = f.mock.inputs;
  // Staging a later picture must not replace the last actually sent IQ/syntax.
  f.Begin(3); auto future = Custom(73); f.Parameters(Picture(16,32,3,1,2), &future);
  Require(TryMpeg2Auxiliary(&f.driver, f.decode.get(), first) == VA_STATUS_SUCCESS &&
              f.mock.inputs.size() == 4,
          "held previous anchor gets two private backward-copy B pictures");
  Require(f.decode->pending == pending && f.decode->next_submission_ordinal == ordinal &&
              f.decode->next_timestamp == 5*kTimestampStep && !first->ready &&
              f.decode->mpeg2_replay.cached_pictures() == 2,
          "auxiliaries own tokens but no public picture, ordinal or replay entry");
  CheckMatrices(f.mock.inputs[2].bytes, matrix);
  Require(f.mock.inputs[2].bytes == f.mock.inputs[3].bytes &&
              f.mock.inputs[0].bytes == real[0].bytes && f.mock.inputs[1].bytes == real[1].bytes,
          "generated copies preserve current IQ and all original AU bytes");
  Require(TryMpeg2Auxiliary(&f.driver, f.decode.get(), first) == VA_STATUS_SUCCESS &&
              f.mock.inputs.size() == 4, "one bounded attempt per last real input");
  f.mock.Queue(kTimestampStep,16,32); f.mock.Queue(2*kTimestampStep,16,32); f.Receive();
  Require(f.decode->pending.empty() && f.decode->mpeg2_auxiliary.PendingCount() == 2 &&
              HasAutonomousDecodeWork(f.decode), "public completion leaves private output debt serviced");
  f.mock.Queue(3*kTimestampStep,16,32); f.mock.Queue(4*kTimestampStep,16,32); f.Receive();
  Require(f.decode->mpeg2_auxiliary.PendingCount() == 0 && f.decode->pending.empty() &&
              f.decode->decoded_frames.size() == 2, "auxiliary output is released without a public surface");
  Require(SealDecodeBatch(&f.driver, f.decode.get()) == VA_STATUS_SUCCESS && f.mock.flushes == 0,
          "idle private completion is not EOF");
  f.decode->closing = true;
  Require(SealDecodeBatch(&f.driver, f.decode.get()) == VA_STATUS_SUCCESS && f.mock.flushes == 1,
          "explicit close seals an auxiliary-only tail");
  f.mock.Eos(); f.Receive();
  Require(!f.decode->mpeg2_auxiliary.needs_eos, "only actual EOS retires the private stream tail");
}

void PrivateMpeg2BCompletion() {
  Fixture f;
  f.Submit(1); f.Submit(2,2,1); f.Submit(3,3,1,2);
  const auto original = f.mock.inputs.back();
  const auto b = f.driver.surfaces.at(3);
  Require(TryMpeg2Auxiliary(&f.driver, f.decode.get(), b) == VA_STATUS_SUCCESS &&
              f.mock.inputs.size() == 4 && f.mock.inputs.back().bytes == original.bytes &&
              f.mock.inputs.back().timestamp == 4*kTimestampStep,
          "latest B receives an exact nonreference clone with its own token");
  f.mock.Queue(3*kTimestampStep,16,32); f.Receive();
  Require(b->ready && !f.decode->mpeg2_replay.Find(3*kTimestampStep) &&
              f.decode->mpeg2_auxiliary.PendingCount() == 1,
          "original B pruning cannot forget its accepted auxiliary debt");
  f.mock.Queue(4*kTimestampStep,16,32); f.Receive();
  Require(f.decode->mpeg2_auxiliary.PendingCount() == 0 && f.mock.releases == 2,
          "private clone retires after output release");
  f.mock.Queue(4*kTimestampStep,16,32);
  Require(ReceiveAvailable(&f.driver, f.decode.get()) == VA_STATUS_ERROR_DECODING_ERROR &&
              !f.mock.leased, "duplicate auxiliary output is not an ignored unknown timestamp");
}

void SubmitInterlaced(Fixture &f, unsigned target, unsigned kind,
                      bool bottom_first, bool progressive = false) {
  auto p = Picture(16, 32, kind, kind == 1 ? VA_INVALID_SURFACE : 1,
                   kind == 3 ? 2 : VA_INVALID_SURFACE);
  p.picture_coding_extension.bits.progressive_frame = progressive;
  p.picture_coding_extension.bits.top_field_first = !bottom_first;
  f.Begin(target); f.Parameters(p); f.End();
}

void PrivateMpeg2FieldCompletion() {
  for (bool bottom_first : {false, true}) {
    Fixture f;
    SubmitInterlaced(f, 1, 1, bottom_first);
    SubmitInterlaced(f, 2, 2, bottom_first);
    const auto originals = f.mock.inputs;
    Require(TryMpeg2Auxiliary(&f.driver, f.decode.get(), f.driver.surfaces.at(1)) ==
                VA_STATUS_SUCCESS && f.mock.inputs.size() == 4,
            "interlaced anchors receive bounded private lookahead without EOS");
    Require(f.mock.inputs[0].bytes == originals[0].bytes &&
                f.mock.inputs[1].bytes == originals[1].bytes &&
                f.decode->mpeg2_replay.cached_pictures() == 2 &&
                f.decode->next_submission_ordinal == 3,
            "private field completion never changes public originals or ordinals");
    for (unsigned token : {1U, 2U}) {
      f.mock.Field(token*kTimestampStep, bottom_first, bottom_first, token);
      f.mock.Field(token*kTimestampStep, !bottom_first, bottom_first, token);
    }
    f.Receive();
    Require(f.decode->pending.empty() && f.decode->mpeg2_auxiliary.PendingCount() == 2,
            "complete public field pairs do not retire private output debt");
    // Generated copies use explicit TFF, independently of their source order.
    f.mock.Field(3*kTimestampStep, false, false, 30); f.Receive();
    Require(f.decode->mpeg2_auxiliary.PendingCount() == 2,
            "first private field release cannot retire the whole picture");
    f.mock.Field(4*kTimestampStep, false, false, 40); f.Receive();
    f.mock.Field(4*kTimestampStep, true, false, 40); f.Receive();
    Require(f.decode->mpeg2_auxiliary.PendingCount() == 1,
            "interleaved auxiliary identities retire independently after both fields");
    f.mock.Field(3*kTimestampStep, true, false, 30); f.Receive();
    Require(f.decode->mpeg2_auxiliary.PendingCount() == 0 &&
                f.mock.releases == 8 && f.decode->decoded_frames.size() == 2 &&
                !f.decode->h264_first_field.valid,
            "auxiliary fields make no public picture or shared first-field latch");
    f.decode->closing = true;
    Require(SealDecodeBatch(&f.driver, f.decode.get()) == VA_STATUS_SUCCESS &&
                f.mock.flushes == 1, "only terminal close flushes private field work");
    f.mock.Eos(); f.Receive();
    Require(!f.decode->mpeg2_auxiliary.needs_eos,
            "actual EOS completes fully released private field debt");
  }
  for (bool progressive_b : {false, true}) {
    Fixture f;
    SubmitInterlaced(f, 1, 1, true);
    SubmitInterlaced(f, 2, 2, true);
    SubmitInterlaced(f, 3, 3, true, progressive_b);
    const auto original = f.mock.inputs.back();
    Require(TryMpeg2Auxiliary(&f.driver, f.decode.get(), f.driver.surfaces.at(3)) ==
                VA_STATUS_SUCCESS && f.mock.inputs.size() == 5 &&
                f.mock.inputs.back().bytes == original.bytes,
            "mixed/interlaced B lookahead clones the exact accepted AU");
    if (progressive_b) {
      f.mock.Queue(4*kTimestampStep, 16, 32); f.Receive();
      Require(f.decode->mpeg2_auxiliary.PendingCount() == 1,
              "progressive B in an interlaced sequence still owns a second clone");
      f.mock.Queue(5*kTimestampStep, 16, 32); f.Receive();
    } else {
      f.mock.Field(4*kTimestampStep, true, true); f.Receive();
      Require(f.decode->mpeg2_auxiliary.PendingCount() == 2,
              "BFF cloned first field retains its own debt");
      f.mock.Field(4*kTimestampStep, false, true); f.Receive();
      Require(f.decode->mpeg2_auxiliary.PendingCount() == 1,
              "first clone retirement preserves the independently accepted second clone");
      f.mock.Field(5*kTimestampStep, true, true);
      f.mock.Field(5*kTimestampStep, false, true); f.Receive();
    }
    Require(f.decode->mpeg2_auxiliary.PendingCount() == 0,
            "a clone follows its picture flag, not the interlaced sequence flag");
  }
}

void PrepareInterlacedBCompletion(Fixture &f) {
  SubmitInterlaced(f, 1, 1, false);
  SubmitInterlaced(f, 2, 2, false);
  SubmitInterlaced(f, 3, 3, false);
  Require(TryMpeg2Auxiliary(&f.driver, f.decode.get(), f.driver.surfaces.at(3)) ==
              VA_STATUS_SUCCESS && f.decode->mpeg2_auxiliary.PendingCount() == 2,
          "prepare two independently owned interlaced auxiliary clones");
  f.mock.Field(5*kTimestampStep,false,false);
  f.mock.Field(5*kTimestampStep,true,false); f.Receive();
  Require(f.decode->mpeg2_auxiliary.PendingCount() == 1,
          "retire the other identity before isolated first-clone failure checks");
}

void PrivateMpeg2FieldFailures() {
  for (unsigned mode = 0; mode < 18; ++mode) {
    Fixture f; PrepareInterlacedBCompletion(f);
    const bool first_released = mode >= 9 && mode != 16;
    if (first_released) {
      f.mock.Field(4*kTimestampStep, false, false); f.Receive();
      Require(f.decode->mpeg2_auxiliary.PendingCount() == 1,
              "failure fixture retains a released first field");
    }
    if (mode == 15) f.mock.Eos();
    else {
      f.mock.Field(4*kTimestampStep, first_released, false);
      auto &out = f.mock.outputs.back();
      switch (mode) {
        case 0: out.flags = 0; out.pixels.resize(16*32*2); break;
        case 1: out.flags &= ~VDEC_FLAG_BOTTOMFIELD; break;
        case 2: out.pixels.resize(16*32*2); break;
        case 3: out.pixels.resize(16*32-4); break;
        case 4: ++out.width; break;
        case 5: out.height -= 4; break;
        case 6: out.flags |= VDEC_FLAG_BOTTOM_FIRST; break;
        case 7: ++f.decode->generation; break;
        case 8: FindMpeg2Auxiliary(f.decode.get(),4*kTimestampStep)->device = &f.driver; break;
        case 9: out.flags = VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_TOPFIELD; break;
        case 10: ++out.number; break;
        case 11: ++out.session; break;
        case 12: ++out.metadata; break;
        case 13: out.flags |= VDEC_FLAG_BOTTOM_FIRST; break;
        case 14: ++out.width; break;
        case 16: case 17: f.mock.release_error = true; break;
      }
    }
    Require(ReceiveAvailable(&f.driver,f.decode.get()) == VA_STATUS_ERROR_DECODING_ERROR &&
                f.decode->mpeg2_auxiliary.PendingCount() == 1 &&
                f.decode->mpeg2_auxiliary.aborted && f.decode->mpeg2_replay.failed(),
            "invalid/partial/release-failed private fields never retire accepted debt");
    Require(FindMpeg2Auxiliary(f.decode.get(),4*kTimestampStep)->fields ==
                (first_released ? 1U : 0U),
            "failed validation or lease release leaves the prior field mask intact");
    Require(f.mock.leased == (mode == 16 || mode == 17),
            "only an actual release failure retains the synthetic output lease");
    // As in the existing release-failure tests, only the mock lease needs
    // manual containment; production ownership remains for consuming close.
    f.mock.release_error = false; f.mock.leased = false; f.mock.outputs.clear();
  }
  {
    Fixture f; PrepareInterlacedBCompletion(f);
    f.mock.Field(4*kTimestampStep,false,false); f.Receive();
    f.mock.close_busy = true;
    Require(f.decode->CloseHardware(0) == BC_STS_BUSY &&
                f.decode->mpeg2_auxiliary.PendingCount() == 1 &&
                FindMpeg2Auxiliary(f.decode.get(),4*kTimestampStep)->fields == 1,
            "non-consuming close preserves partial private-field ownership");
    f.mock.close_busy = false;
    Require(f.decode->CloseHardware(0) == BC_STS_SUCCESS &&
                f.decode->mpeg2_auxiliary.PendingCount() == 0,
            "only consuming close clears partial private-field debt");
  }
  {
    Fixture f; PrepareInterlacedBCompletion(f);
    f.mock.Field(4*kTimestampStep,false,false);
    f.mock.Field(4*kTimestampStep,true,false); f.Receive();
    f.mock.Field(4*kTimestampStep,true,false);
    Require(ReceiveAvailable(&f.driver,f.decode.get()) == VA_STATUS_ERROR_DECODING_ERROR &&
                !f.mock.leased && f.decode->mpeg2_auxiliary.PendingCount() == 0,
            "a third field after complete retirement is an unknown-token failure");
  }
}

void PrivateMpeg2CompletionRejection() {
  for (unsigned mode = 0; mode < 14; ++mode) {
    Fixture f(mode == 0 ? VAProfileMPEG2Simple : VAProfileMPEG2Main);
    f.Submit(1); f.Submit(2,2,1);
    const auto first = f.driver.surfaces.at(1);
    if (mode == 1) f.decode->is_70012 = true;
    if (mode == 2) f.decode->partial_input_timestamp = kTimestampStep;
    if (mode == 3) f.decode->closing = true;
    if (mode == 4) f.mock.capacity = false;
    if (mode == 5) first->ready = true;
    if (mode == 6) ++f.decode->generation;
    if (mode == 7) {
      f.mock.capacity = false;
      f.Submit(3,3,1,2);
      f.mock.capacity = true;
    }
    if (mode == 8) f.decode->mpeg2_replay.Seal();
    if (mode == 9) f.decode->mpeg2_auxiliary.candidate.device = &f.driver;
    if (mode == 10) f.decode->mpeg2_auxiliary.candidate.eligible = false;
    if (mode == 11) f.decode->mpeg2_auxiliary.replayed = true;
    if (mode == 12) f.decode->processing_geometry.enabled = true;
    if (mode == 13)
      for (auto &entry : f.decode->mpeg2_auxiliary.outputs)
        entry.timestamp = 99*kTimestampStep;
    const auto next = f.decode->next_timestamp;
    Require(TryMpeg2Auxiliary(&f.driver, f.decode.get(), first) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 2 && f.decode->next_timestamp == next,
            "ineligible, queued, backpressured or stale demand cannot inject auxiliary bytes");
  }
}

void PrivateMpeg2SyncAndFailures() {
  {
    Fixture f; f.Submit(1); f.Submit(2,2,1);
    Require(SyncSurface2(&f.context,1,0) == VA_STATUS_ERROR_TIMEDOUT &&
                f.mock.inputs.size() == 2, "zero-timeout public sync cannot inject work");
    f.mock.complete_after_input = 4; f.mock.complete_token = kTimestampStep;
    Require(SyncSurface2(&f.context,1,50000000) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 4 && f.mock.flushes == 0 && f.mock.opens == 1,
            "actual public sync advances exact original without EOS/restart");
    Require(f.driver.surfaces.at(1)->frame_timestamp == kTimestampStep &&
                f.decode->next_submission_ordinal == 3 &&
                f.decode->mpeg2_auxiliary.PendingCount() == 2,
            "sync completion preserves original identity and auxiliary debt");
  }
  for (unsigned failure = 1; failure <= 2; ++failure) {
    Fixture f; f.Submit(1); f.Submit(2,2,1);
    f.mock.fail_input_call = f.mock.input_calls + failure;
    Require(TryMpeg2Auxiliary(&f.driver,f.decode.get(),f.driver.surfaces.at(1)) ==
                VA_STATUS_ERROR_DECODING_ERROR && f.decode->ReplayFailed() &&
                f.decode->mpeg2_auxiliary.PendingCount() == failure-1,
            "failed whole/second auxiliary call is terminal and retains accepted debt");
    const auto calls = f.mock.input_calls;
    Require(TryMpeg2Auxiliary(&f.driver,f.decode.get(),f.driver.surfaces.at(1)) ==
                VA_STATUS_SUCCESS && f.mock.input_calls == calls,
            "uncertain input failure never retries auxiliary bytes");
  }
  {
    Fixture f; f.Submit(1); f.Submit(2,2,1);
    Require(TryMpeg2Auxiliary(&f.driver,f.decode.get(),f.driver.surfaces.at(1)) == VA_STATUS_SUCCESS,
            "prepare private output debt");
    Require(SealDecodeBatch(&f.driver,f.decode.get()) == VA_STATUS_SUCCESS,
            "existing real-input EOS fallback remains available");
    f.mock.Eos();
    Require(ReceiveAvailable(&f.driver,f.decode.get()) == VA_STATUS_ERROR_DECODING_ERROR &&
                !f.mock.leased && f.decode->mpeg2_auxiliary.PendingCount() == 2,
            "actual early EOS releases its lease but cannot hide missing auxiliaries");
  }
  {
    Fixture f; f.Submit(1); f.Submit(2,2,1);
    Require(TryMpeg2Auxiliary(&f.driver,f.decode.get(),f.driver.surfaces.at(1)) == VA_STATUS_SUCCESS,
            "prepare release-error case");
    f.mock.Queue(3*kTimestampStep,16,32); f.mock.release_error = true;
    Require(ReceiveAvailable(&f.driver,f.decode.get()) == VA_STATUS_ERROR_DECODING_ERROR &&
                f.decode->mpeg2_auxiliary.PendingCount() == 2 && f.mock.leased,
            "output debt cannot retire before successful lease release");
    // The mock has no kernel terminal containment; consume its synthetic lease
    // solely to let fixture teardown model the already-failed channel.
    f.mock.release_error = false; f.mock.leased = false; f.mock.outputs.clear();
  }
  {
    Fixture f; f.Submit(1); f.Submit(2,2,1);
    Require(TryMpeg2Auxiliary(&f.driver,f.decode.get(),f.driver.surfaces.at(1)) == VA_STATUS_SUCCESS,
            "prepare non-consuming close");
    f.mock.close_busy = true;
    Require(f.decode->CloseHardware(0) == BC_STS_BUSY && f.decode->device == &f.mock &&
                f.decode->hardware_close_pending &&
                f.decode->mpeg2_auxiliary.PendingCount() == 2 &&
                f.decode->mpeg2_auxiliary.aborted,
            "BUSY close retains exact ownership ledger without permitting more decode");
    f.mock.close_busy = false;
    Require(f.decode->CloseHardware(0) == BC_STS_SUCCESS && !f.decode->device &&
                f.decode->mpeg2_auxiliary.PendingCount() == 0,
            "only consuming close clears aborted private ownership");
  }
}

void PrivateMpeg2NormalizedAndNewAnchor() {
  std::vector<uint8_t> first_i, first_p;
  for (unsigned flags = 0; flags < 4; ++flags) {
    Fixture f;
    auto i = Picture(), p = Picture(16,32,2,1);
    i.picture_coding_extension.bits.repeat_first_field = flags & 1;
    i.picture_coding_extension.bits.top_field_first = flags >> 1;
    p.picture_coding_extension = i.picture_coding_extension;
    f.Begin(1); f.Parameters(i); f.End();
    f.Begin(2); f.Parameters(p); f.End();
    if (!flags) { first_i = f.mock.inputs[0].bytes; first_p = f.mock.inputs[1].bytes; }
    Require(f.mock.inputs[0].bytes == first_i && f.mock.inputs[1].bytes == first_p &&
                TryMpeg2Auxiliary(&f.driver,f.decode.get(),f.driver.surfaces.at(1)) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 4 &&
                i.picture_coding_extension.bits.repeat_first_field == (flags & 1),
            "existing display normalization preserves original/caller metadata and completion eligibility");
  }
  {
    Fixture f; f.Submit(1); f.Submit(2,2,1);
    const auto newer = Custom(91);
    f.Begin(3); f.Parameters(Picture(), &newer); f.End();
    Require(TryMpeg2Auxiliary(&f.driver,f.decode.get(),f.driver.surfaces.at(1)) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 3, "obsolete anchor is not current lookahead target");
    Require(TryMpeg2Auxiliary(&f.driver,f.decode.get(),f.driver.surfaces.at(2)) == VA_STATUS_SUCCESS &&
                f.mock.inputs.size() == 5, "new I advances only its actual previous anchor demand");
    CheckMatrices(f.mock.inputs[3].bytes, newer);
  }
}
}

void RejectedConsumedEndReadback() {
  for (VAProfile profile : {VAProfileMPEG2Simple, VAProfileMPEG2Main}) {
    for (bool ready : {true, false}) {
      Fixture f(profile);
      // All progress below is explicit. An unused VPP worker must not turn
      // these exact readback-state checks into mutex-admission race tests.
      f.driver.StopWorkers();
      f.driver.stopping = false;
      f.Submit(1);
      const auto target = f.driver.surfaces.at(1);
      const auto picture = f.decode->decoded_frames.at(kTimestampStep);
      const auto identity = picture->decode_identity;
      const auto anchor = f.decode->mpeg2_replay.newest_anchor();
      const auto original_input = f.mock.inputs.front().bytes;
      if (ready) {
        f.mock.Queue(kTimestampStep, f.width, f.height); f.Receive();
        Require(SyncSurface2(&f.context, 1, 0) == VA_STATUS_SUCCESS,
                "accepted completed public picture is initially readable");
      }
      VAImage snapshot = {};
      if (ready)
        Require(DeriveImage(&f.context, 1, &snapshot) == VA_STATUS_SUCCESS,
                "capture an independent accepted image before rejection");
      auto old_vpp = std::make_shared<DecodeContext>();
      old_vpp->video_process = true;
      f.driver.contexts[100] = old_vpp;
      VAProcPipelineParameterBuffer pipeline = {};
      pipeline.surface = 1;
      VABufferID parameters = f.Buffer(VAProcPipelineParameterBufferType,
                                       &pipeline, sizeof(pipeline), 1, 100);
      Require(BeginPicture(&f.context, 100, 4) == VA_STATUS_SUCCESS &&
                  RenderPicture(&f.context, 100, &parameters, 1) == VA_STATUS_SUCCESS &&
                  old_vpp->vpp_frame == picture,
              "capture the exact accepted private picture before rejection");
      f.Begin(1);
      Require(EndPicture(&f.context, f.id) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                  f.decode->target == VA_INVALID_SURFACE &&
                  f.decode->next_timestamp == 2 * kTimestampStep &&
                  f.mock.inputs.size() == 1 && f.mock.inputs.front().bytes == original_input &&
                  f.decode->mpeg2_replay.newest_anchor() == anchor,
              "empty End consumes only the new transaction, not accepted reference/input state");
      Require(SyncSurface2(&f.context, 1, 0) == VA_STATUS_ERROR_DECODING_ERROR,
              "failed reused-target End cannot publish the old frame as new success");
      if (!ready) {
        f.mock.Queue(kTimestampStep, f.width, f.height); f.Receive();
      }
      Require(SyncSurface2(&f.context, 1, 0) == VA_STATUS_ERROR_DECODING_ERROR,
              "late accepted completion cannot clear the public rejection barrier");
      VASurfaceStatus state = VASurfaceReady;
      Require(QuerySurfaceStatus(&f.context, 1, &state) == VA_STATUS_ERROR_DECODING_ERROR,
              "new query cannot report the rejected transaction READY");
      VAImage derived = {}, destination = {};
      VAImageFormat format = {}; format.fourcc = VA_FOURCC_NV12;
      Require(DeriveImage(&f.context, 1, &derived) == VA_STATUS_ERROR_DECODING_ERROR &&
                  CreateImage(&f.context, &format, f.width, f.height, &destination) == VA_STATUS_SUCCESS,
              "new derive rejects while an independent destination remains allocatable");
      auto &storage = f.driver.buffers.at(destination.buf).data;
      std::fill(storage.begin(), storage.end(), 0x93);
      Require(GetImage(&f.context, 1, 0, 0, f.width, f.height, destination.image_id) ==
                  VA_STATUS_ERROR_DECODING_ERROR &&
                  std::all_of(storage.begin(), storage.end(), [](uint8_t v) { return v == 0x93; }),
              "new GetImage rejects without copying old pixels");
      Require(DestroyImage(&f.context, destination.image_id) == VA_STATUS_SUCCESS,
              "independent image remains releasable");
      VADRMPRIMESurfaceDescriptor descriptor = {};
      for (uint32_t access : {uint32_t(VA_EXPORT_SURFACE_READ_ONLY),
                              uint32_t(VA_EXPORT_SURFACE_READ_WRITE), uint32_t(0)})
        Require(ExportSurfaceHandle(&f.context, 1, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                    access, &descriptor) == VA_STATUS_ERROR_DECODING_ERROR,
                "new read/default export rejects before graphics access");
      auto new_vpp = std::make_shared<DecodeContext>();
      new_vpp->video_process = true;
      f.driver.contexts[101] = new_vpp;
      VABufferID rejected_parameters = f.Buffer(VAProcPipelineParameterBufferType,
                                                &pipeline, sizeof(pipeline), 1, 101);
      Require(BeginPicture(&f.context, 101, 3) == VA_STATUS_SUCCESS &&
                  RenderPicture(&f.context, 101, &rejected_parameters, 1) == VA_STATUS_ERROR_DECODING_ERROR,
              "new VPP capture cannot acquire the rejected public picture");
      const auto pixels = picture->storage;
      {
        std::unique_lock<std::mutex> lock(f.driver.mutex);
        Require(SyncDecodeSurface(&f.driver, old_vpp->vpp_frame, &lock, 0) == VA_STATUS_SUCCESS &&
                    picture->decode_identity == identity && picture->frame_timestamp == kTimestampStep &&
                    picture->planes[0][0] == 128 && !picture->failed,
                "old captured private pixels and timestamp remain readable");
      }
      if (ready) {
        void *mapped = nullptr;
        Require(MapBufferWithFlags(&f.context, snapshot.buf, &mapped, kImageMapRead) ==
                    VA_STATUS_SUCCESS && static_cast<uint8_t *>(mapped)[0] == 128 &&
                    UnmapBuffer(&f.context, snapshot.buf) == VA_STATUS_SUCCESS &&
                    DestroyImage(&f.context, snapshot.image_id) == VA_STATUS_SUCCESS,
                "old derived snapshot remains readable and releasable");
      }
      const auto rejected_transaction = target->picture_transaction;
      ++target->vpp_writers;
      Require(BeginPicture(&f.context, f.id, 1) == VA_STATUS_ERROR_HW_BUSY &&
                  target->picture_transaction == rejected_transaction && PublicReadRejected(target.get()),
              "failed Begin cannot supersede the rejection barrier");
      --target->vpp_writers;
      f.Submit(1);
      Require(!PublicReadRejected(target.get()), "valid newer transaction supersedes the barrier");
      f.mock.Queue(2 * kTimestampStep, f.width, f.height);
      std::fill(f.mock.outputs.back().pixels.begin(), f.mock.outputs.back().pixels.end(), 80);
      f.Receive();
      Require(SyncSurface2(&f.context, 1, 0) == VA_STATUS_SUCCESS &&
                  target->frame_timestamp == 2 * kTimestampStep && target->planes[0][0] == 80 &&
                  old_vpp->vpp_frame == picture && picture->storage == pixels &&
                  picture->frame_timestamp == kTimestampStep && !picture->failed,
              "new accepted output is distinct while the old captured picture stays immutable");
    }
  }
}

int main() {
  const std::pair<const char *, void (*)()> tests[] = {
    {"failed consumed End preserves private pictures and rejects new public reads", RejectedConsumedEndReadback},
    {"profiles and MPEG2 input format", ProfilesAndInputFormat},
    {"owned buffers and all public call orders", OwnedBuffersAndOrdering},
    {"slice arrays, groups and offsets", SliceGroupsAndOffsets},
    {"matrix commit and failure atomicity", MatrixCommitAndFailureAtomicity},
    {"malformed buffers poison and recover", MalformedBuffersPoison},
    {"bad calls, context and pair ownership", BadCallsContextsAndMissingPairs},
    {"unsupported picture/slice syntax", UnsupportedPictureAndSlicePoison},
    {"progressive display flags preserve pixels request and caller ownership", ProgressiveDisplayFlags},
    {"Main interlaced public submission and bounded syntax", InterlacedSequenceAdmission},
    {"frozen MPEG-2 sequence mode and logical reset", SequenceModeLifetime},
    {"mixed frame flags preserve sequence, replay and auxiliary policy", MixedFrameSequenceLifetime},
    {"reference identity and unready anchors", ReferenceValidation},
    {"late poison and immutable reference snapshot", LatePoisonAndReferenceSnapshot},
    {"visible versus allocated geometry", VisibleAndAllocatedGeometry},
    {"actual EOS reopen and original replay", SealedReplayOriginalBytes},
    {"backpressure and Simple profile restriction", BackpressureAndSimpleBRejection},
    {"private MPEG2 anchor completion and explicit tail EOS", PrivateMpeg2Completion},
    {"private MPEG2 exact B clone and output debt", PrivateMpeg2BCompletion},
    {"private MPEG2 complete field-pair ownership", PrivateMpeg2FieldCompletion},
    {"private MPEG2 partial/malformed field retirement", PrivateMpeg2FieldFailures},
    {"private MPEG2 completion rejection boundaries", PrivateMpeg2CompletionRejection},
    {"private MPEG2 public sync and terminal failure ownership", PrivateMpeg2SyncAndFailures},
    {"private MPEG2 normalized display flags and new anchor", PrivateMpeg2NormalizedAndNewAnchor},
  };
  unsigned failed = 0;
  for (const auto &test : tests) {
    try { test.second(); std::printf("%s: PASS\n", test.first); }
    catch (const std::exception &error) { ++failed; std::fprintf(stderr, "%s: FAIL: %s\n", test.first, error.what()); }
  }
  std::printf("%zu groups, %u checks, %u failures\n", sizeof(tests)/sizeof(tests[0]), checks, failed);
  return failed ? 1 : 0;
}
