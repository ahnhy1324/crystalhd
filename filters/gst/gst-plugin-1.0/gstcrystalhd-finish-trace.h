/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_CRYSTALHD_FINISH_TRACE_H
#define GST_CRYSTALHD_FINISH_TRACE_H
#include <gst/video/gstvideodecoder.h>
#define GST_CRYSTALHD_FINISH_TRACE_MAX 8192U
typedef struct _GstCrystalHdFinishTrace GstCrystalHdFinishTrace;
/* Capture under the decoder stream lock. Instance is process-local, not GPU. */
typedef struct {
  guint64 instance, generation, recovery_epoch, frame_number;
  GstClockTime pts, duration;
} GstCrystalHdFinishTraceContext;
GstCrystalHdFinishTrace *gst_crystalhd_finish_trace_new(guint capacity);
GstCrystalHdFinishTrace *gst_crystalhd_finish_trace_ref(GstCrystalHdFinishTrace *trace);
void gst_crystalhd_finish_trace_unref(GstCrystalHdFinishTrace *trace);
guint gst_crystalhd_finish_trace_capacity(GstCrystalHdFinishTrace *trace);
gchar *gst_crystalhd_finish_trace_snapshot(GstCrystalHdFinishTrace *trace);
/* Stable own-source probe symbols; omitted first-N admission is G_MAXUINT. */
guint gst_crystalhd_finish_trace_enter(GstCrystalHdFinishTrace *trace,
    const GstCrystalHdFinishTraceContext *context);
void gst_crystalhd_finish_trace_leave(GstCrystalHdFinishTrace *trace,
    guint slot, GstFlowReturn flow);
/* Never dereference frame after transfer-full finish_frame(). */
GstFlowReturn gst_crystalhd_finish_trace_call(GstCrystalHdFinishTrace *trace,
    GstVideoDecoder *decoder, GstVideoCodecFrame *frame,
    const GstCrystalHdFinishTraceContext *context);
#endif
