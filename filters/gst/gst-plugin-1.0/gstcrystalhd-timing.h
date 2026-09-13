/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_CRYSTALHD_TIMING_H
#define GST_CRYSTALHD_TIMING_H

#include <glib.h>

#define GST_CRYSTALHD_DRAIN_TIMEOUT_US ((gint64)10 * G_USEC_PER_SEC)

/* Arguments use g_get_monotonic_time() units. Receiving a picture does not
 * spend an arbitrary retry token or extend the absolute drain deadline.
 */
static gint64
gst_crystalhd_drain_remaining_us(gint64 deadline, gint64 now)
{
  return now < deadline ? deadline - now : 0;
}

#endif
