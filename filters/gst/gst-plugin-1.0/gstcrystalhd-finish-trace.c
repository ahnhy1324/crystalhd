/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <time.h>
#include <stdint.h>
#include <string.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include "gstcrystalhd-finish-trace.h"
typedef struct {
  GstCrystalHdFinishTraceContext context;
  guint64 call_id, enter_ns, return_ns, enter_tid, return_tid;
  gboolean entered_clock_valid, returned_clock_valid, complete;
  GstFlowReturn flow;
} FinishSpan;
struct _GstCrystalHdFinishTrace {
  gint references;
  GMutex lock;
  guint capacity, retained, inflight;
  guint64 observed;
  gboolean saturated;
  FinishSpan *spans;
};
static gboolean
finish_clock(guint64 *value)
{
  struct timespec stamp;
  if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0 || stamp.tv_sec < 0 ||
      stamp.tv_nsec < 0 || stamp.tv_nsec >= 1000000000L ||
      (guint64)stamp.tv_sec > (G_MAXUINT64 - 999999999U) / 1000000000U)
    return FALSE;
  *value = (guint64)stamp.tv_sec * 1000000000U + stamp.tv_nsec;
  return TRUE;
}
static guint64
finish_tid(void)
{
#if defined(__linux__) && defined(SYS_gettid)
  long tid = syscall(SYS_gettid);
  return tid > 0 ? (guint64)tid : 0;
#else
  return 0;
#endif
}
GstCrystalHdFinishTrace *
gst_crystalhd_finish_trace_new(guint capacity)
{
  GstCrystalHdFinishTrace *trace;
  if (!capacity || capacity > GST_CRYSTALHD_FINISH_TRACE_MAX)
    return NULL;
  trace = g_try_new0(GstCrystalHdFinishTrace, 1);
  if (!trace)
    return NULL;
  trace->spans = g_try_new0(FinishSpan, capacity);
  if (!trace->spans) {
    g_free(trace);
    return NULL;
  }
  trace->references = 1;
  trace->capacity = capacity;
  g_mutex_init(&trace->lock);
  return trace;
}
GstCrystalHdFinishTrace *
gst_crystalhd_finish_trace_ref(GstCrystalHdFinishTrace *trace)
{
  if (trace)
    g_atomic_int_inc(&trace->references);
  return trace;
}
void
gst_crystalhd_finish_trace_unref(GstCrystalHdFinishTrace *trace)
{
  if (trace && g_atomic_int_dec_and_test(&trace->references)) {
    g_assert(trace->inflight == 0);
    g_mutex_clear(&trace->lock);
    g_free(trace->spans);
    g_free(trace);
  }
}
guint
gst_crystalhd_finish_trace_capacity(GstCrystalHdFinishTrace *trace)
{
  return trace ? trace->capacity : 0;
}
#if defined(__GNUC__)
__attribute__((noinline, used))
#endif
guint
gst_crystalhd_finish_trace_enter(GstCrystalHdFinishTrace *trace,
    const GstCrystalHdFinishTraceContext *context)
{
  guint slot = G_MAXUINT;
  FinishSpan *span;
  if (!trace)
    return slot;
  g_mutex_lock(&trace->lock);
  if (trace->observed == G_MAXUINT64) {
    trace->saturated = TRUE;
  } else {
    trace->observed++;
    if (trace->retained < trace->capacity) {
      slot = trace->retained++;
      span = &trace->spans[slot];
      span->context = *context;
      span->call_id = trace->observed;
      span->entered_clock_valid = finish_clock(&span->enter_ns);
      span->enter_tid = finish_tid();
      trace->inflight++;
    }
  }
  g_mutex_unlock(&trace->lock);
  return slot;
}
#if defined(__GNUC__)
__attribute__((noinline, used))
#endif
void
gst_crystalhd_finish_trace_leave(GstCrystalHdFinishTrace *trace,
    guint slot, GstFlowReturn flow)
{
  guint64 now = 0, tid;
  gboolean valid;
  FinishSpan *span;
  if (!trace || slot == G_MAXUINT)
    return;
  /* Timestamp precedes collector contention and later outer stream locking. */
  valid = finish_clock(&now);
  tid = finish_tid();
  g_mutex_lock(&trace->lock);
  g_assert(slot < trace->retained);
  span = &trace->spans[slot];
  g_assert(!span->complete && trace->inflight > 0);
  span->return_ns = now;
  span->returned_clock_valid = valid;
  span->return_tid = tid;
  span->flow = flow;
  span->complete = TRUE;
  trace->inflight--;
  g_mutex_unlock(&trace->lock);
}
GstFlowReturn
gst_crystalhd_finish_trace_call(GstCrystalHdFinishTrace *trace,
    GstVideoDecoder *decoder, GstVideoCodecFrame *frame,
    const GstCrystalHdFinishTraceContext *context)
{
  guint slot;
  GstFlowReturn flow;
  if (!trace)
    return gst_video_decoder_finish_frame(decoder, frame);
  slot = gst_crystalhd_finish_trace_enter(trace, context);
  flow = gst_video_decoder_finish_frame(decoder, frame);
  gst_crystalhd_finish_trace_leave(trace, slot, flow);
  return flow;
}

static void
finish_json_u64(GString *out, guint64 value, gboolean valid)
{
  if (valid)
    g_string_append_printf(out, "%" G_GUINT64_FORMAT, value);
  else
    g_string_append(out, "null");
}

gchar *
gst_crystalhd_finish_trace_snapshot(GstCrystalHdFinishTrace *trace)
{
  FinishSpan *copy = NULL;
  guint retained, capacity, inflight, i;
  guint64 observed;
  gboolean saturated;
  GString *out;
  if (!trace)
    return g_strdup("{\"schema\":1,\"enabled\":false,\"records\":[]}");
  /* Capacity is immutable and the caller holds a reference. Allocate before
   * taking the streaming-side mutex; only bounded copying happens inside. */
  copy = g_try_new(FinishSpan, trace->capacity);
  if (!copy)
    return g_strdup("{\"schema\":1,\"enabled\":true,"
                    "\"snapshot_error\":\"allocation_failed\",\"records\":[]}");
  g_mutex_lock(&trace->lock);
  retained = trace->retained;
  capacity = trace->capacity;
  inflight = trace->inflight;
  observed = trace->observed;
  saturated = trace->saturated;
  if (retained)
    memcpy(copy, trace->spans, sizeof(*copy) * retained);
  g_mutex_unlock(&trace->lock);
  /* Never serialize JSON while holding a streaming-side collector lock. */
  out = g_string_new("{\"schema\":1,\"enabled\":true,"
      "\"clock\":\"CLOCK_MONOTONIC\",\"retention\":\"first\","
      "\"inflight_scope\":\"retained_records_only\",");
  g_string_append_printf(out,
      "\"capacity\":%u,\"observed\":%" G_GUINT64_FORMAT
      ",\"retained\":%u,\"omitted\":%" G_GUINT64_FORMAT
      ",\"inflight\":%u,\"counters_saturated\":%s,\"records\":[",
      capacity, observed, retained, observed - retained, inflight,
      saturated ? "true" : "false");
  for (i = 0; i < retained; i++) {
    FinishSpan *s = &copy[i];
    gboolean ordered = s->complete && s->entered_clock_valid &&
        s->returned_clock_valid && s->return_ns >= s->enter_ns;
    if (i)
      g_string_append_c(out, ',');
    g_string_append_printf(out,
        "{\"call_id\":%" G_GUINT64_FORMAT ",\"instance\":%" G_GUINT64_FORMAT
        ",\"generation\":%" G_GUINT64_FORMAT
        ",\"recovery_epoch\":%" G_GUINT64_FORMAT
        ",\"frame_number\":%" G_GUINT64_FORMAT ",\"pts\":",
        s->call_id, s->context.instance, s->context.generation,
        s->context.recovery_epoch, s->context.frame_number);
    finish_json_u64(out, s->context.pts, s->context.pts != GST_CLOCK_TIME_NONE);
    g_string_append(out, ",\"duration\":");
    finish_json_u64(out, s->context.duration,
                    s->context.duration != GST_CLOCK_TIME_NONE);
    g_string_append(out, ",\"enter_ns\":");
    finish_json_u64(out, s->enter_ns, s->entered_clock_valid);
    g_string_append(out, ",\"return_ns\":");
    finish_json_u64(out, s->return_ns, s->complete && s->returned_clock_valid);
    g_string_append(out, ",\"enter_tid\":");
    finish_json_u64(out, s->enter_tid, s->enter_tid != 0);
    g_string_append(out, ",\"return_tid\":");
    finish_json_u64(out, s->return_tid, s->complete && s->return_tid != 0);
    g_string_append_printf(out, ",\"complete\":%s,\"flow\":",
                            s->complete ? "true" : "false");
    if (s->complete)
      g_string_append_printf(out, "%d", s->flow);
    else
      g_string_append(out, "null");
    g_string_append(out, ",\"elapsed_ns\":");
    finish_json_u64(out, s->return_ns - s->enter_ns, ordered);
    g_string_append_printf(out, ",\"monotonic_order_valid\":%s}",
                            ordered ? "true" : "false");
  }
  g_string_append(out, "]}");
  g_free(copy);
  return g_string_free(out, FALSE);
}
