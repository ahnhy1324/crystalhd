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

enum action {
	ACTION_NONE, ACTION_INFO, ACTION_VERSION, ACTION_H264,
	ACTION_H261, ACTION_H263, ACTION_MPEG1,
	ACTION_SCALING_FILTERS, ACTION_PIC_CAPTURE, ACTION_SET_CSC,
	ACTION_SET_FGT, ACTION_CUSTOM_VIDOUT, ACTION_FILL_PIC_BUF,
	ACTION_FIXED_STATE,
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
	      "       flea-fw-probe --help\n"
	      "\n"
	      "The live selectors reload firmware and reset an idle card. They do\n"
	      "not decode video or establish codec capability. Command-only probes\n"
	      "send one fixed zero-argument command after INIT and VERSION, without\n"
	      "opening or starting a decoder. A reply is not raw-processing support.\n"
	      "Fixed-state reads use stock INIT and H.264 OPEN; completed reads do\n"
	      "not certify cache coherence or backend ownership.\n"
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

int main(int argc, char **argv)
{
	struct crystalhd_fw_research_info info = { 0 };
	struct crystalhd_fw_research_result result = { 0 };
	struct crystalhd_fw_research_request request = { 0 };
	struct crystalhd_fw_research_state_result state_result = { 0 };
	struct crystalhd_fw_research_state_request state_request = { 0 };
	struct options options;
	struct stat statbuf;
	bool have_info = false, have_result = false, have_state = false;
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
	if (output_finish())
		rc = 1;
	return rc;
}
