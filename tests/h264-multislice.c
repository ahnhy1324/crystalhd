/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "crystalhd_h264_range_binding.h"
#include "fixtures/issue538/h264-multislice-prefixes-20261102.h"
#include "fixtures/issue538/h264-poc0-multislice-prefixes-20261102.h"

static const unsigned char full[] = CHD_TEST_MULTI2_SPS;
static const unsigned char limited[] = CHD_TEST_MULTI_SPS_LIMITED;
static const unsigned char pps[] = CHD_TEST_MULTI2_PPS;
static const unsigned char idr0[] = CHD_TEST_MULTI2_VCL0;
static const unsigned char idr1[] = CHD_TEST_MULTI2_VCL1;
static const unsigned char p0[] = CHD_TEST_MULTI2_VCL2;
static const unsigned char p1[] = CHD_TEST_MULTI2_VCL3;
static const unsigned char other_picture[] = CHD_TEST_MULTI2_VCL5;
static const unsigned char four0[] = CHD_TEST_MULTI4_VCL0;
static const unsigned char four1[] = CHD_TEST_MULTI4_VCL1;
static const unsigned char four2[] = CHD_TEST_MULTI4_VCL2;
static const unsigned char four3[] = CHD_TEST_MULTI4_VCL3;
static unsigned char au[4096], rbsp[128], prefix[160];
static unsigned int bytes, bit, checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static void add(const unsigned char *nal, unsigned int size)
{
	assert(bytes + size + 3 < sizeof(au));
	au[bytes++] = 0; au[bytes++] = 0; au[bytes++] = 1;
	memcpy(au + bytes, nal, size); bytes += size;
}
static void put(unsigned int value, unsigned int count)
{
	assert(bit + count < sizeof(rbsp) * 8);
	while (count--) { rbsp[bit / 8] |= ((value >> count) & 1) << (7 - bit % 8); bit++; }
}
static void ue(unsigned int value)
{
	unsigned int width = 0, n = value + 1;
	assert(n);
	while (n >>= 1) width++;
	put(0, width); put(value + 1, width + 1);
}
static unsigned int finish(unsigned int header)
{
	unsigned int i, size, used = 1, zeros = 0;
	put(1, 1); size = (bit + 7) / 8; prefix[0] = header;
	for (i = 0; i < size; i++) {
		if (zeros == 2 && rbsp[i] <= 3) { prefix[used++] = 3; zeros = 0; }
		prefix[used++] = rbsp[i]; zeros = rbsp[i] ? 0 : zeros + 1;
	}
	return used;
}
/* Synthetic slice-header controls, not decoded pictures. The bit layout is
 * independent of the production reader, fixed to the traced four-bit SPS.
 */
static void synthetic(unsigned int first, unsigned int picture, bool idr,
	unsigned int id, bool reference, unsigned int parameter, unsigned int type)
{
	unsigned int size;
	memset(rbsp, 0, sizeof(rbsp)); bit = 0;
	ue(first); ue(type); ue(parameter); put(picture, 4);
	if (idr) ue(id);
	size = finish((reference ? 0x60 : 0) | (idr ? 5 : 1));
	add(prefix, size);
}
static void parameter_bottom(unsigned int id, unsigned int groups, bool redundant, bool bottom)
{
	unsigned int size;
	memset(rbsp, 0, sizeof(rbsp)); bit = 0;
	ue(id); ue(0); put(0, 1); put(bottom, 1); ue(groups);
	/* Deliberately stop unsupported FMO before its map syntax. */
	if (!groups) {
		ue(0); ue(0); put(0, 1); put(0, 2);
		ue(0); ue(0); ue(0); /* signed zero QP/QS/chroma offsets */
		put(1, 1); put(0, 1); put(redundant, 1);
	}
	size = finish(0x68); add(prefix, size);
}
static void parameter(unsigned int id, unsigned int groups, bool redundant)
{
    parameter_bottom(id, groups, redundant, false);
}
static void config(void)
{
	bytes = 0; add(full, sizeof(full)); add(pps, sizeof(pps));
}
static void pair(void)
{
	bytes = 0; add(idr0, sizeof(idr0)); add(idr1, sizeof(idr1));
}
/* Independent synthetic POC0 fields after the traced frame_num/IDR prefix.
 * Only the header is generated; these controls are not decoded pictures. */
static unsigned int poc_slice(unsigned int first, unsigned int lsb, int delta,
    bool bottom)
{
    unsigned int size;
    memset(rbsp, 0, sizeof(rbsp)); bit = 0;
    ue(first); ue(1); ue(0); put(2, 4); put(lsb, 5);
    if (bottom) ue(delta > 0 ? (unsigned int)delta * 2 - 1 : (unsigned int)-delta * 2);
    size = finish(1); add(prefix, size); return size;
}
static int poc0_checks(void)
{
    static const unsigned char sps[] = CHD_TEST_POC0_2_SPS;
    static const unsigned char lim[] = CHD_TEST_POC0_SPS_LIMITED;
    static const unsigned char param[] = CHD_TEST_POC0_2_PPS;
    static const unsigned char vcl[][16] = {
        CHD_TEST_POC0_2_VCL0, CHD_TEST_POC0_2_VCL1,
        CHD_TEST_POC0_2_VCL2, CHD_TEST_POC0_2_VCL3,
        CHD_TEST_POC0_2_VCL4, CHD_TEST_POC0_2_VCL5,
        CHD_TEST_POC0_2_VCL6, CHD_TEST_POC0_2_VCL7 };
    static const unsigned char four[][16] = { CHD_TEST_POC0_4_VCL8,
        CHD_TEST_POC0_4_VCL9, CHD_TEST_POC0_4_VCL10, CHD_TEST_POC0_4_VCL11 };
    struct chd_h264_range_state state = {0};
    struct chd_h264_range_snapshot a, b;
    struct chd_h264_range_picture x, y;
    struct chd_h264_sps_range value;
    unsigned int i, size;
    CHECK(chd_h264_read_sps_range(sps, sizeof(sps), &value) == CHD_H264_RANGE_OK);
    CHECK(value.poc_type == 0 && value.poc_lsb_bits == 5 && value.frame_num_bits == 4);
    bytes=0; add(sps,sizeof(sps)); add(param,sizeof(param));
    add(vcl[0],16);add(vcl[1],16);
    chd_h264_observe_range(&state,au,bytes,&a);
    CHECK(a.associated && a.full_range && a.sps_generation == 1 &&
        !state.pps[0].bottom_field_poc_present);
    for(i=1;i<4;i++) {
        bytes=0;add(vcl[i*2],16);add(vcl[i*2+1],16);
        chd_h264_observe_range(&state,au,bytes,&b); CHECK(b.associated && b.full_range);
    }
    CHECK(chd_h264_range_picture_prefix(&state,vcl[4],16,&x));
    CHECK(chd_h264_range_picture_prefix(&state,vcl[7],16,&y));
    CHECK(x.frame_num == 2 && y.frame_num == 2 && !x.reference && !y.reference &&
        x.pic_order_cnt_lsb == 2 && y.pic_order_cnt_lsb == 4 &&
        !chd_h264_range_same_picture(&x,&y));
    bytes=0;add(vcl[4],16);add(vcl[7],16);
    chd_h264_observe_range(&state,au,bytes,&b); CHECK(!b.associated);
    bytes=0;for(i=0;i<4;i++)add(four[i],16);
    chd_h264_observe_range(&state,au,bytes,&b);CHECK(b.associated && b.full_range);
    bytes=0;add(lim,sizeof(lim));add(vcl[4],16);add(vcl[5],16);
    chd_h264_observe_range(&state,au,bytes,&b);
    CHECK(b.associated && !b.full_range && b.sps_generation == 2 && a.full_range);
    /* Delta is signed and is present even in a progressive frame picture. */
    for(i=0;i<2;i++) {
        int delta=i ? -19 : 19;
        bytes=0;parameter_bottom(0,0,false,true);
        poc_slice(0,2,delta,true);size=poc_slice(480,2,delta,true);
        chd_h264_observe_range(&state,au,bytes,&b);
        CHECK(b.associated && state.pps[0].bottom_field_poc_present);
        CHECK(chd_h264_range_picture_prefix(&state,prefix,
            size,&x) && x.delta_pic_order_cnt_bottom==delta);
        bytes=0;poc_slice(0,2,delta,true);poc_slice(480,2,-delta,true);
        chd_h264_observe_range(&state,au,bytes,&b); CHECK(!b.associated);
        bytes=0;poc_slice(0,2,delta,true);size=poc_slice(480,2,delta,true);
        bytes-=size+3;add(prefix,size-2);
        chd_h264_observe_range(&state,au,bytes,&b);
        CHECK(!b.associated && state.pps[0].picture_qualified);
    }
    bytes=0;parameter(0,0,false);poc_slice(0,2,0,false);poc_slice(480,2,0,false);
    chd_h264_observe_range(&state,au,bytes,&b);
    CHECK(b.associated && !state.pps[0].bottom_field_poc_present);
    /* With bottom=0 these different SE-looking payload bits are not delta. */
    bytes=0;size=poc_slice(0,2,19,true);
    CHECK(chd_h264_range_picture_prefix(&state,prefix,size,&x) && !x.delta_pic_order_cnt_bottom);
    size=poc_slice(480,2,-19,true);
    CHECK(chd_h264_range_picture_prefix(&state,prefix,size,&y) && !y.delta_pic_order_cnt_bottom);
    chd_h264_observe_range(&state,au,bytes,&b);CHECK(b.associated);
    for(i=1;i<=4;i++) {
        bytes=0;poc_slice(0,2,0,false);size=poc_slice(480,2,0,false);
        bytes-=size+3;add(prefix,i);
        chd_h264_observe_range(&state,au,bytes,&b);
        CHECK(!b.associated && state.pps[0].picture_qualified);
    }
    for(i=0;i<2;i++) {
        state.sps[0].value.poc_lsb_bits=i?17:3;
        bytes=0;add(vcl[4],16);add(vcl[5],16);
        chd_h264_observe_range(&state,au,bytes,&b);CHECK(!b.associated);
    }
    state.sps[0].value.poc_lsb_bits=5;
    bytes=0;parameter_bottom(0,0,false,true);
    chd_h264_observe_range(&state,au,bytes,&b);CHECK(state.pps[0].bottom_field_poc_present);
    { static const unsigned char short_pps[]={0x68,0xe0};
      bytes=0;add(short_pps,sizeof(short_pps));add(vcl[4],16);add(vcl[5],16);
      chd_h264_observe_range(&state,au,bytes,&b);
      CHECK(!b.associated && !state.pps[0].picture_qualified &&
          !state.pps[0].bottom_field_poc_present); }
    /* POC2 must not read POC0 fields, even when the PPS bottom flag is set. */
    config();parameter_bottom(0,0,false,true);add(idr0,sizeof(idr0));add(idr1,sizeof(idr1));
    chd_h264_observe_range(&state,au,bytes,&b);
    CHECK(b.associated && state.pps[0].bottom_field_poc_present);
    return 0;
}
int main(void)
{
	struct chd_h264_range_state state = {0};
	struct chd_h264_range_snapshot a, b;
	struct chd_h264_sps_range sps;
	struct chd_h264_range_picture x, y;
	unsigned int n;
	CHECK(chd_h264_read_sps_range(full, sizeof(full), &sps) == CHD_H264_RANGE_OK);
	CHECK(sps.frame_num_bits == 4 && sps.poc_type == 2 && sps.frame_mbs_only &&
	      sps.pic_size_in_map_units == 40 * 23);
	config(); add(idr0, sizeof(idr0)); add(idr1, sizeof(idr1));
	chd_h264_observe_range(&state, au, bytes, &a);
	CHECK(a.associated && a.full_range && a.range_present && a.sps_generation == 1);
	CHECK(state.pps[0].picture_qualified);
	CHECK(chd_h264_range_picture_prefix(&state, idr0, sizeof(idr0), &x));
	CHECK(chd_h264_range_picture_prefix(&state, idr1, sizeof(idr1), &y));
	CHECK(x.first_mb == 0 && y.first_mb == 480 && !x.frame_num && !y.frame_num &&
	      x.idr && y.idr && !x.idr_pic_id && !y.idr_pic_id && x.reference && y.reference);
	bytes = 0; add(p0, sizeof(p0)); add(p1, sizeof(p1));
	chd_h264_observe_range(&state, au, bytes, &b); CHECK(b.associated && b.full_range);
	CHECK(chd_h264_range_picture_prefix(&state, p1, sizeof(p1), &y) &&
	      y.frame_num == 1 && !y.idr && y.reference);
	bytes = 0; add(four0, sizeof(four0)); add(four1, sizeof(four1));
	add(four2, sizeof(four2)); add(four3, sizeof(four3));
	chd_h264_observe_range(&state, au, bytes, &b); CHECK(b.associated && b.full_range);
	bytes = 0; add(limited, sizeof(limited)); add(idr0, sizeof(idr0)); add(idr1, sizeof(idr1));
	chd_h264_observe_range(&state, au, bytes, &b);
	CHECK(b.associated && !b.full_range && b.sps_generation == 2);
	CHECK(a.full_range && a.sps_generation == 1);
	/* Actual different P pictures have different frame_num. */
	bytes = 0; add(p0, sizeof(p0)); add(other_picture, sizeof(other_picture));
	chd_h264_observe_range(&state, au, bytes, &b); CHECK(!b.associated);
	for (n = 0; n < 6; n++) {
		bytes = 0;
		synthetic(0, 0, true, 0, true, 0, 2);
		/* Isolate each necessary picture key, raster order and MB extent. */
		if (n == 0) synthetic(480, 0, true, 1, true, 0, 2);
		if (n == 1) synthetic(480, 0, false, 0, true, 0, 2);
		if (n == 2) synthetic(0, 0, true, 0, true, 0, 2);
		if (n == 3) synthetic(920, 0, true, 0, true, 0, 2);
		if (n == 4) { synthetic(480, 0, true, 0, true, 0, 2); synthetic(240, 0, true, 0, true, 0, 2); }
		if (n == 5) { synthetic(480, 0, true, 0, true, 0, 2); synthetic(480, 0, true, 0, true, 0, 2); }
		chd_h264_observe_range(&state, au, bytes, &b); CHECK(!b.associated);
	}
	bytes = 0; synthetic(0, 1, false, 0, true, 0, 0); synthetic(480, 1, false, 0, false, 0, 0);
	chd_h264_observe_range(&state, au, bytes, &b); CHECK(!b.associated);
	/* Distinct nonzero nal_ref_idc priorities still identify one reference picture. */
	bytes = 0; add(p0, sizeof(p0)); memcpy(prefix, p1, sizeof(p1)); prefix[0] = 0x61;
	add(prefix, sizeof(p1)); chd_h264_observe_range(&state, au, bytes, &b); CHECK(b.associated);
	bytes = 0; parameter(1, 0, false);
	synthetic(0, 1, false, 0, true, 0, 0); synthetic(480, 1, false, 0, true, 1, 0);
	chd_h264_observe_range(&state, au, bytes, &b); CHECK(!b.associated && state.pps[1].picture_qualified);
	for (n = 0; n < 5; n++) {
		config(); add(idr0, sizeof(idr0)); add(idr1, n);
		chd_h264_observe_range(&state, au, bytes, &b);
		CHECK(!b.associated && (n == 0 || state.pps[0].valid));
	}
	/* Reload real parameters after the empty-NAL cache invalidation above. */
	config(); add(idr0, sizeof(idr0)); add(idr1, sizeof(idr1));
	chd_h264_observe_range(&state, au, bytes, &b); CHECK(b.associated);
	for (n = 0; n < 2; n++) {
		if (n == 0) state.sps[0].value.poc_type = 3;
		else { state.sps[0].value.poc_type = 2; state.sps[0].value.frame_mbs_only = false; }
		pair(); chd_h264_observe_range(&state, au, bytes, &b); CHECK(!b.associated);
		bytes = 0; add(idr0, sizeof(idr0));
		chd_h264_observe_range(&state, au, bytes, &b); CHECK(b.associated);
	}
	for (n = 0; n < 2; n++) {
		config(); parameter(0, n == 0 ? 1 : 0, n == 1);
		add(idr0, sizeof(idr0)); add(idr1, sizeof(idr1));
		chd_h264_observe_range(&state, au, bytes, &b);
		CHECK(!b.associated && state.pps[0].valid && !state.pps[0].picture_qualified);
	}
	{
		static const unsigned char short_pps[] = {0x68, 0xe0};
		config(); add(short_pps, sizeof(short_pps)); add(idr0, sizeof(idr0)); add(idr1, sizeof(idr1));
		chd_h264_observe_range(&state, au, bytes, &b);
		CHECK(!b.associated && state.pps[0].valid && !state.pps[0].picture_qualified);
	}
	config(); add(idr0, sizeof(idr0)); add(idr1, sizeof(idr1));
	add(p0, sizeof(p0)); add(p1, sizeof(p1));
	chd_h264_observe_range(&state, au, bytes, &b); CHECK(!b.associated);
	config(); add(idr0, sizeof(idr0)); add(limited, sizeof(limited)); add(idr1, sizeof(idr1));
	chd_h264_observe_range(&state, au, bytes, &b); CHECK(!b.associated);
	pair(); chd_h264_observe_range(&state, au, bytes, &b); CHECK(b.associated && !b.full_range);
	bytes = 0; add(idr0, sizeof(idr0)); parameter(0, 0, false); add(idr1, sizeof(idr1));
	chd_h264_observe_range(&state, au, bytes, &b); CHECK(!b.associated);
	bytes = 0; synthetic(0, 0, true, 65535, true, 0, 2); synthetic(919, 0, true, 65535, true, 0, 2);
	chd_h264_observe_range(&state, au, bytes, &b); CHECK(b.associated);
	bytes = 0; synthetic(0, 0, true, 65536, true, 0, 2); synthetic(480, 0, true, 65536, true, 0, 2);
	chd_h264_observe_range(&state, au, bytes, &b); CHECK(!b.associated);
	CHECK(!poc0_checks());
	printf("H264 progressive POC0/2 multi-slice association: %u checks PASS\n", checks);
	return 0;
}
