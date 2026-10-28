/*
 * H.264 Annex-B access-unit and parameter-set construction for CrystalHD.
 *
 * The parameter-set reconstruction follows the ChromiumOS libva fake
 * driver's BSD-licensed H.264 decoder delegate.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef CRYSTALHD_H264_H
#define CRYSTALHD_H264_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

#include <va/va.h>

#if defined(__GNUC__)
#pragma GCC visibility push(hidden)
#endif

namespace crystalhd_h264 {

class BitWriter {
 public:
  void PutBit(bool value) {
    if (bit_offset_ == 0)
      bytes_.push_back(0);
    bytes_.back() |= static_cast<uint8_t>(value) << (7 - bit_offset_);
    bit_offset_ = (bit_offset_ + 1) & 7;
  }

  void PutBits(unsigned int count, uint64_t value) {
    for (unsigned int bit = count; bit > 0; --bit)
      PutBit(((value >> (bit - 1)) & 1U) != 0);
  }

  void PutUE(uint32_t value) {
    uint64_t code = static_cast<uint64_t>(value) + 1;
    unsigned int bits = 0;
    for (uint64_t temp = code; temp != 0; temp >>= 1)
      ++bits;
    for (unsigned int i = 1; i < bits; ++i)
      PutBit(false);
    PutBits(bits, code);
  }

  void PutSE(int32_t value) {
    PutUE(value <= 0 ? static_cast<uint32_t>(-2LL * value)
                     : static_cast<uint32_t>(2LL * value - 1));
  }

  std::vector<uint8_t> FinishRbsp() {
    PutBit(true);
    while (bit_offset_ != 0)
      PutBit(false);
    return std::move(bytes_);
  }

 private:
  std::vector<uint8_t> bytes_;
  unsigned int bit_offset_ = 0;
};

inline void AppendNal(std::vector<uint8_t> *output, uint8_t type,
                      uint8_t reference_idc, std::vector<uint8_t> rbsp) {
  static constexpr uint8_t start_code[] = {0, 0, 0, 1};
  output->insert(output->end(), std::begin(start_code), std::end(start_code));
  output->push_back(static_cast<uint8_t>((reference_idc << 5) | type));

  unsigned int zero_count = 0;
  for (uint8_t byte : rbsp) {
    if (zero_count >= 2 && byte <= 3) {
      output->push_back(3);
      zero_count = 0;
    }
    output->push_back(byte);
    zero_count = byte == 0 ? zero_count + 1 : 0;
  }
}

inline void BeginAccessUnit(std::vector<uint8_t> *output) {
  // H.264 7.4.1.2.3 puts AUD first in its access unit. Keep that boundary in
  // the same logical input as its picture. A prefed timestamp is consumed by
  // submitting this complete AU (including its own AUD) with timestamp zero;
  // a trailing AUD in the previous AU would instead start the next picture and
  // misassociate firmware PTS. A subsequent AU or explicit sealed-batch EOS
  // delimits the last slice.
  // primary_pic_type=7 permits all slice types, followed by rbsp_stop_one_bit.
  AppendNal(output, 9, 0, {0xf0});
}

inline bool SupportedH264PictureFormat(
    const VAPictureParameterBufferH264 &picture) {
  // These fields describe input syntax, not the negotiated NV12 output.
  // The reconstructed SPS and this decoder path support only 8-bit 4:2:0.
  return picture.seq_fields.bits.chroma_format_idc == 1 &&
         !picture.seq_fields.bits.residual_colour_transform_flag &&
         picture.bit_depth_luma_minus8 == 0 &&
         picture.bit_depth_chroma_minus8 == 0;
}

inline bool ValidateH264ProcessingGeometry(
    unsigned int coded_width, unsigned int coded_height,
    unsigned int visible_width, unsigned int visible_height,
    unsigned int capture_width, unsigned int capture_height) {
  // The card accepts a scale width and derives its height from the source
  // aspect ratio. Admit only an exact ratio here: a rounded storage height
  // cannot be repaired by clipping rows after the card has resampled them.
  // The caller supplies an explicit origin-zero visible rectangle; 1080 must
  // never be inferred from a 1088-line coded picture.
  return coded_width != 0 && coded_width <= 1920 &&
         coded_height != 0 && coded_height <= 1088 &&
         ((coded_width | coded_height) & 15U) == 0 &&
         visible_width == coded_width && visible_height != 0 &&
         visible_height <= coded_height &&
         ((visible_width | visible_height) & 1U) == 0 &&
         capture_width >= 128 && capture_width < visible_width &&
         capture_height != 0 && capture_height <= visible_height &&
         ((capture_width | capture_height) & 1U) == 0 &&
         static_cast<uint64_t>(visible_height) * capture_width ==
             static_cast<uint64_t>(capture_height) * visible_width;
}

inline bool BuildSps(const VAPictureParameterBufferH264 &picture,
                     VAProfile profile, std::vector<uint8_t> *output,
                     unsigned int visible_width = 0,
                     unsigned int visible_height = 0) {
  if (!SupportedH264PictureFormat(picture))
    return false;
  BitWriter bits;
  uint8_t profile_idc;
  unsigned int height_in_map_units;
  unsigned int crop_bottom = 0;

  if (visible_width != 0 || visible_height != 0) {
    const unsigned int coded_width =
        (picture.picture_width_in_mbs_minus1 + 1U) * 16U;
    const unsigned int coded_height =
        (picture.picture_height_in_mbs_minus1 + 1U) * 16U;
    const unsigned int crop_unit_y =
        picture.seq_fields.bits.frame_mbs_only_flag ? 2U : 4U;
    if (coded_width > 1920 || coded_height > 1088 ||
        visible_width != coded_width || visible_height == 0 ||
        visible_height > coded_height ||
        (coded_height - visible_height) % crop_unit_y != 0)
      return false;
    crop_bottom = (coded_height - visible_height) / crop_unit_y;
  }

  switch (profile) {
    case VAProfileH264ConstrainedBaseline:
      profile_idc = 66;
      break;
    case VAProfileH264Main:
      profile_idc = 77;
      break;
    case VAProfileH264High:
      profile_idc = 100;
      break;
    default:
      return false;
  }

  // VA reports the decoded frame height in macroblocks. H.264 SPS syntax
  // instead counts map units, where an interlaced sequence has two
  // macroblock rows per map unit (7.4.2.1.1). Passing the VA value through
  // unchanged advertises twice the coded height for MBAFF/field sequences and
  // leaves the firmware waiting for a picture that can never complete.
  height_in_map_units = picture.picture_height_in_mbs_minus1 + 1U;
  if (!picture.seq_fields.bits.frame_mbs_only_flag) {
    if ((height_in_map_units & 1U) != 0)
      return false;
    height_in_map_units /= 2U;
  }

  bits.PutBits(8, profile_idc);
  bits.PutBits(8, (profile == VAProfileH264ConstrainedBaseline ||
                   profile == VAProfileH264Main)
                      ? 0x40
                      : 0);
  const unsigned int macroblocks =
      (picture.picture_width_in_mbs_minus1 + 1) *
      (picture.picture_height_in_mbs_minus1 + 1);
  bits.PutBits(8, macroblocks <= 3600 ? 31 : 40);
  bits.PutUE(0);

  if (profile_idc == 100) {
    bits.PutUE(1);
    bits.PutUE(0);
    bits.PutUE(0);
    bits.PutBit(false);
    bits.PutBit(false);
  }

  bits.PutUE(picture.seq_fields.bits.log2_max_frame_num_minus4);
  bits.PutUE(picture.seq_fields.bits.pic_order_cnt_type);
  if (picture.seq_fields.bits.pic_order_cnt_type == 0) {
    bits.PutUE(picture.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
  } else if (picture.seq_fields.bits.pic_order_cnt_type == 1) {
    return false;
  }
  bits.PutUE(picture.num_ref_frames);
  bits.PutBit(picture.seq_fields.bits.gaps_in_frame_num_value_allowed_flag);
  bits.PutUE(picture.picture_width_in_mbs_minus1);
  bits.PutUE(height_in_map_units - 1U);
  bits.PutBit(picture.seq_fields.bits.frame_mbs_only_flag);
  if (!picture.seq_fields.bits.frame_mbs_only_flag)
    bits.PutBit(picture.seq_fields.bits.mb_adaptive_frame_field_flag);
  bits.PutBit(picture.seq_fields.bits.direct_8x8_inference_flag);
  bits.PutBit(crop_bottom != 0);
  if (crop_bottom != 0) {
    bits.PutUE(0);
    bits.PutUE(0);
    bits.PutUE(0);
    bits.PutUE(crop_bottom);
  }
  bits.PutBit(true);
  bits.PutBit(true);
  bits.PutBits(8, 1);
  bits.PutBit(false);
  bits.PutBit(false);
  bits.PutBit(false);
  bits.PutBit(true);
  // The browser extension rejects formats above 30 fps. Describe that exact
  // cadence to CrystalHD instead of the old 25 fps VUI: a mismatched timing
  // signal makes the firmware's output cadence fight Chromium's presentation
  // clock, which is observed as regular frame dropping and short stalls.
  bits.PutBits(32, 1);
  bits.PutBits(32, 60);
  bits.PutBit(true);
  bits.PutBit(false);
  bits.PutBit(false);
  bits.PutBit(false);
  bits.PutBit(true);
  bits.PutBit(true);
  bits.PutUE(0);
  bits.PutUE(0);
  bits.PutUE(11);
  bits.PutUE(11);
  bits.PutUE(0);
  bits.PutUE(picture.num_ref_frames);
  AppendNal(output, 7, 3, bits.FinishRbsp());
  return true;
}

inline VAIQMatrixBufferH264 FlatH264IqMatrix() {
  VAIQMatrixBufferH264 matrix = {};
  std::memset(matrix.ScalingList4x4, 16, sizeof(matrix.ScalingList4x4));
  std::memset(matrix.ScalingList8x8, 16, sizeof(matrix.ScalingList8x8));
  return matrix;
}

inline bool ValidateH264IqMatrix(const VAPictureParameterBufferH264 &picture,
                                 VAProfile profile,
                                 const VAIQMatrixBufferH264 &matrix,
                                 bool *flat = nullptr) {
  bool all_flat = true;
  const auto valid_list = [&all_flat](const uint8_t *list, size_t size) {
    for (size_t i = 0; i < size; ++i) {
      if (list[i] == 0)
        return false;
      all_flat &= list[i] == 16;
    }
    return true;
  };
  for (const auto &list : matrix.ScalingList4x4) {
    if (!valid_list(list, sizeof(list)))
      return false;
  }
  if (picture.pic_fields.bits.transform_8x8_mode_flag) {
    for (const auto &list : matrix.ScalingList8x8) {
      if (!valid_list(list, sizeof(list)))
        return false;
    }
  }
  if (profile != VAProfileH264High && !all_flat)
    return false;
  if (flat != nullptr)
    *flat = all_flat;
  return true;
}

template <size_t Size>
inline void PutH264ScalingList(BitWriter *bits,
                               const uint8_t (&matrix)[Size],
                               const uint8_t (&scan)[Size]) {
  bits->PutBit(true);
  int last = 8;
  for (uint8_t position : scan) {
    const int next = matrix[position];
    int delta = next - last;
    if (delta > 127)
      delta -= 256;
    else if (delta < -128)
      delta += 256;
    bits->PutSE(delta);
    last = next;
  }
}

inline bool BuildPps(const VAPictureParameterBufferH264 &picture,
                     const VASliceParameterBufferH264 &slice,
                     VAProfile profile, const VAIQMatrixBufferH264 &matrix,
                     std::vector<uint8_t> *output) {
  bool flat;
  if (!ValidateH264IqMatrix(picture, profile, matrix, &flat))
    return false;
  BitWriter bits;
  bits.PutUE(0);
  bits.PutUE(0);
  bits.PutBit(picture.pic_fields.bits.entropy_coding_mode_flag);
  bits.PutBit(picture.pic_fields.bits.pic_order_present_flag);
  bits.PutUE(0);
  bits.PutUE(slice.num_ref_idx_l0_active_minus1);
  bits.PutUE(slice.num_ref_idx_l1_active_minus1);
  bits.PutBit(picture.pic_fields.bits.weighted_pred_flag);
  bits.PutBits(2, picture.pic_fields.bits.weighted_bipred_idc);
  bits.PutSE(picture.pic_init_qp_minus26);
  bits.PutSE(picture.pic_init_qs_minus26);
  bits.PutSE(picture.chroma_qp_index_offset);
  bits.PutBit(picture.pic_fields.bits.deblocking_filter_control_present_flag);
  bits.PutBit(picture.pic_fields.bits.constrained_intra_pred_flag);
  bits.PutBit(picture.pic_fields.bits.redundant_pic_cnt_present_flag);
  if (profile == VAProfileH264High) {
    bits.PutBit(picture.pic_fields.bits.transform_8x8_mode_flag);
    bits.PutBit(!flat);
    if (!flat) {
      // VA arrays are raster ordered; H.264 scaling_list() is zigzag ordered.
      // Clients already resolve SPS/PPS inheritance and JVT defaults. Emit
      // every active effective list explicitly, including flat ones: omitting
      // an individual list here would invoke a different fallback rule.
      static constexpr uint8_t scan4[16] = {
          0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
      static constexpr uint8_t scan8[64] = {
          0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
          12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
          35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
          58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
      for (const auto &list : matrix.ScalingList4x4)
        PutH264ScalingList(&bits, list, scan4);
      if (picture.pic_fields.bits.transform_8x8_mode_flag) {
        for (const auto &list : matrix.ScalingList8x8)
          PutH264ScalingList(&bits, list, scan8);
      }
    }
    bits.PutSE(picture.second_chroma_qp_index_offset);
  }
  AppendNal(output, 8, 3, bits.FinishRbsp());
  return true;
}

}  // namespace crystalhd_h264

#if defined(__GNUC__)
#pragma GCC visibility pop
#endif

#endif  // CRYSTALHD_H264_H
