/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYSTALHD_V4L2_DECODER_H_
#define _CRYSTALHD_V4L2_DECODER_H_
#include <linux/types.h>

#define CRYSTALHD_V4L2_TIMESTAMPS 256U
struct crystalhd_cmd;
struct crystalhd_rx_metadata;
struct crystalhd_v4l2_output_buffer;

enum crystalhd_v4l2_decoder_phase {
	CHD_V4L2_OFF, CHD_V4L2_RUNNING, CHD_V4L2_DRAINING,
	CHD_V4L2_DRAINED, CHD_V4L2_STOPPED, CHD_V4L2_FAILED,
};

struct crystalhd_v4l2_decoder {
	enum crystalhd_v4l2_decoder_phase phase;
	u64 epoch;
	u32 next_token;
	u32 count;
	struct {
		u64 timestamp;
		u32 token;
	} timestamps[CRYSTALHD_V4L2_TIMESTAMPS];
};

/* All calls require caller serialization. Never reinitialize a live context:
 * tokens are never reused, even across resets. RX registrations save epoch.
 */
void crystalhd_v4l2_decoder_init(struct crystalhd_v4l2_decoder *d);
/* Only after RX/TX are reclaimed and the channel is closed or hardware has
 * proven terminal quiescence. Invalidates every prior RX registration.
 */
void crystalhd_v4l2_decoder_reset(struct crystalhd_v4l2_decoder *d);
/* Stable session owner, removal exclusion and user_lock write held. Open
 * bootstraps only if needed; reopen after CLOSE preserves loaded firmware.
 * Restart sequence is STOP, caller RX flush, CLOSE, OPEN.
 */
int crystalhd_v4l2_decoder_open_locked(struct crystalhd_v4l2_decoder *d,
	struct crystalhd_cmd *cmd, const void *owner);
int crystalhd_v4l2_decoder_stop_locked(struct crystalhd_v4l2_decoder *d,
	struct crystalhd_cmd *cmd, const void *owner);
int crystalhd_v4l2_decoder_close_locked(struct crystalhd_v4l2_decoder *d,
	struct crystalhd_cmd *cmd, const void *owner);
/* Two-stage asynchronous transport admission. Hold the frontend state lock
 * for these calls, but release it while executing the shared TX operation so
 * RX can retire pictures concurrently. reserve returns the raw PES token
 * (already shifted for XPT), and submitted must run after transport returns.
 * A successful submitted preserves concurrent RX state, including DRAINED.
 */
int crystalhd_v4l2_decoder_reserve(struct crystalhd_v4l2_decoder *d,
	struct crystalhd_cmd *cmd, const void *owner, u64 timestamp, u64 *raw_pts);
int crystalhd_v4l2_decoder_submitted(struct crystalhd_v4l2_decoder *d, int rc);
/* Arm before sending E-M-E-E outside the frontend state lock. The caller
 * must hold any observed LAST until the transport completes successfully;
 * DRAINED alone does not prove that concurrent EOS TX has returned success.
 */
int crystalhd_v4l2_decoder_begin_drain(struct crystalhd_v4l2_decoder *d);
/* Stable owner, removal exclusion, user_lock and tx_lock held. -EAGAIN is
 * timestamp-ledger backpressure; successful TX does not retire its timestamp.
 */
int crystalhd_v4l2_decoder_submit(struct crystalhd_v4l2_decoder *d,
	struct crystalhd_cmd *cmd, const void *owner, const u8 *data,
	size_t bytes, u64 timestamp, u32 timeout_ms);
/* Direct mapped OUTPUT variant, with identical ledger and failure semantics.
 * Caller additionally preserves the adapter buffer until its core lease ends.
 */
int crystalhd_v4l2_decoder_submit_mapped(struct crystalhd_v4l2_decoder *d,
	struct crystalhd_cmd *cmd, const void *owner,
	struct crystalhd_v4l2_output_buffer *buffer, u64 timestamp, u32 timeout_ms);
int crystalhd_v4l2_decoder_drain(struct crystalhd_v4l2_decoder *d,
	struct crystalhd_cmd *cmd, const void *owner, u32 timeout_ms);
/* Call in RX order after finish_yuyv success/NO_DATA, or IO_ERROR with a valid
 * firmware decode-error snapshot. Returns
 * 0 and exact application timestamp for a picture, 1 for genuine drained EOS,
 * -EILSEQ and exact application timestamp for a correlated failed picture,
 * -ENODATA for nonpicture metadata, -ESTALE for old epochs/unknown tokens.
 * Only 0/-EILSEQ consume one picture token; other errors retain the ledger.
 * Do not call twice on one completion.
 */
int crystalhd_v4l2_decoder_complete(struct crystalhd_v4l2_decoder *d,
	const struct crystalhd_rx_metadata *metadata, u64 epoch, u64 *timestamp);
bool crystalhd_v4l2_decoder_drained(const struct crystalhd_v4l2_decoder *d);
#endif
