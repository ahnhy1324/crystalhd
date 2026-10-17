/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_CRYSTALHD_CODECS_H
#define GST_CRYSTALHD_CODECS_H

#include <stdint.h>
#include <string.h>
#include <gst/gst.h>
#include <bc_dts_defs.h>

#define GST_CRYSTALHD_VC1_MAX_PICTURE_SIZE (16U * 1024U * 1024U)
#define GST_CRYSTALHD_MPEG4_MAX_INPUT_SIZE ((gsize)1024 * 1024)

typedef enum {
  CRYSTALHD_MPEG4_OBJECT_TYPE_NONE = 0,
  CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE = 1,
  CRYSTALHD_MPEG4_OBJECT_TYPE_ADVANCED_SIMPLE = 17
} CrystalHdMpeg4ObjectType;

typedef enum {
  CRYSTALHD_PAYLOAD_INVALID = 0,
  CRYSTALHD_PAYLOAD_CODED,
  CRYSTALHD_PAYLOAD_NOT_CODED
} CrystalHdPayloadStatus;

typedef struct {
  BC_MEDIA_SUBTYPE subtype;
  gboolean vc1_bdu;
  gboolean frame_layer;
  CrystalHdMpeg4ObjectType mpeg4_object_type;
  guint8 mpeg4_profile_level;
  guint mpeg4_width;
  guint mpeg4_height;
  guint mpeg4_time_increment_resolution;
  guint mpeg4_time_increment_bits;
  guint mpeg2_profile;
  guint mpeg2_height;
  gboolean mpeg2_progressive_sequence;
} CrystalHdCodec;

typedef struct {
  guint picture_type;
  guint picture_count;
  guint picture_structure;
  gboolean have_sequence;
  guint height;
  gboolean have_sequence_extension;
  guint profile;
  gboolean progressive_sequence;
  gboolean have_minimum_picture_header;
  guint slice_prediction;
  guint64 slice_rows[3];
} CrystalHdMpeg2Picture;

static gsize
gst_crystalhd_mpeg2_next_start_code(const guint8 *data, gsize size, gsize from)
{
  gsize i;
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
static guint
gst_crystalhd_mpeg2_first_slice_prediction(const guint8 *data, gsize size,
                                          guint picture_type)
{
  guint bits;
  if (size < 2 || (data[0] & 0xf8) == 0 ||
      (data[0] & 4) != 0 || (data[0] & 2) == 0)
    return 0;
  bits = ((guint)(data[0] & 1) << 5) | (data[1] >> 3);
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
static void
gst_crystalhd_mpeg2_picture_headers(const guint8 *data, gsize size,
                                    CrystalHdMpeg2Picture *picture)
{
  gsize i;

  memset(picture, 0, sizeof(*picture));
  i = gst_crystalhd_mpeg2_next_start_code(data, size, 0);
  while (i < size) {
    const gsize next = gst_crystalhd_mpeg2_next_start_code(data, size, i + 4);
    const gsize bytes = next - i;
    const guint code = data[i + 3];
    if (code == 0 && bytes >= 6) {
      picture->picture_count++;
      if (picture->picture_count == 1) {
        picture->picture_type = (data[i + 5] >> 3) & 7;
        picture->have_minimum_picture_header = picture->picture_type == 1
            ? bytes >= 8 : ((picture->picture_type == 2 || picture->picture_type == 3) && bytes >= 9);
      }
    } else if (code == 0xb3 && bytes >= 8) {
      picture->have_sequence = TRUE;
      picture->height = ((guint)(data[i + 5] & 15) << 8) | data[i + 6];
    } else if (code == 0xb5 && bytes >= 5) {
      const guint extension = data[i + 4] >> 4;
      if (extension == 1 && bytes >= 10) {
        picture->have_sequence_extension = TRUE;
        picture->profile = (data[i + 4] & 8) ? 0 : data[i + 4] & 7;
        picture->progressive_sequence = (data[i + 5] & 8) != 0;
        picture->height |= ((guint)(data[i + 6] >> 5) & 3) << 12;
      } else if (extension == 8 && bytes >= 9 && picture->picture_count == 1) {
        picture->picture_structure = data[i + 6] & 3;
      }
    } else if (code >= 1 && code <= 0xaf && picture->picture_count == 1) {
      const guint row = code - 1;
      picture->slice_rows[row / 64] |= G_GUINT64_CONSTANT(1) << (row % 64);
      picture->slice_prediction |= gst_crystalhd_mpeg2_first_slice_prediction(
          data + i + 4, bytes - 4, picture->picture_type);
    }
    i = next;
  }
}

static gboolean
gst_crystalhd_mpeg2_known_frame(const CrystalHdCodec *codec,
                               const CrystalHdMpeg2Picture *picture)
{
  return (codec->mpeg2_profile == 4 || codec->mpeg2_profile == 5) &&
      codec->mpeg2_height != 0 && codec->mpeg2_height <= 1088 &&
      picture->picture_count == 1 && picture->picture_structure == 3 &&
      picture->have_minimum_picture_header;
}

/* H.262 (1995), 6.1.2/6.1.2.2 and Table 8-5 require same-row slices and
 * complete coverage for Main/Simple profiles. Multiple slices per row are
 * legal. Missing a whole row proves damage; present rows do not prove that
 * their macroblocks or entropy data are complete. Preserve field pictures,
 * unknown profiles and incomplete headers for the established decoder path.
 * https://www.itu.int/rec/T-REC-H.262-199507-S/en
 */
static guint
gst_crystalhd_mpeg2_missing_rows(const CrystalHdCodec *codec,
                                  const CrystalHdMpeg2Picture *picture)
{
  guint rows, row, missing = 0;
  if (!gst_crystalhd_mpeg2_known_frame(codec, picture))
    return 0;
  rows = codec->mpeg2_progressive_sequence
      ? (codec->mpeg2_height + 15) / 16 : 2 * ((codec->mpeg2_height + 31) / 32);
  for (row = 0; row < rows; row++)
    if ((picture->slice_rows[row / 64] &
         (G_GUINT64_CONSTANT(1) << (row % 64))) == 0)
      missing++;
  return missing;
}

typedef struct {
  const guint8 *data;
  gsize size;
  gsize bit;
} CrystalHdMpeg4Bits;

static gboolean
gst_crystalhd_mpeg4_read_bits(CrystalHdMpeg4Bits *bits, guint count,
                              guint *value)
{
  guint result = 0;
  guint index;

  if (count > 32 || bits->size > G_MAXSIZE / 8 ||
      bits->bit > bits->size * 8 || count > bits->size * 8 - bits->bit)
    return FALSE;
  for (index = 0; index < count; index++, bits->bit++)
    result = (result << 1) |
        ((bits->data[bits->bit / 8] >> (7 - bits->bit % 8)) & 1);
  *value = result;
  return TRUE;
}

static gboolean
gst_crystalhd_mpeg4_skip_bits(CrystalHdMpeg4Bits *bits, guint count)
{
  guint ignored;
  return gst_crystalhd_mpeg4_read_bits(bits, count, &ignored);
}

static gboolean
gst_crystalhd_mpeg4_marker(CrystalHdMpeg4Bits *bits)
{
  guint marker;
  return gst_crystalhd_mpeg4_read_bits(bits, 1, &marker) && marker == 1;
}

/* Accept only the progressive 8-bit, rectangular Simple/Advanced Simple
 * VOL tools exercised on BCM70015. GStreamer's mpeg4videoparse does not put
 * every VOL tool in caps, so codec_data must be authoritative here.
 */
static gboolean
gst_crystalhd_mpeg4_vol_supported(const guint8 *data, gsize size,
                                  CrystalHdMpeg4ObjectType object_type,
                                  guint *width, guint *height,
                                  guint *time_increment_resolution,
                                  guint *time_increment_bits)
{
  CrystalHdMpeg4Bits bits = { data, size, 0 };
  guint value;
  guint verid = 1;
  guint time_resolution;
  guint time_bits = 1;
  guint remaining;
  guint parsed_width;
  guint parsed_height;

  if ((object_type != CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE &&
       object_type != CRYSTALHD_MPEG4_OBJECT_TYPE_ADVANCED_SIMPLE) ||
      !gst_crystalhd_mpeg4_skip_bits(&bits, 1) ||
      !gst_crystalhd_mpeg4_read_bits(&bits, 8, &value) ||
      value != (guint)object_type ||
      !gst_crystalhd_mpeg4_read_bits(&bits, 1, &value))
    return FALSE;
  if (value != 0) {
    if (!gst_crystalhd_mpeg4_read_bits(&bits, 4, &verid) ||
        (verid != 1 && verid != 5) ||
        !gst_crystalhd_mpeg4_skip_bits(&bits, 3))
      return FALSE;
  }

  if (!gst_crystalhd_mpeg4_read_bits(&bits, 4, &value) || value == 0 ||
      (value > 5 && value != 15))
    return FALSE;
  if (value == 15) {
    guint width, height;
    if (!gst_crystalhd_mpeg4_read_bits(&bits, 8, &width) || width == 0 ||
        !gst_crystalhd_mpeg4_read_bits(&bits, 8, &height) || height == 0)
      return FALSE;
  }

  if (!gst_crystalhd_mpeg4_read_bits(&bits, 1, &value))
    return FALSE;
  if (value != 0) {
    guint chroma;
    guint vbv;
    if (!gst_crystalhd_mpeg4_read_bits(&bits, 2, &chroma) || chroma != 1 ||
        !gst_crystalhd_mpeg4_skip_bits(&bits, 1) ||
        !gst_crystalhd_mpeg4_read_bits(&bits, 1, &vbv))
      return FALSE;
    if (vbv != 0 &&
        (!gst_crystalhd_mpeg4_skip_bits(&bits, 15) ||
         !gst_crystalhd_mpeg4_marker(&bits) ||
         !gst_crystalhd_mpeg4_skip_bits(&bits, 15) ||
         !gst_crystalhd_mpeg4_marker(&bits) ||
         !gst_crystalhd_mpeg4_skip_bits(&bits, 15) ||
         !gst_crystalhd_mpeg4_marker(&bits) ||
         !gst_crystalhd_mpeg4_skip_bits(&bits, 3 + 11) ||
         !gst_crystalhd_mpeg4_marker(&bits) ||
         !gst_crystalhd_mpeg4_skip_bits(&bits, 15) ||
         !gst_crystalhd_mpeg4_marker(&bits)))
      return FALSE;
  }

  /* Only rectangular video has the tested syntax and hardware output. */
  if (!gst_crystalhd_mpeg4_read_bits(&bits, 2, &value) || value != 0 ||
      !gst_crystalhd_mpeg4_marker(&bits) ||
      !gst_crystalhd_mpeg4_read_bits(&bits, 16, &time_resolution) ||
      time_resolution == 0 || !gst_crystalhd_mpeg4_marker(&bits) ||
      !gst_crystalhd_mpeg4_read_bits(&bits, 1, &value))
    return FALSE;
  for (remaining = time_resolution - 1; remaining > 1; remaining >>= 1)
    time_bits++;
  if (value != 0 && !gst_crystalhd_mpeg4_skip_bits(&bits, time_bits))
    return FALSE;

  /* width/height and their marker bits */
  if (!gst_crystalhd_mpeg4_marker(&bits) ||
      !gst_crystalhd_mpeg4_read_bits(&bits, 13, &parsed_width) ||
      parsed_width == 0 || parsed_width > 1920 ||
      !gst_crystalhd_mpeg4_marker(&bits) ||
      !gst_crystalhd_mpeg4_read_bits(&bits, 13, &parsed_height) ||
      parsed_height == 0 || parsed_height > 1088 ||
      !gst_crystalhd_mpeg4_marker(&bits))
    return FALSE;

  /* interlaced=0, obmc_disable=1, sprite_enable=0 */
  if (!gst_crystalhd_mpeg4_read_bits(&bits, 1, &value) || value != 0 ||
      !gst_crystalhd_mpeg4_read_bits(&bits, 1, &value) || value != 1 ||
      !gst_crystalhd_mpeg4_read_bits(&bits, verid == 1 ? 1 : 2, &value) ||
      value != 0)
    return FALSE;

  /* not_8_bit=0 and quant_type=0 (H.263 quantization). */
  if (!gst_crystalhd_mpeg4_read_bits(&bits, 1, &value) || value != 0 ||
      !gst_crystalhd_mpeg4_read_bits(&bits, 1, &value) || value != 0)
    return FALSE;
  if (verid != 1 &&
      (!gst_crystalhd_mpeg4_read_bits(&bits, 1, &value) || value != 0))
    return FALSE; /* quarter_sample */

  /* Require disabled complexity estimation and reject partitioned/RVLC
   * input. BCM70015 decodes Simple with either resync-marker setting, but
   * Advanced Simple with enabled markers loses B-VOPs, so require them
   * disabled for that object type.
   */
  if (!gst_crystalhd_mpeg4_read_bits(&bits, 1, &value) || value != 1 ||
      !gst_crystalhd_mpeg4_read_bits(&bits, 1, &value) ||
      (object_type == CRYSTALHD_MPEG4_OBJECT_TYPE_ADVANCED_SIMPLE &&
       value != 1) ||
      !gst_crystalhd_mpeg4_read_bits(&bits, 1, &value) || value != 0)
    return FALSE;
  if (verid != 1) {
    if (!gst_crystalhd_mpeg4_read_bits(&bits, 1, &value) || value != 0 ||
        !gst_crystalhd_mpeg4_read_bits(&bits, 1, &value) || value != 0)
      return FALSE; /* newpred and reduced_resolution_vop */
  }
  if (!gst_crystalhd_mpeg4_read_bits(&bits, 1, &value) || value != 0)
    return FALSE;
  if (width != NULL)
    *width = parsed_width;
  if (height != NULL)
    *height = parsed_height;
  if (time_increment_resolution != NULL)
    *time_increment_resolution = time_resolution;
  if (time_increment_bits != NULL)
    *time_increment_bits = time_bits;
  return TRUE;
}

static gboolean
gst_crystalhd_mpeg4_start_codes(const guint8 *data, gsize size,
                                guint *vos_count, guint8 *profile_level,
                                guint *vol_count, guint *vop_count,
                                const guint8 **vol, gsize *vol_size,
                                const guint8 **vop, gsize *vop_size)
{
  gsize i;
  gsize first_vol = 0;
  gsize first_vol_end = 0;
  gsize first_vop = 0;
  gsize first_vop_end = 0;
  gsize required_data = 0;

  if (data == NULL || size < 4 || size > GST_CRYSTALHD_MPEG4_MAX_INPUT_SIZE ||
      data[0] != 0 || data[1] != 0 ||
      (data[2] != 1 && (size < 5 || data[2] != 0 || data[3] != 1)))
    return FALSE;
  *vos_count = 0;
  *profile_level = 0;
  *vol_count = 0;
  *vop_count = 0;
  *vol = NULL;
  *vol_size = 0;
  *vop = NULL;
  *vop_size = 0;
  for (i = 0; i <= size - 4; i++) {
    guint8 code;
    if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
      continue;
    if (required_data != 0 && i == required_data)
      return FALSE;
    required_data = 0;
    if (first_vol != 0 && first_vol_end == 0)
      first_vol_end = i;
    if (first_vop != 0 && first_vop_end == 0)
      first_vop_end = i;
    code = data[i + 3];
    if (code == 0xb0) {
      if (i + 4 >= size)
        return FALSE;
      if (*vos_count == 0)
        *profile_level = data[i + 4];
      (*vos_count)++;
      required_data = i + 4;
    } else if ((code & 0xf0) == 0x20) {
      if (*vol_count == 0)
        first_vol = i + 4;
      (*vol_count)++;
      required_data = i + 4;
    } else if (code == 0xb6) {
      if (*vop_count == 0)
        first_vop = i + 4;
      (*vop_count)++;
      required_data = i + 4;
    }
    i += 3;
  }
  if (required_data == size)
    return FALSE;
  if (first_vol != 0) {
    if (first_vol_end == 0)
      first_vol_end = size;
    if (first_vol_end <= first_vol)
      return FALSE;
    *vol = data + first_vol;
    *vol_size = first_vol_end - first_vol;
  }
  if (first_vop != 0) {
    if (first_vop_end == 0)
      first_vop_end = size;
    if (first_vop_end <= first_vop)
      return FALSE;
    *vop = data + first_vop;
    *vop_size = first_vop_end - first_vop;
  }
  return TRUE;
}

static CrystalHdPayloadStatus
gst_crystalhd_mpeg4_vop_payload(const CrystalHdCodec *codec,
                                const guint8 *data, gsize size)
{
  CrystalHdMpeg4Bits bits = { data, size, 0 };
  guint coding_type;
  guint value;

  if (codec->mpeg4_time_increment_bits == 0 ||
      codec->mpeg4_time_increment_bits > 16 ||
      !gst_crystalhd_mpeg4_read_bits(&bits, 2, &coding_type))
    return CRYSTALHD_PAYLOAD_INVALID;
  if ((codec->mpeg4_object_type == CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE &&
       coding_type > 1) ||
      (codec->mpeg4_object_type ==
           CRYSTALHD_MPEG4_OBJECT_TYPE_ADVANCED_SIMPLE &&
       coding_type > 2) ||
      (codec->mpeg4_object_type != CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE &&
       codec->mpeg4_object_type !=
           CRYSTALHD_MPEG4_OBJECT_TYPE_ADVANCED_SIMPLE))
    return CRYSTALHD_PAYLOAD_INVALID;

  do {
    if (!gst_crystalhd_mpeg4_read_bits(&bits, 1, &value))
      return CRYSTALHD_PAYLOAD_INVALID;
  } while (value != 0);
  if (!gst_crystalhd_mpeg4_marker(&bits) ||
      !gst_crystalhd_mpeg4_skip_bits(&bits,
                                     codec->mpeg4_time_increment_bits) ||
      !gst_crystalhd_mpeg4_marker(&bits) ||
      !gst_crystalhd_mpeg4_read_bits(&bits, 1, &value))
    return CRYSTALHD_PAYLOAD_INVALID;
  return value != 0 ? CRYSTALHD_PAYLOAD_CODED
                    : CRYSTALHD_PAYLOAD_NOT_CODED;
}

/* vc1parse defines these formats in gst/videoparsers/gstvc1parse.c:
 * https://gstreamer.freedesktop.org/documentation/videoparsersbad/vc1parse.html
 * ASF packets and Annex-L frame layers carry pictures; a BDU can instead be
 * only a sequence header, entry point, slice, or second field.
 */
static gboolean
gst_crystalhd_codec_from_caps(GstCaps *caps, CrystalHdCodec *codec)
{
  const GstStructure *s = gst_caps_get_structure(caps, 0);
  const gchar *name = gst_structure_get_name(s);
  const gchar *format = gst_structure_get_string(s, "format");
  const gchar *stream = gst_structure_get_string(s, "stream-format");
  const gchar *header = gst_structure_get_string(s, "header-format");
  const gchar *profile;
  const gchar *level;
  const gchar *interlace;
  gboolean systemstream;
  gboolean parsed;
  gint version;
  gint width;
  gint height;

  memset(codec, 0, sizeof(*codec));
  codec->subtype = BC_MSUBTYPE_INVALID;
  if (g_str_equal(name, "video/x-h264"))
    codec->subtype = BC_MSUBTYPE_H264;
  else if (g_str_equal(name, "video/mpeg")) {
    if (!gst_structure_get_int(s, "mpegversion", &version))
      return FALSE;
    if (version == 2) {
      codec->subtype = BC_MSUBTYPE_MPEG2VIDEO;
    } else if (version == 4) {
      profile = gst_structure_get_string(s, "profile");
      level = gst_structure_get_string(s, "level");
      interlace = gst_structure_get_string(s, "interlace-mode");
      if (!gst_structure_get_boolean(s, "systemstream", &systemstream) ||
          systemstream ||
          !gst_structure_get_boolean(s, "parsed", &parsed) || !parsed ||
          (g_strcmp0(profile, "simple") != 0 &&
           g_strcmp0(profile, "advanced-simple") != 0) ||
          (g_strcmp0(level, "3") != 0 && g_strcmp0(level, "5") != 0) ||
          (gst_structure_has_field(s, "interlace-mode") &&
           (interlace == NULL || !g_str_equal(interlace, "progressive"))) ||
          gst_structure_has_field(s, "sprite-warping-points"))
        return FALSE;
      if (gst_structure_has_field(s, "width")) {
        if (!gst_structure_get_int(s, "width", &width) ||
            width <= 0 || width > 1920)
          return FALSE;
        codec->mpeg4_width = (guint)width;
      }
      if (gst_structure_has_field(s, "height")) {
        if (!gst_structure_get_int(s, "height", &height) ||
            height <= 0 || height > 1088)
          return FALSE;
        codec->mpeg4_height = (guint)height;
      }
      if ((codec->mpeg4_width == 0) != (codec->mpeg4_height == 0))
        return FALSE;
      codec->subtype = BC_MSUBTYPE_DIVX;
      codec->mpeg4_object_type = g_str_equal(profile, "simple")
          ? CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE
          : CRYSTALHD_MPEG4_OBJECT_TYPE_ADVANCED_SIMPLE;
      codec->mpeg4_profile_level =
          (codec->mpeg4_object_type ==
               CRYSTALHD_MPEG4_OBJECT_TYPE_ADVANCED_SIMPLE ? 0xf0 : 0) |
          (g_str_equal(level, "3") ? 3 : 5);
    }
  }
  else if (g_str_equal(name, "video/x-vc1")) {
    codec->subtype = BC_MSUBTYPE_VC1;
    codec->vc1_bdu = TRUE;
  } else if (g_str_equal(name, "video/x-wmv")) {
    if (!gst_structure_get_int(s, "wmvversion", &version) || version != 3)
      return FALSE;
    if (g_strcmp0(format, "WVC1") == 0) {
      codec->vc1_bdu = g_strcmp0(stream, "bdu") == 0 ||
                       g_strcmp0(stream, "bdu-frame") == 0;
      if (codec->vc1_bdu) {
        if (header != NULL && !g_str_equal(header, "none"))
          return FALSE;
        codec->subtype = BC_MSUBTYPE_VC1;
      } else {
        if (stream != NULL && !g_str_equal(stream, "asf"))
          return FALSE;
        if (header != NULL && !g_str_equal(header, "asf"))
          return FALSE;
        codec->subtype = BC_MSUBTYPE_WVC1;
      }
    } else if (format == NULL || g_str_equal(format, "WMV3")) {
      if (stream != NULL && !g_str_equal(stream, "asf") &&
          !g_str_equal(stream, "frame-layer"))
        return FALSE;
      if (header != NULL && !g_str_equal(header, "asf"))
        return FALSE;
      codec->subtype = BC_MSUBTYPE_WMV3;
      codec->frame_layer = g_strcmp0(stream, "frame-layer") == 0;
    }
  }
  return codec->subtype != BC_MSUBTYPE_INVALID;
}

static gboolean
gst_crystalhd_codec_metadata(CrystalHdCodec *codec,
                             const guint8 **data, gsize *size)
{
  if (codec == NULL || data == NULL || size == NULL || *size > G_MAXUINT32)
    return FALSE;
  if (codec->subtype == BC_MSUBTYPE_WMV3) {
    /* DtsSetVC1SH copies only the first four STRUCT_C bytes for Simple/Main.
     * AVI/ASF codec_data may append encoder bytes (WMV9 VCM uses six bytes).
     * Keep the original header, including legacy reserved bits, unchanged;
     * this normalizes container metadata, not unsupported coding tools.
     */
    if (*data == NULL || *size < 4)
      return FALSE;
    *size = 4;
  } else if (codec->subtype == BC_MSUBTYPE_WVC1) {
    static const guint8 sequence_start[] = { 0, 0, 1, 0x0f };
    if (*data == NULL || *size < 8)
      return FALSE;
    /* GStreamer's ASF codec_data includes a binding byte. libcrystalhd's
     * DtsSetVC1SH expects the sequence/entry-point startcodes themselves.
     * Also accept demuxers that already removed that byte.
     */
    if (memcmp(*data, sequence_start, sizeof(sequence_start)) != 0) {
      (*data)++;
      (*size)--;
    }
    if (memcmp(*data, sequence_start, sizeof(sequence_start)) != 0)
      return FALSE;
  } else if (codec->subtype == BC_MSUBTYPE_DIVX) {
    guint vos_count, vol_count, vop_count;
    guint8 profile_level;
    const guint8 *vol;
    const guint8 *vop;
    gsize vol_size;
    gsize vop_size;
    guint width;
    guint height;
    guint time_resolution;
    guint time_bits;
    if (!gst_crystalhd_mpeg4_start_codes(*data, *size, &vos_count,
                                         &profile_level, &vol_count,
                                         &vop_count, &vol, &vol_size,
                                         &vop, &vop_size) ||
        vos_count != 1 || profile_level != codec->mpeg4_profile_level ||
        vol_count != 1 || vop_count != 0 ||
        !gst_crystalhd_mpeg4_vol_supported(vol, vol_size,
                                           codec->mpeg4_object_type,
                                           &width, &height, &time_resolution,
                                           &time_bits) ||
        (codec->mpeg4_width != 0 && codec->mpeg4_width != width) ||
        (codec->mpeg4_height != 0 && codec->mpeg4_height != height))
      return FALSE;
    codec->mpeg4_time_increment_resolution = time_resolution;
    codec->mpeg4_time_increment_bits = time_bits;
  }
  return *size <= G_MAXUINT32;
}

static CrystalHdPayloadStatus
gst_crystalhd_codec_payload(const CrystalHdCodec *codec,
                            const guint8 **data, gsize *size)
{
  CrystalHdPayloadStatus payload = CRYSTALHD_PAYLOAD_CODED;

  if (codec == NULL || data == NULL || size == NULL || *data == NULL)
    return CRYSTALHD_PAYLOAD_INVALID;
  if (codec->subtype == BC_MSUBTYPE_DIVX) {
    guint vos_count, vol_count, vop_count;
    guint8 profile_level;
    const guint8 *vol;
    const guint8 *vop;
    gsize vol_size;
    gsize vop_size;
    guint width = codec->mpeg4_width;
    guint height = codec->mpeg4_height;
    guint time_resolution = codec->mpeg4_time_increment_resolution;
    guint time_bits = codec->mpeg4_time_increment_bits;
    if (!gst_crystalhd_mpeg4_start_codes(*data, *size, &vos_count,
                                         &profile_level, &vol_count,
                                         &vop_count, &vol, &vol_size,
                                         &vop, &vop_size) ||
        vop_count != 1 || vos_count > 1 || vol_count > 1 ||
        (vos_count == 1 && profile_level != codec->mpeg4_profile_level) ||
        (vol_count == 1 &&
         !gst_crystalhd_mpeg4_vol_supported(vol, vol_size,
                                            codec->mpeg4_object_type,
                                            &width, &height, &time_resolution,
                                            &time_bits)) ||
        (vol_count == 1 && codec->mpeg4_width != 0 &&
         codec->mpeg4_width != width) ||
        (vol_count == 1 && codec->mpeg4_height != 0 &&
         codec->mpeg4_height != height) ||
        (vol_count == 1 &&
         (codec->mpeg4_time_increment_resolution != time_resolution ||
          codec->mpeg4_time_increment_bits != time_bits)))
      return CRYSTALHD_PAYLOAD_INVALID;
    payload = gst_crystalhd_mpeg4_vop_payload(codec, vop, vop_size);
    if (payload == CRYSTALHD_PAYLOAD_INVALID)
      return payload;
  }
  if (codec->frame_layer) {
    /* SMPTE 421M Annex L: 24-bit little-endian picture length, flags,
     * then a 32-bit timestamp. These bytes are not compressed video.
     */
    if (*size < 8 || GST_READ_UINT24_LE(*data) != *size - 8)
      return CRYSTALHD_PAYLOAD_INVALID;
    *data += 8;
    *size -= 8;
  }
  /* DtsAddVC1SCode uses 32-bit size*2 allocation arithmetic before copying
   * ASF pictures. Bound these codec paths before they reach that API; the
   * same limit already applies to raw BDU input. Other codecs are unchanged.
   */
  if ((codec->subtype == BC_MSUBTYPE_VC1 ||
       codec->subtype == BC_MSUBTYPE_WVC1 ||
       codec->subtype == BC_MSUBTYPE_WMV3) &&
      *size > GST_CRYSTALHD_VC1_MAX_PICTURE_SIZE)
    return CRYSTALHD_PAYLOAD_INVALID;
  if (*size == 0 || *size > G_MAXUINT32 || *size > G_MAXSIZE - 4)
    return CRYSTALHD_PAYLOAD_INVALID;
  return payload;
}

static gsize
gst_crystalhd_vc1_frame_size(const guint8 *data, gsize size, gboolean at_eos)
{
  gboolean have_picture = FALSE;
  gsize i;

  for (i = 0; i + 4 <= size; i++) {
    guint8 type;
    if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
      continue;
    type = data[i + 3];
    if (have_picture && (type == 0x0d || type == 0x0f || type == 0x0e))
      return i;
    if (type == 0x0d)
      have_picture = TRUE;
    i += 3;
  }
  /* Keep sequence/entry headers with their picture and field/slice BDUs
   * with the picture preceding them. Header-only buffers aren't pictures.
   */
  return at_eos && have_picture ? size : 0;
}

#endif
