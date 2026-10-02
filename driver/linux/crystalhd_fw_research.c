// SPDX-License-Identifier: GPL-2.0-or-later
#include <crypto/hash.h>
#include <crypto/sha2.h>
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/firmware.h>
#include <linux/miscdevice.h>

#include "crystalhd_lnx.h"
#include "crystalhd_fw_research.h"
#include "../../include/crystalhd_fw_research.h"

static const u8 crystalhd_fw_research_owner;

/* Research-only wire values: eC011_VIDEO_ALG_H261/H263/MPEG1 in 7411d.h. */
#define CRYSTALHD_FW_RESEARCH_ALGORITHM_H261 2U
#define CRYSTALHD_FW_RESEARCH_ALGORITHM_H263 3U
#define CRYSTALHD_FW_RESEARCH_ALGORITHM_MPEG1 5U

static const u8 crystalhd_fw_research_sha256[SHA256_DIGEST_SIZE] = {
	0x8b, 0xf3, 0xa6, 0x8f, 0x5c, 0x64, 0x68, 0x63,
	0x58, 0xa5, 0x22, 0x74, 0xe4, 0x09, 0x11, 0xa8,
	0x8c, 0x7f, 0x8c, 0x67, 0xec, 0xbf, 0x6c, 0xf1,
	0x55, 0x7a, 0x49, 0xb4, 0xd7, 0xbc, 0x67, 0xc9,
};

static bool crystalhd_fw_research_live(void)
{
	return READ_ONCE(THIS_MODULE->state) == MODULE_STATE_LIVE;
}

static bool crystalhd_fw_research_request_valid(
	const struct crystalhd_fw_research_request *request)
{
	unsigned int i;

	if (request->version != CRYSTALHD_FW_RESEARCH_VERSION ||
	    request->size != sizeof(struct crystalhd_fw_research_result) ||
	    request->flags)
		return false;
	switch (request->selector) {
	case CRYSTALHD_FW_RESEARCH_VERSION_ONLY:
	case CRYSTALHD_FW_RESEARCH_H264_CONTROL:
	case CRYSTALHD_FW_RESEARCH_H261_CONTROL:
	case CRYSTALHD_FW_RESEARCH_H263_CONTROL:
	case CRYSTALHD_FW_RESEARCH_MPEG1_CONTROL:
		break;
	default:
		return false;
	}
	for (i = 0; i < ARRAY_SIZE(request->reserved); i++)
		if (request->reserved[i])
			return false;
	return true;
}

static bool crystalhd_fw_research_retained(struct crystalhd_adp *adp)
{
	struct crystalhd_cmd *ctx = &adp->cmds;

	return ctx->session_owner || ctx->session_module_pinned ||
		ctx->session_lifetime_owner || ctx->session_lifetime_ops ||
		ctx->stream || ctx->hw_ctx || adp->fill_byte_pool ||
		adp->elem_pool_head || adp->ua_map_free_head;
}

static int crystalhd_fw_research_idle(struct crystalhd_adp *adp)
{
	struct crystalhd_cmd *ctx = &adp->cmds;
	unsigned int i;

	lockdep_assert_held_write(&adp->user_lock);
	if (!READ_ONCE(adp->present) || ctx->adp != adp)
		return -ENODEV;
	if (!adp->hw_accessible)
		return -EAGAIN;
	if (adp->pdev->device != BC_PCI_DEVID_FLEA)
		return -EOPNOTSUPP;
	if (adp->cfg_users || ctx->state != BC_LINK_INVALID ||
	    ctx->retain_rx_on_suspend || crystalhd_fw_research_retained(adp))
		return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(ctx->user); i++)
		if (ctx->user[i].in_use || ctx->user[i].mode != DTS_MODE_INV)
			return -EBUSY;
	return 0;
}

static int crystalhd_fw_research_hash(const struct firmware *firmware,
				      u8 digest[SHA256_DIGEST_SIZE],
				      bool *completed)
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	int rc;

	*completed = false;
	memset(digest, 0, SHA256_DIGEST_SIZE);
	if (!firmware || !firmware->data || !firmware->size ||
	    firmware->size > CRYSTALHD_MAX_FIRMWARE_SIZE)
		return -EINVAL;
	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!desc) {
		rc = -ENOMEM;
		goto free_tfm;
	}
	desc->tfm = tfm;
	rc = crypto_shash_digest(desc, firmware->data, firmware->size, digest);
	if (rc) {
		memset(digest, 0, SHA256_DIGEST_SIZE);
	} else {
		*completed = true;
		if (memcmp(digest, crystalhd_fw_research_sha256,
			   SHA256_DIGEST_SIZE))
			rc = -EKEYREJECTED;
	}
	kfree(desc);
free_tfm:
	crypto_free_shash(tfm);
	return rc;
}

static int crystalhd_fw_research_payload(BC_FW_CMD *fw_cmd, u32 command,
					 u32 sequence, u32 selector)
{
	struct crystalhd_fw_init_cmd *init;
	struct crystalhd_fw_channel_open_cmd *open;
	struct crystalhd_fw_channel_stop_close_cmd *close;

	BUILD_BUG_ON(sizeof(struct crystalhd_fw_init_cmd) !=
		     CRYSTALHD_FW_INIT_WORDS * sizeof(u32));
	BUILD_BUG_ON(sizeof(struct crystalhd_fw_channel_open_cmd) !=
		     CRYSTALHD_FW_CHANNEL_OPEN_WORDS * sizeof(u32));
	BUILD_BUG_ON(sizeof(struct crystalhd_fw_channel_stop_close_cmd) !=
		     CRYSTALHD_FW_CHANNEL_STOP_CLOSE_WORDS * sizeof(u32));
	BUILD_BUG_ON(sizeof(fw_cmd->rsp) !=
		     CRYSTALHD_FW_RESEARCH_RESPONSE_WORDS * sizeof(u32));
	memset(fw_cmd, 0, sizeof(*fw_cmd));
	fw_cmd->cmd[0] = command;
	fw_cmd->cmd[1] = sequence;
	switch (command) {
	case eCMD_C011_INIT:
		init = (struct crystalhd_fw_init_cmd *)fw_cmd->cmd;
		init->mem_size_mb = CRYSTALHD_FW_INIT_MEM_SIZE_MB;
		init->input_clk_hz = CRYSTALHD_FW_INIT_INPUT_CLK_HZ;
		init->uart_baud_rate = CRYSTALHD_FW_INIT_UART_BAUD;
		init->init_arcs = CRYSTALHD_FW_INIT_STREAM_ARC |
			CRYSTALHD_FW_INIT_VDEC_ARC;
		init->interrupt = CRYSTALHD_FW_INIT_INT_ENABLE;
		init->brcm_mode = CRYSTALHD_FW_INIT_BRCM_ECG_MODE;
		init->fgt_enable = CRYSTALHD_FW_INIT_FGT_ENABLE;
		break;
	case eCMD_C011_GET_VERSION:
	case eCMD_C011_DEC_CHAN_STATUS:
		break;
	case eCMD_C011_DEC_CHAN_OPEN:
		open = (struct crystalhd_fw_channel_open_cmd *)fw_cmd->cmd;
		open->stream_type = CRYSTALHD_FW_STREAM_TYPE_PES;
		switch (selector) {
		case CRYSTALHD_FW_RESEARCH_H264_CONTROL:
			open->video_algorithm = CRYSTALHD_FW_VIDEO_ALGORITHM_H264;
			break;
		case CRYSTALHD_FW_RESEARCH_H261_CONTROL:
			open->video_algorithm = CRYSTALHD_FW_RESEARCH_ALGORITHM_H261;
			break;
		case CRYSTALHD_FW_RESEARCH_H263_CONTROL:
			open->video_algorithm = CRYSTALHD_FW_RESEARCH_ALGORITHM_H263;
			break;
		case CRYSTALHD_FW_RESEARCH_MPEG1_CONTROL:
			open->video_algorithm = CRYSTALHD_FW_RESEARCH_ALGORITHM_MPEG1;
			break;
		default:
			return -EINVAL;
		}
		break;
	case eCMD_C011_DEC_CHAN_CLOSE:
		close = (struct crystalhd_fw_channel_stop_close_cmd *)fw_cmd->cmd;
		close->picture_release = CRYSTALHD_FW_PICTURE_RELEASE_INTERNAL;
		close->last_picture_display = CRYSTALHD_FW_LAST_PICTURE_DISPLAY_ON;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int crystalhd_fw_research_command(struct crystalhd_cmd *ctx,
	struct crystalhd_fw_research_result *result, u32 command)
{
	struct crystalhd_fw_research_reply *reply;
	BC_FW_CMD fw_cmd;
	BC_STATUS sts;
	int rc;

	if (!READ_ONCE(ctx->adp->present))
		return -ENODEV;
	if (result->command_count >= CRYSTALHD_FW_RESEARCH_MAX_COMMANDS ||
	    ctx->fw_sequence >= CRYSTALHD_FW_RESEARCH_MAX_COMMANDS)
		return -EOVERFLOW;
	rc = crystalhd_fw_research_payload(&fw_cmd, command, ++ctx->fw_sequence,
					  result->request.selector);
	if (rc)
		return rc;
	reply = &result->replies[result->command_count++];
	reply->command = command;
	reply->sequence = fw_cmd.cmd[1];
	sts = crystalhd_fw_exec_locked(ctx, &crystalhd_fw_research_owner, &fw_cmd);
	reply->transport_status = sts;
	/* For these Flea commands only these two statuses establish a complete
	 * DRAM reply read. Admission, timeout and read errors never do. In
	 * particular, a zeroed response status does not establish validity.
	 */
	reply->raw_response_valid = sts == BC_STS_SUCCESS || sts == BC_STS_FW_CMD_ERR;
	if (!reply->raw_response_valid)
		return crystalhd_status_to_errno(sts);
	memcpy(reply->response, fw_cmd.rsp, sizeof(reply->response));
	reply->header_matches = fw_cmd.rsp[0] == command &&
		fw_cmd.rsp[1] == reply->sequence;
	if (!reply->header_matches)
		return -EPROTO;
	if (!READ_ONCE(ctx->adp->present))
		return -ENODEV;
	rc = crystalhd_status_to_errno(sts);
	if (!rc && command == eCMD_C011_DEC_CHAN_OPEN && fw_cmd.rsp[3] != 0)
		return -EPROTO;
	return rc;
}

static void crystalhd_fw_research_run(u64 generation,
	const struct crystalhd_fw_research_request *request,
	struct crystalhd_fw_research_result *result)
{
	struct crystalhd_device_access access;
	struct crystalhd_cmd *ctx;
	const struct firmware *firmware = NULL;
	bool session_attempted = false;
	bool hash_completed;
	BC_STATUS sts;
	int rc;

	memset(result, 0, sizeof(*result));
	result->request = *request;
	result->generation = generation;
	if (!crystalhd_fw_research_live()) {
		result->status = -EAGAIN;
		return;
	}
	if (!capable(CAP_SYS_RAWIO)) {
		result->status = -EPERM;
		return;
	}
	if (!crystalhd_fw_research_request_valid(request)) {
		result->status = -EINVAL;
		return;
	}
	rc = crystalhd_device_enter(generation, true, &access);
	if (rc) {
		result->status = rc;
		return;
	}
	ctx = &access.adp->cmds;
	rc = crystalhd_fw_research_idle(access.adp);
	if (rc)
		goto exit;
	rc = request_firmware(&firmware, CRYSTALHD_FLEA_FIRMWARE_NAME,
			      &access.adp->pdev->dev);
	if (rc)
		goto exit;
	rc = crystalhd_fw_research_hash(firmware, result->firmware_sha256,
				       &hash_completed);
	result->firmware_hash_valid = hash_completed;
	if (rc)
		goto release;
	if (!READ_ONCE(access.adp->present)) {
		rc = -ENODEV;
		goto release;
	}
	session_attempted = true;
	sts = crystalhd_session_acquire_locked(ctx, &crystalhd_fw_research_owner);
	rc = crystalhd_status_to_errno(sts);
	if (rc)
		goto release;
	if (!READ_ONCE(access.adp->present)) {
		rc = -ENODEV;
		goto cleanup;
	}
	result->download_attempted = 1;
	sts = crystalhd_fw_download_locked(ctx, &crystalhd_fw_research_owner,
					  firmware->data, firmware->size);
	result->download_status = sts;
	rc = crystalhd_status_to_errno(sts);
	if (rc)
		goto cleanup;
	rc = crystalhd_fw_research_command(ctx, result, eCMD_C011_INIT);
	if (rc)
		goto cleanup;
	rc = crystalhd_fw_research_command(ctx, result, eCMD_C011_GET_VERSION);
	if (rc || request->selector == CRYSTALHD_FW_RESEARCH_VERSION_ONLY)
		goto cleanup;
	rc = crystalhd_fw_research_command(ctx, result, eCMD_C011_DEC_CHAN_OPEN);
	if (rc)
		goto cleanup;
	if (request->selector == CRYSTALHD_FW_RESEARCH_H264_CONTROL) {
		rc = crystalhd_fw_research_command(ctx, result, eCMD_C011_DEC_CHAN_STATUS);
		if (rc)
			goto cleanup;
	}
	rc = crystalhd_fw_research_command(ctx, result, eCMD_C011_DEC_CHAN_CLOSE);

cleanup:
	if (READ_ONCE(access.adp->present)) {
		result->cleanup_attempted = 1;
		sts = crystalhd_session_release_locked(ctx, &crystalhd_fw_research_owner);
		result->cleanup_status = sts;
		if (!rc)
			rc = crystalhd_status_to_errno(sts);
	} else {
		/* Removal publishes cancellation before waiting for our lifetime
		 * barrier. It owns terminal cleanup; absence is not proof of a stop.
		 */
		result->cleanup_status = BC_STS_CMD_CANCELLED;
		if (!rc)
			rc = -ENODEV;
	}
release:
	release_firmware(firmware);
exit:
	result->retained = session_attempted &&
		crystalhd_fw_research_retained(access.adp);
	result->status = rc;
	crystalhd_device_exit(&access);
}

struct crystalhd_fw_research_file {
	u64 generation;
};

static int crystalhd_fw_research_open(struct inode *inode, struct file *file)
{
	struct crystalhd_fw_research_file *binding;
	int rc;

	/* Do not publish a file whose callback code could outlive failed init. */
	if (!crystalhd_fw_research_live())
		return -EAGAIN;
	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	binding = kzalloc(sizeof(*binding), GFP_KERNEL);
	if (!binding)
		return -ENOMEM;
	rc = crystalhd_fw_research_generation(&binding->generation);
	if (rc) {
		kfree(binding);
		return rc;
	}
	file->private_data = binding;
	rc = nonseekable_open(inode, file);
	if (rc) {
		file->private_data = NULL;
		kfree(binding);
	}
	return rc;
}

static int crystalhd_fw_research_release(struct inode *inode, struct file *file)
{
	(void)inode;
	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}

static long crystalhd_fw_research_info(u64 generation, void __user *user)
{
	struct crystalhd_fw_research_info info = { };
	struct crystalhd_device_access access;
	int rc;

	rc = crystalhd_device_enter(generation, false, &access);
	if (rc)
		return rc;
	if (access.adp->generation != generation) {
		rc = -ESTALE;
	} else {
		info.version = CRYSTALHD_FW_RESEARCH_VERSION;
		info.size = sizeof(info);
		info.generation = generation;
		if (access.adp->pdev->device == BC_PCI_DEVID_FLEA)
			info.selector_mask = CRYSTALHD_FW_RESEARCH_SELECTOR_MASK;
		memcpy(info.firmware_sha256, crystalhd_fw_research_sha256,
		       sizeof(info.firmware_sha256));
	}
	crystalhd_device_exit(&access);
	if (rc)
		return rc;
	return copy_to_user(user, &info, sizeof(info)) ? -EFAULT : 0;
}

static long crystalhd_fw_research_ioctl(struct file *file, unsigned int command,
					unsigned long argument)
{
	struct crystalhd_fw_research_file *binding = file->private_data;
	struct crystalhd_fw_research_request request;
	struct crystalhd_fw_research_result *result;
	void __user *user = (void __user *)argument;
	long rc = 0;

	if (!crystalhd_fw_research_live())
		return -EAGAIN;
	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (!binding)
		return -ENODEV;
	if (command == CRYSTALHD_FW_RESEARCH_GET_INFO)
		return crystalhd_fw_research_info(binding->generation, user);
	if (command != CRYSTALHD_FW_RESEARCH_RUN)
		return -ENOTTY;
	if (copy_from_user(&request, user, sizeof(request)))
		return -EFAULT;
	if (!crystalhd_fw_research_request_valid(&request))
		return -EINVAL;
	result = kzalloc(sizeof(*result), GFP_KERNEL);
	if (!result)
		return -ENOMEM;
	crystalhd_fw_research_run(binding->generation, &request, result);
	if (copy_to_user(user, result, sizeof(*result)))
		rc = -EFAULT;
	kfree(result);
	return rc;
}

#ifdef CONFIG_COMPAT
static long crystalhd_fw_research_compat_ioctl(struct file *file,
					      unsigned int command,
					      unsigned long argument)
{
	return crystalhd_fw_research_ioctl(file, command,
					  (unsigned long)compat_ptr(argument));
}
#endif

static const struct file_operations crystalhd_fw_research_fops = {
	.owner = THIS_MODULE,
	.open = crystalhd_fw_research_open,
	.release = crystalhd_fw_research_release,
	.unlocked_ioctl = crystalhd_fw_research_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = crystalhd_fw_research_compat_ioctl,
#endif
};

static struct miscdevice crystalhd_fw_research_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "crystalhd-fw-research",
	.fops = &crystalhd_fw_research_fops,
	.mode = 0600,
};

int crystalhd_fw_research_init(void)
{
	return misc_register(&crystalhd_fw_research_device);
}

void crystalhd_fw_research_cleanup(void)
{
	misc_deregister(&crystalhd_fw_research_device);
}
