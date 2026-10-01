/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYSTALHD_STREAM_H_
#define _CRYSTALHD_STREAM_H_

#include <linux/types.h>

struct crystalhd_cmd;

int crystalhd_stream_prepare(struct crystalhd_cmd *ctx);
void crystalhd_stream_release(struct crystalhd_cmd *ctx);

/* Submit one complete Annex-B access unit. The caller excludes PCI removal,
 * holds user_lock for session lifetime, and holds tx_lock across the call.
 * pts is the raw 33-bit firmware correlation field, not a 90-kHz timestamp.
 */
int crystalhd_decoder_submit_h264(struct crystalhd_cmd *ctx,
				  const void *owner, const u8 *annexb,
				  size_t bytes, bool pts_valid, u64 pts,
				  u32 total_timeout_ms);

/* Submit the BCM70015 H.264 E-M-E-E marker sequence. Success means that all
 * four packets completed TX; firmware drain completion is a separate event.
 * Any transport failure, or a successful EOS submission, blocks further
 * input on this staging session until the firmware channel is closed.
 */
int crystalhd_decoder_submit_h264_eos(struct crystalhd_cmd *ctx,
				      const void *owner,
				      u32 total_timeout_ms);

#endif
