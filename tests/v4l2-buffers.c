// SPDX-License-Identifier: GPL-2.0-or-later
/* Source-extracted adapter and real pixel finishing with deterministic DMA/
 * vb2 boundary mocks. This does not emulate PCI ordering or physical DMA.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#define KERNEL_VERSION(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#if TEST_VB2_OLD
#define LINUX_VERSION_CODE KERNEL_VERSION(5, 15, 0)
#else
#define LINUX_VERSION_CODE KERNEL_VERSION(5, 16, 0)
#endif
#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#include "crystalhd_fw_if.h"
typedef struct C011_PIB C011_PIB;
#include "bc_dts_glob_lnx.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;
typedef int spinlock_t;
struct crystalhd_adp { int unused; };
struct crystalhd_cmd { int unused; };
struct device { int unused; };
struct scatterlist {
    dma_addr_t address;
    u32 length;
    struct scatterlist *next;
};
struct sg_table { struct scatterlist *sgl; u32 nents, orig_nents; };
struct vb2_mem_ops { int unused; };
static const struct vb2_mem_ops vb2_dma_sg_memops;
enum { DMA_BIDIRECTIONAL, DMA_FROM_DEVICE, VB2_MEMORY_MMAP = 8 };
struct vb2_queue {
    unsigned num_buffers;
    struct device *dev;
    const struct vb2_mem_ops *mem_ops;
    int dma_dir;
};
struct vb2_buffer {
    struct vb2_queue *vb2_queue;
    unsigned memory, num_planes;
    struct sg_table *sgt;
    void *vaddr;
    size_t length, payload;
#if TEST_VB2_OLD
    bool need_cache_sync_on_finish;
#else
    bool skip_cache_sync_on_finish;
#endif
};
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; };
struct v4l2_m2m_buffer {
    struct vb2_v4l2_buffer vb;
    uintptr_t list[2];
};
struct crystalhd_rx_buffer;
#include "types.h"
#include "buffers.h"

static unsigned cpu_syncs, device_syncs, vaddr_calls, notifications, warnings;
static unsigned checks;
static bool in_irq, in_terminal_writer, release_during_submit;
static BC_STATUS submit_status;
static u8 *alias, *backing;
static size_t alias_bytes;
static enum { CACHE_NONE, CACHE_CPU, CACHE_INVALIDATE, CACHE_FLUSH, CACHE_DEVICE } cache_step;
static bool finish_skipped(struct vb2_buffer *vb)
{
#if TEST_VB2_OLD
    return !vb->need_cache_sync_on_finish;
#else
    return vb->skip_cache_sync_on_finish;
#endif
}
static void flush_kernel_vmap_range(void *address, int bytes)
{
    size_t offset = (u8 *)address - alias;
    assert(!in_terminal_writer && bytes > 0 && offset + bytes <= alias_bytes);
    memcpy(backing + offset, address, bytes);
    cache_step = CACHE_FLUSH;
}
static void invalidate_kernel_vmap_range(void *address, int bytes)
{
    size_t offset = (u8 *)address - alias;
    assert(!in_irq && !in_terminal_writer && cache_step == CACHE_CPU);
    assert(bytes > 0 && offset + bytes <= alias_bytes);
    memcpy(address, backing + offset, bytes);
    cache_step = CACHE_INVALIDATE;
}
#include "compat.h"
static void check_at(bool ok, unsigned line)
{
    checks++;
    if (!ok)
        fprintf(stderr, "V4L2 capture check %u failed at line %u\n", checks, line);
    assert(ok);
}
#define check(ok) check_at((ok), __LINE__)
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, update) ((value) = (update))
#define WARN_ON_ONCE(value) ((value) ? (warnings++, 1) : 0)
#define ALIGN(value, align) (((value) + (align) - 1) & ~((align) - 1))
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define spin_lock_init(lock) (*(lock) = 0)
#define spin_lock_irqsave(lock, flags) \
    (assert(*(lock) == 0), *(lock) = 1, (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) \
    (assert(*(lock) == 1), *(lock) = 0, (void)(flags))
#define sg_dma_address(sg) ((sg)->address)
#define sg_dma_len(sg) ((sg)->length)
#define sg_next(sg) ((sg)->next)

static size_t vb2_plane_size(struct vb2_buffer *vb, unsigned plane)
{
    assert(!plane);
    return vb->length;
}
static struct sg_table *vb2_dma_sg_plane_desc(struct vb2_buffer *vb, unsigned plane)
{
    assert(!plane);
    return vb->sgt;
}
static void *vb2_plane_vaddr(struct vb2_buffer *vb, unsigned plane)
{
    assert(!in_irq && !in_terminal_writer && !plane);
    vaddr_calls++;
    return vb->vaddr;
}
static void vb2_set_plane_payload(struct vb2_buffer *vb, unsigned plane, size_t size)
{
    assert(!plane);
    vb->payload = size;
}
static void dma_sync_sgtable_for_cpu(struct device *dev, struct sg_table *sgt, int dir)
{
    assert(dev && sgt && sgt->orig_nents >= sgt->nents);
    assert(dir == DMA_BIDIRECTIONAL && !in_irq && !in_terminal_writer);
    cpu_syncs++;
    cache_step = CACHE_CPU;
}
static void dma_sync_sgtable_for_device(struct device *dev, struct sg_table *sgt, int dir)
{
    assert(dev && sgt && sgt->orig_nents >= sgt->nents);
    assert(dir == DMA_BIDIRECTIONAL);
    assert(cache_step == CACHE_FLUSH);
    cache_step = CACHE_DEVICE;
    device_syncs++;
}
static void allocator_finish(struct vb2_buffer *vb)
{
    /* A noncoherent FROM/BIDIRECTIONAL invalidate can discard dirty CPU
     * pixel fixes. Model that visibly; adapter-owned CPU backing skips it.
     */
    if (!finish_skipped(vb)) {
        dma_sync_sgtable_for_cpu(vb->vb2_queue->dev, vb->sgt, DMA_BIDIRECTIONAL);
        memset(vb->vaddr, 0, sizeof(u32));
    }
}
static BC_STATUS crystalhd_rx_submit(struct crystalhd_cmd *cmd,
                                    struct crystalhd_rx_buffer *rx)
{
    struct crystalhd_v4l2_capture_buffer *buffer = rx->cookie;
    assert(cmd && crystalhd_v4l2_capture_owned(buffer));
    assert(buffer->owner == CRYSTALHD_CAPTURE_CORE);
    in_irq = true;
    rx->ops->sync_for_device(NULL, rx);
    in_irq = false;
    if (release_during_submit) {
        assert(submit_status == BC_STS_SUCCESS);
        in_terminal_writer = true;
        rx->ops->release(NULL, rx);
        in_terminal_writer = false;
    }
    return submit_status;
}
#include "finish.h"
#include "buffers.c"

struct fixture {
    struct crystalhd_v4l2_capture_buffer buffer;
    struct vb2_queue queue;
    struct crystalhd_cmd cmd;
    struct device dev;
    struct sg_table sgt;
    struct scatterlist sg[BC_LINK_MAX_SGLS + 1];
    u8 *pixels;
    u8 *backing;
};
static void retired(void *opaque)
{
    struct fixture *f = opaque;
    assert(f->buffer.owner == CRYSTALHD_CAPTURE_RETIRED);
    assert(crystalhd_v4l2_capture_owned(&f->buffer));
    notifications++;
}
static void setup(struct fixture *f)
{
    struct vb2_buffer *vb;
    u32 size;

    memset(f, 0, sizeof(*f));
    assert(!crystalhd_v4l2_capture_size(16, 8, &size));
    f->pixels = calloc(1, size);
    f->backing = calloc(1, size);
    assert(f->pixels && f->backing);
    alias = f->pixels;
    backing = f->backing;
    alias_bytes = size;
    cache_step = CACHE_NONE;
    f->queue.dev = &f->dev;
    f->queue.mem_ops = &vb2_dma_sg_memops;
    f->queue.dma_dir = DMA_BIDIRECTIONAL;
    f->sg[0].address = 0x1000;
    f->sg[0].length = 128;
    f->sg[0].next = &f->sg[1];
    f->sg[1].address = 0x8000;
    f->sg[1].length = size - 128;
    f->sgt.sgl = f->sg;
    f->sgt.nents = 2;
    f->sgt.orig_nents = 3; /* mapping coalesced the original page SG */
    vb = &f->buffer.m2m.vb.vb2_buf;
    vb->vb2_queue = &f->queue;
    vb->memory = VB2_MEMORY_MMAP;
    vb->num_planes = 1;
    vb->length = size;
    vb->sgt = &f->sgt;
    vb->vaddr = f->pixels;
    vb->payload = size;
    f->buffer.m2m.list[0] = 0x1234;
    crystalhd_v4l2_capture_init(&f->buffer, &f->dev, retired, f);
    assert(f->buffer.m2m.list[0] == 0x1234);
    cpu_syncs = device_syncs = vaddr_calls = notifications = warnings = 0;
    release_during_submit = false;
    submit_status = BC_STS_SUCCESS;
}
static void teardown(struct fixture *f)
{
    assert(!crystalhd_v4l2_capture_owned(&f->buffer));
    free(f->pixels);
    free(f->backing);
}
static struct crystalhd_rx_completion picture(struct fixture *f)
{
    struct crystalhd_rx_completion r;
    memset(&r, 0, sizeof(r));
    r.buffer = &f->buffer.rx;
    r.cookie = &f->buffer;
    r.flags = COMP_FLAG_DATA_VALID;
    r.metadata.valid = true;
    r.metadata.picture_width = r.metadata.row_width = 16;
    r.metadata.picture_height = 8;
    r.metadata.pib_line = 9;
    r.metadata.first_pixel_word = 0xaabbccdd;
    r.metadata.firmware_timestamp = 1234567;
    r.y_done_sz = f->buffer.rx.capacity / 4;
    return r;
}

static void mapping_and_pixels(void)
{
    struct fixture f;
    struct crystalhd_rx_completion r;
    struct crystalhd_rx_image image;
    u32 word = 0;
    setup(&f);
    check(!crystalhd_v4l2_capture_prepare(&f.buffer, 16, 8));
    check(f.buffer.rx.sgl == f.sgt.sgl && f.buffer.rx.dma_nents == 2);
    check(f.buffer.rx.cookie == &f.buffer && f.buffer.vaddr == f.pixels);
    check(vaddr_calls == 1 && !f.buffer.m2m.vb.vb2_buf.payload);
    check(f.buffer.rx.ops->read(&f.buffer.rx, 0, &word, 4) == BC_STS_INV_ARG);
    f.buffer.decoder_epoch = 88;
    check(crystalhd_v4l2_capture_submit(&f.cmd, &f.buffer) == BC_STS_SUCCESS);
    check(device_syncs == 1 && vaddr_calls == 1 && !cpu_syncs);
    check(crystalhd_v4l2_capture_submit(&f.cmd, &f.buffer) == BC_STS_BUSY);
    check(crystalhd_v4l2_capture_prepare(&f.buffer, 16, 8) == -EBUSY);
    f.buffer.rx.ops->sync_for_cpu(NULL, &f.buffer.rx);
    check(f.buffer.rx.ops->write(&f.buffer.rx, 127, "abcdefgh", 8) == BC_STS_SUCCESS);
    check(!memcmp(f.pixels + 127, "abcdefgh", 8));
    f.buffer.rx.ops->sync_for_cpu(NULL, &f.buffer.rx);
    check(cpu_syncs == 1); /* Parser writes survive repeated CPU access. */
    check(f.buffer.rx.ops->read(&f.buffer.rx, f.buffer.rx.capacity, &word, 4) == BC_STS_INV_ARG);
    check(f.buffer.rx.ops->write(&f.buffer.rx, UINT32_MAX, &word, 4) == BC_STS_INV_ARG);
    in_irq = true;
    f.buffer.rx.ops->sync_for_device(NULL, &f.buffer.rx);
    in_irq = false;
    check(f.buffer.rx.ops->write(&f.buffer.rx, 0, &word, 4) == BC_STS_INV_ARG);
    r = picture(&f);
    check(crystalhd_v4l2_capture_complete(&f.buffer, &r, &image) == BC_STS_SUCCESS);
    allocator_finish(&f.buffer.m2m.vb.vb2_buf);
    memcpy(&word, f.backing, 4); /* userspace reads the backing, not vmap */
    check(word == 0xaabbccdd && image.payload_bytes == 256 && image.stride_bytes == 32);
    check(!r.buffer && !r.cookie && r.metadata.firmware_timestamp == 1234567);
    check(f.buffer.decoder_epoch == 88 && !crystalhd_v4l2_capture_owned(&f.buffer));
    check(cpu_syncs == 2 && !notifications);
    check(!crystalhd_v4l2_capture_prepare(&f.buffer, 16, 8));
    check(!finish_skipped(&f.buffer.m2m.vb.vb2_buf));
    teardown(&f);
}

static void cancellation_and_failed_stop(void)
{
    struct fixture f;
    unsigned old_syncs;
    setup(&f);
    check(!crystalhd_v4l2_capture_prepare(&f.buffer, 16, 8));
    submit_status = BC_STS_IO_ERROR;
    check(crystalhd_v4l2_capture_submit(&f.cmd, &f.buffer) == BC_STS_IO_ERROR);
    check(!crystalhd_v4l2_capture_owned(&f.buffer) && cpu_syncs == 1);
    submit_status = BC_STS_SUCCESS;
    check(crystalhd_v4l2_capture_submit(&f.cmd, &f.buffer) == BC_STS_SUCCESS);
    /* Failed stop has no DMA-detach proof: no callback and no reclamation. */
    check(!crystalhd_v4l2_capture_retire(&f.buffer));
    check(crystalhd_v4l2_capture_owned(&f.buffer));
    check(crystalhd_v4l2_capture_prepare(&f.buffer, 16, 8) == -EBUSY);
    old_syncs = cpu_syncs;
    in_terminal_writer = true;
    f.buffer.rx.ops->release(NULL, &f.buffer.rx);
    in_terminal_writer = false;
    check(notifications == 1 && cpu_syncs == old_syncs);
    check(crystalhd_v4l2_capture_owned(&f.buffer));
    check(crystalhd_v4l2_capture_retire(&f.buffer));
    check(cpu_syncs == old_syncs + 1 && !crystalhd_v4l2_capture_owned(&f.buffer));
    check(!crystalhd_v4l2_capture_retire(&f.buffer));
    /* Immediate retirement may race the successful submit return. */
    release_during_submit = true;
    check(crystalhd_v4l2_capture_submit(&f.cmd, &f.buffer) == BC_STS_SUCCESS);
    check(f.buffer.owner == CRYSTALHD_CAPTURE_RETIRED);
    check(crystalhd_v4l2_capture_retire(&f.buffer));
    check(!warnings);
    teardown(&f);
}

static void invalid_mapping(void)
{
    struct fixture f;
    struct vb2_buffer *vb;
    u32 size;
    unsigned i;
    setup(&f);
    vb = &f.buffer.m2m.vb.vb2_buf;
    check(crystalhd_v4l2_capture_size(15, 8, &size) == -EINVAL && !size);
    check(crystalhd_v4l2_capture_size(2049, 8, &size) == -EINVAL);
    check(crystalhd_v4l2_capture_size(16, 1092, &size) == -EINVAL);
    check(crystalhd_v4l2_capture_size(0, 0, &size) == -EINVAL);
    check(!crystalhd_v4l2_capture_size(16, 8, &size));
    check(size == ALIGN(16 * 2 * 9 + 4 + 2 * sizeof(BC_PIC_INFO_BLOCK), 4));
    f.queue.dma_dir = DMA_FROM_DEVICE;
    check(crystalhd_v4l2_capture_prepare(&f.buffer, 16, 8) == -EINVAL);
    f.queue.dma_dir = DMA_BIDIRECTIONAL;
    vb->memory = 99;
    check(crystalhd_v4l2_capture_prepare(&f.buffer, 16, 8) == -EINVAL);
    vb->memory = VB2_MEMORY_MMAP;
    vb->length--;
    check(crystalhd_v4l2_capture_prepare(&f.buffer, 16, 8) == -EINVAL);
    vb->length++;
    f.sg[1].length -= 4;
    check(crystalhd_v4l2_capture_prepare(&f.buffer, 16, 8) == -EINVAL);
    f.sg[1].length += 4;
    f.sg[0].address++;
    check(crystalhd_v4l2_capture_prepare(&f.buffer, 16, 8) == -EINVAL);
    f.sg[0].address--;
    f.sg[0].length++;
    check(crystalhd_v4l2_capture_prepare(&f.buffer, 16, 8) == -EINVAL);
    f.sg[0].length--;
    f.sg[0].next = NULL;
    check(crystalhd_v4l2_capture_prepare(&f.buffer, 16, 8) == -EINVAL);
    f.sg[0].next = &f.sg[1];
    vb->vaddr = NULL;
    check(crystalhd_v4l2_capture_prepare(&f.buffer, 16, 8) == -ENOMEM);
    vb->vaddr = f.pixels;
    check(crystalhd_v4l2_capture_submit(&f.cmd, &f.buffer) == BC_STS_INV_ARG);
    check(!crystalhd_v4l2_capture_size(2048, 1091, &size));
    vb->length = size;
    f.sgt.nents = f.sgt.orig_nents = BC_LINK_MAX_SGLS + 1;
    for (i = 0; i < f.sgt.nents; i++) {
        f.sg[i].address = 0x1000 + (u64)i * 8192;
        f.sg[i].length = 4096;
        f.sg[i].next = i + 1 < f.sgt.nents ? &f.sg[i + 1] : NULL;
    }
    f.sg[BC_LINK_MAX_SGLS].length = size - BC_LINK_MAX_SGLS * 4096;
    check(crystalhd_v4l2_capture_prepare(&f.buffer, 2048, 1091) == -E2BIG);
    teardown(&f);
}

static void completion_errors(void)
{
    struct fixture f;
    struct crystalhd_rx_completion r;
    struct crystalhd_rx_image image;
    unsigned i;
    BC_STATUS expected;
    setup(&f);
    check(!crystalhd_v4l2_capture_prepare(&f.buffer, 16, 8));
    check(crystalhd_v4l2_capture_submit(&f.cmd, &f.buffer) == BC_STS_SUCCESS);
    r = picture(&f);
    r.cookie = &f.cmd;
    check(crystalhd_v4l2_capture_complete(&f.buffer, &r, &image) == BC_STS_INV_ARG);
    check(crystalhd_v4l2_capture_owned(&f.buffer) && !cpu_syncs);
    for (i = 0; i < 7; i++) {
        if (i)
            check(crystalhd_v4l2_capture_submit(&f.cmd, &f.buffer) == BC_STS_SUCCESS);
        r = picture(&f);
        expected = BC_STS_INV_ARG;
        switch (i) {
        case 0: r.metadata.picture_height = 7; r.metadata.pib_line = 8; break;
        case 1: r.metadata.pib_line = 7; break;
        case 2: r.flags = COMP_FLAG_FMT_CHANGE; expected = BC_STS_NO_DATA; break;
        case 3: r.metadata.eos_trailer = true; expected = BC_STS_NO_DATA; break;
        case 4: r.metadata.picture_flags = 0x20; expected = BC_STS_NOT_IMPL; break;
        case 5: r.metadata.picture_flags = FLEA_DECODE_ERROR_FLAG; expected = BC_STS_IO_ERROR; break;
        case 6: r.y_done_sz = f.buffer.rx.capacity / 4 + 1; break;
        }
        check(crystalhd_v4l2_capture_complete(&f.buffer, &r, &image) == expected);
        check(!image.payload_bytes && !crystalhd_v4l2_capture_owned(&f.buffer));
        check(!r.buffer && !r.cookie);
    }
    teardown(&f);
}

int main(void)
{
    mapping_and_pixels();
    cancellation_and_failed_stop();
    invalid_mapping();
    completion_errors();
    printf("V4L2 direct SG capture: %u checks passed\n", checks);
    return 0;
}
