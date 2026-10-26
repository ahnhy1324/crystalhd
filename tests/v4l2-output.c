// SPDX-License-Identifier: GPL-2.0-or-later
/* Compile the actual adapter, replacing allocator/vb2/DMA/TX boundaries.
 * Reconstruct every PES from submitted DMA addresses and compare its payload
 * with the source AU. No firmware behavior or PCI timing is emulated here.
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
typedef unsigned refcount_t;
struct crystalhd_adp { int unused; };
struct crystalhd_cmd { int unused; };
struct device { int unused; };
struct scatterlist { dma_addr_t address; u32 length; struct scatterlist *next; };
struct sg_table { struct scatterlist *sgl; u32 nents, orig_nents; };
struct vb2_mem_ops { int unused; };
static const struct vb2_mem_ops vb2_dma_sg_memops;
enum { DMA_TO_DEVICE, DMA_FROM_DEVICE, VB2_MEMORY_MMAP = 8 };
struct vb2_queue { struct device *dev; const struct vb2_mem_ops *mem_ops; int dma_dir; unsigned num_buffers; };
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
struct v4l2_m2m_buffer { struct vb2_v4l2_buffer vb; uintptr_t list[2]; };
struct crystalhd_tx_buffer;
#include "types.h"
#include "output.h"

static unsigned checks, warnings, cpu_syncs, device_syncs, vaddr_calls;
static unsigned deadline_calls, tx_calls, validator_calls, frees;
static int validator_error;
static bool terminal_writer, retain_failure;
static unsigned fail_packet;
static unsigned long expected_deadline;
static struct crystalhd_v4l2_output_buffer *current_buffer;
static size_t sent;
static u64 expected_pts;
static size_t sidecar_bytes;
static unsigned stage_calls, stage_packets;
static bool stage_owned;
static u8 stage_wire[65528];
static size_t stage_bytes;
static u8 *physical;
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
    struct crystalhd_v4l2_output_buffer *b = current_buffer;
    size_t offset = (u8 *)address - (u8 *)b->vaddr;
    assert(!terminal_writer && !crystalhd_v4l2_output_owned(b));
    assert(bytes > 0 && offset + bytes <= b->payload);
    memcpy(physical + offset, address, bytes);
    cache_step = CACHE_FLUSH;
}
static void invalidate_kernel_vmap_range(void *address, int bytes)
{
    struct crystalhd_v4l2_output_buffer *b = current_buffer;
    size_t offset = (u8 *)address - (u8 *)b->vaddr;
    assert(!terminal_writer && !crystalhd_v4l2_output_owned(b));
    assert(cache_step == CACHE_CPU && bytes > 0 && offset + bytes <= b->payload);
    memcpy(address, physical + offset, bytes);
    cache_step = CACHE_INVALIDATE;
}
#include "compat.h"
static void check_at(bool ok, unsigned line)
{
    checks++;
    if (!ok) {
        fprintf(stderr, "direct OUTPUT check %u failed at line %u\n", checks, line);
        abort();
    }
}
#define check(ok) check_at((ok), __LINE__)
#define U32_MAX UINT32_MAX
#define GFP_KERNEL 0
#define WARN_ON_ONCE(value) ((value) ? (warnings++, 1) : 0)
#define min(a, b) ((a) < (b) ? (a) : (b))
#define ALIGN(v, a) (((v) + (a) - 1) & ~((a) - 1))
#define GENMASK_ULL(high, low) (((~0ULL) << (low)) & (~0ULL >> (63 - (high))))
#define sg_dma_address(sg) ((sg)->address)
#define sg_dma_len(sg) ((sg)->length)
#define sg_next(sg) ((sg)->next)
#define refcount_set(ref, value) (*(ref) = (value))
#define refcount_read(ref) (*(ref))
#define refcount_inc(ref) (assert(*(ref) > 0), ++*(ref))
#define refcount_dec(ref) (assert(*(ref) > 1), --*(ref))
static void *kcalloc(size_t count, size_t size, int flags)
{
    (void)flags;
    assert(!terminal_writer);
    return calloc(count, size);
}
static void kfree(void *ptr)
{
    assert(!terminal_writer);
    if (ptr)
        frees++;
    free(ptr);
}
static void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma, int flags)
{
    (void)flags;
    assert(dev && !terminal_writer);
    *dma = 0x8000;
    sidecar_bytes = size;
    return calloc(1, size);
}
static void dma_free_coherent(struct device *dev, size_t size, void *cpu, dma_addr_t dma)
{
    assert(dev && size == sidecar_bytes && dma == 0x8000 && !terminal_writer);
    frees++;
    free(cpu);
}
static void sg_init_table(struct scatterlist *sg, unsigned nents)
{
    unsigned i;
    memset(sg, 0, sizeof(*sg) * nents);
    for (i = 0; i < nents; i++)
        sg[i].next = i + 1 < nents ? &sg[i + 1] : NULL;
}
static size_t vb2_plane_size(struct vb2_buffer *vb, unsigned plane)
{
    assert(!plane);
    return vb->length;
}
static unsigned long vb2_get_plane_payload(struct vb2_buffer *vb, unsigned plane)
{
    assert(!plane);
    return vb->payload;
}
static struct sg_table *vb2_dma_sg_plane_desc(struct vb2_buffer *vb, unsigned plane)
{
    assert(!plane);
    return vb->sgt;
}
static void *vb2_plane_vaddr(struct vb2_buffer *vb, unsigned plane)
{
    assert(!plane && !terminal_writer);
    vaddr_calls++;
    return vb->vaddr;
}
static void dma_sync_sgtable_for_cpu(struct device *dev, struct sg_table *sgt, int dir)
{
    assert(dev && sgt && dir == DMA_TO_DEVICE && !terminal_writer);
    assert(sgt->orig_nents >= sgt->nents);
    assert(!crystalhd_v4l2_output_owned(current_buffer));
    cpu_syncs++;
    cache_step = CACHE_CPU;
}
static void dma_sync_sgtable_for_device(struct device *dev, struct sg_table *sgt, int dir)
{
    assert(dev && sgt && dir == DMA_TO_DEVICE && !terminal_writer);
    assert(sgt->orig_nents >= sgt->nents);
    assert(!crystalhd_v4l2_output_owned(current_buffer));
    assert(cache_step == CACHE_FLUSH);
    cache_step = CACHE_DEVICE;
    device_syncs++;
}
static void dma_wmb(void) { }
static int crystalhd_decoder_validate_h264_locked(struct crystalhd_cmd *cmd, const void *owner)
{
    assert(cmd && owner);
    validator_calls++;
    return validator_error;
}
static BC_STATUS crystalhd_tx_deadline_from_ms(u32 ms, unsigned long *deadline)
{
    deadline_calls++;
    if (!ms)
        return BC_STS_INV_ARG;
    *deadline = expected_deadline = 987654;
    return BC_STS_SUCCESS;
}
static int crystalhd_status_to_errno(BC_STATUS status)
{
    switch (status) {
    case BC_STS_SUCCESS: return 0;
    case BC_STS_INV_ARG: return -EINVAL;
    case BC_STS_TIMEOUT: return -ETIMEDOUT;
    default: return -EIO;
    }
}
static const u8 *resolve_dma(dma_addr_t address, u32 size)
{
    struct crystalhd_v4l2_output_buffer *b = current_buffer;
    struct scatterlist *sg;
    u32 i, offset = 0;

    if (address >= b->header_dma && address + size <= b->header_dma + sidecar_bytes)
        return b->header + (address - b->header_dma);
    sg = b->source->sgl;
    for (i = 0; i < b->source->nents; i++, sg = sg->next) {
        if (address >= sg->address && address + size <= sg->address + sg->length)
            return physical + offset + (address - sg->address);
        offset += sg->length;
    }
    abort();
}
static BC_STATUS crystalhd_tx_transfer_until(struct crystalhd_cmd *cmd,
    const struct crystalhd_tx_buffer *tx, u8 flags, unsigned long deadline)
{
    struct crystalhd_v4l2_output_buffer *b = tx->cookie;
    struct scatterlist *sg = tx->sgl;
    size_t capacity = b->payload + sidecar_bytes;
    u8 *whole = malloc(capacity), *wire = whole;
    u32 i, bytes = 0, header_bytes, payload;
    u64 pts;

    assert(whole && cmd && !flags && deadline == expected_deadline && deadline_calls == 1);
    check(!b->cpu_synced && device_syncs == tx_calls + 1);
    tx_calls++;
    tx->ops->get(tx);
    check(crystalhd_v4l2_output_owned(b));
    check(tx->dma_nents + !!tx->tail_size <= BC_LINK_MAX_SGLS);
    for (i = 0; i < tx->dma_nents; i++, sg = sg->next) {
        check(sg && sg->length && !(sg->address & 3) && !(sg->length & 3));
        check(bytes + sg->length <= capacity);
        memcpy(wire + bytes, resolve_dma(sg->address, sg->length), sg->length);
        bytes += sg->length;
    }
    if (tx->tail_size) {
        const u8 *tail = resolve_dma(tx->tail_addr, 4);
        check(tx->tail_size < 4 && !(tx->tail_addr & 3));
        for (i = tx->tail_size; i < 4; i++)
            check(!tail[i]);
        memcpy(wire + bytes, tail, tx->tail_size);
        bytes += tx->tail_size;
    }
    check(bytes == tx->bytes);
    check(!wire[0] && !wire[1] && wire[2] == 1 && wire[3] == 0xe0);
    check((((u32)wire[4] << 8) | wire[5]) == bytes - 6);
    check(bytes - 6 <= 0xfff0 && wire[6] == 0x81);
    header_bytes = 9 + wire[8];
    check(header_bytes == (sent ? 12U : 16U));
    check(wire[7] == (sent ? 0 : 0x80));
    if (!sent) {
        check((wire[9] & 0xf1) == 0x21 && (wire[11] & 1) && (wire[13] & 1));
        pts = ((u64)((wire[9] >> 1) & 7) << 30) |
              ((u64)wire[10] << 22) | ((u64)(wire[11] >> 1) << 15) |
              ((u64)wire[12] << 7) | (wire[13] >> 1);
        check(pts == expected_pts);
    }
    for (i = sent ? 9 : 14; i < header_bytes; i++)
        check(wire[i] == 0xff);
    payload = bytes - header_bytes;
    check(payload && sent + payload <= b->payload);
    check(!memcmp(wire + header_bytes, physical + sent, payload));
    /* Independent oracle: a missing invalidate must not be hidden by a
     * stale alias flush overwriting both DMA bytes and expected backing.
     */
    for (i = 0; i < payload; i++)
        assert(wire[header_bytes + i] == (u8)((sent + i) * 17 + (sent + i) / 251));
    if (sent + payload < b->payload)
        check(!(bytes & 3) && !(payload & 3));
    sent += payload;
    free(whole);
    if (tx_calls == fail_packet && retain_failure)
        return BC_STS_IO_ERROR;
    tx->ops->put(NULL, tx);
    return tx_calls == fail_packet ? BC_STS_TIMEOUT : BC_STS_SUCCESS;
}
#include "formatter.h"
/* Staging transport boundary mock uses the actual canonical formatter. Its
 * backing lease is deliberately independent of the borrowed OUTPUT plane.
 * The full production staging sender/retention is tested by h264-stream.sh.
 */
static int crystalhd_decoder_submit_h264(struct crystalhd_cmd *cmd, const void *owner,
    const u8 *data, size_t bytes, bool pts_valid, u64 pts, u32 timeout_ms)
{
    size_t offset = 0;
    unsigned long deadline;
    BC_STATUS sts;
    assert(cmd && owner && pts_valid && pts == expected_pts && !stage_owned);
    assert(current_buffer->cpu_synced && !crystalhd_v4l2_output_owned(current_buffer));
    stage_calls++;
    sts = crystalhd_tx_deadline_from_ms(timeout_ms, &deadline);
    if (sts != BC_STS_SUCCESS) return crystalhd_status_to_errno(sts);
    while (offset < bytes) {
        size_t consumed, header;
        check(!crystalhd_h264_format_pes(stage_wire, sizeof(stage_wire), data + offset,
            bytes - offset, !offset, pts, &consumed, &stage_bytes));
        stage_packets++;
        header = offset ? 9 : 14;
        check(stage_wire[8] == (offset ? 0 : 5) && stage_wire[7] == (offset ? 0 : 0x80));
        if (!offset) {
            u64 decoded = ((u64)((stage_wire[9] >> 1) & 7) << 30) |
                ((u64)stage_wire[10] << 22) | ((u64)(stage_wire[11] >> 1) << 15) |
                ((u64)stage_wire[12] << 7) | (stage_wire[13] >> 1);
            check(decoded == expected_pts);
        }
        check(stage_bytes == header + consumed);
        check((((u32)stage_wire[4] << 8) | stage_wire[5]) == stage_bytes - 6);
        check(!memcmp(stage_wire + header, physical + offset, consumed));
        for (size_t i = 0; i < consumed; i++)
            assert(stage_wire[header + i] == (u8)((offset + i) * 17 + (offset + i) / 251));
        if (offset + consumed < bytes) check(stage_bytes - 6 == 0xfff0);
        offset += consumed;
        sent = offset;
        if (stage_packets == fail_packet) {
            stage_owned = retain_failure;
            return retain_failure ? -EIO : -ETIMEDOUT;
        }
    }
    return 0;
}
#include "output.c"

struct fixture {
    struct crystalhd_v4l2_output_buffer buffer;
    struct device dev;
    struct crystalhd_cmd cmd;
    struct vb2_queue queue;
    struct sg_table table;
    u8 *data;
    u8 *alias;
};
static void setup(struct fixture *f, u32 payload, u32 segment)
{
    struct vb2_buffer *vb;
    u32 length = (payload + 3) & ~3U;
    u32 i, remaining;
    memset(f, 0, sizeof(*f));
    f->data = malloc(length);
    f->alias = calloc(1, length);
    assert(f->data && f->alias);
    physical = f->data;
    cache_step = CACHE_NONE;
    for (i = 0; i < length; i++)
        f->data[i] = (u8)(i * 17 + i / 251);
    f->table.nents = (length + segment - 1) / segment;
    f->table.orig_nents = f->table.nents + 3;
    f->table.sgl = calloc(f->table.nents, sizeof(*f->table.sgl));
    assert(f->table.sgl);
    sg_init_table(f->table.sgl, f->table.nents);
    remaining = length;
    for (i = 0; i < f->table.nents; i++) {
        f->table.sgl[i].address = 0x100000ULL + (u64)i * 0x10000;
        f->table.sgl[i].length = min(remaining, segment);
        remaining -= f->table.sgl[i].length;
    }
    f->queue.dev = &f->dev;
    f->queue.dma_dir = DMA_TO_DEVICE;
    f->queue.mem_ops = &vb2_dma_sg_memops;
    vb = &f->buffer.m2m.vb.vb2_buf;
    vb->vb2_queue = &f->queue;
    vb->memory = VB2_MEMORY_MMAP;
    vb->num_planes = 1;
    vb->length = length;
    vb->payload = payload;
    vb->vaddr = f->alias;
    vb->sgt = &f->table;
    f->buffer.m2m.list[0] = 0xcafe;
    current_buffer = &f->buffer;
    warnings = cpu_syncs = device_syncs = vaddr_calls = deadline_calls = 0;
    tx_calls = validator_calls = frees = fail_packet = 0;
    stage_calls = stage_packets = 0;
    stage_owned = false;
    terminal_writer = retain_failure = false;
    validator_error = 0;
    expected_pts = (1ULL << 32) + 3456789;
    sent = 0;
    check(!crystalhd_v4l2_output_init(&f->buffer, &f->dev));
    check(f->buffer.m2m.list[0] == 0xcafe);
}
static void teardown(struct fixture *f)
{
    check(!crystalhd_v4l2_output_owned(&f->buffer));
    crystalhd_v4l2_output_cleanup(&f->buffer);
    check(frees == 2);
    free(f->table.sgl);
    free(f->data);
    free(f->alias);
}
static void packet_cases(void)
{
    static const u32 lengths[] = { 1, 2, 3, 4, 65507, 65508, 65509, 65510,
                                  65511, 65512, 65513, 65514, 130000,
                                  131026, 131027, 131028, 200003 };
    struct fixture f;
    unsigned i;
    for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        setup(&f, lengths[i], 256);
        check(!crystalhd_v4l2_output_prepare(&f.buffer));
        check(vaddr_calls == 1 && f.buffer.source == &f.table);
        check(!crystalhd_v4l2_output_submit(&f.cmd, &f, &f.buffer, expected_pts, 100));
        check(sent == lengths[i] && deadline_calls == 1 && validator_calls == 1);
        check(cpu_syncs == tx_calls + 1 && device_syncs == tx_calls);
        check(lengths[i] > 65508 ? stage_calls == 1 && !tx_calls : !stage_calls && tx_calls == 1);
        check(f.buffer.cpu_synced && finish_skipped(&f.buffer.m2m.vb.vb2_buf));
        check(!warnings);
        check(!crystalhd_v4l2_output_prepare(&f.buffer));
        check(!finish_skipped(&f.buffer.m2m.vb.vb2_buf));
        teardown(&f);
    }
    setup(&f, 8U * 1024U * 1024U, 16384);
    check(!crystalhd_v4l2_output_prepare(&f.buffer));
    check(!crystalhd_v4l2_output_submit(&f.cmd, &f, &f.buffer, expected_pts, 100));
    check(!tx_calls && stage_packets == 129 && sent == 8U * 1024U * 1024U);
    f.buffer.m2m.vb.vb2_buf.payload++;
    f.buffer.m2m.vb.vb2_buf.length += 4;
    check(crystalhd_v4l2_output_prepare(&f.buffer) == -E2BIG);
    teardown(&f);
}
static void retained_and_timeout(void)
{
    struct fixture f;
    u8 *header;
    struct scatterlist *saved_wire;
    struct crystalhd_tx_buffer tx;
    struct scatterlist sg0;
    unsigned cpu_before;
    setup(&f, 60003, 256);
    check(!crystalhd_v4l2_output_prepare(&f.buffer));
    fail_packet = 1;
    retain_failure = true;
    check(crystalhd_v4l2_output_submit(&f.cmd, &f, &f.buffer, expected_pts, 100) == -EIO);
    check(crystalhd_v4l2_output_owned(&f.buffer) && tx_calls == 1);
    header = malloc(sidecar_bytes);
    saved_wire = malloc(sizeof(*saved_wire) * BC_LINK_MAX_SGLS);
    assert(header && saved_wire);
    memcpy(header, f.buffer.header, sidecar_bytes);
    memcpy(saved_wire, f.buffer.wire_sg, sizeof(*saved_wire) * BC_LINK_MAX_SGLS);
    tx = f.buffer.tx;
    sg0 = f.buffer.wire_sg[0];
    cpu_before = cpu_syncs;
    check(!crystalhd_v4l2_output_retire(&f.buffer));
    check(crystalhd_v4l2_output_prepare(&f.buffer) == -EBUSY);
    check(crystalhd_v4l2_output_submit(&f.cmd, &f, &f.buffer, 0, 100) == -EBUSY);
    crystalhd_v4l2_output_cleanup(&f.buffer);
    check(!frees && warnings == 1 && cpu_syncs == cpu_before);
    check(!memcmp(header, f.buffer.header, sidecar_bytes));
    check(!memcmp(saved_wire, f.buffer.wire_sg, sizeof(*saved_wire) * BC_LINK_MAX_SGLS));
    check(!memcmp(&tx, &f.buffer.tx, sizeof(tx)));
    check(!memcmp(&sg0, &f.buffer.wire_sg[0], sizeof(sg0)));
    free(header);
    free(saved_wire);
    terminal_writer = true;
    f.buffer.tx.ops->put(NULL, &f.buffer.tx);
    terminal_writer = false;
    check(cpu_syncs == cpu_before && !crystalhd_v4l2_output_owned(&f.buffer));
    check(crystalhd_v4l2_output_retire(&f.buffer));
    check(cpu_syncs == cpu_before + 1);
    check(crystalhd_v4l2_output_submit(&f.cmd, &f, &f.buffer, 0, 100) == -EPIPE);
    teardown(&f);

    setup(&f, 60003, 4096);
    check(!crystalhd_v4l2_output_prepare(&f.buffer));
    fail_packet = 1;
    check(crystalhd_v4l2_output_submit(&f.cmd, &f, &f.buffer, expected_pts, 100) == -ETIMEDOUT);
    check(!crystalhd_v4l2_output_owned(&f.buffer) && f.buffer.cpu_synced);
    check(tx_calls == 1 && deadline_calls == 1);
    teardown(&f);

    setup(&f, 200003, 4096);
    check(!crystalhd_v4l2_output_prepare(&f.buffer));
    fail_packet = 2;
    retain_failure = true;
    check(crystalhd_v4l2_output_submit(&f.cmd, &f, &f.buffer, expected_pts, 100) == -EIO);
    check(stage_owned && stage_packets == 2 && deadline_calls == 1 && !tx_calls);
    check(!crystalhd_v4l2_output_owned(&f.buffer) && f.buffer.cpu_synced);
    header = malloc(stage_bytes);
    assert(header);
    memcpy(header, stage_wire, stage_bytes);
    memset(f.data, 0xa5, f.buffer.payload);
    memset(f.alias, 0x5a, f.buffer.payload);
    check(!memcmp(header, stage_wire, stage_bytes));
    check(crystalhd_v4l2_output_retire(&f.buffer));
    check(crystalhd_v4l2_output_submit(&f.cmd, &f, &f.buffer, 0, 100) == -EPIPE);
    teardown(&f); /* Source backing can go; retained DMA names staging only. */
    check(!memcmp(header, stage_wire, stage_bytes));
    stage_owned = false; /* Separate terminal staging retirement. */
    free(header);
}
static void validation(void)
{
    struct fixture f;
    struct vb2_buffer *vb;
    setup(&f, 100, 256);
    vb = &f.buffer.m2m.vb.vb2_buf;
    f.queue.dma_dir = DMA_FROM_DEVICE;
    check(crystalhd_v4l2_output_prepare(&f.buffer) == -EINVAL);
    f.queue.dma_dir = DMA_TO_DEVICE;
    vb->memory = 99;
    check(crystalhd_v4l2_output_prepare(&f.buffer) == -EINVAL);
    vb->memory = VB2_MEMORY_MMAP;
    f.table.sgl[0].address++;
    check(crystalhd_v4l2_output_prepare(&f.buffer) == -EINVAL);
    f.table.sgl[0].address--;
    f.table.sgl[0].length -= 4;
    check(crystalhd_v4l2_output_prepare(&f.buffer) == -EINVAL);
    f.table.sgl[0].length += 4;
    vb->vaddr = NULL;
    check(crystalhd_v4l2_output_prepare(&f.buffer) == -ENOMEM);
    vb->vaddr = f.alias;
    check(!crystalhd_v4l2_output_prepare(&f.buffer));
    validator_error = -EBUSY;
    check(crystalhd_v4l2_output_submit(&f.cmd, &f, &f.buffer, expected_pts, 100) == -EBUSY);
    check(!tx_calls && !deadline_calls && f.buffer.cpu_synced);
    validator_error = 0;
    check(crystalhd_v4l2_output_submit(&f.cmd, &f, &f.buffer, 1ULL << 33, 100) == -EINVAL);
    check(crystalhd_v4l2_output_submit(&f.cmd, &f, &f.buffer, expected_pts, 0) == -EINVAL);
    check(!tx_calls);
    teardown(&f);

    setup(&f, 8000, 4);
    check(!crystalhd_v4l2_output_prepare(&f.buffer));
    check(crystalhd_v4l2_output_submit(&f.cmd, &f, &f.buffer, expected_pts, 100) == -E2BIG);
    check(!tx_calls && !crystalhd_v4l2_output_owned(&f.buffer));
    teardown(&f);
}
int main(void)
{
    packet_cases();
    retained_and_timeout();
    validation();
    printf("V4L2 direct OUTPUT: %u checks passed\n", checks);
    return 0;
}
