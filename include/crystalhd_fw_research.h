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

struct crystalhd_fw_research_heap_packet_sample {
	__u32 attempted;
	__s32 status;
	/* Completed physical reads and equal brackets, not freshness or ownership. */
	__u32 read_complete;
	__u32 reserved;
	__u32 root_before;
	__u32 root_after;
	__u32 image_before[4];
	__u32 image_after[4];
	/* Raw aliases C+0x94/C+0x98 and physical declaration C+0x1cc. */
	__u32 packet_before[3];
	__u32 packet_after[3];
	/* Independently bounded image base + the fixed 0x70000 offset. */
	__u32 packet_address;
	__u32 header_words[5];
	/* Raw stored translation results C+0x250/C+0x254, never followed. */
	__u32 slots_before[2];
	__u32 slots_after[2];
};

struct crystalhd_fw_research_heap_packet_result {
	struct crystalhd_fw_research_image_result image;
	struct crystalhd_fw_research_heap_packet_sample after_init;
	struct crystalhd_fw_research_heap_packet_sample after_open;
};

/* Fixed heap span only; header and stored reply values never select addresses. */
#define CRYSTALHD_FW_RESEARCH_RUN_HEAP_PACKET \
	_IOWR('R', 0x97, struct crystalhd_fw_research_heap_packet_result)

/* One exact, fresh-INIT command observation.  Register values are masked raw
 * levels, not transfer ownership or visibility receipts.  Samples are taken
 * only while the ordinary firmware-command transaction remains pending; the
 * normal IRQ and reply copy retain completion authority.
 */
#define CRYSTALHD_FW_RESEARCH_COMMAND_SAMPLE_MAX 64U

struct crystalhd_fw_research_command_sample {
	__aligned_u64 cpudma_ns;
	__aligned_u64 sdram_ns;
	__u32 cpudma_active;
	__u32 sdram_busy;
};

struct crystalhd_fw_research_command_observation {
	__u32 attempted;
	__s32 status;
	/* Set only after exact reply, boundary read and normal cleanup succeed. */
	__u32 read_complete;
	__u32 sample_count;
	__aligned_u64 generation;
	/* Frozen host words 0..2; no in-flight device packet is read. */
	__u32 request_words[3];
	__u32 response_words[3];
	__u32 header_matches;
	/* At most one non-clearing host aggregate-status observation. */
	__u32 host_intr_attempted;
	__u32 host_intr_valid;
	__u32 host_intr_arm_mbox0;
	__aligned_u64 host_intr_ns;
	__u32 reserved[4];
	struct crystalhd_fw_research_command_sample
		samples[CRYSTALHD_FW_RESEARCH_COMMAND_SAMPLE_MAX];
};

struct crystalhd_fw_research_command_observation_result {
	struct crystalhd_fw_research_state_request request;
	struct crystalhd_fw_research_result control;
	struct crystalhd_fw_research_command_observation in_flight;
	/* Existing bracketed five-word reader, used only after INIT completes and
	 * before the following GET_VERSION command is posted.
	 */
	struct crystalhd_fw_research_heap_packet_sample boundary_after_init;
};

#define CRYSTALHD_FW_RESEARCH_RUN_COMMAND_OBSERVATION \
	_IOWR('R', 0x9e, struct crystalhd_fw_research_command_observation_result)

struct crystalhd_fw_research_clock_sample {
	__u32 attempted;
	__s32 status;
	/* Completed fixed control reads, not bus-error or coherence certification. */
	__u32 read_complete;
	__u32 reserved;
	__u32 reset_ctrl;
	__u32 perst_clock_ctrl;
	__u32 clk_pm_ctrl;
};

struct crystalhd_fw_research_clock_result {
	struct crystalhd_fw_research_state_result state;
	struct crystalhd_fw_research_clock_sample after_init;
	struct crystalhd_fw_research_clock_sample after_open;
};

/* Fixed read-only control samples within the stock reset/INIT/OPEN lifecycle. */
#define CRYSTALHD_FW_RESEARCH_RUN_CLOCK \
	_IOWR('R', 0x98, struct crystalhd_fw_research_clock_result)

struct crystalhd_fw_research_uart_sample {
	__u32 attempted;
	__s32 status;
	/* Fixed configuration reads only, not FIFO, board routing or bus health. */
	__u32 read_complete;
	__u32 reserved;
	__u32 arm_uart_ctl;
	__u32 pin_mux_ctrl_0;
	__u32 uart_router_sel;
};

struct crystalhd_fw_research_uart_result {
	struct crystalhd_fw_research_state_result state;
	struct crystalhd_fw_research_uart_sample after_init;
	struct crystalhd_fw_research_uart_sample after_open;
};

/* No UART DATA/STATUS access or UART/pinmux/router configuration writes. */
#define CRYSTALHD_FW_RESEARCH_RUN_UART \
	_IOWR('R', 0x99, struct crystalhd_fw_research_uart_result)

struct crystalhd_fw_research_crypto_sample {
	__u32 attempted;
	__s32 status;
	/* Fixed raw state reads only; the indirect GISB selector is programmed. */
	__u32 read_complete;
	__u32 error_capture_complete;
	__u32 target_reads_attempted;
	__u32 guard_reads_complete;
	__u32 gisb_before;
	__u32 gisb_last;
	__u32 sharf_revision;
	__u32 sharf_status;
	__u32 bop_gr_bridge_revision;
	__u32 bop_aes_status;
	__u32 error_capture_address;
	__u32 error_capture_master;
};

struct crystalhd_fw_research_crypto_result {
	struct crystalhd_fw_research_state_result state;
	struct crystalhd_fw_research_crypto_sample after_init;
	struct crystalhd_fw_research_crypto_sample after_open;
};

/* No crypto control, context, key, IV, nonce, OTP or scrub-register access. */
#define CRYSTALHD_FW_RESEARCH_RUN_CRYPTO \
	_IOWR('R', 0x9a, struct crystalhd_fw_research_crypto_result)

/* Opt-in driver-observer records.  These are research measurements, not part
 * of the legacy CrystalHD ioctl or libcrystalhd ABI.  Each stored record
 * follows one admitted Flea RX DMA packet; admissions beyond capacity are
 * counted in overflow_count.  Missing stages remain zero and are explicit in
 * stage_flags.  Timestamps use the host monotonic clock in nanoseconds.
 */
#define CRYSTALHD_FW_RESEARCH_RX_RECEIPT_VERSION 1U
#define CRYSTALHD_FW_RESEARCH_RX_RECEIPT_READ_RECORDS 32U
#define CRYSTALHD_FW_RESEARCH_RX_RECEIPT_MAX_RECORDS 65536U
#define CRYSTALHD_FW_RESEARCH_RX_RECEIPT_SEAL_CURRENT (1U << 0)

#define CRYSTALHD_FW_RESEARCH_RX_STAGE_RESERVED       (1U << 0)
#define CRYSTALHD_FW_RESEARCH_RX_STAGE_FIRE_RETURN    (1U << 1)
#define CRYSTALHD_FW_RESEARCH_RX_STAGE_IRQ_ENTRY      (1U << 2)
#define CRYSTALHD_FW_RESEARCH_RX_STAGE_COMPLETE       (1U << 3)
#define CRYSTALHD_FW_RESEARCH_RX_STAGE_PARSED         (1U << 4)
#define CRYSTALHD_FW_RESEARCH_RX_STAGE_PARSE_FAILED   (1U << 5)
#define CRYSTALHD_FW_RESEARCH_RX_STAGE_DISPOSITION    (1U << 6)
#define CRYSTALHD_FW_RESEARCH_RX_STAGE_CLOCK_INVERTED (1U << 7)
#define CRYSTALHD_FW_RESEARCH_RX_STAGE_LIST_REPLACED  (1U << 8)

#define CRYSTALHD_FW_RESEARCH_RX_DISPOSITION_UNKNOWN          0U
#define CRYSTALHD_FW_RESEARCH_RX_DISPOSITION_OUTPUT           1U
#define CRYSTALHD_FW_RESEARCH_RX_DISPOSITION_FORMAT_CHANGE    2U
#define CRYSTALHD_FW_RESEARCH_RX_DISPOSITION_RECYCLE_ZERO     3U
#define CRYSTALHD_FW_RESEARCH_RX_DISPOSITION_REPEAT_LAST      4U
#define CRYSTALHD_FW_RESEARCH_RX_DISPOSITION_REPEAT_TWO_BACK  5U

struct crystalhd_fw_research_rx_receipt {
	__aligned_u64 ordinal;
	__aligned_u64 capture_epoch;
	/* Sampled by the outer post wrapper immediately after a successful
	 * fire call returns. A fast IRQ may legitimately precede this sample;
	 * CLOCK_INVERTED records that observed ordering instead of hiding it.
	 */
	__aligned_u64 fire_return_ns;
	/* Entry of the Flea RX ISR invocation that finally completed this list;
	 * two lists completed by one invocation may carry the same timestamp.
	 * This is not a timestamp for the first PCIe payload beat.
	 */
	__aligned_u64 irq_entry_ns;
	__aligned_u64 completion_ns;
	__aligned_u64 parse_ns;
	__aligned_u64 disposition_ns;
	/* Raw firmware timestamp units; this is not a host clock sample/token. */
	__aligned_u64 firmware_timestamp;
	__u32 rx_sequence;
	__u32 list_index;
	__u32 packet_tag;
	__u32 channel_id;
	__s32 completion_status;
	__u32 y_done_words;
	__u32 uv_done_words;
	__u32 y_descriptor_count;
	__u32 uv_descriptor_count;
	__u32 picture_number;
	__u32 picture_flags;
	__u32 picture_width;
	__u32 picture_height;
	__u32 row_width;
	__u32 pib_line;
	__u32 session_number;
	__u32 packet_flags;
	__u32 stage_flags;
	/* OUTPUT means accepted as an output candidate by the driver.  It does
	 * not prove a later userspace DtsProcOutput call returned the picture.
	 */
	__u32 disposition;
	__u32 eos_trailer;
};

struct crystalhd_fw_research_rx_receipt_arm {
	__u32 version;
	__u32 size;
	__u32 capacity;
	__u32 flags;
	__aligned_u64 generation;
	__aligned_u64 run_id;
	__u32 reserved[4];
};

struct crystalhd_fw_research_rx_receipt_seal {
	__u32 version;
	__u32 size;
	__u32 flags;
	__u32 sealed;
	__aligned_u64 generation;
	__aligned_u64 run_id;
	__u32 record_count;
	__u32 overflow_count;
	__u32 reserved[4];
};

struct crystalhd_fw_research_rx_receipt_read {
	__u32 version;
	__u32 size;
	__u32 flags;
	__u32 start;
	__aligned_u64 generation;
	__aligned_u64 run_id;
	__u32 returned;
	__u32 record_count;
	__u32 overflow_count;
	__u32 sealed;
	__u32 reserved[4];
	struct crystalhd_fw_research_rx_receipt
		records[CRYSTALHD_FW_RESEARCH_RX_RECEIPT_READ_RECORDS];
};

#define CRYSTALHD_FW_RESEARCH_RX_RECEIPT_ARM \
	_IOWR('R', 0x9b, struct crystalhd_fw_research_rx_receipt_arm)
#define CRYSTALHD_FW_RESEARCH_RX_RECEIPT_SEAL \
	_IOWR('R', 0x9c, struct crystalhd_fw_research_rx_receipt_seal)
#define CRYSTALHD_FW_RESEARCH_RX_RECEIPT_READ \
	_IOWR('R', 0x9d, struct crystalhd_fw_research_rx_receipt_read)

#endif
