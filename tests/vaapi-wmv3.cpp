// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../filters/vaapi/crystalhd-wmv3.h"

#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

namespace w = crystalhd_wmv3;
namespace {
unsigned checks = 0;
void Check(bool condition, const char *message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
struct Bits {
  std::vector<uint8_t> bytes;
  size_t position = 0;
  void Put(unsigned count, unsigned value) {
    for (unsigned bit = count; bit; --bit) {
      if (!(position % 8)) bytes.push_back(0);
      bytes.back() |= ((value >> (bit - 1)) & 1) << (7 - position % 8);
      ++position;
    }
  }
  void Code012(unsigned value) { Put(value ? 2 : 1, value ? value + 1 : 0); }
};
struct Read {
  const std::array<uint8_t, 4> &bytes;
  unsigned position = 0;
  unsigned Get(unsigned count) {
    Check(position + count <= 32, "metadata reader bound");
    unsigned value = 0;
    while (count--) {
      value = value * 2 + ((bytes[position / 8] >> (7 - position % 8)) & 1);
      ++position;
    }
    return value;
  }
  void Is(unsigned count, unsigned expected) { Check(Get(count) == expected, "metadata field roundtrip"); }
};
struct Input {
  VAPictureParameterBufferVC1 p = {};
  std::vector<uint8_t> bytes, planes;
  VASliceParameterBufferVC1 slice = {};
  std::vector<w::Slice> Slices() const { return {{slice, bytes.data(), bytes.size()}}; }
};
unsigned Scale(unsigned mode, unsigned index) {
  if (mode || index < 9) return index;
  if (index < 29) return index - 3;
  return index * 2 - 31;
}
Input Make(unsigned count = 0, unsigned type = 0, unsigned quantizer = 0,
           unsigned index = 5, unsigned half = 0, unsigned explicit_kind = 0,
           unsigned flags = 0) {
  Input in;
  auto &p = in.p;
  p.forward_reference_picture = p.backward_reference_picture = p.inloop_decoded_picture = VA_INVALID_SURFACE;
  p.coded_width = p.coded_height = 16;
  p.sequence_fields.bits.profile = 1;
  p.sequence_fields.bits.max_b_frames = count;
  p.sequence_fields.bits.finterpflag = flags & 1;
  p.sequence_fields.bits.rangered = (flags >> 1) & 1;
  p.range_reduction_frame = p.sequence_fields.bits.rangered;
  p.mv_fields.bits.extended_mv_flag = (flags >> 2) & 1;
  p.mv_fields.bits.extended_mv_range = p.mv_fields.bits.extended_mv_flag ? 3 : 0;
  p.picture_fields.bits.picture_type = type;
  p.picture_fields.bits.is_first_field = 1;
  p.rounding_control = type == 0 || type == 3;
  p.b_picture_fraction = type == 3 ? 22 : 2;
  p.pic_quantizer_fields.bits.quantizer = quantizer;
  p.pic_quantizer_fields.bits.pic_quantizer_scale = Scale(quantizer, index);
  p.pic_quantizer_fields.bits.pic_quantizer_type = quantizer == 0 ? index < 9 :
      quantizer == 1 ? explicit_kind : quantizer == 3;
  p.pic_quantizer_fields.bits.half_qp = half;
  p.transform_fields.bits.transform_ac_codingset_idx1 = 1;
  p.transform_fields.bits.transform_ac_codingset_idx2 = 2;
  p.transform_fields.bits.intra_transform_dc_table = 1;
  if (type == 1) p.raw_coding.flags.skip_mb = 1;
  if (type == 2) p.raw_coding.flags.direct_mb = p.raw_coding.flags.skip_mb = 1;
  Bits b;
  if (p.sequence_fields.bits.finterpflag) b.Put(1, 1);
  b.Put(2, 3); // Deliberately nonzero FRMCNT must survive unchanged.
  if (p.sequence_fields.bits.rangered) b.Put(1, p.range_reduction_frame);
  if (type == 1) b.Put(1, 1);
  else {
    b.Put(1, 0);
    if (count) b.Put(1, type == 0);
  }
  if (type == 2 || type == 3) {
    if (type == 3) { b.Put(3, 7); b.Put(4, 15); }
    else b.Put(3, 2);
  }
  if (type == 0 || type == 3) b.Put(7, 93);
  b.Put(5, index);
  if (index < 9) b.Put(1, half);
  if (quantizer == 1) b.Put(1, explicit_kind);
  if (p.mv_fields.bits.extended_mv_flag) b.Put(3, 7); // maximal unary MVRANGE.
  if (type == 0 || type == 3) {
    b.Code012(1); b.Code012(2); b.Put(1, 1);
  } else {
    // Synthetic opaque P/B header suffix: these unit tests check transport and
    // prefix contracts, not decoded macroblocks. Real capture pixel tests are
    // a separate gate; the helper must never rewrite this suffix.
    if (type == 1 && p.pic_quantizer_fields.bits.pic_quantizer_scale > 12)
      b.Put(2, 1); // Higher-QP primary mode table: 01 means 1MV.
    else b.Put(1, 1); // Lower-QP P unary=0, or B's explicit 1MV bit.
    b.Put(25, 0x123456);
  }
  in.slice.macroblock_offset = b.position;
  b.Put(24, 0x123456); // No alignment at the picture/macroblock boundary.
  in.bytes = std::move(b.bytes);
  in.slice.slice_data_size = in.bytes.size();
  in.slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
  return in;
}
void Metadata(const Input &in, const std::array<uint8_t, 4> &metadata) {
  const auto &p = in.p;
  Read r{metadata};
  r.Is(2, p.sequence_fields.bits.profile); r.Is(2, 0);
  r.Is(3, 5); r.Is(5, 31);
  r.Is(1, p.entrypoint_fields.bits.loopfilter); r.Is(1, 0); r.Is(1, 0); r.Is(1, 1);
  r.Is(1, p.fast_uvmc_flag); r.Is(1, p.mv_fields.bits.extended_mv_flag);
  r.Is(2, p.pic_quantizer_fields.bits.dquant);
  r.Is(1, p.transform_fields.bits.variable_sized_transform_flag); r.Is(1, 0);
  r.Is(1, p.sequence_fields.bits.overlap); r.Is(1, p.sequence_fields.bits.syncmarker);
  r.Is(1, p.sequence_fields.bits.rangered); r.Is(3, p.sequence_fields.bits.max_b_frames);
  r.Is(2, p.pic_quantizer_fields.bits.quantizer); r.Is(1, p.sequence_fields.bits.finterpflag); r.Is(1, 1);
  Check(r.position == 32, "exact four-byte STRUCT_C");
}
void Accept(const Input &in, VAProfile profile = VAProfileVC1Main) {
  std::vector<uint8_t> output{99};
  std::array<uint8_t, 4> metadata{{99, 98, 97, 96}};
  Check(w::Assemble(in.p, profile, in.planes.data(), in.planes.size(), in.Slices(), &output, &metadata),
        "supported standard picture accepted");
  const auto first = in.bytes.begin() + in.slice.slice_data_offset;
  Check(output == std::vector<uint8_t>(first, first + in.slice.slice_data_size), "whole packet remains byte-exact");
  Metadata(in, metadata);
}
void Reject(const Input &in, VAProfile profile = VAProfileVC1Main) {
  const std::vector<uint8_t> before{0xde, 0xad, 0xbe, 0xef};
  const std::array<uint8_t, 4> old{{1, 2, 3, 4}};
  auto output = before;
  auto metadata = old;
  Check(!w::Assemble(in.p, profile, in.planes.data(), in.planes.size(), in.Slices(), &output, &metadata),
        "unsupported/malformed picture rejected");
  Check(output == before && metadata == old, "failed call leaves both outputs unchanged");
}
void LegalContracts() {
  for (unsigned count = 0; count < 8; ++count)
    for (unsigned type = 0; type < (count ? 4U : 2U); ++type) Accept(Make(count, type));
  for (unsigned flags = 0; flags < 8; ++flags)
    for (unsigned type = 0; type < 4; ++type) Accept(Make(2, type, 0, 5, 1, 0, flags));
  for (unsigned mode = 0; mode < 4; ++mode)
    for (unsigned index = 1; index < 32; ++index)
      for (unsigned half = 0; half < (index < 9 ? 2U : 1U); ++half)
        for (unsigned kind = 0; kind < (mode == 1 ? 2U : 1U); ++kind)
          for (unsigned type : {0U, 1U, 2U, 3U}) Accept(Make(3, type, mode, index, half, kind));
  for (unsigned type : {0U, 1U}) {
    auto in = Make(0, type);
    in.p.sequence_fields.bits.profile = 0;
    in.p.fast_uvmc_flag = 1;
    Accept(in, VAProfileVC1Simple);
  }
  auto flags = Make();
  flags.p.entrypoint_fields.bits.loopfilter = 1;
  flags.p.sequence_fields.bits.overlap = flags.p.sequence_fields.bits.syncmarker = 1;
  flags.p.fast_uvmc_flag = 1;
  flags.p.transform_fields.bits.variable_sized_transform_flag = 1;
  Accept(flags);
  for (unsigned type : {1U, 2U}) {
    auto in = Make(1, type, 0, 5, 1);
    auto &q = in.p.pic_quantizer_fields.bits;
    q.dquant = 1; q.dq_frame = 1; q.dq_profile = 3;
    q.dq_binary_level = 0; q.half_qp = 0;
    Accept(in); // FFmpeg clears effective HALFQP for this later DQUANT mode.
    q.half_qp = 1; Reject(in);
  }
}
void MalformedAndTransactional() {
  for (unsigned variant = 0; variant < 37; ++variant) {
    auto in = Make(); auto &p = in.p; auto &s = in.slice;
    switch (variant) {
      case 0: p.sequence_fields.bits.profile = 3; break;
      case 1: p.coded_width = 0; break;
      case 2: p.coded_height = 1089; break;
      case 3: p.coded_width = 1922; break;
      case 4: p.sequence_fields.bits.pulldown = 1; break;
      case 5: p.sequence_fields.bits.interlace = 1; break;
      case 6: p.sequence_fields.bits.tfcntrflag = 1; break;
      case 7: p.sequence_fields.bits.psf = 1; break;
      case 8: p.sequence_fields.bits.multires = 1; break;
      case 9: p.picture_resolution_index = 1; break;
      case 10: p.picture_fields.bits.frame_coding_mode = 1; break;
      case 11: p.picture_fields.bits.picture_type = 4; break;
      case 12: p.inloop_decoded_picture = 0; break;
      case 13: p.entrypoint_fields.bits.panscan_flag = 1; break;
      case 14: p.range_mapping_fields.bits.luma_flag = 1; break;
      case 15: p.mv_fields.bits.extended_dmv_flag = 1; break;
      case 16: p.post_processing = 1; break;
      case 17: p.fast_uvmc_flag = 2; break;
      case 18: p.rounding_control = 2; break;
      case 19: p.range_reduction_frame = 1; break;
      case 20: p.pic_quantizer_fields.bits.pic_quantizer_scale = 0; break;
      case 21: p.pic_quantizer_fields.bits.dquant = 3; break;
      case 22: p.sequence_fields.value |= 1U << 31; break;
      case 23: p.raw_coding.value |= 1U << 31; break;
      case 24: p.bitplane_present.value |= 1U << 31; break;
      case 25: s.slice_data_flag = VA_SLICE_DATA_FLAG_BEGIN; break;
      case 26: s.slice_vertical_position = 1; break;
      case 27: s.slice_data_size = 0; break;
      case 28: s.slice_data_size = std::numeric_limits<uint32_t>::max(); break;
      case 29: s.slice_data_offset = std::numeric_limits<uint32_t>::max(); break;
      case 30: s.macroblock_offset = std::numeric_limits<uint32_t>::max(); break;
      case 31: ++s.macroblock_offset; break; // legacy X8-style extra header bit.
      case 32: --s.macroblock_offset; break;
      case 33: in.bytes.resize(1); break;
      case 34: p.pic_quantizer_fields.bits.half_qp = 1; break;
      case 35: p.pic_quantizer_fields.bits.pic_quantizer_type ^= 1; break;
      case 36: p.transform_fields.bits.transform_ac_codingset_idx1 = 2; break;
    }
    Reject(in);
  }
  Reject(Make(), VAProfileVC1Advanced);
  Reject(Make(0, 2)); Reject(Make(0, 3));
  for (unsigned variant = 0; variant < 8; ++variant) {
    auto in = Make(); in.p.sequence_fields.bits.profile = 0; in.p.fast_uvmc_flag = 1;
    switch (variant) {
      case 0: in.p.sequence_fields.bits.max_b_frames = 1; break;
      case 1: in.p.entrypoint_fields.bits.loopfilter = 1; break;
      case 2: in.p.fast_uvmc_flag = 0; break;
      case 3: in.p.mv_fields.bits.extended_mv_flag = 1; break;
      case 4: in.p.sequence_fields.bits.rangered = 1; break;
      case 5: in.p.pic_quantizer_fields.bits.dquant = 1; break;
      case 6: in.p.sequence_fields.bits.syncmarker = 1; break;
      case 7: in.p.picture_fields.bits.picture_type = 2; break;
    }
    Reject(in, VAProfileVC1Simple);
  }
  auto in = Make(); std::vector<uint8_t> out{7}; std::array<uint8_t, 4> meta{{1, 2, 3, 4}};
  Check(!w::Assemble(in.p, VAProfileVC1Main, nullptr, 0, {}, &out, &meta), "no slices");
  auto two = in.Slices(); two.push_back(two.front());
  Check(!w::Assemble(in.p, VAProfileVC1Main, nullptr, 0, two, &out, &meta), "multiple slices");
  auto null = in.Slices(); null[0].data = nullptr;
  Check(!w::Assemble(in.p, VAProfileVC1Main, nullptr, 0, null, &out, &meta), "null data");
  Check(!w::Assemble(in.p, VAProfileVC1Main, nullptr, 0, in.Slices(), nullptr, &meta), "null output");
  Check(!w::Assemble(in.p, VAProfileVC1Main, nullptr, 0, in.Slices(), &out, nullptr), "null metadata");
  Check(out == std::vector<uint8_t>{7} && meta == std::array<uint8_t, 4>{{1, 2, 3, 4}}, "early failures transactional");
}
void AssociationAndPlanes() {
  auto offset = Make();
  offset.bytes.insert(offset.bytes.begin(), 17, 0xa5);
  offset.bytes.insert(offset.bytes.end(), 9, 0x5a);
  offset.slice.slice_data_offset = 17;
  Accept(offset);
  for (unsigned type : {1U, 2U}) {
    auto in = Make(1, type);
    in.p.raw_coding.value = 0;
    in.p.bitplane_present.value = type == 1 ? 4 : 6;
    in.planes = {0x20}; // one MB, high nibble first; odd padding stays zero.
    Accept(in);
    in.planes[0] |= 1; Reject(in); in.planes[0] = 0x20;
    in.planes[0] |= 0x80; Reject(in); in.planes[0] = 0x20;
    in.planes.clear(); Reject(in); in.planes = {0x20};
    in.p.raw_coding.flags.skip_mb = 1; Reject(in);
    in.p.raw_coding.value = in.p.bitplane_present.value = 0; in.planes.clear(); Reject(in);
  }
  auto mixed = Make(1, 1);
  mixed.p.mv_fields.bits.mv_mode = VAMvModeMixedMv;
  mixed.p.raw_coding.flags.mv_type_mb = 1;
  Reject(mixed); // VA mode disagrees with the preserved original header.
  mixed.p.raw_coding.flags.mv_type_mb = 0; Reject(mixed);
}
} // namespace
int main() {
  try {
    LegalContracts(); MalformedAndTransactional(); AssociationAndPlanes();
    std::printf("WMV3 assembly: PASS %u checks\n", checks);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "WMV3 assembly: FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
