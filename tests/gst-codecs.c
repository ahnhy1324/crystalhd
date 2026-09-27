/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstcrystalhd-codecs.h"
#include "gstcrystalhd-timing.h"

static CrystalHdCodec
codec_from_string(const gchar *text, gboolean supported)
{
  GstCaps *caps = gst_caps_from_string(text);
  CrystalHdCodec codec;
  g_assert_nonnull(caps);
  g_assert_cmpint(gst_crystalhd_codec_from_caps(caps, &codec), ==, supported);
  gst_caps_unref(caps);
  return codec;
}

static void
test_caps(void)
{
  CrystalHdCodec codec;

  codec = codec_from_string("video/x-h264,stream-format=byte-stream,alignment=au,parsed=true", TRUE);
  g_assert_cmpint(codec.subtype, ==, BC_MSUBTYPE_H264);
  g_assert_false(codec.vc1_bdu);
  codec = codec_from_string("video/mpeg,mpegversion=2,systemstream=false,parsed=true", TRUE);
  g_assert_cmpint(codec.subtype, ==, BC_MSUBTYPE_MPEG2VIDEO);
  codec = codec_from_string("video/mpeg,mpegversion=4,systemstream=false,"
      "parsed=true,profile=simple,level=(string)3", TRUE);
  g_assert_cmpint(codec.subtype, ==, BC_MSUBTYPE_DIVX);
  g_assert_cmpint(codec.mpeg4_object_type, ==,
                  CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE);
  g_assert_cmpuint(codec.mpeg4_profile_level, ==, 0x03);
  codec = codec_from_string("video/mpeg,mpegversion=4,systemstream=false,"
      "parsed=true,profile=advanced-simple,level=(string)3,"
      "interlace-mode=progressive", TRUE);
  g_assert_cmpint(codec.subtype, ==, BC_MSUBTYPE_DIVX);
  g_assert_cmpint(codec.mpeg4_object_type, ==,
                  CRYSTALHD_MPEG4_OBJECT_TYPE_ADVANCED_SIMPLE);
  g_assert_cmpuint(codec.mpeg4_profile_level, ==, 0xf3);
  codec = codec_from_string("video/mpeg,mpegversion=4,systemstream=false,"
      "parsed=true,profile=simple,level=(string)5,width=1920,height=1080", TRUE);
  g_assert_cmpint(codec.mpeg4_object_type, ==,
                  CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE);
  g_assert_cmpuint(codec.mpeg4_profile_level, ==, 0x05);
  g_assert_cmpuint(codec.mpeg4_width, ==, 1920);
  g_assert_cmpuint(codec.mpeg4_height, ==, 1080);
  codec = codec_from_string("video/mpeg,mpegversion=4,systemstream=false,"
      "parsed=true,profile=advanced-simple,level=(string)5", TRUE);
  g_assert_cmpint(codec.mpeg4_object_type, ==,
                  CRYSTALHD_MPEG4_OBJECT_TYPE_ADVANCED_SIMPLE);
  g_assert_cmpuint(codec.mpeg4_profile_level, ==, 0xf5);
  codec = codec_from_string("video/x-wmv,wmvversion=3,format=WVC1,stream-format=asf,header-format=asf", TRUE);
  g_assert_cmpint(codec.subtype, ==, BC_MSUBTYPE_WVC1);
  g_assert_false(codec.vc1_bdu);
  codec = codec_from_string("video/x-wmv,wmvversion=3,format=WVC1,stream-format=bdu,header-format=none", TRUE);
  g_assert_cmpint(codec.subtype, ==, BC_MSUBTYPE_VC1);
  g_assert_true(codec.vc1_bdu);
  codec = codec_from_string("video/x-wmv,wmvversion=3,format=WVC1,stream-format=bdu-frame,header-format=none", TRUE);
  g_assert_true(codec.vc1_bdu);
  codec = codec_from_string("video/x-vc1,parsed=true", TRUE);
  g_assert_true(codec.vc1_bdu);
  codec = codec_from_string("video/x-wmv,wmvversion=3,format=WMV3,stream-format=frame-layer,header-format=asf", TRUE);
  g_assert_cmpint(codec.subtype, ==, BC_MSUBTYPE_WMV3);
  g_assert_true(codec.frame_layer);
  codec = codec_from_string("video/x-wmv,wmvversion=3,format=WMV3", TRUE);
  g_assert_false(codec.frame_layer); /* legacy demuxer caps mean ASF packets */
  codec_from_string("video/x-wmv,wmvversion=2,format=WMV2", FALSE);
  codec_from_string("video/x-wmv,wmvversion=3,format=unknown", FALSE);
  codec_from_string("video/x-wmv,wmvversion=3,format=WMV3,stream-format=sequence-layer-frame-layer", FALSE);
  codec_from_string("video/x-wmv,wmvversion=3,format=WMV3,header-format=none", FALSE);
  codec_from_string("video/x-wmv,wmvversion=3,format=WVC1,stream-format=bdu,header-format=asf", FALSE);
  codec_from_string("video/x-wmv,wmvversion=3,format=WVC1,stream-format=frame-layer", FALSE);
  codec_from_string("video/mpeg,systemstream=false,parsed=true", FALSE);
  codec_from_string("video/mpeg,mpegversion=1,systemstream=false,parsed=true", FALSE);
  codec_from_string("video/mpeg,mpegversion=4,systemstream=false,parsed=true,level=(string)3", FALSE);
  codec_from_string("video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=main,level=(string)3", FALSE);
  codec_from_string("video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=simple", FALSE);
  codec_from_string("video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=simple,level=(string)4", FALSE);
  codec_from_string("video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=simple,level=(int)5", FALSE);
  codec_from_string("video/mpeg,mpegversion=4,systemstream=true,parsed=true,profile=simple,level=(string)3", FALSE);
  codec_from_string("video/mpeg,mpegversion=4,systemstream=false,parsed=false,profile=simple,level=(string)3", FALSE);
  codec_from_string("video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=advanced-simple,level=(string)3,interlace-mode=interleaved", FALSE);
  codec_from_string("video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=advanced-simple,level=(string)3,interlace-mode=(int)0", FALSE);
  codec_from_string("video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=advanced-simple,level=(string)3,sprite-warping-points=1", FALSE);
  codec_from_string("video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=advanced-simple,level=(string)3,sprite-warping-points=0", FALSE);
  codec_from_string("video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=simple,level=(string)3,width=1921,height=1080", FALSE);
  codec_from_string("video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=simple,level=(string)3,width=1920,height=1089", FALSE);
  codec_from_string("video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=simple,level=(string)3,width=640", FALSE);
  codec_from_string("video/x-divx,divxversion=5", FALSE);
}

static void
test_pad_caps(void)
{
  const gchar *accepted[] = {
    /* Exact asfdemux WMV3 output: no stream/header-format or framerate. */
    "video/x-wmv,wmvversion=3,format=WMV3,width=720,height=576,codec_data=(buffer)41f38001",
    "video/x-wmv,wmvversion=3,format=WVC1,width=176,height=144",
    "video/x-wmv,wmvversion=3,width=720,height=576",
    "video/x-wmv,wmvversion=3,format=WVC1,stream-format=bdu,header-format=none",
    "video/x-wmv,wmvversion=3,format=WVC1,stream-format=bdu-frame",
    "video/x-wmv,wmvversion=3,format=WMV3,stream-format=frame-layer,header-format=asf",
    "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true",
    "video/mpeg,mpegversion=2,systemstream=false,parsed=true",
    "video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=simple,"
      "level=(string)3,interlace-mode=progressive,width=640,height=360,"
      "codec_data=(buffer)"
      "000001b003000001b58913000001000000012000c48d8800f514042d1463"
      "000001b24c61766336312e31392e313031",
    "video/mpeg,mpegversion=4,systemstream=false,parsed=true,"
      "profile=advanced-simple,level=(string)3,width=640,height=360,"
      "codec_data=(buffer)"
      "000001b0f3000001b5a913000001000000012008d48d0800f514042d14183f"
      "000001b24c61766336312e31392e313031",
    "video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=simple,"
      "level=(string)5,interlace-mode=progressive,width=1920,height=1080,"
      "codec_data=(buffer)"
      "000001b005000001b58913000001000000012000c48d8800f53c04871463"
      "000001b24c61766336312e31392e313031",
    "video/mpeg,mpegversion=4,systemstream=false,parsed=true,"
      "profile=advanced-simple,level=(string)5,width=1920,height=1080,"
      "codec_data=(buffer)"
      "000001b0f5000001b5a913000001000000012008d48d0800f53c048714183f"
      "000001b24c61766336312e31392e313031"
  };
  const gchar *rejected[] = {
    "video/x-wmv,wmvversion=2,format=WMV2",
    "video/x-wmv,wmvversion=3,format=unknown",
    "video/x-wmv,wmvversion=3,format=WMV3,stream-format=sequence-layer-frame-layer",
    "video/x-wmv,wmvversion=3,format=WMV3,header-format=none",
    "video/x-wmv,wmvversion=3,format=WVC1,stream-format=bdu,header-format=asf",
    "video/x-wmv,wmvversion=3,format=WVC1,stream-format=frame-layer",
    "video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=simple,level=(string)4",
    "video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=advanced-simple,level=(string)3,sprite-warping-points=2",
    "video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=simple,level=(string)3",
    "video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=simple,"
      "level=(string)3,codec_data=(buffer)00000120",
    "video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=simple,"
      "level=(string)3,codec_data=(buffer)"
      "000001b0f3000001b5a913000001000000012008d48d0800f514042d14103f"
      "000001b24c61766336312e31392e313031",
    "video/mpeg,mpegversion=4,systemstream=false,parsed=true,"
      "profile=advanced-simple,level=(string)3,codec_data=(buffer)"
      "000001b003000001b58913000001000000012000c48d8800f514042d1463"
      "000001b24c61766336312e31392e313031",
    "video/mpeg,mpegversion=4,systemstream=false,parsed=true,"
      "profile=advanced-simple,level=(string)3,codec_data=(buffer)"
      "000001b0f3000001b5a913000001000000012008d48d0800f514042d14103f"
      "000001b24c61766336312e31392e313031",
    "video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=simple,"
      "codec_data=(buffer)"
      "000001b003000001b58913000001000000012000c48d8800f514042d1463"
      "000001b24c61766336312e31392e313031",
    "video/mpeg,mpegversion=4,systemstream=false,parsed=true,profile=simple,"
      "level=(string)4,codec_data=(buffer)"
      "000001b003000001b58913000001000000012000c48d8800f514042d1463"
      "000001b24c61766336312e31392e313031",
    "video/x-divx,divxversion=5"
  };
  GstElement *decoder = gst_element_factory_make("crystalhddec", NULL);
  GstPad *sink;
  guint i;

  /* Construct/query only: never set a format/state or open hardware. */
  g_assert_nonnull(decoder);
  sink = gst_element_get_static_pad(decoder, "sink");
  g_assert_nonnull(sink);
  for (i = 0; i < G_N_ELEMENTS(accepted); i++) {
    GstCaps *caps = gst_caps_from_string(accepted[i]);
    g_assert_true(gst_pad_query_accept_caps(sink, caps));
    gst_caps_unref(caps);
  }
  for (i = 0; i < G_N_ELEMENTS(rejected); i++) {
    GstCaps *caps = gst_caps_from_string(rejected[i]);
    g_assert_false(gst_pad_query_accept_caps(sink, caps));
    gst_caps_unref(caps);
  }
  gst_object_unref(sink);
  gst_object_unref(decoder);
}

static void
test_metadata(void)
{
  const guint8 wmv[] = { 0x41, 0xf3, 0x80, 0x01, 0 };
  const guint8 asf[] = { 0x2b, 0, 0, 1, 0x0f, 0xc2, 0, 0, 0,
                         0, 0, 1, 0x0e, 0x80, 0 };
  const guint8 bad[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  CrystalHdCodec codec = { .subtype = BC_MSUBTYPE_WMV3 };
  const guint8 *data = wmv;
  gsize size = sizeof(wmv);

  g_assert_true(gst_crystalhd_codec_metadata(&codec, &data, &size));
  g_assert_cmpuint(size, ==, 4);
  size = 3;
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  data = NULL;
  size = 0;
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));

  codec.subtype = BC_MSUBTYPE_WVC1;
  data = asf;
  size = sizeof(asf);
  g_assert_true(gst_crystalhd_codec_metadata(&codec, &data, &size));
  g_assert_true(data == asf + 1);
  g_assert_cmpuint(size, ==, sizeof(asf) - 1);
  g_assert_true(gst_crystalhd_codec_metadata(&codec, &data, &size));
  g_assert_true(data == asf + 1); /* already-normalized FFmpeg codec_data */
  data = bad;
  size = sizeof(bad);
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
}

static void
set_mpeg4_vol_bit(guint8 *data, gsize size, gsize body_offset, guint bit,
                  gboolean enabled)
{
  gsize byte = body_offset + bit / 8;
  guint8 mask = (guint8)(1U << (7 - bit % 8));

  g_assert_cmpuint(byte, <, size);
  if (enabled)
    data[byte] |= mask;
  else
    data[byte] &= (guint8)~mask;
}

static void
set_mpeg4_vol_bits(guint8 *data, gsize size, gsize body_offset, guint bit,
                   guint count, guint value)
{
  guint index;

  g_assert_cmpuint(count, <=, 32);
  for (index = 0; index < count; index++)
    set_mpeg4_vol_bit(data, size, body_offset, bit + index,
        ((value >> (count - index - 1)) & 1U) != 0);
}

static void
test_mpeg4_framing(void)
{
  const guint8 metadata[] = {
    0,0,1,0xb0,3,0,0,1,0xb5,0x89,0x13,0,0,1,0,
    0,0,1,0x20,0,0xc4,0x8d,0x88,0,0xf5,0x14,4,0x2d,0x14,0x63,
    0,0,1,0xb2,'L','a','v','c','6','1','.','1','9','.','1','0','1'
  };
  const guint8 asp_metadata[] = {
    0,0,1,0xb0,0xf3,0,0,1,0xb5,0xa9,0x13,0,0,1,0,
    0,0,1,0x20,0x08,0xd4,0x8d,0x08,0,0xf5,0x14,4,0x2d,0x14,0x18,0x3f,
    0,0,1,0xb2,'L','a','v','c','6','1','.','1','9','.','1','0','1'
  };
  const guint8 picture[] = {
    0,0,1,0xb3,0x12,0,0,1,0xb6,0x1a,0x2b,0x3c,0,0,1,0xb1
  };
  const guint8 with_vol[] = {
    0,0,1,0x20,0,0xc4,0x8d,0x88,0,0xf5,0x14,4,0x2d,0x14,0x63,
    0,0,1,0xb6,0x1a,0x2b
  };
  const guint8 with_asp_vol[] = {
    0,0,1,0x20,0x08,0xd4,0x8d,0x08,0,0xf5,0x14,4,0x2d,0x14,0x18,0x3f,
    0,0,1,0xb6,0x1a,0x2b
  };
  const guint8 duplicate_vol[] = {
    0,0,1,0x20,0,0xc4,0x8d,0x88,0,0xf5,0x14,4,0x2d,0x14,0x63,
    0,0,1,0x20,0,0xc4,0x8d,0x88,0,0xf5,0x14,4,0x2d,0x14,0x63
  };
  const guint8 duplicate[] = {
    0,0,1,0xb6,0x11,0,0,1,0xb6,0x22
  };
  const guint8 vop_tail[] = { 0,0,1,0xb6,0x1a,0x2b };
  const guint8 matching_vos_picture[] = {
    0,0,1,0xb0,3,0,0,1,0xb6,0x1a,0x2b
  };
  const guint8 wrong_vos_picture[] = {
    0,0,1,0xb0,5,0,0,1,0xb6,0x1a,0x2b
  };
  const guint8 duplicate_vos_picture[] = {
    0,0,1,0xb0,3,0,0,1,0xb0,3,0,0,1,0xb6,0x1a,0x2b
  };
  const guint8 no_vol[] = { 0,0,1,0xb0,3,0,0,1,0xb5,0x89 };
  const guint8 metadata_vop[] = { 0,0,1,0x20,0xaa,0,0,1,0xb6,0x11 };
  const guint8 bare_vol[] = { 0,0,1,0x20 };
  const guint8 bare_vol_four[] = { 0,0,0,1,0x20 };
  const guint8 bare_vop[] = { 0,0,1,0xb6 };
  const guint8 bare_vop_four[] = { 0,0,0,1,0xb6 };
  const struct {
    guint bit;
    gboolean enabled;
  } unsupported_simple_tools[] = {
    { 76, TRUE },  /* interlaced */
    { 78, TRUE },  /* sprite_enable */
    { 79, TRUE },  /* not_8_bit */
    { 80, TRUE },  /* quant_type */
    { 81, FALSE }, /* complexity_estimation_disable */
    { 83, TRUE },  /* data_partitioned */
    { 84, TRUE }   /* scalability */
  };
  CrystalHdCodec codec = {
    .subtype = BC_MSUBTYPE_DIVX,
    .mpeg4_object_type = CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE,
    .mpeg4_profile_level = 0x03
  };
  CrystalHdCodec asp_codec = {
    .subtype = BC_MSUBTYPE_DIVX,
    .mpeg4_object_type = CRYSTALHD_MPEG4_OBJECT_TYPE_ADVANCED_SIMPLE,
    .mpeg4_profile_level = 0xf3
  };
  const guint8 *data = metadata;
  gsize size = sizeof(metadata);
  guint8 modified[128];
  CrystalHdCodec bounded_codec;
  gsize n;

  g_assert_true(gst_crystalhd_codec_metadata(&codec, &data, &size));
  g_assert_true(data == metadata);
  g_assert_cmpuint(size, ==, sizeof(metadata));
  modified[0] = 0;
  memcpy(modified + 1, metadata, sizeof(metadata));
  data = modified;
  size = sizeof(metadata) + 1;
  g_assert_true(gst_crystalhd_codec_metadata(&codec, &data, &size));
  data = asp_metadata;
  size = sizeof(asp_metadata);
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  data = metadata;
  size = sizeof(metadata);
  g_assert_false(gst_crystalhd_codec_metadata(&asp_codec, &data, &size));
  data = asp_metadata;
  size = sizeof(asp_metadata);
  g_assert_true(gst_crystalhd_codec_metadata(&asp_codec, &data, &size));
  memcpy(modified, metadata, sizeof(metadata));
  modified[4] = 5;
  data = modified;
  size = sizeof(metadata);
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  modified[4] = 4;
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  modified[4] = 0xf3;
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  data = metadata + 5;
  size = sizeof(metadata) - 5;
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  memcpy(modified, metadata, 5);
  memcpy(modified + 5, metadata, sizeof(metadata));
  data = modified;
  size = sizeof(metadata) + 5;
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));

  bounded_codec = codec;
  bounded_codec.mpeg4_width = 640;
  bounded_codec.mpeg4_height = 360;
  data = metadata;
  size = sizeof(metadata);
  g_assert_true(gst_crystalhd_codec_metadata(&bounded_codec, &data, &size));
  bounded_codec.mpeg4_width = 1920;
  bounded_codec.mpeg4_height = 1080;
  g_assert_false(gst_crystalhd_codec_metadata(&bounded_codec, &data, &size));
  memcpy(modified, metadata, sizeof(metadata));
  set_mpeg4_vol_bits(modified, sizeof(metadata), 19, 48, 13, 1921);
  data = modified;
  size = sizeof(metadata);
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  memcpy(modified, metadata, sizeof(metadata));
  set_mpeg4_vol_bits(modified, sizeof(metadata), 19, 62, 13, 1089);
  data = modified;
  size = sizeof(metadata);
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  memcpy(modified, metadata, sizeof(metadata));
  set_mpeg4_vol_bit(modified, sizeof(metadata), 19, 82, FALSE);
  data = modified;
  size = sizeof(metadata);
  g_assert_true(gst_crystalhd_codec_metadata(&codec, &data, &size));
  memcpy(modified, asp_metadata, sizeof(asp_metadata));
  set_mpeg4_vol_bit(modified, sizeof(asp_metadata), 19, 84, FALSE);
  data = modified;
  size = sizeof(asp_metadata);
  g_assert_false(gst_crystalhd_codec_metadata(&asp_codec, &data, &size));
  for (n = 0; n < G_N_ELEMENTS(unsupported_simple_tools); n++) {
    memcpy(modified, metadata, sizeof(metadata));
    set_mpeg4_vol_bit(modified, sizeof(metadata), 19,
        unsupported_simple_tools[n].bit,
        unsupported_simple_tools[n].enabled);
    data = modified;
    size = sizeof(metadata);
    g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  }
  memcpy(modified, asp_metadata, sizeof(asp_metadata));
  set_mpeg4_vol_bit(modified, sizeof(asp_metadata), 19, 82, TRUE); /* qpel */
  data = modified;
  size = sizeof(asp_metadata);
  g_assert_false(gst_crystalhd_codec_metadata(&asp_codec, &data, &size));
  for (n = 0; n < 4; n++) {
    data = metadata;
    size = n;
    g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  }
  data = no_vol;
  size = sizeof(no_vol);
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  data = metadata_vop;
  size = sizeof(metadata_vop);
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  data = duplicate_vol;
  size = sizeof(duplicate_vol);
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  data = bare_vol;
  size = sizeof(bare_vol);
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  data = bare_vol_four;
  size = sizeof(bare_vol_four);
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  data = NULL;
  size = 0;
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  data = metadata;
  size = GST_CRYSTALHD_MPEG4_MAX_INPUT_SIZE + 1;
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));

  data = picture;
  size = sizeof(picture);
  g_assert_true(gst_crystalhd_codec_payload(&codec, &data, &size));
  data = matching_vos_picture;
  size = sizeof(matching_vos_picture);
  g_assert_true(gst_crystalhd_codec_payload(&codec, &data, &size));
  data = wrong_vos_picture;
  size = sizeof(wrong_vos_picture);
  g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
  data = duplicate_vos_picture;
  size = sizeof(duplicate_vos_picture);
  g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
  modified[0] = 0;
  memcpy(modified + 1, picture, sizeof(picture));
  data = modified;
  size = sizeof(picture) + 1;
  g_assert_true(gst_crystalhd_codec_payload(&codec, &data, &size));
  data = with_vol;
  size = sizeof(with_vol);
  g_assert_true(gst_crystalhd_codec_payload(&codec, &data, &size));
  bounded_codec = codec;
  bounded_codec.mpeg4_width = 640;
  bounded_codec.mpeg4_height = 360;
  memcpy(modified, with_vol, sizeof(with_vol));
  set_mpeg4_vol_bits(modified, sizeof(with_vol), 4, 48, 13, 320);
  data = modified;
  size = sizeof(with_vol);
  g_assert_false(gst_crystalhd_codec_payload(&bounded_codec, &data, &size));
  data = with_vol;
  size = sizeof(with_vol);
  g_assert_false(gst_crystalhd_codec_payload(&asp_codec, &data, &size));
  data = with_asp_vol;
  size = sizeof(with_asp_vol);
  g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
  data = with_asp_vol;
  size = sizeof(with_asp_vol);
  g_assert_true(gst_crystalhd_codec_payload(&asp_codec, &data, &size));
  memcpy(modified, with_vol, sizeof(with_vol));
  set_mpeg4_vol_bit(modified, sizeof(with_vol), 4, 82, FALSE);
  data = modified;
  size = sizeof(with_vol);
  g_assert_true(gst_crystalhd_codec_payload(&codec, &data, &size));
  memcpy(modified, with_asp_vol, sizeof(with_asp_vol));
  set_mpeg4_vol_bit(modified, sizeof(with_asp_vol), 4, 84, FALSE);
  data = modified;
  size = sizeof(with_asp_vol);
  g_assert_false(gst_crystalhd_codec_payload(&asp_codec, &data, &size));
  memcpy(modified, with_vol, sizeof(with_vol));
  set_mpeg4_vol_bit(modified, sizeof(with_vol), 4, 76, TRUE);
  data = modified;
  size = sizeof(with_vol);
  g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
  data = duplicate;
  size = sizeof(duplicate);
  g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
  memcpy(modified, duplicate_vol, sizeof(duplicate_vol));
  memcpy(modified + sizeof(duplicate_vol), vop_tail, sizeof(vop_tail));
  data = modified;
  size = sizeof(duplicate_vol) + sizeof(vop_tail);
  g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
  data = bare_vop;
  size = sizeof(bare_vop);
  g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
  data = bare_vop_four;
  size = sizeof(bare_vop_four);
  g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
  data = no_vol;
  size = sizeof(no_vol);
  g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
  data = picture;
  size = GST_CRYSTALHD_MPEG4_MAX_INPUT_SIZE + 1;
  g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
  for (n = 0; n < 4; n++) {
    data = picture;
    size = n;
    g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
  }
}

static void
test_wmv_metadata(void)
{
  /* Original WMV9 VCM Simple codec_data, including the encoder's suffix.
   * Normalization must not replace STRUCT_C with reconstructed VA metadata.
   */
  const guint8 actual[] = { 0x0f, 0xf1, 0x8a, 0x01, 0x40, 0x0f };
  const guint8 legacy[] = { 0x05, 0xf1, 0x88, 0x00, 0x40, 0x0f };
  guint8 larger[64];
  guint8 saved[sizeof(larger)];
  CrystalHdCodec codec = { .subtype = BC_MSUBTYPE_WMV3 };
  const guint8 *data;
  gsize size, length;

  for (length = 4; length <= sizeof(actual); length++) {
    data = actual;
    size = length;
    g_assert_true(gst_crystalhd_codec_metadata(&codec, &data, &size));
    g_assert_true(data == actual);
    g_assert_cmpuint(size, ==, 4);
    g_assert_cmpmem(data, size, actual, 4);
  }
  data = legacy;
  size = sizeof(legacy);
  g_assert_true(gst_crystalhd_codec_metadata(&codec, &data, &size));
  g_assert_true(data == legacy);
  g_assert_cmpuint(size, ==, 4);
  g_assert_cmpmem(data, size, legacy, 4); /* Preserve RTM0, not a decode claim. */

  memset(larger, 0xa5, sizeof(larger));
  memcpy(larger, actual, sizeof(actual));
  memcpy(saved, larger, sizeof(saved));
  data = larger;
  size = sizeof(larger);
  g_assert_true(gst_crystalhd_codec_metadata(&codec, &data, &size));
  g_assert_true(data == larger);
  g_assert_cmpuint(size, ==, 4);
  g_assert_cmpmem(larger, sizeof(larger), saved, sizeof(saved));

  for (length = 0; length < 4; length++) {
    data = actual;
    size = length;
    g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
    g_assert_true(data == actual);
    g_assert_cmpuint(size, ==, length);
  }
  for (length = 0; length <= sizeof(actual); length++) {
    data = NULL;
    size = length;
    g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
    g_assert_null(data);
    g_assert_cmpuint(size, ==, length);
  }
  data = actual;
  size = sizeof(actual);
  g_assert_false(gst_crystalhd_codec_metadata(NULL, &data, &size));
  g_assert_false(gst_crystalhd_codec_metadata(&codec, NULL, &size));
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, NULL));
  g_assert_true(data == actual);
  g_assert_cmpuint(size, ==, sizeof(actual));

  /* No payload bytes are read here. Synthetic lengths check that truncating
   * to four cannot bypass the library's original 32-bit metadata bound.
   */
  size = G_MAXUINT32;
  g_assert_true(gst_crystalhd_codec_metadata(&codec, &data, &size));
  g_assert_true(data == actual);
  g_assert_cmpuint(size, ==, 4);
#if GLIB_SIZEOF_SIZE_T > 4
  size = (gsize)G_MAXUINT32 + 1;
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  g_assert_true(data == actual);
  g_assert_cmpuint(size, ==, (gsize)G_MAXUINT32 + 1);
  size = G_MAXSIZE;
  g_assert_false(gst_crystalhd_codec_metadata(&codec, &data, &size));
  g_assert_cmpuint(size, ==, G_MAXSIZE);
#endif
}

static void
test_frame_layer(void)
{
  const guint8 frame[] = { 3, 0, 0, 0x80, 0x28, 0, 0, 0, 0xa1, 0xb2, 0xc3 };
  CrystalHdCodec codec = { .subtype = BC_MSUBTYPE_WMV3, .frame_layer = TRUE };
  const guint8 *data = frame;
  gsize size = sizeof(frame);

  g_assert_true(gst_crystalhd_codec_payload(&codec, &data, &size));
  g_assert_true(data == frame + 8);
  g_assert_cmpuint(size, ==, 3);
  g_assert_cmpuint(data[0], ==, 0xa1);
  data = frame;
  size = sizeof(frame) - 1;
  g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
  size = 7;
  g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
  codec.frame_layer = FALSE;
  data = frame + 8;
  size = 1; /* short ASF pictures are valid; the caller adds read padding */
  g_assert_true(gst_crystalhd_codec_payload(&codec, &data, &size));
  size = 0;
  g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
}

static void
test_picture_size(void)
{
  const BC_MEDIA_SUBTYPE bounded[] = {
    BC_MSUBTYPE_VC1, BC_MSUBTYPE_WVC1, BC_MSUBTYPE_WMV3
  };
  const BC_MEDIA_SUBTYPE unchanged[] = {
    BC_MSUBTYPE_H264, BC_MSUBTYPE_MPEG2VIDEO
  };
  const guint8 byte = 0x80;
  guint i;

  /* ASF/raw payload validation reads no bytes. Use a one-byte sentinel and
   * synthetic lengths, never allocate the oversized compressed picture.
   */
  for (i = 0; i < G_N_ELEMENTS(bounded); i++) {
    CrystalHdCodec codec = { .subtype = bounded[i] };
    const guint8 *data = &byte;
    gsize size = GST_CRYSTALHD_VC1_MAX_PICTURE_SIZE;
    g_assert_true(gst_crystalhd_codec_payload(&codec, &data, &size));
    g_assert_true(data == &byte);
    size++;
    g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
    size = 0x80000080U; /* legacy uint32_t size*2 would wrap to 256 */
    g_assert_false(gst_crystalhd_codec_payload(&codec, &data, &size));
  }
  for (i = 0; i < G_N_ELEMENTS(unchanged); i++) {
    CrystalHdCodec codec = { .subtype = unchanged[i] };
    const guint8 *data = &byte;
    gsize size = GST_CRYSTALHD_VC1_MAX_PICTURE_SIZE + 1;
    g_assert_true(gst_crystalhd_codec_payload(&codec, &data, &size));
  }
}

static void
test_drain_deadline(void)
{
  const gint64 started = 123456789;
  const gint64 deadline = started + GST_CRYSTALHD_DRAIN_TIMEOUT_US;
  guint pictures;

  /* More than the old 500 iterations can produce output without exhausting
   * the time budget. Conversely, activity never extends the fixed deadline.
   */
  for (pictures = 0; pictures < 1000; pictures++) {
    gint64 elapsed = (gint64)pictures * 1000;
    g_assert_cmpint(gst_crystalhd_drain_remaining_us(deadline, started + elapsed),
                    ==, GST_CRYSTALHD_DRAIN_TIMEOUT_US - elapsed);
  }
  g_assert_cmpint(gst_crystalhd_drain_remaining_us(deadline, deadline - 1), ==, 1);
  g_assert_cmpint(gst_crystalhd_drain_remaining_us(deadline, deadline), ==, 0);
  g_assert_cmpint(gst_crystalhd_drain_remaining_us(deadline, deadline + 1), ==, 0);
  g_assert_cmpint(gst_crystalhd_drain_remaining_us(deadline, deadline + G_USEC_PER_SEC),
                  ==, 0);
}

static void
test_bdu_frames(void)
{
  const guint8 stream[] = {
    0, 0, 1, 0x0f, 0x81,             /* sequence */
    0, 0, 1, 0x0e, 0x82,             /* entry point */
    0, 0, 1, 0x0d, 0x83,             /* first picture */
    0, 0, 1, 0x0c, 0x84,             /* second field of that picture */
    0, 0, 1, 0x0b, 0x85,             /* slice of that picture */
    0, 0, 1, 0x0d, 0x86,             /* second picture */
    0, 0, 1, 0x0a                    /* end of sequence */
  };
  gsize available;

  /* A split startcode must wait for more bytes, with no phantom picture
   * generated for the sequence, entry point, field, or slice buffers.
   */
  for (available = 0; available < 29; available++)
    g_assert_cmpuint(gst_crystalhd_vc1_frame_size(stream, available, FALSE), ==, 0);
  g_assert_cmpuint(gst_crystalhd_vc1_frame_size(stream, 29, FALSE), ==, 25);
  g_assert_cmpuint(gst_crystalhd_vc1_frame_size(stream + 25, sizeof(stream) - 25, FALSE), ==, 0);
  g_assert_cmpuint(gst_crystalhd_vc1_frame_size(stream + 25, sizeof(stream) - 25, TRUE), ==, sizeof(stream) - 25);
  g_assert_cmpuint(gst_crystalhd_vc1_frame_size(stream, 10, TRUE), ==, 0);
}

int main(int argc, char **argv)
{
  gst_init(&argc, &argv);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/crystalhd/codecs/caps", test_caps);
  g_test_add_func("/crystalhd/codecs/pad-caps", test_pad_caps);
  g_test_add_func("/crystalhd/codecs/metadata", test_metadata);
  g_test_add_func("/crystalhd/codecs/mpeg4-framing", test_mpeg4_framing);
  g_test_add_func("/crystalhd/codecs/wmv-metadata", test_wmv_metadata);
  g_test_add_func("/crystalhd/codecs/frame-layer", test_frame_layer);
  g_test_add_func("/crystalhd/codecs/picture-size", test_picture_size);
  g_test_add_func("/crystalhd/codecs/drain-deadline", test_drain_deadline);
  g_test_add_func("/crystalhd/codecs/bdu-frames", test_bdu_frames);
  return g_test_run();
}
