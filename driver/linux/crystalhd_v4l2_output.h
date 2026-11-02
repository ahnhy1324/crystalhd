/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYSTALHD_V4L2_OUTPUT_H_
#define _CRYSTALHD_V4L2_OUTPUT_H_

#include <media/v4l2-mem2mem.h>
#include "crystalhd_misc.h"

struct crystalhd_cmd;

struct crystalhd_v4l2_output_buffer {
	struct v4l2_m2m_buffer m2m;
	struct crystalhd_tx_buffer tx;
	struct device *dev;
	struct sg_table *source;
	struct scatterlist *wire_sg;
	void *vaddr;
	u8 *header;
	dma_addr_t header_dma;
	refcount_t refs; /* vb2 base plus core's submission/retained backing lease. */
	u32 payload;
	bool cpu_synced;
	bool prepared;
	bool failed;
};

/* Queue uses this allocation size and MMAP/dma-sg/DMA_TO_DEVICE. Process
 * calls are serialized by the frontend. Core get/put are process-context,
 * possibly under terminal device writers: they only update refs, never sync,
 * complete vb2, free memory, or call back into frontend teardown.
 * The parent native lease retains the entire queue, wrapper, device and
 * module until all core references and the whole-session guard retire.
 */
int crystalhd_v4l2_output_init(struct crystalhd_v4l2_output_buffer *buffer,
		struct device *dev);
int crystalhd_v4l2_output_prepare(struct crystalhd_v4l2_output_buffer *buffer);
/* One Annex-B access unit. pts is raw 33-bit PES token, not nanoseconds.
 * Caller holds admitted device/user and TX locks. One deadline covers every
 * packet of the AU. Single-PES AUs borrow mapped SG payload with at most three
 * tail bytes copied. Larger AUs require canonical unstuffed framing and use
 * the shared bounded stager, whose retained backing is separate from OUTPUT.
 * On failure owned()==true means uncertain DMA still retains this buffer:
 * neither vb2_buffer_done nor queue destruction is then permitted.
 */
int crystalhd_v4l2_output_submit(struct crystalhd_cmd *cmd, const void *owner,
		struct crystalhd_v4l2_output_buffer *buffer, u64 pts, u32 timeout_ms);
bool crystalhd_v4l2_output_owned(struct crystalhd_v4l2_output_buffer *buffer);
/* Reclaim CPU ownership after final core put; required before returning a
 * formerly retained buffer. This does not replace whole-session admission.
 */
bool crystalhd_v4l2_output_retire(struct crystalhd_v4l2_output_buffer *buffer);
/* Only after !owned and the frontend release guard. Leaves the m2m prefix. */
void crystalhd_v4l2_output_cleanup(struct crystalhd_v4l2_output_buffer *buffer);

#endif
