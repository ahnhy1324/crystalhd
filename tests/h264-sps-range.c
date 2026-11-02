/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "crystalhd_h264_sps_range.h"
#include "range-goldens.h"

static unsigned int checks;
#define CHECK(x) do { checks++; if (!(x)) { \
 fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int rejected(const unsigned char *nal, unsigned int bytes,
                    enum chd_h264_range_status expected)
{
	struct chd_h264_sps_range value, before;
	memset(&value, 0, sizeof(value)); value.sps_id = 1234;
	value.vui_present = value.video_signal_present = true;
	value.range_present = value.full_range = true;
	before = value;
	CHECK(chd_h264_read_sps_range(nal, bytes, &value) == expected);
	CHECK(!memcmp(&before, &value, sizeof(value)));
	return 0;
}
int main(void)
{
	unsigned int i, j;
	unsigned char buffer[CHD_H264_SPS_MAX_BYTES + 1];
	struct chd_h264_sps_range value;
	static const unsigned char id32[] = {0x67,0x42,0x00,0x1e,0x04,0x20};
	static const unsigned char overflow[] = {0x67,0x42,0x00,0x1e,
		0,0,3,0,0,3,0,0x80};
	static const unsigned char negative_max[] = {0,0,3,0,1,255,255,255,254};
	static const unsigned char positive_max[] = {0,0,3,0,1,255,255,255,252};
	/* High marker + SPS id0: chroma0/2/3, luma-depth1, chroma-depth1.
	 * These are unsupported syntax prefixes, not full decoded-video fixtures. */
	static const unsigned char unsupported_high[][5] = {
		{0x67,100,0,30,0xc0}, {0x67,100,0,30,0xb0},
		{0x67,100,0,30,0x90}, {0x67,100,0,30,0xa4},
		{0x67,100,0,30,0xaa}
	};
	struct chd_h264_range_bits bits = {negative_max, sizeof(negative_max),0,0,0,0,false};
	CHECK(UINT_MAX == 0xffffffffU && INT_MAX == 2147483647);
	CHECK(chd_h264_range_se(&bits) == -2147483647 && !bits.failed);
	bits.nal = positive_max; bits.offset = bits.zeros = bits.left = 0;
	CHECK(chd_h264_range_se(&bits) == 2147483647 && !bits.failed);
	for (i = 0; i < sizeof(goldens)/sizeof(goldens[0]); i++) {
		const struct range_golden *g = &goldens[i];
		CHECK(chd_h264_read_sps_range(g->nal, g->bytes, &value) == CHD_H264_RANGE_OK);
		CHECK(value.sps_id == g->expected.sps_id);
		CHECK(value.vui_present == g->expected.vui_present);
		CHECK(value.video_signal_present == g->expected.video_signal_present);
		CHECK(value.range_present == g->expected.range_present);
		CHECK(value.full_range == g->expected.full_range);
		for (j = 0; j < g->minimum; j++)
			CHECK(!rejected(g->nal, j, CHD_H264_RANGE_INVALID));
		/* A readable prefix can precede an incomplete suffix. This API does
		 * not claim full SPS syntax validity or decode acceptance. */
		CHECK(chd_h264_read_sps_range(g->nal, g->minimum, &value) == CHD_H264_RANGE_OK);
	}
	CHECK(!rejected(NULL, 4, CHD_H264_RANGE_INVALID));
	CHECK(chd_h264_read_sps_range(goldens[0].nal, goldens[0].bytes, NULL) == CHD_H264_RANGE_INVALID);
	CHECK(!rejected(id32, sizeof(id32), CHD_H264_RANGE_INVALID));
	CHECK(!rejected(overflow, sizeof(overflow), CHD_H264_RANGE_INVALID));
	for (i = 0; i < sizeof(unsupported_high)/sizeof(unsupported_high[0]); i++)
		CHECK(!rejected(unsupported_high[i], sizeof(unsupported_high[i]), CHD_H264_RANGE_UNSUPPORTED));
	memcpy(buffer, goldens[0].nal, goldens[0].bytes);
	buffer[0] = 0xe7; CHECK(!rejected(buffer, goldens[0].bytes, CHD_H264_RANGE_INVALID));
	buffer[0] = 0x07; CHECK(!rejected(buffer, goldens[0].bytes, CHD_H264_RANGE_INVALID));
	buffer[0] = 0x68; CHECK(!rejected(buffer, goldens[0].bytes, CHD_H264_RANGE_INVALID));
	buffer[0] = 0x67; buffer[2] |= 1;
	CHECK(!rejected(buffer, goldens[0].bytes, CHD_H264_RANGE_INVALID));
	memcpy(buffer, goldens[0].nal, goldens[0].bytes); buffer[1] = 122;
	CHECK(!rejected(buffer, goldens[0].bytes, CHD_H264_RANGE_UNSUPPORTED));
	memcpy(buffer, goldens[0].nal, goldens[0].bytes);
	for (i = 0; i < 4; i++) {
		unsigned int n = goldens[0].bytes;
		buffer[n] = buffer[n+1] = 0; buffer[n+2] = 3; buffer[n+3] = i;
		CHECK(chd_h264_read_sps_range(buffer, n + 4, &value) == CHD_H264_RANGE_OK);
	}
	for (i = 0; i < 5; i++) {
		unsigned int n = goldens[0].bytes;
		buffer[n] = buffer[n+1] = 0; buffer[n+2] = i < 3 ? i : 3;
		buffer[n+3] = 4;
		CHECK(!rejected(buffer, n + (i == 3 ? 3 : 4), CHD_H264_RANGE_INVALID));
	}
	memcpy(buffer, goldens[0].nal, goldens[0].bytes);
	memset(buffer + goldens[0].bytes, 255, sizeof(buffer) - goldens[0].bytes);
	CHECK(chd_h264_read_sps_range(buffer, CHD_H264_SPS_MAX_BYTES, &value) == CHD_H264_RANGE_OK);
	CHECK(!rejected(buffer, sizeof(buffer), CHD_H264_RANGE_INVALID));
	printf("H264 SPS range prefix: %u goldens, %u checks PASS\n",
	       (unsigned int)(sizeof(goldens)/sizeof(goldens[0])), checks);
	return 0;
}
