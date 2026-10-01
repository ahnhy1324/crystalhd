/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYSTALHD_V4L2_COMPAT_H_
#define _CRYSTALHD_V4L2_COMPAT_H_

#include <linux/highmem.h>
#include <linux/limits.h>
#include <linux/version.h>
#include <media/videobuf2-core.h>

/* vb2_get_num_buffers first appears in upstream v6.8; v6.7 and older
 * expose the count directly. vb2_get_buffer itself exists on v5.15.
 */
static inline unsigned int crystalhd_v4l2_num_buffers(struct vb2_queue *q)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
	return q->num_buffers;
#else
	return vb2_get_num_buffers(q);
#endif
}

/* Through upstream v5.15 this flag gates the memop in vb2-core. v5.16
 * inverted its polarity and moved the check into the allocator memops.
 */
static inline void crystalhd_v4l2_skip_finish(struct vb2_buffer *vb, bool skip)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0)
	vb->need_cache_sync_on_finish = !skip;
#else
	vb->skip_cache_sync_on_finish = skip;
#endif
}

/* DMA SG cache operations cover backing pages, not an additional vmap alias.
 * These non-sleeping helpers also work in the IRQ device-sync callback. The
 * kernel cache interfaces take signed int lengths; chunk larger extents.
 */
static inline void crystalhd_v4l2_flush_alias(void *vaddr, size_t bytes)
{
	while (bytes) {
		int part = bytes > INT_MAX ? INT_MAX : bytes;

		flush_kernel_vmap_range(vaddr, part);
		vaddr = (char *)vaddr + part;
		bytes -= part;
	}
}

static inline void crystalhd_v4l2_invalidate_alias(void *vaddr, size_t bytes)
{
	while (bytes) {
		int part = bytes > INT_MAX ? INT_MAX : bytes;

		invalidate_kernel_vmap_range(vaddr, part);
		vaddr = (char *)vaddr + part;
		bytes -= part;
	}
}

#endif
