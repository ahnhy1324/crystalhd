/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _UAPI_CRYSTALHD_FW_RESEARCH_H_
#define _UAPI_CRYSTALHD_FW_RESEARCH_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define CRYSTALHD_FW_RESEARCH_VERSION 1U
#define CRYSTALHD_FW_RESEARCH_VERSION_ONLY 1U
#define CRYSTALHD_FW_RESEARCH_H264_CONTROL 2U
#define CRYSTALHD_FW_RESEARCH_H261_CONTROL 3U
#define CRYSTALHD_FW_RESEARCH_H263_CONTROL 4U
#define CRYSTALHD_FW_RESEARCH_MPEG1_CONTROL 5U
#define CRYSTALHD_FW_RESEARCH_MAX_COMMANDS 5U
#define CRYSTALHD_FW_RESEARCH_RESPONSE_WORDS 64U
#define CRYSTALHD_FW_RESEARCH_SELECTOR_MASK 31U
#define CRYSTALHD_FW_RESEARCH_FIRMWARE_SHA256 \
	"8bf3a68f5c64686358a52274e40911a88c7f8c67ecbf6cf1557a49b4d7bc67c9"

struct crystalhd_fw_research_info {
	__u32 version;
	__u32 size;
	__aligned_u64 generation;
	/* Available research selectors, not decoder/codec capabilities. */
	__u32 selector_mask;
	__u32 reserved[3];
	/* Expected probe image, not a measurement of loaded device memory. */
	__u8 firmware_sha256[32];
};

struct crystalhd_fw_research_request {
	__u32 version;
	__u32 size;
	__u32 selector;
	__u32 flags;
	__u32 reserved[4];
};

struct crystalhd_fw_research_reply {
	__u32 command;
	__u32 sequence;
	__u32 transport_status;
	/* A completed reply read, not a successful firmware command. */
	__u32 raw_response_valid;
	__u32 header_matches;
	__u32 response[CRYSTALHD_FW_RESEARCH_RESPONSE_WORDS];
};

struct crystalhd_fw_research_result {
	struct crystalhd_fw_research_request request;
	__aligned_u64 generation;
	/* Experiment outcome; successful result delivery is not device success. */
	__s32 status;
	__u32 download_attempted;
	__u32 download_status;
	__u32 cleanup_attempted;
	__u32 cleanup_status;
	/* This attempt still owns resources or its failed acquisition kept them. */
	__u32 retained;
	__u32 command_count;
	/* Complete request-data digest; a valid digest may still fail the pin. */
	__u32 firmware_hash_valid;
	__u8 firmware_sha256[32];
	struct crystalhd_fw_research_reply replies[CRYSTALHD_FW_RESEARCH_MAX_COMMANDS];
};

#define CRYSTALHD_FW_RESEARCH_RUN \
	_IOWR('R', 0x92, struct crystalhd_fw_research_result)
#define CRYSTALHD_FW_RESEARCH_GET_INFO \
	_IOR('R', 0x91, struct crystalhd_fw_research_info)

#endif
