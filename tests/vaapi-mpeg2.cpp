// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../filters/vaapi/crystalhd-mpeg2.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace mpeg = crystalhd_mpeg2;
namespace {
unsigned checks = 0, groups = 0;
void Require(bool value, const char *message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

struct Reader {
  const std::vector<uint8_t> &bytes;
  size_t position = 0;
  unsigned Get(unsigned count) {
    Require(count <= 32 && position + count <= bytes.size() * 8, "bounded syntax read");
    unsigned value = 0;
    while (count--) {
      value = value * 2 + ((bytes[position / 8] >> (7 - position % 8)) & 1);
      ++position;
    }
    return value;
  }
  void Equal(unsigned count, unsigned expected) {
    Require(Get(count) == expected, "independently parsed syntax value");
  }
  void Align() {
    while (position % 8) Equal(1, 0); // MPEG zero alignment, not RBSP stop bits.
  }
  void Start(unsigned code) { Align(); Equal(24, 1); Equal(8, code); }
};

struct Writer {
  std::vector<uint8_t> bytes;
  size_t position = 0;
  void Put(unsigned count, unsigned value) {
    for (unsigned i = count; i != 0; --i) {
      if (position % 8 == 0) bytes.push_back(0);
      if ((value >> (i - 1)) & 1) bytes.back() |= 0x80 >> (position % 8);
      ++position;
    }
  }
};

mpeg::SequenceOptions Sequence() {
  return {VAProfileMPEG2Main, 4, true, false, 1, 5, 0, 0, 100000, 112};
}
VAPictureParameterBufferMPEG2 Picture(unsigned type = 1, unsigned width = 640,
                                     unsigned height = 360) {
  VAPictureParameterBufferMPEG2 picture = {};
  picture.horizontal_size = width;
  picture.vertical_size = height;
  picture.picture_coding_type = type;
  picture.forward_reference_picture = VA_INVALID_SURFACE;
  picture.backward_reference_picture = VA_INVALID_SURFACE;
  picture.f_code = 0x4532;
  picture.picture_coding_extension.bits.picture_structure = 3;
  picture.picture_coding_extension.bits.frame_pred_frame_dct = 1;
  picture.picture_coding_extension.bits.progressive_frame = 1;
  picture.picture_coding_extension.bits.is_first_field = 1;
  return picture;
}
struct OwnedSlice {
  std::vector<uint8_t> bytes;
  VASliceParameterBufferMPEG2 parameters = {};
  mpeg::Slice View() const { return {parameters, bytes.data(), bytes.size()}; }
};
OwnedSlice Slice(unsigned row = 0, unsigned offset = 0, bool extension = false,
                 unsigned extra_bytes = 0) {
  Writer writer;
  writer.Put(24, 1); writer.Put(8, row + 1);
  writer.Put(5, 2); writer.Put(1, extension);
  if (extension) {
    writer.Put(8, 0x80); // intra_slice=1, no picture ID.
    for (unsigned i = 0; i < extra_bytes; ++i) {
      writer.Put(1, 1); writer.Put(8, 0xa5);
    }
    writer.Put(1, 0);
  }
  const unsigned macroblock_offset = writer.position;
  // A genuine flat intra macroblock for the optional software-decode smoke:
  // address increment 1, intra type, four zero luma DC differences plus EOB,
  // then two zero chroma DC differences plus EOB.
  writer.Put(1, 1); writer.Put(1, 1);
  for (unsigned i = 0; i < 4; ++i) { writer.Put(3, 4); writer.Put(2, 2); }
  for (unsigned i = 0; i < 2; ++i) { writer.Put(2, 0); writer.Put(2, 2); }
  OwnedSlice slice;
  slice.bytes.assign(offset, 0xee);
  slice.bytes.insert(slice.bytes.end(), writer.bytes.begin(), writer.bytes.end());
  slice.parameters.slice_data_offset = offset;
  slice.parameters.slice_data_size = writer.bytes.size();
  slice.parameters.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
  slice.parameters.macroblock_offset = macroblock_offset;
  slice.parameters.slice_vertical_position = row;
  slice.parameters.quantiser_scale_code = 2;
  slice.parameters.intra_slice_flag = extension;
  return slice;
}

void Parse(const std::vector<uint8_t> &bytes,
           const VAPictureParameterBufferMPEG2 &picture,
           const mpeg::SequenceOptions &sequence, const mpeg::PictureOptions &options,
           const mpeg::QuantMatrices &matrices, const std::vector<mpeg::Slice> &slices) {
  Reader reader{bytes};
  if (options.emit_sequence) {
    reader.Start(0xb3);
    reader.Equal(12, picture.horizontal_size); reader.Equal(12, picture.vertical_size);
    reader.Equal(4, sequence.aspect_ratio_information); reader.Equal(4, sequence.frame_rate_code);
    const uint32_t bitrate_low = reader.Get(18);
    reader.Equal(1, 1);
    const uint32_t vbv_low = reader.Get(10);
    reader.Equal(3, 0); // constrained + absent sequence matrices
    reader.Start(0xb5); reader.Equal(4, 1);
    reader.Equal(4, sequence.profile == VAProfileMPEG2Simple ? 5 : 4);
    reader.Equal(4, sequence.level); reader.Equal(1, sequence.progressive_sequence);
    reader.Equal(2, 1); reader.Equal(4, 0); // chroma format + dimension extensions
    Require((reader.Get(12) << 18 | bitrate_low) == sequence.bit_rate, "full bitrate preserved");
    reader.Equal(1, 1);
    Require((reader.Get(8) << 10 | vbv_low) == sequence.vbv_buffer_size, "full VBV preserved");
    reader.Equal(1, sequence.low_delay);
    reader.Equal(2, sequence.frame_rate_extension_n); reader.Equal(5, sequence.frame_rate_extension_d);
  }
  reader.Start(0);
  reader.Equal(10, options.temporal_reference); reader.Equal(3, picture.picture_coding_type);
  reader.Equal(16, 0xffff);
  if (picture.picture_coding_type >= 2) reader.Equal(4, 7);
  if (picture.picture_coding_type == 3) reader.Equal(4, 7);
  reader.Equal(1, 0);
  reader.Start(0xb5); reader.Equal(4, 8);
  const auto &p = picture.picture_coding_extension.bits;
  for (unsigned direction = 0; direction < 2; ++direction) {
    const bool used = direction == 0 ? picture.picture_coding_type >= 2 ||
        p.concealment_motion_vectors : picture.picture_coding_type == 3;
    reader.Equal(8, used ? (picture.f_code >> (8 - direction * 8)) & 255 : 255);
  }
  reader.Equal(2, p.intra_dc_precision); reader.Equal(2, 3);
  reader.Equal(1, p.top_field_first); reader.Equal(1, p.frame_pred_frame_dct);
  reader.Equal(1, p.concealment_motion_vectors); reader.Equal(1, p.q_scale_type);
  reader.Equal(1, p.intra_vlc_format); reader.Equal(1, p.alternate_scan);
  reader.Equal(1, p.repeat_first_field); reader.Equal(1, 1); reader.Equal(1, 1);
  reader.Equal(1, 0);
  reader.Start(0xb5); reader.Equal(4, 3);
  for (const auto &list : matrices.lists) {
    reader.Equal(1, 1);
    for (uint8_t value : list) reader.Equal(8, value);
  }
  reader.Align();
  size_t offset = reader.position / 8;
  for (const auto &slice : slices) {
    Require(offset + slice.parameters.slice_data_size <= bytes.size(), "complete slice range");
    Require(std::equal(slice.data + slice.parameters.slice_data_offset,
                       slice.data + slice.parameters.slice_data_offset + slice.parameters.slice_data_size,
                       bytes.begin() + offset), "slice bytes unchanged, no NAL escape or extra prefix");
    offset += slice.parameters.slice_data_size;
  }
  Require(offset == bytes.size(), "no invented trailing AUD/GOP/EOS");
}

void Defaults() {
  ++groups;
  static constexpr unsigned raster[64] = {
      8,16,19,22,26,27,29,34, 16,16,22,24,27,29,34,37,
      19,22,26,27,29,34,34,38, 22,22,26,27,29,34,37,40,
      22,26,27,29,32,35,40,48, 26,27,29,32,35,40,48,58,
      26,27,29,34,38,46,56,69, 27,29,35,38,46,56,69,83};
  const auto matrices = mpeg::DefaultQuantMatrices();
  unsigned index = 0;
  for (unsigned diagonal = 0; diagonal < 15; ++diagonal) {
    const unsigned first = diagonal < 8 ? 0 : diagonal - 7;
    const unsigned last = std::min(diagonal, 7U);
    for (unsigned step = first; step <= last; ++step) {
      const unsigned row = diagonal & 1 ? step : last - (step - first);
      const unsigned column = diagonal - row;
      Require(matrices.lists[0][index] == raster[row * 8 + column], "normative intra zigzag default");
      Require(matrices.lists[2][index] == matrices.lists[0][index], "default chroma inheritance");
      Require(matrices.lists[1][index] == 16 && matrices.lists[3][index] == 16, "flat inter defaults");
      ++index;
    }
  }
}

void Headers() {
  ++groups;
  for (unsigned type = 1; type <= 3; ++type)
    for (unsigned height : {360U, 720U, 1080U, 1088U})
      for (unsigned flags = 0; flags < 16; ++flags) {
        auto picture = Picture(type, 1920, height);
        auto &p = picture.picture_coding_extension.bits;
        p.intra_dc_precision = flags % 4;
        p.q_scale_type = flags & 1; p.intra_vlc_format = (flags >> 1) & 1;
        p.alternate_scan = (flags >> 2) & 1; p.concealment_motion_vectors = (flags >> 3) & 1;
        const auto sequence = Sequence();
        const mpeg::PictureOptions options{(flags * 67) % 1024, (flags & 1) != 0};
        const auto slice = Slice();
        const std::vector<mpeg::Slice> slices{slice.View()};
        auto current = mpeg::DefaultQuantMatrices(), next = current;
        std::vector<uint8_t> bytes;
        Require(mpeg::Assemble(picture, nullptr, current, slices, sequence, options, &bytes, &next), "I/P/B assembly");
        Parse(bytes, picture, sequence, options, current, slices);
      }
  for (unsigned rate = 1; rate <= 8; ++rate) {
    auto sequence = Sequence();
    sequence.frame_rate_code = rate; sequence.frame_rate_extension_n = 3;
    sequence.frame_rate_extension_d = 31;
    sequence.bit_rate = 0x3fffffff; sequence.vbv_buffer_size = 0x3ffff;
    auto picture = Picture(); const auto slice = Slice();
    auto current = mpeg::DefaultQuantMatrices(), next = current;
    std::vector<uint8_t> bytes; const mpeg::PictureOptions options{1023, true};
    Require(mpeg::Assemble(picture, nullptr, current, {slice.View()}, sequence, options, &bytes, &next), "explicit rate/extension limits");
    Parse(bytes, picture, sequence, options, current, {slice.View()});
  }
  for (unsigned type : {1U, 2U}) {
    auto sequence = Sequence(); sequence.profile = VAProfileMPEG2Simple; sequence.level = 8;
    sequence.low_delay = true;
    const auto picture = Picture(type, 720, 480);
    const auto slice = Slice(); auto current = mpeg::DefaultQuantMatrices(), next = current;
    std::vector<uint8_t> bytes; const mpeg::PictureOptions options{0, true};
    Require(mpeg::Assemble(picture, nullptr, current, {slice.View()}, sequence, options, &bytes, &next), "Simple I/P supported subset");
    Parse(bytes, picture, sequence, options, current, {slice.View()});
  }
}

VAIQMatrixBufferMPEG2 Custom(unsigned flags) {
  VAIQMatrixBufferMPEG2 iq = {};
  iq.load_intra_quantiser_matrix = flags & 1;
  iq.load_non_intra_quantiser_matrix = (flags >> 1) & 1;
  iq.load_chroma_intra_quantiser_matrix = (flags >> 2) & 1;
  iq.load_chroma_non_intra_quantiser_matrix = (flags >> 3) & 1;
  uint8_t *lists[] = {iq.intra_quantiser_matrix, iq.non_intra_quantiser_matrix,
                     iq.chroma_intra_quantiser_matrix, iq.chroma_non_intra_quantiser_matrix};
  for (unsigned list = 0; list < 4; ++list)
    for (unsigned i = 0; i < 64; ++i) lists[list][i] = 1 + (i * 11 + list * 37) % 255;
  iq.intra_quantiser_matrix[0] = iq.chroma_intra_quantiser_matrix[0] = 8;
  return iq;
}

void Matrices() {
  ++groups;
  const auto picture = Picture(); const auto sequence = Sequence(); const auto slice = Slice();
  auto state = mpeg::DefaultQuantMatrices();
  for (unsigned flags = 0; flags < 16; ++flags) {
    auto iq = Custom(flags);
    auto expected = state;
    if (flags & 1) {
      std::copy_n(iq.intra_quantiser_matrix, 64, expected.lists[0].begin());
      expected.lists[2] = expected.lists[0];
    }
    if (flags & 2) {
      std::copy_n(iq.non_intra_quantiser_matrix, 64, expected.lists[1].begin());
      expected.lists[3] = expected.lists[1];
    }
    if (flags & 4) std::copy_n(iq.chroma_intra_quantiser_matrix, 64, expected.lists[2].begin());
    if (flags & 8) std::copy_n(iq.chroma_non_intra_quantiser_matrix, 64, expected.lists[3].begin());
    const auto original = state;
    auto next = state; std::vector<uint8_t> bytes;
    const mpeg::PictureOptions options{flags, true};
    Require(mpeg::Assemble(picture, &iq, state, {slice.View()}, sequence, options, &bytes, &next), "all IQ update combinations");
    Require(state.lists == original.lists, "caller state remains immutable");
    Require(next.lists == expected.lists, "effective IQ update/inheritance order");
    Parse(bytes, picture, sequence, options, expected, {slice.View()});
    state = next;
  }
  const auto ignored = VAIQMatrixBufferMPEG2{}; // zero unloaded coefficients are ignored.
  for (const auto *update : {static_cast<const VAIQMatrixBufferMPEG2 *>(nullptr), &ignored}) {
    std::vector<uint8_t> bytes;
    auto next = mpeg::DefaultQuantMatrices();
    Require(mpeg::Assemble(picture, update, state, {slice.View()}, sequence, {17, true}, &bytes, &next), "missing/unloaded IQ preserves effective state");
    Require(next.lists == state.lists, "restart retains custom IQ snapshot");
    Parse(bytes, picture, sequence, {17, true}, state, {slice.View()});
  }
}

struct Fixture {
  VAPictureParameterBufferMPEG2 picture = Picture(3);
  mpeg::SequenceOptions sequence = Sequence();
  mpeg::PictureOptions options{0, true};
  mpeg::QuantMatrices state = mpeg::DefaultQuantMatrices();
  VAIQMatrixBufferMPEG2 iq = Custom(15);
  OwnedSlice slice = Slice();
  void Reject() const {
    std::vector<uint8_t> bytes{0x12, 0x34};
    auto next = state; next.lists[1][3] = 233;
    const auto original = next;
    Require(!mpeg::Assemble(picture, &iq, state, {slice.View()}, sequence, options, &bytes, &next), "invalid request rejected");
    Require(bytes == std::vector<uint8_t>({0x12, 0x34}), "failed output is transactional");
    Require(next.lists == original.lists, "failed matrix update is transactional");
  }
};
void Rejections() {
  ++groups;
  for (unsigned mode = 0; mode < 34; ++mode) {
    Fixture f;
    switch (mode) {
      case 0: f.sequence.profile = VAProfileH264Main; break;
      case 1: f.sequence.level = 7; break;
      case 2: f.sequence.profile = VAProfileMPEG2Simple; break;
      case 3: f.sequence.progressive_sequence = false; break;
      case 4: f.picture.picture_coding_extension.bits.picture_structure = 1; break;
      case 5: f.picture.picture_coding_extension.bits.progressive_frame = 0; break;
      case 6: f.picture.picture_coding_extension.bits.frame_pred_frame_dct = 0; break;
      case 7: f.picture.picture_coding_extension.bits.repeat_first_field = 1; break;
      case 8: f.picture.picture_coding_extension.bits.top_field_first = 1; break;
      case 9: f.picture.horizontal_size = 0; break;
      case 10: f.picture.vertical_size = 1089; break;
      case 11: f.picture.horizontal_size = 641; break;
      case 12: f.picture.picture_coding_type = 4; break;
      case 13: f.sequence.low_delay = true; break;
      case 14: f.picture.f_code = -1; break;
      case 15: f.picture.f_code = 0x10000; break;
      case 16: f.picture.f_code = 0x0032; break;
      case 17: f.picture.f_code = 0x45a2; break;
      case 18: f.picture.f_code = 0x4536; break;
      case 19: f.options.temporal_reference = 1024; break;
      case 20: f.sequence.aspect_ratio_information = 0; break;
      case 21: f.sequence.aspect_ratio_information = 5; break;
      case 22: f.sequence.frame_rate_code = 0; break;
      case 23: f.sequence.frame_rate_code = 9; break;
      case 24: f.sequence.frame_rate_extension_n = 4; break;
      case 25: f.sequence.frame_rate_extension_d = 32; break;
      case 26: f.sequence.bit_rate = 0; break;
      case 27: f.sequence.bit_rate = 0x40000000; break;
      case 28: f.sequence.vbv_buffer_size = 0; break;
      case 29: f.sequence.vbv_buffer_size = 0x40000; break;
      case 30: f.sequence.level = 10; break;
      case 31: f.sequence.level = 8; f.picture.vertical_size = 720; break;
      case 32: f.sequence.level = 6; f.picture.horizontal_size = 1920; break;
      case 33: f.state.lists[1][0] = 0; break;
    }
    f.Reject();
  }
  for (unsigned list = 0; list < 4; ++list)
    for (unsigned index = 0; index < 64; ++index) {
      Fixture f;
      uint8_t *lists[] = {f.iq.intra_quantiser_matrix, f.iq.non_intra_quantiser_matrix,
                         f.iq.chroma_intra_quantiser_matrix, f.iq.chroma_non_intra_quantiser_matrix};
      lists[list][index] = 0; f.Reject();
    }
  for (unsigned mode = 0; mode < 4; ++mode) {
    Fixture f;
    if (mode == 0) f.iq.load_intra_quantiser_matrix = -1;
    if (mode == 1) f.iq.load_chroma_non_intra_quantiser_matrix = 2;
    if (mode == 2) f.iq.intra_quantiser_matrix[0] = 16;
    if (mode == 3) f.iq.chroma_intra_quantiser_matrix[0] = 16;
    f.Reject();
  }
}

void SliceRanges() {
  ++groups;
  const auto picture = Picture(); const auto sequence = Sequence();
  const auto state = mpeg::DefaultQuantMatrices(); auto next = state;
  for (bool extension : {false, true})
    for (unsigned extra : {0U, 1U, 10U}) {
      const auto first = Slice(0, 7, extension, extra), second = Slice(1, 3);
      std::vector<mpeg::Slice> slices{first.View(), second.View()};
      std::vector<uint8_t> bytes;
      Require(mpeg::Assemble(picture, nullptr, state, slices, sequence, {0, true}, &bytes, &next), "separate data buffers, offsets and slice extras");
      Parse(bytes, picture, sequence, {0, true}, state, slices);
    }
  for (unsigned mode = 0; mode < 18; ++mode) {
    Fixture f;
    switch (mode) {
      case 0: f.slice.parameters.slice_data_offset = UINT32_MAX; break;
      case 1: f.slice.parameters.slice_data_size = UINT32_MAX; break;
      case 2: f.slice.parameters.slice_data_size = 4; break;
      case 3: f.slice.parameters.slice_data_flag = VA_SLICE_DATA_FLAG_BEGIN; break;
      case 4: f.slice.parameters.slice_data_flag = VA_SLICE_DATA_FLAG_MIDDLE; break;
      case 5: f.slice.parameters.slice_data_flag = VA_SLICE_DATA_FLAG_END; break;
      case 6: f.slice.parameters.slice_horizontal_position = 40; break;
      case 7: f.slice.parameters.slice_vertical_position = 23; break;
      case 8: f.slice.parameters.quantiser_scale_code = 0; break;
      case 9: f.slice.parameters.quantiser_scale_code = 32; break;
      case 10: f.slice.parameters.intra_slice_flag = 2; break;
      case 11: f.slice.parameters.macroblock_offset = 39; break;
      case 12: f.slice.bytes[2] = 0; break;
      case 13: f.slice.bytes[3] = 0xb7; break;
      case 14: f.slice.bytes[3] = 2; break;
      case 15: f.slice.bytes[4] ^= 8; break;
      case 16: f.slice.parameters.intra_slice_flag = 1; break;
      case 17: f.slice.bytes.insert(f.slice.bytes.end(), {0, 0, 1, 0xb7});
               f.slice.parameters.slice_data_size += 4; break;
    }
    f.Reject();
  }
  const auto owned = Slice(); auto view = owned.View(); view.data = nullptr;
  std::vector<uint8_t> bytes;
  Require(!mpeg::Assemble(picture, nullptr, state, {view}, sequence, {0, true}, &bytes, &next), "null slice rejected");
  Require(!mpeg::Assemble(picture, nullptr, state, {}, sequence, {0, true}, &bytes, &next), "empty picture rejected");
  Require(!mpeg::Assemble(picture, nullptr, state, {owned.View()}, sequence, {0, true}, nullptr, &next), "null output rejected");
  Require(!mpeg::Assemble(picture, nullptr, state, {owned.View()}, sequence, {0, true}, &bytes, nullptr), "null next-state rejected");
}

void Capacity() {
  ++groups;
  const auto picture = Picture(); const auto sequence = Sequence();
  auto state = mpeg::DefaultQuantMatrices(), next = state;
  auto slice = Slice(); std::vector<uint8_t> bytes;
  Require(mpeg::Assemble(picture, nullptr, state, {slice.View()}, sequence, {0, true}, &bytes, &next), "measure fixed headers");
  const size_t header_bytes = bytes.size() - slice.bytes.size();
  slice.bytes.resize(mpeg::kMaxAccessUnitBytes - header_bytes, 0x55);
  slice.parameters.slice_data_size = slice.bytes.size();
  Require(mpeg::Assemble(picture, nullptr, state, {slice.View()}, sequence, {0, true}, &bytes, &next), "exact AU capacity accepted");
  Require(bytes.size() == mpeg::kMaxAccessUnitBytes, "AU includes all header bytes in bound");
  const auto original = bytes;
  slice.bytes.push_back(0x55); ++slice.parameters.slice_data_size;
  Require(!mpeg::Assemble(picture, nullptr, state, {slice.View()}, sequence, {0, true}, &bytes, &next), "one byte beyond AU capacity rejected");
  Require(bytes == original, "capacity failure keeps old output");
  slice.bytes.resize(mpeg::kMaxAccessUnitBytes, 0x55);
  slice.parameters.slice_data_size = slice.bytes.size();
  Require(!mpeg::Assemble(picture, nullptr, state, {slice.View(), slice.View()}, sequence, {0, true}, &bytes, &next), "aggregate slice capacity bounded");
}

// This emits a tiny real I-picture stream for an optional external software
// decoder check. It is not a CrystalHD firmware/replay or VA profile claim.
void Stream() {
  const auto picture = Picture(1, 16, 16); const auto sequence = Sequence();
  auto state = mpeg::DefaultQuantMatrices(), next = state;
  const auto slice = Slice(); std::vector<uint8_t> bytes;
  Require(mpeg::Assemble(picture, nullptr, state, {slice.View()}, sequence, {0, true}, &bytes, &next), "real intra picture assembly");
  bytes.insert(bytes.end(), {0, 0, 1, 0xb7}); // explicit caller-owned sequence end
  Require(std::fwrite(bytes.data(), 1, bytes.size(), stdout) == bytes.size(), "write software smoke stream");
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::strcmp(argv[1], "--stream") == 0) { Stream(); return 0; }
    if (argc != 1) throw std::runtime_error("usage: vaapi-mpeg2-test [--stream]");
    Defaults(); Headers(); Matrices(); Rejections(); SliceRanges(); Capacity();
    std::printf("MPEG-2 assembly: %u groups, %u checks PASS\n", groups, checks);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "MPEG-2 assembly failed after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
