/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_CRYSTALHD_MPEG1_INPUT_H
#define GST_CRYSTALHD_MPEG1_INPUT_H

#include <gst/gst.h>

/* Structural admission for the explicitly selected 352x288/25 MPEG-1
 * experiment. This is not an entropy decoder, decoded-pixel oracle, or
 * reference-picture availability proof. It never changes the input bytes. */
#define GST_CRYSTALHD_MPEG1_MAX_BYTES ((gsize)1024 * 1024)

typedef struct {
  const guint8 *data;
  gsize size, byte;
  guint bit;
} GstCrystalHdMpeg1Bits;

static gboolean
gst_crystalhd_mpeg1_reject(const gchar **reason, const gchar *message)
{
  if (reason != NULL)
    *reason = message;
  return FALSE;
}

static gboolean
gst_crystalhd_mpeg1_read(GstCrystalHdMpeg1Bits *bits, guint count,
                          guint *value)
{
  guint result = 0;
  if (count > 32)
    return FALSE;
  for (guint i = 0; i < count; i++) {
    if (bits->byte >= bits->size)
      return FALSE;
    result = (result << 1) |
        ((bits->data[bits->byte] >> (7 - bits->bit)) & 1U);
    if (++bits->bit == 8) {
      bits->bit = 0;
      bits->byte++;
    }
  }
  *value = result;
  return TRUE;
}

static gboolean
gst_crystalhd_mpeg1_zero_tail(GstCrystalHdMpeg1Bits bits)
{
  guint value;
  while (bits.byte < bits.size) {
    if (!gst_crystalhd_mpeg1_read(&bits, 1, &value) || value != 0)
      return FALSE;
  }
  return TRUE;
}

static gboolean
gst_crystalhd_mpeg1_extra(GstCrystalHdMpeg1Bits *bits)
{
  guint extra, information;
  do {
    if (!gst_crystalhd_mpeg1_read(bits, 1, &extra))
      return FALSE;
    if (extra && !gst_crystalhd_mpeg1_read(bits, 8, &information))
      return FALSE;
  } while (extra);
  return TRUE;
}

static gboolean
gst_crystalhd_mpeg1_sequence(const guint8 *data, gsize size,
                             const gchar **reason)
{
  GstCrystalHdMpeg1Bits bits = {data, size, 0, 0};
  guint width, height, aspect, rate, bitrate, marker, vbv, constrained, load;
  if (!gst_crystalhd_mpeg1_read(&bits, 12, &width) ||
      !gst_crystalhd_mpeg1_read(&bits, 12, &height) ||
      !gst_crystalhd_mpeg1_read(&bits, 4, &aspect) ||
      !gst_crystalhd_mpeg1_read(&bits, 4, &rate) ||
      !gst_crystalhd_mpeg1_read(&bits, 18, &bitrate) ||
      !gst_crystalhd_mpeg1_read(&bits, 1, &marker) ||
      !gst_crystalhd_mpeg1_read(&bits, 10, &vbv) ||
      !gst_crystalhd_mpeg1_read(&bits, 1, &constrained))
    return gst_crystalhd_mpeg1_reject(reason, "truncated MPEG-1 sequence header");
  if (width != 352 || height != 288 || rate != 3)
    return gst_crystalhd_mpeg1_reject(reason,
        "MPEG-1 sequence must be 352x288 with frame_rate_code 3 (25 fps)");
  if (aspect == 0 || aspect == 15 || bitrate == 0 || marker != 1 || vbv == 0)
    return gst_crystalhd_mpeg1_reject(reason,
        "invalid MPEG-1 sequence aspect, bitrate, marker, or VBV size");
  (void)constrained;
  for (guint matrix = 0; matrix < 2; matrix++) {
    if (!gst_crystalhd_mpeg1_read(&bits, 1, &load))
      return gst_crystalhd_mpeg1_reject(reason, "truncated quantizer-matrix flag");
    if (load) {
      for (guint coefficient = 0; coefficient < 64; coefficient++) {
        guint value;
        if (!gst_crystalhd_mpeg1_read(&bits, 8, &value) || value == 0)
          return gst_crystalhd_mpeg1_reject(reason,
              "truncated or zero MPEG-1 quantizer-matrix coefficient");
      }
    }
  }
  if (!gst_crystalhd_mpeg1_zero_tail(bits))
    return gst_crystalhd_mpeg1_reject(reason,
        "unexpected data after MPEG-1 sequence header");
  return TRUE;
}

static gboolean
gst_crystalhd_mpeg1_gop(const guint8 *data, gsize size,
                        const gchar **reason)
{
  GstCrystalHdMpeg1Bits bits = {data, size, 0, 0};
  guint drop, hours, minutes, marker, seconds, pictures, closed, broken;
  if (!gst_crystalhd_mpeg1_read(&bits, 1, &drop) ||
      !gst_crystalhd_mpeg1_read(&bits, 5, &hours) ||
      !gst_crystalhd_mpeg1_read(&bits, 6, &minutes) ||
      !gst_crystalhd_mpeg1_read(&bits, 1, &marker) ||
      !gst_crystalhd_mpeg1_read(&bits, 6, &seconds) ||
      !gst_crystalhd_mpeg1_read(&bits, 6, &pictures) ||
      !gst_crystalhd_mpeg1_read(&bits, 1, &closed) ||
      !gst_crystalhd_mpeg1_read(&bits, 1, &broken))
    return gst_crystalhd_mpeg1_reject(reason, "truncated MPEG-1 GOP header");
  (void)drop; (void)closed; (void)broken;
  if (hours > 23 || minutes > 59 || seconds > 59 ||
      pictures > 24 || marker != 1 || !gst_crystalhd_mpeg1_zero_tail(bits))
    return gst_crystalhd_mpeg1_reject(reason, "invalid MPEG-1 GOP time code");
  return TRUE;
}

static gboolean
gst_crystalhd_mpeg1_picture(const guint8 *data, gsize size,
                            const gchar **reason)
{
  GstCrystalHdMpeg1Bits bits = {data, size, 0, 0};
  guint temporal_reference, type, vbv_delay, full_pel, f_code;
  if (!gst_crystalhd_mpeg1_read(&bits, 10, &temporal_reference) ||
      !gst_crystalhd_mpeg1_read(&bits, 3, &type) ||
      !gst_crystalhd_mpeg1_read(&bits, 16, &vbv_delay))
    return gst_crystalhd_mpeg1_reject(reason, "truncated MPEG-1 picture header");
  (void)temporal_reference; (void)vbv_delay;
  if (type < 1 || type > 3)
    return gst_crystalhd_mpeg1_reject(reason,
        "MPEG-1 experiment requires an I, P, or B picture");
  if (type == 2 || type == 3) {
    if (!gst_crystalhd_mpeg1_read(&bits, 1, &full_pel) ||
        !gst_crystalhd_mpeg1_read(&bits, 3, &f_code) || f_code == 0)
      return gst_crystalhd_mpeg1_reject(reason,
          "truncated or zero MPEG-1 forward f_code");
  }
  if (type == 3) {
    if (!gst_crystalhd_mpeg1_read(&bits, 1, &full_pel) ||
        !gst_crystalhd_mpeg1_read(&bits, 3, &f_code) || f_code == 0)
      return gst_crystalhd_mpeg1_reject(reason,
          "truncated or zero MPEG-1 backward f_code");
  }
  if (!gst_crystalhd_mpeg1_extra(&bits) ||
      !gst_crystalhd_mpeg1_zero_tail(bits))
    return gst_crystalhd_mpeg1_reject(reason,
        "truncated extra_information_picture or invalid header padding");
  return TRUE;
}

static gboolean
gst_crystalhd_mpeg1_slice(const guint8 *data, gsize size, guint code,
                          const gchar **reason)
{
  GstCrystalHdMpeg1Bits bits = {data, size, 0, 0};
  guint quantizer_scale;
  /* MPEG-1 slice_vertical_position is the start-code value itself. */
  if (code == 0 || code > 18)
    return gst_crystalhd_mpeg1_reject(reason,
        "MPEG-1 slice row lies outside the qualified 288-line frame");
  if (!gst_crystalhd_mpeg1_read(&bits, 5, &quantizer_scale) ||
      quantizer_scale == 0 || !gst_crystalhd_mpeg1_extra(&bits))
    return gst_crystalhd_mpeg1_reject(reason,
        "truncated slice header, zero quantizer scale, or incomplete extra info");
  /* At least one nonzero entropy bit is necessary, but not sufficient:
   * macroblock syntax, completeness, reference use and pixels are not
   * established by this bounded structural admission check. */
  if (gst_crystalhd_mpeg1_zero_tail(bits))
    return gst_crystalhd_mpeg1_reject(reason,
        "MPEG-1 slice has no nonzero macroblock payload");
  return TRUE;
}

static gboolean
gst_crystalhd_mpeg1_start_code(const guint8 *data, gsize size,
                               gsize from, gsize *position)
{
  for (gsize i = from; i < size && size - i >= 4; i++) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      *position = i;
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
gst_crystalhd_mpeg1_input_bound(const guint8 *data, gsize size,
                                gsize *first, const gchar **reason)
{
  if (reason != NULL)
    *reason = NULL;
  if (data == NULL || size < 4 || size > GST_CRYSTALHD_MPEG1_MAX_BYTES)
    return gst_crystalhd_mpeg1_reject(reason,
        "missing MPEG-1 bytes or input exceeds the 1 MiB bound");
  if (size >= 3 && data[size - 3] == 0 &&
      data[size - 2] == 0 && data[size - 1] == 1)
    return gst_crystalhd_mpeg1_reject(reason, "truncated MPEG start code");
  if (!gst_crystalhd_mpeg1_start_code(data, size, 0, first))
    return gst_crystalhd_mpeg1_reject(reason, "MPEG-1 input has no start code");
  for (gsize i = 0; i < *first; i++) {
    if (data[i] != 0)
      return gst_crystalhd_mpeg1_reject(reason,
          "unexpected bytes before MPEG-1 start code");
  }
  return TRUE;
}

static gboolean
gst_crystalhd_mpeg1_validate(const guint8 *data, gsize size,
                             gboolean metadata, const gchar **reason)
{
  gsize position;
  guint sequences = 0, pictures = 0, slices = 0;
  gboolean ended = FALSE;
  if (!gst_crystalhd_mpeg1_input_bound(data, size, &position, reason))
    return FALSE;

  do {
    gsize next;
    gboolean have_next = gst_crystalhd_mpeg1_start_code(
        data, size, position + 4, &next);
    gsize end = have_next ? next : size;
    const guint8 *payload = data + position + 4;
    gsize payload_size = end - position - 4;
    guint code = data[position + 3];
    if (ended)
      return gst_crystalhd_mpeg1_reject(reason,
          "another MPEG start code follows sequence_end_code");
    if (code == 0xb5)
      return gst_crystalhd_mpeg1_reject(reason,
          "MPEG-2 extension is forbidden in the MPEG-1 experiment");
    if (code == 0xb3) {
      if (!gst_crystalhd_mpeg1_sequence(payload, payload_size, reason))
        return FALSE;
      sequences++;
    } else if (code == 0xb8) {
      if (!gst_crystalhd_mpeg1_gop(payload, payload_size, reason))
        return FALSE;
    } else if (code == 0xb2) {
      /* MPEG-1 user_data is opaque until the next start-code prefix. */
    } else if (code == 0xb7) {
      GstCrystalHdMpeg1Bits tail = {payload, payload_size, 0, 0};
      if (!gst_crystalhd_mpeg1_zero_tail(tail))
        return gst_crystalhd_mpeg1_reject(reason,
            "unexpected bytes after MPEG-1 sequence_end_code");
      if (!metadata && (pictures != 1 || slices == 0))
        return gst_crystalhd_mpeg1_reject(reason,
            "sequence_end_code precedes the access unit picture/slices");
      ended = TRUE;
    } else if (code == 0x00) {
      if (metadata)
        return gst_crystalhd_mpeg1_reject(reason,
            "codec_data must not contain a coded picture");
      if (++pictures != 1)
        return gst_crystalhd_mpeg1_reject(reason,
            "an MPEG-1 access unit must contain exactly one picture header");
      if (!gst_crystalhd_mpeg1_picture(payload, payload_size, reason))
        return FALSE;
    } else if (code >= 0x01 && code <= 0xaf) {
      if (metadata)
        return gst_crystalhd_mpeg1_reject(reason,
            "codec_data must not contain a coded slice");
      if (pictures != 1)
        return gst_crystalhd_mpeg1_reject(reason,
            "MPEG-1 slice precedes its complete picture header");
      if (!gst_crystalhd_mpeg1_slice(payload, payload_size, code, reason))
        return FALSE;
      slices++;
    } else {
      return gst_crystalhd_mpeg1_reject(reason,
          "unsupported or reserved MPEG start code");
    }
    if (!have_next)
      break;
    position = next;
  } while (TRUE);

  if (metadata && sequences == 0)
    return gst_crystalhd_mpeg1_reject(reason,
        "codec_data requires an actual MPEG-1 sequence header");
  if (!metadata && (pictures != 1 || slices == 0))
    return gst_crystalhd_mpeg1_reject(reason,
        "MPEG-1 access unit requires one complete picture and coded slices");
  return TRUE;
}

static gboolean
gst_crystalhd_mpeg1_sequence_header_valid(const guint8 *data, gsize size,
                                          const gchar **reason)
{
  return gst_crystalhd_mpeg1_validate(data, size, TRUE, reason);
}

static gboolean
gst_crystalhd_mpeg1_access_unit_valid(const guint8 *data, gsize size,
                                      const gchar **reason)
{
  return gst_crystalhd_mpeg1_validate(data, size, FALSE, reason);
}

#endif
