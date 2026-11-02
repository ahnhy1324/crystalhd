/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CRYSTALHD_H264_SPS_RANGE_H
#define CRYSTALHD_H264_SPS_RANGE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#endif

/* Private, allocation-free SPS prefix reader. This does not validate a whole
 * SPS, bind a picture to an SPS, or establish decoded-pixel range. Admission
 * users must snapshot observations per token; this sets no output policy.
 * ITU-T H.264 (08/2024), 7.3.2.1.1 and E.1.1.
 * Input is ONE SPS NAL (header + EBSP), without an Annex-B start code or length.
 */
#define CHD_H264_SPS_MAX_BYTES 4096U
enum chd_h264_range_status {
	CHD_H264_RANGE_OK, CHD_H264_RANGE_INVALID, CHD_H264_RANGE_UNSUPPORTED
};
struct chd_h264_sps_range {
	unsigned int sps_id;
	bool vui_present, video_signal_present, range_present, full_range;
	/* Additional input-prefix facts for bounded progressive picture grouping.
	 * Geometry is never used for allocation by this reader.
	 */
	unsigned int frame_num_bits, poc_type, pic_size_in_map_units;
	bool frame_mbs_only;
};
struct chd_h264_range_bits {
	const unsigned char *nal;
	unsigned int bytes, offset, zeros, left, byte;
	bool failed;
};

static inline unsigned int chd_h264_range_get(struct chd_h264_range_bits *b,
	unsigned int count)
{
	unsigned int value = 0;
	while (count-- && !b->failed) {
		if (!b->left) {
			if (b->offset >= b->bytes) { b->failed = true; break; }
			b->byte = b->nal[b->offset++];
			if (b->zeros == 2) {
				if (b->byte < 3) { b->failed = true; break; }
				if (b->byte == 3) {
					if (b->offset >= b->bytes || b->nal[b->offset] > 3) {
						b->failed = true; break;
					}
					b->byte = b->nal[b->offset++]; b->zeros = 0;
				}
			}
			b->zeros = b->byte ? 0 : b->zeros + 1;
			b->left = 8;
		}
		value = (value << 1) | ((b->byte >> --b->left) & 1);
	}
	return value;
}
static inline unsigned int chd_h264_range_ue(struct chd_h264_range_bits *b)
{
	unsigned int zeros = 0, suffix;
	while (!b->failed && !chd_h264_range_get(b, 1)) {
		if (++zeros > 31) { b->failed = true; return 0; }
	}
	if (b->failed) return 0;
	suffix = chd_h264_range_get(b, zeros);
	return ((1U << zeros) - 1) + suffix;
}
static inline int chd_h264_range_se(struct chd_h264_range_bits *b)
{
	unsigned int code = chd_h264_range_ue(b);
	return code & 1 ? (int)((code >> 1) + 1) : -(int)(code >> 1);
}
static inline bool chd_h264_range_ebsp(const unsigned char *nal, unsigned int bytes)
{
	unsigned int i, zeros = 0;
	for (i = 1; i < bytes; i++) {
		unsigned int value = nal[i];
		if (zeros == 2) {
			if (value < 3) return false;
			if (value == 3) {
				if (i + 1 == bytes || nal[i + 1] > 3) return false;
				zeros = 0; continue;
			}
		}
		zeros = value ? 0 : zeros + 1;
	}
	return true;
}
static inline enum chd_h264_range_status chd_h264_read_sps_range(
	const unsigned char *nal, unsigned int bytes, struct chd_h264_sps_range *result)
{
	struct chd_h264_sps_range out = {0};
	struct chd_h264_range_bits b = {nal, bytes, 1, 0, 0, 0, false};
	unsigned int profile, value, i, width, height;
	unsigned long long map_units;
	if (!nal || !result || bytes < 4 || bytes > CHD_H264_SPS_MAX_BYTES ||
	    (nal[0] & 0x80) || !(nal[0] & 0x60) || (nal[0] & 31) != 7 ||
	    !chd_h264_range_ebsp(nal, bytes)) return CHD_H264_RANGE_INVALID;
	profile = chd_h264_range_get(&b, 8);
	if (chd_h264_range_get(&b, 8) & 3) return CHD_H264_RANGE_INVALID;
	chd_h264_range_get(&b, 8); /* level_idc; no level/conformance claim */
	out.sps_id = chd_h264_range_ue(&b);
	if (b.failed || out.sps_id > 31) return CHD_H264_RANGE_INVALID;
	if (profile != 66 && profile != 77 && profile != 100)
		return CHD_H264_RANGE_UNSUPPORTED;
	if (profile == 100) {
		/* Initial scope: High 8-bit 4:2:0, including explicit scaling lists. */
		if (chd_h264_range_ue(&b) != 1 || chd_h264_range_ue(&b) ||
		    chd_h264_range_ue(&b))
			return b.failed ? CHD_H264_RANGE_INVALID : CHD_H264_RANGE_UNSUPPORTED;
		chd_h264_range_get(&b, 1); /* qpprime_y_zero_transform_bypass_flag */
		if (chd_h264_range_get(&b, 1)) {
			for (i = 0; i < 8 && !b.failed; i++) {
				if (chd_h264_range_get(&b, 1)) {
					unsigned int j, last = 8, next = 8;
					for (j = 0; j < (i < 6 ? 16U : 64U) && !b.failed; j++) {
						if (next) {
							int delta = chd_h264_range_se(&b);
							if (delta < -128 || delta > 127) b.failed = true;
							next = (last + delta + 256) & 255;
						}
						if (next) last = next;
					}
				}
			}
		}
	}
	value = chd_h264_range_ue(&b);
	if (value > 12) return CHD_H264_RANGE_INVALID;
	out.frame_num_bits = value + 4;
	value = chd_h264_range_ue(&b); /* pic_order_cnt_type */
	if (value > 2) return CHD_H264_RANGE_INVALID;
	out.poc_type = value;
	if (!value) {
		if (chd_h264_range_ue(&b) > 12) return CHD_H264_RANGE_INVALID;
	} else if (value == 1) {
		chd_h264_range_get(&b, 1);
		chd_h264_range_se(&b); chd_h264_range_se(&b);
		value = chd_h264_range_ue(&b);
		if (value > 255) return CHD_H264_RANGE_INVALID;
		for (i = 0; i < value && !b.failed; i++) chd_h264_range_se(&b);
	}
	if (chd_h264_range_ue(&b) > 16) return CHD_H264_RANGE_INVALID;
	chd_h264_range_get(&b, 1); /* gaps_in_frame_num_value_allowed_flag */
	width = chd_h264_range_ue(&b); height = chd_h264_range_ue(&b);
	map_units = (width + 1ULL) * (height + 1ULL);
	if (map_units <= ~0U) out.pic_size_in_map_units = (unsigned int)map_units;
	out.frame_mbs_only = chd_h264_range_get(&b, 1) != 0;
	if (!out.frame_mbs_only) chd_h264_range_get(&b, 1); /* MBAFF */
	chd_h264_range_get(&b, 1);
	if (chd_h264_range_get(&b, 1))
		for (i = 0; i < 4; i++) chd_h264_range_ue(&b);
	out.vui_present = chd_h264_range_get(&b, 1) != 0;
	if (out.vui_present && !b.failed) {
		if (chd_h264_range_get(&b, 1)) {
			value = chd_h264_range_get(&b, 8);
			if (value == 255) {
				unsigned int width = chd_h264_range_get(&b, 16);
				unsigned int height = chd_h264_range_get(&b, 16);
				if (!width || !height) return CHD_H264_RANGE_INVALID;
			} else if (value > 16) return CHD_H264_RANGE_INVALID;
		}
		if (chd_h264_range_get(&b, 1)) chd_h264_range_get(&b, 1);
		out.video_signal_present = chd_h264_range_get(&b, 1) != 0;
		if (out.video_signal_present) {
			if (chd_h264_range_get(&b, 3) > 5) return CHD_H264_RANGE_INVALID;
			out.range_present = true;
			out.full_range = chd_h264_range_get(&b, 1) != 0;
			if (chd_h264_range_get(&b, 1)) chd_h264_range_get(&b, 24);
		}
	}
	if (b.failed) return CHD_H264_RANGE_INVALID;
	/* Absent is retained separately; do not silently promote a default to an
	 * observed explicit source flag. All error exits preserve caller state. */
	*result = out;
	return CHD_H264_RANGE_OK;
}
#endif
