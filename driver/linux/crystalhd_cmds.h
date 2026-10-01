/***************************************************************************
 * Copyright (c) 2005-2009, Broadcom Corporation.
 *
 *  Name: crystalhd_cmds . h
 *
 *  Description:
 *		BCM70010 Linux driver user command interfaces.
 *
 *  HISTORY:
 *
 **********************************************************************
 * This file is part of the crystalhd device driver.
 *
 * This driver is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This driver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this driver.  If not, see <http://www.gnu.org/licenses/>.
 **********************************************************************/

#ifndef _CRYSTALHD_CMDS_H_
#define _CRYSTALHD_CMDS_H_

/*
 * NOTE:: This is the main interface file between the Linux layer
 *        and the harware layer. This file will use the definitions
 *        from _dts_glob and dts_defs etc.. which are defined for
 *        windows.
 */

#include "crystalhd_hw.h"
#include "crystalhd_misc.h"

extern struct device * chddev(void);

enum _crystalhd_state{
	BC_LINK_INVALID		= 0x00,
	BC_LINK_INIT		= 0x01,
	BC_LINK_CAP_EN		= 0x02,
	BC_LINK_FMT_CHG		= 0x04,
	BC_LINK_SUSPEND		= 0x10,
	BC_LINK_PAUSED		= 0x20,
	BC_LINK_RESUME		= 0x40,
	BC_LINK_READY	= (BC_LINK_INIT | BC_LINK_CAP_EN | BC_LINK_FMT_CHG),
};

enum crystalhd_decoder_phase {
	CRYSTALHD_DECODER_COLD = 0,
	CRYSTALHD_DECODER_BOOTSTRAPPED,
	CRYSTALHD_DECODER_CHANNEL_CONFIGURED,
	CRYSTALHD_DECODER_CHANNEL_STARTED,
	CRYSTALHD_DECODER_RECOVERY_REQUIRED,
};

enum crystalhd_decoder_codec {
	CRYSTALHD_DECODER_CODEC_INVALID = -1,
	CRYSTALHD_DECODER_CODEC_H264 = 0,
};

struct crystalhd_decoder_config {
	enum crystalhd_decoder_codec codec;
};

struct crystalhd_user {
	uint32_t	uid;
	uint32_t	in_use;
	uint32_t	mode;
};

#define DTS_MODE_INV	(-1)

#define CRYSTALHD_LINK_FIRMWARE_NAME "bcm70012fw.bin"
#define CRYSTALHD_FLEA_FIRMWARE_NAME "bcm70015fw.bin"

struct crystalhd_cmd {
	uint32_t		state;
	struct crystalhd_adp	*adp;
	struct crystalhd_user	user[BC_LINK_MAX_OPENS];
	/* Opaque owner identity; never dereferenced. Device teardown may revoke it
	 * without a frontend release.
	 */
	const void		*session_owner;
	enum crystalhd_decoder_phase decoder_phase;
	enum crystalhd_decoder_codec decoder_codec;
	uint32_t		fw_sequence;
	uint32_t		decoder_channel_id;

	spinlock_t		ctx_lock;
	uint32_t		tx_list_id;
	uint32_t		cin_wait_exit;
	uint32_t		pwr_state_change; /* 0 is running, 1 is going to suspend, 2 is going to resume */
	struct crystalhd_hw		*hw_ctx;
};

typedef BC_STATUS (*crystalhd_cmd_proc)(struct crystalhd_cmd *, crystalhd_ioctl_data *);

struct crystalhd_cmd_tbl {
	uint32_t		cmd_id;
	const crystalhd_cmd_proc	cmd_proc;
	uint32_t		requires_session_owner;
};


BC_STATUS crystalhd_suspend(struct crystalhd_cmd *ctx, crystalhd_ioctl_data *idata);
BC_STATUS crystalhd_resume(struct crystalhd_cmd *ctx);
crystalhd_cmd_proc crystalhd_get_cmd_proc(struct crystalhd_cmd *ctx, uint32_t cmd,
				      struct crystalhd_user *uc);
BC_STATUS crystalhd_user_open(struct crystalhd_cmd *ctx, struct crystalhd_user **user_ctx);
BC_STATUS crystalhd_user_set_mode(struct crystalhd_cmd *ctx,
				 struct crystalhd_user *uc, uint32_t mode);
/* Caller excludes PCI removal (normally with chd_device_lock for read), has
 * verified a present adapter, and holds adp->user_lock exclusively. The owner
 * token is stable and unique from successful acquisition through release.
 * Do not call release after the device becomes unavailable; teardown forcibly
 * revokes the token without dereferencing it.
 */
BC_STATUS crystalhd_session_acquire_locked(struct crystalhd_cmd *ctx,
					   const void *owner);
BC_STATUS crystalhd_session_release_locked(struct crystalhd_cmd *ctx,
					   const void *owner);
/* The caller excludes device removal, has verified a present adapter, holds
 * adp->user_lock exclusively, and keeps the exact session-owner token and image
 * valid through return. The image is validated for the detected chip before
 * any firmware transaction or hardware access. A successful download publishes
 * INIT and resets mailbox quarantine before releasing the transaction.
 */
BC_STATUS crystalhd_fw_download_locked(struct crystalhd_cmd *ctx,
				       const void *owner,
				       const uint8_t *image, size_t size);
/* Synchronously request the image selected for the adapter and download it
 * through the shared owner/state/recovery boundary above. The caller excludes
 * device removal, holds adp->user_lock exclusively, and keeps the exact owner
 * token live through return. This operation may sleep.
 */
int crystalhd_request_firmware_locked(struct crystalhd_cmd *ctx,
				      const void *owner);
/* The caller excludes device removal, holds adp->user_lock for read or write,
 * and supplies the token used to acquire the active decoder session.
 * Firmware-command transaction locking, timeout quarantine and the matching
 * local pause/flush transitions are handled here for every frontend.
 */
BC_STATUS crystalhd_fw_exec_locked(struct crystalhd_cmd *ctx,
				   const void *owner, BC_FW_CMD *fw_cmd);
/* Request the chip firmware and issue its fixed C011 INIT command as one
 * retryable controller transition from BC_LINK_INVALID, BC_LINK_RESUME, or
 * BC_LINK_INIT while decoder recovery is required. The caller excludes device
 * removal, holds adp->user_lock exclusively, and owns the active session. A
 * successful verified download resets the decoder phase and command sequence;
 * on failure, the next attempt must perform another fresh verified download
 * before issuing a command.
 */
int crystalhd_fw_bootstrap_locked(struct crystalhd_cmd *ctx,
				  const void *owner);
/* Configure the first BCM70015 decoder channel with one OPEN + INPUT_PARAMS
 * transition. The caller excludes PCI removal, holds adp->user_lock
 * exclusively, owns the active session, and has completed the bootstrap above
 * without dropping that lock. A partial channel open is never published; it
 * requires a fresh bootstrap before retry.
 */
int crystalhd_decoder_channel_open_locked(
	struct crystalhd_cmd *ctx, const void *owner,
	const struct crystalhd_decoder_config *config);
/* Activate and start the configured BCM70015 channel using the progressive
 * H.264 baseline. The caller retains the same exclusion, write lock and owner
 * token used for channel setup. Once activation succeeds, any incomplete start
 * requires a fresh bootstrap rather than a partial retry.
 */
int crystalhd_decoder_channel_start_locked(struct crystalhd_cmd *ctx,
					   const void *owner);
/* Stop only the firmware channel after TX admission and ownership have been
 * quiesced. Capture remains caller-owned so it can be flushed after firmware
 * stops producing pictures and before channel_close_locked(). Keep removal
 * lifetime and the write lock across that whole sequence. A successful stop
 * returns to the configured phase and may be followed by start or close.
 */
int crystalhd_decoder_channel_stop_locked(struct crystalhd_cmd *ctx,
					  const void *owner);
/* Close a configured firmware channel, including one returned there by STOP.
 * The caller must already have reclaimed every RX registration; exact
 * BC_LINK_INIT is required so an active capture engine cannot be mistaken for
 * a closed channel.
 */
int crystalhd_decoder_channel_close_locked(struct crystalhd_cmd *ctx,
					   const void *owner);
/* Caller retains device/session lifetime and serializes TX submission through
 * return; the legacy ioctl adapter does this with user_lock and tx_lock.
 * A supported nonzero timeout bounds admission and completion waiting as one
 * budget; values above INT_MAX or conversions to MAX_JIFFY_OFFSET are rejected.
 * Safe cancellation can extend return and posted DMA keeps its three-second
 * hardware watchdog. Zero preserves the legacy unbounded-admission policy.
 */
BC_STATUS crystalhd_tx_transfer_sync(struct crystalhd_cmd *ctx,
				     const struct crystalhd_tx_buffer *buffer,
				     uint8_t data_flags,
				     uint32_t total_timeout_ms);
BC_STATUS crystalhd_rx_submit(struct crystalhd_cmd *ctx,
			      struct crystalhd_rx_buffer *buffer);
BC_STATUS crystalhd_rx_dequeue(struct crystalhd_cmd *ctx,
			       struct crystalhd_rx_completion *result);
BC_STATUS crystalhd_capture_start(struct crystalhd_cmd *ctx,
				  uint32_t pause_threshold,
				  uint32_t resume_threshold);
BC_STATUS crystalhd_capture_flush(struct crystalhd_cmd *ctx, bool discard_only);
void crystalhd_user_close(struct crystalhd_cmd *ctx, struct crystalhd_user *uc);
BC_STATUS crystalhd_setup_cmd_context(struct crystalhd_cmd *ctx, struct crystalhd_adp *adp);
BC_STATUS crystalhd_delete_cmd_context(struct crystalhd_cmd *ctx);
bool crystalhd_cmd_interrupt(struct crystalhd_cmd *ctx);

#endif
