/*
 * GStreamer 1.x decoder for Broadcom Crystal HD BCM70012/BCM70015 devices.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/base/gstadapter.h>
#include <gst/video/gstvideodecoder.h>
#include <gst/video/video.h>

#include <bc_dts_defs.h>
#include <bc_dts_types.h>
#include <libcrystalhd_if.h>
#include "../../crystalhd-h264-output.h"
#include "gstcrystalhd-codecs.h"
#include "gstcrystalhd-input.h"
#include "gstcrystalhd-timing.h"
#include "gstcrystalhd-diqos.h"
#include "gstcrystalhd-stage-trace.h"
#include "gstcrystalhd-finish-trace.h"

/* Older libcrystalhd.so.3 installations need not provide this additive API. */
extern BC_STATUS DtsDupOutputPollFd(HANDLE, int *) __attribute__((weak));

G_STATIC_ASSERT(GST_CRYSTALHD_MPEG4_MAX_INPUT_SIZE ==
                GST_CRYSTALHD_INPUT_CAPACITY);

#define GST_TYPE_CRYSTALHD_DEC (gst_crystalhd_dec_get_type())
#define GST_CRYSTALHD_DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_CRYSTALHD_DEC, GstCrystalHdDec))

#define CRYSTALHD_TIMESTAMP_STEP 100000ULL
/* Legacy libcrystalhd input flag: retain queue/TX counters but do not peek
 * and synchronize the READY-head DMA buffer just to read its timestamp. */
#define CRYSTALHD_STATUS_SKIP_READY_HEAD_PEEK (1U << 30)
#define CRYSTALHD_SCALE_MIN_WIDTH 128U
#define CRYSTALHD_SCALE_MAX_WIDTH 1918U

enum {
  PROP_0,
  PROP_SCALE_WIDTH,
  PROP_FINISH_TRACE_LIMIT
};

typedef struct {
  guint64 hardware_timestamp;
  guint32 frame_number;
  GstClockTime pts;
  GstClockTime duration;
} CrystalHdTimestamp;

typedef struct _GstCrystalHdDec {
  GstVideoDecoder parent;

  HANDLE device;
  gboolean decoder_open;
  gboolean decoder_started;
  gboolean input_flushed;
  gboolean is_70012;
  gboolean output_configured;
  gboolean progressive_field_sequence;
  gboolean need_second_field;
  gboolean field_bottom;
  gboolean field_bottom_first;
  CrystalHdCodec codec;
  gsize input_metadata_size;
  guint32 field_frame_number;
  guint field_width;
  guint field_height;
  guint width;
  guint height;
  /* scale_width is the READY-mutable property.  active_scale_width is the
   * immutable snapshot used by one PAUSED/PLAYING decoder session. */
  guint scale_width;
  guint active_scale_width;
  GstCrystalHdFinishTrace *finish_trace;
  GstCrystalHdFinishTrace *active_finish_trace;
  guint64 next_hardware_timestamp;
  guint64 recovery_generation_seen;
  gboolean recovery_needs_sync;
  gboolean recovery_request_headers;
  guint32 recovery_h264_config_size;
  gint64 recovery_abort_started_us;
  guint32 simple_picture_number;
  gboolean have_simple_picture_number;
  gboolean wmv3_reordered_output;
  gboolean wmv3_timing_valid;
  GstClockTime wmv3_last_duration;
  guint mpeg2_reference_pictures;
  gboolean mpeg2_frame_reference_tracking;
  guint mpeg2_dropped_inputs;
  GQueue timestamps;
  GstVideoCodecState *input_state;
  GstAdapter *parse_adapter;
  gboolean parse_pending;
  GstVideoInfo output_info;

  /* The decoder stream lock protects the device, timestamps and flow state.
   * output_lock only protects worker scheduling and the delivery barrier;
   * never take the stream lock while holding output_lock. */
  GMutex output_lock;
  GCond output_cond;
  GThread *output_thread;
  gboolean output_stop;
  gboolean output_paused;
  gboolean output_active;
  gboolean output_waiting;
  gboolean output_wake_pending;
  gint output_control_fd;
  gint output_poll_fd;
  gboolean output_recovering;
  GstFlowReturn output_flow;
  guint64 generation;
  gboolean draining;
  gboolean output_eos;
  gboolean drain_idle;
  gboolean parsing_frame;
  gint64 last_delivery_us;
} GstCrystalHdDec;

typedef struct _GstCrystalHdDecClass {
  GstVideoDecoderClass parent_class;
} GstCrystalHdDecClass;

G_DEFINE_TYPE(GstCrystalHdDec, gst_crystalhd_dec, GST_TYPE_VIDEO_DECODER)

GST_DEBUG_CATEGORY_STATIC(gst_crystalhd_debug);
#define GST_CAT_DEFAULT gst_crystalhd_debug

#ifdef GST_CRYSTALHD_MPEG1_EXPERIMENTAL
/* Separately built, explicitly selected, rank-NONE experimental plugin. */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(
        "video/mpeg, mpegversion=(int)1, systemstream=(boolean)false, "
        "parsed=(boolean)true, width=(int)352, height=(int)288, "
        "framerate=(fraction)25/1, interlace-mode=(string)progressive"));
#elif defined(GST_CRYSTALHD_PRIVATE_MPEG1_RESEARCH)
/* Only the separately named tests/mpeg1-research-decoder.c build. */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(
        "video/mpeg, mpegversion=(int)1, systemstream=(boolean)false, "
        "parsed=(boolean)true, width=(int)[2,1920], height=(int)[2,1088], "
        "interlace-mode=(string)progressive"));
#else
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-h264, stream-format=(string)byte-stream, "
        "alignment=(string)au, parsed=(boolean)true; "
        "video/mpeg, mpegversion=(int)2, systemstream=(boolean)false, "
        "parsed=(boolean)true; "
        "video/mpeg, mpegversion=(int)4, systemstream=(boolean)false, "
        "parsed=(boolean)true, profile=(string){simple,advanced-simple}, "
        "level=(string){\"3\",\"5\"}, width=(int)[1,1920], "
        "height=(int)[1,1088]; "
        "video/x-vc1, parsed=(boolean)true; "
        "video/x-wmv, wmvversion=(int)3, format=(string)WVC1, "
        "stream-format=(string){bdu,bdu-frame}, header-format=(string)none; "
        "video/x-wmv, wmvversion=(int)3, format=(string)WVC1, "
        "stream-format=(string)asf, header-format=(string)asf; "
        "video/x-wmv, wmvversion=(int)3, format=(string)WMV3, "
        "stream-format=(string){frame-layer,asf}, header-format=(string)asf"));
#endif

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw, format=(string)YUY2, "
                    "width=(int)[1,1920], height=(int)[1,1088], "
                    "framerate=(fraction)[0/1,MAX]"));

static gboolean
gst_crystalhd_scale_width_valid(guint width)
{
  return width >= CRYSTALHD_SCALE_MIN_WIDTH &&
      width <= CRYSTALHD_SCALE_MAX_WIDTH && (width & 1) == 0;
}

static GstCaps *
gst_crystalhd_getcaps(GstVideoDecoder *decoder, GstCaps *filter)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  GstCaps *output, *peer, *allowed, *caps, *input, *result;
  guint scale_width, scan, i, j, k, mode;
  static const gchar *invariant_fields[] = {
    "framerate", "colorimetry", "chroma-site"
  };

  GST_OBJECT_LOCK(self);
  scale_width = self->scale_width;
  GST_OBJECT_UNLOCK(self);
  if (scale_width == 0)
    return gst_video_decoder_proxy_getcaps(decoder, NULL, filter);
  if (!gst_crystalhd_scale_width_valid(scale_width))
    return gst_caps_new_empty();

  /* Preserve the base proxy's non-geometric video fields and every encoded
   * template restriction. Width/height/PAR describe different surfaces here.
   * Exact output height/PAR are still negotiated from the hardware result. */
  input = gst_pad_get_pad_template_caps(GST_VIDEO_DECODER_SINK_PAD(decoder));
  input = gst_caps_make_writable(input);
  gst_caps_set_simple(input, "width", GST_TYPE_INT_RANGE, (gint)scale_width, 1920,
                       "height", GST_TYPE_INT_RANGE, 1, 1088, NULL);
  result = gst_caps_new_empty();
  for (scan = 0; scan != 2; ++scan) {
    /* Project each compatible scan branch separately: a mixed-only peer's
     * rate must not admit progressive input or another codec. Compact scaled
     * H.264 fields have only been qualified with 16-pixel-aligned pitches. */
    if (scan != 0 && (scale_width & 15U) != 0)
      continue;
    output = gst_pad_get_pad_template_caps(GST_VIDEO_DECODER_SRC_PAD(decoder));
    output = gst_caps_make_writable(output);
    gst_caps_set_simple(output, "width", G_TYPE_INT, (gint)scale_width,
        "interlace-mode", G_TYPE_STRING, scan == 0 ? "progressive" : "mixed", NULL);
    peer = gst_pad_peer_query_caps(GST_VIDEO_DECODER_SRC_PAD(decoder), output);
    if (peer == NULL) {
      gst_caps_unref(output);
      continue;
    }
    allowed = gst_caps_intersect_full(peer, output, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(peer);
    gst_caps_unref(output);
    for (i = 0; i < gst_caps_get_size(input); ++i) {
      const GstStructure *encoded = gst_caps_get_structure(input, i);
      if (scan != 0 && !gst_structure_has_name(encoded, "video/x-h264"))
        continue;
      for (j = 0; j < gst_caps_get_size(allowed); ++j) {
        const GstStructure *downstream = gst_caps_get_structure(allowed, j);
        for (mode = 0; mode < (scan == 0 ? 1U : 2U); ++mode) {
          GstStructure *upstream = gst_structure_copy(encoded);
          gst_structure_set(upstream, "interlace-mode", G_TYPE_STRING,
              scan == 0 ? "progressive" : (mode == 0 ? "interleaved" : "mixed"), NULL);
          for (k = 0; k < G_N_ELEMENTS(invariant_fields); ++k) {
            const GValue *value = gst_structure_get_value(downstream,
                invariant_fields[k]);
            if (value != NULL)
              gst_structure_set_value(upstream, invariant_fields[k], value);
          }
          gst_caps_append_structure_full(result, upstream,
              gst_caps_features_copy(gst_caps_get_features(input, i)));
        }
      }
    }
    gst_caps_unref(allowed);
  }
  gst_caps_unref(input);
  if (filter != NULL) {
    caps = gst_caps_intersect_full(filter, result, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(result);
    result = caps;
  }
  return result;
}

static gboolean
gst_crystalhd_caps_have_mpeg4_metadata(GstCaps *caps,
                                       const CrystalHdCodec *codec)
{
  const GstStructure *structure = gst_caps_get_structure(caps, 0);
  const GValue *value = gst_structure_get_value(structure, "codec_data");
  GstBuffer *buffer;
  GstMapInfo map;
  const guint8 *data;
  gsize size;
  gboolean valid;
  CrystalHdCodec checked_codec = *codec;

  if (value == NULL || !GST_VALUE_HOLDS_BUFFER(value))
    return FALSE;
  buffer = gst_value_get_buffer(value);
  if (buffer == NULL || !gst_buffer_map(buffer, &map, GST_MAP_READ))
    return FALSE;
  data = map.data;
  size = map.size;
  valid = gst_crystalhd_codec_metadata(&checked_codec, &data, &size);
  gst_buffer_unmap(buffer, &map);
  return valid;
}

static gboolean
gst_crystalhd_sink_query(GstVideoDecoder *decoder, GstQuery *query)
{
  GstVideoDecoderClass *parent =
      GST_VIDEO_DECODER_CLASS(gst_crystalhd_dec_parent_class);

  if (GST_QUERY_TYPE(query) == GST_QUERY_ACCEPT_CAPS) {
    GstCaps *caps;
    gst_query_parse_accept_caps(query, &caps);
    if (gst_caps_is_fixed(caps)) {
      CrystalHdCodec codec;

      if (!gst_crystalhd_codec_from_caps(caps, &codec)) {
        gst_query_set_accept_caps_result(query, FALSE);
        return TRUE;
      }
      if (codec.subtype == BC_MSUBTYPE_DIVX &&
          !gst_crystalhd_caps_have_mpeg4_metadata(caps, &codec)) {
        gst_query_set_accept_caps_result(query, FALSE);
        return TRUE;
      }
      if (gst_structure_has_name(gst_caps_get_structure(caps, 0),
                                 "video/x-wmv")) {
        GstCaps *normalized;
        GstStructure *s;
        GstQuery *check;
        gboolean accepted = FALSE;
        gboolean result;

        /* asfdemux omits stream-format/header-format (and older producers
         * also omit format). Those caps still describe ASF packets. Normalize
         * only this query, retaining strict advertised parser framing and the
         * original caps/codec_data for set_format.
         */
        normalized = gst_caps_copy(caps);
        s = gst_caps_get_structure(normalized, 0);
        if (!gst_structure_has_field(s, "format"))
          gst_structure_set(s, "format", G_TYPE_STRING, "WMV3", NULL);
        if (!gst_structure_has_field(s, "stream-format"))
          gst_structure_set(s, "stream-format", G_TYPE_STRING, "asf", NULL);
        if (!gst_structure_has_field(s, "header-format"))
          gst_structure_set(s, "header-format", G_TYPE_STRING,
                            codec.vc1_bdu ? "none" : "asf", NULL);
        check = gst_query_new_accept_caps(normalized);
        result = parent->sink_query(decoder, check);
        if (result)
          gst_query_parse_accept_caps_result(check, &accepted);
        gst_query_set_accept_caps_result(query, accepted);
        gst_query_unref(check);
        gst_caps_unref(normalized);
        return result;
      }
    }
  }
  return parent->sink_query(decoder, query);
}

static const gchar *
gst_crystalhd_status_hint(BC_STATUS status)
{
  switch (status) {
    case BC_STS_BUSY:
    case BC_STS_DEC_EXIST_OPEN:
      return "CrystalHD supports one playback session; close other hardware players";
    case BC_STS_NO_ACCESS:
      return "check /dev/crystalhd permissions and video-group or desktop udev access";
    case BC_STS_FWHEX_NOT_FOUND:
      return "install the matching bcm70012fw.bin or bcm70015fw.bin in /lib/firmware";
    case BC_STS_FW_AUTH_FAILED:
    case BC_STS_BOOTLOADER_FAILED:
    case BC_STS_CERT_VERIFY_ERROR:
    case BC_STS_FW_CMD_ERR:
      return "firmware startup or command failed; check the firmware and kernel log";
    case BC_STS_INV_ARG:
    case BC_STS_NOT_IMPL:
      return "check the input codec/framing and supported output format (BCM70015 uses YUY2)";
    case BC_STS_TIMEOUT:
      return "decoder timed out; check Annex-B input, YUY2 output selection and the kernel log";
    default:
      return "check module binding, /dev/crystalhd access, firmware and other playback clients";
  }
}

static void
gst_crystalhd_set_property(GObject *object, guint property_id,
                           const GValue *value, GParamSpec *spec)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(object);

  switch (property_id) {
    case PROP_FINISH_TRACE_LIMIT: {
      GstCrystalHdFinishTrace *next, *previous = NULL;
      guint capacity = g_value_get_uint(value);
      gboolean mutable;
      next = gst_crystalhd_finish_trace_new(capacity);
      if (capacity && !next) {
        GST_WARNING_OBJECT(self, "could not allocate bounded finish-frame trace");
        break;
      }
      GST_OBJECT_LOCK(self);
      mutable = GST_STATE(self) <= GST_STATE_READY &&
                GST_STATE_PENDING(self) == GST_STATE_VOID_PENDING;
      if (mutable) {
        previous = self->finish_trace;
        self->finish_trace = next;
      }
      GST_OBJECT_UNLOCK(self);
      if (!mutable) {
        gst_crystalhd_finish_trace_unref(next);
        GST_WARNING_OBJECT(self, "finish-trace-limit can only change in NULL or READY");
      } else {
        gst_crystalhd_finish_trace_unref(previous);
      }
      break;
    }
    case PROP_SCALE_WIDTH: {
      gboolean mutable;

      /* GstElement state fields are protected by the object lock.  Taking the
       * state-change lock here can deadlock a downstream callback against
       * PAUSED -> READY teardown while stop() joins the output worker. */
      GST_OBJECT_LOCK(self);
      mutable = GST_STATE(self) <= GST_STATE_READY &&
                GST_STATE_PENDING(self) == GST_STATE_VOID_PENDING;
      if (mutable)
        self->scale_width = g_value_get_uint(value);
      GST_OBJECT_UNLOCK(self);
      if (!mutable)
        GST_WARNING_OBJECT(self,
                           "scale-width can only change in NULL or READY");
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, spec);
      break;
  }
}

static void
gst_crystalhd_get_property(GObject *object, guint property_id,
                           GValue *value, GParamSpec *spec)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(object);

  switch (property_id) {
    case PROP_FINISH_TRACE_LIMIT:
      GST_OBJECT_LOCK(self);
      g_value_set_uint(value, gst_crystalhd_finish_trace_capacity(self->finish_trace));
      GST_OBJECT_UNLOCK(self);
      break;
    case PROP_SCALE_WIDTH:
      GST_OBJECT_LOCK(self);
      g_value_set_uint(value, self->scale_width);
      GST_OBJECT_UNLOCK(self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, spec);
      break;
  }
}

static gchar *
gst_crystalhd_finish_trace_snapshot_action(GstCrystalHdDec *self)
{
  GstCrystalHdFinishTrace *trace;
  gchar *snapshot;
  GST_OBJECT_LOCK(self);
  trace = gst_crystalhd_finish_trace_ref(self->finish_trace);
  GST_OBJECT_UNLOCK(self);
  snapshot = gst_crystalhd_finish_trace_snapshot(trace);
  gst_crystalhd_finish_trace_unref(trace);
  return snapshot;
}

static gboolean
gst_crystalhd_validate_scale_request(GstCrystalHdDec *self,
                                     GstVideoCodecState *state)
{
  const GstStructure *structure;
  const gchar *interlace_mode;
  guint coded_width;

  if (self->active_scale_width == 0)
    return TRUE;

  coded_width = GST_VIDEO_INFO_WIDTH(&state->info);
  if (self->active_scale_width < CRYSTALHD_SCALE_MIN_WIDTH ||
      self->active_scale_width > CRYSTALHD_SCALE_MAX_WIDTH ||
      (self->active_scale_width & 1) != 0) {
    GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                      ("Invalid CrystalHD hardware scale width %u",
                       self->active_scale_width),
                      ("Use 0 for native output or an even width from %u "
                       "through %u", CRYSTALHD_SCALE_MIN_WIDTH,
                       CRYSTALHD_SCALE_MAX_WIDTH));
    return FALSE;
  }
  if (coded_width == 0 || self->active_scale_width > coded_width) {
    GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                      ("CrystalHD hardware upscaling is not supported"),
                      ("Requested width %u; coded width %u",
                       self->active_scale_width, coded_width));
    return FALSE;
  }

  structure = gst_caps_get_structure(state->caps, 0);
  if (gst_structure_has_field(structure, "interlace-mode")) {
    interlace_mode = gst_structure_get_string(structure, "interlace-mode");
    if (interlace_mode == NULL ||
        (g_strcmp0(interlace_mode, "progressive") != 0 &&
         (self->codec.subtype != BC_MSUBTYPE_H264 ||
          (self->active_scale_width & 15U) != 0 ||
          (g_strcmp0(interlace_mode, "interleaved") != 0 &&
           g_strcmp0(interlace_mode, "mixed") != 0)))) {
      GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                        ("Unsupported CrystalHD scaled input scan mode"),
                        ("Input interlace-mode is %s",
                         interlace_mode != NULL ? interlace_mode :
                                                  "not a fixed string"));
      return FALSE;
    }
  }

  return TRUE;
}

static void
gst_crystalhd_clear_timestamps(GstCrystalHdDec *self)
{
  g_queue_clear_full(&self->timestamps, g_free);
  self->mpeg2_reference_pictures = 0;
  self->mpeg2_frame_reference_tracking = TRUE;
  self->mpeg2_dropped_inputs = 0;
  self->have_simple_picture_number = FALSE;
  self->wmv3_reordered_output = FALSE;
  self->wmv3_timing_valid = TRUE;
  self->wmv3_last_duration = GST_CLOCK_TIME_NONE;
}

/* Called with the decoder stream lock. Recovery discards every pre-epoch
 * hardware queue, so retire the matching base-class frames and timestamp
 * mirror together. @current remains owned by handle_frame when present. */
static GstFlowReturn
gst_crystalhd_reconcile_recovery(GstCrystalHdDec *self,
                                 GstVideoCodecFrame *current,
                                 gboolean *in_progress,
                                 gboolean *changed)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  guint64 generation = 0;
  BC_STATUS recovery_status = BC_STS_SUCCESS;
  guint8 active = FALSE;
  guint32 replay_bytes = 0;
  BC_STATUS status;

  *in_progress = FALSE;
  *changed = FALSE;
  self->output_recovering = FALSE;
  if (self->device == NULL)
    return GST_FLOW_OK;
  status = DtsGetRecoveryStatus(self->device, &generation,
                                &recovery_status, &active, &replay_bytes);
  if (status != BC_STS_SUCCESS) {
    GST_ELEMENT_ERROR(self, LIBRARY, FAILED,
                      ("Could not query CrystalHD recovery state"),
                      ("DtsGetRecoveryStatus returned %d", status));
    return GST_FLOW_ERROR;
  }
  if (recovery_status != BC_STS_SUCCESS) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("CrystalHD recovery failed: %s",
                       gst_crystalhd_status_hint(recovery_status)),
                      ("Recovery epoch %" G_GUINT64_FORMAT " returned %d",
                       generation, recovery_status));
    return GST_FLOW_ERROR;
  }

  *in_progress = active != FALSE;
  self->output_recovering = *in_progress;
  /* Do not consume the epoch until recovery has a final outcome. This keeps
   * completion observable so drain gets a fresh progress window and no old
   * frame is purged while firmware is still mutating its queues. */
  if (active)
    return GST_FLOW_OK;
  if (generation == self->recovery_generation_seen)
    return GST_FLOW_OK;

  self->recovery_h264_config_size = replay_bytes;

  self->recovery_generation_seen = generation;
  self->recovery_needs_sync = TRUE;
  self->recovery_request_headers = self->codec.subtype == BC_MSUBTYPE_H264;
  self->recovery_abort_started_us = 0;
  *changed = TRUE;
  while (!g_queue_is_empty(&self->timestamps)) {
    CrystalHdTimestamp *entry = g_queue_pop_head(&self->timestamps);
    GstVideoCodecFrame *pending = gst_video_decoder_get_frame(
        decoder, entry->frame_number);
    if (pending != NULL) {
      if (pending == current)
        gst_video_codec_frame_unref(pending);
      else
        gst_video_decoder_drop_frame(decoder, pending);
    }
    g_free(entry);
  }
  gst_crystalhd_clear_timestamps(self);
  self->need_second_field = FALSE;
  self->progressive_field_sequence = FALSE;
  self->output_eos = FALSE;
  self->drain_idle = FALSE;
  self->last_delivery_us = g_get_monotonic_time();
  GST_INFO_OBJECT(self, "reconciled CrystalHD recovery epoch %"
                  G_GUINT64_FORMAT, generation);
  return GST_FLOW_OK;
}

static gboolean
gst_crystalhd_recovery_abort_is_transient(GstCrystalHdDec *self,
                                          BC_STATUS status,
                                          GstFlowReturn *flow)
{
  gboolean active, changed;
  gint64 now;

  /* DtsDrvCmd can latch a PM epoch and still surface a generic I/O error
   * when ioctl(2) itself failed. Always inspect the epoch first. */
  *flow = gst_crystalhd_reconcile_recovery(self, NULL, &active, &changed);
  if (*flow != GST_FLOW_OK)
    return FALSE;
  if (active || changed) {
    self->recovery_abort_started_us = 0;
    return TRUE;
  }
  if (status != BC_STS_IO_USER_ABORT && status != BC_STS_PWR_MGMT)
    return FALSE;
  now = g_get_monotonic_time();
  if (self->recovery_abort_started_us == 0)
    self->recovery_abort_started_us = now;
  return now - self->recovery_abort_started_us <
      GST_CRYSTALHD_DRAIN_TIMEOUT_US;
}

static BC_STATUS
gst_crystalhd_close_device(GstCrystalHdDec *self)
{
  BC_STATUS result = BC_STS_SUCCESS;
  BC_STATUS status;
  BC_STATUS recovery_status = BC_STS_SUCCESS;
  guint64 recovery_generation = 0;
  guint8 recovery_active = FALSE;
  guint32 replay_bytes = 0;
  gint64 deadline;
  gboolean skip_firmware_cleanup = FALSE;

  /* A poll borrower must retire before this descriptor or the device can be
   * closed/reopened. Normal callers have quiesced or joined the worker. */
  g_mutex_lock(&self->output_lock);
  if (self->output_active || self->output_waiting) {
    g_mutex_unlock(&self->output_lock);
    return BC_STS_BUSY;
  }
  if (self->output_poll_fd >= 0) {
    close(self->output_poll_fd);
    self->output_poll_fd = -1;
  }
  g_mutex_unlock(&self->output_lock);

  if (self->device == NULL)
    goto cleared;

  deadline = g_get_monotonic_time() + GST_CRYSTALHD_DRAIN_TIMEOUT_US;
  for (;;) {
    status = DtsGetRecoveryStatus(self->device, &recovery_generation,
                                  &recovery_status, &recovery_active,
                                  &replay_bytes);
    if (status != BC_STS_SUCCESS)
      return status; /* Ownership is unknown; retain the live handle. */
    if (!recovery_active)
      break;
    if (g_get_monotonic_time() >= deadline)
      return BC_STS_BUSY; /* DeviceClose would itself be non-consuming BUSY. */
    g_usleep(1000);
  }
  skip_firmware_cleanup |= recovery_status != BC_STS_SUCCESS;

  while (!skip_firmware_cleanup && self->decoder_started) {
    status = DtsStopDecoder(self->device);
    if (status == BC_STS_SUCCESS) {
      self->decoder_started = FALSE;
      break;
    }
    {
      BC_STATUS query = DtsGetRecoveryStatus(self->device,
          &recovery_generation, &recovery_status, &recovery_active,
          &replay_bytes);
      if (query != BC_STS_SUCCESS)
        return status;
      if (query == BC_STS_SUCCESS && recovery_status != BC_STS_SUCCESS) {
        skip_firmware_cleanup = TRUE;
        if (result == BC_STS_SUCCESS)
          result = status;
        break;
      }
      if (query == BC_STS_SUCCESS && recovery_active) {
        if (g_get_monotonic_time() < deadline) {
          g_usleep(1000);
          continue;
        }
        return status;
      } else if (status == BC_STS_BUSY || status == BC_STS_PWR_MGMT ||
                 status == BC_STS_IO_USER_ABORT) {
        if (g_get_monotonic_time() < deadline) {
          g_usleep(1000);
          continue;
        }
      }
      if (result == BC_STS_SUCCESS)
        result = status;
      break;
    }
  }

  while (!skip_firmware_cleanup && self->decoder_open) {
    status = DtsCloseDecoder(self->device);
    if (status == BC_STS_SUCCESS) {
      self->decoder_open = FALSE;
      self->decoder_started = FALSE; /* Close subsumes a failed prior Stop. */
      break;
    }
    {
      BC_STATUS query = DtsGetRecoveryStatus(self->device,
          &recovery_generation, &recovery_status, &recovery_active,
          &replay_bytes);
      if (query != BC_STS_SUCCESS)
        return status;
      if (query == BC_STS_SUCCESS && recovery_status != BC_STS_SUCCESS) {
        skip_firmware_cleanup = TRUE;
        if (result == BC_STS_SUCCESS)
          result = status;
        break;
      }
      if (query == BC_STS_SUCCESS && recovery_active) {
        if (g_get_monotonic_time() < deadline) {
          g_usleep(1000);
          continue;
        }
        return status;
      } else if (status == BC_STS_BUSY || status == BC_STS_PWR_MGMT ||
                 status == BC_STS_IO_USER_ABORT) {
        if (g_get_monotonic_time() < deadline) {
          g_usleep(1000);
          continue;
        }
      }
      if (result == BC_STS_SUCCESS)
        result = status;
      break;
    }
  }

  /* DeviceClose is a consuming API even when final driver cleanup reports an
   * error. Call it exactly once after normal Stop/Close, or directly for a
   * quarantined failed-recovery channel that must receive no firmware command. */
  do {
    status = DtsDeviceClose(self->device);
    if (status != BC_STS_BUSY)
      break;
    if (g_get_monotonic_time() >= deadline)
      return status; /* Sole documented non-consuming close result. */
    g_usleep(1000);
  } while (TRUE);
  if (result == BC_STS_SUCCESS)
    result = status;
  self->device = NULL;
  self->decoder_started = FALSE;
  self->decoder_open = FALSE;

cleared:
  self->output_configured = FALSE;
  self->progressive_field_sequence = FALSE;
  self->input_flushed = FALSE;
  self->input_metadata_size = 0;
  self->need_second_field = FALSE;
  self->recovery_generation_seen = 0;
  self->recovery_needs_sync = FALSE;
  self->recovery_request_headers = FALSE;
  self->recovery_h264_config_size = 0;
  self->recovery_abort_started_us = 0;
  gst_crystalhd_clear_timestamps(self);
  return result;
}

static CrystalHdTimestamp *
gst_crystalhd_find_timestamp(GstCrystalHdDec *self, guint64 timestamp,
                             GList **link_out)
{
  GList *link;

  for (link = self->timestamps.head; link != NULL; link = link->next) {
    CrystalHdTimestamp *entry = link->data;
    if (entry->hardware_timestamp == timestamp) {
      if (link_out != NULL)
        *link_out = link;
      return entry;
    }
  }

  if (link_out != NULL)
    *link_out = NULL;
  return NULL;
}

static gboolean
gst_crystalhd_take_wmv3_timing(GstCrystalHdDec *self,
                               CrystalHdTimestamp *decoded,
                               GstClockTime *pts,
                               GstClockTime *duration)
{
  CrystalHdTimestamp *oldest = NULL;
  GstClockTime remaining_pts = GST_CLOCK_TIME_NONE;
  GstClockTime selected_duration;
  GstClockTime selected_pts;
  GList *link;

  if (!self->wmv3_timing_valid)
    return FALSE;

  /* Mirror GstVideoDecoder's pending-PTS ordering with the associated
   * duration. The hardware token identifies storage, while the lowest pending
   * PTS identifies the next already-display-ordered WMV3 picture. */
  for (link = self->timestamps.head; link != NULL; link = link->next) {
    CrystalHdTimestamp *candidate = link->data;

    if (!GST_CLOCK_TIME_IS_VALID(candidate->pts)) {
      /* A sparse timestamp prevents both the base class and this mirror from
       * proving a complete presentation order. Stay disabled for this
       * generation instead of resuming from potentially divergent swaps. */
      self->wmv3_timing_valid = FALSE;
      return FALSE;
    }
    if (oldest == NULL || candidate->pts < oldest->pts)
      oldest = candidate;
  }
  if (oldest == NULL)
    return FALSE;

  selected_pts = oldest->pts;
  selected_duration = oldest->duration;
  /* GstVideoDecoder preserves the decoded token's timing in the entry whose
   * earliest PTS is being consumed. Mirror that replacement so later output
   * retains the full PTS/duration tuple instead of only the PTS. */
  if (oldest != decoded) {
    oldest->pts = decoded->pts;
    oldest->duration = decoded->duration;
  }

  /* Find the next presentation PTS after the decoded token is logically
   * removed. This includes the replacement above and deliberately preserves
   * equal PTS as a reason not to infer a positive cadence. */
  for (link = self->timestamps.head; link != NULL; link = link->next) {
    CrystalHdTimestamp *candidate = link->data;

    if (candidate == decoded)
      continue;
    if (!GST_CLOCK_TIME_IS_VALID(remaining_pts) ||
        candidate->pts < remaining_pts)
      remaining_pts = candidate->pts;
  }

  *pts = selected_pts;
  *duration = selected_duration;
  if (GST_CLOCK_TIME_IS_VALID(remaining_pts) && remaining_pts > *pts) {
    GstClockTime cadence = remaining_pts - *pts;

    if (!GST_CLOCK_TIME_IS_VALID(*duration) || *duration > cadence)
      *duration = cadence;
  } else if (!GST_CLOCK_TIME_IS_VALID(remaining_pts)) {
    /* With no successor, the most recent positive presentation interval is
     * the only stream-local estimate. Do not use segment stop here: drain()
     * also runs at non-EOS boundaries where that would stretch a frame. */
    if (!GST_CLOCK_TIME_IS_VALID(*duration))
      *duration = self->wmv3_last_duration;
  }
  if (GST_CLOCK_TIME_IS_VALID(*duration) && *duration != 0)
    self->wmv3_last_duration = *duration;
  return TRUE;
}

static gboolean
gst_crystalhd_progressive_field_flags(guint32 pulldown, gboolean *top_first,
                                      gboolean *repeat_first)
{
  switch (pulldown) {
    case vdecTopBottom:
      *top_first = TRUE;
      *repeat_first = FALSE;
      return TRUE;
    case vdecBottomTop:
      *top_first = FALSE;
      *repeat_first = FALSE;
      return TRUE;
    case vdecTopBottomTop:
      *top_first = TRUE;
      *repeat_first = TRUE;
      return TRUE;
    case vdecBottomTopBottom:
      *top_first = FALSE;
      *repeat_first = TRUE;
      return TRUE;
    default:
      /* Single-field and whole-frame repeat values have not been measured
       * against GstVideo buffer semantics. Do not infer flags for them. */
      return FALSE;
  }
}

static gboolean
gst_crystalhd_validate_scaled_output(GstCrystalHdDec *self, guint width,
                                     guint height, gboolean interlaced)
{
  if (self->active_scale_width == 0)
    return TRUE;

  if (width != self->active_scale_width) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("CrystalHD hardware scaling was not applied"),
                      ("Requested width %u; hardware returned width %u",
                       self->active_scale_width, width));
    return FALSE;
  }
  if (interlaced && (self->is_70012 ||
      self->codec.subtype != BC_MSUBTYPE_H264 || (width & 15U) != 0 ||
      height == 0 || height > 1088 || (height & 3U) != 0)) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("Unsupported CrystalHD scaled interlaced output"),
                      ("Paired BCM70015 H.264 fields require aligned width "
                       "and complete field geometry"));
    return FALSE;
  }
  return TRUE;
}

static gboolean
gst_crystalhd_configure_output(GstCrystalHdDec *self, guint width, guint height,
                               gboolean interlaced)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  GstVideoCodecState *state;
  GstVideoInterlaceMode mode =
      interlaced || self->progressive_field_sequence
          ? GST_VIDEO_INTERLACE_MODE_MIXED
          : GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;
  gint output_par_n;
  gint output_par_d;
  gint scaled_par_n;
  gint scaled_par_d;

  if (!gst_crystalhd_validate_scaled_output(self, width, height, interlaced))
    return FALSE;
  if (width == 0 || height == 0 || width > 1920 || height > 1088) {
    GST_ERROR_OBJECT(self, "invalid output dimensions %ux%u", width, height);
    return FALSE;
  }

  output_par_n = GST_VIDEO_INFO_PAR_N(&self->input_state->info);
  output_par_d = GST_VIDEO_INFO_PAR_D(&self->input_state->info);
  if (output_par_n <= 0 || output_par_d <= 0) {
    output_par_n = 1;
    output_par_d = 1;
  }
  if (self->active_scale_width != 0) {
    gint64 geometry_n =
        (gint64)GST_VIDEO_INFO_WIDTH(&self->input_state->info) * height;
    gint64 geometry_d =
        (gint64)GST_VIDEO_INFO_HEIGHT(&self->input_state->info) * width;

    if (geometry_n <= 0 || geometry_d <= 0 ||
        geometry_n > G_MAXINT || geometry_d > G_MAXINT ||
        !gst_util_fraction_multiply(output_par_n, output_par_d,
                                    (gint)geometry_n, (gint)geometry_d,
                                    &scaled_par_n, &scaled_par_d) ||
        scaled_par_n <= 0 || scaled_par_d <= 0) {
      GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                        ("Could not represent scaled pixel aspect ratio"),
                        ("Input %ux%u PAR %d/%d; output %ux%u",
                         GST_VIDEO_INFO_WIDTH(&self->input_state->info),
                         GST_VIDEO_INFO_HEIGHT(&self->input_state->info),
                         output_par_n, output_par_d, width, height));
      return FALSE;
    }
    output_par_n = scaled_par_n;
    output_par_d = scaled_par_d;
  }

  if (self->output_configured && self->width == width &&
      self->height == height &&
      GST_VIDEO_INFO_INTERLACE_MODE(&self->output_info) == mode &&
      GST_VIDEO_INFO_PAR_N(&self->output_info) == output_par_n &&
      GST_VIDEO_INFO_PAR_D(&self->output_info) == output_par_d)
    return TRUE;

  state = gst_video_decoder_set_output_state(
      decoder, GST_VIDEO_FORMAT_YUY2, width, height, self->input_state);
  if (state == NULL)
    return FALSE;

  state->info.interlace_mode = mode;
  state->info.par_n = output_par_n;
  state->info.par_d = output_par_d;
  self->output_info = state->info;
  gst_video_codec_state_unref(state);

  if (!gst_video_decoder_negotiate(decoder))
    return FALSE;

  self->width = width;
  self->height = height;
  self->output_configured = TRUE;
  GST_INFO_OBJECT(self, "negotiated YUY2 output %ux%u", width, height);
  return TRUE;
}

static GstFlowReturn
gst_crystalhd_copy_output(GstCrystalHdDec *self, BC_DTS_PROC_OUT *output,
                          GstVideoCodecFrame **completed)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  GstVideoCodecFrame *frame;
  GstVideoFrame video_frame;
  GstVideoInfo copy_info;
  CrystalHdTimestamp *entry;
  GList *timestamp_link = NULL;
  guint32 frame_number;
  guint width = output->PicInfo.width;
  guint height = output->PicInfo.height;
  guint source_stride;
  guint destination_stride;
  guint row;
  guint rows;
  guint destination_row = 0;
  gboolean interlaced;
  gboolean whole_weave = FALSE;
  gboolean field_payload;
  gboolean bottom_field;
  gboolean bottom_first;
  gboolean progressive_top_first = FALSE;
  gboolean progressive_repeat_first = FALSE;
  gboolean progressive_field_sequence;
  gboolean ordered_mpeg4_simple;
  GstClockTime wmv3_pts = GST_CLOCK_TIME_NONE;
  GstClockTime wmv3_duration = GST_CLOCK_TIME_NONE;
  gboolean have_wmv3_timing = FALSE;

  *completed = NULL;

  interlaced = (output->PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) != 0;
  if (!gst_crystalhd_validate_scaled_output(self, width, height, interlaced))
    return GST_FLOW_ERROR;
  if (interlaced && !self->is_70012 &&
      self->codec.subtype == BC_MSUBTYPE_H264) {
    CrystalHDH264Payload layout = crystalhd_h264_interlaced_payload(output);

    if (layout == CRYSTALHD_H264_PAYLOAD_INVALID) {
      GST_ELEMENT_ERROR(self, STREAM, DECODE,
                        ("Invalid CrystalHD H.264 interlaced payload or identity"),
                        (NULL));
      return GST_FLOW_ERROR;
    }
    if (self->active_scale_width != 0 &&
        layout != CRYSTALHD_H264_PAYLOAD_FIELD) {
      GST_ELEMENT_ERROR(self, STREAM, DECODE,
                        ("Scaled H.264 output must contain separate fields"),
                        ("Whole Weave scaling has not been qualified"));
      return GST_FLOW_ERROR;
    }
    whole_weave = layout == CRYSTALHD_H264_PAYLOAD_WEAVE;
  }
  if (whole_weave && self->need_second_field) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("Whole CrystalHD H.264 picture interrupted a field pair"),
                      (NULL));
    return GST_FLOW_ERROR;
  }
  field_payload = interlaced && !whole_weave;
  /* These are values in a two-bit picture-type field, not independent bits:
   * TOPFIELD=0x10 shares a bit with BOTTOMFIELD=0x18; FIELDPAIR is 0x08. */
  bottom_field = (output->PicInfo.flags & VDEC_FLAG_BOTTOMFIELD) ==
                 VDEC_FLAG_BOTTOMFIELD;
  bottom_first = (output->PicInfo.flags & VDEC_FLAG_BOTTOM_FIRST) != 0;
  progressive_field_sequence =
      !interlaced && gst_crystalhd_progressive_field_flags(
                         output->PicInfo.pulldown, &progressive_top_first,
                         &progressive_repeat_first);
  ordered_mpeg4_simple =
      self->codec.subtype == BC_MSUBTYPE_DIVX &&
      self->codec.mpeg4_object_type == CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE &&
      !interlaced && !self->need_second_field;
  if (field_payload &&
      (output->PicInfo.flags & VDEC_FLAG_BOTTOMFIELD) != VDEC_FLAG_TOPFIELD &&
      !bottom_field) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("Unsupported CrystalHD interlaced output picture type"),
                      ("Expected separate TOPFIELD/BOTTOMFIELD buffers, flags=0x%08x",
                       output->PicInfo.flags));
    return GST_FLOW_ERROR;
  }

  entry = gst_crystalhd_find_timestamp(self, output->PicInfo.timeStamp,
                                       &timestamp_link);
  /* MPEG-4 Simple Profile has no B-VOP reordering. BCM70015 can return a
   * valid picture with its timestamp metadata cleared; picture zero is not
   * recoverable because the kernel uses it for invalid/repeated output. */
  if (entry == NULL && ordered_mpeg4_simple &&
      output->PicInfo.timeStamp == 0 &&
      output->PicInfo.picture_number != 0 &&
      self->have_simple_picture_number &&
      output->PicInfo.picture_number == self->simple_picture_number + 1 &&
      self->timestamps.head != NULL) {
    timestamp_link = self->timestamps.head;
    entry = timestamp_link->data;
    GST_WARNING_OBJECT(self,
        "recovering MPEG-4 Simple picture %u with missing hardware "
        "timestamp as token=%" G_GUINT64_FORMAT " frame=%u",
        output->PicInfo.picture_number, entry->hardware_timestamp,
        entry->frame_number);
  }
  if (entry == NULL) {
    /* Flush/retry can expose a late or duplicate hardware picture. Never
     * attach its pixels to an unrelated (possibly not yet submitted) frame.
     * Field pairing without a matching token is not a supported contract.
     */
    if (interlaced || self->need_second_field) {
      GST_ELEMENT_ERROR(self, STREAM, DECODE,
                        ("CrystalHD field has no matching input timestamp"),
                        ("Hardware timestamp: %" G_GUINT64_FORMAT,
                         (guint64)output->PicInfo.timeStamp));
      return GST_FLOW_ERROR;
    }
    GST_DEBUG_OBJECT(self, "ignoring unmatched hardware timestamp %"
                     G_GUINT64_FORMAT, (guint64)output->PicInfo.timeStamp);
    return GST_FLOW_OK;
  }
  if (ordered_mpeg4_simple && timestamp_link != self->timestamps.head) {
    CrystalHdTimestamp *head = self->timestamps.head->data;
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("CrystalHD MPEG-4 Simple output skipped a pending picture"),
                      ("Hardware timestamp: %" G_GUINT64_FORMAT "; "
                       "oldest pending timestamp: %" G_GUINT64_FORMAT "; "
                       "picture: %u",
                       (guint64)output->PicInfo.timeStamp,
                       head->hardware_timestamp, output->PicInfo.picture_number));
    return GST_FLOW_ERROR;
  }
  if (self->codec.subtype == BC_MSUBTYPE_WMV3 && !interlaced &&
      timestamp_link != self->timestamps.head) {
    if (!self->wmv3_reordered_output)
      GST_DEBUG_OBJECT(self,
          "WMV3 display-order output carries reordered input tokens");
    /* Keep this one-shot diagnostic separate from the timing repair, which
     * starts at the first progressive picture so seek clipping is correct
     * even before a non-head B-picture token is observed. */
    self->wmv3_reordered_output = TRUE;
  }
  frame_number = entry->frame_number;
  if (self->need_second_field &&
      (!interlaced || frame_number != self->field_frame_number)) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("CrystalHD fields belong to different input frames"),
                      (NULL));
    return GST_FLOW_ERROR;
  }
  if (self->need_second_field &&
      (bottom_field == self->field_bottom || width != self->field_width ||
       height != self->field_height)) {
    /* Both fields share one output allocation and must fill complementary
     * rows using exactly the same layout. A repeated parity or changed
     * geometry would otherwise finish partially unwritten/corrupt pixels. */
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("CrystalHD fields have repeated parity or different geometry"),
                      ("First field: bottom=%d %ux%u; next: bottom=%d %ux%u "
                       "flags=0x%08x token=%" G_GUINT64_FORMAT " picture=%u",
                       self->field_bottom, self->field_width, self->field_height,
                       bottom_field, width, height, output->PicInfo.flags,
                       (guint64)output->PicInfo.timeStamp,
                       output->PicInfo.picture_number));
    return GST_FLOW_ERROR;
  }
  if (self->need_second_field && bottom_first != self->field_bottom_first) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("CrystalHD fields have different presentation order"),
                      ("First bottom-first=%d; next bottom-first=%d "
                       "flags=0x%08x token=%" G_GUINT64_FORMAT " picture=%u",
                       self->field_bottom_first, bottom_first, output->PicInfo.flags,
                       (guint64)output->PicInfo.timeStamp,
                       output->PicInfo.picture_number));
    return GST_FLOW_ERROR;
  }

  if (width == 0 || height == 0 || width > 1920 || height > 1088 ||
      (interlaced && (height & 1)) ||
      !gst_video_info_set_format(&copy_info, GST_VIDEO_FORMAT_YUY2,
                                  width, height))
    return GST_FLOW_ERROR;

  frame = gst_video_decoder_get_frame(decoder, frame_number);
  if (frame == NULL) {
    GST_WARNING_OBJECT(self, "input frame %u is no longer queued", frame_number);
    return GST_FLOW_OK;
  }

  if (frame->output_buffer == NULL) {
    /* Plain video/x-raw caps permit owned system memory. Do not wait for a
     * downstream pool while holding a hardware RX lease or the stream lock:
     * that can prevent the demuxer feeding the audio needed for preroll. */
    frame->output_buffer = gst_buffer_new_allocate(NULL, copy_info.size, NULL);
    if (frame->output_buffer == NULL) {
      gst_video_codec_frame_unref(frame);
      return GST_FLOW_ERROR;
    }
  }

  if (!gst_video_frame_map(&video_frame, &copy_info,
                           frame->output_buffer, GST_MAP_WRITE)) {
    gst_video_codec_frame_unref(frame);
    return GST_FLOW_ERROR;
  }

  source_stride = width * 2;
  if (self->is_70012) {
    guint padded_width = width <= 720 ? 720 : (width <= 1280 ? 1280 : 1920);
    source_stride = padded_width * 2;
  }
  destination_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 0);
  rows = field_payload ? height / 2 : height;
  destination_row = field_payload && bottom_field ? 1 : 0;

  if (output->Ybuff == NULL ||
      (guint64)output->YBuffDoneSz * 4 < (guint64)source_stride * rows) {
    GST_ERROR_OBJECT(self, "short CrystalHD output buffer (%u dwords)",
                     output->YBuffDoneSz);
    gst_video_frame_unmap(&video_frame);
    gst_video_codec_frame_unref(frame);
    return GST_FLOW_ERROR;
  }

  for (row = 0; row < rows; row++) {
    guint8 *destination =
        GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 0) +
        destination_row * destination_stride;
    memcpy(destination, output->Ybuff + row * source_stride, width * 2);
    destination_row += field_payload ? 2 : 1;
  }

  gst_video_frame_unmap(&video_frame);

  if (field_payload && !self->need_second_field) {
    self->need_second_field = TRUE;
    self->field_frame_number = frame_number;
    self->field_bottom = bottom_field;
    self->field_bottom_first = bottom_first;
    self->field_width = width;
    self->field_height = height;
    gst_video_codec_frame_unref(frame);
    return GST_FLOW_OK;
  }

  /* Mixed caps alone do not mark an individual buffer as interlaced. The
   * complete weave's presentation order is explicit BOTTOM_FIRST metadata:
   * capture can still return the top field first during a BFF transition.
   * Clear stale flags even when an existing output allocation is reused. */
  GST_BUFFER_FLAG_UNSET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_INTERLACED);
  GST_BUFFER_FLAG_UNSET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_TFF);
  GST_BUFFER_FLAG_UNSET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_ONEFIELD);
  GST_BUFFER_FLAG_UNSET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_RFF);
  if (interlaced) {
    GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_INTERLACED);
    if (!bottom_first)
      GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_TFF);
  } else if (progressive_field_sequence) {
    /* Progressive soft telecine remains a progressive buffer. MIXED caps
     * allow its per-picture field order/repeat flags, and stay negotiated for
     * the rest of this session so no-info pictures cannot churn caps. */
    self->progressive_field_sequence = TRUE;
    if (progressive_top_first)
      GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_TFF);
    if (progressive_repeat_first)
      GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_RFF);
  }
  self->need_second_field = FALSE;
  if (self->codec.subtype == BC_MSUBTYPE_WMV3 && !interlaced)
    have_wmv3_timing = gst_crystalhd_take_wmv3_timing(
        self, entry, &wmv3_pts, &wmv3_duration);
  if (timestamp_link != NULL) {
    g_free(timestamp_link->data);
    g_queue_delete_link(&self->timestamps, timestamp_link);
  }
  if (ordered_mpeg4_simple) {
    if (output->PicInfo.picture_number != 0 &&
        output->PicInfo.picture_number != G_MAXUINT32) {
      self->simple_picture_number = output->PicInfo.picture_number;
      self->have_simple_picture_number = TRUE;
    } else {
      self->have_simple_picture_number = FALSE;
    }
  }

  GST_LOG_OBJECT(self, "decoded frame %u (%ux%u), picture %u", frame_number,
                 width, height, output->PicInfo.picture_number);
  if (have_wmv3_timing && self->codec.subtype == BC_MSUBTYPE_WMV3 &&
      !interlaced) {
    frame->pts = wmv3_pts;
    frame->duration = wmv3_duration;
  }
  *completed = frame;
  return GST_FLOW_OK;
}

static GstFlowReturn
gst_crystalhd_receive_one(GstCrystalHdDec *self, guint timeout_ms,
                          gboolean *activity, GstVideoCodecFrame **completed)
{
  BC_DTS_PROC_OUT output;
  BC_STATUS status;
  GstFlowReturn flow = GST_FLOW_OK;

  memset(&output, 0, sizeof(output));
  output.PicInfo.width = self->width;
  output.PicInfo.height = self->height;
  *activity = FALSE;
  *completed = NULL;

  status = DtsProcOutputNoCopy(self->device, timeout_ms, &output);
  if (status == BC_STS_FMT_CHANGE || status == BC_STS_NO_DATA ||
      status == BC_STS_BUSY ||
      status == BC_STS_TIMEOUT)
    self->recovery_abort_started_us = 0;
  if (status == BC_STS_SUCCESS || status == BC_STS_FMT_CHANGE)
    GST_LOG_OBJECT(self, "received PIB: status=%d flags=0x%08x output-flags=0x%08x "
        "token=%" G_GUINT64_FORMAT " picture=%u %ux%u Y-dwords=%u",
        status, output.PicInfo.flags, output.PoutFlags,
        (guint64)output.PicInfo.timeStamp, output.PicInfo.picture_number,
        output.PicInfo.width, output.PicInfo.height, output.YBuffDoneSz);
  if (status == BC_STS_FMT_CHANGE) {
    *activity = TRUE;
    if (!gst_crystalhd_configure_output(
            self, output.PicInfo.width, output.PicInfo.height,
            (output.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) != 0))
      return GST_FLOW_NOT_NEGOTIATED;
    return GST_FLOW_OK;
  }

  if (status == BC_STS_SUCCESS) {
    *activity = TRUE;
    if ((output.PoutFlags & BC_POUT_FLAGS_PIB_VALID) != 0)
      flow = gst_crystalhd_copy_output(self, &output, completed);
    else
      GST_WARNING_OBJECT(self, "decoder returned a picture without valid PIB");

    status = DtsReleaseOutputBuffs(self->device, NULL, FALSE);
    if (status != BC_STS_SUCCESS && flow == GST_FLOW_OK) {
      if (gst_crystalhd_recovery_abort_is_transient(self, status, &flow)) {
        *activity = FALSE;
        if (*completed != NULL) {
          /* copy_output has already consumed the timestamp. Retire both the
           * returned reference and the base pending frame before retrying. */
          gst_video_decoder_drop_frame(GST_VIDEO_DECODER(self), *completed);
          *completed = NULL;
        }
        return GST_FLOW_OK;
      }
      if (flow == GST_FLOW_OK) {
        GST_ERROR_OBJECT(self, "failed to release output buffers: %d", status);
        flow = GST_FLOW_ERROR;
      }
    }
    if (status == BC_STS_SUCCESS)
      self->recovery_abort_started_us = 0;
    /* No hardware pointer or lease survives negotiation/downstream calls. */
    if (flow == GST_FLOW_OK && *completed != NULL &&
        !gst_crystalhd_configure_output(
            self, output.PicInfo.width, output.PicInfo.height,
            (output.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) != 0))
      flow = GST_FLOW_NOT_NEGOTIATED;
    if (flow != GST_FLOW_OK && *completed != NULL) {
      /* copy_output already removed the timestamp and returned a get_frame
       * reference. Retire the base pending frame as well; an unacknowledged
       * hardware buffer must never be delivered or left orphaned. */
      gst_video_decoder_drop_frame(GST_VIDEO_DECODER(self), *completed);
      *completed = NULL;
    }
    return flow;
  }

  if (status == BC_STS_NO_DATA || status == BC_STS_BUSY ||
      status == BC_STS_TIMEOUT)
    return GST_FLOW_OK;

  if (gst_crystalhd_recovery_abort_is_transient(self, status, &flow))
    return GST_FLOW_OK;
  if (flow != GST_FLOW_OK)
    return flow;

  GST_ERROR_OBJECT(self, "DtsProcOutputNoCopy failed: %d", status);
  return GST_FLOW_ERROR;
}

static gboolean
gst_crystalhd_is_flushing(GstCrystalHdDec *self)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  return GST_PAD_IS_FLUSHING(GST_VIDEO_DECODER_SINK_PAD(decoder)) ||
         GST_PAD_IS_FLUSHING(GST_VIDEO_DECODER_SRC_PAD(decoder));
}

static void
gst_crystalhd_signal_output_locked(GstCrystalHdDec *self)
{
  const uint64_t one = 1;
  ssize_t result;
  self->output_wake_pending = TRUE;
  if (self->output_control_fd >= 0) {
    do {
      result = write(self->output_control_fd, &one, sizeof(one));
    } while (result < 0 && errno == EINTR);
    /* EAGAIN means that an earlier control wake is already latched. */
  }
  g_cond_signal(&self->output_cond);
}

static void
gst_crystalhd_wake_output(GstCrystalHdDec *self)
{
  g_mutex_lock(&self->output_lock);
  gst_crystalhd_signal_output_locked(self);
  g_mutex_unlock(&self->output_lock);
}

/* One sole-consumer iteration; callers must NOT own the decoder stream lock.
 * output_active covers the gap before finish_frame obtains its own lock as
 * well as its downstream push. A flushing generation cannot retire until
 * that delivery has actually returned. */
static gboolean
gst_crystalhd_output_iteration(GstCrystalHdDec *self)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  GstVideoCodecFrame *completed = NULL;
  BC_DTS_STATUS decoder_status;
  BC_STATUS status;
  GstFlowReturn flow = GST_FLOW_OK;
  gboolean activity = FALSE;
  gboolean recovery_active = FALSE;
  gboolean recovery_changed = FALSE;
  GstCrystalHdFinishTraceContext finish_context;
  GstCrystalHdFinishTrace *finish_trace;

  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  flow = gst_crystalhd_reconcile_recovery(self, NULL, &recovery_active,
                                          &recovery_changed);
  if (flow != GST_FLOW_OK) {
    self->output_flow = flow;
    GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
    return FALSE;
  }
  if (recovery_active) {
    GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
    return FALSE;
  }
  g_mutex_lock(&self->output_lock);
  if (self->output_stop || self->output_paused || self->output_active ||
      !self->decoder_started || self->output_flow != GST_FLOW_OK ||
      gst_crystalhd_is_flushing(self) ||
      (g_queue_is_empty(&self->timestamps) && !self->draining)) {
    g_mutex_unlock(&self->output_lock);
    GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
    return FALSE;
  }
  self->output_active = TRUE;
  g_mutex_unlock(&self->output_lock);

  memset(&decoder_status, 0, sizeof(decoder_status));
  /* BCM70012's READY-head peek also retires repeated/garbage pictures before
   * advertising availability to legacy single-threaded callers.  Only Flea's
   * queue poll is metadata-free and safe to request without that peek. */
  if (!self->is_70012)
    decoder_status.cpbEmptySize = CRYSTALHD_STATUS_SKIP_READY_HEAD_PEEK;
  status = DtsGetDriverStatus(self->device, &decoder_status);
  if (status != BC_STS_SUCCESS) {
    if (!gst_crystalhd_recovery_abort_is_transient(self, status, &flow) &&
        flow == GST_FLOW_OK) {
      GST_ERROR_OBJECT(self, "DtsGetDriverStatus failed: %d", status);
      flow = GST_FLOW_ERROR;
    }
  } else if (decoder_status.ReadyListCount != 0) {
    self->drain_idle = FALSE;
    flow = gst_crystalhd_receive_one(self, 0, &activity, &completed);
  } else if (self->draining) {
    guint8 eos = FALSE;
    self->drain_idle = TRUE;
    status = DtsIsEndOfStream(self->device, &eos);
    if (status != BC_STS_SUCCESS) {
      if (!gst_crystalhd_recovery_abort_is_transient(self, status, &flow) &&
          flow == GST_FLOW_OK)
        flow = GST_FLOW_ERROR;
    }
    else {
      self->recovery_abort_started_us = 0;
      self->output_eos = eos != 0;
    }
  } else {
    self->recovery_abort_started_us = 0;
  }
  finish_trace = self->active_finish_trace;
  if (completed != NULL && finish_trace != NULL) {
    finish_context = (GstCrystalHdFinishTraceContext){
        (guint64)(guintptr)self, self->generation, self->recovery_generation_seen,
        completed->system_frame_number, completed->pts, completed->duration};
  }
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);

  /* finish_frame drops only its own recursive stream-lock level around
   * downstream delivery. Entering with an outer level would block input,
   * including the demuxed audio required to complete video preroll. */
  if (completed != NULL) {
    /* OFF is a pointer branch: no diagnostic clock, TID, lock or allocation.
     * The active collector survives until stop() has joined this sole worker. */
    if (finish_trace != NULL)
      flow = gst_crystalhd_finish_trace_call(finish_trace,
                                            decoder, completed, &finish_context);
    else
      flow = gst_video_decoder_finish_frame(decoder, completed);
  }

  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  if (completed != NULL && flow == GST_FLOW_OK)
    self->last_delivery_us = g_get_monotonic_time();
  if (flow == GST_FLOW_OK) {
    GstFlowReturn recovery_flow = gst_crystalhd_reconcile_recovery(
        self, NULL, &recovery_active, &recovery_changed);
    if (recovery_flow != GST_FLOW_OK)
      flow = recovery_flow;
  }
  if (flow != GST_FLOW_OK) {
    self->output_flow = flow;
    if (flow != GST_FLOW_FLUSHING && flow != GST_FLOW_EOS)
      GST_ELEMENT_ERROR(self, STREAM, DECODE,
                        ("CrystalHD output delivery failed"),
                        ("Flow: %s", gst_flow_get_name(flow)));
  }
  g_mutex_lock(&self->output_lock);
  self->output_active = FALSE;
  g_cond_broadcast(&self->output_cond);
  g_mutex_unlock(&self->output_lock);
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  return activity;
}

static gboolean
gst_crystalhd_wait_ready(GstCrystalHdDec *self, gboolean no_progress_ready,
                         gboolean *rx_ready)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  struct pollfd descriptors[2];
  gint timeout, result;
  uint64_t counter;
  ssize_t drained;

  *rx_ready = FALSE;

  g_mutex_lock(&self->output_lock);
  if (self->output_stop || self->output_paused || self->output_wake_pending) {
    gboolean running = !self->output_stop;
    self->output_wake_pending = FALSE;
    g_mutex_unlock(&self->output_lock);
    return running;
  }
  if (self->output_poll_fd < 0 || self->output_control_fd < 0) {
    g_cond_wait_until(&self->output_cond, &self->output_lock,
                      g_get_monotonic_time() + 5000);
    self->output_wake_pending = FALSE;
    g_mutex_unlock(&self->output_lock);
    return TRUE;
  }
  g_mutex_unlock(&self->output_lock);

  /* Snapshot stream state in the established stream -> scheduling order.
   * Registration is level-triggered, and the private control counter latches
   * an input/stop wake that arrives between this snapshot and poll(). */
  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  g_mutex_lock(&self->output_lock);
  if (self->output_stop || self->output_paused || self->output_wake_pending ||
      self->output_poll_fd < 0) {
    gboolean running = !self->output_stop;
    self->output_wake_pending = FALSE;
    g_mutex_unlock(&self->output_lock);
    GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
    return running;
  }
  do {
    drained = read(self->output_control_fd, &counter, sizeof(counter));
  } while (drained < 0 && errno == EINTR);
  descriptors[0] = (struct pollfd){self->output_control_fd, POLLIN, 0};
  descriptors[1] = (struct pollfd){
      !no_progress_ready && self->decoder_started && self->output_flow == GST_FLOW_OK &&
      !g_queue_is_empty(&self->timestamps) ? self->output_poll_fd : -1,
      POLLIN, 0};
  /* EOS/recovery can change in the library without a kernel RX edge. Keep
   * the existing bounded state probes for those windows. No idle READY queue
   * is polled when this session has no outstanding picture identities. */
  timeout = no_progress_ready || self->draining || self->output_recovering ||
      self->recovery_abort_started_us != 0 ? 5 : -1;
  self->output_waiting = TRUE;
  g_mutex_unlock(&self->output_lock);
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  do {
    result = poll(descriptors, 2, timeout);
  } while (result < 0 && errno == EINTR);
  g_mutex_lock(&self->output_lock);
  self->output_waiting = FALSE;
  *rx_ready = result > 0 && (descriptors[1].revents & POLLIN) != 0;
  if (result < 0 ||
      ((descriptors[0].revents | descriptors[1].revents) &
       (POLLERR | POLLHUP | POLLNVAL)) != 0) {
    /* Suspend/removal and optional-fd failures retain ordinary status/recovery
     * handling; never spin on a terminal descriptor or reuse it after reopen. */
    GST_WARNING_OBJECT(self,
        "output readiness ended (poll=%d, control=0x%x, RX=0x%x); using timed status checks",
        result, (unsigned int)descriptors[0].revents,
        (unsigned int)descriptors[1].revents);
    close(self->output_poll_fd);
    self->output_poll_fd = -1;
  }
  g_cond_broadcast(&self->output_cond);
  g_mutex_unlock(&self->output_lock);
  return TRUE;
}

static gpointer
gst_crystalhd_output_thread(gpointer data)
{
  GstCrystalHdDec *self = data;
  gboolean rx_ready = FALSE;

  for (;;) {
    gboolean activity = FALSE;
    g_mutex_lock(&self->output_lock);
    while (self->output_paused && !self->output_stop)
      g_cond_wait(&self->output_cond, &self->output_lock);
    if (self->output_stop) {
      g_mutex_unlock(&self->output_lock);
      break;
    }
    g_mutex_unlock(&self->output_lock);
    /* Consume ready output without a sleep between pictures, but release
     * the stream lock between iterations so input and flush can progress. */
    while (gst_crystalhd_output_iteration(self))
      activity = TRUE;
    /* A stale READY indication must not turn a BUSY/NO_DATA response into
     * a tight status loop. Mask RX for one bounded control/state-probe turn. */
    if (!gst_crystalhd_wait_ready(self, rx_ready && !activity, &rx_ready))
      break;
  }
  return NULL;
}

/* Called with exactly the vfunc's stream-lock level. In particular, neither
 * stop nor the nonserialized FLUSH_START event may call this without first
 * taking that lock. Flush cannot return before quiescence: the base class
 * ignores its boolean result and resets queued frames unconditionally. */
static gboolean
gst_crystalhd_quiesce_output(GstCrystalHdDec *self, gboolean bounded)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  gint64 deadline = g_get_monotonic_time() + GST_CRYSTALHD_DRAIN_TIMEOUT_US;
  gboolean active;

  g_mutex_lock(&self->output_lock);
  self->output_paused = TRUE;
  gst_crystalhd_signal_output_locked(self);
  g_mutex_unlock(&self->output_lock);
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  do {
    g_mutex_lock(&self->output_lock);
    active = self->output_active || self->output_waiting;
    g_mutex_unlock(&self->output_lock);
    if (!active || (bounded && g_get_monotonic_time() >= deadline))
      break;
    g_usleep(1000);
  } while (TRUE);
  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  if (active)
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("Output delivery did not stop before a format change"),
                      ("A flushing transition is required to cancel downstream playback"));
  return !active;
}

static void
gst_crystalhd_resume_output(GstCrystalHdDec *self)
{
  g_mutex_lock(&self->output_lock);
  self->output_paused = FALSE;
  gst_crystalhd_signal_output_locked(self);
  g_mutex_unlock(&self->output_lock);
}

/* Progress waits never consume RX themselves. Revalidate the generation
 * after dropping the stream lock: a flushing seek may have reopened the
 * device and invalidated the old compressed-input/drain request. */
static GstFlowReturn
gst_crystalhd_wait_output(GstCrystalHdDec *self, guint64 generation,
                          gint64 delay_us, guint lock_levels)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  guint level;
  gst_crystalhd_wake_output(self);
  for (level = 0; level < lock_levels; level++)
    GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  g_usleep((gulong)MAX((gint64)1, delay_us));
  for (level = 0; level < lock_levels; level++)
    GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  if (generation != self->generation || gst_crystalhd_is_flushing(self))
    return GST_FLOW_FLUSHING;
  return self->output_flow;
}

static GstFlowReturn
gst_crystalhd_wait_input_progress(GstCrystalHdDec *self, guint64 generation,
                                  gint64 deadline, guint lock_levels)
{
  gint64 remaining = gst_crystalhd_drain_remaining_us(
      deadline, g_get_monotonic_time());

  if (remaining == 0) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("Timed out waiting for CrystalHD input recovery"),
                      ("No stable input admission before the 10 second deadline"));
    return GST_FLOW_ERROR;
  }
  return gst_crystalhd_wait_output(self, generation,
                                   MIN(remaining, (gint64)1000), lock_levels);
}

/* A human pause is not stalled decoding. Inspect the top-level state so a
 * child already PAUSED while its pipeline still awaits PLAYING/preroll is
 * not mistaken for a deliberate pause. No state change is performed here. */
static gboolean
gst_crystalhd_playback_paused(GstCrystalHdDec *self)
{
  GstObject *object = gst_object_ref(self);
  GstObject *parent;
  GstState state = GST_STATE_NULL;
  GstState pending = GST_STATE_VOID_PENDING;

  while ((parent = gst_object_get_parent(object)) != NULL) {
    gst_object_unref(object);
    object = parent;
  }
  if (GST_IS_ELEMENT(object)) {
    /* State fields are protected by GstObject's lock. Do not take the
     * element state-change lock from inside the decoder stream lock. */
    GST_OBJECT_LOCK(object);
    state = GST_STATE(object);
    pending = GST_STATE_PENDING(object);
    GST_OBJECT_UNLOCK(object);
  }
  gst_object_unref(object);
  return state == GST_STATE_PAUSED && pending == GST_STATE_VOID_PENDING;
}

static GstFlowReturn
gst_crystalhd_wait_input_space(GstCrystalHdDec *self, gsize reservation,
                               gint64 deadline, guint lock_levels)
{
  guint64 generation = self->generation;
  for (;;) {
    gint64 remaining;
    GstFlowReturn flow;

    if (gst_crystalhd_is_flushing(self))
      return GST_FLOW_FLUSHING;
    if (self->output_flow != GST_FLOW_OK)
      return self->output_flow;
    deadline = MAX(deadline,
                    self->last_delivery_us + GST_CRYSTALHD_INPUT_TIMEOUT_US);
    if (gst_crystalhd_playback_paused(self))
      deadline = g_get_monotonic_time() + GST_CRYSTALHD_INPUT_TIMEOUT_US;
    remaining = gst_crystalhd_drain_remaining_us(deadline, g_get_monotonic_time());
    if (remaining == 0) {
      GST_ELEMENT_ERROR(self, STREAM, DECODE,
                        ("Timed out waiting for CrystalHD input capacity"),
                        ("Whole-call reservation: %" G_GSIZE_FORMAT " bytes",
                         reservation));
      return GST_FLOW_ERROR;
    }
    if (DtsTxFreeSize(self->device) >= reservation)
      return GST_FLOW_OK;

    /* The RX worker runs even when demuxed input is waiting for audio
     * preroll. Let it retire hardware output before retrying admission. */
    flow = gst_crystalhd_wait_output(self, generation,
                                    MIN(remaining, (gint64)1000), lock_levels);
    if (flow != GST_FLOW_OK)
      return flow;
  }
}

static gboolean
gst_crystalhd_start(GstVideoDecoder *decoder)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);

  g_return_val_if_fail(self->output_thread == NULL, FALSE);
  GST_OBJECT_LOCK(self);
  self->active_scale_width = self->scale_width;
  GST_OBJECT_UNLOCK(self);
#ifdef GST_CRYSTALHD_MPEG1_EXPERIMENTAL
  if (!gst_crystalhd_mpeg1_scale_allowed(self->active_scale_width)) {
    GST_ELEMENT_ERROR(self, STREAM, FORMAT,
        ("Experimental MPEG-1 requires native 352x288 output"),
        ("Hardware scaling is outside the qualified experimental scope"));
    return FALSE;
  }
#endif
  if (gst_crystalhd_close_device(self) != BC_STS_SUCCESS)
    return FALSE;
  if (self->output_control_fd < 0)
    self->output_control_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  self->next_hardware_timestamp = CRYSTALHD_TIMESTAMP_STEP;
  self->output_flow = GST_FLOW_OK;
  self->output_stop = FALSE;
  self->output_paused = TRUE;
  self->draining = FALSE;
  self->output_eos = FALSE;
  self->last_delivery_us = 0;
  self->generation++;
  GST_OBJECT_LOCK(self);
  self->active_finish_trace = gst_crystalhd_finish_trace_ref(self->finish_trace);
  GST_OBJECT_UNLOCK(self);
  self->output_thread = g_thread_new("crystalhd-output",
                                     gst_crystalhd_output_thread, self);
  gst_video_decoder_set_packetized(decoder, TRUE);
  return TRUE;
}

static gboolean
gst_crystalhd_stop(GstVideoDecoder *decoder)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  BC_STATUS status;

  /* start/stop are outside the base stream lock. Pad deactivation/flush
   * cancels a downstream push before this join; never close a live lease. */
  g_mutex_lock(&self->output_lock);
  self->output_stop = TRUE;
  self->output_paused = TRUE;
  gst_crystalhd_signal_output_locked(self);
  g_cond_broadcast(&self->output_cond);
  g_mutex_unlock(&self->output_lock);
  if (self->output_thread != NULL) {
    g_thread_join(self->output_thread);
    self->output_thread = NULL;
  }
  g_clear_pointer(&self->active_finish_trace, gst_crystalhd_finish_trace_unref);
  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  self->generation++;
  status = gst_crystalhd_close_device(self);
  g_clear_pointer(&self->input_state, gst_video_codec_state_unref);
  g_clear_object(&self->parse_adapter);
  self->parse_pending = FALSE;
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  if (status != BC_STS_SUCCESS)
    GST_ELEMENT_ERROR(self, LIBRARY, SHUTDOWN,
                      ("Could not close CrystalHD playback session"),
                      ("Device teardown returned %d", status));
  return status == BC_STS_SUCCESS;
}

static gboolean
gst_crystalhd_reopen_format(GstVideoDecoder *decoder, GstVideoCodecState *state,
                            gboolean discard)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  GstStructure *structure = gst_caps_get_structure(state->caps, 0);
  BC_INPUT_FORMAT input_format;
  BC_INFO_CRYSTAL version;
  BC_MEDIA_SUBTYPE subtype;
  BC_STATUS status;
  guint32 mode;
  const GValue *codec_data_value;
  GstMapInfo codec_data_map;
  gboolean codec_data_mapped = FALSE;
  const gchar *operation = "DtsDeviceOpen";
  GstVideoCodecState *retained_state = gst_video_codec_state_ref(state);
  guint64 generation = self->generation;

  if (!gst_crystalhd_quiesce_output(self, TRUE) ||
      generation != self->generation ||
      (!discard && gst_crystalhd_is_flushing(self))) {
    gst_video_codec_state_unref(retained_state);
    return FALSE;
  }
  self->generation++;
  self->output_flow = GST_FLOW_OK;
  self->draining = FALSE;
  self->output_eos = FALSE;
  self->last_delivery_us = 0;
  g_clear_object(&self->parse_adapter);
  self->parse_pending = FALSE;
  status = gst_crystalhd_close_device(self);
  if (status != BC_STS_SUCCESS) {
    gst_video_codec_state_unref(retained_state);
    GST_ELEMENT_ERROR(self, LIBRARY, SHUTDOWN,
                      ("Could not close previous CrystalHD session"),
                      ("Device teardown returned %d", status));
    return FALSE;
  }
  g_clear_pointer(&self->input_state, gst_video_codec_state_unref);
  self->input_state = retained_state;

  if (!gst_crystalhd_codec_from_caps(state->caps, &self->codec)) {
    GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                      ("Unsupported CrystalHD input codec"),
                      ("Caps: %" GST_PTR_FORMAT, state->caps));
    return FALSE;
  }
  subtype = self->codec.subtype;
  gst_video_decoder_set_packetized(decoder, !self->codec.vc1_bdu);

  memset(&input_format, 0, sizeof(input_format));
  input_format.FGTEnable = FALSE;
  input_format.Progressive = TRUE;
  input_format.OptFlags = 0x80000000U | vdecFrameRate59_94 | 0x40U;
  input_format.mSubtype = subtype;
  input_format.width = GST_VIDEO_INFO_WIDTH(&state->info);
  input_format.height = GST_VIDEO_INFO_HEIGHT(&state->info);
  if (!gst_crystalhd_validate_scale_request(self, state))
    goto fail;
  if (subtype == BC_MSUBTYPE_DIVX) {
    if (input_format.width == 0 || input_format.width > 1920 ||
        input_format.height == 0 || input_format.height > 1088 ||
        (self->codec.mpeg4_width != 0 &&
         self->codec.mpeg4_width != input_format.width) ||
        (self->codec.mpeg4_height != 0 &&
         self->codec.mpeg4_height != input_format.height)) {
      GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                        ("Invalid MPEG-4 coded dimensions"),
                        ("Caps/state dimensions are outside the supported "
                         "range or disagree"));
      goto fail;
    }
    self->codec.mpeg4_width = input_format.width;
    self->codec.mpeg4_height = input_format.height;
  }
  if (subtype == BC_MSUBTYPE_H264 || subtype == BC_MSUBTYPE_DIVX)
    input_format.startCodeSz = 4;

  codec_data_value = gst_structure_get_value(structure, "codec_data");
  if (codec_data_value != NULL && GST_VALUE_HOLDS_BUFFER(codec_data_value)) {
    GstBuffer *codec_data = gst_value_get_buffer(codec_data_value);
    if (codec_data != NULL &&
        gst_buffer_map(codec_data, &codec_data_map, GST_MAP_READ)) {
      codec_data_mapped = TRUE;
      if (codec_data_map.size > G_MAXUINT32) {
        GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                          ("Codec data exceeds the input limit"), (NULL));
        goto fail;
      }
      input_format.pMetaData = codec_data_map.data;
      input_format.metaDataSz = codec_data_map.size;
    }
  }

  {
    const guint8 *metadata = input_format.pMetaData;
    gsize metadata_size = input_format.metaDataSz;
    gsize minimum_reservation;
    if (!gst_crystalhd_codec_metadata(&self->codec, &metadata, &metadata_size) ||
        (subtype == BC_MSUBTYPE_WMV3 &&
         (!input_format.width || !input_format.height))) {
      GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                        ("Missing or invalid codec data or dimensions"),
                        ("Use a parser with a supported stream/header format"));
      goto fail;
    }
    if (!gst_crystalhd_input_reservation(subtype, 1, metadata_size,
                                         &minimum_reservation)) {
      GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                        ("Codec metadata exceeds CrystalHD input ring capacity"),
                        (NULL));
      goto fail;
    }
    self->input_metadata_size = metadata_size;
    input_format.pMetaData = (guint8 *)metadata;
    input_format.metaDataSz = metadata_size;
  }
  if (subtype == BC_MSUBTYPE_MPEG2VIDEO) {
    CrystalHdMpeg2Picture metadata;
    self->codec.mpeg2_height = input_format.height;
    crystalhd_mpeg2_picture_headers(input_format.pMetaData,
                                    input_format.metaDataSz, &metadata);
    if (metadata.have_sequence)
      self->codec.mpeg2_height = metadata.height;
    if (metadata.have_sequence_extension) {
      self->codec.mpeg2_profile = metadata.profile;
      self->codec.mpeg2_progressive_sequence = metadata.progressive_sequence;
    }
  }

  mode = DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW | DTS_SKIP_TX_CHK_CPB |
         DTS_PLAYBACK_DROP_RPT_MODE | DTS_SINGLE_THREADED_MODE |
         DTS_DFLT_RESOLUTION(vdecRESOLUTION_1080p23_976);
  status = DtsDeviceOpen(&self->device, mode);
  if (status != BC_STS_SUCCESS) {
    GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ,
                      ("Could not open /dev/crystalhd: %s",
                       gst_crystalhd_status_hint(status)),
                      ("DtsDeviceOpen returned %d", status));
    goto fail;
  }

  {
    BC_STATUS recovery_status;
    guint8 recovery_active;
    guint32 recovery_replay_bytes;
    operation = "DtsGetRecoveryStatus";
    status = DtsGetRecoveryStatus(self->device,
                                  &self->recovery_generation_seen,
                                  &recovery_status, &recovery_active,
                                  &recovery_replay_bytes);
    if (status != BC_STS_SUCCESS || recovery_status != BC_STS_SUCCESS ||
        recovery_active) {
      if (status == BC_STS_SUCCESS)
        status = recovery_active ? BC_STS_BUSY : recovery_status;
      goto fail_status;
    }
    self->recovery_needs_sync = FALSE;
    self->recovery_request_headers = FALSE;
    self->recovery_h264_config_size = 0;
    self->recovery_abort_started_us = 0;
  }

  memset(&version, 0, sizeof(version));
  operation = "DtsCrystalHDVersion";
  status = DtsCrystalHDVersion(self->device, &version);
  if (status != BC_STS_SUCCESS)
    goto fail_status;
  self->is_70012 = version.device == 0;
#ifdef GST_CRYSTALHD_MPEG1_EXPERIMENTAL
  if (!gst_crystalhd_mpeg1_device_allowed(version.device)) {
    GST_ELEMENT_ERROR(self, STREAM, FORMAT,
        ("Experimental MPEG-1 requires BCM70015"),
        ("DtsCrystalHDVersion device selector was %u", version.device));
    goto fail;
  }
#endif
  if (subtype == BC_MSUBTYPE_DIVX && self->is_70012) {
    GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                      ("MPEG-4 Part 2 requires BCM70015"),
                      ("BCM70012 does not expose the validated DIVX decoder"));
    goto fail;
  }
  if (self->active_scale_width != 0 && self->is_70012) {
    GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                      ("CrystalHD hardware scaling requires BCM70015"),
                      ("BCM70012 does not implement DtsSetScaleParams"));
    goto fail;
  }

  operation = "DtsSetInputFormat";
  status = DtsSetInputFormat(self->device, &input_format);
  if (status != BC_STS_SUCCESS)
    goto fail_status;

  if (self->active_scale_width != 0) {
    BC_SCALING_PARAMS scale_params;

    memset(&scale_params, 0, sizeof(scale_params));
    scale_params.sWidth = self->active_scale_width;
    operation = "DtsSetScaleParams";
    status = DtsSetScaleParams(self->device, &scale_params);
    if (status != BC_STS_SUCCESS)
      goto fail_status;
  }

  operation = "DtsOpenDecoder";
  status = DtsOpenDecoder(self->device, BC_STREAM_TYPE_ES);
  if (status != BC_STS_SUCCESS)
    goto fail_status;
  self->decoder_open = TRUE;

  operation = "DtsSetColorSpace(YUY2)";
  status = DtsSetColorSpace(self->device, OUTPUT_MODE422_YUY2);
  if (status != BC_STS_SUCCESS)
    goto fail_status;

  operation = "DtsStartDecoder";
  status = DtsStartDecoder(self->device);
  if (status != BC_STS_SUCCESS)
    goto fail_status;
  self->decoder_started = TRUE;

  operation = "DtsStartCapture";
  status = DtsStartCapture(self->device);
  if (status != BC_STS_SUCCESS)
    goto fail_status;

  if (self->output_control_fd >= 0 && DtsDupOutputPollFd != NULL) {
    int descriptor = -1;
    status = DtsDupOutputPollFd(self->device, &descriptor);
    g_mutex_lock(&self->output_lock);
    if (status == BC_STS_SUCCESS)
      self->output_poll_fd = descriptor;
    g_mutex_unlock(&self->output_lock);
    GST_DEBUG_OBJECT(self, "optional output readiness: %d (status %d)",
                      descriptor >= 0 && status == BC_STS_SUCCESS, status);
  }

  if (codec_data_mapped)
    gst_buffer_unmap(gst_value_get_buffer(codec_data_value), &codec_data_map);

  GST_INFO_OBJECT(self, "opened BCM7001%u for caps %" GST_PTR_FORMAT,
                  self->is_70012 ? 2 : 5, state->caps);
  gst_crystalhd_resume_output(self);
  return TRUE;

fail_status:
  GST_ELEMENT_ERROR(self, LIBRARY, INIT,
                    ("Could not initialize CrystalHD decoder: %s",
                     gst_crystalhd_status_hint(status)),
                    ("%s returned %d", operation, status));
fail:
  if (codec_data_mapped)
    gst_buffer_unmap(gst_value_get_buffer(codec_data_value), &codec_data_map);
  gst_crystalhd_close_device(self);
  return FALSE;
}

static GstFlowReturn gst_crystalhd_drain(GstVideoDecoder *decoder);
static GstFlowReturn gst_crystalhd_parse(GstVideoDecoder *decoder,
    GstVideoCodecFrame *frame, GstAdapter *adapter, gboolean at_eos);

static gboolean
gst_crystalhd_set_format(GstVideoDecoder *decoder, GstVideoCodecState *state)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  GstVideoCodecState *retained_state = gst_video_codec_state_ref(state);
  guint64 generation = self->generation;
  gboolean active, result = FALSE;

  /* GstVideoDecoder does not drain before set_format. Keep the old state
   * installed until every accepted old picture has reached downstream,
   * including a copied frame whose token retired before finish_frame returns.
   * Retain state first: the flush/reopen caller may alias input_state. */
  if (self->output_flow != GST_FLOW_OK || gst_crystalhd_is_flushing(self))
    goto done;
  if (self->codec.vc1_bdu && self->parse_adapter != NULL &&
      gst_adapter_available(self->parse_adapter) != 0) {
    GstFlowReturn flow;
    /* parse_available leaves a current frame allocated when our parser
     * requests more data. All complete boundaries were already consumed:
     * the unchanged adapter contains at most one undelimited picture (or
     * trailing headers). Finalize it through the same public parser API
     * used at EOS, while the OLD codec/state and timestamps still apply.
     * Never access the base class's private current_frame or adapters. */
    if (!self->parse_pending || decoder->input_segment.rate < 0.0 ||
        gst_video_decoder_get_pending_frame_size(decoder) != 0) {
      GST_ELEMENT_ERROR(self, STREAM, DECODE,
                        ("Cannot finalize pending VC-1 input at a format change"),
                        ("A successful forward parser boundary is required"));
      goto done;
    }
    flow = gst_crystalhd_parse(decoder, NULL, self->parse_adapter, TRUE);
    if ((flow != GST_FLOW_OK && flow != GST_VIDEO_DECODER_FLOW_NEED_DATA) ||
        generation != self->generation || gst_crystalhd_is_flushing(self))
      goto done;
    if (gst_adapter_available(self->parse_adapter) != 0)
      goto done;
  }
  g_mutex_lock(&self->output_lock);
  active = self->output_active;
  g_mutex_unlock(&self->output_lock);
  if (self->decoder_started &&
      (!g_queue_is_empty(&self->timestamps) || self->need_second_field || active)) {
    if (gst_crystalhd_drain(decoder) != GST_FLOW_OK)
      goto done;
  }
  if (generation != self->generation || gst_crystalhd_is_flushing(self) ||
      self->output_flow != GST_FLOW_OK)
    goto done;
  result = gst_crystalhd_reopen_format(decoder, retained_state, FALSE);
done:
  gst_video_codec_state_unref(retained_state);
  return result;
}

static GstFlowReturn
gst_crystalhd_parse(GstVideoDecoder *decoder, GstVideoCodecFrame *frame,
                    GstAdapter *adapter, gboolean at_eos)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  gsize available = gst_adapter_available(adapter);
  const guint8 *data;
  gsize size;

  (void)frame;
  g_set_object(&self->parse_adapter, adapter);
  self->parse_pending = FALSE;
  if (available == 0)
    return GST_VIDEO_DECODER_FLOW_NEED_DATA;
  if (available > 16 * 1024 * 1024) {
    GST_ELEMENT_ERROR(decoder, STREAM, DECODE,
                      ("VC-1 picture exceeds the input limit"), (NULL));
    return GST_FLOW_ERROR;
  }
  data = gst_adapter_map(adapter, available);
  size = gst_crystalhd_vc1_frame_size(data, available, at_eos);
  gst_adapter_unmap(adapter);
  if (size != 0) {
    GstFlowReturn flow;
    gst_video_decoder_add_to_frame(decoder, size);
    /* have_frame adds one recursive lock around handle_frame. Preserve
     * that known extra level only for waits made by this callback. */
    self->parsing_frame = TRUE;
    flow = gst_video_decoder_have_frame(decoder);
    self->parsing_frame = FALSE;
    return flow;
  }
  if (at_eos)
    gst_adapter_flush(adapter, available); /* trailing headers, no picture */
  else
    self->parse_pending = TRUE;
  return GST_VIDEO_DECODER_FLOW_NEED_DATA;
}

static GstFlowReturn
gst_crystalhd_handle_frame(GstVideoDecoder *decoder,
                           GstVideoCodecFrame *frame)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  CrystalHdTimestamp *entry;
  GstMapInfo map;
  BC_STATUS status = BC_STS_ERROR;
  GstFlowReturn flow;
  gint64 deadline;
  guint64 hardware_timestamp;
  const guint8 *data;
  gsize size;
  gsize base_reservation;
  gsize reservation;
  guint8 *padded = NULL;
  guint64 generation = self->generation;
  guint lock_levels = self->parsing_frame ? 2 : 1;
  CrystalHdPayloadStatus payload;
  gboolean recovery_active = FALSE;
  gboolean recovery_changed = FALSE;
  gboolean h264_has_parameter_sets = FALSE;
  gboolean submitted_recovery_sync = FALSE;
  CrystalHdMpeg2Picture mpeg2_picture = { 0 };

  if (!self->decoder_started || frame->input_buffer == NULL)
    return gst_video_decoder_drop_frame(decoder, frame);

  flow = gst_crystalhd_reconcile_recovery(self, frame, &recovery_active,
                                          &recovery_changed);
  if (flow != GST_FLOW_OK) {
    gst_video_decoder_drop_frame(decoder, frame);
    return flow;
  }
  if (self->recovery_request_headers) {
    /* handle_frame runs on the upstream streaming thread.  Emit here rather
     * than from output-side reconciliation to avoid reversing pad locks. */
    gst_pad_push_event(GST_VIDEO_DECODER_SINK_PAD(decoder),
        gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE,
                                                     TRUE, 0));
    self->recovery_request_headers = FALSE;
  }
  if (self->recovery_needs_sync &&
      !GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT(frame)) {
    gst_video_decoder_request_sync_point(
        decoder, frame, GST_VIDEO_DECODER_REQUEST_SYNC_POINT_DISCARD_INPUT);
    return gst_video_decoder_drop_frame(decoder, frame);
  }

  if (!gst_buffer_map(frame->input_buffer, &map, GST_MAP_READ))
    return gst_video_decoder_drop_frame(decoder, frame);

  data = map.data;
  size = map.size;
#ifdef GST_CRYSTALHD_MPEG1_EXPERIMENTAL
  {
    const gchar *reason = NULL;
    if (!gst_crystalhd_mpeg1_access_unit_valid(data, size, &reason)) {
      GST_ELEMENT_ERROR(self, STREAM, DECODE,
          ("Experimental MPEG-1 rejected the compressed access unit"),
          ("%s; no picture bytes submitted to DIL",
           reason ? reason : "invalid MPEG-1 framing"));
      gst_buffer_unmap(frame->input_buffer, &map);
      gst_video_decoder_drop_frame(decoder, frame);
      return GST_FLOW_ERROR;
    }
  }
#endif
  payload = gst_crystalhd_codec_payload(&self->codec, &data, &size);
  if (payload == CRYSTALHD_PAYLOAD_INVALID) {
    gst_buffer_unmap(frame->input_buffer, &map);
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("Invalid compressed picture framing"), (NULL));
    gst_video_decoder_drop_frame(decoder, frame);
    return GST_FLOW_ERROR;
  }
  if (self->codec.subtype == BC_MSUBTYPE_MPEG2VIDEO) {
    guint missing_rows;
    const gchar *drop_reason = NULL;
    if (GST_BUFFER_FLAG_IS_SET(frame->input_buffer, GST_BUFFER_FLAG_DISCONT)) {
      self->mpeg2_reference_pictures = 0;
      self->mpeg2_frame_reference_tracking = TRUE;
    }
    crystalhd_mpeg2_picture_headers(data, size, &mpeg2_picture);
    if (mpeg2_picture.have_sequence) {
      self->codec.mpeg2_height = mpeg2_picture.height;
      self->codec.mpeg2_profile = 0;
      self->codec.mpeg2_progressive_sequence = FALSE;
    }
    if (mpeg2_picture.have_sequence_extension) {
      self->codec.mpeg2_profile = mpeg2_picture.profile;
      self->codec.mpeg2_progressive_sequence = mpeg2_picture.progressive_sequence;
    }
    /* Field-pair buffers, separately submitted fields and unproven headers
     * keep their existing admission semantics. Once such input is seen,
     * accepted-buffer counts cannot prove reference-frame availability.
     */
    if (!gst_crystalhd_mpeg2_known_frame(&self->codec, &mpeg2_picture))
      self->mpeg2_frame_reference_tracking = FALSE;
    missing_rows = gst_crystalhd_mpeg2_missing_rows(&self->codec, &mpeg2_picture);
    if (missing_rows != 0) {
      drop_reason = "coded frame is missing whole macroblock rows";
      if (mpeg2_picture.picture_type == 1 || mpeg2_picture.picture_type == 2) {
        self->mpeg2_reference_pictures = 0;
        self->mpeg2_frame_reference_tracking = TRUE;
      }
    } else if (mpeg2_picture.picture_type == 2 &&
               self->mpeg2_frame_reference_tracking &&
               self->mpeg2_reference_pictures == 0 &&
               (mpeg2_picture.slice_prediction & 1) != 0) {
      drop_reason = "P slice macroblock uses an unavailable reference";
    }
    /* A single accepted I/P anchor is the backward reference for following
     * B pictures in coded order. Only reject positive macroblock prediction
     * use, not picture types or optional/open GOP flags: backward-only and
     * intra B pictures can be decoded without an earlier forward reference.
     */
    if (mpeg2_picture.picture_type == 3 && self->mpeg2_frame_reference_tracking &&
        ((self->mpeg2_reference_pictures == 0 && mpeg2_picture.slice_prediction != 0) ||
         (self->mpeg2_reference_pictures == 1 && (mpeg2_picture.slice_prediction & 1) != 0)))
      drop_reason = "B slice macroblock uses an unavailable reference";
    if (drop_reason != NULL) {
      self->mpeg2_dropped_inputs++;
      GST_ELEMENT_WARNING(self, STREAM, DECODE,
          ("Skipping undecodable MPEG-2 input frame %u: %s",
           frame->system_frame_number, drop_reason),
          ("Input frame %u: %s; missing rows %u; accepted references %u; "
           "picture was not submitted to hardware",
           frame->system_frame_number, drop_reason, missing_rows, self->mpeg2_reference_pictures));
      gst_buffer_unmap(frame->input_buffer, &map);
      /* Keep standard drop/QoS accounting, but this unsubmitted picture has
       * no presentation time. In particular, do not advance the base-class
       * timestamp estimator ahead of older pictures still in hardware.
       */
      GST_VIDEO_CODEC_FRAME_SET_DECODE_ONLY(frame);
      return gst_video_decoder_drop_frame(decoder, frame);
    }
  }
  if (self->codec.subtype == BC_MSUBTYPE_H264)
    h264_has_parameter_sets =
        gst_crystalhd_h264_has_parameter_sets(data, size);
  if (self->recovery_needs_sync && self->codec.subtype == BC_MSUBTYPE_H264 &&
      self->recovery_h264_config_size == 0 &&
      !h264_has_parameter_sets)
    goto recovery_drop;
  if (!gst_crystalhd_input_reservation(self->codec.subtype, size,
                                      self->input_metadata_size,
                                      &base_reservation)) {
    gst_buffer_unmap(frame->input_buffer, &map);
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("Compressed picture exceeds CrystalHD input ring capacity"),
                      ("Picture: %" G_GSIZE_FORMAT " bytes; metadata: %"
                       G_GSIZE_FORMAT " bytes; ring: 1048576 bytes",
                       size, self->input_metadata_size));
    gst_video_decoder_drop_frame(decoder, frame);
    return GST_FLOW_ERROR;
  }
  if (self->codec.subtype == BC_MSUBTYPE_WMV3 ||
      self->codec.subtype == BC_MSUBTYPE_WVC1) {
    /* The library probes a four-byte startcode even for valid sub-word
     * ASF pictures. Supply readable padding without submitting extra data.
     */
    padded = g_malloc0(size + 4);
    memcpy(padded, data, size);
    data = padded;
  }

  hardware_timestamp = self->next_hardware_timestamp;
  self->next_hardware_timestamp += CRYSTALHD_TIMESTAMP_STEP;
  deadline = g_get_monotonic_time() + GST_CRYSTALHD_INPUT_TIMEOUT_US;

  for (;;) {
    flow = gst_crystalhd_reconcile_recovery(self, frame, &recovery_active,
                                            &recovery_changed);
    if (flow != GST_FLOW_OK)
      goto input_flow_error;
    if (self->recovery_request_headers) {
      gst_pad_push_event(GST_VIDEO_DECODER_SINK_PAD(decoder),
          gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE,
                                                       TRUE, 0));
      self->recovery_request_headers = FALSE;
    }
    if (self->recovery_needs_sync &&
        !GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT(frame))
      goto recovery_drop;
    if (self->recovery_needs_sync && self->codec.subtype == BC_MSUBTYPE_H264 &&
        self->recovery_h264_config_size == 0 && !h264_has_parameter_sets)
      goto recovery_drop;
    reservation = base_reservation;
    if (self->recovery_needs_sync && self->recovery_h264_config_size != 0 &&
        !gst_crystalhd_add_recovery_config_reservation(
            self->recovery_h264_config_size, &reservation)) {
      flow = GST_FLOW_ERROR;
      goto input_flow_error;
    }
    if (recovery_active) {
      flow = gst_crystalhd_wait_input_progress(self, generation, deadline,
                                               lock_levels);
      if (flow != GST_FLOW_OK)
        goto input_flow_error;
      continue;
    }
    flow = gst_crystalhd_wait_input_space(self, reservation, deadline,
                                         lock_levels);
    if (flow != GST_FLOW_OK)
      goto input_flow_error;
    flow = gst_crystalhd_reconcile_recovery(self, frame, &recovery_active,
                                            &recovery_changed);
    if (flow != GST_FLOW_OK)
      goto input_flow_error;
    if (self->recovery_request_headers) {
      gst_pad_push_event(GST_VIDEO_DECODER_SINK_PAD(decoder),
          gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE,
                                                       TRUE, 0));
      self->recovery_request_headers = FALSE;
    }
    if (self->recovery_needs_sync &&
        !GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT(frame))
      goto recovery_drop;
    if (self->recovery_needs_sync && self->codec.subtype == BC_MSUBTYPE_H264 &&
        self->recovery_h264_config_size == 0 && !h264_has_parameter_sets)
      goto recovery_drop;
    {
      gsize current_reservation = base_reservation;
      if (self->recovery_needs_sync && self->recovery_h264_config_size != 0 &&
          !gst_crystalhd_add_recovery_config_reservation(
              self->recovery_h264_config_size, &current_reservation)) {
        flow = GST_FLOW_ERROR;
        goto input_flow_error;
      }
      if (current_reservation > reservation)
        continue;
    }
    if (recovery_active)
      continue;
    submitted_recovery_sync = self->recovery_needs_sync;
    /* Once submission starts, this session is no longer an empty flush. */
    self->input_flushed = FALSE;
    status = DtsProcInput(self->device, (guint8 *)data, size,
                          hardware_timestamp, 0);
    flow = gst_crystalhd_reconcile_recovery(self, frame, &recovery_active,
                                            &recovery_changed);
    if (flow != GST_FLOW_OK)
      goto input_flow_error;
    /* A successful call can straddle the PM epoch: its AU may have reached
     * either the queue that recovery flushed or the reconstructed decoder.
     * It cannot safely own a timestamp in either case, and resubmitting it
     * could duplicate an AU that the new decoder already accepted. Retire it
     * before applying the new epoch's H.264 gate; the persistent cache
     * capability reported by the library governs the next sync AU. */
    if (recovery_active || recovery_changed)
      goto recovery_drop;
    if (self->recovery_request_headers) {
      gst_pad_push_event(GST_VIDEO_DECODER_SINK_PAD(decoder),
          gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE,
                                                       TRUE, 0));
      self->recovery_request_headers = FALSE;
    }
    if (self->recovery_needs_sync &&
        !GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT(frame))
      goto recovery_drop;
    if (self->recovery_needs_sync && self->codec.subtype == BC_MSUBTYPE_H264 &&
        self->recovery_h264_config_size == 0 && !h264_has_parameter_sets)
      goto recovery_drop;
    if (status == BC_STS_IO_USER_ABORT || status == BC_STS_PWR_MGMT) {
      /* The call may have queued only part of this AU before PM became
       * visible. Recovery flushes it; never resubmit that same AU. */
      self->recovery_needs_sync = TRUE;
      goto recovery_drop;
    }
    if (status == BC_STS_SUCCESS && !recovery_active && !recovery_changed) {
      if (submitted_recovery_sync) {
        self->recovery_needs_sync = FALSE;
        self->recovery_h264_config_size = 0;
      }
      break;
    }
    if (status != BC_STS_BUSY && status != BC_STS_SUCCESS)
      break;

    flow = gst_crystalhd_wait_input_progress(self, generation, deadline,
                                             lock_levels);
    if (flow != GST_FLOW_OK)
      goto input_flow_error;
  }
  g_free(padded);
  gst_buffer_unmap(frame->input_buffer, &map);

  if (status != BC_STS_SUCCESS) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("CrystalHD rejected compressed input: %s",
                       gst_crystalhd_status_hint(status)),
                      ("DtsProcInput returned %d", status));
    gst_video_decoder_drop_frame(decoder, frame);
    return GST_FLOW_ERROR;
  }

  if (self->codec.subtype == BC_MSUBTYPE_MPEG2VIDEO &&
      self->mpeg2_frame_reference_tracking &&
      (mpeg2_picture.picture_type == 1 || mpeg2_picture.picture_type == 2))
    self->mpeg2_reference_pictures = MIN(self->mpeg2_reference_pictures + 1, 2U);

  if (payload == CRYSTALHD_PAYLOAD_NOT_CODED) {
    /* An uncoded VOP has no output picture, but its header still advances the
     * decoder's MPEG-4 timing state (including the state used by later B-VOPs).
     * Give it a unique transport token without creating an expected-output
     * mapping.  It also breaks the proof needed to infer a missing Simple
     * timestamp from consecutive output ordinals. */
    self->have_simple_picture_number = FALSE;
    gst_crystalhd_wake_output(self);
    return gst_video_decoder_drop_frame(decoder, frame);
  }

  /* Only accepted input owns a token. Output polling during BUSY must not
   * consume a provisional mapping or finish the current unaccepted frame.
   */
  entry = g_new0(CrystalHdTimestamp, 1);
  entry->hardware_timestamp = hardware_timestamp;
  entry->frame_number = frame->system_frame_number;
  entry->pts = frame->pts;
  entry->duration = frame->duration;
  g_queue_push_tail(&self->timestamps, entry);
  GST_LOG_OBJECT(self, "submitted token=%" G_GUINT64_FORMAT " pending=%u",
                 hardware_timestamp, self->timestamps.length);
  /* GstVideoDecoder keeps its own queued reference; get_frame() supplies the
   * reference consumed by finish_frame() when this picture is received.
   */
  gst_video_codec_frame_unref(frame);
  gst_crystalhd_wake_output(self);
  return self->output_flow;

recovery_drop:
  self->recovery_needs_sync = TRUE;
  gst_video_decoder_request_sync_point(
      decoder, frame, GST_VIDEO_DECODER_REQUEST_SYNC_POINT_DISCARD_INPUT);
  g_free(padded);
  gst_buffer_unmap(frame->input_buffer, &map);
  return gst_video_decoder_drop_frame(decoder, frame);

input_flow_error:
  g_free(padded);
  gst_buffer_unmap(frame->input_buffer, &map);
  if (generation != self->generation)
    gst_video_codec_frame_unref(frame);
  else
    gst_video_decoder_drop_frame(decoder, frame);
  return flow;
}

static gboolean
gst_crystalhd_flush(GstVideoDecoder *decoder)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);

  /* The base resets frames even when this vfunc returns FALSE. Therefore
   * cancellation must reach genuine quiescence, not a timed false return. */
  gst_crystalhd_quiesce_output(self, FALSE);
  self->generation++;
  self->output_flow = GST_FLOW_OK;
  self->draining = FALSE;
  self->output_eos = FALSE;
  self->last_delivery_us = 0;
  self->progressive_field_sequence = FALSE;
  g_clear_object(&self->parse_adapter);
  self->parse_pending = FALSE;
  if (self->device != NULL && !self->input_flushed) {
    /* Decoder-only flush can leave BCM70015's old firmware session unable
     * to output after an in-flight seek. Recreate the device from retained
     * caps/metadata. Keep token numbering monotonic across this boundary.
     * reopen_format retains its argument before releasing input_state.
     * This is intentional discard, not a natural caps change: never drain
     * old pictures after the flushing seek has canceled their delivery.
     */
    if (self->input_state == NULL ||
        !gst_crystalhd_reopen_format(decoder, self->input_state, TRUE))
      return FALSE;
    self->input_flushed = TRUE;
  }
  gst_crystalhd_clear_timestamps(self);
  self->need_second_field = FALSE;
  gst_crystalhd_resume_output(self);
  return TRUE;
}

static gboolean
gst_crystalhd_drain_complete(GstCrystalHdDec *self, gboolean active)
{
  return !active && self->drain_idle && self->output_eos &&
         g_queue_is_empty(&self->timestamps) && !self->need_second_field;
}

static GstFlowReturn
gst_crystalhd_drain(GstVideoDecoder *decoder)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  gint64 started;
  gint64 deadline;
  guint64 generation = self->generation;
  BC_STATUS status;
  GstFlowReturn flow = GST_FLOW_OK;
  gboolean active = FALSE;
  gboolean recovery_active = FALSE;
  gboolean recovery_changed = FALSE;

  if (!self->decoder_started || self->input_flushed)
    return GST_FLOW_OK;

  started = g_get_monotonic_time();
  deadline = started + GST_CRYSTALHD_DRAIN_TIMEOUT_US;
  for (;;) {
    flow = gst_crystalhd_reconcile_recovery(self, NULL, &recovery_active,
                                            &recovery_changed);
    if (flow != GST_FLOW_OK)
      return flow;
    if (!recovery_active) {
      flow = gst_crystalhd_wait_input_space(
          self, GST_CRYSTALHD_EOS_RESERVATION, deadline, 1);
      if (flow != GST_FLOW_OK)
        return flow;
      status = DtsFlushInput(self->device, 0);
      flow = gst_crystalhd_reconcile_recovery(
          self, NULL, &recovery_active, &recovery_changed);
      if (flow != GST_FLOW_OK)
        return flow;
      if (status == BC_STS_SUCCESS && !recovery_active && !recovery_changed)
        break;
      if (!recovery_active && !recovery_changed &&
          status != BC_STS_BUSY && status != BC_STS_PWR_MGMT &&
          status != BC_STS_IO_USER_ABORT && status != BC_STS_SUCCESS) {
        GST_ELEMENT_ERROR(self, STREAM, DECODE,
                          ("Could not drain CrystalHD decoder: %s",
                           gst_crystalhd_status_hint(status)),
                          ("DtsFlushInput returned %d", status));
        return GST_FLOW_ERROR;
      }
    }
    if (g_get_monotonic_time() >= deadline)
      return GST_FLOW_ERROR;
    flow = gst_crystalhd_wait_output(self, generation, 1000, 1);
    if (flow != GST_FLOW_OK)
      return flow;
  }

  self->draining = TRUE;
  self->output_eos = FALSE;
  self->drain_idle = FALSE;
  gst_crystalhd_wake_output(self);
  for (;;) {
    gint64 remaining;
    flow = gst_crystalhd_reconcile_recovery(self, NULL, &recovery_active,
                                            &recovery_changed);
    if (flow != GST_FLOW_OK)
      goto done;
    if (recovery_changed)
      deadline = g_get_monotonic_time() + GST_CRYSTALHD_DRAIN_TIMEOUT_US;
    if (generation != self->generation || gst_crystalhd_is_flushing(self)) {
      flow = GST_FLOW_FLUSHING;
      goto done;
    }
    if (self->output_flow != GST_FLOW_OK) {
      flow = self->output_flow;
      goto done;
    }
    g_mutex_lock(&self->output_lock);
    active = self->output_active;
    g_mutex_unlock(&self->output_lock);
    /* Tokens retire after copying, not after the clocked downstream push.
     * EOS must wait for both, otherwise the final real picture can vanish. */
    if (gst_crystalhd_drain_complete(self, active))
      break;
    /* Asynchronous input may reach EOS long before clocked playback ends.
     * Bound lack of completed delivery, not the entire remaining movie. */
    deadline = MAX(deadline,
                    self->last_delivery_us + GST_CRYSTALHD_DRAIN_TIMEOUT_US);
    if (gst_crystalhd_playback_paused(self))
      deadline = g_get_monotonic_time() + GST_CRYSTALHD_DRAIN_TIMEOUT_US;
    remaining = gst_crystalhd_drain_remaining_us(deadline, g_get_monotonic_time());
    if (remaining == 0)
      break;
    flow = gst_crystalhd_wait_output(self, generation,
                                    MIN(remaining, (gint64)1000), 1);
    if (flow != GST_FLOW_OK)
      goto done;
  }

  if (!gst_crystalhd_drain_complete(self, active)) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("CrystalHD drain ended before all input pictures were decoded"),
                      ("%u input timestamps remain after %" G_GINT64_FORMAT
                       " ms (delivery active: %d; output idle: %d; "
                       "firmware EOS: %d; "
                       "no-progress limit: 10000 ms)",
                       self->timestamps.length,
                       (g_get_monotonic_time() - started) / 1000, active,
                       self->drain_idle, self->output_eos));
    flow = GST_FLOW_ERROR;
  }
done:
  if (generation == self->generation) {
    self->draining = FALSE;
    if (flow == GST_FLOW_OK) {
      BC_DTS_STATUS final_status;
      if (self->codec.subtype == BC_MSUBTYPE_MPEG2VIDEO && self->mpeg2_dropped_inputs != 0)
        GST_INFO_OBJECT(self, "MPEG-2 input pictures explicitly skipped before hardware: %u",
                         self->mpeg2_dropped_inputs);
      memset(&final_status, 0, sizeof(final_status));
      if (DtsGetDriverStatus(self->device, &final_status) == BC_STS_SUCCESS)
        GST_INFO_OBJECT(self,
            "drain complete: captured=%u dropped=%u repeated=%u "
            "pib-misses=%u input-busy=%u",
            final_status.FramesCaptured, final_status.FramesDropped,
            final_status.FramesRepeated, final_status.PIBMissCount,
            final_status.InputBusyCount);
    }
  }
  return flow;
}

static gboolean
gst_crystalhd_sink_event(GstVideoDecoder *decoder, GstEvent *event)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  GstVideoDecoderClass *parent =
      GST_VIDEO_DECODER_CLASS(gst_crystalhd_dec_parent_class);

  if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START) {
    gboolean result;
    g_mutex_lock(&self->output_lock);
    self->output_paused = TRUE;
    gst_crystalhd_signal_output_locked(self);
    g_mutex_unlock(&self->output_lock);
    /* This event is nonserialized: cancel downstream BEFORE waiting on a
     * worker that may be in the sink's preroll/clock wait. */
    result = parent->sink_event(decoder, event);
    GST_VIDEO_DECODER_STREAM_LOCK(decoder);
    gst_crystalhd_quiesce_output(self, FALSE);
    GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
    return result;
  }
  if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
    GST_VIDEO_DECODER_STREAM_LOCK(decoder);
    gst_crystalhd_quiesce_output(self, FALSE);
    GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  }
  return parent->sink_event(decoder, event);
}

static void
gst_crystalhd_finalize(GObject *object)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(object);
  g_assert(self->output_thread == NULL);
  /* stop() may retain a valid handle when recovery is still active or a
   * lock-only recovery query transiently fails. Once the plugin worker is
   * joined, bypass normal firmware Stop/Close and request terminal library
   * retirement directly. DeviceClose atomically quarantines an unclaimed PM
   * epoch, joins its TX worker, and reserves BUSY for a still-live handle. */
  while (self->device != NULL) {
    BC_STATUS status = DtsDeviceClose(self->device);
    if (status == BC_STS_BUSY) {
      GST_WARNING_OBJECT(self,
          "waiting for retained CrystalHD handle during finalization: %d",
          status);
      g_usleep(1000);
    } else {
      self->device = NULL;
      self->decoder_started = FALSE;
      self->decoder_open = FALSE;
    }
  }
  gst_crystalhd_clear_timestamps(self);
  if (self->output_poll_fd >= 0)
    close(self->output_poll_fd);
  if (self->output_control_fd >= 0)
    close(self->output_control_fd);
  g_clear_pointer(&self->active_finish_trace, gst_crystalhd_finish_trace_unref);
  g_clear_pointer(&self->finish_trace, gst_crystalhd_finish_trace_unref);
  g_cond_clear(&self->output_cond);
  g_mutex_clear(&self->output_lock);
  G_OBJECT_CLASS(gst_crystalhd_dec_parent_class)->finalize(object);
}

static void
gst_crystalhd_dec_class_init(GstCrystalHdDecClass *klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  GstVideoDecoderClass *decoder_class = GST_VIDEO_DECODER_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->set_property = gst_crystalhd_set_property;
  object_class->get_property = gst_crystalhd_get_property;
  object_class->finalize = gst_crystalhd_finalize;

  g_object_class_install_property(
      object_class, PROP_SCALE_WIDTH,
      g_param_spec_uint(
          "scale-width", "Hardware scale width",
          "BCM70015 output width (0 = native; otherwise an even width from "
          "128 through 1918 not exceeding the coded width)",
          0, G_MAXUINT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));

  g_object_class_install_property(
      object_class, PROP_FINISH_TRACE_LIMIT,
      g_param_spec_uint("finish-trace-limit", "Finish-frame trace limit",
          "Development first-N finish-frame spans (0 disables; no media policy change)",
          0, GST_CRYSTALHD_FINISH_TRACE_MAX, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));
  g_signal_new_class_handler("finish-trace-snapshot", G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_CALLBACK(gst_crystalhd_finish_trace_snapshot_action),
      NULL, NULL, NULL, G_TYPE_STRING, 0);

  gst_element_class_set_static_metadata(
#ifdef GST_CRYSTALHD_MPEG1_EXPERIMENTAL
      element_class, "CrystalHD experimental MPEG-1 decoder",
      "Codec/Decoder/Video/Hardware",
      "Explicit BCM70015 MPEG-1 352x288 progressive 25fps experiment; algorithm 1",
#elif defined(GST_CRYSTALHD_PRIVATE_MPEG1_RESEARCH)
      element_class, "CrystalHD private MPEG-1 research decoder",
      "Codec/Decoder/Video/Hardware",
      "Explicit test-only MPEG-1 input through the existing algorithm-1 transport",
#else
      element_class, "CrystalHD hardware decoder", "Codec/Decoder/Video/Hardware",
      "Decodes video with Broadcom BCM70012/BCM70015 hardware",
#endif
      "CrystalHD contributors");
  gst_element_class_add_static_pad_template(element_class, &sink_template);
  gst_element_class_add_static_pad_template(element_class, &src_template);

  decoder_class->start = gst_crystalhd_start;
  decoder_class->stop = gst_crystalhd_stop;
  decoder_class->set_format = gst_crystalhd_set_format;
  decoder_class->getcaps = gst_crystalhd_getcaps;
  decoder_class->parse = gst_crystalhd_parse;
  decoder_class->sink_query = gst_crystalhd_sink_query;
  decoder_class->sink_event = gst_crystalhd_sink_event;
  decoder_class->handle_frame = gst_crystalhd_handle_frame;
  decoder_class->flush = gst_crystalhd_flush;
  decoder_class->finish = gst_crystalhd_drain;
  decoder_class->drain = gst_crystalhd_drain;
}

static void
gst_crystalhd_dec_init(GstCrystalHdDec *self)
{
  g_queue_init(&self->timestamps);
  self->wmv3_timing_valid = TRUE;
  self->wmv3_last_duration = GST_CLOCK_TIME_NONE;
  g_mutex_init(&self->output_lock);
  g_cond_init(&self->output_cond);
  self->output_control_fd = -1;
  self->output_poll_fd = -1;
  self->output_paused = TRUE;
  gst_video_info_init(&self->output_info);
  gst_video_decoder_set_packetized(GST_VIDEO_DECODER(self), TRUE);
}

#if !defined(GST_CRYSTALHD_PRIVATE_MPEG1_RESEARCH) && !defined(GST_CRYSTALHD_MPEG1_EXPERIMENTAL)
static gboolean
plugin_init(GstPlugin *plugin)
{
  GST_DEBUG_CATEGORY_INIT(gst_crystalhd_debug, "crystalhd", 0,
                          "CrystalHD hardware decoder");
  return gst_element_register(plugin, "crystalhddec", GST_RANK_PRIMARY + 1,
                              GST_TYPE_CRYSTALHD_DEC) &&
      gst_element_register(plugin, "crystalhddiqos", GST_RANK_NONE,
                            GST_TYPE_CRYSTALHD_DI_QOS) &&
      gst_element_register(plugin, "crystalhdstagetrace", GST_RANK_NONE,
                            GST_TYPE_CRYSTALHD_STAGE_TRACE);
}

#ifndef PACKAGE
#define PACKAGE "crystalhd"
#endif

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, crystalhd,
                  "Broadcom CrystalHD hardware video decoder", plugin_init,
                  "3.10.0", "LGPL", "crystalhd",
                  "https://github.com/ahnhy1324/crystalhd")
#endif
