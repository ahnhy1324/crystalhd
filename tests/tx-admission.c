/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Source-extracted command and hardware TX paths with deterministic IRQ and
 * FIFO boundaries. PCI suspend/release cannot interleave with a live input
 * ioctl: user_lock excludes them; tx_lock excludes a second input ioctl.
 * Flush may cancel a BUSY retry. It is not a persistent admission latch.
 */
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
#include "tx-admission-types.h"
#include "tx-admission-flea-types.h"

_Static_assert(sizeof(TX_INPUT_BUFFER_INFO) == 8 * sizeof(uint32_t),
               "TX notification is eight DWORDs including two reserved words");
_Static_assert(offsetof(TX_INPUT_BUFFER_INFO, DramBuffAdd) == 0 &&
               offsetof(TX_INPUT_BUFFER_INFO, DramBuffSzInBytes) == 4 &&
               offsetof(TX_INPUT_BUFFER_INFO, HostXferSzInBytes) == 8 &&
               offsetof(TX_INPUT_BUFFER_INFO, Flags) == 12 &&
               offsetof(TX_INPUT_BUFFER_INFO, SeqNum) == 16 &&
               offsetof(TX_INPUT_BUFFER_INFO, ChannelID) == 20 &&
               offsetof(TX_INPUT_BUFFER_INFO, Reserved) == 24,
               "TX notification wire fields retain their exact DWORD offsets");

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
typedef union {
    uint64_t full_addr;
    struct { uint32_t low_part, high_part; };
} addr_64;
typedef unsigned spinlock_t;
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
#include "tx-admission-queue-types.h"
typedef struct { uint32_t cmd[64]; } BC_FW_CMD;
struct crystalhd_user { uint32_t uid, in_use, mode; };
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    bool dma_fault;
    unsigned lock;
    struct crystalhd_dioq *tx_freeq, *tx_actq;
    struct { unsigned cin_busy; } stats;
    TX_INPUT_BUFFER_INFO TxFwInputBuffInfo;
    uint32_t TxBuffInfoAddr, EmptyCnt;
    bool WakeUpDecodeDone, SingleThreadAppFIFOEmpty;
    enum FLEA_POWER_STATES FleaPowerState;
    uint32_t tx_list_post_index, tx_ioq_tag_seed;
    enum LIST_STATUS TxList0Sts, TxList1Sts;
    bool (*pfnCheckInputFIFO)(struct crystalhd_hw *, uint32_t, uint32_t *, bool, uint8_t *);
    BC_STATUS (*pfnPrepareTxDMA)(struct crystalhd_hw *, uint32_t);
    void (*pfnStartTxDMA)(struct crystalhd_hw *, uint8_t, addr_64);
    uint32_t (*pfnReadFPGARegister)(struct crystalhd_adp *, uint32_t);
    void (*pfnWriteFPGARegister)(struct crystalhd_adp *, uint32_t, uint32_t);
    BC_STATUS (*pfnStopTxDMA)(struct crystalhd_hw *);
    BC_STATUS (*pfnDoFirmwareCmd)(struct crystalhd_hw *, BC_FW_CMD *);
    BC_STATUS (*pfnIssuePause)(struct crystalhd_hw *, bool);
    BC_STATUS (*pfnDevDRAMRead)(struct crystalhd_hw *, uint32_t, uint32_t, uint32_t *);
    BC_STATUS (*pfnDevDRAMWrite)(struct crystalhd_hw *, uint32_t, uint32_t,
                               const uint32_t *);
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
static struct tx_dma_pkt *QueueHead(const struct crystalhd_dioq *);
static struct tx_dma_pkt *QueueNext(const struct crystalhd_dioq *);
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
static struct crystalhd_elem node_pool[8];
static bool node_allocated[ARRAY_SIZE(node_pool)];
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
    unsigned queue_fetches, queue_adds, node_allocs, node_frees, node_attempts;
    bool allocator_exhausted;
    BC_STATUS prep_status;
    unsigned sleep_budget[4], sleep_budget_count, completion_budget;
    unsigned post_delay_ms, wait_entry_delay_ms;
    unsigned cancel_on_sleep, remove_on_sleep, absolute_waits;
    int wait_result, sleep_result;
    bool mapped, immediate_completion, completion_before_cancel, flush_on_busy;
    bool drain_ok, fault_on_sleep, signal_pending, remove_on_wait;
    bool complete_on_status, saturate_timeout_conversion;
    bool fault_on_wait, fault_after_descriptor;
    unsigned free_add_failures;
    BC_STATUS map_status, descriptor_status, free_add_status, stop_status;
    BC_STATUS completion_status, firmware_status;
    BC_STATUS seen_callback_status;
    void *seen_callback_context;
    uint8_t seen_flags, transfer_flags;
    uint32_t seen_destination, transfer_size;
    struct {
        TX_INPUT_BUFFER_INFO payload;
        BC_STATUS status;
        unsigned copied_words, reads, writes, wakes;
        bool wake_allowed, wake_success;
    } flea;
    struct {
        bool enabled;
        BC_STATUS status;
        unsigned copied_words, writes, reg_reads, reg_writes;
        uint32_t firmware_words[3], control, reg_address[3], reg_value[3];
        uint32_t *caller_tag;
        uint32_t original_tag, index, seed, empty;
        enum LIST_STATUS list0, list1;
        bool single;
        TX_INPUT_BUFFER_INFO before;
        struct tx_dma_pkt *candidate;
        struct crystalhd_elem *reserved;
    } prep;
} run;

static bool signal_pending(void *task)
{
    (void)task;
    if (run.complete_on_status && context.tx_list_id && QueueHead(&activeq)) {
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
static struct tx_dma_pkt *QueueHead(const struct crystalhd_dioq *queue)
{
    return queue->head == (const struct crystalhd_elem *)&queue->head ?
        NULL : queue->head->data;
}
static struct tx_dma_pkt *QueueNext(const struct crystalhd_dioq *queue)
{
    return !QueueHead(queue) ||
        queue->head->flink == (const struct crystalhd_elem *)&queue->head ?
        NULL : queue->head->flink->data;
}
static unsigned QueueCount(const struct crystalhd_dioq *queue)
{
    return queue->count;
}
static bool QueueContains(const struct crystalhd_dioq *queue,
                          const struct tx_dma_pkt *owned)
{
    return QueueHead(queue) == owned || QueueNext(queue) == owned;
}
static void QueueInvariant(const struct crystalhd_dioq *queue)
{
    const struct crystalhd_elem *sentinel =
        (const struct crystalhd_elem *)&queue->head;
    const struct crystalhd_elem *previous = sentinel;
    const struct crystalhd_elem *elem = queue->head;
    unsigned count = 0;

    Check(queue->sig == BC_LINK_DIOQ_SIG && queue->adp == &adapter &&
          !queue->lock, "actual queue retains its signature, adapter and unlocked state");
    while (elem != sentinel && count < ARRAY_SIZE(node_pool)) {
        Check(elem->blink == previous && previous->flink == elem,
              "actual queue retains reciprocal forward/backward node links");
        previous = elem;
        elem = elem->flink;
        count++;
    }
    Check(elem == sentinel && count == queue->count && queue->tail == previous &&
          previous->flink == sentinel && sentinel->blink == previous,
          "actual queue head, tail, count and sentinel agree after node transfer");
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
    Check(QueueHead(&activeq) == packet0 && packet0->list_tag != 0,
          "IRQ completion finds the published TX owner");
    Check(crystalhd_hw_tx_req_complete(&hardware, packet0->list_tag,
                                      run.completion_status) == BC_STS_SUCCESS,
          "IRQ completion retires the real active request");
}
static void Unlock(unsigned *lock)
{
    Check((lock == &hardware.lock || lock == &freeq.lock || lock == &activeq.lock) &&
          *lock == 1, "hardware or actual ownership queue releases its spinlock");
    *lock = 0;
    if (lock == &hardware.lock && run.immediate_completion && QueueHead(&activeq)) {
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
    Check(event && QueueHead(&freeq) == packet0 && !QueueHead(&activeq) &&
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
          ((!QueueHead(&activeq) && !QueueNext(&activeq)) || hardware.dma_fault),
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
          ((!QueueHead(&activeq) && !QueueNext(&activeq)) || hardware.dma_fault),
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
static struct crystalhd_elem *crystalhd_alloc_elem(struct crystalhd_adp *adp)
{
    Check(adp == &adapter, "generic queue nodes use the same adapter pool");
    run.node_attempts++;
    if (run.allocator_exhausted)
        return NULL;
    for (unsigned i = 0; i < ARRAY_SIZE(node_pool); i++) {
        if (!node_allocated[i]) {
            node_allocated[i] = true;
            node_pool[i] = (struct crystalhd_elem){0};
            run.node_allocs++;
            return &node_pool[i];
        }
    }
    return NULL;
}
static void crystalhd_free_elem(struct crystalhd_adp *adp,
                                struct crystalhd_elem *elem)
{
    Check(adp == &adapter && elem >= node_pool &&
          elem < node_pool + ARRAY_SIZE(node_pool),
          "generic queue node returns to its own adapter pool");
    unsigned index = (unsigned)(elem - node_pool);
    Check(node_allocated[index], "a generic queue node is freed exactly once");
    node_allocated[index] = false;
    run.node_frees++;
}

void crystalhd_dioq_add_elem_actual(struct crystalhd_dioq *,
                                   struct crystalhd_elem *, bool, uint32_t);
#include "tx-admission-queue.h"

static struct crystalhd_elem *crystalhd_dioq_fetch_elem(struct crystalhd_dioq *queue)
{
    run.queue_fetches++;
    Check(queue == &freeq || queue == &activeq,
          "submission or cancellation fetches from a TX ownership queue");
    return crystalhd_dioq_fetch_elem_actual(queue);
}
static void crystalhd_dioq_add_elem(struct crystalhd_dioq *queue,
                                   struct crystalhd_elem *elem,
                                   bool wake, uint32_t tag)
{
    struct tx_dma_pkt *owned = elem->data;
    run.queue_adds++;
    Check((owned == packet0 || owned == &packet2 || !owned) && QueueCount(queue) < 2 &&
          !wake && !elem->flink && !elem->blink,
          "the same detached node and packet acquire exactly one queue owner");
    if (queue == &activeq) {
        Check(hardware.lock && tag == owned->list_tag && tag,
              "active ownership is published under lock before DMA can start");
    } else if (owned) {
        Check(queue == &freeq && !tag && !owned->buffer && !owned->cb_context &&
              !owned->call_back && !owned->list_tag,
              "free packets retain no request, callback, cookie or tag ownership");
    } else {
        Check(queue == &freeq && !tag,
              "a malformed reserved node is returned unchanged to its original free queue");
    }
    crystalhd_dioq_add_elem_actual(queue, elem, wake, tag);
}
static void *crystalhd_dioq_fetch(struct crystalhd_dioq *queue)
{
    run.queue_fetches++;
    Check(queue == &freeq || queue == &activeq,
          "submission or cancellation fetches from a TX ownership queue");
    return crystalhd_dioq_fetch_actual(queue);
}
static void *crystalhd_dioq_find_and_fetch(struct crystalhd_dioq *queue, uint32_t tag)
{
    Check(queue == &activeq, "completion searches the active ownership queue");
    return crystalhd_dioq_find_and_fetch_actual(queue, tag);
}
static BC_STATUS crystalhd_dioq_add(struct crystalhd_dioq *queue,
                                  void *data, bool wake, uint32_t tag)
{
    struct tx_dma_pkt *owned = data;
    run.queue_adds++;
    Check((owned == packet0 || owned == &packet2) && QueueCount(queue) < 2 && !wake,
          "a packet is returned to exactly one queue");
    Check(queue == &freeq && !tag && !owned->buffer && !owned->cb_context &&
          !owned->call_back && !owned->list_tag,
          "free packets retain no request, callback, cookie or tag ownership");
    if (run.free_add_failures) {
        run.free_add_failures--;
        return run.free_add_status;
    }
    return crystalhd_dioq_add_actual(queue, data, wake, tag);
}
static void QueueInit(struct crystalhd_dioq *queue)
{
    *queue = (struct crystalhd_dioq){ .sig = BC_LINK_DIOQ_SIG, .adp = &adapter };
    queue->head = queue->tail = (struct crystalhd_elem *)&queue->head;
}
static void QueueSeed(struct crystalhd_dioq *queue, struct tx_dma_pkt *owned)
{
    struct crystalhd_elem *elem = crystalhd_alloc_elem(&adapter);
    if (!elem) abort();
    elem->data = owned;
    crystalhd_dioq_add_elem_actual(queue, elem, false, 0);
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
    Check(adp == &adapter && dio == &request && run.mapped && !QueueHead(&activeq) &&
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
    Check(QueueHead(&freeq) == packet0 && !QueueHead(&activeq) && !packet0->buffer &&
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
        Check(!QueueHead(&activeq) && !QueueNext(&activeq),
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

static BC_STATUS FleaRead(struct crystalhd_hw *hw, uint32_t address,
                          uint32_t words, uint32_t *data)
{
    Check(hw == &hardware && address == hardware.TxBuffInfoAddr && words == 6,
          "the actual TX notification reads exactly six DWORDs from its capture address");
    Check(!hardware.lock && !hardware.dma_fault,
          "notification guarding introduces neither a hardware lock nor a fault latch");
    for (unsigned i = 0; i < 8; i++)
        Check(data[i] == 0, "the whole local notification is initialized before a fallible read");
    Check(run.flea.copied_words <= words,
          "the read boundary copies at most the requested nonreserved words");
    if (run.flea.copied_words > words)
        abort();
    memcpy(data, &run.flea.payload, run.flea.copied_words * sizeof(uint32_t));
    run.flea.reads++;
    return run.flea.status;
}

static BC_STATUS FleaWrite(struct crystalhd_hw *hw, uint32_t address,
                           uint32_t words, const uint32_t *data)
{
    (void)hw; (void)address; (void)words; (void)data;
    run.flea.writes++;
    Check(false, "TX notification tests never request firmware WRAP or write memory");
    return BC_STS_ERROR;
}

static bool crystalhd_flea_wake_up_hw(struct crystalhd_hw *hw)
{
    Check(hw == &hardware, "FIFO wake targets its own hardware context");
    run.flea.wakes++;
    Check(run.flea.wake_allowed,
          "only the explicit FIFO wake-boundary tests may request a wake");
    hw->WakeUpDecodeDone = run.flea.wake_success;
    return run.flea.wake_success;
}

#include "tx-admission-flea.h"
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
static BC_STATUS Prepare(struct crystalhd_hw *hw, uint32_t bytes)
{
    Check(hw == &hardware && hardware.lock && bytes == run.transfer_size,
          "optional TX preparation runs under the publication lock with mapped length");
    return run.prep_status;
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
    endpoint.device = BC_PCI_DEVID_FLEA;
    memset(&run, 0, sizeof(run));
    memset(&request, 0, sizeof(request));
    memset(node_pool, 0, sizeof(node_pool));
    memset(node_allocated, 0, sizeof(node_allocated));
    QueueInit(&freeq);
    QueueInit(&activeq);
    QueueSeed(&freeq, packet0);
    run.node_allocs = run.node_attempts = 0;
    memset(multi_cookie, 0, sizeof(multi_cookie));
    adapter = (struct crystalhd_adp){
        .pdev = &endpoint, .present = true, .user_lock = 1 };
    hardware = (struct crystalhd_hw){ .adp = &adapter,
        .tx_freeq = &freeq, .tx_actq = &activeq, .tx_ioq_tag_seed = 0x100,
        .pfnCheckInputFIFO = Fifo, .pfnStartTxDMA = Start, .pfnStopTxDMA = Stop,
        .pfnDoFirmwareCmd = Firmware, .pfnIssuePause = Pause, .fetch_sem = 1,
        .pfnDevDRAMRead = FleaRead, .pfnDevDRAMWrite = FleaWrite,
        .TxBuffInfoAddr = 0x00d35000, .WakeUpDecodeDone = true,
        .FleaPowerState = FLEA_PS_ACTIVE,
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
    Check(!run.mapped && !QueueHead(&activeq) && QueueHead(&freeq) == packet0 &&
          !QueueNext(&activeq) && !QueueNext(&freeq) && !context.tx_list_id &&
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
          !run.starts && QueueHead(&freeq) == packet0 && !QueueHead(&activeq),
          "empty mapped TX is rejected before FIFO or packet ownership");
    request.tx_buffer = (struct crystalhd_tx_buffer){
        .bytes = run.transfer_size,
    };
    Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer,
                               bc_proc_in_completion, &event, &tag, 0) ==
              BC_STS_INV_ARG &&
          tag == 0xfeed && !run.fifo_calls && !run.descriptors &&
          !run.starts && QueueHead(&freeq) == packet0 && !QueueHead(&activeq),
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
    Check(!run.mapped && !QueueHead(&activeq) && !QueueNext(&activeq) &&
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
        QueueSeed(&freeq, &packet2);
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
        QueueSeed(&freeq, &packet2);
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
              !QueueHead(&activeq) && !QueueNext(&activeq) && QueueCount(&freeq) == 1 &&
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
            QueueSeed(&freeq, &packet2);
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
    Reset();
    struct crystalhd_elem *reserved = crystalhd_dioq_fetch_elem_actual(&freeq);
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_INSUFF_RES &&
          !run.descriptors && !run.starts && run.unmaps == 1,
          "an exhausted packet pool rejects input without building or starting DMA");
    crystalhd_dioq_add_elem_actual(&freeq, reserved, false, 0);
    Balanced();

    Reset(); run.descriptor_status = BC_STS_NOT_IMPL;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_NOT_IMPL && !run.starts,
          "descriptor failure returns the free packet without starting DMA");
    Balanced();
    Reset();
    request.tx_buffer = (struct crystalhd_tx_buffer){
        .bytes = run.transfer_size, .cookie = &request,
    };
    uint32_t tag = 0;
    run.allocator_exhausted = true;
    Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, OpaqueComplete,
                               &opaque_cookie, &tag, run.transfer_flags) ==
              BC_STS_SUCCESS && run.starts == 1 && !run.node_attempts &&
          activeq.head == &node_pool[0] && QueueCount(&freeq) == 0,
          "reserved-node commit succeeds without allocation even when the generic pool is exhausted");
    run.allocator_exhausted = false;
    Check(crystalhd_hw_tx_req_complete(&hardware, tag, BC_STS_SUCCESS) ==
              BC_STS_SUCCESS && run.callback_calls == 1 &&
          !packet0->buffer && !packet0->cb_context && !packet0->call_back,
          "allocation-free active commit still retires callback ownership exactly once");
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
        if (which == 1) {
            run.prep_status = BC_STS_INSUFF_RES;
            hardware.pfnPrepareTxDMA = Prepare;
        }
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
          !QueueHead(&activeq) && QueueHead(&freeq) == packet0 && !packet0->buffer &&
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
          !QueueHead(&activeq) && !QueueNext(&activeq) && !QueueHead(&freeq) && !QueueNext(&freeq),
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

static TX_INPUT_BUFFER_INFO FleaNotificationReset(void)
{
    Reset();
    hardware.TxFwInputBuffInfo = (TX_INPUT_BUFFER_INFO){
        .DramBuffAdd = 0x8000, .DramBuffSzInBytes = 0x40000,
        .HostXferSzInBytes = 0x87654321, .Flags = 0xa5a50000,
        .SeqNum = 0xfedcba98, .ChannelID = 0x76543210,
        .Reserved = { 0xcafebabe, 0xdeadbeef },
    };
    run.flea.payload = (TX_INPUT_BUFFER_INFO){
        .DramBuffAdd = 0xa000, .DramBuffSzInBytes = 0x80000,
        .HostXferSzInBytes = 0x10203040, .Flags = 0x80000000,
        .SeqNum = UINT32_MAX, .ChannelID = 0x11111111,
        .Reserved = { 0x22222222, 0x33333333 },
    };
    run.flea.copied_words = 6;
    return hardware.TxFwInputBuffInfo;
}

static void FleaBlockedPost(TX_INPUT_BUFFER_INFO before)
{
    uint32_t empty = UINT32_MAX, tag = 0xfeedbabe;
    uint8_t flags = 0x04;
    struct tx_dma_pkt unposted;

    memcpy(&unposted, packet0, sizeof(unposted));

    before.DramBuffAdd = 0;
    before.DramBuffSzInBytes = 0;
    Check(memcmp(&hardware.TxFwInputBuffInfo, &before, sizeof(before)) == 0,
          "read failure invalidates only cached address and size, preserving every opaque word");
    Check(hardware.WakeUpDecodeDone && hardware.FleaPowerState == FLEA_PS_ACTIVE &&
          crystalhd_flea_check_input_full(&hardware, run.transfer_size, &empty,
                                          false, &flags) && !empty && !flags,
          "the real wake-completed ACTIVE FIFO rejects the invalidated cached space");
    hardware.pfnCheckInputFIFO = crystalhd_flea_check_input_full;
    request.tx_buffer = (struct crystalhd_tx_buffer){
        .bytes = run.transfer_size, .cookie = &request,
        .ops = &crystalhd_dio_tx_buffer_ops,
    };
    Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, OpaqueComplete,
                               &opaque_cookie, &tag, 0) == BC_STS_BUSY &&
          tag == 0xfeedbabe && hardware.stats.cin_busy == 1,
          "the actual post path reports BUSY before assigning a list tag");
    Check(!run.queue_fetches && !run.queue_adds && !run.descriptors && !run.starts &&
          !run.callback_calls && !QueueHead(&activeq) && !QueueNext(&activeq) &&
          QueueHead(&freeq) == packet0 && !QueueNext(&freeq) &&
          memcmp(packet0, &unposted, sizeof(unposted)) == 0 &&
          !hardware.tx_list_post_index &&
          hardware.TxList0Sts == ListStsFree && hardware.TxList1Sts == ListStsFree,
          "BUSY from the actual FIFO leaves descriptor, queue and callback ownership untouched");
    Check(!hardware.lock && !hardware.dma_fault && !hardware.EmptyCnt &&
          !hardware.SingleThreadAppFIFOEmpty && !run.flea.wakes && !run.flea.writes,
          "notification rejection neither wakes hardware nor adds locks, faults, WRAP writes or FIFO reservations");
}

static void FleaWakeAdmission(void)
{
    for (unsigned low_power = 0; low_power < 2; low_power++) {
        for (unsigned single = 0; single < 2; single++) {
            TX_INPUT_BUFFER_INFO before = FleaNotificationReset();
            struct tx_dma_pkt unposted = *packet0;
            uint32_t empty = UINT32_MAX, tag = 0xfeedbabe;
            uint8_t flags = 0;

            hardware.WakeUpDecodeDone = false;
            hardware.FleaPowerState = low_power ? FLEA_PS_LP_COMPLETE : FLEA_PS_ACTIVE;
            hardware.SingleThreadAppFIFOEmpty = single;
            run.flea.wake_allowed = true;
            Check(crystalhd_flea_check_input_full(&hardware, run.transfer_size,
                      &empty, false, &flags) && !empty && !flags &&
                  run.flea.wakes == 1 && !hardware.WakeUpDecodeDone &&
                  memcmp(&before, &hardware.TxFwInputBuffInfo, sizeof(before)) == 0 &&
                  !hardware.EmptyCnt && hardware.SingleThreadAppFIFOEmpty == !!single,
                  "failed wake blocks the actual FIFO despite cached space or a single-thread reservation");
            hardware.pfnCheckInputFIFO = crystalhd_flea_check_input_full;
            request.tx_buffer = (struct crystalhd_tx_buffer){
                .bytes = run.transfer_size, .cookie = &request,
                .ops = &crystalhd_dio_tx_buffer_ops,
            };
            for (unsigned retry = 0; retry < 3; retry++) {
                Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer,
                          OpaqueComplete, &opaque_cookie, &tag, 0) == BC_STS_BUSY &&
                      tag == 0xfeedbabe && run.flea.wakes == retry + 2 &&
                      hardware.stats.cin_busy == retry + 1 &&
                      !run.queue_fetches && !run.queue_adds && !run.descriptors &&
                      !run.starts && !run.callback_calls && !QueueHead(&activeq) &&
                      QueueHead(&freeq) == packet0 && !QueueNext(&freeq) &&
                      !memcmp(packet0, &unposted, sizeof(unposted)) &&
                      !hardware.tx_list_post_index && !hardware.lock &&
                      !hardware.dma_fault && !hardware.WakeUpDecodeDone &&
                      !run.flea.reads && !run.flea.writes,
                      "each explicit failed-wake post is BUSY before descriptor or queue ownership admission");
            }
            /* A successful boundary is not a silicon readiness certificate. */
            run.flea.wake_success = true;
            hardware.FleaPowerState = FLEA_PS_ACTIVE;
            Check(!crystalhd_flea_check_input_full(&hardware, run.transfer_size,
                      &empty, false, &flags) && empty == before.DramBuffSzInBytes &&
                  flags == 0x80 && run.flea.wakes == 5 && hardware.WakeUpDecodeDone,
                  "explicit successful wake retry resumes the existing cached FIFO admission policy");
            run.mapped = true;
            request.uinfo.xfr_len = run.transfer_size;
            Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer,
                      OpaqueComplete, &opaque_cookie, &tag, 0) == BC_STS_SUCCESS &&
                  tag == 0x100 && run.starts == 1 && run.flea.wakes == 5,
                  "actual TX post can transfer ownership after, but not before, successful wake");
            Complete();
            Check(run.callback_calls == 1 &&
                  run.seen_callback_context == &opaque_cookie &&
                  run.seen_callback_status == BC_STS_SUCCESS,
                  "successful retry completes the exact TX owner once");
            run.mapped = false;
            Balanced();
        }
    }

    /* Preserve the pre-existing non-posting buffer-size query shortcut. */
    FleaNotificationReset();
    hardware.WakeUpDecodeDone = false;
    uint32_t empty = UINT32_MAX;
    uint8_t flags = 0x04;
    Check(!crystalhd_flea_check_input_full(&hardware, run.transfer_size, &empty,
               false, &flags) && empty == 2 * 1024 * 1024 && !flags &&
          !run.flea.wakes && !hardware.WakeUpDecodeDone && !run.descriptors &&
          !run.starts && !run.queue_fetches && !run.queue_adds,
          "status-only FIFO query retains its no-wake shortcut without posting TX");
}

static void FleaNotificationGuard(void)
{
    static const BC_STATUS errors[] = {
        BC_STS_BUSY, BC_STS_ERROR, BC_STS_INV_ARG, BC_STS_IO_ERROR,
    };
    static const unsigned copied[] = { 0, 1, 3, 5, 6 };
    TX_INPUT_BUFFER_INFO before, expected;

    for (unsigned error = 0; error < ARRAY_SIZE(errors); error++) {
        for (unsigned shape = 0; shape < ARRAY_SIZE(copied); shape++) {
            before = FleaNotificationReset();
            run.flea.status = errors[error];
            run.flea.copied_words = copied[shape];
            crystalhd_flea_update_tx_buff_info(&hardware);
            Check(run.flea.reads == 1, "a failed notification is read once without retrying");
            FleaBlockedPost(before);
        }
    }

    before = FleaNotificationReset();
    run.flea.payload = (TX_INPUT_BUFFER_INFO){0};
    run.flea.status = BC_STS_ERROR;
    crystalhd_flea_update_tx_buff_info(&hardware);
    FleaBlockedPost(before);

    for (unsigned low = 1; low < 4; low++) {
        before = FleaNotificationReset();
        run.flea.payload.DramBuffAdd |= low;
        crystalhd_flea_update_tx_buff_info(&hardware);
        FleaBlockedPost(before);
    }

    for (unsigned zero = 0; zero < 4; zero++) {
        uint32_t empty = UINT32_MAX;
        uint8_t flags = 0x04;

        before = FleaNotificationReset();
        if (zero & 1U)
            run.flea.payload.DramBuffAdd = 0;
        if (zero & 2U)
            run.flea.payload.DramBuffSzInBytes = 0;
        expected = before;
        expected.DramBuffAdd = run.flea.payload.DramBuffAdd;
        expected.DramBuffSzInBytes = run.flea.payload.DramBuffSzInBytes;
        expected.HostXferSzInBytes = run.flea.payload.HostXferSzInBytes;
        expected.Flags = run.flea.payload.Flags;
        expected.SeqNum = run.flea.payload.SeqNum;
        crystalhd_flea_update_tx_buff_info(&hardware);
        Check(memcmp(&hardware.TxFwInputBuffInfo, &expected, sizeof(expected)) == 0,
              "successful six-word reads copy exactly five fields, not ChannelID or Reserved");
        Check(crystalhd_flea_check_input_full(&hardware, run.transfer_size, &empty,
                                             false, &flags) == (zero != 0) &&
              empty == (zero ? 0U : expected.DramBuffSzInBytes) &&
              flags == (zero ? 0U : 0x84U),
              "successful zero address or size retains the existing real-FIFO admission rules");
        Check(run.flea.reads == 1 && !run.flea.wakes && !run.flea.writes &&
              !hardware.EmptyCnt && !hardware.lock && !hardware.dma_fault,
              "successful notification and status-only FIFO queries have no new device effects");
    }

    before = FleaNotificationReset();
    run.flea.payload.DramBuffAdd = 0xfffffffc;
    run.flea.payload.DramBuffSzInBytes = UINT32_MAX;
    run.flea.payload.HostXferSzInBytes = UINT32_MAX;
    run.flea.payload.Flags = 0xfffffffe;
    run.flea.payload.SeqNum = UINT32_MAX;
    crystalhd_flea_update_tx_buff_info(&hardware);
    Check(hardware.TxFwInputBuffInfo.DramBuffAdd == 0xfffffffc &&
          hardware.TxFwInputBuffInfo.DramBuffSzInBytes == UINT32_MAX &&
          hardware.TxFwInputBuffInfo.HostXferSzInBytes == UINT32_MAX &&
          hardware.TxFwInputBuffInfo.Flags == 0xfffffffe &&
          hardware.TxFwInputBuffInfo.SeqNum == UINT32_MAX &&
          hardware.TxFwInputBuffInfo.ChannelID == before.ChannelID &&
          memcmp(hardware.TxFwInputBuffInfo.Reserved, before.Reserved,
                 sizeof(before.Reserved)) == 0,
          "successful notification preserves full u32 values without adding range or field policy");

    before = FleaNotificationReset();
    run.flea.status = BC_STS_ERROR;
    crystalhd_flea_update_tx_buff_info(&hardware);
    FleaBlockedPost(before);
    run.flea.status = BC_STS_SUCCESS;
    crystalhd_flea_update_tx_buff_info(&hardware);
    Check(run.flea.reads == 2 &&
          hardware.TxFwInputBuffInfo.DramBuffAdd == run.flea.payload.DramBuffAdd &&
          hardware.TxFwInputBuffInfo.DramBuffSzInBytes == run.flea.payload.DramBuffSzInBytes,
          "a later complete valid notification restores FIFO availability without a fault latch");
    uint32_t tag = 0;
    Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, OpaqueComplete,
                               &opaque_cookie, &tag, 0) == BC_STS_SUCCESS &&
          run.descriptors == 1 && run.starts == 1 &&
          run.seen_destination == run.flea.payload.DramBuffAdd &&
          tag == hardware.tx_ioq_tag_seed && QueueHead(&activeq) == packet0,
          "the actual FIFO and post path admit new work after a valid notification");
    Check(crystalhd_hw_tx_req_complete(&hardware, tag, BC_STS_SUCCESS) == BC_STS_SUCCESS &&
          run.callback_calls == 1 && !QueueHead(&activeq) && QueueHead(&freeq) == packet0 &&
          !packet0->buffer && !packet0->cb_context && !packet0->call_back &&
          !run.flea.writes && !run.flea.wakes && !hardware.dma_fault,
          "recovered post completion retains existing exactly-once callback ownership");
}

static void FleaAbortNotification(void)
{
    for (unsigned success = 0; success < 2; success++) {
        TX_INPUT_BUFFER_INFO before = FleaNotificationReset();
        uint32_t empty = UINT32_MAX, tag = 0xfeedbabe;
        uint8_t flags = 0x04;

        if (success) {
            run.flea.payload.Flags |= DFW_FLAGS_TX_ABORT;
            before.DramBuffAdd = run.flea.payload.DramBuffAdd;
            before.DramBuffSzInBytes = run.flea.payload.DramBuffSzInBytes;
            before.HostXferSzInBytes = run.flea.payload.HostXferSzInBytes;
            before.Flags = run.flea.payload.Flags;
            before.SeqNum = run.flea.payload.SeqNum;
        } else {
            hardware.TxFwInputBuffInfo.Flags |= DFW_FLAGS_TX_ABORT;
            before.Flags |= DFW_FLAGS_TX_ABORT;
            before.DramBuffAdd = before.DramBuffSzInBytes = 0;
            run.flea.status = BC_STS_ERROR;
        }
        crystalhd_flea_update_tx_buff_info(&hardware);
        Check(memcmp(&hardware.TxFwInputBuffInfo, &before, sizeof(before)) == 0,
              "failed reads preserve cached ABORT and successful reads retain the firmware ABORT bit");
        Check(crystalhd_flea_check_input_full(&hardware, run.transfer_size, &empty,
                                             false, &flags) && !empty && flags == 0x05,
              "the actual wake-completed ACTIVE FIFO retains ABORT flags, not a fabricated zero flag");
        hardware.pfnCheckInputFIFO = crystalhd_flea_check_input_full;
        request.tx_buffer = (struct crystalhd_tx_buffer){
            .bytes = run.transfer_size, .cookie = &request,
            .ops = &crystalhd_dio_tx_buffer_ops,
        };
        Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, OpaqueComplete,
                                   &opaque_cookie, &tag, 0) == BC_STS_BUSY &&
              tag == 0xfeedbabe && hardware.stats.cin_busy == 1 &&
              !run.descriptors && !run.starts && !run.queue_fetches && !run.queue_adds &&
              !run.callback_calls && !QueueHead(&activeq) && !QueueNext(&activeq) &&
              QueueHead(&freeq) == packet0 && !QueueNext(&freeq) &&
              !packet0->buffer && !packet0->cb_context && !packet0->call_back &&
              !packet0->list_tag && !hardware.lock && !hardware.dma_fault &&
              !hardware.EmptyCnt && !run.flea.writes && !run.flea.wakes,
              "ABORT prevents descriptor, queue and DMA ownership admission without new side effects");
    }
}

static void FleaNotificationWithActiveOwner(void)
{
    TX_INPUT_BUFFER_INFO before = FleaNotificationReset();
    struct tx_dma_pkt owned;
    uint32_t tag = 0, unposted_tag = 0xfeedbabe;

    request.tx_buffer = (struct crystalhd_tx_buffer){
        .bytes = run.transfer_size, .cookie = &request,
        .ops = &crystalhd_dio_tx_buffer_ops,
    };
    Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, OpaqueComplete,
                               &opaque_cookie, &tag, run.transfer_flags) == BC_STS_SUCCESS,
          "the notification ownership test begins with an already-admitted TX owner");
    before.HostXferSzInBytes = run.transfer_size;
    before.DramBuffAdd = before.DramBuffSzInBytes = 0;
    memcpy(&owned, packet0, sizeof(owned));
    run.flea.status = BC_STS_ERROR;
    crystalhd_flea_update_tx_buff_info(&hardware);
    Check(memcmp(&hardware.TxFwInputBuffInfo, &before, sizeof(before)) == 0 &&
          memcmp(packet0, &owned, sizeof(owned)) == 0 && QueueHead(&activeq) == packet0 &&
          !QueueNext(&activeq) && !QueueHead(&freeq) && !QueueNext(&freeq) && !run.stops &&
          !run.callback_calls && !hardware.dma_fault,
          "notification failure does not cancel, retire or alter already-admitted TX ownership");
    hardware.pfnCheckInputFIFO = crystalhd_flea_check_input_full;
    request2.tx_buffer = (struct crystalhd_tx_buffer){
        .bytes = run.transfer_size, .cookie = &request2,
        .ops = &crystalhd_dio_tx_buffer_ops,
    };
    Check(crystalhd_hw_post_tx(&hardware, &request2.tx_buffer, OpaqueComplete,
                               &opaque_cookie, &unposted_tag, 0) == BC_STS_BUSY &&
          unposted_tag == 0xfeedbabe && run.descriptors == 1 && run.starts == 1 &&
          run.queue_fetches == 1 && run.queue_adds == 1 &&
          memcmp(packet0, &owned, sizeof(owned)) == 0 && QueueHead(&activeq) == packet0,
          "invalidated availability blocks only new work before descriptor or queue admission");
    Check(crystalhd_hw_tx_req_complete(&hardware, tag, BC_STS_SUCCESS) == BC_STS_SUCCESS &&
          run.callback_calls == 1 && !QueueHead(&activeq) && QueueHead(&freeq) == packet0 &&
          !run.flea.writes && !run.flea.wakes && !run.stops && !hardware.dma_fault,
          "an already-admitted owner can still complete normally after a failed notification");
}

static void PrepareUnpublished(void)
{
    struct tx_dma_pkt *owned = run.prep.candidate;
    Check(run.prep.enabled && hardware.lock && run.prep.caller_tag &&
          *run.prep.caller_tag == run.prep.original_tag &&
          hardware.tx_list_post_index == run.prep.index &&
          hardware.tx_ioq_tag_seed == run.prep.seed &&
          hardware.TxList0Sts == run.prep.list0 && hardware.TxList1Sts == run.prep.list1 &&
          hardware.TxFwInputBuffInfo.HostXferSzInBytes == run.prep.before.HostXferSzInBytes,
          "preparation precedes caller tag, list state, index and cached transfer-length publication");
    Check(!owned->buffer && !owned->retained_buffer && !owned->cb_context &&
          !owned->call_back && !owned->list_tag &&
          !QueueContains(&freeq, owned) && !QueueContains(&activeq, owned) &&
          run.prep.reserved->data == owned && !run.prep.reserved->flink &&
          !run.prep.reserved->blink && !run.node_attempts && !run.node_frees,
          "preparation owns the detached original node without publishing request references or using the allocator");
}

static BC_STATUS FleaPrepareWrite(struct crystalhd_hw *hw, uint32_t address,
                                  uint32_t words, const uint32_t *data)
{
    TX_INPUT_BUFFER_INFO expected = run.prep.before;
    PrepareUnpublished();
    expected.DramBuffAdd = expected.DramBuffSzInBytes = 0;
    Check(hw == &hardware && address == hardware.TxBuffInfoAddr && words == 3 &&
          !data[0] && !data[1] && data[2] == run.transfer_size,
          "actual Flea preparation writes exactly the original three-DWORD zero/zero/length wire record");
    for (unsigned word = 3; word < 8; word++)
        Check(!data[word], "the entire fallible-write source record is initialized");
    Check(memcmp(&hardware.TxFwInputBuffInfo, &expected, sizeof(expected)) == 0 &&
          hardware.EmptyCnt == run.prep.empty &&
          hardware.SingleThreadAppFIFOEmpty == run.prep.single,
          "preparation invalidates only cached capacity and preserves FIFO counters, single-thread state and opaque words");
    Check(run.prep.copied_words <= words, "partial writes never exceed the requested record");
    if (run.prep.copied_words > words) abort();
    memcpy(run.prep.firmware_words, data, run.prep.copied_words * sizeof(uint32_t));
    run.prep.writes++;
    return run.prep.status;
}

static BC_STATUS FleaPrepare(struct crystalhd_hw *hw, uint32_t bytes)
{
    PrepareUnpublished();
    Check(bytes == run.transfer_size &&
          memcmp(&hardware.TxFwInputBuffInfo, &run.prep.before,
                 sizeof(run.prep.before)) == 0,
          "actual Flea helper receives the still-uncommitted cached record and complete mapped byte count");
    return crystalhd_flea_prepare_tx_dma(hw, bytes);
}

static void PreparedPublished(void)
{
    struct tx_dma_pkt *owned = run.prep.candidate;
    Check(hardware.lock && run.prep.writes == 1 &&
          run.prep.status == BC_STS_SUCCESS &&
          activeq.tail == run.prep.reserved && run.prep.reserved->data == owned &&
          QueueContains(&activeq, owned) && owned->buffer && owned->cb_context == owned &&
          owned->call_back && owned->list_tag == run.prep.seed + run.prep.index &&
          *run.prep.caller_tag == owned->list_tag &&
          hardware.tx_list_post_index == (run.prep.index + 1) % DMA_ENGINE_CNT &&
          hardware.TxFwInputBuffInfo.HostXferSzInBytes == run.transfer_size &&
          (run.prep.index ? (hardware.TxList1Sts & TxListWaitingForIntr) :
                            (hardware.TxList0Sts & TxListWaitingForIntr)),
          "register access follows successful metadata preparation and same-node active ownership publication");
}

static uint32_t FleaRegisterRead(struct crystalhd_adp *adp, uint32_t address)
{
    PreparedPublished();
    Check(adp == &adapter && address == BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS &&
          !run.prep.reg_reads && !run.prep.reg_writes &&
          hardware.EmptyCnt == run.prep.empty - 1U &&
          !hardware.SingleThreadAppFIFOEmpty,
          "actual register-only start preserves its existing counter decrement and reads control before writing descriptors");
    run.prep.reg_reads++;
    return run.prep.control;
}

static void FleaRegisterWrite(struct crystalhd_adp *adp, uint32_t address, uint32_t value)
{
    PreparedPublished();
    Check(adp == &adapter && run.prep.reg_reads == 1 && run.prep.reg_writes < 3,
          "register-only start writes a bounded sequence after its control read");
    if (run.prep.reg_writes >= 3) abort();
    run.prep.reg_address[run.prep.reg_writes] = address;
    run.prep.reg_value[run.prep.reg_writes++] = value;
}

static void PreparedComplete(void *cookie, BC_STATUS status)
{
    struct tx_dma_pkt *owned = cookie;
    Check((owned == packet0 || owned == &packet2) &&
          QueueContains(&freeq, owned) && !QueueContains(&activeq, owned) &&
          !owned->buffer && !owned->cb_context && !owned->call_back && !owned->list_tag,
          "prepared ownership retires before its exact opaque callback executes");
    run.callback_calls++;
    run.seen_callback_context = cookie;
    run.seen_callback_status = status;
}

static void FleaStart(struct crystalhd_hw *hw, uint8_t index, addr_64 descriptor)
{
    PreparedPublished();
    Check(hw == &hardware && index == run.prep.index &&
          descriptor.full_addr == run.prep.candidate->desc_mem.phy_addr,
          "the actual register-only start receives the selected list and original descriptor address");
    run.starts++;
    crystalhd_flea_start_tx_dma_engine(hw, index, descriptor);
    Check(run.prep.writes == 1, "register-only start never duplicates the metadata DRAM write");
}

static void PrepareSnapshot(uint32_t *tag, struct tx_dma_pkt *candidate)
{
    run.prep.enabled = true;
    run.prep.caller_tag = tag;
    run.prep.original_tag = *tag;
    run.prep.candidate = candidate;
    run.prep.reserved = freeq.head;
    run.prep.index = hardware.tx_list_post_index;
    run.prep.seed = hardware.tx_ioq_tag_seed;
    run.prep.empty = hardware.EmptyCnt;
    run.prep.single = hardware.SingleThreadAppFIFOEmpty;
    run.prep.list0 = hardware.TxList0Sts;
    run.prep.list1 = hardware.TxList1Sts;
    run.prep.before = hardware.TxFwInputBuffInfo;
    run.prep.writes = run.prep.reg_reads = run.prep.reg_writes = 0;
    run.prep.firmware_words[0] = 0x11111111;
    run.prep.firmware_words[1] = 0x22222222;
    run.prep.firmware_words[2] = 0x33333333;
    run.node_attempts = run.node_allocs = run.node_frees = 0;
}

static void FleaPrepareReset(uint32_t *tag)
{
    (void)FleaNotificationReset();
    hardware.EmptyCnt = 7;
    hardware.SingleThreadAppFIFOEmpty = true;
    hardware.pfnPrepareTxDMA = FleaPrepare;
    hardware.pfnStartTxDMA = FleaStart;
    hardware.pfnDevDRAMWrite = FleaPrepareWrite;
    hardware.pfnReadFPGARegister = FleaRegisterRead;
    hardware.pfnWriteFPGARegister = FleaRegisterWrite;
    request.tx_buffer = (struct crystalhd_tx_buffer){
        .bytes = run.transfer_size, .cookie = &request,
    };
    run.prep.copied_words = 3;
    PrepareSnapshot(tag, packet0);
}

static void PreparationRejected(uint32_t tag, const struct tx_dma_pkt *before)
{
    TX_INPUT_BUFFER_INFO expected = run.prep.before;
    expected.DramBuffAdd = expected.DramBuffSzInBytes = 0;
    Check(tag == run.prep.original_tag && freeq.head == run.prep.reserved &&
          QueueHead(&freeq) == run.prep.candidate && !QueueHead(&activeq) &&
          QueueCount(&freeq) == 1 && !QueueCount(&activeq) &&
          memcmp(run.prep.candidate, before, sizeof(*before)) == 0 &&
          hardware.tx_list_post_index == run.prep.index &&
          hardware.tx_ioq_tag_seed == run.prep.seed &&
          hardware.TxList0Sts == run.prep.list0 && hardware.TxList1Sts == run.prep.list1 &&
          !run.starts && !run.prep.reg_reads && !run.prep.reg_writes &&
          !run.callback_calls && !run.node_attempts && !run.node_frees,
          "failed preparation restores the identical node and packet without tag/list/index, DMA, callback or allocation publication");
    Check(memcmp(&hardware.TxFwInputBuffInfo, &expected, sizeof(expected)) == 0 &&
          hardware.EmptyCnt == run.prep.empty &&
          hardware.SingleThreadAppFIFOEmpty == run.prep.single &&
          !hardware.lock && !hardware.dma_fault,
          "failed preparation preserves opaque metadata, old cached length and existing FIFO policy without a fatal latch");
    QueueInvariant(&freeq);
    QueueInvariant(&activeq);
}

static void PreparationQueueValidation(void)
{
    for (unsigned invalid = 0; invalid < 9; invalid++) {
        uint32_t tag = 0xfeedbabe;
        FleaPrepareReset(&tag);
        struct crystalhd_dioq free_before = freeq, active_before = activeq;
        struct tx_dma_pkt packet_before = *packet0;
        TX_INPUT_BUFFER_INFO metadata_before = hardware.TxFwInputBuffInfo;
        if (invalid == 0) hardware.adp = NULL;
        if (invalid == 1) adapter.pdev = NULL;
        if (invalid == 2) hardware.tx_freeq = NULL;
        if (invalid == 3) hardware.tx_actq = NULL;
        if (invalid == 4) hardware.tx_actq = hardware.tx_freeq;
        if (invalid == 5) freeq.sig = 0;
        if (invalid == 6) activeq.sig = 0;
        if (invalid == 7) freeq.adp = NULL;
        if (invalid == 8) activeq.adp = NULL;
        Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, PreparedComplete,
                                   packet0, &tag, run.transfer_flags) == BC_STS_INV_ARG &&
              tag == 0xfeedbabe && !run.fifo_calls && !run.queue_fetches &&
              !run.queue_adds && !run.descriptors && !run.starts &&
              !run.prep.writes && !run.node_attempts && !run.callback_calls &&
              memcmp(packet0, &packet_before, sizeof(packet_before)) == 0 &&
              memcmp(&hardware.TxFwInputBuffInfo, &metadata_before,
                     sizeof(metadata_before)) == 0,
              "invalid adapter, queue, signature or pool identity is rejected before FIFO and every firmware/ownership side effect");
        freeq.sig = free_before.sig;
        activeq.sig = active_before.sig;
        freeq.adp = free_before.adp;
        activeq.adp = active_before.adp;
        Check(memcmp(&freeq, &free_before, sizeof(freeq)) == 0 &&
              memcmp(&activeq, &active_before, sizeof(activeq)) == 0,
              "early validation never mutates either actual queue");
        QueueInvariant(&freeq);
        QueueInvariant(&activeq);
    }

    uint32_t tag = 0xfeedbabe;
    FleaPrepareReset(&tag);
    struct crystalhd_elem *reserved = freeq.head;
    struct tx_dma_pkt packet_before = *packet0;
    reserved->data = NULL;
    run.allocator_exhausted = true;
    Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, PreparedComplete,
                               packet0, &tag, run.transfer_flags) == BC_STS_INSUFF_RES &&
          tag == 0xfeedbabe && freeq.head == reserved && freeq.tail == reserved &&
          !reserved->data && QueueCount(&freeq) == 1 && !QueueCount(&activeq) &&
          !run.descriptors && !run.prep.writes && !run.starts &&
          !run.node_attempts && !run.node_frees && !run.callback_calls &&
          memcmp(packet0, &packet_before, sizeof(packet_before)) == 0,
          "a reserved node with NULL data returns unchanged without allocation, preparation or lost queue ownership");
    QueueInvariant(&freeq);
    QueueInvariant(&activeq);

    FleaPrepareReset(&tag);
    reserved = crystalhd_dioq_fetch_elem_actual(&freeq);
    run.allocator_exhausted = true;
    Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, PreparedComplete,
                               packet0, &tag, run.transfer_flags) == BC_STS_INSUFF_RES &&
          !run.prep.writes && !run.descriptors && !run.starts && !run.node_attempts,
          "an actually empty free queue cannot reach firmware preparation");
    crystalhd_dioq_add_elem_actual(&freeq, reserved, false, 0);
    QueueInvariant(&freeq);
    QueueInvariant(&activeq);
}

static void FleaPreparationFailures(void)
{
    static const BC_STATUS errors[] = {
        BC_STS_BUSY, BC_STS_ERROR, BC_STS_INV_ARG, BC_STS_IO_ERROR,
    };
    for (unsigned error = 0; error < ARRAY_SIZE(errors); error++) {
        for (unsigned copied = 0; copied <= 3; copied++) {
            uint32_t tag = 0xfeedbabe;
            FleaPrepareReset(&tag);
            struct tx_dma_pkt before = *packet0;
            run.prep.status = errors[error];
            run.prep.copied_words = copied;
            run.allocator_exhausted = true;
            Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, PreparedComplete,
                                       packet0, &tag, run.transfer_flags) == errors[error] &&
                  run.prep.writes == 1 && run.descriptors == 1 &&
                  run.seen_destination == run.prep.before.DramBuffAdd,
                  "every preparation failure and partial-write shape propagates after descriptor construction without retry");
            for (unsigned word = 0; word < 3; word++)
                Check(run.prep.firmware_words[word] == (word < copied ?
                      (word == 2 ? run.transfer_size : 0U) :
                      (word + 1U) * 0x11111111U),
                      "containment does not claim to undo a partially written firmware record");
            PreparationRejected(tag, &before);
        }
    }

    for (unsigned fault = 0; fault < 2; fault++) {
        uint32_t tag = 0xfeedbabe;
        FleaPrepareReset(&tag);
        struct crystalhd_elem *reserved = freeq.head;
        struct tx_dma_pkt before = *packet0;
        run.allocator_exhausted = true;
        if (fault) run.fault_after_descriptor = true;
        else run.descriptor_status = BC_STS_NOT_IMPL;
        Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, PreparedComplete,
                                   packet0, &tag, run.transfer_flags) ==
                  (fault ? BC_STS_IO_ERROR : BC_STS_NOT_IMPL) &&
              tag == 0xfeedbabe && freeq.head == reserved &&
              memcmp(packet0, &before, sizeof(before)) == 0 &&
              !run.prep.writes && !run.starts && !run.node_attempts &&
              !run.node_frees && !run.callback_calls,
              "descriptor or second-fault failure restores the retained node without a failable requeue allocation");
        QueueInvariant(&freeq);
        QueueInvariant(&activeq);
    }
}

static void FleaPreparationSuccess(void)
{
    for (unsigned list = 0; list < 2; list++) {
        for (unsigned running = 0; running < 2; running++) {
            uint32_t tag = 0xfeedbabe;
            FleaPrepareReset(&tag);
            packet0->desc_mem.phy_addr = UINT64_C(0x1234567887654320);
            hardware.tx_list_post_index = list;
            PrepareSnapshot(&tag, packet0);
            run.prep.control = running ? 0x41U : 0x40U;
            run.allocator_exhausted = true;
            Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, PreparedComplete,
                                       packet0, &tag, run.transfer_flags) == BC_STS_SUCCESS &&
                  tag == run.prep.seed + list && run.starts == 1 &&
                  run.prep.writes == 1 && !run.node_attempts && !run.node_frees &&
                  activeq.head == run.prep.reserved && activeq.tail == run.prep.reserved,
                  "successful preparation commits the exact reserved node on either DMA list with an exhausted allocator");
            unsigned first_descriptor = running ? 0 : 1;
            Check(run.prep.reg_writes == first_descriptor + 2 &&
                  (running || (run.prep.reg_address[0] == BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS &&
                               run.prep.reg_value[0] == 0x41U)) &&
                  run.prep.reg_address[first_descriptor] == (list ?
                      BCHP_MISC1_TX_FIRST_DESC_U_ADDR_LIST1 : BCHP_MISC1_TX_FIRST_DESC_U_ADDR_LIST0) &&
                  run.prep.reg_value[first_descriptor] == 0x12345678U &&
                  run.prep.reg_address[first_descriptor + 1] == (list ?
                      BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST1 : BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST0) &&
                  run.prep.reg_value[first_descriptor + 1] == 0x87654321U,
                  "actual start retains control/upper/lower-valid publication order and both full descriptor halves");
            QueueInvariant(&freeq);
            QueueInvariant(&activeq);
            run.allocator_exhausted = false;
            Check(crystalhd_hw_tx_req_complete(&hardware, tag, BC_STS_SUCCESS) == BC_STS_SUCCESS &&
                  run.callback_calls == 1 && run.seen_callback_context == packet0 &&
                  crystalhd_hw_tx_req_complete(&hardware, tag, BC_STS_SUCCESS) == BC_STS_NO_DATA &&
                  run.callback_calls == 1,
                  "successful prepared work completes once and rejects duplicate completion");
            QueueInvariant(&freeq);
            QueueInvariant(&activeq);
            Balanced();
        }
    }
}

static void FleaPreparationRecoveryAndOwner(void)
{
    uint32_t tag = 0xfeedbabe;
    FleaPrepareReset(&tag);
    struct tx_dma_pkt before = *packet0;
    run.prep.status = BC_STS_ERROR;
    run.prep.copied_words = 1;
    Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, PreparedComplete,
                               packet0, &tag, run.transfer_flags) == BC_STS_ERROR,
          "recovery starts with a failed partial metadata preparation");
    PreparationRejected(tag, &before);
    run.flea.status = BC_STS_SUCCESS;
    crystalhd_flea_update_tx_buff_info(&hardware);
    Check(hardware.TxFwInputBuffInfo.DramBuffAdd == run.flea.payload.DramBuffAdd &&
          hardware.TxFwInputBuffInfo.DramBuffSzInBytes == run.flea.payload.DramBuffSzInBytes,
          "a subsequent valid firmware notification supplies fresh capacity after failed preparation");
    PrepareSnapshot(&tag, packet0);
    hardware.pfnCheckInputFIFO = crystalhd_flea_check_input_full;
    run.prep.empty++;
    run.prep.status = BC_STS_SUCCESS;
    run.prep.copied_words = 3;
    Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, PreparedComplete,
                               packet0, &tag, run.transfer_flags) == BC_STS_SUCCESS &&
          crystalhd_hw_tx_req_complete(&hardware, tag, BC_STS_SUCCESS) == BC_STS_SUCCESS &&
          run.callback_calls == 1,
          "fresh notification recovery publishes and completes work without resetting or fault-latching the device");
    QueueInvariant(&freeq);
    QueueInvariant(&activeq);

    tag = 0xfeedbabe;
    FleaPrepareReset(&tag);
    QueueSeed(&freeq, &packet2);
    PrepareSnapshot(&tag, packet0);
    Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, PreparedComplete,
                               packet0, &tag, run.transfer_flags) == BC_STS_SUCCESS,
          "peer-failure coverage starts with an already-active prepared owner");
    struct tx_dma_pkt active_before = *packet0;
    struct crystalhd_elem *active_node = activeq.head;
    uint32_t failed_tag = 0xcafebabe;
    request2.tx_buffer = (struct crystalhd_tx_buffer){
        .bytes = run.transfer_size, .cookie = &request2,
    };
    hardware.TxFwInputBuffInfo.DramBuffAdd = 0xa000;
    hardware.TxFwInputBuffInfo.DramBuffSzInBytes = 0x80000;
    PrepareSnapshot(&failed_tag, &packet2);
    run.prep.status = BC_STS_IO_ERROR;
    run.prep.copied_words = 2;
    run.allocator_exhausted = true;
    Check(crystalhd_hw_post_tx(&hardware, &request2.tx_buffer, PreparedComplete,
                               &packet2, &failed_tag, run.transfer_flags) == BC_STS_IO_ERROR &&
          failed_tag == 0xcafebabe && activeq.head == active_node &&
          activeq.tail == active_node && QueueCount(&activeq) == 1 &&
          memcmp(packet0, &active_before, sizeof(active_before)) == 0 &&
          freeq.head == run.prep.reserved && QueueHead(&freeq) == &packet2 &&
          !packet2.buffer && !packet2.cb_context && !packet2.call_back && !packet2.list_tag &&
          run.starts == 1 && !run.prep.reg_reads && !run.prep.reg_writes &&
          !run.callback_calls && !run.stops && !run.node_attempts && !hardware.dma_fault,
          "failed peer preparation preserves the existing active owner and returns only the unpublished candidate node");
    QueueInvariant(&freeq);
    QueueInvariant(&activeq);
    run.allocator_exhausted = false;
    Check(crystalhd_hw_tx_req_complete(&hardware, tag, BC_STS_SUCCESS) == BC_STS_SUCCESS &&
          run.callback_calls == 1 && run.seen_callback_context == packet0 &&
          crystalhd_hw_tx_req_complete(&hardware, tag, BC_STS_SUCCESS) == BC_STS_NO_DATA &&
          run.callback_calls == 1,
          "an already-active owner still completes exactly once after peer preparation fails");
    QueueInvariant(&freeq);
    QueueInvariant(&activeq);
    BalancedTwo();
}

static void FleaPreparationFifoPolicy(void)
{
    static const uint8_t flags[] = { 0, 0x04, 0x08, 0x0c };
    for (unsigned flag = 0; flag < ARRAY_SIZE(flags); flag++) {
        for (unsigned success = 0; success < 2; success++) {
            uint32_t tag = 0xfeedbabe;
            FleaPrepareReset(&tag);
            struct tx_dma_pkt before = *packet0;
            hardware.SingleThreadAppFIFOEmpty = false;
            hardware.pfnCheckInputFIFO = crystalhd_flea_check_input_full;
            run.prep.empty = hardware.EmptyCnt + ((flags[flag] & 0x0c) == 0);
            run.prep.single = (flags[flag] & 0x08) != 0;
            run.prep.status = success ? BC_STS_SUCCESS : BC_STS_BUSY;
            run.allocator_exhausted = true;
            Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, PreparedComplete,
                                       packet0, &tag, flags[flag]) == run.prep.status &&
                  run.prep.writes == 1 && run.seen_destination == run.prep.before.DramBuffAdd,
                  "actual FIFO flags retain their existing reservation and single-thread behavior before fallible preparation");
            if (success) {
                Check(hardware.EmptyCnt == run.prep.empty - 1U &&
                      !hardware.SingleThreadAppFIFOEmpty,
                      "successful register-only start preserves the old decrement/clear policy for every FIFO flag branch");
                run.allocator_exhausted = false;
                Check(crystalhd_hw_tx_req_complete(&hardware, tag, BC_STS_SUCCESS) == BC_STS_SUCCESS &&
                      run.callback_calls == 1, "actual FIFO prepared success still completes once");
                QueueInvariant(&freeq);
                QueueInvariant(&activeq);
            } else {
                PreparationRejected(tag, &before);
                Check(crystalhd_hw_post_tx(&hardware, &request.tx_buffer, PreparedComplete,
                                           packet0, &tag, flags[flag]) == BC_STS_BUSY &&
                      run.prep.writes == 1 && run.descriptors == 1 &&
                      !run.starts && freeq.head == run.prep.reserved &&
                      !run.node_attempts && !run.callback_calls,
                      "invalidated cached capacity blocks a second actual-FIFO post without another metadata write");
            }
        }
    }
}

static void QueueApiCompatibility(void)
{
    Reset();
    unsigned freed = run.node_frees;
    Check(crystalhd_dioq_fetch_actual(&freeq) == packet0 &&
          run.node_frees == freed + 1 && !QueueCount(&freeq),
          "ordinary fetch retains its data-returning and generic-node-freeing API");
    QueueInvariant(&freeq);
    Check(crystalhd_dioq_add_actual(NULL, packet0, false, 1) == BC_STS_INV_ARG &&
          crystalhd_dioq_add_actual(&freeq, NULL, false, 1) == BC_STS_INV_ARG &&
          !QueueCount(&freeq), "ordinary add still rejects NULL queue/data before allocating");
    struct crystalhd_dioq before = freeq;
    freeq.sig = 0;
    unsigned attempts = run.node_attempts;
    Check(crystalhd_dioq_add_actual(&freeq, packet0, false, 1) == BC_STS_INV_ARG &&
          !crystalhd_dioq_fetch_elem_actual(&freeq) &&
          !crystalhd_dioq_fetch_actual(&freeq) &&
          !crystalhd_dioq_find_and_fetch_actual(&freeq, 1) &&
          !crystalhd_dioq_fetch_elem_actual(NULL) &&
          run.node_attempts == attempts,
          "ordinary and retained-node APIs still reject invalid signatures without node allocation");
    freeq.sig = before.sig;
    Check(memcmp(&freeq, &before, sizeof(before)) == 0,
          "invalid queue APIs do not alter head/tail/count/sentinel state");
    run.allocator_exhausted = true;
    Check(crystalhd_dioq_add_actual(&freeq, packet0, false, 1) == BC_STS_INSUFF_RES &&
          !QueueCount(&freeq), "ordinary add retains its allocator-failure status unlike retained-node commit");
    run.allocator_exhausted = false;
    Check(crystalhd_dioq_add_actual(&freeq, packet0, false, 0x77) == BC_STS_SUCCESS &&
          crystalhd_dioq_add_actual(&freeq, &packet2, false, 0x88) == BC_STS_SUCCESS &&
          freeq.head->tag == 0x77 && freeq.tail->tag == 0x88 && QueueCount(&freeq) == 2,
          "ordinary add still allocates nodes, preserves FIFO order and stores searchable tags");
    QueueInvariant(&freeq);
    freed = run.node_frees;
    Check(!crystalhd_dioq_find_and_fetch_actual(&freeq, 0x99) &&
          run.node_frees == freed && QueueCount(&freeq) == 2 &&
          crystalhd_dioq_find_and_fetch_actual(&freeq, 0x88) == &packet2 &&
          run.node_frees == freed + 1 && QueueCount(&freeq) == 1 &&
          QueueHead(&freeq) == packet0,
          "ordinary find-and-fetch preserves miss/no-free and tagged tail detach/free semantics");
    QueueInvariant(&freeq);
    Check(crystalhd_dioq_fetch_actual(&freeq) == packet0 &&
          !crystalhd_dioq_fetch_actual(&freeq) && !QueueCount(&freeq),
          "ordinary fetch frees the remaining node and returns NULL from an empty sentinel queue");
    QueueInvariant(&freeq);
}

static void LinkWithoutPreparation(void)
{
    for (unsigned list = 0; list < 2; list++) {
        uint32_t tag = 0xfeedbabe;
        Reset();
        endpoint.device = BC_PCI_DEVID_LINK;
        hardware.tx_list_post_index = list;
        hardware.TxFwInputBuffInfo.DramBuffSzInBytes = 0x40000;
        TX_INPUT_BUFFER_INFO before = hardware.TxFwInputBuffInfo;
        struct crystalhd_elem *reserved = freeq.head;
        request.tx_buffer = (struct crystalhd_tx_buffer){
            .bytes = run.transfer_size, .cookie = &request,
        };
        run.allocator_exhausted = true;
        Check(!hardware.pfnPrepareTxDMA &&
              crystalhd_hw_post_tx(&hardware, &request.tx_buffer, OpaqueComplete,
                                   &opaque_cookie, &tag, run.transfer_flags) == BC_STS_SUCCESS &&
              tag == hardware.tx_ioq_tag_seed + list && activeq.head == reserved &&
              run.starts == 1 && !run.flea.writes && !run.prep.writes && !run.node_attempts,
              "Link's optional NULL preparation retains ordinary start and allocation-free same-node admission on both lists");
        before.HostXferSzInBytes = run.transfer_size;
        Check(memcmp(&hardware.TxFwInputBuffInfo, &before, sizeof(before)) == 0,
              "no-preparation admission does not invalidate Link's cached address or capacity");
        QueueInvariant(&freeq);
        QueueInvariant(&activeq);
        run.allocator_exhausted = false;
        Check(crystalhd_hw_tx_req_complete(&hardware, tag, BC_STS_SUCCESS) == BC_STS_SUCCESS &&
              run.callback_calls == 1, "Link's no-preparation completion retains exactly-once ownership retirement");
        QueueInvariant(&freeq);
        QueueInvariant(&activeq);
        Balanced();
    }
}

int main(void)
{
    Admission(); OpaqueCookie(); CancelAllOwners(); Completion(); Rollback(); TransferArguments(); BusyErrors(); BusyAndFlush(); Cancellation();
    BoundedTransfer(); BorrowedTransfer(); RetainedLeaseProtocol();
    ActualMappingLifetime();
    FleaNotificationGuard();
    FleaAbortNotification();
    FleaNotificationWithActiveOwner();
    PreparationQueueValidation();
    FleaPreparationFailures();
    FleaPreparationSuccess();
    FleaPreparationRecoveryAndOwner();
    FleaPreparationFifoPolicy();
    QueueApiCompatibility();
    LinkWithoutPreparation();
    FleaWakeAdmission();
    printf("TX admission: %u checks, %u failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
