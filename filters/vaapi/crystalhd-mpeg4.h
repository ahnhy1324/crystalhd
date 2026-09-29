// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_MPEG4_H
#define CRYSTALHD_MPEG4_H

// Bounded MPEG-4 Part 2 elementary-stream assembly for the progressive
// Simple/Advanced Simple subset accepted by the CrystalHD transport.  VA gives
// a decoder macroblock payload, not the VOL/VOP syntax needed by CrystalHD, so
// this reconstructs only fields represented by the VA picture parameters.
#include <va/va.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace crystalhd_mpeg4 {

// A transport/replay bound, not a general MPEG-4 elementary-stream limit.
constexpr size_t kMaxAccessUnitBytes = 512 * 1024;

enum class Kind { I, P, B };

struct Anchor {
  uint64_t token = 0;
  uint64_t tick = 0;
  Kind kind = Kind::I;
};

// Ticks use vop_time_increment_resolution units.  The two-anchor window is
// sufficient for VA's TRB/TRD model and deliberately survives B pictures.
struct TimingState {
  bool have_previous = false;
  bool have_newest = false;
  Anchor previous = {};
  Anchor newest = {};
  VAProfile profile = VAProfileNone;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t resolution = 0;
  bool resync_marker_disable = false;
};

// Offsets are relative to this slice's data buffer.  One Assemble call accepts
// exactly one complete VA_SLICE_DATA_FLAG_ALL slice.
struct Slice {
  VASliceParameterBufferMPEG4 parameters;
  const uint8_t *data;
  size_t size;
};

namespace detail {

inline bool ZeroReserved(const uint32_t *values, size_t count) {
  for (size_t i = 0; i < count; ++i)
    if (values[i] != 0) return false;
  return true;
}

inline bool IsAnchorKind(Kind kind) {
  return kind == Kind::I || kind == Kind::P;
}

inline unsigned TimeBits(uint16_t resolution) {
  // ceil(log2(resolution)), with the syntax-required minimum of one bit.
  unsigned bits = 1;
  while ((uint32_t{1} << bits) < resolution) ++bits;
  return bits;
}

class Bits {
 public:
  bool Put(unsigned count, uint32_t value) {
    if (count > 32 || position_ > kMaxAccessUnitBytes * 8 ||
        count > kMaxAccessUnitBytes * 8 - position_)
      return false;
    while (count--) {
      if ((position_ & 7) == 0) bytes_.push_back(0);
      bytes_.back() |= static_cast<uint8_t>(((value >> count) & 1U)
                                           << (7 - (position_ & 7)));
      ++position_;
    }
    return true;
  }

  bool AlignZero() {
    return (position_ & 7) == 0 || Put(8 - (position_ & 7), 0);
  }

  bool Start(uint8_t code) {
    if (!AlignZero() || bytes_.size() > kMaxAccessUnitBytes - 4) return false;
    bytes_.insert(bytes_.end(), {0, 0, 1, code});
    position_ += 32;
    return true;
  }

  // MPEG-4 next_start_code stuffing: a zero followed by enough ones to reach
  // a byte boundary.  Callers use this after VO and VOL syntax only.
  bool Stuff() {
    if (!Put(1, 0)) return false;
    while (position_ & 7)
      if (!Put(1, 1)) return false;
    return true;
  }

  bool Copy(const uint8_t *source, size_t begin, size_t end) {
    if (source == nullptr || begin > end ||
        position_ > kMaxAccessUnitBytes * 8 ||
        end - begin > kMaxAccessUnitBytes * 8 - position_)
      return false;

    // First fill a partial destination byte, then synthesize whole bytes even
    // when the source macroblock offset itself is not byte aligned.
    while (begin < end && (position_ & 7)) {
      if (!Put(1, (source[begin / 8] >> (7 - begin % 8)) & 1U)) return false;
      ++begin;
    }
    while (end - begin >= 8) {
      const unsigned shift = begin & 7;
      uint16_t joined = static_cast<uint16_t>(source[begin / 8]) << 8;
      if (shift != 0) joined |= source[begin / 8 + 1];
      const uint8_t byte = static_cast<uint8_t>((joined << shift) >> 8);
      bytes_.push_back(byte);
      position_ += 8;
      begin += 8;
    }
    while (begin < end) {
      if (!Put(1, (source[begin / 8] >> (7 - begin % 8)) & 1U)) return false;
      ++begin;
    }
    return true;
  }

  unsigned PositionMod8() const {
    return static_cast<unsigned>(position_ & 7);
  }

  std::vector<uint8_t> Take() { return std::move(bytes_); }

 private:
  std::vector<uint8_t> bytes_;
  size_t position_ = 0;
};

inline bool ValidProfile(VAProfile profile) {
  return profile == VAProfileMPEG4Simple ||
         profile == VAProfileMPEG4AdvancedSimple;
}

inline bool ValidState(const TimingState &state) {
  if (state.have_previous && !state.have_newest) return false;
  if (!state.have_newest) {
    return !state.have_previous && state.previous.token == 0 &&
           state.previous.tick == 0 && state.previous.kind == Kind::I &&
           state.newest.token == 0 && state.newest.tick == 0 &&
           state.newest.kind == Kind::I && state.profile == VAProfileNone &&
           state.width == 0 && state.height == 0 && state.resolution == 0 &&
           !state.resync_marker_disable;
  }
  if (!ValidProfile(state.profile) || state.width == 0 || state.height == 0 ||
      state.resolution == 0 || !state.newest.token ||
      !IsAnchorKind(state.newest.kind))
    return false;
  if (!state.have_previous)
    return state.previous.token == 0 && state.previous.tick == 0 &&
           state.previous.kind == Kind::I;
  return state.previous.token && IsAnchorKind(state.previous.kind) &&
         state.previous.token < state.newest.token &&
         state.previous.tick < state.newest.tick;
}

inline bool ValidPicture(const VAPictureParameterBufferMPEG4 &picture,
                         VAProfile profile, const TimingState &state,
                         uint64_t token, uint64_t forward, uint64_t backward,
                         Kind *kind, uint64_t *tick, uint64_t *base,
                         TimingState *next) {
  if (!ValidProfile(profile) || !ValidState(state) || !token ||
      picture.vop_width == 0 || picture.vop_height == 0 ||
      picture.vop_width > 1920 || picture.vop_height > 1088 ||
      ((picture.vop_width | picture.vop_height) & 1U) ||
      picture.vop_time_increment_resolution == 0 ||
      picture.quant_precision != 5 ||
      (picture.vol_fields.value & ~uint32_t{0x3fff}) != 0 ||
      (picture.vop_fields.value & ~uint32_t{0x03ff}) != 0 ||
      !ZeroReserved(picture.va_reserved, VA_PADDING_LOW))
    return false;

  const auto &vol = picture.vol_fields.bits;
  const auto &vop = picture.vop_fields.bits;
  if (vol.short_video_header || vol.chroma_format != 1 || vol.interlaced ||
      !vol.obmc_disable || vol.sprite_enable || vol.sprite_warping_accuracy ||
      vol.quant_type || vol.quarter_sample || vol.data_partitioned ||
      vol.reversible_vlc ||
      picture.no_of_sprite_warping_points ||
      vop.top_field_first || vop.alternate_vertical_scan_flag)
    return false;
  // VA supplies the complete macroblock payload, including video-packet
  // headers.  The assembler below admits resync-enabled Simple only after
  // validating that every packet omits its optional repeated timing header.
  // Keep Advanced Simple marker-disabled until reordered B-VOP packets are
  // separately validated.
  if (!vol.resync_marker_disable &&
      profile != VAProfileMPEG4Simple)
    return false;
  for (unsigned i = 0; i < 3; ++i)
    if (picture.sprite_trajectory_du[i] || picture.sprite_trajectory_dv[i])
      return false;

  if (vop.vop_coding_type > 2) return false;
  *kind = vop.vop_coding_type == 0 ? Kind::I :
          vop.vop_coding_type == 1 ? Kind::P : Kind::B;
  if (profile == VAProfileMPEG4Simple && *kind == Kind::B) return false;
  if ((*kind == Kind::P || *kind == Kind::B) &&
      (picture.vop_fcode_forward < 1 || picture.vop_fcode_forward > 7))
    return false;
  if (*kind == Kind::B &&
      (picture.vop_fcode_backward < 1 || picture.vop_fcode_backward > 7))
    return false;

  if (!state.have_newest) {
    if (*kind != Kind::I || forward || backward ||
        picture.forward_reference_picture != VA_INVALID_SURFACE ||
        picture.backward_reference_picture != VA_INVALID_SURFACE)
      return false;
    *tick = 0;
    *base = 0;
    *next = {};
    next->have_newest = true;
    next->newest = {token, 0, Kind::I};
    next->profile = profile;
    next->width = picture.vop_width;
    next->height = picture.vop_height;
    next->resolution = picture.vop_time_increment_resolution;
    next->resync_marker_disable = vol.resync_marker_disable;
    return true;
  }

  if (profile != state.profile || picture.vop_width != state.width ||
      picture.vop_height != state.height ||
      picture.vop_time_increment_resolution != state.resolution ||
      static_cast<bool>(vol.resync_marker_disable) !=
          state.resync_marker_disable ||
      token <= state.newest.token)
    return false;

  *next = state;
  if (*kind == Kind::I) {
    if (forward || backward ||
        picture.forward_reference_picture != VA_INVALID_SURFACE ||
        picture.backward_reference_picture != VA_INVALID_SURFACE)
      return false;
  } else if (*kind == Kind::P) {
    if (forward != state.newest.token || backward ||
        picture.forward_reference_picture == VA_INVALID_SURFACE ||
        picture.backward_reference_picture != VA_INVALID_SURFACE)
      return false;
  } else {
    if (!state.have_previous || forward != state.previous.token ||
        backward != state.newest.token ||
        picture.forward_reference_picture == VA_INVALID_SURFACE ||
        picture.backward_reference_picture == VA_INVALID_SURFACE ||
        picture.forward_reference_picture == picture.backward_reference_picture ||
        picture.TRB <= 0 || picture.TRD <= 0 || picture.TRB >= picture.TRD ||
        state.newest.tick - state.previous.tick !=
            static_cast<uint64_t>(picture.TRD) ||
        vop.backward_reference_vop_coding_type !=
            (state.newest.kind == Kind::I ? 0U : 1U))
      return false;
    *tick = state.previous.tick + static_cast<uint64_t>(picture.TRB);
    *base = state.previous.tick;
    return true;  // B pictures never move the anchor window.
  }

  if (picture.TRD < 0) return false;
  // Some VA frontends omit TRD for Simple Profile anchors.  With no B-VOPs
  // to position inside the interval, a single tick is the narrowest monotonic
  // substitute.  Advanced Simple retains an explicit positive interval
  // because subsequent B-VOP timing is defined by that committed TRD.
  const uint64_t delta = picture.TRD == 0 ? 1U :
                         static_cast<uint64_t>(picture.TRD);
  if ((picture.TRD == 0 && profile != VAProfileMPEG4Simple) ||
      state.newest.tick > std::numeric_limits<uint64_t>::max() - delta)
    return false;
  *tick = state.newest.tick + delta;
  *base = state.newest.tick;
  next->have_previous = true;
  next->previous = state.newest;
  next->newest = {token, *tick, *kind};
  return true;
}

inline bool ValidSlice(const Slice &slice) {
  const auto &parameters = slice.parameters;
  if (slice.data == nullptr ||
      parameters.slice_data_flag != VA_SLICE_DATA_FLAG_ALL ||
      parameters.slice_data_size == 0 ||
      parameters.slice_data_size > kMaxAccessUnitBytes ||
      parameters.slice_data_offset > slice.size ||
      parameters.slice_data_size > slice.size - parameters.slice_data_offset ||
      parameters.macroblock_number != 0 ||
      parameters.quant_scale < 1 || parameters.quant_scale > 31 ||
      parameters.macroblock_offset >= parameters.slice_data_size * 8U ||
      !ZeroReserved(parameters.va_reserved, VA_PADDING_LOW))
    return false;
  return true;
}

inline bool ReadSourceBits(const uint8_t *source, size_t source_bits,
                           size_t *position, unsigned count,
                           uint32_t *value) {
  if (source == nullptr || position == nullptr || value == nullptr ||
      count > 32 || *position > source_bits ||
      count > source_bits - *position)
    return false;
  uint32_t result = 0;
  for (unsigned i = 0; i < count; ++i, ++*position)
    result = (result << 1) |
        ((source[*position / 8] >> (7 - *position % 8)) & 1U);
  *value = result;
  return true;
}

inline bool MarkerAt(const uint8_t *source, size_t source_bits,
                     size_t position, unsigned zero_bits) {
  uint32_t value = 0;
  return ReadSourceBits(source, source_bits, &position, zero_bits, &value) &&
         value == 0 &&
         ReadSourceBits(source, source_bits, &position, 1, &value) &&
         value == 1;
}

// VA's complete MPEG-4 slice retains interior byte-aligned video-packet
// headers.  Reusing an original header_extension_code would repeat its source
// VOP clock inside the newly generated VOP and can contradict seek/replay
// timing.  Accept the measured Simple subset only when every exact packet
// marker has a bounded, increasing macroblock number, a real quantizer and no
// header extension.  Packet bytes themselves remain immutable.
inline bool ValidSimpleVideoPackets(
    const VAPictureParameterBufferMPEG4 &picture, Kind kind,
    const Slice &slice, bool *have_packets) {
  if (have_packets == nullptr) return false;
  *have_packets = false;
  if (picture.vol_fields.bits.resync_marker_disable) return true;
  if (kind != Kind::I && kind != Kind::P) return false;

  const uint32_t mb_width = (picture.vop_width + 15U) / 16U;
  const uint32_t mb_height = (picture.vop_height + 15U) / 16U;
  const uint32_t mb_count = mb_width * mb_height;
  unsigned mb_bits = 1;
  while ((uint32_t{1} << mb_bits) < mb_count) ++mb_bits;
  const unsigned zero_bits =
      kind == Kind::I ? 16U : 15U + picture.vop_fcode_forward;
  const uint8_t *source = slice.data + slice.parameters.slice_data_offset;
  const size_t source_bits =
      static_cast<size_t>(slice.parameters.slice_data_size) * 8;
  size_t position =
      (static_cast<size_t>(slice.parameters.macroblock_offset) / 8 + 1) * 8;
  uint32_t previous_mb = 0;

  while (position <= source_bits &&
         zero_bits + 1U <= source_bits - position) {
    if (MarkerAt(source, source_bits, position, zero_bits)) {
      size_t header = position + zero_bits + 1U;
      uint32_t mb = 0, quant = 0, hec = 0;
      if (!ReadSourceBits(source, source_bits, &header, mb_bits, &mb) ||
          !ReadSourceBits(source, source_bits, &header,
                          picture.quant_precision, &quant) ||
          !ReadSourceBits(source, source_bits, &header, 1, &hec) ||
          mb == 0 || mb >= mb_count || mb <= previous_mb || quant == 0 ||
          hec != 0)
        return false;
      previous_mb = mb;
      *have_packets = true;
    }
    position += 8;
  }
  return true;
}

inline bool Sequence(Bits *bits, const VAPictureParameterBufferMPEG4 &picture,
                     VAProfile profile) {
  const bool advanced = profile == VAProfileMPEG4AdvancedSimple;

  // VA supplies no source MPEG-4 level.  The tested FFmpeg/BCM70015 Simple and
  // Advanced Simple paths use 0x05/0xf5, including measured FHD streams; this
  // transport indicator is for firmware compatibility, not level conformance.
  if (!bits->Start(0xb0) || !bits->Put(8, advanced ? 0xf5 : 0x05) ||
      !bits->Start(0xb5) || !bits->Put(1, 1) ||
      !bits->Put(4, advanced ? 5 : 1) || !bits->Put(3, 1) ||
      !bits->Put(4, 1) || !bits->Put(1, 0) || !bits->Stuff() ||
      !bits->Start(0x00) || !bits->Start(0x20))
    return false;

  if (!bits->Put(1, 0) ||                         // random_accessible_vol
      !bits->Put(8, advanced ? 0x11 : 0x01) ||   // video_object_type
      !bits->Put(1, 1) ||                         // object_layer_identifier
      !bits->Put(4, advanced ? 5 : 1) || !bits->Put(3, 1) ||
      !bits->Put(4, 1) ||                         // square pixels
      !bits->Put(1, 1) || !bits->Put(2, 1) ||    // vol_control, 4:2:0
      !bits->Put(1, advanced ? 0 : 1) ||          // low_delay
      !bits->Put(1, 0) ||                         // no VBV parameters
      !bits->Put(2, 0) ||                         // rectangular shape
      !bits->Put(1, 1) ||
      !bits->Put(16, picture.vop_time_increment_resolution) ||
      !bits->Put(1, 1) || !bits->Put(1, 0) ||    // variable VOP rate
      !bits->Put(1, 1) || !bits->Put(13, picture.vop_width) ||
      !bits->Put(1, 1) || !bits->Put(13, picture.vop_height) ||
      !bits->Put(1, 1) || !bits->Put(1, 0) ||    // progressive
      !bits->Put(1, 1))                           // OBMC disabled
    return false;
  if (!bits->Put(advanced ? 2 : 1, 0) ||          // no sprite/GMC
      !bits->Put(1, 0) ||                         // 8-bit quant precision
      !bits->Put(1, 0) ||                         // H.263 quantization
      (advanced && !bits->Put(1, 0)) ||           // no quarter sample
      !bits->Put(1, 1) ||                         // complexity disabled
      !bits->Put(1, picture.vol_fields.bits.resync_marker_disable) ||
                                                    // preserve resync policy
      !bits->Put(1, 0) ||                         // no data partitioning
      (advanced && (!bits->Put(1, 0) ||           // no NEWPRED
                    !bits->Put(1, 0))) ||          // no reduced resolution
      !bits->Put(1, 0) || !bits->Stuff())          // no scalability
    return false;
  return true;
}

inline bool Group(Bits *bits, uint64_t clock_origin_seconds) {
  // A GOV gives firmware an explicit clock origin after the repeated VOL.  It
  // names the earlier anchor for an open GOP, where a leading B decoded after
  // this I still uses that anchor as its temporal reference.  Simple Profile
  // cannot contain B pictures and may instead start directly at the I second.
  // The time code wraps at 24 h as specified; the following VOP carries any
  // remaining whole-second delta.
  const uint64_t day_seconds = clock_origin_seconds % (24U * 60U * 60U);
  const unsigned hours = static_cast<unsigned>(day_seconds / 3600U);
  const unsigned minutes = static_cast<unsigned>((day_seconds / 60U) % 60U);
  const unsigned seconds = static_cast<unsigned>(day_seconds % 60U);
  return bits->Start(0xb3) && bits->Put(5, hours) &&
         bits->Put(6, minutes) && bits->Put(1, 1) &&
         bits->Put(6, seconds) && bits->Put(1, 0) && // closed_gov unknown
         bits->Put(1, 0) &&                         // broken_link false
         bits->Stuff();
}

inline bool Vop(Bits *bits, const VAPictureParameterBufferMPEG4 &picture,
                Kind kind, unsigned quant_scale, uint64_t tick, uint64_t base) {
  const uint16_t resolution = picture.vop_time_increment_resolution;
  const uint64_t seconds = tick / resolution;
  const uint64_t base_seconds = base / resolution;
  if (seconds < base_seconds || seconds - base_seconds > 32767 ||
      !bits->Start(0xb6) ||
      !bits->Put(2, kind == Kind::I ? 0 : kind == Kind::P ? 1 : 2))
    return false;
  const uint64_t modulo = seconds - base_seconds;
  for (uint64_t i = 0; i < modulo; ++i)
    if (!bits->Put(1, 1)) return false;
  if (!bits->Put(1, 0) || !bits->Put(1, 1) ||
      !bits->Put(TimeBits(resolution), static_cast<uint32_t>(tick % resolution)) ||
      !bits->Put(1, 1) || !bits->Put(1, 1))
    return false;
  if (kind == Kind::P && !bits->Put(1, picture.vop_fields.bits.vop_rounding_type))
    return false;
  if (!bits->Put(3, picture.vop_fields.bits.intra_dc_vlc_thr) ||
      !bits->Put(5, quant_scale) ||
      ((kind == Kind::P || kind == Kind::B) &&
       !bits->Put(3, picture.vop_fcode_forward)) ||
      (kind == Kind::B && !bits->Put(3, picture.vop_fcode_backward)))
    return false;
  return true;
}

}  // namespace detail

// References are immutable replay tokens resolved by the caller from VA
// surfaces.  Destination/state/tick are committed together only on success.
inline bool Assemble(const VAPictureParameterBufferMPEG4 &picture,
                     VAProfile profile, uint64_t token, uint64_t forward,
                     uint64_t backward, const Slice &slice,
                     const TimingState &state, std::vector<uint8_t> *output,
                     TimingState *next_state, uint64_t *picture_tick = nullptr) {
  if (output == nullptr || next_state == nullptr || !detail::ValidSlice(slice))
    return false;

  Kind kind = Kind::I;
  uint64_t tick = 0, base = 0;
  TimingState next;
  if (!detail::ValidPicture(picture, profile, state, token, forward, backward,
                            &kind, &tick, &base, &next))
    return false;
  bool have_video_packets = false;
  if (!detail::ValidSimpleVideoPackets(picture, kind, slice,
                                       &have_video_packets))
    return false;

  detail::Bits bits;
  // Native Simple streams put the current I time in GOV and encode a zero I
  // modulo.  This avoids a CrystalHD firmware drop when an I lands exactly on
  // a second boundary.  Advanced Simple retains the previous anchor origin so
  // leading open-GOP B pictures remain representable after the I.
  const uint64_t vop_base =
      kind == Kind::I && profile == VAProfileMPEG4Simple ? tick : base;
  if ((kind == Kind::I &&
       (!detail::Sequence(&bits, picture, profile) ||
        !detail::Group(&bits, vop_base /
                                  picture.vop_time_increment_resolution))) ||
      !detail::Vop(&bits, picture, kind,
                   static_cast<unsigned>(slice.parameters.quant_scale),
                   tick, vop_base))
    return false;

  if (have_video_packets &&
      bits.PositionMod8() != (slice.parameters.macroblock_offset & 7U))
    return false;

  const uint8_t *data = slice.data + slice.parameters.slice_data_offset;
  const size_t source_bits = static_cast<size_t>(slice.parameters.slice_data_size) * 8;
  if (!bits.Copy(data, slice.parameters.macroblock_offset, source_bits))
    return false;

  std::vector<uint8_t> assembled = bits.Take();
  if (assembled.empty() || assembled.size() > kMaxAccessUnitBytes) return false;
  *output = std::move(assembled);
  *next_state = next;
  if (picture_tick != nullptr) *picture_tick = tick;
  return true;
}

}  // namespace crystalhd_mpeg4
#endif
