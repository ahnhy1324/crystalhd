// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual public VA submission, owned buffers and VC-1/WMV3 replay. All decoder
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
  std::vector<std::vector<uint8_t>> metadata;
  uint32_t expected_subtype = BC_MSUBTYPE_VC1;
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
  Require(format && format->mSubtype == active->expected_subtype && format->Progressive,
          "real OpenDecoder requests the selected progressive VC-1 subtype");
  Require(format->metaDataSz == (active->expected_subtype == BC_MSUBTYPE_WMV3 ? 4U : 0U),
          "WMV3 has four metadata bytes; raw AP has in-band headers");
  active->metadata.emplace_back();
  if (format->metaDataSz) {
    Require(format->pMetaData != nullptr, "metadata pointer is valid during configure");
    active->metadata.back().assign(format->pMetaData, format->pMetaData + format->metaDataSz);
  }
  active->format = *format;
  active->format.pMetaData = nullptr; // The mock takes ownership, never retains a borrowed pointer.
  return BC_STS_SUCCESS;
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
using Bytes = std::vector<uint8_t>;
struct Bits {
  Bytes bytes;
  unsigned position = 0;
  void Put(unsigned count, unsigned value) {
    while (count--) {
      if (!(position % 8)) bytes.push_back(0);
      bytes.back() |= ((value >> count) & 1U) << (7-position%8); ++position;
    }
  }
  void Unary(unsigned value, unsigned stop, unsigned maximum) {
    for (unsigned n=0; n<value; ++n) Put(1,!stop);
    if (value < maximum) Put(1,stop);
  }
  void Code012(unsigned value) { Put(value ? 2 : 1, value ? value+1 : 0); }
};
struct PictureInput {
  VAPictureParameterBufferVC1 picture = {};
  VASliceParameterBufferVC1 slice = {};
  Bytes bytes, planes;
};
// Validated picture-header prefixes with opaque macroblock tails. These are
// transport/ownership fixtures, not decodable conformance or pixel evidence.
PictureInput Input(VAProfile profile = VAProfileVC1Advanced, unsigned type = 0,
                   unsigned width = 16, unsigned height = 32,
                   VASurfaceID forward = VA_INVALID_SURFACE,
                   VASurfaceID backward = VA_INVALID_SURFACE, bool planes = false) {
  PictureInput in;
  auto &p = in.picture;
  p.forward_reference_picture = forward; p.backward_reference_picture = backward;
  p.inloop_decoded_picture = VA_INVALID_SURFACE;
  p.coded_width = width; p.coded_height = height;
  p.sequence_fields.bits.profile = profile == VAProfileVC1Advanced ? 3 :
                                   profile == VAProfileVC1Main ? 1 : 0;
  p.sequence_fields.bits.max_b_frames = profile == VAProfileVC1Simple ? 0 : 3;
  p.fast_uvmc_flag = 1;
  p.picture_fields.bits.picture_type = type; p.picture_fields.bits.is_first_field = 1;
  p.rounding_control = type == 0 || type == 3;
  p.b_picture_fraction = type == 3 ? 22 : 2;
  p.pic_quantizer_fields.bits.pic_quantizer_scale = 5;
  p.pic_quantizer_fields.bits.pic_quantizer_type = 1;
  p.transform_fields.bits.transform_ac_codingset_idx1 = 1;
  p.transform_fields.bits.transform_ac_codingset_idx2 = 2;
  p.transform_fields.bits.intra_transform_dc_table = 1;
  p.mv_fields.bits.mv_mode = VAMvMode1Mv;
  unsigned supplied = 0, slot = 0;
  if (profile == VAProfileVC1Advanced && (type == 0 || type == 3)) {
    p.raw_coding.flags.ac_pred = 1; supplied = 1U << 5; slot = 2;
  }
  if (type == 1) { p.raw_coding.flags.skip_mb = 1; supplied = 1U << 2; slot = 2; }
  if (type == 2) p.raw_coding.flags.direct_mb = p.raw_coding.flags.skip_mb = 1;
  if (planes && supplied) {
    p.raw_coding.value &= ~supplied; p.bitplane_present.value = supplied;
    const unsigned mbs = ((width+15)/16)*((height+15)/16);
    in.planes.assign((mbs+1)/2, static_cast<uint8_t>(slot*17));
    if (mbs&1) in.planes.back() &= 0xf0;
  }
  Bits b;
  const auto plane = [&](bool supplied_plane) {
    b.Put(1,0); // INVERT
    if (!supplied_plane) b.Put(4,0); // RAW
    else {
      b.Put(3,2); // ROWSKIP: all-one matching supplied expanded planes.
      for (unsigned row=0;row<(height+15)/16;++row) {
        b.Put(1,1);
        for (unsigned column=0;column<(width+15)/16;++column) b.Put(1,1);
      }
    }
  };
  const auto picture_suffix = [&] {
    if (type == 0 || type == 3) {
      if (profile == VAProfileVC1Advanced) plane(planes);
      b.Code012(1); b.Code012(2); b.Put(1,1);
    } else {
      b.Put(1,1); // P MVMODE unary index zero; B one-MV quarter-pel.
      if (type == 2) plane(false); // DIRECTMB
      plane(planes); // SKIPMB
      b.Put(2,0); b.Put(2,0); // MVTAB and CBPTAB
      b.Code012(1); b.Put(1,1);
    }
  };
  if (profile == VAProfileVC1Advanced) {
    static constexpr unsigned codes[] = {2,0,1,3,4};
    b.Unary(codes[type],0,4);
    if (type != 4) {
      b.Put(1,p.rounding_control);
      if (type == 2) b.Put(3,2);
      b.Put(5,5); b.Put(1,0);
      picture_suffix();
    }
  } else {
    b.Put(2,3); // Nonzero frame counter must survive unchanged.
    if (type == 1) b.Put(1,1);
    else { b.Put(1,0); if (p.sequence_fields.bits.max_b_frames) b.Put(1,type == 0); }
    if (type == 2) b.Put(3,2);
    if (type == 3) { b.Put(3,7); b.Put(4,15); }
    if (type == 0 || type == 3) b.Put(7,93);
    b.Put(5,5); b.Put(1,0);
    picture_suffix();
  }
  in.slice.macroblock_offset = b.position;
  if (type != 4) b.Put(24,0xa5c39e);
  in.bytes = std::move(b.bytes);
  in.slice.slice_data_size = in.bytes.size();
  in.slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
  return in;
}
Bytes ExpectedMetadata(const VAPictureParameterBufferVC1 &p) {
  Bits b;
  b.Put(2,p.sequence_fields.bits.profile); b.Put(2,0);
  b.Put(3,5); b.Put(5,31); b.Put(1,p.entrypoint_fields.bits.loopfilter);
  b.Put(1,0); b.Put(1,0); b.Put(1,1);
  b.Put(1,p.fast_uvmc_flag); b.Put(1,p.mv_fields.bits.extended_mv_flag);
  b.Put(2,p.pic_quantizer_fields.bits.dquant);
  b.Put(1,p.transform_fields.bits.variable_sized_transform_flag); b.Put(1,0);
  b.Put(1,p.sequence_fields.bits.overlap); b.Put(1,p.sequence_fields.bits.syncmarker);
  b.Put(1,p.sequence_fields.bits.rangered); b.Put(3,p.sequence_fields.bits.max_b_frames);
  b.Put(2,p.pic_quantizer_fields.bits.quantizer); b.Put(1,p.sequence_fields.bits.finterpflag);
  b.Put(1,1); Require(b.position == 32,"independent standard STRUCT_C has 32 bits");
  return b.bytes;
}
Bytes Expected(const PictureInput &in, VAProfile profile) {
  if (profile != VAProfileVC1Advanced)
    return Bytes(in.bytes.begin()+in.slice.slice_data_offset,
                 in.bytes.begin()+in.slice.slice_data_offset+in.slice.slice_data_size);
  Bytes out;
  Require(crystalhd_vc1::Assemble(in.picture, in.planes.data(), in.planes.size(),
      {{in.slice,in.bytes.data(),in.bytes.size()}}, {3,5,31},
      in.picture.picture_fields.bits.picture_type == 0, &out), "fixture AP assembly succeeds");
  // Assembler syntax has independent tests; this comparison detects lifecycle
  // mutation/loss, not correctness of the assembler's own bitstream algorithm.
  return out;
}

struct Fixture {
  DecoderMock mock;
  MockScope scope{&mock};
  Driver driver{-1};
  VADriverContext context = {};
  VAConfigID config = VA_INVALID_ID;
  VAContextID id = VA_INVALID_ID;
  std::shared_ptr<DecodeContext> decode;
  VAProfile profile;
  unsigned width, height;
  explicit Fixture(VAProfile p = VAProfileVC1Advanced, unsigned w = 16, unsigned h = 32)
      : profile(p), width(w), height(h) {
    context.pDriverData = &driver;
    mock.expected_subtype = profile == VAProfileVC1Advanced ? BC_MSUBTYPE_VC1 : BC_MSUBTYPE_WMV3;
    Require(CreateConfig(&context, profile, VAEntrypointVLD, nullptr, 0, &config) == VA_STATUS_SUCCESS,
            "public VC1 profile configuration");
    Require(CreateContext(&context, config, width, height, VA_PROGRESSIVE, nullptr, 0, &id) == VA_STATUS_SUCCESS,
            "public progressive context");
    decode = driver.contexts.at(id);
    Require(decode->profile == profile && decode->IsVc1(),"selected profile retained");
    for (unsigned target=1; target<=5; ++target) Surface(target);
  }
  void Surface(VASurfaceID id, unsigned w=0, unsigned h=0) {
    auto s = std::make_shared<::Surface>();
    Require(s->AllocateInternal(nullptr,-1,w?w:width,h?h:height,VA_FOURCC_NV12),"private CPU surface");
    driver.surfaces[id] = std::move(s);
  }
  VABufferID Buffer(VABufferType type, const void *data, unsigned size, unsigned elements=1,
                    VAContextID owner=VA_INVALID_ID) {
    VABufferID b=VA_INVALID_ID;
    Require(CreateBuffer(&context,owner==VA_INVALID_ID?id:owner,type,size,elements,
                         const_cast<void *>(data),&b)==VA_STATUS_SUCCESS,"public buffer allocation");
    return b;
  }
  VABufferID ByteBuffer(VABufferType type,const Bytes &bytes) {
    return Buffer(type,bytes.data(),1,bytes.size());
  }
  void Begin(VASurfaceID target=1) {
    Require(BeginPicture(&context,id,target)==VA_STATUS_SUCCESS,"public BeginPicture");
  }
  VAStatus RenderStatus(VABufferID b) { return RenderPicture(&context,id,&b,1); }
  void Render(VABufferID b) { Require(RenderStatus(b)==VA_STATUS_SUCCESS,"public RenderPicture"); }
  void Parameters(const PictureInput &in) {
    Render(Buffer(VAPictureParameterBufferType,&in.picture,sizeof(in.picture)));
    if (!in.planes.empty()) Render(ByteBuffer(VABitPlaneBufferType,in.planes));
    Render(Buffer(VASliceParameterBufferType,&in.slice,sizeof(in.slice)));
    Render(ByteBuffer(VASliceDataBufferType,in.bytes));
  }
  void End() {
    Require(EndPicture(&context,id)==VA_STATUS_SUCCESS,"public End/Submit");
    Require(!decode->vc1_picture.have_picture && decode->vc1_picture.data.empty() &&
            decode->vc1_picture.bitplanes.empty(),"End releases completed transaction storage");
  }
  void Submit(VASurfaceID target,unsigned type=0,VASurfaceID forward=VA_INVALID_SURFACE,
              VASurfaceID backward=VA_INVALID_SURFACE) {
    Begin(target); Parameters(Input(profile,type,width,height,forward,backward)); End();
  }
  void Receive() { Require(ReceiveAvailable(&driver,decode.get())==VA_STATUS_SUCCESS,"actual output dispatch"); }
};
void NoCommit(const Fixture &f, uint64_t timestamp=kTimestampStep, size_t count=0) {
  Require(f.decode->next_timestamp==timestamp && f.mock.inputs.size()==count,"failed transaction has no token or input commit");
  if (!count) Require(f.mock.opens==0 && f.decode->vc1_configuration.empty(),"invalid first input does not open/configure decoder");
}

void Profiles() {
  for (auto profile : {VAProfileVC1Simple,VAProfileVC1Main,VAProfileVC1Advanced}) {
    Fixture f(profile); VAProfile profiles[16]={}; int count=0;
    Require(QueryConfigProfiles(&f.context,profiles,&count)==VA_STATUS_SUCCESS && count==9 &&
            std::find(profiles,profiles+count,profile)!=profiles+count,
            "standard Simple, Main and Advanced VC1 profiles advertised");
    VAConfigAttrib caps[]={{VAConfigAttribMaxPictureWidth,0},{VAConfigAttribMaxPictureHeight,0},{VAConfigAttribRTFormat,0}};
    Require(GetConfigAttributes(&f.context,profile,VAEntrypointVLD,caps,3)==VA_STATUS_SUCCESS &&
            caps[0].value==1920 && caps[1].value==1088 && caps[2].value==VA_RT_FORMAT_YUV420,"reported bounded progressive geometry");
    unsigned attributes_count=0;
    Require(QuerySurfaceAttributes(&f.context,f.config,nullptr,&attributes_count)==VA_STATUS_SUCCESS,"surface attribute count");
    std::vector<VASurfaceAttrib> attributes(attributes_count);
    Require(QuerySurfaceAttributes(&f.context,f.config,attributes.data(),&attributes_count)==VA_STATUS_SUCCESS,"surface attributes");
    unsigned dimensions=0;
    for(const auto &attribute:attributes) {
      if(attribute.type==VASurfaceAttribMaxWidth) { Require(attribute.value.value.i==1920,"surface/config width agree"); ++dimensions; }
      if(attribute.type==VASurfaceAttribMaxHeight) { Require(attribute.value.value.i==1088,"surface/config height agree"); ++dimensions; }
    }
    Require(dimensions==2,"both surface bounds present");
    for (auto dimensions : {std::pair<int,int>{1936,32},{16,1104}}) {
      VAContextID bad=VA_INVALID_ID;
      Require(CreateContext(&f.context,f.config,dimensions.first,dimensions.second,VA_PROGRESSIVE,
                            nullptr,0,&bad)!=VA_STATUS_SUCCESS,"context enforces advertised maximum");
    }
    f.Submit(1);
    Require(f.mock.opens==1 && f.mock.inputs.size()==1 && f.mock.format.width==16 && f.mock.format.height==32,
            "one input opens selected codec with exact geometry");
    Require(f.decode->vc1_replay.cached_pictures()==1 && f.decode->replay.cached_pictures()==0 &&
            f.decode->mpeg2_replay.cached_pictures()==0,"VC1 never enters another codec's replay");
    const auto in=Input(profile);
    Require(f.mock.inputs[0].bytes==Expected(in,profile),"exact assembler output is transported");
    if (profile!=VAProfileVC1Advanced) Require(f.mock.metadata[0]==ExpectedMetadata(in.picture),"copied standard WMV3 metadata");
    if (profile==VAProfileVC1Simple) {
      f.Submit(2,1,1);
      Require(f.mock.inputs.size()==2 && f.decode->vc1_replay.newest_anchor()==2*kTimestampStep,
              "standard Simple I/P submission uses the same immutable reference contract");
    }
  }
}
void SimpleRestrictions() {
  for(unsigned variant=0;variant<9;++variant) {
    Fixture f(VAProfileVC1Simple); auto in=Input(VAProfileVC1Simple);
    const bool needs_anchors=variant>=7;
    if(needs_anchors) { f.Submit(1); f.Submit(2,1,1); }
    switch(variant) {
      case 0: in.picture.sequence_fields.bits.max_b_frames=1; break;
      case 1: in.picture.entrypoint_fields.bits.loopfilter=1; break;
      case 2: in.picture.mv_fields.bits.extended_mv_flag=1; break;
      case 3: in.picture.fast_uvmc_flag=0; break;
      case 4: in.picture.sequence_fields.bits.rangered=1; break;
      case 5: in.picture.sequence_fields.bits.syncmarker=1; break;
      case 6: in.picture.pic_quantizer_fields.bits.dquant=1; break;
      case 7: in=Input(VAProfileVC1Simple,2,16,32,1,2); break;
      case 8: in=Input(VAProfileVC1Simple,3,16,32,1,2); break;
    }
    const unsigned target=needs_anchors?3:1;
    f.Begin(target);
    VABufferID ids[]={f.Buffer(VAPictureParameterBufferType,&in.picture,sizeof(in.picture)),
      f.Buffer(VASliceParameterBufferType,&in.slice,sizeof(in.slice)),f.ByteBuffer(VASliceDataBufferType,in.bytes)};
    (void)RenderPicture(&f.context,f.id,ids,3);
    Require(EndPicture(&f.context,f.id)!=VA_STATUS_SUCCESS,"Simple rejects non-Simple syntax before hardware admission");
    NoCommit(f,target*kTimestampStep,target-1);
    f.Submit(target); f.Submit(target+1,1,target);
    Require(f.mock.inputs.size()==target+1,"fresh standard Simple I/P transaction recovers after rejection");
  }
}
void OwnedBuffersAndOrder() {
  for (auto profile : {VAProfileVC1Advanced,VAProfileVC1Main})
    for (bool grouped : {false,true}) for (bool byte_elements : {false,true}) {
      std::array<unsigned,4> order{0,1,2,3};
      do {
        Fixture f(profile,32,32);
        const bool ap=profile==VAProfileVC1Advanced;
        if (!ap) f.Submit(1);
        auto in=Input(profile,ap?0:1,32,32,ap?VA_INVALID_SURFACE:1,VA_INVALID_SURFACE,true);
        const auto expected=Expected(in,profile);
        VABufferID ids[]={f.Buffer(VAPictureParameterBufferType,&in.picture,sizeof(in.picture)),
          f.Buffer(VABitPlaneBufferType,in.planes.data(),byte_elements?1:in.planes.size(),byte_elements?in.planes.size():1),
          f.Buffer(VASliceParameterBufferType,&in.slice,sizeof(in.slice)),
          f.Buffer(VASliceDataBufferType,in.bytes.data(),byte_elements?1:in.bytes.size(),byte_elements?in.bytes.size():1)};
        f.Begin(ap?1:2);
        if (grouped) {
          VABufferID ordered[]={ids[order[0]],ids[order[1]],ids[order[2]],ids[order[3]]};
          Require(RenderPicture(&f.context,f.id,ordered,4)==VA_STATUS_SUCCESS,"arbitrary grouped order");
        } else for (unsigned n:order) f.Render(ids[n]);
        for (auto b:ids) {
          void *data=nullptr; Require(MapBuffer(&f.context,b,&data)==VA_STATUS_SUCCESS,"original buffer maps");
          std::memset(data,0xee,f.driver.buffers.at(b).data.size());
          Require(UnmapBuffer(&f.context,b)==VA_STATUS_SUCCESS && DestroyBuffer(&f.context,b)==VA_STATUS_SUCCESS,
                  "caller storage destroyed after Render");
        }
        f.End(); Require(f.mock.inputs.back().bytes==expected,"all bytes owned before caller mutation");
      } while(std::next_permutation(order.begin(),order.end()));
    }
}
void SliceGroups() {
  for (bool array:{false,true}) {
    Fixture f; auto in=Input(); Bits tail; tail.Put(9,1); tail.Put(1,0); tail.Put(20,0xabcde);
    auto second=in.slice; second.slice_vertical_position=1; second.macroblock_offset=10;
    second.slice_data_size=tail.bytes.size();
    auto first=in.slice; first.slice_data_offset=3; second.slice_data_offset=array?19:5;
    Bytes a(array?32:3+in.bytes.size(),0xda),b(5+tail.bytes.size(),0xca);
    std::copy(in.bytes.begin(),in.bytes.end(),a.begin()+3);
    std::copy(tail.bytes.begin(),tail.bytes.end(),array?a.begin()+19:b.begin()+5);
    f.Begin(); f.Render(f.Buffer(VAPictureParameterBufferType,&in.picture,sizeof(in.picture)));
    if(array) {
      VASliceParameterBufferVC1 both[]={first,second};
      f.Render(f.ByteBuffer(VASliceDataBufferType,a));
      f.Render(f.Buffer(VASliceParameterBufferType,both,sizeof(first),2));
    } else {
      f.Render(f.Buffer(VASliceParameterBufferType,&first,sizeof(first)));
      f.Render(f.Buffer(VASliceParameterBufferType,&second,sizeof(second)));
      f.Render(f.ByteBuffer(VASliceDataBufferType,a)); f.Render(f.ByteBuffer(VASliceDataBufferType,b));
    }
    Bytes expected;
    Require(crystalhd_vc1::Assemble(in.picture,nullptr,0,
      {{first,a.data(),a.size()},{second,array?a.data():b.data(),array?a.size():b.size()}},
      {3,5,31},true,&expected),"two independent input slice associations are valid");
    f.End(); Require(f.mock.inputs.back().bytes==expected,"group/array offsets survive production submission");
  }
}
void PoisonAndRecovery() {
  for(auto profile:{VAProfileVC1Advanced,VAProfileVC1Main}) {
    for(unsigned variant=0;variant<15;++variant) {
      Fixture f(profile); auto in=Input(profile); f.Begin();
      VABufferID bad=VA_INVALID_ID;
      if(variant<3) {
        Bytes bytes(sizeof(in.picture)+(int(variant)-1),0);
        bad=f.Buffer(VAPictureParameterBufferType,bytes.data(),bytes.size());
        if (variant==1) Require(BufferSetNumElements(&f.context,bad,0)==VA_STATUS_SUCCESS,"zero-element public resize");
      } else if(variant==3) {
        VAPictureParameterBufferVC1 pair[]={in.picture,in.picture};
        bad=f.Buffer(VAPictureParameterBufferType,pair,sizeof(in.picture),2);
      } else if(variant==4) bad=f.Buffer(VASliceParameterBufferType,&in.slice,sizeof(in.slice)-1);
      else if(variant==5) { uint8_t byte=0; bad=f.Buffer(VABitPlaneBufferType,&byte,1); f.Render(bad); }
      else if(variant==6) { uint8_t byte=0; bad=f.Buffer(VAIQMatrixBufferType,&byte,1); }
      else if(variant==7) { uint8_t byte=0; bad=f.Buffer(VAImageBufferType,&byte,1); }
      else if(variant==8) { f.Parameters(in); }
      else if(variant==9) {
        bad=f.Buffer(VAPictureParameterBufferType,&in.picture,sizeof(in.picture));
        f.Render(bad); // Duplicate picture buffer poisons, not last-one-wins.
      } else if(variant==10) {
        VAContextID other=VA_INVALID_ID;
        Require(CreateContext(&f.context,f.config,16,32,VA_PROGRESSIVE,nullptr,0,&other)==VA_STATUS_SUCCESS,"second context");
        bad=f.Buffer(VAPictureParameterBufferType,&in.picture,sizeof(in.picture),1,other);
      } else if(variant==11) {
        bad=f.Buffer(VAPictureParameterBufferType,&in.picture,sizeof(in.picture));
        Require(BufferSetNumElements(&f.context,bad,0)==VA_STATUS_SUCCESS,"empty public buffer");
      } else if(variant==12) {
        Bytes bytes(512*1024+1,0xaa); bad=f.ByteBuffer(VASliceDataBufferType,bytes);
      }
      VAStatus status;
      if(variant>=13) status=RenderPicture(&f.context,f.id,nullptr,variant==13?-1:1);
      else status=f.RenderStatus(bad);
      Require(status!=VA_STATUS_SUCCESS,"bad public payload/call is rejected");
      auto good=f.Buffer(VAPictureParameterBufferType,&in.picture,sizeof(in.picture));
      Require(f.RenderStatus(good)!=VA_STATUS_SUCCESS,"later valid buffer cannot clear poison");
      Require(EndPicture(&f.context,f.id)!=VA_STATUS_SUCCESS,"ignored Render error fails End");
      NoCommit(f); Require(!f.decode->vc1_picture.invalid && f.decode->vc1_picture.data.empty(),"End clears failed transaction storage");
      f.Submit(1); Require(f.mock.inputs.size()==1,"fresh Begin recovers normally");
    }
  }
}
void UnsupportedPicturesAndSlices() {
  for(auto profile:{VAProfileVC1Advanced,VAProfileVC1Main})
    for(unsigned variant=0;variant<16;++variant) {
      Fixture f(profile); auto in=Input(profile);
      if(variant==0) in.picture.inloop_decoded_picture=2;
      if(variant==1) in.picture.sequence_fields.bits.interlace=1;
      if(variant==2) in.picture.picture_fields.bits.frame_coding_mode=2;
      if(variant==3) in.picture.sequence_fields.bits.psf=1;
      if(variant==4) in.picture.coded_width=15;
      if(variant==5) in.picture.coded_height=0;
      if(variant==6) in.picture.coded_width=32;
      if(variant==7) in.picture.range_mapping_fields.bits.luma_flag=1;
      if(variant==8) in.slice.slice_data_flag=VA_SLICE_DATA_FLAG_BEGIN;
      if(variant==9) in.slice.slice_data_offset=UINT32_MAX;
      if(variant==10) ++in.slice.slice_data_size;
      if(variant==11) in.slice.macroblock_offset=UINT32_MAX;
      if(variant==12) in.slice.slice_vertical_position=1;
      if(variant==13) in.picture.sequence_fields.bits.profile=2;
      if(variant==14) in.picture.picture_fields.bits.picture_type=5;
      if(variant==15) in.picture.entrypoint_fields.bits.panscan_flag=1;
      f.Begin();
      auto p=f.Buffer(VAPictureParameterBufferType,&in.picture,sizeof(in.picture));
      auto s=f.Buffer(VASliceParameterBufferType,&in.slice,sizeof(in.slice));
      auto b=f.ByteBuffer(VASliceDataBufferType,in.bytes);
      VABufferID ids[]={p,s,b}; (void)RenderPicture(&f.context,f.id,ids,3);
      Require(EndPicture(&f.context,f.id)!=VA_STATUS_SUCCESS,"unsupported picture/slice rejected before opening hardware");
      NoCommit(f); f.Submit(1);
    }
}
void MissingPairsAndPlanes() {
  for(unsigned variant=0;variant<6;++variant) {
    Fixture f; auto in=Input(VAProfileVC1Advanced,0,16,32,VA_INVALID_SURFACE,VA_INVALID_SURFACE,true);
    f.Begin(); f.Render(f.Buffer(VAPictureParameterBufferType,&in.picture,sizeof(in.picture)));
    if(variant!=0) f.Render(f.Buffer(VASliceParameterBufferType,&in.slice,sizeof(in.slice)));
    if(variant!=1) f.Render(f.ByteBuffer(VASliceDataBufferType,in.bytes));
    if(variant>=3) {
      auto planes=in.planes;
      if(variant==3) planes.push_back(0);
      if(variant==4) planes[0]|=0x80;
      f.Render(f.ByteBuffer(VABitPlaneBufferType,planes));
      if(variant==5) Require(f.RenderStatus(f.ByteBuffer(VABitPlaneBufferType,planes))!=VA_STATUS_SUCCESS,"duplicate plane buffer poisons");
    }
    Require(EndPicture(&f.context,f.id)!=VA_STATUS_SUCCESS,"missing pairs/planes or bad plane extent rejected"); NoCommit(f);
  }
  Fixture f; f.Begin(); f.Parameters(Input());
  Require(RenderPicture(&f.context,f.id,nullptr,0)==VA_STATUS_SUCCESS,"zero-buffer call remains no-op"); f.End();
}
void References() {
  for(auto profile:{VAProfileVC1Advanced,VAProfileVC1Main})
    for(unsigned variant=0;variant<9;++variant) {
      Fixture f(profile); f.Submit(1); f.Submit(2,1,1);
      auto in=Input(profile,1,16,32,2);
      if(variant==0) in.picture.forward_reference_picture=VA_INVALID_SURFACE;
      if(variant==1) in.picture.forward_reference_picture=99;
      if(variant==2) in.picture.forward_reference_picture=1;
      if(variant==3) in.picture.forward_reference_picture=3;
      if(variant==4) f.driver.surfaces.at(2)->expected_timestamp=0;
      if(variant==5) f.decode->surface_timestamps.erase(f.driver.surfaces.at(2).get());
      if(variant==6) f.driver.surfaces.at(2)->backing_owner=1;
      if(variant==7) in.picture.backward_reference_picture=1;
      if(variant==8) {
        VAContextID other=VA_INVALID_ID;
        Require(CreateContext(&f.context,f.config,16,32,VA_PROGRESSIVE,nullptr,0,&other)==VA_STATUS_SUCCESS,"foreign context");
        f.driver.surfaces.at(4)->expected_timestamp=kTimestampStep;
        f.driver.contexts.at(other)->surface_timestamps[f.driver.surfaces.at(4).get()]=kTimestampStep;
        in.picture.forward_reference_picture=4;
      }
      f.Begin(3);
      VABufferID ids[]={f.Buffer(VAPictureParameterBufferType,&in.picture,sizeof(in.picture)),
        f.Buffer(VASliceParameterBufferType,&in.slice,sizeof(in.slice)),f.ByteBuffer(VASliceDataBufferType,in.bytes)};
      (void)RenderPicture(&f.context,f.id,ids,3);
      Require(EndPicture(&f.context,f.id)!=VA_STATUS_SUCCESS,"invalid reference cannot submit"); NoCommit(f,3*kTimestampStep,2);
    }
  for(auto profile:{VAProfileVC1Advanced,VAProfileVC1Main}) {
    Fixture f(profile); f.Submit(1); f.Submit(2,1,1);
    Require(!f.driver.surfaces.at(1)->ready && !f.driver.surfaces.at(2)->ready,"valid reference anchors are unready");
    f.Begin(3); f.Parameters(Input(profile,2,16,32,1,2));
    f.driver.surfaces.at(1)->expected_timestamp=999*kTimestampStep;
    f.decode->surface_timestamps[f.driver.surfaces.at(1).get()]=999*kTimestampStep;
    f.End(); const auto *unit=f.decode->vc1_replay.Find(3*kTimestampStep);
    Require(unit && unit->forward==kTimestampStep && unit->backward==2*kTimestampStep,"Render owns immutable references after ID rebinding");
  }
  for(unsigned variant=0;variant<4;++variant) {
    Fixture f; f.Submit(1); f.Submit(2,1,1); f.Submit(3,3,1,2);
    auto in=Input(VAProfileVC1Advanced,2,16,32,1,2);
    if(variant==0) std::swap(in.picture.forward_reference_picture,in.picture.backward_reference_picture);
    if(variant==1) in.picture.forward_reference_picture=3; // BI cannot be an anchor.
    if(variant==2) in.picture.backward_reference_picture=VA_INVALID_SURFACE;
    if(variant==3) in.picture.backward_reference_picture=4; // Target alias.
    f.Begin(4);
    VABufferID ids[]={f.Buffer(VAPictureParameterBufferType,&in.picture,sizeof(in.picture)),
      f.Buffer(VASliceParameterBufferType,&in.slice,sizeof(in.slice)),f.ByteBuffer(VASliceDataBufferType,in.bytes)};
    (void)RenderPicture(&f.context,f.id,ids,3);
    Require(EndPicture(&f.context,f.id)!=VA_STATUS_SUCCESS,"B rejects reversed/missing/nonanchor/target references"); NoCommit(f,4*kTimestampStep,3);
  }
  for(unsigned type:{1U,2U,3U,4U}) {
    Fixture f; auto in=Input(VAProfileVC1Advanced,type); f.Begin();
    VABufferID ids[]={f.Buffer(VAPictureParameterBufferType,&in.picture,sizeof(in.picture)),
      f.Buffer(VASliceParameterBufferType,&in.slice,sizeof(in.slice)),f.ByteBuffer(VASliceDataBufferType,in.bytes)};
    (void)RenderPicture(&f.context,f.id,ids,3);
    Require(EndPicture(&f.context,f.id)!=VA_STATUS_SUCCESS,"non-I cannot start a fresh replay epoch"); NoCommit(f);
  }
}
void BiAndSkippedP() {
  for(auto profile:{VAProfileVC1Advanced,VAProfileVC1Main}) {
    Fixture f(profile); f.Submit(1); f.Submit(2,1,1);
    f.Submit(3,3,1,2); const auto *bi=f.decode->vc1_replay.Find(3*kTimestampStep);
    Require(bi && bi->kind==CrystalHDVc1Replay::Kind::BI && !bi->forward && !bi->backward &&
            f.decode->vc1_replay.newest_anchor()==2*kTimestampStep,"incidental BI refs normalized without advancing anchors");
    f.Submit(4,1,2);
    Require(f.decode->vc1_replay.Find(3*kTimestampStep)!=nullptr,"completed/pending BI state retained");
  }
  Fixture f; f.Submit(1); f.Submit(2,4,1);
  Require(f.decode->vc1_replay.newest_anchor()==2*kTimestampStep && f.mock.inputs.size()==2,"skipped P is an accepted distinct anchor");
  f.Submit(3,2,1,2);
  f.mock.Queue(kTimestampStep,16,32); f.mock.Queue(3*kTimestampStep,16,32); f.Receive();
  Require(f.decode->pending.count(2*kTimestampStep)==1 && !f.driver.surfaces.at(2)->ready,"skipped P cannot borrow another picture's completion");
  f.mock.Queue(2*kTimestampStep,16,32); f.Receive(); Require(f.decode->pending.empty(),"skipped own output completes normally");
}
void ConfigurationAndGeometry() {
  for(auto profile:{VAProfileVC1Advanced,VAProfileVC1Main}) {
    Fixture f(profile); f.Submit(1);
    const auto old=f.decode->vc1_configuration; const auto metadata=f.decode->wmv3_metadata;
    auto in=profile==VAProfileVC1Advanced ? Input(profile,1,16,32,1) : Input(profile);
    in.picture.entrypoint_fields.bits.loopfilter=1;
    f.Begin(2); f.Parameters(in);
    Require(EndPicture(&f.context,f.id)!=VA_STATUS_SUCCESS,"AP non-I and all WMV3 configuration changes are rejected");
    NoCommit(f,2*kTimestampStep,1);
    Require(f.decode->vc1_configuration==old && f.decode->wmv3_metadata==metadata,"failed configuration leaves committed snapshot unchanged");
    f.Submit(2);
  }
  Fixture f(VAProfileVC1Advanced,1920,1088);
  f.Begin(); f.Parameters(Input(VAProfileVC1Advanced,0,1920,1080)); f.End();
  Require(f.mock.format.height==1080 && f.decode->height==1088 && f.decode->stream_height==1080,"visible height differs from allocation height");
  f.mock.Queue(kTimestampStep,1920,1080); f.Receive();
  Require(f.driver.surfaces.at(1)->ready && !f.driver.surfaces.at(1)->failed,"1080 output fits 1088 surface");
  f.Begin(2); f.Parameters(Input(VAProfileVC1Advanced,0,1920,1088));
  Require(EndPicture(&f.context,f.id)!=VA_STATUS_SUCCESS,"visible geometry cannot change after first commit"); NoCommit(f,2*kTimestampStep,1);
}
void AdvancedConfigurationEpochReplay() {
  Fixture f;
  auto picture=[](unsigned type,VASurfaceID forward,VASurfaceID backward,bool closed) {
    auto in=Input(VAProfileVC1Advanced,type,16,32,forward,backward);
    in.picture.entrypoint_fields.bits.loopfilter=1;
    in.picture.entrypoint_fields.bits.closed_entry=closed;
    return in;
  };
  auto submit=[&](VASurfaceID target,unsigned type,VASurfaceID forward,
                 VASurfaceID backward,bool closed) {
    f.Begin(target); f.Parameters(picture(type,forward,backward,closed)); f.End();
  };
  const auto none=VA_INVALID_SURFACE;
  submit(1,0,none,none,true);
  submit(2,1,1,none,true);
  const auto old_configuration=f.decode->vc1_configuration;
  submit(3,0,none,none,false);
  const auto new_configuration=f.decode->vc1_configuration;
  Require(new_configuration!=old_configuration && f.mock.inputs.size()==3,
          "AP I commits changed closed-entry configuration with unchanged geometry");
  // Both entry epochs remain live through the previous/newest I/P anchors.
  // A completed B may be omitted on restart, unlike the stateful BI below.
  submit(4,2,2,3,false);
  f.Begin(5); f.Parameters(picture(1,3,none,true));
  Require(EndPicture(&f.context,f.id)==VA_STATUS_ERROR_INVALID_PARAMETER,
          "P cannot switch back to the earlier entry configuration");
  NoCommit(f,5*kTimestampStep,4);
  Require(f.decode->vc1_configuration==new_configuration &&
          f.decode->vc1_replay.newest_anchor()==3*kTimestampStep,
          "rejected P preserves committed configuration and anchor identity");
  const auto original=f.mock.inputs;
  const uint8_t entry_marker[]={0,0,1,0x0e};
  auto entry_closed=[&](const Bytes &bytes) {
    const auto at=std::search(bytes.begin(),bytes.end(),std::begin(entry_marker),std::end(entry_marker));
    Require(at!=bytes.end() && bytes.end()-at>4,"each retained I owns an entrypoint header");
    return (at[4] & 0x40U)!=0;
  };
  Require(entry_closed(original[0].bytes) && !entry_closed(original[2].bytes) &&
          original[0].bytes!=original[2].bytes,
          "original I access units encode different closed-entry bits");
  Require(SealDecodeBatch(&f.driver,f.decode.get())==VA_STATUS_SUCCESS && f.mock.flushes==1,
          "configuration epochs sealed with genuine EOS request");
  submit(5,3,2,3,false);
  const auto *late=f.decode->vc1_replay.Find(5*kTimestampStep);
  Require(late && f.mock.inputs.size()==4,"late BI remains owned behind the seal");
  const auto late_bytes=late->bytes;
  for(unsigned n=1;n<=4;++n) f.mock.Queue(n*kTimestampStep,16,32);
  f.mock.Eos(); f.Receive();
  Require(f.mock.releases==5 && f.decode->vc1_replay.NeedsRestart(),
          "all earlier pictures and the real EOS marker are released before reopen");
  Require(PumpDecodeInput(&f.driver,f.decode.get())==VA_STATUS_SUCCESS &&
          f.mock.opens==2 && f.mock.closes==1 && f.mock.inputs.size()==8,
          "full reopen retains both entry epochs while omitting completed B");
  Require(!f.decode->vc1_replay.Find(4*kTimestampStep),"only completed non-reference B is pruned");
  for(size_t n=0;n<3;++n)
    Require(f.mock.inputs[n+4].timestamp==original[n].timestamp &&
            f.mock.inputs[n+4].bytes==original[n].bytes,
            "reopen replays immutable bytes and timestamps from each original configuration");
  Require(entry_closed(f.mock.inputs[4].bytes) && !entry_closed(f.mock.inputs[6].bytes) &&
          f.mock.inputs.back().bytes==late_bytes && f.decode->vc1_configuration==new_configuration,
          "replay never rebuilds older I headers from the newest configuration");
  for(unsigned n=1;n<=4;++n) f.mock.Queue(n*kTimestampStep,16,32);
  f.Receive();
  Require(f.decode->pending.size()==1 && f.decode->pending.count(5*kTimestampStep)==1 &&
          !f.driver.surfaces.at(5)->ready,"old-epoch replay output cannot complete late BI");
  f.mock.Queue(5*kTimestampStep,16,32); f.Receive();
  Require(f.decode->pending.empty() && f.driver.surfaces.at(5)->ready,
          "late BI completes only from its immutable output identity");
  f.Surface(6); submit(6,1,3,none,false);
  Require(f.decode->vc1_configuration==new_configuration &&
          f.decode->vc1_replay.newest_anchor()==6*kTimestampStep,
          "subsequent P accepts the configuration committed by its I anchor");
}
void EosReopenAndPendingIdentity() {
  for(auto profile:{VAProfileVC1Advanced,VAProfileVC1Main}) {
    Fixture f(profile); f.Submit(1); f.Submit(2,1,1); f.Submit(3,3,1,2);
    const auto original=f.mock.inputs; const auto metadata=f.mock.metadata.front();
    Require(SealDecodeBatch(&f.driver,f.decode.get())==VA_STATUS_SUCCESS && f.mock.flushes==1,"actual EOS seal");
    f.Submit(4,2,1,2);
    const auto *queued=f.decode->vc1_replay.Find(4*kTimestampStep);
    Require(queued!=nullptr && f.mock.inputs.size()==3,"late input retained behind seal"); const auto late=queued->bytes;
    for(unsigned n=1;n<=3;++n) f.mock.Queue(n*kTimestampStep,16,32);
    f.mock.Eos(); f.Receive();
    Require(f.mock.releases==4 && f.decode->vc1_replay.NeedsRestart(),"all leases and genuine EOS completed");
    auto held=f.decode->decoded_frames.at(kTimestampStep); const auto held_bytes=held->storage;
    Require(PumpDecodeInput(&f.driver,f.decode.get())==VA_STATUS_SUCCESS,"full hardware-only reopen");
    Require(f.mock.opens==2 && f.mock.closes==1 && f.mock.stops==1 && f.mock.inputs.size()==7,"no decoder-only reset or lost BI");
    Require(f.mock.metadata.back()==metadata,"WMV3 metadata copy survives full reopen");
    for(size_t n=0;n<original.size();++n)
      Require(f.mock.inputs[n+3].timestamp==original[n].timestamp && f.mock.inputs[n+3].bytes==original[n].bytes,"all original timestamped I/P/BI bytes replay unchanged");
    Require(f.mock.inputs.back().bytes==late,"queued B retained exact original bytes");
    for(unsigned n=1;n<=3;++n) {
      f.mock.Queue(n*kTimestampStep,16,32);
      // Replay output must be ignored for an already completed token even if
      // a bad device supplies different bytes; unchanged flat mocks would not
      // detect an accidental overwrite of an externally held old picture.
      std::fill(f.mock.outputs.back().pixels.begin(),f.mock.outputs.back().pixels.end(),9+n);
    }
    f.Receive();
    Require(f.decode->pending.count(4*kTimestampStep)==1 && !f.driver.surfaces.at(4)->ready,"replay duplicates do not complete pending B");
    f.mock.Queue(4*kTimestampStep,16,32); f.Receive();
    Require(f.decode->pending.empty() && held->storage==held_bytes,"held decoded payload remains immutable through replay");
  }
}
void BackpressureOwnership() {
  for(auto profile:{VAProfileVC1Advanced,VAProfileVC1Main}) {
    Fixture f(profile); f.mock.capacity=false; auto in=Input(profile); f.Begin(); f.Parameters(in); f.End();
    Require(f.mock.inputs.empty() && f.decode->next_timestamp==2*kTimestampStep,"accepted backpressure retains ownership");
    const auto *unit=f.decode->vc1_replay.Find(kTimestampStep); Require(unit!=nullptr,"pending owned AU exists"); const auto bytes=unit->bytes;
    f.mock.capacity=true; Require(PumpDecodeInput(&f.driver,f.decode.get())==VA_STATUS_SUCCESS &&
      f.mock.inputs.size()==1 && f.mock.inputs[0].bytes==bytes,"capacity retry transmits unchanged bytes once");
  }
}
}
int main() {
  const std::pair<const char *,void(*)()> tests[]={
    {"profiles, capabilities and correct library subtype",Profiles},
    {"standard Simple profile restrictions and recovery",SimpleRestrictions},
    {"owned buffers and every grouped/separate order",OwnedBuffersAndOrder},
    {"AP slice groups, arrays and independent offsets",SliceGroups},
    {"malformed public buffers poison and fresh Begin recovers",PoisonAndRecovery},
    {"unsupported picture, geometry and slice contracts",UnsupportedPicturesAndSlices},
    {"missing pairs and packed bitplane validation",MissingPairsAndPlanes},
    {"reference identity, ownership and unready anchors",References},
    {"BI incidental refs and skipped-P own completion",BiAndSkippedP},
    {"configuration atomicity and visible geometry",ConfigurationAndGeometry},
    {"AP I configuration epochs preserve original headers across EOS",AdvancedConfigurationEpochReplay},
    {"actual EOS reopen, metadata and immutable replay",EosReopenAndPendingIdentity},
    {"backpressure retains immutable input ownership",BackpressureOwnership},
  };
  unsigned failures=0;
  for(const auto &test:tests) {
    try { test.second(); std::printf("%s: PASS\n",test.first); }
    catch(const std::exception &e) { ++failures; std::fprintf(stderr,"%s: FAIL: %s\n",test.first,e.what()); }
  }
  std::printf("%zu groups, %u checks, %u failures\n",sizeof(tests)/sizeof(tests[0]),checks,failures);
  return failures?1:0;
}
