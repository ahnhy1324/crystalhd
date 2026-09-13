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
    "video/mpeg,mpegversion=2,systemstream=false,parsed=true"
  };
  const gchar *rejected[] = {
    "video/x-wmv,wmvversion=2,format=WMV2",
    "video/x-wmv,wmvversion=3,format=unknown",
    "video/x-wmv,wmvversion=3,format=WMV3,stream-format=sequence-layer-frame-layer",
    "video/x-wmv,wmvversion=3,format=WMV3,header-format=none",
    "video/x-wmv,wmvversion=3,format=WVC1,stream-format=bdu,header-format=asf",
    "video/x-wmv,wmvversion=3,format=WVC1,stream-format=frame-layer"
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
  g_test_add_func("/crystalhd/codecs/frame-layer", test_frame_layer);
  g_test_add_func("/crystalhd/codecs/picture-size", test_picture_size);
  g_test_add_func("/crystalhd/codecs/drain-deadline", test_drain_deadline);
  g_test_add_func("/crystalhd/codecs/bdu-frames", test_bdu_frames);
  return g_test_run();
}
