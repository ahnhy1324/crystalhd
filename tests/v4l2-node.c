// SPDX-License-Identifier: GPL-2.0-or-later
/* Execute extracted syscall/cancellation code with modeled core and VB2 edges. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint64_t u64;
typedef uintptr_t dma_addr_t;
typedef unsigned spinlock_t;
#define BC_LINK_CAP_EN 2
#define BC_LINK_FMT_CHG 4
#define COMP_FLAG_FMT_CHANGE 1
#define COMP_FLAG_DATA_VALID 4
#include "node-constants.h"
#define READ_ONCE(x) (x)
#define BC_LINK_MAX_SGLS 1024U
#define PAGE_SIZE 4096U
#define GFP_KERNEL 1
#define __GFP_ZERO 2
#define DMA_BIDIRECTIONAL 3
#define VM_MAP 4
#define PAGE_KERNEL 5
#define DIV_ROUND_UP(v, a) (((v) + (a) - 1) / (a))
#define ALIGN(v, a) (((v) + (a) - 1) & ~((a) - 1))
#include "node-size.h"
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define V4L2_MEMORY_MMAP 1
#define V4L2_DEC_CMD_START 0
#define V4L2_DEC_CMD_STOP 1
enum v4l2_buf_type { V4L2_BUF_TYPE_VIDEO_CAPTURE = 1,
    V4L2_BUF_TYPE_VIDEO_OUTPUT = 2, INVALID_CAPTURE_TYPE = 9 };
#define V4L2_TYPE_IS_OUTPUT(t) ((t) == V4L2_BUF_TYPE_VIDEO_OUTPUT)
enum { CHD_V4L2_OFF, CHD_V4L2_RUNNING, CHD_V4L2_DRAINING, CHD_V4L2_DRAINED, CHD_V4L2_FAILED };
#define V4L2_EVENT_EOS 1
#define V4L2_FIELD_NONE 0
#define V4L2_BUF_FLAG_ERROR 1
#define V4L2_BUF_FLAG_LAST 2
#define VB2_BUF_STATE_ERROR 1
#define VB2_BUF_STATE_DONE 2
#define dev_err(...) ((void)0)
#define clamp_t(t, v, lo, hi) ((t)(v) < (t)(lo) ? (t)(lo) : (t)(v) > (t)(hi) ? (t)(hi) : (t)(v))
#define V4L2_PIX_FMT_H264 1
#define V4L2_PIX_FMT_YUYV 2
#define V4L2_COLORSPACE_REC709 3
#define V4L2_COLORSPACE_SMPTE170M 1
#define V4L2_COLORSPACE_BT878 4
#define V4L2_PIX_FMT_PRIV_MAGIC 0xfeedcafeU
#define U8_MAX 255U
#define V4L2_YCBCR_ENC_DEFAULT 0
#define V4L2_QUANTIZATION_DEFAULT 0
#define V4L2_XFER_FUNC_DEFAULT 0
static bool v4l2_is_colorspace_valid(u32 v) { return v > 0 && v <= 12; }
static bool v4l2_is_xfer_func_valid(u32 v) { return v > 0 && v <= 7; }
static bool v4l2_is_ycbcr_enc_valid(uint8_t v) { return v > 0 && v <= 8; }
static bool v4l2_is_quant_valid(uint8_t v) { return v == 1 || v == 2; }
struct work_struct { bool pending; void (*function)(struct work_struct *); };
struct vb2_buffer { u64 timestamp; unsigned payload, index, size; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; unsigned field, sequence, flags, done; };
struct v4l2_m2m_buffer { struct vb2_v4l2_buffer vb; struct v4l2_m2m_buffer *next; };
struct crystalhd_v4l2_output_buffer { struct { struct vb2_v4l2_buffer vb; } m2m; bool owned; };
struct crystalhd_rx_metadata { bool valid, eos_trailer; u32 picture_number, picture_flags; };
struct crystalhd_rx_image { unsigned payload_bytes; };
struct v4l2_event { unsigned type; };
enum { CRYSTALHD_DECODER_CHANNEL_CONFIGURED = 1,
    CRYSTALHD_DECODER_CHANNEL_STARTED = 2 };
struct mutex { bool held; };
struct device { int unused; };
struct v4l2_device { int unused; };
struct video_device {
    struct v4l2_device *v4l2_dev;
    void *ctrl_handler;
    const void *fops, *ioctl_ops;
    void (*release)(struct video_device *);
    struct mutex *lock;
    unsigned vfl_dir, device_caps;
    char name[32]; void *drvdata;
};
struct workqueue_struct { int unused; };
struct delayed_work { bool pending; void (*function)(struct work_struct *); };
struct scatterlist { int unused; };
struct page { bool head; unsigned dma_refs, vmap_refs; };
struct sg_table {
    struct scatterlist *sgl; unsigned nents, orig_nents;
    struct page **pages; unsigned nr_pages; bool mapped;
};
struct crystalhd_rx_buffer {
    u32 capacity, dma_nents, output_format;
    struct scatterlist *sgl; const void *ops; void *cookie;
};
static const unsigned chd_discovery_ops;
struct crystalhd_v4l2_capture_buffer {
    struct v4l2_m2m_buffer m2m;
    u64 decoder_epoch; bool owned; u32 width, height;
};
struct crystalhd_v4l2_decoder { int phase; unsigned count; u64 epoch; };
struct crystalhd_v4l2 { int unused; };
struct crystalhd_v4l2_ctx { int unused; };
#define KERNEL_VERSION(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#define LINUX_VERSION_CODE NODE_KERNEL_VERSION
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
#define NODE_NEW_QUEUE_API
#endif
#ifdef NODE_NEW_QUEUE_API
#define QUEUE_COUNT(q) ((q)->allocated_buffers)
#else
#define QUEUE_COUNT(q) ((q)->num_buffers)
#endif
struct vb2_queue {
#ifdef NODE_NEW_QUEUE_API
    unsigned allocated_buffers; /* Deliberately no removed num_buffers member. */
#else
    unsigned num_buffers;
#endif
    bool streaming, error, last;
    struct vb2_buffer *bufs[BC_RX_LIST_CNT];
    unsigned type, io_modes, buf_struct_size, timestamp_flags, bidirectional;
    void *drv_priv; const void *ops, *mem_ops;
    struct mutex *lock; struct device *dev;
    bool initialized;
};
#ifdef NODE_NEW_QUEUE_API
static unsigned vb2_get_num_buffers(struct vb2_queue *q) { return QUEUE_COUNT(q); }
#endif
static struct vb2_buffer *vb2_get_buffer(struct vb2_queue *q, unsigned index)
{ return index < QUEUE_COUNT(q) ? q->bufs[index] : NULL; }
#include "node-queue.h"
struct v4l2_m2m_ctx { struct vb2_queue src, dst; struct v4l2_m2m_buffer *dst_ready; };
#define v4l2_m2m_for_each_dst_buf(ctx, b) for ((b) = (ctx)->dst_ready; (b); (b) = (b)->next)
struct v4l2_ctrl_handler { int error; };
struct v4l2_ctrl { unsigned flags; };
struct v4l2_m2m_dev { int unused; };
struct v4l2_fh { struct v4l2_m2m_ctx *m2m_ctx; };
struct v4l2_pix_format { u32 width, height, bytesperline, sizeimage, pixelformat, field, colorspace, ycbcr_enc, quantization, xfer_func, priv, flags; };
struct v4l2_format { unsigned type; struct { struct v4l2_pix_format pix; } fmt; };
struct v4l2_fmtdesc { unsigned index, type, pixelformat, flags; };
struct file { void *private_data; };
/* Mutually exclusive prototypes and members: wrong compatibility branches
 * fail to compile, rather than silently accepting an obsolete API.
 */
static unsigned fh_add_calls, fh_del_calls, wait_prepare_calls, wait_finish_calls;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 18, 0)
static void v4l2_fh_add(struct v4l2_fh *fh) { assert(fh); fh_add_calls++; }
static void v4l2_fh_del(struct v4l2_fh *fh) { assert(fh); fh_del_calls++; }
#else
static void v4l2_fh_add(struct v4l2_fh *fh, struct file *file)
{ assert(fh && file); file->private_data = fh; fh_add_calls++; }
static void v4l2_fh_del(struct v4l2_fh *fh, struct file *file)
{ assert(file->private_data == fh); file->private_data = NULL; fh_del_calls++; }
#endif
struct vb2_ops {
    unsigned present;
#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 0, 0)
    void (*wait_prepare)(struct vb2_queue *);
    void (*wait_finish)(struct vb2_queue *);
#endif
};
#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 0, 0)
static void vb2_ops_wait_prepare(struct vb2_queue *q) { assert(q); wait_prepare_calls++; }
static void vb2_ops_wait_finish(struct vb2_queue *q) { assert(q); wait_finish_calls++; }
#endif
#include "node-api.h"
static void node_api_cases(void)
{
    struct v4l2_fh fh = {0};
    struct file file = {0};
    const struct vb2_ops ops = { .present = 1, CRYSTALHD_V4L2_WAIT_OPS };
    crystalhd_v4l2_fh_add(&fh, &file);
    assert(file.private_data == &fh && fh_add_calls == 1);
    crystalhd_v4l2_fh_del(&fh, &file);
    assert(!file.private_data && fh_del_calls == 1 && ops.present);
#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 0, 0)
    struct vb2_queue q = {0};
    ops.wait_prepare(&q); ops.wait_finish(&q);
    assert(wait_prepare_calls == 1 && wait_finish_calls == 1);
#else
    assert(!wait_prepare_calls && !wait_finish_calls);
#endif
}
struct v4l2_requestbuffers { unsigned type, memory, count; };
struct v4l2_buffer { unsigned type; };
struct v4l2_decoder_cmd { unsigned cmd, flags; struct { int speed; unsigned format; } start; };
struct semaphore { bool held; };
struct crystalhd_rx_dma_pkt {
    struct crystalhd_rx_dma_pkt *next; struct crystalhd_rx_buffer *buffer; void *cookie;
};
struct crystalhd_dioq { unsigned count; };
enum { sts_free = 0, rx_sts_waiting = 1 };
struct crystalhd_hw {
    struct semaphore fetch_sem; u64 rx_cancel_epoch; bool dma_fault;
    u32 RxCaptureState, PicQSts;
    spinlock_t rx_lock, lock; unsigned rx_list_sts[2];
    struct crystalhd_dioq *rx_actq, *rx_rdyq, *rx_freeq;
    struct crystalhd_rx_dma_pkt *rx_fallback_head, *rx_pkt_pool_head;
};
struct crystalhd_cmd {
    void *session_owner; unsigned decoder_phase, state;
    struct crystalhd_hw *hw_ctx; struct crystalhd_adp *adp;
};
struct crystalhd_rx_completion {
    struct crystalhd_rx_buffer *buffer; void *cookie; unsigned flags; u64 capture_epoch;
    struct crystalhd_rx_metadata metadata;
};
struct pci_dev { unsigned device, irq; };
struct crystalhd_adp { struct crystalhd_cmd cmds; struct pci_dev *pdev; struct mutex tx_lock; };
struct crystalhd_device_access { struct crystalhd_adp *adp; };
#include "node-types.h"

static struct crystalhd_v4l2_file fixture;
static struct crystalhd_v4l2_node node;
static struct crystalhd_v4l2_ctx lease;
static struct v4l2_m2m_ctx queues;
static struct crystalhd_adp adapter;
static struct file handle;
static unsigned checks, scenarios, warnings, kicks, joins, enters, exits;
static unsigned releases, stops, flushes, closes, req_calls, streamoff_calls;
static unsigned streamon_calls, qbuf_calls, acquires, returns;
static unsigned ctx_destroys, dma_frees, private_frees;
static int enter_error, stop_error, release_error, acquire_error, streamon_error;
static bool access_active, hardware_owned;
static bool constructor_mode, ctor_file_live, ctor_node_live, ctor_fh_live;
static bool ctor_ctrl_live, ctor_m2m_live, ctor_video_live, ctor_close_pending;
static unsigned ctor_fail, ctor_allocs, ctor_queue_calls, ctor_queue_releases;
static unsigned ctor_dev_refs, ctor_parent_refs, ctor_wq_calls, ctor_wq_live;
static unsigned ctor_fh_exits, ctor_ctx_creates, ctor_binds, ctor_video_releases;
static int ctor_ctx_error;
static void (*ctor_cleanup)(void *);
static struct device ctor_device;
static struct crystalhd_v4l2 ctor_parent;
static struct v4l2_device ctor_v4l2;
static struct v4l2_m2m_dev ctor_m2m;
static struct workqueue_struct ctor_workqueues[2];
static bool ctor_workqueue_live[2];
static struct v4l2_ctrl ctor_minimum;
enum { CTOR_OK, CTOR_ALLOC, CTOR_CTRL_INIT, CTOR_CTRL_NEW, CTOR_RX_WQ,
    CTOR_TX_WQ, CTOR_M2M, CTOR_VIDEO, CTOR_CTX_INIT, CTOR_SRC_QUEUE, CTOR_DST_QUEUE };
static void (*join_hook)(void *);
static struct crystalhd_v4l2_output_buffer tx_buffer;
static struct crystalhd_v4l2_capture_buffer last_capture;
#define last_buffer last_capture.m2m.vb
static struct vb2_v4l2_buffer *source;
static struct vb2_v4l2_buffer *source_next;
static unsigned tx_queues, tx_joins, transports, completed, eos_events;
static int transport_error;
static int resume_error, controller_resume_error;
static unsigned firmware_resumes, controller_resumes;
static bool access_exclusive;
static void (*transport_hook)(void);
static void (*tx_join_hook)(void);
static bool eos_receive;
static bool error_receive;
static int decode_error_result;
static struct crystalhd_hw pause_hw;
static struct pci_dev pause_pci;
static struct crystalhd_dioq pause_queues[3];
static struct crystalhd_rx_dma_pkt pause_packets[BC_RX_LIST_CNT + 1];
static unsigned pause_stops, pause_releases, irq_disables, irq_enables;
static BC_STATUS pause_status;
static bool detached_owned, irq_disabled;
static void (*irq_drain_hook)(void);

enum allocation_edge { NO_FAILURE, ARRAY_EDGE, PAGE_EDGE, SG_EDGE, MAP_EDGE,
                       VMAP_EDGE, NENTS_EDGE };
static enum allocation_edge fail_edge;
static unsigned fail_at, edge_calls, array_calls, block_calls;
static unsigned live_arrays, live_blocks, live_tables, live_maps, live_vmaps;
static unsigned map_calls, unmap_calls, block_frees, max_segments;
struct vmap_record { struct page **pages; unsigned nr_pages; };

static bool allocation_fails(enum allocation_edge edge)
{ return fail_edge == edge && ++edge_calls == fail_at; }

static void *kvcalloc(size_t count, size_t size, int flags)
{
    void *result;
    assert(count == 1094 && size == sizeof(struct page *) && flags == GFP_KERNEL);
    array_calls++;
    if (allocation_fails(ARRAY_EDGE)) return NULL;
    result = calloc(count, size); assert(result); live_arrays++; return result;
}
static void kvfree(void *pointer)
{ if (pointer) { assert(live_arrays); live_arrays--; free(pointer); } }
static struct page *alloc_pages(int flags, unsigned order)
{
    struct page *pages;
    assert(flags == (GFP_KERNEL | __GFP_ZERO) && order == 1);
    block_calls++;
    if (allocation_fails(PAGE_EDGE)) return NULL;
    pages = calloc(2, sizeof(*pages)); assert(pages);
    pages[0].head = true; live_blocks++; return pages;
}
static void __free_pages(struct page *pages, unsigned order)
{
    assert(pages && order == 1 && pages[0].head && !pages[1].head &&
           !pages[0].dma_refs && !pages[0].vmap_refs && live_blocks);
    live_blocks--; block_frees++; free(pages);
}
static int sg_alloc_table_from_pages(struct sg_table *sgt, struct page **pages,
        unsigned nr_pages, unsigned offset, unsigned size, int flags)
{
    assert(!sgt->sgl && nr_pages == 1094 && !offset && flags == GFP_KERNEL &&
           size > 1092 * PAGE_SIZE && size <= 1093 * PAGE_SIZE);
    for (unsigned i = 0; i < nr_pages; i += 2)
        assert(pages[i]->head && pages[i + 1] == pages[i] + 1 && !pages[i + 1]->head);
    if (allocation_fails(SG_EDGE)) return -ENOMEM;
    /* Every two-page block is physically separate: no accidental coalescing. */
    sgt->nents = sgt->orig_nents = nr_pages / 2;
    sgt->sgl = calloc(sgt->nents, sizeof(*sgt->sgl)); assert(sgt->sgl);
    sgt->pages = pages; sgt->nr_pages = nr_pages; live_tables++;
    if (sgt->nents > max_segments) max_segments = sgt->nents;
    return 0;
}
static int dma_map_sgtable(struct device *dev, struct sg_table *sgt, int direction,
                         unsigned attrs)
{
    (void)dev;
    assert(sgt->sgl && !sgt->mapped && direction == DMA_BIDIRECTIONAL && !attrs);
    map_calls++;
    if (allocation_fails(MAP_EDGE)) return -EIO;
    if (allocation_fails(NENTS_EDGE)) sgt->nents = BC_LINK_MAX_SGLS + 1;
    for (unsigned i = 0; i < sgt->nr_pages; i += 2) sgt->pages[i]->dma_refs++;
    sgt->mapped = true; live_maps++; return 0;
}
static void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt, int direction,
                            unsigned attrs)
{
    (void)dev;
    assert(sgt->mapped && direction == DMA_BIDIRECTIONAL && !attrs && live_maps);
    for (unsigned i = 0; i < sgt->nr_pages; i += 2) {
        assert(sgt->pages[i]->dma_refs == 1); sgt->pages[i]->dma_refs--;
    }
    sgt->mapped = false; live_maps--; unmap_calls++; dma_frees++;
}
static void sg_free_table(struct sg_table *sgt)
{
    assert(sgt->sgl && !sgt->mapped && live_tables);
    live_tables--; free(sgt->sgl); memset(sgt, 0, sizeof(*sgt));
}
static void *vmap(struct page **pages, unsigned nr_pages, unsigned flags, int prot)
{
    struct vmap_record *record;
    assert(nr_pages == 1094 && flags == VM_MAP && prot == PAGE_KERNEL);
    if (allocation_fails(VMAP_EDGE)) return NULL;
    record = malloc(sizeof(*record)); assert(record);
    record->pages = pages; record->nr_pages = nr_pages;
    for (unsigned i = 0; i < nr_pages; i += 2) pages[i]->vmap_refs++;
    live_vmaps++; return record;
}
static void vunmap(void *pointer)
{
    struct vmap_record *record = pointer;
    assert(record && live_vmaps);
    for (unsigned i = 0; i < record->nr_pages; i += 2) {
        assert(record->pages[i]->vmap_refs == 1); record->pages[i]->vmap_refs--;
    }
    live_vmaps--; free(record);
}

#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "V4L2 node failure at line %d: %s\n", __LINE__, #c); \
    assert(c); } } while (0)
static bool warn_once(bool condition) { if (condition) warnings++; return condition; }
#define WARN_ON_ONCE(c) warn_once(c)
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
static void spin_acquire(spinlock_t *lock)
{
    if (lock == &pause_hw.lock || lock == &pause_hw.rx_lock)
        assert(irq_disabled && pause_hw.fetch_sem.held);
    assert(!*lock); *lock = 1;
}
#define spin_lock_irqsave(l, f) do { spin_acquire(l); (f) = 0; } while (0)
#define spin_unlock_irqrestore(l, f) do { (void)(f); assert(*(l)); *(l) = 0; } while (0)
static void mod_delayed_work(struct workqueue_struct *w, struct delayed_work *d, int delay)
{ (void)w; assert(!delay && fixture.kick_lock); d->pending = true; kicks++; }
static void cancel_delayed_work_sync(struct delayed_work *d)
{
    assert(node.ioctl_lock.held && !fixture.run_lock.held && !fixture.kick_lock &&
           !fixture.admitted && !access_active);
    joins++;
    if (join_hook) join_hook(&fixture);
    d->pending = false;
}
static bool queue_work(struct workqueue_struct *w, struct work_struct *work)
{
    (void)w; assert(fixture.run_lock.held && fixture.kick_lock && fixture.tx_active);
    if (work->pending) return false;
    work->pending = true; tx_queues++; return true;
}
static bool cancel_work_sync(struct work_struct *work)
{
    bool pending = work->pending;
    assert(!fixture.run_lock.held && !fixture.kick_lock && !access_active);
    tx_joins++;
    if (tx_join_hook) tx_join_hook();
    work->pending = false; return pending;
}
static struct vb2_v4l2_buffer *v4l2_m2m_next_src_buf(struct v4l2_m2m_ctx *m)
{ assert(m == &queues); return source; }
static unsigned v4l2_m2m_num_src_bufs_ready(struct v4l2_m2m_ctx *m)
{ assert(m == &queues); return !!source + !!source_next; }
static void v4l2_m2m_src_buf_remove(struct v4l2_m2m_ctx *m)
{ assert(m == &queues && source); source = source_next; source_next = NULL; }
static struct vb2_v4l2_buffer *v4l2_m2m_dst_buf_remove(struct v4l2_m2m_ctx *m)
{
    struct v4l2_m2m_buffer *b = m->dst_ready;
    if (!b) return NULL;
    m->dst_ready = b->next; b->next = NULL; return &b->vb;
}
static unsigned vb2_plane_size(struct vb2_buffer *b, unsigned plane)
{ assert(b && !plane); return b->size; }
static void vb2_set_plane_payload(struct vb2_buffer *b, unsigned plane, unsigned size)
{ assert(!plane); b->payload = size; }
static void v4l2_m2m_buf_done(struct vb2_v4l2_buffer *b, unsigned state)
{ assert(!b->done); b->done = state; }
static void v4l2_event_queue_fh(struct v4l2_fh *fh, struct v4l2_event *event)
{ assert(fh == &fixture.fh && event->type == V4L2_EVENT_EOS); eos_events++; }
/* 5.15 get_vq classifies all non-output types as capture: it is not validation. */
static struct vb2_queue *v4l2_m2m_get_vq(struct v4l2_m2m_ctx *m, unsigned type)
{ return V4L2_TYPE_IS_OUTPUT(type) ? &m->src : &m->dst; }
static struct vb2_queue *v4l2_m2m_get_src_vq(struct v4l2_m2m_ctx *m) { return &m->src; }
static struct vb2_queue *v4l2_m2m_get_dst_vq(struct v4l2_m2m_ctx *m) { return &m->dst; }
static bool vb2_is_streaming(struct vb2_queue *q) { return q->streaming; }
static bool vb2_is_busy(struct vb2_queue *q) { return QUEUE_COUNT(q) != 0; }
static void vb2_queue_error(struct vb2_queue *q) { q->error = true; }
static void vb2_clear_last_buffer_dequeued(struct vb2_queue *q) { q->last = false; }
static int crystalhd_device_enter(u64 generation, bool exclusive,
                                  struct crystalhd_device_access *a)
{
    assert(generation == node.generation && fixture.run_lock.held &&
           !access_active && (!exclusive || !fixture.admitted));
    enters++;
    if (enter_error) return enter_error;
    access_active = true; access_exclusive = exclusive; a->adp = &adapter; return 0;
}
static void crystalhd_device_exit(struct crystalhd_device_access *a)
{ assert(access_active && a->adp == &adapter); access_active = false; exits++; }
static int crystalhd_status_to_errno(BC_STATUS status) { return status; }
static BC_STATUS crystalhd_session_release_locked(struct crystalhd_cmd *c, void *owner)
{
    assert(access_active && c == &adapter.cmds && owner == &lease);
    releases++;
    if (release_error) return release_error;
    c->session_owner = NULL; c->state = 0; c->decoder_phase = 0;
    hardware_owned = false; return 0;
}
static void crystalhd_v4l2_decoder_reset(struct crystalhd_v4l2_decoder *d)
{ d->phase = CHD_V4L2_OFF; d->count = 0; }
static int crystalhd_v4l2_decoder_stop_locked(struct crystalhd_v4l2_decoder *d,
        struct crystalhd_cmd *c, void *owner)
{
    (void)d; assert(access_active && owner == &lease); stops++;
    if (stop_error) return stop_error;
    c->decoder_phase = CRYSTALHD_DECODER_CHANNEL_CONFIGURED; return 0;
}
static BC_STATUS crystalhd_capture_flush(struct crystalhd_cmd *c, bool discard)
{
    assert(access_active && !discard); flushes++;
    c->state &= ~BC_LINK_CAP_EN; hardware_owned = false; return 0;
}
static int crystalhd_v4l2_decoder_close_locked(struct crystalhd_v4l2_decoder *d,
        struct crystalhd_cmd *c, void *owner)
{
    assert(access_active && !hardware_owned && owner == &lease);
    closes++; c->decoder_phase = 0; crystalhd_v4l2_decoder_reset(d); return 0;
}
static bool crystalhd_v4l2_decoder_drained(struct crystalhd_v4l2_decoder *d)
{ return d->phase == CHD_V4L2_DRAINED; }
static int crystalhd_decoder_resume_h264_locked(struct crystalhd_cmd *cmd, void *owner)
{
    assert(cmd == &adapter.cmds && owner == &lease && access_active && access_exclusive &&
           adapter.tx_lock.held && fixture.run_lock.held && !fixture.admitted &&
           !fixture.tx_active && !fixture.tx_work.pending && !fixture.run_work.pending);
    firmware_resumes++; return resume_error;
}
static int crystalhd_v4l2_decoder_resume(struct crystalhd_v4l2_decoder *d)
{
    assert(fixture.run_lock.held && !access_active && !adapter.tx_lock.held &&
           d->phase == CHD_V4L2_DRAINED);
    controller_resumes++;
    if (controller_resume_error) return controller_resume_error;
    d->phase = CHD_V4L2_RUNNING; return 0;
}
static void chd_return_active(struct crystalhd_v4l2_file *f)
{ assert(f == &fixture && !hardware_owned); returns++; }
static int crystalhd_v4l2_ctx_acquire(struct crystalhd_v4l2_ctx *l)
{
    assert(l == &lease && node.ioctl_lock.held); acquires++;
    if (acquire_error) return acquire_error;
    assert(!adapter.cmds.session_owner); adapter.cmds.session_owner = l; return 0;
}
static int v4l2_m2m_ioctl_reqbufs(struct file *file, void *fh,
                                struct v4l2_requestbuffers *req)
{
    struct vb2_queue *q = v4l2_m2m_get_vq(&queues, req->type);
    assert(file == &handle && fh == &fixture.fh && node.ioctl_lock.held &&
           !hardware_owned && !q->streaming);
    req_calls++; QUEUE_COUNT(q) = req->count; return 0;
}
static int v4l2_m2m_ioctl_streamoff(struct file *file, void *fh, enum v4l2_buf_type type)
{
    assert(file == &handle && fh == &fixture.fh && node.ioctl_lock.held && !hardware_owned);
    streamoff_calls++; v4l2_m2m_get_vq(&queues, type)->streaming = false; return 0;
}
static int v4l2_m2m_ioctl_streamon(struct file *file, void *fh, enum v4l2_buf_type type)
{
    assert(file == &handle && fh == &fixture.fh && node.ioctl_lock.held);
    streamon_calls++;
    if (streamon_error) return streamon_error;
    v4l2_m2m_get_vq(&queues, type)->streaming = true; return 0;
}
static int v4l2_m2m_ioctl_qbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{ (void)b; assert(file == &handle && fh == &fixture.fh); qbuf_calls++; return 0; }
static void v4l2_m2m_ctx_release(struct v4l2_m2m_ctx *m)
{
    assert(m == &queues && !hardware_owned && !fixture.admitted &&
           !fixture.run_work.pending && !fixture.tx_work.pending &&
           !fixture.tx_active && !access_active && !fixture.run_lock.held);
    ctx_destroys++;
    if (constructor_mode) {
        assert(m->src.initialized && m->dst.initialized);
        m->src.initialized = m->dst.initialized = false;
        ctor_queue_releases += 2;
    }
}
static void kfree(void *p)
{
    if (constructor_mode) {
        if (p == &fixture) {
            assert(ctor_file_live && !ctor_fh_live && !access_active);
            ctor_file_live = false;
        } else {
            assert(p == &node && ctor_node_live && !ctor_ctrl_live &&
                   !ctor_m2m_live && !ctor_wq_live && !ctor_dev_refs && !ctor_parent_refs);
            ctor_node_live = false;
        }
        private_frees++; return;
    }
    assert(p == &fixture && ctx_destroys && !access_active); private_frees++;
}

/* Constructor dependencies model framework contracts, not driver decisions. */
#define VB2_MMAP 1U
#define V4L2_BUF_FLAG_TIMESTAMP_COPY 0x100U
#define V4L2_FMT_FLAG_COMPRESSED 1U
#define V4L2_CID_MIN_BUFFERS_FOR_CAPTURE 77U
#define V4L2_CTRL_FLAG_READ_ONLY 1U
#define VFL_DIR_M2M 2U
#define VFL_TYPE_VIDEO 3U
#define V4L2_CAP_VIDEO_M2M 0x10U
#define V4L2_CAP_STREAMING 0x20U
#define WQ_MEM_RECLAIM 1U
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define INIT_WORK(w, fn) do { (w)->pending = false; (w)->function = (fn); } while (0)
#define INIT_DELAYED_WORK(w, fn) INIT_WORK(w, fn)
/* Constructor captures the production binding; execution remains in worker tests. */
static void chd_run(struct work_struct *w) { (void)w; assert(!"constructor ran RX work"); }
static const unsigned chd_queue_ops, vb2_dma_sg_memops, chd_m2m_ops, chd_fops, chd_ioctl_ops;
static void mutex_init(struct mutex *m) { m->held = false; }
static void spin_lock_init(spinlock_t *l) { *l = 0; }
static void *kzalloc(size_t bytes, unsigned flags)
{
    assert(constructor_mode && flags == GFP_KERNEL); ctor_allocs++;
    if (ctor_fail == CTOR_ALLOC) return NULL;
    if (bytes == sizeof(fixture)) {
        assert(!ctor_file_live); ctor_file_live = true;
        memset(&fixture, 0, sizeof(fixture)); return &fixture;
    }
    assert(bytes == sizeof(node) && !ctor_node_live); ctor_node_live = true;
    memset(&node, 0, sizeof(node)); return &node;
}
static void *video_drvdata(struct file *file) { assert(file == &handle); return &node; }
static void crystalhd_v4l2_decoder_init(struct crystalhd_v4l2_decoder *d)
{ memset(d, 0, sizeof(*d)); }
static void v4l2_fh_init(struct v4l2_fh *fh, struct video_device *v)
{ assert(fh == &fixture.fh && v == &node.video && !ctor_fh_live); ctor_fh_live = true; }
static void v4l2_fh_exit(struct v4l2_fh *fh)
{ assert(fh == &fixture.fh && ctor_fh_live); ctor_fh_live = false; ctor_fh_exits++; }
static int vb2_queue_init(struct vb2_queue *q)
{
    ctor_queue_calls++;
    assert(!q->initialized && q->lock == &node.ioctl_lock && q->drv_priv == &fixture);
    if ((ctor_fail == CTOR_SRC_QUEUE && q == &queues.src) ||
        (ctor_fail == CTOR_DST_QUEUE && q == &queues.dst)) return -EINVAL;
    q->initialized = true; return 0;
}
static void vb2_queue_release(struct vb2_queue *q)
{ assert(q->initialized); q->initialized = false; ctor_queue_releases++; }
static struct v4l2_m2m_ctx *v4l2_m2m_ctx_init(struct v4l2_m2m_dev *m,
        void *private, int (*init)(void *, struct vb2_queue *, struct vb2_queue *))
{
    assert(m == &ctor_m2m && private == &fixture && ctor_fh_live);
    if (ctor_fail == CTOR_CTX_INIT) return ERR_PTR(-ENOMEM);
    int rc = init(private, &queues.src, &queues.dst);
    return rc ? ERR_PTR(rc) : &queues;
}
static int crystalhd_v4l2_ctx_create(struct crystalhd_v4l2 *p, struct crystalhd_v4l2_ctx **out)
{
    assert(p == &ctor_parent && queues.src.initialized && queues.dst.initialized);
    ctor_ctx_creates++;
    if (ctor_ctx_error) return ctor_ctx_error;
    *out = &lease; return 0;
}
static void crystalhd_v4l2_ctx_bind(struct crystalhd_v4l2_ctx *l, void *p, void (*cleanup)(void *))
{ assert(l == &lease && p == &fixture); ctor_binds++; ctor_cleanup = cleanup; }
static void crystalhd_v4l2_ctx_close(struct crystalhd_v4l2_ctx *l)
{
    assert(l == &lease && !handle.private_data && !ctor_fh_live &&
           !node.ioctl_lock.held && !fixture.run_lock.held &&
           !fixture.tx_work.pending && !fixture.run_work.pending);
    ctor_close_pending = true; /* Core retirement may occur after release returns. */
}
static struct device *get_device(struct device *d)
{ assert(d == &ctor_device); ctor_dev_refs++; return d; }
static void put_device(struct device *d)
{ assert(d == &ctor_device && ctor_dev_refs == 1); ctor_dev_refs--; }
static void v4l2_ctrl_handler_init(struct v4l2_ctrl_handler *h, unsigned count)
{ assert(count == 1 && !ctor_ctrl_live); ctor_ctrl_live = true; h->error = ctor_fail == CTOR_CTRL_INIT ? -ENOMEM : 0; }
static struct v4l2_ctrl *v4l2_ctrl_new_std(struct v4l2_ctrl_handler *h,
        void *ops, unsigned id, int min, int max, int step, int def)
{
    assert(!ops && id == V4L2_CID_MIN_BUFFERS_FOR_CAPTURE && min == 2 && max == 2 && step == 1 && def == 2);
    if (ctor_fail == CTOR_CTRL_NEW) h->error = -EINVAL;
    return h->error ? NULL : &ctor_minimum;
}
static void v4l2_ctrl_handler_free(struct v4l2_ctrl_handler *h)
{ assert(h == &node.ctrls && ctor_ctrl_live); ctor_ctrl_live = false; }
static struct workqueue_struct *alloc_ordered_workqueue(const char *name, unsigned flags)
{
    unsigned i = ctor_wq_calls++;
    assert(i < 2 && flags == WQ_MEM_RECLAIM && !strcmp(name, i ? "crystalhd-input" : "crystalhd-decode"));
    if (ctor_fail == (i ? CTOR_TX_WQ : CTOR_RX_WQ)) return NULL;
    assert(!ctor_workqueue_live[i]); ctor_workqueue_live[i] = true;
    ctor_wq_live++; return &ctor_workqueues[i];
}
static void destroy_workqueue(struct workqueue_struct *w)
{
    unsigned i = w == &ctor_workqueues[0] ? 0 : 1;
    assert(w == &ctor_workqueues[i] && ctor_workqueue_live[i] && ctor_wq_live);
    ctor_workqueue_live[i] = false; ctor_wq_live--;
}
static struct v4l2_m2m_dev *v4l2_m2m_init(const void *ops)
{
    assert(ops == &chd_m2m_ops);
    if (ctor_fail == CTOR_M2M) return ERR_PTR(-ENODEV);
    ctor_m2m_live = true; return &ctor_m2m;
}
static void v4l2_m2m_release(struct v4l2_m2m_dev *m)
{ assert(m == &ctor_m2m && ctor_m2m_live && !ctor_wq_live); ctor_m2m_live = false; }
static void crystalhd_v4l2_parent_get(struct crystalhd_v4l2 *p)
{ assert(p == &ctor_parent); ctor_parent_refs++; }
static void crystalhd_v4l2_parent_put(struct crystalhd_v4l2 *p)
{ assert(p == &ctor_parent && ctor_parent_refs == 1); ctor_parent_refs--; }
static void strscpy(char *dst, const char *src, size_t size)
{ assert(strlen(src) < size); strcpy(dst, src); }
static void video_set_drvdata(struct video_device *v, void *p) { v->drvdata = p; }
static int video_register_device(struct video_device *v, unsigned type, int number)
{
    assert(v == &node.video && type == VFL_TYPE_VIDEO && number == -1 &&
           ctor_parent_refs == 1 && v->drvdata == &node);
    if (ctor_fail == CTOR_VIDEO) return -ENFILE; /* No release callback on failure. */
    ctor_video_live = true; return 0;
}
static void video_unregister_device(struct video_device *v)
{
    assert(v == &node.video && ctor_video_live);
    ctor_video_live = false; ctor_video_releases++; v->release(v);
}
void crystalhd_v4l2_node_destroy(struct crystalhd_v4l2_node *node);
static int crystalhd_v4l2_decoder_complete(struct crystalhd_v4l2_decoder *d,
        struct crystalhd_rx_metadata *metadata, u64 epoch, u64 *timestamp)
{
    (void)metadata; (void)epoch; *timestamp = 0;
    if (metadata->picture_flags & CRYSTALHD_PICTURE_FLAG_DECODE_ERROR) {
        assert(fixture.run_lock.held);
        if (decode_error_result == -EILSEQ) { d->count--; *timestamp = UINT64_C(0x1234567890); }
        completed++; return decode_error_result;
    }
    assert(fixture.run_lock.held && !fixture.tx_active && fixture.drain_tx_done &&
           d->phase == CHD_V4L2_DRAINING);
    if (d->count) return -EPROTO;
    completed++; d->phase = CHD_V4L2_DRAINED; return 1;
}
static int crystalhd_v4l2_decoder_reserve(struct crystalhd_v4l2_decoder *d,
        struct crystalhd_cmd *cmd, void *owner, u64 timestamp, u64 *pts)
{
    assert(fixture.run_lock.held && access_active && !access_exclusive &&
           cmd == &adapter.cmds && owner == &lease);
    d->count++; *pts = timestamp; return 0;
}
static int crystalhd_v4l2_decoder_begin_drain(struct crystalhd_v4l2_decoder *d)
{ assert(fixture.run_lock.held); d->phase = CHD_V4L2_DRAINING; return 0; }
static void crystalhd_v4l2_decoder_submitted(struct crystalhd_v4l2_decoder *d, int rc)
{ assert(fixture.run_lock.held && !access_active && !adapter.tx_lock.held); if (rc) d->phase = CHD_V4L2_FAILED; }
static int transport(void)
{
    assert(access_active && !access_exclusive && adapter.tx_lock.held &&
           !fixture.run_lock.held && fixture.tx_active);
    transports++; if (transport_hook) transport_hook(); return transport_error;
}
static int crystalhd_decoder_submit_h264_eos(struct crystalhd_cmd *cmd, void *owner, unsigned timeout)
{ assert(cmd == &adapter.cmds && owner == &lease && timeout == CHD_TX_TIMEOUT_MS); return transport(); }
static int crystalhd_v4l2_output_submit(struct crystalhd_cmd *cmd, void *owner,
        struct crystalhd_v4l2_output_buffer *buffer, u64 pts, unsigned timeout)
{ assert(cmd == &adapter.cmds && owner == &lease && buffer == &tx_buffer &&
         pts == tx_buffer.m2m.vb.vb2_buf.timestamp && timeout == CHD_TX_TIMEOUT_MS); return transport(); }
static bool crystalhd_v4l2_output_owned(struct crystalhd_v4l2_output_buffer *buffer)
{ return buffer->owned; }
static BC_STATUS crystalhd_rx_try_dequeue(struct crystalhd_cmd *cmd,
                                         struct crystalhd_rx_completion *result)
{
    assert(cmd == &adapter.cmds && fixture.run_lock.held);
    if (!eos_receive && !error_receive) return BC_STS_NO_DATA;
    eos_receive = false;
    memset(result, 0, sizeof(*result)); result->cookie = &last_capture;
    result->metadata.valid = result->metadata.eos_trailer = true;
    result->metadata.picture_number = 0xffffffffU;
    if (error_receive) {
        error_receive = false;
        result->flags = COMP_FLAG_DATA_VALID;
        result->metadata.eos_trailer = false;
        result->metadata.picture_number = 22;
        result->metadata.picture_flags = CRYSTALHD_PICTURE_FLAG_DECODE_ERROR;
    }
    return BC_STS_SUCCESS;
}
static int chd_format(struct crystalhd_v4l2_file *f, struct crystalhd_cmd *cmd,
                      struct crystalhd_rx_completion *result)
{ (void)f; (void)cmd; (void)result; assert(!"Unexpected format branch"); return -EIO; }
static BC_STATUS crystalhd_v4l2_capture_complete(struct crystalhd_v4l2_capture_buffer *b,
        struct crystalhd_rx_completion *result, struct crystalhd_rx_image *image)
{ (void)image; assert(b == &last_capture && result->cookie == b); b->owned = false;
  return result->metadata.picture_flags & CRYSTALHD_PICTURE_FLAG_DECODE_ERROR ?
      BC_STS_IO_ERROR : BC_STS_NO_DATA; }
static bool crystalhd_v4l2_capture_owned(struct crystalhd_v4l2_capture_buffer *b)
{ return b->owned; }
static int crystalhd_v4l2_capture_prepare(struct crystalhd_v4l2_capture_buffer *b, u32 width, u32 height)
{
    assert(fixture.run_lock.held && !b->owned);
    b->width = width; b->height = height; return 0;
}
static void crystalhd_rx_buffer_release(struct crystalhd_adp *, struct crystalhd_rx_buffer *);
static unsigned long jiffies;
static unsigned long msecs_to_jiffies(unsigned ms) { return (ms + 9U) / 10U; }
#include "node-production.h"

static void down(struct semaphore *s) { assert(!s->held); s->held = true; }
static void up(struct semaphore *s) { assert(s->held); s->held = false; }
static void disable_irq(unsigned irq)
{
    assert(irq == pause_pci.irq && pause_hw.fetch_sem.held && !irq_disabled);
    if (irq_drain_hook) irq_drain_hook();
    irq_disabled = true; irq_disables++;
}
static void enable_irq(unsigned irq)
{ assert(irq == pause_pci.irq && irq_disabled); irq_disabled = false; irq_enables++; }
static unsigned crystalhd_dioq_count(struct crystalhd_dioq *q)
{ assert(q && irq_disabled && pause_hw.fetch_sem.held); return q->count; }
#include "node-idle.h"
static BC_STATUS crystalhd_hw_stop_capture_locked(struct crystalhd_hw *hw, bool unmap)
{
    assert(hw == &pause_hw && hw->fetch_sem.held && unmap && !irq_disabled);
    pause_stops++;
    if (pause_status == BC_STS_SUCCESS) {
        hw->rx_cancel_epoch++;
        hardware_owned = false;
    }
    return pause_status;
}
static void crystalhd_rx_buffer_release(struct crystalhd_adp *adp,
                                        struct crystalhd_rx_buffer *buffer)
{
    assert(adp == &adapter && buffer && buffer->cookie && detached_owned &&
           !pause_hw.fetch_sem.held && !irq_disabled);
    detached_owned = false; pause_releases++;
}
#include "node-pause.h"

static void reset(bool running)
{
    assert(!access_active);
    for (unsigned i = 0; i < CHD_DISCOVERY_BUFFERS; i++) {
        assert(!fixture.discovery[i].owned);
        chd_discovery_free(&fixture.discovery[i]);
    }
    assert(!live_arrays && !live_blocks && !live_tables && !live_maps && !live_vmaps);
    scenarios++;
    memset(&fixture, 0, sizeof(fixture)); memset(&node, 0, sizeof(node));
    memset(&queues, 0, sizeof(queues)); memset(&adapter, 0, sizeof(adapter));
    fixture.node = &node; fixture.lease = &lease; fixture.fh.m2m_ctx = &queues;
    node.generation = 55; node.ioctl_lock.held = true; handle.private_data = &fixture.fh;
    warnings = kicks = joins = enters = exits = releases = stops = flushes = closes = 0;
    req_calls = streamoff_calls = streamon_calls = qbuf_calls = acquires = 0;
    returns = ctx_destroys = dma_frees = private_frees = 0;
    enter_error = stop_error = release_error = acquire_error = streamon_error = 0;
    fail_edge = NO_FAILURE; fail_at = edge_calls = array_calls = block_calls = 0;
    map_calls = unmap_calls = block_frees = max_segments = 0;
    join_hook = NULL;
    tx_queues = tx_joins = transports = completed = eos_events = 0;
    transport_error = 0; transport_hook = NULL; tx_join_hook = NULL; source = source_next = NULL;
    resume_error = controller_resume_error = 0; firmware_resumes = controller_resumes = 0;
    eos_receive = false;
    error_receive = false; decode_error_result = -EILSEQ;
    memset(&tx_buffer, 0, sizeof(tx_buffer)); memset(&last_capture, 0, sizeof(last_capture));
    hardware_owned = running;
    if (running) {
        fixture.admitted = fixture.output_streaming = fixture.capture_streaming = true;
        fixture.session_acquired = fixture.capture_started = fixture.format_known = true;
        fixture.decoder.phase = CHD_V4L2_RUNNING; fixture.decoder.count = 3;
        fixture.input_seen = true;
        queues.src.streaming = queues.dst.streaming = true;
        QUEUE_COUNT(&queues.src) = QUEUE_COUNT(&queues.dst) = 4;
        adapter.cmds.session_owner = &lease;
        adapter.cmds.decoder_phase = CRYSTALHD_DECODER_CHANNEL_STARTED;
        adapter.cmds.state = BC_LINK_CAP_EN;
    }
}

static void invalid_and_busy(void)
{
    struct v4l2_requestbuffers req = { .type = INVALID_CAPTURE_TYPE,
        .memory = V4L2_MEMORY_MMAP, .count = 0 };
    reset(false);
    CHECK(chd_reqbufs(&handle, &fixture.fh, &req) == -EINVAL);
    CHECK(!enters && !joins && !req_calls);
    CHECK(chd_streamon(&handle, &fixture.fh, INVALID_CAPTURE_TYPE) == -EINVAL);
    CHECK(chd_streamoff(&handle, &fixture.fh, INVALID_CAPTURE_TYPE) == -EINVAL);
    CHECK(!enters && !joins && !acquires && !streamon_calls && !streamoff_calls);
    reset(true); req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    CHECK(chd_reqbufs(&handle, &fixture.fh, &req) == -EBUSY);
    CHECK(!enters && !joins && !req_calls && hardware_owned && fixture.admitted);
    CHECK(!chd_streamon(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_OUTPUT));
    CHECK(!acquires && !streamon_calls);
    reset(false); req.memory = 2;
    CHECK(chd_reqbufs(&handle, &fixture.fh, &req) == -EINVAL);
    CHECK(!enters && !joins && !req_calls);
    CHECK(!chd_streamoff(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_CAPTURE));
    CHECK(!joins && !enters && !streamoff_calls);
}

static void format_reallocation(void)
{
    struct v4l2_requestbuffers req = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP, .count = 0 };
    reset(true); fixture.format_pending = true; fixture.capture_started = false;
    hardware_owned = false; join_hook = chd_kick;
    fixture.tx_active = true; fixture.tx_work.pending = true;
    CHECK(!chd_streamoff(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_CAPTURE));
    CHECK(joins == 1 && !kicks && !enters && streamoff_calls == 1);
    CHECK(!tx_joins && fixture.tx_active && fixture.tx_work.pending);
    CHECK(fixture.format_known && fixture.format_pending && fixture.output_streaming &&
          !fixture.capture_streaming && fixture.decoder.count == 3);
    CHECK(!chd_reqbufs(&handle, &fixture.fh, &req));
    req.count = 6; CHECK(!chd_reqbufs(&handle, &fixture.fh, &req));
    CHECK(req_calls == 2 && !enters && !releases && !stops && fixture.decoder.count == 3);
    CHECK(!tx_joins && fixture.tx_active && fixture.tx_work.pending);
    queues.dst.last = true;
    CHECK(!chd_streamon(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_CAPTURE));
    CHECK(fixture.capture_streaming && fixture.admitted && !fixture.format_pending &&
          !queues.dst.last && kicks == 1 && !acquires);
}

static void stop_failures(void)
{
    for (unsigned i = 0; i < 3; i++) {
        reset(true);
        if (i == 0) release_error = -EIO;
        if (i == 1) enter_error = -EAGAIN;
        if (i == 2) stop_error = -EIO;
        CHECK(chd_streamoff(&handle, &fixture.fh, i == 2 ?
              V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_OUTPUT) < 0);
        CHECK(!streamoff_calls && !req_calls && !returns && hardware_owned &&
              QUEUE_COUNT(&queues.src) == 4 && QUEUE_COUNT(&queues.dst) == 4);
        CHECK(fixture.fatal && !fixture.admitted && queues.src.error && queues.dst.error);
        CHECK(queues.src.streaming && queues.dst.streaming && fixture.session_acquired);
    }
    reset(true); queues.dst.streaming = false; stop_error = -EIO;
    {
        struct v4l2_requestbuffers req = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP, .count = 0 };
        CHECK(chd_reqbufs(&handle, &fixture.fh, &req) == -EIO);
        CHECK(!req_calls && QUEUE_COUNT(&queues.dst) == 4 && hardware_owned && fixture.fatal);
    }
}

static void restart_and_drain(void)
{
    struct v4l2_decoder_cmd cmd = { .cmd = V4L2_DEC_CMD_START };
    struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
    reset(true);
    CHECK(!chd_streamoff(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_OUTPUT));
    CHECK(releases == 1 && !stops && !hardware_owned && !fixture.session_acquired &&
          !fixture.output_streaming && fixture.decoder.phase == CHD_V4L2_OFF);
    CHECK(!chd_streamon(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_OUTPUT));
    CHECK(acquires == 1 && array_calls == CHD_DISCOVERY_BUFFERS &&
          fixture.session_acquired && fixture.admitted);
    reset(true); fixture.decoder.phase = CHD_V4L2_DRAINED; fixture.drain_requested = true;
    queues.dst.last = true;
    CHECK(!chd_decoder_cmd(&handle, &fixture.fh, &cmd));
    CHECK(!releases && !stops && !flushes && !closes &&
          !streamoff_calls && queues.src.streaming && queues.dst.streaming &&
          QUEUE_COUNT(&queues.src) == 4 && QUEUE_COUNT(&queues.dst) == 4 &&
          fixture.session_acquired && fixture.admitted && !queues.dst.last &&
          firmware_resumes == 1 && controller_resumes == 1 && hardware_owned &&
          fixture.decoder.phase == CHD_V4L2_RUNNING && fixture.format_known &&
          fixture.capture_started && !fixture.drain_requested && !fixture.drain_tx_done);
    reset(true);
    CHECK(!chd_decoder_cmd(&handle, &fixture.fh, &cmd));
    CHECK(!enters && !joins && !tx_joins && !firmware_resumes && !controller_resumes &&
          !kicks && fixture.admitted);
    reset(true); fixture.decoder.phase = CHD_V4L2_DRAINING;
    CHECK(chd_decoder_cmd(&handle, &fixture.fh, &cmd) == -EBUSY);
    CHECK(!joins && !enters && !firmware_resumes && !controller_resumes);
    for (unsigned fail = 0; fail < 3; fail++) {
        reset(true); fixture.decoder.phase = CHD_V4L2_DRAINED;
        fixture.drain_requested = fixture.drain_tx_done = queues.dst.last = true;
        if (fail == 0) enter_error = -ENODEV;
        if (fail == 1) resume_error = -EIO;
        if (fail == 2) controller_resume_error = -EPIPE;
        CHECK(chd_decoder_cmd(&handle, &fixture.fh, &cmd) < 0);
        CHECK(!fixture.admitted && !fixture.tx_active && !kicks && queues.dst.last &&
              fixture.drain_requested && fixture.drain_tx_done && hardware_owned &&
              fixture.decoder.phase == CHD_V4L2_DRAINED && !stops && !closes &&
              !flushes && !releases && !streamoff_calls);
        CHECK(firmware_resumes == (fail != 0) && controller_resumes == (fail == 2));
    }
    reset(true); cmd.cmd = V4L2_DEC_CMD_STOP;
    CHECK(!chd_decoder_cmd(&handle, &fixture.fh, &cmd));
    CHECK(fixture.drain_requested && !chd_qbuf(&handle, &fixture.fh, &buf));
    CHECK(qbuf_calls == 1 && !enters && !joins && !releases);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    CHECK(!chd_qbuf(&handle, &fixture.fh, &buf) && qbuf_calls == 2);
    cmd.flags = 1;
    CHECK(!chd_try_decoder_cmd(&handle, &fixture.fh, &cmd) && !cmd.flags);
}

static void cleanup_and_scheduler(void)
{
    reset(false); fixture.admitted = true; fixture.run_work.pending = true;
    join_hook = chd_kick;
    chd_join(&fixture);
    CHECK(!fixture.admitted && !fixture.run_work.pending && !kicks && joins == 1);
    fixture.closing = true;
    chd_admit(&fixture, true); chd_kick(&fixture);
    CHECK(!fixture.admitted && !kicks);
    CHECK(!chd_discovery_alloc(&fixture));
    mutex_unlock(&node.ioctl_lock); /* Deferred lease cleanup follows file release. */
    chd_file_destroy(&fixture);
    CHECK(ctx_destroys == 1 && dma_frees == CHD_DISCOVERY_BUFFERS && private_frees == 1 && !warnings &&
          !live_arrays && !live_blocks && !live_tables && !live_maps && !live_vmaps);
    CHECK(!chd_job_ready(NULL) && !chd_job_ready(&fixture));
    chd_device_run(&fixture);
    CHECK(warnings == 1 && !kicks && !enters);
}

static void streamon_allocation_failures(void)
{
    reset(false);
    CHECK(chd_streamon(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_OUTPUT) == -EINVAL);
    CHECK(!acquires && !enters && !array_calls && !fixture.session_acquired);
    reset(false); QUEUE_COUNT(&queues.src) = 4; acquire_error = -EBUSY;
    CHECK(chd_streamon(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_OUTPUT) == -EBUSY);
    CHECK(acquires == 1 && !array_calls && !streamon_calls && !fixture.session_acquired);
    reset(false); QUEUE_COUNT(&queues.src) = 4; fail_edge = ARRAY_EDGE; fail_at = 1;
    CHECK(chd_streamon(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_OUTPUT) == -ENOMEM);
    CHECK(!fixture.session_acquired && !streamon_calls && !fixture.admitted &&
          releases == 1 && !adapter.cmds.session_owner);
    fail_edge = NO_FAILURE;
    CHECK(!chd_streamon(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_OUTPUT));
    CHECK(acquires == 2 && array_calls == 1 + CHD_DISCOVERY_BUFFERS &&
          streamon_calls == 1 && fixture.admitted);
    reset(false); QUEUE_COUNT(&queues.src) = 4; streamon_error = -EINVAL;
    CHECK(chd_streamon(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_OUTPUT) == -EINVAL);
    CHECK(releases == 1 && !fixture.session_acquired && !fixture.fatal &&
          !live_arrays && !live_blocks && !live_maps && !live_vmaps);
    streamon_error = 0;
    CHECK(!chd_streamon(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_OUTPUT));
    CHECK(acquires == 2 && fixture.session_acquired);
    reset(false); QUEUE_COUNT(&queues.src) = 4;
    streamon_error = -EINVAL; release_error = -EIO;
    CHECK(chd_streamon(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_OUTPUT) == -EIO);
    CHECK(releases == 1 && fixture.session_acquired && fixture.fatal &&
          adapter.cmds.session_owner == &lease && live_arrays && live_maps && live_vmaps);
    reset(true); hardware_owned = false; queues.dst.streaming = false;
    fixture.capture_streaming = false; streamon_error = -EINVAL;
    CHECK(chd_streamon(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_CAPTURE) == -EINVAL);
    CHECK(!releases && !acquires && fixture.session_acquired && fixture.output_streaming &&
          adapter.cmds.session_owner == &lease);
}

static void allocation_failure_cases(void)
{
    for (enum allocation_edge edge = ARRAY_EDGE; edge <= NENTS_EDGE; edge++) {
        unsigned attempts = edge == PAGE_EDGE ? 547 * CHD_DISCOVERY_BUFFERS : CHD_DISCOVERY_BUFFERS;

        for (unsigned nth = 1; nth <= attempts; nth++) {
            struct crystalhd_v4l2_discovery empty = {0};
            unsigned failed_slot = edge == PAGE_EDGE ? (nth - 1) / 547 : nth - 1;
            void *preserved;
            unsigned preserved_calls;
            int expected = edge == MAP_EDGE ? -EIO : edge == NENTS_EDGE ? -ENOSPC : -ENOMEM;

            reset(false); fail_edge = edge; fail_at = nth;
            CHECK(chd_discovery_alloc(&fixture) == expected);
            /* ARRAY failure may retain only dev; no allocation is published. */
            if (edge == ARRAY_EDGE) fixture.discovery[failed_slot].dev = NULL;
            CHECK(!memcmp(&fixture.discovery[failed_slot], &empty, sizeof(empty)));
            CHECK(live_arrays == failed_slot && live_tables == failed_slot &&
                  live_maps == failed_slot && live_vmaps == failed_slot &&
                  live_blocks == failed_slot * 547 && !warnings);
            preserved = fixture.discovery[0].cpu;
            preserved_calls = array_calls;
            fail_edge = NO_FAILURE;
            CHECK(!chd_discovery_alloc(&fixture));
            CHECK(array_calls == preserved_calls + CHD_DISCOVERY_BUFFERS - failed_slot &&
                  (failed_slot == 0 || fixture.discovery[0].cpu == preserved));
            CHECK(live_arrays == CHD_DISCOVERY_BUFFERS &&
                  live_blocks == 547 * CHD_DISCOVERY_BUFFERS &&
                  live_tables == CHD_DISCOVERY_BUFFERS && live_maps == CHD_DISCOVERY_BUFFERS &&
                  live_vmaps == CHD_DISCOVERY_BUFFERS && max_segments == 547);
            for (unsigned i = 0; i < CHD_DISCOVERY_BUFFERS; i++) {
                struct crystalhd_v4l2_discovery *d = &fixture.discovery[i];
                CHECK(d->num_pages == 1094 && d->sgt.nents == 547 &&
                      d->rx.dma_nents == 547 && d->rx.cookie == d &&
                      d->rx.ops == &chd_discovery_ops && d->mapped && !d->owned);
                chd_discovery_free(d);
                CHECK(!memcmp(d, &empty, sizeof(empty)));
                chd_discovery_free(d); /* Empty cleanup remains idempotent. */
            }
            CHECK(!live_arrays && !live_blocks && !live_tables && !live_maps &&
                  !live_vmaps && !warnings &&
                  block_frees == block_calls - (edge == PAGE_EDGE) &&
                  unmap_calls == map_calls - (edge == MAP_EDGE));
        }
    }
}

static void prepare_idle_snapshot(void)
{
    memset(&pause_hw, 0, sizeof(pause_hw));
    memset(pause_queues, 0, sizeof(pause_queues));
    memset(pause_packets, 0, sizeof(pause_packets));
    for (unsigned i = 0; i + 1 < BC_RX_LIST_CNT; i++)
        pause_packets[i].next = &pause_packets[i + 1];
    pause_hw.rx_actq = &pause_queues[0]; pause_hw.rx_rdyq = &pause_queues[1];
    pause_hw.rx_freeq = &pause_queues[2]; pause_hw.rx_pkt_pool_head = pause_packets;
    pause_hw.rx_cancel_epoch = 23; pause_hw.RxCaptureState = 1; pause_hw.PicQSts = 0x5a;
    pause_pci.device = BC_PCI_DEVID_FLEA; pause_pci.irq = 45;
    adapter.pdev = &pause_pci; adapter.cmds.hw_ctx = &pause_hw;
    adapter.cmds.adp = &adapter; adapter.cmds.state = BC_LINK_CAP_EN;
    irq_disabled = false; irq_disables = irq_enables = pause_stops = pause_releases = 0;
    irq_drain_hook = NULL; pause_status = BC_STS_SUCCESS;
    detached_owned = true;
}

static void format_pause_cases(void)
{
    for (unsigned failure = 0; failure < 7; failure++) {
        struct crystalhd_rx_buffer buffer = { .cookie = &fixture };
        struct crystalhd_rx_completion result = {
            .buffer = &buffer, .cookie = &fixture,
            .flags = COMP_FLAG_FMT_CHANGE, .capture_epoch = 23,
        };
        BC_STATUS expected = BC_STS_SUCCESS;

        reset(false); prepare_idle_snapshot();
        pause_hw.rx_list_sts[0] = rx_sts_waiting; /* Exercise the full-stop fallback. */
        detached_owned = hardware_owned = true;
        if (failure < 3) expected = BC_STS_INV_ARG;
        if (failure == 1) result.cookie = &adapter;
        if (failure == 2) result.flags = 0;
        if (failure == 3) result.capture_epoch--;
        if (failure == 4) adapter.cmds.state = 0;
        if (failure == 3 || failure == 4) expected = BC_STS_IO_USER_ABORT;
        if (failure == 5) expected = pause_status = BC_STS_IO_ERROR;
        CHECK(crystalhd_rx_pause_format(failure == 0 ? NULL : &adapter.cmds,
                                       &result) == expected);
        CHECK(!pause_hw.fetch_sem.held && !warnings);
        if (failure < 3) {
            CHECK(result.buffer == &buffer && result.cookie && detached_owned &&
                  !pause_stops && !pause_releases && hardware_owned);
        } else {
            CHECK(!result.buffer && !result.cookie && !detached_owned && pause_releases == 1);
            CHECK(pause_stops == (failure >= 5) && hardware_owned == (failure != 6));
            CHECK(!!(adapter.cmds.state & BC_LINK_FMT_CHG) == (failure == 6));
        }
    }
}

static void finish_pending_irq(void)
{
    assert(!irq_disabled && pause_hw.fetch_sem.held);
    pause_hw.rx_list_sts[0] = sts_free;
    hardware_owned = false;
}

static void fault_pending_irq(void)
{
    assert(!irq_disabled && pause_hw.fetch_sem.held);
    pause_hw.dma_fault = true;
    hardware_owned = true;
}

static void rx_idle_snapshot_cases(void)
{
    for (unsigned failure = 0; failure < 21; failure++) {
        struct crystalhd_rx_buffer buffer = { .cookie = &fixture };
        struct crystalhd_rx_completion result = { .buffer = &buffer, .cookie = &fixture,
            .flags = COMP_FLAG_FMT_CHANGE, .capture_epoch = 23 };
        bool before_idle = failure == 0 || failure == 18 || failure == 20;
        bool fast = failure == 0 || failure == 19;

        reset(false); prepare_idle_snapshot();
        if (failure == 2) pause_hw.dma_fault = true;
        if (failure == 3) pause_hw.rx_actq = NULL;
        if (failure == 4) pause_hw.rx_rdyq = NULL;
        if (failure == 5) pause_hw.rx_freeq = NULL;
        if (failure == 6 || failure == 19) pause_hw.rx_list_sts[0] = rx_sts_waiting;
        if (failure == 7) pause_hw.rx_list_sts[1] = rx_sts_waiting;
        if (failure >= 8 && failure <= 10) pause_queues[failure - 8].count = 1;
        if (failure == 11) pause_hw.rx_fallback_head = &pause_packets[0];
        if (failure == 12) pause_hw.rx_pkt_pool_head = NULL;
        if (failure == 13) pause_packets[BC_RX_LIST_CNT - 2].next = NULL;
        if (failure == 14) pause_packets[BC_RX_LIST_CNT - 1].next = pause_packets;
        if (failure == 15) pause_packets[BC_RX_LIST_CNT - 1].next = &pause_packets[BC_RX_LIST_CNT];
        if (failure == 16) pause_packets[7].buffer = &buffer;
        if (failure == 17) pause_packets[7].cookie = &fixture;
        if (failure == 18) pause_pci.device = BC_PCI_DEVID_FLEA + 1;
        if (failure == 19) irq_drain_hook = finish_pending_irq;
        if (failure == 20) irq_drain_hook = fault_pending_irq;
        pause_hw.fetch_sem.held = irq_disabled = true;
        CHECK(crystalhd_hw_rx_idle(failure == 1 ? NULL : &pause_hw) == before_idle);
        CHECK(!pause_hw.lock && !pause_hw.rx_lock);
        pause_hw.fetch_sem.held = irq_disabled = false;
        if (failure == 1) continue;
        hardware_owned = !before_idle;
        pause_status = BC_STS_IO_ERROR; /* Unproved idle must not manufacture retirement. */
        CHECK(crystalhd_rx_pause_format(&adapter.cmds, &result) ==
              (fast ? BC_STS_SUCCESS : BC_STS_IO_ERROR));
        CHECK(irq_disables == 1 && irq_enables == 1 && !irq_disabled &&
              !pause_hw.fetch_sem.held && pause_releases == 1 && !detached_owned);
        CHECK(pause_stops == !fast && pause_hw.rx_cancel_epoch == (fast ? 24U : 23U));
        CHECK(!!(adapter.cmds.state & BC_LINK_FMT_CHG) == fast && !result.buffer && !result.cookie);
        CHECK(pause_hw.RxCaptureState == 1 && pause_hw.PicQSts == 0x5a && !warnings);
        if (fast) CHECK(!hardware_owned);
        else if (failure != 18) CHECK(hardware_owned);
    }
}

static void schedule_tx(void)
{
    mutex_lock(&fixture.run_lock);
    chd_schedule_tx(&fixture);
    mutex_unlock(&fixture.run_lock);
}

static void execute_tx(void)
{
    assert(fixture.tx_work.pending);
    fixture.tx_work.pending = false;
    chd_tx_run(&fixture.tx_work);
}

static void eos_during_transport(void)
{
    mutex_lock(&fixture.run_lock);
    fixture.active[0] = &last_capture;
    last_capture.owned = true; eos_receive = true;
    CHECK(!chd_receive(&fixture, &adapter.cmds));
    CHECK(fixture.pending_last == &last_buffer && !fixture.active[0] && !last_capture.owned);
    CHECK(fixture.decoder.phase == CHD_V4L2_DRAINING);
    CHECK(!chd_finish_last(&fixture));
    CHECK(!last_buffer.done && !completed && !eos_events &&
          fixture.decoder.phase == CHD_V4L2_DRAINING);
    mutex_unlock(&fixture.run_lock);
}

static void join_rx_progress(void)
{
    CHECK(fixture.tx_stopping && fixture.tx_active && fixture.admitted &&
          fixture.run_work.pending && !fixture.run_lock.held);
    CHECK(!joins); /* TX is joined before RX is canceled. */
    chd_kick(&fixture); /* Closing must still permit RX completion polling. */
    CHECK(kicks == 2 && fixture.run_work.pending);
    mutex_lock(&fixture.run_lock);
    chd_schedule_tx(&fixture);
    mutex_unlock(&fixture.run_lock);
    CHECK(!tx_queues);
}

static void async_tx_cases(void)
{
    static struct crystalhd_v4l2_capture_buffer active;
    reset(true); source = &tx_buffer.m2m.vb;
    schedule_tx(); CHECK(!tx_queues); /* No RX registration, no TX. */
    fixture.active[0] = &active;
    schedule_tx(); CHECK(tx_queues == 1 && fixture.tx_active);
    schedule_tx(); CHECK(tx_queues == 1); /* Queued counts as active. */
    execute_tx();
    CHECK(transports == 1 && !fixture.tx_active && !source &&
          tx_buffer.m2m.vb.done == VB2_BUF_STATE_DONE && enters == exits);

    reset(true); source = &tx_buffer.m2m.vb; fixture.active[0] = &active;
    schedule_tx(); chd_join(&fixture);
    CHECK(tx_joins == 1 && !fixture.tx_active && !fixture.tx_work.pending &&
          !transports && fixture.decoder.count == 3 && source);

    reset(true); source = &tx_buffer.m2m.vb; fixture.active[0] = &active;
    schedule_tx(); chd_admit(&fixture, false); execute_tx();
    CHECK(!transports && !fixture.tx_active && !kicks && source &&
          fixture.decoder.count == 3);

    reset(true); fixture.tx_active = true; fixture.closing = true;
    fixture.admitted = false; tx_join_hook = join_rx_progress;
    chd_join(&fixture);
    CHECK(tx_joins == 1 && joins == 1 && !fixture.tx_active &&
          !fixture.admitted && !fixture.run_work.pending);

    reset(true); source = &tx_buffer.m2m.vb; fixture.active[0] = &active;
    schedule_tx(); fixture.tx_stopping = true; execute_tx();
    CHECK(!transports && !fixture.tx_active && source && fixture.decoder.count == 3);

    reset(true); source = &tx_buffer.m2m.vb; fixture.active[0] = &active;
    fixture.tx_stopping = true; schedule_tx();
    CHECK(!tx_queues && !fixture.tx_active);
    chd_admit(&fixture, true); schedule_tx();
    CHECK(tx_queues == 1 && fixture.tx_active && !fixture.tx_stopping);
    chd_join(&fixture);

    reset(true); source = &tx_buffer.m2m.vb; fixture.active[0] = &active;
    fixture.decoder.count = CRYSTALHD_V4L2_TIMESTAMPS;
    schedule_tx(); CHECK(!tx_queues);
    source = NULL; fixture.drain_requested = true;
    schedule_tx(); CHECK(tx_queues == 1); /* EOS needs no timestamp slot. */
    chd_join(&fixture);

    reset(true); source = &tx_buffer.m2m.vb; fixture.active[0] = &active;
    tx_buffer.owned = true; transport_error = -ETIMEDOUT;
    schedule_tx(); execute_tx();
    CHECK(fixture.fatal && !fixture.tx_active && source && !source->done &&
          !fixture.admitted && enters == exits);

    for (unsigned fail = 0; fail < 2; fail++) {
        reset(true); fixture.active[0] = &active; fixture.drain_requested = true;
        fixture.decoder.count = 0; /* All admitted pictures already returned. */
        transport_hook = eos_during_transport;
        if (fail) transport_error = -ETIMEDOUT;
        schedule_tx(); execute_tx();
        CHECK(!completed && !last_buffer.done && fixture.pending_last == &last_buffer);
        mutex_lock(&fixture.run_lock);
        CHECK(!chd_finish_last(&fixture));
        mutex_unlock(&fixture.run_lock);
        if (fail) {
            CHECK(fixture.fatal && !fixture.drain_tx_done && !last_buffer.done &&
                  !completed && fixture.decoder.phase == CHD_V4L2_FAILED);
            CHECK(!chd_reset_channel(&fixture, true));
            CHECK(last_buffer.done == VB2_BUF_STATE_ERROR && !fixture.pending_last &&
                  !eos_events && !hardware_owned);
        } else {
            CHECK(fixture.drain_tx_done && !fixture.pending_last && completed == 1 &&
                  eos_events == 1 && last_buffer.done == VB2_BUF_STATE_DONE &&
                  (last_buffer.flags & V4L2_BUF_FLAG_LAST) &&
                  fixture.decoder.phase == CHD_V4L2_DRAINED);
        }
    }
    reset(true); fixture.decoder.phase = CHD_V4L2_DRAINING;
    fixture.pending_last = &last_buffer; fixture.drain_tx_done = true;
    mutex_lock(&fixture.run_lock);
    CHECK(chd_finish_last(&fixture) == -EPROTO);
    mutex_unlock(&fixture.run_lock);
    CHECK(fixture.decoder.count == 3 && fixture.decoder.phase == CHD_V4L2_DRAINING &&
          fixture.pending_last == &last_buffer && !last_buffer.done &&
          !completed && !eos_events && !(last_buffer.flags & V4L2_BUF_FLAG_LAST));
    reset(false); fixture.pending_last = &last_buffer;
    chd_file_destroy(&fixture);
    CHECK(last_buffer.done == VB2_BUF_STATE_ERROR && ctx_destroys == 1 && private_frees == 1);
    reset(false);
}

static struct crystalhd_v4l2_output_buffer post_stop_buffer;

static void stop_during_tx(void)
{
    struct v4l2_decoder_cmd cmd = { .cmd = V4L2_DEC_CMD_STOP };
    struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
    CHECK(!chd_decoder_cmd(&handle, &fixture.fh, &cmd));
    CHECK(fixture.drain_left == 1 && fixture.tx_active);
    CHECK(!chd_qbuf(&handle, &fixture.fh, &buf));
    source_next = &post_stop_buffer.m2m.vb;
    CHECK(chd_decoder_cmd(&handle, &fixture.fh, &cmd) == -EBUSY);
    cmd.cmd = V4L2_DEC_CMD_START;
    CHECK(chd_decoder_cmd(&handle, &fixture.fh, &cmd) == -EBUSY);
}

static void stateful_lifecycle_cases(void)
{
    struct v4l2_decoder_cmd cmd = { .cmd = V4L2_DEC_CMD_STOP };
    struct crystalhd_v4l2_capture_buffer spare = {0};
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .fmt.pix = { .width = 1920, .height = 1080 } };
    reset(false);
    CHECK(!chd_set_fmt(&handle, &fixture.fh, &fmt));
    CHECK(fixture.capture.width == 1920 && fixture.capture.height == 1080 &&
          fixture.capture.bytesperline == 3840 && fixture.capture.sizeimage > 1920 * 1080 * 2);
    QUEUE_COUNT(&queues.dst) = 2; fmt.fmt.pix.width = 640;
    CHECK(chd_set_fmt(&handle, &fixture.fh, &fmt) == -EBUSY);
    CHECK(fixture.output.width == 1920);
    reset(false); fmt.fmt.pix.width = fmt.fmt.pix.height = 0;
    CHECK(!chd_set_fmt(&handle, &fixture.fh, &fmt));
    CHECK(fmt.fmt.pix.width && fmt.fmt.pix.height && fmt.fmt.pix.colorspace);

    for (unsigned mask = 0; mask < 3; mask++) {
        reset(false); fixture.output_streaming = mask & 1; fixture.capture_streaming = mask & 2;
        cmd.cmd = V4L2_DEC_CMD_STOP;
        CHECK(!chd_decoder_cmd(&handle, &fixture.fh, &cmd));
        CHECK(!fixture.drain_requested && !enters && !kicks);
        cmd.cmd = V4L2_DEC_CMD_START;
        CHECK(!chd_decoder_cmd(&handle, &fixture.fh, &cmd));
        CHECK(!enters && !joins && !kicks);
    }
    reset(false); fixture.output_streaming = fixture.capture_streaming = true;
    cmd.cmd = V4L2_DEC_CMD_START;
    CHECK(!chd_decoder_cmd(&handle, &fixture.fh, &cmd));
    CHECK(fixture.decoder.phase == CHD_V4L2_OFF && !enters && !joins && !kicks);

    reset(false); fixture.output_streaming = fixture.capture_streaming = fixture.admitted = true;
    cmd.cmd = V4L2_DEC_CMD_STOP;
    CHECK(!chd_decoder_cmd(&handle, &fixture.fh, &cmd));
    mutex_lock(&fixture.run_lock);
    CHECK(chd_empty_drain(&fixture));
    CHECK(fixture.decoder.phase == CHD_V4L2_OFF && !eos_events);
    queues.dst_ready = &last_capture.m2m;
    source = &tx_buffer.m2m.vb; /* New input belongs to the next START. */
    CHECK(chd_empty_drain(&fixture));
    mutex_unlock(&fixture.run_lock);
    CHECK(fixture.decoder.phase == CHD_V4L2_DRAINED && fixture.empty_drain &&
          last_buffer.done == VB2_BUF_STATE_DONE && (last_buffer.flags & V4L2_BUF_FLAG_LAST) &&
          !enters && !transports && eos_events == 1 && source);
    cmd.cmd = V4L2_DEC_CMD_START;
    CHECK(!chd_decoder_cmd(&handle, &fixture.fh, &cmd));
    CHECK(fixture.decoder.phase == CHD_V4L2_OFF && !fixture.drain_requested &&
          !fixture.empty_drain && !firmware_resumes && !enters && source);

    reset(true); fixture.active[0] = &spare; source = &tx_buffer.m2m.vb;
    memset(&post_stop_buffer, 0, sizeof(post_stop_buffer));
    transport_hook = stop_during_tx;
    schedule_tx(); execute_tx();
    CHECK(!fixture.drain_left && source == &post_stop_buffer.m2m.vb && !source->done);
    fixture.decoder.count = 0; /* Model RX returning the entire pre-STOP prefix. */
    transport_hook = eos_during_transport;
    schedule_tx(); execute_tx();
    CHECK(source == &post_stop_buffer.m2m.vb && !source->done && fixture.drain_tx_done);
    mutex_lock(&fixture.run_lock); CHECK(!chd_finish_last(&fixture)); mutex_unlock(&fixture.run_lock);
    CHECK(fixture.decoder.phase == CHD_V4L2_DRAINED && !source->done);
    CHECK(!chd_decoder_cmd(&handle, &fixture.fh, &cmd));
    CHECK(fixture.decoder.phase == CHD_V4L2_RUNNING && !fixture.drain_requested && source);

    reset(true); fixture.decoder.phase = CHD_V4L2_DRAINED;
    fixture.drain_requested = fixture.drain_tx_done = true; hardware_owned = false;
    CHECK(!chd_streamoff(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_CAPTURE));
    CHECK(!enters && !stops && !flushes && !closes && !releases &&
          fixture.decoder.phase == CHD_V4L2_DRAINED && fixture.capture_started);
    CHECK(!chd_streamon(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_CAPTURE));
    CHECK(firmware_resumes == 1 && controller_resumes == 1 && !stops && !closes &&
          fixture.decoder.phase == CHD_V4L2_RUNNING && fixture.capture_streaming);

    reset(true); fixture.format_pending = true; fixture.capture_started = false;
    fixture.capture.width = 640; fixture.capture.height = 480; fixture.capture.sizeimage = 700000;
    QUEUE_COUNT(&queues.dst) = 2; queues.dst.bufs[0] = &last_buffer.vb2_buf;
    queues.dst.bufs[1] = &spare.m2m.vb.vb2_buf;
    last_buffer.vb2_buf.size = 700000; spare.m2m.vb.vb2_buf.size = 699999;
    queues.dst_ready = &last_capture.m2m;
    CHECK(chd_decoder_cmd(&handle, &fixture.fh, &cmd) == -EINVAL);
    CHECK(fixture.format_pending && !kicks && !enters);
    spare.m2m.vb.vb2_buf.size = 700000; queues.dst.last = true;
    fixture.drain_requested = true; fixture.drain_left = 2;
    CHECK(!chd_decoder_cmd(&handle, &fixture.fh, &cmd));
    CHECK(!fixture.format_pending && !queues.dst.last && last_capture.width == 640 &&
          last_capture.height == 480 && !enters && !joins && fixture.drain_left == 2 &&
          fixture.drain_requested);
    reset(false);
}

static void colorimetry_cases(void)
{
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .fmt.pix = { .width = 640, .height = 480, .priv = V4L2_PIX_FMT_PRIV_MAGIC,
                     .colorspace = V4L2_COLORSPACE_SMPTE170M,
                     .xfer_func = 2, .ycbcr_enc = 2, .quantization = 1 } };
    reset(false);
    CHECK(!chd_set_fmt(&handle, &fixture.fh, &fmt));
    CHECK(fixture.output.colorspace == V4L2_COLORSPACE_SMPTE170M &&
          fixture.output.xfer_func == 2 && fixture.output.ycbcr_enc == 2 &&
          fixture.output.quantization == 1 && !fixture.color_default &&
          fmt.fmt.pix.priv == V4L2_PIX_FMT_PRIV_MAGIC);
    CHECK(fixture.capture.colorspace == fixture.output.colorspace &&
          fixture.capture.xfer_func == 2 && fixture.capture.ycbcr_enc == 2 &&
          fixture.capture.quantization == 1);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
    fmt.fmt.pix.xfer_func = fmt.fmt.pix.ycbcr_enc = fmt.fmt.pix.quantization = 0;
    CHECK(!chd_set_fmt(&handle, &fixture.fh, &fmt));
    CHECK(fmt.fmt.pix.colorspace == V4L2_COLORSPACE_SMPTE170M &&
          fmt.fmt.pix.xfer_func == 2 && fmt.fmt.pix.ycbcr_enc == 2 && fmt.fmt.pix.quantization == 1);
    fixture.output.width = 1920; fixture.output.height = 1080;
    chd_source_colors(&fixture);
    CHECK(fixture.capture.colorspace == V4L2_COLORSPACE_SMPTE170M &&
          fixture.capture.xfer_func == 2 && fixture.capture.quantization == 1);

    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix = (struct v4l2_pix_format){ .width = 640, .height = 480,
        .priv = V4L2_PIX_FMT_PRIV_MAGIC };
    CHECK(!chd_set_fmt(&handle, &fixture.fh, &fmt));
    CHECK(fixture.color_default && fixture.capture.colorspace == V4L2_COLORSPACE_SMPTE170M &&
          !fixture.capture.xfer_func && !fixture.capture.ycbcr_enc && !fixture.capture.quantization);
    fixture.output.width = 1920; fixture.output.height = 1080;
    chd_source_colors(&fixture);
    CHECK(fixture.capture.colorspace == V4L2_COLORSPACE_REC709 &&
          fixture.output.colorspace == V4L2_COLORSPACE_REC709);
    fixture.output.width = 720; fixture.output.height = 576;
    chd_source_colors(&fixture);
    CHECK(fixture.capture.colorspace == V4L2_COLORSPACE_SMPTE170M);

    fmt.fmt.pix = (struct v4l2_pix_format){ .width = 1280, .height = 720,
        .colorspace = V4L2_COLORSPACE_REC709, .xfer_func = 2,
        .ycbcr_enc = 2, .quantization = 1 }; /* Missing priv magic. */
    CHECK(!chd_set_fmt(&handle, &fixture.fh, &fmt));
    CHECK(!fmt.fmt.pix.xfer_func && !fmt.fmt.pix.ycbcr_enc && !fmt.fmt.pix.quantization);
    fmt.fmt.pix.colorspace = V4L2_COLORSPACE_BT878;
    fmt.fmt.pix.xfer_func = UINT32_MAX; fmt.fmt.pix.ycbcr_enc = 257;
    fmt.fmt.pix.quantization = 257;
    CHECK(!chd_set_fmt(&handle, &fixture.fh, &fmt));
    CHECK(fmt.fmt.pix.colorspace == V4L2_COLORSPACE_REC709 && !fmt.fmt.pix.xfer_func &&
          !fmt.fmt.pix.ycbcr_enc && !fmt.fmt.pix.quantization && fixture.color_default);
}

static void native_error_completion(void)
{
    for (unsigned failure = 0; failure < 2; failure++) {
        reset(true);
        fixture.active[0] = &last_capture;
        last_capture.owned = true; error_receive = true;
        if (failure) decode_error_result = -ESTALE;
        mutex_lock(&fixture.run_lock);
        CHECK(chd_receive(&fixture, &adapter.cmds) == (failure ? -ESTALE : 0));
        CHECK(last_buffer.done == VB2_BUF_STATE_ERROR && !last_buffer.vb2_buf.payload &&
              !(last_buffer.flags & V4L2_BUF_FLAG_LAST) && !fixture.pending_last &&
              !fixture.active[0] && !last_capture.owned && !eos_events &&
              fixture.decoder.phase == CHD_V4L2_RUNNING);
        CHECK(last_buffer.vb2_buf.timestamp == (failure ? 0 : UINT64_C(0x1234567890)) &&
              fixture.decoder.count == (failure ? 3 : 2));
        mutex_unlock(&fixture.run_lock);
    }
}

static void unsolicited_eos_completion(void)
{
    for (unsigned state = 0; state < 3; state++) {
        reset(true);
        fixture.active[0] = &last_capture;
        last_capture.owned = true;
        eos_receive = true;
        fixture.drain_requested = state == 1;
        fixture.decoder.phase = state == 2 ? CHD_V4L2_DRAINING : CHD_V4L2_RUNNING;
        int phase = fixture.decoder.phase;
        unsigned count = fixture.decoder.count;
        last_buffer.flags = V4L2_BUF_FLAG_LAST;
        last_buffer.vb2_buf.timestamp = 123;
        last_buffer.vb2_buf.payload = 456;
        mutex_lock(&fixture.run_lock);
        CHECK(chd_receive(&fixture, &adapter.cmds) == -EPROTO);
        CHECK(last_buffer.done == VB2_BUF_STATE_ERROR &&
              (last_buffer.flags & V4L2_BUF_FLAG_ERROR) &&
              !(last_buffer.flags & V4L2_BUF_FLAG_LAST) &&
              !last_buffer.vb2_buf.payload && !last_buffer.vb2_buf.timestamp);
        CHECK(!fixture.pending_last && !fixture.active[0] && !last_capture.owned &&
              !eos_events && !completed && fixture.decoder.count == count &&
              fixture.decoder.phase == phase && !fixture.drain_tx_done);
        mutex_unlock(&fixture.run_lock);
    }
}

static void discovery_watchdog_cases(void)
{
    const unsigned long limit = msecs_to_jiffies(CHD_DRAIN_TIMEOUT_MS);
    for (unsigned owed = 0; owed < 2; owed++) {
        for (unsigned gate = 0; gate < 4; gate++) {
            reset(false);
            QUEUE_COUNT(&queues.src) = QUEUE_COUNT(&queues.dst) = 4;
            CHECK(!chd_streamon(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_OUTPUT));
            CHECK(!chd_streamon(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_CAPTURE));
            CHECK(fixture.output_streaming && fixture.capture_streaming && !fixture.format_known);
            /* Model completed bootstrap and short input, not a discovered source. */
            fixture.decoder.phase = CHD_V4L2_RUNNING;
            fixture.decoder.count = owed; fixture.input_seen = true;
            fixture.capture_started = fixture.discovery[0].owned = true;
            struct v4l2_decoder_cmd stop = { .cmd = V4L2_DEC_CMD_STOP };
            CHECK(!chd_decoder_cmd(&handle, &fixture.fh, &stop));
            CHECK(fixture.drain_requested && !fixture.drain_left);
            schedule_tx(); CHECK(fixture.tx_active);
            execute_tx();
            CHECK(fixture.drain_tx_done && !fixture.tx_active && transports == 1 &&
                  fixture.decoder.phase == CHD_V4L2_DRAINING);
            if (gate == 1) fixture.discovery[0].owned = false;
            if (gate == 2) fixture.format_pending = true;
            jiffies = ULONG_MAX - limit / 2; chd_drain_watchdog(&fixture);
            if (gate == 3) {
                jiffies += limit - 1; chd_drain_watchdog(&fixture);
                CHECK(fixture.drain_clock_elapsed == limit - 1);
                fixture.format_known = fixture.format_pending = true;
                fixture.discovery[0].owned = false;
            }
            jiffies += limit; chd_drain_watchdog(&fixture);
            CHECK(fixture.fatal == (gate == 0));
            CHECK(fixture.decoder.count == owed &&
                  fixture.decoder.phase == CHD_V4L2_DRAINING &&
                  !eos_events && !fixture.pending_last && !last_buffer.done);
            if (!gate) CHECK(queues.src.error && queues.dst.error && fixture.discovery[0].owned);
            if (gate == 3) CHECK(!fixture.drain_clock_elapsed && !fixture.drain_clock_running);
            /* Later core retirement, not timeout, releases discovery ownership. */
            fixture.discovery[0].owned = false;
        }
    }
    for (unsigned gate = 0; gate < 6; gate++) {
        reset(true); fixture.format_known = false;
        fixture.discovery[0].owned = true; source = &tx_buffer.m2m.vb;
        fixture.decoder.count = CRYSTALHD_V4L2_TIMESTAMPS;
        struct v4l2_decoder_cmd stop = { .cmd = V4L2_DEC_CMD_STOP };
        CHECK(!chd_decoder_cmd(&handle, &fixture.fh, &stop));
        schedule_tx(); CHECK(!tx_queues && fixture.drain_left == 1);
        if (gate == 1) fixture.discovery[0].owned = false;
        if (gate == 2) fixture.format_pending = true;
        if (gate == 3) fixture.capture_started = false;
        if (gate == 4) fixture.admitted = false;
        if (gate == 5) fixture.pending_last = &last_buffer;
        jiffies = ULONG_MAX - limit / 2; chd_drain_watchdog(&fixture);
        jiffies += limit; chd_drain_watchdog(&fixture);
        CHECK(fixture.fatal == (gate == 0));
        CHECK(fixture.decoder.count == CRYSTALHD_V4L2_TIMESTAMPS &&
              fixture.decoder.phase == CHD_V4L2_RUNNING && fixture.drain_left == 1 &&
              !last_buffer.done && !eos_events && !source->done);
        if (!gate) CHECK(queues.src.error && queues.dst.error && fixture.discovery[0].owned);
        /* End fixture only after modeling the later core retirement callback. */
        fixture.discovery[0].owned = false;
    }
    reset(true); fixture.format_known = false;
    fixture.discovery[0].owned = true; source = &tx_buffer.m2m.vb;
    fixture.decoder.count = CRYSTALHD_V4L2_TIMESTAMPS;
    fixture.drain_requested = true; fixture.drain_left = 1;
    jiffies = 0; chd_drain_watchdog(&fixture);
    jiffies += limit - 1; chd_drain_watchdog(&fixture);
    CHECK(fixture.drain_clock_elapsed == limit - 1);
    /* Confirmed format is progress; client CAPTURE negotiation is not timed. */
    fixture.format_known = fixture.format_pending = true;
    fixture.discovery[0].owned = false;
    chd_drain_watchdog(&fixture);
    jiffies += limit * 10; chd_drain_watchdog(&fixture);
    CHECK(!fixture.fatal && !fixture.drain_clock_elapsed && !fixture.drain_clock_running);
    fixture.format_pending = false;
    chd_drain_watchdog(&fixture); /* No posted client buffer yet. */
    CHECK(!fixture.drain_clock_running);
    fixture.active[0] = &last_capture; last_capture.owned = true;
    chd_drain_watchdog(&fixture);
    jiffies += limit - 1; chd_drain_watchdog(&fixture);
    CHECK(!fixture.fatal && fixture.drain_clock_elapsed == limit - 1);
    fixture.decoder.count--; chd_drain_watchdog(&fixture);
    CHECK(!fixture.fatal && !fixture.drain_clock_elapsed);
    fixture.drain_clock_running = true; fixture.drain_clock_elapsed = limit - 1;
    CHECK(!chd_streamoff(&handle, &fixture.fh, V4L2_BUF_TYPE_VIDEO_CAPTURE));
    CHECK(!fixture.drain_clock_running && !fixture.drain_clock_elapsed && !fixture.capture_streaming);
}

static void credit_watchdog_cases(void)
{
    const unsigned long limit = msecs_to_jiffies(CHD_DRAIN_TIMEOUT_MS);
    for (unsigned gate = 0; gate < 9; gate++) {
        reset(true);
        source = &tx_buffer.m2m.vb;
        fixture.active[0] = &last_capture; last_capture.owned = true;
        fixture.decoder.count = CRYSTALHD_V4L2_TIMESTAMPS;
        struct v4l2_decoder_cmd stop = { .cmd = V4L2_DEC_CMD_STOP };
        CHECK(!chd_decoder_cmd(&handle, &fixture.fh, &stop));
        CHECK(fixture.drain_requested && fixture.drain_left == 1);
        schedule_tx();
        CHECK(!tx_queues && !fixture.tx_active && !fixture.drain_tx_done);
        if (gate == 1) fixture.drain_requested = false;
        if (gate == 2) fixture.decoder.count--;
        if (gate == 3) source = NULL;
        if (gate == 4) fixture.drain_left = 0;
        if (gate == 5) fixture.tx_active = true;
        if (gate == 6) fixture.format_pending = true;
        if (gate == 7) last_capture.owned = false;
        if (gate == 8) fixture.capture_streaming = false;
        jiffies = ULONG_MAX - limit / 2;
        chd_drain_watchdog(&fixture);
        jiffies += limit; chd_drain_watchdog(&fixture);
        CHECK(fixture.fatal == (gate == 0));
        CHECK(fixture.decoder.phase == CHD_V4L2_RUNNING && !eos_events &&
              !last_buffer.done && !fixture.pending_last &&
              fixture.active[0] == &last_capture);
        if (!gate)
            CHECK(queues.src.error && queues.dst.error && last_capture.owned &&
                  fixture.decoder.count == CRYSTALHD_V4L2_TIMESTAMPS &&
                  fixture.drain_left == 1 && source && !source->done);
    }
    reset(true); source = &tx_buffer.m2m.vb;
    fixture.active[0] = &last_capture; last_capture.owned = true;
    fixture.decoder.count = CRYSTALHD_V4L2_TIMESTAMPS;
    fixture.drain_requested = true; fixture.drain_left = 1;
    fixture.drain_clock_count = fixture.decoder.count;
    jiffies = 0; chd_drain_watchdog(&fixture);
    jiffies += limit - 1; chd_drain_watchdog(&fixture);
    /* Run the actual receive path: exact-token ERROR retires one credit. */
    error_receive = true; decode_error_result = -EILSEQ;
    mutex_lock(&fixture.run_lock);
    CHECK(!chd_receive(&fixture, &adapter.cmds));
    CHECK(fixture.decoder.count == CRYSTALHD_V4L2_TIMESTAMPS - 1);
    chd_drain_watchdog(&fixture);
    CHECK(!fixture.fatal && !fixture.drain_clock_elapsed && !fixture.drain_clock_running);
    /* Model the next client capture registration, then use real TX admission. */
    fixture.active[0] = &last_capture; last_capture.owned = true;
    chd_schedule_tx(&fixture);
    CHECK(tx_queues == 1 && fixture.tx_active && fixture.drain_left == 1);
    jiffies += 2 * limit; chd_drain_watchdog(&fixture);
    CHECK(!fixture.fatal && !fixture.drain_clock_elapsed);
    mutex_unlock(&fixture.run_lock);
    chd_join(&fixture);
}

static void drain_watchdog_cases(void)
{
    const unsigned long limit = msecs_to_jiffies(CHD_DRAIN_TIMEOUT_MS);
    reset(true);
    fixture.drain_requested = fixture.drain_tx_done = true;
    fixture.decoder.phase = CHD_V4L2_DRAINING;
    fixture.active[0] = &last_capture; last_capture.owned = true;
    fixture.drain_clock_count = fixture.decoder.count;
    jiffies = ULONG_MAX - limit / 2;
    chd_drain_watchdog(&fixture);
    jiffies += limit - 1;
    chd_drain_watchdog(&fixture);
    CHECK(!fixture.fatal && fixture.drain_clock_elapsed == limit - 1);
    jiffies++;
    chd_drain_watchdog(&fixture);
    CHECK(fixture.fatal && queues.src.error && queues.dst.error &&
          !fixture.admitted && last_capture.owned &&
          fixture.active[0] == &last_capture && !last_buffer.done &&
          !fixture.pending_last && !eos_events && fixture.decoder.count == 3 &&
          fixture.decoder.phase == CHD_V4L2_DRAINING);

    for (unsigned pause = 0; pause < 5; pause++) {
        reset(true); fixture.drain_requested = fixture.drain_tx_done = true;
        fixture.decoder.phase = CHD_V4L2_DRAINING;
        fixture.active[0] = &last_capture; last_capture.owned = true;
        fixture.drain_clock_count = fixture.decoder.count;
        jiffies = 10; chd_drain_watchdog(&fixture);
        jiffies += limit / 2; chd_drain_watchdog(&fixture);
        if (pause == 0) fixture.format_pending = true;
        if (pause == 1) fixture.capture_streaming = false;
        if (pause == 2) fixture.active[0] = NULL;
        if (pause == 3) last_capture.owned = false;
        if (pause == 4) fixture.capture_started = false;
        chd_drain_watchdog(&fixture);
        jiffies += 10 * limit; chd_drain_watchdog(&fixture);
        CHECK(!fixture.fatal && fixture.drain_clock_elapsed == limit / 2);
        fixture.format_pending = false; fixture.capture_streaming = true;
        fixture.active[0] = &last_capture; last_capture.owned = true;
        fixture.capture_started = true;
        chd_drain_watchdog(&fixture);
        CHECK(!fixture.fatal && fixture.drain_clock_elapsed == limit / 2);
        /* Genuine progress, including exact-token ERROR, restarts budget. */
        for (unsigned i = 0; i < 3; i++) {
            jiffies += limit - 1; fixture.decoder.count--;
            chd_drain_watchdog(&fixture);
            CHECK(!fixture.fatal && !fixture.drain_clock_elapsed);
        }
        jiffies += limit; chd_drain_watchdog(&fixture);
        CHECK(fixture.fatal && !fixture.decoder.count && !eos_events);
    }
    for (unsigned gate = 0; gate < 5; gate++) {
        reset(true); fixture.drain_requested = fixture.drain_tx_done = true;
        fixture.decoder.phase = CHD_V4L2_DRAINING;
        fixture.active[0] = &last_capture; last_capture.owned = true;
        fixture.drain_clock_count = fixture.decoder.count;
        jiffies = 0; chd_drain_watchdog(&fixture);
        jiffies += limit / 2; chd_drain_watchdog(&fixture);
        if (gate == 0) fixture.tx_active = true;
        if (gate == 1) fixture.drain_tx_done = false;
        if (gate == 2) fixture.drain_requested = false;
        if (gate == 3) fixture.decoder.phase = CHD_V4L2_DRAINED;
        if (gate == 4) fixture.decoder.epoch++;
        jiffies += 10 * limit; chd_drain_watchdog(&fixture);
        CHECK(!fixture.fatal && !fixture.drain_clock_running && !fixture.drain_clock_elapsed);
    }
    reset(true); fixture.drain_clock_running = true; fixture.drain_clock_elapsed = limit / 2;
    chd_join(&fixture);
    CHECK(!fixture.drain_clock_running && !fixture.drain_clock_elapsed);
    reset(true); fixture.drain_requested = fixture.drain_tx_done = true;
    fixture.decoder.phase = CHD_V4L2_DRAINING;
    fixture.pending_last = &last_buffer;
    fixture.drain_clock_count = fixture.decoder.count = 0;
    fixture.drain_clock_running = true; fixture.drain_clock_last = 0;
    jiffies = 10 * limit;
    chd_drain_watchdog(&fixture);
    CHECK(!fixture.fatal && !fixture.drain_clock_running && !last_buffer.done);
    mutex_lock(&fixture.run_lock);
    CHECK(!chd_finish_last(&fixture));
    mutex_unlock(&fixture.run_lock);
    chd_drain_watchdog(&fixture);
    CHECK(!fixture.fatal && !fixture.drain_clock_elapsed &&
          fixture.decoder.phase == CHD_V4L2_DRAINED && eos_events == 1);
}

static void constructor_reset(unsigned failure)
{
    assert(!ctor_file_live && !ctor_node_live && !ctor_fh_live &&
           !ctor_ctrl_live && !ctor_m2m_live && !ctor_video_live &&
           !ctor_dev_refs && !ctor_parent_refs && !ctor_wq_live);
    reset(false); constructor_mode = true;
    ctor_fail = failure; ctor_allocs = ctor_queue_calls = ctor_queue_releases = 0;
    ctor_wq_calls = ctor_fh_exits = ctor_ctx_creates = ctor_binds = ctor_video_releases = 0;
    ctor_ctx_error = 0; ctor_cleanup = NULL; ctor_close_pending = false;
    ctor_minimum.flags = 0; node.m2m = &ctor_m2m;
    node.parent = &ctor_parent; node.dma_dev = &ctor_device;
    node.ioctl_lock.held = false; handle.private_data = NULL;
}

static void constructor_cases(void)
{
    const unsigned failures[] = { CTOR_ALLOC, CTOR_CTX_INIT, CTOR_SRC_QUEUE,
        CTOR_DST_QUEUE, CTOR_OK, CTOR_OK, CTOR_OK, CTOR_OK };
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        constructor_reset(failures[i]);
        if (i == 4) ctor_ctx_error = -ENOMEM;
        if (i == 5) ctor_ctx_error = -ENODEV;
        if (i == 6) ctor_ctx_error = -EIO;
        unsigned adds = fh_add_calls, dels = fh_del_calls;
        int rc = chd_open(&handle);
        if (i < 7) {
            int expected = i < 2 ? -ENOMEM : i < 4 ? -EINVAL : ctor_ctx_error;
            CHECK(rc == expected && !ctor_file_live && !ctor_fh_live && !handle.private_data);
            CHECK(fh_add_calls == adds && fh_del_calls == dels && !ctor_binds);
            CHECK(ctor_fh_exits == (i != 0) && !queues.src.initialized && !queues.dst.initialized);
            CHECK(ctor_queue_calls == (i < 2 ? 0U : i == 2 ? 1U : 2U));
            CHECK(ctor_queue_releases == (i < 3 ? 0U : i == 3 ? 1U : 2U));
            CHECK(ctx_destroys == (i >= 4));
            continue;
        }
        CHECK(!rc && handle.private_data == &fixture.fh && ctor_file_live && ctor_fh_live);
        CHECK(ctor_binds == 1 && ctor_cleanup == chd_file_destroy && fh_add_calls == adds + 1);
        CHECK(fixture.decoder.phase == CHD_V4L2_OFF && !fixture.admitted && !enters && !acquires && !transports);
        CHECK(fixture.run_work.function == chd_run && fixture.tx_work.function == chd_tx_run &&
              !fixture.run_work.pending && !fixture.tx_work.pending);
        CHECK(fixture.output.pixelformat == V4L2_PIX_FMT_H264 && fixture.output.width == 640 &&
              fixture.output.height == 480 && fixture.output.sizeimage == CHD_CODED_SIZE &&
              fixture.output.field == V4L2_FIELD_NONE && fixture.color_default);
        CHECK(fixture.capture.pixelformat == V4L2_PIX_FMT_YUYV && fixture.capture.width == 640 &&
              fixture.capture.height == 480 && fixture.capture.bytesperline == 1280 && fixture.capture.sizeimage);
        for (unsigned qindex = 0; qindex < 2; qindex++) {
            struct vb2_queue *q = qindex ? &queues.dst : &queues.src;
            CHECK(q->initialized && q->type == (qindex ? V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_OUTPUT));
            CHECK(q->io_modes == VB2_MMAP && q->ops == &chd_queue_ops && q->mem_ops == &vb2_dma_sg_memops);
            CHECK(q->timestamp_flags == V4L2_BUF_FLAG_TIMESTAMP_COPY && q->bidirectional == qindex &&
                  q->dev == &ctor_device && q->lock == &node.ioctl_lock && q->drv_priv == &fixture);
            CHECK(q->buf_struct_size == (qindex ? sizeof(struct crystalhd_v4l2_capture_buffer) :
                  sizeof(struct crystalhd_v4l2_output_buffer)));
        }
        CHECK(!chd_release(&handle));
        CHECK(!handle.private_data && !ctor_fh_live && ctor_fh_exits == 1 &&
              fh_del_calls == dels + 1 && ctor_close_pending && ctor_file_live);
        CHECK(!ctx_destroys && !ctor_queue_releases && queues.src.initialized && queues.dst.initialized);
        /* Simulate positive lease retirement after release, invoking its real cleanup. */
        ctor_cleanup(&fixture);
        CHECK(!ctor_file_live && ctx_destroys == 1 && ctor_queue_releases == 2 && private_frees == 1);
    }
    for (unsigned fail = CTOR_OK; fail <= CTOR_VIDEO; fail++) {
        constructor_reset(fail);
        struct crystalhd_v4l2_node *out = (void *)1;
        int rc = crystalhd_v4l2_node_register(&ctor_parent, &ctor_v4l2, &ctor_device,
                                             BC_PCI_DEVID_FLEA, 123, &out);
        if (fail) {
            int expected = fail == CTOR_CTRL_NEW ? -EINVAL : fail == CTOR_M2M ? -ENODEV :
                           fail == CTOR_VIDEO ? -ENFILE : -ENOMEM;
            CHECK(rc == expected && !out && !ctor_node_live && !ctor_video_live);
            CHECK(!ctor_ctrl_live && !ctor_m2m_live && !ctor_wq_live && !ctor_dev_refs && !ctor_parent_refs);
            CHECK(!ctor_video_releases && ctor_allocs == 1);
            continue;
        }
        CHECK(!rc && out == &node && ctor_node_live && ctor_parent_refs == 1 && ctor_dev_refs == 1);
        CHECK(node.generation == 123 && node.parent == &ctor_parent && node.dma_dev == &ctor_device);
        CHECK(node.video.v4l2_dev == &ctor_v4l2 && node.video.ctrl_handler == &node.ctrls &&
              node.video.lock == &node.ioctl_lock && node.video.fops == &chd_fops && node.video.ioctl_ops == &chd_ioctl_ops);
        CHECK(node.video.vfl_dir == VFL_DIR_M2M && node.video.device_caps == (V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING));
        CHECK(ctor_minimum.flags == V4L2_CTRL_FLAG_READ_ONLY && ctor_wq_live == 2 && ctor_m2m_live);
        crystalhd_v4l2_node_unregister(out);
        CHECK(!ctor_video_live && !ctor_parent_refs && ctor_video_releases == 1 && ctor_node_live);
        crystalhd_v4l2_node_destroy(out);
        CHECK(!ctor_node_live && !ctor_dev_refs && !ctor_wq_live && !ctor_m2m_live && !ctor_ctrl_live);
    }
    constructor_reset(CTOR_OK);
    struct crystalhd_v4l2_node *out = (void *)1;
    CHECK(!crystalhd_v4l2_node_register(&ctor_parent, &ctor_v4l2, &ctor_device, 0x1612, 123, &out));
    CHECK(!out && !ctor_allocs && !ctor_dev_refs && !ctor_parent_refs);
    crystalhd_v4l2_node_unregister(NULL); crystalhd_v4l2_node_destroy(NULL);
    for (unsigned output = 0; output < 2; output++) {
        struct v4l2_fmtdesc fmt = { .type = output ? V4L2_BUF_TYPE_VIDEO_OUTPUT : V4L2_BUF_TYPE_VIDEO_CAPTURE };
        CHECK(!chd_enum_fmt(&handle, &fixture.fh, &fmt));
        CHECK(fmt.pixelformat == (output ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_YUYV));
        CHECK(fmt.flags == (output ? V4L2_FMT_FLAG_COMPRESSED : 0));
        fmt.index = 1;
        struct v4l2_fmtdesc saved = fmt;
        CHECK(chd_enum_fmt(&handle, &fixture.fh, &fmt) == -EINVAL && !memcmp(&fmt, &saved, sizeof(fmt)));
    }
    constructor_mode = false;
}

int main(void)
{
    node_api_cases();
    constructor_cases();
    discovery_watchdog_cases();
    credit_watchdog_cases();
    drain_watchdog_cases();
    invalid_and_busy(); format_reallocation(); stop_failures();
    restart_and_drain(); cleanup_and_scheduler();
    streamon_allocation_failures();
    native_error_completion();
    unsolicited_eos_completion();
    allocation_failure_cases();
    format_pause_cases();
    rx_idle_snapshot_cases();
    async_tx_cases();
    stateful_lifecycle_cases();
    colorimetry_cases();
    printf("V4L2 node: %u scenarios, %u checks, 0 failures\n", scenarios, checks);
    return 0;
}
