// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../filters/vaapi/crystalhd-vc1.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>

namespace vc = crystalhd_vc1;
using Bytes = std::vector<uint8_t>;
namespace {
unsigned checks = 0, groups = 0;
void Check(bool yes, const char *why) {
  ++checks;
  if (!yes) throw std::runtime_error(why);
}
void RejectDetail(const std::function<void()> &fn) {
  bool rejected = false;
  try { fn(); } catch (const vc::detail::Invalid &) { rejected = true; }
  Check(rejected, "expected bounded syntax rejection");
}

// Independent test bit I/O and BDU unescaping; no production scan/read helper.
struct Writer {
  Bytes bytes;
  size_t position = 0;
  void Put(unsigned count, unsigned value) {
    for (unsigned i = count; i; --i, ++position) {
      if (!(position % 8)) bytes.push_back(0);
      if ((value >> (i - 1)) & 1) bytes.back() |= 0x80 >> (position % 8);
    }
  }
  void Copy(const Bytes &source, size_t begin, size_t end) {
    for (size_t bit = begin; bit < end; ++bit)
      Put(1, (source.at(bit / 8) >> (7 - bit % 8)) & 1);
  }
};
struct Reader {
  const Bytes &bytes;
  size_t position = 0;
  unsigned Get(unsigned count) {
    Check(count <= 32 && position + count <= bytes.size() * 8, "bounded independent read");
    unsigned value = 0;
    while (count--) {
      value = value * 2 + ((bytes[position / 8] >> (7 - position % 8)) & 1);
      ++position;
    }
    return value;
  }
  void Equal(unsigned count, unsigned value) { Check(Get(count) == value, "parsed field equals expected"); }
};
Bytes Escape(const Bytes &bytes) {
  Bytes out;
  for (uint8_t byte : bytes) {
    if (out.size() >= 2 && out[out.size()-2] == 0 && out.back() == 0 && byte < 4)
      out.push_back(3);
    out.push_back(byte);
  }
  return out;
}
struct Unit { unsigned code; Bytes bytes; };
std::vector<Unit> Units(const Bytes &stream) {
  std::vector<Unit> result;
  size_t at = 0;
  while (at < stream.size()) {
    Check(at + 4 <= stream.size() && !stream[at] && !stream[at+1] && stream[at+2] == 1,
          "complete BDU start code");
    Unit unit{stream[at+3], {}};
    size_t end = at + 4;
    while (end + 3 >= stream.size() || stream[end] || stream[end+1] || stream[end+2] != 1) {
      if (end == stream.size()) break;
      ++end;
    }
    for (size_t i = at + 4; i < end; ++i) {
      if (i >= at + 6 && stream[i-2] == 0 && stream[i-1] == 0 && stream[i] == 3) {
        Check(i+1 < end && stream[i+1] <= 3, "canonical escape byte");
        continue;
      }
      unit.bytes.push_back(stream[i]);
    }
    result.push_back(std::move(unit)); at = end;
  }
  return result;
}

struct OwnedSlice {
  VASliceParameterBufferVC1 parameters{};
  Bytes data;
  vc::Slice View() const { return {parameters, data.data(), data.size()}; }
};
struct Picture {
  VAPictureParameterBufferVC1 p{};
  Bytes planes;
  std::vector<OwnedSlice> slices;
  std::vector<vc::Slice> Views() const {
    std::vector<vc::Slice> result;
    for (const auto &slice : slices) result.push_back(slice.View());
    return result;
  }
  bool Assemble(Bytes *out, bool headers = true, vc::SequenceOptions options = {3,5,31}) const {
    return vc::Assemble(p, planes.data(), planes.size(), Views(), options, headers, out);
  }
};
void Type(Writer &w, unsigned type) {
  // P=0, B=10, I=110, BI=1110, skipped-P=1111.
  const unsigned lengths[] = {3,1,2,4,4}, codes[] = {6,0,2,14,15};
  w.Put(lengths[type], codes[type]);
}
void Code012(Writer &w, unsigned value) { w.Put(value ? 2 : 1, value ? value + 1 : 0); }
Picture Make(unsigned type = 0, unsigned width = 48, unsigned height = 32,
             unsigned timing = 0, bool original_postproc_present = true) {
  Picture result;
  auto &p = result.p;
  p.forward_reference_picture = p.backward_reference_picture = p.inloop_decoded_picture = VA_INVALID_SURFACE;
  p.sequence_fields.bits.profile = 3;
  p.sequence_fields.bits.tfcntrflag = timing & 1;
  p.sequence_fields.bits.pulldown = (timing >> 1) & 1;
  p.sequence_fields.bits.finterpflag = (timing >> 2) & 1;
  p.coded_width = width; p.coded_height = height;
  p.entrypoint_fields.bits.loopfilter = 1; p.fast_uvmc_flag = 1;
  p.picture_fields.bits.picture_type = type;
  auto &q = p.pic_quantizer_fields.bits;
  q.quantizer = 1; q.pic_quantizer_scale = 10; q.pic_quantizer_type = 1;
  p.rounding_control = 1; p.post_processing = original_postproc_present ? 2 : 0;
  p.transform_fields.bits.transform_ac_codingset_idx1 = 1;
  p.transform_fields.bits.transform_ac_codingset_idx2 = 2;
  p.transform_fields.bits.intra_transform_dc_table = 1;
  if (type == 0 || type == 3) p.raw_coding.value = 1U << 5;
  if (type == 1) p.raw_coding.value = 1U << 2;
  if (type == 2) {
    p.raw_coding.value = (1U << 1) | (1U << 2);
    p.mv_fields.bits.mv_mode = VAMvMode1MvHalfPelBilinear;
    p.b_picture_fraction = 3;
  }
  Writer b; Type(b, type);
  if (timing & 1) b.Put(8, 0x97);
  if (timing & 2) b.Put(2, 2);
  if (type != 4) {
    b.Put(1, 1);
    if (timing & 4) b.Put(1, 1);
    if (type == 2) b.Put(3, 3);
    b.Put(5, 10); b.Put(1, 1); // Explicit quantizer; no HALFQP at index 10.
    if (original_postproc_present) b.Put(2, 2);
    if (type == 0 || type == 3) b.Put(5, 0); // RAW ACPRED.
    else {
      b.Put(1, type == 1 ? 1 : 0); // P MVMODE / B MVMODE.
      if (type == 2) b.Put(5, 0); // RAW DIRECTMB.
      b.Put(5, 0); b.Put(2, 0); b.Put(2, 0); // RAW SKIPMB, MV/CBP tables.
    }
    Code012(b, 1);
    if (type == 0 || type == 3) Code012(b, 2);
    b.Put(1, 1);
  }
  OwnedSlice slice;
  slice.parameters.macroblock_offset = b.position;
  if (type != 4) { b.Put(8, 0xa5); b.Put(8, 0x67); b.Put(13, 0x1c57); }
  slice.data = Escape(b.bytes);
  slice.parameters.slice_data_size = slice.data.size();
  slice.parameters.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
  result.slices.push_back(std::move(slice));
  return result;
}
void Bad(const Picture &picture) {
  Bytes output{0x12,0x34,0x56}, original = output;
  const auto before = picture.p;
  std::vector<Bytes> source;
  for (const auto &s : picture.slices) source.push_back(s.data);
  Check(!picture.Assemble(&output), "malformed picture must reject");
  Check(output == original, "rejection leaves destination unchanged");
  Check(!std::memcmp(&before, &picture.p, sizeof(before)), "rejection leaves picture state unchanged");
  for (size_t i = 0; i < source.size(); ++i)
    Check(source[i] == picture.slices[i].data, "rejection leaves compressed input unchanged");
}

void Pictures() {
  for (unsigned type = 0; type < 5; ++type)
    for (auto geometry : {std::pair<unsigned,unsigned>{2,2}, {48,32}, {1920,1080}, {1920,1088}})
      for (unsigned timing = 0; timing < 8; ++timing) {
        auto picture = Make(type, geometry.first, geometry.second, timing);
        Bytes out;
        Check(picture.Assemble(&out), "whole-frame progressive picture accepted");
        const auto units = Units(out);
        Check(units.size() == 3 && units[0].code == 0xf && units[1].code == 0xe && units[2].code == 0xd,
              "sequence, entrypoint, picture only; no implicit file EOS");
        Reader seq{units[0].bytes};
        seq.Equal(2,3); seq.Equal(3,3); seq.Equal(2,1); seq.Equal(3,5); seq.Equal(5,31); seq.Equal(1,1);
        seq.Equal(12,geometry.first/2-1); seq.Equal(12,geometry.second/2-1);
        seq.Equal(1,(timing>>1)&1); seq.Equal(1,0); seq.Equal(1,timing&1);
        seq.Equal(1,(timing>>2)&1); seq.Equal(1,1); seq.Equal(3,0);
        Reader entry{units[1].bytes};
        entry.Equal(4,0); entry.Equal(1,1); entry.Equal(1,1); entry.Equal(1,0); entry.Equal(2,0);
        entry.Equal(1,0); entry.Equal(1,0); entry.Equal(2,1); entry.Equal(3,0);
        // Synthetic source already uses explicit POSTPROCFLAG, so all original
        // header/payload bits must be unchanged, not merely the same bit count.
        Check(Escape(units[2].bytes) == picture.slices[0].data, "complete independent synthetic frame exact");
        Bytes no_headers;
        Check(picture.Assemble(&no_headers,false) && Units(no_headers).size() == 1,
              "caller controls restart headers");
      }
  ++groups;
}

void MissingPostprocFlag() {
  for (unsigned type = 0; type < 4; ++type) for (unsigned timing = 0; timing < 8; ++timing) {
    auto picture = Make(type,48,32,timing,false);
    Bytes out; Check(picture.Assemble(&out), "missing sequence POSTPROCFLAG reconstructed explicitly");
    const auto rebuilt = Units(out).back().bytes;
    const auto original = vc::detail::Unescape(picture.slices[0].data);
    const unsigned lengths[] = {3,1,2,4};
    const size_t prefix = lengths[type] + ((timing&1) ? 8 : 0) +
        ((timing&2) ? 2 : 0) + 1 + ((timing&4) ? 1 : 0) + (type==2 ? 3 : 0) + 6;
    Reader a{original}, b{rebuilt};
    while (a.position < prefix) Check(a.Get(1)==b.Get(1), "prefix before POSTPROC preserved");
    b.Equal(2,0); // Zero is a value, not evidence that POSTPROCFLAG was absent.
    while (a.position < original.size()*8)
      Check(a.Get(1)==b.Get(1), "unaligned header and MB tail preserved after insertion");
    Check(rebuilt.size()*8-b.position < 8, "only final byte alignment added");
    while (b.position < rebuilt.size()*8) b.Equal(1,0);
  }
  ++groups;
}

void Planes() {
  for (unsigned width : {1U,2U,3U,120U}) for (unsigned height : {1U,3U}) {
    VAPictureParameterBufferVC1 p{};
    p.coded_width=width*16; p.coded_height=height*16;
    const unsigned count=width*height;
    Bytes planes((count+1)/2);
    for(unsigned mb=0;mb<count;++mb) planes[mb/2] |= ((mb%3)+1)<<((mb&1)?0:4);
    std::vector<vc::Slice> slices;
    vc::detail::Picture picture{p,planes.data(),planes.size(),slices};
    for(unsigned slot=0;slot<3;++slot) {
      p.raw_coding.value=0; p.bitplane_present.value=1U<<5;
      vc::detail::Writer b; vc::detail::Plane(b,picture,5,slot); Reader r{b.bytes};
      r.Equal(1,0); r.Equal(3,2);
      for(unsigned y=0;y<height;++y) {
        bool any=false;
        for(unsigned x=0;x<width;++x) any |= (((y*width+x)%3+1)>>slot)&1;
        r.Equal(1,any);
        if(any) for(unsigned x=0;x<width;++x) r.Equal(1,(((y*width+x)%3+1)>>slot)&1);
      }
      Check(r.position==b.position,"ROWSKIP length and row crossing");
    }
    p.bitplane_present.value=0; p.raw_coding.value=1U<<5;
    vc::detail::Writer raw; vc::detail::Plane(raw,picture,5,1); Reader r{raw.bytes};
    r.Equal(5,0); Check(raw.position==5,"RAW has no plane data");
    p.bitplane_present.value=1U<<5;
    RejectDetail([&]{vc::detail::Writer b;vc::detail::Plane(b,picture,5,1);});
    p.raw_coding.value=p.bitplane_present.value=0;
    RejectDetail([&]{vc::detail::Writer b;vc::detail::Plane(b,picture,5,1);});
  }
  auto picture=Make(); picture.p.raw_coding.value=0; picture.p.bitplane_present.value=1U<<5;
  picture.planes={0x22,0x02,0x20}; Bytes out;
  Check(picture.Assemble(&out),"public expanded-plane path");
  picture.planes[0]|=0x80; Bad(picture);
  picture.planes[0]&=0x7f; picture.planes.pop_back(); Bad(picture);
  auto odd=Make(0,2,2); odd.p.raw_coding.value=0; odd.p.bitplane_present.value=1U<<5;
  odd.planes={0x20}; Check(odd.Assemble(&out),"one MB high nibble");
  odd.planes[0]|=2; Bad(odd);
  ++groups;
}

void Quantizers() {
  const unsigned implicit[32]={0,1,2,3,4,5,6,7,8,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,27,29,31};
  for(unsigned mode=0;mode<4;++mode) for(unsigned index=1;index<32;++index)
    for(unsigned half=0;half<(index<9?2U:1U);++half) for(unsigned kind=0;kind<(mode==1?2U:1U);++kind) {
      VAPictureParameterBufferVC1 p{}; auto &q=p.pic_quantizer_fields.bits;
      q.quantizer=mode; q.pic_quantizer_scale=mode?index:implicit[index]; q.half_qp=half;
      q.pic_quantizer_type=mode==0?(index<9):mode==1?kind:mode==3;
      Check(vc::detail::PqIndex(p)==index,"quantizer inverse including duplicate implicit scales");
    }
  for(unsigned missing : {0U,26U,28U,30U}) {
    VAPictureParameterBufferVC1 p{}; p.pic_quantizer_fields.bits.pic_quantizer_scale=missing;
    RejectDetail([&]{(void)vc::detail::PqIndex(p);});
  }
  const unsigned primary[2][5]={{2,0,1,4,3},{0,3,1,4,2}};
  const unsigned secondary[2][4]={{2,0,1,3},{0,3,1,2}};
  for(unsigned row=0;row<2;++row) {
    for(unsigned n=0;n<5;++n) Check(vc::detail::ModeIndex(primary[row][n],row?12:13,false)==n,"primary motion inverse");
    for(unsigned n=0;n<4;++n) Check(vc::detail::ModeIndex(secondary[row][n],row?12:13,true)==n,"secondary motion inverse");
  }
  for(unsigned profile=0;profile<4;++profile) {
    VAPictureParameterBufferVC1 p{}; auto &q=p.pic_quantizer_fields.bits;
    q.dquant=1; q.dq_frame=1; q.dq_profile=profile; q.dq_db_edge=1; q.dq_sb_edge=2;
    q.dq_binary_level=1; q.alt_pic_quantizer=23;
    vc::detail::Writer b; vc::detail::Dquant(b,p); Reader r{b.bytes};
    r.Equal(1,1); r.Equal(2,profile);
    if(profile==1) r.Equal(2,1);
    if(profile==2) r.Equal(2,2);
    if(profile==3) r.Equal(1,1);
    r.Equal(3,7); r.Equal(5,23); Check(r.position==b.position,"explicit alternate QP");
  }
  ++groups;
}

void Escaping() {
  for(unsigned tail=0;tail<256;++tail) {
    Bytes in{0,0,static_cast<uint8_t>(tail),0,0,3,3,0,0,0,0,1,0,0,2};
    Check(vc::detail::Escape(in)==Escape(in),"independent escape oracle");
    Check(vc::detail::Unescape(Escape(in))==in,"canonical escape roundtrip");
  }
  for(const Bytes &bad : {Bytes{0,0,0},Bytes{0,0,1},Bytes{0,0,2},Bytes{0,0,3},Bytes{0,0,3,4}})
    RejectDetail([&]{(void)vc::detail::Unescape(bad);});
  for(unsigned head=0;head<8;++head) for(unsigned offset=0;offset<24;++offset) {
    Bytes in{0xad,0x76,0xb9}; vc::detail::Writer b; b.Put(head,0); b.Tail(in,offset); Reader r{b.bytes};
    if(head) r.Equal(head,0);
    for(unsigned bit=offset;bit<24;++bit) r.Equal(1,(in[bit/8]>>(7-bit%8))&1);
    Check(b.position==head+24-offset,"no alignment before macroblock tail");
  }
  ++groups;
}

OwnedSlice Continuation(const Picture &picture, unsigned row, bool repeated) {
  const auto &first=picture.slices.front();
  const Bytes plain=vc::detail::Unescape(first.data);
  Writer b; b.Put(9,row); b.Put(1,repeated);
  if(repeated) b.Copy(plain,0,first.parameters.macroblock_offset);
  OwnedSlice result;
  result.parameters.macroblock_offset=b.position;
  b.Put(13,0x15e3); b.Put(8,0x72);
  result.data=Escape(b.bytes);
  result.parameters.slice_data_size=result.data.size();
  result.parameters.slice_data_flag=VA_SLICE_DATA_FLAG_ALL;
  result.parameters.slice_vertical_position=row;
  return result;
}
void MultiSlice() {
  for(unsigned type : {0U,1U,2U,3U}) for(unsigned timing=0;timing<8;++timing) {
    auto picture=Make(type,48,64,timing);
    picture.slices.push_back(Continuation(picture,1,false));
    picture.slices.push_back(Continuation(picture,3,true));
    Bytes out; Check(picture.Assemble(&out),"headerless plus proven identical repeated-header slices");
    const auto units=Units(out);
    Check(units.size()==5 && units[3].code==0xb && units[4].code==0xb,"slice BDU count/code");
    for(unsigned s=1;s<3;++s)
      Check(Escape(units[s+2].bytes)==picture.slices[s].data,"slice header and tail exact");
    auto bad=picture; bad.slices[2].data[1]^=0x20; Bad(bad); // Header bit after row/header flag.
    bad=picture; bad.slices[2].parameters.macroblock_offset--; Bad(bad);
    bad=picture; bad.slices[1].parameters.macroblock_offset++; Bad(bad);
    bad=picture; bad.slices[2].parameters.slice_vertical_position=1; Bad(bad);
    bad=picture; bad.slices[2].parameters.slice_vertical_position=4; Bad(bad);
    bad=picture; bad.slices[1].data[0]^=1; Bad(bad);
  }
  ++groups;
}

void InvalidAndInactive() {
  for(unsigned n=0;n<47;++n) {
    auto v=Make(); auto &p=v.p; auto &s=v.slices[0];
    switch(n) {
      case 0:p.sequence_fields.bits.profile=1;break;
      case 1:p.sequence_fields.bits.interlace=1;break;
      case 2:p.sequence_fields.bits.psf=1;break;
      case 3:p.entrypoint_fields.bits.panscan_flag=1;break;
      case 4:p.picture_fields.bits.frame_coding_mode=1;break;
      case 5:p.picture_fields.bits.picture_type=7;break;
      case 6:p.coded_width=0;break;
      case 7:p.coded_height=1089;break;
      case 8:p.coded_width=1922;break;
      case 9:p.post_processing=4;break;
      case 10:p.fast_uvmc_flag=2;break;
      case 11:p.rounding_control=2;break;
      case 12:p.inloop_decoded_picture=0;break;
      case 13:p.range_mapping_fields.bits.luma_flag=1;break;
      case 14:p.range_mapping_fields.bits.chroma_flag=1;break;
      case 15:p.pic_quantizer_fields.bits.dquant=3;break;
      case 16:p.pic_quantizer_fields.bits.pic_quantizer_scale=0;break;
      case 17:p.pic_quantizer_fields.bits.half_qp=1;break;
      case 18:p.transform_fields.bits.transform_ac_codingset_idx1=3;break;
      case 19:p.transform_fields.bits.transform_ac_codingset_idx2=3;break;
      case 20:p.raw_coding.value=0;break;
      case 21:p.bitplane_present.value=1U<<5;break;
      case 22:p.bitplane_present.value=1U<<7;break;
      case 23:p.raw_coding.value|=1U<<7;break;
      case 24:p.sequence_fields.value|=1U<<14;break;
      case 25:p.entrypoint_fields.value|=1U<<4;break;
      case 26:p.range_mapping_fields.value|=1U<<8;break;
      case 27:p.picture_fields.value|=1U<<9;break;
      case 28:p.reference_fields.value|=1U<<8;break;
      case 29:p.mv_fields.value|=1U<<20;break;
      case 30:p.pic_quantizer_fields.value|=1U<<24;break;
      case 31:p.transform_fields.value|=1U<<9;break;
      case 32:p.va_reserved[0]=1;break;
      case 33:s.parameters.va_reserved[0]=1;break;
      case 34:s.parameters.slice_data_size=0;break;
      case 35:s.parameters.slice_data_size=UINT32_MAX;break;
      case 36:s.parameters.slice_data_offset=UINT32_MAX;break;
      case 37:s.parameters.macroblock_offset=UINT32_MAX;break;
      case 38:s.parameters.slice_data_flag=VA_SLICE_DATA_FLAG_BEGIN;break;
      case 39:s.parameters.slice_vertical_position=1;break;
      case 40:s.data.resize(1);break;
      case 41:s.data[0]^=0x80;break;
      case 42:v.slices.clear();break;
      case 43:v.planes={0};break;
      case 44:s.data.insert(s.data.end(),{0,0,3});s.parameters.slice_data_size=s.data.size();break;
      case 45:s.data.insert(s.data.end(),{0,0,1,13});s.parameters.slice_data_size=s.data.size();break;
      case 46:p.rounding_control^=1;break;
    }
    Bad(v);
  }
  auto stale=Make(4); // FFmpeg may retain all these inactive per-picture values.
  stale.p.raw_coding.value=0x7f;
  stale.p.pic_quantizer_fields.bits.pic_quantizer_scale=0;
  stale.p.post_processing=255; stale.p.rounding_control=255;
  stale.p.mv_fields.bits.mv_mode=7; stale.p.mv_fields.bits.mv_table=7;
  stale.p.transform_fields.bits.transform_ac_codingset_idx1=3;
  stale.p.conditional_overlap_flag=255; Bytes out;
  Check(stale.Assemble(&out),"inactive skipped-P fields are not interpreted");
  auto intra=Make(); intra.p.mv_fields.bits.mv_mode=7; intra.p.mv_fields.bits.mv_table=7;
  intra.p.cbp_table=255; intra.p.luma_scale=255; intra.p.conditional_overlap_flag=255;
  intra.p.raw_coding.value|=0x1f;
  Check(intra.Assemble(&out),"inactive intra motion/raw flags may retain earlier values");
  for(unsigned kind=0;kind<5;++kind) {
    auto p=Make(2);
    if(kind==0)p.p.b_picture_fraction=21;
    if(kind==1)p.p.b_picture_fraction=22;
    if(kind==2)p.p.mv_fields.bits.mv_mode=VAMvModeMixedMv;
    if(kind==3)p.p.mv_fields.bits.mv_table=4;
    if(kind==4)p.p.cbp_table=4;
    Bad(p);
  }
  ++groups;
}

void RangesAndTransaction() {
  auto picture=Make(); Bytes out{1,2,3};
  Check(!picture.Assemble(nullptr),"null output rejected");
  for(auto options : {vc::SequenceOptions{2,5,31}, {3,8,31}, {3,5,32}}) {
    Check(!picture.Assemble(&out,true,options) && out==Bytes({1,2,3}),"explicit sequence policy range");
  }
  auto slices=picture.Views(); slices[0].data=nullptr;
  Check(!vc::Assemble(picture.p,nullptr,0,slices,{3,5,31},true,&out) && out==Bytes({1,2,3}),"null slice range");
  auto planes=picture; planes.p.raw_coding.value=0; planes.p.bitplane_present.value=1U<<5;
  Check(!vc::Assemble(planes.p,nullptr,3,planes.Views(),{3,5,31},true,&out),"null nonempty planes");
  Bytes expected; Check(picture.Assemble(&expected),"unshifted reference");
  auto shifted=picture; auto &s=shifted.slices[0];
  s.data.insert(s.data.begin(),17,0xa5); s.data.insert(s.data.end(),13,0x5a); s.parameters.slice_data_offset=17;
  Check(shifted.Assemble(&out) && out==expected,"borrowed range excludes prefix/suffix allocation bytes");
  // Output may alias the source vector: commit only after all borrowed reads.
  auto alias=picture;
  Check(alias.Assemble(&alias.slices[0].data) && alias.slices[0].data==expected,"transaction supports output/source vector alias");
  auto large=picture; auto &data=large.slices[0];
  data.data.resize(vc::kMaxAccessUnitBytes-64,0x55); data.parameters.slice_data_size=data.data.size();
  Check(large.Assemble(&out),"near-limit complete access unit");
  data.data.resize(vc::kMaxAccessUnitBytes,0x55); data.parameters.slice_data_size=data.data.size(); Bad(large);
  data.data.push_back(0x55); data.parameters.slice_data_size=data.data.size(); Bad(large);
  // Escaping expansion is independently bounded, not just original byte count.
  vc::detail::Writer bits; bits.bytes.assign(vc::kMaxAccessUnitBytes,0);
  Bytes unchanged{1};
  RejectDetail([&]{vc::detail::Bdu(unchanged,0xd,bits);});
  Check(unchanged==Bytes({1}),"failed BDU insertion cannot append a partial unit");
  ++groups;
}
} // namespace

int main() {
  try {
    Pictures(); MissingPostprocFlag(); Planes(); Quantizers(); Escaping(); MultiSlice(); InvalidAndInactive(); RangesAndTransaction();
    std::printf("PASS %u groups / %u checks (VC-1 Advanced assembly, no hardware)\n",groups,checks);
    return 0;
  } catch(const std::exception &e) {
    std::fprintf(stderr,"FAIL after %u groups / %u checks: %s\n",groups,checks,e.what());
    return 1;
  } catch(const vc::detail::Invalid &) {
    std::fprintf(stderr,"FAIL after %u groups / %u checks: unexpected syntax rejection\n",groups,checks);
    return 1;
  }
}
