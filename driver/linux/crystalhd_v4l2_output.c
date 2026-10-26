// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/dma-mapping.h>
#include <media/videobuf2-dma-sg.h>
#include "crystalhd_lnx.h"
#include "crystalhd_stream.h"
#include "crystalhd_v4l2_output.h"
#include "crystalhd_v4l2_compat.h"

#define CHD_OUTPUT_HEADER_BYTES 16U
#define CHD_OUTPUT_MAX_AU_BYTES (8U * 1024U * 1024U)
#define CHD_OUTPUT_TAIL_OFFSET CHD_OUTPUT_HEADER_BYTES
#define CHD_OUTPUT_SIDECAR_BYTES (CHD_OUTPUT_TAIL_OFFSET + 4U)
#define CHD_OUTPUT_DIRECT_LIMIT 65508U

static void crystalhd_output_get(const struct crystalhd_tx_buffer *tx)
{
	struct crystalhd_v4l2_output_buffer *buffer = tx->cookie;

	refcount_inc(&buffer->refs);
}

static void crystalhd_output_put(struct crystalhd_adp *adp,
		const struct crystalhd_tx_buffer *tx)
{
	struct crystalhd_v4l2_output_buffer *buffer = tx->cookie;

	(void)adp;
	/* The vb2 base reference is retained until process cleanup. */
	if (WARN_ON_ONCE(refcount_read(&buffer->refs) <= 1))
		return;
	refcount_dec(&buffer->refs);
}

static const struct crystalhd_tx_buffer_ops crystalhd_output_ops = {
	.get = crystalhd_output_get,
	.put = crystalhd_output_put,
};

bool crystalhd_v4l2_output_owned(struct crystalhd_v4l2_output_buffer *buffer)
{
	return refcount_read(&buffer->refs) > 1;
}

static void crystalhd_output_cpu(struct crystalhd_v4l2_output_buffer *buffer)
{
	if (!buffer->cpu_synced) {
		dma_sync_sgtable_for_cpu(buffer->dev, buffer->source, DMA_TO_DEVICE);
		crystalhd_v4l2_invalidate_alias(buffer->vaddr, buffer->payload);
		buffer->cpu_synced = true;
	}
}

bool crystalhd_v4l2_output_retire(struct crystalhd_v4l2_output_buffer *buffer)
{
	if (crystalhd_v4l2_output_owned(buffer))
		return false;
	if (buffer->prepared) {
		crystalhd_output_cpu(buffer);
		crystalhd_v4l2_skip_finish(&buffer->m2m.vb.vb2_buf, true);
	}
	return true;
}

int crystalhd_v4l2_output_init(struct crystalhd_v4l2_output_buffer *buffer,
		struct device *dev)
{
	if (!dev)
		return -EINVAL;
	buffer->dev = dev;
	refcount_set(&buffer->refs, 1);
	buffer->prepared = false;
	buffer->failed = false;
	buffer->source = NULL;
	buffer->vaddr = NULL;
	buffer->header = NULL;
	buffer->wire_sg = kcalloc(BC_LINK_MAX_SGLS, sizeof(*buffer->wire_sg),
				 GFP_KERNEL);
	if (!buffer->wire_sg)
		return -ENOMEM;
	buffer->header = dma_alloc_coherent(dev, CHD_OUTPUT_SIDECAR_BYTES,
					   &buffer->header_dma, GFP_KERNEL);
	if (!buffer->header) {
		kfree(buffer->wire_sg);
		buffer->wire_sg = NULL;
		return -ENOMEM;
	}
	sg_init_table(buffer->wire_sg, BC_LINK_MAX_SGLS);
	memset(&buffer->tx, 0, sizeof(buffer->tx));
	buffer->tx.sgl = buffer->wire_sg;
	buffer->tx.ops = &crystalhd_output_ops;
	buffer->tx.cookie = buffer;
	return 0;
}

void crystalhd_v4l2_output_cleanup(struct crystalhd_v4l2_output_buffer *buffer)
{
	if (WARN_ON_ONCE(!crystalhd_v4l2_output_retire(buffer)))
		return;
	if (buffer->header)
		dma_free_coherent(buffer->dev, CHD_OUTPUT_SIDECAR_BYTES,
				  buffer->header, buffer->header_dma);
	kfree(buffer->wire_sg);
	buffer->header = NULL;
	buffer->wire_sg = NULL;
	buffer->prepared = false;
	buffer->source = NULL;
	buffer->vaddr = NULL;
}

int crystalhd_v4l2_output_prepare(struct crystalhd_v4l2_output_buffer *buffer)
{
	struct vb2_buffer *vb = &buffer->m2m.vb.vb2_buf;
	struct vb2_queue *q = vb->vb2_queue;
	struct sg_table *sgt;
	struct scatterlist *sg;
	u64 available = 0;
	unsigned long payload;
	u32 i, len;
	void *vaddr;

	if (crystalhd_v4l2_output_owned(buffer))
		return -EBUSY;
	crystalhd_v4l2_skip_finish(vb, false);
	buffer->prepared = false;
	if (!q || q->dev != buffer->dev || !buffer->header || !buffer->wire_sg ||
	    q->mem_ops != &vb2_dma_sg_memops || q->dma_dir != DMA_TO_DEVICE ||
	    vb->memory != VB2_MEMORY_MMAP || vb->num_planes != 1)
		return -EINVAL;
	payload = vb2_get_plane_payload(vb, 0);
	if (!payload || payload > U32_MAX || payload > vb2_plane_size(vb, 0))
		return -EINVAL;
	if (payload > CHD_OUTPUT_MAX_AU_BYTES)
		return -E2BIG;
	sgt = vb2_dma_sg_plane_desc(vb, 0);
	if (!sgt || !sgt->sgl || !sgt->nents || sgt->nents > sgt->orig_nents)
		return -EINVAL;
	sg = sgt->sgl;
	for (i = 0; i < sgt->nents; i++) {
		if (!sg)
			return -EINVAL;
		len = sg_dma_len(sg);
		if (!len || ((len | sg_dma_address(sg)) & 3) ||
		    sg_dma_address(sg) + len - 1 < sg_dma_address(sg))
			return -EINVAL;
		available += len;
		sg = sg_next(sg);
	}
	if (available < payload)
		return -EINVAL;
	vaddr = vb2_plane_vaddr(vb, 0);
	if (!vaddr)
		return -ENOMEM;
	buffer->source = sgt;
	buffer->vaddr = vaddr;
	buffer->payload = payload;
	buffer->prepared = true;
	buffer->failed = false;
	/* vb2's mem_prepare follows this callback and hands backing to DMA. */
	buffer->cpu_synced = false;
	return 0;
}

static int crystalhd_output_packet(struct crystalhd_v4l2_output_buffer *buffer,
	u64 pts)
{
	u32 header_bytes = 16;
	u32 overhead = header_bytes - 6;
	u32 payload = buffer->payload;
	u32 aligned = payload & ~3U, remaining = aligned;
	u32 tail = payload & 3U;
	u32 length = payload + overhead;
	u32 i, nents = 0;
	struct scatterlist *sg = buffer->source->sgl;
	u8 *header;

	if (WARN_ON_ONCE(crystalhd_v4l2_output_owned(buffer)))
		return -EBUSY;
	crystalhd_output_cpu(buffer);
	header = buffer->header;
	memset(header, 0xff, CHD_OUTPUT_HEADER_BYTES);
	header[0] = 0;
	header[1] = 0;
	header[2] = 1;
	header[3] = 0xe0;
	header[4] = length >> 8;
	header[5] = length;
	header[6] = 0x81;
	header[7] = 0x80;
	header[8] = header_bytes - 9;
	header[9] = 0x21 | (((pts >> 30) & 7) << 1);
	header[10] = pts >> 22;
	header[11] = 1 | (((pts >> 15) & 0x7f) << 1);
	header[12] = pts >> 7;
	header[13] = 1 | ((pts & 0x7f) << 1);
	sg_dma_address(&buffer->wire_sg[nents]) = buffer->header_dma;
	sg_dma_len(&buffer->wire_sg[nents++]) = header_bytes;
	for (i = 0; i < buffer->source->nents && remaining; i++, sg = sg_next(sg)) {
		u32 len = sg_dma_len(sg);
		u32 take;

		/* Reserve one descriptor for the coherent final partial word. */
		if (nents >= BC_LINK_MAX_SGLS - !!tail)
			return -E2BIG;
		take = min(remaining, len);
		sg_dma_address(&buffer->wire_sg[nents]) = sg_dma_address(sg);
		sg_dma_len(&buffer->wire_sg[nents]) = take;
		nents++;
		remaining -= take;
	}
	if (remaining)
		return -EINVAL;
	memset(buffer->header + CHD_OUTPUT_TAIL_OFFSET, 0, sizeof(u32));
	if (tail)
		memcpy(buffer->header + CHD_OUTPUT_TAIL_OFFSET,
		       (u8 *)buffer->vaddr + aligned, tail);
	buffer->tx.dma_nents = nents;
	buffer->tx.bytes = header_bytes + payload;
	buffer->tx.tail_size = tail;
	buffer->tx.tail_addr = buffer->tx.tail_size ?
		buffer->header_dma + CHD_OUTPUT_TAIL_OFFSET : 0;
	return 0;
}

int crystalhd_v4l2_output_submit(struct crystalhd_cmd *cmd, const void *owner,
		struct crystalhd_v4l2_output_buffer *buffer, u64 pts, u32 timeout_ms)
{
	unsigned long deadline;
	BC_STATUS sts;
	int rc;

	if (crystalhd_v4l2_output_owned(buffer))
		return -EBUSY;
	if (!buffer->prepared)
		return -EINVAL;
	if (buffer->failed) {
		rc = -EPIPE;
		goto retire;
	}
	rc = crystalhd_decoder_validate_h264_locked(cmd, owner);
	if (rc)
		goto retire;
	if (pts >> 33) {
		rc = -EINVAL;
		goto retire;
	}
	/* Firmware requires canonical unstuffed continuation headers. Their
	 * 14/9-byte framing cannot share word-aligned DMA with the original SG
	 * payload, so multi-PES AUs use the existing bounded canonical stager.
	 * Its retained DMA lease belongs to staging, never to this source plane.
	 */
	if (buffer->payload > CHD_OUTPUT_DIRECT_LIMIT) {
		crystalhd_output_cpu(buffer);
		rc = crystalhd_decoder_submit_h264(cmd, owner, buffer->vaddr,
			buffer->payload, true, pts, timeout_ms);
		goto submitted;
	}
	sts = crystalhd_tx_deadline_from_ms(timeout_ms, &deadline);
	rc = crystalhd_status_to_errno(sts);
	if (rc)
		goto retire;
	rc = crystalhd_output_packet(buffer, pts);
	if (!rc) {
		buffer->cpu_synced = false;
		crystalhd_v4l2_flush_alias(buffer->vaddr, buffer->payload);
		dma_sync_sgtable_for_device(buffer->dev, buffer->source, DMA_TO_DEVICE);
		dma_wmb();
		sts = crystalhd_tx_transfer_until(cmd, &buffer->tx, 0, deadline);
		rc = crystalhd_status_to_errno(sts);
		/* Never rewrite the wire representation if DMA retained its lease,
		 * even if a broken boundary were to report successful transfer.
		 */
		if (crystalhd_v4l2_output_owned(buffer) && !rc)
			rc = -EIO;
	}
submitted:
	if (rc)
		buffer->failed = true;
retire:
	crystalhd_v4l2_output_retire(buffer);
	return rc;
}
