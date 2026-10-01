/***************************************************************************
 * Copyright (c) 2005-2009, Broadcom Corporation.
 *
 *  Name: crystalhd_cmds . c
 *
 *  Description:
 *		BCM70012/BCM70015 Linux driver user command interfaces.
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

#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/sched/signal.h>

#include "crystalhd_lnx.h"
#include "crystalhd_hw.h"
int bc_get_userhandle_count(struct crystalhd_cmd *ctx);
BC_STATUS bc_cproc_release_user(struct crystalhd_cmd *ctx, crystalhd_ioctl_data *idata);
static struct crystalhd_user *bc_cproc_get_uid(struct crystalhd_cmd *ctx)
{
	struct crystalhd_user *user = NULL;
	int i;

	for (i = 0; i < BC_LINK_MAX_OPENS; i++) {
		if (!ctx->user[i].in_use) {
			user = &ctx->user[i];
			break;
		}
	}

	return user;
}

int bc_get_userhandle_count(struct crystalhd_cmd *ctx)
{
	int i, count = 0;

	for (i = 0; i < BC_LINK_MAX_OPENS; i++) {
		if (ctx->user[i].in_use)
			count++;
	}

	return count;
}

static void bc_cproc_mark_pwr_state(struct crystalhd_cmd *ctx, uint32_t state)
{
	int i;

	for (i = 0; i < BC_LINK_MAX_OPENS; i++) {
		if (!ctx->user[i].in_use)
			continue;
		if ((ctx->user[i].mode & 0xFF) == DTS_DIAG_MODE ||
		    (ctx->user[i].mode & 0xFF) == DTS_PLAYBACK_MODE) {
			ctx->pwr_state_change = state;
			break;
		}
	}
}

static void crystalhd_decoder_tracking_reset(struct crystalhd_cmd *ctx)
{
	ctx->decoder_phase = CRYSTALHD_DECODER_COLD;
	ctx->fw_sequence = 0;
	ctx->decoder_channel_id = 0;
}

/* Caller holds user_lock exclusively on a present, resumed device. */
static BC_STATUS crystalhd_ensure_hw_context(struct crystalhd_cmd *ctx)
{
	BC_STATUS sts;

	if (ctx->hw_ctx)
		return BC_STS_SUCCESS;

	disable_irq(ctx->adp->pdev->irq);
	ctx->hw_ctx = kmalloc(sizeof(struct crystalhd_hw), GFP_KERNEL);
	if (!ctx->hw_ctx) {
		enable_irq(ctx->adp->pdev->irq);
		return BC_STS_ERROR;
	}
	memset(ctx->hw_ctx, 0, sizeof(struct crystalhd_hw));

	sts = crystalhd_hw_open(ctx->hw_ctx, ctx->adp);
	if (sts != BC_STS_SUCCESS) {
		kfree(ctx->hw_ctx);
		ctx->hw_ctx = NULL;
	}
	enable_irq(ctx->adp->pdev->irq);
	return sts;
}

static BC_STATUS crystalhd_session_setup(struct crystalhd_cmd *ctx)
{
	BC_STATUS sts;
	int rc;

	/* Create list pools */
	rc = crystalhd_create_elem_pool(ctx->adp, BC_LINK_ELEM_POOL_SZ);
	if (rc) {
		crystalhd_delete_elem_pool(ctx->adp);
		return BC_STS_ERROR;
	}
	/* Setup mmap pool for uaddr sgl mapping..*/
	rc = crystalhd_create_dio_pool(ctx->adp, BC_LINK_MAX_SGLS);
	if (rc) {
		crystalhd_delete_elem_pool(ctx->adp);
		return BC_STS_ERROR;
	}

	/* Setup Hardware DMA rings */
	sts = crystalhd_hw_setup_dma_rings(ctx->hw_ctx);
	if (sts != BC_STS_SUCCESS) {
		crystalhd_destroy_dio_pool(ctx->adp);
		crystalhd_delete_elem_pool(ctx->adp);
		return sts;
	}

	return BC_STS_SUCCESS;
}

static void crystalhd_retire_hw_context(struct crystalhd_cmd *ctx,
					bool retire_session_resources)
{
	crystalhd_decoder_tracking_reset(ctx);
	if (!ctx->hw_ctx)
		return;

	if (retire_session_resources) {
		ctx->cin_wait_exit = 1;
		/* Stop capture in case flush was not called before session release. */
		ctx->pwr_state_change = BC_HW_RUNNING;
	}
	disable_irq(ctx->adp->pdev->irq);
	if (retire_session_resources) {
		crystalhd_hw_stop_capture(ctx->hw_ctx, true);
		crystalhd_hw_free_dma_rings(ctx->hw_ctx);
		crystalhd_destroy_dio_pool(ctx->adp);
		crystalhd_delete_elem_pool(ctx->adp);
	}
	ctx->state = BC_LINK_INVALID;
	crystalhd_hw_close(ctx->hw_ctx);
	kfree(ctx->hw_ctx);
	ctx->hw_ctx = NULL;
	enable_irq(ctx->adp->pdev->irq);
}

BC_STATUS crystalhd_session_acquire_locked(struct crystalhd_cmd *ctx,
					   const void *owner)
{
	BC_STATUS sts;
	bool opened_context;

	if (!ctx || !ctx->adp || !owner)
		return BC_STS_INV_ARG;
	if (ctx->session_owner)
		return BC_STS_BUSY;
	if (ctx->state != BC_LINK_INVALID)
		return BC_STS_ERR_USAGE;

	opened_context = !ctx->hw_ctx;
	sts = crystalhd_ensure_hw_context(ctx);
	if (sts != BC_STS_SUCCESS)
		return sts;

	sts = crystalhd_session_setup(ctx);
	if (sts != BC_STS_SUCCESS) {
		/* A standalone frontend has no legacy file close to retire a context
		 * opened by this attempt. The setup helper already unwound pools and
		 * rings; retain a context supplied by an existing file lifetime.
		 */
		if (opened_context)
			crystalhd_retire_hw_context(ctx, false);
		return sts;
	}

	/* Publish ownership only after the complete resource set exists. */
	crystalhd_decoder_tracking_reset(ctx);
	ctx->session_owner = owner;
	ctx->cin_wait_exit = 0;
	return BC_STS_SUCCESS;
}

BC_STATUS crystalhd_session_release_locked(struct crystalhd_cmd *ctx,
					   const void *owner)
{
	if (!ctx || !owner)
		return BC_STS_INV_ARG;
	if (ctx->session_owner != owner)
		return BC_STS_ERR_USAGE;

	crystalhd_retire_hw_context(ctx, true);
	ctx->session_owner = NULL;
	return BC_STS_SUCCESS;
}

static BC_STATUS crystalhd_session_require_owner(struct crystalhd_cmd *ctx,
						 const void *owner)
{
	if (!ctx || !ctx->adp || !owner)
		return BC_STS_INV_ARG;
	lockdep_assert_held(&ctx->adp->user_lock);
	if (!ctx->session_owner)
		return BC_STS_ERR_USAGE;
	if (ctx->session_owner != owner)
		return BC_STS_ERR_USAGE;

	return BC_STS_SUCCESS;
}

/* Caller excludes PCI removal and holds user_lock exclusively on a present device. */
BC_STATUS crystalhd_user_set_mode(struct crystalhd_cmd *ctx,
				 struct crystalhd_user *uc, uint32_t mode)
{
	BC_STATUS sts;
	int i;

	if (!ctx || !uc)
		return BC_STS_INV_ARG;

	if (uc->mode != DTS_MODE_INV || ctx->session_owner == uc) {
		dev_err(chddev(), "Close the handle first..\n");
		return BC_STS_ERR_USAGE;
	}

	if ((mode & 0xFF) == DTS_MONITOR_MODE) {
		uc->mode = mode;
		return BC_STS_SUCCESS;
	}

	if (ctx->state != BC_LINK_INVALID) {
		dev_err(chddev(), "Link invalid state notify mode %x \n", ctx->state);
		return BC_STS_ERR_USAGE;
	}

	/* Check for duplicate playback sessions..*/
	for (i = 0; i < BC_LINK_MAX_OPENS; i++) {
		if ((ctx->user[i].mode & 0xFF) == DTS_DIAG_MODE ||
		    (ctx->user[i].mode & 0xFF) == DTS_PLAYBACK_MODE) {
			dev_err(chddev(), "multiple playback sessions are not "
				"supported..\n");
			return BC_STS_ERR_USAGE;
		}
	}

	if (ctx->session_owner) {
		dev_err(chddev(), "Decoder session is already owned\n");
		return BC_STS_ERR_USAGE;
	}

	/* A legacy file retains its opened hardware context after setup failure
	 * so the same handle can retry and its normal close can retire it.
	 */
	sts = crystalhd_ensure_hw_context(ctx);
	if (sts != BC_STS_SUCCESS)
		return sts;

	sts = crystalhd_session_acquire_locked(ctx, uc);
	if (sts != BC_STS_SUCCESS)
		return sts;

	uc->mode = mode;
	return BC_STS_SUCCESS;
}

static BC_STATUS bc_cproc_notify_mode(struct crystalhd_cmd *ctx,
				    crystalhd_ioctl_data *idata)
{
	if (!ctx || !idata) {
		dev_err(chddev(), "%s: Invalid Arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	return crystalhd_user_set_mode(ctx, &ctx->user[idata->u_id],
				       idata->udata.u.NotifyMode.Mode);
}

static BC_STATUS bc_cproc_get_version(struct crystalhd_cmd *ctx,
				      crystalhd_ioctl_data *idata)
{
	if (!ctx || !idata) {
		dev_err(chddev(), "%s: Invalid Arg\n", __func__);
		return BC_STS_INV_ARG;
	}
	idata->udata.u.VerInfo.DriverMajor = crystalhd_kmod_major;
	idata->udata.u.VerInfo.DriverMinor = crystalhd_kmod_minor;
	idata->udata.u.VerInfo.DriverRevision	= crystalhd_kmod_rev;
	return BC_STS_SUCCESS;
}


static BC_STATUS bc_cproc_get_hwtype(struct crystalhd_cmd *ctx, crystalhd_ioctl_data *idata)
{
	if (!ctx || !idata) {
		dev_err(chddev(), "%s: Invalid Arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	crystalhd_pci_cfg_rd(ctx->adp, 0, 2,
			   (uint32_t *)&idata->udata.u.hwType.PciVenId);
	crystalhd_pci_cfg_rd(ctx->adp, 2, 2,
			   (uint32_t *)&idata->udata.u.hwType.PciDevId);
	crystalhd_pci_cfg_rd(ctx->adp, 8, 1,
			   (uint32_t *)&idata->udata.u.hwType.HwRev);

	return BC_STS_SUCCESS;
}

static BC_STATUS bc_cproc_reg_rd(struct crystalhd_cmd *ctx,
				 crystalhd_ioctl_data *idata)
{
	if (!ctx || !ctx->hw_ctx || !idata)
		return BC_STS_INV_ARG;
	idata->udata.u.regAcc.Value = ctx->hw_ctx->pfnReadDevRegister(ctx->adp,
					idata->udata.u.regAcc.Offset);
	return BC_STS_SUCCESS;
}

static BC_STATUS bc_cproc_reg_wr(struct crystalhd_cmd *ctx,
				 crystalhd_ioctl_data *idata)
{
	if (!ctx || !ctx->hw_ctx || !idata)
		return BC_STS_INV_ARG;

	ctx->hw_ctx->pfnWriteDevRegister(ctx->adp, idata->udata.u.regAcc.Offset,
		      idata->udata.u.regAcc.Value);

	return BC_STS_SUCCESS;
}

static BC_STATUS bc_cproc_link_reg_rd(struct crystalhd_cmd *ctx,
				      crystalhd_ioctl_data *idata)
{
	if (!ctx || !ctx->hw_ctx || !idata)
		return BC_STS_INV_ARG;

	idata->udata.u.regAcc.Value = ctx->hw_ctx->pfnReadFPGARegister(ctx->adp,
					idata->udata.u.regAcc.Offset);
	return BC_STS_SUCCESS;
}

static BC_STATUS bc_cproc_link_reg_wr(struct crystalhd_cmd *ctx,
				      crystalhd_ioctl_data *idata)
{
	if (!ctx || !ctx->hw_ctx || !idata)
		return BC_STS_INV_ARG;

	ctx->hw_ctx->pfnWriteFPGARegister(ctx->adp, idata->udata.u.regAcc.Offset,
		       idata->udata.u.regAcc.Value);

	return BC_STS_SUCCESS;
}

static BC_STATUS bc_cproc_mem_rd(struct crystalhd_cmd *ctx,
				 crystalhd_ioctl_data *idata)
{
	BC_STATUS sts = BC_STS_SUCCESS;

	if (!ctx || !ctx->hw_ctx || !idata || !idata->add_cdata)
		return BC_STS_INV_ARG;

	if (idata->udata.u.devMem.NumDwords > (idata->add_cdata_sz / 4)) {
		dev_err(chddev(), "insufficient buffer\n");
		return BC_STS_INV_ARG;
	}
	sts = ctx->hw_ctx->pfnDevDRAMRead(ctx->hw_ctx, idata->udata.u.devMem.StartOff,
			     idata->udata.u.devMem.NumDwords,
			     (uint32_t *)idata->add_cdata);
	return sts;

}

static BC_STATUS bc_cproc_mem_wr(struct crystalhd_cmd *ctx,
				 crystalhd_ioctl_data *idata)
{
	BC_STATUS sts = BC_STS_SUCCESS;

	if (!ctx || !ctx->hw_ctx || !idata || !idata->add_cdata)
		return BC_STS_INV_ARG;

	if (idata->udata.u.devMem.NumDwords > (idata->add_cdata_sz / 4)) {
		dev_err(chddev(), "insufficient buffer\n");
		return BC_STS_INV_ARG;
	}

	sts = ctx->hw_ctx->pfnDevDRAMWrite(ctx->hw_ctx, idata->udata.u.devMem.StartOff,
			     idata->udata.u.devMem.NumDwords,
			     (uint32_t *)idata->add_cdata);
	return sts;
}

static BC_STATUS bc_cproc_cfg_rd(struct crystalhd_cmd *ctx,
				 crystalhd_ioctl_data *idata)
{
	uint32_t ix, cnt, off, len;
	BC_STATUS sts = BC_STS_SUCCESS;
	uint32_t *temp;

	if (!ctx || !idata)
		return BC_STS_INV_ARG;
	if (!crystalhd_valid_pci_cfg(idata->udata.u.pciCfg.Size,
				     idata->udata.u.pciCfg.Offset))
		return BC_STS_INV_ARG;

	temp = (uint32_t *) idata->udata.u.pciCfg.pci_cfg_space;
	off = idata->udata.u.pciCfg.Offset;
	len = idata->udata.u.pciCfg.Size;

	if (len <= 4) {
		sts = crystalhd_pci_cfg_rd(ctx->adp, off, len, temp);
		return sts;
	}

	/* Truncate to dword alignment..*/
	len = 4;
	cnt = idata->udata.u.pciCfg.Size / len;
	for (ix = 0; ix < cnt; ix++) {
		sts = crystalhd_pci_cfg_rd(ctx->adp, off, len, &temp[ix]);
		if (sts != BC_STS_SUCCESS) {
			dev_err(chddev(), "config read : %d\n", sts);
			return sts;
		}
		off += len;
	}

	return sts;
}

static BC_STATUS bc_cproc_cfg_wr(struct crystalhd_cmd *ctx,
				 crystalhd_ioctl_data *idata)
{
	uint32_t ix, cnt, off, len;
	BC_STATUS sts = BC_STS_SUCCESS;
	uint32_t *temp;

	if (!ctx || !idata)
		return BC_STS_INV_ARG;
	if (!crystalhd_valid_pci_cfg(idata->udata.u.pciCfg.Size,
				     idata->udata.u.pciCfg.Offset))
		return BC_STS_INV_ARG;

	temp = (uint32_t *) idata->udata.u.pciCfg.pci_cfg_space;
	off = idata->udata.u.pciCfg.Offset;
	len = idata->udata.u.pciCfg.Size;

	if (len <= 4)
		return crystalhd_pci_cfg_wr(ctx->adp, off, len, temp[0]);

	/* Truncate to dword alignment..*/
	len = 4;
	cnt = idata->udata.u.pciCfg.Size / len;
	for (ix = 0; ix < cnt; ix++) {
		sts = crystalhd_pci_cfg_wr(ctx->adp, off, len, temp[ix]);
		if (sts != BC_STS_SUCCESS) {
			dev_err(chddev(), "config write : %d\n", sts);
			return sts;
		}
		off += len;
	}

	return sts;
}

BC_STATUS crystalhd_fw_download_locked(struct crystalhd_cmd *ctx,
				       const void *owner,
				       const uint8_t *image, size_t size)
{
	BC_STATUS sts = BC_STS_SUCCESS;
	uint32_t minimum;

	if (!ctx || !ctx->adp || !ctx->adp->pdev || !ctx->hw_ctx ||
	    !ctx->hw_ctx->pfnFWDwnld || !owner || !image || !size)
		return BC_STS_INV_ARG;

	sts = crystalhd_session_require_owner(ctx, owner);
	if (sts != BC_STS_SUCCESS)
		return sts;

	switch (ctx->adp->pdev->device) {
	case BC_PCI_DEVID_FLEA:
		minimum = CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE;
		break;
	case BC_PCI_DEVID_LINK:
		minimum = CRYSTALHD_LINK_MIN_FIRMWARE_SIZE;
		break;
	default:
		return BC_STS_INV_ARG;
	}
	if (size > CRYSTALHD_MAX_FIRMWARE_SIZE ||
	    !crystalhd_valid_firmware_image(image, (uint32_t)size, minimum))
		return BC_STS_INV_ARG;

	dev_dbg(chddev(), "Downloading FW\n");
	/* A verified firmware download is the recovery path for a quarantined
	 * mailbox, so it takes transaction serialization without normal admission.
	 */
	sts = crystalhd_hw_fw_cmd_recovery_enter(ctx->hw_ctx);
	if (sts != BC_STS_SUCCESS)
		return sts;

	if (ctx->state != BC_LINK_INVALID && ctx->state != BC_LINK_RESUME &&
	    !(ctx->state == BC_LINK_INIT &&
	      ctx->decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED)) {
		dev_dbg(chddev(), "Link invalid state download fw %x \n", ctx->state);
		sts = BC_STS_ERR_USAGE;
		goto done;
	}

	sts = ctx->hw_ctx->pfnFWDwnld(ctx->hw_ctx, image, (uint32_t)size);

	if (sts != BC_STS_SUCCESS) {
		dev_info(chddev(), "Firmware Download Failure!! - %d\n", sts);
	} else {
		ctx->state |= BC_LINK_INIT;
		crystalhd_decoder_tracking_reset(ctx);
		/* A successful image download is the verified firmware reset that
		 * reconciles any earlier timed-out mailbox command.
		 */
		crystalhd_hw_fw_cmd_reset_locked(ctx->hw_ctx);
	}

	ctx->pwr_state_change = BC_HW_RUNNING;

done:
	crystalhd_hw_fw_cmd_leave(ctx->hw_ctx);
	return sts;
}

static int crystalhd_fw_status_to_errno(BC_STATUS sts)
{
	switch (sts) {
	case BC_STS_SUCCESS:
		return 0;
	case BC_STS_INV_ARG:
		return -EINVAL;
	case BC_STS_BUSY:
		return -EBUSY;
	case BC_STS_INSUFF_RES:
		return -ENOMEM;
	case BC_STS_NO_ACCESS:
		return -EACCES;
	case BC_STS_TIMEOUT:
		return -ETIMEDOUT;
	case BC_STS_IO_USER_ABORT:
		return -ERESTARTSYS;
	case BC_STS_FW_AUTH_FAILED:
	case BC_STS_CERT_VERIFY_ERROR:
		return -EKEYREJECTED;
	case BC_STS_PWR_MGMT:
		return -EAGAIN;
	default:
		return -EIO;
	}
}

int crystalhd_request_firmware_locked(struct crystalhd_cmd *ctx,
				      const void *owner)
{
	const struct firmware *firmware;
	const char *name;
	BC_STATUS sts;
	int rc;

	if (!ctx || !owner)
		return -EINVAL;
	if (!ctx->adp || !ctx->adp->pdev || !ctx->hw_ctx ||
	    !ctx->hw_ctx->pfnFWDwnld)
		return -ENODEV;

	lockdep_assert_held_write(&ctx->adp->user_lock);
	if (!READ_ONCE(ctx->adp->present))
		return -ENODEV;
	if (!ctx->session_owner)
		return -EINVAL;
	if (ctx->session_owner != owner)
		return -EBUSY;
	if (ctx->state & BC_LINK_SUSPEND)
		return -EAGAIN;
	if (ctx->state != BC_LINK_INVALID && ctx->state != BC_LINK_RESUME &&
	    !(ctx->state == BC_LINK_INIT &&
	      ctx->decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED))
		return -EBUSY;

	switch (ctx->adp->pdev->device) {
	case BC_PCI_DEVID_FLEA:
		name = CRYSTALHD_FLEA_FIRMWARE_NAME;
		break;
	case BC_PCI_DEVID_LINK:
		name = CRYSTALHD_LINK_FIRMWARE_NAME;
		break;
	default:
		return -ENODEV;
	}

	rc = request_firmware(&firmware, name, &ctx->adp->pdev->dev);
	if (rc)
		return rc;
	if (!READ_ONCE(ctx->adp->present)) {
		rc = -ENODEV;
		goto release;
	}

	sts = crystalhd_fw_download_locked(ctx, owner, firmware->data,
					    firmware->size);
	rc = crystalhd_fw_status_to_errno(sts);

release:
	release_firmware(firmware);
	return rc;
}

static BC_STATUS bc_cproc_download_fw(struct crystalhd_cmd *ctx,
				      crystalhd_ioctl_data *idata)
{
	if (!ctx || !idata || !idata->add_cdata || !idata->add_cdata_sz ||
	    idata->u_id >= BC_LINK_MAX_OPENS) {
		dev_err(chddev(), "%s: Invalid Arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	return crystalhd_fw_download_locked(ctx, &ctx->user[idata->u_id],
					    idata->add_cdata,
					    idata->add_cdata_sz);
}

/*
 * We use the FW_CMD interface to sync up playback state with application
 * and  firmware. This function will perform the required pre and post
 * processing of the Firmware commands.
 *
 * Pause -
 *	Disable capture after decoder pause.
 * Resume -
 *	First enable capture and issue decoder resume command.
 * Flush -
 *	Abort pending input transfers and issue decoder flush command.
 *
 */
BC_STATUS crystalhd_fw_exec_locked(struct crystalhd_cmd *ctx,
				   const void *owner, BC_FW_CMD *fw_cmd)
{
	struct device *dev;
	BC_STATUS rollback_sts, sts;
	uint32_t *cmd;
	bool resume_prepared = false, was_paused = false;

	if (!ctx || !ctx->adp || !ctx->adp->pdev || !ctx->hw_ctx ||
	    !ctx->hw_ctx->pfnDoFirmwareCmd || !owner || !fw_cmd)
		return BC_STS_INV_ARG;
	sts = crystalhd_session_require_owner(ctx, owner);
	if (sts != BC_STS_SUCCESS)
		return sts;
	dev = &ctx->adp->pdev->dev;

	sts = crystalhd_hw_fw_cmd_enter(ctx->hw_ctx);
	if (sts != BC_STS_SUCCESS)
		return sts;

	if (!(ctx->state & BC_LINK_INIT)) {
		dev_dbg(dev, "Link invalid state do fw cmd %x \n", ctx->state);
		sts = BC_STS_ERR_USAGE;
		goto done;
	}

	cmd = fw_cmd->cmd;

	/* Pre-Process */
	if (cmd[0] == eCMD_C011_DEC_CHAN_PAUSE) {
		if (!cmd[3]) {
			was_paused = (ctx->state & BC_LINK_PAUSED) != 0;
			if (down_interruptible(&ctx->hw_ctx->fetch_sem)) {
				sts = BC_STS_IO_USER_ABORT;
				goto done;
			}
			sts = ctx->hw_ctx->pfnIssuePause(ctx->hw_ctx, false);
			/* Link has already cleared its pause mailbox when capture has no
			 * queued RX buffer. Match the other start-capture call sites and let
			 * firmware resume complete; NO_DATA is not a transition failure.
			 */
			if (sts == BC_STS_NO_DATA)
				sts = BC_STS_SUCCESS;
			if (sts == BC_STS_SUCCESS) {
				ctx->state &= ~BC_LINK_PAUSED;
				resume_prepared = was_paused;
			} else if (was_paused) {
				ctx->state |= BC_LINK_PAUSED;
				ctx->hw_ctx->pfnIssuePause(ctx->hw_ctx, true);
			} else {
				ctx->state &= ~BC_LINK_PAUSED;
			}
			up(&ctx->hw_ctx->fetch_sem);
			if (sts != BC_STS_SUCCESS)
				goto done;
		}
	} else if (cmd[0] == eCMD_C011_DEC_CHAN_FLUSH) {
		dev_dbg(dev, "Flush issued\n");
		if (cmd[3])
			ctx->cin_wait_exit = 1;
	}

	sts = ctx->hw_ctx->pfnDoFirmwareCmd(ctx->hw_ctx, fw_cmd);

	if (sts != BC_STS_SUCCESS) {
		if (resume_prepared) {
			/* Local capture resumes before firmware. Restore the previous
			 * fail-closed state when firmware rejects or times out.
			 */
			down(&ctx->hw_ctx->fetch_sem);
			ctx->state |= BC_LINK_PAUSED;
			rollback_sts = ctx->hw_ctx->pfnIssuePause(ctx->hw_ctx, true);
			up(&ctx->hw_ctx->fetch_sem);
			if (rollback_sts != BC_STS_SUCCESS)
				dev_err(dev, "failed to restore capture pause: %d\n",
					rollback_sts);
		}
		dev_dbg(dev, "fw cmd %x failed\n", cmd[0]);
		goto done;
	}

	/* Post-Process */
	if (cmd[0] == eCMD_C011_DEC_CHAN_PAUSE) {
		if (cmd[3]) {
			/* Firmware has already accepted pause; finish the matching
			 * capture transition even if the caller receives a signal.
			 */
			down(&ctx->hw_ctx->fetch_sem);
			ctx->state |= BC_LINK_PAUSED;
			ctx->hw_ctx->pfnIssuePause(ctx->hw_ctx, true);
			up(&ctx->hw_ctx->fetch_sem);
		}
	}

done:
	crystalhd_hw_fw_cmd_leave(ctx->hw_ctx);
	return sts;
}

int crystalhd_fw_bootstrap_locked(struct crystalhd_cmd *ctx,
				  const void *owner)
{
	struct crystalhd_fw_init_cmd *init;
	BC_FW_CMD fw_cmd = { };
	uint32_t initial_power_state, initial_state;
	BC_STATUS sts;
	bool firmware_loaded = false;
	int rc;

	if (!ctx || !owner)
		return -EINVAL;
	if (!ctx->adp || !ctx->adp->pdev || !ctx->hw_ctx)
		return -ENODEV;

	lockdep_assert_held_write(&ctx->adp->user_lock);
	initial_state = ctx->state;
	initial_power_state = ctx->pwr_state_change;

	rc = crystalhd_request_firmware_locked(ctx, owner);
	if (rc)
		goto rollback;
	firmware_loaded = true;
	if (!READ_ONCE(ctx->adp->present)) {
		rc = -ENODEV;
		goto rollback;
	}

	BUILD_BUG_ON(sizeof(*init) !=
		     CRYSTALHD_FW_INIT_WORDS * sizeof(uint32_t));
	BUILD_BUG_ON(sizeof(*init) > sizeof(fw_cmd.cmd));
	init = (struct crystalhd_fw_init_cmd *)fw_cmd.cmd;
	init->command = eCMD_C011_INIT;
	init->sequence = ++ctx->fw_sequence;
	init->mem_size_mb = CRYSTALHD_FW_INIT_MEM_SIZE_MB;
	init->input_clk_hz = CRYSTALHD_FW_INIT_INPUT_CLK_HZ;
	init->uart_baud_rate = CRYSTALHD_FW_INIT_UART_BAUD;
	init->init_arcs = CRYSTALHD_FW_INIT_STREAM_ARC |
			  CRYSTALHD_FW_INIT_VDEC_ARC;
	init->interrupt = CRYSTALHD_FW_INIT_INT_ENABLE;
	init->brcm_mode = CRYSTALHD_FW_INIT_BRCM_ECG_MODE;
	init->fgt_enable = CRYSTALHD_FW_INIT_FGT_ENABLE;
	if (ctx->adp->pdev->device == BC_PCI_DEVID_LINK)
		init->rsa_decrypt = CRYSTALHD_FW_INIT_RSA_DECRYPT;

	sts = crystalhd_fw_exec_locked(ctx, owner, &fw_cmd);
	rc = crystalhd_fw_status_to_errno(sts);
	if (rc)
		goto rollback;
	if (!READ_ONCE(ctx->adp->present)) {
		rc = -ENODEV;
		goto rollback;
	}

	/* RESUME is only an admission state for reloading firmware. */
	ctx->state = BC_LINK_INIT;
	ctx->pwr_state_change = BC_HW_RUNNING;
	ctx->decoder_phase = CRYSTALHD_DECODER_BOOTSTRAPPED;
	ctx->decoder_channel_id = 0;
	return 0;

rollback:
	/* A timed-out command may still complete later. Force every retry
	 * through a fresh download, which is the mailbox recovery boundary.
	 */
	ctx->state = initial_state;
	ctx->pwr_state_change = initial_power_state;
	if (firmware_loaded) {
		ctx->decoder_phase = CRYSTALHD_DECODER_RECOVERY_REQUIRED;
		ctx->decoder_channel_id = 0;
	}
	return rc;
}

int crystalhd_decoder_channel_open_locked(
	struct crystalhd_cmd *ctx, const void *owner,
	const struct crystalhd_decoder_config *config)
{
	struct crystalhd_fw_channel_open_cmd *open;
	struct crystalhd_fw_input_params_cmd *input;
	BC_FW_CMD fw_cmd = { };
	uint32_t channel_id, video_algorithm;
	BC_STATUS sts;
	int rc;

	if (!ctx || !owner || !config)
		return -EINVAL;
	if (!ctx->adp || !ctx->adp->pdev || !ctx->hw_ctx ||
	    !ctx->hw_ctx->pfnDoFirmwareCmd)
		return -ENODEV;

	lockdep_assert_held_write(&ctx->adp->user_lock);
	if (!READ_ONCE(ctx->adp->present))
		return -ENODEV;
	if (!ctx->session_owner)
		return -EINVAL;
	if (ctx->session_owner != owner)
		return -EBUSY;
	if (ctx->adp->pdev->device != BC_PCI_DEVID_FLEA)
		return -EOPNOTSUPP;
	if (ctx->state != BC_LINK_INIT ||
	    ctx->decoder_phase != CRYSTALHD_DECODER_BOOTSTRAPPED ||
	    !ctx->fw_sequence)
		return -EBUSY;

	switch (config->codec) {
	case CRYSTALHD_DECODER_CODEC_H264:
		video_algorithm = CRYSTALHD_FW_VIDEO_ALGORITHM_H264;
		break;
	default:
		return -EINVAL;
	}

	BUILD_BUG_ON(sizeof(*open) !=
		     CRYSTALHD_FW_CHANNEL_OPEN_WORDS * sizeof(uint32_t));
	BUILD_BUG_ON(sizeof(*open) > sizeof(fw_cmd.cmd));
	BUILD_BUG_ON(offsetof(struct crystalhd_fw_channel_open_cmd,
			      stream_type) != 16U);
	BUILD_BUG_ON(offsetof(struct crystalhd_fw_channel_open_cmd,
			      video_algorithm) != 36U);
	BUILD_BUG_ON(offsetof(struct crystalhd_fw_channel_open_cmd,
			      picture_info_interrupt_enable) != 144U);
	BUILD_BUG_ON(sizeof(struct DecRspChannelChannelOpen) !=
		     13U * sizeof(uint32_t));
	BUILD_BUG_ON(offsetof(struct DecRspChannelChannelOpen, ChannelID) != 12U);
	BUILD_BUG_ON(offsetof(struct DecRspChannelChannelOpen,
			      transportStreamCaptureAddr) != 44U);
	open = (struct crystalhd_fw_channel_open_cmd *)fw_cmd.cmd;
	open->command = eCMD_C011_DEC_CHAN_OPEN;
	open->sequence = ++ctx->fw_sequence;
	open->stream_type = CRYSTALHD_FW_STREAM_TYPE_PES;
	open->video_algorithm = video_algorithm;

	sts = crystalhd_fw_exec_locked(ctx, owner, &fw_cmd);
	rc = crystalhd_fw_status_to_errno(sts);
	if (rc) {
		/* A firmware rejection is the one result known not to have opened a
		 * channel. Transport failures have an ambiguous device-side result.
		 */
		if (sts != BC_STS_FW_CMD_ERR)
			ctx->decoder_phase =
				CRYSTALHD_DECODER_RECOVERY_REQUIRED;
		ctx->decoder_channel_id = 0;
		return rc;
	}
	if (!READ_ONCE(ctx->adp->present)) {
		ctx->decoder_phase = CRYSTALHD_DECODER_RECOVERY_REQUIRED;
		ctx->decoder_channel_id = 0;
		return -ENODEV;
	}

	channel_id = fw_cmd.rsp[3];
	if (channel_id != 0) {
		ctx->decoder_phase = CRYSTALHD_DECODER_RECOVERY_REQUIRED;
		ctx->decoder_channel_id = 0;
		return -EIO;
	}

	memset(&fw_cmd, 0, sizeof(fw_cmd));
	BUILD_BUG_ON(sizeof(*input) !=
		     CRYSTALHD_FW_INPUT_PARAMS_WORDS * sizeof(uint32_t));
	BUILD_BUG_ON(sizeof(*input) > sizeof(fw_cmd.cmd));
	BUILD_BUG_ON(offsetof(struct crystalhd_fw_input_params_cmd,
			      channel_id) != 8U);
	BUILD_BUG_ON(offsetof(struct crystalhd_fw_input_params_cmd,
			      sync_mode) != 12U);
	BUILD_BUG_ON(offsetof(struct crystalhd_fw_input_params_cmd,
			      disable_pcr_offset) != 32U);
	input = (struct crystalhd_fw_input_params_cmd *)fw_cmd.cmd;
	input->command = eCMD_C011_DEC_CHAN_INPUT_PARAMS;
	input->sequence = ++ctx->fw_sequence;
	input->channel_id = channel_id;
	input->sync_mode = CRYSTALHD_FW_SYNC_MODE_SYNCPIN;

	sts = crystalhd_fw_exec_locked(ctx, owner, &fw_cmd);
	rc = crystalhd_fw_status_to_errno(sts);
	if (rc || !READ_ONCE(ctx->adp->present)) {
		ctx->decoder_phase = CRYSTALHD_DECODER_RECOVERY_REQUIRED;
		ctx->decoder_channel_id = 0;
		return rc ? rc : -ENODEV;
	}

	ctx->decoder_channel_id = channel_id;
	ctx->decoder_phase = CRYSTALHD_DECODER_CHANNEL_CONFIGURED;
	return 0;
}

static BC_STATUS bc_cproc_do_fw_cmd(struct crystalhd_cmd *ctx,
				     crystalhd_ioctl_data *idata)
{
	if (!ctx || !idata || idata->u_id >= BC_LINK_MAX_OPENS)
		return BC_STS_INV_ARG;

	return crystalhd_fw_exec_locked(ctx, &ctx->user[idata->u_id],
					&idata->udata.u.fwCmd);
}

struct crystalhd_tx_completion {
	wait_queue_head_t event;
	BC_STATUS status;
	bool done;
};

static void bc_proc_in_completion(void *context, BC_STATUS sts)
{
	struct crystalhd_tx_completion *completion = context;

	if (!completion) {
		dev_err(chddev(), "%s: Invalid Arg\n", __func__);
		return;
	}

	WRITE_ONCE(completion->status, sts);
	WRITE_ONCE(completion->done, true);
	crystalhd_set_event(&completion->event);
}

static BC_STATUS bc_cproc_codein_sleep(struct crystalhd_cmd *ctx)
{
	wait_queue_head_t sleep_ev;
	int rc = 0;

	/* Removal must cancel every waiter, even after another thread consumes
	 * the one-shot flush/close cancellation flag.
	 */
	if (!READ_ONCE(ctx->adp->present))
		return BC_STS_CMD_CANCELLED;
	if (ctx->state & BC_LINK_SUSPEND)
		return BC_STS_PWR_MGMT;

	if (ctx->cin_wait_exit) {
		ctx->cin_wait_exit = 0;
		return BC_STS_CMD_CANCELLED;
	}
	crystalhd_create_event(&sleep_ev);
	crystalhd_wait_on_event(&sleep_ev, 0, 100, rc, false);
	if (rc == -EINTR)
		return BC_STS_IO_USER_ABORT;

	return BC_STS_SUCCESS;
}

/*
 * Synchronous mapped-input transfer. The caller keeps the command/device
 * lifetime and TX serialization locks, and owns a fresh mapped request until
 * this function returns.
 */
BC_STATUS crystalhd_tx_transfer_sync(struct crystalhd_cmd *ctx,
				     struct crystalhd_dio_req *dio,
				     uint8_t data_flags)
{
	struct device *dev = chddev();
	struct crystalhd_tx_completion completion = {
		.status = BC_STS_SUCCESS,
	};
	uint32_t tx_listid = 0;
	BC_STATUS sts = BC_STS_SUCCESS;
	int rc = 0;

	if (!ctx || !ctx->hw_ctx || !dio) {
		dev_err(dev, "%s: Invalid Arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	crystalhd_create_event(&completion.event);

	ctx->tx_list_id = 0;
	/* msleep_interruptible(2000); */
	sts = crystalhd_hw_post_tx(ctx->hw_ctx, dio, bc_proc_in_completion,
				 &completion, &tx_listid, data_flags);

	while (sts == BC_STS_BUSY) {
		sts = bc_cproc_codein_sleep(ctx);
		if (sts != BC_STS_SUCCESS)
			break;
		sts = crystalhd_hw_post_tx(ctx->hw_ctx, dio,
					 bc_proc_in_completion,
					 &completion, &tx_listid, data_flags);
	}
	if (sts != BC_STS_SUCCESS) {
		dev_dbg(dev, "_hw_txdma returning sts:%d\n", sts);
		return sts;
	}
	if (ctx->cin_wait_exit)
		ctx->cin_wait_exit = 0;

	ctx->tx_list_id = tx_listid;

	dev_dbg(dev, "Sending TX\n");

	/* _post() succeeded.. wait for the completion. */
	crystalhd_wait_on_event(&completion.event,
				READ_ONCE(completion.done), 3000, rc, false);
	ctx->tx_list_id = 0;
	if (!rc) {
		/* The wakeup occurs inside the ISR callback. Wait until it has
		 * stopped using both the request and this stack's completion cookie.
		 */
		synchronize_irq(ctx->adp->pdev->irq);
		return READ_ONCE(completion.status);
	} else if (rc == -EBUSY) {
		dev_dbg(dev, "_tx_post() T/O \n");
		sts = BC_STS_TIMEOUT;
	} else if (rc == -EINTR) {
		dev_dbg(dev, "Tx Wait Signal int.\n");
		sts = BC_STS_IO_USER_ABORT;
	} else {
		sts = BC_STS_IO_ERROR;
	}

	/* We are cancelling the IO from the same context as the _post().
	 * so no need to wait on the event again.. the return itself
	 * ensures the release of our resources.
	 */
	crystalhd_hw_cancel_all_tx(ctx->hw_ctx);

	return sts;
}

/* Helper function to check on user buffers */
static BC_STATUS bc_cproc_check_inbuffs(bool pin, void *ubuff, uint32_t ub_sz,
					uint32_t uv_off, bool en_422)
{
	struct device *dev = chddev();
	if (!ubuff || !ub_sz) {
		dev_err(dev, "%s->Invalid Arg %p %x\n",
			((pin) ? "TX" : "RX"), ubuff, ub_sz);
		return BC_STS_INV_ARG;
	}

	/* Check for alignment */
	if (((uintptr_t)ubuff) & 0x03) {
		dev_err(dev, "%s-->Un-aligned address not implemented yet.. %p \n",
				((pin) ? "TX" : "RX"), ubuff);
		return BC_STS_NOT_IMPL;
	}
	if (pin)
		return BC_STS_SUCCESS;

	if (!en_422 && !uv_off) {
		dev_err(dev, "Need UV offset for 420 mode.\n");
		return BC_STS_INV_ARG;
	}

	if (en_422 && uv_off) {
		dev_err(dev, "UV offset in 422 mode ??\n");
		return BC_STS_INV_ARG;
	}

	return BC_STS_SUCCESS;
}

static BC_STATUS bc_cproc_proc_input(struct crystalhd_cmd *ctx, crystalhd_ioctl_data *idata)
{
	struct device *dev = chddev();
	void *ubuff;
	uint32_t ub_sz;
	struct crystalhd_dio_req *dio_hnd = NULL;
	BC_STATUS sts = BC_STS_SUCCESS;

	if (!ctx || !idata) {
		dev_err(dev, "%s: Invalid Arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	if (ctx->state & BC_LINK_SUSPEND) {
		dev_err(dev, "proc_input: Link Suspended\n");
		return BC_STS_PWR_MGMT;
	}

	ubuff = idata->udata.u.ProcInput.pDmaBuff;
	ub_sz = idata->udata.u.ProcInput.BuffSz;

	sts = bc_cproc_check_inbuffs(1, ubuff, ub_sz, 0, 0);
	if (sts != BC_STS_SUCCESS)
		return sts;

	sts = crystalhd_map_dio(ctx->adp, ubuff, ub_sz, 0, MODE420, true,
			      &dio_hnd);
	if (sts != BC_STS_SUCCESS) {
		dev_err(dev, "dio map - %d \n", sts);
		return sts;
	}

	if (!dio_hnd)
		return BC_STS_ERROR;

	sts = crystalhd_tx_transfer_sync(ctx, dio_hnd,
					 idata->udata.u.ProcInput.Encrypted);

	crystalhd_unmap_dio(ctx->adp, dio_hnd);

	return sts;
}

/*
 * Transfer ownership of a fresh capture buffer to the RX queues.
 * The caller keeps command/device lifetime protection but not fetch_sem. It
 * keeps buffer ownership on every error; hardware BUSY means the buffer
 * was queued for retry and is therefore reported as successful admission. On
 * success the caller must not inspect or release the buffer again.
 */
BC_STATUS crystalhd_rx_submit(struct crystalhd_cmd *ctx,
			      struct crystalhd_rx_buffer *buffer)
{
	BC_STATUS sts;

	if (!ctx || !ctx->hw_ctx || !buffer)
		return BC_STS_INV_ARG;

	if (down_interruptible(&ctx->hw_ctx->fetch_sem))
		return BC_STS_IO_USER_ABORT;
	sts = crystalhd_hw_add_cap_buffer(ctx->hw_ctx, buffer,
					     ctx->state == BC_LINK_READY);
	up(&ctx->hw_ctx->fetch_sem);

	return sts == BC_STS_BUSY ? BC_STS_SUCCESS : sts;
}

static BC_STATUS bc_cproc_add_cap_buff(struct crystalhd_cmd *ctx,
				       crystalhd_ioctl_data *idata)
{
	struct device *dev = chddev();
	void *ubuff;
	uint32_t ub_sz, uv_off;
	BC_OUTPUT_FORMAT output_format;
	struct crystalhd_dio_req *dio_hnd = NULL;
	BC_STATUS sts = BC_STS_SUCCESS;

	if (!ctx || !ctx->hw_ctx || !idata) {
		dev_err(dev, "%s: Invalid Arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	ubuff = idata->udata.u.RxBuffs.YuvBuff;
	ub_sz = idata->udata.u.RxBuffs.YuvBuffSz;
	uv_off = idata->udata.u.RxBuffs.UVbuffOffset;
	output_format = idata->udata.u.RxBuffs.b422Mode;

	sts = bc_cproc_check_inbuffs(false, ubuff, ub_sz, uv_off,
				     output_format != MODE420);

	if (sts != BC_STS_SUCCESS)
		return sts;

	sts = crystalhd_map_dio(ctx->adp, ubuff, ub_sz, uv_off,
			      output_format, false, &dio_hnd);
	if (sts != BC_STS_SUCCESS) {
		dev_err(dev, "dio map - %d \n", sts);
		return sts;
	}

	if (!dio_hnd)
		return BC_STS_ERROR;

	sts = crystalhd_rx_submit(ctx, &dio_hnd->rx_buffer);
	if (sts != BC_STS_SUCCESS) {
		crystalhd_rx_buffer_release(ctx->adp, &dio_hnd->rx_buffer);
		return sts;
	}

	return BC_STS_SUCCESS;
}

static BC_STATUS bc_cproc_fmt_change(struct crystalhd_cmd *ctx,
				     struct crystalhd_rx_completion *result)
{
	struct crystalhd_rx_buffer *buffer;
	void *cookie;
	BC_STATUS sts = BC_STS_SUCCESS;

	if (!ctx || !ctx->hw_ctx || !result || !result->buffer ||
	    !result->cookie)
		return BC_STS_INV_ARG;

	buffer = result->buffer;
	cookie = result->cookie;
	result->buffer = NULL;
	result->cookie = NULL;
	if (buffer->cookie != cookie) {
		crystalhd_rx_buffer_release(ctx->adp, buffer);
		return BC_STS_INV_ARG;
	}
	if (down_interruptible(&ctx->hw_ctx->fetch_sem)) {
		crystalhd_rx_buffer_release(ctx->adp, buffer);
		return BC_STS_IO_USER_ABORT;
	}
	if (result->capture_epoch != ctx->hw_ctx->rx_cancel_epoch ||
	    !(ctx->state & BC_LINK_CAP_EN)) {
		up(&ctx->hw_ctx->fetch_sem);
		crystalhd_rx_buffer_release(ctx->adp, buffer);
		return BC_STS_IO_USER_ABORT;
	}
	sts = crystalhd_hw_add_cap_buffer(ctx->hw_ctx, buffer, false);
	if (sts != BC_STS_SUCCESS) {
		up(&ctx->hw_ctx->fetch_sem);
		crystalhd_rx_buffer_release(ctx->adp, buffer);
		return sts;
	}

	ctx->state |= BC_LINK_FMT_CHG;
	if (ctx->state == BC_LINK_READY)
		sts = crystalhd_hw_start_capture(ctx->hw_ctx);
	up(&ctx->hw_ctx->fetch_sem);

	return sts == BC_STS_NO_DATA ? BC_STS_SUCCESS : sts;
}

static void bc_cproc_copy_pib(struct C011_PIB *dst,
			      const struct C011_PIB *src)
{
	dst->ppb.picture_number = src->ppb.picture_number;
	dst->ppb.width = src->ppb.width;
	dst->ppb.height = src->ppb.height;
	dst->ppb.chroma_format = src->ppb.chroma_format;
	dst->ppb.pulldown = src->ppb.pulldown;
	dst->ppb.flags = src->ppb.flags;
	dst->ptsStcOffset = src->ptsStcOffset;
	dst->ppb.aspect_ratio = src->ppb.aspect_ratio;
	dst->ppb.colour_primaries = src->ppb.colour_primaries;
	dst->ppb.picture_meta_payload = src->ppb.picture_meta_payload;
	dst->resolution = src->resolution;
}

/*
 * Fetch one completed RX buffer using the legacy blocking timeout. The caller
 * keeps command/device lifetime protection but does not hold fetch_sem.
 * Success transfers result->buffer and its opaque cookie to the caller, which
 * must requeue or release the buffer exactly once.
 */
BC_STATUS crystalhd_rx_dequeue(struct crystalhd_cmd *ctx,
			       struct crystalhd_rx_completion *result)
{
	uint64_t expected_epoch;
	BC_STATUS sts;

	if (!result) {
		dev_err(chddev(), "%s: Invalid Arg\n", __func__);
		return BC_STS_INV_ARG;
	}
	memset(result, 0, sizeof(*result));
	if (!ctx) {
		dev_err(chddev(), "%s: Invalid Arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	if (ctx->state & BC_LINK_SUSPEND)
		return BC_STS_PWR_MGMT;

	if (!(ctx->state & BC_LINK_CAP_EN)) {
		dev_dbg(chddev(), "Capture not enabled..%x\n", ctx->state);
		return BC_STS_ERR_USAGE;
	}
	if (!ctx->hw_ctx) {
		dev_err(chddev(), "%s: Invalid Arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	/* Couple capture admission to the generation passed through the blocking
	 * wait. A full stop in either wait gap then suppresses stale HW wakeups.
	 */
	if (down_interruptible(&ctx->hw_ctx->fetch_sem))
		return BC_STS_IO_USER_ABORT;
	if (ctx->state & BC_LINK_SUSPEND) {
		up(&ctx->hw_ctx->fetch_sem);
		return BC_STS_PWR_MGMT;
	}
	if (!(ctx->state & BC_LINK_CAP_EN)) {
		up(&ctx->hw_ctx->fetch_sem);
		return BC_STS_ERR_USAGE;
	}
	expected_epoch = ctx->hw_ctx->rx_cancel_epoch;
	up(&ctx->hw_ctx->fetch_sem);

	sts = crystalhd_hw_get_cap_buffer(ctx->hw_ctx, result, expected_epoch);
	if (sts != BC_STS_SUCCESS)
		return (ctx->state & BC_LINK_SUSPEND) ? BC_STS_PWR_MGMT : sts;

	dev_dbg(chddev(), "Got Picture\n");
	return BC_STS_SUCCESS;
}

static BC_STATUS bc_cproc_fetch_frame(struct crystalhd_cmd *ctx,
				      crystalhd_ioctl_data *idata)
{
	struct crystalhd_rx_completion result;
	struct crystalhd_dio_req *dio;
	BC_DEC_OUT_BUFF *frame;
	BC_STATUS sts;

	if (!ctx || !idata) {
		dev_err(chddev(), "%s: Invalid Arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	sts = crystalhd_rx_dequeue(ctx, &result);
	if (sts != BC_STS_SUCCESS)
		return sts;

	frame = &idata->udata.u.DecOutData;
	frame->Flags = result.flags;
	if (result.flags & COMP_FLAG_PIB_VALID)
		bc_cproc_copy_pib(&frame->PibInfo, &result.pib);

	if (result.flags & COMP_FLAG_FMT_CHANGE)
		return bc_cproc_fmt_change(ctx, &result);

	dio = crystalhd_dio_from_rx_buffer(result.buffer);
	if (!dio || result.cookie != dio) {
		crystalhd_rx_buffer_release(ctx->adp, result.buffer);
		return BC_STS_IO_ERROR;
	}

	frame->OutPutBuffs.YuvBuff = dio->uinfo.xfr_buff;
	frame->OutPutBuffs.YuvBuffSz = dio->uinfo.xfr_len;
	frame->OutPutBuffs.UVbuffOffset = dio->uinfo.uv_offset;
	frame->OutPutBuffs.b422Mode = dio->uinfo.b422mode;

	frame->OutPutBuffs.YBuffDoneSz = result.y_done_sz;
	frame->OutPutBuffs.UVBuffDoneSz = result.uv_done_sz;

	crystalhd_rx_buffer_release(ctx->adp, result.buffer);

	return BC_STS_SUCCESS;
}

/*
 * Start capture with scalar thresholds. The caller keeps command/device
 * lifetime protection; this function acquires fetch_sem itself.
 */
BC_STATUS crystalhd_capture_start(struct crystalhd_cmd *ctx,
				  uint32_t pause_threshold,
				  uint32_t resume_threshold)
{
	BC_STATUS sts = BC_STS_SUCCESS;

	if (!ctx || !ctx->hw_ctx)
		return BC_STS_INV_ARG;

	if (down_interruptible(&ctx->hw_ctx->fetch_sem))
		return BC_STS_IO_USER_ABORT;

	if (pause_threshold)
		ctx->hw_ctx->PauseThreshold = pause_threshold;
	else
		ctx->hw_ctx->PauseThreshold = HW_PAUSE_THRESHOLD;

	if (resume_threshold)
		ctx->hw_ctx->ResumeThreshold = resume_threshold;
	else
		ctx->hw_ctx->ResumeThreshold = HW_RESUME_THRESHOLD;

	printk(KERN_DEBUG "start_capture: pause_th:%d, resume_th:%d\n", ctx->hw_ctx->PauseThreshold, ctx->hw_ctx->ResumeThreshold);

	ctx->hw_ctx->DrvTotalFrmCaptured = 0;

	ctx->hw_ctx->DefaultPauseThreshold = ctx->hw_ctx->PauseThreshold; /* used to restore on FMTCH */

	if (!ctx->hw_ctx->pfnNotifyHardware(ctx->hw_ctx, BC_EVENT_START_CAPTURE)) {
		sts = BC_STS_IO_ERROR;
	} else {
		ctx->state |= BC_LINK_CAP_EN;
		if (ctx->state == BC_LINK_READY)
			sts = crystalhd_hw_start_capture(ctx->hw_ctx);
	}
	up(&ctx->hw_ctx->fetch_sem);
	return sts == BC_STS_NO_DATA ? BC_STS_SUCCESS : sts;
}

static BC_STATUS bc_cproc_start_capture(struct crystalhd_cmd *ctx,
					crystalhd_ioctl_data *idata)
{
	if (!idata)
		return BC_STS_INV_ARG;

	return crystalhd_capture_start(ctx, idata->udata.u.RxCap.PauseThsh,
				       idata->udata.u.RxCap.ResumeThsh);
}

/*
 * Flush or discard capture registrations. The caller keeps command/device
 * lifetime protection; this function acquires fetch_sem itself.
 */
BC_STATUS crystalhd_capture_flush(struct crystalhd_cmd *ctx, bool discard_only)
{
	struct device *dev = chddev();
	BC_STATUS sts;

	if (!ctx || !ctx->hw_ctx) {
		dev_err(dev, "%s: Invalid Arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	if (down_interruptible(&ctx->hw_ctx->fetch_sem))
		return BC_STS_IO_USER_ABORT;
	if (!(ctx->state & BC_LINK_CAP_EN)) {
		sts = BC_STS_ERR_USAGE;
		goto out;
	}

	dev_dbg(dev, "number of rx success %u and failure %u\n", ctx->hw_ctx->stats.rx_success, ctx->hw_ctx->stats.rx_errors);
	if (discard_only) {
		/* just flush without unmapping and then resume */
		sts = crystalhd_hw_stop_capture_locked(ctx->hw_ctx, false);
		if (sts != BC_STS_SUCCESS)
			goto out;
		if (!ctx->hw_ctx->pfnNotifyHardware(ctx->hw_ctx,
						 BC_EVENT_START_CAPTURE)) {
			sts = BC_STS_IO_ERROR;
			goto out;
		}
		sts = crystalhd_hw_start_capture(ctx->hw_ctx);
		/* An empty free queue is valid; later ADD_RXBUFFS resumes it. */
		if (sts == BC_STS_NO_DATA)
			sts = BC_STS_SUCCESS;
	} else {
		ctx->state &= ~(BC_LINK_CAP_EN|BC_LINK_FMT_CHG);
		sts = crystalhd_hw_stop_capture_locked(ctx->hw_ctx, true);
	}
out:
	up(&ctx->hw_ctx->fetch_sem);
	return sts;
}

static BC_STATUS bc_cproc_flush_cap_buffs(struct crystalhd_cmd *ctx,
					  crystalhd_ioctl_data *idata)
{
	if (!idata) {
		dev_err(chddev(), "%s: Invalid Arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	return crystalhd_capture_flush(ctx,
				       idata->udata.u.FlushRxCap.bDiscardOnly);
}

static BC_STATUS bc_cproc_get_stats(struct crystalhd_cmd *ctx,
				    crystalhd_ioctl_data *idata)
{
	BC_DTS_STATS *stats;
	struct crystalhd_hw_stats	hw_stats;
	uint32_t pic_width;
	uint8_t flags = 0;
	bool readTxOnly = false;
	unsigned long irqflags;

	if (!ctx || !idata || !ctx->hw_ctx) {
		dev_err(chddev(), "%s: Invalid Arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	crystalhd_hw_stats(ctx->hw_ctx, &hw_stats);

	stats = &idata->udata.u.drvStat;
	stats->drvRLL = hw_stats.rdyq_count;
	stats->drvFLL = hw_stats.freeq_count;
	stats->DrvTotalFrmDropped = hw_stats.rx_errors;
	stats->DrvTotalHWErrs = hw_stats.rx_errors + hw_stats.tx_errors;
	stats->intCount = hw_stats.num_interrupts;
	stats->DrvIgnIntrCnt = hw_stats.num_interrupts -
				hw_stats.dev_interrupts;
	stats->TxFifoBsyCnt = hw_stats.cin_busy;
	stats->pauseCount = hw_stats.pause_cnt;

	/* Indicate that we are checking stats on the input buffer for a single threaded application */
	/* this will prevent the HW from going to low power because we assume that once we have told the application */
	/* that we have space in the HW, the app is going to try to DMA. And if we block that DMA, a single threaded application */
	/* will deadlock */
	if(stats->DrvNextMDataPLD & BC_BIT(31))
	{
		flags |= 0x08;
		/* Also for single threaded applications, check to see if we have reduced the power down */
		/* pause threshold to too low and increase it if the RLL is close to the threshold */
/*		if(pDrvStat->drvRLL >= pDevExt->pHwExten->PauseThreshold)
			pDevExt->pHwExten->PauseThreshold++;
		PeekNextTS = TRUE;*/
	}

	/* also indicate that we are just checking stats and not posting */
	/* This allows multi-threaded applications to be placed into low power state */
	/* because eveentually the RX thread will wake up the HW when needed */
	flags |= 0x04;

	stats->pwr_state_change = ctx->pwr_state_change;

	if (ctx->state & BC_LINK_PAUSED)
		stats->DrvPauseTime = 1;

	/* use bit 29 of the input status to indicate that we are trying to read VC1 status */
	/* This is important for the BCM70012 which uses a different input queue for VC1 */
	if(stats->DrvcpbEmptySize & BC_BIT(29))
		flags = 0x2;
	/* Bit 30 is used to indicate that we are reading only the TX stats and to not touch the Ready list */
	if(stats->DrvcpbEmptySize & BC_BIT(30))
		readTxOnly = true;

	spin_lock_irqsave(&ctx->hw_ctx->lock, irqflags);
	ctx->hw_ctx->pfnCheckInputFIFO(ctx->hw_ctx, 0, &stats->DrvcpbEmptySize,
				      false, &flags);
	spin_unlock_irqrestore(&ctx->hw_ctx->lock, irqflags);

	/* status peek ahead to retreive the next decoded frame timestamp */
/*	if (!readTxOnly && stats->drvRLL && (stats->DrvNextMDataPLD & BC_BIT(31))) { */
	if (!readTxOnly && stats->drvRLL) {
		dev_dbg(chddev(), "Have Pictures %d\n", stats->drvRLL);
		pic_width = stats->DrvNextMDataPLD & 0xffff;
		stats->DrvNextMDataPLD = 0;
		if (pic_width <= 1920) {
			/* get fetch lock to make sure that fetch is not in progress as wel peek */
			if(down_interruptible(&ctx->hw_ctx->fetch_sem))
				goto get_out;
			if(ctx->hw_ctx->pfnPeekNextDeodedFr(ctx->hw_ctx,&stats->DrvNextMDataPLD, &stats->picNumFlags, pic_width)) {
				/* Check in case we dropped a picture here */
				crystalhd_hw_stats(ctx->hw_ctx, &hw_stats);
				stats->drvRLL = hw_stats.rdyq_count;
				stats->drvFLL = hw_stats.freeq_count;
			}
			up(&ctx->hw_ctx->fetch_sem);
			dev_dbg(chddev(), "peeking done\n");
		}
	}

get_out:
	return BC_STS_SUCCESS;
}

static BC_STATUS bc_cproc_reset_stats(struct crystalhd_cmd *ctx,
				      crystalhd_ioctl_data *idata)
{
	if (!ctx || !ctx->hw_ctx)
		return BC_STS_INV_ARG;

	crystalhd_hw_stats(ctx->hw_ctx, NULL);

	return BC_STS_SUCCESS;
}

/* Caller holds the user write lock on a present device. Failed-PM close and
 * device removal use accounting-only and quiesced cleanup paths, respectively.
 */
void crystalhd_user_close(struct crystalhd_cmd *ctx, struct crystalhd_user *uc)
{
	uint32_t mode;
	bool owns_session;

	if (!uc->in_use)
		return;

	mode = uc->mode;
	uc->mode = DTS_MODE_INV;
	uc->in_use = 0;

	dev_info(chddev(), "Closing user[%x] handle with mode %x\n", uc->uid, mode);

	owns_session = ctx->session_owner == uc;
	if (owns_session)
		crystalhd_session_release_locked(ctx, uc);
	else if (!ctx->session_owner && bc_get_userhandle_count(ctx) == 0)
		crystalhd_retire_hw_context(ctx, true);

	if (ctx->adp->cfg_users > 0)
		ctx->adp->cfg_users--;
}

BC_STATUS bc_cproc_release_user(struct crystalhd_cmd *ctx, crystalhd_ioctl_data *idata)
{
	if (!ctx || !idata) {
		dev_err(chddev(), "%s: Invalid Arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	if (ctx->user[idata->u_id].mode == DTS_MODE_INV) {
		dev_err(chddev(), "Handle is already closed\n");
		return BC_STS_ERR_USAGE;
	}

	crystalhd_user_close(ctx, &ctx->user[idata->u_id]);

	return BC_STS_SUCCESS;
}

/*=============== Cmd Proc Table.. ======================================*/
static const struct crystalhd_cmd_tbl	g_crystalhd_cproc_tbl[] = {
	{ BCM_IOC_GET_VERSION,		bc_cproc_get_version,	0},
	{ BCM_IOC_GET_HWTYPE,		bc_cproc_get_hwtype,	0},
	{ BCM_IOC_REG_RD,			bc_cproc_reg_rd,	0},
	{ BCM_IOC_REG_WR,			bc_cproc_reg_wr,	0},
	{ BCM_IOC_FPGA_RD,			bc_cproc_link_reg_rd,	0},
	{ BCM_IOC_FPGA_WR,			bc_cproc_link_reg_wr,	0},
	{ BCM_IOC_MEM_RD,			bc_cproc_mem_rd,	0},
	{ BCM_IOC_MEM_WR,			bc_cproc_mem_wr,	0},
	{ BCM_IOC_RD_PCI_CFG,		bc_cproc_cfg_rd,	0},
	{ BCM_IOC_WR_PCI_CFG,		bc_cproc_cfg_wr,	1},
	{ BCM_IOC_FW_DOWNLOAD,		bc_cproc_download_fw,	1},
	{ BCM_IOC_FW_CMD,			bc_cproc_do_fw_cmd,	1},
	{ BCM_IOC_PROC_INPUT,		bc_cproc_proc_input,	1},
	{ BCM_IOC_ADD_RXBUFFS,		bc_cproc_add_cap_buff,	1},
	{ BCM_IOC_FETCH_RXBUFF,		bc_cproc_fetch_frame,	1},
	{ BCM_IOC_START_RX_CAP,		bc_cproc_start_capture,	1},
	{ BCM_IOC_FLUSH_RX_CAP,		bc_cproc_flush_cap_buffs, 1},
	{ BCM_IOC_GET_DRV_STAT,		bc_cproc_get_stats,	0},
	{ BCM_IOC_RST_DRV_STAT,		bc_cproc_reset_stats,	0},
	{ BCM_IOC_NOTIFY_MODE,		bc_cproc_notify_mode,	0},
	{ BCM_IOC_RELEASE,			bc_cproc_release_user,  0},
	{ BCM_IOC_END,				NULL},
};

static BC_STATUS bc_cproc_session_owner_required(struct crystalhd_cmd *ctx,
						 crystalhd_ioctl_data *idata)
{
	(void)ctx;
	(void)idata;
	return BC_STS_ERR_USAGE;
}

/*=============== Cmd Proc Functions.. ===================================*/
/**
 * crystalhd_suspend - Power management suspend request.
 * @ctx: Command layer context.
 * @idata: Iodata - required for internal use.
 *
 * Return:
 *	status
 *
 * 1. Set the state to Suspend.
 * 2. Flush the Rx Buffers it will unmap all the buffers and
 *    stop the RxDMA engine.
 * 3. Cancel The TX Io and Stop Dma Engine.
 * 4. Put the DDR in to deep sleep.
 * 5. Stop the hardware putting it in to Reset State.
 *
 * Current gstreamer frame work does not provide any power management
 * related notification to user mode decoder plug-in. As a work-around
 * we pass on the power mangement notification to our plug-in by completing
 * all outstanding requests with BC_STS_IO_USER_ABORT return code.
 */
BC_STATUS crystalhd_suspend(struct crystalhd_cmd *ctx, crystalhd_ioctl_data *idata)
{
	struct device *dev = chddev();
	BC_STATUS sts = BC_STS_SUCCESS;

	if (!ctx || !idata) {
		dev_err(dev, "Invalid Parameters\n");
		return BC_STS_ERROR;
	}

	if (ctx->state == BC_LINK_INVALID) {
		dev_dbg(dev, "Nothing To Do Suspend Success\n");
		return BC_STS_SUCCESS;
	}

	if (!ctx->hw_ctx)
		return BC_STS_INV_ARG;

	if (ctx->state & BC_LINK_SUSPEND)
		return BC_STS_SUCCESS;

	dev_dbg(dev, "State before suspend is %x\n", ctx->state);

	bc_cproc_mark_pwr_state(ctx, BC_HW_SUSPEND); /* going to suspend */

	if (ctx->state & BC_LINK_CAP_EN) {
		// Clean any pending RX
		sts = crystalhd_hw_stop_capture(ctx->hw_ctx, false);
		if (sts != BC_STS_SUCCESS)
			return sts;
	}

	/* TX stop is engine-wide. Drain every list owner, including owners that
	 * are not represented by the legacy synchronous tag.
	 */
	sts = crystalhd_hw_cancel_all_tx(ctx->hw_ctx);
	if (sts != BC_STS_SUCCESS)
		return sts;

	ctx->state = BC_LINK_SUSPEND;
	ctx->decoder_phase = CRYSTALHD_DECODER_RECOVERY_REQUIRED;
	ctx->decoder_channel_id = 0;

	sts = crystalhd_hw_suspend(ctx->hw_ctx);
	if (sts != BC_STS_SUCCESS)
		return sts;

	dev_dbg(dev, "Crystal HD suspend success\n");

	return BC_STS_SUCCESS;
}

/**
 * crystalhd_resume - Resume frame capture.
 * @ctx: Command layer contextx.
 *
 * Return:
 *	status
 *
 *
 * Resume frame capture.
 *
 * PM_Resume can't resume the playback state back to pre-suspend state
 * because we don't keep video clip related information within driver.
 * To get back to the pre-suspend state App will re-open the device and
 * start a new playback session from the pre-suspend clip position.
 *
 */
BC_STATUS crystalhd_resume(struct crystalhd_cmd *ctx)
{
	BC_STATUS sts = BC_STS_SUCCESS;

	if (!ctx)
		return BC_STS_INV_ARG;

	/* No decoder context exists before the first open or after the last close. */
	if (!ctx->hw_ctx)
		return ctx->state == BC_LINK_INVALID ? BC_STS_SUCCESS : BC_STS_INV_ARG;

	sts = crystalhd_hw_resume(ctx->hw_ctx);
	if (sts != BC_STS_SUCCESS)
		return sts;

	/* An open monitor/unconfigured handle must still admit a playback session. */
	if (ctx->state == BC_LINK_INVALID)
		return BC_STS_SUCCESS;

	bc_cproc_mark_pwr_state(ctx, BC_HW_RESUME); /* Starting resume */

	ctx->state = BC_LINK_RESUME;

	dev_dbg(chddev(), "crystalhd_resume Success %x\n", ctx->state);

	return BC_STS_SUCCESS;
}

/**
 * crystalhd_user_open - Create application handle.
 * @ctx: Command layer contextx.
 * @user_ctx: User ID context.
 *
 * Return:
 *	status
 *
 * Creates an application specific UID and allocates
 * application specific resources. HW layer initialization
 * is done for the first open request.
 */
BC_STATUS crystalhd_user_open(struct crystalhd_cmd *ctx,
			      struct crystalhd_user **user_ctx)
{
	struct device *dev = chddev();
	struct crystalhd_user *uc;
	BC_STATUS sts;

	if (!ctx || !user_ctx) {
		dev_err(dev, "Invalid arg..\n");
		return BC_STS_INV_ARG;
	}

	uc = bc_cproc_get_uid(ctx);
	if (!uc) {
		dev_info(dev, "No free user context...\n");
		return BC_STS_BUSY;
	}

	dev_info(dev, "Opening new user[%x] handle\n", uc->uid);

	uc->mode = DTS_MODE_INV;
	uc->in_use = 0;

	sts = crystalhd_ensure_hw_context(ctx);
	if (sts != BC_STS_SUCCESS)
		return sts;

	uc->in_use = 1;

	*user_ctx = uc;

	ctx->pwr_state_change = BC_HW_RUNNING;

	return BC_STS_SUCCESS;
}

/**
 * crystalhd_setup_cmd_context - Setup Command layer resources.
 * @ctx: Command layer contextx.
 * @adp: Adapter context
 *
 * Return:
 *	status
 *
 * Called at the time of driver load.
 */
BC_STATUS crystalhd_setup_cmd_context(struct crystalhd_cmd *ctx,
				    struct crystalhd_adp *adp)
{
	struct device *dev = &adp->pdev->dev;
	int i = 0;
	BC_STATUS sts;

	if (!ctx || !adp) {
		dev_err(dev, "%s: Invalid arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	if (ctx->adp)
		dev_dbg(dev, "Resetting Cmd context delete missing..\n");

	ctx->adp = adp;
	ctx->session_owner = NULL;
	crystalhd_decoder_tracking_reset(ctx);
	for (i = 0; i < BC_LINK_MAX_OPENS; i++) {
		ctx->user[i].uid = i;
		ctx->user[i].in_use = 0;
		ctx->user[i].mode = DTS_MODE_INV;
	}

	ctx->hw_ctx = kzalloc(sizeof(struct crystalhd_hw), GFP_KERNEL);
	if (!ctx->hw_ctx) {
		dev_err(dev, "%s: Failed to allocate hw context\n", __func__);
		return BC_STS_ERROR;
	}

	/*Open and Close the Hardware to put it in to sleep state*/
	disable_irq(ctx->adp->pdev->irq);
	sts = crystalhd_hw_open(ctx->hw_ctx, ctx->adp);
	if (sts == BC_STS_SUCCESS)
		crystalhd_hw_close(ctx->hw_ctx);
	kfree(ctx->hw_ctx);
	ctx->hw_ctx = NULL;
	enable_irq(ctx->adp->pdev->irq);

	return sts;
}

/**
 * crystalhd_delete_cmd_context - Release Command layer resources.
 * @ctx: Command layer contextx.
 *
 * Return:
 *	status
 *
 * Called at the time of driver un-load.
 */
BC_STATUS crystalhd_delete_cmd_context(struct crystalhd_cmd *ctx)
{
	dev_dbg(chddev(), "Deleting Command context..\n");

	/* PCI removal has excluded all ioctls, disabled bus mastering and
	 * released the IRQ before entry. No hardware or IRQ callbacks here.
	 */
	if (ctx->hw_ctx) {
		crystalhd_hw_free_dma_rings(ctx->hw_ctx);
		kfree(ctx->hw_ctx);
		ctx->hw_ctx = NULL;
	}
	if (ctx->adp->fill_byte_pool)
		crystalhd_destroy_dio_pool(ctx->adp);
	if (ctx->adp->elem_pool_head)
		crystalhd_delete_elem_pool(ctx->adp);
	ctx->state = BC_LINK_INVALID;
	ctx->session_owner = NULL;
	crystalhd_decoder_tracking_reset(ctx);
	ctx->adp = NULL;

	return BC_STS_SUCCESS;
}

/**
 * crystalhd_get_cmd_proc  - Cproc table lookup.
 * @ctx: Command layer contextx.
 * @cmd: IOCTL command code.
 * @uc: User ID context.
 *
 * Return:
 *	command proc function pointer
 *
 * This function checks the process context, application's
 * mode of operation and returns the function pointer
 * from the cproc table.
 */
crystalhd_cmd_proc crystalhd_get_cmd_proc(struct crystalhd_cmd *ctx, uint32_t cmd,
				      struct crystalhd_user *uc)
{
	struct device *dev = chddev();
	crystalhd_cmd_proc cproc = NULL;
	unsigned int i, tbl_sz;

	if (!ctx) {
		dev_err(dev, "Invalid arg.. Cmd[%d]\n", cmd);
		return NULL;
	}

	if ((cmd != BCM_IOC_GET_DRV_STAT) && (ctx->state & BC_LINK_SUSPEND)) {
		dev_err(dev, "Invalid State [suspend Set].. Cmd[%x]\n", cmd);
		return NULL;
	}

	tbl_sz = sizeof(g_crystalhd_cproc_tbl) / sizeof(struct crystalhd_cmd_tbl);
	for (i = 0; i < tbl_sz; i++) {
		if (g_crystalhd_cproc_tbl[i].cmd_id == cmd) {
			if (g_crystalhd_cproc_tbl[i].requires_session_owner &&
			    ctx->session_owner != uc) {
				dev_dbg(dev, "Blocking cmd %d \n", cmd);
				if ((uc->mode & 0xFF) != DTS_MONITOR_MODE)
					cproc = bc_cproc_session_owner_required;
				break;
			}
			cproc = g_crystalhd_cproc_tbl[i].cmd_proc;
			break;
		}
	}

	return cproc;
}

/**
 * crystalhd_cmd_interrupt - ISR entry point
 * @ctx: Command layer contextx.
 *
 * Return:
 *	TRUE: If interrupt from CrystalHD device.
 *
 *
 * ISR entry point from OS layer.
 */
bool crystalhd_cmd_interrupt(struct crystalhd_cmd *ctx)
{
	if (!ctx) {
		printk(KERN_ERR "%s: Invalid arg..\n", __func__);
		return false;
	}

	/* If HW has not been initialized then all interrupts are spurious */
	if ((ctx->hw_ctx == NULL) || (ctx->hw_ctx->pfnFindAndClearIntr == NULL))
		return false;

	return ctx->hw_ctx->pfnFindAndClearIntr(ctx->adp, ctx->hw_ctx);
}
