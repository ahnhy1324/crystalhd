/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYSTALHD_V4L2_BUFFERS_H_
#define _CRYSTALHD_V4L2_BUFFERS_H_

#include <media/v4l2-mem2mem.h>
#include "crystalhd_hw.h"

struct crystalhd_cmd;

enum crystalhd_v4l2_capture_owner {
	CRYSTALHD_CAPTURE_CPU,
	CRYSTALHD_CAPTURE_CORE,
	CRYSTALHD_CAPTURE_RETIRED,
};

/* vb2 owns this allocation, including the immutable RX identity. Its queue
 * must use this size, MMAP, dma-sg and bidirectional=1 before queue_init.
 * The m2m prefix includes the list used by v4l2_m2m_buf_queue().
 */
struct crystalhd_v4l2_capture_buffer {
	struct v4l2_m2m_buffer m2m;
	struct crystalhd_rx_buffer rx;
	struct sg_table *sgt;
	struct device *dev;
	void *vaddr;
	spinlock_t lock;
	enum crystalhd_v4l2_capture_owner owner;
	bool cpu_synced;
	void (*retired)(void *opaque);
	void *opaque;
	u32 width;
	u32 height;
	u64 decoder_epoch; /* Frontend correlation; not the core capture epoch. */
};

/* All public calls are process-context and serialized by the frontend mutex.
 * init runs once per vb2 allocation and leaves its m2m prefix intact. The
 * frontend retains dev, opaque, queue and module lifetime through a native
 * core owner lease until both owned()==false and the native release guard
 * prove retirement. A failed stop without a callback retains CORE forever.
 * retired is schedule-only: it may run under the terminal device writer and
 * must not wait, free queues, sync backing or call vb2_buffer_done().
 */
void crystalhd_v4l2_capture_init(struct crystalhd_v4l2_capture_buffer *buffer,
		struct device *dev, void (*retired)(void *opaque), void *opaque);
int crystalhd_v4l2_capture_size(u32 width, u32 height, u32 *size);
int crystalhd_v4l2_capture_prepare(struct crystalhd_v4l2_capture_buffer *buffer,
		u32 width, u32 height);
BC_STATUS crystalhd_v4l2_capture_submit(struct crystalhd_cmd *cmd,
		struct crystalhd_v4l2_capture_buffer *buffer);
/* Only pass a successfully dequeued result. On identity/state mismatch,
 * ownership is unchanged. Otherwise this consumes the result identities and
 * returns CPU ownership even on pixel/format/EOS failure; metadata remains.
 * Only SUCCESS publishes image. No function calls vb2_buffer_done itself.
 */
BC_STATUS crystalhd_v4l2_capture_complete(
		struct crystalhd_v4l2_capture_buffer *buffer,
		struct crystalhd_rx_completion *result,
		struct crystalhd_rx_image *image);
bool crystalhd_v4l2_capture_retire(struct crystalhd_v4l2_capture_buffer *buffer);
bool crystalhd_v4l2_capture_owned(struct crystalhd_v4l2_capture_buffer *buffer);

#endif
