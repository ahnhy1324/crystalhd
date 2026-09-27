// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_MPEG2_H
#define CRYSTALHD_MPEG2_H

#include <va/va.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Elementary-stream assembly only. This does not advertise a VA profile or
// decide reference ownership, GOP boundaries, timing, or decoder replay policy.
namespace crystalhd_mpeg2 {

// A transport bound matching CrystalHDDecodeReplay::Limits::picture_bytes,
// not a universal MPEG-2 or CrystalHD hardware limit.
constexpr size_t kMaxAccessUnitBytes = 512 * 1024;

struct SequenceOptions {
  VAProfile profile;
  unsigned level;  // MPEG-2 level code: 10/8/6/4, not a VA enum.
  bool progressive_sequence;
  bool low_delay;
  unsigned aspect_ratio_information;
  unsigned frame_rate_code;
  unsigned frame_rate_extension_n;
  unsigned frame_rate_extension_d;
  uint32_t bit_rate;        // Units of 400 bits/s, including extension bits.
  uint32_t vbv_buffer_size; // Units of 16384 bits, including extension bits.
};

struct PictureOptions {
  unsigned temporal_reference; // Explicit caller policy; never inferred here.
  bool emit_sequence;
};

// All four lists are in VA/MPEG-2 zigzag order, NOT raster order.
struct QuantMatrices {
  std::array<std::array<uint8_t, 64>, 4> lists;
};

inline QuantMatrices DefaultQuantMatrices() {
  static constexpr uint8_t intra[64] = {
      8,16,16,19,16,19,22,22,22,22,22,22,26,24,26,27,
      27,27,26,26,26,26,27,27,27,29,29,29,34,34,34,29,
      29,29,27,27,29,29,32,32,34,34,37,38,37,35,35,34,
      35,38,38,40,40,40,48,48,46,46,56,56,58,69,69,83};
  QuantMatrices result = {};
  for (size_t i = 0; i < 64; ++i) {
    result.lists[0][i] = result.lists[2][i] = intra[i];
    result.lists[1][i] = result.lists[3][i] = 16;
  }
  return result;
}

namespace detail {
inline bool ValidMatrices(const QuantMatrices &matrices) {
  for (size_t list = 0; list < 4; ++list) {
    if ((list == 0 || list == 2) && matrices.lists[list][0] != 8)
      return false;
    for (uint8_t value : matrices.lists[list])
      if (value == 0) return false;
  }
  return true;
}

inline bool UpdateMatrices(const QuantMatrices &previous,
                           const VAIQMatrixBufferMPEG2 *update,
                           QuantMatrices *next) {
  if (!ValidMatrices(previous)) return false;
  *next = previous;
  if (update == nullptr) return true;
  const int32_t flags[] = {update->load_intra_quantiser_matrix,
      update->load_non_intra_quantiser_matrix,
      update->load_chroma_intra_quantiser_matrix,
      update->load_chroma_non_intra_quantiser_matrix};
  const uint8_t *lists[] = {update->intra_quantiser_matrix,
      update->non_intra_quantiser_matrix, update->chroma_intra_quantiser_matrix,
      update->chroma_non_intra_quantiser_matrix};
  for (size_t list = 0; list < 4; ++list) {
    if (flags[list] != 0 && flags[list] != 1) return false;
    if (!flags[list]) continue;
    for (size_t i = 0; i < 64; ++i) next->lists[list][i] = lists[list][i];
    // Loading a luminance list also updates its chrominance default. An
    // explicitly loaded chrominance list later in this extension overrides it.
    if (list < 2) next->lists[list + 2] = next->lists[list];
  }
  return ValidMatrices(*next);
}

class Bits {
 public:
  void Put(unsigned count, uint32_t value) {
    while (count--) {
      if (position_ == 0) bytes.push_back(0);
      bytes.back() |= ((value >> count) & 1U) << (7 - position_);
      position_ = (position_ + 1) & 7;
    }
  }
  void Align() { if (position_ != 0) Put(8 - position_, 0); }
  void Start(uint8_t code) {
    Align();
    bytes.insert(bytes.end(), {0, 0, 1, code});
  }
  std::vector<uint8_t> bytes;
 private:
  unsigned position_ = 0;
};

inline bool ValidPicture(const VAPictureParameterBufferMPEG2 &picture,
                         const SequenceOptions &sequence,
                         const PictureOptions &options) {
  const auto &p = picture.picture_coding_extension.bits;
  if ((sequence.profile != VAProfileMPEG2Simple &&
       sequence.profile != VAProfileMPEG2Main) ||
      (sequence.level != 10 && sequence.level != 8 &&
       sequence.level != 6 && sequence.level != 4) ||
      (sequence.profile == VAProfileMPEG2Simple && sequence.level != 8) ||
      !sequence.progressive_sequence || p.picture_structure != 3 ||
      !p.progressive_frame || !p.frame_pred_frame_dct ||
      p.repeat_first_field || p.top_field_first ||
      picture.horizontal_size == 0 || picture.vertical_size == 0 ||
      picture.horizontal_size > 1920 || picture.vertical_size > 1088 ||
      ((picture.horizontal_size | picture.vertical_size) & 1U) ||
      picture.picture_coding_type < 1 || picture.picture_coding_type > 3 ||
      ((sequence.profile == VAProfileMPEG2Simple || sequence.low_delay) &&
       picture.picture_coding_type == 3) ||
      picture.f_code < 0 || picture.f_code > 0xffff ||
      options.temporal_reference > 1023 ||
      sequence.aspect_ratio_information < 1 || sequence.aspect_ratio_information > 4 ||
      sequence.frame_rate_code < 1 || sequence.frame_rate_code > 8 ||
      sequence.frame_rate_extension_n > 3 || sequence.frame_rate_extension_d > 31 ||
      sequence.bit_rate == 0 || sequence.bit_rate > 0x3fffffff ||
      sequence.vbv_buffer_size == 0 || sequence.vbv_buffer_size > 0x3ffff)
    return false;
  const unsigned max_width = sequence.level == 10 ? 352 :
      sequence.level == 8 ? 720 : sequence.level == 6 ? 1440 : 1920;
  const unsigned max_height = sequence.level == 10 ? 288 :
      sequence.level == 8 ? 576 : 1088;
  if (picture.horizontal_size > max_width || picture.vertical_size > max_height)
    return false;
  // Only active vector components affect decoding. Inactive components are
  // emitted as the MPEG-2 'not used' value 15, not arbitrary caller padding.
  const unsigned max_horizontal = sequence.level == 10 ? 7 :
      sequence.level == 8 ? 8 : 9;
  const unsigned max_vertical = sequence.level == 10 ? 4 : 5;
  for (unsigned direction = 0; direction < 2; ++direction) {
    const bool active = direction == 0
        ? picture.picture_coding_type != 1 || p.concealment_motion_vectors
        : picture.picture_coding_type == 3;
    if (!active) continue;
    const unsigned horizontal = (picture.f_code >> (12 - 8 * direction)) & 15;
    const unsigned vertical = (picture.f_code >> (8 - 8 * direction)) & 15;
    if (horizontal == 0 || horizontal > max_horizontal ||
        vertical == 0 || vertical > max_vertical) return false;
  }
  return true;
}
} // namespace detail

// Each slice explicitly names its data buffer; offsets are relative to that
// buffer, not to a concatenation of unrelated vaRenderPicture buffers.
struct Slice {
  VASliceParameterBufferMPEG2 parameters;
  const uint8_t *data;
  size_t size;
};

namespace detail {
inline bool ValidSlice(const Slice &slice,
                       const VAPictureParameterBufferMPEG2 &picture) {
  const auto &p = slice.parameters;
  if (slice.data == nullptr || p.slice_data_flag != VA_SLICE_DATA_FLAG_ALL ||
      p.slice_data_size < 5 || p.slice_data_size > kMaxAccessUnitBytes ||
      p.slice_data_offset > slice.size ||
      p.slice_data_size > slice.size - p.slice_data_offset ||
      p.slice_horizontal_position >= (picture.horizontal_size + 15U) / 16U ||
      p.slice_vertical_position >= (picture.vertical_size + 15U) / 16U ||
      p.quantiser_scale_code < 1 || p.quantiser_scale_code > 31 ||
      (p.intra_slice_flag != 0 && p.intra_slice_flag != 1)) return false;
  const uint8_t *data = slice.data + p.slice_data_offset;
  if (data[0] || data[1] || data[2] != 1 || data[3] == 0 ||
      data[3] > 0xaf || data[3] != p.slice_vertical_position + 1 ||
      (data[4] >> 3) != p.quantiser_scale_code ||
      ((data[4] >> 2) & 1) != p.intra_slice_flag) return false;
  // Parse only the slice header, preserving every macroblock payload byte.
  // With this <=1088-line nonscalable subset there is no vertical extension.
  size_t bit = 38;
  const size_t end = static_cast<size_t>(p.slice_data_size) * 8;
  if (p.intra_slice_flag) {
    bit += 8; // intra_slice, slice_picture_id_enable, slice_picture_id.
    for (;;) {
      if (bit >= end) return false;
      const bool extra = (data[bit / 8] >> (7 - bit % 8)) & 1;
      ++bit;
      if (!extra) break;
      if (end - bit < 8) return false;
      bit += 8;
    }
  }
  if (bit != p.macroblock_offset || bit >= end) return false;
  // A single slice range must not smuggle in another header, slice, or EOS.
  for (size_t i = 4; i + 2 < p.slice_data_size; ++i)
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return false;
  return true;
}
} // namespace detail

// Replace output and next_matrices only after validation and complete assembly.
// No GOP header/EOS is invented. The caller owns references and finite-sequence
// policy. A sequence restart can carry a previously resolved matrix snapshot.
inline bool Assemble(const VAPictureParameterBufferMPEG2 &picture,
                     const VAIQMatrixBufferMPEG2 *update,
                     const QuantMatrices &current,
                     const std::vector<Slice> &slices,
                     const SequenceOptions &sequence,
                     const PictureOptions &options,
                     std::vector<uint8_t> *output, QuantMatrices *next_matrices) {
  if (output == nullptr || next_matrices == nullptr || slices.empty() ||
      !detail::ValidPicture(picture, sequence, options)) return false;
  QuantMatrices next;
  if (!detail::UpdateMatrices(current, update, &next)) return false;
  size_t slice_bytes = 0;
  for (const Slice &slice : slices) {
    if (!detail::ValidSlice(slice, picture) ||
        slice.parameters.slice_data_size > kMaxAccessUnitBytes - slice_bytes)
      return false;
    slice_bytes += slice.parameters.slice_data_size;
  }
  detail::Bits bits;
  if (options.emit_sequence) {
    bits.Start(0xb3);
    bits.Put(12, picture.horizontal_size); bits.Put(12, picture.vertical_size);
    bits.Put(4, sequence.aspect_ratio_information); bits.Put(4, sequence.frame_rate_code);
    bits.Put(18, sequence.bit_rate & 0x3ffff); bits.Put(1, 1);
    bits.Put(10, sequence.vbv_buffer_size & 0x3ff);
    bits.Put(1, 0); // constrained_parameters_flag
    bits.Put(1, 0); bits.Put(1, 0); // defaults, overridden by the full IQ snapshot
    bits.Start(0xb5); bits.Put(4, 1);
    bits.Put(8, (sequence.profile == VAProfileMPEG2Simple ? 0x50 : 0x40) | sequence.level);
    bits.Put(1, sequence.progressive_sequence); bits.Put(2, 1); // 4:2:0
    bits.Put(2, 0); bits.Put(2, 0); // dimensions are below 4096
    bits.Put(12, sequence.bit_rate >> 18); bits.Put(1, 1);
    bits.Put(8, sequence.vbv_buffer_size >> 10); bits.Put(1, sequence.low_delay);
    bits.Put(2, sequence.frame_rate_extension_n); bits.Put(5, sequence.frame_rate_extension_d);
  }
  bits.Start(0x00);
  bits.Put(10, options.temporal_reference); bits.Put(3, picture.picture_coding_type);
  bits.Put(16, 0xffff); // VBR/unspecified VBV delay
  if (picture.picture_coding_type != 1) { bits.Put(1, 0); bits.Put(3, 7); }
  if (picture.picture_coding_type == 3) { bits.Put(1, 0); bits.Put(3, 7); }
  bits.Put(1, 0); // extra_bit_picture
  bits.Start(0xb5); bits.Put(4, 8);
  const auto &p = picture.picture_coding_extension.bits;
  for (unsigned component = 0; component < 4; ++component) {
    const bool active = component < 2
        ? picture.picture_coding_type != 1 || p.concealment_motion_vectors
        : picture.picture_coding_type == 3;
    bits.Put(4, active ? ((picture.f_code >> (12 - component * 4)) & 15) : 15);
  }
  bits.Put(2, p.intra_dc_precision); bits.Put(2, p.picture_structure);
  bits.Put(1, p.top_field_first); bits.Put(1, p.frame_pred_frame_dct);
  bits.Put(1, p.concealment_motion_vectors); bits.Put(1, p.q_scale_type);
  bits.Put(1, p.intra_vlc_format); bits.Put(1, p.alternate_scan);
  bits.Put(1, p.repeat_first_field); bits.Put(1, p.progressive_frame); // chroma_420_type
  bits.Put(1, p.progressive_frame); bits.Put(1, 0); // no composite display extension
  bits.Start(0xb5); bits.Put(4, 3);
  // Emit every effective list, avoiding dependence on the hardware's previous
  // IQ state after EOS/reopen or a repeated sequence header.
  for (const auto &list : next.lists) {
    bits.Put(1, 1);
    for (uint8_t value : list) bits.Put(8, value);
  }
  bits.Align();
  if (slice_bytes > kMaxAccessUnitBytes - bits.bytes.size()) return false;
  bits.bytes.reserve(bits.bytes.size() + slice_bytes);
  for (const Slice &slice : slices) {
    const uint8_t *first = slice.data + slice.parameters.slice_data_offset;
    bits.bytes.insert(bits.bytes.end(), first, first + slice.parameters.slice_data_size);
  }
  *output = std::move(bits.bytes);
  *next_matrices = next;
  return true;
}
} // namespace crystalhd_mpeg2
#endif
