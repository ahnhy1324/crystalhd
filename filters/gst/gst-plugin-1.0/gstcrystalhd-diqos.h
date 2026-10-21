/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_CRYSTALHD_DI_QOS_H
#define GST_CRYSTALHD_DI_QOS_H

#include <gst/gst.h>

G_BEGIN_DECLS
#define GST_TYPE_CRYSTALHD_DI_QOS (gst_crystalhd_di_qos_get_type())
GType gst_crystalhd_di_qos_get_type(void);
G_END_DECLS

/* The rank-NONE crystalhddiqos controller has no pads and is not parented in
 * the playback pipeline. Its action signals are:
 *   attach(GstElement *target, GstBus *bus) -> gint
 *       1 attached, 0 unsupported, -1 invalid/duplicate attachment.
 *   detach() -> void, idempotent even during an upstream callback.
 * A controller accepts one successful attachment. Arguments are borrowed;
 * target/pad/bus references are released after detach and pending callbacks.
 * Read-only properties: attached, active, eligible (booleans), qos-count,
 * forward-count, failure-count (guint64). Eligibility inspection never
 * changes the permanent latch; only an ineligible QoS event does that. */
#endif
