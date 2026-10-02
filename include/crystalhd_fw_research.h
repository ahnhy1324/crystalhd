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
#define CRYSTALHD_FW_RESEARCH_SCALING_FILTERS_COMMAND 6U
#define CRYSTALHD_FW_RESEARCH_PIC_CAPTURE_COMMAND 7U
#define CRYSTALHD_FW_RESEARCH_SET_CSC_COMMAND 8U
#define CRYSTALHD_FW_RESEARCH_SET_FGT_COMMAND 9U
#define CRYSTALHD_FW_RESEARCH_CUSTOM_VIDOUT_COMMAND 10U
#define CRYSTALHD_FW_RESEARCH_FILL_PIC_BUF_COMMAND 11U
#define CRYSTALHD_FW_RESEARCH_MAX_COMMANDS 5U
#define CRYSTALHD_FW_RESEARCH_RESPONSE_WORDS 64U
#define CRYSTALHD_FW_RESEARCH_SELECTOR_MASK 2047U
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

struct crystalhd_fw_research_state_request {
	__u32 version;
	__u32 size;
	__u32 flags;
	__u32 reserved;
};

struct crystalhd_fw_research_state_sample {
	__u32 attempted;
	__s32 status;
	/* Completed host reads, not bus-error or cache-coherence certification. */
	__u32 read_complete;
	__u32 reserved;
	__u32 words[4];
};

struct crystalhd_fw_research_state_result {
	struct crystalhd_fw_research_state_request request;
	/* Fixed H.264 OPEN-only control; its existing wire contract is unchanged. */
	struct crystalhd_fw_research_result control;
	struct crystalhd_fw_research_state_sample calibration;
	struct crystalhd_fw_research_state_sample after_init;
	struct crystalhd_fw_research_state_sample after_open;
};

/* Reloads stock firmware. No caller-provided address, selector or data. */
#define CRYSTALHD_FW_RESEARCH_RUN_STATE \
	_IOWR('R', 0x93, struct crystalhd_fw_research_state_result)

struct crystalhd_fw_research_controller_sample {
	__u32 attempted;
	__s32 status;
	/* A completed fixed ARM DRAM read, not controller lifetime or ownership. */
	__u32 read_complete;
	/* Observed value only: never followed as an address by this diagnostic. */
	__u32 root;
};

struct crystalhd_fw_research_controller_result {
	/* Same fixed control/state samples; request.size names this larger result. */
	struct crystalhd_fw_research_state_result state;
	struct crystalhd_fw_research_controller_sample after_init;
	struct crystalhd_fw_research_controller_sample after_open;
};

/* Additive fixed-root observation; RUN_STATE's layout/semantics are unchanged. */
#define CRYSTALHD_FW_RESEARCH_RUN_CONTROLLER \
	_IOWR('R', 0x94, struct crystalhd_fw_research_controller_result)

struct crystalhd_fw_research_image_sample {
	__u32 attempted;
	__s32 status;
	/* Completed bracketed reads, not lifetime, ownership or coherence. */
	__u32 read_complete;
	__u32 reserved;
	__u32 root_before;
	__u32 root_after;
	/* Raw order: virtual image pointer, translated physical image base, extent,
	 * owned-byte word (low byte only; upper 24 bits remain raw).
	 */
	__u32 words[4];
};

struct crystalhd_fw_research_image_result {
	struct crystalhd_fw_research_controller_result controller;
	struct crystalhd_fw_research_image_sample after_init;
	struct crystalhd_fw_research_image_sample after_open;
};

/* Fixed C-relative tuple only; returned image-pointer words are never followed. */
#define CRYSTALHD_FW_RESEARCH_RUN_IMAGE \
	_IOWR('R', 0x95, struct crystalhd_fw_research_image_result)

struct crystalhd_fw_research_packet_sample {
	__u32 attempted;
	__s32 status;
	/* Completed bracketed reads, not lifetime, ownership or coherence. */
	__u32 read_complete;
	__u32 reserved;
	__u32 root_before;
	__u32 root_after;
	/* Same raw image tuple order as image_sample.words. */
	__u32 image_words[4];
	/* Raw order: virtual packet aliases C+0x94/C+0x98, physical packet C+0x1cc. */
	__u32 packet_words[3];
};

struct crystalhd_fw_research_packet_result {
	struct crystalhd_fw_research_image_result image;
	struct crystalhd_fw_research_packet_sample after_init;
	struct crystalhd_fw_research_packet_sample after_open;
};

/* Fixed C-relative fields only; returned image/packet pointers are never followed. */
#define CRYSTALHD_FW_RESEARCH_RUN_PACKET \
	_IOWR('R', 0x96, struct crystalhd_fw_research_packet_result)

#endif
