// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/slab.h>

#include "crystalhd_lnx.h"
#include "crystalhd_stream.h"

#define CRYSTALHD_H264_PES_MAX_LENGTH	0xfff0U
#define CRYSTALHD_H264_PES_HEADER	9U
#define CRYSTALHD_H264_PTS_HEADER	14U
#define CRYSTALHD_H264_PES_MAX_WIRE	(CRYSTALHD_H264_PES_MAX_LENGTH + 6U)
#define CRYSTALHD_H264_STAGE_BYTES	ALIGN(CRYSTALHD_H264_PES_MAX_WIRE, 4U)
#define CRYSTALHD_H264_EOS_BYTES		17U
#define CRYSTALHD_H264_MARKER_BYTES	184U

struct crystalhd_stream {
	struct device *dev;
	u8 *cpu;
	dma_addr_t dma;
	struct scatterlist sg;
	struct crystalhd_tx_buffer buffer;
	refcount_t refs;
	bool failed;
	bool eos_submitted;
};

static int crystalhd_h264_format_pes(u8 *dst, size_t capacity,
				     const u8 *src, size_t source_bytes,
				     bool pts_valid, u64 pts,
				     size_t *consumed, size_t *wire_bytes)
{
	size_t header_bytes = pts_valid ? CRYSTALHD_H264_PTS_HEADER :
					  CRYSTALHD_H264_PES_HEADER;
	size_t payload_limit = CRYSTALHD_H264_PES_MAX_LENGTH -
				 (pts_valid ? 8U : 3U);
	size_t payload_bytes, packet_bytes;
	u32 packet_length;
	u64 raw_pts;

	if (!dst || !src || !source_bytes || !consumed || !wire_bytes)
		return -EINVAL;

	payload_bytes = min(source_bytes, payload_limit);
	packet_bytes = header_bytes + payload_bytes;
	if (capacity < packet_bytes)
		return -ENOSPC;

	packet_length = payload_bytes + (pts_valid ? 8U : 3U);
	dst[0] = 0x00;
	dst[1] = 0x00;
	dst[2] = 0x01;
	dst[3] = 0xe0;
	dst[4] = packet_length >> 8;
	dst[5] = packet_length;
	dst[6] = 0x81;
	dst[7] = pts_valid ? 0x80 : 0x00;
	dst[8] = pts_valid ? 0x05 : 0x00;
	if (pts_valid) {
		raw_pts = pts & GENMASK_ULL(32, 0);
		dst[9] = 0x21 | (((raw_pts >> 30) & 0x07) << 1);
		dst[10] = raw_pts >> 22;
		dst[11] = 0x01 | (((raw_pts >> 15) & 0x7f) << 1);
		dst[12] = raw_pts >> 7;
		dst[13] = 0x01 | ((raw_pts & 0x7f) << 1);
	}
	memcpy(dst + header_bytes, src, payload_bytes);
	*consumed = payload_bytes;
	*wire_bytes = packet_bytes;
	return 0;
}

static int crystalhd_h264_format_eos(u8 *dst, size_t capacity,
				     unsigned int packet_index,
				     size_t *wire_bytes)
{
	static const u8 eos[CRYSTALHD_H264_EOS_BYTES] = {
		0x00, 0x00, 0x01, 0xe0, 0x00, 0x0b, 0x81, 0x00, 0x00,
		0x00, 0x00, 0x01, 0x0a, 0x00, 0x00, 0x01, 0x0a,
	};
	u8 *body;

	if (!dst || !wire_bytes || packet_index >= 4)
		return -EINVAL;
	if (packet_index != 1) {
		if (capacity < sizeof(eos))
			return -ENOSPC;
		memcpy(dst, eos, sizeof(eos));
		*wire_bytes = sizeof(eos);
		return 0;
	}
	if (capacity < CRYSTALHD_H264_MARKER_BYTES)
		return -ENOSPC;

	memset(dst, 0, CRYSTALHD_H264_MARKER_BYTES);
	dst[2] = 0x01;
	dst[3] = 0xe0;
	dst[4] = 0x00;
	dst[5] = 0xb2;
	dst[6] = 0x81;
	dst[7] = 0x01;
	dst[8] = 0x14;
	dst[9] = 0x80;
	dst[10] = 'B';
	dst[11] = 'R';
	dst[12] = 'C';
	dst[13] = 'M';
	dst[26] = 0xff;
	dst[27] = 0xff;
	dst[28] = 0xff;

	body = dst + 29;
	body[4] = 0x0c;
	body[13] = 0xff;
	body[14] = 0xff;
	body[16] = 0x01;
	memset(body + 17, 0xff, 12);
	body[36] = 0xbc;
	memset(body + 37, 0xff, 118);
	*wire_bytes = CRYSTALHD_H264_MARKER_BYTES;
	return 0;
}

static void crystalhd_stream_get(const struct crystalhd_tx_buffer *buffer)
{
	struct crystalhd_stream *stream = buffer->cookie;

	refcount_inc(&stream->refs);
}

static void crystalhd_stream_put(struct crystalhd_adp *adp,
				 const struct crystalhd_tx_buffer *buffer)
{
	struct crystalhd_stream *stream = buffer->cookie;

	(void)adp;
	if (!refcount_dec_and_test(&stream->refs))
		return;
	if (stream->cpu)
		dma_free_coherent(stream->dev,
				  CRYSTALHD_H264_STAGE_BYTES,
				  stream->cpu, stream->dma);
	kfree(stream);
}

static const struct crystalhd_tx_buffer_ops crystalhd_stream_buffer_ops = {
	.get = crystalhd_stream_get,
	.put = crystalhd_stream_put,
};

int crystalhd_stream_prepare(struct crystalhd_cmd *ctx)
{
	struct crystalhd_stream *stream;

	if (!ctx || !ctx->adp || !ctx->adp->pdev)
		return -ENODEV;
	if (ctx->stream)
		return 0;

	stream = kzalloc(sizeof(*stream), GFP_KERNEL);
	if (!stream)
		return -ENOMEM;
	stream->dev = &ctx->adp->pdev->dev;
	stream->cpu = dma_alloc_coherent(stream->dev,
					 CRYSTALHD_H264_STAGE_BYTES,
					 &stream->dma, GFP_KERNEL);
	if (!stream->cpu) {
		kfree(stream);
		return -ENOMEM;
	}
	sg_init_table(&stream->sg, 1);
	refcount_set(&stream->refs, 1);
	stream->buffer.sgl = &stream->sg;
	stream->buffer.dma_nents = 1;
	stream->buffer.ops = &crystalhd_stream_buffer_ops;
	stream->buffer.cookie = stream;
	ctx->stream = stream;
	return 0;
}

void crystalhd_stream_release(struct crystalhd_cmd *ctx)
{
	struct crystalhd_stream *stream;

	if (!ctx)
		return;
	stream = ctx->stream;
	if (!stream)
		return;
	/* An admitted controller operation may discover fail-stop concurrently.
	 * Keep the context reference until terminal cleanup has proved safety.
	 */
	if (ctx->hw_ctx && ctx->adp &&
	    (READ_ONCE(ctx->hw_ctx->dma_fault) ||
	     !READ_ONCE(ctx->adp->present)) &&
	    !ctx->adp->dma_terminal_quiesced)
		return;
	ctx->stream = NULL;
	crystalhd_tx_buffer_put(ctx->adp, &stream->buffer);
}

int crystalhd_decoder_validate_h264_locked(struct crystalhd_cmd *ctx,
				   const void *owner)
{
	const u32 allowed_state = BC_LINK_INIT | BC_LINK_CAP_EN |
				  BC_LINK_FMT_CHG;

	if (!ctx || !owner)
		return -EINVAL;
	if (!ctx->adp || !ctx->adp->pdev || !ctx->hw_ctx)
		return -ENODEV;
	lockdep_assert_held(&ctx->adp->user_lock);
	lockdep_assert_held(&ctx->adp->tx_lock);
	if (!READ_ONCE(ctx->adp->present))
		return -ENODEV;
	if (READ_ONCE(ctx->hw_ctx->dma_fault))
		return -EIO;
	if (!ctx->session_owner)
		return -EINVAL;
	if (ctx->session_owner != owner)
		return -EBUSY;
	if (ctx->adp->pdev->device != BC_PCI_DEVID_FLEA)
		return -EOPNOTSUPP;
	if (!(ctx->state & BC_LINK_INIT) || (ctx->state & ~allowed_state) ||
	    ctx->decoder_phase != CRYSTALHD_DECODER_CHANNEL_STARTED ||
	    ctx->decoder_codec != CRYSTALHD_DECODER_CODEC_H264 ||
	    !ctx->fw_sequence || ctx->decoder_channel_id || !ctx->stream)
		return -EBUSY;
	if (ctx->stream->failed || ctx->stream->eos_submitted)
		return -EPIPE;
	return 0;
}

static int crystalhd_h264_send_staged(struct crystalhd_cmd *ctx,
				      size_t wire_bytes,
				      unsigned long deadline)
{
	struct crystalhd_stream *stream = ctx->stream;
	size_t aligned_bytes = wire_bytes & ~3U;
	BC_STATUS sts;

	stream->buffer.bytes = wire_bytes;
	stream->buffer.tail_addr = stream->dma + aligned_bytes;
	stream->buffer.tail_size = wire_bytes & 3U;
	memset(stream->cpu + wire_bytes, 0,
	       ALIGN(wire_bytes, 4U) - wire_bytes);
	sg_dma_address(&stream->sg) = stream->dma;
	sg_dma_len(&stream->sg) = aligned_bytes;
	dma_wmb();
	sts = crystalhd_tx_transfer_until(ctx, &stream->buffer, 0, deadline);
	if (sts != BC_STS_SUCCESS)
		stream->failed = true;
	return crystalhd_status_to_errno(sts);
}

int crystalhd_decoder_submit_h264(struct crystalhd_cmd *ctx,
				  const void *owner, const u8 *annexb,
				  size_t bytes, bool pts_valid, u64 pts,
				  u32 total_timeout_ms)
{
	unsigned long deadline;
	size_t offset = 0;
	BC_STATUS sts;
	int rc;

	if (!annexb || !bytes)
		return -EINVAL;
	rc = crystalhd_decoder_validate_h264_locked(ctx, owner);
	if (rc)
		return rc;
	sts = crystalhd_tx_deadline_from_ms(total_timeout_ms, &deadline);
	if (sts != BC_STS_SUCCESS)
		return crystalhd_status_to_errno(sts);

	while (offset < bytes) {
		size_t consumed, wire_bytes;

		rc = crystalhd_h264_format_pes(ctx->stream->cpu,
					       CRYSTALHD_H264_STAGE_BYTES,
					       annexb + offset,
					       bytes - offset,
					       pts_valid && !offset, pts,
					       &consumed, &wire_bytes);
		if (rc)
			return rc;
		rc = crystalhd_h264_send_staged(ctx, wire_bytes, deadline);
		if (rc)
			return rc;
		offset += consumed;
	}
	return 0;
}

int crystalhd_decoder_submit_h264_eos(struct crystalhd_cmd *ctx,
				      const void *owner,
				      u32 total_timeout_ms)
{
	unsigned long deadline;
	BC_STATUS sts;
	unsigned int i;
	int rc;

	rc = crystalhd_decoder_validate_h264_locked(ctx, owner);
	if (rc)
		return rc;
	sts = crystalhd_tx_deadline_from_ms(total_timeout_ms, &deadline);
	if (sts != BC_STS_SUCCESS)
		return crystalhd_status_to_errno(sts);

	for (i = 0; i < 4; i++) {
		size_t wire_bytes;

		rc = crystalhd_h264_format_eos(ctx->stream->cpu,
					       CRYSTALHD_H264_STAGE_BYTES,
					       i, &wire_bytes);
		if (rc)
			return rc;
		rc = crystalhd_h264_send_staged(ctx, wire_bytes, deadline);
		if (rc)
			return rc;
	}
	ctx->stream->eos_submitted = true;
	return 0;
}
