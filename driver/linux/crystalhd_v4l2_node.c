// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/vmalloc.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-common.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-sg.h>

#include "crystalhd_lnx.h"
#include "crystalhd_v4l2_node.h"
#include "crystalhd_v4l2_buffers.h"
#include "crystalhd_v4l2_decoder.h"
#include "crystalhd_v4l2_output.h"
#include "crystalhd_v4l2_compat.h"
#include "crystalhd_stream.h"

#define CHD_CODED_SIZE (2U * 1024U * 1024U)
#define CHD_TX_TIMEOUT_MS 2000U
#define CHD_DRAIN_TIMEOUT_MS 30000U
#define CHD_DISCOVERY_BUFFERS 1U

struct crystalhd_v4l2_node {
	struct video_device video;
	struct v4l2_ctrl_handler ctrls;
	struct crystalhd_v4l2 *parent;
	struct v4l2_m2m_dev *m2m;
	struct workqueue_struct *run_wq;
	struct workqueue_struct *tx_wq;
	struct device *dma_dev;
	struct mutex ioctl_lock;
	u64 generation;
};

/* This bounded pool receives only the initial format announcement. It is
 * retired before client CAPTURE buffers become the steady-state DMA target.
 */
struct crystalhd_v4l2_discovery {
	struct crystalhd_rx_buffer rx;
	struct sg_table sgt;
	struct page **pages;
	u32 num_pages;
	struct device *dev;
	void *cpu;
	bool owned;
	bool mapped;
	bool cpu_synced;
};

struct crystalhd_v4l2_file {
	struct v4l2_fh fh;
	struct crystalhd_v4l2_node *node;
	struct crystalhd_v4l2_ctx *lease;
	struct mutex run_lock; /* Worker never takes the VB2/ioctl lock. */
	spinlock_t kick_lock;
	struct delayed_work run_work;
	struct work_struct tx_work;
	struct crystalhd_v4l2_decoder decoder;
	struct crystalhd_v4l2_discovery discovery[CHD_DISCOVERY_BUFFERS];
	struct crystalhd_v4l2_capture_buffer *active[BC_RX_LIST_CNT];
	struct v4l2_pix_format output;
	struct v4l2_pix_format capture;
	u32 sequence;
	bool closing;
	bool admitted;
	bool output_streaming;
	bool capture_streaming;
	bool format_known;
	bool color_default;
	bool format_pending;
	bool capture_started;
	bool drain_requested;
	bool fatal;
	bool session_acquired;
	u32 drain_left; /* Ready source snapshot includes the in-flight TX head. */
	bool input_seen;
	bool empty_drain;
	bool empty_drain_unopened;
	bool tx_active;
	bool tx_stopping;
	bool drain_tx_done;
	bool drain_clock_running;
	unsigned long drain_clock_last;
	unsigned long drain_clock_elapsed;
	u64 drain_clock_epoch;
	u32 drain_clock_count;
	struct vb2_v4l2_buffer *pending_last;
	struct crystalhd_rx_metadata last_metadata;
	u64 last_epoch;
};

static struct crystalhd_v4l2_file *chd_file(void *fh)
{
	return container_of(fh, struct crystalhd_v4l2_file, fh);
}

static void chd_kick(void *opaque)
{
	struct crystalhd_v4l2_file *f = opaque;
	unsigned long flags;

	spin_lock_irqsave(&f->kick_lock, flags);
	if (f->admitted && (!f->closing || READ_ONCE(f->tx_active)))
		mod_delayed_work(f->node->run_wq, &f->run_work, 0);
	spin_unlock_irqrestore(&f->kick_lock, flags);
}

static void chd_admit(struct crystalhd_v4l2_file *f, bool admit)
{
	unsigned long flags;

	spin_lock_irqsave(&f->kick_lock, flags);
	f->admitted = admit && !f->closing;
	if (f->admitted)
		f->tx_stopping = false;
	spin_unlock_irqrestore(&f->kick_lock, flags);
}

static void chd_error(struct crystalhd_v4l2_file *f)
{
	f->fatal = true;
	chd_admit(f, false);
	vb2_queue_error(v4l2_m2m_get_src_vq(f->fh.m2m_ctx));
	vb2_queue_error(v4l2_m2m_get_dst_vq(f->fh.m2m_ctx));
}

static void chd_copy_colors(struct v4l2_pix_format *dst,
			    const struct v4l2_pix_format *src)
{
	dst->colorspace = src->colorspace;
	dst->xfer_func = src->xfer_func;
	dst->ycbcr_enc = src->ycbcr_enc;
	dst->quantization = src->quantization;
}

static bool chd_default_colorspace(u32 colorspace)
{
	return !v4l2_is_colorspace_valid(colorspace) || colorspace == V4L2_COLORSPACE_BT878;
}

static void chd_normalize_colors(struct v4l2_pix_format *p)
{
	if (chd_default_colorspace(p->colorspace))
		p->colorspace = p->width <= 720 && p->height <= 576 ?
			V4L2_COLORSPACE_SMPTE170M : V4L2_COLORSPACE_REC709;
	if (p->priv != V4L2_PIX_FMT_PRIV_MAGIC) {
		p->xfer_func = V4L2_XFER_FUNC_DEFAULT;
		p->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
		p->quantization = V4L2_QUANTIZATION_DEFAULT;
	}
	if (!v4l2_is_xfer_func_valid(p->xfer_func))
		p->xfer_func = V4L2_XFER_FUNC_DEFAULT;
	if (p->ycbcr_enc > U8_MAX || !v4l2_is_ycbcr_enc_valid(p->ycbcr_enc))
		p->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	if (p->quantization > U8_MAX || !v4l2_is_quant_valid(p->quantization))
		p->quantization = V4L2_QUANTIZATION_DEFAULT;
}

static void chd_source_colors(struct crystalhd_v4l2_file *f)
{
	/* PIB colour_primaries alone does not specify matrix/transfer/range.
	 * Keep explicit client colorimetry; only the unspecified SD/HD default
	 * follows newly discovered dimensions. No color conversion is advertised.
	 */
	if (f->color_default)
		f->output.colorspace = f->output.width <= 720 && f->output.height <= 576 ?
			V4L2_COLORSPACE_SMPTE170M : V4L2_COLORSPACE_REC709;
	chd_copy_colors(&f->capture, &f->output);
}

static void chd_discovery_cpu(struct crystalhd_adp *adp,
			      struct crystalhd_rx_buffer *buffer)
{
	struct crystalhd_v4l2_discovery *d = buffer->cookie;

	(void)adp;
	if (!READ_ONCE(d->cpu_synced)) {
		dma_sync_sgtable_for_cpu(d->dev, &d->sgt, DMA_BIDIRECTIONAL);
		crystalhd_v4l2_invalidate_alias(d->cpu, buffer->capacity);
		WRITE_ONCE(d->cpu_synced, true);
	}
}

static void chd_discovery_device(struct crystalhd_adp *adp,
				 struct crystalhd_rx_buffer *buffer)
{
	struct crystalhd_v4l2_discovery *d = buffer->cookie;

	(void)adp;
	crystalhd_v4l2_flush_alias(d->cpu, buffer->capacity);
	dma_sync_sgtable_for_device(d->dev, &d->sgt, DMA_BIDIRECTIONAL);
	WRITE_ONCE(d->cpu_synced, false);
}

static BC_STATUS chd_discovery_read(struct crystalhd_rx_buffer *buffer,
				   u32 offset, void *dst, size_t size)
{
	struct crystalhd_v4l2_discovery *d = buffer->cookie;

	if (offset > buffer->capacity || size > buffer->capacity - offset)
		return BC_STS_INV_ARG;
	memcpy(dst, (u8 *)d->cpu + offset, size);
	return BC_STS_SUCCESS;
}

static BC_STATUS chd_discovery_write(struct crystalhd_rx_buffer *buffer,
				    u32 offset, const void *src, size_t size)
{
	struct crystalhd_v4l2_discovery *d = buffer->cookie;

	if (offset > buffer->capacity || size > buffer->capacity - offset)
		return BC_STS_INV_ARG;
	memcpy((u8 *)d->cpu + offset, src, size);
	return BC_STS_SUCCESS;
}

static void chd_discovery_release(struct crystalhd_adp *adp,
				  struct crystalhd_rx_buffer *buffer)
{
	struct crystalhd_v4l2_discovery *d = buffer->cookie;

	(void)adp;
	if (READ_ONCE(d->cpu_synced))
		crystalhd_v4l2_flush_alias(d->cpu, buffer->capacity);
	WRITE_ONCE(d->owned, false);
}

static const struct crystalhd_rx_buffer_ops chd_discovery_ops = {
	.report_decode_errors = true,
	.sync_for_cpu = chd_discovery_cpu,
	.sync_for_device = chd_discovery_device,
	.read = chd_discovery_read,
	.write = chd_discovery_write,
	.release = chd_discovery_release,
};

static void chd_discovery_free(struct crystalhd_v4l2_discovery *d)
{
	u32 i;

	if (d->cpu)
		vunmap(d->cpu);
	if (d->mapped)
		dma_unmap_sgtable(d->dev, &d->sgt, DMA_BIDIRECTIONAL, 0);
	if (d->sgt.sgl)
		sg_free_table(&d->sgt);
	for (i = 0; i < d->num_pages; i += 2)
		if (d->pages[i])
			__free_pages(d->pages[i], 1);
	kvfree(d->pages);
	memset(d, 0, sizeof(*d));
}

static int chd_discovery_alloc(struct crystalhd_v4l2_file *f)
{
	u32 size, i, j;
	int rc;

	rc = crystalhd_v4l2_capture_size(2048, 1091, &size);
	if (rc)
		return rc;
	for (i = 0; i < CHD_DISCOVERY_BUFFERS; i++) {
		struct crystalhd_v4l2_discovery *d = &f->discovery[i];

		if (d->cpu)
			continue;
		d->dev = f->node->dma_dev;
		/* Two-page blocks bound even an uncoalesced maximum-size discovery
		 * mapping below the hardware's 1024-descriptor limit.
		 */
		d->num_pages = ALIGN(DIV_ROUND_UP(size, PAGE_SIZE), 2);
		d->pages = kvcalloc(d->num_pages, sizeof(*d->pages), GFP_KERNEL);
		if (!d->pages) {
			d->num_pages = 0;
			return -ENOMEM;
		}
		for (j = 0; j < d->num_pages; j += 2) {
			d->pages[j] = alloc_pages(GFP_KERNEL | __GFP_ZERO, 1);
			if (!d->pages[j]) {
				rc = -ENOMEM;
				goto failed;
			}
			d->pages[j + 1] = d->pages[j] + 1;
		}
		rc = sg_alloc_table_from_pages(&d->sgt, d->pages, d->num_pages,
					      0, size, GFP_KERNEL);
		if (rc)
			goto failed;
		rc = dma_map_sgtable(d->dev, &d->sgt, DMA_BIDIRECTIONAL, 0);
		if (rc)
			goto failed;
		d->mapped = true;
		if (d->sgt.nents > BC_LINK_MAX_SGLS) {
			rc = -ENOSPC;
			goto failed;
		}
		d->cpu = vmap(d->pages, d->num_pages, VM_MAP, PAGE_KERNEL);
		if (!d->cpu) {
			rc = -ENOMEM;
			goto failed;
		}
		d->rx.sgl = d->sgt.sgl;
		d->rx.dma_nents = d->sgt.nents;
		d->rx.capacity = size;
		d->rx.output_format = MODE422_YUY2;
		d->rx.ops = &chd_discovery_ops;
		d->rx.cookie = d;
	}
	return 0;
failed:
	chd_discovery_free(&f->discovery[i]);
	return rc;
}

static void chd_return_active(struct crystalhd_v4l2_file *f)
{
	u32 i;

	for (i = 0; i < BC_RX_LIST_CNT; i++) {
		struct crystalhd_v4l2_capture_buffer *b = f->active[i];

		if (!b)
			continue;
		crystalhd_v4l2_capture_retire(b);
		if (WARN_ON_ONCE(crystalhd_v4l2_capture_owned(b)))
			continue;
		f->active[i] = NULL;
		vb2_set_plane_payload(&b->m2m.vb.vb2_buf, 0, 0);
		v4l2_m2m_buf_done(&b->m2m.vb, VB2_BUF_STATE_ERROR);
	}
}

static void chd_return_ready(struct crystalhd_v4l2_file *f, bool output)
{
	struct vb2_v4l2_buffer *b;

	for (;;) {
		b = output ? v4l2_m2m_src_buf_remove(f->fh.m2m_ctx) :
			     v4l2_m2m_dst_buf_remove(f->fh.m2m_ctx);
		if (!b)
			break;
		if (output) {
			struct crystalhd_v4l2_output_buffer *src = container_of(b,
					struct crystalhd_v4l2_output_buffer, m2m.vb);

			if (WARN_ON_ONCE(!crystalhd_v4l2_output_retire(src)))
				return;
		}
		vb2_set_plane_payload(&b->vb2_buf, 0, 0);
		v4l2_m2m_buf_done(b, VB2_BUF_STATE_ERROR);
	}
}

static int chd_start_capture(struct crystalhd_v4l2_file *f,
			     struct crystalhd_cmd *cmd)
{
	BC_STATUS sts;
	u32 i;

	if (!f->format_known) {
		for (i = 0; i < CHD_DISCOVERY_BUFFERS; i++) {
			struct crystalhd_v4l2_discovery *d = &f->discovery[i];

			if (d->owned)
				continue;
			d->owned = true;
			sts = crystalhd_rx_submit(cmd, &d->rx);
			if (sts != BC_STS_SUCCESS) {
				d->owned = false;
				return crystalhd_status_to_errno(sts);
			}
		}
	} else if (!f->capture_streaming || f->format_pending) {
		return 0;
	}
	sts = crystalhd_capture_start(cmd, 8, 4);
	if (sts == BC_STS_SUCCESS)
		f->capture_started = true;
	return crystalhd_status_to_errno(sts);
}

static int chd_submit_capture(struct crystalhd_v4l2_file *f,
			      struct crystalhd_cmd *cmd)
{
	struct vb2_v4l2_buffer *vb;
	struct crystalhd_v4l2_capture_buffer *b;
	BC_STATUS sts;
	u32 i;

	if (!f->capture_streaming || !f->format_known || f->format_pending)
		return 0;
	if (f->pending_last)
		return 0;
	/* One native registration at a time prevents a second picture DMA from
	 * crossing an unacknowledged format boundary. Client queues stay buffered.
	 */
	for (i = 0; i < BC_RX_LIST_CNT; i++)
		if (f->active[i])
			return 0;
	while ((vb = v4l2_m2m_dst_buf_remove(f->fh.m2m_ctx)) != NULL) {
		b = container_of(vb, struct crystalhd_v4l2_capture_buffer, m2m.vb);
		if (b->width != f->capture.width || b->height != f->capture.height) {
			v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
			return -EINVAL;
		}
		if (vb->vb2_buf.index >= BC_RX_LIST_CNT ||
		    f->active[vb->vb2_buf.index]) {
			v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
			return -EIO;
		}
		b->decoder_epoch = f->decoder.epoch;
		f->active[vb->vb2_buf.index] = b;
		sts = crystalhd_v4l2_capture_submit(cmd, b);
		if (sts != BC_STS_SUCCESS) {
			f->active[vb->vb2_buf.index] = NULL;
			v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
			return crystalhd_status_to_errno(sts);
		}
		break;
	}
	return 0;
}

static int chd_format(struct crystalhd_v4l2_file *f,
		       struct crystalhd_cmd *cmd,
		       struct crystalhd_rx_completion *result)
{
	struct v4l2_event event = { .type = V4L2_EVENT_SOURCE_CHANGE };
	u32 width = result->pib.ppb.width, height = result->pib.ppb.height;
	u32 size;
	bool initial = !f->format_known;
	struct crystalhd_v4l2_capture_buffer *boundary = NULL;
	u32 i;
	BC_STATUS sts;
	int rc;

	rc = crystalhd_v4l2_capture_size(width, height, &size);
	if (rc) {
		crystalhd_rx_buffer_release(cmd->adp, result->buffer);
		return rc;
	}
	sts = crystalhd_rx_pause_format(cmd, result);
	if (sts != BC_STS_SUCCESS)
		return crystalhd_status_to_errno(sts);
	f->capture_started = false;
	if (!initial) {
		for (i = 0; i < BC_RX_LIST_CNT; i++) {
			if (!f->active[i])
				continue;
			boundary = f->active[i];
			crystalhd_v4l2_capture_retire(boundary);
			if (crystalhd_v4l2_capture_owned(boundary))
				return -EIO;
			f->active[i] = NULL;
			break;
		}
	}
	chd_return_active(f);
	f->capture.width = width;
	f->capture.height = height;
	f->capture.bytesperline = width * 2;
	f->capture.sizeimage = size;
	f->output.width = width;
	f->output.height = height;
	chd_source_colors(f);
	f->format_known = true;
	f->format_pending = true;
	event.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION;
	v4l2_event_queue_fh(&f->fh, &event);
	if (boundary) {
		vb2_set_plane_payload(&boundary->m2m.vb.vb2_buf, 0, 0);
		boundary->m2m.vb.flags |= V4L2_BUF_FLAG_LAST;
		v4l2_m2m_buf_done(&boundary->m2m.vb, VB2_BUF_STATE_DONE);
	}
	/* A hinted initial CAPTURE setup may already match the discovered format.
	 * Dynamic changes always require a fresh CAPTURE negotiation.
	 */
	if (initial && f->capture_streaming) {
		struct vb2_v4l2_buffer *vb = v4l2_m2m_next_dst_buf(f->fh.m2m_ctx);
		struct crystalhd_v4l2_capture_buffer *b;

		if (vb) {
			b = container_of(vb, struct crystalhd_v4l2_capture_buffer, m2m.vb);
			if (b->width == width && b->height == height)
				f->format_pending = false;
		}
	}
	return 0;
}

static int chd_receive(struct crystalhd_v4l2_file *f, struct crystalhd_cmd *cmd)
{
	struct crystalhd_rx_completion result;
	struct crystalhd_rx_image image;
	struct crystalhd_v4l2_capture_buffer *b;
	struct vb2_v4l2_buffer *vb;
	struct v4l2_event event = { .type = V4L2_EVENT_EOS };
	u64 timestamp;
	BC_STATUS sts, pixels;
	int rc;

	for (;;) {
		sts = crystalhd_rx_try_dequeue(cmd, &result);
		if (sts == BC_STS_NO_DATA || sts == BC_STS_TIMEOUT)
			return 0;
		if (sts != BC_STS_SUCCESS)
			return crystalhd_status_to_errno(sts);
		if (result.flags & COMP_FLAG_FMT_CHANGE)
			return chd_format(f, cmd, &result);
		if (!f->format_known) {
			crystalhd_rx_buffer_release(cmd->adp, result.buffer);
			return -EPROTO;
		}
		/* Only native client registrations remain after discovery retirement. */
		b = result.cookie;
		vb = &b->m2m.vb;
		if (vb->vb2_buf.index >= BC_RX_LIST_CNT ||
		    f->active[vb->vb2_buf.index] != b)
			return -EIO;
		pixels = crystalhd_v4l2_capture_complete(b, &result, &image);
		if (crystalhd_v4l2_capture_owned(b))
			return -EIO;
		f->active[vb->vb2_buf.index] = NULL;
		if (pixels == BC_STS_NO_DATA && result.metadata.valid &&
		    result.metadata.eos_trailer &&
		    result.metadata.picture_number == 0xffffffffU) {
			/* An unsolicited marker must not block input behind a drain
			 * TX that was never started. Epoch alone is not EOS proof.
			 */
			if (!f->drain_requested ||
			    f->decoder.phase != CHD_V4L2_DRAINING) {
				vb->field = V4L2_FIELD_NONE;
				vb->sequence = f->sequence++;
				vb->flags &= ~V4L2_BUF_FLAG_LAST;
				vb->flags |= V4L2_BUF_FLAG_ERROR;
				vb->vb2_buf.timestamp = 0;
				vb2_set_plane_payload(&vb->vb2_buf, 0, 0);
				v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
				return -EPROTO;
			}
			/* Firmware may deliver its marker before the final EOS TX
			 * returns. Keep controller drain and LAST unpublished until
			 * the complete bounded EOS transport has succeeded.
			 */
			f->pending_last = vb;
			f->last_metadata = result.metadata;
			f->last_epoch = b->decoder_epoch;
			return 0;
		}
		rc = (pixels == BC_STS_SUCCESS || pixels == BC_STS_NO_DATA ||
		      (pixels == BC_STS_IO_ERROR && result.metadata.valid &&
		       (result.flags & COMP_FLAG_DATA_VALID) &&
		       (result.metadata.picture_flags & CRYSTALHD_PICTURE_FLAG_DECODE_ERROR))) ?
			crystalhd_v4l2_decoder_complete(&f->decoder, &result.metadata,
						       b->decoder_epoch, &timestamp) : -EIO;
		vb->field = V4L2_FIELD_NONE;
		vb->sequence = f->sequence++;
		vb->flags &= ~(V4L2_BUF_FLAG_LAST | V4L2_BUF_FLAG_ERROR);
		if (!rc && pixels == BC_STS_SUCCESS) {
			vb->vb2_buf.timestamp = timestamp;
			vb2_set_plane_payload(&vb->vb2_buf, 0, image.payload_bytes);
			v4l2_m2m_buf_done(vb, VB2_BUF_STATE_DONE);
		} else if (rc == -EILSEQ && pixels == BC_STS_IO_ERROR) {
			/* The controller retired exactly the failed picture's token.
			 * Report the failure at its original timestamp, never as LAST.
			 */
			vb->vb2_buf.timestamp = timestamp;
			vb2_set_plane_payload(&vb->vb2_buf, 0, 0);
			vb->flags |= V4L2_BUF_FLAG_ERROR;
			v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
		} else if (rc == 1) {
			vb2_set_plane_payload(&vb->vb2_buf, 0, 0);
			vb->flags |= V4L2_BUF_FLAG_LAST;
			v4l2_m2m_buf_done(vb, VB2_BUF_STATE_DONE);
			v4l2_event_queue_fh(&f->fh, &event);
			return 0;
		} else {
			vb->vb2_buf.timestamp = 0;
			vb2_set_plane_payload(&vb->vb2_buf, 0, 0);
			v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
			return rc;
		}
	}
}

static int chd_finish_last(struct crystalhd_v4l2_file *f)
{
	struct v4l2_event event = { .type = V4L2_EVENT_EOS };
	struct vb2_v4l2_buffer *vb = f->pending_last;
	u64 timestamp;
	int rc;

	if (!vb || f->tx_active || !f->drain_tx_done)
		return 0;
	rc = crystalhd_v4l2_decoder_complete(&f->decoder, &f->last_metadata,
					   f->last_epoch, &timestamp);
	if (rc != 1)
		return rc ? rc : -EPROTO;
	f->pending_last = NULL;
	vb->field = V4L2_FIELD_NONE;
	vb->sequence = f->sequence++;
	vb->flags &= ~V4L2_BUF_FLAG_ERROR;
	vb->flags |= V4L2_BUF_FLAG_LAST;
	vb2_set_plane_payload(&vb->vb2_buf, 0, 0);
	v4l2_m2m_buf_done(vb, VB2_BUF_STATE_DONE);
	v4l2_event_queue_fh(&f->fh, &event);
	return 0;
}

static bool chd_input_ready(struct crystalhd_v4l2_file *f)
{
	u32 i;

	if (!f->format_known)
		return f->discovery[0].owned;
	if (!f->capture_streaming || f->format_pending || f->pending_last)
		return false;
	for (i = 0; i < BC_RX_LIST_CNT; i++)
		if (f->active[i])
			return true;
	return false;
}

static struct vb2_v4l2_buffer *chd_next_input(struct crystalhd_v4l2_file *f)
{
	if (f->drain_requested && !f->drain_left)
		return NULL;
	return v4l2_m2m_next_src_buf(f->fh.m2m_ctx);
}

/* No input has entered firmware since initialization/the previous drain.
 * This explicit STOP boundary needs no hardware EOS or idle inference.
 */
static bool chd_empty_drain(struct crystalhd_v4l2_file *f)
{
	struct v4l2_event event = { .type = V4L2_EVENT_EOS };
	struct vb2_v4l2_buffer *vb;

	if (!f->drain_requested || f->drain_left || f->input_seen || f->tx_active)
		return false;
	vb = v4l2_m2m_dst_buf_remove(f->fh.m2m_ctx);
	if (vb) {
		f->empty_drain = true;
		f->empty_drain_unopened = f->decoder.phase == CHD_V4L2_OFF;
		f->decoder.phase = CHD_V4L2_DRAINED;
		f->drain_tx_done = true;
		vb->field = V4L2_FIELD_NONE;
		vb->sequence = f->sequence++;
		vb->flags &= ~V4L2_BUF_FLAG_ERROR;
		vb->flags |= V4L2_BUF_FLAG_LAST;
		vb2_set_plane_payload(&vb->vb2_buf, 0, 0);
		v4l2_m2m_buf_done(vb, VB2_BUF_STATE_DONE);
		v4l2_event_queue_fh(&f->fh, &event);
	}
	return true; /* Wait for a client CAPTURE buffer if none was queued. */
}

static void chd_tx_run(struct work_struct *work)
{
	struct crystalhd_v4l2_file *f = container_of(work,
						struct crystalhd_v4l2_file, tx_work);
	struct crystalhd_device_access access;
	struct crystalhd_v4l2_output_buffer *buffer = NULL;
	struct vb2_v4l2_buffer *src;
	u64 pts = 0;
	bool eos = false;
	int rc;

	mutex_lock(&f->run_lock);
	if (!READ_ONCE(f->admitted) || READ_ONCE(f->tx_stopping) ||
	    f->fatal || !f->output_streaming ||
	    f->decoder.phase != CHD_V4L2_RUNNING || !chd_input_ready(f))
		goto inactive;
	rc = crystalhd_device_enter(f->node->generation, false, &access);
	if (rc) {
		if (rc != -EAGAIN)
			chd_error(f);
		goto inactive;
	}
	src = chd_next_input(f);
	if (src) {
		buffer = container_of(src, struct crystalhd_v4l2_output_buffer, m2m.vb);
		rc = crystalhd_v4l2_decoder_reserve(&f->decoder, &access.adp->cmds,
						 f->lease, src->vb2_buf.timestamp, &pts);
	} else if (f->drain_requested) {
		eos = true;
		rc = crystalhd_v4l2_decoder_begin_drain(&f->decoder);
	} else {
		rc = -EAGAIN;
	}
	if (rc) {
		crystalhd_device_exit(&access);
		if (rc != -EAGAIN)
			chd_error(f);
		goto inactive;
	}
	if (src)
		f->input_seen = true;
	mutex_unlock(&f->run_lock);
	/* RX must continue while firmware input admission is backpressured.
	 * The session reader keeps owner/backing live; no frontend state mutex
	 * or device writer is held across the bounded DMA wait.
	 */
	mutex_lock(&access.adp->tx_lock);
	rc = eos ? crystalhd_decoder_submit_h264_eos(&access.adp->cmds, f->lease,
						     CHD_TX_TIMEOUT_MS) :
		crystalhd_v4l2_output_submit(&access.adp->cmds, f->lease, buffer,
					     pts, CHD_TX_TIMEOUT_MS);
	mutex_unlock(&access.adp->tx_lock);
	crystalhd_device_exit(&access);
	/* Release device/TX barriers before taking the frontend mutex: an RX
	 * worker holding it may be waiting for an exclusive controller turn.
	 */
	mutex_lock(&f->run_lock);
	crystalhd_v4l2_decoder_submitted(&f->decoder, rc);
	if (src && !crystalhd_v4l2_output_owned(buffer)) {
		v4l2_m2m_src_buf_remove(f->fh.m2m_ctx);
		if (f->drain_requested && f->drain_left)
			f->drain_left--;
		v4l2_m2m_buf_done(src, rc ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	}
	if (eos && !rc)
		f->drain_tx_done = true;
	if (rc) {
		dev_err(f->node->dma_dev, "Native input operation failed: %d\n", rc);
		chd_error(f);
	}
inactive:
	f->tx_active = false;
	mutex_unlock(&f->run_lock);
	chd_kick(f);
}

static void chd_schedule_tx(struct crystalhd_v4l2_file *f)
{
	unsigned long flags;

	if (f->tx_active || f->decoder.phase != CHD_V4L2_RUNNING || !chd_input_ready(f))
		return;
	if (chd_next_input(f) &&
	    f->decoder.count == CRYSTALHD_V4L2_TIMESTAMPS)
		return;
	if (!chd_next_input(f) && !f->drain_requested)
		return;
	spin_lock_irqsave(&f->kick_lock, flags);
	if (f->admitted && !f->closing && !f->tx_stopping) {
		f->tx_active = true;
		if (WARN_ON_ONCE(!queue_work(f->node->tx_wq, &f->tx_work)))
			f->tx_active = false;
	}
	spin_unlock_irqrestore(&f->kick_lock, flags);
}

/* Account only intervals with receive capacity. Client starvation is not a
 * firmware timeout. A correlated picture/error restarts the inactivity budget.
 * Unsigned subtraction also handles a jiffies wrap during an eligible interval.
 */
static void chd_drain_watchdog(struct crystalhd_v4l2_file *f)
{
	unsigned long now = jiffies;
	unsigned long limit = msecs_to_jiffies(CHD_DRAIN_TIMEOUT_MS);
	bool eligible = false;
	u32 i;

	if (!f->drain_requested || !f->drain_tx_done || f->tx_active ||
	    f->decoder.phase != CHD_V4L2_DRAINING ||
	    f->drain_clock_epoch != f->decoder.epoch) {
		f->drain_clock_running = false;
		f->drain_clock_elapsed = 0;
		f->drain_clock_epoch = f->decoder.epoch;
		f->drain_clock_count = f->decoder.count;
		return;
	}
	if (f->decoder.count < f->drain_clock_count) {
		f->drain_clock_elapsed = 0;
		f->drain_clock_running = false;
	}
	f->drain_clock_count = f->decoder.count;
	if (f->admitted && !f->fatal && f->output_streaming &&
	    f->capture_streaming && f->capture_started && f->format_known &&
	    !f->format_pending && !f->pending_last) {
		for (i = 0; i < BC_RX_LIST_CNT; i++)
			if (f->active[i] && crystalhd_v4l2_capture_owned(f->active[i])) {
				eligible = true;
				break;
			}
	}
	if (eligible && f->drain_clock_running) {
		unsigned long elapsed = now - f->drain_clock_last;

		/* Saturate before addition, including across long scheduling gaps. */
		if (elapsed >= limit - f->drain_clock_elapsed)
			f->drain_clock_elapsed = limit;
		else
			f->drain_clock_elapsed += elapsed;
	}
	f->drain_clock_running = eligible;
	f->drain_clock_last = now;
	if (f->drain_clock_elapsed >= limit)
		chd_error(f);
}

static void chd_run(struct work_struct *work)
{
	struct crystalhd_v4l2_file *f = container_of(to_delayed_work(work),
						struct crystalhd_v4l2_file, run_work);
	struct crystalhd_device_access access;
	struct crystalhd_cmd *cmd;
	unsigned long flags;
	int rc;

	mutex_lock(&f->run_lock);
	rc = chd_finish_last(f);
	if (rc)
		chd_error(f);
	if (!READ_ONCE(f->admitted) || f->fatal || !f->output_streaming ||
	    f->decoder.phase == CHD_V4L2_DRAINED)
		goto unlock;
	if (chd_empty_drain(f))
		goto unlock;
	if (!f->input_seen && !chd_next_input(f))
		goto unlock;
	rc = crystalhd_device_enter(f->node->generation,
				    f->decoder.phase == CHD_V4L2_OFF, &access);
	if (rc == -EAGAIN) {
		f->drain_clock_running = false;
		goto retry;
	}
	if (rc) {
		chd_error(f);
		goto unlock;
	}
	cmd = &access.adp->cmds;
	if (cmd->session_owner != f->lease) {
		rc = -EBUSY;
		goto exit;
	}
	if (f->decoder.phase == CHD_V4L2_OFF) {
		rc = crystalhd_v4l2_decoder_open_locked(&f->decoder, cmd, f->lease);
		if (rc)
			goto exit;
	}
	if (!f->capture_started) {
		rc = chd_submit_capture(f, cmd);
		if (!rc)
			rc = chd_start_capture(f, cmd);
		if (rc)
			goto exit;
	}
	if (f->capture_started) {
		rc = chd_receive(f, cmd);
		if (rc)
			goto exit;
		if (f->decoder.phase == CHD_V4L2_DRAINED)
			goto exit;
		rc = chd_submit_capture(f, cmd);
		if (rc)
			goto exit;
	}
	chd_schedule_tx(f);
	chd_drain_watchdog(f);
exit:
	crystalhd_device_exit(&access);
	if (rc) {
		dev_err(f->node->dma_dev, "Native decoder operation failed: %d\n", rc);
		chd_error(f);
		goto unlock;
	}
retry:
	/* RX can arrive without another OUTPUT buffer. This owned delayed work
	 * provides bounded completion polling, never framework-private job work.
	 */
	spin_lock_irqsave(&f->kick_lock, flags);
	if (f->admitted && (!f->closing || f->tx_active) &&
	    f->decoder.phase != CHD_V4L2_DRAINED)
		queue_delayed_work(f->node->run_wq, &f->run_work, msecs_to_jiffies(2));
	spin_unlock_irqrestore(&f->kick_lock, flags);
unlock:
	mutex_unlock(&f->run_lock);
}

/* Caller holds ioctl_lock, but never run_lock while joining the worker. */
static void chd_join(struct crystalhd_v4l2_file *f)
{
	unsigned long flags;

	/* Stop admitting inputs, but keep RX progress available to a TX that
	 * already entered firmware backpressure. File/queue backing stays live
	 * until both workers have joined.
	 */
	mutex_lock(&f->run_lock);
	spin_lock_irqsave(&f->kick_lock, flags);
	f->tx_stopping = true;
	if (f->tx_active && !f->fatal && f->output_streaming) {
		f->admitted = true;
		mod_delayed_work(f->node->run_wq, &f->run_work, 0);
	}
	spin_unlock_irqrestore(&f->kick_lock, flags);
	mutex_unlock(&f->run_lock);
	cancel_work_sync(&f->tx_work);
	chd_admit(f, false);
	cancel_delayed_work_sync(&f->run_work);
	mutex_lock(&f->run_lock);
	f->tx_active = false; /* A canceled queued work item never ran its tail. */
	f->drain_clock_running = false;
	f->drain_clock_elapsed = 0;
	mutex_unlock(&f->run_lock);
}

static int chd_reset_channel(struct crystalhd_v4l2_file *f, bool release)
{
	struct crystalhd_device_access access;
	struct crystalhd_cmd *cmd;
	BC_STATUS sts;
	int rc;

	chd_join(f);
	mutex_lock(&f->run_lock);
	rc = crystalhd_device_enter(f->node->generation, true, &access);
	if (rc)
		goto unlock;
	cmd = &access.adp->cmds;
	if (cmd->session_owner != f->lease) {
		rc = f->decoder.phase == CHD_V4L2_OFF ? 0 : -ENODEV;
		goto exit;
	}
	if (release) {
		sts = crystalhd_session_release_locked(cmd, f->lease);
		rc = crystalhd_status_to_errno(sts);
		if (!rc) {
			crystalhd_v4l2_decoder_reset(&f->decoder);
			f->session_acquired = false;
		}
	} else {
		if (cmd->decoder_phase == CRYSTALHD_DECODER_CHANNEL_STARTED) {
			rc = crystalhd_v4l2_decoder_stop_locked(&f->decoder, cmd, f->lease);
			if (rc)
				goto exit;
		}
		if (cmd->state & BC_LINK_CAP_EN) {
			sts = crystalhd_capture_flush(cmd, false);
			rc = crystalhd_status_to_errno(sts);
			if (rc)
				goto exit;
		}
		if (cmd->decoder_phase == CRYSTALHD_DECODER_CHANNEL_CONFIGURED)
			rc = crystalhd_v4l2_decoder_close_locked(&f->decoder, cmd, f->lease);
	}
	if (!rc) {
		if (f->pending_last) {
			vb2_set_plane_payload(&f->pending_last->vb2_buf, 0, 0);
			v4l2_m2m_buf_done(f->pending_last, VB2_BUF_STATE_ERROR);
			f->pending_last = NULL;
		}
		chd_return_active(f);
		f->capture_started = false;
		f->drain_requested = false;
		f->drain_tx_done = false;
		f->drain_left = 0;
		f->input_seen = false;
		f->empty_drain = false;
		f->format_known = false;
		f->format_pending = false;
	}
exit:
	crystalhd_device_exit(&access);
unlock:
	if (rc)
		chd_error(f);
	mutex_unlock(&f->run_lock);
	return rc;
}

static int chd_queue_setup(struct vb2_queue *q, unsigned int *count,
			    unsigned int *planes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct crystalhd_v4l2_file *f = vb2_get_drv_priv(q);
	u32 size;

	(void)alloc_devs;
	mutex_lock(&f->run_lock);
	size = V4L2_TYPE_IS_OUTPUT(q->type) ? f->output.sizeimage : f->capture.sizeimage;
	mutex_unlock(&f->run_lock);
	if (*planes)
		return *planes == 1 && sizes[0] >= size ? 0 : -EINVAL;
	*count = clamp_t(unsigned int, *count, 2, BC_RX_LIST_CNT);
	*planes = 1;
	sizes[0] = size;
	return 0;
}

static int chd_buf_init(struct vb2_buffer *vb)
{
	struct crystalhd_v4l2_file *f = vb2_get_drv_priv(vb->vb2_queue);
	struct crystalhd_v4l2_capture_buffer *b;

	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
		return crystalhd_v4l2_output_init(container_of(to_vb2_v4l2_buffer(vb),
			struct crystalhd_v4l2_output_buffer, m2m.vb), f->node->dma_dev);
	} else {
		b = container_of(to_vb2_v4l2_buffer(vb),
				 struct crystalhd_v4l2_capture_buffer, m2m.vb);
		crystalhd_v4l2_capture_init(b, f->node->dma_dev, chd_kick, f);
	}
	return 0;
}

static void chd_buf_cleanup(struct vb2_buffer *vb)
{
	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type))
		crystalhd_v4l2_output_cleanup(container_of(to_vb2_v4l2_buffer(vb),
			struct crystalhd_v4l2_output_buffer, m2m.vb));
}

static int chd_buf_prepare(struct vb2_buffer *vb)
{
	struct crystalhd_v4l2_file *f = vb2_get_drv_priv(vb->vb2_queue);
	struct crystalhd_v4l2_capture_buffer *b;
	int rc;

	mutex_lock(&f->run_lock);
	if (f->fatal) {
		rc = -EIO;
		goto unlock;
	}
	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
		rc = crystalhd_v4l2_output_prepare(container_of(to_vb2_v4l2_buffer(vb),
				struct crystalhd_v4l2_output_buffer, m2m.vb));
		goto unlock;
	}
	b = container_of(to_vb2_v4l2_buffer(vb),
			 struct crystalhd_v4l2_capture_buffer, m2m.vb);
	rc = crystalhd_v4l2_capture_prepare(b, f->capture.width, f->capture.height);
unlock:
	mutex_unlock(&f->run_lock);
	return rc;
}

static void chd_buf_queue(struct vb2_buffer *vb)
{
	struct crystalhd_v4l2_file *f = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(f->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
	chd_kick(f);
}

static int chd_start_streaming(struct vb2_queue *q, unsigned int count)
{
	(void)q;
	(void)count;
	/* Hardware admission happens only after the ioctl publishes streaming. */
	return 0;
}

static void chd_stop_streaming(struct vb2_queue *q)
{
	struct crystalhd_v4l2_file *f = vb2_get_drv_priv(q);

	/* Every entry is preceded by successful native retirement or by final
	 * reference-backed cleanup after terminal retirement. No fallible stop.
	 */
	if (!V4L2_TYPE_IS_OUTPUT(q->type))
		chd_return_active(f);
	chd_return_ready(f, V4L2_TYPE_IS_OUTPUT(q->type));
}

static const struct vb2_ops chd_queue_ops = {
	.queue_setup = chd_queue_setup,
	.buf_init = chd_buf_init,
	.buf_cleanup = chd_buf_cleanup,
	.buf_prepare = chd_buf_prepare,
	.buf_queue = chd_buf_queue,
	.start_streaming = chd_start_streaming,
	.stop_streaming = chd_stop_streaming,
	CRYSTALHD_V4L2_WAIT_OPS
};

static int chd_queues(void *private, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct crystalhd_v4l2_file *f = private;
	struct vb2_queue *q;
	unsigned int i;
	int rc;

	for (i = 0; i < 2; i++) {
		q = i ? dst : src;
		q->type = i ? V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
		q->io_modes = VB2_MMAP;
		q->drv_priv = f;
		q->ops = &chd_queue_ops;
		q->mem_ops = &vb2_dma_sg_memops;
		q->buf_struct_size = i ? sizeof(struct crystalhd_v4l2_capture_buffer) :
					 sizeof(struct crystalhd_v4l2_output_buffer);
		q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		q->lock = &f->node->ioctl_lock;
		q->dev = f->node->dma_dev;
		q->bidirectional = i ? 1 : 0;
		rc = vb2_queue_init(q);
		if (rc) {
			if (i)
				vb2_queue_release(src);
			return rc;
		}
	}
	return 0;
}

static int chd_job_ready(void *private)
{
	(void)private;
	return 0;
}

static void chd_device_run(void *private)
{
	(void)private;
	WARN_ON_ONCE(1); /* Native work is the sole scheduler on every baseline. */
}

static const struct v4l2_m2m_ops chd_m2m_ops = {
	.job_ready = chd_job_ready,
	.device_run = chd_device_run,
};

static void chd_file_destroy(void *private)
{
	struct crystalhd_v4l2_file *f = private;
	u32 i;

	/* The lease's final cleanup runs after safe whole-session retirement,
	 * outside device barriers and after file release joined our work.
	 */
	chd_return_active(f);
	if (f->pending_last)
		v4l2_m2m_buf_done(f->pending_last, VB2_BUF_STATE_ERROR);
	v4l2_m2m_ctx_release(f->fh.m2m_ctx);
	for (i = 0; i < CHD_DISCOVERY_BUFFERS; i++) {
		struct crystalhd_v4l2_discovery *d = &f->discovery[i];

		WARN_ON_ONCE(d->owned);
		chd_discovery_free(d);
	}
	kfree(f);
}

static int chd_open(struct file *file)
{
	struct crystalhd_v4l2_node *node = video_drvdata(file);
	struct crystalhd_v4l2_file *f;
	int rc;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->node = node;
	mutex_init(&f->run_lock);
	spin_lock_init(&f->kick_lock);
	INIT_DELAYED_WORK(&f->run_work, chd_run);
	INIT_WORK(&f->tx_work, chd_tx_run);
	crystalhd_v4l2_decoder_init(&f->decoder);
	f->output.pixelformat = V4L2_PIX_FMT_H264;
	f->output.width = 640;
	f->output.height = 480;
	f->output.colorspace = V4L2_COLORSPACE_SMPTE170M;
	f->output.priv = V4L2_PIX_FMT_PRIV_MAGIC;
	f->color_default = true;
	f->output.sizeimage = CHD_CODED_SIZE;
	f->output.field = V4L2_FIELD_NONE;
	f->capture.pixelformat = V4L2_PIX_FMT_YUYV;
	f->capture.width = 640;
	f->capture.height = 480;
	f->capture.bytesperline = 1280;
	f->capture.field = V4L2_FIELD_NONE;
	f->capture.colorspace = V4L2_COLORSPACE_SMPTE170M;
	f->capture.priv = V4L2_PIX_FMT_PRIV_MAGIC;
	crystalhd_v4l2_capture_size(640, 480, &f->capture.sizeimage);
	v4l2_fh_init(&f->fh, &node->video);
	f->fh.m2m_ctx = v4l2_m2m_ctx_init(node->m2m, f, chd_queues);
	if (IS_ERR(f->fh.m2m_ctx)) {
		rc = PTR_ERR(f->fh.m2m_ctx);
		goto free_fh;
	}
	rc = crystalhd_v4l2_ctx_create(node->parent, &f->lease);
	if (rc)
		goto free_m2m;
	crystalhd_v4l2_ctx_bind(f->lease, f, chd_file_destroy);
	crystalhd_v4l2_fh_add(&f->fh, file);
	return 0;
free_m2m:
	v4l2_m2m_ctx_release(f->fh.m2m_ctx);
free_fh:
	v4l2_fh_exit(&f->fh);
	kfree(f);
	return rc;
}

static int chd_release(struct file *file)
{
	struct crystalhd_v4l2_file *f = chd_file(file->private_data);
	unsigned long flags;

	mutex_lock(&f->node->ioctl_lock);
	spin_lock_irqsave(&f->kick_lock, flags);
	f->closing = true;
	f->admitted = false;
	spin_unlock_irqrestore(&f->kick_lock, flags);
	chd_join(f);
	crystalhd_v4l2_fh_del(&f->fh, file);
	v4l2_fh_exit(&f->fh);
	mutex_unlock(&f->node->ioctl_lock);
	crystalhd_v4l2_ctx_close(f->lease);
	return 0;
}

static int chd_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	(void)fh;
	strscpy(cap->driver, "crystalhd", sizeof(cap->driver));
	strscpy(cap->card, "CrystalHD BCM70015 decoder", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s",
		 dev_name(chd_file(file->private_data)->node->dma_dev));
	return 0;
}

static int chd_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *fmt)
{
	(void)file;
	(void)fh;
	if (fmt->index)
		return -EINVAL;
	if (V4L2_TYPE_IS_OUTPUT(fmt->type)) {
		fmt->pixelformat = V4L2_PIX_FMT_H264;
		fmt->flags = V4L2_FMT_FLAG_COMPRESSED;
	} else {
		fmt->pixelformat = V4L2_PIX_FMT_YUYV;
	}
	return 0;
}

static int chd_try_fmt_locked(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct crystalhd_v4l2_file *f = chd_file(fh);
	struct v4l2_pix_format *p = &fmt->fmt.pix;
	bool output = V4L2_TYPE_IS_OUTPUT(fmt->type);

	(void)file;
	p->pixelformat = output ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_YUYV;
	p->field = V4L2_FIELD_NONE;
	if (output) {
		p->width = clamp_t(u32, p->width, 16, 2048) & ~1U;
		p->height = clamp_t(u32, p->height, 16, 1091);
		p->bytesperline = 0;
		p->sizeimage = clamp_t(u32, p->sizeimage, CHD_CODED_SIZE, 8U * 1024U * 1024U);
		chd_normalize_colors(p);
	} else {
		if (f->format_known) {
			p->width = f->capture.width;
			p->height = f->capture.height;
		} else {
			p->width = clamp_t(u32, p->width, 16, 2048) & ~1U;
			p->height = clamp_t(u32, p->height, 16, 1091);
		}
		p->bytesperline = p->width * 2;
		crystalhd_v4l2_capture_size(p->width, p->height, &p->sizeimage);
		chd_copy_colors(p, &f->capture);
	}
	p->priv = V4L2_PIX_FMT_PRIV_MAGIC;
	p->flags = 0;
	return 0;
}

static int chd_try_fmt(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct crystalhd_v4l2_file *f = chd_file(fh);
	int rc;

	mutex_lock(&f->run_lock);
	rc = chd_try_fmt_locked(file, fh, fmt);
	mutex_unlock(&f->run_lock);
	return rc;
}

static int chd_get_fmt(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct crystalhd_v4l2_file *f = chd_file(fh);

	(void)file;
	mutex_lock(&f->run_lock);
	fmt->fmt.pix = V4L2_TYPE_IS_OUTPUT(fmt->type) ? f->output : f->capture;
	mutex_unlock(&f->run_lock);
	return 0;
}

static int chd_set_fmt(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct crystalhd_v4l2_file *f = chd_file(fh);
	struct vb2_queue *q = v4l2_m2m_get_vq(f->fh.m2m_ctx, fmt->type);
	bool color_default = chd_default_colorspace(fmt->fmt.pix.colorspace);
	int rc;

	if (!q)
		return -EINVAL;
	if (vb2_is_busy(q) ||
	    (V4L2_TYPE_IS_OUTPUT(fmt->type) &&
	     vb2_is_busy(v4l2_m2m_get_dst_vq(f->fh.m2m_ctx))))
		return -EBUSY;
	mutex_lock(&f->run_lock);
	rc = chd_try_fmt_locked(file, fh, fmt);
	if (!rc) {
		if (V4L2_TYPE_IS_OUTPUT(fmt->type)) {
			f->output = fmt->fmt.pix;
			f->color_default = color_default;
			f->format_known = false;
			f->capture.width = clamp_t(u32, f->output.width, 16, 2048) & ~1U;
			f->capture.height = clamp_t(u32, f->output.height, 16, 1091);
			f->capture.bytesperline = f->capture.width * 2;
			crystalhd_v4l2_capture_size(f->capture.width, f->capture.height,
						  &f->capture.sizeimage);
			chd_source_colors(f);
		} else
			f->capture = fmt->fmt.pix;
	}
	mutex_unlock(&f->run_lock);
	return rc;
}

static int chd_reqbufs(struct file *file, void *fh, struct v4l2_requestbuffers *req)
{
	struct crystalhd_v4l2_file *f = chd_file(fh);
	struct vb2_queue *q = v4l2_m2m_get_vq(f->fh.m2m_ctx, req->type);
	int rc;
	bool reset;

	if (req->type != V4L2_BUF_TYPE_VIDEO_OUTPUT &&
	    req->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (!q)
		return -EINVAL;
	if (vb2_is_streaming(q))
		return -EBUSY;
	mutex_lock(&f->run_lock);
	if (req->memory != V4L2_MEMORY_MMAP || f->fatal) {
		rc = f->fatal ? -EIO : -EINVAL;
		mutex_unlock(&f->run_lock);
		return rc;
	}
	/* REQBUFS may destroy old allocations even when replacing with count>0. */
	reset = vb2_is_busy(q) &&
		(f->capture_started || f->decoder.phase != CHD_V4L2_OFF) &&
		(V4L2_TYPE_IS_OUTPUT(req->type) ||
		 (!f->format_pending && !crystalhd_v4l2_decoder_drained(&f->decoder)));
	mutex_unlock(&f->run_lock);
	if (reset) {
		rc = chd_reset_channel(f, false);
		if (rc)
			return rc;
	}
	return v4l2_m2m_ioctl_reqbufs(file, fh, req);
}

static int chd_qbuf(struct file *file, void *fh, struct v4l2_buffer *buf)
{
	struct crystalhd_v4l2_file *f = chd_file(fh);
	int rc = 0;

	mutex_lock(&f->run_lock);
	if (f->fatal)
		rc = -EIO;
	mutex_unlock(&f->run_lock);
	if (rc)
		return rc;
	return v4l2_m2m_ioctl_qbuf(file, fh, buf);
}

static int chd_resume(struct crystalhd_v4l2_file *f)
{
	struct crystalhd_device_access access;
	int rc = 0;

	chd_join(f);
	mutex_lock(&f->run_lock);
	if (f->empty_drain) {
		f->decoder.phase = f->empty_drain_unopened ? CHD_V4L2_OFF : CHD_V4L2_RUNNING;
	} else {
		rc = crystalhd_device_enter(f->node->generation, true, &access);
		if (!rc) {
			mutex_lock(&access.adp->tx_lock);
			rc = crystalhd_decoder_resume_h264_locked(&access.adp->cmds, f->lease);
			mutex_unlock(&access.adp->tx_lock);
			crystalhd_device_exit(&access);
		}
		if (!rc)
			rc = crystalhd_v4l2_decoder_resume(&f->decoder);
	}
	if (!rc) {
		f->drain_requested = false;
		f->drain_tx_done = false;
		f->drain_left = 0;
		f->input_seen = false;
		f->empty_drain = false;
		vb2_clear_last_buffer_dequeued(v4l2_m2m_get_dst_vq(f->fh.m2m_ctx));
		chd_admit(f, f->output_streaming);
	}
	mutex_unlock(&f->run_lock);
	return rc;
}

/* run_lock plus ioctl_lock exclude worker consumption and client QBUF. */
static int chd_reuse_capture(struct crystalhd_v4l2_file *f)
{
	struct vb2_queue *q = v4l2_m2m_get_dst_vq(f->fh.m2m_ctx);
	struct v4l2_m2m_buffer *m2m;
	u32 i;
	int rc;

	if (!f->capture_streaming || crystalhd_v4l2_num_buffers(q) < 2)
		return -EINVAL;
	for (i = 0; i < BC_RX_LIST_CNT; i++)
		if (f->active[i])
			return -EBUSY;
	for (i = 0; i < crystalhd_v4l2_num_buffers(q); i++)
		if (!vb2_get_buffer(q, i) ||
		    vb2_plane_size(vb2_get_buffer(q, i), 0) < f->capture.sizeimage)
			return -EINVAL;
	v4l2_m2m_for_each_dst_buf(f->fh.m2m_ctx, m2m) {
		struct crystalhd_v4l2_capture_buffer *b = container_of(m2m,
					struct crystalhd_v4l2_capture_buffer, m2m);

		rc = crystalhd_v4l2_capture_prepare(b, f->capture.width, f->capture.height);
		if (rc)
			return rc;
	}
	f->format_pending = false;
	vb2_clear_last_buffer_dequeued(q);
	return 0;
}

static int chd_streamon(struct file *file, void *fh, enum v4l2_buf_type type)
{
	struct crystalhd_v4l2_file *f = chd_file(fh);
	bool acquired = false;
	u32 i;
	int rc, cleanup_rc;

	if (f->fatal)
		return -EIO;
	if (type != V4L2_BUF_TYPE_VIDEO_OUTPUT && type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (vb2_is_streaming(v4l2_m2m_get_vq(f->fh.m2m_ctx, type)))
		return 0;
	/* Reject an unallocated queue before claiming the single engine. */
	if (!vb2_is_busy(v4l2_m2m_get_vq(f->fh.m2m_ctx, type)))
		return -EINVAL;
	if (V4L2_TYPE_IS_OUTPUT(type) && !f->session_acquired) {
		rc = crystalhd_v4l2_ctx_acquire(f->lease);
		if (rc)
			return rc;
		f->session_acquired = true;
		acquired = true;
	}
	if (V4L2_TYPE_IS_OUTPUT(type)) {
		rc = chd_discovery_alloc(f);
		if (rc)
			goto rollback;
	}
	rc = v4l2_m2m_ioctl_streamon(file, fh, type);
	if (rc)
		goto rollback;
	if (!V4L2_TYPE_IS_OUTPUT(type) && crystalhd_v4l2_decoder_drained(&f->decoder)) {
		rc = chd_resume(f);
		if (rc) {
			v4l2_m2m_ioctl_streamoff(file, fh, type);
			return rc;
		}
	}
	mutex_lock(&f->run_lock);
	if (V4L2_TYPE_IS_OUTPUT(type)) {
		f->output_streaming = true;
	} else {
		f->capture_streaming = true;
		if (f->format_known) {
			vb2_clear_last_buffer_dequeued(v4l2_m2m_get_dst_vq(f->fh.m2m_ctx));
			f->format_pending = false;
		}
	}
	chd_admit(f, f->output_streaming);
	mutex_unlock(&f->run_lock);
	chd_kick(f);
	return 0;
rollback:
	/* A failed second-queue operation must not release an existing stream.
	 * For a new claim, retire the owner before freeing private DMA backing.
	 * Failed retirement deliberately retains both until lease cleanup.
	 */
	if (acquired) {
		cleanup_rc = chd_reset_channel(f, true);
		if (cleanup_rc)
			return cleanup_rc;
		for (i = 0; i < CHD_DISCOVERY_BUFFERS; i++)
			chd_discovery_free(&f->discovery[i]);
	}
	return rc;
}

static int chd_streamoff(struct file *file, void *fh, enum v4l2_buf_type type)
{
	struct crystalhd_v4l2_file *f = chd_file(fh);
	bool output = V4L2_TYPE_IS_OUTPUT(type);
	int rc;

	if (type != V4L2_BUF_TYPE_VIDEO_OUTPUT && type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (!vb2_is_streaming(v4l2_m2m_get_vq(f->fh.m2m_ctx, type)))
		return 0;
	if (!output && f->format_pending) {
		/* RX backing is already retired; a still-running TX may require
		 * the replacement CAPTURE setup to make forward progress.
		 */
		chd_admit(f, false);
		cancel_delayed_work_sync(&f->run_work);
	} else {
		chd_join(f);
	}
	/* A format pause has already proved RX retirement; preserve the firmware
	 * channel and buffered OUTPUT during the CAPTURE reallocation handshake.
	 */
	if (output || (!f->format_pending && !crystalhd_v4l2_decoder_drained(&f->decoder))) {
		rc = chd_reset_channel(f, output);
		if (rc)
			return rc;
	}
	mutex_lock(&f->run_lock);
	if (output)
		f->output_streaming = false;
	else
		f->capture_streaming = false;
	mutex_unlock(&f->run_lock);
	rc = v4l2_m2m_ioctl_streamoff(file, fh, type);
	if (!output && f->output_streaming && !f->format_pending &&
	    !crystalhd_v4l2_decoder_drained(&f->decoder)) {
		chd_admit(f, true);
		chd_kick(f);
	}
	return rc;
}

static int chd_try_decoder_cmd(struct file *file, void *fh,
				struct v4l2_decoder_cmd *cmd)
{
	(void)file;
	(void)fh;
	if (cmd->cmd != V4L2_DEC_CMD_STOP && cmd->cmd != V4L2_DEC_CMD_START)
		return -EINVAL;
	cmd->flags = 0;
	memset(&cmd->start, 0, sizeof(cmd->start));
	return 0;
}

static int chd_decoder_cmd(struct file *file, void *fh, struct v4l2_decoder_cmd *cmd)
{
	struct crystalhd_v4l2_file *f = chd_file(fh);
	int rc = chd_try_decoder_cmd(file, fh, cmd);

	if (rc || f->fatal)
		return rc ? rc : -EIO;
	mutex_lock(&f->run_lock);
	if (!f->output_streaming || !f->capture_streaming) {
		mutex_unlock(&f->run_lock);
		return 0;
	}
	if (f->drain_requested && !crystalhd_v4l2_decoder_drained(&f->decoder) &&
	    !(cmd->cmd == V4L2_DEC_CMD_START && f->format_pending)) {
		mutex_unlock(&f->run_lock);
		return -EBUSY;
	}
	if (cmd->cmd == V4L2_DEC_CMD_START) {
		if (f->format_pending) {
			rc = chd_reuse_capture(f);
			mutex_unlock(&f->run_lock);
			if (!rc)
				chd_kick(f);
			return rc;
		}
		if (f->decoder.phase == CHD_V4L2_RUNNING ||
		    f->decoder.phase == CHD_V4L2_OFF) {
			mutex_unlock(&f->run_lock);
			return 0;
		}
		if (!crystalhd_v4l2_decoder_drained(&f->decoder)) {
			mutex_unlock(&f->run_lock);
			return -EBUSY;
		}
		mutex_unlock(&f->run_lock);
		rc = chd_resume(f);
		if (rc)
			return rc;
	} else {
		if (f->output_streaming && f->capture_streaming &&
		    !crystalhd_v4l2_decoder_drained(&f->decoder)) {
			f->drain_left = v4l2_m2m_num_src_bufs_ready(f->fh.m2m_ctx);
			f->drain_requested = true;
			f->drain_clock_running = false;
			f->drain_clock_elapsed = 0;
			f->drain_clock_epoch = f->decoder.epoch;
			f->drain_clock_count = f->decoder.count;
		}
		mutex_unlock(&f->run_lock);
	}
	chd_kick(f);
	return 0;
}

static int chd_subscribe(struct v4l2_fh *fh, const struct v4l2_event_subscription *sub)
{
	if (sub->type == V4L2_EVENT_CTRL)
		return v4l2_ctrl_subscribe_event(fh, sub);
	if (sub->type == V4L2_EVENT_SOURCE_CHANGE)
		return v4l2_src_change_event_subscribe(fh, sub);
	if (sub->type == V4L2_EVENT_EOS)
		return v4l2_event_subscribe(fh, sub, 2, NULL);
	return -EINVAL;
}

static const struct v4l2_ioctl_ops chd_ioctl_ops = {
	.vidioc_querycap = chd_querycap,
	.vidioc_enum_fmt_vid_cap = chd_enum_fmt,
	.vidioc_enum_fmt_vid_out = chd_enum_fmt,
	.vidioc_try_fmt_vid_cap = chd_try_fmt,
	.vidioc_try_fmt_vid_out = chd_try_fmt,
	.vidioc_g_fmt_vid_cap = chd_get_fmt,
	.vidioc_g_fmt_vid_out = chd_get_fmt,
	.vidioc_s_fmt_vid_cap = chd_set_fmt,
	.vidioc_s_fmt_vid_out = chd_set_fmt,
	.vidioc_reqbufs = chd_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = chd_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_streamon = chd_streamon,
	.vidioc_streamoff = chd_streamoff,
	.vidioc_try_decoder_cmd = chd_try_decoder_cmd,
	.vidioc_decoder_cmd = chd_decoder_cmd,
	.vidioc_subscribe_event = chd_subscribe,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations chd_fops = {
	.owner = THIS_MODULE,
	.open = chd_open,
	.release = chd_release,
	.poll = v4l2_m2m_fop_poll,
	.mmap = v4l2_m2m_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static void chd_video_release(struct video_device *video)
{
	struct crystalhd_v4l2_node *node = container_of(video,
						struct crystalhd_v4l2_node, video);

	crystalhd_v4l2_parent_put(node->parent);
}

int crystalhd_v4l2_node_register(struct crystalhd_v4l2 *parent,
				struct v4l2_device *device, struct device *dma_dev,
				u16 chip, u64 generation,
				struct crystalhd_v4l2_node **out)
{
	struct crystalhd_v4l2_node *node;
	struct v4l2_ctrl *minimum;
	int rc;

	*out = NULL;
	if (chip != BC_PCI_DEVID_FLEA)
		return 0;
	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;
	node->parent = parent;
	node->generation = generation;
	node->dma_dev = get_device(dma_dev);
	mutex_init(&node->ioctl_lock);
	v4l2_ctrl_handler_init(&node->ctrls, 1);
	minimum = v4l2_ctrl_new_std(&node->ctrls, NULL,
				  V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, 2, 2, 1, 2);
	if (node->ctrls.error) {
		rc = node->ctrls.error;
		goto free_node;
	}
	minimum->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	node->run_wq = alloc_ordered_workqueue("crystalhd-decode", WQ_MEM_RECLAIM);
	if (!node->run_wq) {
		rc = -ENOMEM;
		goto free_node;
	}
	node->tx_wq = alloc_ordered_workqueue("crystalhd-input", WQ_MEM_RECLAIM);
	if (!node->tx_wq) {
		rc = -ENOMEM;
		goto free_node;
	}
	node->m2m = v4l2_m2m_init(&chd_m2m_ops);
	if (IS_ERR(node->m2m)) {
		rc = PTR_ERR(node->m2m);
		node->m2m = NULL;
		goto free_node;
	}
	node->video.v4l2_dev = device;
	node->video.ctrl_handler = &node->ctrls;
	node->video.fops = &chd_fops;
	node->video.ioctl_ops = &chd_ioctl_ops;
	node->video.release = chd_video_release;
	node->video.lock = &node->ioctl_lock;
	node->video.vfl_dir = VFL_DIR_M2M;
	node->video.device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
	strscpy(node->video.name, "crystalhd-decoder", sizeof(node->video.name));
	video_set_drvdata(&node->video, node);
	crystalhd_v4l2_parent_get(parent);
	rc = video_register_device(&node->video, VFL_TYPE_VIDEO, -1);
	if (rc) {
		crystalhd_v4l2_parent_put(parent);
		goto free_node;
	}
	*out = node;
	return 0;
free_node:
	crystalhd_v4l2_node_destroy(node);
	return rc;
}

void crystalhd_v4l2_node_unregister(struct crystalhd_v4l2_node *node)
{
	if (node)
		video_unregister_device(&node->video);
}

void crystalhd_v4l2_node_destroy(struct crystalhd_v4l2_node *node)
{
	if (!node)
		return;
	if (node->run_wq)
		destroy_workqueue(node->run_wq);
	if (node->tx_wq)
		destroy_workqueue(node->tx_wq);
	if (node->m2m)
		v4l2_m2m_release(node->m2m);
	v4l2_ctrl_handler_free(&node->ctrls);
	put_device(node->dma_dev);
	kfree(node);
}
