/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <gst/gst.h>
#include <gst/video/video.h>
#include <string.h>

typedef struct {
  guint frames;
  gboolean invalid_output;
  GstClockTime previous_pts;
  gboolean require_timestamps;
  GChecksum *checksum;
} PlaybackAudit;

static void
count_frame(GstElement *sink, GstBuffer *buffer, GstPad *pad, gpointer data)
{
  PlaybackAudit *audit = data;
  GstVideoInfo info;
  GstCaps *caps = gst_pad_get_current_caps(pad);
  GstClockTime pts = GST_BUFFER_PTS(buffer);
  GstVideoFrame frame;

  (void)sink;
  if (caps == NULL || !gst_video_info_from_caps(&info, caps) ||
      GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_YUY2 ||
      gst_buffer_get_size(buffer) < info.size ||
      (audit->require_timestamps && !GST_CLOCK_TIME_IS_VALID(pts)) ||
      (GST_CLOCK_TIME_IS_VALID(pts) &&
       GST_CLOCK_TIME_IS_VALID(audit->previous_pts) && pts < audit->previous_pts))
    audit->invalid_output = TRUE;
  else if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ))
    audit->invalid_output = TRUE;
  else {
    guint row;
    for (row = 0; row < (guint)GST_VIDEO_INFO_HEIGHT(&info); row++)
      g_checksum_update(audit->checksum,
          (const guchar *)GST_VIDEO_FRAME_PLANE_DATA(&frame, 0) +
          row * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0),
          GST_VIDEO_INFO_WIDTH(&info) * 2);
    gst_video_frame_unmap(&frame);
  }
  if (caps != NULL)
    gst_caps_unref(caps);
  audit->previous_pts = pts;
  audit->frames++;
}

static gboolean
run_pipeline(const gchar *description, const gchar *filename, guint expected,
             GstClockTime timeout, gboolean seek_replay, gboolean report)
{
  GError *error = NULL;
  PlaybackAudit audit = {0, FALSE, GST_CLOCK_TIME_NONE,
                        filename == NULL || strstr(description, "qtdemux") != NULL,
                        NULL};
  GstElement *pipeline = gst_parse_launch(description, &error);
  GstElement *source;
  GstElement *sink;
  GstBus *bus;
  GstMessage *message = NULL;
  gboolean success = TRUE;
  gchar *reference_hash = NULL;
  guint pass;

  if (error != NULL || pipeline == NULL) {
    g_printerr("Cannot construct playback pipeline: %s\n",
               error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    if (pipeline != NULL)
      gst_object_unref(pipeline);
    return FALSE;
  }
  if (filename != NULL) {
    source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    g_object_set(source, "location", filename, NULL);
    gst_object_unref(source);
  }
  sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
  g_object_set(sink, "signal-handoffs", TRUE, "sync", FALSE,
               "enable-last-sample", FALSE, NULL);
  g_signal_connect(sink, "handoff", G_CALLBACK(count_frame), &audit);
  gst_object_unref(sink);

  audit.checksum = g_checksum_new(G_CHECKSUM_SHA256);
  bus = gst_element_get_bus(pipeline);
  for (pass = 0; pass < (seek_replay ? 2U : 1U); pass++) {
    gboolean eos = FALSE;
    const gchar *hash;

    if (pass != 0) {
      /* EOS has stopped output. Pause before resetting the audit so a
       * flushing seek cannot race a new handoff in the streaming thread. */
      if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE ||
          gst_element_get_state(pipeline, NULL, NULL, timeout) != GST_STATE_CHANGE_SUCCESS) {
        g_printerr("Could not pause the pipeline for seek replay\n");
        success = FALSE;
        break;
      }
      audit.frames = 0;
      audit.invalid_output = FALSE;
      audit.previous_pts = GST_CLOCK_TIME_NONE;
      g_checksum_reset(audit.checksum);
      if (!gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
                                    GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, 0)) {
        g_printerr("Pipeline rejected flushing seek to zero\n");
        success = FALSE;
        break;
      }
    }
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE)
      message = gst_bus_timed_pop_filtered(bus, timeout,
                                           GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    else
      message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);

    if (message != NULL && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
      eos = TRUE;
    } else if (message != NULL) {
      gchar *debug = NULL;
      gst_message_parse_error(message, &error, &debug);
      if (report) {
        g_printerr("Playback failed in %s: %s\n",
                   GST_OBJECT_NAME(message->src), error->message);
        if (debug != NULL)
          g_printerr("%s\n", debug);
      }
      g_clear_error(&error);
      g_free(debug);
    } else if (report) {
      g_printerr("Playback did not reach EOS within %.1f seconds\n",
                 (gdouble)timeout / GST_SECOND);
    }
    if (message != NULL) {
      gst_message_unref(message);
      message = NULL;
    }
    /* EOS is serialized after the final handoff. On error/timeout, stop the
     * streaming threads before reading the audit. */
    if (!eos)
      gst_element_set_state(pipeline, GST_STATE_NULL);
    hash = g_checksum_get_string(audit.checksum);
    if (report) {
      g_print("GStreamer%s decoded %u/%u YUY2 frames; EOS=%s; SHA256=%s\n",
               pass != 0 ? " seek replay" : "", audit.frames, expected,
               eos ? "yes" : "no", hash);
      if (audit.invalid_output)
        g_printerr("Output contained invalid YUY2 buffers or regressing timestamps\n");
    }
    success = eos && audit.frames == expected && !audit.invalid_output;
    if (pass == 0)
      reference_hash = g_strdup(hash);
    else if (g_strcmp0(hash, reference_hash) != 0) {
      g_printerr("Decoded pixels changed after the flushing seek\n");
      success = FALSE;
    }
    if (!success)
      break;
  }

  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  g_checksum_free(audit.checksum);
  g_free(reference_hash);
  return success;
}

static gboolean
positive_number(const gchar *value, guint *number)
{
  gchar *end = NULL;
  guint64 parsed = g_ascii_strtoull(value, &end, 10);
  if (*value == '\0' || *value == '-' || *end != '\0' ||
      parsed == 0 || parsed > G_MAXINT)
    return FALSE;
  *number = (guint)parsed;
  return TRUE;
}

int
main(int argc, char **argv)
{
  const gchar *mp4_pipeline =
      "filesrc name=source ! qtdemux name=demux "
      "demux.video_0 ! queue ! h264parse ! "
      "video/x-h264,stream-format=byte-stream,alignment=au ! "
      "crystalhddec ! fakesink name=sink";
  const gchar *annex_b_pipeline =
      "filesrc name=source ! h264parse ! "
      "video/x-h264,stream-format=byte-stream,alignment=au ! "
      "crystalhddec ! fakesink name=sink";
  const gchar *test_pipeline =
      "videotestsrc num-buffers=12 ! video/x-raw,format=YUY2 ! "
      "fakesink name=sink";
  guint expected;
  guint timeout = 120;

  gst_init(&argc, &argv);
  if (argc == 2 && g_str_equal(argv[1], "--self-test")) {
    if (!run_pipeline(test_pipeline, NULL, 12, 5 * GST_SECOND, FALSE, TRUE) ||
        run_pipeline(test_pipeline, NULL, 13, 5 * GST_SECOND, FALSE, FALSE) ||
        run_pipeline("fakesrc num-buffers=1 ! "
                     "identity sleep-time=100000 ! fakesink name=sink",
                     NULL, 1, GST_MSECOND, FALSE, FALSE)) {
      g_printerr("GStreamer frame-count/EOS self-test failed\n");
      return 1;
    }
    g_print("GStreamer frame-count/EOS self-test passed\n");
    return 0;
  }
  if ((argc < 4 || argc > 6) || !positive_number(argv[2], &expected) ||
      (argc >= 5 && !positive_number(argv[4], &timeout)) ||
      (argc == 6 && !g_str_equal(argv[5], "--seek")) ||
      (!g_str_equal(argv[3], "mp4") && !g_str_equal(argv[3], "h264"))) {
    g_printerr("usage: %s VIDEO EXPECTED_FRAMES mp4|h264 [TIMEOUT_SECONDS [--seek]]\n",
               argv[0]);
    return 2;
  }
  return run_pipeline(g_str_equal(argv[3], "mp4") ? mp4_pipeline : annex_b_pipeline,
                       argv[1], expected, (GstClockTime)timeout * GST_SECOND,
                       argc == 6, TRUE) ? 0 : 1;
}
