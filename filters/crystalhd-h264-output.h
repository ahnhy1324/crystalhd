/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CRYSTALHD_H264_OUTPUT_H
#define CRYSTALHD_H264_OUTPUT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <bc_dts_types.h>
#include <bc_dts_defs.h>

typedef enum {
  CRYSTALHD_H264_PAYLOAD_FIELD,
  CRYSTALHD_H264_PAYLOAD_WEAVE,
  CRYSTALHD_H264_PAYLOAD_INVALID
} CrystalHDH264Payload;

/* Private frontend helper for BCM70015 H.264 interlaced output. libcrystalhd
 * can report TOPFIELD for a complete woven picture. Establish that layout
 * from completed data and its embedded PIB; BCM70012 keeps its legacy path. */
static inline CrystalHDH264Payload
crystalhd_h264_interlaced_payload(const BC_DTS_PROC_OUT *output)
{
  uint32_t width, height, trailer_width = 0, line = 0, ordinal = 0;
  uint64_t done, pitch, full_bytes, ordinal_offset, pib_offset;
  const uint64_t max_done = (uint64_t)1920U * 2U * (1088U + 2U) + 4096U;
  const size_t common_bytes = offsetof(BC_PIC_INFO_BLOCK, other);
  const uint32_t flea_decode_error = 0x800U; /* FleaDefs.h firmware flag. */
  int whole, half, timestamp_matches;
  BC_PIC_INFO_BLOCK raw;

  if (output == NULL)
    return CRYSTALHD_H264_PAYLOAD_INVALID;
  width = output->PicInfo.width;
  height = output->PicInfo.height;
  done = (uint64_t)output->YBuffDoneSz * 4U;
  pitch = (uint64_t)width * 2U;
  full_bytes = pitch * height;
  if (output->Ybuff == NULL || width == 0 || width > 1920U ||
      height == 0 || height > 1088U || (width & 1U) != 0 ||
      (height & 3U) != 0 ||
      (output->PoutFlags & BC_POUT_FLAGS_PIB_VALID) == 0 ||
      (output->PoutFlags & BC_POUT_FLAGS_ENCRYPTED) != 0)
    return CRYSTALHD_H264_PAYLOAD_INVALID;
  if (done < full_bytes)
    return CRYSTALHD_H264_PAYLOAD_FIELD;

  /* The kernel bounds completion by its registered plane. Also reject
   * impossible dimensions/counts before constructing frontend pointers. */
  if (done < 8U || done > max_done || done > SIZE_MAX ||
      output->b422Mode != OUTPUT_MODE422_YUY2)
    return CRYSTALHD_H264_PAYLOAD_INVALID;
  memcpy(&trailer_width, output->Ybuff + (size_t)(done - 8U), 4);
  memcpy(&line, output->Ybuff + (size_t)(done - 4U), 4);
  if (trailer_width != width)
    return CRYSTALHD_H264_PAYLOAD_INVALID;
  whole = line == height || line == height + 1U;
  half = line == height / 2U || line == height / 2U + 1U;
  if (!whole && !half)
    return CRYSTALHD_H264_PAYLOAD_INVALID;
  ordinal_offset = pitch * line;
  pib_offset = ordinal_offset + sizeof(uint32_t);
  if (pib_offset > done - 8U || common_bytes > done - 8U - pib_offset)
    return CRYSTALHD_H264_PAYLOAD_INVALID;
  memcpy(&ordinal, output->Ybuff + (size_t)ordinal_offset, 4);
  memset(&raw, 0, sizeof(raw));
  memcpy(&raw, output->Ybuff + (size_t)pib_offset, common_bytes);
  /* Flea's ordinary shifted-millisecond timestamp becomes 100 ns units in
   * libcrystalhd. The preload sentinel and multiplication overflow are invalid. */
  timestamp_matches = raw.timeStamp != UINT64_C(0xffffffff) &&
      raw.timeStamp <= UINT64_MAX / 20000U &&
      raw.timeStamp * 20000U == output->PicInfo.timeStamp;
  if ((ordinal & 0x80000000U) != 0 ||
      (ordinal & 0x3fffffffU) != output->PicInfo.picture_number ||
      !timestamp_matches ||
      raw.picture_number != output->PicInfo.picture_number ||
      raw.width != width || raw.height != height ||
      raw.sess_num != output->PicInfo.sess_num ||
      raw.picture_meta_payload != output->PicInfo.picture_meta_payload ||
      (raw.flags & (VDEC_FLAG_EOS | flea_decode_error)) != 0)
    return CRYSTALHD_H264_PAYLOAD_INVALID;
  if (half)
    return CRYSTALHD_H264_PAYLOAD_FIELD;
  if (full_bytes > ordinal_offset || (ordinal & 0x40000000U) != 0 ||
      (raw.flags & VDEC_FLAG_BOTTOMFIELD) != VDEC_FLAG_FIELDPAIR)
    return CRYSTALHD_H264_PAYLOAD_INVALID;
  return CRYSTALHD_H264_PAYLOAD_WEAVE;
}

#endif
