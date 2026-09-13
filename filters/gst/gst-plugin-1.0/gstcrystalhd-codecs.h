/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_CRYSTALHD_CODECS_H
#define GST_CRYSTALHD_CODECS_H

#include <stdint.h>
#include <string.h>
#include <gst/gst.h>
#include <bc_dts_defs.h>

#define GST_CRYSTALHD_VC1_MAX_PICTURE_SIZE (16U * 1024U * 1024U)

typedef struct {
  BC_MEDIA_SUBTYPE subtype;
  gboolean vc1_bdu;
  gboolean frame_layer;
} CrystalHdCodec;

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
  gint version;

  memset(codec, 0, sizeof(*codec));
  codec->subtype = BC_MSUBTYPE_INVALID;
  if (g_str_equal(name, "video/x-h264"))
    codec->subtype = BC_MSUBTYPE_H264;
  else if (g_str_equal(name, "video/mpeg"))
    codec->subtype = BC_MSUBTYPE_MPEG2VIDEO;
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
gst_crystalhd_codec_metadata(const CrystalHdCodec *codec,
                             const guint8 **data, gsize *size)
{
  if (codec->subtype == BC_MSUBTYPE_WMV3) {
    /* DtsSetVC1SH reads exactly four STRUCT_C bytes for Simple/Main. */
    if (*data == NULL || (*size != 4 && *size != 5))
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
  }
  return *size <= G_MAXUINT32;
}

static gboolean
gst_crystalhd_codec_payload(const CrystalHdCodec *codec,
                            const guint8 **data, gsize *size)
{
  if (codec->frame_layer) {
    /* SMPTE 421M Annex L: 24-bit little-endian picture length, flags,
     * then a 32-bit timestamp. These bytes are not compressed video.
     */
    if (*size < 8 || GST_READ_UINT24_LE(*data) != *size - 8)
      return FALSE;
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
    return FALSE;
  return *size > 0 && *size <= G_MAXUINT32 && *size <= G_MAXSIZE - 4;
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
