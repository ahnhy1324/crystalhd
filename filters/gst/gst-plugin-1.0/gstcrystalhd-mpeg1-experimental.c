/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Optional, rank-NONE MPEG-1 experiment. Never linked into libgstcrystalhd.so.
 * Qualified input: actual MPEG-1 352x288 progressive 25/1, native output,
 * BCM70015, existing algorithm-1 DIL. No selector5 or DIL changes.
 *
 * External MPEG-1 caps/codec_data/payload/PTS remain unchanged. The existing
 * DIL transport subtype MPEG2VIDEO selects algorithm1 internally; it does not
 * relabel input caps as MPEG-2 or enable ordinary crystalhddec admission.
 */
#include <gst/gst.h>
#include "gstcrystalhd-codecs.h"
#include "gstcrystalhd-mpeg1-input.h"

G_STATIC_ASSERT(BC_VID_ALGO_MPEG2 == 1);

static gboolean
gst_crystalhd_mpeg1_device_allowed(guint device)
{
  /* DtsCrystalHDVersion: 0 = BCM70012, 1 = BCM70015. Unknown is rejected. */
  return device == 1;
}

static gboolean
gst_crystalhd_mpeg1_scale_allowed(guint width)
{
  return width == 0;
}

static gboolean
experimental_mpeg1_codec_from_caps(GstCaps *caps, CrystalHdCodec *codec)
{
  CrystalHdCodec ordinary;
  const GstStructure *s;
  const GValue *value;
  GstBuffer *metadata;
  GstMapInfo map;
  const gchar *reason = NULL;
  gboolean parsed, systemstream, valid;
  gint version, width, height, fps_n, fps_d;

  if (codec == NULL)
    return FALSE;
  memset(codec, 0, sizeof(*codec));
  codec->subtype = BC_MSUBTYPE_INVALID;
  if (caps == NULL || !gst_caps_is_fixed(caps) || gst_caps_get_size(caps) != 1)
    return FALSE;
  /* Refer to the original reader, but never admit its other public codecs
   * through this explicitly selected, MPEG-1-only factory. */
  if (gst_crystalhd_codec_from_caps(caps, &ordinary))
    return FALSE;
  s = gst_caps_get_structure(caps, 0);
  if (!gst_structure_has_name(s, "video/mpeg") ||
      !gst_structure_get_int(s, "mpegversion", &version) || version != 1 ||
      !gst_structure_get_boolean(s, "systemstream", &systemstream) || systemstream ||
      !gst_structure_get_boolean(s, "parsed", &parsed) || !parsed ||
      !gst_structure_get_int(s, "width", &width) || width != 352 ||
      !gst_structure_get_int(s, "height", &height) || height != 288 ||
      !gst_structure_get_fraction(s, "framerate", &fps_n, &fps_d) ||
      fps_n != 25 || fps_d != 1 ||
      g_strcmp0(gst_structure_get_string(s, "interlace-mode"), "progressive") != 0)
    return FALSE;
  value = gst_structure_get_value(s, "codec_data");
  if (value == NULL || !GST_VALUE_HOLDS_BUFFER(value))
    return FALSE;
  metadata = gst_value_get_buffer(value);
  if (metadata == NULL || gst_buffer_get_size(metadata) == 0 ||
      gst_buffer_get_size(metadata) > 1024U * 1024U ||
      !gst_buffer_map(metadata, &map, GST_MAP_READ))
    return FALSE;
  valid = gst_crystalhd_mpeg1_sequence_header_valid(map.data, map.size, &reason);
  gst_buffer_unmap(metadata, &map);
  if (!valid)
    return FALSE;
  codec->subtype = BC_MSUBTYPE_MPEG2VIDEO;
  return TRUE;
}

#define GST_CRYSTALHD_MPEG1_EXPERIMENTAL 1
#define GstCrystalHdDec GstCrystalHdMpeg1ExperimentalDec
#define GstCrystalHdDecClass GstCrystalHdMpeg1ExperimentalDecClass
#define gst_crystalhd_dec gst_crystalhd_mpeg1_experimental_dec
#define gst_crystalhd_dec_get_type gst_crystalhd_mpeg1_experimental_dec_get_type
#define gst_crystalhd_dec_class_init gst_crystalhd_mpeg1_experimental_dec_class_init
#define gst_crystalhd_dec_init gst_crystalhd_mpeg1_experimental_dec_init
#define gst_crystalhd_dec_parent_class gst_crystalhd_mpeg1_experimental_dec_parent_class
#define gst_crystalhd_codec_from_caps experimental_mpeg1_codec_from_caps
#include "gstcrystalhd.c"
#undef gst_crystalhd_codec_from_caps
#undef gst_crystalhd_dec_parent_class
#undef gst_crystalhd_dec_init
#undef gst_crystalhd_dec_class_init
#undef gst_crystalhd_dec_get_type
#undef gst_crystalhd_dec
#undef GstCrystalHdDecClass
#undef GstCrystalHdDec
#undef GST_CRYSTALHD_MPEG1_EXPERIMENTAL

static gboolean
mpeg1_experimental_register(GstPlugin *plugin)
{
  const gchar *name = "crystalhdmpeg1experimentaldec";
  GType type;
  GstElementFactory *factory;
  GST_DEBUG_CATEGORY_INIT(gst_crystalhd_debug, "crystalhdmpeg1experimental", 0,
                          "Scoped experimental MPEG-1 algorithm-1 decoder");
  type = gst_crystalhd_mpeg1_experimental_dec_get_type();
  factory = gst_element_factory_find(name);
  if (factory != NULL) {
    GType existing_type = gst_element_factory_get_element_type(factory);
    gboolean same = existing_type == type &&
        gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factory)) == GST_RANK_NONE;
    gst_object_unref(factory);
    /* A cached factory can precede plugin loading; register its type now. */
    if (existing_type != 0)
      return same;
  }
  return gst_element_register(plugin, name, GST_RANK_NONE, type);
}

gboolean
gst_crystalhd_mpeg1_experimental_register(void)
{
  return mpeg1_experimental_register(NULL);
}

/* Structural self-test vectors are never submitted to DIL. */
static const guint8 mpeg1_test_sequence[] = {
  0, 0, 1, 0xb3, 0x16, 0x01, 0x20, 0x13, 0xff, 0xff, 0xe0, 0xa0
};

static void
mpeg1_test_bits(guint8 *data, guint *bit, guint width, guint value)
{
  g_assert(*bit + width <= 72);
  for (guint n = width; n != 0; n--, (*bit)++)
    data[*bit / 8] |= ((value >> (n - 1)) & 1) << (7 - *bit % 8);
}

static guint
mpeg1_test_picture(guint8 data[9], guint type, guint forward, guint backward)
{
  guint bit = 32;
  memset(data, 0, 9);
  data[2] = 1;
  mpeg1_test_bits(data, &bit, 10, 0);
  mpeg1_test_bits(data, &bit, 3, type);
  mpeg1_test_bits(data, &bit, 16, 0xffff);
  if (type == 2 || type == 3) {
    mpeg1_test_bits(data, &bit, 1, 0);
    mpeg1_test_bits(data, &bit, 3, forward);
  }
  if (type == 3) {
    mpeg1_test_bits(data, &bit, 1, 0);
    mpeg1_test_bits(data, &bit, 3, backward);
  }
  mpeg1_test_bits(data, &bit, 1, 0);
  return (bit + 7) / 8;
}

static GByteArray *
mpeg1_test_au(guint type, guint forward, guint backward, gboolean admin)
{
  const guint8 gop[] = {0, 0, 1, 0xb8, 0, 8, 0, 0};
  const guint8 user[] = {0, 0, 1, 0xb2, 'q'};
  const guint8 slice[] = {0, 0, 1, 1, 0x43};
  const guint8 end[] = {0, 0, 1, 0xb7};
  guint8 picture[9];
  guint count = mpeg1_test_picture(picture, type, forward, backward);
  GByteArray *au = g_byte_array_new();
  if (admin) {
    g_byte_array_append(au, mpeg1_test_sequence, sizeof(mpeg1_test_sequence));
    g_byte_array_append(au, gop, sizeof(gop));
    g_byte_array_append(au, user, sizeof(user));
  }
  g_byte_array_append(au, picture, count);
  g_byte_array_append(au, slice, sizeof(slice));
  if (admin)
    g_byte_array_append(au, end, sizeof(end));
  return au;
}

static GstCaps *
mpeg1_test_caps(const guint8 *data, gsize size)
{
  GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
  GstCaps *caps;
  if (size)
    gst_buffer_fill(buffer, 0, data, size);
  caps = gst_caps_new_simple("video/mpeg", "mpegversion", G_TYPE_INT, 1,
      "systemstream", G_TYPE_BOOLEAN, FALSE, "parsed", G_TYPE_BOOLEAN, TRUE,
      "width", G_TYPE_INT, 352, "height", G_TYPE_INT, 288,
      "framerate", GST_TYPE_FRACTION, 25, 1,
      "interlace-mode", G_TYPE_STRING, "progressive",
      "codec_data", GST_TYPE_BUFFER, buffer, NULL);
  gst_buffer_unref(buffer);
  return caps;
}

/* This API inspects caps, static bytes and type metadata only. It never
 * instantiates/drives a decoder, opens DIL, or submits a compressed picture. */
gboolean
gst_crystalhd_mpeg1_experimental_self_test(void)
{
  const gchar *reason = NULL;
  GstCaps *caps = mpeg1_test_caps(mpeg1_test_sequence, sizeof(mpeg1_test_sequence));
  GstCaps *before = gst_caps_copy(caps);
  GstCaps *other;
  GstElementFactory *factory;
  CrystalHdCodec codec;
  gboolean ok = TRUE;
  guint checks = 0;
#define MPEG1_CHECK(condition, label) G_STMT_START { \
  checks++; \
  if (!(condition)) { \
    g_printerr("MPEG-1 experimental self-test failed: %s\n", (label)); \
    ok = FALSE; \
  } \
} G_STMT_END
  MPEG1_CHECK(gst_crystalhd_mpeg1_experimental_register(), "registration");
  factory = gst_element_factory_find("crystalhdmpeg1experimentaldec");
  MPEG1_CHECK(factory != NULL, "exact factory");
  if (factory != NULL) {
    MPEG1_CHECK(gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factory)) ==
        GST_RANK_NONE, "rank NONE");
    MPEG1_CHECK(gst_element_factory_get_element_type(factory) ==
        gst_crystalhd_mpeg1_experimental_dec_get_type(), "exact experimental type");
    gst_object_unref(factory);
  }
  MPEG1_CHECK(!gst_crystalhd_codec_from_caps(caps, &codec),
               "ordinary reader rejects MPEG-1");
  other = gst_caps_copy(caps);
  gst_caps_set_simple(other, "mpegversion", G_TYPE_INT, 2, NULL);
  MPEG1_CHECK(gst_crystalhd_codec_from_caps(other, &codec) &&
      codec.subtype == BC_MSUBTYPE_MPEG2VIDEO, "ordinary reader accepts MPEG-2");
  MPEG1_CHECK(!experimental_mpeg1_codec_from_caps(other, &codec) &&
      codec.subtype == BC_MSUBTYPE_INVALID, "experimental MPEG-2 rejection");
  gst_caps_unref(other);
  MPEG1_CHECK(experimental_mpeg1_codec_from_caps(caps, &codec) &&
      codec.subtype == BC_MSUBTYPE_MPEG2VIDEO, "qualified MPEG-1 algorithm1 route");
  MPEG1_CHECK(gst_caps_is_equal(caps, before), "original caps preserved");
  MPEG1_CHECK(gst_crystalhd_mpeg1_device_allowed(1) &&
      !gst_crystalhd_mpeg1_device_allowed(0) &&
      !gst_crystalhd_mpeg1_device_allowed(2), "BCM70015 only");
  MPEG1_CHECK(gst_crystalhd_mpeg1_scale_allowed(0) &&
      !gst_crystalhd_mpeg1_scale_allowed(128), "native output only");
  {
    const gchar *const fields[] = {"width", "height", "framerate", "parsed",
                                  "systemstream", "interlace-mode", "codec_data"};
    for (guint n = 0; n < G_N_ELEMENTS(fields); n++) {
      other = gst_caps_copy(caps);
      gst_structure_remove_field(gst_caps_get_structure(other, 0), fields[n]);
      MPEG1_CHECK(!experimental_mpeg1_codec_from_caps(other, &codec), fields[n]);
      gst_caps_unref(other);
    }
  }
  /* Additional malformed-header/AU tests below exercise the same predicates
   * called by ACCEPT_CAPS/configure and before the DIL submission path. */
  {
    const gchar *const integer_fields[] = {"mpegversion", "width", "height"};
    const gint values[] = {2, 320, 240};
    for (guint n = 0; n < G_N_ELEMENTS(values); n++) {
      other = gst_caps_copy(caps);
      gst_caps_set_simple(other, integer_fields[n], G_TYPE_INT, values[n], NULL);
      MPEG1_CHECK(!experimental_mpeg1_codec_from_caps(other, &codec),
                   "out-of-scope integer caps");
      gst_caps_unref(other);
    }
    other = gst_caps_copy(caps);
    gst_caps_set_simple(other, "framerate", GST_TYPE_FRACTION, 30, 1, NULL);
    MPEG1_CHECK(!experimental_mpeg1_codec_from_caps(other, &codec), "non-25fps caps");
    gst_caps_unref(other);
    other = gst_caps_copy(caps);
    gst_caps_set_simple(other, "parsed", G_TYPE_BOOLEAN, FALSE, NULL);
    MPEG1_CHECK(!experimental_mpeg1_codec_from_caps(other, &codec), "unparsed input");
    gst_caps_unref(other);
    other = gst_caps_copy(caps);
    gst_caps_set_simple(other, "systemstream", G_TYPE_BOOLEAN, TRUE, NULL);
    MPEG1_CHECK(!experimental_mpeg1_codec_from_caps(other, &codec), "system stream");
    gst_caps_unref(other);
    other = gst_caps_copy(caps);
    gst_caps_set_simple(other, "interlace-mode", G_TYPE_STRING, "interleaved", NULL);
    MPEG1_CHECK(!experimental_mpeg1_codec_from_caps(other, &codec), "interlaced input");
    gst_caps_unref(other);
    other = gst_caps_from_string(
        "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true");
    MPEG1_CHECK(!experimental_mpeg1_codec_from_caps(other, &codec), "non-MPEG-1 codec");
    gst_caps_unref(other);
  }
  MPEG1_CHECK(gst_crystalhd_mpeg1_sequence_header_valid(mpeg1_test_sequence,
      sizeof(mpeg1_test_sequence), &reason), "real MPEG-1 sequence header");
  for (guint size = 0; size < sizeof(mpeg1_test_sequence); size++) {
    MPEG1_CHECK(!gst_crystalhd_mpeg1_sequence_header_valid(
        mpeg1_test_sequence, size, &reason), "truncated sequence header");
    other = mpeg1_test_caps(mpeg1_test_sequence, size);
    MPEG1_CHECK(!experimental_mpeg1_codec_from_caps(other, &codec),
                 "truncated codec_data admission");
    gst_caps_unref(other);
  }
  for (guint test = 0; test < 5; test++) {
    guint8 bad[sizeof(mpeg1_test_sequence)];
    memcpy(bad, mpeg1_test_sequence, sizeof(bad));
    if (test == 0) bad[4] ^= 1;          /* width */
    if (test == 1) bad[6] ^= 0x10;       /* height */
    if (test == 2) bad[7] = 0x14;        /* frame_rate_code */
    if (test == 3) bad[10] &= ~0x20;     /* marker */
    if (test == 4) bad[11] |= 2;         /* truncated intra matrix */
    MPEG1_CHECK(!gst_crystalhd_mpeg1_sequence_header_valid(bad, sizeof(bad), &reason),
                 "invalid sequence fields");
    other = mpeg1_test_caps(bad, sizeof(bad));
    MPEG1_CHECK(!experimental_mpeg1_codec_from_caps(other, &codec),
                 "invalid sequence codec_data");
    gst_caps_unref(other);
  }
  for (guint type = 1; type <= 3; type++) {
    for (guint admin = 0; admin <= 1; admin++) {
      GByteArray *au = mpeg1_test_au(type, 1, 1, admin);
      guint8 *copy = g_memdup2(au->data, au->len);
      MPEG1_CHECK(gst_crystalhd_mpeg1_access_unit_valid(au->data, au->len, &reason),
                   "complete I/P/B with valid sequence/GOP/user-data/EOS");
      MPEG1_CHECK(memcmp(copy, au->data, au->len) == 0, "AU bytes preserved");
      g_free(copy);
      g_byte_array_unref(au);
    }
  }
  {
    const guint types[] = {4, 2, 3};
    const guint forward[] = {1, 0, 1};
    const guint backward[] = {1, 1, 0};
    for (guint n = 0; n < G_N_ELEMENTS(types); n++) {
      GByteArray *au = mpeg1_test_au(types[n], forward[n], backward[n], FALSE);
      MPEG1_CHECK(!gst_crystalhd_mpeg1_access_unit_valid(au->data, au->len, &reason),
                   "reserved picture type or zero f_code");
      g_byte_array_unref(au);
    }
  }
  {
    const guint8 extension[] = {0, 0, 1, 0xb5, 0x10};
    const guint8 slice[] = {0, 0, 1, 1, 0x43};
    const guint8 partial_prefix[] = {0, 0, 1};
    guint8 picture[9];
    guint count = mpeg1_test_picture(picture, 2, 1, 1);
    GByteArray *au = g_byte_array_new();
    g_byte_array_append(au, picture, count - 1);
    g_byte_array_append(au, slice, sizeof(slice));
    MPEG1_CHECK(!gst_crystalhd_mpeg1_access_unit_valid(au->data, au->len, &reason),
                 "truncated complete picture header");
    g_byte_array_unref(au);
    au = mpeg1_test_au(1, 1, 1, FALSE);
    {
      guint size = au->len;
      guint8 *duplicate = g_memdup2(au->data, size);
      g_byte_array_append(au, duplicate, size);
      g_free(duplicate);
    }
    MPEG1_CHECK(!gst_crystalhd_mpeg1_access_unit_valid(au->data, au->len, &reason),
                 "duplicate picture headers");
    g_byte_array_unref(au);
    au = mpeg1_test_au(1, 1, 1, FALSE);
    g_byte_array_append(au, partial_prefix, sizeof(partial_prefix));
    MPEG1_CHECK(!gst_crystalhd_mpeg1_access_unit_valid(au->data, au->len, &reason),
                 "truncated start code");
    g_byte_array_unref(au);
    au = mpeg1_test_au(1, 1, 1, FALSE);
    g_byte_array_append(au, extension, sizeof(extension));
    MPEG1_CHECK(!gst_crystalhd_mpeg1_access_unit_valid(au->data, au->len, &reason),
                 "MPEG-2 extension in compressed AU");
    g_byte_array_unref(au);
    au = g_byte_array_new();
    g_byte_array_append(au, mpeg1_test_sequence, sizeof(mpeg1_test_sequence));
    g_byte_array_append(au, extension, sizeof(extension));
    MPEG1_CHECK(!gst_crystalhd_mpeg1_sequence_header_valid(au->data, au->len, &reason),
                 "MPEG-2 extension in metadata");
    other = mpeg1_test_caps(au->data, au->len);
    MPEG1_CHECK(!experimental_mpeg1_codec_from_caps(other, &codec),
                 "extension-bearing codec_data admission");
    gst_caps_unref(other);
    g_byte_array_unref(au);
  }
  MPEG1_CHECK(!gst_crystalhd_mpeg1_access_unit_valid(mpeg1_test_sequence,
      sizeof(mpeg1_test_sequence), &reason), "headers alone are not a coded picture");
  MPEG1_CHECK(!experimental_mpeg1_codec_from_caps(NULL, &codec), "missing caps");
  MPEG1_CHECK(!experimental_mpeg1_codec_from_caps(caps, NULL), "missing destination");
  gst_caps_unref(before);
  gst_caps_unref(caps);
#undef MPEG1_CHECK
  g_print("MPEG-1 experimental self-test: %s checks=%u device-calls=0\n",
          ok ? "PASS" : "FAIL", checks);
  return ok;
}

#ifndef GST_CRYSTALHD_MPEG1_EXPERIMENTAL_SELF_TEST
#ifndef PACKAGE
#define PACKAGE "crystalhd"
#endif
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, crystalhdmpeg1experimental,
    "Explicit BCM70015 MPEG-1 352x288 progressive 25fps experiment",
    mpeg1_experimental_register, "3.10.0", "LGPL", "crystalhd",
    "https://github.com/ahnhy1324/crystalhd")
#endif
