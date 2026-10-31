/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real CPU-only pad/pipeline controls. No decoder, DIL, device or display. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <gst/check/gstharness.h>
#include <gst/video/video.h>
#include <string.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

static gint thread_id_calls;
#if defined(__linux__) && defined(SYS_gettid)
static long
trace_test_syscall(long number, ...)
{
  g_assert_cmpint(number, ==, SYS_gettid);
  g_atomic_int_inc(&thread_id_calls);
  return syscall(number);
}
#define syscall trace_test_syscall
#endif

/* Unit-only scheduling barriers around the production clock and mutex calls.
 * Pause a real probe while it owns state, then observe a real stop attempting
 * that lock before releasing the probe. Production gets no test hooks.
 */
static struct {
  GMutex lock;
  GCond cond;
  GMutex *state_lock;
  GThread *stop_thread;
  gboolean enabled, entered, release, stop_waiting;
} clock_gate;

static GstClockTime
trace_test_timestamp(void)
{
  GstClockTime timestamp = gst_util_get_timestamp();
  g_mutex_lock(&clock_gate.lock);
  if (clock_gate.enabled && !clock_gate.entered) {
    clock_gate.entered = TRUE;
    g_cond_broadcast(&clock_gate.cond);
    while (!clock_gate.release)
      g_cond_wait(&clock_gate.cond, &clock_gate.lock);
  }
  g_mutex_unlock(&clock_gate.lock);
  return timestamp;
}

static void
trace_test_mutex_lock(GMutex *mutex)
{
  g_mutex_lock(&clock_gate.lock);
  if (clock_gate.enabled && mutex == clock_gate.state_lock &&
      g_thread_self() == clock_gate.stop_thread) {
    clock_gate.stop_waiting = TRUE;
    g_cond_broadcast(&clock_gate.cond);
  }
  g_mutex_unlock(&clock_gate.lock);
  g_mutex_lock(mutex);
}

#define gst_util_get_timestamp trace_test_timestamp
#define g_mutex_lock trace_test_mutex_lock
#include "../filters/gst/gst-plugin-1.0/gstcrystalhd-stage-trace.c"
#undef g_mutex_lock
#undef gst_util_get_timestamp
#if defined(__linux__) && defined(SYS_gettid)
#undef syscall
#endif

#define INPUT_CAPS "video/x-raw,format=YUY2,width=16,height=8," \
  "interlace-mode=mixed,framerate=25/1,pixel-aspect-ratio=1/1"
#define OUTPUT_CAPS "video/x-raw,format=YUY2,width=16,height=8," \
  "interlace-mode=progressive,framerate=50/1,pixel-aspect-ratio=1/1"

typedef struct {
  GstElement *helper;
  GstPad *src, *sink;
  guint received, destroyed;
  GstClockTime expected_pts, expected_dts, expected_duration;
  guint expected_flags;
  GMutex lock;
  GCond cond;
  gboolean block_first, first, release;
} PadFixture;

static GstElement *
helper_new(guint capacity)
{
  return gst_object_ref_sink(g_object_new(GST_TYPE_CRYSTALHD_STAGE_TRACE,
      "capacity", capacity, NULL));
}

static gint
attach(GstElement *helper, GstPad *pad, const gchar *stage, guint64 token)
{
  gint result = -99;
  g_signal_emit_by_name(helper, "attach", pad, stage, token, &result);
  return result;
}

static gchar *
snapshot(GstElement *helper)
{
  gchar *result = NULL;
  g_signal_emit_by_name(helper, "snapshot", &result);
  g_assert_nonnull(result);
  return result;
}

static void
emit_case(GstElement *helper, const gchar *name)
{
  gchar *json = snapshot(helper);
  g_print("TRACE_CASE %s %s\n", name, json);
  g_free(json);
}

static void
buffer_destroyed(gpointer data, GstMiniObject *object)
{
  PadFixture *f = data;
  (void) object;
  f->destroyed++;
}

static void
object_destroyed(gpointer data, GObject *object)
{
  gboolean *destroyed = data;
  (void) object;
  *destroyed = TRUE;
}

static GstFlowReturn
receive_buffer(GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
  PadFixture *f = g_object_get_data(G_OBJECT(pad), "fixture");
  guint8 bytes[16];
  (void) parent;
  g_assert_cmpuint(GST_BUFFER_PTS(buffer), ==, f->expected_pts);
  g_assert_cmpuint(GST_BUFFER_DTS(buffer), ==, f->expected_dts);
  g_assert_cmpuint(GST_BUFFER_DURATION(buffer), ==, f->expected_duration);
  g_assert_cmpuint(GST_BUFFER_FLAGS(buffer), ==, f->expected_flags);
  g_assert_cmpuint(gst_buffer_extract(buffer, 0, bytes, sizeof(bytes)), ==,
      sizeof(bytes));
  for (guint i = 0; i < sizeof(bytes); i++)
    g_assert_cmpuint(bytes[i], ==, (guint8) (i * 7 + 3));
  g_mutex_lock(&f->lock);
  f->received++;
  if (f->block_first && !f->first) {
    f->first = TRUE;
    g_cond_broadcast(&f->cond);
    while (!f->release)
      g_cond_wait(&f->cond, &f->lock);
  }
  g_mutex_unlock(&f->lock);
  gst_buffer_unref(buffer);
  return GST_FLOW_OK;
}

static gboolean
receive_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
  (void) pad; (void) parent;
  gst_event_unref(event);
  return TRUE;
}

static gboolean
receive_query(GstPad *pad, GstObject *parent, GstQuery *query)
{
  /* This unit sink consumes opaque bytes under all supplied test CAPS. Bare
   * GstPad's default query otherwise fixes negotiation to the previous CAPS,
   * preventing the explicit renegotiation/window-context control. */
  if (GST_QUERY_TYPE(query) == GST_QUERY_ACCEPT_CAPS) {
    gst_query_set_accept_caps_result(query, TRUE);
    return TRUE;
  }
  if (GST_QUERY_TYPE(query) == GST_QUERY_CAPS) {
    GstCaps *filter, *caps;
    gst_query_parse_caps(query, &filter);
    caps = filter ? gst_caps_ref(filter) : gst_caps_new_any();
    gst_query_set_caps_result(query, caps);
    gst_caps_unref(caps);
    return TRUE;
  }
  return gst_pad_query_default(pad, parent, query);
}

static GstFlowReturn
receive_list(GstPad *pad, GstObject *parent, GstBufferList *list)
{
  for (guint n = 0; n < gst_buffer_list_length(list); n++) {
    GstFlowReturn status = receive_buffer(pad, parent,
        gst_buffer_ref(gst_buffer_list_get(list, n)));
    g_assert_cmpint(status, ==, GST_FLOW_OK);
  }
  gst_buffer_list_unref(list);
  return GST_FLOW_OK;
}

static void
fixture_init(PadFixture *f, guint capacity)
{
  memset(f, 0, sizeof(*f));
  g_mutex_init(&f->lock);
  g_cond_init(&f->cond);
  f->helper = helper_new(capacity);
  f->src = gst_object_ref_sink(gst_pad_new("source", GST_PAD_SRC));
  f->sink = gst_object_ref_sink(gst_pad_new("sink", GST_PAD_SINK));
  g_object_set_data(G_OBJECT(f->sink), "fixture", f);
  gst_pad_set_chain_function(f->sink, receive_buffer);
  gst_pad_set_event_function(f->sink, receive_event);
  gst_pad_set_query_function(f->sink, receive_query);
  gst_pad_set_event_function(f->src, receive_event);
  g_assert_true(gst_pad_set_active(f->src, TRUE));
  g_assert_true(gst_pad_set_active(f->sink, TRUE));
  g_assert_cmpint(gst_pad_link(f->src, f->sink), ==, GST_PAD_LINK_OK);
}

static void
fixture_clear(PadFixture *f)
{
  if (f->helper) {
    g_signal_emit_by_name(f->helper, "stop");
    gst_object_unref(f->helper);
  }
  if (f->src && f->sink)
    gst_pad_unlink(f->src, f->sink);
  if (f->src) {
    gst_pad_set_active(f->src, FALSE);
    gst_object_unref(f->src);
  }
  if (f->sink) {
    gst_pad_set_active(f->sink, FALSE);
    gst_object_unref(f->sink);
  }
  g_cond_clear(&f->cond);
  g_mutex_clear(&f->lock);
}

static void
push_segment(PadFixture *f, GstFormat format, guint64 start, guint64 time,
             guint64 base, guint32 seqnum)
{
  GstSegment segment;
  gst_segment_init(&segment, format);
  segment.start = start;
  segment.time = time;
  segment.base = base;
  GstEvent *event = gst_event_new_segment(&segment);
  gst_event_set_seqnum(event, seqnum);
  g_assert_true(gst_pad_push_event(f->src, event));
}

static void
push_headers(PadFixture *f)
{
  GstCaps *caps = gst_caps_from_string(INPUT_CAPS);
  g_assert_true(gst_pad_push_event(f->src, gst_event_new_stream_start("trace-test")));
  g_assert_true(gst_pad_push_event(f->src, gst_event_new_caps(caps)));
  push_segment(f, GST_FORMAT_TIME, 0, 0, 0, 0x901);
  gst_caps_unref(caps);
}

static void
push_buffer(PadFixture *f, GstClockTime pts, GstClockTime dts,
            GstClockTime duration, guint flags)
{
  guint8 bytes[16];
  GstBuffer *buffer = gst_buffer_new_allocate(NULL, sizeof(bytes), NULL);
  for (guint i = 0; i < sizeof(bytes); i++)
    bytes[i] = (guint8) (i * 7 + 3);
  g_assert_cmpuint(gst_buffer_fill(buffer, 0, bytes, sizeof(bytes)), ==,
      sizeof(bytes));
  GST_BUFFER_PTS(buffer) = f->expected_pts = pts;
  GST_BUFFER_DTS(buffer) = f->expected_dts = dts;
  GST_BUFFER_DURATION(buffer) = f->expected_duration = duration;
  GST_BUFFER_FLAGS(buffer) = f->expected_flags = flags;
  gst_mini_object_weak_ref(GST_MINI_OBJECT(buffer), buffer_destroyed, f);
  g_assert_cmpint(gst_pad_push(f->src, buffer), ==, GST_FLOW_OK);
}

static void
finish(PadFixture *f, const gchar *name)
{
  g_assert_true(gst_pad_push_event(f->src, gst_event_new_eos()));
  g_signal_emit_by_name(f->helper, "stop");
  gchar *first = snapshot(f->helper);
  g_signal_emit_by_name(f->helper, "stop");
  gchar *second = snapshot(f->helper);
  g_assert_cmpstr(first, ==, second);
  g_free(first); g_free(second);
  g_assert_cmpuint(f->received, ==, f->destroyed);
  emit_case(f->helper, name);
}

static void
test_metadata(void)
{
  PadFixture f;
  gint calls_before = g_atomic_int_get(&thread_id_calls);
  fixture_init(&f, 128);
  g_assert_null(GST_ELEMENT_PADS(f.helper));
  g_assert_null(GST_OBJECT_PARENT(f.helper));
  g_assert_cmpint(attach(f.helper, NULL, "decoder", 1), ==, -1);
  g_assert_cmpint(attach(f.helper, f.src, "invalid-stage", 1), ==, -1);
  g_assert_cmpint(attach(f.helper, f.src, "sink", 1), ==, -1);
  g_assert_cmpint(attach(f.helper, f.sink, "decoder", 1), ==, -1);
  g_assert_cmpint(attach(f.helper, f.sink, "deinterlace", 1), ==, -1);
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 11), ==, 1);
  g_assert_cmpint(attach(f.helper, f.src, "deinterlace", 99), ==, 0);
  g_assert_cmpint(attach(f.helper, f.sink, "sink", 22), ==, 1);
  push_headers(&f);
  push_buffer(&f, 0, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND,
      GST_VIDEO_BUFFER_FLAG_INTERLACED | GST_VIDEO_BUFFER_FLAG_TFF |
      GST_VIDEO_BUFFER_FLAG_RFF | GST_VIDEO_BUFFER_FLAG_ONEFIELD);
  push_buffer(&f, GST_CLOCK_TIME_NONE, 0, GST_CLOCK_TIME_NONE, 0);
  push_segment(&f, GST_FORMAT_TIME, GST_SECOND, 2 * GST_SECOND,
      3 * GST_SECOND, 0x902);
  push_buffer(&f, 1250 * GST_MSECOND, 1240 * GST_MSECOND,
      20 * GST_MSECOND, GST_BUFFER_FLAG_DISCONT);
  g_assert_true(gst_pad_push_event(f.sink, gst_event_new_qos(
      GST_QOS_TYPE_UNDERFLOW, 1.25, GST_MSECOND, 20 * GST_MSECOND)));
  finish(&f, "metadata");
  gchar *before = snapshot(f.helper);
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 33), ==, -1);
  g_object_set(f.helper, "capacity", 8, NULL);
  gboolean accepted = TRUE;
  g_signal_emit_by_name(f.helper, "control", "pause", (guint64) 0, &accepted);
  g_assert_false(accepted);
  g_signal_emit_by_name(f.helper, "detach-pad", f.src);
  gchar *after = snapshot(f.helper);
  g_assert_cmpstr(before, ==, after);
  g_free(before); g_free(after);
  fixture_clear(&f);
  g_assert_cmpint(g_atomic_int_get(&thread_id_calls), ==, calls_before);
}

static void
test_flush(void)
{
  PadFixture f;
  fixture_init(&f, 128);
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 31), ==, 1);
  push_headers(&f);
  push_buffer(&f, 0, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  g_assert_true(gst_pad_push_event(f.src, gst_event_new_flush_start()));
  g_assert_true(gst_pad_push_event(f.src, gst_event_new_flush_stop(TRUE)));
  /* Real GstPad diagnostics are expected for this deliberately incomplete
   * sequence: the collector must report unknown instead of using stale TIME.
   */
  g_test_expect_message("GStreamer", G_LOG_LEVEL_WARNING,
      "*Got data flow before segment event*");
  g_test_expect_message("GStreamer", G_LOG_LEVEL_WARNING,
      "*Got data flow before segment event*");
  push_buffer(&f, 40 * GST_MSECOND, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  g_test_assert_expected_messages();
  push_segment(&f, GST_FORMAT_TIME, GST_SECOND, 2 * GST_SECOND,
      9 * GST_SECOND, 0x903);
  push_buffer(&f, 1250 * GST_MSECOND, GST_CLOCK_TIME_NONE, 20 * GST_MSECOND, 0);
  push_segment(&f, GST_FORMAT_BYTES, 0, 0, 0, 0x904);
  push_buffer(&f, 42 * GST_MSECOND, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, 0);
  finish(&f, "flush");
  fixture_clear(&f);
}

static void
test_capacity_and_controls(void)
{
  PadFixture f;
  gboolean accepted = FALSE;
  fixture_init(&f, 3);
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 41), ==, 1);
  g_object_set(f.helper, "capacity", 2, NULL); /* Immutable once attached. */
  push_headers(&f);
  for (guint i = 0; i < 256; i++)
    push_buffer(&f, i * 40 * GST_MSECOND, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  g_signal_emit_by_name(f.helper, "control", "pause", (guint64) 0, &accepted);
  g_assert_true(accepted);
  g_signal_emit_by_name(f.helper, "control", "resume", (guint64) 0, &accepted);
  g_assert_true(accepted);
  g_signal_emit_by_name(f.helper, "control", "seek-request", (guint64) 1000, &accepted);
  g_assert_true(accepted);
  g_signal_emit_by_name(f.helper, "control", "seek-failed", (guint64) 1000, &accepted);
  g_assert_true(accepted);
  g_signal_emit_by_name(f.helper, "control", "not-a-control", (guint64) 0, &accepted);
  g_assert_false(accepted);
  finish(&f, "capacity");
  fixture_clear(&f);

  fixture_init(&f, 128);
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 42), ==, 1);
  push_headers(&f);
  push_buffer(&f, 0, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  const gchar *controls[] = {"pause", "resume", "seek-request", "seek-failed"};
  for (guint i = 0; i < G_N_ELEMENTS(controls); i++) {
    g_signal_emit_by_name(f.helper, "control", controls[i], (guint64) i, &accepted);
    g_assert_true(accepted);
  }
  push_buffer(&f, 40 * GST_MSECOND, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  finish(&f, "controls");
  fixture_clear(&f);
}

static void
test_detach_and_lifetime(void)
{
  PadFixture f;
  gboolean helper_freed = FALSE, src_freed = FALSE, sink_freed = FALSE;
  fixture_init(&f, 128);
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 51), ==, 1);
  push_headers(&f);
  push_buffer(&f, 0, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  g_signal_emit_by_name(f.helper, "detach-pad", f.src);
  g_signal_emit_by_name(f.helper, "detach-pad", f.src);
  push_buffer(&f, 40 * GST_MSECOND, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 52), ==, 1);
  push_buffer(&f, 80 * GST_MSECOND, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  finish(&f, "detach");
  g_object_weak_ref(G_OBJECT(f.src), object_destroyed, &src_freed);
  g_object_weak_ref(G_OBJECT(f.sink), object_destroyed, &sink_freed);
  gst_pad_unlink(f.src, f.sink);
  gst_pad_set_active(f.src, FALSE); gst_pad_set_active(f.sink, FALSE);
  gst_object_unref(f.src); gst_object_unref(f.sink);
  f.src = f.sink = NULL;
  g_assert_true(src_freed); g_assert_true(sink_freed);
  emit_case(f.helper, "detached_snapshot");
  fixture_clear(&f);

  fixture_init(&f, 128);
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 53), ==, 1);
  push_headers(&f);
  g_object_weak_ref(G_OBJECT(f.helper), object_destroyed, &helper_freed);
  gst_object_unref(f.helper); f.helper = NULL;
  g_assert_true(helper_freed);
  push_buffer(&f, 0, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  g_assert_cmpuint(f.destroyed, ==, 1);
  fixture_clear(&f);
}

static void
test_buffer_list_and_attachment_limit(void)
{
  PadFixture f;
  for (guint path = 0; path < 4; path++) {
    fixture_init(&f, path >= 2 ? 3 : 128);
    if (path == 3) g_object_set(f.helper, "window", TRUE, NULL);
    if (path == 1) gst_pad_set_chain_list_function(f.sink, receive_list);
    g_assert_cmpint(attach(f.helper, f.src, "decoder", 54), ==, 1);
    g_assert_cmpint(attach(f.helper, f.sink, "sink", 55), ==, 1);
    push_headers(&f);
    GstBufferList *list = gst_buffer_list_new_sized(3);
    f.expected_pts = 0;
    f.expected_dts = GST_CLOCK_TIME_NONE;
    f.expected_duration = 40 * GST_MSECOND;
    for (guint n = 0; n < 3; n++) {
      guint8 bytes[16];
      GstBuffer *buffer = gst_buffer_new_allocate(NULL, sizeof(bytes), NULL);
      for (guint i = 0; i < sizeof(bytes); i++) bytes[i] = (guint8) (i * 7 + 3);
      gst_buffer_fill(buffer, 0, bytes, sizeof(bytes));
      GST_BUFFER_PTS(buffer) = 0; /* Duplicate PTS are observations, not IDs. */
      GST_BUFFER_DURATION(buffer) = 40 * GST_MSECOND;
      gst_mini_object_weak_ref(GST_MINI_OBJECT(buffer), buffer_destroyed, &f);
      gst_buffer_list_insert(list, -1, buffer);
    }
    g_assert_cmpint(gst_pad_push_list(f.src, list), ==, GST_FLOW_OK);
    g_assert_cmpuint(f.received, ==, 3);
    finish(&f, path == 3 ? "window_buffer_list" :
        (path == 2 ? "truncated_buffer_list" :
        (path == 1 ? "custom_buffer_list" : "buffer_list")));
    fixture_clear(&f);
  }

  fixture_init(&f, 4);
  for (guint n = 0; n < 64; n++) {
    g_assert_cmpint(attach(f.helper, f.src, "decoder", 100 + n), ==, 1);
    g_signal_emit_by_name(f.helper, "detach-pad", f.src);
  }
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 164), ==, -1);
  g_signal_emit_by_name(f.helper, "stop");
  emit_case(f.helper, "attachment_limit");
  fixture_clear(&f);
}

static gpointer
push_worker(gpointer data)
{
  PadFixture *f = data;
  for (guint i = 0; i < 128; i++)
    push_buffer(f, i * 40 * GST_MSECOND, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  return NULL;
}

static void
test_concurrent_stop(void)
{
  for (guint repeat = 0; repeat < 24; repeat++) {
    PadFixture f;
    fixture_init(&f, 16);
    g_assert_cmpint(attach(f.helper, f.src, "decoder", 61), ==, 1);
    push_headers(&f);
    f.block_first = TRUE;
    GThread *worker = g_thread_new("trace-push", push_worker, &f);
    g_mutex_lock(&f.lock);
    while (!f.first)
      g_cond_wait(&f.cond, &f.lock);
    g_mutex_unlock(&f.lock);
    g_signal_emit_by_name(f.helper, "stop");
    if (repeat == 0) emit_case(f.helper, "concurrent_stop");
    gst_object_unref(f.helper); f.helper = NULL;
    g_mutex_lock(&f.lock);
    f.release = TRUE;
    g_cond_broadcast(&f.cond);
    g_mutex_unlock(&f.lock);
    g_thread_join(worker);
    g_assert_cmpuint(f.received, ==, 128);
    g_assert_cmpuint(f.destroyed, ==, 128);
    fixture_clear(&f);
  }
}

static gpointer
stop_and_release_worker(gpointer data)
{
  PadFixture *f = data;
  g_mutex_lock(&clock_gate.lock);
  clock_gate.stop_thread = g_thread_self();
  g_mutex_unlock(&clock_gate.lock);
  g_signal_emit_by_name(f->helper, "stop");
  gst_object_unref(f->helper);
  f->helper = NULL;
  return NULL;
}

static void
test_inflight_stop(void)
{
  for (guint repeat = 0; repeat < 8; repeat++) {
    PadFixture f;
    gboolean freed = FALSE;
    fixture_init(&f, 16);
    g_object_set(f.helper, "window", (gboolean) (repeat % 2), NULL);
    g_assert_cmpint(attach(f.helper, f.src, "decoder", 62), ==, 1);
    push_headers(&f);
    g_object_weak_ref(G_OBJECT(f.helper), object_destroyed, &freed);
    g_mutex_lock(&clock_gate.lock);
    clock_gate.state_lock = &((GstCrystalHdStageTrace *) f.helper)->state->lock;
    clock_gate.stop_thread = NULL;
    clock_gate.enabled = TRUE;
    clock_gate.entered = clock_gate.release = clock_gate.stop_waiting = FALSE;
    g_mutex_unlock(&clock_gate.lock);
    GThread *pushing = g_thread_new("trace-inflight", push_worker, &f);
    g_mutex_lock(&clock_gate.lock);
    while (!clock_gate.entered)
      g_cond_wait(&clock_gate.cond, &clock_gate.lock);
    g_mutex_unlock(&clock_gate.lock);
    GThread *stopping = g_thread_new("trace-stop", stop_and_release_worker, &f);
    g_mutex_lock(&clock_gate.lock);
    while (!clock_gate.stop_waiting)
      g_cond_wait(&clock_gate.cond, &clock_gate.lock);
    clock_gate.release = TRUE;
    g_cond_broadcast(&clock_gate.cond);
    g_mutex_unlock(&clock_gate.lock);
    g_thread_join(stopping);
    g_thread_join(pushing);
    g_assert_true(freed);
    g_assert_cmpuint(f.received, ==, 128);
    g_assert_cmpuint(f.destroyed, ==, 128);
    g_mutex_lock(&clock_gate.lock);
    clock_gate.enabled = FALSE;
    clock_gate.state_lock = NULL;
    clock_gate.stop_thread = NULL;
    g_mutex_unlock(&clock_gate.lock);
    fixture_clear(&f);
  }
}

typedef struct { guint observed; } DropControl;

static GstPadProbeReturn
drop_every_tenth(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  DropControl *control = data;
  (void) pad;
  g_assert_nonnull(GST_PAD_PROBE_INFO_BUFFER(info));
  control->observed++;
  return control->observed % 10 == 0 ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
}

static GPtrArray *
pipeline_run(gboolean monitored, gboolean drop, gboolean tff, const gchar *name)
{
  GstHarness *h = gst_harness_new("deinterlace");
  GstElement *helper = monitored ? helper_new(4096) : NULL;
  GstPad *sink = gst_element_get_static_pad(h->element, "sink");
  GstPad *src = gst_element_get_static_pad(h->element, "src");
  DropControl control = {0};
  gulong drop_id = 0;
  GPtrArray *outputs = g_ptr_array_new_with_free_func((GDestroyNotify) gst_buffer_unref);
  g_object_set(h->element, "method", 4, NULL);
  if (helper) {
    g_assert_cmpint(attach(helper, h->srcpad, "decoder", 71), ==, 1);
    g_assert_cmpint(attach(helper, src, "deinterlace", 72), ==, 1);
    g_assert_cmpint(attach(helper, h->sinkpad, "sink", 73), ==, 1);
  }
  /* Installed after the DI observation, before the downstream harness sink. */
  if (drop)
    drop_id = gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER,
        drop_every_tenth, &control, NULL);
  gst_harness_set_caps_str(h, INPUT_CAPS, OUTPUT_CAPS);
  for (guint n = 0; n < 50; n++) {
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, 256, NULL);
    GstMapInfo map;
    g_assert_true(gst_buffer_map(buffer, &map, GST_MAP_WRITE));
    for (guint row = 0; row < 8; row++)
      for (guint col = 0; col < 32; col++)
        map.data[row * 32 + col] = (guint8) (n * 37 + row * 19 + col * 3);
    gst_buffer_unmap(buffer, &map);
    GST_BUFFER_PTS(buffer) = n * 40 * GST_MSECOND;
    GST_BUFFER_DURATION(buffer) = 40 * GST_MSECOND;
    GST_BUFFER_FLAG_SET(buffer, GST_VIDEO_BUFFER_FLAG_INTERLACED);
    if (tff) GST_BUFFER_FLAG_SET(buffer, GST_VIDEO_BUFFER_FLAG_TFF);
    g_assert_cmpint(gst_harness_push(h, buffer), ==, GST_FLOW_OK);
  }
  g_assert_true(gst_harness_push_event(h, gst_event_new_eos()));
  while (gst_harness_buffers_in_queue(h))
    g_ptr_array_add(outputs, gst_harness_pull(h));
  g_assert_cmpuint(outputs->len, ==, drop ? 90 : 100);
  if (drop) g_assert_cmpuint(control.observed, ==, 100);
  if (helper) {
    g_signal_emit_by_name(helper, "stop");
    emit_case(helper, name);
    gst_object_unref(helper);
  }
  if (drop_id) gst_pad_remove_probe(src, drop_id);
  gst_object_unref(src); gst_object_unref(sink);
  gst_harness_teardown(h);
  return outputs;
}

static void
compare_output(GPtrArray *reference, GPtrArray *actual, gboolean dropped)
{
  guint matched = 0;
  for (guint n = 0; n < reference->len; n++) {
    if (dropped && (n + 1) % 10 == 0) continue;
    GstBuffer *expected = g_ptr_array_index(reference, n);
    GstBuffer *observed = g_ptr_array_index(actual, matched++);
    GstMapInfo a, b;
    g_assert_cmpuint(GST_BUFFER_PTS(expected), ==, n * 20 * GST_MSECOND);
    g_assert_cmpuint(GST_BUFFER_PTS(observed), ==, GST_BUFFER_PTS(expected));
    g_assert_cmpuint(GST_BUFFER_DURATION(observed), ==, GST_BUFFER_DURATION(expected));
    g_assert_cmpuint(GST_BUFFER_FLAGS(observed), ==, GST_BUFFER_FLAGS(expected));
    g_assert_true(gst_buffer_map(expected, &a, GST_MAP_READ));
    g_assert_true(gst_buffer_map(observed, &b, GST_MAP_READ));
    g_assert_cmpuint(a.size, ==, b.size);
    g_assert_cmpint(memcmp(a.data, b.data, a.size), ==, 0);
    gst_buffer_unmap(observed, &b); gst_buffer_unmap(expected, &a);
  }
  g_assert_cmpuint(matched, ==, actual->len);
}

static void
test_pipeline_fields(void)
{
  for (guint tff = 0; tff < 2; tff++) {
    GPtrArray *reference = pipeline_run(FALSE, FALSE, tff, NULL);
    GPtrArray *observed = pipeline_run(TRUE, FALSE, tff,
        tff ? "pipeline_tff" : "pipeline_bff");
    GPtrArray *dropped = pipeline_run(TRUE, TRUE, tff,
        tff ? "pipeline_tff_drop" : "pipeline_bff_drop");
    compare_output(reference, observed, FALSE);
    compare_output(reference, dropped, TRUE);
    g_ptr_array_unref(reference); g_ptr_array_unref(observed); g_ptr_array_unref(dropped);
  }
}

static GPtrArray *
output_pipeline_run(gboolean monitored, gboolean drop, const gchar *name)
{
  GstHarness *h = gst_harness_new_parse(
      "videoconvert name=pack ! capsfilter caps=\"video/x-raw,format=UYVY\" ! "
      "videoscale name=scale method=4 ! "
      "capsfilter caps=\"video/x-raw,format=UYVY,width=32,height=16\"");
  GstElement *pack = gst_bin_get_by_name(GST_BIN(h->element), "pack");
  GstElement *scale = gst_bin_get_by_name(GST_BIN(h->element), "scale");
  GstPad *pack_src = gst_element_get_static_pad(pack, "src");
  GstPad *scale_src = gst_element_get_static_pad(scale, "src");
  GstElement *helper = monitored ? helper_new(4096) : NULL;
  DropControl control = {0};
  gulong drop_id = 0;
  GPtrArray *outputs = g_ptr_array_new_with_free_func((GDestroyNotify) gst_buffer_unref);
  if (helper) {
    g_assert_cmpint(attach(helper, h->sinkpad, "output-pack", 81), ==, -1);
    g_assert_cmpint(attach(helper, h->sinkpad, "output-scale", 82), ==, -1);
    g_assert_cmpint(attach(helper, pack_src, "output-pack", 81), ==, 1);
    g_assert_cmpint(attach(helper, scale_src, "output-scale", 82), ==, 1);
    g_assert_cmpint(attach(helper, h->sinkpad, "sink", 83), ==, 1);
  }
  /* A known loss between the two output observations must stay visible. */
  if (drop)
    drop_id = gst_pad_add_probe(pack_src, GST_PAD_PROBE_TYPE_BUFFER,
        drop_every_tenth, &control, NULL);
  gst_harness_set_caps_str(h, OUTPUT_CAPS,
      "video/x-raw,format=UYVY,width=32,height=16,"
      "interlace-mode=progressive,framerate=50/1,pixel-aspect-ratio=1/1");
  for (guint n = 0; n < 50; n++) {
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, 256, NULL);
    GstMapInfo map;
    g_assert_true(gst_buffer_map(buffer, &map, GST_MAP_WRITE));
    for (guint row = 0; row < 8; row++)
      for (guint col = 0; col < 32; col++)
        map.data[row * 32 + col] = (guint8) (n * 37 + row * 19 + col * 3);
    gst_buffer_unmap(buffer, &map);
    GST_BUFFER_PTS(buffer) = n * 20 * GST_MSECOND;
    GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
    g_assert_cmpint(gst_harness_push(h, buffer), ==, GST_FLOW_OK);
  }
  g_assert_true(gst_harness_push_event(h, gst_event_new_eos()));
  while (gst_harness_buffers_in_queue(h))
    g_ptr_array_add(outputs, gst_harness_pull(h));
  g_assert_cmpuint(outputs->len, ==, drop ? 45 : 50);
  if (drop) g_assert_cmpuint(control.observed, ==, 50);
  if (helper) {
    g_signal_emit_by_name(helper, "stop");
    emit_case(helper, name);
    gst_object_unref(helper);
  }
  if (drop_id) gst_pad_remove_probe(pack_src, drop_id);
  gst_object_unref(pack_src); gst_object_unref(scale_src);
  gst_object_unref(pack); gst_object_unref(scale);
  gst_harness_teardown(h);
  return outputs;
}

static void
test_output_stages(void)
{
  GPtrArray *reference = output_pipeline_run(FALSE, FALSE, NULL);
  GPtrArray *observed = output_pipeline_run(TRUE, FALSE, "output_stages");
  GPtrArray *dropped = output_pipeline_run(TRUE, TRUE, "output_stages_drop");
  compare_output(reference, observed, FALSE);
  compare_output(reference, dropped, TRUE);
  g_ptr_array_unref(reference); g_ptr_array_unref(observed); g_ptr_array_unref(dropped);
}

static gboolean
trigger(GstElement *helper, guint64 seqnum)
{
  gboolean accepted = FALSE;
  g_signal_emit_by_name(helper, "trigger", seqnum, &accepted);
  return accepted;
}

static void
set_changed_caps(PadFixture *f)
{
  GstCaps *caps = gst_caps_from_string(
      "video/x-raw,format=UYVY,width=16,height=8,interlace-mode=progressive,"
      "framerate=50/1,pixel-aspect-ratio=1/1");
  g_assert_true(gst_pad_push_event(f->src, gst_event_new_caps(caps)));
  gst_caps_unref(caps);
}

static void
test_window_context(void)
{
  PadFixture f;
  gboolean enabled = FALSE, accepted = FALSE;
  fixture_init(&f, 8);
  g_assert_false(trigger(f.helper, 0)); /* First-N does not silently turn into a ring. */
  g_object_set(f.helper, "window", TRUE, NULL);
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 91), ==, 1);
  g_object_set(f.helper, "window", FALSE, NULL); /* Locked after first attachment. */
  g_object_get(f.helper, "window", &enabled, NULL);
  g_assert_true(enabled);
  push_headers(&f);
  for (guint n = 0; n < 20; n++)
    push_buffer(&f, n * 40 * GST_MSECOND, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  finish(&f, "window_tail");
  fixture_clear(&f);

  fixture_init(&f, 8);
  g_object_set(f.helper, "window", TRUE, NULL);
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 92), ==, 1);
  push_headers(&f);
  for (guint n = 0; n < 20; n++)
    push_buffer(&f, n * 40 * GST_MSECOND, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  g_assert_true(gst_pad_push_event(f.src, gst_event_new_stream_start("trace-second")));
  set_changed_caps(&f);
  push_segment(&f, GST_FORMAT_TIME, 0, 0, 5 * GST_SECOND, 0x905);
  for (guint n = 0; n < 20; n++)
    push_buffer(&f, n * 20 * GST_MSECOND, GST_CLOCK_TIME_NONE, 20 * GST_MSECOND, 0);
  finish(&f, "window_epoch");
  fixture_clear(&f);

  fixture_init(&f, 4);
  g_object_set(f.helper, "window", TRUE, NULL);
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 93), ==, 1);
  push_headers(&f);
  for (guint n = 0; n < 10; n++)
    push_buffer(&f, n * 40 * GST_MSECOND, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  g_assert_true(gst_pad_push_event(f.src, gst_event_new_flush_start()));
  g_assert_true(gst_pad_push_event(f.src, gst_event_new_flush_stop(TRUE)));
  g_test_expect_message("GStreamer", G_LOG_LEVEL_WARNING,
      "*Got data flow before segment event*");
  g_test_expect_message("GStreamer", G_LOG_LEVEL_WARNING,
      "*Got data flow before segment event*");
  push_buffer(&f, 0, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  g_test_assert_expected_messages();
  for (guint n = 0; n < 9; n++) {
    g_signal_emit_by_name(f.helper, "control", "pause", (guint64) n, &accepted);
    g_assert_true(accepted);
  }
  finish(&f, "window_flush");
  fixture_clear(&f);

  fixture_init(&f, 128);
  g_object_set(f.helper, "window", TRUE, NULL);
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 94), ==, 1);
  push_headers(&f);
  push_buffer(&f, 0, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  g_assert_true(trigger(f.helper, 0xabcd));
  g_assert_false(trigger(f.helper, 0xbcde));
  push_buffer(&f, 40 * GST_MSECOND, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  finish(&f, "window_post_incomplete");
  gchar *before = snapshot(f.helper);
  g_assert_false(trigger(f.helper, 0xcdef));
  g_object_set(f.helper, "window", FALSE, NULL);
  gchar *after = snapshot(f.helper);
  g_assert_cmpstr(before, ==, after);
  g_free(before); g_free(after);
  fixture_clear(&f);

  fixture_init(&f, 1);
  g_object_set(f.helper, "window", TRUE, NULL);
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 95), ==, 1);
  push_headers(&f);
  push_buffer(&f, 0, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  g_assert_true(trigger(f.helper, 0x99));
  push_buffer(&f, 40 * GST_MSECOND, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  /* Frozen window context must remain at the old mixed/YUY2 TIME segment,
   * even though terminal CAPS/segment and live counters continue changing. */
  set_changed_caps(&f);
  push_segment(&f, GST_FORMAT_TIME, 0, 0, 9 * GST_SECOND, 0x906);
  push_buffer(&f, 0, GST_CLOCK_TIME_NONE, 20 * GST_MSECOND, 0);
  finish(&f, "window_tiny");
  fixture_clear(&f);

  fixture_init(&f, 4);
  g_object_set(f.helper, "window", TRUE, NULL);
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 96), ==, 1);
  for (guint n = 0; n < 16; n++) {
    g_signal_emit_by_name(f.helper, "control", "pause", (guint64) n, &accepted);
    g_assert_true(accepted);
  }
  finish(&f, "window_unknown");
  fixture_clear(&f);

  fixture_init(&f, 4);
  g_object_set(f.helper, "window", TRUE, NULL);
  g_assert_cmpint(attach(f.helper, f.src, "decoder", 97), ==, 1);
  push_headers(&f);
  push_buffer(&f, 0, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  g_assert_true(gst_pad_push_event(f.src, gst_event_new_stream_start("trace-unqualified")));
  for (guint n = 0; n < 16; n++) {
    g_signal_emit_by_name(f.helper, "control", "pause", (guint64) n, &accepted);
    g_assert_true(accepted);
  }
  finish(&f, "window_stream_unknown");
  fixture_clear(&f);
}

typedef struct { GstElement *helper; guint observed; gboolean window; } PointDropControl;

static GstPadProbeReturn
drop_late_picture(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  PointDropControl *control = data;
  (void) pad;
  g_assert_nonnull(GST_PAD_PROBE_INFO_BUFFER(info));
  if (++control->observed != 150) return GST_PAD_PROBE_OK;
  if (control->helper)
    g_assert_cmpint(trigger(control->helper, 0xd01), ==, control->window);
  return GST_PAD_PROBE_DROP;
}

static GPtrArray *
window_output_run(guint mode, gboolean drop, const gchar *name)
{
  GstHarness *h = gst_harness_new_parse(
      "videoconvert name=pack ! capsfilter caps=\"video/x-raw,format=UYVY\" ! "
      "videoscale name=scale method=4 ! "
      "capsfilter caps=\"video/x-raw,format=UYVY,width=32,height=16\"");
  GstElement *pack = gst_bin_get_by_name(GST_BIN(h->element), "pack");
  GstElement *scale = gst_bin_get_by_name(GST_BIN(h->element), "scale");
  GstPad *pack_src = gst_element_get_static_pad(pack, "src");
  GstPad *scale_src = gst_element_get_static_pad(scale, "src");
  GstElement *helper = mode ? helper_new(64) : NULL;
  PointDropControl control = {helper, 0, mode == 2};
  gulong drop_id = 0;
  GPtrArray *outputs = g_ptr_array_new_with_free_func((GDestroyNotify) gst_buffer_unref);
  if (helper) {
    g_object_set(helper, "window", mode == 2, NULL);
    g_assert_cmpint(attach(helper, pack_src, "output-pack", 101), ==, 1);
    g_assert_cmpint(attach(helper, scale_src, "output-scale", 102), ==, 1);
    g_assert_cmpint(attach(helper, h->sinkpad, "sink", 103), ==, 1);
  }
  if (drop)
    drop_id = gst_pad_add_probe(pack_src, GST_PAD_PROBE_TYPE_BUFFER,
        drop_late_picture, &control, NULL);
  gst_harness_set_caps_str(h, OUTPUT_CAPS,
      "video/x-raw,format=UYVY,width=32,height=16,"
      "interlace-mode=progressive,framerate=50/1,pixel-aspect-ratio=1/1");
  for (guint n = 0; n < 200; n++) {
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, 256, NULL);
    GstMapInfo map;
    g_assert_true(gst_buffer_map(buffer, &map, GST_MAP_WRITE));
    for (guint row = 0; row < 8; row++)
      for (guint col = 0; col < 32; col++)
        map.data[row * 32 + col] = (guint8) (n * 37 + row * 19 + col * 3);
    gst_buffer_unmap(buffer, &map);
    GST_BUFFER_PTS(buffer) = n * 20 * GST_MSECOND;
    GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
    g_assert_cmpint(gst_harness_push(h, buffer), ==, GST_FLOW_OK);
  }
  g_assert_true(gst_harness_push_event(h, gst_event_new_eos()));
  while (gst_harness_buffers_in_queue(h))
    g_ptr_array_add(outputs, gst_harness_pull(h));
  g_assert_cmpuint(outputs->len, ==, drop ? 199 : 200);
  if (drop) g_assert_cmpuint(control.observed, ==, 200);
  if (helper) {
    g_signal_emit_by_name(helper, "stop");
    emit_case(helper, name);
    gst_object_unref(helper);
  }
  if (drop_id) gst_pad_remove_probe(pack_src, drop_id);
  gst_object_unref(pack_src); gst_object_unref(scale_src);
  gst_object_unref(pack); gst_object_unref(scale);
  gst_harness_teardown(h);
  return outputs;
}

static void
compare_point_drop(GPtrArray *reference, GPtrArray *actual)
{
  guint matched = 0;
  for (guint n = 0; n < reference->len; n++) {
    if (n == 149) continue;
    GstBuffer *expected = g_ptr_array_index(reference, n);
    GstBuffer *observed = g_ptr_array_index(actual, matched++);
    GstMapInfo a, b;
    g_assert_cmpuint(GST_BUFFER_PTS(observed), ==, n * 20 * GST_MSECOND);
    g_assert_cmpuint(GST_BUFFER_PTS(observed), ==, GST_BUFFER_PTS(expected));
    g_assert_cmpuint(GST_BUFFER_DURATION(observed), ==, GST_BUFFER_DURATION(expected));
    g_assert_cmpuint(GST_BUFFER_FLAGS(observed), ==, GST_BUFFER_FLAGS(expected));
    g_assert_true(gst_buffer_map(expected, &a, GST_MAP_READ));
    g_assert_true(gst_buffer_map(observed, &b, GST_MAP_READ));
    g_assert_cmpuint(a.size, ==, b.size);
    g_assert_cmpint(memcmp(a.data, b.data, a.size), ==, 0);
    gst_buffer_unmap(observed, &b); gst_buffer_unmap(expected, &a);
  }
  g_assert_cmpuint(matched, ==, actual->len);
}

static void
test_window_output_loss(void)
{
  GPtrArray *reference = window_output_run(0, FALSE, NULL);
  GPtrArray *observed = window_output_run(2, FALSE, "window_output");
  GPtrArray *window_drop = window_output_run(2, TRUE, "window_output_drop");
  GPtrArray *first_drop = window_output_run(1, TRUE, "first_output_drop");
  compare_output(reference, observed, FALSE);
  compare_point_drop(reference, window_drop);
  compare_point_drop(reference, first_drop);
  g_ptr_array_unref(reference); g_ptr_array_unref(observed);
  g_ptr_array_unref(window_drop); g_ptr_array_unref(first_drop);
}

/* Ring4 deliberately discards all headers in first-N mode and all early
 * defects in rolling mode. These drive real pad probes, not summary helpers. */
static void
summary_headers(PadFixture *f, gboolean fifty)
{
  GstCaps *caps = gst_caps_from_string(fifty ? OUTPUT_CAPS : INPUT_CAPS);
  g_assert_true(gst_pad_push_event(f->src, gst_event_new_stream_start("summary-test")));
  g_assert_true(gst_pad_push_event(f->src, gst_event_new_caps(caps)));
  push_segment(f, GST_FORMAT_TIME, 0, 0, 0, 0xa01);
  gst_caps_unref(caps);
}

static void
test_pts_summary_retention(void)
{
  const gchar *names[3][2] = {
    {"summary_first_25", "summary_first_50"},
    {"window_summary_25", "window_summary_50"},
    {"window_summary_frozen_25", "window_summary_frozen_50"}
  };
  for (guint mode = 0; mode < 3; mode++) {
    for (guint fifty = 0; fifty < 2; fifty++) {
      PadFixture f;
      gboolean accepted = FALSE;
      GstClockTime period = (fifty ? 20 : 40) * GST_MSECOND;
      fixture_init(&f, 4);
      g_object_set(f.helper, "window", mode != 0, NULL);
      g_assert_cmpint(attach(f.helper, f.src, "output-scale", 120), ==, 1);
      for (guint n = 0; n < 6; n++) {
        g_signal_emit_by_name(f.helper, "control", "pause", (guint64) n, &accepted);
        g_assert_true(accepted);
      }
      summary_headers(&f, fifty);
      for (guint n = 0; n < 20; n++) {
        push_buffer(&f, n * period, GST_CLOCK_TIME_NONE, period, 0);
        if (mode == 2 && n == 3) g_assert_true(trigger(f.helper, 0xa02));
        if (n == 5) {
          GstCaps *caps = gst_caps_from_string(fifty ? OUTPUT_CAPS : INPUT_CAPS);
          g_assert_true(gst_pad_push_event(f.src, gst_event_new_caps(caps)));
          gst_caps_unref(caps);
        }
      }
      finish(&f, names[mode][fifty]);
      fixture_clear(&f);
    }
  }
}

static void
test_pts_summary_defects(void)
{
  const gchar *defects[] = {
    "gap", "duplicate", "reverse", "off_grid", "unknown_pts", "unknown_duration",
    "duration", "changed_caps", "unknown_caps", "vfr_caps", "segment", "rate",
    "applied_rate", "bytes_segment", "stream", "flush", "seek", "buffer_gap", "discont"
  };
  for (guint defect = 0; defect < G_N_ELEMENTS(defects); defect++) {
    PadFixture f;
    const GstClockTime period = 40 * GST_MSECOND;
    gchar *name = g_strconcat("window_summary_", defects[defect], NULL);
    fixture_init(&f, 4);
    g_object_set(f.helper, "window", TRUE, NULL);
    g_assert_cmpint(attach(f.helper, f.src, "output-scale", 121), ==, 1);
    summary_headers(&f, FALSE);
    for (guint n = 0; n < 20; n++) {
      GstClockTime pts = n * period, duration = period;
      guint flags = 0;
      if (defect == 0 && n >= 3) pts += period;
      if (defect == 1 && n >= 3) pts -= period;
      if (defect == 2 && n >= 3) pts -= 2 * period;
      if (defect == 3 && n >= 3) pts += 1;
      if (n == 3) {
        if (defect == 4) pts = GST_CLOCK_TIME_NONE;
        if (defect == 5) duration = GST_CLOCK_TIME_NONE;
        if (defect == 6) duration = period / 2;
        if (defect == 7) set_changed_caps(&f);
        if (defect == 8 || defect == 9) {
          GstCaps *caps = gst_caps_from_string(defect == 8 ? "application/x-summary-test" :
              "video/x-raw,format=YUY2,width=16,height=8,framerate=0/1");
          g_assert_true(gst_pad_push_event(f.src, gst_event_new_caps(caps)));
          gst_caps_unref(caps);
        }
        if (defect >= 10 && defect <= 13) {
          GstSegment segment;
          gst_segment_init(&segment, defect == 13 ? GST_FORMAT_BYTES : GST_FORMAT_TIME);
          if (defect == 11) segment.rate = 2.0;
          if (defect == 12) segment.applied_rate = 0.5;
          g_assert_true(gst_pad_push_event(f.src, gst_event_new_segment(&segment)));
        }
        if (defect == 14) summary_headers(&f, FALSE);
        if (defect == 15) {
          g_assert_true(gst_pad_push_event(f.src, gst_event_new_flush_start()));
          g_assert_true(gst_pad_push_event(f.src, gst_event_new_flush_stop(TRUE)));
          push_segment(&f, GST_FORMAT_TIME, 0, 0, 0, 0xa03);
        }
        if (defect == 16) {
          gboolean accepted = FALSE;
          g_signal_emit_by_name(f.helper, "control", "seek-request", (guint64) 0, &accepted);
          g_assert_true(accepted);
        }
        if (defect == 17) flags = GST_BUFFER_FLAG_GAP;
        if (defect == 18) flags = GST_BUFFER_FLAG_DISCONT;
      }
      push_buffer(&f, pts, GST_CLOCK_TIME_NONE, duration, flags);
    }
    finish(&f, name);
    fixture_clear(&f);
    g_free(name);
  }

  /* Changed CAPS must invalidate the summary after post-trigger storage has
   * frozen; the retained details deliberately still show only the old CAPS. */
  PadFixture f;
  fixture_init(&f, 4);
  g_object_set(f.helper, "window", TRUE, NULL);
  g_assert_cmpint(attach(f.helper, f.src, "output-scale", 122), ==, 1);
  summary_headers(&f, FALSE);
  push_buffer(&f, 0, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  g_assert_true(trigger(f.helper, 0xa04));
  push_buffer(&f, 40 * GST_MSECOND, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  set_changed_caps(&f);
  for (guint n = 4; n < 24; n++)
    push_buffer(&f, n * 20 * GST_MSECOND, GST_CLOCK_TIME_NONE, 20 * GST_MSECOND, 0);
  finish(&f, "window_summary_frozen_caps");
  fixture_clear(&f);
}

static void
test_pts_summary_limits(void)
{
  PadFixture f;
  fixture_init(&f, 4);
  g_object_set(f.helper, "window", TRUE, NULL);
  g_assert_cmpint(attach(f.helper, f.src, "output-scale", 123), ==, 1);
  summary_headers(&f, FALSE);
  push_buffer(&f, GST_CLOCK_TIME_NONE - 80 * GST_MSECOND,
      GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  push_buffer(&f, GST_CLOCK_TIME_NONE - 40 * GST_MSECOND,
      GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  finish(&f, "window_summary_overflow");
  fixture_clear(&f);

  fixture_init(&f, 4);
  g_object_set(f.helper, "window", TRUE, NULL);
  g_assert_cmpint(attach(f.helper, f.src, "output-scale", 124), ==, 1);
  summary_headers(&f, FALSE);
  push_buffer(&f, 0, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  /* Seed only a counter near its limit; the real next probe must saturate it.
   * Production gets no counter override API or special compilation path. */
  TraceState *state = ((GstCrystalHdStageTrace *) f.helper)->state;
  g_mutex_lock(&state->lock);
  state->attachments[0].pts_summary.gap_steps = G_MAXUINT64;
  g_mutex_unlock(&state->lock);
  push_buffer(&f, 80 * GST_MSECOND, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  finish(&f, "window_summary_saturation");
  fixture_clear(&f);

  const gchar *initial_names[] = {"window_summary_initial_unknown_caps",
    "window_summary_initial_vfr", "window_summary_initial_rate",
    "window_summary_initial_applied_rate", "window_summary_initial_bytes",
    "window_summary_negative_running_time", "window_summary_initial_vfr_max"};
  for (guint n = 0; n < G_N_ELEMENTS(initial_names); n++) {
    GstSegment segment;
    GstCaps *caps = gst_caps_from_string(n == 0 ? "application/x-summary-test" : n == 1 ?
        "video/x-raw,format=YUY2,width=16,height=8,framerate=0/1" : n == 6 ?
        "video/x-raw,format=YUY2,width=16,height=8,framerate=0/1,max-framerate=50/1" : INPUT_CAPS);
    fixture_init(&f, 4);
    g_object_set(f.helper, "window", TRUE, NULL);
    g_assert_cmpint(attach(f.helper, f.src, "output-scale", 125), ==, 1);
    g_assert_true(gst_pad_push_event(f.src, gst_event_new_stream_start("summary-initial")));
    g_assert_true(gst_pad_push_event(f.src, gst_event_new_caps(caps)));
    gst_caps_unref(caps);
    gst_segment_init(&segment, n == 4 ? GST_FORMAT_BYTES : GST_FORMAT_TIME);
    if (n == 2) segment.rate = 2.0;
    if (n == 3) segment.applied_rate = 0.5;
    if (n == 5) segment.start = GST_SECOND;
    g_assert_true(gst_pad_push_event(f.src, gst_event_new_segment(&segment)));
    for (guint k = 0; k < 20; k++)
      push_buffer(&f, k * 40 * GST_MSECOND, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
    finish(&f, initial_names[n]);
    fixture_clear(&f);
  }

  fixture_init(&f, 4);
  g_assert_cmpint(attach(f.helper, f.src, "output-scale", 126), ==, 1);
  summary_headers(&f, FALSE);
  push_buffer(&f, 0, GST_CLOCK_TIME_NONE, 40 * GST_MSECOND, 0);
  g_signal_emit_by_name(f.helper, "stop");
  emit_case(f.helper, "summary_missing_eos");
  fixture_clear(&f);

  fixture_init(&f, 4);
  g_assert_cmpint(attach(f.helper, f.src, "output-scale", 127), ==, 1);
  summary_headers(&f, FALSE);
  finish(&f, "summary_empty");
  fixture_clear(&f);
}

int
main(int argc, char **argv)
{
  gst_init(&argc, &argv);
  g_test_init(&argc, &argv, NULL);
  g_mutex_init(&clock_gate.lock);
  g_cond_init(&clock_gate.cond);
  guint64 expected_tid = 0;
#if defined(__linux__) && defined(SYS_gettid)
  long observed_tid = syscall(SYS_gettid);
  if (observed_tid > 0) expected_tid = (guint64) observed_tid;
#endif
  g_print("TRACE_EXPECT {\"flags\":%u,\"mixed_mode\":%u,\"progressive_mode\":%u,\"linux_tid\":%" G_GUINT64_FORMAT "}\n",
      GST_VIDEO_BUFFER_FLAG_INTERLACED | GST_VIDEO_BUFFER_FLAG_TFF |
      GST_VIDEO_BUFFER_FLAG_RFF | GST_VIDEO_BUFFER_FLAG_ONEFIELD,
      GST_VIDEO_INTERLACE_MODE_MIXED, GST_VIDEO_INTERLACE_MODE_PROGRESSIVE, expected_tid);
  g_test_add_func("/stage-trace/metadata", test_metadata);
  g_test_add_func("/stage-trace/flush-segment", test_flush);
  g_test_add_func("/stage-trace/capacity-controls", test_capacity_and_controls);
  g_test_add_func("/stage-trace/detach-lifetimes", test_detach_and_lifetime);
  g_test_add_func("/stage-trace/buffer-list-attachment-limit", test_buffer_list_and_attachment_limit);
  g_test_add_func("/stage-trace/concurrent-stop", test_concurrent_stop);
  g_test_add_func("/stage-trace/inflight-stop", test_inflight_stop);
  g_test_add_func("/stage-trace/50-picture-fields", test_pipeline_fields);
  g_test_add_func("/stage-trace/output-pack-scale", test_output_stages);
  g_test_add_func("/stage-trace/window-context", test_window_context);
  g_test_add_func("/stage-trace/window-output-loss", test_window_output_loss);
  g_test_add_func("/stage-trace/whole-run-pts-retention", test_pts_summary_retention);
  g_test_add_func("/stage-trace/whole-run-pts-defects", test_pts_summary_defects);
  g_test_add_func("/stage-trace/whole-run-pts-limits", test_pts_summary_limits);
  gint result = g_test_run();
  g_cond_clear(&clock_gate.cond);
  g_mutex_clear(&clock_gate.lock);
  return result;
}
