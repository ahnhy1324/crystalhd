// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../filters/vaapi/crystalhd-mpeg4.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mpeg4 = crystalhd_mpeg4;
using Bytes = std::vector<uint8_t>;

namespace {
unsigned checks = 0;
unsigned groups = 0;

void Check(bool yes, const char *why) {
  ++checks;
  if (!yes) throw std::runtime_error(why);
}

unsigned IndependentTimeBits(uint16_t resolution) {
  unsigned bits = 1;
  while ((uint32_t{1} << bits) < resolution) ++bits;
  return bits;
}

unsigned Bit(const uint8_t *bytes, size_t bit) {
  return (bytes[bit / 8] >> (7 - bit % 8)) & 1U;
}

struct Reader {
  const Bytes &bytes;
  size_t position;

  Reader(const Bytes &source, size_t begin) : bytes(source), position(begin) {}

  uint32_t Get(unsigned count) {
    Check(count <= 32 && position <= bytes.size() * 8 &&
              count <= bytes.size() * 8 - position,
          "bounded independent bit read");
    uint32_t value = 0;
    while (count--) {
      value = (value << 1) | Bit(bytes.data(), position);
      ++position;
    }
    return value;
  }

  void Equal(unsigned count, uint32_t expected, const char *why) {
    Check(Get(count) == expected, why);
  }

  void StuffTo(size_t next_byte) {
    Equal(1, 0, "stuffing begins with zero");
    while (position & 7) Equal(1, 1, "stuffing continuation is one");
    Check(position / 8 == next_byte, "stuffing reaches exact next start code");
  }
};

struct StartCode {
  size_t offset;
  uint8_t code;
};

std::vector<StartCode> StartCodes(const Bytes &bytes) {
  std::vector<StartCode> result;
  for (size_t i = 0; i + 3 < bytes.size(); ++i) {
    if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
      result.push_back({i, bytes[i + 3]});
      i += 3;
    }
  }
  return result;
}

struct OwnedSlice {
  VASliceParameterBufferMPEG4 parameters = {};
  Bytes data;

  mpeg4::Slice View() const {
    return {parameters, data.empty() ? nullptr : data.data(), data.size()};
  }
};

OwnedSlice MakeSlice(unsigned macroblock_offset = 3) {
  OwnedSlice slice;
  slice.data = {0x19, 0xe7, 0xd3, 0x6a, 0xb5, 0x79};
  slice.parameters.slice_data_offset = 2;
  slice.parameters.slice_data_size = 4;
  slice.parameters.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
  slice.parameters.macroblock_offset = macroblock_offset;
  slice.parameters.macroblock_number = 0;
  slice.parameters.quant_scale = 17;
  return slice;
}

struct PacketHeader {
  unsigned macroblock;
  unsigned quant;
  unsigned hec;
};

void PutTestBits(Bytes *bytes, size_t *position, unsigned count,
                 uint32_t value) {
  Check(bytes != nullptr && position != nullptr && count <= 32,
        "bounded test packet writer");
  const size_t end = *position + count;
  if (bytes->size() < (end + 7) / 8) bytes->resize((end + 7) / 8, 0);
  while (count--) {
    (*bytes)[*position / 8] |=
        static_cast<uint8_t>(((value >> count) & 1U)
                             << (7 - *position % 8));
    ++*position;
  }
}

OwnedSlice MakePacketSlice(unsigned zero_bits, unsigned macroblock_offset,
                           const std::vector<PacketHeader> &headers) {
  OwnedSlice slice;
  Check(macroblock_offset < 8, "test packet offset is within its first byte");
  slice.data.resize(1, static_cast<uint8_t>((1U << (8 - macroblock_offset)) - 1));
  size_t position = 8;
  for (const PacketHeader &header : headers) {
    PutTestBits(&slice.data, &position, zero_bits, 0);
    PutTestBits(&slice.data, &position, 1, 1);
    PutTestBits(&slice.data, &position, 10, header.macroblock);
    PutTestBits(&slice.data, &position, 5, header.quant);
    PutTestBits(&slice.data, &position, 1, header.hec);
    while (position & 7) PutTestBits(&slice.data, &position, 1, 1);
    PutTestBits(&slice.data, &position, 8, 0x55);
  }
  PutTestBits(&slice.data, &position, 8, 0xa5);
  slice.parameters.slice_data_offset = 0;
  slice.parameters.slice_data_size = slice.data.size();
  slice.parameters.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
  slice.parameters.macroblock_offset = macroblock_offset;
  slice.parameters.macroblock_number = 0;
  slice.parameters.quant_scale = 17;
  return slice;
}

VAPictureParameterBufferMPEG4 Picture(mpeg4::Kind kind) {
  VAPictureParameterBufferMPEG4 picture = {};
  picture.vop_width = 640;
  picture.vop_height = 360;
  picture.forward_reference_picture = VA_INVALID_SURFACE;
  picture.backward_reference_picture = VA_INVALID_SURFACE;
  picture.vol_fields.bits.chroma_format = 1;
  picture.vol_fields.bits.obmc_disable = 1;
  picture.vol_fields.bits.resync_marker_disable = 1;
  picture.quant_precision = 5;
  picture.vop_fields.bits.vop_coding_type =
      kind == mpeg4::Kind::I ? 0 : kind == mpeg4::Kind::P ? 1 : 2;
  picture.vop_fields.bits.intra_dc_vlc_thr = 5;
  picture.vop_fields.bits.vop_rounding_type = 1;
  picture.vop_fcode_forward = 3;
  picture.vop_fcode_backward = 4;
  picture.vop_time_increment_resolution = 16;
  // These short-header-only values are populated by FFmpeg even for MPEG-4.
  picture.num_gobs_in_vop = 9;
  picture.num_macroblocks_in_gob = 20;
  if (kind == mpeg4::Kind::P) {
    picture.forward_reference_picture = 11;
  } else if (kind == mpeg4::Kind::B) {
    picture.forward_reference_picture = 11;
    picture.backward_reference_picture = 12;
  }
  return picture;
}

bool Equal(const mpeg4::Anchor &a, const mpeg4::Anchor &b) {
  return a.token == b.token && a.tick == b.tick && a.kind == b.kind;
}

bool Equal(const mpeg4::TimingState &a, const mpeg4::TimingState &b) {
  return a.have_previous == b.have_previous &&
         a.have_newest == b.have_newest && Equal(a.previous, b.previous) &&
         Equal(a.newest, b.newest) && a.profile == b.profile &&
         a.width == b.width && a.height == b.height &&
         a.resolution == b.resolution &&
         a.resync_marker_disable == b.resync_marker_disable;
}

struct Input {
  VAPictureParameterBufferMPEG4 picture = Picture(mpeg4::Kind::I);
  VAProfile profile = VAProfileMPEG4AdvancedSimple;
  uint64_t token = 1;
  uint64_t forward = 0;
  uint64_t backward = 0;
  OwnedSlice slice = MakeSlice();
  mpeg4::TimingState state = {};
};

mpeg4::TimingState TwoAnchors(VAProfile profile =
                                  VAProfileMPEG4AdvancedSimple) {
  mpeg4::TimingState state;
  state.have_previous = true;
  state.have_newest = true;
  state.previous = {100, 0, mpeg4::Kind::I};
  state.newest = {200, 4, mpeg4::Kind::P};
  state.profile = profile;
  state.width = 640;
  state.height = 360;
  state.resolution = 16;
  state.resync_marker_disable = true;
  return state;
}

Input Later(mpeg4::Kind kind) {
  Input input;
  input.picture = Picture(kind);
  input.token = 300;
  input.state = TwoAnchors();
  if (kind == mpeg4::Kind::I) {
    input.picture.TRD = 4;
  } else if (kind == mpeg4::Kind::P) {
    input.forward = 200;
    input.picture.TRD = 4;
  } else {
    input.forward = 100;
    input.backward = 200;
    input.picture.TRB = 2;
    input.picture.TRD = 4;
    input.picture.vop_fields.bits.backward_reference_vop_coding_type = 1;
  }
  return input;
}

void Reject(const Input &input, const char *why) {
  Bytes output = {0xde, 0xad, 0xbe, 0xef};
  const Bytes original_output = output;
  mpeg4::TimingState destination = TwoAnchors(VAProfileMPEG4Simple);
  destination.previous = {7, 2, mpeg4::Kind::P};
  destination.newest = {9, 8, mpeg4::Kind::I};
  const mpeg4::TimingState original_destination = destination;
  uint64_t tick = 0xfedcba9876543210ULL;
  const auto original_picture = input.picture;
  const auto original_parameters = input.slice.parameters;
  const Bytes original_data = input.slice.data;
  const bool accepted = mpeg4::Assemble(
      input.picture, input.profile, input.token, input.forward, input.backward,
      input.slice.View(), input.state, &output, &destination, &tick);
  Check(!accepted, why);
  Check(output == original_output, "rejection leaves output unchanged");
  Check(Equal(destination, original_destination),
        "rejection leaves destination state unchanged");
  Check(tick == 0xfedcba9876543210ULL,
        "rejection leaves destination tick unchanged");
  Check(std::memcmp(&original_picture, &input.picture,
                    sizeof(original_picture)) == 0,
        "assembler does not mutate picture parameters");
  Check(std::memcmp(&original_parameters, &input.slice.parameters,
                    sizeof(original_parameters)) == 0 &&
            original_data == input.slice.data,
        "assembler does not mutate slice input");
}

struct ExpectedVop {
  mpeg4::Kind kind;
  uint64_t tick;
  uint64_t base;
  unsigned quant_scale;
};

void ParseSequence(const Bytes &bytes, const std::vector<StartCode> &codes,
                   const VAPictureParameterBufferMPEG4 &picture,
                   VAProfile profile, uint64_t group_seconds) {
  const bool advanced = profile == VAProfileMPEG4AdvancedSimple;
  Check(codes.size() == 6,
        "I AU has VOS, VO, video object, VOL, GOV and VOP only");
  Check(codes[0].offset == 0 && codes[0].code == 0xb0,
        "visual object sequence start code");
  Check(codes[1].code == 0xb5 && codes[2].code == 0x00 &&
            codes[3].code == 0x20 && codes[4].code == 0xb3 &&
            codes[5].code == 0xb6,
        "restart header and explicit GOV clock ordering");
  Check(codes[1].offset == 5, "one profile-level byte follows VOS");
  Check(bytes[4] == (advanced ? 0xf5 : 0x05),
        "Simple/Advanced Simple level-5 indication");

  Reader visual(bytes, (codes[1].offset + 4) * 8);
  visual.Equal(1, 1, "visual object identifier present");
  visual.Equal(4, advanced ? 5 : 1, "visual object version");
  visual.Equal(3, 1, "visual object priority");
  visual.Equal(4, 1, "visual object is video");
  visual.Equal(1, 0, "no visual signal type");
  visual.StuffTo(codes[2].offset);
  Check(codes[3].offset == codes[2].offset + 4,
        "empty video-object start precedes VOL");

  Reader vol(bytes, (codes[3].offset + 4) * 8);
  vol.Equal(1, 0, "VOL random access flag");
  vol.Equal(8, advanced ? 0x11 : 0x01, "video object type indication");
  vol.Equal(1, 1, "VOL identifier present");
  vol.Equal(4, advanced ? 5 : 1, "VOL version");
  vol.Equal(3, 1, "VOL priority");
  vol.Equal(4, 1, "square sample aspect ratio");
  vol.Equal(1, 1, "VOL control parameters present");
  vol.Equal(2, 1, "4:2:0 chroma format");
  vol.Equal(1, advanced ? 0 : 1, "profile-compatible low-delay flag");
  vol.Equal(1, 0, "no invented VBV model");
  vol.Equal(2, 0, "rectangular shape");
  vol.Equal(1, 1, "time resolution marker");
  vol.Equal(16, picture.vop_time_increment_resolution,
            "time increment resolution");
  vol.Equal(1, 1, "time resolution trailing marker");
  vol.Equal(1, 0, "variable VOP rate");
  vol.Equal(1, 1, "width marker");
  vol.Equal(13, picture.vop_width, "VOL width");
  vol.Equal(1, 1, "height separator marker");
  vol.Equal(13, picture.vop_height, "VOL height");
  vol.Equal(1, 1, "height trailing marker");
  vol.Equal(1, 0, "progressive VOL");
  vol.Equal(1, 1, "OBMC disabled");
  vol.Equal(advanced ? 2 : 1, 0, "sprite/GMC disabled");
  vol.Equal(1, 0, "default eight-bit quant precision");
  vol.Equal(1, 0, "H.263 quantization matrices");
  if (advanced) vol.Equal(1, 0, "quarter sample disabled");
  vol.Equal(1, 1, "complexity estimation disabled");
  vol.Equal(1, picture.vol_fields.bits.resync_marker_disable,
            "VA resync-marker policy preserved");
  vol.Equal(1, 0, "data partitioning disabled");
  if (advanced) {
    vol.Equal(1, 0, "NEWPRED disabled");
    vol.Equal(1, 0, "reduced-resolution VOP disabled");
  }
  vol.Equal(1, 0, "scalability disabled");
  vol.StuffTo(codes[4].offset);

  const uint64_t day_seconds = group_seconds % (24U * 60U * 60U);
  Reader group(bytes, (codes[4].offset + 4) * 8);
  group.Equal(5, static_cast<uint32_t>(day_seconds / 3600U),
              "GOV hour from earlier-anchor clock");
  group.Equal(6, static_cast<uint32_t>((day_seconds / 60U) % 60U),
              "GOV minute from earlier-anchor clock");
  group.Equal(1, 1, "GOV time-code marker");
  group.Equal(6, static_cast<uint32_t>(day_seconds % 60U),
              "GOV second from earlier-anchor clock");
  group.Equal(1, 0, "GOV is not claimed closed");
  group.Equal(1, 0, "GOV broken-link flag is clear");
  group.StuffTo(codes[5].offset);
}

void Parse(const Bytes &bytes, const VAPictureParameterBufferMPEG4 &picture,
           VAProfile profile, const OwnedSlice &slice,
           const ExpectedVop &expected) {
  const auto codes = StartCodes(bytes);
  if (expected.kind == mpeg4::Kind::I)
    ParseSequence(bytes, codes, picture, profile,
                  expected.base / picture.vop_time_increment_resolution);
  else
    Check(codes.size() == 1 && codes[0].offset == 0 && codes[0].code == 0xb6,
          "P/B AU contains only a VOP start code");
  const StartCode &vop = codes.back();
  Check(vop.code == 0xb6, "last start code is VOP");
  Reader reader(bytes, (vop.offset + 4) * 8);
  const unsigned type = expected.kind == mpeg4::Kind::I ? 0 :
                        expected.kind == mpeg4::Kind::P ? 1 : 2;
  reader.Equal(2, type, "VOP coding type");
  const uint64_t expected_modulo =
      expected.tick / picture.vop_time_increment_resolution -
      expected.base / picture.vop_time_increment_resolution;
  uint64_t modulo = 0;
  while (reader.Get(1) != 0) {
    ++modulo;
    Check(modulo <= 32767, "bounded modulo_time_base unary code");
  }
  Check(modulo == expected_modulo, "relative modulo_time_base");
  reader.Equal(1, 1, "VOP time marker");
  reader.Equal(IndependentTimeBits(picture.vop_time_increment_resolution),
               static_cast<uint32_t>(expected.tick %
                                     picture.vop_time_increment_resolution),
               "absolute in-second VOP time increment");
  reader.Equal(1, 1, "VOP time trailing marker");
  reader.Equal(1, 1, "VOP is coded");
  if (expected.kind == mpeg4::Kind::P)
    reader.Equal(1, picture.vop_fields.bits.vop_rounding_type,
                 "P-VOP rounding type");
  reader.Equal(3, picture.vop_fields.bits.intra_dc_vlc_thr,
               "intra DC VLC threshold");
  reader.Equal(5, expected.quant_scale, "VOP quantizer scale");
  if (expected.kind != mpeg4::Kind::I)
    reader.Equal(3, picture.vop_fcode_forward, "forward fcode");
  if (expected.kind == mpeg4::Kind::B)
    reader.Equal(3, picture.vop_fcode_backward, "backward fcode");

  const uint8_t *source = slice.data.data() + slice.parameters.slice_data_offset;
  const size_t source_end = static_cast<size_t>(slice.parameters.slice_data_size) * 8;
  const size_t copied = source_end - slice.parameters.macroblock_offset;
  Check(reader.position + copied <= bytes.size() * 8,
        "copied macroblock range remains in output");
  for (size_t i = 0; i < copied; ++i)
    Check(Bit(bytes.data(), reader.position + i) ==
              Bit(source, slice.parameters.macroblock_offset + i),
          "macroblock tail copied bit-for-bit");
  reader.position += copied;
  Check(bytes.size() * 8 - reader.position < 8,
        "only byte alignment follows macroblock tail");
  while (reader.position < bytes.size() * 8)
    reader.Equal(1, 0, "final storage padding is zero");
}

void ProfilesAndBitOffsets() {
  for (VAProfile profile : {VAProfileMPEG4Simple,
                            VAProfileMPEG4AdvancedSimple}) {
    for (unsigned offset = 0; offset < 8; ++offset) {
      Input input;
      input.profile = profile;
      input.slice = MakeSlice(offset);
      // Inactive I-picture fields are deliberately nonzero and must not be
      // mistaken for active B/P syntax.
      input.picture.vop_fcode_forward = 255;
      input.picture.vop_fcode_backward = 255;
      input.picture.vop_fields.bits.backward_reference_vop_coding_type = 3;
      Bytes output;
      mpeg4::TimingState next;
      uint64_t tick = 99;
      Check(mpeg4::Assemble(input.picture, input.profile, input.token, 0, 0,
                            input.slice.View(), input.state, &output, &next,
                            &tick),
            "strict Simple/Advanced Simple I picture accepted");
      Check(tick == 0, "first I starts at tick zero");
      Check(!next.have_previous && next.have_newest &&
                next.newest.token == input.token && next.newest.tick == 0 &&
                next.newest.kind == mpeg4::Kind::I &&
                next.profile == profile && next.width == 640 &&
                next.height == 360 && next.resolution == 16 &&
                next.resync_marker_disable,
            "first I initializes immutable sequence timing state");
      Parse(output, input.picture, profile, input.slice,
            {mpeg4::Kind::I, 0, 0, 17});
    }
  }
  ++groups;
}

void AnchorTimingAndOpenGop() {
  const VAProfile profile = VAProfileMPEG4AdvancedSimple;
  OwnedSlice slice = MakeSlice(5);
  mpeg4::TimingState state;
  Bytes output;
  uint64_t tick = 0;

  auto i0 = Picture(mpeg4::Kind::I);
  Check(mpeg4::Assemble(i0, profile, 100, 0, 0, slice.View(), state,
                        &output, &state, &tick),
        "initial anchor I accepted");
  Check(tick == 0, "initial anchor tick");
  Parse(output, i0, profile, slice, {mpeg4::Kind::I, 0, 0, 17});

  auto p4 = Picture(mpeg4::Kind::P);
  p4.TRD = 4;
  Check(mpeg4::Assemble(p4, profile, 200, 100, 0, slice.View(), state,
                        &output, &state, &tick),
        "P advances newest anchor by TRD");
  Check(tick == 4 && state.previous.token == 100 &&
            state.previous.tick == 0 && state.newest.token == 200 &&
            state.newest.tick == 4 && state.newest.kind == mpeg4::Kind::P,
        "P commits two-anchor timing window");
  Parse(output, p4, profile, slice, {mpeg4::Kind::P, 4, 0, 17});

  const mpeg4::TimingState before_b = state;
  auto b2 = Picture(mpeg4::Kind::B);
  b2.TRB = 2;
  b2.TRD = 4;
  b2.vop_fields.bits.backward_reference_vop_coding_type = 1;
  Check(mpeg4::Assemble(b2, profile, 300, 100, 200, slice.View(), state,
                        &output, &state, &tick),
        "B between current anchors accepted");
  Check(tick == 2 && Equal(state, before_b),
        "B uses previous plus TRB and does not move anchors");
  Parse(output, b2, profile, slice, {mpeg4::Kind::B, 2, 0, 17});

  auto i24 = Picture(mpeg4::Kind::I);
  i24.TRD = 20;
  Check(mpeg4::Assemble(i24, profile, 400, 0, 0, slice.View(), state,
                        &output, &state, &tick),
        "later I advances newest anchor and restarts headers");
  Check(tick == 24 && state.previous.token == 200 &&
            state.previous.tick == 4 && state.newest.token == 400 &&
            state.newest.tick == 24 && state.newest.kind == mpeg4::Kind::I,
        "later I retains earlier P for an open-GOP leading B");
  Parse(output, i24, profile, slice, {mpeg4::Kind::I, 24, 4, 17});

  const mpeg4::TimingState before_open_b = state;
  auto b14 = Picture(mpeg4::Kind::B);
  b14.TRB = 10;
  b14.TRD = 20;
  b14.vop_fields.bits.backward_reference_vop_coding_type = 0;
  Check(mpeg4::Assemble(b14, profile, 500, 200, 400, slice.View(), state,
                        &output, &state, &tick),
        "open-GOP B referencing the pre-I anchor accepted");
  Check(tick == 14 && Equal(state, before_open_b),
        "open-GOP B timing is relative to previous anchor");
  Parse(output, b14, profile, slice, {mpeg4::Kind::B, 14, 4, 17});
  ++groups;
}

void SimpleZeroTrdAnchors() {
  const VAProfile profile = VAProfileMPEG4Simple;
  OwnedSlice slice = MakeSlice(4);
  mpeg4::TimingState state;
  Bytes output;
  uint64_t tick = 99;

  auto i0 = Picture(mpeg4::Kind::I);
  Check(mpeg4::Assemble(i0, profile, 100, 0, 0, slice.View(), state,
                        &output, &state, &tick),
        "initial Simple I accepted");
  Check(tick == 0, "initial Simple I starts at zero");

  auto p1 = Picture(mpeg4::Kind::P);
  p1.TRD = 0;
  Check(mpeg4::Assemble(p1, profile, 200, 100, 0, slice.View(), state,
                        &output, &state, &tick),
        "zero-TRD Simple P receives a synthetic anchor interval");
  Check(tick == 1 && state.have_previous &&
            state.previous.token == 100 && state.previous.tick == 0 &&
            state.newest.token == 200 && state.newest.tick == 1 &&
            state.newest.kind == mpeg4::Kind::P,
        "zero-TRD Simple P commits exactly one monotonic tick");
  Parse(output, p1, profile, slice, {mpeg4::Kind::P, 1, 0, 17});

  auto i2 = Picture(mpeg4::Kind::I);
  i2.TRD = 0;
  Check(mpeg4::Assemble(i2, profile, 300, 0, 0, slice.View(), state,
                        &output, &state, &tick),
        "zero-TRD later Simple I receives a synthetic anchor interval");
  Check(tick == 2 && state.previous.token == 200 &&
            state.previous.tick == 1 && state.previous.kind == mpeg4::Kind::P &&
            state.newest.token == 300 && state.newest.tick == 2 &&
            state.newest.kind == mpeg4::Kind::I,
        "zero-TRD Simple I commits exactly one monotonic tick");
  Parse(output, i2, profile, slice, {mpeg4::Kind::I, 2, 2, 17});

  auto p6 = Picture(mpeg4::Kind::P);
  p6.TRD = 4;
  Check(mpeg4::Assemble(p6, profile, 400, 300, 0, slice.View(), state,
                        &output, &state, &tick),
        "positive Simple TRD remains accepted");
  Check(tick == 6 && state.previous.tick == 2 && state.newest.tick == 6,
        "positive Simple TRD is preserved exactly");
  Parse(output, p6, profile, slice, {mpeg4::Kind::P, 6, 2, 17});
  ++groups;
}

void SecondBoundaryGroupClock() {
  OwnedSlice slice = MakeSlice(6);
  Bytes output;
  uint64_t tick = 0;

  // Simple has no leading B pictures, so match native streams: GOV names the
  // current I second and the I has a zero modulo_time_base.
  mpeg4::TimingState simple;
  simple.have_previous = true;
  simple.have_newest = true;
  simple.previous = {59, 58, mpeg4::Kind::P};
  simple.newest = {60, 59, mpeg4::Kind::P};
  simple.profile = VAProfileMPEG4Simple;
  simple.width = 640;
  simple.height = 360;
  simple.resolution = 30;
  simple.resync_marker_disable = true;
  auto simple_i = Picture(mpeg4::Kind::I);
  simple_i.vop_time_increment_resolution = 30;
  simple_i.TRD = 1;
  mpeg4::TimingState simple_next;
  Check(mpeg4::Assemble(simple_i, VAProfileMPEG4Simple, 61, 0, 0,
                        slice.View(), simple, &output, &simple_next, &tick),
        "second-boundary Simple I accepted");
  Check(tick == 60 && simple_next.previous.tick == 59 &&
            simple_next.newest.tick == 60,
        "second-boundary Simple anchors remain absolute");
  Parse(output, simple_i, VAProfileMPEG4Simple, slice,
        {mpeg4::Kind::I, 60, 60, 17});

  // This is the native ASP ordering around display frames 58--61: P57, I60,
  // then leading B58/B59.  GOV must remain at second 1 and I modulo must remain
  // one; setting GOV to I's second and forcing modulo zero would make both B
  // timestamps precede the decoder's last_time_base and break the open GOP.
  mpeg4::TimingState advanced;
  advanced.have_previous = true;
  advanced.have_newest = true;
  advanced.previous = {55, 54, mpeg4::Kind::P};
  advanced.newest = {58, 57, mpeg4::Kind::P};
  advanced.profile = VAProfileMPEG4AdvancedSimple;
  advanced.width = 640;
  advanced.height = 360;
  advanced.resolution = 30;
  advanced.resync_marker_disable = true;
  auto advanced_i = Picture(mpeg4::Kind::I);
  advanced_i.vop_time_increment_resolution = 30;
  advanced_i.TRD = 3;
  mpeg4::TimingState advanced_next;
  Check(mpeg4::Assemble(advanced_i, VAProfileMPEG4AdvancedSimple, 61, 0, 0,
                        slice.View(), advanced, &output, &advanced_next,
                        &tick),
        "second-boundary Advanced Simple I accepted");
  Check(tick == 60 && advanced_next.previous.tick == 57 &&
            advanced_next.newest.tick == 60,
        "ASP I retains the pre-I anchor for leading B pictures");
  Parse(output, advanced_i, VAProfileMPEG4AdvancedSimple, slice,
        {mpeg4::Kind::I, 60, 57, 17});

  for (int16_t trb : {int16_t{1}, int16_t{2}}) {
    auto leading_b = Picture(mpeg4::Kind::B);
    leading_b.vop_time_increment_resolution = 30;
    leading_b.TRB = trb;
    leading_b.TRD = 3;
    leading_b.vop_fields.bits.backward_reference_vop_coding_type = 0;
    const mpeg4::TimingState before_b = advanced_next;
    Check(mpeg4::Assemble(leading_b, VAProfileMPEG4AdvancedSimple,
                          61 + static_cast<uint64_t>(trb), 58, 61,
                          slice.View(), advanced_next, &output,
                          &advanced_next, &tick),
          "second-boundary leading B accepted");
    Check(tick == static_cast<uint64_t>(57 + trb) &&
              Equal(advanced_next, before_b),
          "leading B remains between the GOV/I anchors");
    Parse(output, leading_b, VAProfileMPEG4AdvancedSimple, slice,
          {mpeg4::Kind::B, static_cast<uint64_t>(57 + trb), 57, 17});
  }
  ++groups;
}

void TimeResolutionEdges() {
  for (uint16_t resolution : {uint16_t{1}, uint16_t{2}, uint16_t{16},
                              uint16_t{17}, uint16_t{65535}}) {
    Input input;
    input.picture.vop_time_increment_resolution = resolution;
    Bytes output;
    mpeg4::TimingState next;
    uint64_t tick = 7;
    Check(mpeg4::Assemble(input.picture, input.profile, 1, 0, 0,
                          input.slice.View(), input.state, &output, &next,
                          &tick),
          "valid time resolution accepted");
    Check(tick == 0, "first-I edge resolution tick");
    Parse(output, input.picture, input.profile, input.slice,
          {mpeg4::Kind::I, 0, 0, 17});
  }
  ++groups;
}

void SimpleResyncMarkers() {
  Input input;
  input.profile = VAProfileMPEG4Simple;
  input.picture.vol_fields.bits.resync_marker_disable = 0;
  input.picture.vop_time_increment_resolution = 30;
  input.slice = MakePacketSlice(16, 3,
                                {{320, 4, 0}, {600, 4, 0}});
  Check(input.slice.data[1] == 0 && input.slice.data[2] == 0 &&
            input.slice.data[3] == 0xa8 && input.slice.data[4] == 0x04,
        "independent I packet header encodes MB 320, q=4 and HEC=0");
  Bytes output;
  mpeg4::TimingState next;
  uint64_t tick = 7;
  Check(mpeg4::Assemble(input.picture, input.profile, input.token, 0, 0,
                        input.slice.View(), input.state, &output, &next,
                        &tick),
        "resync-enabled Simple picture accepted");
  Check(!next.resync_marker_disable && tick == 0,
        "Simple resync policy is retained in timing state");
  Parse(output, input.picture, input.profile, input.slice,
        {mpeg4::Kind::I, 0, 0, 17});

  for (unsigned fcode : {1U, 7U}) {
    Input p = Later(mpeg4::Kind::P);
    p.profile = VAProfileMPEG4Simple;
    p.state.profile = VAProfileMPEG4Simple;
    p.state.resolution = 30;
    p.state.resync_marker_disable = false;
    p.picture.vop_time_increment_resolution = 30;
    p.picture.vol_fields.bits.resync_marker_disable = 0;
    p.picture.vop_fcode_forward = fcode;
    p.slice = MakePacketSlice(15 + fcode, 7,
                              {{320, 4, 0}, {600, 4, 0}});
    const mpeg4::TimingState before = p.state;
    Check(mpeg4::Assemble(p.picture, p.profile, p.token, p.forward, p.backward,
                          p.slice.View(), p.state, &output, &next, &tick),
          "boundary-length Simple P packet markers accepted");
    Check(tick == 8 && next.have_previous &&
              next.previous.token == before.newest.token,
          "resync-enabled Simple P commits ordinary anchor timing");
    Parse(output, p.picture, p.profile, p.slice,
          {mpeg4::Kind::P, 8, 4, 17});
  }

  {
    Input x = input;
    x.slice = MakePacketSlice(16, 3, {{0, 4, 0}});
    Reject(x, "zero packet macroblock rejected");
  }
  {
    Input x = input;
    x.slice = MakePacketSlice(16, 3, {{920, 4, 0}});
    Reject(x, "out-of-range packet macroblock rejected");
  }
  {
    Input x = input;
    x.slice = MakePacketSlice(16, 3, {{320, 0, 0}});
    Reject(x, "zero packet quantizer rejected");
  }
  {
    Input x = input;
    x.slice = MakePacketSlice(16, 3, {{320, 4, 1}});
    Reject(x, "packet header extension rejected");
  }
  {
    Input x = input;
    x.slice = MakePacketSlice(16, 3,
                              {{320, 4, 0}, {320, 4, 0}});
    Reject(x, "duplicate packet macroblock rejected");
  }
  {
    Input x = input;
    x.slice = MakePacketSlice(16, 3,
                              {{600, 4, 0}, {320, 4, 0}});
    Reject(x, "decreasing packet macroblock rejected");
  }
  {
    Input x = input;
    x.slice = MakePacketSlice(16, 3, {{320, 4, 0}});
    x.slice.data.resize(5);
    x.slice.parameters.slice_data_size = x.slice.data.size();
    Reject(x, "truncated packet header rejected");
  }
  {
    Input x = input;
    x.slice = MakePacketSlice(16, 3, {{320, 4, 0}});
    x.slice.parameters.macroblock_offset = 2;
    Reject(x, "packet marker output alignment mismatch rejected");
  }
  ++groups;
}

void PictureRejections() {
  { Input x; x.profile = VAProfileMPEG4Main; Reject(x, "MPEG-4 Main rejected"); }
  { Input x; x.picture.vop_width = 0; Reject(x, "zero width rejected"); }
  { Input x; x.picture.vop_width = 641; Reject(x, "odd width rejected"); }
  { Input x; x.picture.vop_width = 1922; Reject(x, "oversize width rejected"); }
  { Input x; x.picture.vop_height = 1089; Reject(x, "oversize height rejected"); }
  { Input x; x.picture.vop_time_increment_resolution = 0; Reject(x, "zero timing resolution rejected"); }
  { Input x; x.picture.quant_precision = 4; Reject(x, "non-eight-bit quant precision rejected"); }
  { Input x; x.picture.vol_fields.value |= 1U << 31; Reject(x, "reserved VOL flags rejected"); }
  { Input x; x.picture.vop_fields.value |= 1U << 31; Reject(x, "reserved VOP flags rejected"); }
  { Input x; x.picture.va_reserved[0] = 1; Reject(x, "reserved picture bytes rejected"); }
  { Input x; x.picture.vol_fields.bits.short_video_header = 1; Reject(x, "short-video header rejected"); }
  { Input x; x.picture.vol_fields.bits.chroma_format = 2; Reject(x, "non-420 chroma rejected"); }
  { Input x; x.picture.vol_fields.bits.interlaced = 1; Reject(x, "interlace rejected"); }
  { Input x; x.picture.vol_fields.bits.obmc_disable = 0; Reject(x, "OBMC rejected"); }
  { Input x; x.picture.vol_fields.bits.sprite_enable = 1; Reject(x, "sprite rejected"); }
  { Input x; x.picture.vol_fields.bits.sprite_warping_accuracy = 1; Reject(x, "sprite accuracy rejected"); }
  { Input x; x.picture.no_of_sprite_warping_points = 1; Reject(x, "sprite points rejected"); }
  { Input x; x.picture.sprite_trajectory_du[1] = 1; Reject(x, "sprite trajectory rejected"); }
  { Input x; x.picture.sprite_trajectory_dv[2] = -1; Reject(x, "sprite trajectory sign rejected"); }
  { Input x; x.picture.vol_fields.bits.quant_type = 1; Reject(x, "custom matrices rejected"); }
  { Input x; x.picture.vol_fields.bits.quarter_sample = 1; Reject(x, "quarter sample rejected"); }
  { Input x; x.picture.vol_fields.bits.data_partitioned = 1; Reject(x, "data partitioning rejected"); }
  { Input x; x.picture.vol_fields.bits.reversible_vlc = 1; Reject(x, "reversible VLC rejected"); }
  { Input x; x.picture.vol_fields.bits.resync_marker_disable = 0; Reject(x, "resync-enabled Advanced Simple rejected"); }
  { Input x; x.picture.vop_fields.bits.top_field_first = 1; Reject(x, "field order rejected"); }
  { Input x; x.picture.vop_fields.bits.alternate_vertical_scan_flag = 1; Reject(x, "alternate scan rejected"); }
  { Input x; x.picture.vop_fields.bits.vop_coding_type = 3; Reject(x, "reserved coding type rejected"); }

  {
    Input x = Later(mpeg4::Kind::B);
    x.profile = VAProfileMPEG4Simple;
    x.state.profile = VAProfileMPEG4Simple;
    Reject(x, "B-VOP rejected in Simple profile");
  }
  { Input x = Later(mpeg4::Kind::P); x.picture.vop_fcode_forward = 0; Reject(x, "zero P fcode rejected"); }
  { Input x = Later(mpeg4::Kind::P); x.picture.vop_fcode_forward = 8; Reject(x, "large P fcode rejected"); }
  { Input x = Later(mpeg4::Kind::B); x.picture.vop_fcode_backward = 0; Reject(x, "zero B fcode rejected"); }
  { Input x = Later(mpeg4::Kind::B); x.picture.vop_fcode_backward = 8; Reject(x, "large B fcode rejected"); }
  ++groups;
}

void TimingAndReferenceRejections() {
  { Input x; x.picture = Picture(mpeg4::Kind::P); x.picture.TRD = 1; x.forward = 1; Reject(x, "stream cannot begin with P"); }
  { Input x; x.picture.forward_reference_picture = 4; Reject(x, "first I surface reference rejected"); }
  { Input x; x.forward = 9; Reject(x, "first I token reference rejected"); }

  { Input x = Later(mpeg4::Kind::I); x.profile = VAProfileMPEG4Simple; Reject(x, "midstream profile change rejected"); }
  { Input x = Later(mpeg4::Kind::I); x.picture.vop_width = 642; Reject(x, "midstream geometry change rejected"); }
  { Input x = Later(mpeg4::Kind::I); x.picture.vop_time_increment_resolution = 15; Reject(x, "midstream time base change rejected"); }
  { Input x = Later(mpeg4::Kind::I); x.profile = VAProfileMPEG4Simple; x.state.profile = VAProfileMPEG4Simple; x.picture.vol_fields.bits.resync_marker_disable = 0; Reject(x, "midstream Simple resync policy change rejected"); }
  { Input x = Later(mpeg4::Kind::I); x.profile = VAProfileMPEG4Simple; x.state.profile = VAProfileMPEG4Simple; x.state.resync_marker_disable = false; Reject(x, "reverse midstream Simple resync policy change rejected"); }
  { Input x = Later(mpeg4::Kind::I); x.token = 200; Reject(x, "nonmonotonic anchor token rejected"); }
  { Input x = Later(mpeg4::Kind::I); x.picture.TRD = 0; Reject(x, "zero Advanced Simple I TRD rejected"); }
  { Input x = Later(mpeg4::Kind::I); x.picture.TRD = -1; Reject(x, "negative anchor TRD rejected"); }
  { Input x = Later(mpeg4::Kind::I); x.profile = VAProfileMPEG4Simple; x.state.profile = VAProfileMPEG4Simple; x.picture.TRD = -1; Reject(x, "negative Simple I TRD rejected"); }
  { Input x = Later(mpeg4::Kind::I); x.picture.forward_reference_picture = 7; Reject(x, "later I surface reference rejected"); }

  { Input x = Later(mpeg4::Kind::P); x.forward = 100; Reject(x, "P must reference newest token"); }
  { Input x = Later(mpeg4::Kind::P); x.backward = 200; Reject(x, "P backward token rejected"); }
  { Input x = Later(mpeg4::Kind::P); x.picture.forward_reference_picture = VA_INVALID_SURFACE; Reject(x, "P missing VA reference rejected"); }
  { Input x = Later(mpeg4::Kind::P); x.picture.backward_reference_picture = 9; Reject(x, "P backward VA reference rejected"); }
  { Input x = Later(mpeg4::Kind::P); x.picture.TRD = 0; Reject(x, "zero Advanced Simple P TRD rejected"); }
  { Input x = Later(mpeg4::Kind::P); x.picture.TRD = -1; Reject(x, "negative Advanced Simple P TRD rejected"); }
  { Input x = Later(mpeg4::Kind::P); x.profile = VAProfileMPEG4Simple; x.state.profile = VAProfileMPEG4Simple; x.picture.TRD = -1; Reject(x, "negative Simple P TRD rejected"); }
  { Input x = Later(mpeg4::Kind::P); x.profile = VAProfileMPEG4Simple; x.state.profile = VAProfileMPEG4Simple; x.picture.vol_fields.bits.resync_marker_disable = 0; Reject(x, "P cannot change Simple resync policy"); }
  { Input x = Later(mpeg4::Kind::P); x.profile = VAProfileMPEG4Simple; x.state.profile = VAProfileMPEG4Simple; x.state.resync_marker_disable = false; Reject(x, "P cannot reverse Simple resync policy"); }

  { Input x = Later(mpeg4::Kind::B); x.state.have_previous = false; x.state.previous = {}; Reject(x, "B without two anchors rejected"); }
  { Input x = Later(mpeg4::Kind::B); x.forward = 200; Reject(x, "B forward token must be previous"); }
  { Input x = Later(mpeg4::Kind::B); x.backward = 100; Reject(x, "B backward token must be newest"); }
  { Input x = Later(mpeg4::Kind::B); x.picture.forward_reference_picture = VA_INVALID_SURFACE; Reject(x, "B missing VA reference rejected"); }
  { Input x = Later(mpeg4::Kind::B); x.picture.backward_reference_picture = x.picture.forward_reference_picture; Reject(x, "B aliases VA references"); }
  { Input x = Later(mpeg4::Kind::B); x.picture.TRB = 0; Reject(x, "B zero TRB rejected"); }
  { Input x = Later(mpeg4::Kind::B); x.picture.TRB = -1; Reject(x, "B negative TRB rejected"); }
  { Input x = Later(mpeg4::Kind::B); x.picture.TRB = 4; Reject(x, "B TRB equal to TRD rejected"); }
  { Input x = Later(mpeg4::Kind::B); x.picture.TRD = 5; Reject(x, "B TRD inconsistent with anchors rejected"); }
  { Input x = Later(mpeg4::Kind::B); x.picture.vop_fields.bits.backward_reference_vop_coding_type = 0; Reject(x, "B backward coding type mismatch rejected"); }
  { Input x = Later(mpeg4::Kind::B); x.picture.vol_fields.bits.resync_marker_disable = 0; Reject(x, "B cannot enter resync-enabled Advanced Simple mode"); }

  { Input x = Later(mpeg4::Kind::P); x.state.newest.tick = UINT64_MAX - 1; x.state.previous.tick = UINT64_MAX - 5; Reject(x, "anchor tick overflow rejected"); }
  { Input x = Later(mpeg4::Kind::P); x.state.newest.kind = mpeg4::Kind::B; Reject(x, "B cannot be stored as an anchor"); }
  { Input x = Later(mpeg4::Kind::P); x.state.previous.token = x.state.newest.token; Reject(x, "duplicate state token rejected"); }
  { Input x; x.state.have_previous = true; Reject(x, "previous state without newest rejected"); }
  ++groups;
}

void SliceAndBoundRejections() {
  {
    Input x;
    Bytes output = {1, 2, 3};
    mpeg4::TimingState next;
    Check(!mpeg4::Assemble(x.picture, x.profile, x.token, 0, 0,
                           x.slice.View(), x.state, nullptr, &next),
          "null output destination rejected");
    Check(!mpeg4::Assemble(x.picture, x.profile, x.token, 0, 0,
                           x.slice.View(), x.state, &output, nullptr),
          "null state destination rejected");
    Check(output == Bytes({1, 2, 3}),
          "null-destination rejection remains transactional");
  }
  { Input x; x.slice.data.clear(); Reject(x, "null slice storage rejected"); }
  { Input x; x.slice.parameters.slice_data_flag = VA_SLICE_DATA_FLAG_BEGIN; Reject(x, "partial BEGIN slice rejected"); }
  { Input x; x.slice.parameters.slice_data_size = 0; Reject(x, "empty slice rejected"); }
  { Input x; x.slice.parameters.slice_data_size = mpeg4::kMaxAccessUnitBytes + 1; Reject(x, "oversize declared slice rejected"); }
  { Input x; x.slice.parameters.slice_data_offset = x.slice.data.size() + 1; Reject(x, "slice offset outside buffer rejected"); }
  { Input x; x.slice.parameters.slice_data_size = 5; Reject(x, "slice range outside buffer rejected"); }
  { Input x; x.slice.parameters.macroblock_number = 1; Reject(x, "nonzero first macroblock rejected"); }
  { Input x; x.slice.parameters.quant_scale = 0; Reject(x, "zero quantizer rejected"); }
  { Input x; x.slice.parameters.quant_scale = 32; Reject(x, "large quantizer rejected"); }
  { Input x; x.slice.parameters.macroblock_offset = 32; Reject(x, "macroblock offset at range end rejected"); }
  { Input x; x.slice.parameters.va_reserved[0] = 1; Reject(x, "reserved slice bytes rejected"); }
  {
    Input x;
    x.slice.data.assign(mpeg4::kMaxAccessUnitBytes, 0xa5);
    x.slice.parameters.slice_data_offset = 0;
    x.slice.parameters.slice_data_size = mpeg4::kMaxAccessUnitBytes;
    x.slice.parameters.macroblock_offset = 0;
    Reject(x, "headers plus maximum slice cannot exceed AU bound");
  }
  ++groups;
}

}  // namespace

int main() {
  try {
    ProfilesAndBitOffsets();
    AnchorTimingAndOpenGop();
    SimpleZeroTrdAnchors();
    SecondBoundaryGroupClock();
    TimeResolutionEdges();
    SimpleResyncMarkers();
    PictureRejections();
    TimingAndReferenceRejections();
    SliceAndBoundRejections();
    std::printf("PASS: %u MPEG-4 VA-API groups, %u checks\n", groups, checks);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL after %u groups and %u checks: %s\n",
                 groups, checks, error.what());
    return 1;
  }
}
