/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Source-extracted command and hardware TX paths with deterministic IRQ and
 * FIFO boundaries. PCI suspend/release cannot interleave with a live input
 * ioctl: user_lock excludes them; tx_lock excludes a second input ioctl.
 * Flush may cancel a BUSY retry. It is not a persistent admission latch.
 */
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#include "tx-admission-types.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef struct { unsigned int refs; } refcount_t;
static void refcount_set(refcount_t *ref, unsigned int count) { ref->refs = count; }
static void refcount_inc(refcount_t *ref)
{ if (!ref->refs) abort(); ref->refs++; }
static bool refcount_dec_and_test(refcount_t *ref)
{ if (!ref->refs) abort(); return --ref->refs == 0; }

#define KERN_ERR ""
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, next) ((value) = (next))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define WARN_ON_ONCE(value) ((bool)(value))
#define DMA_TO_DEVICE 1
#define DMA_BIDIRECTIONAL 2
#define crystalhd_dio_locked 1
#define crystalhd_dio_sg_mapped 2
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define FOLL_LONGTERM 1U
#define FOLL_WRITE 2U
#define __user
#define offset_in_page(address) ((address) & (PAGE_SIZE - 1U))
#define DIV_ROUND_UP(value, divisor) (((value) + (divisor) - 1U) / (divisor))
#define min_t(type, left, right) \
    ((type)(left) < (type)(right) ? (type)(left) : (type)(right))
#define MAX_JIFFY_OFFSET ((LONG_MAX >> 1) - 1)
#define msecs_to_jiffies(value) \
    (run.saturate_timeout_conversion ? MAX_JIFFY_OFFSET : \
                                       (unsigned long)(value))
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define current NULL
#define printk(...) ((void)0)
#define dev_err(dev, ...) ((void)(dev))
#define dev_dbg(dev, ...) ((void)(dev))
#define lockdep_assert_held(lock) \
    Check((lock) == &adapter.user_lock && *(lock) == 1, \
          "firmware flush retains shared user admission")
#define eCMD_C011_CMD_BASE 0x73763000U
#define eCMD_C011_DEC_CHAN_FLUSH (eCMD_C011_CMD_BASE + 0x104U)
#define eCMD_C011_DEC_CHAN_PAUSE (eCMD_C011_CMD_BASE + 0x11dU)
typedef struct { unsigned wakeups; } wait_queue_head_t;
typedef union { uint64_t full_addr; } addr_64;
struct device { int unused; };
struct pci_dev { struct device dev; int irq; uint32_t device; };
struct crystalhd_adp {
    struct pci_dev *pdev;
    bool present;
    int user_lock;
    bool dma_terminal_quiesced;
    struct { uint32_t cin_wait_exit; } cmds;
};
struct crystalhd_tx_buffer;
struct page { unsigned index; };
struct scatterlist {
    struct page *page;
    unsigned length, offset;
    uint64_t dma_address;
    unsigned dma_length;
};
#define sg_dma_len(sg) ((sg)->dma_length)
static struct scatterlist *sg_next(struct scatterlist *sg) { return sg + 1; }
struct crystalhd_tx_buffer_ops {
    void (*get)(const struct crystalhd_tx_buffer *);
    void (*put)(struct crystalhd_adp *, const struct crystalhd_tx_buffer *);
};
struct crystalhd_tx_buffer {
    void *sgl;
    uint32_t dma_nents, bytes;
    uint64_t tail_addr;
    uint32_t tail_size;
    void *cookie;
    const struct crystalhd_tx_buffer_ops *ops;
};
struct crystalhd_dio_req {
    struct {
        uint32_t xfr_len, uv_offset, uv_sg_ix, uv_sg_off;
        void *xfr_buff;
        BC_OUTPUT_FORMAT b422mode;
        bool dir_tx;
    } uinfo;
    struct crystalhd_tx_buffer tx_buffer;
    struct {
        struct scatterlist *sgl;
        uint32_t dma_nents, capacity, uv_offset, uv_sg_ix, uv_sg_off;
        BC_OUTPUT_FORMAT output_format;
        const void *ops;
        void *cookie;
    } rx_buffer;
    refcount_t tx_refs;
    int sig, page_cnt, sg_nents, sg_cnt, direction;
    unsigned max_pages, fb_size;
    uint64_t fb_pa;
    void *fb_va;
    struct page **pages;
    struct scatterlist *sg;
};
typedef void (*hw_comp_callback)(void *, BC_STATUS);
struct dma_desc_mem { uint64_t phy_addr; };
struct tx_dma_pkt {
    struct dma_desc_mem desc_mem;
    hw_comp_callback call_back;
    const struct crystalhd_tx_buffer *buffer;
    const struct crystalhd_tx_buffer *retained_buffer;
    void *cb_context;
    uint32_t list_tag;
};
struct crystalhd_dioq {
    struct tx_dma_pkt *head, *next;
};
typedef struct { uint32_t cmd[64]; } BC_FW_CMD;
struct crystalhd_user { uint32_t uid, in_use, mode; };
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    bool dma_fault;
    unsigned lock;
    struct crystalhd_dioq *tx_freeq, *tx_actq;
    struct { unsigned cin_busy; } stats;
    struct { uint32_t DramBuffAdd, HostXferSzInBytes; } TxFwInputBuffInfo;
    uint32_t tx_list_post_index, tx_ioq_tag_seed;
    enum LIST_STATUS TxList0Sts, TxList1Sts;
    bool (*pfnCheckInputFIFO)(struct crystalhd_hw *, uint32_t, uint32_t *, bool, uint8_t *);
    void (*pfnStartTxDMA)(struct crystalhd_hw *, uint8_t, addr_64);
    BC_STATUS (*pfnStopTxDMA)(struct crystalhd_hw *);
    BC_STATUS (*pfnDoFirmwareCmd)(struct crystalhd_hw *, BC_FW_CMD *);
    BC_STATUS (*pfnIssuePause)(struct crystalhd_hw *, bool);
    int fetch_sem;
    struct tx_dma_pkt tx_pkt_pool[DMA_ENGINE_CNT];
};
struct crystalhd_cmd {
    uint32_t state, tx_list_id, cin_wait_exit;
    struct crystalhd_adp *adp;
    struct crystalhd_user user[BC_LINK_MAX_OPENS];
    const void *session_owner;
    struct crystalhd_hw *hw_ctx;
};
typedef struct {
    uint32_t u_id;
    struct { union {
        struct { void *pDmaBuff; uint32_t BuffSz; uint8_t Encrypted; } ProcInput;
        BC_FW_CMD fwCmd;
    } u; } udata;
} crystalhd_ioctl_data;

static unsigned checks, failures;
static unsigned long jiffies;
static struct pci_dev endpoint = { .irq = 19, .device = BC_PCI_DEVID_FLEA };
static struct crystalhd_adp adapter;
static struct crystalhd_hw hardware;
static struct crystalhd_cmd context;
static struct crystalhd_dio_req request;
static struct crystalhd_dio_req request2;
static struct tx_dma_pkt *const packet0 = &hardware.tx_pkt_pool[0];
#define packet2 (hardware.tx_pkt_pool[1])
static struct crystalhd_dioq freeq, activeq;
static uint32_t input[16];
static uint32_t opaque_cookie;
static crystalhd_ioctl_data input_ioctl;
/* Dedicated boundaries for the separately named, source-extracted map path.
 * Ordinary TX scenarios continue using their existing mapping mock.
 */
static _Alignas(PAGE_SIZE) uint8_t map_input[3 * PAGE_SIZE];
static struct page map_page_storage[3];
static struct page *map_pages[3];
static struct scatterlist map_sg[3];
static uint8_t map_tail[4];
static const unsigned crystalhd_dio_rx_buffer_ops;
static struct map_boundary_state {
    bool active, allocated, mapped, mapping_succeeded;
    bool alloc_fail, copy_fail, map_fail;
    bool expected_tx;
    unsigned max_pages, expected_bytes, expected_pins;
    long pin_result;
    unsigned allocs, frees, pins, copies, sg_maps, sg_unmaps, syncs;
    unsigned unpin_calls, unpinned_pages, live_pages;
} map_boundary;
static void Complete(void);
static struct {
    unsigned maps, unmaps, descriptors, starts, stops, syncs, sleeps, waits;
    unsigned wakes, irq_depth, irq_disables, irq_enables, fifo_calls, busy;
    unsigned firmware_calls, firmware_depth, bus_clears, bus_drains;
    unsigned callback_calls, masks;
    unsigned sleep_budget[4], sleep_budget_count, completion_budget;
    unsigned post_delay_ms, wait_entry_delay_ms;
    unsigned cancel_on_sleep, remove_on_sleep, absolute_waits;
    int wait_result, sleep_result;
    bool mapped, immediate_completion, completion_before_cancel, flush_on_busy;
    bool drain_ok, fault_on_sleep, signal_pending, remove_on_wait;
    bool complete_on_status, saturate_timeout_conversion;
    bool fault_on_wait, fault_after_descriptor;
    unsigned free_add_failures;
    BC_STATUS map_status, descriptor_status, active_add_status, free_add_status, stop_status;
    BC_STATUS completion_status, firmware_status;
    BC_STATUS seen_callback_status;
    void *seen_callback_context;
    uint8_t seen_flags, transfer_flags;
    uint32_t seen_destination, transfer_size;
} run;

static bool signal_pending(void *task)
{
    (void)task;
    if (run.complete_on_status && context.tx_list_id && activeq.head) {
        run.complete_on_status = false;
        Complete();
    }
    return run.signal_pending;
}

struct multi_cookie {
    unsigned calls;
    bool mapped;
    BC_STATUS status;
};
static struct multi_cookie multi_cookie[2];

static void Check(bool condition, const char *why)
{
    checks++;
    if (!condition) { failures++; fprintf(stderr, "FAIL: %s\n", why); }
}
static unsigned QueueCount(const struct crystalhd_dioq *queue)
{
    return (queue->head != NULL) + (queue->next != NULL);
}
static bool QueueContains(const struct crystalhd_dioq *queue,
                          const struct tx_dma_pkt *owned)
{
    return queue->head == owned || queue->next == owned;
}
static struct device *chddev(void) { return &endpoint.dev; }
static BC_STATUS bc_cproc_do_fw_cmd(struct crystalhd_cmd *, crystalhd_ioctl_data *);
static BC_STATUS crystalhd_hw_tx_req_complete(struct crystalhd_hw *, uint32_t, BC_STATUS);
static void crystalhd_hw_dma_fatal_stop(struct crystalhd_hw *);
static void disable_irq(int);
static void enable_irq(int);
static void synchronize_irq(int);
static BC_STATUS crystalhd_unmap_dio(struct crystalhd_adp *, struct crystalhd_dio_req *);
static const struct crystalhd_tx_buffer_ops crystalhd_dio_tx_buffer_ops;

static void Complete(void)
{
    Check(activeq.head == packet0 && packet0->list_tag != 0,
          "IRQ completion finds the published TX owner");
    Check(crystalhd_hw_tx_req_complete(&hardware, packet0->list_tag,
                                      run.completion_status) == BC_STS_SUCCESS,
          "IRQ completion retires the real active request");
}
static void Unlock(unsigned *lock)
{
    Check(lock == &hardware.lock && *lock == 1, "TX publication releases its spinlock");
    *lock = 0;
    if (run.immediate_completion && activeq.head) {
        run.immediate_completion = false;
        Complete();
    }
}
#define spin_lock_irqsave(lock, flags) do { \
    (flags) = 0; Check(*(lock) == 0, "TX publication acquires a free spinlock"); \
    *(lock) = 1; \
} while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); Unlock(lock); } while (0)
#define crystalhd_create_event(event) (*(event) = (wait_queue_head_t){0})
static void crystalhd_set_event(wait_queue_head_t *event)
{
    Check(event && freeq.head == packet0 && !activeq.head &&
          !packet0->buffer && !packet0->cb_context && !packet0->call_back &&
          !packet0->list_tag,
          "completion wakes only after common TX ownership has retired");
    event->wakeups++; run.wakes++;
}
static int Wait(wait_queue_head_t *event, int condition, unsigned timeout)
{
    if (!context.tx_list_id) {
        Check(timeout && timeout <= 100,
              "FIFO retry sleep is clipped to the remaining budget");
        Check(!condition && !event->wakeups, "FIFO retry uses a separate idle event");
        if (run.sleep_budget_count < sizeof(run.sleep_budget) /
                                     sizeof(run.sleep_budget[0]))
            run.sleep_budget[run.sleep_budget_count] = timeout;
        run.sleep_budget_count++;
        run.sleeps++;
        jiffies += timeout;
        if (run.fault_on_sleep)
            hardware.dma_fault = true;
        if (run.cancel_on_sleep == run.sleeps)
            context.cin_wait_exit = 1;
        if (run.remove_on_sleep == run.sleeps)
            adapter.present = false;
        return run.sleep_result;
    }
    Check(timeout && timeout <= 3000,
          "submitted TX wait retains its published tag and DMA watchdog");
    run.completion_budget = timeout;
    run.waits++;
    if (run.fault_on_wait) {
        disable_irq(endpoint.irq);
        crystalhd_hw_dma_fatal_stop(&hardware);
        enable_irq(endpoint.irq);
        synchronize_irq(endpoint.irq);
        return -EIO;
    }
    if (run.remove_on_wait)
        adapter.present = false;
    if (run.wait_result)
        jiffies += timeout;
    if (run.wait_result && !run.completion_before_cancel)
        return run.wait_result;
    if (!condition)
        Complete();
    Check(event->wakeups == 1,
          "normal completion signals the opaque completion cookie exactly once");
    return run.wait_result;
}
#define crystalhd_wait_on_event(event, condition, timeout, result, nosig) do { \
    (void)(nosig); (result) = Wait(event, condition, timeout); \
    if (context.tx_list_id && !(result)) \
        Check((condition), "TX wait rechecks the completion predicate after wakeup"); \
} while (0)
#define crystalhd_wait_on_event_until(event, condition, deadline, result, nosig) do { \
    unsigned long __test_deadline = (deadline); \
    (void)(nosig); \
    run.absolute_waits++; \
    if (run.wait_entry_delay_ms) { \
        jiffies += run.wait_entry_delay_ms; \
        run.wait_entry_delay_ms = 0; \
    } \
    if (condition) { \
        (result) = 0; \
    } else if (time_after_eq(jiffies, __test_deadline)) { \
        (result) = -EBUSY; \
    } else { \
        (result) = Wait(event, condition, \
                        (unsigned)(__test_deadline - jiffies)); \
        if (!(result) && !(condition) && \
            time_after_eq(jiffies, __test_deadline)) \
            (result) = -EBUSY; \
    } \
    if (context.tx_list_id && !(result)) \
        Check((condition), "bounded TX wait rechecks completion after wakeup"); \
} while (0)
static void synchronize_irq(int irq)
{
    Check(irq == endpoint.irq && !hardware.lock &&
          ((!activeq.head && !activeq.next) || hardware.dma_fault),
          "IRQ synchronization excludes ownership callbacks on completed or fault-retained TX");
    run.syncs++;
}
static void disable_irq(int irq)
{
    Check(irq == endpoint.irq && !hardware.lock && !run.irq_depth,
          "cancellation drains IRQ callbacks without holding the hardware spinlock");
    run.irq_depth++; run.irq_disables++;
}
static void enable_irq(int irq)
{
    Check(irq == endpoint.irq && run.irq_depth == 1 &&
          ((!activeq.head && !activeq.next) || hardware.dma_fault),
          "IRQ resumes only with retired ownership or fault-gated completion delivery");
    run.irq_depth--; run.irq_enables++;
}
static void pci_clear_master(struct pci_dev *pdev)
{
    Check(pdev == &endpoint && hardware.dma_fault,
          "failed DMA stop poisons admission before revoking bus mastering");
    run.bus_clears++;
}
static bool pci_wait_for_pending_transaction(struct pci_dev *pdev)
{
    Check(pdev == &endpoint && run.bus_clears, "fatal stop drains outstanding PCI transactions");
    run.bus_drains++;
    return run.drain_ok;
}
static void crystalhd_flea_disable_interrupts(struct crystalhd_hw *hw)
{
    Check(hw == &hardware && hardware.dma_fault && !adapter.present,
          "fatal stop masks the endpoint without returning any DMA backing");
    run.masks++;
}
static void crystalhd_link_disable_interrupts(struct crystalhd_hw *hw)
{ crystalhd_flea_disable_interrupts(hw); }
static void *crystalhd_dioq_fetch(struct crystalhd_dioq *queue)
{
    struct tx_dma_pkt *owned = queue->head;
    Check(queue == &freeq || queue == &activeq,
          "submission or cancellation fetches from a TX ownership queue");
    queue->head = queue->next;
    queue->next = NULL;
    return owned;
}
static void *crystalhd_dioq_find_and_fetch(struct crystalhd_dioq *queue, uint32_t tag)
{
    struct tx_dma_pkt *owned = NULL;
    Check(queue == &activeq, "completion searches the active ownership queue");
    if (queue->head && queue->head->list_tag == tag) {
        owned = queue->head;
        queue->head = queue->next;
        queue->next = NULL;
    } else if (queue->next && queue->next->list_tag == tag) {
        owned = queue->next;
        queue->next = NULL;
    }
    return owned;
}
static BC_STATUS crystalhd_dioq_add(struct crystalhd_dioq *queue,
                                  struct tx_dma_pkt *owned, bool wake, uint32_t tag)
{
    Check((owned == packet0 || owned == &packet2) && !queue->next && !wake,
          "a packet is returned to exactly one queue");
    if (queue == &activeq) {
        Check(hardware.lock && tag == owned->list_tag && tag,
              "active ownership is published under lock before DMA can start");
        if (run.active_add_status != BC_STS_SUCCESS)
            return run.active_add_status;
    } else {
        Check(queue == &freeq && !tag && !owned->buffer && !owned->cb_context &&
              !owned->call_back && !owned->list_tag,
              "free packets retain no request, callback, cookie or tag ownership");
        if (run.free_add_failures) {
            run.free_add_failures--;
            return run.free_add_status;
        }
    }
    if (queue->head)
        queue->next = owned;
    else
        queue->head = owned;
    return BC_STS_SUCCESS;
}
static BC_STATUS crystalhd_tx_buffer_preflight(const struct crystalhd_tx_buffer *buffer,
                                                uint32_t max_descriptors)
{
    Check(buffer && max_descriptors == BC_LINK_MAX_SGLS,
          "TX preflight receives the mapped frontend buffer");
    return buffer && buffer->bytes && buffer->cookie ?
        BC_STS_SUCCESS : BC_STS_INV_ARG;
}
static BC_STATUS crystalhd_xlat_tx_buffer_to_dma_desc(
        const struct crystalhd_tx_buffer *buffer,
        struct dma_desc_mem *desc, uint32_t *index, struct device *dev, uint32_t destination)
{
    Check(((buffer == &request.tx_buffer && desc == &packet0->desc_mem) ||
           (buffer == &request2.tx_buffer && desc == &packet2.desc_mem)) &&
          index && dev == &endpoint.dev,
          "TX descriptor construction uses its request and reserved packet");
    run.descriptors++; run.seen_destination = destination;
    if (run.fault_after_descriptor) {
        disable_irq(endpoint.irq);
        crystalhd_hw_dma_fatal_stop(&hardware);
        enable_irq(endpoint.irq);
        synchronize_irq(endpoint.irq);
    }
    return run.descriptor_status;
}
static BC_STATUS crystalhd_map_dio(struct crystalhd_adp *adp, void *bytes, uint32_t size,
        uint32_t offset, bool packed, bool tx, struct crystalhd_dio_req **dio)
{
    Check(adp == &adapter && bytes == input && size == run.transfer_size && !offset &&
          !packed && tx && !run.mapped, "input mapping owns the submitted buffer once");
    run.maps++;
    if (run.map_status != BC_STS_SUCCESS)
        return run.map_status;
    run.mapped = true;
    request = (struct crystalhd_dio_req){
        .uinfo = { .xfr_len = size, .dir_tx = true },
        .tx_buffer = { .bytes = size, .cookie = &request,
                       .ops = &crystalhd_dio_tx_buffer_ops },
        .sig = crystalhd_dio_sg_mapped, .page_cnt = 1, .sg_nents = 1,
        .direction = DMA_TO_DEVICE,
    };
    refcount_set(&request.tx_refs, 1);
    *dio = &request;
    return BC_STS_SUCCESS;
}
static void crystalhd_dio_to_device(struct crystalhd_adp *adp, struct crystalhd_dio_req *dio)
{
    if (map_boundary.active) {
        Check(adp == &adapter && dio == &request && map_boundary.allocated &&
              map_boundary.mapped,
              "actual-map cleanup synchronizes a still-live mapping before unmap");
        map_boundary.syncs++;
        return;
    }
    Check(adp == &adapter && dio == &request && run.mapped, "final unmap retains mapped DIO");
}
static void dma_unmap_sg(struct device *dev, void *sg, int count, int direction)
{
    if (map_boundary.active) {
        Check(dev == &endpoint.dev && sg == map_sg && count == request.sg_nents &&
              count > 0 && direction == request.direction &&
              map_boundary.mapped && map_boundary.syncs == 1 &&
              map_boundary.live_pages == map_boundary.expected_pins,
              "actual-map unmap uses original SG count and direction while pages remain pinned");
        map_boundary.mapped = false;
        map_boundary.sg_unmaps++;
        return;
    }
    Check(dev == &endpoint.dev && sg == request.sg && count == 1 && direction == DMA_TO_DEVICE,
          "final TX lease unmaps the original SG mapping");
}
static void unpin_user_pages_dirty_lock(void *pages, int count, bool dirty)
{
    if (map_boundary.active) {
        Check(pages == map_pages && count > 0 &&
              (unsigned)count == map_boundary.live_pages && !map_boundary.mapped &&
              dirty == (map_boundary.mapping_succeeded && !map_boundary.expected_tx),
              "actual-map cleanup unpins only acquired pages and dirties only mapped RX pages");
        map_boundary.unpin_calls++;
        map_boundary.unpinned_pages += count;
        map_boundary.live_pages = 0;
        return;
    }
    Check(pages == request.pages && count == 1 && !dirty,
          "final TX lease unpins read-only source pages without dirtying them");
}
static void crystalhd_free_dio(struct crystalhd_adp *adp, struct crystalhd_dio_req *dio)
{
    if (map_boundary.active) {
        Check(adp == &adapter && dio == &request && map_boundary.allocated &&
              !map_boundary.mapped && !map_boundary.live_pages &&
              (!dio->uinfo.dir_tx || !dio->tx_refs.refs),
              "actual-map DIO storage returns to its pool only after final TX reference and physical cleanup");
        map_boundary.allocated = false;
        map_boundary.frees++;
        return;
    }
    Check(adp == &adapter && dio == &request && run.mapped && !activeq.head &&
          !packet0->buffer && !packet0->retained_buffer && !packet0->cb_context && !packet0->call_back &&
          !context.tx_list_id,
          "input unmaps exactly once after all TX/callback ownership has retired");
    if (run.starts)
        Check(run.syncs || run.irq_disables,
              "DMA ownership is not unmapped before completion or cancellation drains IRQ");
    run.mapped = false; run.unmaps++;
}

static struct crystalhd_dio_req *crystalhd_alloc_dio(struct crystalhd_adp *adp)
{
    Check(map_boundary.active && adp == &adapter && !map_boundary.allocated,
          "actual map allocates exactly one DIO pool object");
    if (map_boundary.alloc_fail)
        return NULL;
    request = (struct crystalhd_dio_req){
        .max_pages = map_boundary.max_pages, .pages = map_pages, .sg = map_sg,
        .fb_va = map_tail, .fb_pa = 0,
    };
    /* Deliberately leave tx_refs zero: only production map_dio may initialize
     * the caller reference, before any pin or mapping failure can unwind.
     */
    map_boundary.allocated = true;
    map_boundary.allocs++;
    return &request;
}

static long pin_user_pages_fast(unsigned long address, unsigned long count,
                                unsigned flags, struct page **pages)
{
    Check(map_boundary.active && map_boundary.allocated &&
          address == (unsigned long)map_input && count == map_boundary.expected_pins &&
          pages == map_pages && request.uinfo.dir_tx == map_boundary.expected_tx &&
          request.direction == (map_boundary.expected_tx ? DMA_TO_DEVICE : DMA_BIDIRECTIONAL) &&
          flags == (FOLL_LONGTERM | (map_boundary.expected_tx ? 0U : FOLL_WRITE)) &&
          request.tx_refs.refs == (map_boundary.expected_tx ? 1U : 0U),
          "production initializes TX direction and its reference before the first fallible pin boundary; RX stays unrefcounted");
    map_boundary.pins++;
    if (map_boundary.pin_result > 0) {
        if ((unsigned long)map_boundary.pin_result > count) abort();
        for (long i = 0; i < map_boundary.pin_result; i++)
            pages[i] = &map_page_storage[i];
        map_boundary.live_pages = map_boundary.pin_result;
    }
    return map_boundary.pin_result;
}

static unsigned long copy_from_user(void *dst, const void *src, size_t count)
{
    Check(map_boundary.active && map_boundary.allocated && dst == map_tail &&
          count == (map_boundary.expected_bytes & 3U) && count &&
          src == map_input + map_boundary.expected_bytes - count &&
          request.tx_refs.refs == 1 && !map_boundary.mapped,
          "TX tail copy follows caller-reference initialization but precedes SG mapping");
    map_boundary.copies++;
    if (map_boundary.copy_fail)
        return count;
    memcpy(dst, src, count);
    return 0;
}

static void crystalhd_init_sg(struct scatterlist *sg, unsigned count)
{
    Check(map_boundary.active && sg == map_sg && count && count <= ARRAY_SIZE(map_sg),
          "actual map initializes a bounded original SG table");
    memset(sg, 0, count * sizeof(*sg));
}

static void crystalhd_set_sg(struct scatterlist *sg, struct page *page,
                            unsigned length, unsigned offset)
{
    Check(map_boundary.active && sg >= map_sg && sg < map_sg + ARRAY_SIZE(map_sg) &&
          page == map_pages[sg - map_sg] && length && offset + length <= PAGE_SIZE,
          "actual map builds SG entries from exactly the pinned pages");
    sg->page = page;
    sg->length = length;
    sg->offset = offset;
}

static int dma_map_sg(struct device *dev, struct scatterlist *sg, int count, int direction)
{
    Check(map_boundary.active && map_boundary.allocated && !map_boundary.mapped &&
          dev == &endpoint.dev && sg == map_sg && count == request.sg_nents &&
          count > 0 && direction == request.direction &&
          map_boundary.live_pages == map_boundary.expected_pins &&
          request.tx_refs.refs == (map_boundary.expected_tx ? 1U : 0U),
          "DMA mapping sees a complete pin set and the production-owned initial TX reference");
    map_boundary.sg_maps++;
    if (map_boundary.map_fail)
        return 0;
    for (int i = 0; i < count; i++) {
        sg[i].dma_address = 0x100000U + i * PAGE_SIZE;
        sg[i].dma_length = sg[i].length;
    }
    map_boundary.mapped = map_boundary.mapping_succeeded = true;
    return count;
}
static void OpaqueComplete(void *context, BC_STATUS status)
{
    Check(context == &opaque_cookie && context != &request,
          "hardware retirement returns the exact frontend cookie, not its DMA backing");
    Check(freeq.head == packet0 && !activeq.head && !packet0->buffer &&
          !packet0->cb_context && !packet0->call_back && !packet0->list_tag,
          "opaque completion runs only after common packet ownership retires");
    run.callback_calls++;
    run.seen_callback_context = context;
    run.seen_callback_status = status;
}
static void MultiComplete(void *context, BC_STATUS status)
{
    struct multi_cookie *cookie = context;
    unsigned index = (unsigned)(cookie - multi_cookie);
    struct tx_dma_pkt *owned = index ? &packet2 : packet0;

    Check(index < 2 && cookie->mapped && !hardware.lock &&
          !QueueContains(&activeq, owned) &&
          !owned->buffer && !owned->cb_context && !owned->call_back &&
          !owned->list_tag,
          "each TX callback observes its retired packet while its backing remains mapped");
    if (status == BC_STS_IO_USER_ABORT)
        Check(!activeq.head && !activeq.next,
              "cancel callbacks run only after every stopped TX owner is detached");
    cookie->calls++;
    cookie->status = status;
    run.callback_calls++;
}
static BC_STATUS crystalhd_hw_fw_cmd_enter(struct crystalhd_hw *hw)
{
    Check(hw == &hardware && !run.firmware_depth, "flush enters firmware serialization");
    run.firmware_depth++;
    return BC_STS_SUCCESS;
}
static void crystalhd_hw_fw_cmd_leave(struct crystalhd_hw *hw)
{
    Check(hw == &hardware && run.firmware_depth == 1, "flush releases firmware serialization");
    run.firmware_depth--;
}
static int down_interruptible(int *sem) { Check(*sem == 1, "capture semaphore is free"); *sem = 0; return 0; }
static void down(int *sem) { (void)down_interruptible(sem); }
static void up(int *sem) { Check(*sem == 0, "capture semaphore is held"); *sem = 1; }

#include "tx-admission-buffer.h"
#include "tx-admission-hardware.h"
#include "tx-admission-command.h"

static BC_STATUS Flush(bool cancel)
{
    crystalhd_ioctl_data command = {0};
    command.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_FLUSH;
    command.udata.u.fwCmd.cmd[3] = cancel;
    return bc_cproc_do_fw_cmd(&context, &command);
}
static bool Fifo(struct crystalhd_hw *hw, uint32_t size, uint32_t *index,
                 bool update, uint8_t *flags)
{
    Check(hw == &hardware && size == run.transfer_size && index && !update &&
          *flags == run.transfer_flags,
          "pre-submit FIFO admission receives the whole mapped input");
    run.fifo_calls++; run.seen_flags = *flags;
    jiffies += run.post_delay_ms;
    if (run.busy) {
        run.busy--;
        if (run.flush_on_busy) {
            run.flush_on_busy = false;
            Check(Flush(true) == run.firmware_status,
                  "a concurrent firmware flush publishes cancellation before BUSY retry");
        }
        return true;
    }
    return false;
}
static void Start(struct crystalhd_hw *hw, uint8_t index, addr_64 descriptor)
{
    struct tx_dma_pkt *owned = descriptor.full_addr == packet0->desc_mem.phy_addr ?
        packet0 : &packet2;
    Check(hw == &hardware && hardware.lock &&
          QueueContains(&activeq, owned) && owned->buffer &&
          owned->call_back && owned->cb_context &&
          owned->cb_context != owned->buffer->cookie &&
          owned->list_tag == hardware.tx_ioq_tag_seed + index &&
          descriptor.full_addr == owned->desc_mem.phy_addr &&
          hw->TxFwInputBuffInfo.HostXferSzInBytes == run.transfer_size,
          "DMA starts only after request, callback, opaque cookie, tag and length publication");
    run.starts++;
}
static BC_STATUS Stop(struct crystalhd_hw *hw)
{
    Check(hw == &hardware && run.irq_depth && !hardware.lock &&
          (run.mapped || !QueueCount(&activeq)),
          "cancel stops DMA with IRQ drained, buffer still mapped and no spinlock held");
    run.stops++;
    return run.stop_status;
}
static BC_STATUS Firmware(struct crystalhd_hw *hw, BC_FW_CMD *command)
{
    Check(hw == &hardware && run.firmware_depth == 1 &&
          command->cmd[0] == eCMD_C011_DEC_CHAN_FLUSH,
          "the real flush command reaches the serialized firmware boundary");
    run.firmware_calls++;
    return run.firmware_status;
}
static BC_STATUS Pause(struct crystalhd_hw *hw, bool pause)
{
    (void)hw; (void)pause;
    Check(false, "TX-only tests never enter the capture pause path");
    return BC_STS_ERROR;
}
static void Reset(void)
{
    jiffies = 0;
    memset(&run, 0, sizeof(run));
    memset(&request, 0, sizeof(request));
    freeq = (struct crystalhd_dioq){ .head = packet0 };
    activeq = (struct crystalhd_dioq){0};
    memset(multi_cookie, 0, sizeof(multi_cookie));
    adapter = (struct crystalhd_adp){
        .pdev = &endpoint, .present = true, .user_lock = 1 };
    hardware = (struct crystalhd_hw){ .adp = &adapter,
        .tx_freeq = &freeq, .tx_actq = &activeq, .tx_ioq_tag_seed = 0x100,
        .pfnCheckInputFIFO = Fifo, .pfnStartTxDMA = Start, .pfnStopTxDMA = Stop,
        .pfnDoFirmwareCmd = Firmware, .pfnIssuePause = Pause, .fetch_sem = 1,
        .TxFwInputBuffInfo.DramBuffAdd = 0x8000 };
    *packet0 = (struct tx_dma_pkt){ .desc_mem.phy_addr = 0x1000 };
    packet2 = (struct tx_dma_pkt){ .desc_mem.phy_addr = 0x2000 };
    context = (struct crystalhd_cmd){ .state = BC_LINK_READY, .adp = &adapter,
        .hw_ctx = &hardware };
    context.user[0] = (struct crystalhd_user){
        .uid = 0, .in_use = 1, .mode = DTS_PLAYBACK_MODE };
    context.session_owner = &context.user[0];
    input_ioctl = (crystalhd_ioctl_data){ .udata.u.ProcInput = {
        .pDmaBuff = input, .BuffSz = sizeof(input), .Encrypted = 0x80 } };
    run.drain_ok = true;
    run.free_add_status = BC_STS_INSUFF_RES;
    run.transfer_flags = input_ioctl.udata.u.ProcInput.Encrypted;
    run.transfer_size = input_ioctl.udata.u.ProcInput.BuffSz;
}
static void Balanced(void)
{
    Check(!run.mapped && !activeq.head && freeq.head == packet0 &&
          !activeq.next && !freeq.next && !context.tx_list_id &&
          !hardware.lock && !run.irq_depth && !run.firmware_depth &&
          !packet0->buffer && !packet0->call_back && !packet0->cb_context && !packet0->list_tag,
          "completed input leaves balanced mapping, packet, locks and callback ownership");
}

static void RetireTerminalTx(void)
{
    unsigned int unmaps = run.unmaps;

    Check(hardware.dma_fault && !adapter.present && !run.irq_depth,
          "terminal retirement starts with fault-retained ownership and no admitted TX");
    /* Model the separately tested PCI-writer fence and queue deletion. Do
     * not clear dma_fault or manufacture a successful engine-stop retry.
     */
    adapter.dma_terminal_quiesced = true;
    disable_irq(endpoint.irq);
    while (crystalhd_dioq_fetch(&activeq)) { }
    crystalhd_hw_retire_tx_quiesced(&hardware);
    Check(!packet0->buffer && !packet0->retained_buffer && !packet0->call_back &&
          !packet0->cb_context && !packet2.buffer && !packet2.retained_buffer &&
          !packet2.call_back && !packet2.cb_context,
          "pure terminal retirement clears every fixed TX identity before its final put");
    crystalhd_hw_retire_tx_quiesced(&hardware);
    Check(run.unmaps <= unmaps + 1U,
          "repeated terminal retirement cannot release backing twice");
    enable_irq(endpoint.irq);
}
static void Admission(void)
{
    wait_queue_head_t event = {0};
    uint32_t tag = 0;

    Reset();
    Check(bc_cproc_proc_input(NULL, &input_ioctl) == BC_STS_INV_ARG &&
          bc_cproc_proc_input(&context, NULL) == BC_STS_INV_ARG,
          "invalid command arguments are rejected before mapping");
    context.state = BC_LINK_SUSPEND;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_PWR_MGMT && !run.maps,
          "suspended input is rejected before DMA mapping");
    context.state = BC_LINK_READY;
    input_ioctl.udata.u.ProcInput.pDmaBuff = (unsigned char *)input + 1;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_NOT_IMPL && !run.maps,
          "unaligned input is rejected before DMA mapping");
    input_ioctl.udata.u.ProcInput.pDmaBuff = input;
    input_ioctl.udata.u.ProcInput.BuffSz = 0;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_INV_ARG && !run.maps,
          "empty input is rejected before DMA mapping");
    request.tx_buffer = (struct crystalhd_tx_buffer){
        .bytes = run.transfer_size, .cookie = &request,
    };
    Check(crystalhd_hw_post_tx(NULL, &request.tx_buffer, bc_proc_in_completion,
                              &event, &tag, 0) == BC_STS_INV_ARG &&
          crystalhd_hw_post_tx(&hardware, NULL, bc_proc_in_completion,
                              &event, &tag, 0) == BC_STS_INV_ARG &&
          crystalhd_hw_post_tx(&hardware, &request.tx_buffer, NULL,
                              &event, &tag, 0) == BC_STS_INV_ARG &&
          crystalhd_hw_post_tx(&hardware, &request.tx_buffer, bc_proc_in_completion,
                              NULL, &tag, 0) == BC_STS_INV_ARG &&
          crystalhd_hw_post_tx(&hardware, &request.tx_buffer, bc_proc_in_completion,
                              &event, NULL, 0) == BC_STS_INV_ARG && !run.fifo_calls,
          "incomplete hardware admission arguments cannot reach FIFO or packet ownership");
    Balanced();

    Reset();
    tag = 0xfeed;
    request.tx_buffer = (struct crystalhd_tx_buffer){ .cookie = &request };
    Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer,
                               bc_proc_in_completion, &event, &tag, 0) ==
              BC_STS_INV_ARG &&
          tag == 0xfeed && !run.fifo_calls && !run.descriptors &&
          !run.starts && freeq.head == packet0 && !activeq.head,
          "empty mapped TX is rejected before FIFO or packet ownership");
    request.tx_buffer = (struct crystalhd_tx_buffer){
        .bytes = run.transfer_size,
    };
    Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer,
                               bc_proc_in_completion, &event, &tag, 0) ==
              BC_STS_INV_ARG &&
          tag == 0xfeed && !run.fifo_calls && !run.descriptors &&
          !run.starts && freeq.head == packet0 && !activeq.head,
          "ownerless mapped TX is rejected before FIFO or packet ownership");
    Balanced();

    Reset(); run.map_status = BC_STS_INSUFF_RES;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_INSUFF_RES &&
          run.maps == 1 && !run.unmaps && !run.fifo_calls,
          "failed mapping is propagated without unmapping an unowned request");
    Balanced();

    Reset(); context.hw_ctx = NULL;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_INV_ARG &&
          run.maps == 1 && run.unmaps == 1 && !run.fifo_calls && !run.starts,
          "the legacy adapter still releases its mapping when hardware is unavailable");
    Balanced();

    Reset(); hardware.dma_fault = true;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_IO_ERROR &&
          run.maps == 1 && run.unmaps == 1 && !run.fifo_calls && !run.starts,
          "a fatal DMA latch rejects TX before checking FIFO or starting hardware");
    Balanced();
}
static void OpaqueCookie(void)
{
    for (unsigned immediate = 0; immediate < 2; immediate++) {
        uint32_t tag = 0;

        Reset();
        run.immediate_completion = immediate;
        run.mapped = true;
        request.uinfo.xfr_len = run.transfer_size;
        request.tx_buffer = (struct crystalhd_tx_buffer){
            .bytes = run.transfer_size, .cookie = &request,
        };
        Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, OpaqueComplete,
                                   &opaque_cookie, &tag, run.transfer_flags) ==
              BC_STS_SUCCESS && tag == 0x100,
              "TX accepts a frontend cookie independent from DMA backing");
        if (!immediate)
            Complete();
        Check(run.callback_calls == 1 &&
              run.seen_callback_context == &opaque_cookie &&
              run.seen_callback_status == BC_STS_SUCCESS,
              "normal and immediate retirement complete the exact cookie once");
        Check(crystalhd_hw_tx_req_complete(&hardware, tag, BC_STS_SUCCESS) ==
              BC_STS_NO_DATA && run.callback_calls == 1,
              "a duplicate IRQ cannot complete a retired opaque cookie");
        run.mapped = false;
        Balanced();
    }

    Reset();
    run.mapped = true;
    run.completion_status = BC_STS_ERROR;
    request.uinfo.xfr_len = run.transfer_size;
    request.tx_buffer = (struct crystalhd_tx_buffer){
        .bytes = run.transfer_size, .cookie = &request,
    };
    {
        uint32_t tag = 0;

        Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, OpaqueComplete,
                                   &opaque_cookie, &tag, run.transfer_flags) ==
              BC_STS_SUCCESS,
              "TX admits an opaque cookie before an ISR error");
        Complete();
        Check(run.callback_calls == 1 &&
              run.seen_callback_status == BC_STS_ERROR,
              "the real TX ISR error status reaches the opaque cookie unchanged");
    }
    run.mapped = false;
    Balanced();

    Reset();
    run.mapped = true;
    request.uinfo.xfr_len = run.transfer_size;
    request.tx_buffer = (struct crystalhd_tx_buffer){
        .bytes = run.transfer_size, .cookie = &request,
    };
    {
        uint32_t tag = 0;

        Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, OpaqueComplete,
                                   &opaque_cookie, &tag, run.transfer_flags) ==
              BC_STS_SUCCESS &&
              crystalhd_hw_cancel_all_tx(&hardware) == BC_STS_SUCCESS &&
              run.callback_calls == 1 &&
              run.seen_callback_context == &opaque_cookie &&
              run.seen_callback_status == BC_STS_IO_USER_ABORT,
              "cancellation retires and completes the same opaque cookie once");
        Check(crystalhd_hw_tx_req_complete(&hardware, tag, BC_STS_SUCCESS) ==
              BC_STS_NO_DATA && run.callback_calls == 1,
              "an IRQ after cancellation cannot complete the opaque cookie twice");
    }
    run.mapped = false;
    Balanced();
}
static void BalancedTwo(void)
{
    Check(!run.mapped && !activeq.head && !activeq.next &&
          QueueCount(&freeq) == 2 && QueueContains(&freeq, packet0) &&
          QueueContains(&freeq, &packet2) && !hardware.lock && !run.irq_depth &&
          !packet0->buffer && !packet0->cb_context && !packet0->call_back &&
          !packet0->list_tag && !packet2.buffer && !packet2.cb_context &&
          !packet2.call_back && !packet2.list_tag,
          "two-list completion leaves both packets and mappings with one owner");
}
static void CancelAllOwners(void)
{
    Reset();
    Check(crystalhd_hw_cancel_all_tx(NULL) == BC_STS_INV_ARG && !run.stops &&
          crystalhd_hw_cancel_all_tx(&hardware) == BC_STS_SUCCESS &&
          run.stops == 1 && run.irq_disables == 1 && run.irq_enables == 1 &&
          run.syncs == 1 && !run.callback_calls,
          "invalid cancel is inert and empty cancel still resets the shared engine");
    Balanced();

    for (unsigned stop_failure = 0; stop_failure < 2; stop_failure++) {
        uint32_t tag[2] = {0, 0};

        Reset();
        freeq.next = &packet2;
        request.uinfo.xfr_len = run.transfer_size;
        request2.uinfo.xfr_len = run.transfer_size;
        request.tx_buffer = (struct crystalhd_tx_buffer){
            .bytes = run.transfer_size, .cookie = &request,
        };
        request2.tx_buffer = (struct crystalhd_tx_buffer){
            .bytes = run.transfer_size, .cookie = &request2,
        };
        multi_cookie[0].mapped = multi_cookie[1].mapped = true;
        run.mapped = true;
        run.stop_status = stop_failure ? BC_STS_IO_ERROR : BC_STS_SUCCESS;

        Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, MultiComplete,
                                   &multi_cookie[0], &tag[0], run.transfer_flags) ==
              BC_STS_SUCCESS &&
              crystalhd_hw_post_tx(&hardware, &request2.tx_buffer, MultiComplete,
                                   &multi_cookie[1], &tag[1], run.transfer_flags) ==
              BC_STS_SUCCESS && tag[0] == 0x100 && tag[1] == 0x101 &&
              QueueCount(&activeq) == 2 && !QueueCount(&freeq),
              "both hardware TX lists publish independent owners before cancellation");
        Check(crystalhd_hw_cancel_all_tx(&hardware) ==
              (stop_failure ? BC_STS_IO_ERROR : BC_STS_SUCCESS),
              "one engine-wide cancellation reports the stop result");
        Check(run.stops == 1 && run.irq_disables == 1 &&
              run.irq_enables == 1 && run.syncs == 1 &&
              multi_cookie[0].calls == !stop_failure &&
              multi_cookie[1].calls == !stop_failure &&
              (stop_failure || (multi_cookie[0].status == BC_STS_IO_USER_ABORT &&
                                multi_cookie[1].status == BC_STS_IO_USER_ABORT)),
              "cancel returns both owners only after a successful engine stop");
        if (stop_failure)
            Check(hardware.dma_fault && !adapter.present &&
                  adapter.cmds.cin_wait_exit && run.bus_clears == 1 &&
                  run.bus_drains == 1 && run.masks == 1 &&
                  QueueCount(&activeq) == 2 && !QueueCount(&freeq),
                  "a failed shared stop retains both borrowed owners even after local drain success");
        Check(crystalhd_hw_tx_req_complete(&hardware, tag[0], BC_STS_SUCCESS) ==
              (stop_failure ? BC_STS_IO_ERROR : BC_STS_NO_DATA) &&
              crystalhd_hw_tx_req_complete(&hardware, tag[1], BC_STS_SUCCESS) ==
              (stop_failure ? BC_STS_IO_ERROR : BC_STS_NO_DATA) &&
              run.callback_calls == (stop_failure ? 0U : 2U),
              "late IRQs cannot complete either cancelled owner twice");
        Check(crystalhd_hw_cancel_all_tx(&hardware) ==
              (stop_failure ? BC_STS_IO_ERROR : BC_STS_SUCCESS) &&
              run.callback_calls == (stop_failure ? 0U : 2U),
              "repeated cancel-all cannot repeat an owner callback");
        if (stop_failure) {
            RetireTerminalTx();
            Check(!run.callback_calls,
                  "terminal cleanup detaches borrowed callbacks rather than invoking an expired context");
        }
        multi_cookie[0].mapped = multi_cookie[1].mapped = false;
        run.mapped = false;
        if (!stop_failure)
            BalancedTwo();
    }

    {
        uint32_t tag[2] = {0, 0};

        Reset();
        freeq.next = &packet2;
        request.uinfo.xfr_len = run.transfer_size;
        request2.uinfo.xfr_len = run.transfer_size;
        request.tx_buffer = (struct crystalhd_tx_buffer){
            .bytes = run.transfer_size, .cookie = &request,
        };
        request2.tx_buffer = (struct crystalhd_tx_buffer){
            .bytes = run.transfer_size, .cookie = &request2,
        };
        multi_cookie[0].mapped = multi_cookie[1].mapped = true;
        run.mapped = true;
        run.free_add_failures = 1;
        Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, MultiComplete,
                                   &multi_cookie[0], &tag[0], run.transfer_flags) ==
              BC_STS_SUCCESS &&
              crystalhd_hw_post_tx(&hardware, &request2.tx_buffer, MultiComplete,
                                   &multi_cookie[1], &tag[1], run.transfer_flags) ==
              BC_STS_SUCCESS,
              "cleanup-error coverage starts with both TX owners published");
        Check(crystalhd_hw_cancel_all_tx(&hardware) == BC_STS_INSUFF_RES &&
              !activeq.head && !activeq.next && QueueCount(&freeq) == 1 &&
              multi_cookie[0].calls == 1 && multi_cookie[1].calls == 1 &&
              multi_cookie[0].status == BC_STS_IO_USER_ABORT &&
              multi_cookie[1].status == BC_STS_IO_USER_ABORT &&
              !packet0->buffer && !packet0->cb_context && !packet0->call_back &&
              !packet0->list_tag && !packet2.buffer && !packet2.cb_context &&
              !packet2.call_back && !packet2.list_tag,
              "a first requeue error is returned without stranding the peer owner");
        multi_cookie[0].mapped = multi_cookie[1].mapped = false;
        run.mapped = false;
        Check(!hardware.lock && !run.irq_depth,
              "cleanup failure leaves no hardware or IRQ lock held");
    }

    for (unsigned first_list = 0; first_list < 2; first_list++) {
        for (unsigned first_failed = 0; first_failed < 2; first_failed++) {
            uint32_t tag[2] = {0, 0};
            unsigned remaining = first_list ^ 1U;
            BC_STATUS first_status = first_failed ? BC_STS_ERROR : BC_STS_SUCCESS;

            Reset();
            freeq.next = &packet2;
            request.uinfo.xfr_len = run.transfer_size;
            request2.uinfo.xfr_len = run.transfer_size;
            request.tx_buffer = (struct crystalhd_tx_buffer){
                .bytes = run.transfer_size, .cookie = &request,
            };
            request2.tx_buffer = (struct crystalhd_tx_buffer){
                .bytes = run.transfer_size, .cookie = &request2,
            };
            multi_cookie[0].mapped = multi_cookie[1].mapped = true;
            run.mapped = true;
            Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, MultiComplete,
                                       &multi_cookie[0], &tag[0], run.transfer_flags) ==
                  BC_STS_SUCCESS &&
                  crystalhd_hw_post_tx(&hardware, &request2.tx_buffer, MultiComplete,
                                       &multi_cookie[1], &tag[1], run.transfer_flags) ==
                  BC_STS_SUCCESS,
                  "mixed completion/cancel starts with two independent owners");
            Check(crystalhd_hw_tx_req_complete(&hardware, tag[first_list], first_status) ==
                  BC_STS_SUCCESS && multi_cookie[first_list].calls == 1 &&
                  multi_cookie[first_list].status == first_status,
                  "either list may complete successfully or fail before cancellation");
            Check(crystalhd_hw_cancel_all_tx(&hardware) == BC_STS_SUCCESS &&
                  multi_cookie[first_list].calls == 1 &&
                  multi_cookie[remaining].calls == 1 &&
                  multi_cookie[remaining].status == BC_STS_IO_USER_ABORT,
                  "cancel-all aborts only the owner that remained after the IRQ");
            Check(crystalhd_hw_tx_req_complete(&hardware, tag[0], BC_STS_SUCCESS) ==
                  BC_STS_NO_DATA &&
                  crystalhd_hw_tx_req_complete(&hardware, tag[1], BC_STS_SUCCESS) ==
                  BC_STS_NO_DATA && run.callback_calls == 2,
                  "mixed completion and cancellation still retire each cookie once");
            multi_cookie[0].mapped = multi_cookie[1].mapped = false;
            run.mapped = false;
            BalancedTwo();
        }
    }
}
static void Completion(void)
{
    for (unsigned immediate = 0; immediate < 2; immediate++) {
        Reset(); run.immediate_completion = immediate;
        Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_SUCCESS &&
              run.starts == 1 && run.syncs == 1 && run.wakes == 1 && run.unmaps == 1 &&
              run.seen_flags == 0x80 && run.seen_destination == 0x8000,
              "normal and immediate IRQ completions preserve flags and retire before unmap");
        Check(crystalhd_hw_tx_req_complete(&hardware, 0x100, BC_STS_SUCCESS) == BC_STS_NO_DATA &&
              run.wakes == 1, "a duplicate IRQ cannot callback into a retired stack event");
        Balanced();
    }
    Reset(); run.completion_status = BC_STS_IO_ERROR;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_IO_ERROR && run.syncs == 1,
          "a completed hardware error is propagated after IRQ retirement");
    Balanced();

    Reset();
    for (unsigned i = 0; i < 3; i++) {
        Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_SUCCESS &&
              hardware.tx_list_post_index == (i + 1) % DMA_ENGINE_CNT,
              "successive inputs alternate DMA list tags and safely reuse the freed packet");
        Balanced();
    }
}
static void Rollback(void)
{
    Reset(); freeq.head = NULL;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_INSUFF_RES &&
          !run.descriptors && !run.starts && run.unmaps == 1,
          "an exhausted packet pool rejects input without building or starting DMA");
    freeq.head = packet0;
    Balanced();

    Reset(); run.descriptor_status = BC_STS_NOT_IMPL;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_NOT_IMPL && !run.starts,
          "descriptor failure returns the free packet without starting DMA");
    Balanced();
    Reset(); run.active_add_status = BC_STS_INSUFF_RES;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_INSUFF_RES && !run.starts,
          "active-queue failure clears callback ownership before returning the packet");
    Balanced();
}
static void TransferArguments(void)
{
    const uint8_t flags[] = { 0, 1, 0x80, 0xff };
    const uint32_t sizes[] = { 1, 12, sizeof(input) };

    for (unsigned f = 0; f < sizeof(flags) / sizeof(flags[0]); f++) {
        for (unsigned n = 0; n < sizeof(sizes) / sizeof(sizes[0]); n++) {
            for (unsigned busy = 0; busy < 2; busy++) {
                Reset();
                run.busy = 2 * busy;
                run.transfer_flags = input_ioctl.udata.u.ProcInput.Encrypted = flags[f];
                run.transfer_size = input_ioctl.udata.u.ProcInput.BuffSz = sizes[n];
                Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_SUCCESS &&
                      run.seen_flags == flags[f] && run.sleeps == 2 * busy &&
                      run.seen_destination == ((flags[f] & 0x80) ? 0x8000U : 0U) &&
                      request.uinfo.xfr_len == sizes[n] && run.maps == 1 && run.unmaps == 1,
                      "legacy input forwards every flag bit and mapped length through FIFO retries");
                Balanced();
            }
        }
    }
}
static void BusyErrors(void)
{
    const int sleep_errors[] = { -EBUSY, -EIO, -EINTR };

    for (unsigned n = 0; n < sizeof(sleep_errors) / sizeof(sleep_errors[0]); n++) {
        Reset(); run.busy = 1; run.sleep_result = sleep_errors[n];
        Check(bc_cproc_proc_input(&context, &input_ioctl) ==
              (sleep_errors[n] == -EINTR ? BC_STS_IO_USER_ABORT : BC_STS_SUCCESS) &&
              run.sleeps == 1 && run.maps == 1 && run.unmaps == 1,
              "only interrupted BUSY sleep aborts; other legacy sleep results retry");
        Balanced();
    }
    for (unsigned which = 0; which < 3; which++) {
        Reset(); run.busy = 1;
        if (which == 0) run.descriptor_status = BC_STS_NOT_IMPL;
        if (which == 1) run.active_add_status = BC_STS_INSUFF_RES;
        if (which == 2) run.fault_on_sleep = true;
        Check(bc_cproc_proc_input(&context, &input_ioctl) ==
              (which == 0 ? BC_STS_NOT_IMPL : which == 1 ? BC_STS_INSUFF_RES : BC_STS_IO_ERROR) &&
              run.sleeps == 1 && run.maps == 1 && run.unmaps == 1 && !run.starts && !run.waits,
              "hard post errors after BUSY retry preserve status and retire the single mapping");
        Balanced();
    }
}
static void BusyAndFlush(void)
{
    Reset(); run.busy = 31;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_SUCCESS &&
          run.sleeps == 31 && jiffies == 3100 &&
          run.completion_budget == 3000 && run.starts == 1,
          "legacy admission remains unbounded beyond the DMA watchdog interval");
    Balanced();
    Reset(); run.busy = 2;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_SUCCESS &&
          run.sleeps == 2 && run.fifo_calls == 3 && hardware.stats.cin_busy == 2 &&
          run.descriptors == 1 && run.starts == 1,
          "BUSY retries retain one mapping and allocate descriptors only after admission");
    Balanced();
    Reset(); run.busy = 1; run.sleep_result = -EINTR;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_IO_USER_ABORT &&
          !run.starts && run.unmaps == 1, "a signal during BUSY wait aborts without posting DMA");
    Balanced();
    for (unsigned rejected = 0; rejected < 2; rejected++) {
        Reset(); run.busy = 1; run.flush_on_busy = true;
        run.firmware_status = rejected ? BC_STS_TIMEOUT : BC_STS_SUCCESS;
        Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_CMD_CANCELLED &&
              run.firmware_calls == 1 && !run.sleeps && !run.starts &&
              !run.stops && !run.irq_disables && !context.cin_wait_exit,
              "flush cancels one BUSY input retry even if firmware reports failure");
        Balanced();
        Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_SUCCESS,
              "the consumed flush flag does not permanently reject a subsequent input");
        Balanced();
    }
    Reset(); run.busy = 1;
    Check(Flush(false) == BC_STS_SUCCESS && !context.cin_wait_exit &&
          bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_SUCCESS &&
          !run.stops && !run.irq_disables,
          "non-cancelling flush preserves normal BUSY retry admission");
    Balanced();
    Reset();
    Check(Flush(true) == BC_STS_SUCCESS && context.cin_wait_exit &&
          bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_SUCCESS &&
          !context.cin_wait_exit && !run.sleeps && !run.stops && !run.irq_disables,
          "a cancelling flush flag applies to BUSY retry, not immediately admissible input");
    Balanced();
    Reset(); context.cin_wait_exit = 1; context.state = BC_LINK_SUSPEND;
    Check(bc_cproc_codein_sleep(&context, 100, false, 0) ==
              BC_STS_PWR_MGMT &&
          context.cin_wait_exit,
          "suspended retry reports PM before consuming the one-shot flush flag");
    adapter.present = false;
    Check(bc_cproc_codein_sleep(&context, 100, false, 0) ==
              BC_STS_CMD_CANCELLED &&
          bc_cproc_codein_sleep(&context, 100, false, 0) ==
              BC_STS_CMD_CANCELLED &&
          !run.sleeps,
          "removal cancels every retry even if another caller consumed a flush flag");
    Balanced();
}
static void Cancellation(void)
{
    const int wait_errors[] = {-EBUSY, -EINTR, -EIO};
    const BC_STATUS expected[] = {BC_STS_TIMEOUT, BC_STS_IO_USER_ABORT, BC_STS_IO_ERROR};
    for (unsigned i = 0; i < sizeof(wait_errors) / sizeof(wait_errors[0]); i++) {
        for (unsigned completed = 0; completed < 2; completed++) {
            for (unsigned stop_failure = 0; stop_failure < 3; stop_failure++) {
                Reset(); run.wait_result = wait_errors[i];
                run.completion_before_cancel = completed;
                run.stop_status = stop_failure ? BC_STS_IO_ERROR : BC_STS_SUCCESS;
                run.drain_ok = stop_failure != 2;
                Check(bc_cproc_proc_input(&context, &input_ioctl) == expected[i] &&
                      run.stops == 1 && run.irq_disables == (stop_failure ? 2U : 1U) &&
                      run.irq_enables == run.irq_disables &&
                      run.wakes == (completed || !stop_failure ? 1U : 0U) &&
                      run.syncs == run.irq_disables &&
                      run.unmaps == (completed || !stop_failure ? 1U : 0U),
                      "wait status survives cancellation while only proven ownership is released");
                if (stop_failure) {
                    Check(hardware.dma_fault && !adapter.present &&
                          adapter.cmds.cin_wait_exit && run.bus_clears == 1 &&
                          run.bus_drains == 1 && run.masks == 1,
                          "failed stop revokes DMA even when IRQ completion already retired the request");
                    if (!completed) {
                        Check(run.mapped && request.tx_refs.refs == 1 &&
                              packet0->retained_buffer == &request.tx_buffer &&
                              !packet0->buffer && !packet0->call_back && !packet0->cb_context,
                              "returning input leaves its extra DIO lease, never its stack waiter, in fixed inventory");
                        Check(crystalhd_hw_tx_req_complete(&hardware, 0x100, BC_STS_SUCCESS) ==
                                  BC_STS_IO_ERROR && !run.unmaps && !run.wakes,
                              "late completion cannot release retained pages or touch the returned stack");
                    } else {
                        Check(!run.mapped && !request.tx_refs.refs &&
                              !packet0->retained_buffer,
                              "a completion winning cancellation returns its extra lease instead of leaking it");
                    }
                    RetireTerminalTx();
                    Check(!run.mapped && run.unmaps == 1 && !request.tx_refs.refs,
                          "later proven terminal retirement releases retained DIO exactly once");
                } else {
                    Balanced();
                }
            }
        }
    }
}

static void PrepareBorrowedInput(void)
{
    run.transfer_size = 7;
    run.transfer_flags = 0x81;
    request = (struct crystalhd_dio_req){
        .uinfo = { .xfr_len = run.transfer_size, .dir_tx = true },
        .tx_buffer = {
            .bytes = run.transfer_size,
            .cookie = &request,
            .ops = &crystalhd_dio_tx_buffer_ops,
        },
        .sig = crystalhd_dio_sg_mapped, .page_cnt = 1, .sg_nents = 1,
        .direction = DMA_TO_DEVICE,
    };
    refcount_set(&request.tx_refs, 1);
    run.mapped = true;
}

static void FinishBorrowedInput(void)
{
    if (packet0->retained_buffer) {
        Check(run.mapped && !run.maps && !run.unmaps &&
              request.tx_refs.refs == 2 && !context.tx_list_id &&
              packet0->retained_buffer == &request.tx_buffer &&
              !packet0->buffer && !packet0->cb_context && !packet0->call_back &&
              hardware.dma_fault && !adapter.present,
              "unsafe borrowed return keeps backing but no expired stack waiter");
        crystalhd_unmap_dio(&adapter, &request);
        Check(run.mapped && !run.unmaps && request.tx_refs.refs == 1,
              "caller release cannot unmap an unsafe retained transfer");
        Check(crystalhd_hw_tx_req_complete(&hardware, packet0->list_tag,
                                           BC_STS_SUCCESS) == BC_STS_IO_ERROR &&
              !run.unmaps && !run.wakes,
              "late IRQ cannot touch a returned borrowed-transfer waiter");
        RetireTerminalTx();
        Check(!run.mapped && run.unmaps == 1 && !request.tx_refs.refs,
              "terminal proof releases the final borrowed lease exactly once");
        return;
    }
    Check(run.mapped && !run.maps && !run.unmaps && !context.tx_list_id &&
          !activeq.head && freeq.head == packet0 && !packet0->buffer &&
          !packet0->cb_context && !packet0->call_back && !packet0->list_tag,
          "borrowed transfer retires hardware ownership before caller release");
    crystalhd_unmap_dio(&adapter, &request);
    Balanced();
}

static void BoundedTransfer(void)
{
	unsigned long deadline = 0xfeedUL;

	Reset();
	jiffies = ULONG_MAX - 19UL;
	Check(crystalhd_tx_deadline_from_ms(50, &deadline) == BC_STS_SUCCESS &&
	      deadline == 30UL,
	      "finite TX deadline construction remains wrap-safe");
	Check(crystalhd_tx_deadline_from_ms(0, &deadline) == BC_STS_INV_ARG &&
	      crystalhd_tx_deadline_from_ms(1, NULL) == BC_STS_INV_ARG &&
	      deadline == 30UL,
	      "deadline construction rejects absent budgets and output without mutation");

	Reset(); PrepareBorrowedInput(); run.busy = 1; run.wait_result = -EBUSY;
	jiffies = 50;
	Check(crystalhd_tx_transfer_until(&context, &request.tx_buffer,
				      run.transfer_flags, 250) == BC_STS_TIMEOUT &&
	      run.fifo_calls == 2 && run.sleeps == 1 && run.starts == 1 &&
	      run.completion_budget == 100 && jiffies == 250 && run.stops == 1,
	      "absolute TX entry consumes an existing budget instead of renewing it");
	FinishBorrowedInput();

	Reset(); PrepareBorrowedInput();
	Check(crystalhd_tx_transfer_until(&context, &request.tx_buffer,
				      run.transfer_flags, 0) == BC_STS_TIMEOUT &&
	      !run.fifo_calls && !run.starts,
	      "an expired absolute deadline is rejected before hardware");
	FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.busy = 10;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) == BC_STS_TIMEOUT &&
          run.fifo_calls == 3 && run.sleeps == 3 &&
          run.absolute_waits == 3 &&
          run.sleep_budget_count == 3 && run.sleep_budget[0] == 100 &&
          run.sleep_budget[1] == 100 && run.sleep_budget[2] == 50 &&
          jiffies == 250 && !run.descriptors && !run.starts && !run.stops,
          "bounded BUSY admission expires at one clipped absolute deadline");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.busy = 10;
    run.wait_entry_delay_ms = 120;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) == BC_STS_TIMEOUT &&
          run.fifo_calls == 3 && run.absolute_waits == 3 && run.sleeps == 2 &&
          run.sleep_budget[0] == 100 && run.sleep_budget[1] == 30 &&
          jiffies == 250 && !run.starts,
          "preemption before a bounded sleep cannot create a fresh relative budget");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.busy = 2; run.wait_result = -EBUSY;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) == BC_STS_TIMEOUT &&
          run.fifo_calls == 3 && run.sleeps == 2 && run.starts == 1 &&
          run.completion_budget == 50 && jiffies == 250 && run.stops == 1 &&
          run.irq_disables == 1 && run.irq_enables == 1 && run.syncs == 1,
          "DMA completion receives only the total budget left after admission");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.post_delay_ms = 250;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) == BC_STS_TIMEOUT &&
          run.starts == 1 && !run.waits && run.stops == 1 && jiffies == 250,
          "a posted request crossing the deadline is cancelled before return");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.post_delay_ms = 250;
    run.immediate_completion = true;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) == BC_STS_SUCCESS &&
          run.starts == 1 && !run.waits && !run.stops && run.syncs == 1,
          "an IRQ completion already observed at the deadline wins without cancellation");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.post_delay_ms = 250;
    run.complete_on_status = true;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) == BC_STS_SUCCESS &&
          run.starts == 1 && !run.waits && !run.stops && run.wakes == 1 &&
          run.syncs == 1,
          "completion racing the deadline status check is observed before cancellation");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.busy = 2;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 40) == BC_STS_TIMEOUT &&
          run.fifo_calls == 1 && run.sleeps == 1 &&
          run.sleep_budget[0] == 40 && jiffies == 40 && !run.starts,
          "a sub-retry budget cannot oversleep or attempt DMA after expiry");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.busy = 10;
    jiffies = ULONG_MAX - 149;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) == BC_STS_TIMEOUT &&
          run.fifo_calls == 3 && run.sleep_budget[0] == 100 &&
          run.sleep_budget[1] == 100 && run.sleep_budget[2] == 50 &&
          jiffies == 100 && !run.starts,
          "bounded admission keeps its deadline across jiffies wrap");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.busy = 1; run.remove_on_sleep = 1;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) ==
              BC_STS_CMD_CANCELLED &&
          run.fifo_calls == 1 && run.sleeps == 1 && !run.starts,
          "removal during bounded BUSY sleep prevents the now-ready repost");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.busy = 1; run.cancel_on_sleep = 1;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) ==
              BC_STS_CMD_CANCELLED &&
          run.fifo_calls == 1 && run.sleeps == 1 && !run.starts &&
          !context.cin_wait_exit,
          "a legacy flush arriving during bounded sleep is consumed before repost");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.busy = 1; run.sleep_result = -EINTR;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) ==
              BC_STS_IO_USER_ABORT &&
          run.fifo_calls == 1 && run.sleeps == 1 && !run.starts,
          "a signal during bounded admission aborts without DMA ownership");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.busy = 1; run.sleep_result = -EIO;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) == BC_STS_IO_ERROR &&
          run.fifo_calls == 1 && run.sleeps == 1 && !run.starts,
          "an unexpected bounded admission wait error cannot repost DMA");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.wait_result = -EBUSY;
    run.remove_on_wait = true;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) ==
              BC_STS_CMD_CANCELLED &&
          run.starts == 1 && run.stops == 1 && run.wakes == 1 &&
          run.syncs == 1,
          "removal during a posted bounded wait cancels and retires DMA once");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.wait_result = -EBUSY;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 5000) == BC_STS_TIMEOUT &&
          run.completion_budget == 3000 && jiffies == 3000 &&
          run.starts == 1 && run.stops == 1,
          "bounded completion retains the three-second hardware watchdog cap");
    FinishBorrowedInput();

    for (unsigned which = 0; which < 2; which++) {
        Reset(); PrepareBorrowedInput();
        run.wait_result = which ? -EIO : -EINTR;
        Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                         run.transfer_flags, 250) ==
                  (which ? BC_STS_IO_ERROR : BC_STS_IO_USER_ABORT) &&
              run.starts == 1 && run.stops == 1 &&
              run.irq_disables == 1 && run.irq_enables == 1 &&
              run.syncs == 1,
              "bounded posted wait errors cancel and retire DMA exactly once");
        FinishBorrowedInput();
    }

    Reset(); PrepareBorrowedInput(); run.wait_result = -EBUSY;
    run.completion_before_cancel = true;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) == BC_STS_SUCCESS &&
          run.starts == 1 && !run.stops && run.wakes == 1 && run.syncs == 1,
          "bounded completion observed at deadline wins over cancellation");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.wait_result = -EBUSY;
    run.stop_status = BC_STS_IO_ERROR; run.drain_ok = false;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) == BC_STS_TIMEOUT &&
          run.starts == 1 && run.stops == 1 && hardware.dma_fault &&
          run.bus_clears == 1 && run.bus_drains == 1 && run.syncs == 2,
          "bounded timeout status survives a failed fatal DMA stop");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); context.state = BC_LINK_SUSPEND;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) == BC_STS_PWR_MGMT &&
          !run.fifo_calls && !run.starts,
          "bounded admission rejects an already suspended session before hardware");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.signal_pending = true;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) ==
              BC_STS_IO_USER_ABORT &&
          !run.fifo_calls && !run.starts,
          "bounded admission observes a pending signal before hardware");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.post_delay_ms = 250;
    run.descriptor_status = BC_STS_NOT_IMPL;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) ==
              BC_STS_NOT_IMPL &&
          !run.starts && !run.stops,
          "a hard post failure is not replaced by an expired budget");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.immediate_completion = true;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, INT_MAX) ==
              BC_STS_SUCCESS &&
          run.fifo_calls == 1 && run.starts == 1 && !run.absolute_waits,
          "the largest signed millisecond budget remains finite when representable");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput();
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags,
                                     (uint32_t)INT_MAX + 1U) ==
              BC_STS_INV_ARG &&
          !run.fifo_calls && !run.starts && !run.absolute_waits,
          "a millisecond budget above INT_MAX is rejected before hardware");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput();
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, UINT32_MAX) ==
              BC_STS_INV_ARG &&
          !run.fifo_calls && !run.starts && !run.absolute_waits,
          "UINT32_MAX cannot turn a finite TX budget into an infinite wait");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput(); run.saturate_timeout_conversion = true;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) ==
              BC_STS_INV_ARG &&
          !run.fifo_calls && !run.starts && !run.absolute_waits,
          "a saturating jiffies conversion is rejected before hardware");
    FinishBorrowedInput();
}

static void BorrowedTransfer(void)
{
    const BC_STATUS expected[] = {
        BC_STS_SUCCESS, BC_STS_SUCCESS, BC_STS_SUCCESS, BC_STS_TIMEOUT,
        BC_STS_IO_USER_ABORT, BC_STS_IO_ERROR, BC_STS_NOT_IMPL,
        BC_STS_CMD_CANCELLED, BC_STS_TIMEOUT, BC_STS_IO_ERROR, BC_STS_INV_ARG
    };

    Reset();
    Check(crystalhd_tx_transfer_sync(NULL, &request.tx_buffer, 0, 0) == BC_STS_INV_ARG &&
          crystalhd_tx_transfer_sync(&context, NULL, 0, 0) == BC_STS_INV_ARG,
          "the ioctl-independent transfer rejects NULL context or request");
    context.hw_ctx = NULL;
    context.tx_list_id = 0x1234;
    context.cin_wait_exit = 1;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer, 0, 0) == BC_STS_INV_ARG &&
          context.tx_list_id == 0x1234 && context.cin_wait_exit == 1,
          "missing hardware is rejected without changing command ownership or cancellation state");
    Check(!run.maps && !run.unmaps && !run.fifo_calls && !run.starts && !run.waits,
          "invalid borrowed input has no mapping or hardware effects");
    context.tx_list_id = 0;
    Balanced();
    for (unsigned which = 0; which < sizeof(expected) / sizeof(expected[0]); which++) {
        Reset();
        /* The caller lends a freshly mapped request, including clear completion
         * fields. The transfer must retire DMA, not unmap the caller's request. */
        PrepareBorrowedInput();
        if (which == 1) run.immediate_completion = true;
        if (which == 2) run.busy = 2;
        if (which == 3 || which == 8) run.wait_result = -EBUSY;
        if (which == 4) run.wait_result = -EINTR;
        if (which == 5) run.wait_result = -EIO;
        if (which == 6) { run.busy = 1; run.descriptor_status = BC_STS_NOT_IMPL; }
        if (which == 7) { run.busy = 1; run.flush_on_busy = true; }
        if (which == 8) run.stop_status = BC_STS_IO_ERROR;
        if (which == 9) run.completion_status = BC_STS_IO_ERROR;
        if (which == 10) context.hw_ctx = NULL;
        Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                         run.transfer_flags, 0) == expected[which],
              "borrowed transfers preserve success, retry, completion and cancellation statuses");
        FinishBorrowedInput();
    }
}

struct lease_owner {
    struct crystalhd_tx_buffer buffer;
    unsigned refs, gets, puts;
};

static void LeaseGet(const struct crystalhd_tx_buffer *buffer)
{
    struct lease_owner *owner = buffer->cookie;

    Check(owner && buffer == &owner->buffer && owner->refs,
          "core lease get refers to a stable live frontend wrapper");
    owner->refs++;
    owner->gets++;
}

static void LeasePut(struct crystalhd_adp *adp,
                     const struct crystalhd_tx_buffer *buffer)
{
    struct lease_owner *owner = buffer->cookie;

    Check(adp == &adapter && owner && buffer == &owner->buffer && owner->refs &&
          !packet0->buffer && !packet0->retained_buffer && !packet0->call_back &&
          !packet0->cb_context && !packet2.buffer && !packet2.retained_buffer &&
          !packet2.call_back && !packet2.cb_context &&
          !activeq.head && !activeq.next && !freeq.head && !freeq.next,
          "every queue and fixed TX identity is detached before any terminal lease put");
    owner->refs--;
    owner->puts++;
}

static void RetainedLeaseProtocol(void)
{
    const struct crystalhd_tx_buffer_ops ops = { .get = LeaseGet, .put = LeasePut };
    struct lease_owner owner[2] = {
        { .refs = 1 }, { .refs = 1 },
    };
    unsigned waiter[2] = {0};

    Reset();
    for (unsigned i = 0; i < 2; i++) {
        owner[i].buffer.cookie = &owner[i];
        owner[i].buffer.ops = &ops;
        crystalhd_tx_buffer_get(&owner[i].buffer);
    }
    Check(!crystalhd_hw_retain_tx_buffer(&hardware, &owner[0].buffer, &waiter[0]) &&
          !run.irq_disables && owner[0].refs == 2,
          "healthy retention is inert and cannot take a frontend lease");
    disable_irq(endpoint.irq);
    crystalhd_hw_dma_fatal_stop(&hardware);
    enable_irq(endpoint.irq);
    for (unsigned i = 0; i < 2; i++) {
        hardware.tx_pkt_pool[i].buffer = &owner[i].buffer;
        hardware.tx_pkt_pool[i].cb_context = &waiter[i];
        hardware.tx_pkt_pool[i].call_back = OpaqueComplete;
    }
    Check(!crystalhd_hw_retain_tx_buffer(&hardware, &owner[0].buffer, &waiter[1]) &&
          !crystalhd_hw_retain_tx_buffer(&hardware, &owner[1].buffer, &waiter[0]) &&
          packet0->buffer == &owner[0].buffer &&
          packet2.buffer == &owner[1].buffer &&
          !packet0->retained_buffer && !packet2.retained_buffer,
          "retention requires both exact buffer and exact stack-context identities");
    for (unsigned i = 0; i < 2; i++) {
        struct tx_dma_pkt *packet = &hardware.tx_pkt_pool[i];

        Check(crystalhd_hw_retain_tx_buffer(&hardware, &owner[i].buffer, &waiter[i]) &&
              packet->retained_buffer == &owner[i].buffer &&
              !packet->buffer && !packet->call_back && !packet->cb_context &&
              owner[i].refs == 2 && owner[i].gets == 1 && !owner[i].puts,
              "either fixed slot adopts one existing lease and severs the stack identity");
        Check(!crystalhd_hw_retain_tx_buffer(&hardware, &owner[i].buffer, &waiter[i]),
              "duplicate adoption cannot create another retained reference");
        /* Return the caller reference while the retained lease remains live. */
        owner[i].refs--;
    }
    RetireTerminalTx();
    Check(!owner[0].refs && !owner[1].refs &&
          owner[0].puts == 1 && owner[1].puts == 1 && !run.callback_calls,
          "terminal retirement puts both detached leases exactly once without borrowed callbacks");

    Reset(); PrepareBorrowedInput();
    run.fault_on_wait = true;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) == BC_STS_IO_ERROR &&
          run.starts == 1 && !run.stops && run.bus_clears == 1 &&
          run.bus_drains == 1 && request.tx_refs.refs == 2 &&
          packet0->retained_buffer == &request.tx_buffer,
          "a posted TX wait survives an RX-origin fatal stop without a second engine stop");
    FinishBorrowedInput();

    Reset(); PrepareBorrowedInput();
    run.fault_after_descriptor = true;
    Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer,
                                     run.transfer_flags, 250) == BC_STS_IO_ERROR &&
          run.descriptors == 1 && !run.starts && !run.stops &&
          request.tx_refs.refs == 1 && !packet0->retained_buffer &&
          !packet0->buffer && !packet0->call_back && !packet0->cb_context,
          "fault between preflight and publication rolls back the unposted packet and extra lease");
    FinishBorrowedInput();

    for (unsigned missing = 0; missing < 3; missing++) {
        struct crystalhd_tx_buffer_ops incomplete = crystalhd_dio_tx_buffer_ops;

        Reset(); PrepareBorrowedInput();
        if (missing == 0) request.tx_buffer.ops = NULL;
        if (missing == 1) { incomplete.get = NULL; request.tx_buffer.ops = &incomplete; }
        if (missing == 2) { incomplete.put = NULL; request.tx_buffer.ops = &incomplete; }
        Check(crystalhd_tx_transfer_sync(&context, &request.tx_buffer, 0, 0) ==
                  BC_STS_INV_ARG && request.tx_refs.refs == 1 &&
              !run.fifo_calls && !run.starts && !run.unmaps,
              "synchronous TX rejects an incomplete lease contract before taking ownership");
        FinishBorrowedInput();
    }
}
static void MapBoundaryReset(bool tx, unsigned bytes)
{
    Check(!map_boundary.allocated && !map_boundary.mapped && !map_boundary.live_pages,
          "each actual-map case starts with no retained pool, pin or SG ownership");
    Reset();
    map_boundary = (struct map_boundary_state){
        .active = true, .expected_tx = tx, .expected_bytes = bytes,
        .max_pages = ARRAY_SIZE(map_pages),
        .expected_pins = DIV_ROUND_UP(bytes, PAGE_SIZE),
        .pin_result = DIV_ROUND_UP(bytes, PAGE_SIZE),
    };
    for (unsigned i = 0; i < sizeof(map_input); i++)
        map_input[i] = (uint8_t)(i * 17U + 3U);
    memset(map_pages, 0, sizeof(map_pages));
    memset(map_sg, 0, sizeof(map_sg));
    memset(map_tail, 0xa5, sizeof(map_tail));
}

static BC_STATUS MapActual(struct crystalhd_dio_req **out)
{
    return crystalhd_map_dio_actual(&adapter, map_input,
        map_boundary.expected_bytes, map_boundary.expected_tx ? 0 : PAGE_SIZE,
        MODE420, map_boundary.expected_tx, out);
}

static void MapBoundaryBalanced(void)
{
    Check(!map_boundary.allocated && !map_boundary.mapped &&
          !map_boundary.live_pages && map_boundary.allocs == map_boundary.frees &&
          !request.tx_refs.refs,
          "actual mapping cleanup balances the caller reference, DIO pool, SG and all acquired pages");
}

static void ActualMappingLifetime(void)
{
    static const long failed_pins[] = { -EFAULT, 0, 1 };
    struct crystalhd_dio_req *out;

    for (unsigned tx = 0; tx < 2; tx++) {
        for (unsigned failure = 0; failure < ARRAY_SIZE(failed_pins); failure++) {
            MapBoundaryReset(tx, PAGE_SIZE + 8);
            map_boundary.pin_result = failed_pins[failure];
            out = &request2;
            Check(MapActual(&out) == BC_STS_ERROR && !out &&
                  map_boundary.allocs == 1 && map_boundary.frees == 1 &&
                  map_boundary.pins == 1 && !map_boundary.copies &&
                  !map_boundary.sg_maps && !map_boundary.sg_unmaps &&
                  map_boundary.unpinned_pages == (failure == 2 ? 1U : 0U) &&
                  map_boundary.unpin_calls == (failure == 2 ? 1U : 0U),
                  "negative, zero and short pin returns unwind exact TX/RX page ownership without publishing a DIO");
            MapBoundaryBalanced();
        }

        MapBoundaryReset(tx, PAGE_SIZE + 8);
        map_boundary.map_fail = true;
        out = &request2;
        Check(MapActual(&out) == BC_STS_ERROR && !out &&
              map_boundary.pins == 1 && map_boundary.sg_maps == 1 &&
              !map_boundary.sg_unmaps && !map_boundary.syncs &&
              map_boundary.unpin_calls == 1 && map_boundary.unpinned_pages == 2 &&
              map_boundary.frees == 1,
              "failed SG mapping unpins the complete TX/RX pin set without unmapping a nonexistent DMA mapping");
        MapBoundaryBalanced();
    }

    MapBoundaryReset(true, PAGE_SIZE + 7);
    map_boundary.copy_fail = true;
    out = &request2;
    Check(MapActual(&out) == BC_STS_ERROR && !out &&
          map_boundary.copies == 1 && !map_boundary.sg_maps &&
          map_boundary.unpin_calls == 1 && map_boundary.unpinned_pages == 2 &&
          map_boundary.frees == 1,
          "TX tail-copy failure consumes the production-initialized caller reference during unwind");
    MapBoundaryBalanced();

    for (unsigned tail_only = 0; tail_only < 2; tail_only++) {
        MapBoundaryReset(true, tail_only ? 3 : PAGE_SIZE + 7);
        out = NULL;
        Check(MapActual(&out) == BC_STS_SUCCESS && out == &request &&
              request.tx_refs.refs == 1 &&
              request.tx_buffer.cookie == &request &&
              request.tx_buffer.ops == &crystalhd_dio_tx_buffer_ops &&
              request.tx_buffer.sgl == map_sg &&
              request.tx_buffer.dma_nents == (tail_only ? 0U : 2U) &&
              request.tx_buffer.bytes == map_boundary.expected_bytes &&
              request.tx_buffer.tail_addr == 0 && request.tx_buffer.tail_size == 3 &&
              !memcmp(map_tail, map_input + map_boundary.expected_bytes - 3, 3) &&
              map_tail[3] == 0,
              "actual TX mapping publishes its embedded lease descriptor with a valid DMA-zero coherent tail");
        crystalhd_tx_buffer_get(&request.tx_buffer);
        Check(request.tx_refs.refs == 2 &&
              crystalhd_unmap_dio(&adapter, out) == BC_STS_SUCCESS &&
              request.tx_refs.refs == 1 && map_boundary.allocated &&
              map_boundary.live_pages == map_boundary.expected_pins &&
              map_boundary.mapped == !tail_only &&
              !map_boundary.sg_unmaps && !map_boundary.unpin_calls && !map_boundary.frees,
              "caller unmap only drops its reference while a core TX lease retains pages, SG and coherent tail");
        crystalhd_tx_buffer_put(&adapter, &request.tx_buffer);
        Check(map_boundary.sg_unmaps == !tail_only &&
              map_boundary.unpin_calls == 1 &&
              map_boundary.unpinned_pages == map_boundary.expected_pins &&
              map_boundary.frees == 1,
              "the final core TX put performs physical unmap and unpin exactly once, including tail-only input");
        MapBoundaryBalanced();
    }

    MapBoundaryReset(false, PAGE_SIZE + 8);
    out = NULL;
    Check(MapActual(&out) == BC_STS_SUCCESS && out == &request &&
          !request.tx_refs.refs && request.rx_buffer.cookie == &request &&
          request.rx_buffer.ops == &crystalhd_dio_rx_buffer_ops &&
          request.rx_buffer.sgl == map_sg && request.rx_buffer.dma_nents == 2 &&
          request.rx_buffer.capacity == PAGE_SIZE + 8 &&
          request.rx_buffer.uv_offset == PAGE_SIZE &&
          request.rx_buffer.uv_sg_ix == 1 && !request.rx_buffer.uv_sg_off &&
          request.rx_buffer.output_format == MODE420,
          "actual RX mapping remains unrefcounted and publishes its mapped UV-plane layout");
    Check(crystalhd_unmap_dio(&adapter, out) == BC_STS_SUCCESS &&
          map_boundary.sg_unmaps == 1 && map_boundary.unpin_calls == 1 &&
          map_boundary.unpinned_pages == 2 && map_boundary.frees == 1,
          "unchanged RX unmap synchronizes, unmaps and dirty-unpins all pages once without a TX reference");
    MapBoundaryBalanced();

    MapBoundaryReset(true, PAGE_SIZE + 8);
    map_boundary.max_pages = 1;
    out = &request2;
    Check(MapActual(&out) == BC_STS_INSUFF_RES && !out &&
          map_boundary.allocs == 1 && map_boundary.frees == 1 &&
          !map_boundary.pins && !request.tx_refs.refs,
          "oversize DIO storage returns directly to its pool before reference initialization or pinning");
    MapBoundaryBalanced();
    MapBoundaryReset(true, PAGE_SIZE + 8);
    map_boundary.alloc_fail = true;
    out = &request2;
    Check(MapActual(&out) == BC_STS_INSUFF_RES && !out &&
          !map_boundary.allocs && !map_boundary.frees && !map_boundary.pins,
          "DIO pool allocation failure publishes no reference or page ownership");
    MapBoundaryBalanced();
    map_boundary.active = false;
}

int main(void)
{
    Admission(); OpaqueCookie(); CancelAllOwners(); Completion(); Rollback(); TransferArguments(); BusyErrors(); BusyAndFlush(); Cancellation();
    BoundedTransfer(); BorrowedTransfer(); RetainedLeaseProtocol();
    ActualMappingLifetime();
    printf("TX admission: %u checks, %u failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
