/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Production collector/wrapper with controlled clock/TID and a transfer-full
 * callee. No DIL, device, procfs, or installed plugin is used. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <gst/video/gstvideodecoder.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>
static int test_clock(clockid_t clock, struct timespec *stamp);
static long test_tid(long number, ...);
static GstFlowReturn test_finish(GstVideoDecoder *decoder, GstVideoCodecFrame *frame);
#define clock_gettime test_clock
#define syscall test_tid
#define gst_video_decoder_finish_frame test_finish
#include "../filters/gst/gst-plugin-1.0/gstcrystalhd-finish-trace.c"
#undef clock_gettime
#undef syscall
#undef gst_video_decoder_finish_frame
static gint clock_calls, tid_calls, finish_calls, fail_clock_at, fail_tid;
static GstFlowReturn return_flow;
static GstCrystalHdFinishTraceContext *mutate_context;
static GMutex block_lock;
static GCond block_cond;
static gboolean block_finish, entered_finish, release_finish;
static GPrivate synthetic_tid = G_PRIVATE_INIT(NULL);
static int
test_clock(clockid_t clock, struct timespec *stamp)
{
  gint nth = g_atomic_int_add(&clock_calls, 1) + 1;
  g_assert_cmpint(clock, ==, CLOCK_MONOTONIC);
  if (nth == fail_clock_at)
    return -1;
  stamp->tv_sec = 100;
  stamp->tv_nsec = nth * 1000;
  return 0;
}
static long
test_tid(long number, ...)
{
  g_atomic_int_inc(&tid_calls);
  g_assert_cmpint(number, ==, SYS_gettid);
  return fail_tid ? -1 : GPOINTER_TO_INT(g_private_get(&synthetic_tid));
}
static GstFlowReturn
test_finish(GstVideoDecoder *decoder, GstVideoCodecFrame *frame)
{
  (void)decoder;
  g_assert_nonnull(frame);
  g_atomic_int_inc(&finish_calls);
  memset(frame, 0xa5, sizeof(*frame));
  g_free(frame);
  if (mutate_context) {
    mutate_context->pts = GST_CLOCK_TIME_NONE;
    mutate_context->generation = 999;
  }
  g_mutex_lock(&block_lock);
  if (block_finish) {
    entered_finish = TRUE;
    g_cond_broadcast(&block_cond);
    while (!release_finish)
      g_cond_wait(&block_cond, &block_lock);
  }
  g_mutex_unlock(&block_lock);
  return return_flow;
}
static GstCrystalHdFinishTraceContext
context(guint64 generation, GstClockTime pts, guint64 frame)
{
  GstCrystalHdFinishTraceContext result = {1234, generation, 8, frame, pts, 40000000};
  return result;
}
static GstFlowReturn
call(GstCrystalHdFinishTrace *trace, GstCrystalHdFinishTraceContext *metadata)
{
  GstVideoCodecFrame *frame = g_new0(GstVideoCodecFrame, 1);
  frame->pts = metadata ? metadata->pts : 0;
  return gst_crystalhd_finish_trace_call(trace, NULL, frame, metadata);
}
static void
reset(void)
{
  clock_calls = tid_calls = finish_calls = fail_clock_at = fail_tid = 0;
  return_flow = GST_FLOW_OK;
  mutate_context = NULL;
  block_finish = entered_finish = release_finish = FALSE;
  g_private_set(&synthetic_tid, GINT_TO_POINTER(17));
}
static void
emit(const gchar *name, GstCrystalHdFinishTrace *trace)
{
  gchar *text = gst_crystalhd_finish_trace_snapshot(trace);
  g_print("FINISH_CASE %s %s\n", name, text);
  g_free(text);
}
static void
test_off(void)
{
  reset(); return_flow = GST_FLOW_ERROR;
  g_assert_cmpint(call(NULL, NULL), ==, GST_FLOW_ERROR);
  g_assert_cmpint(finish_calls, ==, 1);
  g_assert_cmpint(clock_calls, ==, 0); g_assert_cmpint(tid_calls, ==, 0);
  emit("off", NULL);
}
static void
test_metadata(void)
{
  GstCrystalHdFinishTrace *trace = gst_crystalhd_finish_trace_new(4);
  GstCrystalHdFinishTraceContext metadata = context(2, 0, 7);
  reset(); mutate_context = &metadata;
  g_assert_cmpint(call(trace, &metadata), ==, GST_FLOW_OK);
  g_assert_cmpuint(trace->spans[0].context.pts, ==, 0);
  g_assert_cmpuint(trace->spans[0].context.generation, ==, 2);
  mutate_context = NULL; metadata = context(3, 0, 7); return_flow = GST_FLOW_FLUSHING;
  g_assert_cmpint(call(trace, &metadata), ==, GST_FLOW_FLUSHING);
  metadata = context(3, GST_CLOCK_TIME_NONE, 8); metadata.duration = GST_CLOCK_TIME_NONE;
  return_flow = GST_FLOW_ERROR; g_assert_cmpint(call(trace, &metadata), ==, GST_FLOW_ERROR);
  g_assert_cmpint(finish_calls, ==, 3); g_assert_cmpint(clock_calls, ==, 6);
  g_assert_cmpint(tid_calls, ==, 6);
  emit("metadata", trace); gst_crystalhd_finish_trace_unref(trace);
}
static void
test_capacity(void)
{
  GstCrystalHdFinishTrace *trace = gst_crystalhd_finish_trace_new(1);
  GstCrystalHdFinishTraceContext metadata = context(1, 10, 3);
  reset(); g_assert_null(gst_crystalhd_finish_trace_new(0));
  g_assert_null(gst_crystalhd_finish_trace_new(GST_CRYSTALHD_FINISH_TRACE_MAX + 1));
  for (guint n = 0; n < 3; n++)
    g_assert_cmpint(call(trace, &metadata), ==, GST_FLOW_OK);
  g_assert_cmpint(finish_calls, ==, 3); g_assert_cmpint(clock_calls, ==, 2);
  g_assert_cmpint(tid_calls, ==, 2);
  emit("capacity", trace); gst_crystalhd_finish_trace_unref(trace);
}
static void
test_unknown(void)
{
  GstCrystalHdFinishTrace *trace = gst_crystalhd_finish_trace_new(2);
  GstCrystalHdFinishTraceContext metadata = context(1, 0, 0);
  reset(); fail_clock_at = 1; fail_tid = TRUE;
  g_assert_cmpint(call(trace, &metadata), ==, GST_FLOW_OK);
  emit("unknown", trace); gst_crystalhd_finish_trace_unref(trace);
}
typedef struct { GstCrystalHdFinishTrace *trace; GstCrystalHdFinishTraceContext metadata; } Worker;
static gpointer
worker(gpointer data)
{
  Worker *w = data;
  g_private_set(&synthetic_tid, GINT_TO_POINTER(29));
  g_assert_cmpint(call(w->trace, &w->metadata), ==, GST_FLOW_OK);
  return NULL;
}
static void
test_inflight(void)
{
  GstCrystalHdFinishTrace *trace = gst_crystalhd_finish_trace_new(2);
  Worker w = {trace, context(4, 0, 1)}; GThread *thread;
  reset(); block_finish = TRUE; thread = g_thread_new("finish-fixture", worker, &w);
  g_mutex_lock(&block_lock);
  while (!entered_finish) g_cond_wait(&block_cond, &block_lock);
  g_mutex_unlock(&block_lock);
  emit("inflight", trace);
  g_mutex_lock(&block_lock); release_finish = TRUE; g_cond_broadcast(&block_cond);
  g_mutex_unlock(&block_lock);
  g_thread_join(thread); emit("returned", trace);
  gst_crystalhd_finish_trace_unref(trace);

  /* A blocked omitted call is not represented by retained-record inflight. */
  trace = gst_crystalhd_finish_trace_new(1);
  reset(); g_assert_cmpint(call(trace, &w.metadata), ==, GST_FLOW_OK);
  w.trace = trace; block_finish = TRUE;
  thread = g_thread_new("omitted-finish-fixture", worker, &w);
  g_mutex_lock(&block_lock);
  while (!entered_finish) g_cond_wait(&block_cond, &block_lock);
  g_mutex_unlock(&block_lock);
  emit("omitted-blocked", trace);
  g_mutex_lock(&block_lock); release_finish = TRUE; g_cond_broadcast(&block_cond);
  g_mutex_unlock(&block_lock);
  g_thread_join(thread);
  g_assert_cmpint(clock_calls, ==, 2); g_assert_cmpint(tid_calls, ==, 2);
  gst_crystalhd_finish_trace_unref(trace);
}
static void
test_saturation(void)
{
  GstCrystalHdFinishTrace *trace = gst_crystalhd_finish_trace_new(1);
  GstCrystalHdFinishTraceContext metadata = context(1, 1, 1);
  reset(); trace->observed = G_MAXUINT64;
  g_assert_cmpint(call(trace, &metadata), ==, GST_FLOW_OK);
  g_assert_cmpint(clock_calls, ==, 0); g_assert_cmpint(tid_calls, ==, 0);
  emit("saturation", trace); gst_crystalhd_finish_trace_unref(trace);
}
int main(int argc, char **argv)
{
  int result;
  g_test_init(&argc, &argv, NULL); g_mutex_init(&block_lock); g_cond_init(&block_cond);
  g_test_add_func("/finish/off", test_off); g_test_add_func("/finish/metadata", test_metadata);
  g_test_add_func("/finish/capacity", test_capacity); g_test_add_func("/finish/unknown", test_unknown);
  g_test_add_func("/finish/inflight", test_inflight); g_test_add_func("/finish/saturation", test_saturation);
  result = g_test_run(); g_cond_clear(&block_cond); g_mutex_clear(&block_lock); return result;
}
