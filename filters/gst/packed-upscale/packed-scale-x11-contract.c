/* SPDX-License-Identifier: MIT
 * Source-only finite X11 allocation/retirement control. Root sole operator.
 * Explicit --execute-x11 required. No decoder, device or input media is used.
 * sync=false is ONLY an unclocked component ownership control: this program
 * supplies no clocked playback, performance, thermal, pixel or FHD acceptance.
 */
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <stdio.h>
#include <string.h>

enum { INPUT_WIDTH = 1280, INPUT_HEIGHT = 720, FRAME_COUNT = 24 };
typedef struct {
  gint errors, warnings, eos;
} BusCounts;
typedef struct {
  guint count;
  gboolean invalid;
  gboolean require_output_meta;
} StageCount;
typedef struct {
  guint64 submitted, acknowledged, video, redraw, terminal, copied, pool;
  guint64 allocated, freed;
  guint pending, images, bytes;
  gboolean failed, closed, quarantined;
} XvStats;
typedef struct {
  gboolean qos, sync;
  gint64 lateness;
  guint64 deadline;
} Policy;

static GstBusSyncReply count_bus(GstBus *bus, GstMessage *message, gpointer data)
{
  (void)bus;
  BusCounts *counts = data;
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR ||
      GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
    gboolean error_message = GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR;
    GError *error = NULL; gchar *debug = NULL;
    if (error_message) {
      g_atomic_int_inc(&counts->errors);
      gst_message_parse_error(message, &error, &debug);
    } else {
      g_atomic_int_inc(&counts->warnings);
      gst_message_parse_warning(message, &error, &debug);
    }
    g_printerr("X11 allocation control %s: %s (%s)\n",
        error_message ? "error" : "warning", error ? error->message : "unknown",
        debug ? debug : "");
    g_clear_error(&error); g_free(debug);
  } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
    g_atomic_int_inc(&counts->eos);
  }
  return GST_BUS_PASS;
}

static GstPadProbeReturn count_stage(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  (void)pad;
  StageCount *stage = data;
  GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buffer) { stage->invalid = TRUE; return GST_PAD_PROBE_OK; }
  guint ordinal = stage->count++;
  if (ordinal >= FRAME_COUNT || GST_BUFFER_PTS(buffer) != ordinal * 20 * GST_MSECOND ||
      GST_BUFFER_DURATION(buffer) != 20 * GST_MSECOND) stage->invalid = TRUE;
  if (stage->require_output_meta) {
    GstVideoMeta *meta = gst_buffer_get_video_meta(buffer);
    if (!meta || meta->format != GST_VIDEO_FORMAT_UYVY ||
        meta->width != 1920 || meta->height != 1080 || meta->n_planes != 1 ||
        meta->stride[0] < 1920 * 2) stage->invalid = TRUE;
  }
  return GST_PAD_PROBE_OK;
}

static gboolean attach_stage(GstElement *element, const gchar *pad_name, StageCount *count)
{
  GstPad *pad = gst_element_get_static_pad(element, pad_name);
  if (!pad) return FALSE;
  gulong id = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
      count_stage, count, NULL);
  gst_object_unref(pad);
  return id != 0;
}

static void read_policy(GstElement *sink, Policy *policy)
{
  g_object_get(sink, "qos", &policy->qos, "sync", &policy->sync,
      "max-lateness", &policy->lateness, "processing-deadline", &policy->deadline,
      NULL);
}

static gboolean same_policy(const Policy *a, const Policy *b)
{
  return a->qos == b->qos && a->sync == b->sync &&
      a->lateness == b->lateness && a->deadline == b->deadline;
}

static gboolean read_stats(GstElement *sink, XvStats *out, const gchar *phase)
{
  GstStructure *s = NULL;
  g_object_get(sink, "xv-stats", &s, NULL);
  if (!s) {
    g_printerr("Missing Xv statistics at %s\n", phase);
    return FALSE;
  }
  gboolean valid = gst_structure_get_uint64(s, "submitted", &out->submitted) &&
      gst_structure_get_uint64(s, "acknowledged", &out->acknowledged) &&
      gst_structure_get_uint64(s, "video-submitted", &out->video) &&
      gst_structure_get_uint64(s, "redraw-submitted", &out->redraw) &&
      gst_structure_get_uint64(s, "terminal-retired", &out->terminal) &&
      gst_structure_get_uint64(s, "copied-bytes", &out->copied) &&
      gst_structure_get_uint64(s, "pool-submitted", &out->pool) &&
      gst_structure_get_uint64(s, "allocated", &out->allocated) &&
      gst_structure_get_uint64(s, "freed", &out->freed) &&
      gst_structure_get_uint(s, "pending", &out->pending) &&
      gst_structure_get_uint(s, "images", &out->images) &&
      gst_structure_get_uint(s, "bytes", &out->bytes) &&
      gst_structure_get_boolean(s, "failed", &out->failed) &&
      gst_structure_get_boolean(s, "closed", &out->closed) &&
      gst_structure_get_boolean(s, "quarantined", &out->quarantined);
  g_print("{\"phase\":\"%s\",\"stats_complete\":%s,\"submitted\":%" G_GUINT64_FORMAT
      ",\"acknowledged\":%" G_GUINT64_FORMAT ",\"video_submitted\":%" G_GUINT64_FORMAT
      ",\"redraw_submitted\":%" G_GUINT64_FORMAT ",\"terminal_retired\":%" G_GUINT64_FORMAT
      ",\"pool_submitted\":%" G_GUINT64_FORMAT ",\"copied_bytes\":%" G_GUINT64_FORMAT
      ",\"allocated\":%" G_GUINT64_FORMAT ",\"freed\":%" G_GUINT64_FORMAT
      ",\"pending\":%u,\"images\":%u,\"bytes\":%u,\"failed\":%s,\"closed\":%s,\"quarantined\":%s}\n",
      phase, valid ? "true" : "false", out->submitted, out->acknowledged,
      out->video, out->redraw, out->terminal, out->pool, out->copied,
      out->allocated, out->freed, out->pending, out->images, out->bytes,
      out->failed ? "true" : "false", out->closed ? "true" : "false",
      out->quarantined ? "true" : "false");
  gst_structure_free(s);
  return valid;
}

static GstBuffer *make_input(guint n)
{
  GstBuffer *buffer = gst_buffer_new_allocate(NULL,
      (gsize)INPUT_WIDTH * INPUT_HEIGHT * 2, NULL);
  GstMapInfo map = GST_MAP_INFO_INIT;
  if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
    gst_clear_buffer(&buffer); return NULL;
  }
  for (guint y = 0; y < INPUT_HEIGHT; y++)
    for (guint x = 0; x < INPUT_WIDTH; x += 2) {
      gsize i = ((gsize)y * INPUT_WIDTH + x) * 2;
      map.data[i] = 16 + (x + y + n * 7) % 220;
      map.data[i + 1] = 16 + (x * 3 + y + n * 11) % 225;
      map.data[i + 2] = 16 + (x + y * 3 + n * 13) % 220;
      map.data[i + 3] = 16 + (x + y * 5 + n * 17) % 225;
    }
  gst_buffer_unmap(buffer, &map);
  GST_BUFFER_PTS(buffer) = n * 20 * GST_MSECOND;
  GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
  GST_BUFFER_OFFSET(buffer) = n;
  return buffer;
}

int main(int argc, char **argv)
{
  if (argc != 2 || strcmp(argv[1], "--execute-x11")) {
    puts("Inert X11 allocation/retirement control. Explicit --execute-x11 opens one temporary owned window.");
    return 0;
  }
  argc = 1; argv[1] = NULL; gst_init(&argc, &argv);
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch(
      "appsrc name=input format=time is-live=false "
      "caps=video/x-raw,format=YUY2,width=1280,height=720,pixel-aspect-ratio=1/1,"
      "framerate=50/1,interlace-mode=progressive,colorimetry=2:0:0:0 "
      "! crystalhdpackedscale name=packed "
      "! video/x-raw,format=UYVY,width=1920,height=1080,pixel-aspect-ratio=1/1 "
      "! crystalhdxvsink name=output sync=false", &error);
  GstElement *source = NULL, *packed = NULL, *sink = NULL;
  GstBus *bus = NULL;
  BusCounts counts = {0};
  StageCount stages[3] = {{0}, {0}, {0}};
  XvStats eos = {0}, closed = {0};
  Policy initial = {0}, final = {0};
  gboolean setup = FALSE, natural_eos = FALSE, eos_stats = FALSE,
      closed_stats = FALSE, closed_state = FALSE, accepted = FALSE;
  guint64 transformed = 0;
  gint method = -1; guint threads = 0; gboolean simd = FALSE;
  if (error || !pipeline) goto cleanup;
  source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
  packed = gst_bin_get_by_name(GST_BIN(pipeline), "packed");
  sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
  bus = gst_element_get_bus(pipeline);
  if (!source || !packed || !sink || !bus) goto cleanup;
  gst_bus_set_sync_handler(bus, count_bus, &counts, NULL);
  stages[1].require_output_meta = TRUE;
  stages[2].require_output_meta = TRUE;
  if (!attach_stage(source, "src", &stages[0]) ||
      !attach_stage(packed, "src", &stages[1]) ||
      !attach_stage(sink, "sink", &stages[2])) goto cleanup;
  read_policy(sink, &initial);
  /* Only sync=false is assigned above. QoS, max-lateness, processing-deadline,
   * normal preroll, all queue settings, Xv ring and pool limits are untouched. */
  if (!initial.qos || initial.sync) goto cleanup;
  setup = TRUE;
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    goto cleanup;
  for (guint n = 0; n < FRAME_COUNT; n++) {
    GstBuffer *buffer = make_input(n);
    if (!buffer || gst_app_src_push_buffer(GST_APP_SRC(source), buffer) != GST_FLOW_OK)
      goto cleanup;
  }
  if (gst_app_src_end_of_stream(GST_APP_SRC(source)) != GST_FLOW_OK) goto cleanup;
  GstMessage *message = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND,
      GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  natural_eos = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
  if (message) gst_message_unref(message);
  eos_stats = read_stats(sink, &eos, "natural-eos-before-null");
  read_policy(sink, &final);
  g_object_get(packed, "frames", &transformed, "method", &method,
      "n-threads", &threads, "simd-active", &simd, NULL);
cleanup:
  if (error) { g_printerr("X11 allocation control setup: %s\n", error->message); g_clear_error(&error); }
  if (pipeline) {
    GstStateChangeReturn changed = gst_element_set_state(pipeline, GST_STATE_NULL);
    GstState state = GST_STATE_VOID_PENDING, pending = GST_STATE_VOID_PENDING;
    GstStateChangeReturn waited = gst_element_get_state(pipeline, &state, &pending, 5 * GST_SECOND);
    closed_state = changed != GST_STATE_CHANGE_FAILURE && waited == GST_STATE_CHANGE_SUCCESS &&
        state == GST_STATE_NULL && pending == GST_STATE_VOID_PENDING;
  }
  if (sink) closed_stats = read_stats(sink, &closed, "after-null");
  if (bus) gst_bus_set_sync_handler(bus, NULL, NULL, NULL);
  gboolean grids = TRUE;
  for (guint i = 0; i < 3; i++)
    if (stages[i].count != FRAME_COUNT || stages[i].invalid) grids = FALSE;
  accepted = setup && natural_eos && eos_stats && closed_stats && closed_state &&
      g_atomic_int_get(&counts.errors) == 0 && g_atomic_int_get(&counts.warnings) == 0 &&
      g_atomic_int_get(&counts.eos) == 1 && same_policy(&initial, &final) &&
      grids && transformed == FRAME_COUNT && method == 4 && threads == 2 &&
      (eos.video == FRAME_COUNT || eos.video == FRAME_COUNT + 1) &&
      eos.submitted == eos.video + eos.redraw && eos.pool == eos.submitted &&
      eos.acknowledged == eos.submitted && eos.pending == 0 && eos.terminal == 0 &&
      eos.copied == 0 && eos.allocated == 6 && !eos.failed && !eos.quarantined &&
      closed.video == eos.video && closed.submitted == closed.video + closed.redraw &&
      closed.pool == closed.submitted && closed.acknowledged == closed.submitted &&
      closed.copied == 0 && closed.terminal == 0 && closed.pending == 0 &&
      closed.images == 0 && closed.bytes == 0 && closed.allocated == 6 && closed.freed == 6 &&
      closed.closed && !closed.failed && !closed.quarantined;
  g_print("{\"accepted\":%s,\"scope\":\"UNCLOCKED_X11_COMPONENT_ALLOCATION_RETIREMENT_ONLY\","
      "\"input_frames\":%u,\"source_buffers\":%u,\"fused_buffers\":%u,\"sink_buffers\":%u,"
      "\"stage_grid_valid\":%s,\"transformed\":%" G_GUINT64_FORMAT ",\"method\":%d,"
      "\"n_threads\":%u,\"simd_active\":%s,\"natural_eos\":%s,\"null_closed\":%s,"
      "\"bus_errors\":%d,\"bus_warnings\":%d,\"bus_eos\":%d,\"sync\":false,"
      "\"qos\":%s,\"max_lateness_ns\":%" G_GINT64_FORMAT ",\"processing_deadline_ns\":%" G_GUINT64_FORMAT
      ",\"policy_unchanged\":%s,\"clocked_media_acceptance\":false,\"pixel_acceptance\":false}\n",
      accepted ? "true" : "false", FRAME_COUNT, stages[0].count, stages[1].count,
      stages[2].count, grids ? "true" : "false", transformed, method, threads,
      simd ? "true" : "false", natural_eos ? "true" : "false", closed_state ? "true" : "false",
      g_atomic_int_get(&counts.errors), g_atomic_int_get(&counts.warnings), g_atomic_int_get(&counts.eos),
      initial.qos ? "true" : "false", initial.lateness, initial.deadline,
      same_policy(&initial, &final) ? "true" : "false");
  gst_clear_object(&source); gst_clear_object(&packed); gst_clear_object(&sink);
  gst_clear_object(&bus); gst_clear_object(&pipeline);
  gst_deinit();
  return accepted ? 0 : 1;
}
