// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_VC1_H
#define CRYSTALHD_VC1_H

// Progressive VC-1 Advanced elementary-stream assembly. Reference ownership,
// stable sequence/entry-point configuration and restart policy belong to the
// caller. This is not a complete compressed-macroblock syntax validator.
// Syntax: FFmpeg n7.1.1 vc1.c, vc1data.c and vaapi_vc1.c; libva VC-1 buffers.
#include <va/va.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <vector>

namespace crystalhd_vc1 {

// Matches the backend replay access-unit bound, not a universal codec limit.
constexpr size_t kMaxAccessUnitBytes = 512 * 1024;
struct SequenceOptions {
  unsigned level;
  unsigned frame_rate_hint; // FRMRTQ_POSTPROC, not display frame rate.
  unsigned bit_rate_hint;   // BITRTQ_POSTPROC, not stream bit rate.
};
struct Slice {
  VASliceParameterBufferVC1 parameters;
  const uint8_t *data;
  size_t size;
};

namespace detail {
using Bytes = std::vector<uint8_t>;
struct Invalid {};
inline void Need(bool yes, const char *) { if (!yes) throw Invalid{}; }
constexpr size_t kLimit = kMaxAccessUnitBytes;
struct Picture {
  const VAPictureParameterBufferVC1 &parameters;
  const uint8_t *bitplanes;
  size_t bitplane_size;
  const std::vector<Slice> &slices;
};

class Reader {
  const Bytes &data_;
 public:
  size_t position = 0;
  explicit Reader(const Bytes &data) : data_(data) {}
  unsigned Get(unsigned bits) {
    Need(bits <= 32 && position <= data_.size()*8 && bits <= data_.size()*8-position,
         "truncated input bits");
    unsigned value = 0;
    while (bits--) { value = (value<<1) | ((data_[position/8]>>(7-position%8))&1); ++position; }
    return value;
  }
  unsigned Unary(unsigned stop, unsigned max) {
    unsigned n = 0; while (n < max && Get(1) != stop) ++n; return n;
  }
};
class Writer {
 public:
  Bytes bytes;
  size_t position = 0;
  void Put(unsigned bits, unsigned value) {
    Need(bits <= 32 && (bits == 32 || uint64_t(value) < (uint64_t(1)<<bits)), "output field range");
    Need(position + bits <= kLimit*8, "assembled access unit limit");
    while (bits--) {
      if (!(position&7)) bytes.push_back(0);
      bytes.back() |= ((value>>bits)&1) << (7-(position&7)); ++position;
    }
  }
  void Unary(unsigned value, unsigned stop, unsigned max) {
    Need(value <= max, "unary range");
    for (unsigned n=0;n<value;++n) Put(1,!stop);
    if (value < max) Put(1,stop);
  }
  void Code012(unsigned value) {
    Need(value <= 2,"012 range"); if (!value) Put(1,0); else { Put(1,1); Put(1,value-1); }
  }
  void Tail(const Bytes &source, size_t offset) {
    Need(offset <= source.size()*8,"macroblock offset out of bounds");
    Reader r(source); r.position = offset;
    while (r.position < source.size()*8) Put(1,r.Get(1));
  }
};

inline Bytes Unescape(const Bytes &source) {
  Bytes result; unsigned zeros=0;
  for (size_t n=0;n<source.size();++n) {
    if (zeros>=2 && source[n]<=3) {
      Need(source[n]==3 && n+1<source.size() && source[n+1]<=3,
           "noncanonical escaping or embedded BDU marker");
      zeros=0; continue;
    }
    result.push_back(source[n]);
    zeros=source[n]?0:zeros+1;
  }
  return result;
}
inline Bytes Escape(const Bytes &source) {
  Bytes result; unsigned zeros=0;
  for (uint8_t byte : source) {
    if (zeros>=2 && byte<=3) { result.push_back(3); zeros=0; }
    result.push_back(byte); zeros = byte ? 0 : zeros+1;
  }
  return result;
}
inline void Bdu(Bytes &out, unsigned code, const Writer &bits) {
  auto data = Escape(bits.bytes);
  Need(out.size() <= kLimit && data.size()+4 <= kLimit-out.size(),"escaped access unit limit");
  out.insert(out.end(),{0,0,1,static_cast<uint8_t>(code)});
  out.insert(out.end(),data.begin(),data.end());
}
inline void Sequence(Bytes &out, const VAPictureParameterBufferVC1 &p, const SequenceOptions &o) {
  Writer b;
  b.Put(2,3); b.Put(3,o.level); b.Put(2,1); // Advanced, explicit level, 4:2:0.
  b.Put(3,o.frame_rate_hint); b.Put(5,o.bit_rate_hint); b.Put(1,1); // Always POSTPROCFLAG.
  b.Put(12,p.coded_width/2-1); b.Put(12,p.coded_height/2-1);
  b.Put(1,p.sequence_fields.bits.pulldown); b.Put(1,0);
  b.Put(1,p.sequence_fields.bits.tfcntrflag); b.Put(1,p.sequence_fields.bits.finterpflag);
  b.Put(1,1); b.Put(1,0); // Reserved, PSF off.
  b.Put(1,0); b.Put(1,0); // No invented display extension or HRD model.
  Bdu(out,0x0f,b);
}
inline void Entry(Bytes &out, const VAPictureParameterBufferVC1 &p) {
  Writer b;
  b.Put(1,p.entrypoint_fields.bits.broken_link); b.Put(1,p.entrypoint_fields.bits.closed_entry);
  b.Put(1,0); b.Put(1,p.reference_fields.bits.reference_distance_flag);
  b.Put(1,p.entrypoint_fields.bits.loopfilter); b.Put(1,p.fast_uvmc_flag);
  b.Put(1,p.mv_fields.bits.extended_mv_flag); b.Put(2,p.pic_quantizer_fields.bits.dquant);
  b.Put(1,p.transform_fields.bits.variable_sized_transform_flag);
  b.Put(1,p.sequence_fields.bits.overlap); b.Put(2,p.pic_quantizer_fields.bits.quantizer);
  b.Put(1,0); // Sequence already supplies this fixed coded geometry.
  if (p.mv_fields.bits.extended_mv_flag) b.Put(1,p.mv_fields.bits.extended_dmv_flag);
  b.Put(1,p.range_mapping_fields.bits.luma_flag);
  if (p.range_mapping_fields.bits.luma_flag) b.Put(3,p.range_mapping_fields.bits.luma);
  b.Put(1,p.range_mapping_fields.bits.chroma_flag);
  if (p.range_mapping_fields.bits.chroma_flag) b.Put(3,p.range_mapping_fields.bits.chroma);
  Bdu(out,0x0e,b);
}

inline unsigned PlaneBit(const Picture &picture, unsigned mb, unsigned slot) {
  // Actual FFmpeg + i965 ordering, opposite to the old libva prose comment.
  return (picture.bitplanes[mb/2] >> ((mb&1 ? 0 : 4)+slot))&1;
}
inline void Plane(Writer &b, const Picture &picture, unsigned flag, unsigned slot) {
  const auto &p=picture.parameters;
  const bool raw=(p.raw_coding.value>>flag)&1;
  const bool present=(p.bitplane_present.value>>flag)&1;
  Need(raw != present,"active bitplane must be RAW or supplied");
  b.Put(1,0); // INVERT: already applied to expanded non-raw VA planes.
  if (raw) { b.Put(4,0); return; } // RAW leaves per-MB syntax untouched.
  b.Put(3,2); // IMODE_ROWSKIP = 010.
  const unsigned w=(p.coded_width+15)/16, h=(p.coded_height+15)/16;
  for (unsigned y=0;y<h;++y) {
    bool any=false;
    for (unsigned x=0;x<w;++x) any |= PlaneBit(picture,y*w+x,slot);
    b.Put(1,any);
    if (any) for (unsigned x=0;x<w;++x) b.Put(1,PlaneBit(picture,y*w+x,slot));
  }
}
inline unsigned PqScale(unsigned mode, unsigned index) {
  static constexpr uint8_t implicit[32]={0,1,2,3,4,5,6,7,8,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,27,29,31};
  return mode==0 ? implicit[index] : index;
}
inline unsigned PqType(unsigned mode, unsigned index, unsigned explicit_type) {
  return mode==0 ? index<9 : mode==1 ? explicit_type : mode==2 ? 0 : 1;
}
inline unsigned PqIndex(const VAPictureParameterBufferVC1 &p) {
  const auto &q=p.pic_quantizer_fields.bits;
  Need(!(q.dquant && q.dquant!=2 && q.dq_frame && q.dq_profile==3 &&
         !q.dq_binary_level && q.half_qp),"all-MB non-bilevel DQUANT clears half-QP");
  for (unsigned n=1;n<32;++n)
    if (PqScale(q.quantizer,n)==q.pic_quantizer_scale &&
        PqType(q.quantizer,n,q.pic_quantizer_type)==q.pic_quantizer_type &&
        (n<9 || !q.half_qp)) return n;
  throw Invalid{};
}
inline void Dquant(Writer &b,const VAPictureParameterBufferVC1 &p) {
  const auto &q=p.pic_quantizer_fields.bits;
  if (q.dquant!=2) {
    b.Put(1,q.dq_frame); if (!q.dq_frame) return;
    b.Put(2,q.dq_profile);
    if (q.dq_profile==1) b.Put(2,q.dq_db_edge);
    if (q.dq_profile==2) b.Put(2,q.dq_sb_edge);
    if (q.dq_profile==3) { b.Put(1,q.dq_binary_level); if (!q.dq_binary_level) return; }
  }
  Need(q.alt_pic_quantizer>0,"zero alternate quantizer");
  // Explicit alternate QP avoids any ambiguity in the differential form.
  b.Put(3,7); b.Put(5,q.alt_pic_quantizer);
}
inline void Transform(Writer &b,const VAPictureParameterBufferVC1 &p) {
  const auto &t=p.transform_fields.bits;
  if (t.variable_sized_transform_flag) {
    b.Put(1,t.mb_level_transform_type_flag);
    if (t.mb_level_transform_type_flag) b.Put(2,t.frame_level_transform_type);
  }
}
inline unsigned ModeIndex(unsigned mode,unsigned pq,bool secondary) {
  static constexpr unsigned primary[2][5]={
    {VAMvMode1MvHalfPelBilinear,VAMvMode1Mv,VAMvMode1MvHalfPel,VAMvModeIntensityCompensation,VAMvModeMixedMv},
    {VAMvMode1Mv,VAMvModeMixedMv,VAMvMode1MvHalfPel,VAMvModeIntensityCompensation,VAMvMode1MvHalfPelBilinear}};
  static constexpr unsigned second[2][4]={
    {VAMvMode1MvHalfPelBilinear,VAMvMode1Mv,VAMvMode1MvHalfPel,VAMvModeMixedMv},
    {VAMvMode1Mv,VAMvModeMixedMv,VAMvMode1MvHalfPel,VAMvMode1MvHalfPelBilinear}};
  const unsigned row=pq<=12, count=secondary?4:5;
  for(unsigned n=0;n<count;++n) if((secondary?second[row][n]:primary[row][n])==mode) return n;
  throw Invalid{};
}
inline unsigned Fraction(Reader &r) {
  unsigned n=r.Get(3); if(n==7) n+=r.Get(4);
  Need(n!=21,"invalid B fraction"); return n;
}
inline void Fraction(Writer &b,unsigned n) {
  Need(n<=22 && n!=21,"invalid B fraction");
  if(n<7) b.Put(3,n); else { b.Put(3,7); b.Put(4,n-7); }
}
inline void Frame(Bytes &out,const Picture &picture,const Bytes &plain,
                  const VASliceParameterBufferVC1 &s, bool continuation) {
  const auto &p=picture.parameters;
  const auto &q=p.pic_quantizer_fields.bits;
  const auto &m=p.mv_fields.bits;
  const auto &t=p.transform_fields.bits;
  const unsigned type=p.picture_fields.bits.picture_type;
  Reader r(plain); Writer b;
  if (continuation) {
    const unsigned row = r.Get(9);
    Need(row == s.slice_vertical_position, "slice row mismatch");
    b.Put(9, row);
    const unsigned repeated = r.Get(1);
    b.Put(1, repeated);
    if (!repeated) {
      Need(s.macroblock_offset == 10, "headerless slice offset");
      b.Tail(plain, s.macroblock_offset);
      Bdu(out, 0x0b, b);
      return;
    }
  }
  const unsigned original_type=r.Unary(0,4);
  static constexpr unsigned ptype_code[5]={2,0,1,3,4};
  Need(original_type==ptype_code[type],"original PTYPE disagrees with VA");
  b.Unary(ptype_code[type],0,4);
  if(p.sequence_fields.bits.tfcntrflag) b.Put(8,r.Get(8));
  if(p.sequence_fields.bits.pulldown) b.Put(2,r.Get(2));
  if(type==4) { Need(r.position==s.macroblock_offset,"skipped frame header offset"); Bdu(out,continuation ? 0x0b : 0x0d,b); return; }
  Need(r.Get(1)==p.rounding_control,"rounding control mismatch"); b.Put(1,p.rounding_control);
  if(p.sequence_fields.bits.finterpflag) b.Put(1,r.Get(1));
  if(type==2) { Need(Fraction(r)==p.b_picture_fraction,"B fraction mismatch"); Fraction(b,p.b_picture_fraction); }
  const unsigned original_pq=r.Get(5);
  Need(original_pq && PqScale(q.quantizer,original_pq)==q.pic_quantizer_scale,"original QP mismatch");
  const unsigned original_half=original_pq<9?r.Get(1):0;
  const unsigned original_type_q=q.quantizer==1?r.Get(1):PqType(q.quantizer,original_pq,0);
  Need(original_type_q==q.pic_quantizer_type,"original quantizer type mismatch");
  const bool resets_half=q.dquant && q.dquant!=2 && q.dq_frame && q.dq_profile==3 && !q.dq_binary_level;
  Need(resets_half || original_half==q.half_qp,"original half-QP mismatch");
  Need(r.position<=s.macroblock_offset,"header exceeds supplied macroblock offset");
  const unsigned pqindex=PqIndex(p);
  b.Put(5,pqindex); if(pqindex<9) b.Put(1,q.half_qp);
  if(q.quantizer==1) b.Put(1,q.pic_quantizer_type);
  b.Put(2,p.post_processing); // Explicitly present, even when zero.
  if(type==0 || type==3) {
    Plane(b,picture,5,1);
    if(p.sequence_fields.bits.overlap && q.pic_quantizer_scale<=8) {
      b.Code012(p.conditional_overlap_flag);
      if(p.conditional_overlap_flag==2) Plane(b,picture,6,2);
    }
  } else {
    if(m.extended_mv_flag) b.Unary(m.extended_mv_range,0,3);
    if(type==1) {
      b.Unary(ModeIndex(m.mv_mode,q.pic_quantizer_scale,false),1,4);
      if(m.mv_mode==4) {
        b.Unary(ModeIndex(m.mv_mode2,q.pic_quantizer_scale,true),1,3);
        b.Put(6,p.luma_scale); b.Put(6,p.luma_shift);
      }
      if(m.mv_mode==3 || (m.mv_mode==4 && m.mv_mode2==3)) Plane(b,picture,0,2);
      Plane(b,picture,2,1);
    } else {
      Need(m.mv_mode==VAMvMode1MvHalfPelBilinear || m.mv_mode==VAMvMode1Mv,"progressive B motion mode");
      b.Put(1,m.mv_mode==VAMvMode1Mv); Plane(b,picture,1,0); Plane(b,picture,2,1);
    }
    b.Put(2,m.mv_table); b.Put(2,p.cbp_table);
    if(q.dquant) Dquant(b,p);
    Transform(b,p);
  }
  b.Code012(t.transform_ac_codingset_idx1);
  if(type==0 || type==3) b.Code012(t.transform_ac_codingset_idx2);
  b.Put(1,t.intra_transform_dc_table);
  if((type==0 || type==3) && q.dquant) Dquant(b,p);
  b.Tail(plain,s.macroblock_offset); // Deliberately no alignment at this splice.
  Bdu(out,continuation ? 0x0b : 0x0d,b);
}
// Only reserved bits are rejected unconditionally. FFmpeg legitimately retains
// inactive motion/quantizer/raw-plane fields from preceding pictures.
inline void Validate(const Picture &picture, const SequenceOptions &o) {
  const auto &p = picture.parameters;
  Need(p.sequence_fields.bits.profile == 3 &&
       !p.sequence_fields.bits.interlace && !p.sequence_fields.bits.psf &&
       !p.entrypoint_fields.bits.panscan_flag &&
       p.picture_fields.bits.frame_coding_mode == 0 &&
       p.picture_fields.bits.picture_type <= 4, "unsupported picture mode");
  Need(p.coded_width && p.coded_height && !(p.coded_width & 1) &&
       !(p.coded_height & 1) && p.coded_width <= 1920 &&
       p.coded_height <= 1088, "unsupported geometry");
  // Level 3 is the measured FHD subset; do not invent a lower-level policy.
  Need(o.level == 3 && o.frame_rate_hint <= 7 && o.bit_rate_hint <= 31,
       "unsupported sequence policy");
  Need(p.inloop_decoded_picture == VA_INVALID_SURFACE &&
       !p.range_mapping_fields.bits.luma_flag &&
       !p.range_mapping_fields.bits.chroma_flag, "unsupported output processing");
  Need(!(p.sequence_fields.value & ~0x3fffU) &&
       !(p.entrypoint_fields.value & ~0xfU) &&
       !(p.range_mapping_fields.value & ~0xffU) &&
       !(p.picture_fields.value & ~0x1ffU) &&
       !(p.reference_fields.value & ~0xffU) &&
       !(p.mv_fields.value & ~0xfffffU) &&
       !(p.pic_quantizer_fields.value & ~0xffffffU) &&
       !(p.transform_fields.value & ~0x1ffU) &&
       !(p.raw_coding.value & ~0x7fU), "reserved picture bits");
  for (uint32_t word : p.va_reserved) Need(!word, "reserved picture bytes");
  Need(p.fast_uvmc_flag <= 1, "FASTUVMC range");

  const unsigned type = p.picture_fields.bits.picture_type;
  const auto &q = p.pic_quantizer_fields.bits;
  const auto &m = p.mv_fields.bits;
  const auto &t = p.transform_fields.bits;
  Need(q.dquant <= 2, "reserved DQUANT mode");
  unsigned active = 0;
  if (type != 4) {
    Need(p.post_processing <= 3 && p.rounding_control <= 1 &&
         t.transform_ac_codingset_idx1 <= 2, "active picture scalar");
    (void)PqIndex(p);
    if (type == 0 || type == 3) {
      active = 1U << 5;
      Need(t.transform_ac_codingset_idx2 <= 2, "intra AC table");
      if (p.sequence_fields.bits.overlap && q.pic_quantizer_scale <= 8) {
        Need(p.conditional_overlap_flag <= 2, "conditional overlap");
        if (p.conditional_overlap_flag == 2) active |= 1U << 6;
      }
    } else {
      Need(p.cbp_table <= 3 && m.mv_table <= 3, "progressive VLC table");
      active = 1U << 2;
      if (type == 1) {
        (void)ModeIndex(m.mv_mode, q.pic_quantizer_scale, false);
        if (m.mv_mode == VAMvModeIntensityCompensation) {
          (void)ModeIndex(m.mv_mode2, q.pic_quantizer_scale, true);
          Need(p.luma_scale <= 63 && p.luma_shift <= 63, "intensity compensation");
        }
        if (m.mv_mode == VAMvModeMixedMv ||
            (m.mv_mode == VAMvModeIntensityCompensation && m.mv_mode2 == VAMvModeMixedMv))
          active |= 1U << 0;
      } else {
        Need(p.b_picture_fraction <= 20 &&
             (m.mv_mode == VAMvMode1Mv || m.mv_mode == VAMvMode1MvHalfPelBilinear),
             "progressive B picture syntax");
        active |= 1U << 1;
      }
    }
  }
  Need(!(p.bitplane_present.value & ~active), "inactive supplied bitplane");
  Need(((p.raw_coding.value ^ p.bitplane_present.value) & active) == active,
       "each active bitplane must be RAW or supplied");
  const size_t mbs = size_t((p.coded_width + 15) / 16) * ((p.coded_height + 15) / 16);
  const size_t bytes = p.bitplane_present.value ? (mbs + 1) / 2 : 0;
  Need(picture.bitplane_size == bytes && (!bytes || picture.bitplanes),
       "bitplane buffer extent");
  unsigned slots = 0;
  if (type == 0 || type == 3)
    slots = ((p.bitplane_present.value >> 5) & 1) * 2 +
            ((p.bitplane_present.value >> 6) & 1) * 4;
  if (type == 1)
    slots = ((p.bitplane_present.value >> 2) & 1) * 2 +
            (p.bitplane_present.value & 1) * 4;
  if (type == 2)
    slots = ((p.bitplane_present.value >> 1) & 1) +
            ((p.bitplane_present.value >> 2) & 1) * 2;
  for (size_t n = 0; n < bytes; ++n) {
    Need(!(picture.bitplanes[n] & ~((slots << 4) | slots)), "bitplane nibble");
    if (n + 1 == bytes && (mbs & 1))
      Need(!(picture.bitplanes[n] & 15), "odd-MB padding nibble");
  }
  const unsigned rows = (p.coded_height + 15) / 16;
  Need(!picture.slices.empty() && picture.slices.size() <= rows &&
       (type != 4 || picture.slices.size() == 1), "slice count");
  size_t total = 0;
  unsigned last_row = 0;
  for (size_t n = 0; n < picture.slices.size(); ++n) {
    const auto &slice = picture.slices[n];
    const auto &v = slice.parameters;
    for (uint32_t word : v.va_reserved) Need(!word, "reserved slice bytes");
    Need(slice.data && v.slice_data_flag == VA_SLICE_DATA_FLAG_ALL &&
         v.slice_data_size && v.slice_data_offset <= slice.size &&
         v.slice_data_size <= slice.size - v.slice_data_offset &&
         v.slice_data_size <= kLimit - total, "slice association/range");
    total += v.slice_data_size;
    Need(n ? v.slice_vertical_position > last_row &&
                 v.slice_vertical_position < rows :
             v.slice_vertical_position == 0, "slice row ordering");
    last_row = v.slice_vertical_position;
  }
}
inline Bytes Assemble(const Picture &picture, const SequenceOptions &options,
                      bool emit_headers) {
  Validate(picture, options);
  Bytes result, first_header;
  const unsigned type = picture.parameters.picture_fields.bits.picture_type;
  const size_t first_offset = picture.slices[0].parameters.macroblock_offset;
  if (emit_headers) {
    Sequence(result, picture.parameters, options);
    Entry(result, picture.parameters);
  }
  for (size_t n = 0; n < picture.slices.size(); ++n) {
    const auto &slice = picture.slices[n];
    const auto &s = slice.parameters;
    Bytes escaped(slice.data + s.slice_data_offset,
                  slice.data + s.slice_data_offset + s.slice_data_size);
    Bytes plain = Unescape(escaped);
    Need(s.macroblock_offset <= plain.size() * 8 &&
         (type == 4 || s.macroblock_offset < plain.size() * 8),
         "unescaped macroblock range");
    if (!n) first_header = plain;
    else {
      Reader r(plain);
      Need(r.Get(9) == s.slice_vertical_position, "encoded slice row");
      if (r.Get(1)) {
        // One supplied VA picture cannot describe changed per-slice headers.
        // Accept a repeat only after proving the complete original header is
        // identical, including fields absent from VA (e.g. POSTPROCFLAG).
        Need(s.macroblock_offset == first_offset + 10, "changed slice header size");
        Reader first(first_header);
        while (r.position < s.macroblock_offset)
          Need(r.Get(1) == first.Get(1), "changed repeated picture header");
      } else Need(s.macroblock_offset == 10, "headerless slice offset");
    }
    Frame(result, picture, plain, s, n != 0);
  }
  return result;
}
} // namespace detail

// On rejection (including malformed/unsupported data and allocation failure),
// output and all caller-owned inputs remain unchanged. No file EOS BDU is
// appended: decoder drain is an explicit caller operation. Slice payloads are
// original escaped VC-1 bytes without the four-byte BDU marker; macroblock_offset
// addresses the unescaped payload. Headerless continuations and byte-identical
// repeated picture headers are supported; changed repeated headers are rejected.
inline bool Assemble(const VAPictureParameterBufferVC1 &parameters,
                     const uint8_t *bitplanes, size_t bitplane_size,
                     const std::vector<Slice> &slices,
                     const SequenceOptions &options, bool emit_headers,
                     std::vector<uint8_t> *output) {
  if (!output) return false;
  try {
    auto bytes = detail::Assemble(
        {parameters, bitplanes, bitplane_size, slices}, options, emit_headers);
    output->swap(bytes);
    return true;
  } catch (const detail::Invalid &) {
    return false;
  } catch (const std::exception &) {
    return false;
  }
}
} // namespace crystalhd_vc1
#endif
