/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Private normal-cadence MPEG-1 research frontend. No public registration,
 * caps relabelling, clock forcing, or playback policy tuning.
 * An external process timeout remains mandatory: DIL/kernel close may block.
 * Pad/counter observations are not physical scanout or audible-output proof.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/audio/gstaudioclock.h>
#include <gst/video/video.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

gboolean gst_crystalhd_mpeg1_research_register(void);
gboolean gst_crystalhd_mpeg1_research_self_test(void);
enum { INPUT, DECODED, VIDEO, AUDIO, N_STREAMS };
static const gchar *const names[] = {"decoder:sink-mpeg1-input", "decoder:src",
                                     "video:sink", "audio:sink"};
enum { CAPTURE_WIDTH = 352, CAPTURE_HEIGHT = 288,
       CAPTURE_ROW_BYTES = CAPTURE_WIDTH*2,
       CAPTURE_FRAME_BYTES = CAPTURE_ROW_BYTES*CAPTURE_HEIGHT };
static const guint capture_indices[] = {0, 320, 373, 498, 582, 645, 813, 849};
typedef struct {
  GstClockTime pts, duration;
  GstVideoFormat format;
  gint stride;
} CapturedFrame;
typedef struct {
  gchar *directory;
  guint8 *bytes; /* Eight tight copies; no retained decoder/pool buffers. */
  GstVideoInfo info;
  gboolean have_info, failed;
  guint count;
  gchar reason[160];
  CapturedFrame frames[G_N_ELEMENTS(capture_indices)];
} Capture;
typedef struct {
  GstSegment segment;
  gboolean have_segment, caps_valid, nonzero;
  guint64 count, missing_pts, missing_duration, backwards, eos, segments, clock_samples;
  GstClockTime first, last, end, first_duration, running_start, running_end;
  gint64 first_offset, last_offset, min_offset, max_offset;
  gchar *caps;
  GArray *pts;
} Stream;
typedef struct {
  GMutex lock;
  GstElement *pipeline;
  guint expected;
  gboolean failed;
  gchar reason[256];
  Stream streams[N_STREAMS];
  guint64 av_pairs;
  gint64 av_min_gap, av_max_gap;
  Capture capture;
} Audit;
typedef struct { Audit *audit; guint kind; } ProbeData;

static void capture_fail(Capture *c, const gchar *reason) {
  if (!c->failed) { c->failed = TRUE; g_strlcpy(c->reason, reason, sizeof(c->reason)); }
}
static gboolean capture_init(Capture *c, guint expected) {
  const gchar *directory = g_getenv("CRYSTALHD_MPEG1_TEST_CAPTURE_DIR");
  if (directory == NULL) return TRUE;
  c->directory = g_strdup(directory);
  if (!*directory || !g_file_test(directory, G_FILE_TEST_IS_DIR) ||
      expected <= capture_indices[G_N_ELEMENTS(capture_indices)-1]) {
    capture_fail(c, "capture requires an existing directory and at least 850 video frames");
    return FALSE;
  }
  c->bytes = g_try_malloc((gsize)CAPTURE_FRAME_BYTES*G_N_ELEMENTS(capture_indices));
  if (!c->bytes) { capture_fail(c, "capture RAM allocation failed"); return FALSE; }
  gst_video_info_init(&c->info);
  return TRUE;
}
/* Called under the audit lock at the actual VIDEO:sink probe. Never writes to
 * disk, changes the buffer, or retains a GstBuffer/GstVideoFrame reference. */
static void capture_frame(Capture *c, GstBuffer *buffer, guint64 index) {
  if (!c->directory || c->failed || c->count == G_N_ELEMENTS(capture_indices) ||
      index != capture_indices[c->count]) return;
  GstVideoFormat format = GST_VIDEO_INFO_FORMAT(&c->info);
  if (!c->have_info || GST_VIDEO_INFO_WIDTH(&c->info) != CAPTURE_WIDTH ||
      GST_VIDEO_INFO_HEIGHT(&c->info) != CAPTURE_HEIGHT ||
      (format != GST_VIDEO_FORMAT_UYVY && format != GST_VIDEO_FORMAT_YUY2) ||
      !GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))) {
    capture_fail(c, "capture requires actual 352x288 UYVY/YUY2 caps and valid PTS"); return;
  }
  GstVideoFrame frame;
  if (!gst_video_frame_map(&frame, &c->info, buffer, GST_MAP_READ)) {
    capture_fail(c, "capture GstVideoFrame read map failed"); return;
  }
  gint stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
  const guint8 *source = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
  if (!source || (stride > -CAPTURE_ROW_BYTES && stride < CAPTURE_ROW_BYTES) ||
      GST_VIDEO_FRAME_WIDTH(&frame) != CAPTURE_WIDTH ||
      GST_VIDEO_FRAME_HEIGHT(&frame) != CAPTURE_HEIGHT ||
      GST_VIDEO_FRAME_FORMAT(&frame) != format) {
    capture_fail(c, "capture mapped geometry/format/stride is invalid");
  } else {
    guint8 *destination = c->bytes+(gsize)c->count*CAPTURE_FRAME_BYTES;
    for (guint row = 0; row < CAPTURE_HEIGHT; ++row)
      memcpy(destination+(gsize)row*CAPTURE_ROW_BYTES,
          source+(gssize)row*stride, CAPTURE_ROW_BYTES);
    c->frames[c->count++] = (CapturedFrame){GST_BUFFER_PTS(buffer),
        GST_BUFFER_DURATION(buffer), format, stride};
  }
  gst_video_frame_unmap(&frame);
}
static gboolean capture_write(Capture *c, const gchar *name, const guint8 *data, gsize size) {
  gchar *path = g_build_filename(c->directory, name, NULL);
  int fd = g_open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  g_free(path);
  if (fd < 0) { capture_fail(c, "exclusive capture file creation failed (existing files are never replaced)"); return FALSE; }
  gsize offset = 0;
  while (offset < size) {
    ssize_t written = write(fd, data+offset, size-offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) { capture_fail(c, "capture file write failed"); break; }
    offset += (gsize)written;
  }
  if (close(fd) != 0) capture_fail(c, "capture file close failed");
  return !c->failed;
}
/* Only after successful synchronous NULL teardown: no playback-time disk I/O. */
static gboolean capture_save(Capture *c) {
  if (!c->directory) return TRUE;
  if (c->count != G_N_ELEMENTS(capture_indices)) capture_fail(c, "incomplete selected VIDEO:sink frame capture");
  if (!c->failed) {
    GString *json = g_string_new("{\"capture_pad\":\"video:sink\",\"index_basis\":\"zero-based observed buffer\","
        "\"width\":352,\"height\":288,\"saved_stride\":704,\"frames\":[\n");
    for (guint n = 0; n < c->count && !c->failed; ++n) {
      CapturedFrame *f = &c->frames[n];
      gchar *name = g_strdup_printf("frame-%u.raw", capture_indices[n]);
      if (capture_write(c, name, c->bytes+(gsize)n*CAPTURE_FRAME_BYTES, CAPTURE_FRAME_BYTES)) {
        g_string_append_printf(json, "%s{\"index\":%u,\"file\":\"%s\",\"format\":\"%s\","
            "\"pts_ns\":%" G_GUINT64_FORMAT ",\"duration_ns\":", n ? ",\n" : "",
            capture_indices[n], name, gst_video_format_to_string(f->format), f->pts);
        if (GST_CLOCK_TIME_IS_VALID(f->duration)) g_string_append_printf(json, "%" G_GUINT64_FORMAT, f->duration);
        else g_string_append(json, "null");
        g_string_append_printf(json, ",\"source_stride\":%d,\"bytes\":%u}", f->stride, CAPTURE_FRAME_BYTES);
      }
      g_free(name);
    }
    g_string_append(json, "\n]}\n");
    if (!c->failed) capture_write(c, "frames.json", (const guint8 *)json->str, json->len);
    g_string_free(json, TRUE);
  }
  g_print("MPEG1_CAPTURE_RESULT captured=%u expected=%u active-bytes=%u result=%s reason=%s\n",
      c->count, (guint)G_N_ELEMENTS(capture_indices), CAPTURE_FRAME_BYTES*(guint)G_N_ELEMENTS(capture_indices),
      c->failed ? "FAIL" : "PASS", c->failed ? c->reason : "actual video:sink bytes; saved after NULL");
  return !c->failed;
}
static void capture_clear(Capture *c) {
  g_free(c->bytes); g_free(c->directory);
}
static void fail_locked(Audit *a, const gchar *text) {
  if (!a->failed) { a->failed = TRUE; g_strlcpy(a->reason, text, sizeof(a->reason)); }
}
static gint64 difference(GstClockTime a, GstClockTime b) {
  return a >= b ? (gint64)MIN(a-b, (guint64)G_MAXINT64)
                : -(gint64)MIN(b-a, (guint64)G_MAXINT64);
}
static GstClockTime running_clock(GstElement *pipeline) {
  GstClock *clock = gst_element_get_clock(pipeline);
  GstClockTime result = GST_CLOCK_TIME_NONE;
  if (clock != NULL) {
    GstClockTime now = gst_clock_get_time(clock), base = gst_element_get_base_time(pipeline);
    if (GST_CLOCK_TIME_IS_VALID(now) && GST_CLOCK_TIME_IS_VALID(base) && now >= base)
      result = now-base;
    gst_object_unref(clock);
  }
  return result;
}
static gboolean input_caps(GstCaps *caps) {
  gint version = 0;
  gboolean parsed = FALSE, system = TRUE;
  if (caps == NULL || gst_caps_get_size(caps) != 1) return FALSE;
  const GstStructure *s = gst_caps_get_structure(caps, 0);
  return gst_structure_has_name(s, "video/mpeg") &&
      gst_structure_get_int(s, "mpegversion", &version) && version == 1 &&
      gst_structure_get_boolean(s, "parsed", &parsed) && parsed &&
      gst_structure_get_boolean(s, "systemstream", &system) && !system;
}
static GstPadProbeReturn observe(GstPad *pad, GstPadProbeInfo *info, gpointer opaque) {
  ProbeData *p = opaque;
  Audit *a = p->audit;
  Stream *s = &a->streams[p->kind];
  GstPadProbeType type = GST_PAD_PROBE_INFO_TYPE(info);
  (void)pad;
  if (type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    g_mutex_lock(&a->lock);
    switch (GST_EVENT_TYPE(event)) {
      case GST_EVENT_CAPS: {
        GstCaps *caps = NULL;
        gst_event_parse_caps(event, &caps);
        g_free(s->caps); s->caps = gst_caps_to_string(caps);
        if (p->kind == INPUT) s->caps_valid = input_caps(caps);
        else if (p->kind == AUDIO) {
          GstAudioInfo audio;
          s->caps_valid = gst_audio_info_from_caps(&audio, caps) &&
              GST_AUDIO_INFO_RATE(&audio) > 0 && GST_AUDIO_INFO_BPF(&audio) > 0;
        } else {
          GstVideoInfo video;
          s->caps_valid = gst_video_info_from_caps(&video, caps);
          if (p->kind == VIDEO && a->capture.directory) {
            a->capture.have_info = s->caps_valid;
            if (s->caps_valid) a->capture.info = video;
          }
        }
        if (!s->caps_valid) fail_locked(a, "invalid caps or MPEG-1 identity changed");
        break;
      }
      case GST_EVENT_SEGMENT: {
        const GstSegment *segment = NULL;
        gst_event_parse_segment(event, &segment);
        s->segment = *segment; s->segments++;
        s->have_segment = segment->format == GST_FORMAT_TIME &&
            segment->rate == 1.0 && segment->applied_rate == 1.0;
        if (!s->have_segment) fail_locked(a, "normal-cadence TIME segment required");
        break;
      }
      case GST_EVENT_EOS: s->eos++; break;
      default: break;
    }
    g_mutex_unlock(&a->lock);
  } else if (type & GST_PAD_PROBE_TYPE_BUFFER) {
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    GstClockTime pts = GST_BUFFER_PTS(buffer), duration = GST_BUFFER_DURATION(buffer);
    GstClockTime clock = running_clock(a->pipeline);
    g_mutex_lock(&a->lock);
    s->count++;
    if (p->kind == VIDEO) capture_frame(&a->capture, buffer, s->count-1);
    if (!s->caps_valid || !s->have_segment || gst_buffer_get_size(buffer) == 0)
      fail_locked(a, "buffer lacks caps, TIME segment, or payload");
    if (!GST_CLOCK_TIME_IS_VALID(pts)) s->missing_pts++;
    else {
      if (!GST_CLOCK_TIME_IS_VALID(s->first)) { s->first = pts; s->first_duration = duration; }
      if (GST_CLOCK_TIME_IS_VALID(s->last) && pts <= s->last) s->backwards++;
      s->last = pts;
      if (p->kind != AUDIO) {
        if (s->pts->len <= a->expected) g_array_append_val(s->pts, pts);
        else fail_locked(a, "video observations exceeded declared finite bound");
      }
      s->running_start = gst_segment_to_running_time(&s->segment, GST_FORMAT_TIME, pts);
      if (GST_CLOCK_TIME_IS_VALID(duration) && duration && pts < GST_CLOCK_TIME_NONE-duration) {
        s->end = pts+duration;
        s->running_end = gst_segment_to_running_time(&s->segment, GST_FORMAT_TIME, s->end);
      }
      if (GST_CLOCK_TIME_IS_VALID(clock) && GST_CLOCK_TIME_IS_VALID(s->running_start)) {
        gint64 offset = difference(clock, s->running_start);
        if (s->clock_samples++ == 0) s->first_offset = offset;
        s->last_offset = offset; s->min_offset = MIN(s->min_offset, offset);
        s->max_offset = MAX(s->max_offset, offset);
      }
    }
    if (!GST_CLOCK_TIME_IS_VALID(duration) || !duration) s->missing_duration++;
    if (p->kind == AUDIO && !s->nonzero) {
      GstMapInfo map;
      if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        for (gsize n = 0; n < map.size; ++n)
          if (map.data[n]) { s->nonzero = TRUE; break; }
        gst_buffer_unmap(buffer, &map);
      } else fail_locked(a, "decoded audio payload map failed");
    }
    if (p->kind == VIDEO && GST_CLOCK_TIME_IS_VALID(s->running_start)) {
      const Stream *audio = &a->streams[AUDIO];
      if (GST_CLOCK_TIME_IS_VALID(audio->running_start) && GST_CLOCK_TIME_IS_VALID(audio->running_end)) {
        gint64 gap = s->running_start < audio->running_start
            ? difference(s->running_start, audio->running_start)
            : s->running_start > audio->running_end
                ? difference(s->running_start, audio->running_end) : 0;
        if (a->av_pairs++ == 0) a->av_min_gap = a->av_max_gap = gap;
        else { a->av_min_gap = MIN(a->av_min_gap, gap); a->av_max_gap = MAX(a->av_max_gap, gap); }
      }
    }
    g_mutex_unlock(&a->lock);
  }
  return GST_PAD_PROBE_OK;
}
static gint compare_time(gconstpointer a, gconstpointer b) {
  GstClockTime x = *(const GstClockTime *)a, y = *(const GstClockTime *)b;
  return x < y ? -1 : x > y;
}
static gboolean same_pts(const GArray *x, const GArray *y) {
  if (!x->len || x->len != y->len) return FALSE;
  GArray *a = g_array_sized_new(FALSE, FALSE, sizeof(GstClockTime), x->len);
  GArray *b = g_array_sized_new(FALSE, FALSE, sizeof(GstClockTime), y->len);
  g_array_append_vals(a, x->data, x->len); g_array_append_vals(b, y->data, y->len);
  g_array_sort(a, compare_time); g_array_sort(b, compare_time);
  gboolean equal = memcmp(a->data, b->data, a->len*sizeof(GstClockTime)) == 0;
  for (guint n = 1; equal && n < a->len; ++n)
    equal = g_array_index(a, GstClockTime, n-1) < g_array_index(a, GstClockTime, n);
  g_array_unref(a); g_array_unref(b);
  return equal;
}
static void print_properties(GstElement *element) {
  static const gchar *const properties[] = {"sync", "qos", "max-lateness",
    "processing-deadline", "render-delay", "ts-offset", "buffer-time", "latency-time", "provide-clock", NULL};
  GstElementFactory *factory = gst_element_get_factory(element);
  g_print("element name=%s factory=%s type=%s\n", GST_OBJECT_NAME(element),
      factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)) : "none", G_OBJECT_TYPE_NAME(element));
  for (guint n = 0; properties[n]; ++n) {
    GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), properties[n]);
    if (spec && (spec->flags & G_PARAM_READABLE)) {
      GValue value = G_VALUE_INIT;
      g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(spec));
      g_object_get_property(G_OBJECT(element), properties[n], &value);
      gchar *text = g_strdup_value_contents(&value);
      g_print("property element=%s name=%s value=%s\n", GST_OBJECT_NAME(element), properties[n], text);
      g_free(text); g_value_unset(&value);
    }
  }
}
typedef struct {
  GMutex lock; GCond condition; GstElement *pipeline; gboolean done;
  GstStateChangeReturn set_result, wait_result; GstState state, pending;
} Cleanup;
static gpointer cleanup_worker(gpointer opaque) {
  Cleanup *c = opaque;
  c->set_result = gst_element_set_state(c->pipeline, GST_STATE_NULL);
  c->wait_result = gst_element_get_state(c->pipeline, &c->state, &c->pending, 5*GST_SECOND);
  g_mutex_lock(&c->lock); c->done = TRUE; g_cond_signal(&c->condition); g_mutex_unlock(&c->lock);
  return NULL;
}
static gboolean cleanup_pipeline(GstElement *pipeline) {
  Cleanup c = {0}; c.pipeline = pipeline;
  g_mutex_init(&c.lock); g_cond_init(&c.condition);
  GThread *thread = g_thread_new("mpeg1-cleanup", cleanup_worker, &c);
  gint64 deadline = g_get_monotonic_time()+15*G_USEC_PER_SEC;
  g_mutex_lock(&c.lock);
  while (!c.done) {
    if (!g_cond_wait_until(&c.condition, &c.lock, deadline) && !c.done) {
      g_printerr("cleanup-timeout: DIL/NULL close unfinished; external timeout/device-retirement check required\n");
      fflush(NULL); _Exit(3); /* No freeing objects still owned by blocked threads. */
    }
  }
  g_mutex_unlock(&c.lock); g_thread_join(thread);
  gboolean ok = c.set_result != GST_STATE_CHANGE_FAILURE && c.wait_result == GST_STATE_CHANGE_SUCCESS &&
      c.state == GST_STATE_NULL && c.pending == GST_STATE_VOID_PENDING;
  g_print("cleanup set=%d wait=%d state=%s pending=%s complete=%s\n", c.set_result, c.wait_result,
      gst_element_state_get_name(c.state), gst_element_state_get_name(c.pending), ok ? "yes" : "no");
  g_cond_clear(&c.condition); g_mutex_clear(&c.lock);
  return ok;
}
static gboolean parse_uint(const gchar *text, guint max, guint *result) {
  gchar *end = NULL; errno = 0;
  guint64 value = g_ascii_strtoull(text, &end, 10);
  if (errno || text == end || *end || !value || value > max) return FALSE;
  *result = (guint)value; return TRUE;
}

int main(int argc, char **argv) {
  guint expected = 0, timeout_seconds = 90;
  gboolean experimental = FALSE;
  const gchar *factory = "crystalhdmpeg1researchdec";
  gst_init(&argc, &argv);
  if (argc == 2 && !strcmp(argv[1], "--self-test"))
    return gst_crystalhd_mpeg1_research_self_test() ? 0 : 1;
  if (argc >= 2 && !strcmp(argv[1], "--experimental")) {
    experimental = TRUE;
    factory = "crystalhdmpeg1experimentaldec";
    argc--; argv++;
  }
  if ((argc != 3 && argc != 4) || !parse_uint(argv[2], 100000, &expected) ||
      (argc == 4 && !parse_uint(argv[3], 600, &timeout_seconds))) {
    g_printerr("usage: %s [--experimental] INPUT_MKV EXPECTED_VIDEO_FRAMES [TIMEOUT_SECONDS] | --self-test\n", argv[0]);
    return 2;
  }
  if (!experimental && !gst_crystalhd_mpeg1_research_register()) {
    g_printerr("private crystalhdmpeg1researchdec registration failed\n"); return 2;
  }
  if (experimental) {
    GstElementFactory *feature = gst_element_factory_find(factory);
    if (feature == NULL) {
      g_printerr("explicit experimental plugin unavailable: %s\n", factory);
      return 2;
    }
    gst_object_unref(feature);
  }
  GError *error = NULL;
  gchar *description = g_strdup_printf(
      "filesrc name=source ! matroskademux name=demux "
      "demux.video_0 ! queue ! mpegvideoparse ! %s name=decoder ! "
      "videoconvert ! xvimagesink name=video "
      "demux.audio_0 ! queue ! aacparse ! avdec_aac ! audioconvert ! audioresample ! "
      "pulsesink name=audio", factory);
  GstElement *pipeline = gst_parse_launch(description, &error);
  g_free(description);
  if (!pipeline || error) {
    g_printerr("pipeline-construction-error: %s\n", error ? error->message : "none");
    g_clear_error(&error); if (pipeline) gst_object_unref(pipeline); return 2;
  }
  GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
  GstElement *decoder = gst_bin_get_by_name(GST_BIN(pipeline), "decoder");
  GstElement *video = gst_bin_get_by_name(GST_BIN(pipeline), "video");
  GstElement *audio = gst_bin_get_by_name(GST_BIN(pipeline), "audio");
  g_object_set(source, "location", argv[1], NULL); gst_object_unref(source);
  Audit a = {0}; g_mutex_init(&a.lock); a.pipeline = pipeline; a.expected = expected;
  if (!capture_init(&a.capture, expected)) {
    g_printerr("capture-configuration-error: %s\n", a.capture.reason);
    capture_clear(&a.capture); g_mutex_clear(&a.lock);
    gst_object_unref(decoder); gst_object_unref(video); gst_object_unref(audio);
    gst_object_unref(pipeline); return 2;
  }
  for (guint n = 0; n < N_STREAMS; ++n) {
    Stream *s = &a.streams[n];
    gst_segment_init(&s->segment, GST_FORMAT_UNDEFINED);
    s->first = s->last = s->end = s->first_duration =
        s->running_start = s->running_end = GST_CLOCK_TIME_NONE;
    s->min_offset = G_MAXINT64; s->max_offset = G_MININT64;
    s->pts = g_array_sized_new(FALSE, FALSE, sizeof(GstClockTime), n == AUDIO ? 0 : expected);
  }
  ProbeData data[N_STREAMS] = {{&a, INPUT}, {&a, DECODED}, {&a, VIDEO}, {&a, AUDIO}};
  GstPad *pads[N_STREAMS] = {gst_element_get_static_pad(decoder, "sink"),
      gst_element_get_static_pad(decoder, "src"), gst_element_get_static_pad(video, "sink"),
      gst_element_get_static_pad(audio, "sink")};
  gulong probes[N_STREAMS] = {0};
  for (guint n = 0; n < N_STREAMS; ++n) {
    if (!pads[n]) { fail_locked(&a, "missing observation pad"); continue; }
    probes[n] = gst_pad_add_probe(pads[n],
        GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, observe, &data[n], NULL);
    if (!probes[n]) fail_locked(&a, "observation probe installation failed");
  }
  g_print("MPEG1_RESEARCH input=%s expected=%u factory=%s "
          "source-codec=mpeg1video internal-algorithm=1 external-caps-relabelled=no "
          "queues=defaults sink-policy=defaults\n", argv[1], expected, factory);
  print_properties(decoder); print_properties(video); print_properties(audio);
  GstBus *bus = gst_element_get_bus(pipeline);
  gboolean eos = FALSE;
  guint warnings = 0, qos_messages = 0;
  gint64 begin = g_get_monotonic_time(), deadline = begin+(gint64)timeout_seconds*G_USEC_PER_SEC;
  gint64 next_progress = begin+5*G_USEC_PER_SEC;
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_mutex_lock(&a.lock); fail_locked(&a, "PLAYING transition failed"); g_mutex_unlock(&a.lock);
  }
  while (!eos) {
    g_mutex_lock(&a.lock); gboolean failed = a.failed; g_mutex_unlock(&a.lock);
    if (failed) break;
    if (g_get_monotonic_time() >= deadline) {
      g_mutex_lock(&a.lock); fail_locked(&a, "finite playback timeout before natural EOS");
      g_mutex_unlock(&a.lock); break;
    }
    GstMessage *message = gst_bus_timed_pop_filtered(bus, 250*GST_MSECOND,
        GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS | GST_MESSAGE_NEW_CLOCK |
        GST_MESSAGE_QOS | GST_MESSAGE_CLOCK_LOST | GST_MESSAGE_LATENCY | GST_MESSAGE_STATE_CHANGED);
    if (message) {
      const gchar *name = GST_MESSAGE_SRC(message) ? GST_OBJECT_NAME(GST_MESSAGE_SRC(message)) : "none";
      switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
          gchar *debug = NULL; gst_message_parse_error(message, &error, &debug);
          g_printerr("bus-error source=%s message=%s debug=%s\n", name, error->message, debug ? debug : "none");
          g_mutex_lock(&a.lock); fail_locked(&a, error->message); g_mutex_unlock(&a.lock);
          g_clear_error(&error); g_free(debug); break;
        }
        case GST_MESSAGE_WARNING: {
          gchar *debug = NULL; gst_message_parse_warning(message, &error, &debug);
          g_print("bus-warning source=%s message=%s debug=%s\n", name, error->message, debug ? debug : "none");
          warnings++; g_clear_error(&error); g_free(debug); break;
        }
        case GST_MESSAGE_EOS:
          eos = TRUE;
          g_print("bus-eos source=%s natural=yes elapsed=%.6f\n", name,
              (g_get_monotonic_time()-begin)/(gdouble)G_USEC_PER_SEC); break;
        case GST_MESSAGE_NEW_CLOCK: {
          GstClock *clock = NULL; gst_message_parse_new_clock(message, &clock);
          g_print("bus-new-clock name=%s type=%s\n", GST_OBJECT_NAME(clock), G_OBJECT_TYPE_NAME(clock)); break;
        }
        case GST_MESSAGE_STATE_CHANGED:
          if (GST_MESSAGE_SRC(message) == GST_OBJECT(pipeline)) {
            GstState old, current, pending;
            gst_message_parse_state_changed(message, &old, &current, &pending);
            g_print("pipeline-state old=%s current=%s pending=%s\n",
                gst_element_state_get_name(old), gst_element_state_get_name(current),
                gst_element_state_get_name(pending));
          }
          break;
        case GST_MESSAGE_CLOCK_LOST:
          g_mutex_lock(&a.lock); fail_locked(&a, "selected clock lost; no clock policy change attempted");
          g_mutex_unlock(&a.lock); break;
        case GST_MESSAGE_LATENCY:
          if (!gst_bin_recalculate_latency(GST_BIN(pipeline))) {
            g_mutex_lock(&a.lock); fail_locked(&a, "normal pipeline latency recalculation failed");
            g_mutex_unlock(&a.lock);
          }
          break;
        case GST_MESSAGE_QOS: {
          GstFormat format; guint64 processed, dropped;
          gst_message_parse_qos_stats(message, &format, &processed, &dropped); qos_messages++;
          if (qos_messages <= 16)
            g_print("bus-qos source=%s format=%s processed=%" G_GUINT64_FORMAT " dropped=%" G_GUINT64_FORMAT "\n",
                name, gst_format_get_name(format), processed, dropped);
          break;
        }
        default: break;
      }
      gst_message_unref(message);
    }
    if (g_get_monotonic_time() >= next_progress) {
      g_mutex_lock(&a.lock);
      g_print("progress elapsed=%.3f input=%" G_GUINT64_FORMAT " decoded=%" G_GUINT64_FORMAT
              " video=%" G_GUINT64_FORMAT " audio=%" G_GUINT64_FORMAT "\n",
          (g_get_monotonic_time()-begin)/(gdouble)G_USEC_PER_SEC, a.streams[INPUT].count,
          a.streams[DECODED].count, a.streams[VIDEO].count, a.streams[AUDIO].count);
      g_mutex_unlock(&a.lock); next_progress += 5*G_USEC_PER_SEC;
    }
  }
  GstClock *clock = gst_element_get_clock(pipeline);
  gboolean pulse_clock = clock && GST_IS_AUDIO_CLOCK(clock) &&
      g_str_has_prefix(GST_OBJECT_NAME(clock), "GstPulseSinkClock");
  g_print("terminal-clock name=%s type=%s pulse-audio-clock=%s\n",
      clock ? GST_OBJECT_NAME(clock) : "none", clock ? G_OBJECT_TYPE_NAME(clock) : "none", pulse_clock ? "yes" : "no");
  if (clock) gst_object_unref(clock);
  GstStructure *stats = NULL; guint64 rendered = 0, dropped = 0;
  g_object_get(video, "stats", &stats, NULL);
  gboolean stats_valid = stats && gst_structure_get_uint64(stats, "rendered", &rendered) &&
      gst_structure_get_uint64(stats, "dropped", &dropped);
  if (stats) {
    gchar *text = gst_structure_to_string(stats); g_print("video-stats-before-null %s\n", text);
    g_free(text); gst_structure_free(stats);
  }
  g_mutex_lock(&a.lock);
  gboolean pts_join = same_pts(a.streams[INPUT].pts, a.streams[DECODED].pts) &&
      same_pts(a.streams[DECODED].pts, a.streams[VIDEO].pts);
  if (!eos || !pulse_clock || !stats_valid || rendered != expected || dropped || !pts_join)
    fail_locked(&a, "natural EOS/Pulse clock/exact original PTS joins/zero-drop render census required");
  for (guint n = 0; n < N_STREAMS; ++n) {
    Stream *s = &a.streams[n];
    if (s->missing_pts || s->missing_duration || s->eos != 1 ||
        (n != INPUT && s->backwards) || (n != AUDIO && s->count != expected))
      fail_locked(&a, "incomplete stream count/timestamp/EOS evidence");
    if (n == DECODED || n == VIDEO) {
      for (guint j = 0; j < s->pts->len; ++j) {
        GstClockTime value = g_array_index(s->pts, GstClockTime, j);
        if (value < s->first || value-s->first != (GstClockTime)j*40*GST_MSECOND)
          fail_locked(&a, "decoded/video presentation PTS do not match the 25fps grid");
      }
    }
    /* Exact-pad arrival offsets include buffering lead. They are not
     * rendered A/V drift, calibrated lipsync error, or a pass threshold. */
    g_print("stream name=%s buffers=%" G_GUINT64_FORMAT " first-pts=%" G_GUINT64_FORMAT
            " last-pts=%" G_GUINT64_FORMAT " last-end=%" G_GUINT64_FORMAT
            " missing-pts=%" G_GUINT64_FORMAT " missing-duration=%" G_GUINT64_FORMAT
            " backwards-pts=%" G_GUINT64_FORMAT " segments=%" G_GUINT64_FORMAT " eos=%" G_GUINT64_FORMAT
            " clock-samples=%" G_GUINT64_FORMAT " arrival-offset-first-ns=%" G_GINT64_FORMAT
            " arrival-offset-last-ns=%" G_GINT64_FORMAT " arrival-offset-min-ns=%" G_GINT64_FORMAT
            " arrival-offset-max-ns=%" G_GINT64_FORMAT " arrival-offset-change-ns=%" G_GINT64_FORMAT
            " nonzero-audio-payload-seen=%s caps=%s\n", names[n], s->count, s->first, s->last, s->end,
        s->missing_pts, s->missing_duration, s->backwards, s->segments, s->eos, s->clock_samples,
        s->first_offset, s->last_offset, s->clock_samples ? s->min_offset : 0,
        s->clock_samples ? s->max_offset : 0, s->clock_samples ? s->last_offset-s->first_offset : 0,
        s->nonzero ? "yes" : "no", s->caps ? s->caps : "none");
  }
  const Stream *as = &a.streams[AUDIO], *vs = &a.streams[VIDEO];
  gboolean audio_covers_video = GST_CLOCK_TIME_IS_VALID(as->first) &&
      GST_CLOCK_TIME_IS_VALID(as->end) && GST_CLOCK_TIME_IS_VALID(vs->first_duration) &&
      vs->first < GST_CLOCK_TIME_NONE-vs->first_duration &&
      as->first < vs->first+vs->first_duration && as->end > vs->last;
  if (as->count < 2 || !as->nonzero || !as->clock_samples || !audio_covers_video)
    fail_locked(&a, "decoded nonzero audio and endpoint coverage of the video PTS span required");
  g_print("audio-video-observation pairs=%" G_GUINT64_FORMAT
          " latest-audio-interval-gap-min-ns=%" G_GINT64_FORMAT " latest-audio-interval-gap-max-ns=%" G_GINT64_FORMAT
          " audio-covers-video-pts=%s original-pts-set-join=%s\n", a.av_pairs, a.av_min_gap, a.av_max_gap,
      audio_covers_video ? "yes" : "no", pts_join ? "yes" : "no");
  gboolean success = !a.failed;
  g_print("MPEG1_RESEARCH_RESULT natural-eos=%s expected=%u rendered=%" G_GUINT64_FORMAT
          " dropped=%" G_GUINT64_FORMAT " warnings=%u qos-messages=%u result=%s reason=%s\n",
      eos ? "yes" : "no", expected, rendered, dropped, warnings, qos_messages, success ? "PASS" : "FAIL",
      a.failed ? a.reason : "metadata/count/clock/audio evidence only; no physical display or audible-output claim");
  g_mutex_unlock(&a.lock);
  gboolean cleaned = cleanup_pipeline(pipeline);
  if (cleaned) success = capture_save(&a.capture) && success;
  success = cleaned && success;
  for (guint n = 0; n < N_STREAMS; ++n) {
    if (pads[n]) { if (probes[n]) gst_pad_remove_probe(pads[n], probes[n]); gst_object_unref(pads[n]); }
    g_array_unref(a.streams[n].pts); g_free(a.streams[n].caps);
  }
  gst_object_unref(bus); gst_object_unref(decoder); gst_object_unref(video); gst_object_unref(audio);
  gst_object_unref(pipeline); capture_clear(&a.capture); g_mutex_clear(&a.lock);
  g_print("MPEG1_RESEARCH_EXIT status=%d\n", success ? 0 : 1);
  return success ? 0 : 1;
}
