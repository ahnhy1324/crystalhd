// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_WMV3_H
#define CRYSTALHD_WMV3_H

#include <va/va.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Standard VC-1 Simple/Main STRUCT_C plus complete WMV3 picture packets.
// This helper does not advertise profiles, track reference surfaces/rounding
// history, or decide decoder replay/EOS policy. No start codes or RCV framing
// belong in its returned payload.
namespace crystalhd_wmv3 {
constexpr size_t kMaxAccessUnitBytes = 512 * 1024;

struct Slice {
  VASliceParameterBufferVC1 parameters;
  const uint8_t *data;
  size_t size;
};

namespace detail {
class Reader {
 public:
  Reader(const uint8_t *data, size_t bytes) : data_(data), bits_(bytes * 8) {}
  unsigned Get(unsigned count) {
    if (count > 32 || count > bits_ - position) { good = false; return 0; }
    unsigned value = 0;
    while (count--) {
      value = (value << 1) | ((data_[position / 8] >> (7 - position % 8)) & 1U);
      ++position;
    }
    return value;
  }
  unsigned Unary(unsigned stop, unsigned maximum) {
    unsigned value = 0;
    while (good && value < maximum && Get(1) != stop) ++value;
    return value;
  }
  unsigned Code012() { return Get(1) ? 1 + Get(1) : 0; }
  bool good = true;
  size_t position = 0;
 private:
  const uint8_t *data_;
  size_t bits_;
};

class MetadataBits {
 public:
  void Put(unsigned count, unsigned value) {
    while (count--) {
      bytes[position_ / 8] |= ((value >> count) & 1U) << (7 - position_ % 8);
      ++position_;
    }
  }
  std::array<uint8_t, 4> bytes = {};
 private:
  unsigned position_ = 0;
};

inline bool ValidPicture(const VAPictureParameterBufferVC1 &p, VAProfile profile) {
  const bool simple = profile == VAProfileVC1Simple;
  const auto &s = p.sequence_fields.bits;
  const auto &q = p.pic_quantizer_fields.bits;
  if ((!simple && profile != VAProfileVC1Main) || s.profile != (simple ? 0U : 1U) ||
      !p.coded_width || !p.coded_height || p.coded_width > 1920 || p.coded_height > 1088 ||
      ((p.coded_width | p.coded_height) & 1U) || s.pulldown || s.interlace || s.tfcntrflag ||
      s.psf || s.multires || p.picture_resolution_index ||
      p.picture_fields.bits.frame_coding_mode || p.picture_fields.bits.picture_type > 3 ||
      p.inloop_decoded_picture != VA_INVALID_SURFACE ||
      p.entrypoint_fields.bits.broken_link || p.entrypoint_fields.bits.closed_entry ||
      p.entrypoint_fields.bits.panscan_flag || p.range_mapping_fields.value ||
      p.mv_fields.bits.extended_dmv_flag || p.mv_fields.bits.extended_dmv_range ||
      p.reference_fields.bits.reference_distance_flag || p.post_processing ||
      p.conditional_overlap_flag || p.fast_uvmc_flag > 1 || p.rounding_control > 1 ||
      p.range_reduction_frame > 1 || (!s.rangered && p.range_reduction_frame) ||
      q.pic_quantizer_scale == 0 || q.dquant == 3 ||
      (p.sequence_fields.value & ~0x3fffU) || (p.entrypoint_fields.value & ~15U) ||
      (p.raw_coding.value & ~0x7fU) || (p.bitplane_present.value & ~0x7fU))
    return false;
  const unsigned type = p.picture_fields.bits.picture_type;
  if ((type == 2 || type == 3) && !s.max_b_frames) return false;
  if (p.transform_fields.bits.transform_ac_codingset_idx1 > 2 ||
      ((type == 0 || type == 3) && p.transform_fields.bits.transform_ac_codingset_idx2 > 2) ||
      ((type == 1 || type == 2) && (p.cbp_table > 3 || p.mv_fields.bits.mv_table > 3)) ||
      (type == 1 && (p.mv_fields.bits.mv_mode > VAMvModeIntensityCompensation ||
       (p.mv_fields.bits.mv_mode == VAMvModeIntensityCompensation &&
        (p.mv_fields.bits.mv_mode2 > VAMvModeMixedMv || p.luma_scale > 63 || p.luma_shift > 63)))) ||
      (type == 2 && p.mv_fields.bits.mv_mode != VAMvMode1Mv &&
                    p.mv_fields.bits.mv_mode != VAMvMode1MvHalfPelBilinear))
    return false;
  if (simple && (s.max_b_frames || p.entrypoint_fields.bits.loopfilter ||
                 p.mv_fields.bits.extended_mv_flag || !p.fast_uvmc_flag || s.rangered ||
                 s.syncmarker || q.dquant)) return false;
  if ((type == 0 || type == 3) && p.rounding_control != 1) return false;
  return true;
}

inline std::array<uint8_t, 4> Metadata(const VAPictureParameterBufferVC1 &p) {
  MetadataBits b;
  const auto &s = p.sequence_fields.bits;
  b.Put(2, s.profile); b.Put(2, 0); // Standard reserved bits; no legacy sprite/Y411.
  // VA omits FRMRTQ/BITRTQ. These postprocessing hints are not media timestamps.
  b.Put(3, 5); b.Put(5, 31);
  b.Put(1, p.entrypoint_fields.bits.loopfilter); b.Put(1, 0); // X8 off.
  b.Put(1, 0); b.Put(1, 1); // MULTIRES off; standard FASTTX reserved bit.
  b.Put(1, p.fast_uvmc_flag); b.Put(1, p.mv_fields.bits.extended_mv_flag);
  b.Put(2, p.pic_quantizer_fields.bits.dquant);
  b.Put(1, p.transform_fields.bits.variable_sized_transform_flag);
  b.Put(1, 0); b.Put(1, s.overlap); b.Put(1, s.syncmarker); b.Put(1, s.rangered);
  b.Put(3, s.max_b_frames); b.Put(2, p.pic_quantizer_fields.bits.quantizer);
  b.Put(1, s.finterpflag); b.Put(1, 1); // Standard RTM reserved bit.
  return b.bytes;
}

inline bool ValidPlanes(const VAPictureParameterBufferVC1 &p,
                        const uint8_t *planes, size_t size) {
  const unsigned type = p.picture_fields.bits.picture_type;
  unsigned active = 0, slots = 0;
  if (type == 1) {
    active = 1U << 2;
    if (p.mv_fields.bits.mv_mode == VAMvModeMixedMv ||
        (p.mv_fields.bits.mv_mode == VAMvModeIntensityCompensation &&
         p.mv_fields.bits.mv_mode2 == VAMvModeMixedMv)) active |= 1U;
    slots = ((p.bitplane_present.value >> 2) & 1U) * 2 + (p.bitplane_present.value & 1U) * 4;
  } else if (type == 2) {
    active = (1U << 1) | (1U << 2);
    slots = ((p.bitplane_present.value >> 1) & 1U) + ((p.bitplane_present.value >> 2) & 1U) * 2;
  }
  if ((p.bitplane_present.value & ~active) ||
      ((p.bitplane_present.value ^ p.raw_coding.value) & active) != active)
    return false;
  const size_t mbs = size_t((p.coded_width + 15) / 16) * ((p.coded_height + 15) / 16);
  const size_t expected = p.bitplane_present.value ? (mbs + 1) / 2 : 0;
  if (size != expected || (size && !planes)) return false;
  // FFmpeg and i965 pack the first MB in the high nibble; reserved and absent
  // planes must not carry data. The compressed packet remains authoritative.
  for (size_t n = 0; n < size; ++n)
    if (planes[n] & ~((slots << 4) | slots)) return false;
  return !size || !(mbs & 1) || !(planes[size - 1] & 15);
}

inline bool ValidHeader(const VAPictureParameterBufferVC1 &p, const uint8_t *data,
                        size_t size, size_t macroblock_offset) {
  const auto &s = p.sequence_fields.bits;
  const auto &q = p.pic_quantizer_fields.bits;
  const unsigned wanted_type = p.picture_fields.bits.picture_type;
  Reader r(data, size);
  if (s.finterpflag) r.Get(1); // INTERPFRM and FRMCNT are absent from VA metadata.
  r.Get(2);
  if (s.rangered && r.Get(1) != p.range_reduction_frame) return false;
  unsigned type = r.Get(1) ? 1 : (s.max_b_frames && !r.Get(1) ? 2 : 0);
  if (type == 2) {
    unsigned fraction = r.Get(3);
    if (fraction == 7) fraction += r.Get(4);
    if (fraction == 21 || fraction != p.b_picture_fraction) return false;
    if (fraction == 22) type = 3;
  }
  if (type != wanted_type) return false;
  if (type == 0 || type == 3) r.Get(7); // BF: buffer fullness, not a picture token.
  const unsigned index = r.Get(5);
  static constexpr uint8_t implicit[32] = {
      0,1,2,3,4,5,6,7,8,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,27,29,31};
  if (!index || (q.quantizer ? index : implicit[index]) != q.pic_quantizer_scale) return false;
  const unsigned half = index < 9 ? r.Get(1) : 0;
  const unsigned kind = !q.quantizer ? index < 9 : q.quantizer == 1 ? r.Get(1) : q.quantizer == 3;
  const bool resets_half = (type == 1 || type == 2) && q.dquant && q.dquant != 2 &&
      q.dq_frame && q.dq_profile == 3 && !q.dq_binary_level;
  if (kind != q.pic_quantizer_type || (!resets_half && half != q.half_qp) ||
      (resets_half && q.half_qp)) return false;
  if (p.mv_fields.bits.extended_mv_flag && r.Unary(0, 3) != p.mv_fields.bits.extended_mv_range)
    return false;
  if (type == 0 || type == 3) {
    if (r.Code012() != p.transform_fields.bits.transform_ac_codingset_idx1 ||
        r.Code012() != p.transform_fields.bits.transform_ac_codingset_idx2 ||
        r.Get(1) != p.transform_fields.bits.intra_transform_dc_table)
      return false;
    // Canonical I/BI headers end here. In particular, no legacy X8 bit is
    // silently inserted/removed or mistaken for the macroblock payload.
    return r.good && r.position == macroblock_offset;
  }
  if (type == 1) {
    static constexpr unsigned modes[2][5] = {
        {VAMvMode1MvHalfPelBilinear, VAMvMode1Mv, VAMvMode1MvHalfPel,
         VAMvModeIntensityCompensation, VAMvModeMixedMv},
        {VAMvMode1Mv, VAMvModeMixedMv, VAMvMode1MvHalfPel,
         VAMvModeIntensityCompensation, VAMvMode1MvHalfPelBilinear}};
    static constexpr unsigned secondary[2][4] = {
        {VAMvMode1MvHalfPelBilinear, VAMvMode1Mv, VAMvMode1MvHalfPel, VAMvModeMixedMv},
        {VAMvMode1Mv, VAMvModeMixedMv, VAMvMode1MvHalfPel, VAMvMode1MvHalfPelBilinear}};
    const unsigned row = q.pic_quantizer_scale <= 12;
    if (modes[row][r.Unary(1, 4)] != p.mv_fields.bits.mv_mode) return false;
    if (p.mv_fields.bits.mv_mode == VAMvModeIntensityCompensation &&
        (secondary[row][r.Unary(1, 3)] != p.mv_fields.bits.mv_mode2 ||
         r.Get(6) != p.luma_scale || r.Get(6) != p.luma_shift)) return false;
  } else if ((r.Get(1) ? VAMvMode1Mv : VAMvMode1MvHalfPelBilinear) != p.mv_fields.bits.mv_mode) {
    return false;
  }
  // P/B mode/bitplane/table syntax is already present in the original packet.
  // Do not normalize it or treat the VA macroblock offset as a byte offset.
  return r.good && r.position < macroblock_offset;
}
} // namespace detail

// Transactional: neither output changes on validation failure. Supports one
// complete standard Simple/Main WMV3 packet, not MSS2, sprites, old X8/FASTTX0
// variants, multiresolution or Advanced-profile BDUs. VA does not convey the
// original reserved FASTTX/RTM bits: their legacy alternatives cannot be
// identified by this API and are outside its standard-STRUCT_C contract.
inline bool Assemble(const VAPictureParameterBufferVC1 &picture, VAProfile profile,
                     const uint8_t *bitplanes, size_t bitplane_size,
                     const std::vector<Slice> &slices, std::vector<uint8_t> *payload,
                     std::array<uint8_t, 4> *metadata) {
  if (!payload || !metadata || !detail::ValidPicture(picture, profile) ||
      !detail::ValidPlanes(picture, bitplanes, bitplane_size) || slices.size() != 1)
    return false;
  const auto &slice = slices.front();
  const auto &s = slice.parameters;
  if (!slice.data || s.slice_data_flag != VA_SLICE_DATA_FLAG_ALL || s.slice_vertical_position ||
      !s.slice_data_size || s.slice_data_size > kMaxAccessUnitBytes ||
      s.slice_data_offset > slice.size || s.slice_data_size > slice.size - s.slice_data_offset ||
      s.macroblock_offset >= size_t(s.slice_data_size) * 8)
    return false;
  const uint8_t *data = slice.data + s.slice_data_offset;
  if (!detail::ValidHeader(picture, data, s.slice_data_size, s.macroblock_offset)) return false;
  const auto next_metadata = detail::Metadata(picture);
  std::vector<uint8_t> next_payload(data, data + s.slice_data_size);
  payload->swap(next_payload);
  *metadata = next_metadata;
  return true;
}
} // namespace crystalhd_wmv3
#endif
