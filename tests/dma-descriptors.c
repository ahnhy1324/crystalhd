// SPDX-License-Identifier: GPL-2.0-or-later
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t dma_addr_t;
typedef unsigned int BC_OUTPUT_FORMAT;
typedef enum {
	BC_STS_SUCCESS, BC_STS_INV_ARG, BC_STS_NOT_IMPL,
	BC_STS_ERROR, BC_STS_INSUFF_RES
} BC_STATUS;
typedef union {
	uint64_t full_addr;
	struct { uint32_t low_part, high_part; };
} addr_64;
struct device { int unused; };
struct scatterlist { dma_addr_t address; uint32_t length; };
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "This harness exercises the CrystalHD little-endian descriptor format"
#endif
#define __LITTLE_ENDIAN_BITFIELD
#define dev_err(dev, ...) ((void)(dev))
#define crystalhd_get_sgle_paddr(dio, ix) ((dio)->sg[ix].address)
#define crystalhd_get_sgle_len(dio, ix) ((dio)->sg[ix].length)
#include "dma-types.h"
#include "dma-builders.h"

static void capture_plane_split(bool merged, uint32_t uv_offset)
{
	struct scatterlist sg[] = {
		{ 0x100000, merged ? 12288 : 4096 },
		{ 0x200000, 4096 }, { 0x300000, 4096 },
	};
	struct dma_descriptor desc[5];
	struct dma_desc_mem mem = { .pdma_desc_start = desc,
		.phy_addr = 0x400000, .sz = sizeof(desc) };
	struct crystalhd_dio_req req = { .sg = sg, .sg_cnt = merged ? 1 : 3 };
	uint32_t uv_index = 99, y_bytes = 0, uv_bytes = 0;
	uint32_t expected_index = merged ? 1 : (uv_offset + 4095) / 4096;
	unsigned int total = req.sg_cnt + (merged || uv_offset % 4096 != 0);
	unsigned int i;

	req.uinfo.xfr_len = 12288;
	req.uinfo.uv_offset = uv_offset;
	req.uinfo.uv_sg_ix = merged ? 0 : uv_offset / 4096;
	req.uinfo.uv_sg_off = merged ? uv_offset : uv_offset % 4096;
	assert(crystalhd_xlat_sgl_to_dma_desc(&req, &mem, &uv_index, NULL, 0)
	       == BC_STS_SUCCESS);
	assert(uv_index == expected_index);
	for (i = 0; i < uv_index; i++)
		y_bytes += desc[i].xfer_size * 4;
	for (; i < total; i++)
		uv_bytes += desc[i].xfer_size * 4;
	assert(y_bytes == uv_offset);
	assert(uv_bytes == 12288 - uv_offset);
	assert(desc[uv_index - 1].last_rec_indicator);
	assert(desc[total - 1].last_rec_indicator);
	assert(desc[uv_index].buff_addr_low ==
	       sg[req.uinfo.uv_sg_ix].address + req.uinfo.uv_sg_off);

	/* No write past a descriptor ring which cannot fit the extra split. */
	mem.sz = (total - 1) * sizeof(*desc);
	memset(desc, 0xa5, sizeof(desc));
	assert(crystalhd_xlat_sgl_to_dma_desc(&req, &mem, &uv_index, NULL, 0)
	       == BC_STS_INSUFF_RES);
	for (i = 0; i < sizeof(desc); i++)
		assert(((unsigned char *)desc)[i] == 0xa5);
}

static void input_tail(unsigned int aligned_bytes, unsigned int tail)
{
	struct scatterlist sg = { 0x100000, aligned_bytes };
	struct dma_descriptor desc[2];
	struct dma_desc_mem mem = { .pdma_desc_start = desc,
		.phy_addr = 0x400000, .sz = sizeof(desc) };
	struct crystalhd_dio_req req = { .sg = &sg, .sg_cnt = !!aligned_bytes,
		.fb_size = tail, .fb_pa = 0x500000 };
	uint32_t uv_index = 99;

	req.uinfo.dir_tx = true;
	req.uinfo.xfr_len = aligned_bytes + tail;
	assert(crystalhd_xlat_sgl_to_dma_desc(&req, &mem, &uv_index, NULL, 0)
	       == BC_STS_SUCCESS);
	assert(uv_index == 0);
	if (aligned_bytes)
		assert(desc[0].xfer_size * 4 == aligned_bytes);
	assert(desc[!!aligned_bytes].buff_addr_low == req.fb_pa);
	assert(desc[!!aligned_bytes].xfer_size == 1);
	assert(desc[!!aligned_bytes].fill_bytes == 4 - tail);
	assert(desc[!!aligned_bytes].last_rec_indicator);
}

int main(void)
{
	unsigned int tail;

	_Static_assert(sizeof(struct dma_descriptor) == 32, "descriptor size");
	capture_plane_split(false, 4096);
	capture_plane_split(false, 8192);
	capture_plane_split(false, 6144);
	capture_plane_split(true, 4096);
	capture_plane_split(true, 8192);
	for (tail = 1; tail <= 3; tail++) {
		input_tail(0, tail);
		input_tail(4096, tail);
	}
	puts("DMA descriptor tests passed (ASan/UBSan)");
	return 0;
}
