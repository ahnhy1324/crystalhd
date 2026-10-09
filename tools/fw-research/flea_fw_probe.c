// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#include "crystalhd_fw_research.h"

#define PROBE_DEVICE "/dev/crystalhd-fw-research"
#define CONTROLLER_ROOT_ADDRESS 0x000d3a08U
#define CONTROLLER_CANDIDATE_START 0x000d5384U
#define CONTROLLER_CANDIDATE_END 0x00116000U
#define CONTROLLER_CANDIDATE_BYTES 0x378U
#define IMAGE_CONTEXT_MIN 0x000d53dcU
#define IMAGE_TUPLE_OFFSET 0x1acU
#define PACKET_ALIASES_OFFSET 0x94U
#define PACKET_PHYSICAL_OFFSET 0x1ccU
#define HEAP_SLOTS_OFFSET 0x250U
#define HEAP_IMAGE_MIN 0x00117000U
#define HEAP_IMAGE_LIMIT 0x03ffc000U
#define HEAP_IMAGE_BYTES 0x00100000U
#define HEAP_PACKET_OFFSET 0x00070000U
#define HEAP_PACKET_SECTION_BYTES 256U
#define DRAM_LIMIT 0x04000000U
#define CLOCK_RESET_CTRL_ADDRESS 0x00502200U
#define CLOCK_PERST_CTRL_ADDRESS 0x0050229cU
#define CLOCK_PM_CTRL_ADDRESS 0x00070004U
#define UART_ARM_CTL_ADDRESS 0x000f3004U
#define UART_PIN_MUX_ADDRESS 0x00404100U
#define UART_ROUTER_ADDRESS 0x0040421cU
#define CRYPTO_SHARF_REVISION_ADDRESS 0x000f4000U
#define CRYPTO_SHARF_STATUS_ADDRESS 0x000f4004U
#define CRYPTO_BOP_GR_BRIDGE_REVISION_ADDRESS 0x00511000U
#define CRYPTO_BOP_AES_STATUS_ADDRESS 0x0051000cU
#define CRYPTO_GISB_ERROR_CAPTURE_ADDRESS 0x004000ccU
#define CRYPTO_GISB_ERROR_STATUS_ADDRESS 0x004000d4U
#define CRYPTO_GISB_ERROR_CAPTURE_MASTER 0x004000d8U
#define CRYPTO_GISB_ERROR_MASK 0x00001801U

static const uint32_t crypto_target_addresses[] = {
	CRYPTO_SHARF_REVISION_ADDRESS,
	CRYPTO_SHARF_STATUS_ADDRESS,
	CRYPTO_BOP_GR_BRIDGE_REVISION_ADDRESS,
	CRYPTO_BOP_AES_STATUS_ADDRESS,
};

enum action {
	ACTION_NONE, ACTION_INFO, ACTION_VERSION, ACTION_H264,
	ACTION_H261, ACTION_H263, ACTION_MPEG1,
	ACTION_SCALING_FILTERS, ACTION_PIC_CAPTURE, ACTION_SET_CSC,
	ACTION_SET_FGT, ACTION_CUSTOM_VIDOUT, ACTION_FILL_PIC_BUF,
	ACTION_FIXED_STATE, ACTION_CONTROLLER_ROOT, ACTION_CONTROLLER_IMAGE,
	ACTION_CONTROLLER_PACKET,
	ACTION_HEAP_PACKET,
	ACTION_CLOCK_STATE,
	ACTION_UART_STATE,
	ACTION_CRYPTO_STATE,
};

struct options {
	enum action action;
	bool acknowledge;
	bool generation_set;
	uint64_t generation;
};

static int output_finish(void)
{
	int failed = ferror(stdout);

	if (fflush(stdout) == EOF || ferror(stdout))
		failed = 1;
	return failed ? 1 : 0;
}

static void usage(FILE *stream)
{
	fputs("Usage: flea-fw-probe --info\n"
	      "       flea-fw-probe (--version | --h264-control | --h261-control |\n"
	      "         --h263-control | --mpeg1-control)\n"
	      "         --acknowledge-card-reset --expected-generation DECIMAL\n"
	      "       flea-fw-probe (--scaling-filters-command | --pic-capture-command |\n"
	      "         --csc-command | --fgt-command | --custom-vidout-command |\n"
	      "         --fill-pic-buf-command)\n"
	      "         --acknowledge-card-reset --expected-generation DECIMAL\n"
	      "       flea-fw-probe --fixed-state\n"
	      "         --acknowledge-card-reset --expected-generation DECIMAL\n"
	      "       flea-fw-probe --controller-root\n"
	      "         --acknowledge-card-reset --expected-generation DECIMAL\n"
	      "       flea-fw-probe --controller-image\n"
	      "         --acknowledge-card-reset --expected-generation DECIMAL\n"
	      "       flea-fw-probe --controller-packet\n"
	      "         --acknowledge-card-reset --expected-generation DECIMAL\n"
	      "       flea-fw-probe --heap-packet\n"
	      "         --acknowledge-card-reset --expected-generation DECIMAL\n"
	      "       flea-fw-probe --clock-state\n"
	      "         --acknowledge-card-reset --expected-generation DECIMAL\n"
	      "       flea-fw-probe --uart-state\n"
	      "         --acknowledge-card-reset --expected-generation DECIMAL\n"
	      "       flea-fw-probe --crypto-state\n"
	      "         --acknowledge-card-reset --expected-generation DECIMAL\n"
	      "       flea-fw-probe --help\n"
	      "\n"
	      "The live selectors reload firmware and reset an idle card. They do\n"
	      "not decode video or establish codec capability. Command-only probes\n"
	      "send one fixed zero-argument command after INIT and VERSION, without\n"
	      "opening or starting a decoder. A reply is not raw-processing support.\n"
	      "Fixed-state reads use stock INIT and H.264 OPEN; completed reads do\n"
	      "not certify cache coherence or backend ownership.\n"
	      "Controller-root reads add two fixed observations, never follow the\n"
	      "returned pointer, and do not establish ownership, coherence or a lease.\n"
	      "Controller-image reads add one bounded four-word tuple per stage;\n"
	      "tuple values are not followed and the low-byte owned declaration\n"
	      "is not evidence of ownership. Matching bracket roots do not exclude ABA.\n"
	      "Admission failures are diagnostic restrictions, not firmware rejection.\n"
	      "Controller-packet adds the image tuple and three packet fields within\n"
	      "one fresh root bracket. Returned values are not followed and do not\n"
	      "establish ownership, coherence, a lease or DMA suitability.\n"
	      "Heap-packet computes one admitted fixed DRAM span from the declared\n"
	      "image base. Header and stored reply words remain raw and are never\n"
	      "followed. Equal brackets do not certify freshness, object lifetime,\n"
	      "atomic coherence, queue validity or independent fetch-error detection.\n"
	      "Clock-state reads three fixed control registers after verified INIT\n"
	      "and OPEN, with no clock/reset writes. All bit patterns remain raw;\n"
	      "completed reads do not certify fetch errors or atomic coherence.\n"
	      "UART-state reads three fixed configuration registers after verified\n"
	      "INIT and OPEN, without status/FIFO reads or UART writes. Raw values\n"
	      "do not prove board pads, voltage, measured baud or console availability.\n"
	      "Crypto-state interleaves four fixed raw state-register reads after\n"
	      "verified INIT and OPEN with five GISB error-status reads. On the\n"
	      "first checked error it may also read raw capture address and master;\n"
	      "it never clears the capture or reads keys, contexts, IVs or nonces.\n"
	      "These observations do not establish cause or algorithm support.\n"
	      "Use --info first;\n"
	      "it reports metadata only, including the bound device generation.\n"
	      "The separate research device requires CAP_SYS_RAWIO and an opt-in\n"
	      "driver build. There are no retries, fallback devices or unloads.\n",
	      stream);
}

static bool decimal_generation(const char *text, uint64_t *generation)
{
	uint64_t value = 0;
	const unsigned char *p = (const unsigned char *)text;

	if (!*p)
		return false;
	for (; *p; p++) {
		unsigned int digit;

		if (*p < '0' || *p > '9')
			return false;
		digit = *p - '0';
		if (value > (UINT64_MAX - digit) / 10)
			return false;
		value = value * 10 + digit;
	}
	if (!value)
		return false;
	*generation = value;
	return true;
}

static bool parse_options(int argc, char **argv, struct options *options)
{
	int i;

	memset(options, 0, sizeof(*options));
	for (i = 1; i < argc; i++) {
		enum action action = ACTION_NONE;

		if (!strcmp(argv[i], "--info"))
			action = ACTION_INFO;
		else if (!strcmp(argv[i], "--version"))
			action = ACTION_VERSION;
		else if (!strcmp(argv[i], "--h264-control"))
			action = ACTION_H264;
		else if (!strcmp(argv[i], "--h261-control"))
			action = ACTION_H261;
		else if (!strcmp(argv[i], "--h263-control"))
			action = ACTION_H263;
		else if (!strcmp(argv[i], "--mpeg1-control"))
			action = ACTION_MPEG1;
		else if (!strcmp(argv[i], "--scaling-filters-command"))
			action = ACTION_SCALING_FILTERS;
		else if (!strcmp(argv[i], "--pic-capture-command"))
			action = ACTION_PIC_CAPTURE;
		else if (!strcmp(argv[i], "--csc-command"))
			action = ACTION_SET_CSC;
		else if (!strcmp(argv[i], "--fgt-command"))
			action = ACTION_SET_FGT;
		else if (!strcmp(argv[i], "--custom-vidout-command"))
			action = ACTION_CUSTOM_VIDOUT;
		else if (!strcmp(argv[i], "--fill-pic-buf-command"))
			action = ACTION_FILL_PIC_BUF;
		else if (!strcmp(argv[i], "--fixed-state"))
			action = ACTION_FIXED_STATE;
		else if (!strcmp(argv[i], "--controller-root"))
			action = ACTION_CONTROLLER_ROOT;
		else if (!strcmp(argv[i], "--controller-image"))
			action = ACTION_CONTROLLER_IMAGE;
		else if (!strcmp(argv[i], "--controller-packet"))
			action = ACTION_CONTROLLER_PACKET;
		else if (!strcmp(argv[i], "--heap-packet"))
			action = ACTION_HEAP_PACKET;
		else if (!strcmp(argv[i], "--clock-state"))
			action = ACTION_CLOCK_STATE;
		else if (!strcmp(argv[i], "--uart-state"))
			action = ACTION_UART_STATE;
		else if (!strcmp(argv[i], "--crypto-state"))
			action = ACTION_CRYPTO_STATE;
		else if (!strcmp(argv[i], "--acknowledge-card-reset")) {
			if (options->acknowledge)
				return false;
			options->acknowledge = true;
		} else if (!strcmp(argv[i], "--expected-generation")) {
			if (options->generation_set || ++i == argc ||
			    !decimal_generation(argv[i], &options->generation))
				return false;
			options->generation_set = true;
		} else {
			return false;
		}
		if (action != ACTION_NONE) {
			if (options->action != ACTION_NONE)
				return false;
			options->action = action;
		}
	}
	if (options->action == ACTION_INFO)
		return !options->acknowledge && !options->generation_set;
	return options->action != ACTION_NONE && options->acknowledge &&
		options->generation_set;
}

static void hex_digest(const __u8 digest[32], char hex[65])
{
	static const char digits[] = "0123456789abcdef";
	unsigned int i;

	for (i = 0; i < 32; i++) {
		hex[2 * i] = digits[digest[i] >> 4];
		hex[2 * i + 1] = digits[digest[i] & 15];
	}
	hex[64] = '\0';
}

static bool digest_matches(const __u8 digest[32])
{
	char hex[65];

	hex_digest(digest, hex);
	return !strcmp(hex, CRYSTALHD_FW_RESEARCH_FIRMWARE_SHA256);
}

static bool info_valid(const struct crystalhd_fw_research_info *info)
{
	unsigned int i;

	if (info->version != CRYSTALHD_FW_RESEARCH_VERSION ||
	    info->size != sizeof(*info) || !info->generation ||
	    (info->selector_mask & ~CRYSTALHD_FW_RESEARCH_SELECTOR_MASK) ||
	    !digest_matches(info->firmware_sha256))
		return false;
	for (i = 0; i < sizeof(info->reserved) / sizeof(info->reserved[0]); i++)
		if (info->reserved[i])
			return false;
	return true;
}

static bool status_valid(__u32 status)
{
	return status <= BC_STS_PWR_MGMT || status == (__u32)BC_STS_ERROR;
}

static __u32 raw_command(__u32 selector)
{
	/* Fixed wire values from include/7411d.h; never caller-provided. */
	switch (selector) {
	case CRYSTALHD_FW_RESEARCH_SCALING_FILTERS_COMMAND:
		return 0x7376310bU;
	case CRYSTALHD_FW_RESEARCH_PIC_CAPTURE_COMMAND:
		return 0x7376311cU;
	case CRYSTALHD_FW_RESEARCH_SET_CSC_COMMAND:
		return 0x73763180U;
	case CRYSTALHD_FW_RESEARCH_SET_FGT_COMMAND:
		return 0x73763182U;
	case CRYSTALHD_FW_RESEARCH_CUSTOM_VIDOUT_COMMAND:
		return 0x737631ffU;
	case CRYSTALHD_FW_RESEARCH_FILL_PIC_BUF_COMMAND:
		return 0x73763126U;
	default:
		return 0;
	}
}

static bool result_valid(const struct crystalhd_fw_research_result *result,
			 const struct crystalhd_fw_research_request *request,
			 uint64_t generation)
{
	static const __u32 channel_commands[] = {
		/* The fixed V1 command sequence, from include/7411d.h. */
		0x73763001U, 0x73763004U, 0x73763100U,
		0x73763103U, 0x73763101U,
	};
	static const __u32 open_only_commands[] = {
		0x73763001U, 0x73763004U, 0x73763100U, 0x73763101U,
	};
	__u32 raw_commands[] = {0x73763001U, 0x73763004U, 0};
	const __u32 *commands;
	unsigned int expected_count;
	unsigned int i, j;

	switch (request->selector) {
	case CRYSTALHD_FW_RESEARCH_VERSION_ONLY:
		commands = channel_commands;
		expected_count = 2;
		break;
	case CRYSTALHD_FW_RESEARCH_H264_CONTROL:
		commands = channel_commands;
		expected_count = 5;
		break;
	case CRYSTALHD_FW_RESEARCH_H261_CONTROL:
	case CRYSTALHD_FW_RESEARCH_H263_CONTROL:
	case CRYSTALHD_FW_RESEARCH_MPEG1_CONTROL:
		commands = open_only_commands;
		expected_count = 4;
		break;
	case CRYSTALHD_FW_RESEARCH_SCALING_FILTERS_COMMAND:
	case CRYSTALHD_FW_RESEARCH_PIC_CAPTURE_COMMAND:
	case CRYSTALHD_FW_RESEARCH_SET_CSC_COMMAND:
	case CRYSTALHD_FW_RESEARCH_SET_FGT_COMMAND:
	case CRYSTALHD_FW_RESEARCH_CUSTOM_VIDOUT_COMMAND:
	case CRYSTALHD_FW_RESEARCH_FILL_PIC_BUF_COMMAND:
		raw_commands[2] = raw_command(request->selector);
		commands = raw_commands;
		expected_count = 3;
		break;
	default:
		return false;
	}
	if (memcmp(&result->request, request, sizeof(*request)) ||
	    result->generation != generation || result->status > 0 ||
	    result->status < -4095 || result->firmware_hash_valid > 1 ||
	    result->download_attempted > 1 || result->cleanup_attempted > 1 ||
	    result->retained > 1 || result->command_count > expected_count ||
	    !status_valid(result->download_status) ||
	    !status_valid(result->cleanup_status))
		return false;
	if ((!result->download_attempted && result->command_count) ||
	    (result->download_attempted &&
	     (!result->firmware_hash_valid || !digest_matches(result->firmware_sha256))))
		return false;
	if (!result->firmware_hash_valid)
		for (i = 0; i < sizeof(result->firmware_sha256); i++)
			if (result->firmware_sha256[i])
				return false;
	for (i = 0; i < result->command_count; i++) {
		const struct crystalhd_fw_research_reply *reply = &result->replies[i];
		bool completed = reply->transport_status == BC_STS_SUCCESS ||
			reply->transport_status == BC_STS_FW_CMD_ERR;

		if (reply->command != commands[i] || reply->sequence != i + 1 ||
		    !status_valid(reply->transport_status) ||
		    reply->raw_response_valid != completed || reply->header_matches > 1)
			return false;
		if (!completed) {
			if (reply->header_matches)
				return false;
			for (j = 0; j < CRYSTALHD_FW_RESEARCH_RESPONSE_WORDS; j++)
				if (reply->response[j])
					return false;
		} else if (reply->header_matches !=
			   (reply->response[0] == reply->command &&
			    reply->response[1] == reply->sequence)) {
			return false;
		}
		/* Flea's completed transport-success path requires the fixed
		 * reply status word to be zero. Other values stay uninterpreted.
		 */
		if (completed && reply->transport_status == BC_STS_SUCCESS &&
		    reply->response[2])
			return false;
		/* V1 only follows OPEN on channel zero. Preserve a correctly
		 * stopped error record, but reject claimed success/follow-ons.
		 */
		if (reply->command == 0x73763100U && reply->transport_status == BC_STS_SUCCESS &&
		    reply->header_matches && reply->response[3] &&
		    (!result->status || i + 1 < result->command_count))
			return false;
		if ((i + 1 < result->command_count || !result->status) &&
		    (reply->transport_status != BC_STS_SUCCESS || !reply->header_matches))
			return false;
	}
	if (!result->status &&
	    (!result->download_attempted || result->download_status != BC_STS_SUCCESS ||
	     !result->cleanup_attempted || result->cleanup_status != BC_STS_SUCCESS ||
	     result->retained || result->command_count != expected_count))
		return false;
	return true;
}

static bool reply_succeeded(const struct crystalhd_fw_research_reply *reply)
{
	return reply->transport_status == BC_STS_SUCCESS &&
		reply->raw_response_valid && reply->header_matches &&
		!reply->response[2];
}

static bool state_sample_matches(const struct crystalhd_fw_research_state_sample *sample,
				 unsigned int stage)
{
	if (!stage)
		return sample->words[0] == 0x000d3a00U && !sample->words[1] &&
			!sample->words[2] && !sample->words[3];
	return sample->words[0] == 1 && sample->words[1] == 0x000d3a00U &&
		(sample->words[2] & 0xffU) == (stage == 2 ? 1U : 0U) &&
		(sample->words[3] & 0xffffffU) == (stage == 2 ? 0x200U : 0U);
}

static bool state_result_valid(const struct crystalhd_fw_research_state_result *result,
			       const struct crystalhd_fw_research_state_request *request,
			       uint64_t generation)
{
	const struct crystalhd_fw_research_state_sample *samples[] = {
		&result->calibration, &result->after_init, &result->after_open,
	};
	struct crystalhd_fw_research_request control_request = {
		.version = CRYSTALHD_FW_RESEARCH_VERSION,
		.size = sizeof(result->control),
		.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL,
	};
	unsigned int i, j;

	if (memcmp(&result->request, request, sizeof(*request)) ||
	    !result_valid(&result->control, &control_request, generation))
		return false;
	for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
		const struct crystalhd_fw_research_state_sample *sample = samples[i];

		if (sample->attempted > 1 || sample->read_complete > 1 || sample->reserved ||
		    sample->status > 0 || sample->status < -4095)
			return false;
		if (!i && (sample->words[1] || sample->words[2] || sample->words[3]))
			return false;
		if (!sample->attempted || !sample->read_complete) {
			if ((!sample->attempted && sample->read_complete) ||
			    (sample->attempted && !sample->status))
				return false;
			for (j = 0; j < sizeof(sample->words) / sizeof(sample->words[0]); j++)
				if (sample->words[j])
					return false;
		} else if ((sample->status != 0 && sample->status != -EPROTO) ||
			   (!sample->status != state_sample_matches(sample, i))) {
			return false;
		}
		if ((sample->attempted || sample->status) &&
		    (result->control.command_count < (i == 2 ? 3U : 2U) ||
		     !reply_succeeded(&result->control.replies[0]) ||
		     !reply_succeeded(&result->control.replies[1]) ||
		     (i && (!samples[i - 1]->attempted || samples[i - 1]->status)) ||
		     (i == 2 && (!reply_succeeded(&result->control.replies[2]) ||
				 result->control.replies[2].response[3]))))
			return false;
		if (sample->status &&
		    (result->control.status != sample->status ||
		     result->control.command_count != (i == 2 ? 3U : 2U)))
			return false;
	}
	if (result->control.command_count > 2 &&
	    (!result->calibration.attempted || result->calibration.status ||
	     !result->after_init.attempted || result->after_init.status))
		return false;
	if (result->control.command_count > 3 &&
	    (!result->after_open.attempted || result->after_open.status))
		return false;
	if (!result->control.status &&
	    (!result->calibration.attempted || !result->after_init.attempted ||
	     !result->after_open.attempted))
		return false;
	return true;
}

static bool state_sample_succeeded(const struct crystalhd_fw_research_state_sample *sample)
{
	return sample->attempted && sample->read_complete && !sample->status;
}

static bool clock_sample_succeeded(const struct crystalhd_fw_research_clock_sample *sample)
{
	return sample->attempted && sample->read_complete && !sample->status;
}

static bool clock_result_valid(const struct crystalhd_fw_research_clock_result *result,
	const struct crystalhd_fw_research_state_request *request, uint64_t generation)
{
	const struct crystalhd_fw_research_state_result *state = &result->state;
	const struct crystalhd_fw_research_result *control = &state->control;
	const struct crystalhd_fw_research_clock_sample *samples[] = {
		&result->after_init, &result->after_open,
	};
	const struct crystalhd_fw_research_state_sample *prerequisites[] = {
		&state->after_init, &state->after_open,
	};
	unsigned int i;

	if (!state_result_valid(state, request, generation) ||
	    (!control->download_attempted && control->download_status != BC_STS_SUCCESS) ||
	    (control->command_count && control->download_status != BC_STS_SUCCESS) ||
	    (control->cleanup_attempted &&
	     (!control->download_attempted || !control->firmware_hash_valid ||
	      !digest_matches(control->firmware_sha256))) ||
	    (!control->cleanup_attempted &&
	     (control->cleanup_status != BC_STS_SUCCESS &&
	      control->cleanup_status != BC_STS_CMD_CANCELLED)) ||
	    (!control->cleanup_attempted && control->download_attempted &&
	     control->cleanup_status != BC_STS_CMD_CANCELLED))
		return false;
	for (i = control->command_count; i < CRYSTALHD_FW_RESEARCH_MAX_COMMANDS; i++) {
		const struct crystalhd_fw_research_reply zero = { 0 };

		if (memcmp(&control->replies[i], &zero, sizeof(zero)))
			return false;
	}
	/* A successful calibration is immediately followed by the INIT sample. */
	if (state_sample_succeeded(&state->calibration) &&
	    !state->after_init.attempted && !state->after_init.status)
		return false;
	for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
		const struct crystalhd_fw_research_clock_sample *sample = samples[i];
		bool active = sample->attempted || sample->status || sample->read_complete;

		if (sample->attempted > 1 || sample->read_complete > 1 || sample->reserved ||
		    sample->status > 0 || sample->status < -4095 ||
		    active != state_sample_succeeded(prerequisites[i]))
			return false;
		if (sample->read_complete) {
			/* No register value, including zero/all ones, is a success gate. */
			if (!sample->attempted || sample->status)
				return false;
		} else if (sample->reset_ctrl || sample->perst_clock_ctrl || sample->clk_pm_ctrl ||
			   (sample->attempted && !sample->status)) {
			return false;
		}
		if (active && (!state_sample_succeeded(&state->calibration) ||
			       (i && !clock_sample_succeeded(samples[0]))))
			return false;
		if (sample->status &&
		    (control->status != sample->status || control->command_count != (i ? 3U : 2U)))
			return false;
	}
	if ((control->command_count > 2 && !clock_sample_succeeded(samples[0])) ||
	    (control->command_count > 3 && !clock_sample_succeeded(samples[1])) ||
	    (!control->status && (!clock_sample_succeeded(samples[0]) ||
				 !clock_sample_succeeded(samples[1]))))
		return false;
	return true;
}

static bool uart_sample_succeeded(const struct crystalhd_fw_research_uart_sample *sample)
{
	return sample->attempted && sample->read_complete && !sample->status;
}

static bool uart_result_valid(const struct crystalhd_fw_research_uart_result *result,
	const struct crystalhd_fw_research_state_request *request, uint64_t generation)
{
	const struct crystalhd_fw_research_state_result *state = &result->state;
	const struct crystalhd_fw_research_result *control = &state->control;
	const struct crystalhd_fw_research_uart_sample *samples[] = {
		&result->after_init, &result->after_open,
	};
	const struct crystalhd_fw_research_state_sample *prerequisites[] = {
		&state->after_init, &state->after_open,
	};
	unsigned int i;

	if (!state_result_valid(state, request, generation) ||
	    (!control->download_attempted && control->download_status != BC_STS_SUCCESS) ||
	    (control->command_count && control->download_status != BC_STS_SUCCESS) ||
	    (control->cleanup_attempted &&
	     (!control->download_attempted || !control->firmware_hash_valid ||
	      !digest_matches(control->firmware_sha256))) ||
	    (!control->cleanup_attempted &&
	     (control->cleanup_status != BC_STS_SUCCESS &&
	      control->cleanup_status != BC_STS_CMD_CANCELLED)) ||
	    (!control->cleanup_attempted && control->download_attempted &&
	     control->cleanup_status != BC_STS_CMD_CANCELLED))
		return false;
	for (i = control->command_count; i < CRYSTALHD_FW_RESEARCH_MAX_COMMANDS; i++) {
		const struct crystalhd_fw_research_reply zero = { 0 };

		if (memcmp(&control->replies[i], &zero, sizeof(zero)))
			return false;
	}
	if (state_sample_succeeded(&state->calibration) &&
	    !state->after_init.attempted && !state->after_init.status)
		return false;
	for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
		const struct crystalhd_fw_research_uart_sample *sample = samples[i];
		bool active = sample->attempted || sample->status || sample->read_complete;

		if (sample->attempted > 1 || sample->read_complete > 1 || sample->reserved ||
		    sample->status > 0 || sample->status < -4095 ||
		    active != state_sample_succeeded(prerequisites[i]))
			return false;
		if (sample->read_complete) {
			/* Every raw configuration bit pattern is admissible. */
			if (!sample->attempted || sample->status)
				return false;
		} else if (sample->arm_uart_ctl || sample->pin_mux_ctrl_0 || sample->uart_router_sel ||
			   (sample->attempted && !sample->status)) {
			return false;
		}
		if (active && (!state_sample_succeeded(&state->calibration) ||
			       (i && !uart_sample_succeeded(samples[0]))))
			return false;
		if (sample->status &&
		    (control->status != sample->status || control->command_count != (i ? 3U : 2U)))
			return false;
	}
	if ((control->command_count > 2 && !uart_sample_succeeded(samples[0])) ||
	    (control->command_count > 3 && !uart_sample_succeeded(samples[1])) ||
	    (!control->status && (!uart_sample_succeeded(samples[0]) ||
				 !uart_sample_succeeded(samples[1]))))
		return false;
	return true;
}

static bool crypto_sample_succeeded(
	const struct crystalhd_fw_research_crypto_sample *sample)
{
	return sample->attempted && sample->read_complete && !sample->status;
}

static bool crypto_result_valid(
	const struct crystalhd_fw_research_crypto_result *result,
	const struct crystalhd_fw_research_state_request *request, uint64_t generation)
{
	const struct crystalhd_fw_research_state_result *state = &result->state;
	const struct crystalhd_fw_research_result *control = &state->control;
	const struct crystalhd_fw_research_crypto_sample *samples[] = {
		&result->after_init, &result->after_open,
	};
	const struct crystalhd_fw_research_state_sample *prerequisites[] = {
		&state->after_init, &state->after_open,
	};
	unsigned int i;

	if (!state_result_valid(state, request, generation) ||
	    (!control->download_attempted && control->download_status != BC_STS_SUCCESS) ||
	    (control->command_count && control->download_status != BC_STS_SUCCESS) ||
	    (control->cleanup_attempted &&
	     (!control->download_attempted || !control->firmware_hash_valid ||
	      !digest_matches(control->firmware_sha256))) ||
	    (!control->cleanup_attempted &&
	     (control->cleanup_status != BC_STS_SUCCESS &&
	      control->cleanup_status != BC_STS_CMD_CANCELLED)) ||
	    (!control->cleanup_attempted && control->download_attempted &&
	     control->cleanup_status != BC_STS_CMD_CANCELLED))
		return false;
	for (i = control->command_count; i < CRYSTALHD_FW_RESEARCH_MAX_COMMANDS; i++) {
		const struct crystalhd_fw_research_reply zero = { 0 };

		if (memcmp(&control->replies[i], &zero, sizeof(zero)))
			return false;
	}
	if (state_sample_succeeded(&state->calibration) &&
	    !state->after_init.attempted && !state->after_init.status)
		return false;
	for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
		const struct crystalhd_fw_research_crypto_sample *sample = samples[i];
		bool active = sample->attempted || sample->status || sample->read_complete ||
			sample->target_reads_attempted || sample->guard_reads_complete ||
			sample->gisb_before || sample->gisb_last ||
			sample->error_capture_complete || sample->error_capture_address ||
			sample->error_capture_master;
		bool guard_error = sample->guard_reads_complete &&
			(sample->gisb_last & CRYPTO_GISB_ERROR_MASK);

		if (sample->attempted > 1 || sample->read_complete > 1 ||
		    sample->error_capture_complete > 1 ||
		    sample->status > 0 || sample->status < -4095 ||
		    sample->target_reads_attempted > 4 || sample->guard_reads_complete > 5 ||
		    !!sample->attempted != !!sample->target_reads_attempted ||
		    sample->guard_reads_complete < sample->target_reads_attempted ||
		    sample->guard_reads_complete > sample->target_reads_attempted + 1 ||
		    (!sample->guard_reads_complete && (sample->gisb_before || sample->gisb_last)) ||
		    (sample->guard_reads_complete == 1 &&
		     sample->gisb_before != sample->gisb_last) ||
		    (sample->guard_reads_complete > 1 &&
		     (sample->gisb_before & CRYPTO_GISB_ERROR_MASK)) ||
		    (guard_error &&
		     (sample->guard_reads_complete != sample->target_reads_attempted + 1 ||
		      sample->status != -EIO)) ||
		    (sample->error_capture_complete && !guard_error) ||
		    (!sample->error_capture_complete &&
		     (sample->error_capture_address || sample->error_capture_master)) ||
		    active != state_sample_succeeded(prerequisites[i]))
			return false;
		if (sample->read_complete) {
			/* Every raw state bit pattern is admissible. */
			if (!sample->attempted || sample->status ||
			    sample->target_reads_attempted != 4 ||
			    sample->guard_reads_complete != 5 || guard_error)
				return false;
		} else if (sample->sharf_revision || sample->sharf_status ||
			   sample->bop_gr_bridge_revision || sample->bop_aes_status ||
			   (sample->target_reads_attempted == 4 &&
			    sample->guard_reads_complete == 5 && !guard_error) ||
			   (active && !sample->status)) {
			return false;
		}
		if (active && (!state_sample_succeeded(&state->calibration) ||
			       (i && !crypto_sample_succeeded(samples[0]))))
			return false;
		if (sample->status &&
		    (control->status != sample->status || control->command_count != (i ? 3U : 2U)))
			return false;
	}
	if ((control->command_count > 2 && !crypto_sample_succeeded(samples[0])) ||
	    (control->command_count > 3 && !crypto_sample_succeeded(samples[1])) ||
	    (!control->status && (!crypto_sample_succeeded(samples[0]) ||
				 !crypto_sample_succeeded(samples[1]))))
		return false;
	return true;
}

static bool controller_sample_succeeded(
	const struct crystalhd_fw_research_controller_sample *sample)
{
	return sample->attempted && sample->read_complete && !sample->status;
}

static bool controller_result_valid(
	const struct crystalhd_fw_research_controller_result *result,
	const struct crystalhd_fw_research_state_request *request, uint64_t generation)
{
	const struct crystalhd_fw_research_state_result *state = &result->state;
	const struct crystalhd_fw_research_result *control = &state->control;
	const struct crystalhd_fw_research_controller_sample *samples[] = {
		&result->after_init, &result->after_open,
	};
	const struct crystalhd_fw_research_state_sample *prerequisites[] = {
		&state->after_init, &state->after_open,
	};
	unsigned int i;

	if (!state_result_valid(state, request, generation) ||
	    (!control->download_attempted && control->download_status != BC_STS_SUCCESS) ||
	    (control->command_count && control->download_status != BC_STS_SUCCESS) ||
	    (control->cleanup_attempted &&
	     (!control->download_attempted || !control->firmware_hash_valid ||
	      !digest_matches(control->firmware_sha256))) ||
	    (!control->cleanup_attempted &&
	     (control->cleanup_status != BC_STS_SUCCESS &&
	      control->cleanup_status != BC_STS_CMD_CANCELLED)) ||
	    (!control->cleanup_attempted && control->download_attempted &&
	     control->cleanup_status != BC_STS_CMD_CANCELLED))
		return false;
	/* The kernel zeroes unused reply storage. Do not accept forged follow-on
	 * bytes hidden behind a shorter command_count in this additive result.
	 */
	for (i = control->command_count; i < CRYSTALHD_FW_RESEARCH_MAX_COMMANDS; i++) {
		const struct crystalhd_fw_research_reply zero = { 0 };

		if (memcmp(&control->replies[i], &zero, sizeof(zero)))
			return false;
	}
	for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
		const struct crystalhd_fw_research_controller_sample *sample = samples[i];
		bool active = sample->attempted || sample->status || sample->read_complete;

		if (sample->attempted > 1 || sample->read_complete > 1 ||
		    sample->status > 0 || sample->status < -4095 ||
		    active != state_sample_succeeded(prerequisites[i]))
			return false;
		if (sample->read_complete) {
			/* Numeric root values are observations, never protocol failures. */
			if (!sample->attempted || sample->status)
				return false;
		} else if (sample->root || (sample->attempted && !sample->status)) {
			return false;
		}
		if (active && (!state_sample_succeeded(&state->calibration) ||
			       (i && !controller_sample_succeeded(samples[0]))))
			return false;
		if (sample->status &&
		    (control->status != sample->status || control->command_count != (i ? 3U : 2U)))
			return false;
	}
	if ((control->command_count > 2 && !controller_sample_succeeded(samples[0])) ||
	    (control->command_count > 3 && !controller_sample_succeeded(samples[1])) ||
	    (!control->status && (!controller_sample_succeeded(samples[0]) ||
				 !controller_sample_succeeded(samples[1]))))
		return false;
	return true;
}

static bool image_sample_succeeded(const struct crystalhd_fw_research_image_sample *sample)
{
	return sample->attempted && sample->read_complete && !sample->status;
}

static bool image_result_valid(const struct crystalhd_fw_research_image_result *result,
	const struct crystalhd_fw_research_state_request *request, uint64_t generation)
{
	const struct crystalhd_fw_research_result *control = &result->controller.state.control;
	const struct crystalhd_fw_research_image_sample *samples[] = {&result->after_init, &result->after_open};
	const struct crystalhd_fw_research_controller_sample *prerequisites[] = {
		&result->controller.after_init, &result->controller.after_open,
	};
	unsigned int i, j;

	if (!controller_result_valid(&result->controller, request, generation))
		return false;
	for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
		const struct crystalhd_fw_research_image_sample *sample = samples[i];
		bool active = sample->attempted || sample->status || sample->read_complete;

		if (sample->attempted > 1 || sample->read_complete > 1 || sample->reserved ||
		    sample->status > 0 || sample->status < -4095 ||
		    active != controller_sample_succeeded(prerequisites[i]))
			return false;
		if (sample->read_complete) {
			uint64_t root = sample->root_before;
			uint64_t tuple = root + IMAGE_TUPLE_OFFSET;

			if (!sample->attempted || sample->status || sample->root_before != sample->root_after ||
			    (sample->root_before & 3U) || root < IMAGE_CONTEXT_MIN ||
			    root + CONTROLLER_CANDIDATE_BYTES > CONTROLLER_CANDIDATE_END ||
			    (tuple & 0xffffU) + sizeof(sample->words) > 0x10000U)
				return false;
			/* Tuple values, including every byte of word3, are raw observations.
			 * This bracket is independent of the earlier controller-root read.
			 */
		} else {
			if (sample->root_before || sample->root_after || (sample->attempted && !sample->status))
				return false;
			for (j = 0; j < sizeof(sample->words) / sizeof(sample->words[0]); j++)
				if (sample->words[j])
					return false;
		}
		if (active && i && !image_sample_succeeded(samples[0]))
			return false;
		if (sample->status && (control->status != sample->status || control->command_count != (i ? 3U : 2U)))
			return false;
	}
	if ((control->command_count > 2 && !image_sample_succeeded(samples[0])) ||
	    (control->command_count > 3 && !image_sample_succeeded(samples[1])) ||
	    (!control->status && (!image_sample_succeeded(samples[0]) || !image_sample_succeeded(samples[1]))))
		return false;
	return true;
}

static bool packet_sample_succeeded(const struct crystalhd_fw_research_packet_sample *sample)
{
	return sample->attempted && sample->read_complete && !sample->status;
}

static bool packet_result_valid(const struct crystalhd_fw_research_packet_result *result,
	const struct crystalhd_fw_research_state_request *request, uint64_t generation)
{
	const struct crystalhd_fw_research_result *control = &result->image.controller.state.control;
	const struct crystalhd_fw_research_packet_sample *samples[] = {&result->after_init, &result->after_open};
	const struct crystalhd_fw_research_image_sample *prerequisites[] = {
		&result->image.after_init, &result->image.after_open,
	};
	unsigned int i, j;

	if (!image_result_valid(&result->image, request, generation))
		return false;
	for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
		const struct crystalhd_fw_research_packet_sample *sample = samples[i];
		bool active = sample->attempted || sample->status || sample->read_complete;

		if (sample->attempted > 1 || sample->read_complete > 1 || sample->reserved ||
		    sample->status > 0 || sample->status < -4095 ||
		    active != image_sample_succeeded(prerequisites[i]))
			return false;
		if (sample->read_complete) {
			uint64_t root = sample->root_before;
			uint64_t image = root + IMAGE_TUPLE_OFFSET;
			uint64_t aliases = root + PACKET_ALIASES_OFFSET;
			uint64_t physical = root + PACKET_PHYSICAL_OFFSET;

			if (!sample->attempted || sample->status || sample->root_before != sample->root_after ||
			    (sample->root_before & 3U) || root < IMAGE_CONTEXT_MIN ||
			    root + CONTROLLER_CANDIDATE_BYTES > CONTROLLER_CANDIDATE_END ||
			    (image & 0xffffU) + sizeof(sample->image_words) > 0x10000U ||
			    (aliases & 0xffffU) + 2U * sizeof(uint32_t) > 0x10000U ||
			    (physical & 0xffffU) + sizeof(uint32_t) > 0x10000U)
				return false;
			/* Values are raw, independent of every preceding root and tuple.
			 * Equal bracket roots do not prove lifetime or exclude ABA.
			 */
		} else {
			if (sample->root_before || sample->root_after || (sample->attempted && !sample->status))
				return false;
			for (j = 0; j < sizeof(sample->image_words) / sizeof(sample->image_words[0]); j++)
				if (sample->image_words[j])
					return false;
			for (j = 0; j < sizeof(sample->packet_words) / sizeof(sample->packet_words[0]); j++)
				if (sample->packet_words[j])
					return false;
		}
		if (active && i && !packet_sample_succeeded(samples[0]))
			return false;
		if (sample->status && (control->status != sample->status || control->command_count != (i ? 3U : 2U)))
			return false;
	}
	if ((control->command_count > 2 && !packet_sample_succeeded(samples[0])) ||
	    (control->command_count > 3 && !packet_sample_succeeded(samples[1])) ||
	    (!control->status && (!packet_sample_succeeded(samples[0]) || !packet_sample_succeeded(samples[1]))))
		return false;
	return true;
}

static bool heap_span_valid(uint64_t address, uint64_t bytes)
{
	return !(address & 3U) && bytes && address < DRAM_LIMIT &&
		bytes <= DRAM_LIMIT - address &&
		(address & 0xffffU) + bytes <= 0x10000U;
}

static bool heap_packet_sample_succeeded(
	const struct crystalhd_fw_research_heap_packet_sample *sample)
{
	return sample->attempted && sample->read_complete && !sample->status;
}

static bool heap_packet_result_valid(const struct crystalhd_fw_research_heap_packet_result *result,
	const struct crystalhd_fw_research_state_request *request, uint64_t generation)
{
	const struct crystalhd_fw_research_result *control = &result->image.controller.state.control;
	const struct crystalhd_fw_research_heap_packet_sample *samples[] = {&result->after_init, &result->after_open};
	const struct crystalhd_fw_research_image_sample *prerequisites[] = {
		&result->image.after_init, &result->image.after_open,
	};
	unsigned int i;

	if (!image_result_valid(&result->image, request, generation))
		return false;
	for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
		const struct crystalhd_fw_research_heap_packet_sample *sample = samples[i];
		bool active = sample->attempted || sample->status || sample->read_complete;

		if (sample->attempted > 1 || sample->read_complete > 1 || sample->reserved ||
		    sample->status > 0 || sample->status < -4095 ||
		    active != image_sample_succeeded(prerequisites[i]))
			return false;
		if (sample->read_complete) {
			uint64_t root = sample->root_before;
			uint64_t base = sample->image_before[1];
			uint64_t target = base + HEAP_PACKET_OFFSET;

			if (!sample->attempted || sample->status || sample->root_before != sample->root_after ||
			    (root & 3U) || root < IMAGE_CONTEXT_MIN ||
			    root + CONTROLLER_CANDIDATE_BYTES > CONTROLLER_CANDIDATE_END ||
			    !heap_span_valid(root + IMAGE_TUPLE_OFFSET, sizeof(sample->image_before)) ||
			    !heap_span_valid(root + PACKET_ALIASES_OFFSET, 2U * sizeof(uint32_t)) ||
			    !heap_span_valid(root + PACKET_PHYSICAL_OFFSET, sizeof(uint32_t)) ||
			    !heap_span_valid(root + HEAP_SLOTS_OFFSET, sizeof(sample->slots_before)) ||
			    sample->image_before[0] != base || (base & 0xfffU) || base < HEAP_IMAGE_MIN ||
			    base + HEAP_IMAGE_BYTES > HEAP_IMAGE_LIMIT ||
			    sample->image_before[2] != HEAP_IMAGE_BYTES || (sample->image_before[3] & 0xffU) != 1U ||
			    !heap_span_valid(target, HEAP_PACKET_SECTION_BYTES) ||
			    !heap_span_valid(target, sizeof(sample->header_words)) ||
			    sample->packet_address != target || sample->packet_before[0] != target ||
			    sample->packet_before[1] != target || sample->packet_before[2] != target ||
			    memcmp(sample->image_before, sample->image_after, sizeof(sample->image_before)) ||
			    memcmp(sample->packet_before, sample->packet_after, sizeof(sample->packet_before)) ||
			    memcmp(sample->slots_before, sample->slots_after, sizeof(sample->slots_before)))
				return false;
			/* Only this fresh bracket admits the B-derived fixed physical span.
			 * Header/stored reply values are raw; none select another access.
			 * Earlier embedded roots/tuples are not numeric admission inputs.
			 */
		} else {
			const struct crystalhd_fw_research_heap_packet_sample zero = { 0 };
			struct crystalhd_fw_research_heap_packet_sample cleared = *sample;

			cleared.attempted = 0;
			cleared.status = 0;
			if ((sample->attempted && !sample->status) || memcmp(&cleared, &zero, sizeof(zero)))
				return false;
		}
		if (active && i && !heap_packet_sample_succeeded(samples[0]))
			return false;
		if (sample->status && (control->status != sample->status || control->command_count != (i ? 3U : 2U)))
			return false;
	}
	if ((control->command_count > 2 && !heap_packet_sample_succeeded(samples[0])) ||
	    (control->command_count > 3 && !heap_packet_sample_succeeded(samples[1])) ||
	    (!control->status && (!heap_packet_sample_succeeded(samples[0]) || !heap_packet_sample_succeeded(samples[1]))))
		return false;
	return true;
}

static void print_info(const struct crystalhd_fw_research_info *info)
{
	char hex[65];

	hex_digest(info->firmware_sha256, hex);
	printf("{\"version\":%" PRIu32 ",\"generation\":\"%" PRIu64 "\","
	       "\"research_selector_mask\":%" PRIu32 ","
	       "\"expected_firmware_sha256\":\"%s\"}\n",
	       (uint32_t)info->version, (uint64_t)info->generation,
	       (uint32_t)info->selector_mask, hex);
}

static void print_decoded_response(const struct crystalhd_fw_research_reply *reply)
{
	/* C011RspGetVersion (include/7411d.h): these three fields are also
	 * populated by the pinned firmware. Its STATUS handler is ACK-only.
	 */
	if (reply->command != 0x73763004U || !reply->raw_response_valid ||
	    !reply->header_matches || reply->response[0] != reply->command ||
	    reply->response[1] != reply->sequence ||
	    reply->transport_status != BC_STS_SUCCESS || reply->response[2]) {
		fputs("null", stdout);
		return;
	}
	printf("{\"stream_firmware_version\":%" PRIu32 ","
	       "\"decoder_firmware_version\":%" PRIu32 ","
	       "\"firmware_reported_chip_hw_version\":%" PRIu32 "}",
	       (uint32_t)reply->response[3], (uint32_t)reply->response[4],
	       (uint32_t)reply->response[5]);
}

static void print_result_object(const struct crystalhd_fw_research_result *result)
{
	char hex[65];
	unsigned int i, j;

	hex_digest(result->firmware_sha256, hex);
	printf("{\"version\":%" PRIu32 ",\"selector\":%" PRIu32 ","
	       "\"generation\":\"%" PRIu64 "\",\"status\":%" PRId32 ","
	       "\"expected_firmware_sha256\":\"%s\","
	       "\"firmware_hash_valid\":%s,\"observed_firmware_sha256\":",
	       (uint32_t)result->request.version, (uint32_t)result->request.selector,
	       (uint64_t)result->generation, (int32_t)result->status,
	       CRYSTALHD_FW_RESEARCH_FIRMWARE_SHA256,
	       result->firmware_hash_valid ? "true" : "false");
	if (result->firmware_hash_valid)
		printf("\"%s\"", hex);
	else
		fputs("null", stdout);
	printf(","
	       "\"download_attempted\":%s,\"download_status\":%" PRIu32 ","
	       "\"cleanup_attempted\":%s,\"cleanup_status\":%" PRIu32 ","
	       "\"retained\":%s,\"command_count\":%" PRIu32 ",\"replies\":[",
	       result->download_attempted ? "true" : "false",
	       (uint32_t)result->download_status,
	       result->cleanup_attempted ? "true" : "false",
	       (uint32_t)result->cleanup_status, result->retained ? "true" : "false",
	       (uint32_t)result->command_count);
	for (i = 0; i < result->command_count; i++) {
		const struct crystalhd_fw_research_reply *reply = &result->replies[i];

		printf("%s{\"command\":%" PRIu32 ",\"sequence\":%" PRIu32 ","
		       "\"transport_status\":%" PRIu32 ",\"raw_response_valid\":%s,"
		       "\"header_matches\":%s,\"raw_response_words\":",
		       i ? "," : "", (uint32_t)reply->command, (uint32_t)reply->sequence,
		       (uint32_t)reply->transport_status,
		       reply->raw_response_valid ? "true" : "false",
		       reply->header_matches ? "true" : "false");
		if (!reply->raw_response_valid) {
			fputs("null", stdout);
		} else {
			putchar('[');
			for (j = 0; j < CRYSTALHD_FW_RESEARCH_RESPONSE_WORDS; j++)
				printf("%s%" PRIu32, j ? "," : "", (uint32_t)reply->response[j]);
			putchar(']');
		}
		fputs(",\"decoded_response\":", stdout);
		print_decoded_response(reply);
		putchar('}');
	}
	fputs("]}", stdout);
}

static void print_result(const struct crystalhd_fw_research_result *result)
{
	print_result_object(result);
	putchar('\n');
}

static void print_state_sample(const struct crystalhd_fw_research_state_sample *sample)
{
	unsigned int i;

	printf("{\"attempted\":%s,\"status\":%" PRId32 ",\"read_complete\":%s,\"raw_words\":[",
	       sample->attempted ? "true" : "false", (int32_t)sample->status,
	       sample->read_complete ? "true" : "false");
	for (i = 0; i < sizeof(sample->words) / sizeof(sample->words[0]); i++)
		printf("%s%" PRIu32, i ? "," : "", (uint32_t)sample->words[i]);
	fputs("]}", stdout);
}

static void print_state_result(const struct crystalhd_fw_research_state_result *result)
{
	printf("{\"version\":%" PRIu32 ",\"fixed_state\":true,\"control\":",
	       (uint32_t)result->request.version);
	print_result_object(&result->control);
	fputs(",\"samples\":{\"calibration\":", stdout);
	print_state_sample(&result->calibration);
	fputs(",\"after_init\":", stdout);
	print_state_sample(&result->after_init);
	fputs(",\"after_open\":", stdout);
	print_state_sample(&result->after_open);
	fputs("}}\n", stdout);
}

static void print_clock_sample(const struct crystalhd_fw_research_clock_sample *sample)
{
	printf("{\"attempted\":%s,\"status\":%" PRId32 ",\"read_complete\":%s,\"reset_ctrl\":",
	       sample->attempted ? "true" : "false", (int32_t)sample->status,
	       sample->read_complete ? "true" : "false");
	if (sample->read_complete)
		printf("%" PRIu32 ",\"perst_clock_ctrl\":%" PRIu32 ",\"clk_pm_ctrl\":%" PRIu32,
		       (uint32_t)sample->reset_ctrl, (uint32_t)sample->perst_clock_ctrl,
		       (uint32_t)sample->clk_pm_ctrl);
	else
		fputs("null,\"perst_clock_ctrl\":null,\"clk_pm_ctrl\":null", stdout);
	putchar('}');
}

static void print_clock_result(const struct crystalhd_fw_research_clock_result *result)
{
	printf("{\"version\":%" PRIu32 ",\"clock_state\":true,\"control\":",
	       (uint32_t)result->state.request.version);
	print_result_object(&result->state.control);
	fputs(",\"fixed_state_samples\":{\"calibration\":", stdout);
	print_state_sample(&result->state.calibration);
	fputs(",\"after_init\":", stdout);
	print_state_sample(&result->state.after_init);
	fputs(",\"after_open\":", stdout);
	print_state_sample(&result->state.after_open);
	fputs("},\"clock_samples\":{\"after_init\":", stdout);
	print_clock_sample(&result->after_init);
	fputs(",\"after_open\":", stdout);
	print_clock_sample(&result->after_open);
	printf("},\"scope\":{\"register_addresses\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 "],"
	       "\"reads_per_sample\":3,\"maximum_register_reads\":6,\"raw_values_only\":true,"
	       "\"sample_clock_reset_writes\":false,\"independent_fetch_errors_certified\":false,"
	       "\"atomic_coherence_established\":false}}\n",
	       CLOCK_RESET_CTRL_ADDRESS, CLOCK_PERST_CTRL_ADDRESS, CLOCK_PM_CTRL_ADDRESS);
}

static void print_uart_sample(const struct crystalhd_fw_research_uart_sample *sample)
{
	printf("{\"attempted\":%s,\"status\":%" PRId32 ",\"read_complete\":%s,\"arm_uart_ctl\":",
	       sample->attempted ? "true" : "false", (int32_t)sample->status,
	       sample->read_complete ? "true" : "false");
	if (sample->read_complete)
		printf("%" PRIu32 ",\"pin_mux_ctrl_0\":%" PRIu32 ",\"uart_router_sel\":%" PRIu32,
		       (uint32_t)sample->arm_uart_ctl, (uint32_t)sample->pin_mux_ctrl_0,
		       (uint32_t)sample->uart_router_sel);
	else
		fputs("null,\"pin_mux_ctrl_0\":null,\"uart_router_sel\":null", stdout);
	putchar('}');
}

static void print_uart_result(const struct crystalhd_fw_research_uart_result *result)
{
	printf("{\"version\":%" PRIu32 ",\"uart_state\":true,\"control\":",
	       (uint32_t)result->state.request.version);
	print_result_object(&result->state.control);
	fputs(",\"fixed_state_samples\":{\"calibration\":", stdout);
	print_state_sample(&result->state.calibration);
	fputs(",\"after_init\":", stdout);
	print_state_sample(&result->state.after_init);
	fputs(",\"after_open\":", stdout);
	print_state_sample(&result->state.after_open);
	fputs("},\"uart_samples\":{\"after_init\":", stdout);
	print_uart_sample(&result->after_init);
	fputs(",\"after_open\":", stdout);
	print_uart_sample(&result->after_open);
	printf("},\"scope\":{\"register_addresses\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 "],"
	       "\"reads_per_sample\":3,\"maximum_register_reads\":6,\"raw_values_only\":true,"
	       "\"sample_status_fifo_reads\":false,\"sample_uart_writes\":false,"
	       "\"board_pads_proven\":false,\"voltage_proven\":false,"
	       "\"measured_baud_proven\":false,\"console_availability_proven\":false}}\n",
	       UART_ARM_CTL_ADDRESS, UART_PIN_MUX_ADDRESS, UART_ROUTER_ADDRESS);
}

static void print_crypto_sample(
	const struct crystalhd_fw_research_crypto_sample *sample)
{
	bool after_target = sample->guard_reads_complete > 1 &&
		(sample->gisb_last & CRYPTO_GISB_ERROR_MASK);
	uint32_t target_index = after_target ? sample->guard_reads_complete - 2 : 0;

	printf("{\"attempted\":%s,\"status\":%" PRId32 ",\"read_complete\":%s,"
	       "\"target_reads_attempted\":%" PRIu32
	       ",\"guard_reads_complete\":%" PRIu32 ",\"gisb_before\":",
	       sample->attempted ? "true" : "false", (int32_t)sample->status,
	       sample->read_complete ? "true" : "false",
	       (uint32_t)sample->target_reads_attempted,
	       (uint32_t)sample->guard_reads_complete);
	if (sample->guard_reads_complete)
		printf("%" PRIu32 ",\"gisb_last\":%" PRIu32,
		       (uint32_t)sample->gisb_before, (uint32_t)sample->gisb_last);
	else
		fputs("null,\"gisb_last\":null", stdout);
	printf(",\"error_capture_complete\":%s,\"error_capture_address\":",
	       sample->error_capture_complete ? "true" : "false");
	if (sample->error_capture_complete)
		printf("%" PRIu32 ",\"error_capture_master\":%" PRIu32,
		       (uint32_t)sample->error_capture_address,
		       (uint32_t)sample->error_capture_master);
	else
		fputs("null,\"error_capture_master\":null", stdout);
	fputs(",\"guard_failure_observed_after_target_index\":", stdout);
	if (after_target)
		printf("%" PRIu32 ",\"guard_failure_observed_after_target_address\":%" PRIu32,
		       target_index, crypto_target_addresses[target_index]);
	else
		fputs("null,\"guard_failure_observed_after_target_address\":null", stdout);
	fputs(",\"sharf_revision\":", stdout);
	if (sample->read_complete)
		printf("%" PRIu32 ",\"sharf_status\":%" PRIu32
		       ",\"bop_gr_bridge_revision\":%" PRIu32
		       ",\"bop_aes_status\":%" PRIu32,
		       (uint32_t)sample->sharf_revision, (uint32_t)sample->sharf_status,
		       (uint32_t)sample->bop_gr_bridge_revision,
		       (uint32_t)sample->bop_aes_status);
	else
		fputs("null,\"sharf_status\":null,\"bop_gr_bridge_revision\":null,"
		      "\"bop_aes_status\":null", stdout);
	putchar('}');
}

static void print_crypto_result(
	const struct crystalhd_fw_research_crypto_result *result)
{
	printf("{\"version\":%" PRIu32 ",\"crypto_state\":true,\"control\":",
	       (uint32_t)result->state.request.version);
	print_result_object(&result->state.control);
	fputs(",\"fixed_state_samples\":{\"calibration\":", stdout);
	print_state_sample(&result->state.calibration);
	fputs(",\"after_init\":", stdout);
	print_state_sample(&result->state.after_init);
	fputs(",\"after_open\":", stdout);
	print_state_sample(&result->state.after_open);
	fputs("},\"crypto_samples\":{\"after_init\":", stdout);
	print_crypto_sample(&result->after_init);
	fputs(",\"after_open\":", stdout);
	print_crypto_sample(&result->after_open);
	printf("},\"scope\":{\"register_addresses\":[%" PRIu32 ",%" PRIu32
	       ",%" PRIu32 ",%" PRIu32 "],\"gisb_error_guard_address\":%" PRIu32
	       ",\"gisb_error_mask\":%" PRIu32
	       ",\"gisb_error_capture_address_register\":%" PRIu32
	       ",\"gisb_error_capture_master_register\":%" PRIu32
	       ",\"target_register_reads_per_sample\":4,\"guard_reads_per_sample\":5,"
	       "\"successful_sample_register_reads\":9,"
	       "\"conditional_error_capture_reads_maximum\":2,"
	       "\"maximum_register_reads_per_sample\":11,"
	       "\"successful_run_register_reads\":18,\"maximum_register_reads\":20,"
	       "\"raw_values_only\":true,\"target_register_writes\":false,"
	       "\"error_capture_clear_writes\":false,\"retries\":false,"
	       "\"indirect_gisb_selector_write\":true,"
	       "\"successful_sample_indirect_gisb_selector_writes\":9,"
	       "\"maximum_indirect_gisb_selector_writes_per_sample\":11,"
	       "\"successful_run_indirect_gisb_selector_writes\":18,"
	       "\"maximum_indirect_gisb_selector_writes\":20,\"passive\":false,"
	       "\"bop_aes_revision_register_present\":false,"
	       "\"bop_gr_bridge_revision_is_aes_core_revision\":false,"
	       "\"sha_cmac_context_reads\":false,"
	       "\"key_iv_nonce_otp_scrub_reads\":false,"
	       "\"engine_enable_or_start_writes\":false,"
	       "\"algorithm_support_established\":false,"
	       "\"guard_failure_attribution_causal\":false,"
	       "\"independent_fetch_errors_certified\":false,"
	       "\"atomic_coherence_established\":false}}\n",
	       CRYPTO_SHARF_REVISION_ADDRESS, CRYPTO_SHARF_STATUS_ADDRESS,
	       CRYPTO_BOP_GR_BRIDGE_REVISION_ADDRESS, CRYPTO_BOP_AES_STATUS_ADDRESS,
	       CRYPTO_GISB_ERROR_STATUS_ADDRESS, CRYPTO_GISB_ERROR_MASK,
	       CRYPTO_GISB_ERROR_CAPTURE_ADDRESS,
	       CRYPTO_GISB_ERROR_CAPTURE_MASTER);
}

static void print_controller_sample(
	const struct crystalhd_fw_research_controller_sample *sample)
{
	printf("{\"attempted\":%s,\"status\":%" PRId32 ",\"read_complete\":%s,\"raw_root\":",
	       sample->attempted ? "true" : "false", (int32_t)sample->status,
	       sample->read_complete ? "true" : "false");
	if (sample->read_complete) {
		uint64_t start = sample->root;
		bool aligned = !(sample->root & 3U);
		bool span = start >= CONTROLLER_CANDIDATE_START &&
			start + CONTROLLER_CANDIDATE_BYTES <= CONTROLLER_CANDIDATE_END;

		printf("%" PRIu32 ",\"candidate_alignment_4\":%s,"
		       "\"candidate_full_span_in_window\":%s",
		       (uint32_t)sample->root, aligned ? "true" : "false", span ? "true" : "false");
	} else {
		fputs("null,\"candidate_alignment_4\":null,\"candidate_full_span_in_window\":null", stdout);
	}
	putchar('}');
}

static void print_controller_result_object(const struct crystalhd_fw_research_controller_result *result)
{
	printf("{\"version\":%" PRIu32 ",\"controller_root\":true,\"control\":",
	       (uint32_t)result->state.request.version);
	print_result_object(&result->state.control);
	fputs(",\"fixed_state_samples\":{\"calibration\":", stdout);
	print_state_sample(&result->state.calibration);
	fputs(",\"after_init\":", stdout);
	print_state_sample(&result->state.after_init);
	fputs(",\"after_open\":", stdout);
	print_state_sample(&result->state.after_open);
	fputs("},\"controller_root_samples\":{\"after_init\":", stdout);
	print_controller_sample(&result->after_init);
	fputs(",\"after_open\":", stdout);
	print_controller_sample(&result->after_open);
	fputs("},\"root_values_equal\":", stdout);
	if (result->after_init.read_complete && result->after_open.read_complete)
		fputs(result->after_init.root == result->after_open.root ? "true" : "false", stdout);
	else
		fputs("null", stdout);
	printf(",\"scope\":{\"fixed_root_read_address\":%" PRIu32 ","
	       "\"candidate_lower_bound\":%" PRIu32 ",\"candidate_upper_bound_exclusive\":%" PRIu32 ","
	       "\"candidate_span_bytes\":%" PRIu32 ",\"returned_pointer_followed\":false,"
	       "\"ownership_established\":false,\"coherence_established\":false,"
	       "\"lease_established\":false,\"equality_excludes_aba\":false}}",
	       CONTROLLER_ROOT_ADDRESS, CONTROLLER_CANDIDATE_START,
	       CONTROLLER_CANDIDATE_END, CONTROLLER_CANDIDATE_BYTES);
}

static void print_controller_result(const struct crystalhd_fw_research_controller_result *result)
{
	print_controller_result_object(result);
	putchar('\n');
}

static void print_image_sample(const struct crystalhd_fw_research_image_sample *sample)
{
	printf("{\"attempted\":%s,\"status\":%" PRId32 ",\"read_complete\":%s,\"root_before\":",
	       sample->attempted ? "true" : "false", (int32_t)sample->status,
	       sample->read_complete ? "true" : "false");
	if (sample->read_complete) {
		printf("%" PRIu32 ",\"root_after\":%" PRIu32 ",\"raw_words\":[%" PRIu32 ",%" PRIu32
		       ",%" PRIu32 ",%" PRIu32 "],\"owned_declaration_low8\":%" PRIu32 ",\"owned_upper24_raw\":%" PRIu32,
		       (uint32_t)sample->root_before, (uint32_t)sample->root_after,
		       (uint32_t)sample->words[0], (uint32_t)sample->words[1],
		       (uint32_t)sample->words[2], (uint32_t)sample->words[3],
		       (uint32_t)(sample->words[3] & 0xffU), (uint32_t)(sample->words[3] >> 8));
	} else {
		fputs("null,\"root_after\":null,\"raw_words\":null,\"owned_declaration_low8\":null,\"owned_upper24_raw\":null", stdout);
	}
	putchar('}');
}

static void print_image_result_object(const struct crystalhd_fw_research_image_result *result)
{
	printf("{\"version\":%" PRIu32 ",\"controller_image\":true,\"controller\":",
	       (uint32_t)result->controller.state.request.version);
	print_controller_result_object(&result->controller);
	fputs(",\"image_samples\":{\"after_init\":", stdout);
	print_image_sample(&result->after_init);
	fputs(",\"after_open\":", stdout);
	print_image_sample(&result->after_open);
	printf("},\"scope\":{\"fresh_root_lower_bound\":%" PRIu32 ",\"tuple_word_offset\":%" PRIu32
	       ",\"tuple_words\":4,\"controller_root_used_for_fixed_tuple\":true,"
	       "\"tuple_values_followed\":false,\"ownership_established\":false,"
	       "\"coherence_established\":false,\"lease_established\":false,\"bracket_equality_excludes_aba\":false}}",
	       IMAGE_CONTEXT_MIN, IMAGE_TUPLE_OFFSET);
}

static void print_image_result(const struct crystalhd_fw_research_image_result *result)
{
	print_image_result_object(result);
	putchar('\n');
}

static void print_packet_sample(const struct crystalhd_fw_research_packet_sample *sample)
{
	printf("{\"attempted\":%s,\"status\":%" PRId32 ",\"read_complete\":%s,\"root_before\":",
	       sample->attempted ? "true" : "false", (int32_t)sample->status,
	       sample->read_complete ? "true" : "false");
	if (sample->read_complete) {
		printf("%" PRIu32 ",\"root_after\":%" PRIu32 ",\"image_words\":[%" PRIu32 ",%" PRIu32
		       ",%" PRIu32 ",%" PRIu32 "],\"packet_words\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 "]",
		       (uint32_t)sample->root_before, (uint32_t)sample->root_after,
		       (uint32_t)sample->image_words[0], (uint32_t)sample->image_words[1],
		       (uint32_t)sample->image_words[2], (uint32_t)sample->image_words[3],
		       (uint32_t)sample->packet_words[0], (uint32_t)sample->packet_words[1],
		       (uint32_t)sample->packet_words[2]);
	} else {
		fputs("null,\"root_after\":null,\"image_words\":null,\"packet_words\":null", stdout);
	}
	putchar('}');
}

static void print_packet_result(const struct crystalhd_fw_research_packet_result *result)
{
	printf("{\"version\":%" PRIu32 ",\"controller_packet\":true,\"image\":",
	       (uint32_t)result->image.controller.state.request.version);
	print_image_result_object(&result->image);
	fputs(",\"packet_samples\":{\"after_init\":", stdout);
	print_packet_sample(&result->after_init);
	fputs(",\"after_open\":", stdout);
	print_packet_sample(&result->after_open);
	printf("},\"scope\":{\"fresh_root_lower_bound\":%" PRIu32 ",\"image_word_offset\":%" PRIu32
	       ",\"packet_aliases_offset\":%" PRIu32 ",\"packet_physical_offset\":%" PRIu32
	       ",\"controller_root_used_for_fixed_fields\":true,\"returned_values_followed\":false,"
	       "\"ownership_established\":false,\"coherence_established\":false,\"lease_established\":false,"
	       "\"dma_suitability_established\":false,\"bracket_equality_excludes_aba\":false}}\n",
	       IMAGE_CONTEXT_MIN, IMAGE_TUPLE_OFFSET, PACKET_ALIASES_OFFSET, PACKET_PHYSICAL_OFFSET);
}

static void print_heap_array(const __u32 *words, unsigned int count, bool complete)
{
	unsigned int i;

	if (!complete) {
		fputs("null", stdout);
		return;
	}
	putchar('[');
	for (i = 0; i < count; i++)
		printf("%s%" PRIu32, i ? "," : "", (uint32_t)words[i]);
	putchar(']');
}

static void print_heap_packet_sample(const struct crystalhd_fw_research_heap_packet_sample *sample)
{
	printf("{\"attempted\":%s,\"status\":%" PRId32 ",\"read_complete\":%s,\"root_before\":",
	       sample->attempted ? "true" : "false", (int32_t)sample->status,
	       sample->read_complete ? "true" : "false");
	if (sample->read_complete)
		printf("%" PRIu32 ",\"root_after\":%" PRIu32, (uint32_t)sample->root_before, (uint32_t)sample->root_after);
	else
		fputs("null,\"root_after\":null", stdout);
	fputs(",\"image_before\":", stdout);
	print_heap_array(sample->image_before, 4, sample->read_complete);
	fputs(",\"image_after\":", stdout);
	print_heap_array(sample->image_after, 4, sample->read_complete);
	fputs(",\"packet_before\":", stdout);
	print_heap_array(sample->packet_before, 3, sample->read_complete);
	fputs(",\"packet_after\":", stdout);
	print_heap_array(sample->packet_after, 3, sample->read_complete);
	fputs(",\"packet_address\":", stdout);
	if (sample->read_complete)
		printf("%" PRIu32, (uint32_t)sample->packet_address);
	else
		fputs("null", stdout);
	fputs(",\"header_words\":", stdout);
	print_heap_array(sample->header_words, 5, sample->read_complete);
	fputs(",\"slots_before\":", stdout);
	print_heap_array(sample->slots_before, 2, sample->read_complete);
	fputs(",\"slots_after\":", stdout);
	print_heap_array(sample->slots_after, 2, sample->read_complete);
	putchar('}');
}

static void print_heap_packet_result(const struct crystalhd_fw_research_heap_packet_result *result)
{
	printf("{\"version\":%" PRIu32 ",\"heap_packet\":true,\"image\":",
	       (uint32_t)result->image.controller.state.request.version);
	print_image_result_object(&result->image);
	fputs(",\"heap_packet_samples\":{\"after_init\":", stdout);
	print_heap_packet_sample(&result->after_init);
	fputs(",\"after_open\":", stdout);
	print_heap_packet_sample(&result->after_open);
	printf("},\"scope\":{\"fresh_root_lower_bound\":%" PRIu32 ",\"image_word_offset\":%" PRIu32
	       ",\"packet_aliases_offset\":%" PRIu32 ",\"packet_physical_offset\":%" PRIu32
	       ",\"stored_reply_offset\":%" PRIu32 ",\"image_base_lower_bound\":%" PRIu32
	       ",\"image_upper_bound_exclusive\":%" PRIu32 ",\"image_extent_bytes\":%" PRIu32
	       ",\"fixed_packet_offset\":%" PRIu32 ",\"section_bytes\":%" PRIu32 ",\"header_words\":5,"
	       "\"image_base_used_for_computed_admitted_fixed_span\":true,\"packet_declarations_used_as_equality_gates\":true,"
	       "\"header_values_followed\":false,\"stored_reply_values_followed\":false,"
	       "\"freshness_established\":false,\"object_lifetime_established\":false,\"atomic_coherence_established\":false,"
	       "\"ownership_established\":false,\"lease_established\":false,\"queue_validity_established\":false,"
	       "\"dma_suitability_established\":false,\"independent_fetch_errors_certified\":false,"
	       "\"bracket_equality_excludes_aba\":false}}\n",
	       IMAGE_CONTEXT_MIN, IMAGE_TUPLE_OFFSET, PACKET_ALIASES_OFFSET, PACKET_PHYSICAL_OFFSET,
	       HEAP_SLOTS_OFFSET, HEAP_IMAGE_MIN, HEAP_IMAGE_LIMIT, HEAP_IMAGE_BYTES,
	       HEAP_PACKET_OFFSET, HEAP_PACKET_SECTION_BYTES);
}

int main(int argc, char **argv)
{
	struct crystalhd_fw_research_info info = { 0 };
	struct crystalhd_fw_research_result result = { 0 };
	struct crystalhd_fw_research_request request = { 0 };
	struct crystalhd_fw_research_state_result state_result = { 0 };
	struct crystalhd_fw_research_state_request state_request = { 0 };
	struct crystalhd_fw_research_controller_result controller_result = { 0 };
	struct crystalhd_fw_research_image_result image_result = { 0 };
	struct crystalhd_fw_research_packet_result packet_result = { 0 };
	struct crystalhd_fw_research_heap_packet_result heap_packet_result = { 0 };
	struct crystalhd_fw_research_clock_result clock_result = { 0 };
	struct crystalhd_fw_research_uart_result uart_result = { 0 };
	struct crystalhd_fw_research_crypto_result crypto_result = { 0 };
	struct options options;
	struct stat statbuf;
	bool have_info = false, have_result = false, have_state = false, have_controller = false;
	bool have_image = false, have_packet = false;
	bool have_heap_packet = false;
	bool have_clock = false;
	bool have_uart = false;
	bool have_crypto = false;
	int fd = -1, rc = 1;

	if (argc == 2 && !strcmp(argv[1], "--help")) {
		usage(stdout);
		return output_finish();
	}
	if (!parse_options(argc, argv, &options)) {
		usage(stderr);
		return 1;
	}
	fd = open(PROBE_DEVICE, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		perror("open research device");
		goto out;
	}
	if (fstat(fd, &statbuf) < 0) {
		perror("stat research device");
		goto out;
	}
	if (!S_ISCHR(statbuf.st_mode)) {
		fputs("Research device is not a character device.\n", stderr);
		goto out;
	}
	if (ioctl(fd, CRYSTALHD_FW_RESEARCH_GET_INFO, &info) < 0) {
		perror("read research metadata");
		goto out;
	}
	if (!info_valid(&info)) {
		fputs("Invalid research metadata; no live command attempted.\n", stderr);
		goto out;
	}
	if (options.action == ACTION_INFO) {
		have_info = true;
		rc = 0;
		goto out;
	}
	if (info.generation != options.generation) {
		fputs("Device generation changed; no live command attempted.\n", stderr);
		goto out;
	}
	request.version = CRYSTALHD_FW_RESEARCH_VERSION;
	request.size = sizeof(result);
	switch (options.action) {
	case ACTION_VERSION:
		request.selector = CRYSTALHD_FW_RESEARCH_VERSION_ONLY;
		break;
	case ACTION_H264:
	case ACTION_FIXED_STATE:
	case ACTION_CONTROLLER_ROOT:
	case ACTION_CONTROLLER_IMAGE:
	case ACTION_CONTROLLER_PACKET:
	case ACTION_HEAP_PACKET:
	case ACTION_CLOCK_STATE:
	case ACTION_UART_STATE:
	case ACTION_CRYPTO_STATE:
		request.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
		break;
	case ACTION_H261:
		request.selector = CRYSTALHD_FW_RESEARCH_H261_CONTROL;
		break;
	case ACTION_H263:
		request.selector = CRYSTALHD_FW_RESEARCH_H263_CONTROL;
		break;
	case ACTION_MPEG1:
		request.selector = CRYSTALHD_FW_RESEARCH_MPEG1_CONTROL;
		break;
	case ACTION_SCALING_FILTERS:
		request.selector = CRYSTALHD_FW_RESEARCH_SCALING_FILTERS_COMMAND;
		break;
	case ACTION_PIC_CAPTURE:
		request.selector = CRYSTALHD_FW_RESEARCH_PIC_CAPTURE_COMMAND;
		break;
	case ACTION_SET_CSC:
		request.selector = CRYSTALHD_FW_RESEARCH_SET_CSC_COMMAND;
		break;
	case ACTION_SET_FGT:
		request.selector = CRYSTALHD_FW_RESEARCH_SET_FGT_COMMAND;
		break;
	case ACTION_CUSTOM_VIDOUT:
		request.selector = CRYSTALHD_FW_RESEARCH_CUSTOM_VIDOUT_COMMAND;
		break;
	case ACTION_FILL_PIC_BUF:
		request.selector = CRYSTALHD_FW_RESEARCH_FILL_PIC_BUF_COMMAND;
		break;
	default:
		fputs("Invalid research selector; no live command attempted.\n", stderr);
		goto out;
	}
	if (!(info.selector_mask & (1U << (request.selector - 1)))) {
		fputs("Research selector unavailable; no live command attempted.\n", stderr);
		goto out;
	}
	if (options.action == ACTION_CRYPTO_STATE) {
		const struct crystalhd_fw_research_result *control = &crypto_result.state.control;

		state_request.version = CRYSTALHD_FW_RESEARCH_VERSION;
		state_request.size = sizeof(crypto_result);
		crypto_result.state.request = state_request;
		if (ioctl(fd, CRYSTALHD_FW_RESEARCH_RUN_CRYPTO, &crypto_result) < 0) {
			perror("run crypto-state readback");
			fputs("Device state may have changed; no retry was attempted.\n", stderr);
			goto out;
		}
		if (control->retained)
			fputs("Research resources remain retained. Do not force unload the driver.\n", stderr);
		if (!crypto_result_valid(&crypto_result, &state_request, info.generation)) {
			fputs("Invalid crypto-state result; no further command attempted.\n", stderr);
			goto out;
		}
		have_crypto = true;
		rc = control->status || control->retained ||
			(control->cleanup_attempted && control->cleanup_status != BC_STS_SUCCESS) ? 1 : 0;
		goto out;
	}
	if (options.action == ACTION_UART_STATE) {
		const struct crystalhd_fw_research_result *control = &uart_result.state.control;

		state_request.version = CRYSTALHD_FW_RESEARCH_VERSION;
		state_request.size = sizeof(uart_result);
		uart_result.state.request = state_request;
		if (ioctl(fd, CRYSTALHD_FW_RESEARCH_RUN_UART, &uart_result) < 0) {
			perror("run uart-state readback");
			fputs("Device state may have changed; no retry was attempted.\n", stderr);
			goto out;
		}
		if (control->retained)
			fputs("Research resources remain retained. Do not force unload the driver.\n", stderr);
		if (!uart_result_valid(&uart_result, &state_request, info.generation)) {
			fputs("Invalid uart-state result; no further command attempted.\n", stderr);
			goto out;
		}
		have_uart = true;
		rc = control->status || control->retained ||
			(control->cleanup_attempted && control->cleanup_status != BC_STS_SUCCESS) ? 1 : 0;
		goto out;
	}
	if (options.action == ACTION_CLOCK_STATE) {
		const struct crystalhd_fw_research_result *control = &clock_result.state.control;

		state_request.version = CRYSTALHD_FW_RESEARCH_VERSION;
		state_request.size = sizeof(clock_result);
		clock_result.state.request = state_request;
		if (ioctl(fd, CRYSTALHD_FW_RESEARCH_RUN_CLOCK, &clock_result) < 0) {
			perror("run clock-state readback");
			fputs("Device state may have changed; no retry was attempted.\n", stderr);
			goto out;
		}
		if (control->retained)
			fputs("Research resources remain retained. Do not force unload the driver.\n", stderr);
		if (!clock_result_valid(&clock_result, &state_request, info.generation)) {
			fputs("Invalid clock-state result; no further command attempted.\n", stderr);
			goto out;
		}
		have_clock = true;
		rc = control->status || control->retained ||
			(control->cleanup_attempted && control->cleanup_status != BC_STS_SUCCESS) ? 1 : 0;
		goto out;
	}
	if (options.action == ACTION_HEAP_PACKET) {
		const struct crystalhd_fw_research_result *control = &heap_packet_result.image.controller.state.control;

		state_request.version = CRYSTALHD_FW_RESEARCH_VERSION;
		state_request.size = sizeof(heap_packet_result);
		heap_packet_result.image.controller.state.request = state_request;
		if (ioctl(fd, CRYSTALHD_FW_RESEARCH_RUN_HEAP_PACKET, &heap_packet_result) < 0) {
			perror("run heap-packet readback");
			fputs("Device state may have changed; no retry was attempted.\n", stderr);
			goto out;
		}
		if (control->retained)
			fputs("Research resources remain retained. Do not force unload the driver.\n", stderr);
		if (!heap_packet_result_valid(&heap_packet_result, &state_request, info.generation)) {
			fputs("Invalid heap-packet result; no further command attempted.\n", stderr);
			goto out;
		}
		have_heap_packet = true;
		rc = control->status || control->retained ||
			(control->cleanup_attempted && control->cleanup_status != BC_STS_SUCCESS) ? 1 : 0;
		goto out;
	}
	if (options.action == ACTION_CONTROLLER_PACKET) {
		const struct crystalhd_fw_research_result *control = &packet_result.image.controller.state.control;

		state_request.version = CRYSTALHD_FW_RESEARCH_VERSION;
		state_request.size = sizeof(packet_result);
		packet_result.image.controller.state.request = state_request;
		if (ioctl(fd, CRYSTALHD_FW_RESEARCH_RUN_PACKET, &packet_result) < 0) {
			perror("run controller-packet readback");
			fputs("Device state may have changed; no retry was attempted.\n", stderr);
			goto out;
		}
		if (control->retained)
			fputs("Research resources remain retained. Do not force unload the driver.\n", stderr);
		if (!packet_result_valid(&packet_result, &state_request, info.generation)) {
			fputs("Invalid controller-packet result; no further command attempted.\n", stderr);
			goto out;
		}
		have_packet = true;
		rc = control->status || control->retained ||
			(control->cleanup_attempted && control->cleanup_status != BC_STS_SUCCESS) ? 1 : 0;
		goto out;
	}
	if (options.action == ACTION_CONTROLLER_IMAGE) {
		const struct crystalhd_fw_research_result *control = &image_result.controller.state.control;

		state_request.version = CRYSTALHD_FW_RESEARCH_VERSION;
		state_request.size = sizeof(image_result);
		image_result.controller.state.request = state_request;
		if (ioctl(fd, CRYSTALHD_FW_RESEARCH_RUN_IMAGE, &image_result) < 0) {
			perror("run controller-image readback");
			fputs("Device state may have changed; no retry was attempted.\n", stderr);
			goto out;
		}
		if (control->retained)
			fputs("Research resources remain retained. Do not force unload the driver.\n", stderr);
		if (!image_result_valid(&image_result, &state_request, info.generation)) {
			fputs("Invalid controller-image result; no further command attempted.\n", stderr);
			goto out;
		}
		have_image = true;
		rc = control->status || control->retained ||
			(control->cleanup_attempted && control->cleanup_status != BC_STS_SUCCESS) ? 1 : 0;
		goto out;
	}
	if (options.action == ACTION_CONTROLLER_ROOT) {
		state_request.version = CRYSTALHD_FW_RESEARCH_VERSION;
		state_request.size = sizeof(controller_result);
		controller_result.state.request = state_request;
		if (ioctl(fd, CRYSTALHD_FW_RESEARCH_RUN_CONTROLLER, &controller_result) < 0) {
			perror("run controller-root readback");
			fputs("Device state may have changed; no retry was attempted.\n", stderr);
			goto out;
		}
		if (controller_result.state.control.retained)
			fputs("Research resources remain retained. Do not force unload the driver.\n", stderr);
		if (!controller_result_valid(&controller_result, &state_request, info.generation)) {
			fputs("Invalid controller-root result; no further command attempted.\n", stderr);
			goto out;
		}
		have_controller = true;
		rc = controller_result.state.control.status || controller_result.state.control.retained ||
			(controller_result.state.control.cleanup_attempted &&
			 controller_result.state.control.cleanup_status != BC_STS_SUCCESS) ? 1 : 0;
		goto out;
	}
	if (options.action == ACTION_FIXED_STATE) {
		state_request.version = CRYSTALHD_FW_RESEARCH_VERSION;
		state_request.size = sizeof(state_result);
		state_result.request = state_request;
		if (ioctl(fd, CRYSTALHD_FW_RESEARCH_RUN_STATE, &state_result) < 0) {
			perror("run fixed-state readback");
			fputs("Device state may have changed; no retry was attempted.\n", stderr);
			goto out;
		}
		if (state_result.control.retained)
			fputs("Research resources remain retained. Do not force unload the driver.\n", stderr);
		if (!state_result_valid(&state_result, &state_request, info.generation)) {
			fputs("Invalid fixed-state result; no further command attempted.\n", stderr);
			goto out;
		}
		have_state = true;
		rc = state_result.control.status || state_result.control.retained ||
			(state_result.control.cleanup_attempted &&
			 state_result.control.cleanup_status != BC_STS_SUCCESS) ? 1 : 0;
		goto out;
	}
	result.request = request;
	if (ioctl(fd, CRYSTALHD_FW_RESEARCH_RUN, &result) < 0) {
		perror("run research selector");
		fputs("Device state may have changed; no retry was attempted.\n", stderr);
		goto out;
	}
	if (result.retained)
		fputs("Research resources remain retained. Do not force unload the driver.\n",
		      stderr);
	if (!result_valid(&result, &request, info.generation)) {
		fputs("Invalid research result; no further command attempted.\n", stderr);
		goto out;
	}
	have_result = true;
	rc = result.status || result.retained ||
		(result.cleanup_attempted && result.cleanup_status != BC_STS_SUCCESS) ? 1 : 0;
out:
	if (fd >= 0 && close(fd) < 0) {
		perror("close research device");
		rc = 1;
	}
	if (have_info)
		print_info(&info);
	if (have_result)
		print_result(&result);
	if (have_state)
		print_state_result(&state_result);
	if (have_controller)
		print_controller_result(&controller_result);
	if (have_image)
		print_image_result(&image_result);
	if (have_packet)
		print_packet_result(&packet_result);
	if (have_heap_packet)
		print_heap_packet_result(&heap_packet_result);
	if (have_clock)
		print_clock_result(&clock_result);
	if (have_uart)
		print_uart_result(&uart_result);
	if (have_crypto)
		print_crypto_result(&crypto_result);
	if (output_finish())
		rc = 1;
	return rc;
}
