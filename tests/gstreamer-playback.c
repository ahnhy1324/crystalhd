/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <gst/gst.h>
#include <gst/video/video.h>
#include <string.h>
#include "phase1-progress.h"

#define MAX_EPOCHS 8U
typedef enum { FIELD_ANY, FIELD_PROGRESSIVE, FIELD_TFF, FIELD_BFF } FieldOrder;
typedef struct {
  guint width, height, frames;
  FieldOrder field_order;
} GeometryEpoch;

typedef struct {
  guint frames;
  gboolean invalid_output;
  GstClockTime previous_pts;
  gboolean require_timestamps;
  GChecksum *checksum;
  GChecksum *metadata_checksum;
  gboolean have_last_good;
  guint last_good_frame;
  GstClockTime last_good_pts;
  guint pass;
  Phase1Progress progress;
  const GeometryEpoch *epochs;
  guint epoch_count, epoch_index, epoch_frames;
  gboolean soft_rff;
} PlaybackAudit;

static guint32
canonical_field_flags(GstBufferFlags flags)
{
  guint32 result = 0;

  if (flags & GST_VIDEO_BUFFER_FLAG_INTERLACED)
    result |= 1U << 0;
  if (flags & GST_VIDEO_BUFFER_FLAG_TFF)
    result |= 1U << 1;
  if (flags & GST_VIDEO_BUFFER_FLAG_RFF)
    result |= 1U << 2;
  if (flags & GST_VIDEO_BUFFER_FLAG_ONEFIELD)
    result |= 1U << 3;
  return result;
}

static guint32
canonical_interlace_mode(GstVideoInterlaceMode mode)
{
  switch (mode) {
    case GST_VIDEO_INTERLACE_MODE_PROGRESSIVE: return 0;
    case GST_VIDEO_INTERLACE_MODE_INTERLEAVED: return 1;
    case GST_VIDEO_INTERLACE_MODE_MIXED: return 2;
    case GST_VIDEO_INTERLACE_MODE_FIELDS: return 3;
    case GST_VIDEO_INTERLACE_MODE_ALTERNATE: return 4;
    default: return G_MAXUINT32;
  }
}

static void
checksum_u32_be(GChecksum *checksum, guint32 value)
{
  const guchar bytes[] = {
    (guchar)(value >> 24), (guchar)(value >> 16),
    (guchar)(value >> 8), (guchar)value
  };

  g_checksum_update(checksum, bytes, sizeof(bytes));
}

static void
checksum_u64_be(GChecksum *checksum, guint64 value)
{
  const guchar bytes[] = {
    (guchar)(value >> 56), (guchar)(value >> 48),
    (guchar)(value >> 40), (guchar)(value >> 32),
    (guchar)(value >> 24), (guchar)(value >> 16),
    (guchar)(value >> 8), (guchar)value
  };

  g_checksum_update(checksum, bytes, sizeof(bytes));
}

static void
checksum_frame_metadata(GChecksum *checksum, GstClockTime pts,
                        GstClockTime duration,
                        const GstVideoInfo *info, GstBufferFlags flags)
{
  /* Stable oracle record: version, exact nanosecond PTS/duration (NONE is
   * UINT64_MAX), geometry, frame rate, canonical interlace mode and field
   * flags. */
  static const guchar version[] = {'C', 'H', 'M', 'D', 2};

  g_checksum_update(checksum, version, sizeof(version));
  checksum_u64_be(checksum, pts);
  checksum_u64_be(checksum, duration);
  checksum_u32_be(checksum, GST_VIDEO_INFO_WIDTH(info));
  checksum_u32_be(checksum, GST_VIDEO_INFO_HEIGHT(info));
  checksum_u32_be(checksum, GST_VIDEO_INFO_FPS_N(info));
  checksum_u32_be(checksum, GST_VIDEO_INFO_FPS_D(info));
  checksum_u32_be(checksum,
                  canonical_interlace_mode(GST_VIDEO_INFO_INTERLACE_MODE(info)));
  checksum_u32_be(checksum, canonical_field_flags(flags));
}

static void
report_last_good(const PlaybackAudit *audit)
{
  if (!audit->have_last_good) {
    g_print("LastGoodFrameIndex=none; LastGoodPTS=NONE\n");
  } else if (!GST_CLOCK_TIME_IS_VALID(audit->last_good_pts)) {
    g_print("LastGoodFrameIndex=%u; LastGoodPTS=NONE\n",
            audit->last_good_frame);
  } else {
    g_print("LastGoodFrameIndex=%u; LastGoodPTS=%" G_GUINT64_FORMAT "\n",
            audit->last_good_frame, (guint64)audit->last_good_pts);
  }
}

static gboolean
field_order_matches(FieldOrder expected, GstVideoInterlaceMode mode, guint flags)
{
  gboolean interlaced = (flags & GST_VIDEO_BUFFER_FLAG_INTERLACED) != 0;
  gboolean tff = (flags & GST_VIDEO_BUFFER_FLAG_TFF) != 0;
  gboolean interlaced_mode = mode == GST_VIDEO_INTERLACE_MODE_INTERLEAVED ||
      mode == GST_VIDEO_INTERLACE_MODE_MIXED;
  gboolean progressive_mode = mode == GST_VIDEO_INTERLACE_MODE_PROGRESSIVE ||
      mode == GST_VIDEO_INTERLACE_MODE_MIXED;
  if ((flags & GST_VIDEO_BUFFER_FLAG_ONEFIELD) || (tff && !interlaced) ||
      (interlaced && !interlaced_mode) ||
      (!interlaced && !progressive_mode))
    return FALSE; /* The probe counts full pictures, not individual fields. */
  if (expected == FIELD_ANY)
    return TRUE;
  if (expected == FIELD_PROGRESSIVE)
    return !interlaced;
  return interlaced && tff == (expected == FIELD_TFF);
}

static gboolean
soft_rff_frame_matches(guint pass, guint frame_index,
                       const GstVideoInfo *info, guint flags,
                       GstClockTime pts, GstClockTime duration,
                       GstClockTime previous_pts)
{
  static const struct {
    gboolean top_first;
    gboolean repeat_first;
  } cadence[] = {
    { TRUE, TRUE }, { FALSE, FALSE }, { FALSE, TRUE }, { TRUE, FALSE },
  };
  const guint phase = frame_index % G_N_ELEMENTS(cadence);
  const GstClockTime base =
      gst_util_uint64_scale(GST_SECOND, 1001, 30000);
  const GstClockTime expected_duration =
      cadence[phase].repeat_first && (pass != 0 || frame_index != 0)
          ? base + base / 2
          : base;
  const gboolean interlaced =
      (flags & GST_VIDEO_BUFFER_FLAG_INTERLACED) != 0;
  const gboolean top_first =
      (flags & GST_VIDEO_BUFFER_FLAG_TFF) != 0;
  const gboolean repeat_first =
      (flags & GST_VIDEO_BUFFER_FLAG_RFF) != 0;
  const gboolean one_field =
      (flags & GST_VIDEO_BUFFER_FLAG_ONEFIELD) != 0;

  return GST_VIDEO_INFO_FORMAT(info) == GST_VIDEO_FORMAT_YUY2 &&
      GST_VIDEO_INFO_INTERLACE_MODE(info) == GST_VIDEO_INTERLACE_MODE_MIXED &&
      GST_VIDEO_INFO_FPS_N(info) == 30000 &&
      GST_VIDEO_INFO_FPS_D(info) == 1001 &&
      !interlaced && !one_field &&
      top_first == cadence[phase].top_first &&
      repeat_first == cadence[phase].repeat_first &&
      GST_CLOCK_TIME_IS_VALID(pts) && duration == expected_duration &&
      (frame_index == 0
           ? pts == 0
           : GST_CLOCK_TIME_IS_VALID(previous_pts) && pts >= previous_pts);
}

static void
observe_geometry(PlaybackAudit *audit, guint width, guint height)
{
  if (audit->epoch_count == 0)
    return;
  if (audit->epoch_index >= audit->epoch_count ||
      width != audit->epochs[audit->epoch_index].width ||
      height != audit->epochs[audit->epoch_index].height) {
    audit->invalid_output = TRUE;
    return;
  }
  if (++audit->epoch_frames == audit->epochs[audit->epoch_index].frames) {
    audit->epoch_frames = 0;
    ++audit->epoch_index;
  }
}

static gboolean
geometry_complete(const PlaybackAudit *audit)
{
  return !audit->invalid_output && audit->epoch_index == audit->epoch_count &&
      audit->epoch_frames == 0;
}

static void
count_frame(GstElement *sink, GstBuffer *buffer, GstPad *pad, gpointer data)
{
  PlaybackAudit *audit = data;
  GstVideoInfo info;
  GstCaps *caps = gst_pad_get_current_caps(pad);
  GstClockTime pts = GST_BUFFER_PTS(buffer);
  GstVideoFrame frame;
  gboolean caps_valid;
  gboolean frame_valid;

  (void)sink;
  caps_valid = caps != NULL && gst_video_info_from_caps(&info, caps);
  if (caps_valid)
    checksum_frame_metadata(audit->metadata_checksum, pts,
                            GST_BUFFER_DURATION(buffer), &info,
                            GST_BUFFER_FLAGS(buffer));
  frame_valid = caps_valid &&
      GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_YUY2 &&
      gst_buffer_get_size(buffer) >= info.size &&
      (!audit->require_timestamps || GST_CLOCK_TIME_IS_VALID(pts)) &&
      (!GST_CLOCK_TIME_IS_VALID(pts) ||
       !GST_CLOCK_TIME_IS_VALID(audit->previous_pts) || pts >= audit->previous_pts);
  if (frame_valid && audit->soft_rff &&
      !soft_rff_frame_matches(audit->pass, audit->frames, &info,
                              GST_BUFFER_FLAGS(buffer), pts,
                              GST_BUFFER_DURATION(buffer),
                              audit->previous_pts)) {
    g_printerr("Soft-RFF mismatch at frame %u: caps %s %u/%u, flags 0x%x, "
               "PTS=%" G_GUINT64_FORMAT ", duration=%" G_GUINT64_FORMAT "\n",
               audit->frames,
               gst_video_interlace_mode_to_string(
                   GST_VIDEO_INFO_INTERLACE_MODE(&info)),
               GST_VIDEO_INFO_FPS_N(&info), GST_VIDEO_INFO_FPS_D(&info),
               GST_BUFFER_FLAGS(buffer), (guint64)pts,
               (guint64)GST_BUFFER_DURATION(buffer));
    frame_valid = FALSE;
  }
  if (!frame_valid)
    audit->invalid_output = TRUE;
  else if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
    frame_valid = FALSE;
    audit->invalid_output = TRUE;
  } else {
    guint row;
    const guint width = GST_VIDEO_INFO_WIDTH(&info);
    const guint height = GST_VIDEO_INFO_HEIGHT(&info);
    const gsize row_bytes = (gsize)width * 2;
    const gint stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    const guint8 *plane = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);

    if (plane == NULL || stride <= 0 || (gsize)stride < row_bytes) {
      g_printerr("Invalid YUY2 plane layout: width=%u height=%u stride=%d\n",
          width, height, stride);
      audit->invalid_output = TRUE;
      frame_valid = FALSE;
    }
    /* Check field metadata against the same epoch BEFORE its frame count
     * advances. Equal-size TFF/BFF transitions need no CAPS event. */
    if (!audit->soft_rff && audit->epoch_index < audit->epoch_count &&
        !field_order_matches(audit->epochs[audit->epoch_index].field_order,
            GST_VIDEO_INFO_INTERLACE_MODE(&info), GST_BUFFER_FLAGS(buffer))) {
      if (!audit->invalid_output)
        g_printerr("Field-order mismatch at frame %u, epoch %u: expected %s, caps %s, flags 0x%x\n",
            audit->frames, audit->epoch_index,
            audit->epochs[audit->epoch_index].field_order == FIELD_TFF ? "tff" :
            audit->epochs[audit->epoch_index].field_order == FIELD_BFF ? "bff" : "p",
            gst_video_interlace_mode_to_string(GST_VIDEO_INFO_INTERLACE_MODE(&info)),
            GST_BUFFER_FLAGS(buffer));
      audit->invalid_output = TRUE;
      frame_valid = FALSE;
    }
    observe_geometry(audit, width, height);
    if (audit->invalid_output)
      frame_valid = FALSE;
    if (frame_valid) {
      for (row = 0; row < height; row++)
        g_checksum_update(audit->checksum, plane + (gsize)row * stride,
            row_bytes);
    }
    if (frame_valid) {
      audit->have_last_good = TRUE;
      audit->last_good_frame = audit->frames;
      audit->last_good_pts = pts;
      if (GST_CLOCK_TIME_IS_VALID(pts))
        phase1_progress_write(&audit->progress,
            "probe=gstreamer-playback pass=%u frame-index=%u pts=%" G_GUINT64_FORMAT "\n",
            audit->pass, audit->last_good_frame, (guint64)pts);
      else
        phase1_progress_write(&audit->progress,
            "probe=gstreamer-playback pass=%u frame-index=%u pts=NONE\n",
            audit->pass, audit->last_good_frame);
    }
    gst_video_frame_unmap(&frame);
  }
  if (caps != NULL)
    gst_caps_unref(caps);
  audit->previous_pts = pts;
  audit->frames++;
}

static gboolean
run_pipeline(const gchar *description, const gchar *filename, guint expected,
             GstClockTime timeout, gboolean seek_replay, gboolean report,
             const GeometryEpoch *epochs, guint epoch_count,
             gboolean soft_rff)
{
  GError *error = NULL;
  PlaybackAudit audit = {0};
  GstElement *pipeline = gst_parse_launch(description, &error);
  GstElement *source;
  GstElement *sink;
  GstBus *bus;
  GstMessage *message = NULL;
  gboolean success = TRUE;
  gchar *reference_hash = NULL;
  gchar *reference_metadata_hash = NULL;
  gchar *reference_seek_metadata_hash = NULL;
  guint pass;
  /* mpegvideoparse learns the elementary stream frame duration while
   * handling the first picture. That initial RFF picture keeps the base
   * duration, while a flushing seek retains the learned rate and gives the
   * same picture its repeated-field duration. Freeze both software-oracle
   * states and require two identical post-seek passes. */
  const guint pass_count = soft_rff ? 3U : seek_replay ? 2U : 1U;
  static const gchar *const soft_rff_metadata_hash[] = {
    "6aa875916a487e66638898b0e3a862ebfb320906cf2d0face8d903f408f87521",
    "5a5bb7c1e6a0517f60c8276202a121da49479a09109267a18fb325f896ffecfc",
  };

  audit.previous_pts = GST_CLOCK_TIME_NONE;
  audit.last_good_pts = GST_CLOCK_TIME_NONE;
  if (!phase1_progress_open(&audit.progress)) {
    g_printerr("Could not open Phase 1 progress record\n");
    if (pipeline != NULL)
      gst_object_unref(pipeline);
    g_clear_error(&error);
    return FALSE;
  }
  audit.require_timestamps = soft_rff || filename == NULL ||
      strstr(description, "qtdemux") != NULL;
  audit.epochs = epochs;
  audit.epoch_count = epoch_count;
  audit.soft_rff = soft_rff;
  if (error != NULL || pipeline == NULL) {
    g_printerr("Cannot construct playback pipeline: %s\n",
               error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    if (pipeline != NULL)
      gst_object_unref(pipeline);
    phase1_progress_close(&audit.progress);
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
  audit.metadata_checksum = g_checksum_new(G_CHECKSUM_SHA256);
  bus = gst_element_get_bus(pipeline);
  for (pass = 0; pass < pass_count; pass++) {
    gboolean eos = FALSE;
    const gchar *hash;
    const gchar *metadata_hash;

    audit.pass = pass;

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
      audit.have_last_good = FALSE;
      audit.last_good_frame = 0;
      audit.last_good_pts = GST_CLOCK_TIME_NONE;
      audit.epoch_index = audit.epoch_frames = 0;
      g_checksum_reset(audit.checksum);
      g_checksum_reset(audit.metadata_checksum);
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
    metadata_hash = g_checksum_get_string(audit.metadata_checksum);
    if (report) {
      g_print("GStreamer%s decoded %u/%u YUY2 frames; EOS=%s; "
              "MetadataSHA256=%s; SHA256=%s\n",
               pass != 0 ? " seek replay" : "", audit.frames, expected,
               eos ? "yes" : "no", metadata_hash, hash);
      report_last_good(&audit);
      if (audit.invalid_output)
        g_printerr("Output contained invalid YUY2 buffers, regressing timestamps or unexpected geometry/field order\n");
      if (epoch_count != 0)
        g_print("Geometry epochs: %u/%u complete\n", audit.epoch_index, epoch_count);
    }
    success = eos && audit.frames == expected && geometry_complete(&audit);
    if (soft_rff &&
        (!audit.have_last_good ||
         audit.last_good_pts != G_GUINT64_CONSTANT(4954949901))) {
      g_printerr("Soft-RFF final PTS changed from the software oracle\n");
      success = FALSE;
    }
    if (soft_rff &&
        g_strcmp0(metadata_hash, soft_rff_metadata_hash[pass == 0 ? 0 : 1]) != 0) {
      g_printerr("Soft-RFF metadata changed from the %s software oracle\n",
                 pass == 0 ? "initial" : "post-seek");
      success = FALSE;
    }
    if (pass == 0)
      reference_hash = g_strdup(hash);
    else if (g_strcmp0(hash, reference_hash) != 0) {
      g_printerr("Decoded pixels changed after the flushing seek\n");
      success = FALSE;
    }
    if (pass == 0)
      reference_metadata_hash = g_strdup(metadata_hash);
    else if (!soft_rff &&
             g_strcmp0(metadata_hash, reference_metadata_hash) != 0) {
      g_printerr("Frame metadata changed after the flushing seek\n");
      success = FALSE;
    }
    if (soft_rff && pass == 1)
      reference_seek_metadata_hash = g_strdup(metadata_hash);
    else if (soft_rff && pass > 1 &&
             g_strcmp0(metadata_hash, reference_seek_metadata_hash) != 0) {
      g_printerr("Frame metadata changed between flushing seek replays\n");
      success = FALSE;
    }
    if (!success)
      break;
  }

  if (gst_element_set_state(pipeline, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Decoder teardown failed\n");
    success = FALSE;
  }
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  g_checksum_free(audit.checksum);
  g_checksum_free(audit.metadata_checksum);
  g_free(reference_hash);
  g_free(reference_metadata_hash);
  g_free(reference_seek_metadata_hash);
  phase1_progress_close(&audit.progress);
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

static gboolean
parse_epochs(const gchar *text, guint expected, GeometryEpoch *epochs, guint *count)
{
  gchar **parts = g_strsplit(text, ",", MAX_EPOCHS + 1);
  guint length = g_strv_length(parts);
  guint64 total = 0;
  gboolean valid = length > 0 && length <= MAX_EPOCHS;
  for (guint index = 0; valid && index < length; ++index) {
    gchar **dimensions = g_strsplit(parts[index], "x", 3);
    gchar **tail = g_strv_length(dimensions) == 2 ?
        g_strsplit(dimensions[1], ":", 4) : NULL;
    guint tail_count = tail != NULL ? g_strv_length(tail) : 0;
    epochs[index].field_order = FIELD_ANY;
    valid = tail != NULL && (tail_count == 2 || tail_count == 3) &&
        positive_number(dimensions[0], &epochs[index].width) &&
        positive_number(tail[0], &epochs[index].height) &&
        positive_number(tail[1], &epochs[index].frames) &&
        epochs[index].width <= 1920 && epochs[index].height <= 1088;
    if (valid && tail_count == 3) {
      if (g_str_equal(tail[2], "p"))
        epochs[index].field_order = FIELD_PROGRESSIVE;
      else if (g_str_equal(tail[2], "tff"))
        epochs[index].field_order = FIELD_TFF;
      else if (g_str_equal(tail[2], "bff"))
        epochs[index].field_order = FIELD_BFF;
      else
        valid = FALSE;
    }
    if (valid)
      total += epochs[index].frames;
    g_strfreev(tail);
    g_strfreev(dimensions);
  }
  g_strfreev(parts);
  if (!valid || total != expected)
    return FALSE;
  *count = length;
  return TRUE;
}

static gboolean
soft_rff_epochs_valid(const GeometryEpoch *epochs, guint count)
{
  for (guint index = 0; index < count; ++index) {
    if (epochs[index].field_order != FIELD_ANY &&
        epochs[index].field_order != FIELD_PROGRESSIVE)
      return FALSE;
  }
  return TRUE;
}

static gboolean
geometry_self_test(void)
{
  GeometryEpoch epochs[MAX_EPOCHS];
  guint count = 0;
  PlaybackAudit audit = {0};
  if (!parse_epochs("320x240:2,640x360:2", 4, epochs, &count) || count != 2 ||
      parse_epochs("320x240:3", 4, epochs, &count) ||
      parse_epochs("320x240:0", 0, epochs, &count) ||
      parse_epochs("1921x240:1", 1, epochs, &count) ||
      parse_epochs("320x240:1,", 1, epochs, &count) ||
      parse_epochs("320x240:1:2", 1, epochs, &count) ||
      parse_epochs("320x240:1:", 1, epochs, &count) ||
      parse_epochs("320x240:1:tff:extra", 1, epochs, &count) ||
      parse_epochs("320:240x1", 1, epochs, &count) ||
      parse_epochs("320:240:1", 1, epochs, &count) ||
      !parse_epochs("320x240:2,640x360:2", 4, epochs, &count))
    return FALSE;
  audit.epochs = epochs;
  audit.epoch_count = count;
  observe_geometry(&audit, 320, 240);
  observe_geometry(&audit, 320, 240);
  observe_geometry(&audit, 640, 360);
  if (geometry_complete(&audit))
    return FALSE; /* Missing final new-geometry picture. */
  observe_geometry(&audit, 640, 360);
  if (!geometry_complete(&audit))
    return FALSE;
  observe_geometry(&audit, 640, 360);
  if (!audit.invalid_output)
    return FALSE; /* Extra picture after the final epoch. */
  audit.epoch_index = audit.epoch_frames = 0;
  audit.invalid_output = FALSE;
  observe_geometry(&audit, 320, 240);
  observe_geometry(&audit, 640, 360);
  return audit.invalid_output; /* New caps cannot hide a missing old tail. */
}

static gboolean
field_self_test(void)
{
  GeometryEpoch epochs[MAX_EPOCHS];
  guint count = 0;
  const guint fields = GST_VIDEO_BUFFER_FLAG_INTERLACED;
  const guint top = fields | GST_VIDEO_BUFFER_FLAG_TFF;
  const GstVideoInterlaceMode mixed = GST_VIDEO_INTERLACE_MODE_MIXED;
  if (!parse_epochs("320x240:1:tff,320x240:1:bff,320x240:1:p", 3, epochs, &count) ||
      count != 3 || epochs[0].field_order != FIELD_TFF ||
      epochs[1].field_order != FIELD_BFF || epochs[2].field_order != FIELD_PROGRESSIVE)
    return FALSE;
  return field_order_matches(FIELD_TFF, mixed, top) &&
      field_order_matches(FIELD_BFF, mixed, fields) &&
      field_order_matches(FIELD_TFF, GST_VIDEO_INTERLACE_MODE_INTERLEAVED,
                          top) &&
      field_order_matches(FIELD_BFF, GST_VIDEO_INTERLACE_MODE_INTERLEAVED,
                          fields) &&
      field_order_matches(FIELD_ANY, mixed,
                          top | GST_VIDEO_BUFFER_FLAG_RFF) &&
      field_order_matches(FIELD_PROGRESSIVE, mixed, 0) &&
      !field_order_matches(FIELD_TFF, mixed, fields) &&
      !field_order_matches(FIELD_BFF, mixed, top) &&
      !field_order_matches(FIELD_TFF, mixed, GST_VIDEO_BUFFER_FLAG_TFF) &&
      !field_order_matches(FIELD_TFF, GST_VIDEO_INTERLACE_MODE_PROGRESSIVE, top) &&
      !field_order_matches(FIELD_TFF, mixed, top | GST_VIDEO_BUFFER_FLAG_ONEFIELD) &&
      !field_order_matches(FIELD_ANY, mixed,
                           fields | GST_VIDEO_BUFFER_FLAG_ONEFIELD) &&
      !field_order_matches(FIELD_PROGRESSIVE, mixed, fields) &&
      !field_order_matches(FIELD_PROGRESSIVE, GST_VIDEO_INTERLACE_MODE_INTERLEAVED, 0);
}

static gboolean
soft_rff_self_test(void)
{
  GstVideoInfo info;
  GstVideoInfo wrong;
  GeometryEpoch epochs[MAX_EPOCHS];
  guint epoch_count = 0;
  const guint tff = GST_VIDEO_BUFFER_FLAG_TFF;
  const guint rff = GST_VIDEO_BUFFER_FLAG_RFF;
  const GstClockTime base =
      gst_util_uint64_scale(GST_SECOND, 1001, 30000);
  const GstClockTime repeated = base + base / 2;
  GstClockTime pts[] = { 0, base, base + repeated,
                        base + repeated + base,
                        base + repeated + base + repeated };
  const GstClockTime durations[] = { base, base, repeated, base, repeated };
  const guint flags[] = { tff | rff, 0, rff, tff, tff | rff };

  gst_video_info_init(&info);
  if (!gst_video_info_set_format(&info, GST_VIDEO_FORMAT_YUY2, 720, 480))
    return FALSE;
  GST_VIDEO_INFO_FPS_N(&info) = 30000;
  GST_VIDEO_INFO_FPS_D(&info) = 1001;
  GST_VIDEO_INFO_INTERLACE_MODE(&info) = GST_VIDEO_INTERLACE_MODE_MIXED;
  for (guint frame = 0; frame < G_N_ELEMENTS(pts); ++frame) {
    if (!soft_rff_frame_matches(0, frame, &info, flags[frame], pts[frame],
                                durations[frame],
                                frame == 0 ? GST_CLOCK_TIME_NONE
                                           : pts[frame - 1]))
      return FALSE;
  }
  {
    GstClockTime current_pts = 0;
    GstClockTime previous_pts = GST_CLOCK_TIME_NONE;
    for (guint frame = 0; frame < 120; ++frame) {
      const guint cadence_flags = flags[frame % 4];
      const GstClockTime duration =
          frame != 0 && (cadence_flags & rff) ? repeated : base;
      if (frame != 0)
        current_pts += duration;
      if (!soft_rff_frame_matches(0, frame, &info, cadence_flags, current_pts,
                                  duration, previous_pts))
        return FALSE;
      previous_pts = current_pts;
    }
    /* Frozen 120-picture software/hardware oracle endpoint. */
    if (current_pts != G_GUINT64_CONSTANT(4954949901))
      return FALSE;
  }

  if (!soft_rff_frame_matches(1, 0, &info, tff | rff, 0, repeated,
                              GST_CLOCK_TIME_NONE) ||
      soft_rff_frame_matches(1, 0, &info, tff | rff, 0, base,
                             GST_CLOCK_TIME_NONE))
    return FALSE;

  if (!parse_epochs("720x480:60,720x480:60:p", 120, epochs,
                    &epoch_count) ||
      !soft_rff_epochs_valid(epochs, epoch_count) ||
      !parse_epochs("720x480:120:tff", 120, epochs, &epoch_count) ||
      soft_rff_epochs_valid(epochs, epoch_count) ||
      !parse_epochs("720x480:60:p,720x480:60:bff", 120, epochs,
                    &epoch_count) ||
      soft_rff_epochs_valid(epochs, epoch_count))
    return FALSE;

  wrong = info;
  GST_VIDEO_INFO_INTERLACE_MODE(&wrong) = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;
  if (soft_rff_frame_matches(0, 0, &wrong, tff | rff, 0, base,
                             GST_CLOCK_TIME_NONE))
    return FALSE;
  wrong = info;
  GST_VIDEO_INFO_FPS_N(&wrong) = 24000;
  if (soft_rff_frame_matches(0, 0, &wrong, tff | rff, 0, base,
                             GST_CLOCK_TIME_NONE))
    return FALSE;
  gst_video_info_set_format(&wrong, GST_VIDEO_FORMAT_I420, 720, 480);
  GST_VIDEO_INFO_FPS_N(&wrong) = 30000;
  GST_VIDEO_INFO_FPS_D(&wrong) = 1001;
  GST_VIDEO_INFO_INTERLACE_MODE(&wrong) = GST_VIDEO_INTERLACE_MODE_MIXED;
  if (soft_rff_frame_matches(0, 0, &wrong, tff | rff, 0, base,
                             GST_CLOCK_TIME_NONE))
    return FALSE;
  return !soft_rff_frame_matches(
             0, 0, &info, tff | rff, 0, repeated, GST_CLOCK_TIME_NONE) &&
      !soft_rff_frame_matches(0, 0, &info, tff | rff, 1, base,
                              GST_CLOCK_TIME_NONE) &&
      !soft_rff_frame_matches(0, 0, &info, tff | rff, GST_CLOCK_TIME_NONE,
                              base, GST_CLOCK_TIME_NONE) &&
      !soft_rff_frame_matches(0, 1, &info, 0, base - 1, base, base) &&
      !soft_rff_frame_matches(0, 2, &info, rff, base + repeated, base,
                              base) &&
      !soft_rff_frame_matches(0, 3, &info,
                              tff | GST_VIDEO_BUFFER_FLAG_INTERLACED,
                              base + repeated + base, base,
                              base + repeated) &&
      !soft_rff_frame_matches(0, 3, &info,
                              tff | GST_VIDEO_BUFFER_FLAG_ONEFIELD,
                              base + repeated + base, base,
                              base + repeated) &&
      !soft_rff_frame_matches(0, 3, &info, 0,
                              base + repeated + base, base,
                              base + repeated) &&
      !soft_rff_frame_matches(0, 4, &info, tff, pts[4], repeated, pts[3]);
}

static gboolean
metadata_self_test(void)
{
  GstVideoInfo info;
  GChecksum *first = g_checksum_new(G_CHECKSUM_SHA256);
  GChecksum *same = g_checksum_new(G_CHECKSUM_SHA256);
  GChecksum *different_pts = g_checksum_new(G_CHECKSUM_SHA256);
  GChecksum *different_duration = g_checksum_new(G_CHECKSUM_SHA256);
  GChecksum *different_rate = g_checksum_new(G_CHECKSUM_SHA256);
  GChecksum *different_fields = g_checksum_new(G_CHECKSUM_SHA256);
  GstVideoInfo different_rate_info;
  gboolean valid;
  gboolean success;

  gst_video_info_init(&info);
  valid = gst_video_info_set_format(&info, GST_VIDEO_FORMAT_YUY2, 320, 240);
  GST_VIDEO_INFO_FPS_N(&info) = 25;
  GST_VIDEO_INFO_FPS_D(&info) = 1;
  different_rate_info = info;
  GST_VIDEO_INFO_FPS_N(&different_rate_info) = 30;
  checksum_frame_metadata(first, 0, 40 * GST_MSECOND, &info, 0);
  checksum_frame_metadata(first, 40 * GST_MSECOND, 40 * GST_MSECOND, &info, 0);
  checksum_frame_metadata(same, 0, 40 * GST_MSECOND, &info, 0);
  checksum_frame_metadata(same, 40 * GST_MSECOND, 40 * GST_MSECOND, &info, 0);
  checksum_frame_metadata(different_pts, 0, 40 * GST_MSECOND, &info, 0);
  checksum_frame_metadata(different_pts, 41 * GST_MSECOND, 40 * GST_MSECOND,
                          &info, 0);
  checksum_frame_metadata(different_duration, 0, 41 * GST_MSECOND, &info, 0);
  checksum_frame_metadata(different_duration, 40 * GST_MSECOND,
                          40 * GST_MSECOND, &info, 0);
  checksum_frame_metadata(different_rate, 0, 40 * GST_MSECOND,
                          &different_rate_info, 0);
  checksum_frame_metadata(different_rate, 40 * GST_MSECOND,
                          40 * GST_MSECOND, &different_rate_info, 0);
  checksum_frame_metadata(different_fields, 0, 40 * GST_MSECOND, &info, 0);
  checksum_frame_metadata(different_fields, 40 * GST_MSECOND,
                          40 * GST_MSECOND, &info,
                          GST_VIDEO_BUFFER_FLAG_INTERLACED |
                          GST_VIDEO_BUFFER_FLAG_TFF);
  success = valid &&
      g_str_equal(g_checksum_get_string(first), g_checksum_get_string(same)) &&
      !g_str_equal(g_checksum_get_string(first),
                   g_checksum_get_string(different_pts)) &&
      !g_str_equal(g_checksum_get_string(first),
                   g_checksum_get_string(different_duration)) &&
      !g_str_equal(g_checksum_get_string(first),
                   g_checksum_get_string(different_rate)) &&
      !g_str_equal(g_checksum_get_string(first),
                   g_checksum_get_string(different_fields)) &&
      canonical_interlace_mode(GST_VIDEO_INTERLACE_MODE_PROGRESSIVE) == 0 &&
      canonical_interlace_mode(GST_VIDEO_INTERLACE_MODE_INTERLEAVED) == 1 &&
      canonical_interlace_mode(GST_VIDEO_INTERLACE_MODE_MIXED) == 2;
  g_checksum_free(first);
  g_checksum_free(same);
  g_checksum_free(different_pts);
  g_checksum_free(different_duration);
  g_checksum_free(different_rate);
  g_checksum_free(different_fields);
  return success;
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
  const gchar *mpeg4_pipeline =
      "filesrc name=source ! qtdemux name=demux "
      "demux.video_0 ! queue ! mpeg4videoparse ! "
      "crystalhddec ! fakesink name=sink";
  const gchar *test_pipeline =
      "videotestsrc num-buffers=12 ! video/x-raw,format=YUY2,width=320,height=240 ! "
      "fakesink name=sink";
  const gchar *mpeg2_pipeline =
      "filesrc name=source ! mpegvideoparse ! crystalhddec ! fakesink name=sink";
  GeometryEpoch epochs[MAX_EPOCHS];
  guint epoch_count = 0;
  gboolean seek_replay = FALSE;
  gboolean soft_rff = FALSE;
  guint expected;
  guint timeout = 120;

  gst_init(&argc, &argv);
  if (argc == 2 && g_str_equal(argv[1], "--self-test")) {
    const GeometryEpoch correct = {320, 240, 12, FIELD_PROGRESSIVE};
    const GeometryEpoch wrong = {640, 360, 12, FIELD_ANY};
    const GeometryEpoch wrong_fields = {320, 240, 12, FIELD_TFF};
    if (!geometry_self_test() || !field_self_test() || !metadata_self_test() ||
        !soft_rff_self_test() ||
        !run_pipeline(test_pipeline, NULL, 12, 5 * GST_SECOND, FALSE, TRUE, NULL, 0, FALSE) ||
        !run_pipeline(test_pipeline, NULL, 12, 5 * GST_SECOND, FALSE, TRUE, &correct, 1, FALSE) ||
        run_pipeline(test_pipeline, NULL, 12, 5 * GST_SECOND, FALSE, FALSE, &wrong, 1, FALSE) ||
        run_pipeline(test_pipeline, NULL, 12, 5 * GST_SECOND, FALSE, FALSE, &wrong_fields, 1, FALSE) ||
        run_pipeline(test_pipeline, NULL, 13, 5 * GST_SECOND, FALSE, FALSE, NULL, 0, FALSE) ||
        run_pipeline("fakesrc num-buffers=1 ! "
                     "identity sleep-time=100000 ! fakesink name=sink",
                     NULL, 1, GST_MSECOND, FALSE, FALSE, NULL, 0, FALSE)) {
      g_printerr("GStreamer frame-count/EOS self-test failed\n");
      return 1;
    }
    g_print("GStreamer frame-count/EOS self-test passed\n");
    return 0;
  }
  if ((argc < 4 || argc > 9) || !positive_number(argv[2], &expected) ||
      (argc >= 5 && !positive_number(argv[4], &timeout)) ||
      (!g_str_equal(argv[3], "mp4") && !g_str_equal(argv[3], "h264") &&
       !g_str_equal(argv[3], "mpeg2") && !g_str_equal(argv[3], "mpeg4")))
    goto usage;
  for (gint arg = 5; arg < argc; ++arg) {
    if (g_str_equal(argv[arg], "--seek") && !seek_replay)
      seek_replay = TRUE;
    else if (g_str_equal(argv[arg], "--soft-rff") && !soft_rff)
      soft_rff = TRUE;
    else if (g_str_equal(argv[arg], "--epochs") && epoch_count == 0 && arg + 1 < argc) {
      if (!parse_epochs(argv[++arg], expected, epochs, &epoch_count))
        goto usage;
    } else
      goto usage;
  }
  if (soft_rff &&
      (!g_str_equal(argv[3], "mpeg2") ||
       !soft_rff_epochs_valid(epochs, epoch_count)))
    goto usage;
  if (soft_rff)
    seek_replay = TRUE;
  return run_pipeline(g_str_equal(argv[3], "mp4") ? mp4_pipeline :
                       g_str_equal(argv[3], "h264") ? annex_b_pipeline :
                       g_str_equal(argv[3], "mpeg4") ? mpeg4_pipeline : mpeg2_pipeline,
                       argv[1], expected, (GstClockTime)timeout * GST_SECOND,
                       seek_replay, TRUE, epochs, epoch_count, soft_rff) ? 0 : 1;
usage:
  g_printerr("usage: %s VIDEO EXPECTED_FRAMES mp4|h264|mpeg2|mpeg4 "
             "[TIMEOUT_SECONDS [--seek] [--soft-rff] "
             "[--epochs WIDTHxHEIGHT:FRAMES[:p|tff|bff],...]]\n", argv[0]);
  return 2;
}
