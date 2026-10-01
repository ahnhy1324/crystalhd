/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYSTALHD_V4L2_H_
#define _CRYSTALHD_V4L2_H_

struct crystalhd_adp;
struct crystalhd_v4l2;
struct crystalhd_v4l2_ctx;

/* Caller holds the device writer across registration or disconnection.
 * Registration requires the initialized adapter to own PCI drvdata. The
 * independently referenced parent never retains an adapter pointer; its final
 * release frees only frontend memory. No video node is registered here.
 */
#ifdef CRYSTALHD_ENABLE_V4L2
/* Module lifecycle only: initialize before PCI registration, drain cleanup
 * after PCI unregister and outside every device/session barrier.
 */
int crystalhd_v4l2_init(void);
void crystalhd_v4l2_cleanup(void);
int crystalhd_v4l2_register(struct crystalhd_adp *adp);
void crystalhd_v4l2_unregister(struct crystalhd_adp *adp);
/* Schedule-only hook after successful PM readiness publication. The caller
 * retains adp/parent lifetime and may hold the device/user writer.
 */
void crystalhd_v4l2_resume_ready(struct crystalhd_adp *adp);
/* Lifetime-only native contexts: no node or queue is exposed. Call these in
 * process context without device/user/TX barriers. The caller keeps independent
 * execution/module protection through each call (future file_operations.owner)
 * and a live parent reference through create. The context takes its own parent
 * and module references, and uses only its generation for device admission.
 * Acquire may return an error while retaining a core owner lease. Always close
 * a created context; close consumes the caller's base reference, so do not use
 * the pointer afterward. The caller retains that base through any concurrent
 * acquire operation. Close may remain pending throughout PM or permanently
 * quarantined DMA: unavailable admission alone never authorizes destruction.
 */
int crystalhd_v4l2_ctx_create(struct crystalhd_v4l2 *parent,
			      struct crystalhd_v4l2_ctx **out);
int crystalhd_v4l2_ctx_acquire(struct crystalhd_v4l2_ctx *ctx);
void crystalhd_v4l2_ctx_close(struct crystalhd_v4l2_ctx *ctx);
#else
static inline int crystalhd_v4l2_init(void)
{
	return 0;
}

static inline void crystalhd_v4l2_cleanup(void)
{
}

static inline int crystalhd_v4l2_register(struct crystalhd_adp *adp)
{
	(void)adp;
	return 0;
}

static inline void crystalhd_v4l2_unregister(struct crystalhd_adp *adp)
{
	(void)adp;
}

static inline void crystalhd_v4l2_resume_ready(struct crystalhd_adp *adp)
{
	(void)adp;
}
#endif

#endif
