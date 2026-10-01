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
struct scatterlist {
	dma_addr_t dma_address;
	uint32_t dma_length;
	struct scatterlist *next;
};
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "This harness exercises the CrystalHD little-endian descriptor format"
#endif
#define __LITTLE_ENDIAN_BITFIELD
#define dev_err(dev, ...) ((void)(dev))
#define sg_next(sg) ((sg)->next)
#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg) ((sg)->dma_length)
#define cpu_to_le32(value) ((uint32_t)(value))
#define cpu_to_le64(value) ((uint64_t)(value))
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

enum {
	RX_SEGMENT_SIZE = 4096,
	RX_CAPACITY = 3 * RX_SEGMENT_SIZE,
};

static void init_unmerged_sg(struct scatterlist sg[3])
{
	/* The DMA chain order deliberately differs from the backing array order. */
	sg[0] = (struct scatterlist) {
		.dma_address = 0x100000,
		.dma_length = RX_SEGMENT_SIZE,
		.next = &sg[2],
	};
	sg[1] = (struct scatterlist) {
		.dma_address = 0x300000,
		.dma_length = RX_SEGMENT_SIZE,
		.next = NULL,
	};
	sg[2] = (struct scatterlist) {
		.dma_address = 0x200000,
		.dma_length = RX_SEGMENT_SIZE,
		.next = &sg[1],
	};
}

static uint64_t desc_buffer_address(const struct dma_descriptor *desc)
{
	return ((uint64_t)desc->buff_addr_high << 32) | desc->buff_addr_low;
}

static void rx_descriptor_parity(bool merged, uint32_t uv_offset)
{
	struct scatterlist sg[3];
	struct dma_descriptor generic_desc[5], common_desc[5];
	struct dma_desc_mem generic_mem = {
		.pdma_desc_start = generic_desc,
		.phy_addr = 0x400000,
		.sz = sizeof(generic_desc),
	};
	struct dma_desc_mem common_mem = {
		.pdma_desc_start = common_desc,
		.phy_addr = 0x400000,
		.sz = sizeof(common_desc),
	};
	struct crystalhd_dma_desc_source source;
	struct crystalhd_rx_buffer buffer;
	uint32_t generic_uv_index = 99, common_uv_index = 99;
	uint32_t y_bytes = 0, uv_bytes = 0;
	uint32_t expected_index = merged ? 1 : (uv_offset + 4095) / 4096;
	uint32_t uv_sg_ix = merged ? 0 : uv_offset / RX_SEGMENT_SIZE;
	uint32_t uv_sg_off = merged ? uv_offset : uv_offset % RX_SEGMENT_SIZE;
	unsigned int total = (merged ? 1 : 3) + !!uv_sg_off;
	unsigned int i;

	if (merged) {
		sg[0] = (struct scatterlist) {
			.dma_address = 0x100000,
			.dma_length = RX_CAPACITY,
			.next = NULL,
		};
	} else {
		init_unmerged_sg(sg);
	}
	memset(&buffer, 0, sizeof(buffer));
	buffer.sgl = sg;
	buffer.dma_nents = merged ? 1 : 3;
	buffer.capacity = RX_CAPACITY;
	buffer.uv_offset = uv_offset;
	buffer.uv_sg_ix = uv_sg_ix;
	buffer.uv_sg_off = uv_sg_off;

	source = (struct crystalhd_dma_desc_source) {
		.sgl = sg,
		.dma_nents = buffer.dma_nents,
		.dir_tx = false,
	};

	memset(generic_desc, 0xa5, sizeof(generic_desc));
	memset(common_desc, 0xa5, sizeof(common_desc));
	assert(crystalhd_xlat_rx_buffer_to_dma_desc(&buffer, &generic_mem,
						       &generic_uv_index, NULL)
	       == BC_STS_SUCCESS);
	assert(crystalhd_xlat_dma_to_desc(&source, RX_CAPACITY, uv_offset,
					 uv_sg_ix, uv_sg_off, &common_mem,
					 &common_uv_index, NULL, 0)
	       == BC_STS_SUCCESS);
	assert(generic_uv_index == expected_index);
	assert(common_uv_index == generic_uv_index);
	assert(!memcmp(generic_desc, common_desc, sizeof(generic_desc)));

	for (i = 0; i < generic_uv_index; i++)
		y_bytes += generic_desc[i].xfer_size * 4;
	for (; i < total; i++)
		uv_bytes += generic_desc[i].xfer_size * 4;
	assert(y_bytes == uv_offset);
	assert(uv_bytes == RX_CAPACITY - uv_offset);
	assert(generic_desc[generic_uv_index - 1].last_rec_indicator);
	assert(generic_desc[total - 1].last_rec_indicator);
	assert(desc_buffer_address(&generic_desc[generic_uv_index]) ==
	       (merged ? 0x100000 :
		(uv_sg_ix == 1 ? 0x200000 : 0x300000)) + uv_sg_off);

	if (!merged) {
		/* Array indexing would incorrectly visit sg[1] (0x300000) here. */
		assert(desc_buffer_address(&generic_desc[1]) == 0x200000);
		assert(desc_buffer_address(&generic_desc[total - 1]) == 0x300000);
	}
}

static void insufficient_ring_untouched(bool merged)
{
	struct scatterlist sg[3];
	struct dma_descriptor desc[5], before[5];
	struct dma_desc_mem mem = {
		.pdma_desc_start = desc,
		.phy_addr = 0x400000,
	};
	struct crystalhd_rx_buffer buffer = {0};
	uint32_t uv_index = 99;
	unsigned int required = merged ? 2 : 4;

	if (merged) {
		sg[0] = (struct scatterlist) {
			.dma_address = 0x100000,
			.dma_length = RX_CAPACITY,
		};
	} else {
		init_unmerged_sg(sg);
	}
	buffer.sgl = sg;
	buffer.dma_nents = merged ? 1 : 3;
	buffer.capacity = RX_CAPACITY;
	buffer.uv_offset = 6144;
	buffer.uv_sg_ix = merged ? 0 : 1;
	buffer.uv_sg_off = merged ? 6144 : 2048;
	mem.sz = (required - 1) * sizeof(*desc);
	memset(desc, 0xa5, sizeof(desc));
	memcpy(before, desc, sizeof(before));

	assert(crystalhd_xlat_rx_buffer_to_dma_desc(&buffer, &mem, &uv_index,
						       NULL)
	       == BC_STS_INSUFF_RES);
	assert(!memcmp(desc, before, sizeof(desc)));
}

static void assert_invalid_rx_untouched(struct crystalhd_rx_buffer *buffer)
{
	struct dma_descriptor desc[5], before[5];
	struct dma_desc_mem mem = {
		.pdma_desc_start = desc,
		.phy_addr = 0x400000,
		.sz = sizeof(desc),
	};
	uint32_t uv_index = 99;

	memset(desc, 0xa5, sizeof(desc));
	memcpy(before, desc, sizeof(before));
	assert(crystalhd_xlat_rx_buffer_to_dma_desc(buffer, &mem, &uv_index,
						       NULL)
	       == BC_STS_INV_ARG);
	assert(!memcmp(desc, before, sizeof(desc)));
}

static void invalid_rx_bounds(void)
{
	struct scatterlist sg[3];
	struct crystalhd_rx_buffer buffer = {0};

	init_unmerged_sg(sg);
	buffer.sgl = sg;
	buffer.dma_nents = 3;
	buffer.capacity = RX_CAPACITY;
	buffer.uv_offset = 6144;
	buffer.uv_sg_ix = 1;
	buffer.uv_sg_off = 2048;
	buffer.uv_sg_off = 1024;
	assert_invalid_rx_untouched(&buffer);

	buffer.uv_sg_off = 2050;
	buffer.uv_offset = 6146;
	assert_invalid_rx_untouched(&buffer);

	buffer.uv_sg_off = 2048;
	buffer.uv_offset = 6144;
	buffer.capacity = RX_CAPACITY - 2;
	assert_invalid_rx_untouched(&buffer);

	buffer.capacity = RX_CAPACITY;
	buffer.uv_offset = buffer.capacity;
	assert_invalid_rx_untouched(&buffer);

	buffer.uv_offset = 6144;
	buffer.uv_sg_ix = buffer.dma_nents;
	assert_invalid_rx_untouched(&buffer);

	buffer.uv_offset = RX_SEGMENT_SIZE;
	buffer.uv_sg_ix = 0;
	buffer.uv_sg_off = RX_SEGMENT_SIZE;
	assert_invalid_rx_untouched(&buffer);

	/* The advertised DMA-entry count must not run past the linked chain. */
	buffer.uv_offset = 0;
	buffer.uv_sg_ix = 0;
	buffer.uv_sg_off = 0;
	buffer.capacity = 2 * RX_SEGMENT_SIZE;
	sg[2].next = NULL;
	assert_invalid_rx_untouched(&buffer);

	/* A complete chain still cannot advertise more capacity than it maps. */
	buffer.dma_nents = 2;
	buffer.capacity = RX_CAPACITY;
	assert_invalid_rx_untouched(&buffer);
}

static void unaligned_rx_sg_untouched(void)
{
	struct {
		uint32_t leading_canary[4];
		struct dma_descriptor desc[5];
		uint32_t trailing_canary[4];
	} ring, before;
	struct scatterlist sg[3];
	struct dma_desc_mem mem = {
		.pdma_desc_start = ring.desc,
		.phy_addr = 0x400000,
		.sz = sizeof(ring.desc),
	};
	struct crystalhd_rx_buffer buffer = {0};
	uint32_t uv_index;
	unsigned int invalid;

	for (invalid = 0; invalid < 2; invalid++) {
		init_unmerged_sg(sg);
		buffer.sgl = sg;
		buffer.dma_nents = 3;
		buffer.capacity = RX_CAPACITY;
		buffer.uv_offset = 6144;
		buffer.uv_sg_ix = 1;
		buffer.uv_sg_off = 2048;
		/* Corrupt the final linked entry so two valid entries are scanned
		 * before the common descriptor preflight rejects the chain. */
		if (invalid == 0)
			sg[1].dma_address += 2;
		else
			sg[1].dma_length -= 2;
		memset(&ring, 0xa5, sizeof(ring));
		memcpy(&before, &ring, sizeof(before));
		uv_index = 99;

		assert(crystalhd_xlat_rx_buffer_to_dma_desc(&buffer, &mem,
						       &uv_index, NULL)
		       == BC_STS_NOT_IMPL);
		assert(uv_index == 99);
		assert(!memcmp(&ring, &before, sizeof(ring)));
	}
}

static void oversized_rx_backing(void)
{
	struct scatterlist sg[3];
	struct dma_descriptor desc[5], before[5];
	struct dma_desc_mem mem = {
		.pdma_desc_start = desc,
		.phy_addr = 0x400000,
		.sz = sizeof(desc),
	};
	struct crystalhd_rx_buffer buffer = {0};
	uint32_t uv_index = 99;

	init_unmerged_sg(sg);
	buffer.sgl = sg;
	buffer.dma_nents = 3;
	buffer.capacity = 6144;
	buffer.uv_offset = 4096;
	buffer.uv_sg_ix = 1;
	buffer.uv_sg_off = 0;
	memset(desc, 0xa5, sizeof(desc));
	memcpy(before, desc, sizeof(before));

	assert(crystalhd_xlat_rx_buffer_to_dma_desc(&buffer, &mem,
						       &uv_index, NULL)
	       == BC_STS_SUCCESS);
	assert(uv_index == 1);
	assert(desc[0].xfer_size == 1024 && desc[0].last_rec_indicator);
	assert(desc[1].xfer_size == 512 && desc[1].last_rec_indicator);
	assert(desc_buffer_address(&desc[1]) == 0x200000);
	assert(!memcmp(&desc[2], &before[2], 3 * sizeof(desc[0])));
}

static void input_tail(unsigned int aligned_bytes, unsigned int tail)
{
	const uint32_t dest_dram = 0x123400;
	struct scatterlist sg = {
		.dma_address = 0x100000,
		.dma_length = aligned_bytes,
	};
	struct dma_descriptor desc[2], expected[2];
	struct dma_desc_mem mem = { .pdma_desc_start = desc,
		.phy_addr = 0x400000, .sz = sizeof(desc) };
	struct crystalhd_tx_buffer buffer = {
		.sgl = &sg,
		.dma_nents = !!aligned_bytes,
		.bytes = aligned_bytes + tail,
		.tail_addr = 0x500000,
		.tail_size = tail,
		.cookie = &buffer,
	};
	uint32_t uv_index = 99;
	unsigned int tail_index = !!aligned_bytes;

	memset(desc, 0xa5, sizeof(desc));
	memset(expected, 0, sizeof(expected));
	if (aligned_bytes) {
		expected[0].buff_addr_low = sg.dma_address;
		expected[0].xfer_size = aligned_bytes / 4;
		expected[0].dma_dir = 1;
		expected[0].sdram_buff_addr = dest_dram;
		expected[0].next_desc_addr_low =
			mem.phy_addr + sizeof(struct dma_descriptor);
	}
	expected[tail_index].buff_addr_low = buffer.tail_addr;
	expected[tail_index].xfer_size = 1;
	expected[tail_index].fill_bytes = 4 - tail;
	expected[tail_index].dma_dir = 1;
	expected[tail_index].sdram_buff_addr = dest_dram + aligned_bytes;
	expected[tail_index].last_rec_indicator = 1;
	expected[tail_index].intr_enable = 1;

	assert(crystalhd_xlat_tx_buffer_to_dma_desc(&buffer, &mem, &uv_index,
						   NULL, dest_dram)
	       == BC_STS_SUCCESS);
	assert(uv_index == 0);
	assert(!memcmp(desc, expected,
		       (tail_index + 1) * sizeof(struct dma_descriptor)));
}

static void tx_short_mapped_backing(void)
{
	struct scatterlist sg[3];
	struct dma_descriptor desc[4], before[4];
	struct dma_desc_mem mem = {
		.pdma_desc_start = desc,
		.phy_addr = 0x400000,
		.sz = sizeof(desc),
	};
	struct crystalhd_tx_buffer buffer = {
		.sgl = sg,
		.dma_nents = 3,
		.bytes = 6145,
		.tail_addr = 0x500000,
		.tail_size = 1,
		.cookie = &buffer,
	};
	uint32_t uv_index = 99;

	init_unmerged_sg(sg);
	memset(desc, 0xa5, sizeof(desc));
	memcpy(before, desc, sizeof(before));
	assert(crystalhd_xlat_tx_buffer_to_dma_desc(&buffer, &mem, &uv_index,
						   NULL, 0x120000)
	       == BC_STS_SUCCESS);
	assert(uv_index == 0);
	assert(desc_buffer_address(&desc[0]) == 0x100000 &&
	       desc[0].xfer_size == 1024 && !desc[0].last_rec_indicator);
	assert(desc_buffer_address(&desc[1]) == 0x200000 &&
	       desc[1].xfer_size == 512 && !desc[1].last_rec_indicator);
	assert(desc_buffer_address(&desc[2]) == buffer.tail_addr &&
	       desc[2].xfer_size == 1 && desc[2].fill_bytes == 3 &&
	       desc[2].last_rec_indicator && desc[2].intr_enable);
	assert(!memcmp(&desc[3], &before[3], sizeof(desc[3])));
}

static void assert_tx_rejected(const struct crystalhd_tx_buffer *buffer,
			       size_t descriptor_bytes, BC_STATUS expected)
{
	struct dma_descriptor desc[4], before[4];
	struct dma_desc_mem mem = {
		.pdma_desc_start = desc,
		.phy_addr = 0x400000,
		.sz = descriptor_bytes,
	};
	uint32_t uv_index = 99;

	memset(desc, 0xa5, sizeof(desc));
	memcpy(before, desc, sizeof(before));
	assert(crystalhd_xlat_tx_buffer_to_dma_desc(buffer, &mem, &uv_index,
						   NULL, 0) == expected);
	assert(uv_index == 99);
	assert(!memcmp(desc, before, sizeof(desc)));
}

static void tx_descriptor_width(void)
{
	struct scatterlist sg = {
		.dma_address = 0x100000,
		.dma_length = 0x02000000,
	};
	struct dma_descriptor desc = {0};
	struct dma_desc_mem mem = {
		.pdma_desc_start = &desc,
		.phy_addr = 0x400000,
		.sz = sizeof(desc),
	};
	struct crystalhd_tx_buffer buffer = {
		.sgl = &sg,
		.dma_nents = 1,
		.bytes = 0x01fffffc,
		.cookie = &buffer,
	};
	uint32_t uv_index = 99;

	/* bytes may stop within a larger mapped segment at the hardware limit. */
	assert(crystalhd_xlat_tx_buffer_to_dma_desc(&buffer, &mem, &uv_index,
						   NULL, 0) == BC_STS_SUCCESS);
	assert(desc.xfer_size == 0x7fffff && desc.last_rec_indicator &&
	       desc.intr_enable);

	buffer.bytes = 0x02000000;
	assert_tx_rejected(&buffer, sizeof(desc), BC_STS_NOT_IMPL);
}

static void invalid_tx_bounds(void)
{
	struct scatterlist sg[2] = {
		{ .dma_address = 0x100000, .dma_length = 4096 },
		{ .dma_address = 0x200000, .dma_length = 4096 },
	};
	struct crystalhd_tx_buffer buffer = {
		.sgl = sg,
		.dma_nents = 1,
		.bytes = 4096,
		.cookie = &buffer,
	};

	sg[0].next = &sg[1];
	assert_tx_rejected(&(struct crystalhd_tx_buffer){0},
			   4 * sizeof(struct dma_descriptor), BC_STS_INV_ARG);
	buffer.cookie = NULL;
	assert_tx_rejected(&buffer, 4 * sizeof(struct dma_descriptor),
			   BC_STS_INV_ARG);
	buffer.cookie = &buffer;
	buffer.bytes = 4097;
	assert_tx_rejected(&buffer, 4 * sizeof(struct dma_descriptor),
			   BC_STS_INV_ARG);
	buffer.tail_size = 1;
	buffer.tail_addr = 0x500002;
	assert_tx_rejected(&buffer, 4 * sizeof(struct dma_descriptor),
			   BC_STS_INV_ARG);
	buffer.tail_addr = 0x500000;
	buffer.tail_size = 2;
	assert_tx_rejected(&buffer, 4 * sizeof(struct dma_descriptor),
			   BC_STS_INV_ARG);
	buffer.bytes = 8192;
	buffer.tail_size = 0;
	assert_tx_rejected(&buffer, 4 * sizeof(struct dma_descriptor),
			   BC_STS_INV_ARG);
	buffer.dma_nents = 2;
	sg[0].next = NULL;
	assert_tx_rejected(&buffer, 4 * sizeof(struct dma_descriptor),
			   BC_STS_INV_ARG);
	sg[0].next = &sg[1];
	sg[1].dma_address += 2;
	assert_tx_rejected(&buffer, 4 * sizeof(struct dma_descriptor),
			   BC_STS_NOT_IMPL);
	sg[1].dma_address -= 2;
	assert_tx_rejected(&buffer, sizeof(struct dma_descriptor),
			   BC_STS_INSUFF_RES);
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
	rx_descriptor_parity(false, 4096);
	rx_descriptor_parity(false, 8192);
	rx_descriptor_parity(false, 6144);
	rx_descriptor_parity(true, 4096);
	rx_descriptor_parity(true, 8192);
	rx_descriptor_parity(true, 6144);
	insufficient_ring_untouched(false);
	insufficient_ring_untouched(true);
	invalid_rx_bounds();
	unaligned_rx_sg_untouched();
	oversized_rx_backing();
	for (tail = 1; tail <= 3; tail++) {
		input_tail(0, tail);
		input_tail(4096, tail);
	}
	tx_short_mapped_backing();
	tx_descriptor_width();
	invalid_tx_bounds();
	rx_descriptor_allocation_failure();
	puts("DMA descriptor/setup tests passed (ASan/UBSan)");
	return 0;
}
