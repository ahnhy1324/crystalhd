// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/dma-mapping.h>
#include <media/videobuf2-dma-sg.h>
#include "crystalhd_lnx.h"
#include "crystalhd_v4l2_buffers.h"
#include "crystalhd_v4l2_compat.h"

static void crystalhd_capture_cpu(struct crystalhd_adp *adp,
		struct crystalhd_rx_buffer *rx)
{
	struct crystalhd_v4l2_capture_buffer *buffer = rx->cookie;

	(void)adp;
	/* The parser writes metadata while CPU-owned. A second invalidate
	 * without an intervening device handoff could discard those writes.
	 */
	if (READ_ONCE(buffer->cpu_synced))
		return;
	dma_sync_sgtable_for_cpu(buffer->dev, buffer->sgt, DMA_BIDIRECTIONAL);
	crystalhd_v4l2_invalidate_alias(buffer->vaddr, rx->capacity);
	WRITE_ONCE(buffer->cpu_synced, true);
}

static void crystalhd_capture_device(struct crystalhd_adp *adp,
		struct crystalhd_rx_buffer *rx)
{
	struct crystalhd_v4l2_capture_buffer *buffer = rx->cookie;

	(void)adp;
	/* Also used by IRQ repost. vaddr was established in prepare; neither
	 * mapping nor vb2 allocator callbacks belong in this non-sleeping path.
	 */
	WRITE_ONCE(buffer->cpu_synced, false);
	crystalhd_v4l2_flush_alias(buffer->vaddr, rx->capacity);
	dma_sync_sgtable_for_device(buffer->dev, buffer->sgt, DMA_BIDIRECTIONAL);
}

static BC_STATUS crystalhd_capture_read(struct crystalhd_rx_buffer *rx,
		u32 offset, void *dst, size_t size)
{
	struct crystalhd_v4l2_capture_buffer *buffer = rx->cookie;

	if (!dst || !READ_ONCE(buffer->cpu_synced) ||
	    offset > rx->capacity || size > rx->capacity - offset)
		return BC_STS_INV_ARG;
	memcpy(dst, (u8 *)buffer->vaddr + offset, size);
	return BC_STS_SUCCESS;
}

static BC_STATUS crystalhd_capture_write(struct crystalhd_rx_buffer *rx,
		u32 offset, const void *src, size_t size)
{
	struct crystalhd_v4l2_capture_buffer *buffer = rx->cookie;

	if (!src || !READ_ONCE(buffer->cpu_synced) ||
	    offset > rx->capacity || size > rx->capacity - offset)
		return BC_STS_INV_ARG;
	memcpy((u8 *)buffer->vaddr + offset, src, size);
	return BC_STS_SUCCESS;
}

static void crystalhd_capture_release(struct crystalhd_adp *adp,
		struct crystalhd_rx_buffer *rx)
{
	struct crystalhd_v4l2_capture_buffer *buffer = rx->cookie;
	unsigned long flags;
	bool notify = false;

	(void)adp;
	spin_lock_irqsave(&buffer->lock, flags);
	if (!WARN_ON_ONCE(buffer->owner != CRYSTALHD_CAPTURE_CORE)) {
		buffer->owner = CRYSTALHD_CAPTURE_RETIRED;
		notify = true;
	}
	spin_unlock_irqrestore(&buffer->lock, flags);
	if (notify)
		buffer->retired(buffer->opaque);
}

static const struct crystalhd_rx_buffer_ops crystalhd_capture_ops = {
	.report_decode_errors = true,
	.sync_for_cpu = crystalhd_capture_cpu,
	.sync_for_device = crystalhd_capture_device,
	.read = crystalhd_capture_read,
	.write = crystalhd_capture_write,
	.release = crystalhd_capture_release,
};

void crystalhd_v4l2_capture_init(struct crystalhd_v4l2_capture_buffer *buffer,
		struct device *dev, void (*retired)(void *opaque), void *opaque)
{
	spin_lock_init(&buffer->lock);
	buffer->owner = CRYSTALHD_CAPTURE_CPU;
	buffer->cpu_synced = false;
	buffer->dev = dev;
	buffer->retired = retired;
	buffer->opaque = opaque;
	buffer->sgt = NULL;
	buffer->vaddr = NULL;
	buffer->width = 0;
	buffer->height = 0;
	buffer->decoder_epoch = 0;
	memset(&buffer->rx, 0, sizeof(buffer->rx));
}

int crystalhd_v4l2_capture_size(u32 width, u32 height, u32 *size)
{
	if (!size)
		return -EINVAL;
	*size = 0;
	if (!width || (width & 1) || width > 2048 ||
	    !height || height > 1091)
		return -EINVAL;
	/* Flea's PIB can start one row past the visible image. Packed 422
	 * reads twice the PIB size plus its preceding picture-number word.
	 */
	*size = ALIGN(width * 2 * (height + 1) + sizeof(u32) +
		      2 * sizeof(BC_PIC_INFO_BLOCK), 4);
	return 0;
}

bool crystalhd_v4l2_capture_owned(struct crystalhd_v4l2_capture_buffer *buffer)
{
	unsigned long flags;
	bool owned;

	spin_lock_irqsave(&buffer->lock, flags);
	owned = buffer->owner != CRYSTALHD_CAPTURE_CPU;
	spin_unlock_irqrestore(&buffer->lock, flags);
	return owned;
}

int crystalhd_v4l2_capture_prepare(struct crystalhd_v4l2_capture_buffer *buffer,
		u32 width, u32 height)
{
	struct vb2_buffer *vb = &buffer->m2m.vb.vb2_buf;
	struct vb2_queue *q = vb->vb2_queue;
	struct sg_table *sgt;
	struct scatterlist *sg;
	void *vaddr;
	u64 available = 0;
	u32 size, i, used = 0, len;
	int rc;

	if (crystalhd_v4l2_capture_owned(buffer))
		return -EBUSY;
	crystalhd_v4l2_skip_finish(vb, false);
	/* Invalidate a previous preparation before validating new geometry. */
	buffer->rx.capacity = 0;
	if (!q || !buffer->dev || !buffer->retired || !buffer->opaque ||
	    q->dev != buffer->dev || q->mem_ops != &vb2_dma_sg_memops ||
	    q->dma_dir != DMA_BIDIRECTIONAL || vb->memory != VB2_MEMORY_MMAP ||
	    vb->num_planes != 1)
		return -EINVAL;
	rc = crystalhd_v4l2_capture_size(width, height, &size);
	if (rc)
		return rc;
	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;
	sgt = vb2_dma_sg_plane_desc(vb, 0);
	if (!sgt || !sgt->sgl || !sgt->nents ||
	    sgt->nents > sgt->orig_nents)
		return -EINVAL;
	/* The allocator already mapped this table. Hardware sees nents; DMA
	 * synchronization uses orig_nents through the sgtable DMA helpers.
	 */
	sg = sgt->sgl;
	for (i = 0; i < sgt->nents; i++) {
		if (!sg)
			return -EINVAL;
		len = sg_dma_len(sg);
		if (!len || ((len | sg_dma_address(sg)) & 3))
			return -EINVAL;
		if (available < size) {
			if (min_t(u64, len, size - available) >
			    CRYSTALHD_DMA_DESC_MAX_XFER_BYTES)
				return -E2BIG;
			used++;
		}
		available += len;
		sg = sg_next(sg);
	}
	if (available < size)
		return -EINVAL;
	if (used > BC_LINK_MAX_SGLS)
		return -E2BIG;
	/* vb2-dma-sg may vm_map_ram here. Never defer it to an IRQ callback. */
	vaddr = vb2_plane_vaddr(vb, 0);
	if (!vaddr)
		return -ENOMEM;
	buffer->sgt = sgt;
	buffer->vaddr = vaddr;
	buffer->width = width;
	buffer->height = height;
	buffer->cpu_synced = false;
	buffer->rx.sgl = sgt->sgl;
	buffer->rx.dma_nents = sgt->nents;
	buffer->rx.capacity = size;
	buffer->rx.uv_offset = 0;
	buffer->rx.uv_sg_ix = 0;
	buffer->rx.uv_sg_off = 0;
	buffer->rx.output_format = MODE422_YUY2;
	buffer->rx.ops = &crystalhd_capture_ops;
	buffer->rx.cookie = buffer;
	vb2_set_plane_payload(vb, 0, 0);
	return 0;
}

BC_STATUS crystalhd_v4l2_capture_submit(struct crystalhd_cmd *cmd,
		struct crystalhd_v4l2_capture_buffer *buffer)
{
	unsigned long flags;
	BC_STATUS sts;

	if (!cmd || !buffer->rx.capacity)
		return BC_STS_INV_ARG;
	spin_lock_irqsave(&buffer->lock, flags);
	if (buffer->owner != CRYSTALHD_CAPTURE_CPU) {
		spin_unlock_irqrestore(&buffer->lock, flags);
		return BC_STS_BUSY;
	}
	buffer->owner = CRYSTALHD_CAPTURE_CORE;
	spin_unlock_irqrestore(&buffer->lock, flags);
	WRITE_ONCE(buffer->cpu_synced, false);
	sts = crystalhd_rx_submit(cmd, &buffer->rx);
	if (sts != BC_STS_SUCCESS) {
		/* Core submission retains caller ownership on every error. */
		crystalhd_capture_cpu(NULL, &buffer->rx);
		crystalhd_v4l2_skip_finish(&buffer->m2m.vb.vb2_buf, true);
		spin_lock_irqsave(&buffer->lock, flags);
		buffer->owner = CRYSTALHD_CAPTURE_CPU;
		spin_unlock_irqrestore(&buffer->lock, flags);
	}
	return sts;
}

BC_STATUS crystalhd_v4l2_capture_complete(
		struct crystalhd_v4l2_capture_buffer *buffer,
		struct crystalhd_rx_completion *result,
		struct crystalhd_rx_image *image)
{
	unsigned long flags;
	BC_STATUS sts;

	if (image)
		memset(image, 0, sizeof(*image));
	if (!result || !image || result->buffer != &buffer->rx ||
	    result->cookie != buffer)
		return BC_STS_INV_ARG;
	spin_lock_irqsave(&buffer->lock, flags);
	if (buffer->owner != CRYSTALHD_CAPTURE_CORE) {
		spin_unlock_irqrestore(&buffer->lock, flags);
		return BC_STS_INV_ARG;
	}
	spin_unlock_irqrestore(&buffer->lock, flags);
	/* A successful dequeue detached the core packet; no release callback
	 * or IRQ repost can touch it. Sync explicitly even for error pictures.
	 */
	crystalhd_capture_cpu(NULL, &buffer->rx);
	/* vb2-dma-sg's finish must not invalidate the CPU-written first word
	 * after finish_yuyv. Our explicit synchronization already did its job.
	 * prepare resets this for every subsequent queue cycle.
	 */
	crystalhd_v4l2_skip_finish(&buffer->m2m.vb.vb2_buf, true);
	sts = crystalhd_rx_finish_yuyv(result, image);
	if (sts == BC_STS_SUCCESS &&
	    (image->width != buffer->width || image->height != buffer->height ||
	     image->stride_bytes != buffer->width * 2)) {
		memset(image, 0, sizeof(*image));
		sts = BC_STS_INV_ARG;
	}
	/* Publish parser/pixel writes through the physical pages to userspace's
	 * separate mapping, including the restored first word on success.
	 */
	crystalhd_v4l2_flush_alias(buffer->vaddr, buffer->rx.capacity);
	result->buffer = NULL;
	result->cookie = NULL;
	spin_lock_irqsave(&buffer->lock, flags);
	buffer->owner = CRYSTALHD_CAPTURE_CPU;
	spin_unlock_irqrestore(&buffer->lock, flags);
	return sts;
}

bool crystalhd_v4l2_capture_retire(struct crystalhd_v4l2_capture_buffer *buffer)
{
	unsigned long flags;

	spin_lock_irqsave(&buffer->lock, flags);
	if (buffer->owner != CRYSTALHD_CAPTURE_RETIRED) {
		spin_unlock_irqrestore(&buffer->lock, flags);
		return false;
	}
	spin_unlock_irqrestore(&buffer->lock, flags);
	/* Callback proves DMA detach; the process worker now reclaims backing. */
	crystalhd_capture_cpu(NULL, &buffer->rx);
	crystalhd_v4l2_flush_alias(buffer->vaddr, buffer->rx.capacity);
	crystalhd_v4l2_skip_finish(&buffer->m2m.vb.vb2_buf, true);
	spin_lock_irqsave(&buffer->lock, flags);
	buffer->owner = CRYSTALHD_CAPTURE_CPU;
	spin_unlock_irqrestore(&buffer->lock, flags);
	return true;
}
