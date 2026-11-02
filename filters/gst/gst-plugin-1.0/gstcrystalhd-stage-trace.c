/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Bounded native pad observations, with no streaming Python. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gstcrystalhd-stage-trace.h"
#include <gst/video/video.h>
#include <math.h>
#include <string.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif
#define MAX_ATTACHMENTS 64U
#define DEFAULT_CAPACITY 4096U
#define MAX_CAPACITY 16384U
#define MAX_PTS_EPOCHS 16U

typedef enum {
  RECORD_BUFFER, RECORD_BUFFER_LIST_ITEM, RECORD_STREAM_START, RECORD_SEGMENT, RECORD_FLUSH_START,
  RECORD_FLUSH_STOP, RECORD_EOS, RECORD_QOS, RECORD_CAPS, RECORD_ATTACH,
  RECORD_DETACH, RECORD_CONTROL, RECORD_STOP, RECORD_TRIGGER
} RecordKind;
static const gchar *const kind_names[] = {
  "buffer", "buffer-list-item", "stream-start", "segment", "flush-start", "flush-stop", "eos",
  "qos", "caps", "attach", "detach", "control", "stop", "trigger"
};
static const gchar *const stage_names[] = {
  "decoder", "deinterlace", "sink", "output-pack", "output-scale"
};
static const gchar *const control_names[] = {"pause", "resume", "seek-request", "seek-failed"};
typedef struct {
  RecordKind kind;
  guint attachment, stage, flags, seqnum, control, list_index;
  guint64 order, token, ordinal, monotonic_ns, stream_generation, thread_id;
  guint64 segment_generation, flush_generation, value, list_ordinal;
  GstClockTime pts, dts, duration, running_time;
  gint running_sign;
  GstSegment segment;
  GstQOSType qos_type;
  gdouble proportion;
  GstClockTime qos_timestamp;
  GstClockTimeDiff diff;
  gboolean raw_video, video_info_valid, variable_fps;
  gint width, height, fps_n, fps_d, interlace_mode, field_order;
  GstVideoFormat video_format;
} TraceRecord;
/* State immediately BEFORE the earliest retained record, reconstructed only
 * from evicted observations. It must not be replaced by terminal pad state. */
typedef struct {
  gboolean seen, active, stream_known, caps_known, segment_known;
  gboolean segment_valid, flushing;
  guint64 through_order, stream_generation, segment_generation, flush_generation;
  TraceRecord caps, segment;
} TraceContext;
typedef enum {
  PTS_OK, PTS_CONTEXT_UNKNOWN, PTS_CAPS_UNSUPPORTED, PTS_CAPS_CHANGED,
  PTS_STREAM_CHANGED, PTS_SEGMENT_UNSUPPORTED, PTS_SEGMENT_CHANGED,
  PTS_FLUSH, PTS_SEEK, PTS_LIST, PTS_UNKNOWN, PTS_DURATION_UNKNOWN,
  PTS_DURATION_INVALID, PTS_DUPLICATE, PTS_REVERSE, PTS_OFF_GRID,
  PTS_GAP, PTS_OVERFLOW, PTS_SATURATED, PTS_AFTER_EOS, PTS_DETACHED,
  PTS_BUFFER_GAP, PTS_BUFFER_DISCONT, PTS_EPOCH_OVERFLOW, PTS_EPOCH_EMPTY,
  PTS_FLUSH_UNPAIRED, PTS_SEGMENT_BOUNDS, PTS_RUNNING_NEGATIVE,
  PTS_SEEK_FAILED, PTS_EOS_MISSING
} PtsViolation;
static const gchar *const pts_violation_names[] = {
  "none", "context-unknown", "caps-unsupported", "caps-changed",
  "stream-changed", "segment-unsupported", "segment-changed", "flush",
  "seek-request", "buffer-list", "unknown-pts", "unknown-duration",
  "invalid-duration", "duplicate", "reverse", "off-grid", "gap",
  "arithmetic-overflow", "counter-saturation", "buffer-after-eos", "detached-before-eos",
  "buffer-gap", "buffer-discont", "epoch-overflow", "empty-segment",
  "flush-unpaired", "segment-bounds", "negative-running-time", "seek-failed", "missing-eos"
};
/* Independent of the detail ring: one bounded metadata accumulator per pad.
 * "gap" describes a timestamp discontinuity, never a physical loss census. */
typedef struct {
  guint64 buffers, covered, unknown_pts, unknown_duration, invalid_duration;
  guint64 unqualified_buffers, duplicate_events, reverse_events, off_grid_events;
  guint64 gap_events, gap_steps, epochs, caps_changes, segment_events;
  guint64 flush_events, seek_requests;
  GstClockTime period, first_pts, last_pts, previous_pts, origin_pts;
  gboolean have_pts, previous_valid, origin_valid, caps_supported, segment_supported;
  gboolean arithmetic_overflow, counters_saturated;
  PtsViolation violation;
  guint64 violation_order, violation_ordinal;
  GstClockTime violation_pts, violation_expected;
} TracePtsSummary;
typedef enum {
  EPOCH_OPEN, EPOCH_SEGMENT, EPOCH_FLUSH_START, EPOCH_STREAM_START,
  EPOCH_EOS, EPOCH_DETACH, EPOCH_STOP
} TraceEpochEnd;
static const gchar *const epoch_end_names[] = {
  NULL, "segment", "flush-start", "stream-start", "eos", "detach", "stop"
};
/* Fixed metadata only: no buffer, caps or payload references. */
typedef struct {
  guint64 stream_generation, segment_generation, flush_generation;
  guint64 start_order, end_order;
  guint seqnum;
  TraceEpochEnd end_reason;
  GstSegment segment;
  TraceRecord caps;
  guint64 buffers, covered, unqualified_buffers, unknown_pts, unknown_duration;
  guint64 invalid_duration, duplicate_events, reverse_events, off_grid_events;
  guint64 gap_events, gap_steps, first_ordinal, last_ordinal;
  GstClockTime first_pts, last_pts, first_running_time, last_running_time;
  GstClockTime period, previous_pts, origin_pts;
  gboolean context_supported, previous_valid, origin_valid;
  gboolean arithmetic_overflow, counters_saturated;
  PtsViolation violation;
  guint64 violation_order, violation_ordinal;
  GstClockTime violation_pts, violation_expected;
} TracePtsEpoch;
typedef struct {
  guint n_rows, current, seek_phase;
  guint64 buffers, unassigned_buffers, stream_events, segment_events;
  guint64 flush_starts, flush_stops, seek_requests, seek_failures, seek_value;
  gboolean overflow, counters_saturated, stream_ready, caps_ready;
  gboolean segment_ready, flushing, eos, pending_seek;
  gboolean baseline_known, caps_consistent, stream_since_segment, flush_since_segment;
  TraceRecord caps, baseline_caps;
  PtsViolation violation;
  guint64 violation_order, violation_ordinal;
  GstClockTime violation_pts, violation_expected;
  TracePtsEpoch rows[MAX_PTS_EPOCHS];
} TracePtsEpochs;
typedef struct {
  GstPad *pad; /* Strong ref, moved out before probe removal. */
  gulong probe;
  guint stage;
  guint64 token, buffers, events, missing_pts, eos_count, qos_count;
  guint64 buffer_lists, list_items, list_missing_pts;
  guint64 stream_generation, segment_generation, flush_generation;
  GstSegment segment;
  gboolean active, flushing, has_segment;
  GstCaps *caps; /* One metadata ref; no buffer/payload references. */
  TracePtsSummary pts_summary;
  TracePtsEpochs pts_epochs;
} TraceAttachment;
typedef struct {
  gint references;
  GMutex lock;
  TraceRecord *records;
  TraceAttachment attachments[MAX_ATTACHMENTS];
  TraceContext context[MAX_ATTACHMENTS];
  guint capacity, retained, n_attachments, first;
  guint post_target, post_retained;
  guint64 observed, dropped, rejected_attachments, configuration_rejections;
  guint64 seek_requests, seek_failures;
  guint64 overwritten, unretained_after_window, trigger_seqnum, trigger_order;
  gboolean configured, closed, saturated, attachment_limit_reached, allocation_failed;
  gboolean buffer_list_ambiguity, window, triggered, window_complete;
} TraceState;
typedef struct { TraceState *state; guint attachment; } TraceHook;
typedef struct { GstElement parent; TraceState *state; } GstCrystalHdStageTrace;
typedef struct {
  GstElementClass parent_class;
  gint (*attach)(GstCrystalHdStageTrace *, GstPad *, const gchar *, guint64);
  void (*detach_pad)(GstCrystalHdStageTrace *, GstPad *);
  void (*stop)(GstCrystalHdStageTrace *);
  gchar *(*snapshot)(GstCrystalHdStageTrace *);
  gboolean (*control)(GstCrystalHdStageTrace *, const gchar *, guint64);
  gboolean (*trigger)(GstCrystalHdStageTrace *, guint64);
} GstCrystalHdStageTraceClass;
enum { PROP_0, PROP_CAPACITY, PROP_WINDOW };
G_DEFINE_TYPE(GstCrystalHdStageTrace, gst_crystalhd_stage_trace, GST_TYPE_ELEMENT)

static guint64
increment(TraceState *state, guint64 *value)
{
  if (*value != G_MAXUINT64) ++*value;
  else state->saturated = TRUE;
  return *value;
}
static guint64
emitting_thread_id(void)
{
#if defined(__linux__) && defined(SYS_gettid)
  /* syscall() preserves compatibility with libc predating its gettid wrapper. */
  long tid = syscall(SYS_gettid);
  if (tid > 0) return (guint64) tid;
#endif
  return 0;
}
static TraceState *
trace_state_ref(TraceState *state)
{
  g_atomic_int_inc(&state->references);
  return state;
}
static void
trace_state_unref(TraceState *state)
{
  guint i;
  if (!g_atomic_int_dec_and_test(&state->references)) return;
  /* Hook destruction can run under a pad lock. It releases memory only:
   * pad refs were moved out before stop/detach removed the hooks. */
  g_free(state->records);
  for (i = 0; i < state->n_attachments; i++)
    if (state->attachments[i].caps) gst_caps_unref(state->attachments[i].caps);
  g_mutex_clear(&state->lock);
  g_free(state);
}
/* Called before next_record(): even a full/frozen ring cannot hide a violation.
 * The upcoming order is checked rather than wrapping a saturated global count. */
static void
pts_violation(TraceState *state, TraceAttachment *entry, PtsViolation reason,
              GstClockTime pts, GstClockTime expected)
{
  TracePtsSummary *summary = &entry->pts_summary;
  if (summary->violation != PTS_OK) return;
  summary->violation = reason;
  summary->violation_order = state->observed == G_MAXUINT64 ? G_MAXUINT64 : state->observed + 1;
  summary->violation_ordinal = entry->buffers;
  summary->violation_pts = pts;
  summary->violation_expected = expected;
}
static void
pts_add(TraceState *state, TraceAttachment *entry, guint64 *counter, guint64 amount)
{
  if (amount > G_MAXUINT64 - *counter) {
    *counter = G_MAXUINT64;
    state->saturated = entry->pts_summary.counters_saturated = TRUE;
    pts_violation(state, entry, PTS_SATURATED, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
  } else *counter += amount;
}
static void
pts_boundary(TraceState *state, TraceAttachment *entry, PtsViolation reason)
{
  TracePtsSummary *summary = &entry->pts_summary;
  pts_add(state, entry, &summary->epochs, 1);
  summary->previous_valid = summary->origin_valid = FALSE;
  pts_violation(state, entry, reason, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
}
static void
summarize_buffer(TraceState *state, TraceAttachment *entry, GstBuffer *buffer,
                 gint running_sign, GstClockTime running_time)
{
  TracePtsSummary *summary = &entry->pts_summary;
  GstClockTime pts = GST_BUFFER_PTS(buffer), duration = GST_BUFFER_DURATION(buffer);
  gboolean context = entry->stream_generation == 1 && summary->caps_supported &&
      summary->segment_supported && !entry->flushing && running_sign > 0;
  pts_add(state, entry, &summary->buffers, 1);
  if (state->saturated) {
    summary->counters_saturated = TRUE;
    pts_violation(state, entry, PTS_SATURATED, pts, GST_CLOCK_TIME_NONE);
  }
  if (entry->eos_count)
    pts_violation(state, entry, PTS_AFTER_EOS, pts, GST_CLOCK_TIME_NONE);
  if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_GAP))
    pts_violation(state, entry, PTS_BUFFER_GAP, pts, GST_CLOCK_TIME_NONE);
  if (summary->buffers > 1 && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT))
    pts_boundary(state, entry, PTS_BUFFER_DISCONT);
  if (!GST_CLOCK_TIME_IS_VALID(pts)) {
    pts_add(state, entry, &summary->unknown_pts, 1);
    pts_violation(state, entry, PTS_UNKNOWN, pts, GST_CLOCK_TIME_NONE);
  }
  if (!GST_CLOCK_TIME_IS_VALID(duration)) {
    pts_add(state, entry, &summary->unknown_duration, 1);
    pts_violation(state, entry, PTS_DURATION_UNKNOWN, pts, GST_CLOCK_TIME_NONE);
  } else if (summary->period && duration != summary->period) {
    pts_add(state, entry, &summary->invalid_duration, 1);
    pts_violation(state, entry, PTS_DURATION_INVALID, pts, GST_CLOCK_TIME_NONE);
  }
  if (!context)
    pts_violation(state, entry, PTS_CONTEXT_UNKNOWN, pts, GST_CLOCK_TIME_NONE);
  pts_add(state, entry, context && GST_CLOCK_TIME_IS_VALID(pts) &&
      GST_CLOCK_TIME_IS_VALID(duration) && duration == summary->period ?
      &summary->covered : &summary->unqualified_buffers, 1);
  if (!GST_CLOCK_TIME_IS_VALID(pts)) { summary->previous_valid = FALSE; return; }
  if (!summary->have_pts) summary->first_pts = pts;
  summary->have_pts = TRUE; summary->last_pts = pts;
  if (context) {
    if (!summary->origin_valid) {
      summary->origin_pts = pts; summary->origin_valid = TRUE;
    } else if (pts >= summary->origin_pts && (pts - summary->origin_pts) % summary->period) {
      pts_add(state, entry, &summary->off_grid_events, 1);
      pts_violation(state, entry, PTS_OFF_GRID, pts, GST_CLOCK_TIME_NONE);
    }
    if (summary->previous_valid) {
      GstClockTime previous = summary->previous_pts;
      if (pts == previous) {
        pts_add(state, entry, &summary->duplicate_events, 1);
        pts_violation(state, entry, PTS_DUPLICATE, pts, GST_CLOCK_TIME_NONE);
      } else if (pts < previous) {
        pts_add(state, entry, &summary->reverse_events, 1);
        pts_violation(state, entry, PTS_REVERSE, pts, GST_CLOCK_TIME_NONE);
      } else if (pts - previous > summary->period) {
        pts_add(state, entry, &summary->gap_events, 1);
        /* Only count whole omitted cadence steps on an exact grid delta. */
        if ((pts - previous) % summary->period == 0)
          pts_add(state, entry, &summary->gap_steps, (pts - previous) / summary->period - 1);
        pts_violation(state, entry, PTS_GAP, pts, previous + summary->period);
      }
    }
    /* NONE is reserved, so a mathematically representable UINT64_MAX is
     * already outside the timestamp domain. Check before any addition. */
    if (summary->period >= GST_CLOCK_TIME_NONE - pts ||
        summary->period >= GST_CLOCK_TIME_NONE - running_time) {
      summary->arithmetic_overflow = TRUE;
      pts_violation(state, entry, PTS_OVERFLOW, pts, GST_CLOCK_TIME_NONE);
    }
    summary->previous_pts = pts; summary->previous_valid = TRUE;
  } else summary->previous_valid = summary->origin_valid = FALSE;
}
static guint64
epoch_order(const TraceState *state)
{
  return state->observed == G_MAXUINT64 ? G_MAXUINT64 : state->observed + 1;
}
static TracePtsEpoch *
epoch_current(TraceAttachment *entry)
{
  TracePtsEpochs *epochs = &entry->pts_epochs;
  return epochs->current < epochs->n_rows ? &epochs->rows[epochs->current] : NULL;
}
static void
epoch_violation_at(TraceAttachment *entry, TracePtsEpoch *row, PtsViolation reason,
                   GstClockTime pts, GstClockTime expected, guint64 order)
{
  TracePtsEpochs *epochs = &entry->pts_epochs;
  if (epochs->violation == PTS_OK) {
    epochs->violation = reason; epochs->violation_order = order;
    epochs->violation_ordinal = entry->buffers;
    epochs->violation_pts = pts; epochs->violation_expected = expected;
  }
  if (row && row->violation == PTS_OK) {
    row->violation = reason; row->violation_order = order;
    row->violation_ordinal = entry->buffers;
    row->violation_pts = pts; row->violation_expected = expected;
  }
}
static void
epoch_violation(TraceState *state, TraceAttachment *entry, TracePtsEpoch *row,
                PtsViolation reason, GstClockTime pts, GstClockTime expected)
{
  epoch_violation_at(entry, row, reason, pts, expected, epoch_order(state));
}
static void
epoch_add(TraceState *state, TraceAttachment *entry, TracePtsEpoch *row,
          guint64 *counter, guint64 amount)
{
  if (amount > G_MAXUINT64 - *counter) {
    *counter = G_MAXUINT64;
    state->saturated = entry->pts_epochs.counters_saturated = TRUE;
    if (row) row->counters_saturated = TRUE;
    epoch_violation(state, entry, row, PTS_SATURATED, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
  } else *counter += amount;
}
static void
epoch_close(TraceAttachment *entry, TraceEpochEnd reason, guint64 order)
{
  TracePtsEpoch *row = epoch_current(entry);
  if (!row) return;
  row->end_order = order; row->end_reason = reason;
  if (!row->buffers)
    epoch_violation_at(entry, row, PTS_EPOCH_EMPTY, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, order);
  if (reason == EPOCH_DETACH || reason == EPOCH_STOP)
    epoch_violation_at(entry, row, PTS_EOS_MISSING, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, order);
  entry->pts_epochs.current = MAX_PTS_EPOCHS;
}
static void
epoch_finish(TraceAttachment *entry, TraceEpochEnd reason, guint64 order)
{
  TracePtsEpochs *epochs = &entry->pts_epochs;
  epoch_close(entry, reason, order);
  if (!epochs->n_rows || epochs->rows[epochs->n_rows - 1].end_reason != EPOCH_EOS ||
      epochs->rows[epochs->n_rows - 1].stream_generation != entry->stream_generation ||
      epochs->rows[epochs->n_rows - 1].segment_generation != entry->segment_generation ||
      epochs->rows[epochs->n_rows - 1].flush_generation != entry->flush_generation)
    epoch_violation_at(entry, NULL, PTS_EOS_MISSING, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, order);
  if (epochs->flushing || epochs->flush_starts != epochs->flush_stops)
    epoch_violation_at(entry, NULL, PTS_FLUSH_UNPAIRED, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, order);
  if (epochs->pending_seek)
    epoch_violation_at(entry, NULL, PTS_SEEK, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, order);
}
static gboolean
epoch_running_time(const GstSegment *segment, GstClockTime pts, GstClockTime *running)
{
  GstClockTime delta = pts - segment->start;
  if (delta >= segment->offset) {
    GstClockTime remainder = delta - segment->offset;
    if (remainder >= GST_CLOCK_TIME_NONE - segment->base) return FALSE;
    *running = segment->base + remainder;
  } else {
    GstClockTime remainder = segment->offset - delta;
    if (segment->base < remainder) return FALSE;
    *running = segment->base - remainder;
  }
  return TRUE;
}
static void
epoch_buffer(TraceState *state, TraceAttachment *entry, GstBuffer *buffer,
             gint running_sign, GstClockTime observed_running)
{
  TracePtsEpochs *epochs = &entry->pts_epochs;
  TracePtsEpoch *row = epoch_current(entry);
  GstClockTime pts = GST_BUFFER_PTS(buffer), duration = GST_BUFFER_DURATION(buffer);
  GstClockTime running = GST_CLOCK_TIME_NONE;
  gboolean valid = TRUE, context;
  epoch_add(state, entry, NULL, &epochs->buffers, 1);
  if (!row || epochs->overflow) {
    epoch_add(state, entry, NULL, &epochs->unassigned_buffers, 1);
    epoch_violation(state, entry, NULL, epochs->flushing ? PTS_FLUSH :
        epochs->eos ? PTS_AFTER_EOS : PTS_CONTEXT_UNKNOWN, pts, GST_CLOCK_TIME_NONE);
    return;
  }
  epoch_add(state, entry, row, &row->buffers, 1);
  if (row->buffers == 1) row->first_ordinal = entry->buffers;
  row->last_ordinal = entry->buffers;
  context = row->context_supported && epochs->stream_ready && epochs->caps_ready &&
      epochs->segment_ready && !epochs->flushing && !epochs->eos;
  if (!context) {
    valid = FALSE;
    epoch_violation(state, entry, row, epochs->flushing ? PTS_FLUSH : PTS_CONTEXT_UNKNOWN,
        pts, GST_CLOCK_TIME_NONE);
  }
  if (state->saturated) {
    valid = FALSE; row->counters_saturated = epochs->counters_saturated = TRUE;
    epoch_violation(state, entry, row, PTS_SATURATED, pts, GST_CLOCK_TIME_NONE);
  }
  if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_GAP)) {
    valid = FALSE; epoch_violation(state, entry, row, PTS_BUFFER_GAP, pts, GST_CLOCK_TIME_NONE);
  }
  if (row->buffers > 1 && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)) {
    valid = FALSE; epoch_violation(state, entry, row, PTS_BUFFER_DISCONT, pts, GST_CLOCK_TIME_NONE);
  }
  if (!GST_CLOCK_TIME_IS_VALID(pts)) {
    valid = FALSE; epoch_add(state, entry, row, &row->unknown_pts, 1);
    epoch_violation(state, entry, row, PTS_UNKNOWN, pts, GST_CLOCK_TIME_NONE);
  }
  if (!GST_CLOCK_TIME_IS_VALID(duration)) {
    valid = FALSE; epoch_add(state, entry, row, &row->unknown_duration, 1);
    epoch_violation(state, entry, row, PTS_DURATION_UNKNOWN, pts, GST_CLOCK_TIME_NONE);
  } else if (!duration || (row->period && duration != row->period)) {
    valid = FALSE; epoch_add(state, entry, row, &row->invalid_duration, 1);
    epoch_violation(state, entry, row, PTS_DURATION_INVALID, pts, GST_CLOCK_TIME_NONE);
  }
  if (context && GST_CLOCK_TIME_IS_VALID(pts)) {
    if (pts < row->segment.start ||
        (GST_CLOCK_TIME_IS_VALID(row->segment.stop) && pts >= row->segment.stop)) {
      valid = FALSE; epoch_violation(state, entry, row, PTS_SEGMENT_BOUNDS, pts, GST_CLOCK_TIME_NONE);
    } else if (!epoch_running_time(&row->segment, pts, &running)) {
      GstClockTime delta = pts - row->segment.start;
      valid = FALSE;
      if (delta < row->segment.offset && row->segment.base < row->segment.offset - delta)
        epoch_violation(state, entry, row, PTS_RUNNING_NEGATIVE, pts, GST_CLOCK_TIME_NONE);
      else {
        row->arithmetic_overflow = TRUE;
        epoch_violation(state, entry, row, PTS_OVERFLOW, pts, GST_CLOCK_TIME_NONE);
      }
    } else if (running_sign <= 0 || observed_running != running) {
      valid = FALSE; epoch_violation(state, entry, row, PTS_CONTEXT_UNKNOWN, pts, GST_CLOCK_TIME_NONE);
    }
    if (!row->origin_valid) { row->origin_pts = pts; row->origin_valid = TRUE; }
    else if (pts >= row->origin_pts && (pts - row->origin_pts) % row->period) {
      valid = FALSE; epoch_add(state, entry, row, &row->off_grid_events, 1);
      epoch_violation(state, entry, row, PTS_OFF_GRID, pts, GST_CLOCK_TIME_NONE);
    }
    if (row->previous_valid) {
      GstClockTime previous = row->previous_pts;
      if (pts == previous) {
        valid = FALSE; epoch_add(state, entry, row, &row->duplicate_events, 1);
        epoch_violation(state, entry, row, PTS_DUPLICATE, pts, GST_CLOCK_TIME_NONE);
      } else if (pts < previous) {
        valid = FALSE; epoch_add(state, entry, row, &row->reverse_events, 1);
        epoch_violation(state, entry, row, PTS_REVERSE, pts, GST_CLOCK_TIME_NONE);
      } else if (pts - previous > row->period) {
        valid = FALSE; epoch_add(state, entry, row, &row->gap_events, 1);
        if ((pts - previous) % row->period == 0)
          epoch_add(state, entry, row, &row->gap_steps, (pts - previous) / row->period - 1);
        epoch_violation(state, entry, row, PTS_GAP, pts,
            row->period < GST_CLOCK_TIME_NONE - previous ? previous + row->period : GST_CLOCK_TIME_NONE);
      }
    }
    row->previous_pts = pts; row->previous_valid = TRUE;
  } else row->previous_valid = FALSE;
  if (GST_CLOCK_TIME_IS_VALID(pts) && GST_CLOCK_TIME_IS_VALID(duration)) {
    if (duration >= GST_CLOCK_TIME_NONE - pts ||
        (GST_CLOCK_TIME_IS_VALID(running) && duration >= GST_CLOCK_TIME_NONE - running)) {
      valid = FALSE; row->arithmetic_overflow = TRUE;
      epoch_violation(state, entry, row, PTS_OVERFLOW, pts, GST_CLOCK_TIME_NONE);
    } else if (context && GST_CLOCK_TIME_IS_VALID(row->segment.stop) && pts + duration > row->segment.stop) {
      valid = FALSE; epoch_violation(state, entry, row, PTS_SEGMENT_BOUNDS, pts, GST_CLOCK_TIME_NONE);
    }
  }
  if (row->buffers == 1) { row->first_pts = pts; row->first_running_time = running; }
  row->last_pts = pts; row->last_running_time = running;
  epoch_add(state, entry, row, valid ? &row->covered : &row->unqualified_buffers, 1);
}
static gboolean
epoch_barrier_valid(const TraceAttachment *entry, guint64 order)
{
  const TracePtsEpochs *epochs = &entry->pts_epochs;
  const TracePtsEpoch *previous;
  guint64 stream_delta, flush_delta;
  if (!epochs->n_rows)
    return entry->stream_generation == 1 && entry->segment_generation == 1 &&
        !entry->flush_generation && epochs->stream_events == 1 &&
        !epochs->flush_starts && !epochs->flush_stops;
  previous = &epochs->rows[epochs->n_rows - 1];
  if (entry->stream_generation < previous->stream_generation ||
      entry->flush_generation < previous->flush_generation ||
      previous->segment_generation == G_MAXUINT64 ||
      entry->segment_generation != previous->segment_generation + 1) return FALSE;
  stream_delta = entry->stream_generation - previous->stream_generation;
  flush_delta = entry->flush_generation - previous->flush_generation;
  if (stream_delta > 1 || flush_delta > 1) return FALSE;
  switch (previous->end_reason) {
    case EPOCH_SEGMENT:
      return !stream_delta && !flush_delta && previous->end_order == order;
    case EPOCH_FLUSH_START:
      return flush_delta == 1 && previous->end_order < order;
    case EPOCH_STREAM_START:
      return stream_delta == 1 && previous->end_order < order;
    case EPOCH_EOS:
      return flush_delta == 1 && previous->end_order < order;
    default: return FALSE;
  }
}
static void
epoch_event(TraceState *state, TraceAttachment *entry, RecordKind kind, guint seqnum,
            const GstSegment *segment, const TraceRecord *caps,
            gboolean caps_fixed, gboolean caps_changed)
{
  TracePtsEpochs *epochs = &entry->pts_epochs;
  TracePtsEpoch *row = epoch_current(entry);
  guint64 order = epoch_order(state);
  switch (kind) {
    case RECORD_STREAM_START:
      epoch_add(state, entry, row, &epochs->stream_events, 1);
      if (epochs->stream_since_segment)
        epoch_violation(state, entry, row, PTS_STREAM_CHANGED, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      epoch_close(entry, EPOCH_STREAM_START, order);
      epochs->stream_since_segment = TRUE;
      epochs->stream_ready = TRUE;
      epochs->caps_ready = epochs->segment_ready = FALSE;
      break;
    case RECORD_CAPS: {
      gboolean supported = caps_fixed && caps->raw_video && caps->video_info_valid &&
          !caps->variable_fps && caps->width > 0 && caps->height > 0 &&
          caps->fps_d == 1 && (caps->fps_n == 25 || caps->fps_n == 50);
      if (!epochs->baseline_known) {
        epochs->baseline_caps = *caps; epochs->baseline_known = TRUE;
      }
      epochs->caps = *caps;
      if (caps_changed) {
        epochs->caps_consistent = FALSE;
        epoch_violation(state, entry, row, PTS_CAPS_CHANGED, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      }
      if (!supported) {
        epochs->caps_consistent = FALSE;
        epoch_violation(state, entry, row, PTS_CAPS_UNSUPPORTED, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      }
      if (!epochs->stream_ready)
        epoch_violation(state, entry, row, PTS_CONTEXT_UNKNOWN, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      epochs->caps_ready = supported && epochs->caps_consistent && epochs->stream_ready;
      break;
    }
    case RECORD_SEGMENT: {
      gboolean barrier_valid, supported;
      epoch_add(state, entry, row, &epochs->segment_events, 1);
      epoch_close(entry, EPOCH_SEGMENT, order);
      barrier_valid = epoch_barrier_valid(entry, order);
      if (epochs->n_rows == MAX_PTS_EPOCHS) {
        epochs->overflow = TRUE; epochs->segment_ready = FALSE;
        epoch_violation(state, entry, NULL, PTS_EPOCH_OVERFLOW, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
        break;
      }
      epochs->current = epochs->n_rows++;
      row = &epochs->rows[epochs->current];
      row->stream_generation = entry->stream_generation;
      row->segment_generation = entry->segment_generation;
      row->flush_generation = entry->flush_generation;
      row->start_order = order; row->seqnum = seqnum;
      row->segment = *segment; row->caps = epochs->caps;
      row->first_pts = row->last_pts = row->first_running_time = row->last_running_time = GST_CLOCK_TIME_NONE;
      supported = segment->format == GST_FORMAT_TIME && segment->rate == 1.0 &&
          segment->applied_rate == 1.0 && GST_CLOCK_TIME_IS_VALID(segment->start) &&
          GST_CLOCK_TIME_IS_VALID(segment->time) && GST_CLOCK_TIME_IS_VALID(segment->base) &&
          GST_CLOCK_TIME_IS_VALID(segment->offset) &&
          (!GST_CLOCK_TIME_IS_VALID(segment->stop) || segment->stop > segment->start);
      if (epochs->caps_ready) row->period = GST_SECOND / epochs->caps.fps_n;
      row->context_supported = supported && barrier_valid && epochs->stream_ready &&
          epochs->caps_ready && !epochs->flushing && !epochs->eos;
      if (epochs->eos)
        epoch_violation(state, entry, row, PTS_AFTER_EOS, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      else if (!supported)
        epoch_violation(state, entry, row, PTS_SEGMENT_UNSUPPORTED, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      else if (!row->context_supported)
        epoch_violation(state, entry, row, PTS_CONTEXT_UNKNOWN, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      epochs->segment_ready = row->context_supported;
      epochs->stream_since_segment = epochs->flush_since_segment = FALSE;
      if (epochs->pending_seek && epochs->seek_phase == 3 && row->context_supported) {
        epochs->pending_seek = FALSE; epochs->seek_phase = 0;
      }
      break;
    }
    case RECORD_FLUSH_START:
      epoch_add(state, entry, row, &epochs->flush_starts, 1);
      if (epochs->flushing || epochs->flush_since_segment || !epochs->n_rows)
        epoch_violation(state, entry, row, PTS_FLUSH_UNPAIRED, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      epoch_close(entry, EPOCH_FLUSH_START, order);
      epochs->flushing = TRUE; epochs->flush_since_segment = TRUE;
      epochs->segment_ready = FALSE;
      if (epochs->pending_seek && epochs->seek_phase == 1) epochs->seek_phase = 2;
      break;
    case RECORD_FLUSH_STOP:
      epoch_add(state, entry, row, &epochs->flush_stops, 1);
      if (!epochs->flushing)
        epoch_violation(state, entry, row, PTS_FLUSH_UNPAIRED, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      else {
        epochs->flushing = FALSE; epochs->eos = FALSE;
        if (epochs->pending_seek && epochs->seek_phase == 2) epochs->seek_phase = 3;
      }
      epochs->segment_ready = FALSE;
      break;
    case RECORD_EOS:
      if (!row || !epochs->segment_ready || epochs->flushing || epochs->eos)
        epoch_violation(state, entry, row, epochs->eos ? PTS_AFTER_EOS : PTS_CONTEXT_UNKNOWN,
            GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      epoch_close(entry, EPOCH_EOS, order);
      epochs->eos = TRUE; epochs->segment_ready = FALSE;
      break;
    default: break;
  }
}
static void
epoch_control(TraceState *state, TraceAttachment *entry, guint control, guint64 value)
{
  TracePtsEpochs *epochs = &entry->pts_epochs;
  TracePtsEpoch *row = epoch_current(entry);
  if (control == 2) {
    epoch_add(state, entry, row, &epochs->seek_requests, 1);
    if (epochs->pending_seek || !GST_CLOCK_TIME_IS_VALID(value))
      epoch_violation(state, entry, row, PTS_SEEK, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
    else {
      epochs->pending_seek = TRUE; epochs->seek_value = value; epochs->seek_phase = 1;
    }
  } else if (control == 3) {
    epoch_add(state, entry, row, &epochs->seek_failures, 1);
    if (!epochs->pending_seek || value != epochs->seek_value || epochs->seek_phase != 1)
      epoch_violation(state, entry, row, PTS_SEEK_FAILED, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
    else { epochs->pending_seek = FALSE; epochs->seek_phase = 0; }
  }
}
static gboolean
ensure_records(TraceState *state)
{
  if (!state->records) state->records = g_try_new0(TraceRecord, state->capacity);
  if (!state->records) { state->allocation_failed = TRUE; return FALSE; }
  state->configured = TRUE;
  return TRUE;
}
static void
context_evict(TraceState *state, const TraceRecord *record)
{
  TraceContext *context;
  if (record->attachment >= state->n_attachments) return;
  context = &state->context[record->attachment];
  context->seen = TRUE;
  context->through_order = record->order;
  context->stream_generation = record->stream_generation;
  context->segment_generation = record->segment_generation;
  context->flush_generation = record->flush_generation;
  switch (record->kind) {
    case RECORD_ATTACH: context->active = TRUE; break;
    case RECORD_DETACH: context->active = FALSE; break;
    case RECORD_STREAM_START:
      context->stream_known = TRUE;
      context->caps_known = context->segment_known = context->segment_valid = FALSE;
      break;
    case RECORD_CAPS:
      context->caps = *record; context->caps_known = TRUE; break;
    case RECORD_SEGMENT:
      context->segment = *record; context->segment_known = TRUE;
      context->segment_valid = !context->flushing && record->segment.format == GST_FORMAT_TIME &&
          isfinite(record->segment.rate) && record->segment.rate != 0 &&
          isfinite(record->segment.applied_rate) && record->segment.applied_rate != 0;
      break;
    case RECORD_FLUSH_START:
      context->flushing = TRUE; context->segment_valid = FALSE; break;
    case RECORD_FLUSH_STOP:
      context->flushing = context->segment_valid = FALSE; break;
    default: break;
  }
}
/* Caller holds state->lock. Array allocation precedes probe installation. */
static TraceRecord *
next_record(TraceState *state, RecordKind kind, guint id)
{
  TraceRecord *record;
  guint64 order = increment(state, &state->observed);
  if (state->window && state->window_complete) {
    increment(state, &state->dropped);
    increment(state, &state->unretained_after_window);
    return NULL;
  }
  if (state->retained == state->capacity) {
    increment(state, &state->dropped);
    if (!state->window) return NULL;
    record = &state->records[state->first];
    context_evict(state, record);
    state->first = (state->first + 1) % state->capacity;
    increment(state, &state->overwritten);
  } else {
    record = &state->records[(state->first + state->retained) % state->capacity];
    state->retained++;
  }
  memset(record, 0, sizeof(*record));
  record->kind = kind; record->order = order; record->attachment = id;
  record->monotonic_ns = gst_util_get_timestamp();
  if (state->window) record->thread_id = emitting_thread_id();
  record->pts = record->dts = record->duration = GST_CLOCK_TIME_NONE;
  if (id < state->n_attachments) {
    const TraceAttachment *entry = &state->attachments[id];
    record->stage = entry->stage; record->token = entry->token;
    record->ordinal = entry->buffers;
    record->stream_generation = entry->stream_generation;
    record->segment_generation = entry->segment_generation;
    record->flush_generation = entry->flush_generation;
  }
  if (state->window && state->triggered && ++state->post_retained == state->post_target)
    state->window_complete = TRUE;
  return record;
}

static void
record_buffer(TraceState *state, guint id, GstBuffer *buffer,
               gboolean list_item, guint list_index)
{
  TraceAttachment *entry = &state->attachments[id];
  TraceRecord *record;
  GstClockTime running_time = GST_CLOCK_TIME_NONE;
  gint running_sign = 0;
  increment(state, list_item ? &entry->list_items : &entry->buffers);
  if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)))
    increment(state, list_item ? &entry->list_missing_pts : &entry->missing_pts);
  if (entry->has_segment && !entry->flushing && GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))) {
    running_sign = gst_segment_to_running_time_full(
        &entry->segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer), &running_time);
    if (!GST_CLOCK_TIME_IS_VALID(running_time)) running_sign = 0;
  }
  if (!list_item) {
    summarize_buffer(state, entry, buffer, running_sign, running_time);
    epoch_buffer(state, entry, buffer, running_sign, running_time);
  }
  record = next_record(state, list_item ? RECORD_BUFFER_LIST_ITEM : RECORD_BUFFER, id);
  if (!record) return;
  record->pts = GST_BUFFER_PTS(buffer);
  record->dts = GST_BUFFER_DTS(buffer);
  record->duration = GST_BUFFER_DURATION(buffer);
  record->flags = GST_BUFFER_FLAGS(buffer);
  if (list_item) {
    record->list_ordinal = entry->buffer_lists;
    record->list_index = list_index;
  }
  record->running_sign = running_sign; record->running_time = running_time;
}
static void
read_caps_fields(TraceRecord *record, GstCaps *caps)
{
  record->interlace_mode = record->field_order = -1;
  if (gst_caps_get_size(caps) == 1) {
    const GstStructure *structure = gst_caps_get_structure(caps, 0);
    record->raw_video = gst_structure_has_name(structure, "video/x-raw");
    if (record->raw_video) {
      GstVideoInfo video;
      gst_structure_get_int(structure, "width", &record->width);
      gst_structure_get_int(structure, "height", &record->height);
      gst_video_info_init(&video);
      if (gst_video_info_from_caps(&video, caps)) {
        record->video_info_valid = TRUE;
        record->variable_fps = GST_VIDEO_INFO_FLAG_IS_SET(&video, GST_VIDEO_FLAG_VARIABLE_FPS);
        record->video_format = GST_VIDEO_INFO_FORMAT(&video);
        record->fps_n = GST_VIDEO_INFO_FPS_N(&video);
        record->fps_d = GST_VIDEO_INFO_FPS_D(&video);
        record->interlace_mode = GST_VIDEO_INFO_INTERLACE_MODE(&video);
        record->field_order = GST_VIDEO_INFO_FIELD_ORDER(&video);
      }
    }
  }
}
static void
record_event(TraceState *state, guint id, GstEvent *event)
{
  TraceAttachment *entry = &state->attachments[id];
  TracePtsSummary *summary = &entry->pts_summary;
  TraceRecord *record;
  TraceRecord caps_record = {0};
  RecordKind kind;
  const GstSegment *segment = NULL;
  gboolean caps_fixed = FALSE, caps_changed = FALSE;
  switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_STREAM_START:
      kind = RECORD_STREAM_START;
      increment(state, &entry->stream_generation);
      entry->has_segment = FALSE;
      summary->caps_supported = summary->segment_supported = FALSE;
      summary->previous_valid = summary->origin_valid = FALSE;
      if (entry->stream_generation == 1) pts_add(state, entry, &summary->epochs, 1);
      else pts_boundary(state, entry, PTS_STREAM_CHANGED);
      break;
    case GST_EVENT_SEGMENT:
      kind = RECORD_SEGMENT;
      gst_event_parse_segment(event, &segment);
      entry->segment = *segment;
      increment(state, &entry->segment_generation);
      entry->has_segment = !entry->flushing && segment->format == GST_FORMAT_TIME &&
          isfinite(segment->rate) && segment->rate != 0 &&
          isfinite(segment->applied_rate) && segment->applied_rate != 0;
      pts_add(state, entry, &summary->segment_events, 1);
      summary->segment_supported = !entry->flushing && segment->format == GST_FORMAT_TIME &&
          segment->rate == 1.0 && segment->applied_rate == 1.0 &&
          GST_CLOCK_TIME_IS_VALID(segment->start) && GST_CLOCK_TIME_IS_VALID(segment->time) &&
          GST_CLOCK_TIME_IS_VALID(segment->base) && GST_CLOCK_TIME_IS_VALID(segment->offset);
      if (summary->segment_events > 1) pts_boundary(state, entry, PTS_SEGMENT_CHANGED);
      if (!summary->segment_supported)
        pts_violation(state, entry, PTS_SEGMENT_UNSUPPORTED, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      break;
    case GST_EVENT_FLUSH_START:
      kind = RECORD_FLUSH_START;
      increment(state, &entry->flush_generation);
      entry->flushing = TRUE; entry->has_segment = FALSE;
      summary->segment_supported = FALSE;
      pts_add(state, entry, &summary->flush_events, 1);
      pts_boundary(state, entry, PTS_FLUSH);
      break;
    case GST_EVENT_FLUSH_STOP:
      kind = RECORD_FLUSH_STOP;
      entry->flushing = FALSE; entry->has_segment = FALSE;
      summary->segment_supported = FALSE;
      pts_add(state, entry, &summary->flush_events, 1);
      summary->previous_valid = summary->origin_valid = FALSE;
      pts_violation(state, entry, PTS_FLUSH, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      break;
    case GST_EVENT_EOS:
      kind = RECORD_EOS; increment(state, &entry->eos_count); break;
    case GST_EVENT_QOS:
      kind = RECORD_QOS; increment(state, &entry->qos_count); break;
    case GST_EVENT_CAPS: {
      GstCaps *caps;
      kind = RECORD_CAPS;
      gst_event_parse_caps(event, &caps);
      read_caps_fields(&caps_record, caps);
      caps_fixed = gst_caps_is_fixed(caps);
      caps_changed = entry->caps && !gst_caps_is_equal(entry->caps, caps);
      /* Advance live CAPS before the detail retention cutoff. */
      if (entry->caps && !gst_caps_is_equal(entry->caps, caps)) {
        pts_add(state, entry, &summary->caps_changes, 1);
        pts_boundary(state, entry, PTS_CAPS_CHANGED);
      }
      if (entry->caps) gst_caps_unref(entry->caps);
      entry->caps = gst_caps_ref(caps);
      summary->caps_supported = gst_caps_is_fixed(caps) && caps_record.video_info_valid &&
          !caps_record.variable_fps &&
          caps_record.fps_d == 1 && (caps_record.fps_n == 25 || caps_record.fps_n == 50);
      summary->period = summary->caps_supported ? GST_SECOND / caps_record.fps_n : 0;
      if (!summary->caps_supported)
        pts_violation(state, entry, PTS_CAPS_UNSUPPORTED, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      break;
    }
    case GST_EVENT_GAP:
      /* Legacy schema1 ignores this event. The additive ledger still cannot
       * qualify a GAP annotation, even when no detail record is retained. */
      epoch_violation(state, entry, epoch_current(entry), PTS_BUFFER_GAP,
          GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      return;
    default: return;
  }
  epoch_event(state, entry, kind, gst_event_get_seqnum(event), segment,
      &caps_record, caps_fixed, caps_changed);
  increment(state, &entry->events);
  record = next_record(state, kind, id);
  if (!record) return;
  record->seqnum = gst_event_get_seqnum(event);
  if (segment) record->segment = *segment;
  if (kind == RECORD_QOS)
    gst_event_parse_qos(event, &record->qos_type, &record->proportion,
                        &record->diff, &record->qos_timestamp);
  if (kind == RECORD_CAPS) {
    record->raw_video = caps_record.raw_video;
    record->video_info_valid = caps_record.video_info_valid;
    record->variable_fps = caps_record.variable_fps;
    record->video_format = caps_record.video_format;
    record->width = caps_record.width; record->height = caps_record.height;
    record->fps_n = caps_record.fps_n; record->fps_d = caps_record.fps_d;
    record->interlace_mode = caps_record.interlace_mode;
    record->field_order = caps_record.field_order;
  }
}
static GstPadProbeReturn
observe_pad(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  TraceHook *hook = data;
  TraceState *state = hook->state;
  GstPadProbeType type = GST_PAD_PROBE_INFO_TYPE(info);
  (void)pad;
  g_mutex_lock(&state->lock);
  if (!state->closed && state->attachments[hook->attachment].active) {
    if (type & GST_PAD_PROBE_TYPE_BUFFER) {
      GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
      if (buffer) record_buffer(state, hook->attachment, buffer, FALSE, 0);
    } else if (type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
      GstBufferList *list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
      guint i;
      if (list) {
        /* The default sink chain-list fallback invokes BUFFER probes again
         * for each member. Keep both observation types separate: summing or
         * deduplicating by PTS/address would manufacture a picture census. */
        state->buffer_list_ambiguity = TRUE;
        increment(state, &state->attachments[hook->attachment].buffer_lists);
        pts_violation(state, &state->attachments[hook->attachment], PTS_LIST,
            GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
        epoch_violation(state, &state->attachments[hook->attachment],
            epoch_current(&state->attachments[hook->attachment]), PTS_LIST,
            GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
        for (i = 0; i < gst_buffer_list_length(list); i++)
          record_buffer(state, hook->attachment, gst_buffer_list_get(list, i), TRUE, i);
      }
    } else if (type & (GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_UPSTREAM)) {
      GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
      if (event) record_event(state, hook->attachment, event);
    }
  }
  g_mutex_unlock(&state->lock);
  return GST_PAD_PROBE_OK;
}
static void
hook_destroy(gpointer data)
{
  TraceHook *hook = data;
  trace_state_unref(hook->state);
  g_free(hook);
}
static gint
trace_attach(GstCrystalHdStageTrace *self, GstPad *pad, const gchar *stage, guint64 token)
{
  TraceState *state = self->state;
  TraceAttachment *entry;
  TraceHook *hook;
  guint i, stage_id = G_N_ELEMENTS(stage_names), id;
  if (stage)
    for (i = 0; i < G_N_ELEMENTS(stage_names); i++)
      if (!strcmp(stage, stage_names[i])) stage_id = i;
  g_mutex_lock(&state->lock);
  if (state->closed) {
    g_mutex_unlock(&state->lock);
    return -1;
  }
  if (!pad || !GST_IS_PAD(pad) || stage_id == G_N_ELEMENTS(stage_names) ||
      (stage_id == 2 ? GST_PAD_DIRECTION(pad) != GST_PAD_SINK :
                       GST_PAD_DIRECTION(pad) != GST_PAD_SRC)) {
    increment(state, &state->rejected_attachments);
    g_mutex_unlock(&state->lock);
    return -1;
  }
  for (i = 0; i < state->n_attachments; i++) {
    if (state->attachments[i].active && state->attachments[i].pad == pad) {
      g_mutex_unlock(&state->lock);
      return 0;
    }
  }
  if (state->n_attachments == MAX_ATTACHMENTS || !ensure_records(state)) {
    if (state->n_attachments == MAX_ATTACHMENTS) state->attachment_limit_reached = TRUE;
    increment(state, &state->rejected_attachments);
    g_mutex_unlock(&state->lock);
    return -1;
  }
  hook = g_try_new0(TraceHook, 1);
  if (!hook) {
    state->allocation_failed = TRUE;
    increment(state, &state->rejected_attachments);
    g_mutex_unlock(&state->lock);
    return -1;
  }
  id = state->n_attachments++;
  entry = &state->attachments[id];
  entry->stage = stage_id; entry->token = token;
  entry->pad = gst_object_ref(pad); entry->active = TRUE;
  entry->pts_epochs.current = MAX_PTS_EPOCHS;
  entry->pts_epochs.caps_consistent = TRUE;
  entry->pts_epochs.caps.interlace_mode = entry->pts_epochs.caps.field_order = -1;
  entry->pts_epochs.seek_requests = state->seek_requests;
  entry->pts_epochs.seek_failures = state->seek_failures;
  if (state->seek_requests || state->seek_failures)
    epoch_violation(state, entry, NULL, PTS_SEEK, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
  if (state->seek_requests) {
    entry->pts_summary.seek_requests = state->seek_requests;
    pts_boundary(state, entry, PTS_SEEK);
  }
  gst_segment_init(&entry->segment, GST_FORMAT_UNDEFINED);
  hook->state = trace_state_ref(state); hook->attachment = id;
  next_record(state, RECORD_ATTACH, id);
  /* No IDLE/BLOCK callback can run synchronously on this install. A callback
   * racing installation waits for this lock; stop sees the complete hook. */
  entry->probe = gst_pad_add_probe(pad,
      GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST |
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_UPSTREAM |
      GST_PAD_PROBE_TYPE_EVENT_FLUSH, observe_pad, hook, hook_destroy);
  g_mutex_unlock(&state->lock);
  return 1;
}
static void
trace_detach_pad(GstCrystalHdStageTrace *self, GstPad *pad)
{
  TraceState *state = self->state;
  GstPad *owned = NULL;
  gulong probe = 0;
  guint i;
  g_mutex_lock(&state->lock);
  for (i = 0; i < state->n_attachments; i++) {
    TraceAttachment *entry = &state->attachments[i];
    if (entry->active && entry->pad == pad) {
      if (!entry->eos_count)
        pts_violation(state, entry, PTS_DETACHED, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      epoch_finish(entry, EPOCH_DETACH, epoch_order(state));
      next_record(state, RECORD_DETACH, i);
      entry->active = FALSE; owned = entry->pad; probe = entry->probe;
      entry->pad = NULL; entry->probe = 0;
      break;
    }
  }
  g_mutex_unlock(&state->lock);
  if (owned) {
    if (probe) gst_pad_remove_probe(owned, probe);
    gst_object_unref(owned);
  }
}
static void
trace_stop(GstCrystalHdStageTrace *self)
{
  TraceState *state = self->state;
  GstPad *pads[MAX_ATTACHMENTS];
  gulong probes[MAX_ATTACHMENTS];
  guint i, count = 0;
  g_mutex_lock(&state->lock);
  if (!state->closed) {
    if (state->records) next_record(state, RECORD_STOP, G_MAXUINT);
    state->closed = TRUE;
    for (i = 0; i < state->n_attachments; i++) {
      TraceAttachment *entry = &state->attachments[i];
      if (!entry->active) continue;
      /* next_record(STOP) already advanced the actual global STOP order. */
      epoch_finish(entry, EPOCH_STOP, state->observed);
      entry->active = FALSE; pads[count] = entry->pad; probes[count++] = entry->probe;
      entry->pad = NULL; entry->probe = 0;
    }
  }
  g_mutex_unlock(&state->lock);
  for (i = 0; i < count; i++) {
    if (probes[i]) gst_pad_remove_probe(pads[i], probes[i]);
    gst_object_unref(pads[i]);
  }
}
static gboolean
trace_control(GstCrystalHdStageTrace *self, const gchar *name, guint64 value)
{
  TraceState *state = self->state;
  TraceRecord *record;
  guint i;
  if (!name) return FALSE;
  for (i = 0; i < G_N_ELEMENTS(control_names); i++)
    if (!strcmp(name, control_names[i])) break;
  if (i == G_N_ELEMENTS(control_names)) return FALSE;
  g_mutex_lock(&state->lock);
  if (state->closed || !ensure_records(state)) {
    g_mutex_unlock(&state->lock);
    return FALSE;
  }
  if (i == 2) {
    guint id;
    increment(state, &state->seek_requests);
    for (id = 0; id < state->n_attachments; id++) {
      TraceAttachment *entry = &state->attachments[id];
      if (!entry->active) continue;
      pts_add(state, entry, &entry->pts_summary.seek_requests, 1);
      pts_boundary(state, entry, PTS_SEEK);
      epoch_control(state, entry, i, value);
    }
  } else if (i == 3) {
    guint id;
    increment(state, &state->seek_failures);
    for (id = 0; id < state->n_attachments; id++)
      if (state->attachments[id].active)
        epoch_control(state, &state->attachments[id], i, value);
  }
  record = next_record(state, RECORD_CONTROL, G_MAXUINT);
  if (record) { record->control = i; record->value = value; }
  g_mutex_unlock(&state->lock);
  return TRUE;
}

static gboolean
trace_trigger(GstCrystalHdStageTrace *self, guint64 seqnum)
{
  TraceState *state = self->state;
  TraceRecord *record;
  g_mutex_lock(&state->lock);
  if (state->closed || !state->window || state->triggered || !ensure_records(state)) {
    g_mutex_unlock(&state->lock);
    return FALSE;
  }
  /* Add the actual trigger annotation before starting the post-record budget. */
  record = next_record(state, RECORD_TRIGGER, G_MAXUINT);
  if (record) record->value = seqnum;
  state->triggered = TRUE;
  state->trigger_seqnum = seqnum;
  state->trigger_order = state->observed;
  state->post_target = MAX(1U, state->capacity / 4);
  g_mutex_unlock(&state->lock);
  return TRUE;
}

static void
append_time(GString *json, GstClockTime value)
{
  if (GST_CLOCK_TIME_IS_VALID(value))
    g_string_append_printf(json, "%" G_GUINT64_FORMAT, value);
  else g_string_append(json, "null");
}
static void
append_double(GString *json, gdouble value)
{
  gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
  g_string_append(json, isfinite(value) ? g_ascii_dtostr(buffer, sizeof(buffer), value) : "null");
}
static void
append_thread_id(GString *json, guint64 thread_id)
{
  g_string_append(json, ",\"thread_id\":");
  if (thread_id) g_string_append_printf(json, "%" G_GUINT64_FORMAT, thread_id);
  else g_string_append(json, "null");
}
static void
append_segment_fields(GString *json, const TraceRecord *record)
{
  const GstSegment *segment = &record->segment;
  g_string_append_printf(json, ",\"seqnum\":%u,\"format\":%d,\"rate\":",
      record->seqnum, segment->format);
  append_double(json, segment->rate);
  g_string_append(json, ",\"applied_rate\":"); append_double(json, segment->applied_rate);
  g_string_append(json, ",\"start\":"); append_time(json, segment->start);
  g_string_append(json, ",\"stop\":"); append_time(json, segment->stop);
  g_string_append(json, ",\"time\":"); append_time(json, segment->time);
  g_string_append(json, ",\"base\":"); append_time(json, segment->base);
  g_string_append(json, ",\"offset\":"); append_time(json, segment->offset);
}
static void
append_caps_fields(GString *json, const TraceRecord *record)
{
  const gchar *format = record->video_info_valid ?
      gst_video_format_to_string(record->video_format) : NULL;
  g_string_append_printf(json,
      ",\"seqnum\":%u,\"raw_video\":%s,\"width\":%d,\"height\":%d,"
      "\"video_info_valid\":%s,\"fps_n\":%d,\"fps_d\":%d,\"interlace_mode\":%d,\"field_order\":%d,\"format\":",
      record->seqnum, record->raw_video ? "true" : "false", record->width, record->height,
      record->video_info_valid ? "true" : "false", record->fps_n, record->fps_d,
      record->interlace_mode, record->field_order);
  /* GStreamer supplies a fixed enum name, never caller-controlled caps text. */
  if (format) g_string_append_printf(json, "\"%s\"", format);
  else g_string_append(json, "null");
}
static void
append_checkpoint_record(GString *json, const TraceRecord *record)
{
  g_string_append_printf(json,
      "{\"order\":%" G_GUINT64_FORMAT ",\"kind\":\"%s\",\"stream_generation\":%" G_GUINT64_FORMAT
      ",\"segment_generation\":%" G_GUINT64_FORMAT ",\"flush_generation\":%" G_GUINT64_FORMAT,
      record->order, kind_names[record->kind], record->stream_generation,
      record->segment_generation, record->flush_generation);
  append_thread_id(json, record->thread_id);
  if (record->kind == RECORD_CAPS) append_caps_fields(json, record);
  else append_segment_fields(json, record);
  g_string_append_c(json, '}');
}
static void
append_pts_summary(GString *json, const TraceState *state, const TraceAttachment *entry)
{
  const TracePtsSummary *s = &entry->pts_summary;
  gboolean qualified = state->closed && !state->saturated && !s->counters_saturated &&
      !s->arithmetic_overflow && s->violation == PTS_OK && s->buffers &&
      s->buffers == entry->buffers && s->covered == s->buffers && s->have_pts &&
      s->epochs == 1 && entry->stream_generation == 1 && s->segment_events == 1 &&
      s->caps_supported && s->segment_supported && !entry->flushing &&
      !entry->buffer_lists && entry->eos_count == 1;
  g_string_append_printf(json,
      ",\"pts_summary\":{\"version\":1,\"scope\":\"whole-attachment-observation\",\"qualified\":%s,"
      "\"buffers\":%" G_GUINT64_FORMAT ",\"covered\":%" G_GUINT64_FORMAT
      ",\"unknown_pts\":%" G_GUINT64_FORMAT ",\"unknown_duration\":%" G_GUINT64_FORMAT
      ",\"invalid_duration\":%" G_GUINT64_FORMAT ",\"unqualified_buffers\":%" G_GUINT64_FORMAT
      ",\"duplicate_events\":%" G_GUINT64_FORMAT ",\"reverse_events\":%" G_GUINT64_FORMAT
      ",\"off_grid_events\":%" G_GUINT64_FORMAT ",\"gap_events\":%" G_GUINT64_FORMAT
      ",\"gap_steps\":%" G_GUINT64_FORMAT ",\"epochs\":%" G_GUINT64_FORMAT
      ",\"caps_changes\":%" G_GUINT64_FORMAT ",\"segment_events\":%" G_GUINT64_FORMAT
      ",\"flush_events\":%" G_GUINT64_FORMAT ",\"seek_requests\":%" G_GUINT64_FORMAT
      ",\"arithmetic_overflow\":%s,\"counters_saturated\":%s,\"period_ns\":",
      qualified ? "true" : "false", s->buffers, s->covered, s->unknown_pts, s->unknown_duration,
      s->invalid_duration, s->unqualified_buffers, s->duplicate_events, s->reverse_events,
      s->off_grid_events, s->gap_events, s->gap_steps, s->epochs, s->caps_changes,
      s->segment_events, s->flush_events, s->seek_requests,
      s->arithmetic_overflow ? "true" : "false",
      (s->counters_saturated || state->saturated) ? "true" : "false");
  if (s->period) append_time(json, s->period);
  else g_string_append(json, "null");
  g_string_append(json, ",\"first_pts\":");
  append_time(json, s->have_pts ? s->first_pts : GST_CLOCK_TIME_NONE);
  g_string_append(json, ",\"last_pts\":");
  append_time(json, s->have_pts ? s->last_pts : GST_CLOCK_TIME_NONE);
  g_string_append(json, ",\"first_violation\":");
  if (s->violation == PTS_OK) g_string_append(json, "null");
  else {
    g_string_append_printf(json,
        "{\"reason\":\"%s\",\"order\":%" G_GUINT64_FORMAT ",\"ordinal\":%" G_GUINT64_FORMAT ",\"pts\":",
        pts_violation_names[s->violation], s->violation_order, s->violation_ordinal);
    append_time(json, s->violation_pts);
    g_string_append(json, ",\"expected_pts\":"); append_time(json, s->violation_expected);
    g_string_append_c(json, '}');
  }
  g_string_append_c(json, '}');
}
static gchar *
trace_snapshot(GstCrystalHdStageTrace *self);
static void
append_epoch_counter(GString *json, const gchar *name, guint64 value)
{
  g_string_append_printf(json, ",\"%s\":%" G_GUINT64_FORMAT, name, value);
}
static void
append_epoch_violation(GString *json, PtsViolation reason, guint64 order,
                       guint64 ordinal, GstClockTime pts, GstClockTime expected)
{
  g_string_append(json, ",\"first_violation\":");
  if (reason == PTS_OK) { g_string_append(json, "null"); return; }
  g_string_append_printf(json,
      "{\"reason\":\"%s\",\"order\":%" G_GUINT64_FORMAT ",\"ordinal\":%" G_GUINT64_FORMAT ",\"pts\":",
      pts_violation_names[reason], order, ordinal);
  append_time(json, pts);
  g_string_append(json, ",\"expected_pts\":"); append_time(json, expected);
  g_string_append_c(json, '}');
}
static gboolean
epoch_row_qualified(const TracePtsEpoch *row)
{
  return row->end_reason != EPOCH_OPEN && row->end_reason != EPOCH_DETACH &&
      row->end_reason != EPOCH_STOP && row->end_order > row->start_order &&
      row->context_supported && row->violation == PTS_OK && row->buffers &&
      row->covered == row->buffers && !row->unqualified_buffers &&
      !row->unknown_pts && !row->unknown_duration && !row->invalid_duration &&
      !row->duplicate_events && !row->reverse_events && !row->off_grid_events &&
      !row->gap_events && !row->gap_steps && !row->arithmetic_overflow &&
      !row->counters_saturated && row->period && row->first_ordinal &&
      row->last_ordinal >= row->first_ordinal &&
      row->last_ordinal - row->first_ordinal == row->buffers - 1 &&
      GST_CLOCK_TIME_IS_VALID(row->first_pts) && GST_CLOCK_TIME_IS_VALID(row->last_pts) &&
      GST_CLOCK_TIME_IS_VALID(row->first_running_time) && GST_CLOCK_TIME_IS_VALID(row->last_running_time);
}
static gboolean
epoch_ledger_qualified(const TraceState *state, const TraceAttachment *entry)
{
  const TracePtsEpochs *epochs = &entry->pts_epochs;
  const TracePtsEpoch *last;
  guint i;
  guint64 buffers = 0, eos = 0;
  if (!state->closed || entry->active || state->saturated || state->allocation_failed ||
      state->rejected_attachments || state->configuration_rejections || state->attachment_limit_reached ||
      state->buffer_list_ambiguity || epochs->counters_saturated || epochs->overflow ||
      epochs->violation != PTS_OK || !epochs->n_rows || epochs->unassigned_buffers ||
      epochs->buffers != entry->buffers || epochs->pending_seek || epochs->flushing ||
      entry->flushing || !epochs->eos || epochs->current < epochs->n_rows ||
      epochs->stream_events != entry->stream_generation ||
      epochs->segment_events != entry->segment_generation ||
      epochs->flush_starts != entry->flush_generation || epochs->flush_stops != epochs->flush_starts)
    return FALSE;
  for (i = 0; i < epochs->n_rows; i++) {
    const TracePtsEpoch *row = &epochs->rows[i];
    if (!epoch_row_qualified(row) || row->buffers > G_MAXUINT64 - buffers) return FALSE;
    buffers += row->buffers;
    if (row->end_reason == EPOCH_EOS) eos++;
  }
  last = &epochs->rows[epochs->n_rows - 1];
  return buffers == epochs->buffers && eos == entry->eos_count && last->end_reason == EPOCH_EOS &&
      last->stream_generation == entry->stream_generation &&
      last->segment_generation == entry->segment_generation &&
      last->flush_generation == entry->flush_generation;
}
static void
append_epoch_row(GString *json, const TracePtsEpoch *row, guint index)
{
  const GstSegment *segment = &row->segment;
  const TraceRecord *caps = &row->caps;
  const gchar *format = caps->video_info_valid ? gst_video_format_to_string(caps->video_format) : NULL;
  g_string_append_printf(json,
      "{\"index\":%u,\"stream_generation\":%" G_GUINT64_FORMAT
      ",\"segment_generation\":%" G_GUINT64_FORMAT ",\"flush_generation\":%" G_GUINT64_FORMAT
      ",\"seqnum\":%u,\"start_order\":%" G_GUINT64_FORMAT ",\"end_order\":",
      index, row->stream_generation, row->segment_generation, row->flush_generation,
      row->seqnum, row->start_order);
  if (row->end_reason == EPOCH_OPEN) g_string_append(json, "null,\"end_reason\":null");
  else g_string_append_printf(json, "%" G_GUINT64_FORMAT ",\"end_reason\":\"%s\"",
      row->end_order, epoch_end_names[row->end_reason]);
  g_string_append_printf(json, ",\"qualified\":%s,\"segment\":{\"format\":%d,\"rate\":",
      epoch_row_qualified(row) ? "true" : "false", segment->format);
  append_double(json, segment->rate);
  g_string_append(json, ",\"applied_rate\":"); append_double(json, segment->applied_rate);
  g_string_append(json, ",\"start\":"); append_time(json, segment->start);
  g_string_append(json, ",\"stop\":"); append_time(json, segment->stop);
  g_string_append(json, ",\"time\":"); append_time(json, segment->time);
  g_string_append(json, ",\"base\":"); append_time(json, segment->base);
  g_string_append(json, ",\"offset\":"); append_time(json, segment->offset);
  g_string_append_printf(json,
      "},\"caps\":{\"raw_video\":%s,\"video_info_valid\":%s,\"width\":%d,\"height\":%d,"
      "\"fps_n\":%d,\"fps_d\":%d,\"interlace_mode\":%d,\"field_order\":%d,\"format\":",
      caps->raw_video ? "true" : "false", caps->video_info_valid ? "true" : "false",
      caps->width, caps->height, caps->fps_n, caps->fps_d, caps->interlace_mode, caps->field_order);
  if (format) g_string_append_printf(json, "\"%s\"", format);
  else g_string_append(json, "null");
  g_string_append_c(json, '}');
#define APPEND_EPOCH_COUNTER(name) append_epoch_counter(json, #name, row->name)
  APPEND_EPOCH_COUNTER(buffers); APPEND_EPOCH_COUNTER(covered);
  APPEND_EPOCH_COUNTER(unqualified_buffers); APPEND_EPOCH_COUNTER(unknown_pts);
  APPEND_EPOCH_COUNTER(unknown_duration); APPEND_EPOCH_COUNTER(invalid_duration);
  APPEND_EPOCH_COUNTER(duplicate_events); APPEND_EPOCH_COUNTER(reverse_events);
  APPEND_EPOCH_COUNTER(off_grid_events); APPEND_EPOCH_COUNTER(gap_events); APPEND_EPOCH_COUNTER(gap_steps);
#undef APPEND_EPOCH_COUNTER
  g_string_append_printf(json, ",\"arithmetic_overflow\":%s,\"counters_saturated\":%s,\"first_ordinal\":",
      row->arithmetic_overflow ? "true" : "false", row->counters_saturated ? "true" : "false");
  if (row->buffers) g_string_append_printf(json, "%" G_GUINT64_FORMAT, row->first_ordinal);
  else g_string_append(json, "null");
  g_string_append(json, ",\"last_ordinal\":");
  if (row->buffers) g_string_append_printf(json, "%" G_GUINT64_FORMAT, row->last_ordinal);
  else g_string_append(json, "null");
  g_string_append(json, ",\"first_pts\":"); append_time(json, row->first_pts);
  g_string_append(json, ",\"last_pts\":"); append_time(json, row->last_pts);
  g_string_append(json, ",\"first_running_time\":"); append_time(json, row->first_running_time);
  g_string_append(json, ",\"last_running_time\":"); append_time(json, row->last_running_time);
  g_string_append(json, ",\"period_ns\":");
  if (row->period) append_time(json, row->period);
  else g_string_append(json, "null");
  append_epoch_violation(json, row->violation, row->violation_order, row->violation_ordinal,
      row->violation_pts, row->violation_expected);
  g_string_append_c(json, '}');
}
static void
append_pts_epochs(GString *json, const TraceState *state, const TraceAttachment *entry)
{
  const TracePtsEpochs *epochs = &entry->pts_epochs;
  guint i;
  g_string_append_printf(json,
      ",\"pts_epochs\":{\"version\":1,\"scope\":\"bounded-segment-pts\",\"limit\":%u,"
      "\"qualified\":%s,\"overflow\":%s", MAX_PTS_EPOCHS,
      epoch_ledger_qualified(state, entry) ? "true" : "false", epochs->overflow ? "true" : "false");
#define APPEND_LEDGER_COUNTER(name) append_epoch_counter(json, #name, epochs->name)
  APPEND_LEDGER_COUNTER(buffers); APPEND_LEDGER_COUNTER(unassigned_buffers);
  APPEND_LEDGER_COUNTER(stream_events); APPEND_LEDGER_COUNTER(segment_events);
  APPEND_LEDGER_COUNTER(flush_starts); APPEND_LEDGER_COUNTER(flush_stops);
  APPEND_LEDGER_COUNTER(seek_requests); APPEND_LEDGER_COUNTER(seek_failures);
#undef APPEND_LEDGER_COUNTER
  g_string_append_printf(json, ",\"pending_seek\":%s,\"pending_seek_phase\":%u,\"pending_seek_value\":",
      epochs->pending_seek ? "true" : "false", epochs->seek_phase);
  if (epochs->pending_seek) append_time(json, epochs->seek_value);
  else g_string_append(json, "null");
  append_epoch_violation(json, epochs->violation, epochs->violation_order, epochs->violation_ordinal,
      epochs->violation_pts, epochs->violation_expected);
  g_string_append(json, ",\"rows\":[");
  for (i = 0; i < epochs->n_rows; i++) {
    if (i) g_string_append_c(json, ',');
    append_epoch_row(json, &epochs->rows[i], i);
  }
  g_string_append(json, "]}");
}
static gchar *
trace_snapshot(GstCrystalHdStageTrace *self)
{
  TraceState *state = self->state;
  GString *json = g_string_new(NULL);
  guint i, major, minor, micro, nano;
  gst_version(&major, &minor, &micro, &nano);
  g_mutex_lock(&state->lock);
  g_string_append_printf(json,
      "{\"schema\":1,\"clock\":\"host-monotonic-ns\",\"gstreamer_version\":[%u,%u,%u,%u],\"capacity\":%u,"
      "\"observed\":%" G_GUINT64_FORMAT ",\"retained\":%u,\"dropped\":%" G_GUINT64_FORMAT
      ",\"truncated\":%s,\"closed\":%s,\"counters_saturated\":%s,"
      "\"buffer_list_ambiguity\":%s,"
      "\"rejected_attachments\":%" G_GUINT64_FORMAT ",\"attachment_limit_reached\":%s,"
      "\"configuration_rejections\":%" G_GUINT64_FORMAT ",\"allocation_failed\":%s,\"retention\":\"%s\"",
      major, minor, micro, nano, state->capacity, state->observed, state->retained, state->dropped,
      state->dropped ? "true" : "false", state->closed ? "true" : "false",
      state->saturated ? "true" : "false", state->buffer_list_ambiguity ? "true" : "false",
      state->rejected_attachments,
      state->attachment_limit_reached ? "true" : "false", state->configuration_rejections,
      state->allocation_failed ? "true" : "false", state->window ? "window" : "first");
  append_epoch_counter(json, "seek_requests", state->seek_requests);
  append_epoch_counter(json, "seek_failures", state->seek_failures);
  if (state->window) {
    g_string_append_printf(json, ",\"window\":{\"triggered\":%s,\"trigger_seqnum\":",
        state->triggered ? "true" : "false");
    if (state->triggered) g_string_append_printf(json, "%" G_GUINT64_FORMAT, state->trigger_seqnum);
    else g_string_append(json, "null");
    g_string_append(json, ",\"trigger_order\":");
    if (state->triggered) g_string_append_printf(json, "%" G_GUINT64_FORMAT, state->trigger_order);
    else g_string_append(json, "null");
    g_string_append_printf(json,
        ",\"post_target\":%u,\"post_retained\":%u,\"complete\":%s,\"overwritten\":%" G_GUINT64_FORMAT
        ",\"unretained_after_window\":%" G_GUINT64_FORMAT ",\"context_before_order\":",
        state->post_target, state->post_retained, state->window_complete ? "true" : "false",
        state->overwritten, state->unretained_after_window);
    if (state->retained)
      g_string_append_printf(json, "%" G_GUINT64_FORMAT, state->records[state->first].order);
    else g_string_append(json, "null");
    g_string_append(json, "},\"window_context\":[");
    for (i = 0; i < state->n_attachments; i++) {
      const TraceAttachment *entry = &state->attachments[i];
      const TraceContext *context = &state->context[i];
      if (i) g_string_append_c(json, ',');
      g_string_append_printf(json,
          "{\"attachment\":%u,\"stage\":\"%s\",\"token\":%" G_GUINT64_FORMAT
          ",\"seen\":%s,\"active\":%s,\"stream_known\":%s,\"through_order\":%" G_GUINT64_FORMAT
          ",\"stream_generation\":%" G_GUINT64_FORMAT ",\"segment_generation\":%" G_GUINT64_FORMAT
          ",\"flush_generation\":%" G_GUINT64_FORMAT ",\"segment_valid\":%s,\"flushing\":%s,\"caps\":",
          i, stage_names[entry->stage], entry->token, context->seen ? "true" : "false",
          context->active ? "true" : "false", context->stream_known ? "true" : "false",
          context->through_order, context->stream_generation, context->segment_generation,
          context->flush_generation, context->segment_valid ? "true" : "false",
          context->flushing ? "true" : "false");
      if (context->caps_known) append_checkpoint_record(json, &context->caps);
      else g_string_append(json, "null");
      g_string_append(json, ",\"segment\":");
      if (context->segment_known) append_checkpoint_record(json, &context->segment);
      else g_string_append(json, "null");
      g_string_append_c(json, '}');
    }
    g_string_append_c(json, ']');
  }
  g_string_append(json, ",\"attachments\":[");
  for (i = 0; i < state->n_attachments; i++) {
    const TraceAttachment *entry = &state->attachments[i];
    if (i) g_string_append_c(json, ',');
    g_string_append_printf(json,
        "{\"id\":%u,\"stage\":\"%s\",\"token\":%" G_GUINT64_FORMAT ",\"active\":%s,"
        "\"buffers\":%" G_GUINT64_FORMAT ",\"events\":%" G_GUINT64_FORMAT
        ",\"buffer_lists\":%" G_GUINT64_FORMAT ",\"list_items\":%" G_GUINT64_FORMAT
        ",\"list_missing_pts\":%" G_GUINT64_FORMAT ",\"buffer_list_ambiguity\":%s"
        ",\"missing_pts\":%" G_GUINT64_FORMAT ",\"eos_count\":%" G_GUINT64_FORMAT
        ",\"qos_count\":%" G_GUINT64_FORMAT ",\"stream_generation\":%" G_GUINT64_FORMAT
        ",\"segment_generation\":%" G_GUINT64_FORMAT ",\"flush_generation\":%" G_GUINT64_FORMAT
        ",\"segment_valid\":%s,\"flushing\":%s",
        i, stage_names[entry->stage], entry->token, entry->active ? "true" : "false",
        entry->buffers, entry->events, entry->buffer_lists, entry->list_items,
        entry->list_missing_pts, entry->buffer_lists ? "true" : "false",
        entry->missing_pts, entry->eos_count,
        entry->qos_count, entry->stream_generation, entry->segment_generation,
        entry->flush_generation, entry->has_segment ? "true" : "false",
        entry->flushing ? "true" : "false");
    append_pts_summary(json, state, entry);
    append_pts_epochs(json, state, entry);
    g_string_append_c(json, '}');
  }
  g_string_append(json, "],\"records\":[");
  for (i = 0; i < state->retained; i++) {
    const TraceRecord *record = &state->records[(state->first + i) % state->capacity];
    if (i) g_string_append_c(json, ',');
    g_string_append_printf(json,
        "{\"order\":%" G_GUINT64_FORMAT ",\"kind\":\"%s\",\"monotonic_ns\":%" G_GUINT64_FORMAT
        ",\"attachment\":", record->order, kind_names[record->kind], record->monotonic_ns);
    if (record->attachment == G_MAXUINT) g_string_append(json, "null");
    else g_string_append_printf(json,
        "%u,\"stage\":\"%s\",\"token\":%" G_GUINT64_FORMAT ",\"ordinal\":%" G_GUINT64_FORMAT
        ",\"stream_generation\":%" G_GUINT64_FORMAT ",\"segment_generation\":%" G_GUINT64_FORMAT
        ",\"flush_generation\":%" G_GUINT64_FORMAT,
        record->attachment, stage_names[record->stage], record->token,
        record->ordinal, record->stream_generation, record->segment_generation,
        record->flush_generation);
    if (state->window) append_thread_id(json, record->thread_id);
    if (record->kind == RECORD_BUFFER || record->kind == RECORD_BUFFER_LIST_ITEM) {
      g_string_append(json, ",\"pts\":"); append_time(json, record->pts);
      g_string_append(json, ",\"dts\":"); append_time(json, record->dts);
      g_string_append(json, ",\"duration\":"); append_time(json, record->duration);
      g_string_append(json, ",\"running_time\":");
      if (!record->running_sign) g_string_append(json, "null");
      else {
        if (record->running_sign < 0) g_string_append_c(json, '-');
        g_string_append_printf(json, "%" G_GUINT64_FORMAT, record->running_time);
      }
      g_string_append_printf(json, ",\"flags\":%u", record->flags);
      if (record->kind == RECORD_BUFFER_LIST_ITEM)
        g_string_append_printf(json, ",\"list_ordinal\":%" G_GUINT64_FORMAT ",\"list_index\":%u",
            record->list_ordinal, record->list_index);
    } else if (record->kind == RECORD_SEGMENT) {
      append_segment_fields(json, record);
    } else if (record->kind == RECORD_CONTROL) {
      g_string_append_printf(json, ",\"control\":\"%s\",\"value\":%" G_GUINT64_FORMAT,
          control_names[record->control], record->value);
    } else if (record->kind == RECORD_TRIGGER) {
      g_string_append_printf(json, ",\"value\":%" G_GUINT64_FORMAT, record->value);
    } else if (record->kind == RECORD_QOS) {
      g_string_append_printf(json, ",\"seqnum\":%u,\"qos_type\":%d,\"proportion\":",
          record->seqnum, record->qos_type);
      append_double(json, record->proportion);
      g_string_append_printf(json, ",\"diff\":%" G_GINT64_FORMAT ",\"timestamp\":", record->diff);
      append_time(json, record->qos_timestamp);
    } else if (record->kind == RECORD_CAPS) {
      append_caps_fields(json, record);
    } else if (record->kind != RECORD_ATTACH && record->kind != RECORD_DETACH && record->kind != RECORD_STOP) {
      g_string_append_printf(json, ",\"seqnum\":%u", record->seqnum);
    }
    g_string_append_c(json, '}');
  }
  g_string_append(json, "]}");
  g_mutex_unlock(&state->lock);
  return g_string_free(json, FALSE);
}
static void
trace_get_property(GObject *object, guint property, GValue *value, GParamSpec *spec)
{
  TraceState *state = ((GstCrystalHdStageTrace *)object)->state;
  g_mutex_lock(&state->lock);
  if (property == PROP_CAPACITY) g_value_set_uint(value, state->capacity);
  else if (property == PROP_WINDOW) g_value_set_boolean(value, state->window);
  else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property, spec);
  g_mutex_unlock(&state->lock);
}
static void
trace_set_property(GObject *object, guint property, const GValue *value, GParamSpec *spec)
{
  TraceState *state = ((GstCrystalHdStageTrace *)object)->state;
  g_mutex_lock(&state->lock);
  if (state->closed) {
    g_mutex_unlock(&state->lock);
    return;
  }
  if (property == PROP_CAPACITY) {
    guint capacity = g_value_get_uint(value);
    if (capacity != state->capacity) {
      if (state->configured) increment(state, &state->configuration_rejections);
      else {
        state->capacity = capacity;
        state->post_target = MAX(1U, capacity / 4);
      }
    }
  } else if (property == PROP_WINDOW) {
    gboolean window = g_value_get_boolean(value);
    if (window != state->window) {
      if (state->configured) increment(state, &state->configuration_rejections);
      else state->window = window;
    }
  } else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property, spec);
  g_mutex_unlock(&state->lock);
}
static void
trace_dispose(GObject *object)
{
  trace_stop((GstCrystalHdStageTrace *)object);
  G_OBJECT_CLASS(gst_crystalhd_stage_trace_parent_class)->dispose(object);
}
static void
trace_finalize(GObject *object)
{
  trace_state_unref(((GstCrystalHdStageTrace *)object)->state);
  G_OBJECT_CLASS(gst_crystalhd_stage_trace_parent_class)->finalize(object);
}
static void
gst_crystalhd_stage_trace_class_init(GstCrystalHdStageTraceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = trace_get_property;
  object_class->set_property = trace_set_property;
  object_class->dispose = trace_dispose;
  object_class->finalize = trace_finalize;
  klass->attach = trace_attach; klass->detach_pad = trace_detach_pad; klass->stop = trace_stop;
  klass->snapshot = trace_snapshot; klass->control = trace_control; klass->trigger = trace_trigger;
  gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass),
      "CrystalHD player stage trace controller", "Generic",
      "Bounded native pad observations for optional player diagnostics", "CrystalHD contributors");
  g_signal_new("attach", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_STRUCT_OFFSET(GstCrystalHdStageTraceClass, attach), NULL, NULL, NULL,
      G_TYPE_INT, 3, GST_TYPE_PAD, G_TYPE_STRING, G_TYPE_UINT64);
  g_signal_new("detach-pad", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_STRUCT_OFFSET(GstCrystalHdStageTraceClass, detach_pad), NULL, NULL, NULL,
      G_TYPE_NONE, 1, GST_TYPE_PAD);
  g_signal_new("stop", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_STRUCT_OFFSET(GstCrystalHdStageTraceClass, stop), NULL, NULL, NULL, G_TYPE_NONE, 0);
  g_signal_new("snapshot", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_STRUCT_OFFSET(GstCrystalHdStageTraceClass, snapshot), NULL, NULL, NULL, G_TYPE_STRING, 0);
  g_signal_new("control", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_STRUCT_OFFSET(GstCrystalHdStageTraceClass, control), NULL, NULL, NULL,
      G_TYPE_BOOLEAN, 2, G_TYPE_STRING, G_TYPE_UINT64);
  g_signal_new("trigger", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_STRUCT_OFFSET(GstCrystalHdStageTraceClass, trigger), NULL, NULL, NULL,
      G_TYPE_BOOLEAN, 1, G_TYPE_UINT64);
  g_object_class_install_property(object_class, PROP_CAPACITY,
      g_param_spec_uint("capacity", "Capacity", "Maximum retained records, configured before attach",
          1, MAX_CAPACITY, DEFAULT_CAPACITY, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(object_class, PROP_WINDOW,
      g_param_spec_boolean("window", "Window", "Retain rolling pre/post-trigger records, configured before attach",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}
static void
gst_crystalhd_stage_trace_init(GstCrystalHdStageTrace *self)
{
  self->state = g_new0(TraceState, 1);
  self->state->references = 1; self->state->capacity = DEFAULT_CAPACITY;
  self->state->post_target = DEFAULT_CAPACITY / 4;
  g_mutex_init(&self->state->lock);
}
