/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Clocked barcode/control validation, not display or audible-output proof.
 * Run only against generate-browser-sample.sh's 12-second/360-frame fixture,
 * or timestamp-continuous repetitions of that fixture in --sustain mode.
 * An external timeout remains necessary for an uninterruptible driver close.
 */
#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/video/video.h>
#include <math.h>
#include <string.h>
#include <sys/resource.h>

#define FRAME_COUNT 360U
#define FPS 30U
#define PHASE_TIMEOUT (8 * G_USEC_PER_SEC)
#define MAX_CLOCK_LATE (250 * GST_MSECOND)
#define MAX_AV_SKEW (100 * GST_MSECOND)
/* A demuxer pushes both streams from one task. Audio must not fill its
 * preroll queue before a delayed video decoder has received enough input to
 * produce its first picture. Time and byte caps keep this lookahead bounded. */
#define VIDEO_INPUT_QUEUE "queue max-size-buffers=0 max-size-bytes=16777216 max-size-time=3000000000"
#define AUDIO_INPUT_QUEUE "queue max-size-buffers=0 max-size-bytes=1048576 max-size-time=3000000000"

typedef enum {
  INITIAL, PAUSED, RESUMED, SEEK_FORWARD, HALF_SPEED, DOUBLE_SPEED, RESTORED,
  PHASE_COUNT
} Phase;

static const gchar *phase_names[] = {
  "initial-1x", "paused", "resumed-1x", "seek-6s-1x", "seek-2s-0.5x",
  "seek-8s-2x", "seek-0s-restored-1x"
};

typedef struct {
  GstSegment segment;
  gboolean have_segment;
  gboolean flushing;
  guint32 seqnum;
  guint64 frames;
  GstClockTime last_pts;
  GstClockTime last_end_pts;
  GstClockTime last_raw_pts;
  GstClockTime running_start;
  GstClockTime running_end;
  GstClockTime observed_clock;
} Stream;

typedef struct {
  GMutex mutex;
  GstElement *pipeline;
  gboolean audio_required;
  guint sustain_seconds;
  gboolean failed;
  gchar reason[256];
  Stream video, audio;
  Phase phase;
  guint32 expected_seqnum;
  guint next_identity;
  guint epoch_frames;
  guint64 total_video;
  guint64 total_audio;
  guint64 audio_nonzero;
  guint64 phase_audio_start;
  guint64 phase_audio_nonzero_start;
  guint phase_frames;
  GstClockTime first_pts, last_pts;
  GstClockTime first_clock, last_clock;
  GstClockTime maximum_skew;
  guint av_pairs;
  gboolean phase_passed[PHASE_COUNT];
  GstBuffer *last_buffer;
  GstVideoInfo last_info;
} Audit;

typedef struct { Audit *audit; gboolean audio; } StreamData;

static void fail(Audit *audit, const gchar *reason) {
  if (!audit->failed) {
    audit->failed = TRUE;
    g_strlcpy(audit->reason, reason, sizeof(audit->reason));
  }
}

static gdouble phase_rate(Phase phase) {
  return phase == HALF_SPEED ? 0.5 : phase == DOUBLE_SPEED ? 2.0 : 1.0;
}

static void init_stream(Stream *stream) {
  memset(stream, 0, sizeof(*stream));
  gst_segment_init(&stream->segment, GST_FORMAT_TIME);
  stream->last_pts = GST_CLOCK_TIME_NONE;
  stream->last_end_pts = GST_CLOCK_TIME_NONE;
  stream->last_raw_pts = GST_CLOCK_TIME_NONE;
  stream->running_start = stream->running_end = GST_CLOCK_TIME_NONE;
  stream->observed_clock = GST_CLOCK_TIME_NONE;
}

static void init_audit(Audit *audit, gboolean audio) {
  memset(audit, 0, sizeof(*audit));
  g_mutex_init(&audit->mutex);
  init_stream(&audit->video);
  init_stream(&audit->audio);
  audit->audio_required = audio;
  audit->first_pts = audit->last_pts = GST_CLOCK_TIME_NONE;
  audit->first_clock = audit->last_clock = GST_CLOCK_TIME_NONE;
}

static void begin_phase(Audit *audit, Phase phase) {
  audit->phase = phase;
  audit->phase_frames = 0;
  audit->first_pts = audit->last_pts = GST_CLOCK_TIME_NONE;
  audit->first_clock = audit->last_clock = GST_CLOCK_TIME_NONE;
  audit->phase_audio_start = audit->total_audio;
  audit->phase_audio_nonzero_start = audit->audio_nonzero;
  audit->maximum_skew = 0;
  audit->av_pairs = 0;
}

static gboolean current_stream(const Audit *audit, const Stream *stream) {
  return stream->have_segment && !stream->flushing &&
      (audit->expected_seqnum == 0 || stream->seqnum == audit->expected_seqnum);
}

/* Pure production decisions, also called directly by --self-test. */
static void observe_video(Audit *audit, guint identity, GstClockTime pts,
                          GstClockTime clock, GstClockTime running) {
  if (audit->failed || !current_stream(audit, &audit->video)) return;
  if (audit->phase == PAUSED) {
    fail(audit, "Video rendered after PAUSED state was confirmed");
    return;
  }
  const guint expected_barcode = audit->sustain_seconds != 0 ?
      audit->next_identity % FRAME_COUNT : audit->next_identity;
  if (!GST_CLOCK_TIME_IS_VALID(pts) || !GST_CLOCK_TIME_IS_VALID(clock) ||
      !GST_CLOCK_TIME_IS_VALID(running) || identity >= FRAME_COUNT ||
      audit->next_identity != gst_util_uint64_scale_round(pts, FPS, GST_SECOND) ||
      identity != expected_barcode || (audit->sustain_seconds != 0 &&
      audit->next_identity >= audit->sustain_seconds * FPS)) {
    gchar reason[256];
    g_snprintf(reason, sizeof(reason), "Missing/duplicate/stale barcode: got=%u expected=%u "
        "PTS=%" G_GUINT64_FORMAT " clock=%" G_GUINT64_FORMAT " running=%" G_GUINT64_FORMAT,
        identity, audit->next_identity, pts, clock, running);
    fail(audit, reason);
    return;
  }
  if (fabs(audit->video.segment.rate - phase_rate(audit->phase)) > 0.000001 ||
      fabs(audit->video.segment.applied_rate - 1.0) > 0.000001) {
    fail(audit, "Video segment did not apply the requested playback rate");
    return;
  }
  if (clock + 5 * GST_MSECOND < running || clock > running + MAX_CLOCK_LATE) {
    fail(audit, "Video did not reach the clocked sink within the scheduling bound");
    return;
  }
  if (GST_CLOCK_TIME_IS_VALID(audit->last_clock) && clock < audit->last_clock) {
    fail(audit, "Video observation clock regressed inside a phase");
    return;
  }
  if (!GST_CLOCK_TIME_IS_VALID(audit->first_pts)) {
    audit->first_pts = pts;
    audit->first_clock = clock;
  }
  audit->last_pts = pts;
  audit->last_clock = clock;
  audit->video.last_pts = pts;
  audit->video.running_start = running;
  audit->video.observed_clock = clock;
  ++audit->next_identity;
  ++audit->phase_frames;
  ++audit->epoch_frames;
  ++audit->total_video;
  if (audit->audio_required && current_stream(audit, &audit->audio) &&
      GST_CLOCK_TIME_IS_VALID(audit->audio.running_start) &&
      clock >= audit->first_clock + 500 * GST_MSECOND) {
    const GstClockTime start = audit->audio.running_start;
    const GstClockTime end = audit->audio.running_end;
    const GstClockTime skew = running < start ? start - running :
                              running > end ? running - end : 0;
    if (clock > audit->audio.observed_clock + MAX_CLOCK_LATE || skew > MAX_AV_SKEW) {
      gchar reason[256];
      g_snprintf(reason, sizeof(reason), "A/V scheduling gap: video=%.3fms audio=[%.3f,%.3f]ms "
          "clock=%.3fms audio-observed=%.3fms skew=%.3fms", (gdouble)running / GST_MSECOND,
          (gdouble)start / GST_MSECOND, (gdouble)end / GST_MSECOND,
          (gdouble)clock / GST_MSECOND, (gdouble)audit->audio.observed_clock / GST_MSECOND,
          (gdouble)skew / GST_MSECOND);
      fail(audit, reason);
      return;
    }
    audit->maximum_skew = MAX(audit->maximum_skew, skew);
    ++audit->av_pairs;
  }
}

static gboolean phase_complete(Audit *audit, gboolean final) {
  if (audit->failed || audit->phase == PAUSED || audit->phase_frames < 31 ||
      !GST_CLOCK_TIME_IS_VALID(audit->first_clock) ||
      audit->last_clock < audit->first_clock || audit->last_pts < audit->first_pts)
    return FALSE;
  const gdouble wall = (gdouble)(audit->last_clock - audit->first_clock) / GST_SECOND;
  const gdouble media = (gdouble)(audit->last_pts - audit->first_pts) / GST_SECOND;
  if (wall < 1.0 || media < 1.0) return FALSE;
  const gdouble expected = wall * phase_rate(audit->phase);
  if (fabs(media - expected) > MAX(2.0 / FPS, expected * 0.10)) {
    fail(audit, "Actual output progression did not match the requested rate");
    return FALSE;
  }
  if (audit->audio_required &&
      (audit->total_audio <= audit->phase_audio_start + 3 ||
       audit->audio_nonzero <= audit->phase_audio_nonzero_start || audit->av_pairs < 4)) {
    if (final) fail(audit, "Insufficient real audio/video clock evidence");
    return FALSE;
  }
  audit->phase_passed[audit->phase] = TRUE;
  return TRUE;
}

static gboolean final_complete(Audit *audit) {
  if (audit->failed || audit->phase != RESTORED || audit->next_identity != FRAME_COUNT ||
      audit->epoch_frames != FRAME_COUNT || !phase_complete(audit, TRUE)) {
    if (!audit->failed) fail(audit, "EOS arrived without the complete final 360-frame replay");
    return FALSE;
  }
  for (guint phase = 0; phase < PHASE_COUNT; ++phase)
    if (!audit->phase_passed[phase]) {
      fail(audit, "EOS arrived without every required playback control phase");
      return FALSE;
    }
  if (audit->audio_required &&
      (!GST_CLOCK_TIME_IS_VALID(audit->audio.last_pts) ||
       audit->audio.last_pts < 11900 * GST_MSECOND)) {
    fail(audit, "Audio ended before the final video interval");
    return FALSE;
  }
  return TRUE;
}

static gboolean sustain_complete(Audit *audit) {
  const guint expected = audit->sustain_seconds * FPS;
  if (audit->failed || audit->sustain_seconds == 0 || audit->phase != INITIAL ||
      audit->next_identity != expected || audit->total_video != expected ||
      audit->epoch_frames != expected || !phase_complete(audit, TRUE)) {
    if (!audit->failed) fail(audit, "EOS arrived without every sustained frame in absolute timestamp order");
    return FALSE;
  }
  const GstClockTime duration = (GstClockTime)audit->sustain_seconds * GST_SECOND;
  if (audit->audio_required &&
      (!GST_CLOCK_TIME_IS_VALID(audit->audio.last_pts) ||
       !GST_CLOCK_TIME_IS_VALID(audit->audio.last_end_pts) ||
       audit->audio.last_end_pts <= audit->audio.last_pts ||
       audit->audio.last_pts >= duration ||
       audit->audio.last_end_pts <= audit->last_pts ||
       audit->audio.last_end_pts > duration + MAX_AV_SKEW)) {
    gchar reason[256];
    g_snprintf(reason, sizeof(reason), "Audio did not cover the final sustained video interval: "
        "last=[%.6f,%.6f]s target=%.6fs", (gdouble)audit->audio.last_pts / GST_SECOND,
        (gdouble)audit->audio.last_end_pts / GST_SECOND, (gdouble)duration / GST_SECOND);
    fail(audit, reason);
    return FALSE;
  }
  return TRUE;
}

static GstPadProbeReturn stream_event(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
  StreamData *stream_data = data;
  Audit *audit = stream_data->audit;
  Stream *stream = stream_data->audio ? &audit->audio : &audit->video;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
  (void)pad;
  g_mutex_lock(&audit->mutex);
  if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START) {
    stream->flushing = TRUE;
    stream->have_segment = FALSE;
  } else if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
    stream->flushing = FALSE;
    stream->last_pts = GST_CLOCK_TIME_NONE;
    stream->last_end_pts = GST_CLOCK_TIME_NONE;
    stream->last_raw_pts = GST_CLOCK_TIME_NONE;
    stream->running_start = stream->running_end = GST_CLOCK_TIME_NONE;
  } else if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
    const GstSegment *segment;
    gst_event_parse_segment(event, &segment);
    if (segment->format != GST_FORMAT_TIME) fail(audit, "Non-time output segment");
    else {
      stream->segment = *segment;
      stream->seqnum = gst_event_get_seqnum(event);
      stream->have_segment = TRUE;
      stream->flushing = FALSE;
    }
  }
  g_mutex_unlock(&audit->mutex);
  return GST_PAD_PROBE_OK;
}

static GstClockTime running_clock(Audit *audit) {
  GstClock *clock = gst_element_get_clock(audit->pipeline);
  if (clock == NULL) return GST_CLOCK_TIME_NONE;
  const GstClockTime now = gst_clock_get_time(clock);
  const GstClockTime base = gst_element_get_base_time(audit->pipeline);
  gst_object_unref(clock);
  return now >= base ? now - base : GST_CLOCK_TIME_NONE;
}

static gboolean barcode_buffer(GstBuffer *buffer, const GstVideoInfo *info, guint *identity) {
  GstVideoFrame frame;
  *identity = 0;
  if (buffer == NULL || GST_VIDEO_INFO_FORMAT(info) != GST_VIDEO_FORMAT_YUY2 ||
      GST_VIDEO_INFO_WIDTH(info) < 288 || GST_VIDEO_INFO_HEIGHT(info) < 32 ||
      !gst_video_frame_map(&frame, info, buffer, GST_MAP_READ)) return FALSE;
  const gint stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
  gboolean valid = stride >= GST_VIDEO_INFO_WIDTH(info) * 2;
  if (valid) {
    const guint8 *row = (const guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&frame, 0) + 16 * stride;
    for (guint bit = 0; bit < 9; ++bit)
      if (row[(16 + 24 * bit) * 2] > 128) *identity |= 1U << bit;
    valid = row[248 * 2] >= 200 && row[272 * 2] <= 50;
  }
  gst_video_frame_unmap(&frame);
  return valid;
}

static void video_handoff(GstElement *sink, GstBuffer *buffer, GstPad *pad, gpointer data) {
  Audit *audit = ((StreamData *)data)->audit;
  GstCaps *caps = gst_pad_get_current_caps(pad);
  GstVideoInfo info;
  gboolean valid = caps != NULL && gst_video_info_from_caps(&info, caps) &&
      GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_YUY2 &&
      GST_VIDEO_INFO_WIDTH(&info) >= 288 && GST_VIDEO_INFO_HEIGHT(&info) >= 32 &&
      GST_VIDEO_INFO_FPS_N(&info) == 30 && GST_VIDEO_INFO_FPS_D(&info) == 1;
  guint identity = 0;
  (void)sink;
  const GstClockTime clock = running_clock(audit);
  if (valid) valid = barcode_buffer(buffer, &info, &identity);
  if (caps != NULL) gst_caps_unref(caps);
  g_mutex_lock(&audit->mutex);
  if (!valid) fail(audit, "Invalid YUY2 geometry or barcode reference pixels");
  else {
    GstClockTime pts = GST_BUFFER_PTS(buffer);
    GstClockTime running = gst_segment_to_running_time(&audit->video.segment, GST_FORMAT_TIME, pts);
    /* MP4 may shift raw timestamps to keep B-frame DTS nonnegative. The
     * barcode encodes stream time, not that container/segment offset. */
    GstClockTime media = gst_segment_to_stream_time(&audit->video.segment, GST_FORMAT_TIME, pts);
    const guint64 before = audit->total_video;
    observe_video(audit, identity, media, clock, running);
    if (!audit->failed && audit->total_video != before) {
      gst_buffer_replace(&audit->last_buffer, buffer);
      audit->last_info = info;
    }
  }
  g_mutex_unlock(&audit->mutex);
}

static void audio_handoff(GstElement *sink, GstBuffer *buffer, GstPad *pad, gpointer data) {
  Audit *audit = ((StreamData *)data)->audit;
  GstCaps *caps = gst_pad_get_current_caps(pad);
  GstAudioInfo info;
  GstMapInfo map;
  const GstClockTime pts = GST_BUFFER_PTS(buffer);
  const GstClockTime duration = GST_BUFFER_DURATION(buffer);
  const GstClockTime clock = running_clock(audit);
  gboolean nonzero = FALSE;
  gboolean valid = caps != NULL && gst_audio_info_from_caps(&info, caps) &&
      GST_AUDIO_INFO_RATE(&info) == 48000 && GST_AUDIO_INFO_BPF(&info) > 0 &&
      gst_buffer_get_size(buffer) > 0 &&
      gst_buffer_get_size(buffer) % GST_AUDIO_INFO_BPF(&info) == 0 &&
      GST_CLOCK_TIME_IS_VALID(pts) && GST_CLOCK_TIME_IS_VALID(duration) &&
      duration > 0 && duration <= 250 * GST_MSECOND &&
      pts < GST_CLOCK_TIME_NONE - duration;
  (void)sink;
  if (valid) {
    valid = gst_buffer_map(buffer, &map, GST_MAP_READ);
    if (valid) {
      for (gsize byte = 0; byte < map.size; ++byte)
        if (map.data[byte] != 0) { nonzero = TRUE; break; }
      gst_buffer_unmap(buffer, &map);
    }
  }
  if (caps != NULL) gst_caps_unref(caps);
  g_mutex_lock(&audit->mutex);
  if (!valid) fail(audit, "Invalid decoded 48kHz audio buffer/timestamps");
  else if (current_stream(audit, &audit->audio)) {
    Stream *audio = &audit->audio;
    GstClockTime start, end;
    if (audit->phase == PAUSED) fail(audit, "Audio rendered after PAUSED state was confirmed");
    else if (fabs(audio->segment.rate - phase_rate(audit->phase)) > 0.000001 ||
             fabs(audio->segment.applied_rate - 1.0) > 0.000001)
      fail(audit, "Audio segment did not apply the requested playback rate");
    else if (!gst_segment_clip(&audio->segment, GST_FORMAT_TIME, pts, pts + duration, &start, &end))
      fail(audit, "Audio rendered outside the active segment");
    else {
      const GstClockTime running = gst_segment_to_running_time(&audio->segment, GST_FORMAT_TIME, start);
      const GstClockTime running_end = running + gst_util_uint64_scale(end - start, 1000,
          (guint64)(1000 * fabs(audio->segment.rate)));
      if ((GST_CLOCK_TIME_IS_VALID(audio->last_raw_pts) && pts <= audio->last_raw_pts) ||
          !GST_CLOCK_TIME_IS_VALID(running) || !GST_CLOCK_TIME_IS_VALID(clock) ||
          clock + 5 * GST_MSECOND < running || clock > running + MAX_CLOCK_LATE)
        fail(audit, "Audio timestamps regressed or missed the scheduling bound");
      else {
        audio->last_raw_pts = pts;
        audio->last_pts = gst_segment_to_stream_time(&audio->segment, GST_FORMAT_TIME, start);
        audio->last_end_pts = audio->last_pts + end - start;
        audio->running_start = running;
        audio->running_end = running_end;
        audio->observed_clock = clock;
        ++audit->total_audio;
        if (nonzero) ++audit->audio_nonzero;
      }
    }
  }
  g_mutex_unlock(&audit->mutex);
}

static gboolean has_factory(const gchar *name) {
  GstElementFactory *factory = gst_element_factory_find(name);
  if (factory == NULL) return FALSE;
  gst_object_unref(factory);
  return TRUE;
}

static gboolean report_message(GstMessage *message) {
  if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_ERROR) return TRUE;
  GError *error = NULL;
  gst_message_parse_error(message, &error, NULL);
  g_printerr("GStreamer error in %s: %s\n", GST_OBJECT_NAME(message->src),
             error != NULL ? error->message : "unknown error");
  g_clear_error(&error);
  return FALSE;
}

static gboolean seek_phase(Audit *audit, Phase phase, guint target) {
  GstEvent *event = gst_event_new_seek(phase_rate(phase), GST_FORMAT_TIME,
      GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, GST_SEEK_TYPE_SET,
      (gint64)target * GST_SECOND, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
  const guint32 seqnum = gst_util_seqnum_next();
  gst_event_set_seqnum(event, seqnum);
  g_mutex_lock(&audit->mutex);
  begin_phase(audit, phase);
  audit->expected_seqnum = seqnum;
  audit->next_identity = target * FPS;
  audit->epoch_frames = 0;
  g_mutex_unlock(&audit->mutex);
  return gst_element_send_event(audit->pipeline, event);
}

static int run(const gchar *filename, const gchar *decoder, const gchar *audio_decoder,
               guint timeout_seconds, guint sustain_seconds) {
  Audit audit;
  init_audit(&audit, audio_decoder != NULL);
  audit.sustain_seconds = sustain_seconds;
  GError *error = NULL;
  gchar *description = g_strdup_printf(
      "filesrc name=source ! qtdemux name=demux "
      "demux.video_0 ! " VIDEO_INPUT_QUEUE " ! "
      "h264parse ! video/x-h264,stream-format=byte-stream,alignment=au ! %s name=decoder ! "
      "videoconvert ! video/x-raw,format=YUY2 ! "
      "fakesink name=video sync=true qos=false max-lateness=-1 signal-handoffs=true enable-last-sample=false %s",
      decoder, audio_decoder != NULL ?
      "demux.audio_0 ! " AUDIO_INPUT_QUEUE " ! "
      "aacparse ! AUDIO_DECODER ! audioconvert ! audio/x-raw,format=S16LE,rate=48000,layout=interleaved ! "
      "fakesink name=audio sync=true qos=false max-lateness=-1 signal-handoffs=true enable-last-sample=false" : "");
  if (audio_decoder != NULL) {
    gchar **parts = g_strsplit(description, "AUDIO_DECODER", 2);
    gchar *replacement = g_strconcat(parts[0], audio_decoder, parts[1], NULL);
    g_strfreev(parts);
    g_free(description);
    description = replacement;
  }
  audit.pipeline = gst_parse_launch(description, &error);
  g_free(description);
  if (error != NULL || audit.pipeline == NULL) {
    g_printerr("Cannot construct controls pipeline: %s\n", error != NULL ? error->message : "unknown");
    g_clear_error(&error);
    if (audit.pipeline != NULL) gst_object_unref(audit.pipeline);
    g_mutex_clear(&audit.mutex);
    return 1;
  }
  GstElement *source = gst_bin_get_by_name(GST_BIN(audit.pipeline), "source");
  g_object_set(source, "location", filename, NULL);
  gst_object_unref(source);
  GstElement *video = gst_bin_get_by_name(GST_BIN(audit.pipeline), "video");
  GstElement *audio = audio_decoder != NULL ? gst_bin_get_by_name(GST_BIN(audit.pipeline), "audio") : NULL;
  StreamData video_data = {&audit, FALSE}, audio_data = {&audit, TRUE};
  GstPad *video_pad = gst_element_get_static_pad(video, "sink");
  gst_pad_add_probe(video_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH,
                    stream_event, &video_data, NULL);
  g_signal_connect(video, "handoff", G_CALLBACK(video_handoff), &video_data);
  GstPad *audio_pad = audio != NULL ? gst_element_get_static_pad(audio, "sink") : NULL;
  if (audio != NULL) {
    gst_pad_add_probe(audio_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH,
                      stream_event, &audio_data, NULL);
    g_signal_connect(audio, "handoff", G_CALLBACK(audio_handoff), &audio_data);
  }
  GstClock *clock = gst_system_clock_obtain();
  gst_pipeline_use_clock(GST_PIPELINE(audit.pipeline), clock);
  gst_object_unref(clock);
  GstBus *bus = gst_element_get_bus(audit.pipeline);
  gboolean success = gst_element_set_state(audit.pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
  gboolean eos = FALSE;
  gint64 phase_started = g_get_monotonic_time();
  const gint64 started_at = phase_started;
  gint64 progress_at = phase_started;
  gint64 report_at = phase_started;
  guint64 last_total = 0;
  const gint64 deadline = phase_started + (gint64)timeout_seconds * G_USEC_PER_SEC;
  gint64 paused_at = 0;
  GstBuffer *paused_buffer = NULL;
  GstVideoInfo paused_info;
  guint paused_identity = 0;
  while (success && !eos && g_get_monotonic_time() < deadline) {
    GstMessage *message = gst_bus_timed_pop_filtered(bus, 10 * GST_MSECOND,
        GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    if (message != NULL) {
      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) eos = TRUE;
      else success = report_message(message);
      gst_message_unref(message);
    }
    g_mutex_lock(&audit.mutex);
    if (audit.failed) success = FALSE;
    gboolean complete = success && sustain_seconds == 0 && audit.phase != PAUSED &&
        phase_complete(&audit, FALSE);
    Phase phase = audit.phase;
    if (audit.failed) success = FALSE;
    if (sustain_seconds != 0 && audit.total_video != last_total) {
      last_total = audit.total_video;
      progress_at = g_get_monotonic_time();
    }
    g_mutex_unlock(&audit.mutex);
    if (!success || eos) break;
    const gint64 now = g_get_monotonic_time();
    if (sustain_seconds != 0) {
      if (now - progress_at > PHASE_TIMEOUT) {
        g_printerr("Sustained playback stalled without an exact new video frame for 8 seconds\n");
        success = FALSE;
      }
      if (now - report_at >= 30 * G_USEC_PER_SEC) {
        struct rusage usage;
        const glong maximum_rss = getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_maxrss : -1;
        g_mutex_lock(&audit.mutex);
        g_print("Sustain elapsed=%.1fs video=%" G_GUINT64_FORMAT "/%u audio=%" G_GUINT64_FORMAT
            " A/V max skew=%.3fms maxRSS=%ldKiB\n", (gdouble)(now - started_at) / G_USEC_PER_SEC,
            audit.total_video, sustain_seconds * FPS, audit.total_audio,
            (gdouble)audit.maximum_skew / GST_MSECOND, maximum_rss);
        g_mutex_unlock(&audit.mutex);
        report_at = now;
      }
      continue;
    }
    if (phase == PAUSED && now - paused_at >= G_USEC_PER_SEC) {
      guint retained_identity = 0;
      if (!barcode_buffer(paused_buffer, &paused_info, &retained_identity) ||
          retained_identity != paused_identity) {
        g_printerr("Retained paused picture changed its barcode pixels\n");
        success = FALSE;
        break;
      }
      gst_clear_buffer(&paused_buffer);
      g_mutex_lock(&audit.mutex);
      audit.phase_passed[PAUSED] = TRUE;
      begin_phase(&audit, RESUMED);
      g_mutex_unlock(&audit.mutex);
      success = gst_element_set_state(audit.pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
      phase_started = now;
      g_print("Control paused: retained frame%u stable for %.3f seconds; resuming\n",
          paused_identity, (gdouble)(now - paused_at) / G_USEC_PER_SEC);
    } else if (complete && phase != RESTORED) {
      g_mutex_lock(&audit.mutex);
      g_print("Control %s: %u exact frames, media=%.3fs clock=%.3fs A/V max skew=%.3fms\n",
          phase_names[phase], audit.phase_frames,
          (gdouble)(audit.last_pts - audit.first_pts) / GST_SECOND,
          (gdouble)(audit.last_clock - audit.first_clock) / GST_SECOND,
          (gdouble)audit.maximum_skew / GST_MSECOND);
      g_mutex_unlock(&audit.mutex);
      if (phase == INITIAL) {
        success = gst_element_set_state(audit.pipeline, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE &&
            gst_element_get_state(audit.pipeline, NULL, NULL, 5 * GST_SECOND) == GST_STATE_CHANGE_SUCCESS;
        g_mutex_lock(&audit.mutex);
        begin_phase(&audit, PAUSED);
        if (audit.last_buffer != NULL) {
          paused_buffer = gst_buffer_ref(audit.last_buffer);
          paused_info = audit.last_info;
          paused_identity = audit.next_identity - 1;
        } else success = FALSE;
        g_mutex_unlock(&audit.mutex);
        paused_at = g_get_monotonic_time();
      } else {
        const Phase next = phase == RESUMED ? SEEK_FORWARD : phase == SEEK_FORWARD ? HALF_SPEED :
                           phase == HALF_SPEED ? DOUBLE_SPEED : RESTORED;
        const guint target = next == SEEK_FORWARD ? 6 : next == HALF_SPEED ? 2 : next == DOUBLE_SPEED ? 8 : 0;
        success = seek_phase(&audit, next, target);
      }
      phase_started = g_get_monotonic_time();
    } else if (phase != RESTORED && now - phase_started > PHASE_TIMEOUT) {
      g_printerr("Control %s timed out without sufficient output/segment evidence\n", phase_names[phase]);
      success = FALSE;
    }
  }
  /* Stop streaming before reading final evidence or releasing callback data. */
  gst_element_set_state(audit.pipeline, GST_STATE_NULL);
  gst_clear_buffer(&paused_buffer);
  g_mutex_lock(&audit.mutex);
  success = success && eos && (sustain_seconds != 0 ? sustain_complete(&audit) : final_complete(&audit));
  if (audit.failed) g_printerr("Controls failed: %s\n", audit.reason);
  else if (!success) g_printerr("Controls failed: error, missing EOS, or deadline\n");
  if (sustain_seconds != 0) {
    struct rusage usage;
    const glong maximum_rss = getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_maxrss : -1;
    g_print("Sustain decoder=%s audio=%s: elapsed=%.3fs video=%" G_GUINT64_FORMAT "/%u "
        "audio=%" G_GUINT64_FORMAT " A/V max skew=%.3fms maxRSS=%ldKiB EOS=%s result=%s\n", decoder,
        audio_decoder != NULL ? audio_decoder : "disabled",
        (gdouble)(g_get_monotonic_time() - started_at) / G_USEC_PER_SEC,
        audit.total_video, sustain_seconds * FPS, audit.total_audio,
        (gdouble)audit.maximum_skew / GST_MSECOND, maximum_rss,
        eos ? "yes" : "no", success ? "PASS" : "FAIL");
    if (audit.audio_required && GST_CLOCK_TIME_IS_VALID(audit.audio.last_pts) &&
        GST_CLOCK_TIME_IS_VALID(audit.audio.last_end_pts))
      g_print("Sustain final audio interval=[%.6f,%.6f]s, final video interval=[%.6f,%u]s "
          "(overlap evidence, not an exact audio sample-count claim)\n",
          (gdouble)audit.audio.last_pts / GST_SECOND,
          (gdouble)audit.audio.last_end_pts / GST_SECOND,
          (gdouble)audit.last_pts / GST_SECOND, sustain_seconds);
  } else {
    g_print("Controls decoder=%s audio=%s: video=%" G_GUINT64_FORMAT " audio=%" G_GUINT64_FORMAT
            " final-replay=%u/360 EOS=%s result=%s\n", decoder,
            audio_decoder != NULL ? audio_decoder : "disabled", audit.total_video,
            audit.total_audio, audit.epoch_frames, eos ? "yes" : "no", success ? "PASS" : "FAIL");
  }
  g_mutex_unlock(&audit.mutex);
  gst_object_unref(video_pad);
  if (audio_pad != NULL) gst_object_unref(audio_pad);
  gst_object_unref(video);
  if (audio != NULL) gst_object_unref(audio);
  gst_object_unref(bus);
  gst_object_unref(audit.pipeline);
  gst_clear_buffer(&audit.last_buffer);
  g_mutex_clear(&audit.mutex);
  return success ? 0 : 1;
}

static void test_segment(Audit *audit, guint start, gdouble rate) {
  init_stream(&audit->video);
  audit->video.have_segment = TRUE;
  audit->video.segment.start = (guint64)start * GST_SECOND;
  audit->video.segment.rate = rate;
  audit->next_identity = start * FPS;
}

typedef struct {
  GQueue held;
  guint received;
  gboolean releasing;
  gboolean started;
} PrerollDelay;

typedef struct {
  guint frames;
  gboolean invalid;
} PrerollCount;

typedef struct {
  GstPad *video, *audio;
  gboolean complete;
} PrerollProducer;

/* Unlike a sleep, this models a decoder that cannot emit its first output
 * until it has actually received 1.5 seconds of compressed-picture input. */
static GstPadProbeReturn delay_preroll(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
  PrerollDelay *delay = data;
  if (delay->releasing || delay->started) return GST_PAD_PROBE_OK;
  g_queue_push_tail(&delay->held, gst_buffer_ref(GST_PAD_PROBE_INFO_BUFFER(info)));
  if (++delay->received < 45) return GST_PAD_PROBE_DROP;
  delay->releasing = TRUE;
  while (!g_queue_is_empty(&delay->held)) {
    GstBuffer *buffer = g_queue_pop_head(&delay->held);
    if (gst_pad_push(pad, buffer) != GST_FLOW_OK) break;
  }
  delay->releasing = FALSE;
  delay->started = TRUE;
  return GST_PAD_PROBE_DROP;
}

static void count_preroll(GstElement *sink, GstBuffer *buffer, GstPad *pad, gpointer data) {
  PrerollCount *count = data;
  (void)sink;
  (void)pad;
  if (GST_BUFFER_OFFSET(buffer) != count->frames) count->invalid = TRUE;
  ++count->frames;
}

static gpointer produce_interleaved(gpointer data) {
  PrerollProducer *producer = data;
  GstCaps *caps = gst_caps_new_empty_simple("application/x-controls-preroll");
  GstSegment segment;
  gst_segment_init(&segment, GST_FORMAT_TIME);
  segment.stop = 3 * GST_SECOND;
  GstPad *pads[] = {producer->video, producer->audio};
  for (guint stream = 0; stream < 2; ++stream) {
    if (!gst_pad_send_event(pads[stream], gst_event_new_stream_start(
          stream == 0 ? "controls-preroll-video" : "controls-preroll-audio")) ||
        !gst_pad_send_event(pads[stream], gst_event_new_caps(caps)) ||
        !gst_pad_send_event(pads[stream], gst_event_new_segment(&segment))) {
      gst_caps_unref(caps);
      return NULL;
    }
  }
  gst_caps_unref(caps);
  /* One producer, like qtdemux: a full audio input queue blocks the next
   * video packet too. Independent appsrc threads would miss this deadlock. */
  for (guint frame = 0; frame < 90; ++frame) {
    for (guint stream = 0; stream < 2; ++stream) {
      GstBuffer *buffer = gst_buffer_new_allocate(NULL, 16, NULL);
      GST_BUFFER_OFFSET(buffer) = frame;
      GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(frame, GST_SECOND, FPS);
      GST_BUFFER_DURATION(buffer) = GST_SECOND / FPS;
      if (gst_pad_chain(pads[stream], buffer) != GST_FLOW_OK) return NULL;
    }
  }
  producer->complete = gst_pad_send_event(producer->video, gst_event_new_eos()) &&
      gst_pad_send_event(producer->audio, gst_event_new_eos());
  return NULL;
}

static gboolean preroll_queue_test(gboolean old_limits) {
  gchar *description = g_strdup_printf(
      "%s name=videoq ! fakesink name=video sync=false async=true signal-handoffs=true "
      "%s name=audioq ! fakesink name=audio sync=false async=true signal-handoffs=true",
      old_limits ? "queue max-size-buffers=16 max-size-bytes=0 max-size-time=0" : VIDEO_INPUT_QUEUE,
      old_limits ? "queue max-size-buffers=32 max-size-bytes=0 max-size-time=0" : AUDIO_INPUT_QUEUE);
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch(description, &error);
  g_free(description);
  if (pipeline == NULL || error != NULL) {
    g_clear_error(&error);
    if (pipeline != NULL) gst_object_unref(pipeline);
    return FALSE;
  }
  GstElement *videoq = gst_bin_get_by_name(GST_BIN(pipeline), "videoq");
  GstElement *audioq = gst_bin_get_by_name(GST_BIN(pipeline), "audioq");
  GstElement *video = gst_bin_get_by_name(GST_BIN(pipeline), "video");
  GstElement *audio = gst_bin_get_by_name(GST_BIN(pipeline), "audio");
  GstPad *delayed_pad = gst_element_get_static_pad(videoq, "src");
  PrerollDelay delay = {G_QUEUE_INIT, 0, FALSE, FALSE};
  PrerollCount video_count = {0, FALSE}, audio_count = {0, FALSE};
  PrerollProducer producer = {gst_element_get_static_pad(videoq, "sink"),
      gst_element_get_static_pad(audioq, "sink"), FALSE};
  gst_pad_add_probe(delayed_pad, GST_PAD_PROBE_TYPE_BUFFER, delay_preroll, &delay, NULL);
  g_signal_connect(video, "handoff", G_CALLBACK(count_preroll), &video_count);
  g_signal_connect(audio, "handoff", G_CALLBACK(count_preroll), &audio_count);
  GstBus *bus = gst_element_get_bus(pipeline);
  gboolean ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
  GThread *thread = g_thread_new("interleaved-preroll", produce_interleaved, &producer);
  GstStateChangeReturn state = gst_element_get_state(pipeline, NULL, NULL,
      old_limits ? GST_SECOND : 5 * GST_SECOND);
  guint queued_audio = 0;
  g_object_get(audioq, "current-level-buffers", &queued_audio, NULL);
  gboolean eos = FALSE;
  if (!old_limits && state == GST_STATE_CHANGE_SUCCESS) {
    GstMessage *message = gst_bus_timed_pop_filtered(bus, 2 * GST_SECOND,
        GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    eos = message != NULL && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message != NULL) gst_message_unref(message);
  }
  /* NULL unblocks the intentionally starved old queue before joining its
   * shared producer. No detached thread survives either result. */
  gst_element_set_state(pipeline, GST_STATE_NULL);
  g_thread_join(thread);
  if (old_limits)
    ok = ok && state == GST_STATE_CHANGE_ASYNC && queued_audio == 32 &&
        delay.received > 0 && delay.received < 45 &&
        video_count.frames == 0 && audio_count.frames == 0 && !producer.complete;
  else
    ok = ok && state == GST_STATE_CHANGE_SUCCESS && eos && producer.complete &&
        delay.received == 45 && video_count.frames == 90 && audio_count.frames == 90 &&
        !video_count.invalid && !audio_count.invalid;
  g_print("Preroll queue regression (%s): video=%u audio=%u delayed-input=%u result=%s\n",
      old_limits ? "old starvation reproduced" : "bounded3s", video_count.frames,
      audio_count.frames, delay.received, ok ? "PASS" : "FAIL");
  g_queue_clear_full(&delay.held, (GDestroyNotify)gst_buffer_unref);
  gst_object_unref(producer.video);
  gst_object_unref(producer.audio);
  gst_object_unref(delayed_pad);
  gst_object_unref(videoq);
  gst_object_unref(audioq);
  gst_object_unref(video);
  gst_object_unref(audio);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  return ok;
}

static int sustain_self_test(void) {
  guint tests = 0;
#define REQUIRE(condition) do { ++tests; if (!(condition)) { \
  g_printerr("Sustained self-test failed at line %d\n", __LINE__); return 1; } } while (0)
  Audit audit;
  for (guint fault = 0; fault < 5; ++fault) {
    init_audit(&audit, FALSE);
    audit.sustain_seconds = 24;
    test_segment(&audit, 0, 1.0);
    /* The barcode repeats, but absolute stream time never wraps or skips. */
    const guint count = fault == 4 ? FRAME_COUNT : 2 * FRAME_COUNT;
    for (guint frame = 0; frame < count && !audit.failed; ++frame) {
      guint identity = frame % FRAME_COUNT;
      GstClockTime pts = gst_util_uint64_scale(frame, GST_SECOND, FPS);
      if (frame == FRAME_COUNT) {
        if (fault == 1) identity = 1; /* Wrong picture at the loop boundary. */
        if (fault == 2) pts += 12 * GST_SECOND; /* Missing complete cycle. */
        if (fault == 3) pts = 0; /* Reset PTS can mimic a valid barcode. */
      }
      observe_video(&audit, identity, pts, pts, pts);
    }
    REQUIRE(fault == 0 ? sustain_complete(&audit) : !sustain_complete(&audit) && audit.failed);
    if (fault == 0) {
      audit.audio_required = TRUE;
      audit.total_audio = audit.av_pairs = 10;
      audit.audio_nonzero = 1;
      audit.audio.last_pts = 23980 * GST_MSECOND;
      audit.audio.last_end_pts = 24 * GST_SECOND;
      REQUIRE(sustain_complete(&audit));
      /* AAC decoder priming/tail may leave less than one final video frame;
       * validate a real interval overlap, not an exact decoded sample count. */
      audit.audio.last_pts = 23957333333ULL;
      audit.audio.last_end_pts = 23978666666ULL;
      REQUIRE(sustain_complete(&audit));
      audit.audio.last_end_pts = audit.last_pts;
      REQUIRE(!sustain_complete(&audit) && audit.failed); /* No strict overlap. */
      audit.failed = FALSE;
      audit.audio.last_pts = 24 * GST_SECOND;
      audit.audio.last_end_pts = 24020 * GST_MSECOND;
      REQUIRE(!sustain_complete(&audit) && audit.failed); /* Only after video ended. */
      audit.failed = FALSE;
      audit.audio.last_pts = 23980 * GST_MSECOND;
      audit.audio.last_end_pts = 24101 * GST_MSECOND;
      REQUIRE(!sustain_complete(&audit) && audit.failed); /* Excessive tail. */
    }
    g_mutex_clear(&audit.mutex);
  }
  init_audit(&audit, FALSE);
  audit.sustain_seconds = 12;
  test_segment(&audit, 0, 1.0);
  for (guint frame = 0; frame <= FRAME_COUNT; ++frame) {
    const GstClockTime pts = gst_util_uint64_scale(frame, GST_SECOND, FPS);
    observe_video(&audit, frame % FRAME_COUNT, pts, pts, pts);
  }
  REQUIRE(audit.failed && !sustain_complete(&audit)); /* Extra frame cannot pass. */
  g_mutex_clear(&audit.mutex);
  g_print("GStreamer sustained hardware-free self-test: %u checks passed\n", tests);
  return 0;
#undef REQUIRE
}

static int self_test(void) {
  guint tests = 0;
#define REQUIRE(condition) do { ++tests; if (!(condition)) { \
  g_printerr("Controls self-test failed at line %d\n", __LINE__); return 1; } } while (0)
  Audit audit;
  init_audit(&audit, FALSE);
  test_segment(&audit, 0, 1.0);
  for (guint frame = 0; frame <= 31; ++frame) {
    GstClockTime pts = gst_util_uint64_scale(frame, GST_SECOND, FPS);
    observe_video(&audit, frame, pts, pts, pts);
  }
  REQUIRE(phase_complete(&audit, FALSE) && !audit.failed);
  g_mutex_clear(&audit.mutex);
  for (guint fault = 0; fault < 5; ++fault) {
    init_audit(&audit, FALSE);
    test_segment(&audit, 6, 1.0);
    begin_phase(&audit, SEEK_FORWARD);
    const guint id = fault == 0 ? 179 : fault == 1 ? 181 : 180;
    const GstClockTime pts = fault == 2 ? GST_CLOCK_TIME_NONE : 6 * GST_SECOND;
    const GstClockTime clock = fault == 3 ? GST_SECOND : 0;
    if (fault == 4) audit.phase = PAUSED;
    observe_video(&audit, id, pts, clock, 0);
    REQUIRE(audit.failed);
    g_mutex_clear(&audit.mutex);
  }
  init_audit(&audit, FALSE);
  test_segment(&audit, 0, 1.0);
  observe_video(&audit, 0, 0, 0, 0);
  observe_video(&audit, 0, 0, 0, 0);
  REQUIRE(audit.failed);
  g_mutex_clear(&audit.mutex);
  init_audit(&audit, FALSE);
  test_segment(&audit, 2, 1.0);
  begin_phase(&audit, HALF_SPEED);
  observe_video(&audit, 60, 2 * GST_SECOND, 0, 0);
  REQUIRE(audit.failed); /* Requested rate ignored by segment. */
  g_mutex_clear(&audit.mutex);
  init_audit(&audit, FALSE);
  test_segment(&audit, 2, 0.5);
  begin_phase(&audit, HALF_SPEED);
  for (guint frame = 60; frame <= 91; ++frame) {
    GstClockTime pts = gst_util_uint64_scale(frame, GST_SECOND, FPS);
    /* Fabricated segment scheduling is not sufficient: actual clock pace
     * must also match the selected half-speed phase. */
    observe_video(&audit, frame, pts, pts - 2 * GST_SECOND, pts - 2 * GST_SECOND);
  }
  REQUIRE(!phase_complete(&audit, FALSE) && audit.failed);
  g_mutex_clear(&audit.mutex);
  init_audit(&audit, TRUE);
  test_segment(&audit, 0, 1.0);
  for (guint frame = 0; frame <= 31; ++frame) {
    GstClockTime pts = gst_util_uint64_scale(frame, GST_SECOND, FPS);
    observe_video(&audit, frame, pts, pts, pts);
  }
  REQUIRE(!phase_complete(&audit, TRUE) && audit.failed); /* No audio. */
  g_mutex_clear(&audit.mutex);
  init_audit(&audit, FALSE);
  test_segment(&audit, 0, 1.0);
  audit.expected_seqnum = 42;
  audit.video.seqnum = 41;
  observe_video(&audit, 0, 0, 0, 0);
  REQUIRE(audit.total_video == 0 && !phase_complete(&audit, FALSE));
  audit.phase = RESTORED;
  REQUIRE(!final_complete(&audit) && audit.failed);
  g_mutex_clear(&audit.mutex);
  for (guint test = 0; test < 2; ++test) {
    init_audit(&audit, FALSE);
    const Phase phase = test == 0 ? HALF_SPEED : DOUBLE_SPEED;
    const guint start = test == 0 ? 2 : 8;
    const guint advances = test == 0 ? 32 : 62;
    begin_phase(&audit, phase);
    test_segment(&audit, start, phase_rate(phase));
    for (guint n = 0; n <= advances; ++n) {
      const GstClockTime pts = gst_util_uint64_scale(start * FPS + n, GST_SECOND, FPS);
      const GstClockTime running = gst_util_uint64_scale(n, GST_SECOND * (test == 0 ? 2 : 1),
                                                        FPS * (test == 0 ? 1 : 2));
      observe_video(&audit, start * FPS + n, pts, running, running);
    }
    REQUIRE(phase_complete(&audit, FALSE) && !audit.failed);
    g_mutex_clear(&audit.mutex);
  }
  for (guint test = 0; test < 2; ++test) {
    init_audit(&audit, TRUE);
    test_segment(&audit, 0, 1.0);
    audit.audio.have_segment = TRUE;
    for (guint n = 0; n <= 31; ++n) {
      const GstClockTime pts = gst_util_uint64_scale(n, GST_SECOND, FPS);
      audit.audio.running_start = test == 0 ? pts + 200 * GST_MSECOND : 0;
      audit.audio.running_end = audit.audio.running_start + 20 * GST_MSECOND;
      audit.audio.observed_clock = test == 0 ? pts : 0;
      observe_video(&audit, n, pts, pts, pts);
    }
    REQUIRE(audit.failed); /* Ahead audio or old/stalled audio cannot pass. */
    g_mutex_clear(&audit.mutex);
  }
  for (guint missing = 0; missing < 2; ++missing) {
    init_audit(&audit, FALSE);
    begin_phase(&audit, RESTORED);
    test_segment(&audit, 0, 1.0);
    for (guint phase = 0; phase < PHASE_COUNT; ++phase) audit.phase_passed[phase] = TRUE;
    for (guint n = 0; n < FRAME_COUNT - missing; ++n) {
      GstClockTime pts = gst_util_uint64_scale(n, GST_SECOND, FPS);
      observe_video(&audit, n, pts, pts, pts);
    }
    REQUIRE(missing ? !final_complete(&audit) : final_complete(&audit));
    g_mutex_clear(&audit.mutex);
  }
  GstVideoInfo info;
  gst_video_info_set_format(&info, GST_VIDEO_FORMAT_YUY2, 320, 64);
  GstBuffer *buffer = gst_buffer_new_allocate(NULL, info.size, NULL);
  GstMapInfo map;
  REQUIRE(gst_buffer_map(buffer, &map, GST_MAP_WRITE));
  memset(map.data, 16, map.size);
  const gsize row = 16 * info.stride[0];
  map.data[row + 16 * 2] = map.data[row + 248 * 2] = 235;
  gst_buffer_unmap(buffer, &map);
  guint identity = 0;
  REQUIRE(barcode_buffer(buffer, &info, &identity) && identity == 1);
  REQUIRE(gst_buffer_map(buffer, &map, GST_MAP_WRITE));
  map.data[row + 16 * 2] = 16;
  gst_buffer_unmap(buffer, &map);
  REQUIRE(barcode_buffer(buffer, &info, &identity) && identity != 1);
  REQUIRE(gst_buffer_map(buffer, &map, GST_MAP_WRITE));
  map.data[row + 248 * 2] = 16;
  gst_buffer_unmap(buffer, &map);
  REQUIRE(!barcode_buffer(buffer, &info, &identity));
  gst_buffer_unref(buffer);
  REQUIRE(preroll_queue_test(TRUE));
  REQUIRE(preroll_queue_test(FALSE));
  g_print("GStreamer controls hardware-free self-test: %u checks passed\n", tests);
  return sustain_self_test();
#undef REQUIRE
}

int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  if (argc == 2 && g_str_equal(argv[1], "--self-test")) return self_test();
  gboolean software = FALSE, audio = FALSE;
  gboolean have_timeout = FALSE;
  const gchar *decoder = NULL;
  guint timeout = 90, sustain_seconds = 0;
  if (argc < 2 || argv[1][0] == '-') goto usage;
  for (gint arg = 2; arg < argc; ++arg) {
    if (g_str_equal(argv[arg], "--software") && !software) software = TRUE;
    else if (g_str_equal(argv[arg], "--audio") && !audio) audio = TRUE;
    else if (g_str_equal(argv[arg], "--decoder") && decoder == NULL && arg + 1 < argc)
      decoder = argv[++arg];
    else if (g_str_equal(argv[arg], "--timeout") && !have_timeout && arg + 1 < argc) {
      gchar *end;
      guint64 parsed = g_ascii_strtoull(argv[++arg], &end, 10);
      if (*argv[arg] == '\0' || *argv[arg] == '-' || *end != '\0' || parsed < 10 || parsed > 3600) goto usage;
      timeout = parsed;
      have_timeout = TRUE;
    } else if (g_str_equal(argv[arg], "--sustain") && sustain_seconds == 0 && arg + 1 < argc) {
      gchar *end;
      guint64 parsed = g_ascii_strtoull(argv[++arg], &end, 10);
      if (*argv[arg] == '\0' || *argv[arg] == '-' || *end != '\0' ||
          parsed < 12 || parsed > 3540 || parsed % 12 != 0) goto usage;
      sustain_seconds = parsed;
    } else goto usage;
  }
  if (sustain_seconds == 0 && timeout > 600) goto usage;
  if (sustain_seconds != 0 && !have_timeout) timeout = sustain_seconds + 60;
  if (sustain_seconds != 0 && timeout < sustain_seconds) goto usage;
  if (decoder != NULL && (!software ||
      (!g_str_equal(decoder, "avdec_h264") && !g_str_equal(decoder, "openh264dec")))) goto usage;
  if (decoder == NULL) decoder = software ? (has_factory("avdec_h264") ? "avdec_h264" : "openh264dec") : "crystalhddec";
  const gchar *audio_decoder = audio ? (has_factory("avdec_aac") ? "avdec_aac" : "faad") : NULL;
  if (!has_factory(decoder) || (audio_decoder != NULL && !has_factory(audio_decoder))) {
    g_printerr("Required explicit decoder is unavailable: video=%s audio=%s\n", decoder,
               audio_decoder != NULL ? audio_decoder : "disabled");
    return 2;
  }
  g_print("Testing clocked sinks only: no visible display or audible-output claim\n");
  return run(argv[1], decoder, audio_decoder, timeout, sustain_seconds);
usage:
  g_printerr("usage: %s BARCODE.mp4 [--software [--decoder avdec_h264|openh264dec]] [--audio] "
      "[--sustain SECONDS] [--timeout SECONDS]\n"
      "  --sustain: continuous 12-second barcode repetitions, multiple of12 within12..3540; no controls\n"
      "  --timeout: controls10..600 (default90), sustain duration..3600 (default duration+60)\n", argv[0]);
  return 2;
}
