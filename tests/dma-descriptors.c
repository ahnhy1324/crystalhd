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

#define GFP_KERNEL 0
#define KERN_ERR ""
#define printk(...) ((void)0)

struct pci_dev { struct device dev; };
struct crystalhd_adp { struct pci_dev *pdev; };
struct crystalhd_dioq { int unused; };
struct tx_dma_pkt {
	struct dma_desc_mem desc_mem;
	uint32_t list_tag;
};
struct crystalhd_rx_dma_pkt {
	struct dma_desc_mem desc_mem;
	uint32_t pkt_tag;
	struct crystalhd_rx_dma_pkt *next;
};
struct crystalhd_hw {
	struct tx_dma_pkt tx_pkt_pool[BC_TX_LIST_CNT];
	struct crystalhd_rx_dma_pkt *rx_pkt_pool_head;
	uint32_t rx_pkt_tag_seed;
	struct crystalhd_adp *adp;
	struct crystalhd_dioq *tx_freeq;
};

static struct crystalhd_rx_dma_pkt rx_packet;
static struct dma_descriptor tx_dma[BC_TX_LIST_CNT][BC_LINK_MAX_SGLS];
static unsigned int dma_allocs, packet_allocs, packet_frees, ring_teardowns;
static bool packet_live;

static BC_STATUS crystalhd_hw_create_ioqs(struct crystalhd_hw *hw)
{
	static struct crystalhd_dioq tx_freeq;

	assert(hw && !hw->tx_freeq);
	hw->tx_freeq = &tx_freeq;
	return BC_STS_SUCCESS;
}

static void *bc_kern_dma_alloc(struct crystalhd_adp *adp, uint32_t size,
			       dma_addr_t *physical)
{
	assert(adp && size == sizeof(tx_dma[0]) && physical);
	*physical = 0x100000 + dma_allocs * sizeof(tx_dma[0]);
	if (dma_allocs < BC_TX_LIST_CNT)
		return tx_dma[dma_allocs++];
	dma_allocs++;
	assert(packet_live);
	return NULL;
}

static BC_STATUS crystalhd_dioq_add(struct crystalhd_dioq *queue, void *data,
				    bool wake, uint32_t tag)
{
	(void)data;
	assert(queue && !wake && !tag);
	return BC_STS_SUCCESS;
}

static void *kzalloc(size_t size, int flags)
{
	assert(size == sizeof(rx_packet) && flags == GFP_KERNEL && !packet_live);
	memset(&rx_packet, 0, sizeof(rx_packet));
	packet_allocs++;
	packet_live = true;
	return &rx_packet;
}

void kfree(void *memory)
{
	assert(memory == &rx_packet && packet_live);
	packet_live = false;
	packet_frees++;
}

static BC_STATUS crystalhd_hw_free_dma_rings(struct crystalhd_hw *hw)
{
	unsigned int i;

	assert(hw && !packet_live);
	for (i = 0; i < BC_TX_LIST_CNT; i++) {
		assert(hw->tx_pkt_pool[i].desc_mem.pdma_desc_start == tx_dma[i]);
		hw->tx_pkt_pool[i].desc_mem.pdma_desc_start = NULL;
	}
	ring_teardowns++;
	return BC_STS_SUCCESS;
}

static void crystalhd_hw_free_rx_pkt(struct crystalhd_hw *hw,
				     struct crystalhd_rx_dma_pkt *packet)
{
	(void)hw;
	(void)packet;
	assert(!"failed RX descriptor allocation must not queue its packet");
}

#include "dma-setup.h"

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

static void rx_descriptor_allocation_failure(void)
{
	struct pci_dev pci = {0};
	struct crystalhd_adp adp = { .pdev = &pci };
	struct crystalhd_hw hw = { .adp = &adp, .rx_pkt_tag_seed = 0x70029070 };

	dma_allocs = packet_allocs = packet_frees = ring_teardowns = 0;
	packet_live = false;
	assert(crystalhd_hw_setup_dma_rings(&hw) == BC_STS_INSUFF_RES);
	assert(dma_allocs == BC_TX_LIST_CNT + 1);
	assert(packet_allocs == 1 && packet_frees == 1 && !packet_live);
	assert(ring_teardowns == 1);
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
	rx_descriptor_allocation_failure();
	puts("DMA descriptor/setup tests passed (ASan/UBSan)");
	return 0;
}
