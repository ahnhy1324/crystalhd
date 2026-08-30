// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _CRYSTALHD_COMPAT_IOCTL_H_
#define _CRYSTALHD_COMPAT_IOCTL_H_

#ifdef CONFIG_COMPAT

#include <linux/compat.h>

/* vdec_info.h is packed, so these mirrors deliberately are too. */
struct crystalhd_ppb_mpeg32 {
	u32 to_be_defined;
	u32 valid;
	u32 display_horizontal_size;
	u32 display_vertical_size;
	u32 offset_count;
	s32 horizontal_offset[3];
	s32 vertical_offset[3];
	s32 user_data_size;
	compat_uptr_t user_data;
} __packed;

struct crystalhd_ppb_vc132 {
	u32 to_be_defined;
	u32 valid;
	u32 display_horizontal_size;
	u32 display_vertical_size;
	u32 num_panscan_windows;
	s32 ps_horiz_offset[4];
	s32 ps_vert_offset[4];
	s32 ps_width[4];
	s32 ps_height[4];
	s32 user_data_size;
	compat_uptr_t user_data;
} __packed;

struct crystalhd_ppb_h26432 {
	u32 valid;
	s32 poc_top;
	s32 poc_bottom;
	u32 idr_pic_id;
	u32 pan_scan_count;
	s32 pan_scan_left[3];
	s32 pan_scan_right[3];
	s32 pan_scan_top[3];
	s32 pan_scan_bottom[3];
	u32 ct_type_count;
	u32 ct_type[3];
	s32 sps_crop_left;
	s32 sps_crop_right;
	s32 sps_crop_top;
	s32 sps_crop_bottom;
	u32 chroma_top;
	u32 chroma_bottom;
	u32 user_data_size;
	compat_uptr_t user_data;
	compat_uptr_t fgt;
} __packed;

union crystalhd_ppb_other32 {
	struct crystalhd_ppb_h26432 h264;
	struct crystalhd_ppb_mpeg32 mpeg;
	struct crystalhd_ppb_vc132 vc1;
};

struct crystalhd_ppb32 {
	u8 common[offsetof(struct PPB, other)];
	union crystalhd_ppb_other32 other;
} __packed;

struct crystalhd_pib32 {
	u32 b_format_change;
	u32 resolution;
	u32 channel_id;
	u32 ppb_ptr;
	s32 pts_stc_offset;
	u32 zero_panscan_valid;
	u32 dram_out_buf_addr;
	u32 y_component;
	struct crystalhd_ppb32 ppb;
};

struct crystalhd_yuv_bufs32 {
	u32 b422_mode;
	compat_uptr_t yuv_buf;
	u32 yuv_buf_size;
	u32 uv_buf_offset;
	u32 y_done_size;
	u32 uv_done_size;
	u32 ref_count;
};

struct crystalhd_dec_out32 {
	struct crystalhd_yuv_bufs32 output;
	struct crystalhd_pib32 pib;
	u32 flags;
	u32 bad_frame_count;
};

struct crystalhd_proc_input32 {
	compat_uptr_t dma_buf;
	u32 buffer_size;
	u8 mapped;
	u8 encrypted;
	u8 reserved[2];
	u32 dram_offset;
};

struct crystalhd_ioctl_data32 {
	s32 ret_status;
	u32 ioctl_data_size;
	u32 timeout;
	union {
		u8 raw[sizeof(BC_FW_CMD)];
		struct crystalhd_proc_input32 proc_input;
		struct crystalhd_yuv_bufs32 rx_bufs;
		struct crystalhd_dec_out32 dec_out;
	} u;
	compat_uptr_t next;
};

static_assert(sizeof(struct crystalhd_ppb_mpeg32) == 52);
static_assert(sizeof(struct crystalhd_ppb_vc132) == 92);
static_assert(sizeof(struct crystalhd_ppb_h26432) == 120);
static_assert(sizeof(struct crystalhd_ppb32) == 232);
static_assert(sizeof(struct crystalhd_pib32) == 264);
static_assert(sizeof(struct crystalhd_yuv_bufs32) == 28);
static_assert(sizeof(struct crystalhd_dec_out32) == 300);
static_assert(sizeof(struct crystalhd_proc_input32) == 16);
static_assert(offsetof(struct crystalhd_ioctl_data32, u) == 12);
static_assert(offsetof(struct crystalhd_ioctl_data32, next) == 532);
static_assert(sizeof(struct crystalhd_ioctl_data32) == 536);

#endif /* CONFIG_COMPAT */

#endif /* _CRYSTALHD_COMPAT_IOCTL_H_ */
