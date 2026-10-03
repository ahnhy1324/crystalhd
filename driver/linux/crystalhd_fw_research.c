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

/* Fixed stock ARM locations; no caller-provided address is accepted. */
#define CRYSTALHD_FW_RESEARCH_STATE_CALIBRATION_OFFSET 0x000006fcU
#define CRYSTALHD_FW_RESEARCH_STATE_GLOBAL_OFFSET 0x000d1ff4U
#define CRYSTALHD_FW_RESEARCH_STATE_OPEN_OFFSET 0x000d3ac4U
#define CRYSTALHD_FW_RESEARCH_STATE_CONTEXT_OFFSET 0x000d3ad0U
#define CRYSTALHD_FW_RESEARCH_STATE_CHANNEL_ZERO 0x000d3a00U
#define CRYSTALHD_FW_RESEARCH_CONTROLLER_ROOT_OFFSET 0x000d3a08U
/* Pinned fresh-INIT variable heap: base 0xd5384 + 0x3c heap metadata +
 * 0x1c block header. This envelope is not runtime allocation certification.
 */
#define CRYSTALHD_FW_RESEARCH_IMAGE_CONTEXT_MIN 0x000d53dcU
#define CRYSTALHD_FW_RESEARCH_IMAGE_CONTEXT_END 0x00116000U
#define CRYSTALHD_FW_RESEARCH_IMAGE_CONTEXT_BYTES 0x378U
#define CRYSTALHD_FW_RESEARCH_IMAGE_TUPLE_OFFSET 0x1acU
#define CRYSTALHD_FW_RESEARCH_PACKET_ALIASES_OFFSET 0x94U
#define CRYSTALHD_FW_RESEARCH_PACKET_PHYSICAL_OFFSET 0x1ccU
#define CRYSTALHD_FW_RESEARCH_REPLY_SLOTS_OFFSET 0x250U
#define CRYSTALHD_FW_RESEARCH_HEAP_IMAGE_MIN 0x00117000U
#define CRYSTALHD_FW_RESEARCH_HEAP_END 0x03ffc000U
#define CRYSTALHD_FW_RESEARCH_HEAP_IMAGE_BYTES 0x00100000U
#define CRYSTALHD_FW_RESEARCH_HEAP_PACKET_OFFSET 0x00070000U
#define CRYSTALHD_FW_RESEARCH_HEAP_PACKET_BYTES 0x100U
#define CRYSTALHD_FW_RESEARCH_STATE_FIRMWARE_SIZE 0x000d3014U

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

static u32 crystalhd_fw_research_raw_command(u32 selector)
{
	switch (selector) {
	case CRYSTALHD_FW_RESEARCH_SCALING_FILTERS_COMMAND:
		return eCMD_C011_DEC_CHAN_SCALING_FILTERS;
	case CRYSTALHD_FW_RESEARCH_PIC_CAPTURE_COMMAND:
		return eCMD_C011_DEC_CHAN_PIC_CAPTURE;
	case CRYSTALHD_FW_RESEARCH_SET_CSC_COMMAND:
		return eCMD_C011_DEC_CHAN_SET_CSC;
	case CRYSTALHD_FW_RESEARCH_SET_FGT_COMMAND:
		return eCMD_C011_DEC_CHAN_SET_FGT;
	case CRYSTALHD_FW_RESEARCH_CUSTOM_VIDOUT_COMMAND:
		return eCMD_C011_DEC_CHAN_CUSTOM_VIDOUT;
	case CRYSTALHD_FW_RESEARCH_FILL_PIC_BUF_COMMAND:
		return eCMD_C011_DEC_CHAN_FILL_PIC_BUF;
	default:
		return 0;
	}
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
	case CRYSTALHD_FW_RESEARCH_SCALING_FILTERS_COMMAND:
	case CRYSTALHD_FW_RESEARCH_PIC_CAPTURE_COMMAND:
	case CRYSTALHD_FW_RESEARCH_SET_CSC_COMMAND:
	case CRYSTALHD_FW_RESEARCH_SET_FGT_COMMAND:
	case CRYSTALHD_FW_RESEARCH_CUSTOM_VIDOUT_COMMAND:
	case CRYSTALHD_FW_RESEARCH_FILL_PIC_BUF_COMMAND:
		break;
	default:
		return false;
	}
	for (i = 0; i < ARRAY_SIZE(request->reserved); i++)
		if (request->reserved[i])
			return false;
	return true;
}

static bool crystalhd_fw_research_state_request_valid(
	const struct crystalhd_fw_research_state_request *request)
{
	return request->version == CRYSTALHD_FW_RESEARCH_VERSION &&
		request->size == sizeof(struct crystalhd_fw_research_state_result) &&
		!request->flags && !request->reserved;
}

static bool crystalhd_fw_research_controller_request_valid(
	const struct crystalhd_fw_research_state_request *request)
{
	return request->version == CRYSTALHD_FW_RESEARCH_VERSION &&
		request->size == sizeof(struct crystalhd_fw_research_controller_result) &&
		!request->flags && !request->reserved;
}

static bool crystalhd_fw_research_image_request_valid(
	const struct crystalhd_fw_research_state_request *request)
{
	return request->version == CRYSTALHD_FW_RESEARCH_VERSION &&
		request->size == sizeof(struct crystalhd_fw_research_image_result) &&
		!request->flags && !request->reserved;
}

static bool crystalhd_fw_research_packet_request_valid(
	const struct crystalhd_fw_research_state_request *request)
{
	return request->version == CRYSTALHD_FW_RESEARCH_VERSION &&
		request->size == sizeof(struct crystalhd_fw_research_packet_result) &&
		!request->flags && !request->reserved;
}

static bool crystalhd_fw_research_heap_packet_request_valid(
	const struct crystalhd_fw_research_state_request *request)
{
	return request->version == CRYSTALHD_FW_RESEARCH_VERSION &&
		request->size == sizeof(struct crystalhd_fw_research_heap_packet_result) &&
		!request->flags && !request->reserved;
}

static bool crystalhd_fw_research_clock_request_valid(
	const struct crystalhd_fw_research_state_request *request)
{
	return request->version == CRYSTALHD_FW_RESEARCH_VERSION &&
		request->size == sizeof(struct crystalhd_fw_research_clock_result) &&
		!request->flags && !request->reserved;
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
		/* Fixed zero-argument routes only, bound to the named selector.
		 * The pinned image's fallback need not produce a matching reply.
		 */
		if (!command || command != crystalhd_fw_research_raw_command(selector))
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

static int crystalhd_fw_research_state_apertures(struct crystalhd_adp *adp)
{
	if (!adp->i2o_addr || !adp->mem_addr)
		return -ENODEV;
	/* Stock download traverses the whole DRAM window before the fixed reads.
	 * Its reader/writer validate DRAM ranges, not the mapped BAR2 length.
	 */
	if (adp->pci_i2o_len < FLEA_GISB_INDIRECT_DATA + sizeof(u32) ||
	    adp->pci_mem_len <
	    ~BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK + 1U)
		return -ERANGE;
	return 0;
}

static int crystalhd_fw_research_state_span(struct crystalhd_adp *adp,
					   u32 offset, u32 count)
{
	u32 mask = BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK;
	u32 window_offset = offset & ~mask;
	u32 bytes;

	if (!count || !crystalhd_valid_dram_range(offset, count))
		return -ERANGE;
	bytes = count * sizeof(u32);
	if ((offset & mask) != ((offset + bytes - 1) & mask) ||
	    adp->pci_mem_len < bytes ||
	    window_offset > adp->pci_mem_len - bytes)
		return -ERANGE;
	return 0;
}

static int crystalhd_fw_research_state_context(struct crystalhd_cmd *ctx,
					      u64 generation,
					      struct crystalhd_hw *hw)
{
	struct crystalhd_adp *adp = ctx->adp;

	lockdep_assert_held_write(&adp->user_lock);
	if (!READ_ONCE(adp->present) || !hw || ctx->hw_ctx != hw ||
	    hw->adp != adp || !hw->pfnDevDRAMRead || !hw->pfnWriteDevRegister)
		return -ENODEV;
	if (adp->generation != generation)
		return -ESTALE;
	if (!adp->hw_accessible)
		return -EAGAIN;
	if (adp->pdev->device != BC_PCI_DEVID_FLEA)
		return -EOPNOTSUPP;
	if (ctx->session_owner != &crystalhd_fw_research_owner ||
	    !ctx->session_module_pinned)
		return -EACCES;
	if (ctx->state != BC_LINK_INIT)
		return -EBUSY;
	return crystalhd_fw_research_state_apertures(adp);
}

static int crystalhd_fw_research_state_ready(struct crystalhd_cmd *ctx,
					    u64 generation,
					    struct crystalhd_hw *hw)
{
	int rc;

	lockdep_assert_held(&hw->lock);
	rc = crystalhd_fw_research_state_context(ctx, generation, hw);
	if (rc)
		return rc;
	if (READ_ONCE(hw->dma_fault))
		return -EIO;
	if (hw->fwcmd_pending || hw->fwcmd_poisoned)
		return -EBUSY;
	if (hw->FleaPowerState != FLEA_PS_ACTIVE)
		return -EAGAIN;
	return 0;
}

static int crystalhd_fw_research_state_sample(struct crystalhd_cmd *ctx,
	u64 generation, struct crystalhd_fw_research_state_sample *sample,
	bool calibration, bool opened)
{
	static const struct {
		u32 offset;
		u32 count;
		u32 word;
	} reads[] = {
		{ CRYSTALHD_FW_RESEARCH_STATE_GLOBAL_OFFSET, 2, 0 },
		{ CRYSTALHD_FW_RESEARCH_STATE_OPEN_OFFSET, 1, 2 },
		{ CRYSTALHD_FW_RESEARCH_STATE_CONTEXT_OFFSET, 1, 3 },
	};
	struct crystalhd_hw *hw = ctx->hw_ctx;
	u32 words[4] = { };
	unsigned long flags;
	unsigned int i;
	BC_STATUS sts;
	int rc;

	memset(sample, 0, sizeof(*sample));
	rc = crystalhd_fw_research_state_context(ctx, generation, hw);
	if (rc)
		goto done;
	sts = crystalhd_hw_fw_cmd_enter(hw);
	rc = crystalhd_status_to_errno(sts);
	if (rc) {
		if (!READ_ONCE(ctx->adp->present))
			rc = -ENODEV;
		goto done;
	}
	/* The reader selects a shared DRAM window. Fence ISR users across every
	 * constituent read, with no wake, mailbox post or retry in this sample.
	 */
	spin_lock_irqsave(&hw->lock, flags);
	rc = crystalhd_fw_research_state_ready(ctx, generation, hw);
	if (rc)
		goto unlock;
	for (i = 0; i < (calibration ? 1U : ARRAY_SIZE(reads)); i++) {
		u32 offset = calibration ?
			CRYSTALHD_FW_RESEARCH_STATE_CALIBRATION_OFFSET : reads[i].offset;
		u32 count = calibration ? 1U : reads[i].count;
		u32 word = calibration ? 0U : reads[i].word;

		rc = crystalhd_fw_research_state_span(ctx->adp, offset, count);
		if (!rc)
			rc = crystalhd_fw_research_state_ready(ctx, generation, hw);
		if (rc)
			goto unlock;
		sample->attempted = 1;
		sts = hw->pfnDevDRAMRead(hw, offset, count, &words[word]);
		rc = crystalhd_fw_research_state_ready(ctx, generation, hw);
		if (!rc)
			rc = crystalhd_status_to_errno(sts);
		if (rc)
			goto unlock;
	}
	memcpy(sample->words, words, sizeof(words));
	sample->read_complete = 1;
	if (calibration) {
		if (words[0] != CRYSTALHD_FW_RESEARCH_STATE_CHANNEL_ZERO)
			rc = -EPROTO;
	} else if (words[0] != 1 ||
		   words[1] != CRYSTALHD_FW_RESEARCH_STATE_CHANNEL_ZERO ||
		   (words[2] & 0xffU) != (opened ? 1U : 0U) ||
		   (words[3] & 0xffffffU) != (opened ? 0x200U : 0U)) {
		rc = -EPROTO;
	}
unlock:
	spin_unlock_irqrestore(&hw->lock, flags);
	crystalhd_hw_fw_cmd_leave(hw);
done:
	sample->status = rc;
	return rc;
}

static int crystalhd_fw_research_controller_sample(struct crystalhd_cmd *ctx,
	u64 generation, struct crystalhd_fw_research_controller_sample *sample)
{
	struct crystalhd_hw *hw = ctx->hw_ctx;
	u32 root = 0;
	unsigned long flags;
	BC_STATUS sts;
	int rc;

	memset(sample, 0, sizeof(*sample));
	rc = crystalhd_fw_research_state_context(ctx, generation, hw);
	if (rc)
		goto done;
	sts = crystalhd_hw_fw_cmd_enter(hw);
	rc = crystalhd_status_to_errno(sts);
	if (rc) {
		if (!READ_ONCE(ctx->adp->present))
			rc = -ENODEV;
		goto done;
	}
	/* Only the fixed ARM publication word is read. Its value never selects
	 * another read, register, command or recovery action, including zero.
	 */
	spin_lock_irqsave(&hw->lock, flags);
	rc = crystalhd_fw_research_state_ready(ctx, generation, hw);
	if (rc)
		goto unlock;
	rc = crystalhd_fw_research_state_span(ctx->adp,
		CRYSTALHD_FW_RESEARCH_CONTROLLER_ROOT_OFFSET, 1);
	if (!rc)
		rc = crystalhd_fw_research_state_ready(ctx, generation, hw);
	if (rc)
		goto unlock;
	sample->attempted = 1;
	sts = hw->pfnDevDRAMRead(hw,
		CRYSTALHD_FW_RESEARCH_CONTROLLER_ROOT_OFFSET, 1, &root);
	rc = crystalhd_fw_research_state_ready(ctx, generation, hw);
	if (!rc)
		rc = crystalhd_status_to_errno(sts);
	if (!rc) {
		sample->root = root;
		sample->read_complete = 1;
	}
unlock:
	spin_unlock_irqrestore(&hw->lock, flags);
	crystalhd_hw_fw_cmd_leave(hw);
done:
	sample->status = rc;
	return rc;
}

static int crystalhd_fw_research_bounded_read(struct crystalhd_cmd *ctx,
	u64 generation, u32 roots[2], u32 image_words[4], u32 *packet_words,
	u32 *attempted)
{
	struct crystalhd_hw *hw = ctx->hw_ctx;
	unsigned long flags;
	unsigned int i;
	BC_STATUS sts;
	int rc;

	rc = crystalhd_fw_research_state_context(ctx, generation, hw);
	if (rc)
		goto done;
	sts = crystalhd_hw_fw_cmd_enter(hw);
	rc = crystalhd_status_to_errno(sts);
	if (rc) {
		if (!READ_ONCE(ctx->adp->present))
			rc = -ENODEV;
		goto done;
	}
	/* Fixed C-relative fields between fresh roots, under one host fence.
	 * No observed image/packet field selects an address. Equal roots do not
	 * certify a live object, firmware coherence or freedom from ABA changes.
	 */
	spin_lock_irqsave(&hw->lock, flags);
	rc = crystalhd_fw_research_state_ready(ctx, generation, hw);
	if (rc)
		goto unlock;
	for (i = 0; i < (packet_words ? 5U : 3U); i++) {
		u32 offset = CRYSTALHD_FW_RESEARCH_CONTROLLER_ROOT_OFFSET, count = 1;
		u32 *destination = &roots[i != 0];

		if (i == 1) {
			if ((roots[0] & 3U) ||
			    (u64)roots[0] < CRYSTALHD_FW_RESEARCH_IMAGE_CONTEXT_MIN ||
			    (u64)roots[0] + CRYSTALHD_FW_RESEARCH_IMAGE_CONTEXT_BYTES >
			    CRYSTALHD_FW_RESEARCH_IMAGE_CONTEXT_END) {
				rc = -ERANGE;
				goto unlock;
			}
			offset = roots[0] + CRYSTALHD_FW_RESEARCH_IMAGE_TUPLE_OFFSET;
			count = 4;
			destination = image_words;
		} else if (packet_words && i == 2) {
			offset = roots[0] + CRYSTALHD_FW_RESEARCH_PACKET_ALIASES_OFFSET;
			count = 2;
			destination = packet_words;
		} else if (packet_words && i == 3) {
			offset = roots[0] + CRYSTALHD_FW_RESEARCH_PACKET_PHYSICAL_OFFSET;
			destination = &packet_words[2];
		}
		rc = crystalhd_fw_research_state_span(ctx->adp, offset, count);
		if (!rc)
			rc = crystalhd_fw_research_state_ready(ctx, generation, hw);
		if (rc)
			goto unlock;
		if (!i)
			*attempted = 1;
		sts = hw->pfnDevDRAMRead(hw, offset, count, destination);
		rc = crystalhd_fw_research_state_ready(ctx, generation, hw);
		if (!rc)
			rc = crystalhd_status_to_errno(sts);
		if (rc)
			goto unlock;
	}
	if (roots[0] != roots[1]) {
		rc = -ESTALE;
		goto unlock;
	}
unlock:
	spin_unlock_irqrestore(&hw->lock, flags);
	crystalhd_hw_fw_cmd_leave(hw);
done:
	return rc;
}

static int crystalhd_fw_research_image_sample(struct crystalhd_cmd *ctx,
	u64 generation, struct crystalhd_fw_research_image_sample *sample)
{
	u32 roots[2] = { }, words[4] = { };
	int rc;

	memset(sample, 0, sizeof(*sample));
	rc = crystalhd_fw_research_bounded_read(ctx, generation, roots, words,
					      NULL, &sample->attempted);
	if (!rc) {
		sample->root_before = roots[0];
		sample->root_after = roots[1];
		memcpy(sample->words, words, sizeof(words));
		sample->read_complete = 1;
	}
	sample->status = rc;
	return rc;
}

static int crystalhd_fw_research_packet_sample(struct crystalhd_cmd *ctx,
	u64 generation, struct crystalhd_fw_research_packet_sample *sample)
{
	u32 roots[2] = { }, image_words[4] = { }, packet_words[3] = { };
	int rc;

	memset(sample, 0, sizeof(*sample));
	rc = crystalhd_fw_research_bounded_read(ctx, generation, roots, image_words,
					      packet_words, &sample->attempted);
	if (!rc) {
		sample->root_before = roots[0];
		sample->root_after = roots[1];
		memcpy(sample->image_words, image_words, sizeof(image_words));
		memcpy(sample->packet_words, packet_words, sizeof(packet_words));
		sample->read_complete = 1;
	}
	sample->status = rc;
	return rc;
}

static int crystalhd_fw_research_heap_packet_sample(struct crystalhd_cmd *ctx,
	u64 generation, struct crystalhd_fw_research_heap_packet_sample *sample)
{
	struct crystalhd_fw_research_heap_packet_sample observed = { };
	struct crystalhd_hw *hw = ctx->hw_ctx;
	unsigned long flags;
	unsigned int i;
	BC_STATUS sts;
	int rc;

	memset(sample, 0, sizeof(*sample));
	rc = crystalhd_fw_research_state_context(ctx, generation, hw);
	if (rc)
		goto done;
	sts = crystalhd_hw_fw_cmd_enter(hw);
	rc = crystalhd_status_to_errno(sts);
	if (rc) {
		if (!READ_ONCE(ctx->adp->present))
			rc = -ENODEV;
		goto done;
	}
	/* Stock-initialized DDR and physical BAR reads are the premise. Compute
	 * only one fixed heap span from the independently bounded image base.
	 * Aliases, reply slots and header contents never choose an address.
	 * Equal brackets do not prove allocation lifetime, coherence or freshness.
	 */
	spin_lock_irqsave(&hw->lock, flags);
	rc = crystalhd_fw_research_state_ready(ctx, generation, hw);
	if (rc)
		goto unlock;
	for (i = 0; i < 11; i++) {
		u32 offset = CRYSTALHD_FW_RESEARCH_CONTROLLER_ROOT_OFFSET, count = 1;
		u32 *destination = &observed.root_before;

		switch (i) {
		case 0:
			break;
		case 1:
			if ((observed.root_before & 3U) ||
			    (u64)observed.root_before < CRYSTALHD_FW_RESEARCH_IMAGE_CONTEXT_MIN ||
			    (u64)observed.root_before + CRYSTALHD_FW_RESEARCH_IMAGE_CONTEXT_BYTES >
			    CRYSTALHD_FW_RESEARCH_IMAGE_CONTEXT_END) {
				rc = -ERANGE;
				goto unlock;
			}
			offset = observed.root_before + CRYSTALHD_FW_RESEARCH_IMAGE_TUPLE_OFFSET;
			count = 4;
			destination = observed.image_before;
			break;
		case 2:
			if (observed.image_before[0] != observed.image_before[1] ||
			    (observed.image_before[1] & 0xfffU) ||
			    (u64)observed.image_before[1] < CRYSTALHD_FW_RESEARCH_HEAP_IMAGE_MIN ||
			    (u64)observed.image_before[1] + CRYSTALHD_FW_RESEARCH_HEAP_IMAGE_BYTES >
			    CRYSTALHD_FW_RESEARCH_HEAP_END ||
			    observed.image_before[2] != CRYSTALHD_FW_RESEARCH_HEAP_IMAGE_BYTES ||
			    (observed.image_before[3] & 0xffU) != 1U) {
				rc = -ERANGE;
				goto unlock;
			}
			observed.packet_address = (u32)((u64)observed.image_before[1] +
				CRYSTALHD_FW_RESEARCH_HEAP_PACKET_OFFSET);
			offset = observed.root_before + CRYSTALHD_FW_RESEARCH_PACKET_ALIASES_OFFSET;
			count = 2;
			destination = observed.packet_before;
			break;
		case 3:
			offset = observed.root_before + CRYSTALHD_FW_RESEARCH_PACKET_PHYSICAL_OFFSET;
			destination = &observed.packet_before[2];
			break;
		case 4:
			if (observed.packet_before[0] != observed.packet_address ||
			    observed.packet_before[1] != observed.packet_address ||
			    observed.packet_before[2] != observed.packet_address) {
				rc = -ERANGE;
				goto unlock;
			}
			offset = observed.root_before + CRYSTALHD_FW_RESEARCH_REPLY_SLOTS_OFFSET;
			count = 2;
			destination = observed.slots_before;
			break;
		case 5:
			offset = observed.packet_address;
			/* Admit the whole fixed section independently of the smaller read. */
			if ((u64)offset + CRYSTALHD_FW_RESEARCH_HEAP_PACKET_BYTES >
			    CRYSTALHD_FW_RESEARCH_HEAP_END) {
				rc = -ERANGE;
				goto unlock;
			}
			rc = crystalhd_fw_research_state_span(ctx->adp, offset,
				CRYSTALHD_FW_RESEARCH_HEAP_PACKET_BYTES / sizeof(u32));
			if (rc)
				goto unlock;
			count = ARRAY_SIZE(observed.header_words);
			destination = observed.header_words;
			break;
		case 6:
			offset = observed.root_before + CRYSTALHD_FW_RESEARCH_REPLY_SLOTS_OFFSET;
			count = 2;
			destination = observed.slots_after;
			break;
		case 7:
			offset = observed.root_before + CRYSTALHD_FW_RESEARCH_PACKET_PHYSICAL_OFFSET;
			destination = &observed.packet_after[2];
			break;
		case 8:
			offset = observed.root_before + CRYSTALHD_FW_RESEARCH_PACKET_ALIASES_OFFSET;
			count = 2;
			destination = observed.packet_after;
			break;
		case 9:
			offset = observed.root_before + CRYSTALHD_FW_RESEARCH_IMAGE_TUPLE_OFFSET;
			count = 4;
			destination = observed.image_after;
			break;
		case 10:
			destination = &observed.root_after;
			break;
		}
		rc = crystalhd_fw_research_state_span(ctx->adp, offset, count);
		if (!rc)
			rc = crystalhd_fw_research_state_ready(ctx, generation, hw);
		if (rc)
			goto unlock;
		if (!i)
			sample->attempted = 1;
		sts = hw->pfnDevDRAMRead(hw, offset, count, destination);
		rc = crystalhd_fw_research_state_ready(ctx, generation, hw);
		if (!rc)
			rc = crystalhd_status_to_errno(sts);
		if (rc)
			goto unlock;
	}
	if (observed.root_before != observed.root_after ||
	    memcmp(observed.image_before, observed.image_after, sizeof(observed.image_before)) ||
	    memcmp(observed.packet_before, observed.packet_after, sizeof(observed.packet_before)) ||
	    memcmp(observed.slots_before, observed.slots_after, sizeof(observed.slots_before))) {
		rc = -ESTALE;
		goto unlock;
	}
	observed.attempted = 1;
	observed.read_complete = 1;
	*sample = observed;
unlock:
	spin_unlock_irqrestore(&hw->lock, flags);
	crystalhd_hw_fw_cmd_leave(hw);
done:
	sample->status = rc;
	return rc;
}

static int crystalhd_fw_research_clock_ready(struct crystalhd_cmd *ctx,
	u64 generation, struct crystalhd_hw *hw)
{
	int rc = crystalhd_fw_research_state_ready(ctx, generation, hw);

	if (rc)
		return rc;
	if (!hw->pfnReadDevRegister)
		return -ENODEV;
	if (!READ_ONCE(hw->dev_started))
		return -EAGAIN;
	return 0;
}

static int crystalhd_fw_research_clock_sample(struct crystalhd_cmd *ctx,
	u64 generation, struct crystalhd_fw_research_clock_sample *sample)
{
	static const u32 registers[] = {
		BCHP_MISC3_RESET_CTRL,
		BCHP_MISC_PERST_CLOCK_CTRL,
		BCHP_CLK_PM_CTRL,
	};
	struct crystalhd_hw *hw = ctx->hw_ctx;
	u32 values[ARRAY_SIZE(registers)];
	unsigned long flags;
	unsigned int i;
	BC_STATUS sts;
	int rc;

	memset(sample, 0, sizeof(*sample));
	rc = crystalhd_fw_research_state_context(ctx, generation, hw);
	if (rc)
		goto done;
	sts = crystalhd_hw_fw_cmd_enter(hw);
	rc = crystalhd_status_to_errno(sts);
	if (rc) {
		if (!READ_ONCE(ctx->adp->present))
			rc = -ENODEV;
		goto done;
	}
	/* Called only after this run's successful stock INIT/OPEN state check.
	 * The register accessor takes its innermost GISB lock; no clock/reset
	 * write, wake, retry or caller-selected address belongs to this sample.
	 */
	spin_lock_irqsave(&hw->lock, flags);
	rc = crystalhd_fw_research_clock_ready(ctx, generation, hw);
	if (rc)
		goto unlock;
	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		rc = crystalhd_fw_research_clock_ready(ctx, generation, hw);
		if (rc)
			goto unlock;
		sample->attempted = 1;
		values[i] = hw->pfnReadDevRegister(ctx->adp, registers[i]);
		rc = crystalhd_fw_research_clock_ready(ctx, generation, hw);
		if (rc)
			goto unlock;
	}
	rc = crystalhd_fw_research_clock_ready(ctx, generation, hw);
	if (!rc) {
		/* All bit patterns are raw observations, including zero/all ones. */
		sample->reset_ctrl = values[0];
		sample->perst_clock_ctrl = values[1];
		sample->clk_pm_ctrl = values[2];
		sample->read_complete = 1;
	}
unlock:
	spin_unlock_irqrestore(&hw->lock, flags);
	crystalhd_hw_fw_cmd_leave(hw);
done:
	sample->status = rc;
	return rc;
}

static void crystalhd_fw_research_run_internal(u64 generation,
	const struct crystalhd_fw_research_request *request,
	struct crystalhd_fw_research_result *result,
	struct crystalhd_fw_research_state_result *state,
	struct crystalhd_fw_research_controller_result *controller,
	struct crystalhd_fw_research_image_result *image,
	struct crystalhd_fw_research_packet_result *packet,
	struct crystalhd_fw_research_heap_packet_result *heap_packet,
	struct crystalhd_fw_research_clock_result *clock)
{
	struct crystalhd_device_access access;
	struct crystalhd_cmd *ctx;
	const struct firmware *firmware = NULL;
	bool session_attempted = false;
	bool hash_completed;
	BC_STATUS sts;
	u32 raw_command;
	int rc;

	memset(result, 0, sizeof(*result));
	if (state) {
		memset(&state->calibration, 0, sizeof(state->calibration));
		memset(&state->after_init, 0, sizeof(state->after_init));
		memset(&state->after_open, 0, sizeof(state->after_open));
	}
	if (controller) {
		memset(&controller->after_init, 0, sizeof(controller->after_init));
		memset(&controller->after_open, 0, sizeof(controller->after_open));
	}
	if (image) {
		memset(&image->after_init, 0, sizeof(image->after_init));
		memset(&image->after_open, 0, sizeof(image->after_open));
	}
	if (packet) {
		memset(&packet->after_init, 0, sizeof(packet->after_init));
		memset(&packet->after_open, 0, sizeof(packet->after_open));
	}
	if (heap_packet) {
		memset(&heap_packet->after_init, 0, sizeof(heap_packet->after_init));
		memset(&heap_packet->after_open, 0, sizeof(heap_packet->after_open));
	}
	if (clock) {
		memset(&clock->after_init, 0, sizeof(clock->after_init));
		memset(&clock->after_open, 0, sizeof(clock->after_open));
	}
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
	if (state) {
		if (access.adp->generation != generation) {
			rc = -ESTALE;
			goto exit;
		}
		rc = crystalhd_fw_research_state_apertures(access.adp);
		if (rc)
			goto exit;
	}
	rc = request_firmware(&firmware, CRYSTALHD_FLEA_FIRMWARE_NAME,
			      &access.adp->pdev->dev);
	if (rc)
		goto exit;
	rc = crystalhd_fw_research_hash(firmware, result->firmware_sha256,
				       &hash_completed);
	result->firmware_hash_valid = hash_completed;
	if (rc)
		goto release;
	if (state && firmware->size != CRYSTALHD_FW_RESEARCH_STATE_FIRMWARE_SIZE) {
		rc = -EINVAL;
		goto release;
	}
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
	if (state) {
		rc = crystalhd_fw_research_state_sample(ctx, generation,
						&state->calibration, true, false);
		if (rc)
			goto cleanup;
		rc = crystalhd_fw_research_state_sample(ctx, generation,
						&state->after_init, false, false);
		if (rc)
			goto cleanup;
		if (controller) {
			rc = crystalhd_fw_research_controller_sample(ctx, generation,
							&controller->after_init);
			if (rc)
				goto cleanup;
		}
		if (image) {
			rc = crystalhd_fw_research_image_sample(ctx, generation,
						      &image->after_init);
			if (rc)
				goto cleanup;
		}
		if (packet) {
			rc = crystalhd_fw_research_packet_sample(ctx, generation,
						       &packet->after_init);
			if (rc)
				goto cleanup;
		}
		if (heap_packet) {
			rc = crystalhd_fw_research_heap_packet_sample(ctx, generation,
							    &heap_packet->after_init);
			if (rc)
				goto cleanup;
		}
		if (clock) {
			rc = crystalhd_fw_research_clock_sample(ctx, generation,
						      &clock->after_init);
			if (rc)
				goto cleanup;
		}
	}
	raw_command = crystalhd_fw_research_raw_command(request->selector);
	if (raw_command) {
		rc = crystalhd_fw_research_command(ctx, result, raw_command);
		goto cleanup;
	}
	rc = crystalhd_fw_research_command(ctx, result, eCMD_C011_DEC_CHAN_OPEN);
	if (rc)
		goto cleanup;
	if (state) {
		rc = crystalhd_fw_research_state_sample(ctx, generation,
						&state->after_open, false, true);
		if (rc)
			goto cleanup;
		if (controller) {
			rc = crystalhd_fw_research_controller_sample(ctx, generation,
							&controller->after_open);
			if (rc)
				goto cleanup;
		}
		if (image) {
			rc = crystalhd_fw_research_image_sample(ctx, generation,
						      &image->after_open);
			if (rc)
				goto cleanup;
		}
		if (packet) {
			rc = crystalhd_fw_research_packet_sample(ctx, generation,
						       &packet->after_open);
			if (rc)
				goto cleanup;
		}
		if (heap_packet) {
			rc = crystalhd_fw_research_heap_packet_sample(ctx, generation,
								    &heap_packet->after_open);
			if (rc)
				goto cleanup;
		}
		if (clock) {
			rc = crystalhd_fw_research_clock_sample(ctx, generation,
						      &clock->after_open);
			if (rc)
				goto cleanup;
		}
	}
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

static void crystalhd_fw_research_run(u64 generation,
	const struct crystalhd_fw_research_request *request,
	struct crystalhd_fw_research_result *result)
{
	crystalhd_fw_research_run_internal(generation, request, result, NULL, NULL, NULL, NULL, NULL, NULL);
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
	struct crystalhd_fw_research_state_request state_request;
	struct crystalhd_fw_research_state_result *state_result;
	struct crystalhd_fw_research_controller_result *controller_result;
	struct crystalhd_fw_research_image_result *image_result;
	struct crystalhd_fw_research_packet_result *packet_result;
	struct crystalhd_fw_research_heap_packet_result *heap_packet_result;
	struct crystalhd_fw_research_clock_result *clock_result;
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
	if (command == CRYSTALHD_FW_RESEARCH_RUN_STATE) {
		if (copy_from_user(&state_request, user, sizeof(state_request)))
			return -EFAULT;
		if (!crystalhd_fw_research_state_request_valid(&state_request))
			return -EINVAL;
		state_result = kzalloc(sizeof(*state_result), GFP_KERNEL);
		if (!state_result)
			return -ENOMEM;
		state_result->request = state_request;
		memset(&request, 0, sizeof(request));
		request.version = CRYSTALHD_FW_RESEARCH_VERSION;
		request.size = sizeof(struct crystalhd_fw_research_result);
		request.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
		crystalhd_fw_research_run_internal(binding->generation, &request,
						 &state_result->control, state_result, NULL, NULL, NULL, NULL, NULL);
		if (copy_to_user(user, state_result, sizeof(*state_result)))
			rc = -EFAULT;
		kfree(state_result);
		return rc;
	}
	if (command == CRYSTALHD_FW_RESEARCH_RUN_CONTROLLER) {
		if (copy_from_user(&state_request, user, sizeof(state_request)))
			return -EFAULT;
		if (!crystalhd_fw_research_controller_request_valid(&state_request))
			return -EINVAL;
		controller_result = kzalloc(sizeof(*controller_result), GFP_KERNEL);
		if (!controller_result)
			return -ENOMEM;
		controller_result->state.request = state_request;
		memset(&request, 0, sizeof(request));
		request.version = CRYSTALHD_FW_RESEARCH_VERSION;
		request.size = sizeof(struct crystalhd_fw_research_result);
		request.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
		crystalhd_fw_research_run_internal(binding->generation, &request,
			&controller_result->state.control, &controller_result->state,
			controller_result, NULL, NULL, NULL, NULL);
		if (copy_to_user(user, controller_result, sizeof(*controller_result)))
			rc = -EFAULT;
		kfree(controller_result);
		return rc;
	}
	if (command == CRYSTALHD_FW_RESEARCH_RUN_IMAGE) {
		if (copy_from_user(&state_request, user, sizeof(state_request)))
			return -EFAULT;
		if (!crystalhd_fw_research_image_request_valid(&state_request))
			return -EINVAL;
		image_result = kzalloc(sizeof(*image_result), GFP_KERNEL);
		if (!image_result)
			return -ENOMEM;
		image_result->controller.state.request = state_request;
		memset(&request, 0, sizeof(request));
		request.version = CRYSTALHD_FW_RESEARCH_VERSION;
		request.size = sizeof(struct crystalhd_fw_research_result);
		request.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
		crystalhd_fw_research_run_internal(binding->generation, &request,
			&image_result->controller.state.control,
			&image_result->controller.state, &image_result->controller, image_result, NULL, NULL, NULL);
		if (copy_to_user(user, image_result, sizeof(*image_result)))
			rc = -EFAULT;
		kfree(image_result);
		return rc;
	}
	if (command == CRYSTALHD_FW_RESEARCH_RUN_PACKET) {
		if (copy_from_user(&state_request, user, sizeof(state_request)))
			return -EFAULT;
		if (!crystalhd_fw_research_packet_request_valid(&state_request))
			return -EINVAL;
		packet_result = kzalloc(sizeof(*packet_result), GFP_KERNEL);
		if (!packet_result)
			return -ENOMEM;
		packet_result->image.controller.state.request = state_request;
		memset(&request, 0, sizeof(request));
		request.version = CRYSTALHD_FW_RESEARCH_VERSION;
		request.size = sizeof(struct crystalhd_fw_research_result);
		request.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
		crystalhd_fw_research_run_internal(binding->generation, &request,
			&packet_result->image.controller.state.control,
			&packet_result->image.controller.state, &packet_result->image.controller,
			&packet_result->image, packet_result, NULL, NULL);
		if (copy_to_user(user, packet_result, sizeof(*packet_result)))
			rc = -EFAULT;
		kfree(packet_result);
		return rc;
	}
	if (command == CRYSTALHD_FW_RESEARCH_RUN_HEAP_PACKET) {
		if (copy_from_user(&state_request, user, sizeof(state_request)))
			return -EFAULT;
		if (!crystalhd_fw_research_heap_packet_request_valid(&state_request))
			return -EINVAL;
		heap_packet_result = kzalloc(sizeof(*heap_packet_result), GFP_KERNEL);
		if (!heap_packet_result)
			return -ENOMEM;
		heap_packet_result->image.controller.state.request = state_request;
		memset(&request, 0, sizeof(request));
		request.version = CRYSTALHD_FW_RESEARCH_VERSION;
		request.size = sizeof(struct crystalhd_fw_research_result);
		request.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
		crystalhd_fw_research_run_internal(binding->generation, &request,
			&heap_packet_result->image.controller.state.control,
			&heap_packet_result->image.controller.state, &heap_packet_result->image.controller,
			&heap_packet_result->image, NULL, heap_packet_result, NULL);
		if (copy_to_user(user, heap_packet_result, sizeof(*heap_packet_result)))
			rc = -EFAULT;
		kfree(heap_packet_result);
		return rc;
	}
	if (command == CRYSTALHD_FW_RESEARCH_RUN_CLOCK) {
		if (copy_from_user(&state_request, user, sizeof(state_request)))
			return -EFAULT;
		if (!crystalhd_fw_research_clock_request_valid(&state_request))
			return -EINVAL;
		clock_result = kzalloc(sizeof(*clock_result), GFP_KERNEL);
		if (!clock_result)
			return -ENOMEM;
		clock_result->state.request = state_request;
		memset(&request, 0, sizeof(request));
		request.version = CRYSTALHD_FW_RESEARCH_VERSION;
		request.size = sizeof(struct crystalhd_fw_research_result);
		request.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
		crystalhd_fw_research_run_internal(binding->generation, &request,
			&clock_result->state.control, &clock_result->state,
			NULL, NULL, NULL, NULL, clock_result);
		if (copy_to_user(user, clock_result, sizeof(*clock_result)))
			rc = -EFAULT;
		kfree(clock_result);
		return rc;
	}
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
