/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real command, capture and post-wrapper functions with deterministic kernel
 * primitives. DMA programming, mapping and queue waits are boundary stubs;
 * this verifies ownership transitions, not device timing or IRQ races.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#include "crystalhd_fw_if.h"
typedef struct C011_PIB C011_PIB;
#include "bc_dts_glob_lnx.h"
typedef uint64_t dma_addr_t;
struct crystalhd_adp;
struct scatterlist;
#include "rx-types.h"

static void discard_log(const char *format, ...) { (void)format; }

#define KERN_ERR ""
#define printk(...) ((void)0)
#define dev_err(dev, ...) ((void)(dev), discard_log(__VA_ARGS__))
#define dev_dbg(dev, ...) ((void)(dev))
#define dev_info(dev, ...) ((void)(dev))
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, update) ((value) = (update))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define spin_lock_irqsave(lock, flags) \
    (assert(*(lock) == 0), *(lock) = 1, (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) \
    (assert(*(lock) == 1), *(lock) = 0, (void)(flags))

struct device { int unused; };
struct pci_dev { struct device dev; int irq; uint32_t device; };
struct crystalhd_dio_req {
    struct crystalhd_dio_user_info uinfo;
    struct crystalhd_rx_buffer rx_buffer;
};
struct crystalhd_dioq {
    struct crystalhd_rx_dma_pkt *packets[BC_RX_LIST_CNT];
    uint32_t tags[BC_RX_LIST_CNT];
    unsigned count;
};
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    struct crystalhd_rx_dma_pkt *rx_pkt_pool_head;
    struct crystalhd_rx_dma_pkt *rx_fallback_head;
    struct crystalhd_dioq *rx_actq, *rx_rdyq, *rx_freeq;
    struct crystalhd_hw_stats stats;
    uint64_t rx_cancel_epoch;
    int lock, rx_lock, fetch_sem;
    bool hw_pause_issued, dma_fault;
    uint32_t rx_pkt_tag_seed, rx_list_post_index, DrvTotalFrmCaptured;
    uint32_t FleaFLLUpdateAddr;
    uint32_t rx_list_sts[DMA_ENGINE_CNT];
    uint32_t PauseThreshold, ResumeThreshold, DefaultPauseThreshold, PDRatio;
    uint64_t TickSpentInPD, TickCntDecodePU;
    enum FLEA_POWER_STATES FleaPowerState;
    BC_STATUS (*pfnPostRxSideBuff)(struct crystalhd_hw *, struct crystalhd_rx_dma_pkt *);
    void (*pfnNotifyFLLChange)(struct crystalhd_hw *, bool);
    void (*pfnHWGetDoneSize)(struct crystalhd_hw *, uint32_t, uint32_t *, uint32_t *);
    BC_STATUS (*pfnIssuePause)(struct crystalhd_hw *, bool);
    bool (*pfnNotifyHardware)(struct crystalhd_hw *, enum BRCM_EVENT);
    void (*pfnStopRXDMAEngines)(struct crystalhd_hw *);
    BC_STATUS (*pfnDevDRAMWrite)(struct crystalhd_hw *, uint32_t, uint32_t,
                                 const uint32_t *);
};
struct crystalhd_cmd {
    struct crystalhd_adp *adp;
    struct crystalhd_hw *hw_ctx;
    uint32_t state;
    uint32_t cin_wait_exit;
};
struct crystalhd_adp {
    struct pci_dev *pdev;
    unsigned present;
    struct crystalhd_cmd cmds;
};

static struct pci_dev endpoint = { .irq = 17 };
static struct crystalhd_adp adapter = { .pdev = &endpoint };
static struct crystalhd_hw hardware;
#define context adapter.cmds
static struct crystalhd_dioq active, ready, available;
static struct crystalhd_rx_dma_pkt packets[BC_RX_LIST_CNT];
static struct crystalhd_dio_req requests[BC_RX_LIST_CNT];
static struct crystalhd_rx_buffer direct_buffers[BC_RX_LIST_CNT];
static uint32_t cookies[BC_RX_LIST_CNT];
static uint32_t buffers[BC_RX_LIST_CNT][64];
static struct crystalhd_rx_buffer *registered_buffers[BC_RX_LIST_CNT];
static void *registered_cookies[BC_RX_LIST_CNT];
static bool mapped[BC_RX_LIST_CNT];
static unsigned unmaps[BC_RX_LIST_CNT];
static unsigned checks, failures, groups, maps, post_calls, stop_calls, irq_depth;
static unsigned irq_disables, irq_enables, notify_calls, pause_calls;
static unsigned sem_attempts, hardware_notifications, map_attempts, translate_calls;
static unsigned fetch_wait_calls;
static unsigned fetch_try_calls, try_mutation_at;
static unsigned try_sem_mutation, try_fetch_mutation;
static bool checking_try_release;
static bool checking_quiesced_retire;
static unsigned quiesced_releases, quiesced_downs;
static uint64_t quiesced_epoch;
static unsigned dram_write_calls, firmware_alive_checks;
static uint32_t dram_write_address, dram_write_dwords, dram_write_value;
static unsigned interrupt_after;
static unsigned fail_post_call;
static BC_STATUS map_status, translate_status, post_status, queue_status;
static BC_STATUS wait_flush_status;
static BC_STATUS wait_restart_start_status, wait_restart_add_status;
static BC_STATUS wait_restart_ready_status, wait_restart_complete_status;
static bool interrupt_lock, wait_signal, wait_suspend, wait_full_flush;
static bool wait_restart_fresh, wait_restart_mode422;
static enum FLEA_POWER_STATES wait_restart_power;
static bool stop_fault, notify_ok;
static bool local_pending, master_enabled;
static unsigned master_clears, pending_waits;
static unsigned chip_masks;
static bool firmware_alive;
static bool checking_admission;
static bool checking_metadata_reset;
static bool checking_start, start_post_observed;
static char lifecycle_events[64];
static unsigned lifecycle_event_count;
static uint32_t stop_observed_state, notify_observed_state, post_observed_state;
static unsigned stop_observed_active, stop_observed_ready, stop_observed_free;
static unsigned notify_observed_active, notify_observed_ready, notify_observed_free;
static int stop_observed_sem, notify_observed_sem, post_observed_sem;
static unsigned stop_observed_irq, notify_observed_irq, post_observed_irq;
static unsigned start_notify_observed, start_notify_notifications;
static uint32_t start_notify_state, start_notify_pause, start_notify_resume;
static uint32_t start_notify_default, start_notify_frames;
static int start_notify_sem;
static unsigned start_post_notifications;
static uint32_t start_post_state;
static int start_post_sem;
static struct crystalhd_dioq *fail_queue;
static const struct crystalhd_rx_buffer_ops legacy_buffer_ops;
static const struct crystalhd_rx_buffer_ops direct_buffer_ops;
void crystalhd_hw_dma_fatal_stop(struct crystalhd_hw *hw);

static void check(bool condition, const char *message)
{
    checks++;
    if (!condition) { failures++; fprintf(stderr, "FAIL: %s\n", message); }
}
static bool memory_is_zero(const void *memory, size_t size)
{
    const uint8_t *bytes = memory;

    for (size_t i = 0; i < size; i++)
        if (bytes[i]) return false;
    return true;
}
static void lifecycle_event(char event)
{
    assert(lifecycle_event_count < sizeof(lifecycle_events) - 1);
    lifecycle_events[lifecycle_event_count++] = event;
    lifecycle_events[lifecycle_event_count] = '\0';
}
static void reset_lifecycle_events(void)
{
    memset(lifecycle_events, 0, sizeof(lifecycle_events));
    lifecycle_event_count = 0;
    stop_observed_state = notify_observed_state = post_observed_state = 0;
    stop_observed_active = stop_observed_ready = stop_observed_free = 0;
    notify_observed_active = notify_observed_ready = notify_observed_free = 0;
    stop_observed_sem = notify_observed_sem = post_observed_sem = -1;
    stop_observed_irq = notify_observed_irq = post_observed_irq = 0;
}
static void reset_flush_observers(void)
{
    reset_lifecycle_events();
    post_calls = stop_calls = irq_disables = irq_enables = 0;
    sem_attempts = hardware_notifications = 0;
}
static size_t request_index(const struct crystalhd_dio_req *request)
{
    size_t i;
    for (i = 0; i < BC_RX_LIST_CNT; i++)
        if (request == &requests[i]) return i;
    abort();
}
static size_t buffer_index(const struct crystalhd_rx_buffer *buffer)
{
    size_t i;

    for (i = 0; i < BC_RX_LIST_CNT; i++)
        if (buffer == &direct_buffers[i] || buffer == &requests[i].rx_buffer)
            return i;
    abort();
}
static size_t packet_index(const struct crystalhd_rx_dma_pkt *packet)
{
    size_t i;
    for (i = 0; i < BC_RX_LIST_CNT; i++)
        if (packet == &packets[i]) return i;
    abort();
}
static unsigned queued_buffer(const struct crystalhd_rx_buffer *buffer)
{
    struct crystalhd_dioq *queues[] = { &active, &ready, &available };
    struct crystalhd_rx_dma_pkt *packet;
    unsigned count = 0;

    for (size_t q = 0; q < 3; q++)
        for (unsigned i = 0; i < queues[q]->count; i++)
            count += queues[q]->packets[i]->buffer == buffer;
    for (packet = hardware.rx_fallback_head; packet; packet = packet->next)
        count += packet->buffer == buffer;
    return count;
}
static unsigned fallback_count(void)
{
    struct crystalhd_rx_dma_pkt *packet = hardware.rx_fallback_head;
    unsigned count = 0;

    while (packet) {
        assert(count < BC_RX_LIST_CNT);
        count++;
        packet = packet->next;
    }
    return count;
}
static void inventory_with_private(unsigned active_count, unsigned ready_count,
                                   unsigned free_count,
                                   const struct crystalhd_rx_buffer *private_buffer)
{
    unsigned seen[BC_RX_LIST_CNT] = {0}, pool_count = 0;
    struct crystalhd_dioq *queues[] = { &active, &ready, &available };
    struct crystalhd_rx_dma_pkt *packet = hardware.rx_pkt_pool_head;
    while (packet) {
        assert(pool_count++ < BC_RX_LIST_CNT);
        seen[packet_index(packet)]++;
        check(!packet->buffer && !packet->cookie,
              "pooled RX packet is detached from buffer and cookie");
        check(memory_is_zero(&packet->metadata, sizeof(packet->metadata)),
              "pooled RX packet retains no metadata from its previous owner");
        packet = packet->next;
    }
    packet = hardware.rx_fallback_head;
    while (packet) {
        size_t index = buffer_index(packet->buffer);

        assert(pool_count++ < BC_RX_LIST_CNT);
        seen[packet_index(packet)]++;
        check(packet->buffer && mapped[index] &&
              packet->buffer == registered_buffers[index] &&
              packet->cookie == registered_cookies[index] &&
              packet->cookie == packet->buffer->cookie,
              "retained packet preserves the exact live buffer and cookie");
        packet = packet->next;
    }
    for (size_t q = 0; q < 3; q++) {
        for (unsigned i = 0; i < queues[q]->count; i++) {
            packet = queues[q]->packets[i];
            seen[packet_index(packet)]++;
            check(packet->buffer && mapped[buffer_index(packet->buffer)] &&
                  packet->buffer == registered_buffers[buffer_index(packet->buffer)] &&
                  packet->cookie == registered_cookies[buffer_index(packet->buffer)] &&
                  packet->cookie == packet->buffer->cookie,
                  "queued packet retains the exact live buffer and opaque cookie");
        }
    }
    for (size_t i = 0; i < BC_RX_LIST_CNT; i++) {
        check(seen[i] == 1, "each RX packet has exactly one pool or queue owner");
        check((registered_buffers[i] ? queued_buffer(registered_buffers[i]) : 0) ==
              (unsigned)(mapped[i] && registered_buffers[i] != private_buffer),
              "each live buffer is caller-private or has exactly one queued owner");
        check(unmaps[i] <= 1, "a registration is unmapped at most once");
    }
    check(active.count == active_count && ready.count == ready_count &&
          available.count == free_count, "active/ready/free ownership matches transition");
    check(!irq_depth && !hardware.lock && !hardware.rx_lock &&
          hardware.fetch_sem == 1,
          "transition releases IRQ, pool/RX locks and capture semaphore");
    if (private_buffer)
        check(mapped[buffer_index(private_buffer)] && !queued_buffer(private_buffer),
              "rejected, dequeued or fresh RX buffer remains caller-private");
}
static void inventory(unsigned active_count, unsigned ready_count, unsigned free_count)
{
    inventory_with_private(active_count, ready_count, free_count, NULL);
}
static BC_STATUS crystalhd_dioq_add(struct crystalhd_dioq *queue, void *data,
                                   bool wake, uint32_t tag)
{
    struct crystalhd_rx_dma_pkt *packet = data;
    struct crystalhd_dioq *queues[] = { &active, &ready, &available };
    assert(queue && packet && queue->count < BC_RX_LIST_CNT);
    (void)wake;
    if (checking_metadata_reset)
        check(memory_is_zero(&packet->metadata, sizeof(packet->metadata)),
              "completion clears old metadata before publishing a queue owner");
    if (queue == fail_queue) return queue_status;
    for (size_t q = 0; q < 3; q++)
        for (unsigned i = 0; i < queues[q]->count; i++)
            assert(queues[q]->packets[i] != packet);
    queue->packets[queue->count] = packet;
    queue->tags[queue->count++] = tag;
    return BC_STS_SUCCESS;
}
static void *fetch_index(struct crystalhd_dioq *queue, unsigned index)
{
    struct crystalhd_rx_dma_pkt *packet;
    if (checking_quiesced_retire)
        check(!hardware.fetch_sem,
              "quiesced retirement detaches queue owners under fetch serialization");
    if (index >= queue->count) return NULL;
    packet = queue->packets[index];
    queue->count--;
    memmove(&queue->packets[index], &queue->packets[index + 1],
            (queue->count - index) * sizeof(queue->packets[0]));
    memmove(&queue->tags[index], &queue->tags[index + 1],
            (queue->count - index) * sizeof(queue->tags[0]));
    return packet;
}
static void *crystalhd_dioq_fetch(struct crystalhd_dioq *queue)
{
    return fetch_index(queue, 0);
}
static void *crystalhd_dioq_find_and_fetch(struct crystalhd_dioq *queue, uint32_t tag)
{
    for (unsigned i = 0; i < queue->count; i++)
        if (queue->tags[i] == tag) return fetch_index(queue, i);
    return NULL;
}
static unsigned crystalhd_dioq_count(struct crystalhd_dioq *queue) { return queue->count; }
static void run_wait_full_flush_hook(void);
static void run_wait_restart_fresh_hook(void);
static void *crystalhd_dioq_fetch_wait(struct crystalhd_hw *hw, uint32_t timeout,
                                     uint32_t *signal)
{
    struct crystalhd_rx_dma_pkt *packet;

    assert(hw == &hardware && timeout == BC_PROC_OUTPUT_TIMEOUT / 1000);
    assert(hardware.fetch_sem == 1);
    fetch_wait_calls++;
    if (wait_suspend)
        context.state |= BC_LINK_SUSPEND;
    *signal = wait_signal;
    if (wait_restart_fresh)
        run_wait_restart_fresh_hook();
    packet = wait_signal ? NULL : crystalhd_dioq_fetch(hw->rx_rdyq);
    if (wait_full_flush)
        run_wait_full_flush_hook();
    return packet;
}
static void apply_try_mutation(unsigned mutation)
{
    switch (mutation) {
    case 0: break;
    case 1: context.state |= BC_LINK_SUSPEND; break;
    case 2: context.state &= ~BC_LINK_CAP_EN; break;
    case 3: adapter.present = 0; break;
    case 4: hardware.dma_fault = true; break;
    case 5: hardware.rx_cancel_epoch++; break;
    case 6:
        hardware.dma_fault = true;
        adapter.present = 0;
        hardware.rx_cancel_epoch++;
        break;
    default: abort();
    }
}
static void *crystalhd_dioq_try_fetch_locked(struct crystalhd_hw *hw)
{
    struct crystalhd_rx_dma_pkt *packet;

    assert(hw == &hardware && !hw->fetch_sem);
    fetch_try_calls++;
    packet = crystalhd_dioq_fetch(hw->rx_rdyq);
    apply_try_mutation(try_fetch_mutation);
    return packet;
}
static BC_STATUS crystalhd_map_dio(struct crystalhd_adp *adp, void *buffer,
        uint32_t size, uint32_t uv, BC_OUTPUT_FORMAT output_format, bool tx,
        struct crystalhd_dio_req **result)
{
    size_t i;
    assert(adp == &adapter && !tx && hardware.fetch_sem == 1);
    map_attempts++;
    if (map_status != BC_STS_SUCCESS) return map_status;
    for (i = 0; i < BC_RX_LIST_CNT; i++) if (buffer == buffers[i]) break;
    assert(i < BC_RX_LIST_CNT && !mapped[i] && !unmaps[i]);
    requests[i].uinfo = (struct crystalhd_dio_user_info){
        .xfr_buff = buffer, .xfr_len = size, .uv_offset = uv,
        .b422mode = output_format };
    requests[i].rx_buffer = (struct crystalhd_rx_buffer){
        .sgl = (struct scatterlist *)buffer,
        .dma_nents = 1,
        .capacity = size,
        .uv_offset = uv,
        .uv_sg_ix = 0,
        .uv_sg_off = uv,
        .output_format = output_format,
        .ops = &legacy_buffer_ops,
        .cookie = &requests[i],
    };
    registered_buffers[i] = &requests[i].rx_buffer;
    registered_cookies[i] = &requests[i];
    mapped[i] = true; maps++;
    *result = &requests[i];
    return BC_STS_SUCCESS;
}
static void observe_quiesced_release(void)
{
    if (!checking_quiesced_retire)
        return;
    quiesced_releases++;
    check(quiesced_releases <= 16 && hardware.fetch_sem == 1 &&
          !hardware.lock && !hardware.rx_lock &&
          hardware.rx_cancel_epoch == quiesced_epoch,
          "quiesced release is bounded and runs after epoch publication outside RX locks");
    check(!active.count && !ready.count && !available.count &&
          !hardware.rx_fallback_head,
          "every queue and fallback owner is detached before the first release callback");
    for (unsigned p = 0; p < BC_RX_LIST_CNT; p++)
        check(!packets[p].buffer && !packets[p].cookie &&
              !packets[p].capture_epoch &&
              memory_is_zero(&packets[p].metadata, sizeof(packets[p].metadata)),
              "each release sees every packet identity and metadata already cleared");
}
static BC_STATUS crystalhd_unmap_dio(struct crystalhd_adp *adp, struct crystalhd_dio_req *request)
{
    size_t i = request_index(request);
    assert(adp == &adapter);
    observe_quiesced_release();
    if (checking_try_release)
        check(hardware.fetch_sem == 1,
              "cancelled try completion releases backing only after dropping fetch semaphore");
    check(mapped[i] && !unmaps[i] &&
          registered_buffers[i] == &request->rx_buffer &&
          registered_cookies[i] == request &&
          request->rx_buffer.cookie == request &&
          !queued_buffer(&request->rx_buffer),
          "legacy unmap occurs once after all queue ownership is released");
    for (size_t p = 0; p < BC_RX_LIST_CNT; p++)
        check(packets[p].buffer != &request->rx_buffer &&
              packets[p].cookie != request->rx_buffer.cookie,
              "legacy release observes a packet detached from buffer and cookie");
    mapped[i] = false; unmaps[i]++;
    return BC_STS_SUCCESS;
}
static void direct_release(struct crystalhd_adp *adp,
                           struct crystalhd_rx_buffer *buffer)
{
    size_t i = buffer_index(buffer);

    assert(adp == &adapter && buffer == &direct_buffers[i]);
    observe_quiesced_release();
    if (checking_try_release)
        check(hardware.fetch_sem == 1,
              "cancelled direct completion releases backing outside fetch semaphore");
    check(mapped[i] && !unmaps[i] && registered_buffers[i] == buffer &&
          registered_cookies[i] == &cookies[i] &&
          buffer->cookie == &cookies[i] && !queued_buffer(buffer),
          "direct release occurs once after all queue ownership is released");
    for (size_t p = 0; p < BC_RX_LIST_CNT; p++)
        check(packets[p].buffer != buffer && packets[p].cookie != buffer->cookie,
              "direct release observes a packet detached from buffer and cookie");
    mapped[i] = false;
    unmaps[i]++;
}
static void legacy_release(struct crystalhd_adp *adp,
                           struct crystalhd_rx_buffer *buffer)
{
    struct crystalhd_dio_req *request =
        (struct crystalhd_dio_req *)((char *)buffer -
            offsetof(struct crystalhd_dio_req, rx_buffer));

    check(request == &requests[buffer_index(buffer)],
          "legacy release recovers the embedding DIO without using the cookie");
    crystalhd_unmap_dio(adp, request);
}
static void sync_for_cpu(struct crystalhd_adp *adp,
                         struct crystalhd_rx_buffer *buffer)
{
    assert(adp == &adapter && mapped[buffer_index(buffer)]);
}
static void sync_for_device(struct crystalhd_adp *adp,
                            struct crystalhd_rx_buffer *buffer)
{
    assert(adp == &adapter && mapped[buffer_index(buffer)]);
}
static BC_STATUS unused_read(struct crystalhd_rx_buffer *buffer, uint32_t offset,
                             void *destination, size_t size)
{
    (void)buffer; (void)offset; (void)destination; (void)size;
    return BC_STS_NOT_IMPL;
}
static BC_STATUS unused_write(struct crystalhd_rx_buffer *buffer, uint32_t offset,
                              const void *source, size_t size)
{
    (void)buffer; (void)offset; (void)source; (void)size;
    return BC_STS_NOT_IMPL;
}
static const struct crystalhd_rx_buffer_ops legacy_buffer_ops = {
    .sync_for_cpu = sync_for_cpu,
    .sync_for_device = sync_for_device,
    .read = unused_read,
    .write = unused_write,
    .release = legacy_release,
};
static const struct crystalhd_rx_buffer_ops direct_buffer_ops = {
    .sync_for_cpu = sync_for_cpu,
    .sync_for_device = sync_for_device,
    .read = unused_read,
    .write = unused_write,
    .release = direct_release,
};
static struct crystalhd_dio_req *
crystalhd_dio_from_rx_buffer(struct crystalhd_rx_buffer *buffer)
{
    struct crystalhd_dio_req *request;

    if (!buffer || buffer->ops != &legacy_buffer_ops)
        return NULL;
    request = (struct crystalhd_dio_req *)((char *)buffer -
        offsetof(struct crystalhd_dio_req, rx_buffer));
    return buffer->cookie == request ? request : NULL;
}
static void crystalhd_rx_buffer_release(struct crystalhd_adp *adp,
                                        struct crystalhd_rx_buffer *buffer)
{
    buffer->ops->release(adp, buffer);
}
static int down_interruptible(int *sem)
{
    sem_attempts++;
    assert(sem == &hardware.fetch_sem && *sem == 1);
    if (interrupt_lock && !interrupt_after) {
        interrupt_lock = false;
        return -1;
    }
    if (interrupt_lock)
        interrupt_after--;
    *sem = 0;
    if (try_mutation_at && sem_attempts == try_mutation_at)
        apply_try_mutation(try_sem_mutation);
    return 0;
}
static void down(int *sem)
{
    assert(sem == &hardware.fetch_sem && *sem == 1);
    if (checking_quiesced_retire)
        quiesced_downs++;
    *sem = 0;
}
static void up(int *sem) { assert(sem == &hardware.fetch_sem && !*sem); *sem = 1; }
static void disable_irq(int irq)
{
    assert(irq == endpoint.irq && !irq_depth && !hardware.fetch_sem);
    irq_depth++; irq_disables++; lifecycle_event('D');
}
static void enable_irq(int irq)
{
    assert(irq == endpoint.irq && irq_depth == 1);
    irq_depth--; irq_enables++; lifecycle_event('E');
}
static struct device *chddev(void) { return &endpoint.dev; }
static uint64_t rdtsc_ordered(void) { return 1000; }
static BC_STATUS crystalhd_xlat_rx_buffer_to_dma_desc(struct crystalhd_rx_buffer *buffer,
        struct dma_desc_mem *memory, uint32_t *uv, struct device *dev)
{
    size_t i = buffer_index(buffer);

    assert(mapped[i] && buffer == registered_buffers[i] &&
           buffer->cookie == registered_cookies[i] && buffer->sgl &&
           buffer->dma_nents == 1 && buffer->capacity &&
           memory && dev == &endpoint.dev);
    assert(buffer->uv_sg_ix == 0 &&
           buffer->uv_sg_off == buffer->uv_offset &&
           ((!buffer->uv_offset && buffer->output_format != MODE420) ||
            (buffer->uv_offset && buffer->output_format == MODE420)));
    if (buffer->ops == &legacy_buffer_ops)
        assert(buffer->capacity == requests[i].uinfo.xfr_len &&
               buffer->uv_offset == requests[i].uinfo.uv_offset &&
               buffer->output_format == requests[i].uinfo.b422mode);
    else
        assert(buffer->capacity == 192);
    assert(!hardware.fetch_sem);
    translate_calls++;
    *uv = buffer->uv_offset ? 1 : 0;
    return translate_status;
}
static void notify_free(struct crystalhd_hw *hw, bool change)
{
    assert(hw == &hardware && !change);
    notify_calls++;
}
static bool crystalhd_flea_detect_fw_alive(struct crystalhd_hw *hw)
{
    assert(hw == &hardware);
    firmware_alive_checks++;
    return firmware_alive;
}
static BC_STATUS record_dram_write(struct crystalhd_hw *hw, uint32_t address,
                                   uint32_t dwords, const uint32_t *value)
{
    assert(hw == &hardware && hardware.lock == 1 && value);
    dram_write_calls++;
    dram_write_address = address;
    dram_write_dwords = dwords;
    dram_write_value = *value;
    return BC_STS_SUCCESS;
}
static BC_STATUS pause_capture(struct crystalhd_hw *hw, bool pause)
{
    assert(hw == &hardware);
    (void)pause;
    pause_calls++;
    return BC_STS_SUCCESS;
}
static void done_size(struct crystalhd_hw *hw, uint32_t index, uint32_t *y, uint32_t *uv)
{
    assert(hw == &hardware && index < DMA_ENGINE_CNT);
    *y = 128; *uv = 64;
}
static bool notify_hardware(struct crystalhd_hw *hw, enum BRCM_EVENT event)
{
    assert(hw == &hardware && !hw->fetch_sem && event == BC_EVENT_START_CAPTURE);
    lifecycle_event('N');
    notify_observed_state = context.state;
    notify_observed_active = active.count;
    notify_observed_ready = ready.count;
    notify_observed_free = available.count;
    notify_observed_sem = hw->fetch_sem;
    notify_observed_irq = irq_depth;
    if (checking_start) {
        start_notify_observed++;
        start_notify_notifications = hardware_notifications;
        start_notify_state = context.state;
        start_notify_pause = hw->PauseThreshold;
        start_notify_resume = hw->ResumeThreshold;
        start_notify_default = hw->DefaultPauseThreshold;
        start_notify_frames = hw->DrvTotalFrmCaptured;
        start_notify_sem = hw->fetch_sem;
    }
    hardware_notifications++;
    return notify_ok;
}
static void stop_dma(struct crystalhd_hw *hw)
{
    assert(hw == &hardware && irq_depth == 1 && !hw->fetch_sem);
    lifecycle_event('S');
    stop_observed_state = context.state;
    stop_observed_active = active.count;
    stop_observed_ready = ready.count;
    stop_observed_free = available.count;
    stop_observed_sem = hw->fetch_sem;
    stop_observed_irq = irq_depth;
    stop_calls++;
    if (stop_fault) crystalhd_hw_dma_fatal_stop(hw);
}
static void pci_clear_master(struct pci_dev *pdev)
{
    assert(pdev == &endpoint);
    master_clears++;
    master_enabled = false;
}
static void crystalhd_flea_disable_interrupts(struct crystalhd_hw *hw)
{
    assert(hw == &hardware && endpoint.device == BC_PCI_DEVID_FLEA);
    assert(hw->dma_fault && !adapter.present && context.cin_wait_exit);
    chip_masks++;
}
static void crystalhd_link_disable_interrupts(struct crystalhd_hw *hw)
{
    assert(hw == &hardware && endpoint.device == BC_PCI_DEVID_LINK);
    assert(hw->dma_fault && !adapter.present && context.cin_wait_exit);
    chip_masks++;
}
static int pci_wait_for_pending_transaction(struct pci_dev *pdev)
{
    assert(pdev == &endpoint);
    pending_waits++;
    return local_pending;
}
/* Model only the DMA programming contract: success enters the active queue;
 * BUSY and errors return the unowned packet to the real post wrapper.
 */
static BC_STATUS program_dma(struct crystalhd_hw *hw, struct crystalhd_rx_dma_pkt *packet)
{
    BC_STATUS status;
    unsigned index;
    assert(hw == &hardware && packet->buffer &&
           mapped[buffer_index(packet->buffer)] &&
           packet->buffer == registered_buffers[buffer_index(packet->buffer)] &&
           packet->cookie == registered_cookies[buffer_index(packet->buffer)] &&
           packet->cookie == packet->buffer->cookie);
    lifecycle_event('P');
    post_observed_state = context.state;
    post_observed_sem = hw->fetch_sem;
    post_observed_irq = irq_depth;
    if (checking_admission) assert(!hw->fetch_sem);
    if (checking_start && !start_post_observed) {
        start_post_observed = true;
        start_post_notifications = hardware_notifications;
        start_post_state = context.state;
        start_post_sem = hw->fetch_sem;
    }
    post_calls++;
    status = !fail_post_call || post_calls == fail_post_call ? post_status : BC_STS_SUCCESS;
    if (hw->dma_fault) return BC_STS_IO_ERROR;
    if (status != BC_STS_SUCCESS) return status;
    for (index = 0; index < DMA_ENGINE_CNT; index++) {
        bool used = false;
        for (unsigned i = 0; i < active.count; i++)
            used |= active.tags[i] == hw->rx_pkt_tag_seed + index;
        if (!used) break;
    }
    if (index == DMA_ENGINE_CNT) return BC_STS_BUSY;
    packet->pkt_tag = hw->rx_pkt_tag_seed + index;
    return crystalhd_dioq_add(hw->rx_actq, packet, false, packet->pkt_tag);
}
static BC_STATUS crystalhd_flea_hw_fire_rxdma(struct crystalhd_hw *hw, struct crystalhd_rx_dma_pkt *packet)
{
    return program_dma(hw, packet);
}
static BC_STATUS crystalhd_link_hw_prog_rxdma(struct crystalhd_hw *hw, struct crystalhd_rx_dma_pkt *packet)
{
    return program_dma(hw, packet);
}
BC_STATUS crystalhd_hw_repost_cap_buffer(struct crystalhd_hw *, struct crystalhd_rx_dma_pkt *);
#undef context
#include "rx-post.h"
#include "rx-hardware.h"
#include "rx-flea-fll.h"
#include "rx-command.h"
#define context adapter.cmds

static void run_wait_full_flush_hook(void)
{
    wait_full_flush = false;
    wait_flush_status = crystalhd_capture_flush(&context, false);
}

static void run_wait_restart_fresh_hook(void)
{
    crystalhd_ioctl_data data = {0};

    wait_restart_fresh = false;
    wait_flush_status = crystalhd_capture_flush(&context, false);
    wait_restart_start_status = crystalhd_capture_start(&context, 0, 0);
    data.udata.u.RxBuffs.YuvBuff = (uint8_t *)buffers[0];
    data.udata.u.RxBuffs.YuvBuffSz = sizeof(buffers[0]);
    data.udata.u.RxBuffs.UVbuffOffset = wait_restart_mode422 ? 0 : 128;
    data.udata.u.RxBuffs.b422Mode = wait_restart_mode422;
    wait_restart_add_status = bc_cproc_add_cap_buff(&context, &data);
    context.state |= BC_LINK_FMT_CHG;
    wait_restart_ready_status = crystalhd_capture_start(&context, 0, 0);
    wait_restart_complete_status = crystalhd_rx_pkt_done(&hardware, 0,
                                                          BC_STS_SUCCESS);
    if (wait_restart_complete_status == BC_STS_SUCCESS)
        ready.packets[0]->flags = COMP_FLAG_FMT_CHANGE;
    hardware.FleaPowerState = wait_restart_power;
    hardware.hw_pause_issued = true;
}

static void reset(uint32_t device)
{
    /* Static model instances are independent scenarios. Reset does not call
     * any release callback for ownership retained by a prior fatal case.
     */
    groups++;
    reset_lifecycle_events();
    memset(&active, 0, sizeof(active));
    memset(&ready, 0, sizeof(ready));
    memset(&available, 0, sizeof(available));
    memset(packets, 0, sizeof(packets));
    memset(requests, 0, sizeof(requests));
    memset(direct_buffers, 0, sizeof(direct_buffers));
    memset(registered_buffers, 0, sizeof(registered_buffers));
    memset(registered_cookies, 0, sizeof(registered_cookies));
    memset(mapped, 0, sizeof(mapped));
    memset(unmaps, 0, sizeof(unmaps));
    endpoint.device = device;
    adapter.pdev = &endpoint;
    adapter.present = 1;
    hardware = (struct crystalhd_hw){ .adp = &adapter, .fetch_sem = 1,
        .rx_actq = &active, .rx_rdyq = &ready, .rx_freeq = &available,
        .rx_pkt_tag_seed = 0x70029070, .PauseThreshold = 12, .ResumeThreshold = 4,
        .FleaFLLUpdateAddr = 0x5f110000,
        .PDRatio = 60, .FleaPowerState = FLEA_PS_ACTIVE,
        .pfnPostRxSideBuff = device == BC_PCI_DEVID_FLEA ?
            crystalhd_flea_hw_post_cap_buff : crystalhd_link_hw_post_cap_buff,
        .pfnNotifyFLLChange = notify_free, .pfnIssuePause = pause_capture,
        .pfnHWGetDoneSize = done_size, .pfnNotifyHardware = notify_hardware,
        .pfnStopRXDMAEngines = stop_dma,
        .pfnDevDRAMWrite = record_dram_write };
    context = (struct crystalhd_cmd){ .adp = &adapter, .hw_ctx = &hardware,
        .state = BC_LINK_READY };
    for (unsigned i = 0; i < BC_RX_LIST_CNT; i++) {
        cookies[i] = 0xc00c0000U + i;
        packets[i].desc_mem.phy_addr = 0x10000 + i * 4096;
        crystalhd_hw_free_rx_pkt(&hardware, &packets[i]);
    }
    maps = post_calls = stop_calls = irq_depth = irq_disables = irq_enables = 0;
    notify_calls = pause_calls = fail_post_call = 0;
    sem_attempts = hardware_notifications = map_attempts = translate_calls = 0;
    fetch_wait_calls = interrupt_after = 0;
    fetch_try_calls = try_mutation_at = try_sem_mutation = try_fetch_mutation = 0;
    checking_try_release = false;
    checking_quiesced_retire = false;
    quiesced_releases = quiesced_downs = 0;
    quiesced_epoch = 0;
    dram_write_calls = firmware_alive_checks = 0;
    dram_write_address = dram_write_dwords = dram_write_value = 0;
    map_status = translate_status = post_status = queue_status = BC_STS_SUCCESS;
    wait_flush_status = BC_STS_ERROR;
    interrupt_lock = wait_signal = wait_suspend = wait_full_flush = false;
    wait_restart_fresh = wait_restart_mode422 = false;
    wait_restart_power = FLEA_PS_ACTIVE;
    wait_restart_start_status = wait_restart_add_status = BC_STS_ERROR;
    wait_restart_ready_status = wait_restart_complete_status = BC_STS_ERROR;
    stop_fault = false;
    local_pending = master_enabled = true;
    master_clears = pending_waits = 0;
    chip_masks = 0;
    firmware_alive = true;
    checking_admission = false;
    checking_metadata_reset = false;
    checking_start = start_post_observed = false;
    start_notify_observed = start_notify_notifications = 0;
    start_notify_state = start_notify_pause = start_notify_resume = 0;
    start_notify_default = start_notify_frames = 0;
    start_notify_sem = -1;
    start_post_notifications = start_post_state = 0;
    start_post_sem = -1;
    notify_ok = true; fail_queue = NULL;
    inventory(0, 0, 0);
}
static BC_STATUS add_data(crystalhd_ioctl_data *data)
{
    BC_STATUS status;
    checking_admission = true;
    status = bc_cproc_add_cap_buff(&context, data);
    checking_admission = false;
    return status;
}
static BC_STATUS add(unsigned index)
{
    crystalhd_ioctl_data data = {0};
    assert(index < BC_RX_LIST_CNT);
    data.udata.u.RxBuffs.YuvBuff = (uint8_t *)buffers[index];
    data.udata.u.RxBuffs.YuvBuffSz = sizeof(buffers[index]);
    data.udata.u.RxBuffs.UVbuffOffset = 128;
    return add_data(&data);
}
static BC_STATUS start_capture(bool direct, uint32_t pause, uint32_t resume)
{
    crystalhd_ioctl_data data = {0};
    BC_STATUS status;

    data.udata.u.RxCap.PauseThsh = pause;
    data.udata.u.RxCap.ResumeThsh = resume;
    checking_start = true;
    if (direct)
        status = crystalhd_capture_start(&context, pause, resume);
    else
        status = bc_cproc_start_capture(&context, &data);
    checking_start = false;
    return status;
}
static BC_STATUS flush_capture(struct crystalhd_cmd *ctx, bool direct,
                               uint32_t discard_only)
{
    crystalhd_ioctl_data data = {0};

    if (direct)
        return crystalhd_capture_flush(ctx, discard_only != 0);
    data.udata.u.FlushRxCap.bDiscardOnly = discard_only;
    return bc_cproc_flush_cap_buffs(ctx, &data);
}
static void complete(unsigned index)
{
    check(crystalhd_rx_pkt_done(&hardware, index, BC_STS_SUCCESS) == BC_STS_SUCCESS,
          "successful IRQ completion transfers one active packet to ready");
}
static void drain(void)
{
    check(crystalhd_hw_stop_capture(&hardware, true) == BC_STS_SUCCESS,
          "capture cancellation drains all queue owners");
    inventory(0, 0, 0);
    unsigned released = 0;
    for (unsigned i = 0; i < BC_RX_LIST_CNT; i++) released += unmaps[i];
    check(released == maps, "all successfully mapped registrations are released exactly once");
    check(irq_disables == irq_enables, "capture drain restores IRQ state");
}
static void fill_ready_pib(struct crystalhd_rx_dma_pkt *packet)
{
    packet->pib.picture_number = 23;
    packet->pib.width = 1920;
    packet->pib.height = 1080;
    packet->pib.chroma_format = 0x420;
    packet->pib.pulldown = 5;
    packet->pib.flags = 0x106;
    packet->pib.sess_num = 17;
    packet->pib.aspect_ratio = 3;
    packet->pib.colour_primaries = 7;
    packet->pib.picture_meta_payload = 0x12345678;
    packet->pib.frame_rate = 0x1e0001;
}
static void apply_expected_pib(struct C011_PIB *pib,
                               const struct crystalhd_rx_dma_pkt *packet)
{
    pib->ppb.picture_number = packet->pib.picture_number;
    pib->ppb.width = packet->pib.width;
    pib->ppb.height = packet->pib.height;
    pib->ppb.chroma_format = packet->pib.chroma_format;
    pib->ppb.pulldown = packet->pib.pulldown;
    pib->ppb.flags = packet->pib.flags;
    pib->ptsStcOffset = packet->pib.sess_num;
    pib->ppb.aspect_ratio = packet->pib.aspect_ratio;
    pib->ppb.colour_primaries = packet->pib.colour_primaries;
    pib->ppb.picture_meta_payload = packet->pib.picture_meta_payload;
    pib->resolution = packet->pib.frame_rate;
}
static struct crystalhd_rx_buffer *map_private(bool mode422);
static struct crystalhd_rx_buffer *map_private_at(unsigned index, bool mode422);
static BC_STATUS submit(struct crystalhd_cmd *ctx,
                        struct crystalhd_rx_buffer *buffer);
static void dequeue_argument_cases(uint32_t device)
{
    struct crystalhd_rx_completion result;

    reset(device);
    memset(&result, 0xa5, sizeof(result));
    check(crystalhd_rx_dequeue(NULL, &result) == BC_STS_INV_ARG &&
          memory_is_zero(&result, sizeof(result)) && !fetch_wait_calls,
          "mapped dequeue rejects NULL context and transfers no ownership");
    inventory(0, 0, 0);

    reset(device);
    context.hw_ctx = NULL;
    memset(&result, 0xa5, sizeof(result));
    check(crystalhd_rx_dequeue(&context, &result) == BC_STS_INV_ARG &&
          memory_is_zero(&result, sizeof(result)) && fetch_wait_calls == 0,
          "mapped dequeue rejects missing ready hardware without an owner");
    inventory(0, 0, 0);

    reset(device);
    context.hw_ctx = NULL;
    context.state = BC_LINK_READY | BC_LINK_SUSPEND;
    memset(&result, 0xa5, sizeof(result));
    check(crystalhd_rx_dequeue(&context, &result) == BC_STS_PWR_MGMT &&
          memory_is_zero(&result, sizeof(result)) && !fetch_wait_calls,
          "suspend gate remains authoritative before missing hardware");
    inventory(0, 0, 0);

    reset(device);
    context.hw_ctx = NULL;
    context.state = BC_LINK_INIT;
    memset(&result, 0xa5, sizeof(result));
    check(crystalhd_rx_dequeue(&context, &result) == BC_STS_ERR_USAGE &&
          memory_is_zero(&result, sizeof(result)) && !fetch_wait_calls,
          "capture-enable gate remains authoritative before missing hardware");
    inventory(0, 0, 0);

    reset(device);
    check(crystalhd_rx_dequeue(&context, NULL) == BC_STS_INV_ARG &&
          !fetch_wait_calls && !sem_attempts,
          "mapped dequeue rejects NULL result before waiting or locking");
    inventory(0, 0, 0);
}
static void dequeue_gate_wait_cases(uint32_t device)
{
    enum { SUSPENDED_READY, SUSPENDED_STOPPED, STOPPED,
           INTERRUPTED_ADMISSION, EMPTY,
           SIGNALLED, SUSPEND_DURING_WAIT };

    for (unsigned direct = 0; direct < 2; direct++) {
        for (unsigned variant = SUSPENDED_READY;
             variant <= SUSPEND_DURING_WAIT; variant++) {
            struct crystalhd_rx_completion result;
            crystalhd_ioctl_data data;
            BC_DEC_OUT_BUFF before;
            BC_STATUS expected;
            unsigned expected_wait;

            reset(device);
            memset(&result, 0xa5, sizeof(result));
            memset(&data, 0xa5, sizeof(data));
            memcpy(&before, &data.udata.u.DecOutData, sizeof(before));
            switch (variant) {
            case SUSPENDED_READY:
                context.state = BC_LINK_READY | BC_LINK_SUSPEND;
                expected = BC_STS_PWR_MGMT;
                expected_wait = 0;
                break;
            case SUSPENDED_STOPPED:
                context.state = BC_LINK_INIT | BC_LINK_SUSPEND;
                expected = BC_STS_PWR_MGMT;
                expected_wait = 0;
                break;
            case STOPPED:
                context.state = BC_LINK_INIT;
                expected = BC_STS_ERR_USAGE;
                expected_wait = 0;
                break;
            case INTERRUPTED_ADMISSION:
                interrupt_lock = true;
                expected = BC_STS_IO_USER_ABORT;
                expected_wait = 0;
                break;
            case EMPTY:
                expected = BC_STS_TIMEOUT;
                expected_wait = 1;
                break;
            case SIGNALLED:
                wait_signal = true;
                expected = BC_STS_IO_USER_ABORT;
                expected_wait = 1;
                break;
            default:
                wait_suspend = true;
                expected = BC_STS_PWR_MGMT;
                expected_wait = 1;
                break;
            }
            check((direct ? crystalhd_rx_dequeue(&context, &result) :
                   bc_cproc_fetch_frame(&context, &data)) == expected,
                  "dequeue preserves suspend, capture gate and wait status ordering");
            check(fetch_wait_calls == expected_wait && hardware.fetch_sem == 1,
                  "dequeue waits only after state admission and returns unlocked");
            if (direct)
                check(memory_is_zero(&result, sizeof(result)),
                      "failed mapped dequeue returns an empty result and no owner");
            else
                check(!memcmp(&before, &data.udata.u.DecOutData, sizeof(before)),
                      "failed legacy dequeue leaves caller output byte-for-byte unchanged");
            inventory(0, 0, 0);
        }
    }
}
static void mapped_dequeue_cases(uint32_t device)
{
    for (unsigned mode422 = 0; mode422 < 2; mode422++) {
        struct crystalhd_rx_completion result;
        struct crystalhd_rx_buffer *buffer;

        reset(device);
        buffer = map_private(mode422);
        check(submit(&context, buffer) == BC_STS_SUCCESS,
              "prepare directly submitted buffer for raw dequeue");
        complete(0);
        memset(&result, 0xa5, sizeof(result));
        check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
              result.buffer == buffer && result.cookie == &cookies[0] &&
              result.flags == COMP_FLAG_DATA_VALID &&
              result.y_done_sz == 128 &&
              result.uv_done_sz == (mode422 ? 0U : 64U) &&
              memory_is_zero(&result.pib, sizeof(result.pib)) &&
              !unmaps[0],
              "raw dequeue returns one 420 or 422 buffer/cookie completion without retiring it");
        inventory_with_private(0, 0, 0, result.buffer);
        check(submit(&context, result.buffer) == BC_STS_SUCCESS &&
              maps == 1 && !unmaps[0],
              "raw dequeue buffer can return through RX admission without remapping");
        inventory(1, 0, 0);
        complete(0);
        memset(&result, 0xa5, sizeof(result));
        check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
              result.buffer == buffer && result.cookie == &cookies[0] &&
              result.flags == COMP_FLAG_DATA_VALID &&
              maps == 1 && !unmaps[0],
              "resubmitted buffer completes with the same backing and cookie identities");
        inventory_with_private(0, 0, 0, result.buffer);
        crystalhd_rx_buffer_release(&adapter, result.buffer);
        check(unmaps[0] == 1,
              "raw dequeue caller retires its detached buffer exactly once");
        inventory(0, 0, 0);

        reset(device);
        check(add(0) == BC_STS_SUCCESS,
              "prepare legacy-submitted mapping for raw dequeue");
        complete(0);
        check(ready.packets[0]->flags == COMP_FLAG_DATA_VALID &&
              ready.packets[0]->y_done_sz == 128 &&
              ready.packets[0]->uv_done_sz == 64,
              "IRQ stores all completion data in the RX packet");
        memset(&result, 0xa5, sizeof(result));
        check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
              result.buffer == &requests[0].rx_buffer &&
              result.cookie == &requests[0] &&
              result.flags == COMP_FLAG_DATA_VALID &&
              result.y_done_sz == 128 && result.uv_done_sz == 64 &&
              !unmaps[0],
              "completion fields live in the result, not the legacy DIO, while identities survive");
        inventory_with_private(0, 0, 0, result.buffer);
        crystalhd_rx_buffer_release(&adapter, result.buffer);
        check(unmaps[0] == 1,
              "legacy-submitted raw result remains caller-owned until release");
        inventory(0, 0, 0);
    }
}
static void reverse_list_completion_case(uint32_t device)
{
    struct crystalhd_rx_buffer *first, *second;
    struct crystalhd_rx_dma_pkt *first_completed, *reused;
    struct crystalhd_rx_completion result;

    reset(device);
    first = map_private_at(0, false);
    inventory_with_private(0, 0, 0, first);
    check(submit(&context, first) == BC_STS_SUCCESS,
          "first direct buffer enters RX list zero with its opaque cookie");
    second = map_private_at(1, true);
    inventory_with_private(1, 0, 0, second);
    check(submit(&context, second) == BC_STS_SUCCESS,
          "second direct buffer enters RX list one with its opaque cookie");
    inventory(2, 0, 0);

    complete(1);
    complete(0);
    first_completed = ready.packets[0];
    if (device == BC_PCI_DEVID_FLEA) {
        ready.packets[0]->metadata = (struct crystalhd_rx_metadata){
            .firmware_timestamp = 0, .picture_number = 71,
            .picture_flags = 0x102, .valid = true, .eos_trailer = false,
        };
        ready.packets[1]->metadata = (struct crystalhd_rx_metadata){
            .firmware_timestamp = UINT64_C(0x123456789), .picture_number = 93,
            .picture_flags = 0x405, .valid = true, .eos_trailer = true,
        };
    }
    inventory(0, 2, 0);
    memset(&result, 0xa5, sizeof(result));
    check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
          result.buffer == second && result.cookie == &cookies[1] &&
          result.y_done_sz == 128 && result.uv_done_sz == 0,
          "reverse list completion dequeues list one's exact backing and cookie first");
    reused = crystalhd_hw_alloc_rx_pkt(&hardware);
    check(reused == first_completed &&
          memory_is_zero(&reused->metadata, sizeof(reused->metadata)),
          "dequeue returns its cleared metadata packet to the reusable pool");
    memset(&reused->metadata, 0xa5, sizeof(reused->metadata));
    if (device == BC_PCI_DEVID_FLEA)
        check(result.metadata.valid && !result.metadata.firmware_timestamp &&
              result.metadata.picture_number == 71 &&
              result.metadata.picture_flags == 0x102 &&
              !result.metadata.eos_trailer,
              "zero timestamp remains valid and the result survives packet reuse by value");
    else
        check(memory_is_zero(&result.metadata, sizeof(result.metadata)),
              "Link completion cannot invent a metadata snapshot");
    crystalhd_hw_free_rx_pkt(&hardware, reused);
    inventory_with_private(0, 1, 0, result.buffer);
    crystalhd_rx_buffer_release(&adapter, result.buffer);
    check(unmaps[1] == 1 && !unmaps[0],
          "reverse completion releases only list one's detached buffer");

    memset(&result, 0xa5, sizeof(result));
    check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
          result.buffer == first && result.cookie == &cookies[0] &&
          result.y_done_sz == 128 && result.uv_done_sz == 64,
          "reverse list completion then dequeues list zero without identity crossover");
    if (device == BC_PCI_DEVID_FLEA)
        check(result.metadata.valid &&
              result.metadata.firmware_timestamp == UINT64_C(0x123456789) &&
              result.metadata.picture_number == 93 &&
              result.metadata.picture_flags == 0x405 &&
              result.metadata.eos_trailer,
              "the second packet keeps its own timestamp, picture and EOS metadata");
    else
        check(memory_is_zero(&result.metadata, sizeof(result.metadata)),
              "separate Link completions both retain invalid metadata");
    inventory_with_private(0, 0, 0, result.buffer);
    crystalhd_rx_buffer_release(&adapter, result.buffer);
    check(unmaps[0] == 1,
          "reverse completion releases list zero after its result is detached");
    inventory(0, 0, 0);
    drain();
}
static void metadata_pool_reset_case(uint32_t device)
{
    struct crystalhd_rx_dma_pkt *packet, *head;
    dma_addr_t descriptor_address;

    reset(device);
    head = hardware.rx_pkt_pool_head;
    descriptor_address = head->desc_mem.phy_addr;
    memset(&head->metadata, 0xa5, sizeof(head->metadata));
    packet = crystalhd_hw_alloc_rx_pkt(&hardware);
    check(packet == head &&
          memory_is_zero(&packet->metadata, sizeof(packet->metadata)) &&
          packet->desc_mem.phy_addr == descriptor_address,
          "allocation clears stale metadata without destroying DMA descriptor storage");
    memset(&packet->metadata, 0x5a, sizeof(packet->metadata));
    crystalhd_hw_free_rx_pkt(&hardware, packet);
    check(hardware.rx_pkt_pool_head == packet &&
          memory_is_zero(&packet->metadata, sizeof(packet->metadata)) &&
          packet->desc_mem.phy_addr == descriptor_address,
          "free clears all metadata before publishing the packet to its pool");
    inventory(0, 0, 0);
}
static void metadata_completion_reset_cases(uint32_t device)
{
    for (unsigned variant = 0; variant < 5; variant++) {
        struct crystalhd_rx_dma_pkt *packet;
        BC_STATUS completion_status = variant && variant < 4 ?
            BC_STS_IO_ERROR : BC_STS_SUCCESS;
        BC_STATUS expected = BC_STS_SUCCESS;

        reset(device);
        check(add(0) == BC_STS_SUCCESS,
              "prepare a registration carrying metadata from an earlier frame");
        packet = active.packets[0];
        memset(&packet->metadata, 0xa5, sizeof(packet->metadata));
        if (variant == 2)
            expected = post_status = BC_STS_BUSY;
        else if (variant == 3)
            expected = post_status = BC_STS_IO_ERROR;
        else if (variant == 4) {
            fail_queue = &ready;
            expected = queue_status = BC_STS_INSUFF_RES;
        }
        checking_metadata_reset = true;
        check(crystalhd_rx_pkt_done(&hardware, 0, completion_status) == expected,
              "metadata reset preserves ready and failed-completion status semantics");
        checking_metadata_reset = false;
        check(memory_is_zero(&packet->metadata, sizeof(packet->metadata)),
              "successful, reposted and retained completions clear stale metadata");
        if (variant == 1) {
            check(active.count == 1 && active.packets[0] == packet,
                  "failed completion reuses the exact original registration");
            memset(&packet->metadata, 0x5a, sizeof(packet->metadata));
            checking_metadata_reset = true;
            complete(0);
            checking_metadata_reset = false;
            check(ready.count == 1 && ready.packets[0] == packet &&
                  memory_is_zero(&packet->metadata, sizeof(packet->metadata)),
                  "successful completion after error reuse cannot expose prior metadata");
        }
        fail_queue = NULL;
        queue_status = post_status = BC_STS_SUCCESS;
        drain();
    }
}
static void metadata_suppression_cases(uint32_t device)
{
    for (unsigned variant = 0; variant < 3; variant++) {
        struct crystalhd_rx_completion result;
        uint32_t flags = variant ? COMP_FLAG_FMT_CHANGE |
            (variant == 2 ? COMP_FLAG_PIB_VALID : COMP_FLAG_DATA_VALID) :
            COMP_FLAG_DATA_VALID;

        reset(device);
        check(add(0) == BC_STS_SUCCESS,
              "prepare invalid or format-only metadata for typed dequeue");
        complete(0);
        ready.packets[0]->flags = flags;
        ready.packets[0]->metadata = (struct crystalhd_rx_metadata){
            .firmware_timestamp = UINT64_C(0x1ffffffff),
            .picture_number = 99, .picture_flags = 0x12345678,
            .valid = variant != 0, .eos_trailer = true,
        };
        memset(&result, 0xa5, sizeof(result));
        check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
              result.flags == flags &&
              memory_is_zero(&result.metadata, sizeof(result.metadata)),
              "invalid and format-change metadata never leaks into a typed result");
        inventory_with_private(0, 0, 0, result.buffer);
        crystalhd_rx_buffer_release(&adapter, result.buffer);
        inventory(0, 0, 0);
    }
}
static void pib_dequeue_cases(uint32_t device)
{
    struct crystalhd_rx_completion result, expected_result;
    crystalhd_ioctl_data data;
    BC_DEC_OUT_BUFF expected_frame;

    reset(device);
    check(add(0) == BC_STS_SUCCESS, "prepare PIB completion for raw dequeue");
    complete(0);
    ready.packets[0]->flags = COMP_FLAG_DATA_VALID | COMP_FLAG_PIB_VALID;
    fill_ready_pib(ready.packets[0]);
    memset(&result, 0xa5, sizeof(result));
    memset(&expected_result, 0, sizeof(expected_result));
    expected_result.buffer = &requests[0].rx_buffer;
    expected_result.cookie = &requests[0];
    expected_result.flags = COMP_FLAG_DATA_VALID | COMP_FLAG_PIB_VALID;
    expected_result.y_done_sz = 128;
    expected_result.uv_done_sz = 64;
    apply_expected_pib(&expected_result.pib, ready.packets[0]);
    check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
          !memcmp(&result, &expected_result, sizeof(result)) && !unmaps[0],
          "raw dequeue snapshots only valid completion metadata and keeps mapping ownership");
    inventory_with_private(0, 0, 0, result.buffer);
    crystalhd_rx_buffer_release(&adapter, result.buffer);
    check(unmaps[0] == 1,
          "raw PIB completion is released exactly once by its caller");
    inventory(0, 0, 0);

    reset(device);
    check(add(0) == BC_STS_SUCCESS,
          "prepare completion without valid PIB for legacy dequeue");
    complete(0);
    if (device == BC_PCI_DEVID_FLEA)
        ready.packets[0]->metadata = (struct crystalhd_rx_metadata){
            .firmware_timestamp = UINT64_C(0x123456789),
            .picture_number = 0xfeed, .picture_flags = 0xbeef,
            .valid = true, .eos_trailer = true,
        };
    check(ready.packets[0]->flags == COMP_FLAG_DATA_VALID,
          "ordinary typed metadata does not require the legacy PIB flag");
    memset(&data, 0xa5, sizeof(data));
    memcpy(&expected_frame, &data.udata.u.DecOutData, sizeof(expected_frame));
    expected_frame.Flags = COMP_FLAG_DATA_VALID;
    expected_frame.OutPutBuffs.YuvBuff = (uint8_t *)buffers[0];
    expected_frame.OutPutBuffs.YuvBuffSz = sizeof(buffers[0]);
    expected_frame.OutPutBuffs.UVbuffOffset = 128;
    expected_frame.OutPutBuffs.b422Mode = false;
    expected_frame.OutPutBuffs.YBuffDoneSz = 128;
    expected_frame.OutPutBuffs.UVBuffDoneSz = 64;
    check(bc_cproc_fetch_frame(&context, &data) == BC_STS_SUCCESS &&
          !memcmp(&data.udata.u.DecOutData, &expected_frame, sizeof(expected_frame)) &&
          unmaps[0] == 1,
          "ordinary typed metadata leaves the full legacy output unchanged without a PIB flag");
    inventory(0, 0, 0);

    reset(device);
    check(add(0) == BC_STS_SUCCESS, "prepare PIB completion for legacy dequeue");
    complete(0);
    ready.packets[0]->flags = COMP_FLAG_DATA_VALID | COMP_FLAG_PIB_VALID;
    fill_ready_pib(ready.packets[0]);
    if (device == BC_PCI_DEVID_FLEA)
        ready.packets[0]->metadata = (struct crystalhd_rx_metadata){
            .firmware_timestamp = UINT64_C(0x123456789),
            .picture_number = 0xfeed, .picture_flags = 0xbeef,
            .valid = true, .eos_trailer = true,
        };
    memset(&data, 0xa5, sizeof(data));
    memcpy(&expected_frame, &data.udata.u.DecOutData, sizeof(expected_frame));
    expected_frame.Flags = COMP_FLAG_DATA_VALID | COMP_FLAG_PIB_VALID;
    apply_expected_pib(&expected_frame.PibInfo, ready.packets[0]);
    expected_frame.OutPutBuffs.YuvBuff = (uint8_t *)buffers[0];
    expected_frame.OutPutBuffs.YuvBuffSz = sizeof(buffers[0]);
    expected_frame.OutPutBuffs.UVbuffOffset = 128;
    expected_frame.OutPutBuffs.b422Mode = false;
    expected_frame.OutPutBuffs.YBuffDoneSz = 128;
    expected_frame.OutPutBuffs.UVBuffDoneSz = 64;
    check(bc_cproc_fetch_frame(&context, &data) == BC_STS_SUCCESS &&
          !memcmp(&data.udata.u.DecOutData, &expected_frame, sizeof(expected_frame)) &&
          unmaps[0] == 1,
          "typed metadata leaves legacy Flags, PIB and six output fields unchanged");
    inventory(0, 0, 0);
}
static void direct_format_dequeue_cases(uint32_t device)
{
    for (unsigned pib_valid = 0; pib_valid < 2; pib_valid++) {
        struct crystalhd_rx_completion result, expected;
        uint32_t initial_state = BC_LINK_INIT | BC_LINK_CAP_EN;
        unsigned post_before;

        reset(device);
        check(add(0) == BC_STS_SUCCESS, "prepare format marker for raw dequeue");
        complete(0);
        ready.packets[0]->flags = COMP_FLAG_FMT_CHANGE |
            (pib_valid ? COMP_FLAG_PIB_VALID : 0);
        fill_ready_pib(ready.packets[0]);
        context.state = initial_state;
        post_before = post_calls;
        memset(&result, 0xa5, sizeof(result));
        memset(&expected, 0, sizeof(expected));
        expected.buffer = &requests[0].rx_buffer;
        expected.cookie = &requests[0];
        expected.flags = ready.packets[0]->flags;
        expected.y_done_sz = 128;
        expected.uv_done_sz = 64;
        if (pib_valid)
            apply_expected_pib(&expected.pib, ready.packets[0]);
        check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
              !memcmp(&result, &expected, sizeof(result)) &&
              context.state == initial_state && !unmaps[0] &&
              post_calls == post_before,
              "raw format dequeue transfers a mapping without legacy requeue policy");
        inventory_with_private(0, 0, 0, result.buffer);
        crystalhd_rx_buffer_release(&adapter, result.buffer);
        check(unmaps[0] == 1,
              "raw format result remains caller-owned until explicit release");
        inventory(0, 0, 0);
    }
}
static void completion_case(uint32_t device)
{
    crystalhd_ioctl_data data = {0};
    reset(device);
    for (unsigned i = 0; i < 3; i++) check(add(i) == BC_STS_SUCCESS, "add accepts active and BUSY buffers");
    inventory(2, 0, 1);
    complete(0);
    inventory(1, 1, 1);
    check(crystalhd_rx_pkt_done(&hardware, 0, BC_STS_SUCCESS) == BC_STS_INV_ARG,
          "duplicate completion cannot fetch or release the same packet");
    check(crystalhd_rx_pkt_done(&hardware, DMA_ENGINE_CNT, BC_STS_SUCCESS) == BC_STS_INV_ARG,
          "out-of-range completion preserves ownership");
    ready.packets[0]->flags |= COMP_FLAG_PIB_VALID;
    ready.packets[0]->pib.width = 1920;
    ready.packets[0]->pib.height = 1080;
    ready.packets[0]->pib.picture_number = 23;
    check(bc_cproc_fetch_frame(&context, &data) == BC_STS_SUCCESS,
          "fetch transfers frame data and releases its registration");
    check(data.udata.u.DecOutData.OutPutBuffs.YuvBuff == (uint8_t *)buffers[0] &&
          data.udata.u.DecOutData.OutPutBuffs.YBuffDoneSz == 128 &&
          data.udata.u.DecOutData.OutPutBuffs.UVBuffDoneSz == 64 &&
          data.udata.u.DecOutData.PibInfo.ppb.width == 1920 &&
          data.udata.u.DecOutData.PibInfo.ppb.picture_number == 23 && unmaps[0] == 1,
          "fetch returns the completed buffer, byte counts and picture metadata");
    inventory(1, 0, 1);
    drain(); drain();
}
static void admission_layout_cases(uint32_t device)
{
    for (unsigned variant = 0; variant < 8; variant++) {
        crystalhd_ioctl_data data = {0};
        BC_STATUS expected = variant < 5 ? BC_STS_INV_ARG : BC_STS_SUCCESS;
        reset(device);
        data.udata.u.RxBuffs.YuvBuff = (uint8_t *)buffers[0];
        data.udata.u.RxBuffs.YuvBuffSz = 192;
        data.udata.u.RxBuffs.UVbuffOffset = 64;
        if (variant == 0) data.udata.u.RxBuffs.YuvBuff = NULL;
        if (variant == 1) data.udata.u.RxBuffs.YuvBuffSz = 0;
        if (variant == 2) {
            data.udata.u.RxBuffs.YuvBuff++;
            expected = BC_STS_NOT_IMPL;
        }
        if (variant == 3 || variant == 6) data.udata.u.RxBuffs.UVbuffOffset = 0;
        if (variant == 4 || variant == 6) data.udata.u.RxBuffs.b422Mode = true;
        if (variant == 7) {
            data.udata.u.RxBuffs.UVbuffOffset = 0;
            data.udata.u.RxBuffs.b422Mode = MODE422_UYVY;
        }
        check(add_data(&data) == expected, "legacy admission preserves layout validation status");
        if (variant < 5) {
            check(!map_attempts && !sem_attempts && !post_calls,
                  "invalid layout is rejected before mapping, locking or posting");
            inventory(0, 0, 0);
        } else {
            check(map_attempts == 1 && sem_attempts == 1 &&
                  requests[0].uinfo.xfr_buff == buffers[0] &&
                  requests[0].uinfo.xfr_len == 192 &&
                  requests[0].uinfo.uv_offset == (variant >= 6 ? 0U : 64U) &&
                  requests[0].uinfo.b422mode ==
                      (variant == 7 ? MODE422_UYVY :
                       variant == 6 ? MODE422_YUY2 : MODE420) &&
                  requests[0].rx_buffer.capacity == 192 &&
                  requests[0].rx_buffer.uv_offset == (variant >= 6 ? 0U : 64U) &&
                  requests[0].rx_buffer.output_format ==
                      (variant == 7 ? MODE422_UYVY :
                       variant == 6 ? MODE422_YUY2 : MODE420) &&
                  requests[0].rx_buffer.ops == &legacy_buffer_ops &&
                  requests[0].rx_buffer.cookie == &requests[0] && !unmaps[0],
                  "legacy admission exposes an embedded generic buffer with DIO cookie");
            inventory(1, 0, 0);
            drain();
        }
    }
}
static void admission_state_cases(uint32_t device)
{
    const uint32_t states[] = { BC_LINK_INIT, BC_LINK_INIT | BC_LINK_CAP_EN,
        BC_LINK_READY, BC_LINK_READY | BC_LINK_PAUSED,
        BC_LINK_READY | BC_LINK_SUSPEND, BC_LINK_READY | BC_LINK_RESUME };
    for (unsigned s = 0; s < sizeof(states) / sizeof(states[0]); s++) {
        for (unsigned paused = 0; paused < 2; paused++) {
            for (unsigned fault = 0; fault < 2; fault++) {
                bool post = states[s] == BC_LINK_READY && !paused;
                bool rejected = fault;
                reset(device);
                context.state = states[s];
                hardware.hw_pause_issued = paused;
                hardware.dma_fault = fault;
                check(add(0) == (rejected ? BC_STS_IO_ERROR : BC_STS_SUCCESS),
                      "faulted hardware rejects every RX admission before any deferred or immediate post");
                check(context.state == states[s] && hardware.hw_pause_issued == (bool)paused &&
                      hardware.dma_fault == (bool)fault && post_calls == (unsigned)(post && !fault) &&
                      map_attempts == 1 && sem_attempts == 1 && unmaps[0] == (unsigned)rejected,
                      "admission preserves state, balances locking and releases only rejected mappings");
                inventory(post && !fault, 0, !post && !fault);
                if (!fault)
                    drain();
            }
        }
    }
}
static struct crystalhd_rx_buffer *map_private_at(unsigned index, bool mode422)
{
    struct crystalhd_rx_buffer *buffer;

    assert(index < BC_RX_LIST_CNT && !mapped[index] && !unmaps[index]);
    buffer = &direct_buffers[index];
    *buffer = (struct crystalhd_rx_buffer){
        .sgl = (struct scatterlist *)buffers[index],
        .dma_nents = 1,
        .capacity = 192,
        .uv_offset = mode422 ? 0 : 64,
        .uv_sg_ix = 0,
        .uv_sg_off = mode422 ? 0 : 64,
        .output_format = mode422 ? MODE422_YUY2 : MODE420,
        .ops = &direct_buffer_ops,
        .cookie = &cookies[index],
    };
    registered_buffers[index] = buffer;
    registered_cookies[index] = &cookies[index];
    mapped[index] = true;
    maps++;
    check((void *)buffer != buffer->cookie &&
          buffer->cookie != &requests[index],
          "direct buffer backing and opaque cookie are distinct objects");
    return buffer;
}
static struct crystalhd_rx_buffer *map_private(bool mode422)
{
    struct crystalhd_rx_buffer *buffer = map_private_at(0, mode422);

    inventory_with_private(0, 0, 0, buffer);
    return buffer;
}
static BC_STATUS submit(struct crystalhd_cmd *ctx, struct crystalhd_rx_buffer *buffer)
{
    BC_STATUS status;
    checking_admission = true;
    status = crystalhd_rx_submit(ctx, buffer);
    checking_admission = false;
    return status;
}
static void mapped_admission_cases(uint32_t device)
{
    enum { IMMEDIATE, BUSY, DEFERRED, PAUSED, INTERRUPTED, NO_PACKET,
           BAD_DESCRIPTOR, POST_ERROR, ACTIVE_ERROR, BUSY_QUEUE_ERROR,
           FREE_QUEUE_ERROR, DMA_FAULT, NULL_CONTEXT, NULL_HARDWARE, NULL_REQUEST };
    for (unsigned variant = IMMEDIATE; variant <= NULL_REQUEST; variant++) {
        struct crystalhd_rx_buffer *buffer;
        struct crystalhd_rx_dma_pkt *pool;
        struct crystalhd_rx_buffer before;
        BC_STATUS expected = BC_STS_SUCCESS;
        uint32_t state;
        reset(device);
        buffer = map_private(false);
        memcpy(&before, buffer, sizeof(before));
        pool = hardware.rx_pkt_pool_head;
        switch (variant) {
        case BUSY: post_status = BC_STS_BUSY; break;
        case DEFERRED: context.state = BC_LINK_INIT; break;
        case PAUSED: hardware.hw_pause_issued = true; break;
        case INTERRUPTED: interrupt_lock = true; expected = BC_STS_IO_USER_ABORT; break;
        case NO_PACKET: hardware.rx_pkt_pool_head = NULL; expected = BC_STS_INSUFF_RES; break;
        case BAD_DESCRIPTOR: translate_status = expected = BC_STS_INV_ARG; break;
        case POST_ERROR: post_status = expected = BC_STS_IO_ERROR; break;
        case ACTIVE_ERROR: fail_queue = &active; queue_status = expected = BC_STS_INSUFF_RES; break;
        case BUSY_QUEUE_ERROR:
            post_status = BC_STS_BUSY; fail_queue = &available;
            queue_status = expected = BC_STS_INSUFF_RES; break;
        case FREE_QUEUE_ERROR:
            context.state = BC_LINK_INIT; fail_queue = &available;
            queue_status = expected = BC_STS_INSUFF_RES; break;
        case DMA_FAULT: hardware.dma_fault = true; expected = BC_STS_IO_ERROR; break;
        case NULL_CONTEXT: case NULL_REQUEST: expected = BC_STS_INV_ARG; break;
        case NULL_HARDWARE: context.hw_ctx = NULL; expected = BC_STS_INV_ARG; break;
        }
        state = context.state;
        check(submit(variant == NULL_CONTEXT ? NULL : &context,
                     variant == NULL_REQUEST ? NULL : buffer) == expected,
              "buffer admission normalizes accepted BUSY and preserves rejection status");
        if (variant == NO_PACKET) hardware.rx_pkt_pool_head = pool;
        check(maps == 1 && map_attempts == 0 && !unmaps[0] &&
              !memcmp(buffer, &before, sizeof(before)),
              "buffer admission neither maps, releases nor rewrites the borrowed object");
        check(context.state == state && hardware.fetch_sem == 1 &&
              sem_attempts == (variant < NULL_CONTEXT ? 1U : 0U) &&
              !hardware_notifications && !pause_calls && !irq_disables && !stop_calls,
              "mapped admission balances its lock without unrelated lifecycle work");
        if (variant == INTERRUPTED || variant == NO_PACKET || variant >= NULL_CONTEXT)
            check(!translate_calls && !post_calls && !notify_calls,
                  "early rejection does not translate, post or notify hardware");
        if (expected == BC_STS_SUCCESS) {
            inventory(variant == IMMEDIATE, 0, variant != IMMEDIATE);
        } else {
            inventory_with_private(0, 0, 0, buffer);
            crystalhd_rx_buffer_release(&adapter, buffer);
            check(unmaps[0] == 1,
                  "caller releases the buffer after failed admission");
            inventory(0, 0, 0);
        }
        if (!hardware.dma_fault)
            drain();
    }
}
static void mapped_completion_cases(uint32_t device)
{
    for (unsigned mode422 = 0; mode422 < 2; mode422++) {
        struct crystalhd_rx_completion result;
        struct crystalhd_rx_buffer *buffer;
        reset(device);
        buffer = map_private(mode422);
        check(submit(&context, buffer) == BC_STS_SUCCESS && maps == 1 && !unmaps[0],
              "direct admission transfers the RX buffer and cookie to capture");
        inventory(1, 0, 0);
        complete(0);
        inventory(0, 1, 0);
        check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
              result.buffer == buffer && result.cookie == &cookies[0] &&
              result.y_done_sz == 128 &&
              result.uv_done_sz == (mode422 ? 0U : 64U) &&
              buffer->capacity == 192 &&
              buffer->uv_offset == (mode422 ? 0U : 64U) &&
              buffer->output_format == (mode422 ? MODE422_YUY2 : MODE420) &&
              map_attempts == 0 && maps == 1 && !unmaps[0],
              "generic dequeue preserves direct 420 and 422 layout plus both identities");
        inventory_with_private(0, 0, 0, result.buffer);
        crystalhd_rx_buffer_release(&adapter, result.buffer);
        check(unmaps[0] == 1,
              "generic caller releases its direct completion exactly once");
        inventory(0, 0, 0);
        drain();
    }
}
static void add_failures(uint32_t device)
{
    uint32_t private_cookie = 0xfeedbeef;
    struct crystalhd_rx_buffer private_buffer = {
        .sgl = (struct scatterlist *)buffers[0],
        .dma_nents = 1,
        .capacity = sizeof(buffers[0]),
        .uv_offset = 128,
        .uv_sg_ix = 0,
        .uv_sg_off = 128,
        .output_format = MODE420,
        .ops = &direct_buffer_ops,
        .cookie = &private_cookie,
    };
    reset(device); map_status = BC_STS_INSUFF_RES;
    check(add(0) == BC_STS_INSUFF_RES && !maps, "map failure never creates an RX owner");
    inventory(0, 0, 0);
    reset(device); interrupt_lock = true;
    check(add(0) == BC_STS_IO_USER_ABORT && unmaps[0] == 1,
          "interrupted add releases the private pinned mapping");
    inventory(0, 0, 0);
    reset(device); translate_status = BC_STS_INV_ARG;
    check(add(0) == BC_STS_INV_ARG && unmaps[0] == 1,
          "descriptor failure returns packet and mapping to their callers");
    inventory(0, 0, 0);
    reset(device); post_status = BC_STS_IO_ERROR;
    check(add(0) == BC_STS_IO_ERROR && unmaps[0] == 1,
          "initial hard post failure releases the private mapping");
    inventory(0, 0, 0);
    reset(device); fail_queue = &active; queue_status = BC_STS_INSUFF_RES;
    check(add(0) == BC_STS_INSUFF_RES && unmaps[0] == 1,
          "active-queue admission failure returns ownership for cleanup");
    inventory(0, 0, 0);
    reset(device); post_status = BC_STS_BUSY; fail_queue = &available; queue_status = BC_STS_INSUFF_RES;
    check(add(0) == BC_STS_INSUFF_RES && unmaps[0] == 1,
          "BUSY wrapper queue failure returns ownership for cleanup");
    inventory(0, 0, 0);
    reset(device); context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
    fail_queue = &available; queue_status = BC_STS_INSUFF_RES;
    check(add(0) == BC_STS_INSUFF_RES && unmaps[0] == 1,
          "initial free-queue failure releases the private mapping");
    inventory(0, 0, 0);
    reset(device); context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
    for (unsigned i = 0; i < BC_RX_LIST_CNT; i++) check(add(i) == BC_STS_SUCCESS, "fill RX pool");
    inventory(0, 0, BC_RX_LIST_CNT);
    check(crystalhd_hw_add_cap_buffer(&hardware, &private_buffer, false) == BC_STS_INSUFF_RES,
          "packet pool exhaustion leaves all existing registrations reachable");
    inventory(0, 0, BC_RX_LIST_CNT);
    drain();
}
static void retry_cases(uint32_t device)
{
    BC_STATUS statuses[] = { BC_STS_SUCCESS, BC_STS_BUSY, BC_STS_IO_ERROR };
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        struct crystalhd_rx_buffer *buffer;

        reset(device);
        buffer = map_private(false);
        check(submit(&context, buffer) == BC_STS_SUCCESS,
              "queue direct buffer and cookie for completion retry");
        post_status = statuses[i];
        check(crystalhd_rx_pkt_done(&hardware, 0, BC_STS_IO_ERROR) == statuses[i],
              "failed completion propagates repost result");
        check(!unmaps[0] &&
              (active.count ? active.packets[0] : available.packets[0])->buffer == buffer &&
              (active.count ? active.packets[0] : available.packets[0])->cookie == &cookies[0],
              "completion repost preserves direct backing and cookie in IRQ context");
        inventory(statuses[i] == BC_STS_SUCCESS, 0, statuses[i] != BC_STS_SUCCESS);
        drain();
    }
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        struct crystalhd_rx_dma_pkt *failed;
        unsigned long flags;

        reset(device);
        check(add(0) == BC_STS_SUCCESS && active.count == 1 &&
              active.tags[0] == hardware.rx_pkt_tag_seed,
              "queue the old list-zero registration");
        spin_lock_irqsave(&hardware.rx_lock, flags);
        failed = crystalhd_rx_pkt_detach(&hardware, 0,
                                          BC_STS_ERROR);
        spin_unlock_irqrestore(&hardware.rx_lock, flags);
        check(failed && !active.count,
              "detach the failed packet before exposing its hardware list");

        check(add(1) == BC_STS_SUCCESS && active.count == 1 &&
              active.tags[0] == hardware.rx_pkt_tag_seed,
              "a concurrent submit safely reuses the detached list tag");
        post_status = statuses[i];
        check(crystalhd_rx_pkt_complete(&hardware, failed, 0,
                                         BC_STS_ERROR) == statuses[i],
              "deferred completion preserves the selected retry result");
        check(!unmaps[0] && !unmaps[1],
              "deferred IRQ completion keeps both registrations mapped");
        if (statuses[i] == BC_STS_SUCCESS) {
            check(active.count == 2 &&
                  active.tags[0] == hardware.rx_pkt_tag_seed &&
                  active.tags[1] == hardware.rx_pkt_tag_seed + 1 &&
                  active.tags[0] != active.tags[1],
                  "successful retry uses the other list without a duplicate tag");
            inventory(2, 0, 0);
        } else {
            check(active.count == 1 && available.count == 1 &&
                  active.tags[0] == hardware.rx_pkt_tag_seed &&
                  available.packets[0] == failed,
                  "failed or busy retry leaves exactly one owner for each packet");
            inventory(1, 0, 1);
        }
        drain();
    }
    reset(device); context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
    check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS, "queue capture start buffers");
    fail_post_call = 2; post_status = BC_STS_IO_ERROR;
    check(crystalhd_hw_start_capture(&hardware) == BC_STS_IO_ERROR,
          "second-engine start failure retains both earlier and failing registrations");
    inventory(1, 0, 1); drain();
    reset(device); context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
    check(add(0) == BC_STS_SUCCESS, "queue BUSY capture start buffer");
    post_status = BC_STS_BUSY;
    check(crystalhd_hw_start_capture(&hardware) == BC_STS_SUCCESS && post_calls == 1,
          "BUSY start queues for later retry without spinning");
    inventory(0, 0, 1); drain();
}
static void fallback_ownership_cases(uint32_t device)
{
    struct crystalhd_rx_buffer *buffer;

    reset(device);
    buffer = map_private(false);
    check(submit(&context, buffer) == BC_STS_SUCCESS,
          "prepare direct ownership for a ready-queue failure");
    fail_queue = &ready;
    queue_status = BC_STS_INSUFF_RES;
    check(crystalhd_rx_pkt_done(&hardware, 0, BC_STS_SUCCESS) ==
              BC_STS_INSUFF_RES &&
          fallback_count() == 1 && hardware.rx_fallback_head->buffer == buffer &&
          hardware.rx_fallback_head->cookie == &cookies[0] && !unmaps[0],
          "ready-queue failure retains the exact buffer and cookie off-queue");
    inventory(0, 0, 0);
    fail_queue = NULL;
    queue_status = BC_STS_SUCCESS;
    check(crystalhd_hw_start_capture(&hardware) == BC_STS_NO_DATA &&
          !fallback_count() && active.count == 1 &&
          active.packets[0]->buffer == buffer &&
          active.packets[0]->cookie == &cookies[0],
          "capture start reuses a retained packet before reporting no more data");
    inventory(1, 0, 0);
    check(crystalhd_hw_stop_capture(&hardware, true) == BC_STS_SUCCESS &&
          unmaps[0] == 1 && !mapped[0],
          "full cancellation releases a reused fallback owner exactly once");
    inventory(0, 0, 0);
    drain();

    reset(device);
    buffer = map_private_at(0, true);
    check(submit(&context, buffer) == BC_STS_SUCCESS,
          "prepare direct ownership for a repost/free double failure");
    post_status = BC_STS_IO_ERROR;
    fail_queue = &available;
    queue_status = BC_STS_INSUFF_RES;
    check(crystalhd_rx_pkt_done(&hardware, 0, BC_STS_IO_ERROR) ==
              BC_STS_IO_ERROR &&
          fallback_count() == 1 && hardware.rx_fallback_head->buffer == buffer &&
          hardware.rx_fallback_head->cookie == &cookies[0] && !unmaps[0],
          "repost plus free-queue failure retains one exact fallback owner");
    inventory(0, 0, 0);
    fail_queue = NULL;
    queue_status = BC_STS_SUCCESS;
    post_status = BC_STS_SUCCESS;
    check(crystalhd_hw_stop_capture(&hardware, true) == BC_STS_SUCCESS &&
          !fallback_count() && unmaps[0] == 1 && !mapped[0],
          "destructive stop detaches and releases the repost fallback exactly once");
    inventory(0, 0, 0);
    drain();
}
static void fallback_free_ring_cases(uint32_t device)
{
    struct crystalhd_rx_buffer *retired[BC_RX_LIST_CNT] = {0};
    bool seen[4] = {false};
    unsigned retired_count;

    reset(device);
    check(submit(&context, map_private_at(0, false)) == BC_STS_SUCCESS &&
          submit(&context, map_private_at(1, true)) == BC_STS_SUCCESS,
          "prepare active owners for destructive fallback detachment");
    context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
    check(submit(&context, map_private_at(2, false)) == BC_STS_SUCCESS,
          "prepare a free-queue owner for destructive fallback detachment");
    fail_queue = &ready;
    queue_status = BC_STS_INSUFF_RES;
    check(crystalhd_rx_pkt_done(&hardware, 0, BC_STS_SUCCESS) ==
              BC_STS_INSUFF_RES && fallback_count() == 1,
          "move one completed owner into the fallback list");
    fail_queue = NULL;
    queue_status = BC_STS_SUCCESS;
    context.state = BC_LINK_READY;
    check(submit(&context, map_private_at(3, false)) == BC_STS_SUCCESS,
          "reuse the vacant hardware list for a ready-queue owner");
    complete(0);
    check(active.count == 1 && ready.count == 1 && available.count == 1 &&
          fallback_count() == 1,
          "construct active, ready, free and fallback ownership together");
    inventory(1, 1, 1);

    retired_count = crystalhd_hw_detach_rx_owners(&hardware, retired);
    check(retired_count == 4 && !active.count && !ready.count &&
          !available.count && !fallback_count(),
          "free-ring detachment gathers every queue and fallback owner");
    for (unsigned i = 0; i < retired_count; i++) {
        size_t index = buffer_index(retired[i]);

        check(index < ARRAY_SIZE(seen),
              "free-ring detachment returns only submitted buffers");
        if (index >= ARRAY_SIZE(seen))
            continue;
        check(!seen[index] && retired[i] == registered_buffers[index] &&
              retired[i]->cookie == registered_cookies[index],
              "free-ring detachment returns each exact buffer/cookie pair once");
        seen[index] = true;
    }
    for (unsigned i = 0; i < BC_RX_LIST_CNT; i++)
        check(!packets[i].buffer && !packets[i].cookie,
              "free-ring detachment clears packet identities before release");
    check(!unmaps[0] && !unmaps[1] && !unmaps[2] && !unmaps[3],
          "free-ring detachment delays all releases until every packet is clean");
    for (unsigned i = 0; i < retired_count; i++)
        crystalhd_rx_buffer_release(&adapter, retired[i]);
    check(unmaps[0] == 1 && unmaps[1] == 1 &&
          unmaps[2] == 1 && unmaps[3] == 1,
          "free-ring-style teardown releases all detached buffers exactly once");
    inventory(0, 0, 0);
    drain();
}
static void quiesced_retirement_cases(uint32_t device)
{
    /* Independent owner counts, including the complete sixteen-packet pool.
     * Active ownership never exceeds the two hardware lists.
     */
    static const unsigned layouts[][4] = {
        {0, 0, 0, 0}, {1, 0, 0, 0}, {0, 1, 0, 0},
        {0, 0, 1, 0}, {0, 0, 0, 1}, {1, 1, 1, 1},
        {2, 5, 5, 4}, {0, 16, 0, 0}, {0, 0, 16, 0}, {0, 0, 0, 16},
    };

    _Static_assert(BC_RX_LIST_CNT == 16, "retirement ownership bound");
    for (unsigned backing = 0; backing < 3; backing++) {
        for (unsigned layout = 0; layout < ARRAY_SIZE(layouts); layout++) {
            struct crystalhd_dioq *queues[] = { &active, &ready, &available };
            struct dma_desc_mem descriptors[BC_RX_LIST_CNT];
            unsigned owners = 0;

            reset(device);
            hardware.rx_cancel_epoch = UINT64_C(0x10000002a);
            hardware.dma_fault = true; /* DMA has already been quiesced externally. */
            adapter.present = 0;
            adapter.pdev = NULL; /* No PCI/IRQ/MMIO access is valid at this boundary. */
            hardware.rx_list_sts[0] = 0x1234;
            hardware.rx_list_sts[1] = 0x5678;
            hardware.rx_list_post_index = 1;
            for (unsigned q = 0; q < ARRAY_SIZE(layouts[layout]); q++) {
                for (unsigned n = 0; n < layouts[layout][q]; n++, owners++) {
                    struct crystalhd_rx_dma_pkt *packet;
                    struct crystalhd_rx_buffer *buffer;

                    if (backing == 0 || (backing == 2 && (owners & 1))) {
                        struct crystalhd_dio_req *request = NULL;

                        check(crystalhd_map_dio(&adapter, buffers[owners],
                              sizeof(buffers[owners]), 128, MODE420, false,
                              &request) == BC_STS_SUCCESS,
                              "prepare legacy backing for quiesced retirement");
                        buffer = &request->rx_buffer;
                    } else {
                        buffer = map_private_at(owners, true);
                    }
                    packet = crystalhd_hw_alloc_rx_pkt(&hardware);
                    assert(packet);
                    packet->buffer = buffer;
                    packet->cookie = buffer->cookie;
                    packet->capture_epoch = hardware.rx_cancel_epoch;
                    packet->flags = COMP_FLAG_DATA_VALID;
                    memset(&packet->metadata, 0xa5, sizeof(packet->metadata));
                    if (q < ARRAY_SIZE(queues))
                        check(crystalhd_dioq_add(queues[q], packet, false,
                              owners + 1) == BC_STS_SUCCESS,
                              "prepare exact queue owner for quiesced retirement");
                    else
                        crystalhd_hw_retain_rx_pkt(&hardware, packet);
                }
            }
            for (unsigned p = 0; p < BC_RX_LIST_CNT; p++)
                descriptors[p] = packets[p].desc_mem;
            inventory(layouts[layout][0], layouts[layout][1], layouts[layout][2]);
            interrupt_lock = true;
            checking_quiesced_retire = true;
            quiesced_epoch = UINT64_C(0x10000002b);
            crystalhd_hw_retire_rx_quiesced(&hardware);
            check(quiesced_releases == owners && quiesced_downs == 1 &&
                  !sem_attempts && interrupt_lock &&
                  hardware.rx_cancel_epoch == UINT64_C(0x10000002b),
                  "quiesced retirement advances epoch once and cannot be abandoned by a signal");
            inventory(0, 0, 0);
            for (unsigned p = 0; p < BC_RX_LIST_CNT; p++) {
                check(unmaps[p] == (p < owners) && !mapped[p],
                      "quiesced retirement releases each registered owner exactly once");
                check(!memcmp(&packets[p].desc_mem, &descriptors[p], sizeof(descriptors[p])),
                      "quiesced retirement preserves packet DMA-ring allocations");
            }
            quiesced_epoch++;
            crystalhd_hw_retire_rx_quiesced(&hardware);
            check(quiesced_releases == owners && quiesced_downs == 2 &&
                  hardware.rx_cancel_epoch == UINT64_C(0x10000002c),
                  "repeated empty retirement advances cancellation without a second release");
            check(hardware.rx_actq == &active && hardware.rx_rdyq == &ready &&
                  hardware.rx_freeq == &available && hardware.dma_fault &&
                  hardware.rx_list_sts[0] == 0x1234 &&
                  hardware.rx_list_sts[1] == 0x5678 &&
                  hardware.rx_list_post_index == 1 && context.state == BC_LINK_READY &&
                  !irq_disables && !irq_enables && !post_calls && !stop_calls &&
                  !notify_calls && !pause_calls && !hardware_notifications &&
                  !dram_write_calls && !firmware_alive_checks,
                  "memory-only retirement leaves queues, ring state and command state intact without hardware callbacks");
            checking_quiesced_retire = false;
            inventory(0, 0, 0);
        }
    }

    reset(device);
    hardware.rx_actq = hardware.rx_rdyq = hardware.rx_freeq = NULL;
    checking_quiesced_retire = true;
    quiesced_epoch = 1;
    crystalhd_hw_retire_rx_quiesced(&hardware);
    check(hardware.rx_cancel_epoch == 1 && hardware.fetch_sem == 1 &&
          quiesced_downs == 1 && !quiesced_releases &&
          !hardware.rx_actq && !hardware.rx_rdyq && !hardware.rx_freeq,
          "an unconfigured context without queues still invalidates the cancellation epoch");
    checking_quiesced_retire = false;
    {
        struct crystalhd_rx_buffer *buffer = map_private_at(0, true);
        struct crystalhd_rx_dma_pkt *packet = crystalhd_hw_alloc_rx_pkt(&hardware);

        assert(packet);
        packet->buffer = buffer;
        packet->cookie = buffer->cookie;
        crystalhd_hw_retain_rx_pkt(&hardware, packet);
        checking_quiesced_retire = true;
        quiesced_epoch = 2;
        crystalhd_hw_retire_rx_quiesced(&hardware);
        check(quiesced_releases == 1 && unmaps[0] == 1 && !mapped[0] &&
              !hardware.rx_fallback_head && hardware.rx_cancel_epoch == 2,
              "fallback ownership is retired even when every ordinary queue is absent");
        checking_quiesced_retire = false;
        inventory(0, 0, 0);
    }

    reset(device);
    hardware.adp = NULL;
    {
        struct crystalhd_hw before = hardware;

        checking_quiesced_retire = true;
        crystalhd_hw_retire_rx_quiesced(NULL);
        crystalhd_hw_retire_rx_quiesced(&hardware);
        check(!memcmp(&before, &hardware, sizeof(before)) &&
              !quiesced_releases && !quiesced_downs,
              "NULL hardware and absent adapter are no-ops before semaphore access");
        checking_quiesced_retire = false;
    }
}

static void legacy_fetch_rejects_generic_case(uint32_t device)
{
    crystalhd_ioctl_data data = {0};
    struct crystalhd_rx_buffer *buffer;

    reset(device);
    buffer = map_private(false);
    check(submit(&context, buffer) == BC_STS_SUCCESS,
          "prepare a generic completion for the legacy fetch adapter");
    complete(0);
    check(bc_cproc_fetch_frame(&context, &data) == BC_STS_IO_ERROR &&
          unmaps[0] == 1 && !mapped[0] && !fallback_count(),
          "legacy fetch rejects a generic frontend buffer and releases it once");
    inventory(0, 0, 0);
    drain();
}
static void free_count_consumer_cases(uint32_t device)
{
    struct crystalhd_hw_stats stats;

    reset(device);
    check(crystalhd_hw_count_free_rx_pkts(NULL) == 0,
          "free RX count rejects a NULL hardware context as empty");
    context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
    check(submit(&context, map_private_at(0, false)) == BC_STS_SUCCESS,
          "prepare one ordinary free-queue owner for combined counting");
    context.state = BC_LINK_READY;
    check(submit(&context, map_private_at(1, true)) == BC_STS_SUCCESS,
          "prepare one active owner for fallback counting");
    fail_queue = &ready;
    queue_status = BC_STS_INSUFF_RES;
    check(crystalhd_rx_pkt_done(&hardware, 0, BC_STS_SUCCESS) ==
              BC_STS_INSUFF_RES && available.count == 1 &&
          fallback_count() == 1,
          "ready failure constructs one free-queue and one fallback owner");
    check(crystalhd_hw_count_free_rx_pkts(&hardware) == 2 && !hardware.lock,
          "production free count sums the queue and fallback list under lock");
    inventory(0, 0, 1);

    hardware.stats.rx_errors = 7;
    hardware.stats.tx_errors = 11;
    memset(&stats, 0xa5, sizeof(stats));
    crystalhd_hw_stats(&hardware, &stats);
    check(stats.freeq_count == 2 && hardware.stats.freeq_count == 2 &&
          stats.rdyq_count == 0 && stats.rx_errors == 7 &&
          stats.tx_errors == 11,
          "production stats publishes the combined free-owner count");

    if (device == BC_PCI_DEVID_FLEA) {
        crystalhd_flea_notify_fll_change(&hardware, false);
        check(dram_write_calls == 1 && !firmware_alive_checks &&
              dram_write_address == hardware.FleaFLLUpdateAddr &&
              dram_write_dwords == 1 && dram_write_value == 2 &&
              !hardware.lock,
              "Flea FLL update writes the combined queue and fallback count");

        firmware_alive = false;
        crystalhd_flea_notify_fll_change(&hardware, true);
        check(dram_write_calls == 1 && firmware_alive_checks == 1,
              "dead-firmware cleanup suppresses an FLL write");
        firmware_alive = true;
        crystalhd_flea_notify_fll_change(&hardware, true);
        check(dram_write_calls == 2 && firmware_alive_checks == 2 &&
              dram_write_value == 2 && !hardware.lock,
              "live-firmware cleanup also writes the combined free count");
    }

    fail_queue = NULL;
    queue_status = BC_STS_SUCCESS;
    check(crystalhd_hw_stop_capture(&hardware, true) == BC_STS_SUCCESS &&
          unmaps[0] == 1 && unmaps[1] == 1,
          "combined-count owners remain destructively releasable exactly once");
    inventory(0, 0, 0);
    drain();
}
static void format_case(uint32_t device, unsigned failure)
{
    crystalhd_ioctl_data data = {0};
    BC_DEC_YUV_BUFFS output_before;
    uint32_t bad_before, channel_before, video_before;
    reset(device);
    memset(&data.udata.u.DecOutData, 0xa5,
           sizeof(data.udata.u.DecOutData));
    memcpy(&output_before, &data.udata.u.DecOutData.OutPutBuffs,
           sizeof(output_before));
    bad_before = data.udata.u.DecOutData.BadFrCnt;
    channel_before = data.udata.u.DecOutData.PibInfo.channelId;
    video_before = data.udata.u.DecOutData.PibInfo.ppb.video_buffer;
    check(add(0) == BC_STS_SUCCESS, "submit format-change registration");
    complete(0);
    ready.packets[0]->flags = COMP_FLAG_FMT_CHANGE | COMP_FLAG_PIB_VALID;
    ready.packets[0]->pib.width = 1280;
    ready.packets[0]->pib.height = 720;
    context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
    if (failure == 1) {
        interrupt_lock = true;
        interrupt_after = 1;
    }
    if (failure == 2) translate_status = BC_STS_IO_ERROR;
    if (failure == 3) post_status = BC_STS_IO_ERROR;
    BC_STATUS status = bc_cproc_fetch_frame(&context, &data);
    check(status == (failure == 1 ? BC_STS_IO_USER_ABORT :
          failure ? BC_STS_IO_ERROR : BC_STS_SUCCESS), "format-change status matches the failing boundary");
    check(data.udata.u.DecOutData.Flags == (COMP_FLAG_FMT_CHANGE | COMP_FLAG_PIB_VALID) &&
          data.udata.u.DecOutData.PibInfo.ppb.width == 1280,
          "format-change notification preserves format metadata");
    check(!memcmp(&data.udata.u.DecOutData.OutPutBuffs, &output_before,
                  sizeof(output_before)) &&
          data.udata.u.DecOutData.BadFrCnt == bad_before &&
          data.udata.u.DecOutData.PibInfo.channelId == channel_before &&
          data.udata.u.DecOutData.PibInfo.ppb.video_buffer == video_before,
          "format transition changes only Flags and the valid PIB subset");
    if (failure == 1 || failure == 2) {
        check(unmaps[0] == 1 && !(context.state & BC_LINK_FMT_CHG),
              "failed format admission releases its mapping without committing format state");
        inventory(0, 0, 0);
    } else {
        check(!unmaps[0] && context.state == BC_LINK_READY && maps == 1,
              "format resubmit keeps the original mapping and commits format state");
        inventory(failure ? 0 : 1, 0, failure ? 1 : 0);
        if (!failure) {
            complete(0);
            check(bc_cproc_fetch_frame(&context, &data) == BC_STS_SUCCESS &&
                  data.udata.u.DecOutData.Flags == COMP_FLAG_DATA_VALID &&
                  data.udata.u.DecOutData.OutPutBuffs.YuvBuff == (uint8_t *)buffers[0] &&
                  unmaps[0] == 1 && maps == 1,
                  "format registration becomes a normal completed frame without remapping");
            inventory(0, 0, 0);
        }
    }
    drain();
}
static void format_without_pib_case(uint32_t device)
{
    for (unsigned data_valid = 0; data_valid < 2; data_valid++) {
        crystalhd_ioctl_data data;
        BC_DEC_OUT_BUFF expected;

        reset(device);
        check(add(0) == BC_STS_SUCCESS,
              "submit format-change registration without PIB metadata");
        complete(0);
        ready.packets[0]->flags = COMP_FLAG_FMT_CHANGE |
            (data_valid ? COMP_FLAG_DATA_VALID : 0);
        context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
        memset(&data, 0xa5, sizeof(data));
        memcpy(&expected, &data.udata.u.DecOutData, sizeof(expected));
        expected.Flags = ready.packets[0]->flags;
        check(bc_cproc_fetch_frame(&context, &data) == BC_STS_SUCCESS &&
              !memcmp(&data.udata.u.DecOutData, &expected, sizeof(expected)) &&
              context.state == BC_LINK_READY && !unmaps[0],
              "legacy format dequeue changes only Flags and takes priority over data");
        inventory(1, 0, 0);
        drain();
    }
}
static void format_full_flush_race_cases(uint32_t device)
{
    for (unsigned restart = 0; restart < 2; restart++) {
        for (unsigned mode422 = 0; mode422 < 2; mode422++) {
            struct crystalhd_rx_completion result;
            struct crystalhd_rx_buffer *buffer;
            uint32_t expected_state = BC_LINK_INIT;
            unsigned post_before;

            reset(device);
            buffer = map_private(mode422);
            check(submit(&context, buffer) == BC_STS_SUCCESS,
                  "prepare format completion for a concurrent full flush");
            complete(0);
            ready.packets[0]->flags = COMP_FLAG_FMT_CHANGE |
                COMP_FLAG_DATA_VALID | COMP_FLAG_PIB_VALID;
            fill_ready_pib(ready.packets[0]);
            context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
            check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
                  result.buffer == buffer && result.cookie == &cookies[0] &&
                  !unmaps[0],
                  "format dequeue detaches the exact old buffer and cookie");
            inventory_with_private(0, 0, 0, result.buffer);

            check(flush_capture(&context, true, 0) == BC_STS_SUCCESS &&
                  context.state == BC_LINK_INIT,
                  "full flush completes while the old format result is detached");
            if (restart) {
                check(start_capture(true, 0, 0) == BC_STS_SUCCESS &&
                      add(1) == BC_STS_SUCCESS,
                      "a new capture epoch can start and admit a fresh registration");
                expected_state |= BC_LINK_CAP_EN;
            }

            post_before = post_calls;
            check(crystalhd_rx_ack_format(&context, &result) ==
                      BC_STS_IO_USER_ABORT,
                  "late format completion is cancelled after a full flush");
            check(!result.buffer && !result.cookie &&
                  context.state == expected_state &&
                  unmaps[0] == 1 &&
                  post_calls == post_before && !mapped[0] &&
                  (!restart || (mapped[1] && !unmaps[1])),
                  "cancelled format completion cannot restore state, queue or DMA ownership");
            check(crystalhd_rx_ack_format(&context, &result) == BC_STS_INV_ARG &&
                  unmaps[0] == 1,
                  "a cancelled format result cannot be consumed twice");
            inventory(0, 0, restart ? 1 : 0);
            drain();
        }
    }
}
static void format_wait_flush_race_cases(uint32_t device)
{
    for (unsigned variant = 0; variant < 4; variant++) {
        struct crystalhd_rx_completion result;
        struct crystalhd_rx_buffer *buffer;
        bool mode422 = variant & 1;
        unsigned post_before;

        reset(device);
        buffer = map_private(mode422);
        check(submit(&context, buffer) == BC_STS_SUCCESS,
              "prepare format completion for the dequeue wait race");
        complete(0);
        ready.packets[0]->flags = COMP_FLAG_FMT_CHANGE |
            (variant & 1 ? COMP_FLAG_DATA_VALID : 0) |
            (variant & 2 ? COMP_FLAG_PIB_VALID : 0);
        fill_ready_pib(ready.packets[0]);
        if (device == BC_PCI_DEVID_FLEA) {
            hardware.FleaPowerState = variant & 1 ?
                FLEA_PS_LP_PENDING : FLEA_PS_LP_COMPLETE;
            hardware.hw_pause_issued = true;
        }
        context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
        wait_full_flush = true;
        check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
              wait_flush_status == BC_STS_SUCCESS && !wait_full_flush &&
              result.buffer == buffer && result.cookie == &cookies[0] &&
              result.capture_epoch == 0 &&
              hardware.rx_cancel_epoch == 1 && context.state == BC_LINK_INIT &&
              (device != BC_PCI_DEVID_FLEA ||
               (!pause_calls && hardware.hw_pause_issued)),
              "packet epoch survives a full flush after ready-pop and before dequeue resumes");
        inventory_with_private(0, 0, 0, result.buffer);
        post_before = post_calls;
        check(crystalhd_rx_ack_format(&context, &result) == BC_STS_IO_USER_ABORT &&
              !result.buffer && !result.cookie &&
              unmaps[0] == 1 && !mapped[0] &&
              context.state == BC_LINK_INIT && post_calls == post_before,
              "ready-pop race cancels the old format mapping without requeue or state revival");
        inventory(0, 0, 0);
        drain();
    }
}
static void empty_wait_flush_race_cases(uint32_t device)
{
    if (device != BC_PCI_DEVID_FLEA)
        return;

    for (unsigned power = 0; power < 2; power++) {
        for (unsigned signalled = 0; signalled < 2; signalled++) {
            struct crystalhd_rx_completion result;
            BC_STATUS expected = signalled ? BC_STS_IO_USER_ABORT :
                BC_STS_TIMEOUT;

            reset(device);
            hardware.FleaPowerState = power ? FLEA_PS_LP_PENDING :
                FLEA_PS_LP_COMPLETE;
            hardware.hw_pause_issued = true;
            wait_signal = signalled;
            wait_full_flush = true;
            memset(&result, 0xa5, sizeof(result));
            check(crystalhd_rx_dequeue(&context, &result) == expected &&
                  wait_flush_status == BC_STS_SUCCESS && !wait_full_flush &&
                  memory_is_zero(&result, sizeof(result)) &&
                  hardware.rx_cancel_epoch == 1 &&
                  context.state == BC_LINK_INIT && !pause_calls &&
                  hardware.hw_pause_issued,
                  "full flush suppresses FLEA wake after an empty or signalled wait");
            inventory(0, 0, 0);
            drain();
        }
    }
}
static void fresh_packet_after_wait_restart_cases(uint32_t device)
{
    if (device != BC_PCI_DEVID_FLEA)
        return;

    for (unsigned power = 0; power < 2; power++) {
        for (unsigned mode422 = 0; mode422 < 2; mode422++) {
            struct crystalhd_rx_completion result;

            reset(device);
            wait_restart_mode422 = mode422;
            wait_restart_power = power ? FLEA_PS_LP_PENDING :
                FLEA_PS_LP_COMPLETE;
            wait_restart_fresh = true;
            memset(&result, 0xa5, sizeof(result));
            check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
                  wait_flush_status == BC_STS_SUCCESS &&
                  wait_restart_start_status == BC_STS_SUCCESS &&
                  wait_restart_add_status == BC_STS_SUCCESS &&
                  wait_restart_ready_status == BC_STS_SUCCESS &&
                  wait_restart_complete_status == BC_STS_SUCCESS &&
                  !wait_restart_fresh,
                  "an old waiter accepts a fresh packet after full flush and restart");
            check(result.buffer == &requests[0].rx_buffer &&
                  result.cookie == &requests[0] &&
                  result.capture_epoch == 1 &&
                  result.capture_epoch == hardware.rx_cancel_epoch &&
                  result.flags == COMP_FLAG_FMT_CHANGE &&
                  context.state == BC_LINK_READY,
                  "post-restart dequeue returns only the current-epoch registration");
            check(pause_calls == 1 && !hardware.hw_pause_issued,
                  "a current-epoch packet resumes FLEA after a cross-epoch wait");
            inventory_with_private(0, 0, 0, result.buffer);
            check(crystalhd_rx_ack_format(&context, &result) == BC_STS_SUCCESS &&
                  !result.buffer && !result.cookie && !unmaps[0] &&
                  context.state == BC_LINK_READY,
                  "fresh post-restart format ownership remains consumable");
            inventory(1, 0, 0);
            drain();
        }
    }
}
static void format_discard_epoch_cases(uint32_t device)
{
    for (unsigned mode422 = 0; mode422 < 2; mode422++) {
        struct crystalhd_rx_completion result;
        struct crystalhd_rx_buffer *buffer;
        uint64_t epoch;

        reset(device);
        buffer = map_private(mode422);
        check(submit(&context, buffer) == BC_STS_SUCCESS,
              "prepare format completion for discard control");
        complete(0);
        ready.packets[0]->flags = COMP_FLAG_FMT_CHANGE;
        context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
        check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS,
              "discard control dequeues one format registration");
        epoch = result.capture_epoch;
        check(result.buffer == buffer && result.cookie == &cookies[0],
              "discard control preserves both generic completion identities");
        inventory_with_private(0, 0, 0, result.buffer);
        check(flush_capture(&context, true, 1) == BC_STS_SUCCESS &&
              hardware.rx_cancel_epoch == epoch,
              "discard preserves the capture epoch for retained registrations");
        check(crystalhd_rx_ack_format(&context, &result) == BC_STS_SUCCESS &&
              !result.buffer && !result.cookie &&
              context.state == BC_LINK_READY &&
              !unmaps[0],
              "format completion remains valid across non-destructive discard");
        inventory(1, 0, 0);
        drain();
    }
}
static void format_failed_stop_epoch_case(uint32_t device)
{
    struct crystalhd_rx_completion result;
    struct crystalhd_rx_buffer *buffer;
    unsigned post_before;

    reset(device);
    buffer = map_private(false);
    check(submit(&context, buffer) == BC_STS_SUCCESS,
          "prepare detached format completion for a failed full stop");
    complete(0);
    ready.packets[0]->flags = COMP_FLAG_FMT_CHANGE;
    context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
    check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
          result.capture_epoch == 0,
          "failed-stop case retains the admission epoch in the dequeue result");
    check(result.buffer == buffer && result.cookie == &cookies[0],
          "failed-stop dequeue preserves both generic completion identities");
    inventory_with_private(0, 0, 0, result.buffer);
    stop_fault = true;
    check(flush_capture(&context, true, 0) == BC_STS_IO_ERROR &&
          hardware.dma_fault && hardware.rx_cancel_epoch == 1 &&
          context.state == BC_LINK_INIT,
          "destructive flush invalidates detached results even when DMA stop fails");
    post_before = post_calls;
    check(crystalhd_rx_ack_format(&context, &result) == BC_STS_IO_USER_ABORT &&
          !result.buffer && !result.cookie &&
          unmaps[0] == 1 && post_calls == post_before,
          "failed full stop cannot revive its detached format registration");
    inventory(0, 0, 0);
    check(hardware.dma_fault && !adapter.present && context.cin_wait_exit,
          "failed stop stays terminal even after a previously detached result is released");
}
static void format_fresh_epoch_cases(uint32_t device)
{
    for (unsigned mode422 = 0; mode422 < 2; mode422++) {
        struct crystalhd_rx_completion result;
        crystalhd_ioctl_data fresh = {0};

        reset(device);
        check(add(0) == BC_STS_SUCCESS,
              "prepare an old registration before a destructive flush");
        check(flush_capture(&context, true, 0) == BC_STS_SUCCESS &&
              hardware.rx_cancel_epoch == 1 && context.state == BC_LINK_INIT,
              "full flush starts a new capture epoch");
        check(start_capture(true, 0, 0) == BC_STS_SUCCESS,
              "restart enables capture in the new epoch");
        fresh.udata.u.RxBuffs.YuvBuff = (uint8_t *)buffers[1];
        fresh.udata.u.RxBuffs.YuvBuffSz = sizeof(buffers[1]);
        fresh.udata.u.RxBuffs.UVbuffOffset = mode422 ? 0 : 128;
        fresh.udata.u.RxBuffs.b422Mode = mode422;
        check(add_data(&fresh) == BC_STS_SUCCESS && available.count == 1 &&
              available.packets[0]->capture_epoch ==
                  hardware.rx_cancel_epoch,
              "fresh 420 or 422 admission records the current capture epoch");

        context.state |= BC_LINK_FMT_CHG;
        check(start_capture(true, 0, 0) == BC_STS_SUCCESS,
              "ready restart posts the fresh registration");
        inventory(1, 0, 0);
        complete(0);
        ready.packets[0]->flags = COMP_FLAG_FMT_CHANGE |
            COMP_FLAG_PIB_VALID;
        fill_ready_pib(ready.packets[0]);
        check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
              result.buffer == &requests[1].rx_buffer &&
              result.cookie == &requests[1] && result.capture_epoch == 1 &&
              result.capture_epoch == hardware.rx_cancel_epoch,
              "fresh completion carries the nonzero current epoch");
        inventory_with_private(0, 0, 0, result.buffer);
        check(crystalhd_rx_ack_format(&context, &result) == BC_STS_SUCCESS &&
              !result.buffer && !result.cookie && !unmaps[1] &&
              context.state == BC_LINK_READY,
              "current-epoch format completion requeues normally");
        inventory(1, 0, 0);
        drain();
    }
}
static void format_capture_gate_case(uint32_t device)
{
    struct crystalhd_rx_completion result;
    struct crystalhd_rx_buffer *buffer;
    unsigned post_before;

    reset(device);
    buffer = map_private(false);
    check(submit(&context, buffer) == BC_STS_SUCCESS,
          "prepare format completion for the capture-state gate");
    complete(0);
    ready.packets[0]->flags = COMP_FLAG_FMT_CHANGE;
    context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
    check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
          result.capture_epoch == hardware.rx_cancel_epoch,
          "capture-state gate starts with an otherwise current result");
    check(result.buffer == buffer && result.cookie == &cookies[0],
          "capture-state gate begins with exact generic identities");
    inventory_with_private(0, 0, 0, result.buffer);
    context.state &= ~BC_LINK_CAP_EN;
    post_before = post_calls;
    check(crystalhd_rx_ack_format(&context, &result) == BC_STS_IO_USER_ABORT &&
          !result.buffer && !result.cookie &&
          unmaps[0] == 1 && post_calls == post_before &&
          context.state == BC_LINK_INIT,
          "capture-disabled state rejects an equal-epoch format result");
    inventory(0, 0, 0);
    drain();
}
static void cancellation_cases(uint32_t device)
{
    crystalhd_ioctl_data data = {0};
    reset(device);
    for (unsigned i = 0; i < 3; i++) {
        struct crystalhd_rx_buffer *buffer = map_private_at(i, i == 2);

        check(submit(&context, buffer) == BC_STS_SUCCESS,
              "prepare direct buffer/cookie for active/free/ready cancellation");
    }
    complete(0); inventory(1, 1, 1);
    check(crystalhd_hw_stop_capture(&hardware, false) == BC_STS_SUCCESS,
          "discard moves active and ready registrations back to free");
    inventory(0, 0, 3);
    check(!unmaps[0] && !unmaps[1] && !unmaps[2] &&
          available.packets[0]->buffer == &direct_buffers[2] &&
          available.packets[0]->cookie == &cookies[2],
          "discard retains every direct backing and opaque cookie");
    check(crystalhd_hw_start_capture(&hardware) == BC_STS_SUCCESS, "discarded buffers can restart capture");
    inventory(2, 0, 1);
    data.udata.u.FlushRxCap.bDiscardOnly = 0;
    check(bc_cproc_flush_cap_buffs(&context, &data) == BC_STS_SUCCESS &&
          !(context.state & (BC_LINK_CAP_EN | BC_LINK_FMT_CHG)),
          "full cancellation clears state and releases every direct buffer/cookie owner");
    inventory(0, 0, 0); drain();

    reset(device);
    check(bc_cproc_fetch_frame(&context, &data) == BC_STS_TIMEOUT,
          "empty ready wait returns timeout without creating ownership");
    wait_signal = true;
    check(bc_cproc_fetch_frame(&context, &data) == BC_STS_IO_USER_ABORT,
          "cancelled ready wait returns user abort without creating ownership");
    inventory(0, 0, 0);
    data.udata.u.FlushRxCap.bDiscardOnly = 1;
    check(bc_cproc_flush_cap_buffs(&context, &data) == BC_STS_SUCCESS,
          "discard of empty queues tolerates no buffers to restart");
    inventory(0, 0, 0);

    reset(device);
    check(add(0) == BC_STS_SUCCESS, "prepare discard restart notification failure");
    notify_ok = false;
    check(bc_cproc_flush_cap_buffs(&context, &data) == BC_STS_IO_ERROR,
          "failed restart notification leaves registration on free queue");
    inventory(0, 0, 1); drain();

    reset(device);
    check(add(0) == BC_STS_SUCCESS, "prepare interrupted command cancellation");
    interrupt_lock = true;
    data.udata.u.FlushRxCap.bDiscardOnly = 0;
    check(bc_cproc_flush_cap_buffs(&context, &data) == BC_STS_IO_USER_ABORT &&
          context.state == BC_LINK_READY && !stop_calls,
          "interrupted cancellation preserves capture state and queue ownership");
    inventory(1, 0, 0); drain();

    reset(device);
    for (unsigned i = 0; i < 3; i++) check(add(i) == BC_STS_SUCCESS, "prepare failed DMA stop");
    complete(0); stop_fault = true;
    check(crystalhd_hw_stop_capture(&hardware, true) == BC_STS_IO_ERROR,
          "failed DMA stop reports error and preserves ownership");
    inventory(1, 1, 1);
    check(!unmaps[0] && !unmaps[1] && !unmaps[2], "timed-out DMA stop never unpins active memory");
    check(crystalhd_hw_start_capture(&hardware) == BC_STS_IO_ERROR,
          "faulted capture rejects restarting queued DMA");
    inventory(1, 1, 1);
    stop_fault = false;
    check(crystalhd_hw_stop_capture(&hardware, true) == BC_STS_IO_ERROR &&
          hardware.dma_fault && !adapter.present && context.cin_wait_exit,
          "a later successful engine callback cannot revoke terminal ownership uncertainty");
    inventory(1, 1, 1);
    check(!unmaps[0] && !unmaps[1] && !unmaps[2],
          "failed-stop scenario retains every backing without test-only teardown");

    reset(device);
    hardware.rx_actq = hardware.rx_rdyq = hardware.rx_freeq = NULL;
    check(crystalhd_hw_stop_capture(&hardware, true) == BC_STS_SUCCESS && !stop_calls,
          "monitor context without RX queues needs no DMA stop");
    inventory(0, 0, 0);
}
static void fatal_owner_retention_cases(uint32_t device)
{
    for (unsigned pending = 0; pending < 2; pending++) {
        for (unsigned direct = 0; direct < 2; direct++) {
            struct crystalhd_rx_dma_pkt *fallback;
            unsigned old_posts;

            reset(device);
            for (unsigned i = 0; i < 4; i++) {
                check((direct ? submit(&context, map_private_at(i, false)) : add(i)) ==
                      BC_STS_SUCCESS, "seed every retained RX owner location");
            }
            complete(0);
            fallback = crystalhd_dioq_fetch(&available);
            check(fallback != NULL, "reserve a fallback registration before fatal stop");
            crystalhd_hw_retain_rx_pkt(&hardware, fallback);
            inventory(1, 1, 1);
            check(fallback_count() == 1, "fatal case includes a fallback owner");
            local_pending = pending;
            stop_fault = true;
            check(crystalhd_hw_stop_capture(&hardware, true) == BC_STS_IO_ERROR,
                  "fatal stop cannot authorize ownership release from local pending status");
            check(hardware.dma_fault && !adapter.present && context.cin_wait_exit,
                  "fatal stop publishes sticky engine uncertainty and device cancellation");
            check(chip_masks == 1 && master_clears == 1 && pending_waits == 1 && !master_enabled,
                  "a local clear and pending wait remain best-effort rather than release proof");
            inventory(1, 1, 1);
            check(fallback_count() == 1 && hardware.rx_fallback_head == fallback,
                  "failed stop keeps the exact fallback identity reachable");
            for (unsigned i = 0; i < 4; i++)
                check(mapped[i] && !unmaps[i], "fatal stop retains every DMA backing");
            old_posts = post_calls;
            stop_fault = false;
            check(crystalhd_hw_stop_capture(&hardware, true) == BC_STS_IO_ERROR &&
                  crystalhd_hw_start_capture(&hardware) == BC_STS_IO_ERROR,
                  "a later successful backend stop cannot recover a fatal context");
            check(post_calls == old_posts && hardware.dma_fault && !adapter.present,
                  "terminal context never reposts retained buffers");
            inventory(1, 1, 1);
            for (unsigned i = 0; i < 4; i++)
                check(mapped[i] && !unmaps[i], "no test-only reset or release hides retained ownership");
        }
    }
}
static void fatal_late_rx_cases(uint32_t device)
{
    for (unsigned route = 0; route < 4; route++) {
        struct crystalhd_rx_dma_pkt *packet = NULL;
        struct crystalhd_rx_completion result;
        unsigned old_posts, old_pause;

        reset(device);
        check(submit(&context, map_private(false)) == BC_STS_SUCCESS,
              "prepare RX identity for a late fatal-boundary callback");
        if (route == 1 || route == 2) {
            packet = crystalhd_rx_pkt_detach(&hardware, 0, BC_STS_SUCCESS);
            check(packet != NULL && !active.count && packet->buffer == &direct_buffers[0],
                  "the racing callback owns an exact packet detached before fatal publication");
        } else if (route == 3) {
            complete(0);
            hardware.FleaPowerState = FLEA_PS_LP_COMPLETE;
            hardware.hw_pause_issued = true;
        }
        crystalhd_hw_dma_fatal_stop(&hardware);
        old_posts = post_calls;
        old_pause = pause_calls;
        if (route == 0) {
            check(crystalhd_rx_pkt_detach(&hardware, 0, BC_STS_SUCCESS) == NULL &&
                  crystalhd_rx_pkt_done(&hardware, 0, BC_STS_SUCCESS) == BC_STS_INV_ARG,
                  "late RX completion cannot detach an uncertain active owner");
            inventory(1, 0, 0);
        } else if (route == 1) {
            check(crystalhd_rx_pkt_complete(&hardware, packet, 0, BC_STS_SUCCESS) == BC_STS_IO_ERROR,
                  "late completion retains an already-detached packet instead of publishing data");
            check(hardware.rx_fallback_head == packet, "completion retains the exact detached identity");
            inventory(0, 0, 0);
        } else if (route == 2) {
            check(crystalhd_hw_repost_cap_buffer(&hardware, packet) == BC_STS_IO_ERROR &&
                  hardware.rx_fallback_head == packet,
                  "late repost transfers its exact owner to fallback without DMA programming");
            inventory(0, 0, 0);
        } else {
            memset(&result, 0xa5, sizeof(result));
            check(crystalhd_hw_get_cap_buffer(&hardware, &result, hardware.rx_cancel_epoch) == BC_STS_IO_ERROR &&
                  memory_is_zero(&result, sizeof(result)),
                  "legacy ready fetch cannot return data or release backing after fatal publication");
            inventory(0, 0, 0);
        }
        check(fallback_count() == (unsigned)(route != 0) && mapped[0] && !unmaps[0] &&
              post_calls == old_posts && pause_calls == old_pause && hardware.fetch_sem == 1,
              "late callback retains ownership without release, resume, repost or lock leakage");
        check(!adapter.present && context.cin_wait_exit && hardware.dma_fault,
              "late RX callbacks cannot reverse terminal cancellation");
    }
}
static void flush_argument_and_gate_cases(uint32_t device)
{
    const uint32_t no_capture_states[] = {
        BC_LINK_INVALID, BC_LINK_FMT_CHG,
        BC_LINK_SUSPEND, BC_LINK_PAUSED, BC_LINK_RESUME,
        BC_LINK_INIT | BC_LINK_FMT_CHG,
        BC_LINK_INIT | BC_LINK_SUSPEND,
        BC_LINK_INIT | BC_LINK_PAUSED,
        BC_LINK_INIT | BC_LINK_RESUME,
        BC_LINK_INIT | BC_LINK_FMT_CHG | BC_LINK_PAUSED
    };

    for (unsigned direct = 0; direct < 2; direct++) {
        for (unsigned discard = 0; discard < 2; discard++) {
            reset(device);
            check(flush_capture(NULL, direct, discard) == BC_STS_INV_ARG &&
                  !sem_attempts && !stop_calls && !irq_disables &&
                  !hardware_notifications && !post_calls,
                  "flush rejects a NULL command before locking or hardware callbacks");
            inventory(0, 0, 0);

            reset(device);
            context.hw_ctx = NULL;
            check(flush_capture(&context, direct, discard) == BC_STS_INV_ARG &&
                  !sem_attempts && !stop_calls && !irq_disables &&
                  !hardware_notifications && !post_calls,
                  "flush rejects a NULL hardware context before locking or callbacks");
            inventory(0, 0, 0);

            reset(device);
            check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS &&
                  add(2) == BC_STS_SUCCESS,
                  "prepare mixed ownership for interrupted flush");
            complete(0);
            reset_flush_observers();
            interrupt_lock = true;
            check(flush_capture(&context, direct, discard) == BC_STS_IO_USER_ABORT &&
                  context.state == BC_LINK_READY && sem_attempts == 1 &&
                  !stop_calls && !irq_disables && !hardware_notifications &&
                  !post_calls && !lifecycle_event_count,
                  "interrupted flush preserves state and skips stop, notify and restart");
            inventory(1, 1, 1);
            drain();
        }

        for (unsigned state_index = 0;
             state_index < sizeof(no_capture_states) / sizeof(no_capture_states[0]);
             state_index++) {
            for (unsigned discard = 0; discard < 2; discard++) {
                uint32_t state = no_capture_states[state_index];

                reset(device);
                check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS &&
                      add(2) == BC_STS_SUCCESS,
                      "prepare ownership for capture-enable gate");
                complete(0);
                context.state = state;
                reset_flush_observers();
                check(flush_capture(&context, direct, discard) == BC_STS_ERR_USAGE &&
                      context.state == state && sem_attempts == 1 &&
                      !stop_calls && !irq_disables && !hardware_notifications &&
                      !post_calls && !lifecycle_event_count,
                      "flush requires capture-enable without changing state or queues");
                inventory(1, 1, 1);
                context.state = BC_LINK_READY;
                drain();
            }
        }
    }

    reset(device);
    check(bc_cproc_flush_cap_buffs(&context, NULL) == BC_STS_INV_ARG &&
          !sem_attempts && !stop_calls && !irq_disables &&
          !hardware_notifications && !post_calls,
          "legacy flush rejects NULL ioctl data before locking or callbacks");
    inventory(0, 0, 0);
}
static void pre_capture_full_flush_cases(uint32_t device)
{
    for (unsigned direct = 0; direct < 2; direct++) {
        reset(device);
        context.state = BC_LINK_INIT;
        check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS &&
              add(2) == BC_STS_SUCCESS,
              "prepare deferred registrations for pre-capture destructive flush");
        check(context.state == BC_LINK_INIT && !active.count && !ready.count &&
              available.count == 3 && !post_calls && !unmaps[0] &&
              !unmaps[1] && !unmaps[2],
              "pre-capture registration keeps exact INIT state and deferred ownership");
        reset_flush_observers();
        check(flush_capture(&context, direct, 0) == BC_STS_SUCCESS &&
              !strcmp(lifecycle_events, "DSE") && context.state == BC_LINK_INIT,
              "exact INIT permits destructive unregister without enabling capture");
        check(stop_observed_state == BC_LINK_INIT &&
              !stop_observed_active && !stop_observed_ready &&
              stop_observed_free == 3 && !stop_observed_sem &&
              stop_observed_irq == 1 && stop_calls == 1 &&
              irq_disables == 1 && irq_enables == 1,
              "pre-capture unregister quiesces DMA with every deferred owner visible");
        check(!hardware_notifications && !post_calls &&
              unmaps[0] == 1 && unmaps[1] == 1 && unmaps[2] == 1,
              "pre-capture unregister releases each registration exactly once without restart");
        inventory(0, 0, 0);
        reset_flush_observers();
        check(flush_capture(&context, direct, 0) == BC_STS_SUCCESS &&
              !strcmp(lifecycle_events, "DSE") && context.state == BC_LINK_INIT &&
              stop_calls == 1 && unmaps[0] == 1 && unmaps[1] == 1 &&
              unmaps[2] == 1,
              "repeated pre-capture unregister is idempotent and cannot double-release owners");
        inventory(0, 0, 0);

        reset(device);
        context.state = BC_LINK_INIT;
        check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS &&
              add(2) == BC_STS_SUCCESS,
              "prepare deferred registrations for interrupted pre-capture flush");
        reset_flush_observers();
        interrupt_lock = true;
        check(flush_capture(&context, direct, 0) == BC_STS_IO_USER_ABORT &&
              context.state == BC_LINK_INIT && sem_attempts == 1 &&
              !lifecycle_event_count && !stop_calls && !irq_disables &&
              !hardware_notifications && !post_calls &&
              !unmaps[0] && !unmaps[1] && !unmaps[2],
              "interrupted pre-capture unregister retains state and every owner");
        inventory(0, 0, 3);
        check(flush_capture(&context, direct, 0) == BC_STS_SUCCESS &&
              !strcmp(lifecycle_events, "DSE") && context.state == BC_LINK_INIT &&
              sem_attempts == 2 && stop_calls == 1 &&
              unmaps[0] == 1 && unmaps[1] == 1 && unmaps[2] == 1,
              "retry after interruption releases each pre-capture owner exactly once");
        inventory(0, 0, 0);

        reset(device);
        context.state = BC_LINK_INIT;
        check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS &&
              add(2) == BC_STS_SUCCESS,
              "prepare deferred registrations for pre-capture DMA-stop fault");
        stop_fault = true;
        reset_flush_observers();
        check(flush_capture(&context, direct, 0) == BC_STS_IO_ERROR &&
              !strcmp(lifecycle_events, "DSE") && context.state == BC_LINK_INIT &&
              stop_calls == 1 && hardware.dma_fault &&
              !unmaps[0] && !unmaps[1] && !unmaps[2],
              "faulted pre-capture unregister retains state and all registrations");
        inventory(0, 0, 3);
        check(!adapter.present && context.cin_wait_exit,
              "pre-capture stop fault makes the device unavailable before retaining owners");
        stop_fault = false;
        check(flush_capture(&context, direct, 0) == BC_STS_IO_ERROR &&
              !strcmp(lifecycle_events, "DSE") && context.state == BC_LINK_INIT &&
              sem_attempts == 2 && stop_calls == 1 &&
              irq_disables == 1 && irq_enables == 1 &&
              !unmaps[0] && !unmaps[1] && !unmaps[2],
              "retry after a DMA fault cannot release or stop retained owners again");
        inventory(0, 0, 3);

        reset(device);
        context.state = BC_LINK_INIT;
        check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS,
              "prepare deferred registrations for pre-capture discard rejection");
        reset_flush_observers();
        check(flush_capture(&context, direct, 1) == BC_STS_ERR_USAGE &&
              context.state == BC_LINK_INIT && sem_attempts == 1 &&
              !lifecycle_event_count && !stop_calls && !irq_disables &&
              !hardware_notifications && !post_calls &&
              !unmaps[0] && !unmaps[1],
              "pre-capture discard remains gated on active capture");
        inventory(0, 0, 2);
        check(flush_capture(&context, direct, 0) == BC_STS_SUCCESS &&
              context.state == BC_LINK_INIT &&
              unmaps[0] == 1 && unmaps[1] == 1,
              "destructive retry after rejected discard releases deferred owners once");
        inventory(0, 0, 0);
    }
}
static void discard_flush_cases(uint32_t device)
{
    const uint32_t wrapper_values[] = { 1, 2, UINT32_MAX };

    for (unsigned direct = 0; direct < 2; direct++) {
        unsigned value_count = direct ? 1U :
            (unsigned)(sizeof(wrapper_values) / sizeof(wrapper_values[0]));

        for (unsigned value_index = 0; value_index < value_count; value_index++) {
            uint32_t discard = direct ? 1U : wrapper_values[value_index];
            uint32_t initial_state = BC_LINK_READY | BC_LINK_PAUSED;

            reset(device);
            check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS &&
                  add(2) == BC_STS_SUCCESS,
                  "prepare active, ready and free registrations for discard");
            complete(0);
            context.state = initial_state;
            reset_flush_observers();
            check(flush_capture(&context, direct, discard) == BC_STS_SUCCESS,
                  "every nonzero legacy discard value performs discard and restart");
            check(!strcmp(lifecycle_events, "DSENPP") &&
                  stop_calls == 1 && irq_disables == 1 && irq_enables == 1 &&
                  hardware_notifications == 1 && post_calls == 2,
                  "discard orders IRQ quiesce, stop, IRQ restore, notify and restart posts");
            check(stop_observed_state == initial_state &&
                  stop_observed_active == 1 && stop_observed_ready == 1 &&
                  stop_observed_free == 1 && !stop_observed_sem &&
                  stop_observed_irq == 1,
                  "discard stop observes unchanged state and all queue owners under IRQ exclusion");
            check(notify_observed_state == initial_state &&
                  !notify_observed_active && !notify_observed_ready &&
                  notify_observed_free == 3 && !notify_observed_sem &&
                  !notify_observed_irq,
                  "discard notification follows queue recycling with the capture lock held");
            check(post_observed_state == initial_state && !post_observed_sem &&
                  !post_observed_irq && context.state == initial_state,
                  "discard restart posts after notification without changing command state");
            check(!unmaps[0] && !unmaps[1] && !unmaps[2],
                  "discard retains every mapped registration");
            inventory(2, 0, 1);
            drain();
        }

        reset(device);
        reset_flush_observers();
        check(flush_capture(&context, direct, 1) == BC_STS_SUCCESS &&
              !strcmp(lifecycle_events, "DSEN") && stop_calls == 1 &&
              hardware_notifications == 1 && !post_calls,
              "discard normalizes an empty restart NO_DATA result to success");
        check(notify_observed_state == BC_LINK_READY &&
              !notify_observed_active && !notify_observed_ready &&
              !notify_observed_free && !notify_observed_sem &&
              !notify_observed_irq,
              "empty discard still notifies restart after a balanced stop");
        inventory(0, 0, 0);

        reset(device);
        check(add(0) == BC_STS_SUCCESS, "prepare BUSY discard restart");
        post_status = BC_STS_BUSY;
        reset_flush_observers();
        check(flush_capture(&context, direct, 1) == BC_STS_SUCCESS &&
              !strcmp(lifecycle_events, "DSENP") && post_calls == 1,
              "discard accepts BUSY restart ownership without spinning");
        check(!unmaps[0], "BUSY discard restart retains the mapped registration");
        inventory(0, 0, 1);
        drain();

        reset(device);
        check(add(0) == BC_STS_SUCCESS, "prepare NO_DATA discard restart");
        post_status = BC_STS_NO_DATA;
        reset_flush_observers();
        check(flush_capture(&context, direct, 1) == BC_STS_SUCCESS &&
              !strcmp(lifecycle_events, "DSENP") && post_calls == 1,
              "discard preserves compatibility by normalizing restart NO_DATA");
        inventory(0, 0, 1);
        drain();

        reset(device);
        check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS &&
              add(2) == BC_STS_SUCCESS,
              "prepare partial discard restart");
        complete(0);
        fail_post_call = 2;
        post_status = BC_STS_IO_ERROR;
        reset_flush_observers();
        check(flush_capture(&context, direct, 1) == BC_STS_IO_ERROR &&
              !strcmp(lifecycle_events, "DSENPP") && post_calls == 2,
              "discard reports a hard error after a partial restart");
        check(context.state == BC_LINK_READY && !unmaps[0] && !unmaps[1] &&
              !unmaps[2],
              "partial restart keeps command state and every registration mapped");
        inventory(1, 0, 2);
        drain();

        reset(device);
        check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS,
              "prepare failed discard restart notification");
        notify_ok = false;
        reset_flush_observers();
        check(flush_capture(&context, direct, 1) == BC_STS_IO_ERROR &&
              !strcmp(lifecycle_events, "DSEN") && !post_calls,
              "discard reports notification failure without attempting restart DMA");
        check(context.state == BC_LINK_READY && !unmaps[0] && !unmaps[1],
              "notification failure retains capture state and mappings for later teardown");
        inventory(0, 0, 2);
        drain();

        reset(device);
        check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS &&
              add(2) == BC_STS_SUCCESS,
              "prepare discard DMA-stop fault");
        complete(0);
        stop_fault = true;
        reset_flush_observers();
        check(flush_capture(&context, direct, 1) == BC_STS_IO_ERROR &&
              !strcmp(lifecycle_events, "DSE") && stop_calls == 1 &&
              !hardware_notifications && !post_calls,
              "discard stops before notification and restart when DMA quiesce faults");
        check(context.state == BC_LINK_READY && hardware.dma_fault &&
              !unmaps[0] && !unmaps[1] && !unmaps[2],
              "discard stop fault preserves state and all queue ownership");
        inventory(1, 1, 1);
        check(!adapter.present && context.cin_wait_exit,
              "discard engine failure cancels device admission without releasing ownership");
    }
}
static void full_flush_cases(uint32_t device)
{
    for (unsigned direct = 0; direct < 2; direct++) {
        uint32_t initial_state = BC_LINK_READY | BC_LINK_PAUSED | BC_LINK_RESUME;
        uint32_t stopped_state = initial_state & ~(BC_LINK_CAP_EN | BC_LINK_FMT_CHG);

        reset(device);
        check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS &&
              add(2) == BC_STS_SUCCESS,
              "prepare active, ready and free registrations for full flush");
        complete(0);
        context.state = initial_state;
        reset_flush_observers();
        check(flush_capture(&context, direct, 0) == BC_STS_SUCCESS &&
              !strcmp(lifecycle_events, "DSE") && context.state == stopped_state,
              "full flush clears capture and format state and does not restart");
        check(stop_observed_state == stopped_state &&
              stop_observed_active == 1 && stop_observed_ready == 1 &&
              stop_observed_free == 1 && !stop_observed_sem &&
              stop_observed_irq == 1,
              "full flush publishes stopped state before draining under IRQ exclusion");
        check(!hardware_notifications && !post_calls &&
              unmaps[0] == 1 && unmaps[1] == 1 && unmaps[2] == 1,
              "full flush releases every queue registration exactly once without restart");
        inventory(0, 0, 0);
        drain();

        reset(device);
        check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS &&
              add(2) == BC_STS_SUCCESS,
              "prepare full-flush DMA-stop fault");
        complete(0);
        context.state = initial_state;
        stop_fault = true;
        reset_flush_observers();
        check(flush_capture(&context, direct, 0) == BC_STS_IO_ERROR &&
              !strcmp(lifecycle_events, "DSE") && context.state == stopped_state,
              "full flush preserves stop failure after clearing capture and format state");
        check(stop_observed_state == stopped_state && hardware.dma_fault &&
              !hardware_notifications && !post_calls &&
              !unmaps[0] && !unmaps[1] && !unmaps[2],
              "failed full flush keeps every registration reachable without restart");
        inventory(1, 1, 1);
        check(!adapter.present && context.cin_wait_exit,
              "full flush failure keeps terminal admission cancelled");
    }
}
static void invalid_command_arguments(uint32_t device)
{
    BC_STATUS (*commands[])(struct crystalhd_cmd *, crystalhd_ioctl_data *) = {
        bc_cproc_start_capture, bc_cproc_flush_cap_buffs, bc_cproc_add_cap_buff,
        bc_cproc_fetch_frame };
    for (unsigned c = 0; c < sizeof(commands) / sizeof(commands[0]); c++) {
        for (unsigned arg = 0; arg < 4; arg++) {
            crystalhd_ioctl_data data = {0};
            pid_t child;
            int status;
            reset(device);
            data.udata.u.RxBuffs.YuvBuff = (uint8_t *)buffers[0];
            data.udata.u.RxBuffs.YuvBuffSz = sizeof(buffers[0]);
            data.udata.u.RxBuffs.UVbuffOffset = 128;
            if (arg == 3) context.hw_ctx = NULL;
            fflush(NULL);
            child = fork();
            assert(child >= 0);
            if (!child) {
                struct rlimit no_core = {0, 0};
                assert(setrlimit(RLIMIT_CORE, &no_core) == 0);
                BC_STATUS result = commands[c](arg == 0 || arg == 2 ? NULL : &context,
                                               arg == 1 || arg == 2 ? NULL : &data);
                bool clean = result == BC_STS_INV_ARG && !sem_attempts &&
                    !hardware_notifications && !maps && !post_calls && !stop_calls &&
                    !irq_disables && !notify_calls && !pause_calls &&
                    context.state == BC_LINK_READY && hardware.fetch_sem == 1;
                if (!clean)
                    fprintf(stderr, "invalid RX command %u argument %u changed state or callbacks\n", c, arg);
                _exit(clean ? EXIT_SUCCESS : EXIT_FAILURE);
            }
            assert(waitpid(child, &status, 0) == child);
            if (WIFSIGNALED(status))
                fprintf(stderr, "invalid RX command %u argument %u raised signal %d\n",
                        c, arg, WTERMSIG(status));
            check(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
                  "invalid RX command arguments reject before locking, mapping or hardware callbacks");
            inventory(0, 0, 0);
        }
    }
}
static void invalid_capture_start_arguments(uint32_t device)
{
    for (unsigned arg = 0; arg < 2; arg++) {
        pid_t child;
        int status;

        reset(device);
        if (arg) context.hw_ctx = NULL;
        fflush(NULL);
        child = fork();
        assert(child >= 0);
        if (!child) {
            struct rlimit no_core = {0, 0};
            assert(setrlimit(RLIMIT_CORE, &no_core) == 0);
            BC_STATUS result = crystalhd_capture_start(arg ? &context : NULL, 19, 7);
            bool clean = result == BC_STS_INV_ARG && !sem_attempts &&
                !hardware_notifications && !post_calls && !stop_calls &&
                !irq_disables && !notify_calls && !pause_calls &&
                context.state == BC_LINK_READY && hardware.fetch_sem == 1;
            if (!clean)
                fprintf(stderr, "invalid scalar capture-start argument %u changed state or callbacks\n", arg);
            _exit(clean ? EXIT_SUCCESS : EXIT_FAILURE);
        }
        assert(waitpid(child, &status, 0) == child);
        if (WIFSIGNALED(status))
            fprintf(stderr, "invalid scalar capture-start argument %u raised signal %d\n",
                    arg, WTERMSIG(status));
        check(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
              "scalar capture-start rejects NULL context or hardware before locking or callbacks");
        inventory(0, 0, 0);
    }
}
static void start_command_cases(uint32_t device)
{
    const uint32_t initial_states[] = { BC_LINK_INVALID, BC_LINK_INIT,
        BC_LINK_CAP_EN, BC_LINK_FMT_CHG, BC_LINK_INIT | BC_LINK_FMT_CHG,
        BC_LINK_READY, BC_LINK_READY | BC_LINK_PAUSED,
        BC_LINK_READY | BC_LINK_SUSPEND, BC_LINK_READY | BC_LINK_RESUME };
    const BC_STATUS statuses[] = {
        BC_STS_SUCCESS, BC_STS_BUSY, BC_STS_NO_DATA, BC_STS_IO_ERROR };
    const unsigned frontend_count = 2;

    for (unsigned direct = 0; direct < frontend_count; direct++) {
        for (unsigned variant = 0; variant < 4; variant++) {
            uint32_t pause = variant & 1 ? 17 : 0;
            uint32_t resume = variant & 2 ? 9 : 0;
            uint32_t expected_pause = pause ? pause : HW_PAUSE_THRESHOLD;
            uint32_t expected_resume = resume ? resume : HW_RESUME_THRESHOLD;

            reset(device);
            context.state = BC_LINK_INIT;
            hardware.DrvTotalFrmCaptured = 23;
            hardware.DefaultPauseThreshold = 31;
            check(start_capture(direct, pause, resume) == BC_STS_SUCCESS,
                  "start accepts independent default or explicit scalar thresholds");
            check(hardware.PauseThreshold == expected_pause &&
                  hardware.ResumeThreshold == expected_resume &&
                  hardware.DefaultPauseThreshold == expected_pause &&
                  !hardware.DrvTotalFrmCaptured,
                  "start stores thresholds, records the pause default and resets captured count");
            check(start_notify_observed == 1 && !start_notify_notifications &&
                  start_notify_state == BC_LINK_INIT &&
                  start_notify_pause == expected_pause &&
                  start_notify_resume == expected_resume &&
                  start_notify_default == expected_pause && !start_notify_frames &&
                  !start_notify_sem,
                  "start commits thresholds and counters under the lock before notifying hardware");
            check(context.state == (BC_LINK_INIT | BC_LINK_CAP_EN) &&
                  hardware_notifications == 1 && sem_attempts == 1 &&
                  !post_calls && !start_post_observed,
                  "start enables capture after notification but waits for exact format readiness");
            inventory(0, 0, 0);
        }

        reset(device);
        context.state = BC_LINK_INIT | BC_LINK_FMT_CHG;
        check(start_capture(direct, 0, 0) == BC_STS_SUCCESS &&
              context.state == BC_LINK_READY && hardware_notifications == 1 &&
              sem_attempts == 1 && !post_calls && !start_post_observed,
              "empty ready capture normalizes hardware NO_DATA to success");
        inventory(0, 0, 0);

        reset(device);
        context.state = BC_LINK_INIT | BC_LINK_FMT_CHG;
        hardware.DrvTotalFrmCaptured = 23;
        hardware.DefaultPauseThreshold = 31;
        notify_ok = false;
        check(start_capture(direct, 19, 7) == BC_STS_IO_ERROR &&
              context.state == (BC_LINK_INIT | BC_LINK_FMT_CHG) &&
              hardware.PauseThreshold == 19 && hardware.ResumeThreshold == 7 &&
              hardware.DefaultPauseThreshold == 19 && !hardware.DrvTotalFrmCaptured &&
              hardware_notifications == 1 && !post_calls,
              "failed notification keeps state unpublished after committing start parameters");
        check(start_notify_observed == 1 &&
              start_notify_state == (BC_LINK_INIT | BC_LINK_FMT_CHG) &&
              start_notify_pause == 19 && start_notify_resume == 7 &&
              start_notify_default == 19 && !start_notify_frames &&
              !start_notify_sem && !start_notify_notifications,
              "failed notification observes old state and the newly committed scalar parameters");
        inventory(0, 0, 0);

        reset(device);
        hardware.DrvTotalFrmCaptured = 23;
        hardware.DefaultPauseThreshold = 31;
        interrupt_lock = true;
        check(start_capture(direct, 19, 7) == BC_STS_IO_USER_ABORT &&
              hardware.PauseThreshold == 12 && hardware.ResumeThreshold == 4 &&
              hardware.DefaultPauseThreshold == 31 && hardware.DrvTotalFrmCaptured == 23 &&
              context.state == BC_LINK_READY && !hardware_notifications && !post_calls &&
              !start_notify_observed && !start_post_observed && sem_attempts == 1,
              "interrupted start leaves thresholds, counters and capture state unchanged");
        inventory(0, 0, 0);

        for (unsigned s = 0; s < sizeof(initial_states) / sizeof(initial_states[0]); s++) {
            bool starts_dma = (initial_states[s] | BC_LINK_CAP_EN) == BC_LINK_READY;

            reset(device);
            context.state = BC_LINK_INIT;
            check(add(0) == BC_STS_SUCCESS,
                  "prepare one deferred buffer for start-state ordering");
            context.state = initial_states[s];
            hardware.DrvTotalFrmCaptured = 23;
            check(start_capture(direct, 21, 10) == BC_STS_SUCCESS,
                  "capture start preserves exact state-gated DMA admission and NO_DATA normalization");
            check(start_notify_observed == 1 && start_notify_state == initial_states[s] &&
                  start_notify_pause == 21 && start_notify_resume == 10 &&
                  start_notify_default == 21 && !start_notify_frames &&
                  !start_notify_sem && !start_notify_notifications,
                  "hardware notification observes parameters before capture-state publication");
            check(context.state == (initial_states[s] | BC_LINK_CAP_EN) &&
                  post_calls == (unsigned)starts_dma &&
                  start_post_observed == starts_dma,
                  "only an exact READY state starts queued capture DMA");
            if (starts_dma)
                check(start_post_state == BC_LINK_READY &&
                      start_post_notifications == 1 && !start_post_sem,
                      "DMA posting observes successful notification and published READY state under lock");
            inventory(starts_dma, 0, !starts_dma);
            drain();
        }

        for (unsigned s = 0; s < sizeof(statuses) / sizeof(statuses[0]); s++) {
            BC_STATUS expected = statuses[s] == BC_STS_IO_ERROR ?
                BC_STS_IO_ERROR : BC_STS_SUCCESS;

            reset(device);
            context.state = BC_LINK_INIT;
            check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS,
                  "prepare queued buffers for capture-start return propagation");
            context.state |= BC_LINK_FMT_CHG;
            post_status = statuses[s];
            check(start_capture(direct, 0, 0) == expected,
                  "start normalizes NO_DATA, accepts BUSY and preserves hard post errors");
            check(context.state == BC_LINK_READY && hardware_notifications == 1 &&
                  sem_attempts == 3 &&
                  post_calls == (statuses[s] == BC_STS_SUCCESS ? 2U : 1U),
                  "ready start posts only buffers admitted by the selected device path");
            check(start_post_observed && start_post_state == BC_LINK_READY &&
                  start_post_notifications == 1 && !start_post_sem,
                  "capture start calls the hardware path after publishing READY under the lock");
            inventory(statuses[s] == BC_STS_SUCCESS ? 2 : 0, 0,
                      statuses[s] == BC_STS_SUCCESS ? 0 : 2);
            drain();
        }

        reset(device);
        context.state = BC_LINK_INIT;
        check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS,
              "prepare queued buffers for faulted capture start");
        context.state |= BC_LINK_FMT_CHG;
        hardware.dma_fault = true;
        check(start_capture(direct, 0, 0) == BC_STS_IO_ERROR &&
              context.state == (BC_LINK_INIT | BC_LINK_FMT_CHG) && !hardware_notifications &&
              !post_calls && !start_post_observed,
              "faulted capture start rejects before state publication or hardware notification");
        inventory(0, 0, 2);
        check(!unmaps[0] && !unmaps[1],
              "faulted start leaves its unposted buffers owned until proven terminal teardown");
    }
}
static void try_dequeue_gate_cases(uint32_t device)
{
    for (unsigned variant = 0; variant < 11; variant++) {
        struct crystalhd_rx_completion result;
        struct crystalhd_cmd *ctx = &context;
        BC_STATUS expected = BC_STS_INV_ARG;

        reset(device);
        memset(&result, 0xa5, sizeof(result));
        switch (variant) {
        case 0: ctx = NULL; break;
        case 1: context.hw_ctx = NULL; break;
        case 2:
            context.hw_ctx = NULL;
            context.state |= BC_LINK_SUSPEND;
            expected = BC_STS_PWR_MGMT;
            break;
        case 3:
            context.hw_ctx = NULL;
            context.state = BC_LINK_INIT;
            expected = BC_STS_ERR_USAGE;
            break;
        case 4:
            context.state = BC_LINK_INIT | BC_LINK_SUSPEND;
            expected = BC_STS_PWR_MGMT;
            break;
        case 5:
        case 6:
            interrupt_lock = true;
            interrupt_after = variant == 6;
            expected = BC_STS_IO_USER_ABORT;
            break;
        case 7:
        case 8:
            try_mutation_at = 1;
            try_sem_mutation = variant == 7 ? 1 : 2;
            expected = variant == 7 ? BC_STS_PWR_MGMT : BC_STS_ERR_USAGE;
            break;
        case 9:
            expected = BC_STS_NO_DATA;
            break;
        case 10:
            try_fetch_mutation = 1;
            expected = BC_STS_PWR_MGMT;
            break;
        }
        check(crystalhd_rx_try_dequeue(ctx, &result) == expected &&
              memory_is_zero(&result, sizeof(result)),
              "try-dequeue preserves gate/interruption status ordering and clears failure output");
        check(!fetch_wait_calls && fetch_try_calls == (variant >= 9) &&
              hardware.fetch_sem == 1 && !hardware.lock && !hardware.rx_lock,
              "try-dequeue failure leaves no owner or lock and never enters picture wait");
        inventory(0, 0, 0);
    }

    reset(device);
    check(crystalhd_rx_try_dequeue(&context, NULL) == BC_STS_INV_ARG &&
          !sem_attempts && !fetch_try_calls && !fetch_wait_calls,
          "try-dequeue rejects null output before locking or fetching");
    check(crystalhd_hw_try_get_cap_buffer(&hardware, NULL, 0) == BC_STS_INV_ARG &&
          !sem_attempts && !fetch_try_calls,
          "hardware try-get rejects null output before locking");
    {
        struct crystalhd_rx_completion result;
        memset(&result, 0xa5, sizeof(result));
        check(crystalhd_hw_try_get_cap_buffer(NULL, &result, 0) == BC_STS_INV_ARG &&
              memory_is_zero(&result, sizeof(result)) && !sem_attempts,
              "hardware try-get clears output for invalid hardware");
    }
    for (unsigned invalid = 0; invalid < 3; invalid++) {
        struct crystalhd_rx_completion result;

        reset(device);
        if (invalid == 0)
            hardware.adp = NULL;
        else if (invalid == 1)
            adapter.pdev = NULL;
        else
            hardware.rx_rdyq = NULL;
        memset(&result, 0xa5, sizeof(result));
        check(crystalhd_hw_try_get_cap_buffer(&hardware, &result, 0) == BC_STS_INV_ARG &&
              memory_is_zero(&result, sizeof(result)) && !sem_attempts &&
              !fetch_try_calls && !fetch_wait_calls,
              "hardware try-get rejects absent adapter, PCI device or ready queue before locking");
    }
}

static void try_dequeue_completion_cases(uint32_t device)
{
    for (unsigned direct = 0; direct < 2; direct++) {
        for (unsigned mode422 = 0; mode422 < 2; mode422++) {
            for (unsigned marker = 0; marker < 4; marker++) {
                struct crystalhd_rx_completion result, expected;
                struct crystalhd_rx_buffer *buffer;
                crystalhd_ioctl_data data = {0};
                uint32_t flags = COMP_FLAG_DATA_VALID |
                    (marker & 1 ? COMP_FLAG_FMT_CHANGE : 0) |
                    (marker & 2 ? COMP_FLAG_PIB_VALID : 0);

                reset(device);
                hardware.rx_cancel_epoch = direct ? UINT64_C(0x100000029) : 0;
                if (direct) {
                    buffer = map_private(mode422);
                    check(submit(&context, buffer) == BC_STS_SUCCESS,
                          "prepare direct completion for bounded dequeue");
                } else {
                    data.udata.u.RxBuffs.YuvBuff = (uint8_t *)buffers[0];
                    data.udata.u.RxBuffs.YuvBuffSz = sizeof(buffers[0]);
                    data.udata.u.RxBuffs.UVbuffOffset = mode422 ? 0 : 128;
                    data.udata.u.RxBuffs.b422Mode = mode422;
                    check(add_data(&data) == BC_STS_SUCCESS,
                          "prepare legacy mapping for bounded dequeue");
                    buffer = &requests[0].rx_buffer;
                }
                complete(0);
                ready.packets[0]->flags = flags;
                fill_ready_pib(ready.packets[0]);
                if (device == BC_PCI_DEVID_FLEA)
                    ready.packets[0]->metadata = (struct crystalhd_rx_metadata){
                        .firmware_timestamp = mode422 ? UINT64_C(0x123456789) : 0,
                        .picture_number = 71, .picture_flags = 0x102,
                        .picture_width = 720, .picture_height = 480,
                        .row_width = 720, .pib_line = 481,
                        .first_pixel_word = UINT32_C(0xa0286028),
                        .valid = true, .eos_trailer = !!mode422,
                    };
                memset(&expected, 0, sizeof(expected));
                expected.buffer = buffer;
                expected.cookie = direct ? (void *)&cookies[0] : (void *)&requests[0];
                expected.flags = flags;
                expected.capture_epoch = direct ? UINT64_C(0x100000029) : 0;
                expected.y_done_sz = 128;
                expected.uv_done_sz = mode422 ? 0 : 64;
                if (marker & 2)
                    apply_expected_pib(&expected.pib, ready.packets[0]);
                if (device == BC_PCI_DEVID_FLEA && !(marker & 1))
                    expected.metadata = (struct crystalhd_rx_metadata){
                        .firmware_timestamp = mode422 ? UINT64_C(0x123456789) : 0,
                        .picture_number = 71, .picture_flags = 0x102,
                        .picture_width = 720, .picture_height = 480,
                        .row_width = 720, .pib_line = 481,
                        .first_pixel_word = UINT32_C(0xa0286028),
                        .valid = true, .eos_trailer = !!mode422,
                    };
                memset(&result, 0xa5, sizeof(result));
                check(crystalhd_rx_try_dequeue(&context, &result) == BC_STS_SUCCESS &&
                      !memcmp(&result, &expected, sizeof(result)) &&
                      !unmaps[0] && !fetch_wait_calls && fetch_try_calls == 1 &&
                      hardware.fetch_sem == 1,
                      "try-dequeue returns exact ordinary/format/PIB fields, metadata and owner without waiting");
                inventory_with_private(0, 0, 0, result.buffer);
                if (marker & 1) {
                    check(crystalhd_rx_ack_format(&context, &result) == BC_STS_SUCCESS &&
                          !result.buffer && !result.cookie && !unmaps[0],
                          "try-dequeued format owner remains consumable by the real acknowledgement path");
                    inventory(1, 0, 0);
                    drain();
                } else {
                    crystalhd_rx_buffer_release(&adapter, result.buffer);
                    check(unmaps[0] == 1, "ordinary try completion releases exactly once in caller");
                    inventory(0, 0, 0);
                }
            }
        }
    }
}

static void try_dequeue_cancellation_cases(uint32_t device)
{
    for (unsigned mutation = 3; mutation <= 6; mutation++) {
        for (unsigned after_pop = 0; after_pop < 2; after_pop++) {
            for (unsigned has_packet = 0; has_packet < 2; has_packet++) {
                struct crystalhd_rx_completion result;
                BC_STATUS expected = mutation == 4 || mutation == 6 ?
                    BC_STS_IO_ERROR : BC_STS_IO_USER_ABORT;

                reset(device);
                if (has_packet) {
                    check(submit(&context, map_private(false)) == BC_STS_SUCCESS,
                          "prepare private completion for bounded dequeue cancellation");
                    complete(0);
                }
                hardware.FleaPowerState = FLEA_PS_LP_COMPLETE;
                hardware.hw_pause_issued = true;
                sem_attempts = 0;
                if (after_pop)
                    try_fetch_mutation = mutation;
                else {
                    try_mutation_at = 2; /* After command admission, at HW semaphore acquisition. */
                    try_sem_mutation = mutation;
                }
                checking_try_release = true;
                memset(&result, 0xa5, sizeof(result));
                check(crystalhd_rx_try_dequeue(&context, &result) == expected &&
                      memory_is_zero(&result, sizeof(result)) &&
                      !fetch_wait_calls && fetch_try_calls == after_pop &&
                      hardware.fetch_sem == 1 && !pause_calls && hardware.hw_pause_issued,
                      "absence/fault/epoch cancellation returns an empty result without stale capture wake");
                check(unmaps[0] == (has_packet && after_pop && !hardware.dma_fault) &&
                      ready.count == (has_packet && !after_pop),
                      "fault cancellation retains detached ownership while safe cancellation releases it");
                check(fallback_count() == (unsigned)(has_packet && after_pop && hardware.dma_fault),
                      "faulted dequeue moves the exact detached owner into retained fallback inventory");
                inventory(0, has_packet && !after_pop, 0);
                checking_try_release = false;
                if (adapter.present && !hardware.dma_fault)
                    drain();
            }
        }
    }
}

static void try_dequeue_epoch_wake_cases(uint32_t device)
{
    struct crystalhd_rx_completion result;

    reset(device);
    check(submit(&context, map_private(false)) == BC_STS_SUCCESS,
          "prepare old packet epoch for bounded dequeue");
    complete(0);
    hardware.rx_cancel_epoch = 1;
    hardware.FleaPowerState = FLEA_PS_LP_PENDING;
    hardware.hw_pause_issued = true;
    checking_try_release = true;
    memset(&result, 0xa5, sizeof(result));
    check(crystalhd_rx_try_dequeue(&context, &result) == BC_STS_IO_USER_ABORT &&
          memory_is_zero(&result, sizeof(result)) && unmaps[0] == 1 &&
          fetch_try_calls == 1 && !fetch_wait_calls && !pause_calls &&
          hardware.fetch_sem == 1,
          "matching admission epoch still rejects an independently stale packet epoch");
    inventory(0, 0, 0);
    checking_try_release = false;

    if (device == BC_PCI_DEVID_FLEA) {
        for (unsigned power = 0; power < 2; power++) {
            reset(device);
            hardware.FleaPowerState = power ? FLEA_PS_LP_PENDING : FLEA_PS_LP_COMPLETE;
            hardware.hw_pause_issued = true;
            memset(&result, 0xa5, sizeof(result));
            check(crystalhd_rx_try_dequeue(&context, &result) == BC_STS_NO_DATA &&
                  memory_is_zero(&result, sizeof(result)) && pause_calls == 1 &&
                  !hardware.hw_pause_issued && !fetch_wait_calls &&
                  fetch_try_calls == 1 && hardware.fetch_sem == 1,
                  "current-epoch empty try-dequeue retains Flea low-power wake semantics without picture wait");
            inventory(0, 0, 0);
        }
    }
}

int main(void)
{
    uint32_t devices[] = { BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA };
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        try_dequeue_gate_cases(devices[i]);
        try_dequeue_completion_cases(devices[i]);
        try_dequeue_cancellation_cases(devices[i]);
        try_dequeue_epoch_wake_cases(devices[i]);
        dequeue_argument_cases(devices[i]);
        dequeue_gate_wait_cases(devices[i]);
        mapped_dequeue_cases(devices[i]);
        reverse_list_completion_case(devices[i]);
        metadata_pool_reset_case(devices[i]);
        metadata_completion_reset_cases(devices[i]);
        metadata_suppression_cases(devices[i]);
        pib_dequeue_cases(devices[i]);
        direct_format_dequeue_cases(devices[i]);
        completion_case(devices[i]);
        admission_layout_cases(devices[i]);
        admission_state_cases(devices[i]);
        mapped_admission_cases(devices[i]);
        mapped_completion_cases(devices[i]);
        add_failures(devices[i]);
        retry_cases(devices[i]);
        fallback_ownership_cases(devices[i]);
        fallback_free_ring_cases(devices[i]);
        quiesced_retirement_cases(devices[i]);
        legacy_fetch_rejects_generic_case(devices[i]);
        free_count_consumer_cases(devices[i]);
        for (unsigned failure = 0; failure < 4; failure++) format_case(devices[i], failure);
        format_without_pib_case(devices[i]);
        format_full_flush_race_cases(devices[i]);
        format_wait_flush_race_cases(devices[i]);
        empty_wait_flush_race_cases(devices[i]);
        fresh_packet_after_wait_restart_cases(devices[i]);
        format_discard_epoch_cases(devices[i]);
        format_failed_stop_epoch_case(devices[i]);
        format_fresh_epoch_cases(devices[i]);
        format_capture_gate_case(devices[i]);
        cancellation_cases(devices[i]);
        fatal_owner_retention_cases(devices[i]);
        fatal_late_rx_cases(devices[i]);
        flush_argument_and_gate_cases(devices[i]);
        pre_capture_full_flush_cases(devices[i]);
        discard_flush_cases(devices[i]);
        full_flush_cases(devices[i]);
        start_command_cases(devices[i]);
        invalid_command_arguments(devices[i]);
        invalid_capture_start_arguments(devices[i]);
    }
    printf("RX ownership: %u scenarios, %u checks, %u failures\n", groups, checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
