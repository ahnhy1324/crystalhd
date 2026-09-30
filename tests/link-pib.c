/* SPDX-License-Identifier: GPL-2.0-or-later */
/* BCM70010 picture-info parser with buffer I/O and device logging mocked. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"

#define OFFSETOF(_s_, _m_) ((size_t)(unsigned long)&(((_s_ *)0)->_m_))
#define dev_dbg(dev, ...) ((void)(dev))
#define printk(...) ((void)0)

struct device { int unused; };
struct pci_dev { struct device dev; };
struct crystalhd_adp { struct pci_dev *pdev; };
struct crystalhd_rx_buffer;
struct crystalhd_rx_buffer_ops {
	void (*sync_for_cpu)(struct crystalhd_adp *, struct crystalhd_rx_buffer *);
	void (*sync_for_device)(struct crystalhd_adp *, struct crystalhd_rx_buffer *);
	BC_STATUS (*read)(struct crystalhd_rx_buffer *, uint32_t, void *, size_t);
	BC_STATUS (*write)(struct crystalhd_rx_buffer *, uint32_t, const void *, size_t);
	void (*release)(struct crystalhd_adp *, struct crystalhd_rx_buffer *);
};
struct scatterlist { uint8_t *base; };
struct crystalhd_rx_buffer {
	struct scatterlist *sgl;
	uint32_t dma_nents, capacity, uv_offset, uv_sg_ix, uv_sg_off;
	BC_OUTPUT_FORMAT output_format;
	const struct crystalhd_rx_buffer_ops *ops;
	void *cookie;
};
struct crystalhd_rx_dma_pkt {
	struct crystalhd_rx_buffer *buffer;
	void *cookie;
	uint32_t y_done_sz;
};
struct crystalhd_hw { struct crystalhd_adp *adp; };

static unsigned checks, groups, failures;
static unsigned reads, syncs, fail_read;
static uint32_t read_offsets[3];
static size_t read_sizes[3];

static void check(bool condition, const char *message)
{
	++checks;
	if (!condition) {
		++failures;
		fprintf(stderr, "FAIL: %s\n", message);
	}
}

static void reset_io(void)
{
	reads = syncs = fail_read = 0;
	memset(read_offsets, 0, sizeof(read_offsets));
	memset(read_sizes, 0, sizeof(read_sizes));
}

static void sync_for_cpu(struct crystalhd_adp *adp,
			 struct crystalhd_rx_buffer *buffer)
{
	check(adp && buffer && buffer->sgl && buffer->cookie,
	      "sync receives the admitted backing and cookie");
	++syncs;
}

static void unexpected_sync_for_device(struct crystalhd_adp *adp,
				       struct crystalhd_rx_buffer *buffer)
{
	(void)adp;
	(void)buffer;
	check(false, "metadata parsing never returns the buffer to DMA");
}

static BC_STATUS buffer_read(struct crystalhd_rx_buffer *buffer,
			     uint32_t offset, void *destination, size_t size)
{
	unsigned call = reads++;

	if (call < 3) {
		read_offsets[call] = offset;
		read_sizes[call] = size;
	}
	if (reads == fail_read)
		return BC_STS_IO_ERROR;
	if (offset > buffer->capacity || size > buffer->capacity - offset)
		return BC_STS_INV_ARG;
	memcpy(destination, buffer->sgl->base + offset, size);
	return BC_STS_SUCCESS;
}

static BC_STATUS unexpected_write(struct crystalhd_rx_buffer *buffer,
				  uint32_t offset, const void *source,
				  size_t size)
{
	(void)buffer;
	(void)offset;
	(void)source;
	(void)size;
	check(false, "Link metadata parsing is read-only");
	return BC_STS_IO_ERROR;
}

static void unexpected_release(struct crystalhd_adp *adp,
			       struct crystalhd_rx_buffer *buffer)
{
	(void)adp;
	(void)buffer;
	check(false, "Link metadata parsing borrows its buffer");
}

static const struct crystalhd_rx_buffer_ops buffer_ops = {
	.sync_for_cpu = sync_for_cpu,
	.sync_for_device = unexpected_sync_for_device,
	.read = buffer_read,
	.write = unexpected_write,
	.release = unexpected_release,
};

static void crystalhd_rx_buffer_sync_for_cpu(struct crystalhd_adp *adp,
					     struct crystalhd_rx_buffer *buffer)
{
	buffer->ops->sync_for_cpu(adp, buffer);
}

static BC_STATUS crystalhd_rx_buffer_read(struct crystalhd_rx_buffer *buffer,
					  uint32_t offset,
					  void *destination, size_t size)
{
	return buffer->ops->read(buffer, offset, destination, size);
}

#include "link-pib-functions.h"

enum { STORAGE_SIZE = 4096, TEST_WIDTH = 64, TEST_LINE = 4 };

struct fixture {
	uint8_t storage[STORAGE_SIZE];
	uint8_t original[STORAGE_SIZE];
	struct pci_dev pci;
	struct crystalhd_adp adp;
	struct scatterlist sgl;
	struct crystalhd_rx_buffer buffer;
	uint32_t cookie;
	struct crystalhd_rx_dma_pkt packet;
	struct crystalhd_hw hw;
	uint32_t row_offset;
	uint32_t pib_offset;
	uint32_t completed_bytes;
};

static unsigned stride_for(BC_OUTPUT_FORMAT format)
{
	return format == MODE420 ? 1 : 2;
}

static unsigned lane_for(BC_OUTPUT_FORMAT format)
{
	return format == MODE422_UYVY ? 1 : 0;
}

static void put_lane(struct fixture *f, uint32_t offset, const void *source,
		     size_t size)
{
	const uint8_t *bytes = source;
	unsigned stride = stride_for(f->buffer.output_format);
	unsigned lane = lane_for(f->buffer.output_format);

	for (size_t i = 0; i < size; ++i)
		f->storage[offset + i * stride + lane] = bytes[i];
}

static void put_be32_lane(struct fixture *f, uint32_t offset, uint32_t value)
{
	uint8_t bytes[4] = {
		(uint8_t)(value >> 24), (uint8_t)(value >> 16),
		(uint8_t)(value >> 8), (uint8_t)value,
	};

	put_lane(f, offset, bytes, sizeof(bytes));
}

static void prepare(struct fixture *f, BC_OUTPUT_FORMAT format,
		    uint32_t picture, uint32_t metadata, uint32_t pib_height)
{
	BC_PIC_INFO_BLOCK pib = { 0 };
	unsigned stride = stride_for(format);

	memset(f, 0, sizeof(*f));
	memset(f->storage, 0xa5, sizeof(f->storage));
	f->adp.pdev = &f->pci;
	f->sgl.base = f->storage;
	f->cookie = 0xc001c0de;
	f->buffer = (struct crystalhd_rx_buffer) {
		.sgl = &f->sgl,
		.dma_nents = 1,
		.capacity = sizeof(f->storage),
		.output_format = format,
		.ops = &buffer_ops,
		.cookie = &f->cookie,
	};
	f->packet.buffer = &f->buffer;
	f->packet.cookie = &f->cookie;
	f->hw.adp = &f->adp;
	f->row_offset = TEST_LINE * TEST_WIDTH * stride;
	f->pib_offset = f->row_offset + 4 * stride;
	f->completed_bytes = f->pib_offset + sizeof(pib) * stride;
	f->packet.y_done_sz = f->completed_bytes / 4;
	if (format == MODE420)
		f->buffer.uv_offset = f->completed_bytes;

	put_be32_lane(f, 0, TEST_LINE);
	put_be32_lane(f, f->row_offset, picture);
	pib.height = BC_SWAP32(pib_height);
	pib.picture_meta_payload = BC_SWAP32(metadata);
	put_lane(f, f->pib_offset, &pib, sizeof(pib));
	memcpy(f->original, f->storage, sizeof(f->storage));
	reset_io();
}

static bool parse(struct fixture *f, uint32_t pic_height, uint32_t pic_width,
		  uint32_t *picture, uint64_t *metadata)
{
	return link_GetPictureInfo(&f->hw, pic_height, pic_width, &f->packet,
				   picture, metadata);
}

static void successful_formats(void)
{
	const BC_OUTPUT_FORMAT formats[] = {
		MODE420, MODE422_YUY2, MODE422_UYVY,
	};

	for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
		struct fixture f;
		uint32_t picture = 0;
		uint64_t metadata = 0;
		unsigned stride = stride_for(formats[i]);

		++groups;
		prepare(&f, formats[i], 0x10203040 + i, 0x50607080 + i,
			TEST_LINE * 2);
		check(parse(&f, TEST_LINE * 2, TEST_WIDTH, &picture, &metadata),
		      "all advertised output formats parse successfully");
		check(picture == 0x10203040 + i && metadata == 0x50607080 + i,
		      "picture number, metadata and half-height PIB match exactly");
		check(syncs == 1 && reads == 3,
		      "successful parsing synchronizes once and performs three reads");
		check(read_offsets[0] == 0 && read_sizes[0] == 8 &&
		      read_offsets[1] == f.pib_offset &&
		      read_sizes[1] == sizeof(BC_PIC_INFO_BLOCK) * stride &&
		      read_offsets[2] == f.row_offset && read_sizes[2] == 12,
		      "parser reads the exact header, PIB and picture-number extents");
		check(!memcmp(f.storage, f.original, sizeof(f.storage)) &&
		      f.packet.buffer == &f.buffer && f.packet.cookie == &f.cookie,
		      "successful parsing preserves backing bytes and both identities");

		reset_io();
		check(link_GetRptDropParam(&f.hw, TEST_LINE * 2, TEST_WIDTH,
					   &f.packet) == 0x10203040 + i,
		      "public repeat/drop wrapper returns the parsed picture number");
	}
}

static void read_failures(void)
{
	for (unsigned failed_call = 1; failed_call <= 3; ++failed_call) {
		struct fixture f;
		uint32_t picture = UINT32_MAX;
		uint64_t metadata = UINT64_MAX;

		++groups;
		prepare(&f, MODE422_UYVY, 17, 29, TEST_LINE);
		fail_read = failed_call;
		check(!parse(&f, TEST_LINE, TEST_WIDTH, &picture, &metadata),
		      "each buffer read failure rejects the picture");
		check(picture == 0 && metadata == 0 && reads == failed_call &&
		      syncs == 1,
		      "read failure clears outputs and stops at the failed access");
		check(!memcmp(f.storage, f.original, sizeof(f.storage)),
		      "read failure never changes capture storage");
	}
}

static void completed_extent_cases(void)
{
	struct fixture f;
	uint32_t picture;
	uint64_t metadata;
	const BC_OUTPUT_FORMAT formats[] = {
		MODE420, MODE422_YUY2, MODE422_UYVY,
	};

	for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
		++groups;
		prepare(&f, formats[i], 3 + i, 5 + i, TEST_LINE);
		f.packet.y_done_sz--;
		picture = UINT32_MAX;
		metadata = UINT64_MAX;
		check(parse(&f, TEST_LINE, TEST_WIDTH, &picture, &metadata) &&
		      picture == 3 + i && metadata == 5 + i && reads == 3 &&
		      syncs == 1,
		      "PIB tail past completion parses within the registered Y plane");
		check(!memcmp(f.storage, f.original, sizeof(f.storage)),
		      "PIB tail parsing preserves capture storage");
	}

	++groups;
	prepare(&f, MODE420, 3, 5, TEST_LINE);
	f.packet.y_done_sz = f.pib_offset / 4;
	picture = UINT32_MAX;
	metadata = UINT64_MAX;
	check(!parse(&f, TEST_LINE, TEST_WIDTH, &picture, &metadata) &&
	      picture == 0 && metadata == 0 && reads == 1 && syncs == 1,
	      "PIB starting at the completion point is rejected before its read");

	for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
		unsigned stride = stride_for(formats[i]);

		++groups;
		prepare(&f, formats[i], 3, 5, TEST_LINE);
		f.packet.y_done_sz = (f.pib_offset +
			stride * OFFSETOF(BC_PIC_INFO_BLOCK, other) - 4) / 4;
		picture = UINT32_MAX;
		metadata = UINT64_MAX;
		check(!parse(&f, TEST_LINE, TEST_WIDTH, &picture, &metadata) &&
		      picture == 0 && metadata == 0 && reads == 1 && syncs == 1,
		      "truncated common PIB fields are rejected before their read");
		check(!memcmp(f.storage, f.original, sizeof(f.storage)),
		      "common-prefix rejection preserves capture storage");
	}

	++groups;
	prepare(&f, MODE420, 3, 5, TEST_LINE);
	f.packet.y_done_sz--;
	f.buffer.uv_offset = f.completed_bytes - 4;
	picture = UINT32_MAX;
	metadata = UINT64_MAX;
	check(!parse(&f, TEST_LINE, TEST_WIDTH, &picture, &metadata) &&
	      picture == 0 && metadata == 0 && reads == 1 && syncs == 1,
	      "PIB crossing the planar Y boundary is rejected before its read");

	++groups;
	prepare(&f, MODE422_YUY2, 3, 5, TEST_LINE);
	f.buffer.capacity = UINT32_MAX;
	f.packet.y_done_sz = UINT32_MAX;
	picture = UINT32_MAX;
	metadata = UINT64_MAX;
	check(!parse(&f, TEST_LINE, TEST_WIDTH, &picture, &metadata) &&
	      picture == 0 && metadata == 0 && !reads && !syncs,
	      "overflowing completed-DWORD conversion is rejected before access");

	++groups;
	prepare(&f, MODE422_YUY2, 3, 5, TEST_LINE);
	f.buffer.capacity = UINT32_MAX;
	f.packet.y_done_sz = UINT32_MAX / 4;
	picture = UINT32_MAX;
	metadata = UINT64_MAX;
	check(!parse(&f, TEST_LINE, UINT32_MAX, &picture, &metadata) &&
	      picture == 0 && metadata == 0 && reads == 1 && syncs == 1,
	      "64-bit row-offset arithmetic rejects a wrapped-width layout");

	++groups;
	prepare(&f, MODE420, 3, 5, TEST_LINE);
	f.buffer.uv_offset = f.completed_bytes - 4;
	picture = UINT32_MAX;
	metadata = UINT64_MAX;
	check(!parse(&f, TEST_LINE, TEST_WIDTH, &picture, &metadata) &&
	      picture == 0 && metadata == 0 && !reads && !syncs,
	      "planar Y capacity bounds the firmware-reported completion");
}

static void invalid_inputs(void)
{
	struct fixture f;
	uint32_t picture;
	uint64_t metadata;

	for (unsigned which = 0; which < 6; ++which) {
		++groups;
		prepare(&f, MODE420, 7, 11, TEST_LINE);
		switch (which) {
		case 0:
			f.buffer.output_format = OUTPUT_MODE_INVALID;
			break;
		case 1:
			f.buffer.uv_offset = f.buffer.capacity + 1;
			break;
		case 2:
			f.packet.y_done_sz = 1;
			break;
		case 3:
			f.packet.buffer = NULL;
			break;
		case 4:
			put_be32_lane(&f, 0, 1093);
			memcpy(f.original, f.storage, sizeof(f.storage));
			break;
		default:
			break;
		}
		picture = UINT32_MAX;
		metadata = UINT64_MAX;
		check(!parse(&f, TEST_LINE, which == 5 ? 0 : TEST_WIDTH,
			     &picture, &metadata) && picture == 0 && metadata == 0,
		      "invalid format, extent, packet, line or width is rejected");
		check(!memcmp(f.storage, f.original, sizeof(f.storage)),
		      "invalid input preserves capture bytes");
	}

	++groups;
	prepare(&f, MODE422_YUY2, 7, 11, TEST_LINE + 1);
	picture = UINT32_MAX;
	metadata = UINT64_MAX;
	check(!parse(&f, TEST_LINE, TEST_WIDTH, &picture, &metadata) &&
	      picture == 0 && metadata == 0 && reads == 2,
	      "PIB height mismatch fails after the bounded PIB read");
}

int main(void)
{
	successful_formats();
	read_failures();
	completed_extent_cases();
	invalid_inputs();
	printf("Link PIB: %u groups, %u checks, %u failures\n",
	       groups, checks, failures);
	return failures ? 1 : 0;
}
