// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/errno.h>
#include <linux/string.h>
#include "crystalhd_lnx.h"
#include "crystalhd_stream.h"
#include "crystalhd_v4l2_decoder.h"
#include "crystalhd_v4l2_output.h"

void crystalhd_v4l2_decoder_init(struct crystalhd_v4l2_decoder *d)
{
	memset(d, 0, sizeof(*d));
	d->epoch = 1;
	d->next_token = 1;
}

void crystalhd_v4l2_decoder_reset(struct crystalhd_v4l2_decoder *d)
{
	memset(d->timestamps, 0, sizeof(d->timestamps));
	d->count = 0;
	/* Epoch exhaustion, like token exhaustion, permanently stops admission. */
	if (d->epoch == ~(u64)0) {
		d->phase = CHD_V4L2_FAILED;
		return;
	}
	d->epoch++;
	d->phase = CHD_V4L2_OFF;
}

int crystalhd_v4l2_decoder_open_locked(struct crystalhd_v4l2_decoder *d,
	struct crystalhd_cmd *cmd, const void *owner)
{
	const struct crystalhd_decoder_config config = {
		.codec = CRYSTALHD_DECODER_CODEC_H264,
	};
	int rc;

	if (!d || !cmd || !owner)
		return -EINVAL;
	if (d->phase != CHD_V4L2_OFF)
		return -EBUSY;
	if (cmd->decoder_phase != CRYSTALHD_DECODER_BOOTSTRAPPED) {
		rc = crystalhd_fw_bootstrap_locked(cmd, owner);
		if (rc)
			goto failed;
	}
	rc = crystalhd_decoder_channel_open_locked(cmd, owner, &config);
	if (rc)
		goto failed;
	rc = crystalhd_decoder_channel_start_locked(cmd, owner);
	if (rc)
		goto failed;
	d->phase = CHD_V4L2_RUNNING;
	return 0;
failed:
	d->phase = CHD_V4L2_FAILED;
	return rc;
}

int crystalhd_v4l2_decoder_stop_locked(struct crystalhd_v4l2_decoder *d,
	struct crystalhd_cmd *cmd, const void *owner)
{
	int rc;

	if (!d || !cmd || !owner)
		return -EINVAL;
	/* A transport failure can leave a stoppable firmware channel. */
	if (cmd->decoder_phase != CRYSTALHD_DECODER_CHANNEL_STARTED)
		return -EBUSY;
	rc = crystalhd_decoder_channel_stop_locked(cmd, owner);
	d->phase = rc ? CHD_V4L2_FAILED : CHD_V4L2_STOPPED;
	return rc;
}

int crystalhd_v4l2_decoder_close_locked(struct crystalhd_v4l2_decoder *d,
	struct crystalhd_cmd *cmd, const void *owner)
{
	int rc;

	if (!d || !cmd || !owner)
		return -EINVAL;
	if (cmd->decoder_phase != CRYSTALHD_DECODER_CHANNEL_CONFIGURED)
		return -EBUSY;
	rc = crystalhd_decoder_channel_close_locked(cmd, owner);
	if (rc)
		d->phase = CHD_V4L2_FAILED;
	else
		crystalhd_v4l2_decoder_reset(d);
	return rc;
}

int crystalhd_v4l2_decoder_reserve(struct crystalhd_v4l2_decoder *d,
	struct crystalhd_cmd *cmd, const void *owner, u64 timestamp, u64 *pts)
{
	u32 i, token;

	if (!d || !cmd || !owner || !pts)
		return -EINVAL;
	if (d->phase != CHD_V4L2_RUNNING)
		return -EPIPE;
	if (d->count == CRYSTALHD_V4L2_TIMESTAMPS)
		return -EAGAIN;
	if (d->next_token == 0xffffffffU)
		return -EOVERFLOW;
	for (i = 0; i < CRYSTALHD_V4L2_TIMESTAMPS; i++)
		if (!d->timestamps[i].token)
			break;
	if (i == CRYSTALHD_V4L2_TIMESTAMPS)
		return -EIO;
	token = d->next_token++;
	d->timestamps[i].token = token;
	d->timestamps[i].timestamp = timestamp;
	d->count++;
	/* libcrystalhd_if.cpp documents XPT's right shift of the PES PTS.
	 * Reserve zero and 0xffffffff (firmware preload/EOS special values).
	 */
	*pts = (u64)token << 1;
	return 0;
}

int crystalhd_v4l2_decoder_submitted(struct crystalhd_v4l2_decoder *d,
	int rc)
{
	if (!d)
		return -EINVAL;
	/* Partial TX may already have exposed the token to firmware. Retain it
	 * until teardown; no subsequent input is admitted after any error.
	 */
	if (rc)
		d->phase = CHD_V4L2_FAILED;
	return rc;
}

int crystalhd_v4l2_decoder_submit(struct crystalhd_v4l2_decoder *d,
	struct crystalhd_cmd *cmd, const void *owner, const u8 *data,
	size_t bytes, u64 timestamp, u32 timeout_ms)
{
	u64 pts;
	int rc;

	if (!data || !bytes)
		return -EINVAL;
	rc = crystalhd_v4l2_decoder_reserve(d, cmd, owner, timestamp, &pts);
	if (rc)
		return rc;
	rc = crystalhd_decoder_submit_h264(cmd, owner, data, bytes, true,
					 pts, timeout_ms);
	return crystalhd_v4l2_decoder_submitted(d, rc);
}

int crystalhd_v4l2_decoder_submit_mapped(struct crystalhd_v4l2_decoder *d,
	struct crystalhd_cmd *cmd, const void *owner,
	struct crystalhd_v4l2_output_buffer *buffer, u64 timestamp, u32 timeout_ms)
{
	u64 pts;
	int rc;

	if (!buffer)
		return -EINVAL;
	rc = crystalhd_v4l2_decoder_reserve(d, cmd, owner, timestamp, &pts);
	if (rc)
		return rc;
	rc = crystalhd_v4l2_output_submit(cmd, owner, buffer, pts, timeout_ms);
	return crystalhd_v4l2_decoder_submitted(d, rc);
}

int crystalhd_v4l2_decoder_begin_drain(struct crystalhd_v4l2_decoder *d)
{
	if (!d)
		return -EINVAL;
	if (d->phase != CHD_V4L2_RUNNING)
		return -EPIPE;
	d->phase = CHD_V4L2_DRAINING;
	return 0;
}

int crystalhd_v4l2_decoder_drain(struct crystalhd_v4l2_decoder *d,
	struct crystalhd_cmd *cmd, const void *owner, u32 timeout_ms)
{
	int rc;

	if (!cmd || !owner)
		return -EINVAL;
	rc = crystalhd_v4l2_decoder_begin_drain(d);
	if (rc)
		return rc;
	rc = crystalhd_decoder_submit_h264_eos(cmd, owner, timeout_ms);
	return crystalhd_v4l2_decoder_submitted(d, rc);
}

int crystalhd_v4l2_decoder_complete(struct crystalhd_v4l2_decoder *d,
	const struct crystalhd_rx_metadata *metadata, u64 epoch, u64 *timestamp)
{
	u32 i;

	if (!d || !metadata || !timestamp)
		return -EINVAL;
	if (epoch != d->epoch)
		return -ESTALE;
	if (d->phase != CHD_V4L2_RUNNING && d->phase != CHD_V4L2_DRAINING)
		return -EPIPE;
	if (!metadata->valid)
		return -ENODATA;
	/* A contradictory error/EOS snapshot can prove neither frame nor drain.
	 * Keep its token ledger intact for terminal teardown, not guessed repair.
	 */
	if ((metadata->picture_flags & CRYSTALHD_PICTURE_FLAG_DECODE_ERROR) &&
	    (metadata->eos_trailer ||
	     (metadata->picture_flags & CRYSTALHD_PICTURE_FLAG_EOS)))
		return -EPROTO;
	if (metadata->eos_trailer) {
		/* DtsGetFleaPictureInfo requires the sentinel AND VDEC_FLAG_EOS.
		 * A trailer bit or idle hardware alone does not prove drain.
		 */
		if (d->phase != CHD_V4L2_DRAINING || metadata->pib_line ||
		    metadata->picture_number != 0xffffffffU ||
		    !(metadata->picture_flags & CRYSTALHD_PICTURE_FLAG_EOS))
			return -ENODATA;
		/* H264 is advertised without CONTINUOUS_BYTESTREAM: each admitted
		 * progressive AU owes one correlated picture or explicit error.
		 * A marker cannot erase missing results or prove an earlier drain.
		 */
		if (d->count)
			return -EPROTO;
		d->phase = CHD_V4L2_DRAINED;
		return 1;
	}
	if (!metadata->firmware_timestamp ||
	    metadata->firmware_timestamp >= 0xffffffffULL)
		return -ESTALE;
	for (i = 0; i < CRYSTALHD_V4L2_TIMESTAMPS; i++) {
		if (d->timestamps[i].token != metadata->firmware_timestamp)
			continue;
		*timestamp = d->timestamps[i].timestamp;
		d->timestamps[i].token = 0;
		d->count--;
		return metadata->picture_flags & CRYSTALHD_PICTURE_FLAG_DECODE_ERROR ?
			-EILSEQ : 0;
	}
	return -ESTALE;
}

bool crystalhd_v4l2_decoder_drained(const struct crystalhd_v4l2_decoder *d)
{
	return d && d->phase == CHD_V4L2_DRAINED;
}
