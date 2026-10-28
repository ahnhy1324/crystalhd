/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CRYSTALHD_MPEG2_INSPECT_H
#define CRYSTALHD_MPEG2_INSPECT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  unsigned int picture_type;
  unsigned int picture_count;
  unsigned int picture_structure;
  int have_sequence;
  unsigned int height;
  int have_sequence_extension;
  unsigned int profile;
  int progressive_sequence;
  int have_minimum_picture_header;
  unsigned int slice_prediction;
  uint64_t slice_rows[3];
} CrystalHdMpeg2Picture;

static inline size_t
crystalhd_mpeg2_next_start_code(const uint8_t *data, size_t size, size_t from)
{
  size_t i;
  for (i = from; size >= 4 && i <= size - 4; i++)
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
      return i;
  return size;
}

/* H.262 6.2.4 and Tables B.1/B.3/B.4: positive proof from only the first
 * macroblock of a slice. Recognize quantiser_scale_code != 0,
 * extra_bit_slice == 0 and address_increment == 1; other syntax is unknown.
 * All inspected VLC bits must precede the next start code. Bits 1/2 mean
 * forward/backward prediction. A P non-intra macroblock with no motion
 * vectors still predicts from the previous reference with a zero vector.
 * https://www.itu.int/rec/T-REC-H.262-199507-S/en
 */
static inline unsigned int
crystalhd_mpeg2_first_slice_prediction(const uint8_t *data, size_t size,
                                       unsigned int picture_type)
{
  unsigned int bits;
  if (size < 2 || (data[0] & 0xf8) == 0 ||
      (data[0] & 4) != 0 || (data[0] & 2) == 0)
    return 0;
  bits = ((unsigned int)(data[0] & 1) << 5) | (data[1] >> 3);
  if (picture_type == 2) {
    if ((bits & 32) != 0 || (bits & 48) == 16 || (bits & 56) == 8 ||
        (bits & 62) == 4 || (bits & 62) == 2)
      return 1;
  } else if (picture_type == 3) {
    if ((bits & 32) != 0 || (bits & 62) == 4)
      return 3;
    if ((bits & 48) == 16 || bits == 2)
      return 2;
    if ((bits & 56) == 8 || bits == 3)
      return 1;
  }
  return 0;
}

/* Inspect byte-aligned headers and bounded first-macroblock prefixes, not
 * decoded output or complete macroblock coverage. A parsed field pair may
 * carry two picture headers and is excluded from frame-only admission.
 */
static inline void
crystalhd_mpeg2_picture_headers(const uint8_t *data, size_t size,
                                CrystalHdMpeg2Picture *picture)
{
  size_t i;

  memset(picture, 0, sizeof(*picture));
  i = crystalhd_mpeg2_next_start_code(data, size, 0);
  while (i < size) {
    const size_t next = crystalhd_mpeg2_next_start_code(data, size, i + 4);
    const size_t bytes = next - i;
    const unsigned int code = data[i + 3];
    if (code == 0 && bytes >= 6) {
      picture->picture_count++;
      if (picture->picture_count == 1) {
        picture->picture_type = (data[i + 5] >> 3) & 7;
        picture->have_minimum_picture_header = picture->picture_type == 1
            ? bytes >= 8 : ((picture->picture_type == 2 ||
                             picture->picture_type == 3) && bytes >= 9);
      }
    } else if (code == 0xb3 && bytes >= 8) {
      picture->have_sequence = 1;
      picture->height = ((unsigned int)(data[i + 5] & 15) << 8) | data[i + 6];
    } else if (code == 0xb5 && bytes >= 5) {
      const unsigned int extension = data[i + 4] >> 4;
      if (extension == 1 && bytes >= 10) {
        picture->have_sequence_extension = 1;
        picture->profile = (data[i + 4] & 8) ? 0 : data[i + 4] & 7;
        picture->progressive_sequence = (data[i + 5] & 8) != 0;
        picture->height |= ((unsigned int)(data[i + 6] >> 5) & 3) << 12;
      } else if (extension == 8 && bytes >= 9 && picture->picture_count == 1) {
        picture->picture_structure = data[i + 6] & 3;
      }
    } else if (code >= 1 && code <= 0xaf && picture->picture_count == 1) {
      const unsigned int row = code - 1;
      picture->slice_rows[row / 64] |= ((uint64_t)1) << (row % 64);
      picture->slice_prediction |= crystalhd_mpeg2_first_slice_prediction(
          data + i + 4, bytes - 4, picture->picture_type);
    }
    i = next;
  }
}

#endif
