/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYSTALHD_V4L2_H_
#define _CRYSTALHD_V4L2_H_

struct crystalhd_adp;
struct crystalhd_v4l2;

/* Caller holds the device writer across registration or disconnection.
 * Registration requires the initialized adapter to own PCI drvdata. The
 * independently referenced parent never retains an adapter pointer; its final
 * release frees only frontend memory. No video node is registered here.
 */
#ifdef CRYSTALHD_ENABLE_V4L2
int crystalhd_v4l2_register(struct crystalhd_adp *adp);
void crystalhd_v4l2_unregister(struct crystalhd_adp *adp);
#else
static inline int crystalhd_v4l2_register(struct crystalhd_adp *adp)
{
	(void)adp;
	return 0;
}

static inline void crystalhd_v4l2_unregister(struct crystalhd_adp *adp)
{
	(void)adp;
}
#endif

#endif
