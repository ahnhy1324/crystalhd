/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_CRYSTALHD_STAGE_TRACE_H
#define GST_CRYSTALHD_STAGE_TRACE_H
#include <gst/gst.h>
G_BEGIN_DECLS
#define GST_TYPE_CRYSTALHD_STAGE_TRACE (gst_crystalhd_stage_trace_get_type())
GType gst_crystalhd_stage_trace_get_type(void);
G_END_DECLS
/* Rank-NONE padless controller, never inserted in a playback pipeline.
 * capacity (1..16384, default4096) is fixed before attach/control.
 * Signals: attach(GstPad*, decoder|deinterlace|sink, guint64 token) -> gint
 *   1 attached, 0 duplicate pad here, -1 invalid/closed/resource limit.
 * detach-pad(GstPad*) -> void; stop() -> void (both idempotent).
 * control(pause|resume|seek-request|seek-failed, guint64) -> gboolean
 *   A request annotation only, never changes a media generation.
 * snapshot() -> gchar* owned JSON schema1.
 * Global first-N retention includes admin/event records. Buffer/event/EOS
 * counters continue after truncation. At most64 attachment histories, no ID
 * reuse. buffers counts individual BUFFER callbacks; buffer_lists/list_items
 * are separate batch observations. Sink fallback can report both the list
 * and its members, so they must never be summed into a picture count.
 * buffer_list_ambiguity declares this even after record truncation. Unknown
 * times are null; 0 is valid. Every pad has independent
 * stream/segment/flush generations. These are not unique source identities,
 * downstream delivery, physical display or A/V acceptance. No payload map,
 * buffer retention/mutation, bus messages or file I/O. */
#endif
