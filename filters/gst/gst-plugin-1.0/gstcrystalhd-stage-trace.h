/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_CRYSTALHD_STAGE_TRACE_H
#define GST_CRYSTALHD_STAGE_TRACE_H
#include <gst/gst.h>
G_BEGIN_DECLS
#define GST_TYPE_CRYSTALHD_STAGE_TRACE (gst_crystalhd_stage_trace_get_type())
GType gst_crystalhd_stage_trace_get_type(void);
G_END_DECLS
/* Rank-NONE padless controller, never inserted in a playback pipeline.
 * capacity (1..16384, default4096) and window (boolean, defaultFALSE) are
 * fixed before attach/control/trigger.
 * Signals: attach(GstPad*, stage, guint64 token) -> gint
 *   stage: decoder|deinterlace|output-pack|output-scale (source), sink (input).
 *   1 attached, 0 duplicate pad here, -1 invalid/closed/resource limit.
 * detach-pad(GstPad*) -> void; stop() -> void (both idempotent).
 * control(pause|resume|seek-request|seek-failed, guint64) -> gboolean
 *   A request annotation only, never changes a media generation.
 * trigger(guint64 seqnum) -> gboolean: window only, accepted once. The caller
 *   supplies a video-drop bus-message seqnum, not a source picture identity.
 * snapshot() -> gchar* owned JSON schema1.
 * Global first-N retention includes admin/event records. Buffer/event/EOS
 * counters continue after truncation. At most64 attachment histories, no ID
 * reuse. Optional window mode retains the last capacity records until trigger,
 * then max(1,capacity/4) further records before freezing history; live counters
 * continue. Default first-N semantics are unchanged. Records remain chronological
 * with actual order/ordinals. Window overwritten/unretained records are omitted
 * history, not lost pictures; stopped before the post budget is incomplete.
 * window_context describes observed state before the earliest retained record,
 * including last evicted CAPS/SEGMENT, never substituted terminal state.
 * retention distinguishes first/window; complete describes the post-record
 * budget only, not full-stream or complete-context acceptance. CAPS includes
 * the immutable GStreamer format name when video info is valid.
 * Window records/checkpoints include the emitting Linux thread_id in the
 * caller's PID namespace, or null when unavailable. First-N omits it and
 * performs no thread-ID syscall.
 * The emitting task does not identify internal scaler/decoder worker tasks.
 * buffers counts individual BUFFER callbacks; buffer_lists/list_items
 * are separate batch observations. Sink fallback can report both the list
 * and its members, so they must never be summed into a picture count.
 * buffer_list_ambiguity declares this even after record truncation. Unknown
 * times are null; 0 is valid. Every pad has independent
 * stream/segment/flush generations. These are not unique source identities,
 * downstream delivery, physical display or A/V acceptance. No payload map,
 * buffer retention/mutation, bus messages or file I/O. */
#endif
