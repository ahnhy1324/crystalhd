/* SPDX-License-Identifier: MIT
 * Finite CPU-only qualification source. Author did not compile or run it.
 * No CrystalHD, X11, clocked output or performance result is claimed.
 */
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <stdio.h>
#include <string.h>
#include "packed-scale-engine.h"

enum { IW = 1280, IH = 720, OW = 1920, OH = 1080, FRAMES = 12 };
#define INPUT_CAPS "video/x-raw,format=YUY2,width=1280,height=720," \
    "pixel-aspect-ratio=1/1,framerate=50/1,interlace-mode=progressive,colorimetry=2:0:0:0"
#define OUTPUT_CAPS "video/x-raw,format=UYVY,width=1920,height=1080," \
    "pixel-aspect-ratio=1/1,framerate=50/1,interlace-mode=progressive,colorimetry=2:0:0:0"

typedef struct {
  GstElement *pipeline, *source, *sink;
  GstBus *bus;
} Pipeline;

static GstClockTime input_pts(guint n)
{
  return (n && n % 2 == 0 ? n - 1 : n) * 20 * GST_MSECOND;
}

static void pipeline_close(Pipeline *p)
{
  if (p->pipeline) gst_element_set_state(p->pipeline, GST_STATE_NULL);
  gst_clear_object(&p->source); gst_clear_object(&p->sink);
  gst_clear_object(&p->bus); gst_clear_object(&p->pipeline);
}

static gboolean pipeline_open(Pipeline *p, gboolean candidate)
{
  const gchar *stock = "videoconvert name=pack n-threads=1 ! video/x-raw,format=UYVY "
      "! videoscale name=scale method=4 n-threads=2 add-borders=true";
  gchar *description = g_strdup_printf("appsrc name=source format=time is-live=false "
      "block=false max-bytes=0 caps=\"%s\" ! %s ! %s "
      "! appsink name=sink sync=false max-buffers=2 drop=false wait-on-eos=false",
      INPUT_CAPS, candidate ? "crystalhdpackedscale name=scale" : stock, OUTPUT_CAPS);
  GError *error = NULL;
  p->pipeline = gst_parse_launch(description, &error);
  g_free(description);
  if (error || !p->pipeline) {
    g_printerr("pipeline create: %s\n", error ? error->message : "null");
    g_clear_error(&error); return FALSE;
  }
  p->source = gst_bin_get_by_name(GST_BIN(p->pipeline), "source");
  p->sink = gst_bin_get_by_name(GST_BIN(p->pipeline), "sink");
  p->bus = gst_element_get_bus(p->pipeline);
  return p->source && p->sink && p->bus;
}

static gboolean pipeline_eos(Pipeline *p)
{
  if (gst_app_src_end_of_stream(GST_APP_SRC(p->source)) != GST_FLOW_OK)
    return FALSE;
  GstMessage *m = gst_bus_timed_pop_filtered(p->bus, 5 * GST_SECOND,
      GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  if (!m) return FALSE;
  gboolean ok = GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
  if (!ok) {
    GError *error = NULL; gchar *debug = NULL;
    gst_message_parse_error(m, &error, &debug);
    g_printerr("pipeline EOS: %s\n", error ? error->message : "unknown");
    g_clear_error(&error); g_free(debug);
  }
  gst_message_unref(m);
  return ok && gst_app_sink_is_eos(GST_APP_SINK(p->sink));
}

static gboolean flush_segment(Pipeline *p)
{
  GstPad *pad = gst_element_get_static_pad(p->source, "src");
  if (!pad) return FALSE;
  GstSegment segment; gst_segment_init(&segment, GST_FORMAT_TIME);
  gboolean ok = gst_pad_push_event(pad, gst_event_new_flush_start());
  ok = gst_pad_push_event(pad, gst_event_new_flush_stop(TRUE)) && ok;
  ok = gst_pad_push_event(pad, gst_event_new_segment(&segment)) && ok;
  gst_object_unref(pad);
  return ok;
}

/* Inserted below from the unchanged project-owned compare-v3 corpus. */
static GstBuffer *make_input(guint n)
{
  GstBuffer *buffer = gst_buffer_new_allocate(NULL,
      (gsize)IW * IH * 2, NULL);
  GstMapInfo map = GST_MAP_INFO_INIT;
  if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
    if (buffer) gst_buffer_unref(buffer);
    return NULL;
  }
  /* First6 frames retain the previous variedYUV formula; next3 use a
   * holdout seed; final3 exercise byte extremes. */
  for (guint y = 0; y < IH; y++) for (guint x = 0; x < IW; x += 2) {
    gsize i = ((gsize)y * IW + x) * 2;
    guint u = 16 + (13 * x + 7 * y + 23 * n + (x ^ y)) % 225;
    guint v = 16 + (11 * x + 31 * y + 29 * n + (x * y) % 53) % 225;
    map.data[i] = 16 + (37 * x + 19 * y + 17 * n + (x * y) % 97) % 220;
    map.data[i + 1] = (guint8)u;
    map.data[i + 2] = 16 + (37 * (x + 1) + 19 * y + 17 * n +
        ((x + 1) * y) % 97) % 220;
    map.data[i + 3] = (guint8)v;
    if (n >= 6 && n < 9) {
      guint32 seed = 0x9e3779b9U ^ (n * 0x85ebca6bU) ^
          (y * 0xc2b2ae35U) ^ (x * 0x27d4eb2dU);
      for (guint k = 0; k < 4; k++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        map.data[i + k] = (guint8)(seed >> 16);
      }
    } else if (n == 9) {
      map.data[i] = 0; map.data[i + 1] = 0;
      map.data[i + 2] = 255; map.data[i + 3] = 255;
    } else if (n == 10) {
      map.data[i] = 255; map.data[i + 1] = 255;
      map.data[i + 2] = 0; map.data[i + 3] = 0;
    } else if (n == 11) {
      guint8 bit = ((x / 2 + y) & 1) ? 255 : 0;
      map.data[i] = bit; map.data[i + 1] = (guint8)(255 - bit);
      map.data[i + 2] = (guint8)(255 - bit); map.data[i + 3] = bit;
    }
  }
  gst_buffer_unmap(buffer, &map);
  GST_BUFFER_PTS(buffer) = input_pts(n);
  GST_BUFFER_DTS(buffer) = input_pts(n);
  GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
  GST_BUFFER_OFFSET(buffer) = n;
  GST_BUFFER_OFFSET_END(buffer) = n + 1;
  if (n == 0) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
  return buffer;
}


static gboolean same_metadata(GstBuffer *a, GstBuffer *b)
{
  return GST_BUFFER_PTS(a) == GST_BUFFER_PTS(b) &&
      GST_BUFFER_DTS(a) == GST_BUFFER_DTS(b) &&
      GST_BUFFER_DURATION(a) == GST_BUFFER_DURATION(b) &&
      GST_BUFFER_OFFSET(a) == GST_BUFFER_OFFSET(b) &&
      GST_BUFFER_OFFSET_END(a) == GST_BUFFER_OFFSET_END(b) &&
      GST_BUFFER_FLAGS(a) == GST_BUFFER_FLAGS(b);
}

static gboolean sample_map(GstSample *sample, GstVideoFrame *frame)
{
  GstVideoInfo info; gst_video_info_init(&info);
  return sample && gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
      info.width == OW && info.height == OH &&
      GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_UYVY &&
      gst_video_frame_map(frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ);
}

static gboolean equal_pixels(const GstVideoFrame *a, const GstVideoFrame *b)
{
  const guint8 *ap = GST_VIDEO_FRAME_PLANE_DATA(a, 0);
  const guint8 *bp = GST_VIDEO_FRAME_PLANE_DATA(b, 0);
  gint as = GST_VIDEO_FRAME_PLANE_STRIDE(a, 0), bs = GST_VIDEO_FRAME_PLANE_STRIDE(b, 0);
  if (as < OW * 2 || bs < OW * 2) return FALSE;
  for (guint y = 0; y < OH; y++)
    if (memcmp(ap + (gsize)y * as, bp + (gsize)y * bs, OW * 2)) return FALSE;
  return TRUE;
}

static gboolean padded_engine(CrystalHdPackedScaleEngine *engine,
    GstBuffer *input, const GstVideoInfo *input_info, const GstVideoInfo *output_info,
    const GstVideoFrame *oracle)
{
  const guint offset = 32, input_stride = IW * 2 + 64, output_stride = OW * 2 + 64;
  gsize input_bytes = offset + (gsize)input_stride * IH + 32;
  gsize output_bytes = offset + (gsize)output_stride * OH + 32;
  GstBuffer *in = gst_buffer_new_allocate(NULL, input_bytes, NULL);
  GstBuffer *out = gst_buffer_new_allocate(NULL, output_bytes, NULL);
  GstMapInfo im = GST_MAP_INFO_INIT, om = GST_MAP_INFO_INIT, tight = GST_MAP_INFO_INIT;
  GstVideoFrame fi = {0}, fo = {0};
  gboolean mapped_i = FALSE, mapped_o = FALSE, ok = FALSE;
  if (!in || !out || !gst_buffer_map(in, &im, GST_MAP_WRITE) ||
      !gst_buffer_map(out, &om, GST_MAP_WRITE) ||
      !gst_buffer_map(input, &tight, GST_MAP_READ)) goto done;
  memset(im.data, 0xa5, im.size); memset(om.data, 0xa5, om.size);
  for (guint y = 0; y < IH; y++)
    memcpy(im.data + offset + (gsize)y * input_stride,
        tight.data + (gsize)y * IW * 2, IW * 2);
  gst_buffer_unmap(input, &tight); tight.data = NULL;
  gst_buffer_unmap(in, &im); im.data = NULL;
  gst_buffer_unmap(out, &om); om.data = NULL;
  gsize offsets[GST_VIDEO_MAX_PLANES] = {offset};
  gint strides[GST_VIDEO_MAX_PLANES] = {(gint)input_stride};
  if (!gst_buffer_add_video_meta_full(in, GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_FORMAT_YUY2, IW, IH, 1, offsets, strides)) goto done;
  strides[0] = (gint)output_stride;
  if (!gst_buffer_add_video_meta_full(out, GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_FORMAT_UYVY, OW, OH, 1, offsets, strides)) goto done;
  mapped_i = gst_video_frame_map(&fi, input_info, in, GST_MAP_READ);
  mapped_o = gst_video_frame_map(&fo, output_info, out, GST_MAP_READWRITE);
  if (!mapped_i || !mapped_o) goto done;
  GError *error = NULL;
  ok = crystalhd_packed_scale_engine_run(engine, &fi, &fo, &error) &&
      equal_pixels(oracle, &fo);
  if (error) { g_printerr("padded engine: %s\n", error->message); g_clear_error(&error); }
  gst_video_frame_unmap(&fi); mapped_i = FALSE;
  gst_video_frame_unmap(&fo); mapped_o = FALSE;
  if (!ok || !gst_buffer_map(out, &om, GST_MAP_READ)) { ok = FALSE; goto done; }
  for (gsize i = 0; i < om.size; i++) {
    gboolean active = i >= offset && i < offset + (gsize)output_stride * OH &&
        (i - offset) % output_stride < OW * 2;
    if (!active && om.data[i] != 0xa5) { ok = FALSE; break; }
  }
done:
  if (mapped_i) gst_video_frame_unmap(&fi);
  if (mapped_o) gst_video_frame_unmap(&fo);
  if (tight.data) gst_buffer_unmap(input, &tight);
  if (im.data) gst_buffer_unmap(in, &im);
  if (om.data) gst_buffer_unmap(out, &om);
  gst_clear_buffer(&in); gst_clear_buffer(&out);
  return ok;
}

static gboolean expected_rejection(gboolean spatial_metadata)
{
  Pipeline p = {0};
  gboolean ok = FALSE;
  if (!pipeline_open(&p, TRUE)) goto done;
  if (!spatial_metadata) {
    GstCaps *caps = gst_caps_from_string("video/x-raw,format=YUY2,width=640,height=720,"
        "pixel-aspect-ratio=1/1,framerate=50/1,interlace-mode=progressive,colorimetry=2:0:0:0");
    gst_app_src_set_caps(GST_APP_SRC(p.source), caps);
    gst_caps_unref(caps);
  }
  GstBuffer *input = make_input(0);
  if (!input) goto done;
  if (spatial_metadata) {
    GstVideoCropMeta *crop = gst_buffer_add_video_crop_meta(input);
    if (!crop) { gst_buffer_unref(input); goto done; }
    crop->x = 2; crop->y = 0; crop->width = IW - 2; crop->height = IH;
  }
  if (gst_element_set_state(p.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    gst_buffer_unref(input); goto done;
  }
  GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(p.source), input);
  if (flow != GST_FLOW_OK) goto done;
  GstMessage *message = gst_bus_timed_pop_filtered(p.bus, 5 * GST_SECOND, GST_MESSAGE_ERROR);
  GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(p.sink), 0);
  ok = message != NULL && sample == NULL;
  if (message) gst_message_unref(message);
  if (sample) gst_sample_unref(sample);
done:
  g_print("{\"expected_rejection\":\"%s\",\"ok\":%s}\n",
      spatial_metadata ? "unqualified_crop_metadata" : "unsupported_input_caps",
      ok ? "true" : "false");
  pipeline_close(&p);
  return ok;
}

int main(int argc, char **argv)
{
  if (argc != 2 || strcmp(argv[1], "--execute")) {
    puts("Inert finite CPU packed-filter oracle; explicit --execute required.");
    return 0;
  }
  argc = 1; argv[1] = NULL; gst_init(&argc, &argv);
  GstElement *registered = gst_element_factory_make("crystalhdpackedscale", NULL);
  if (!registered) {
    g_printerr("own packed-scale plugin did not load\n");
    gst_deinit();
    return 1;
  }
  gst_object_unref(registered);
  Pipeline stock = {0}, candidate = {0};
  GstVideoInfo input_info, output_info;
  GstCaps *input_caps = gst_caps_from_string(INPUT_CAPS),
      *output_caps = gst_caps_from_string(OUTPUT_CAPS);
  CrystalHdPackedScaleEngine *engine = NULL;
  GError *error = NULL;
  gboolean ok = gst_video_info_from_caps(&input_info, input_caps) &&
      gst_video_info_from_caps(&output_info, output_caps);
  gst_caps_unref(input_caps); gst_caps_unref(output_caps);
  guint major, minor, micro, nano;
  gst_version(&major, &minor, &micro, &nano);
  if (major != 1 || minor != 28 || micro != 2 || nano != 0) {
    engine = crystalhd_packed_scale_engine_new(&input_info, &output_info, &error);
    ok = ok && engine == NULL && error != NULL;
    g_print("{\"unsupported_runtime_rejected\":%s,\"pixel_oracle_executed\":false}\n",
        ok ? "true" : "false");
    goto cleanup;
  }
  if (!ok || !pipeline_open(&stock, FALSE) || !pipeline_open(&candidate, TRUE)) goto done;
  engine = crystalhd_packed_scale_engine_new(&input_info, &output_info, &error);
  if (!engine) goto done;
  guint compared = 0;
  for (guint cycle = 0; cycle < 3; cycle++) {
    if (gst_element_set_state(stock.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE ||
        gst_element_set_state(candidate.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) goto done;
    for (guint n = 0; n < FRAMES; n++) {
      GstBuffer *input = make_input(n);
      if (!input) goto done;
      if (gst_app_src_push_buffer(GST_APP_SRC(stock.source), gst_buffer_ref(input)) != GST_FLOW_OK ||
          gst_app_src_push_buffer(GST_APP_SRC(candidate.source), gst_buffer_ref(input)) != GST_FLOW_OK) {
        gst_buffer_unref(input); goto done;
      }
      GstSample *a = gst_app_sink_try_pull_sample(GST_APP_SINK(stock.sink), 5 * GST_SECOND);
      GstSample *b = gst_app_sink_try_pull_sample(GST_APP_SINK(candidate.sink), 5 * GST_SECOND);
      GstVideoFrame af = {0}, bf = {0};
      gboolean am = sample_map(a, &af), bm = sample_map(b, &bf);
      gboolean frame_ok = am && bm && same_metadata(gst_sample_get_buffer(a), gst_sample_get_buffer(b)) &&
          same_metadata(input, gst_sample_get_buffer(b)) && equal_pixels(&af, &bf) &&
          padded_engine(engine, input, &input_info, &output_info, &af);
      if (am) gst_video_frame_unmap(&af);
      if (bm) gst_video_frame_unmap(&bf);
      if (a) gst_sample_unref(a);
      if (b) gst_sample_unref(b);
      gst_buffer_unref(input);
      g_print("{\"cycle\":%u,\"frame\":%u,\"oracle_pixels_metadata_padded_guard\":%s}\n",
          cycle, n, frame_ok ? "true" : "false");
      if (!frame_ok) goto done;
      compared++;
      if (n == 5 && (!flush_segment(&stock) || !flush_segment(&candidate))) goto done;
    }
    if (!pipeline_eos(&stock) || !pipeline_eos(&candidate)) goto done;
    gst_element_set_state(stock.pipeline, GST_STATE_NULL);
    gst_element_set_state(candidate.pipeline, GST_STATE_NULL);
  }
  ok = compared == 3 * FRAMES && expected_rejection(FALSE) && expected_rejection(TRUE);
  g_print("{\"qualified\":%s,\"compared\":%u,\"cycles\":3,\"scope\":\"CPU_pixels_metadata_flush_EOS_restart_padded_engine_only\"}\n",
      ok ? "true" : "false", compared);
  goto cleanup;
done:
  ok = FALSE;
  g_printerr("finite packed filter qualification failed%s%s\n", error ? ": " : "", error ? error->message : "");
cleanup:
  g_clear_error(&error);
  crystalhd_packed_scale_engine_free(engine);
  pipeline_close(&stock); pipeline_close(&candidate);
  gst_deinit();
  return ok ? 0 : 1;
}
