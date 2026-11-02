/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crystalhd_h264_range_binding.h"
#include "fixtures/issue538/h264-range-nals-20261102.h"

static const unsigned char limited[] = CHD_TEST_SPS_LIMITED;
static const unsigned char full[] = CHD_TEST_SPS_FULL;
static const unsigned char id31[] = CHD_TEST_SPS_ID31;
static const unsigned char absent[] = CHD_TEST_SPS_ABSENT;
static const unsigned char pps[] = CHD_TEST_PPS;
static const unsigned char pps31[] = CHD_TEST_PPS31;
static const unsigned char slice[] = CHD_TEST_VCL;
static unsigned char au[2048];
static unsigned int bytes, checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static void add(const unsigned char *nal, unsigned int size, unsigned int prefix)
{
	assert(bytes + size + prefix < sizeof(au));
	memset(au + bytes, 0, prefix); au[bytes + prefix - 1] = 1; bytes += prefix;
	memcpy(au + bytes, nal, size); bytes += size;
}
static void config(const unsigned char *sps, unsigned int size)
{
	bytes = 0; add(sps, size, 4); add(pps, sizeof(pps), 3); add(slice, sizeof(slice), 4);
}
int main(void)
{
	struct chd_h264_range_state state = {0};
	struct chd_h264_range_snapshot a, b, c;
	static const unsigned char bad_sps[] = {0x67,66}, bad_pps[] = {0x68};
	static const unsigned char not_first[] = {0x41,0x4f};
	static const unsigned char aud[] = {9,0xf0};
	unsigned int i;
	config(limited, sizeof(limited));
	chd_h264_observe_range(&state, au, bytes, &a);
	CHECK(a.associated && a.range_present && !a.full_range && a.sps_generation == 1 && !a.sps_id && !a.pps_id);
	bytes = 0; add(full, sizeof(full), 3); add(slice, sizeof(slice), 3);
	chd_h264_observe_range(&state, au, bytes, &b);
	CHECK(b.associated && b.full_range && b.sps_generation == 2);
	CHECK(a.associated && !a.full_range && a.sps_generation == 1);
	bytes = 0; add(pps31, sizeof(pps31), 4); add(slice, sizeof(slice), 3);
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(!c.associated);
	bytes = 0; add(id31, sizeof(id31), 4); add(slice, sizeof(slice), 3);
	chd_h264_observe_range(&state, au, bytes, &c);
	CHECK(c.associated && c.sps_id == 31 && !c.pps_id && c.full_range && c.sps_generation == 3);
	CHECK(a.sps_generation == 1 && !a.full_range && b.sps_generation == 2 && b.full_range);
	/* Invalid redefinitions cannot revive an earlier range by ID. */
	bytes = 0; add(bad_sps, sizeof(bad_sps), 3); add(slice, sizeof(slice), 3);
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(!c.associated);
	bytes = 0; add(slice, sizeof(slice), 3);
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(!c.associated);
	config(limited, sizeof(limited)); chd_h264_observe_range(&state, au, bytes, &c);
	CHECK(c.associated && c.sps_generation == 4);
	bytes = 0; add(bad_pps, sizeof(bad_pps), 3); add(slice, sizeof(slice), 3);
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(!c.associated);
	bytes = 0; add(slice, sizeof(slice), 3);
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(!c.associated);
	bytes = 0; add(pps, sizeof(pps), 3); add(slice, sizeof(slice), 3);
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(c.associated && c.sps_generation == 4);
	/* Different slices/AUs are never inferred to be one picture. */
	bytes = 0; add(slice, sizeof(slice), 3); add(slice, sizeof(slice), 3);
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(!c.associated);
	bytes = 0; add(not_first, sizeof(not_first), 3);
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(!c.associated);
	bytes = 0; add(slice, sizeof(slice), 3); add(full, sizeof(full), 4);
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(!c.associated);
	bytes = 0; add(slice, sizeof(slice), 3);
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(c.associated && c.full_range && c.sps_generation == 5);
	bytes = 0; add(id31, sizeof(id31), 3); add(slice, sizeof(slice), 3);
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(c.associated && !c.sps_id);
	bytes = 0; add(slice, sizeof(slice), 3); add(pps31, sizeof(pps31), 4);
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(!c.associated);
	bytes = 0; add(slice, sizeof(slice), 3);
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(c.associated && c.sps_id==31 && c.sps_generation==6);
	bytes = 0; add(aud, sizeof(aud), 3); add(aud, sizeof(aud), 4); add(slice, sizeof(slice), 3);
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(!c.associated);
	for (i = 2; i <= 21; i++) {
		unsigned char unsupported[] = {(unsigned char)(0x60 | i),0x80};
		if (i == 5 || (i >= 6 && i <= 12)) continue;
		config(full, sizeof(full)); chd_h264_observe_range(&state, au, bytes, &c); CHECK(c.associated);
		bytes = 0; add(unsupported, sizeof(unsupported), 3); add(slice, sizeof(slice), 3);
		chd_h264_observe_range(&state, au, bytes, &c); CHECK(!c.associated);
		bytes = 0; add(slice, sizeof(slice), 3);
		chd_h264_observe_range(&state, au, bytes, &c); CHECK(!c.associated);
	}
	config(absent, sizeof(absent)); chd_h264_observe_range(&state, au, bytes, &c);
	CHECK(c.associated && !c.range_present && !c.full_range);
	/* Leading/trailing zero padding and an empty last NAL are distinct. */
	config(full, sizeof(full)); memmove(au + 5, au, bytes); memset(au,0,5); bytes += 5;
	memset(au + bytes,0,4); bytes += 4;
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(c.associated);
	bytes = 0; add(slice, sizeof(slice), 3); au[bytes++]=0;au[bytes++]=0;au[bytes++]=1;
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(!c.associated);
	config(full, sizeof(full)); au[0]=0x7f;
	chd_h264_observe_range(&state, au, bytes, &c); CHECK(!c.associated);
	/* Coded slices may exceed the SPS limit; scan once, read a small prefix. */
	config(full, sizeof(full)); chd_h264_observe_range(&state, au, bytes, &c);
	{
		unsigned int size = 65539;
		unsigned char *large = malloc(size);
		CHECK(large != NULL); memset(large,0x55,size);
		large[0]=0;large[1]=0;large[2]=1; memcpy(large+3,slice,sizeof(slice));
		chd_h264_observe_range(&state,large,size,&c); CHECK(c.associated && c.full_range); free(large);
	}
	chd_h264_observe_range(&state,au,CHD_H264_RANGE_MAX_AU+1,&c); CHECK(!c.associated);
	config(full,sizeof(full)); chd_h264_observe_range(&state,au,bytes,&c); CHECK(c.associated);
	bytes=0;
	for(i=0;i<CHD_H264_RANGE_MAX_NALS-1;i++) { unsigned char filler[]={12,0xff};add(filler,sizeof(filler),3); }
	add(slice,sizeof(slice),3);
	chd_h264_observe_range(&state,au,bytes,&c); CHECK(c.associated && state.pps[0].valid);
	bytes=0;
	for(i=0;i<CHD_H264_RANGE_MAX_NALS;i++) { unsigned char filler[]={12,0xff};add(filler,sizeof(filler),3); }
	add(slice,sizeof(slice),3);
	chd_h264_observe_range(&state,au,bytes,&c); CHECK(!c.associated && !state.pps[0].valid);
	state.generation=~0ULL; config(full,sizeof(full));
	chd_h264_observe_range(&state,au,bytes,&c); CHECK(!c.associated && state.exhausted && state.generation==~0ULL);
	chd_h264_observe_range(&state,au,bytes,&c); CHECK(!c.associated && state.generation==~0ULL);
	memset(&state,0,sizeof(state)); chd_h264_observe_range(&state,au,bytes,&c);
	CHECK(c.associated && c.sps_generation==1);
	printf("H264 input range association: %u checks PASS; state=%zu snapshot=%zu bytes\n",checks,sizeof(state),sizeof(c));
	return 0;
}
