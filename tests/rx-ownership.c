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
#include "rx-types.h"

static void discard_log(const char *format, ...) { (void)format; }

#define KERN_ERR ""
#define printk(...) ((void)0)
#define dev_err(dev, ...) ((void)(dev), discard_log(__VA_ARGS__))
#define dev_dbg(dev, ...) ((void)(dev))
#define dev_info(dev, ...) ((void)(dev))
#define READ_ONCE(value) (value)
#define spin_lock_irqsave(lock, flags) \
    (assert(*(lock) == 0), *(lock) = 1, (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) \
    (assert(*(lock) == 1), *(lock) = 0, (void)(flags))

struct device { int unused; };
struct pci_dev { struct device dev; int irq; uint32_t device; };
struct crystalhd_adp { struct pci_dev *pdev; };
struct crystalhd_dio_req { struct crystalhd_dio_user_info uinfo; };
struct crystalhd_dioq {
    struct crystalhd_rx_dma_pkt *packets[BC_RX_LIST_CNT];
    uint32_t tags[BC_RX_LIST_CNT];
    unsigned count;
};
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    struct crystalhd_rx_dma_pkt *rx_pkt_pool_head;
    struct crystalhd_dioq *rx_actq, *rx_rdyq, *rx_freeq;
    uint64_t rx_cancel_epoch;
    int lock, rx_lock, fetch_sem;
    bool hw_pause_issued, dma_fault;
    uint32_t rx_pkt_tag_seed, rx_list_post_index, DrvTotalFrmCaptured;
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
};
struct crystalhd_cmd {
    struct crystalhd_adp *adp;
    struct crystalhd_hw *hw_ctx;
    uint32_t state;
};

static struct pci_dev endpoint = { .irq = 17 };
static struct crystalhd_adp adapter = { .pdev = &endpoint };
static struct crystalhd_hw hardware;
static struct crystalhd_cmd context;
static struct crystalhd_dioq active, ready, available;
static struct crystalhd_rx_dma_pkt packets[BC_RX_LIST_CNT];
static struct crystalhd_dio_req requests[BC_RX_LIST_CNT];
static uint32_t buffers[BC_RX_LIST_CNT][64];
static bool mapped[BC_RX_LIST_CNT];
static unsigned unmaps[BC_RX_LIST_CNT];
static unsigned checks, failures, groups, maps, post_calls, stop_calls, irq_depth;
static unsigned irq_disables, irq_enables, notify_calls, pause_calls;
static unsigned sem_attempts, hardware_notifications, map_attempts, translate_calls;
static unsigned fetch_wait_calls;
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
static bool checking_admission;
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
static size_t packet_index(const struct crystalhd_rx_dma_pkt *packet)
{
    size_t i;
    for (i = 0; i < BC_RX_LIST_CNT; i++)
        if (packet == &packets[i]) return i;
    abort();
}
static unsigned queued_request(const struct crystalhd_dio_req *request)
{
    struct crystalhd_dioq *queues[] = { &active, &ready, &available };
    unsigned count = 0;
    for (size_t q = 0; q < 3; q++)
        for (unsigned i = 0; i < queues[q]->count; i++)
            count += queues[q]->packets[i]->dio_req == request;
    return count;
}
static void inventory_with_private(unsigned active_count, unsigned ready_count,
                                   unsigned free_count, const struct crystalhd_dio_req *private_request)
{
    unsigned seen[BC_RX_LIST_CNT] = {0}, pool_count = 0;
    struct crystalhd_dioq *queues[] = { &active, &ready, &available };
    struct crystalhd_rx_dma_pkt *packet = hardware.rx_pkt_pool_head;
    while (packet) {
        assert(pool_count++ < BC_RX_LIST_CNT);
        seen[packet_index(packet)]++;
        packet = packet->next;
    }
    for (size_t q = 0; q < 3; q++) {
        for (unsigned i = 0; i < queues[q]->count; i++) {
            packet = queues[q]->packets[i];
            seen[packet_index(packet)]++;
            check(packet->dio_req && mapped[request_index(packet->dio_req)],
                  "queued packet retains a live mapping");
        }
    }
    for (size_t i = 0; i < BC_RX_LIST_CNT; i++) {
        check(seen[i] == 1, "each RX packet has exactly one pool or queue owner");
        check(queued_request(&requests[i]) ==
              (unsigned)(mapped[i] && &requests[i] != private_request),
              "each live mapping is caller-private or has exactly one queued owner");
        check(unmaps[i] <= 1, "a registration is unmapped at most once");
    }
    check(active.count == active_count && ready.count == ready_count &&
          available.count == free_count, "active/ready/free ownership matches transition");
    check(!irq_depth && !hardware.lock && !hardware.rx_lock &&
          hardware.fetch_sem == 1,
          "transition releases IRQ, pool/RX locks and capture semaphore");
    if (private_request)
        check(mapped[request_index(private_request)] && !queued_request(private_request),
              "rejected or fresh mapped request remains caller-private");
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
static BC_STATUS crystalhd_map_dio(struct crystalhd_adp *adp, void *buffer,
        uint32_t size, uint32_t uv, bool mode422, bool tx, struct crystalhd_dio_req **result)
{
    size_t i;
    assert(adp == &adapter && !tx && hardware.fetch_sem == 1);
    map_attempts++;
    if (map_status != BC_STS_SUCCESS) return map_status;
    for (i = 0; i < BC_RX_LIST_CNT; i++) if (buffer == buffers[i]) break;
    assert(i < BC_RX_LIST_CNT && !mapped[i] && !unmaps[i]);
    requests[i].uinfo = (struct crystalhd_dio_user_info){
        .xfr_buff = buffer, .xfr_len = size, .uv_offset = uv, .b422mode = mode422 };
    mapped[i] = true; maps++;
    *result = &requests[i];
    return BC_STS_SUCCESS;
}
static BC_STATUS crystalhd_unmap_dio(struct crystalhd_adp *adp, struct crystalhd_dio_req *request)
{
    size_t i = request_index(request);
    assert(adp == &adapter);
    check(mapped[i] && !unmaps[i] && !queued_request(request),
          "unmap occurs once after all queue ownership is released");
    mapped[i] = false; unmaps[i]++;
    return BC_STS_SUCCESS;
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
    return 0;
}
static void down(int *sem) { assert(sem == &hardware.fetch_sem && *sem == 1); *sem = 0; }
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
static BC_STATUS crystalhd_xlat_sgl_to_dma_desc(struct crystalhd_dio_req *request,
        struct dma_desc_mem *memory, uint32_t *uv, struct device *dev, uint32_t destination)
{
    assert(mapped[request_index(request)] && memory && dev == &endpoint.dev && !destination);
    assert(!hardware.fetch_sem);
    translate_calls++;
    *uv = request->uinfo.uv_offset ? 1 : 0;
    return translate_status;
}
static void notify_free(struct crystalhd_hw *hw, bool change)
{
    assert(hw == &hardware && !change);
    notify_calls++;
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
    if (stop_fault) hw->dma_fault = true;
}
/* Model only the DMA programming contract: success enters the active queue;
 * BUSY and errors return the unowned packet to the real post wrapper.
 */
static BC_STATUS program_dma(struct crystalhd_hw *hw, struct crystalhd_rx_dma_pkt *packet)
{
    BC_STATUS status;
    unsigned index;
    assert(hw == &hardware && mapped[request_index(packet->dio_req)]);
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
#include "rx-post.h"
#include "rx-hardware.h"
#include "rx-command.h"

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
    groups++;
    reset_lifecycle_events();
    memset(&active, 0, sizeof(active));
    memset(&ready, 0, sizeof(ready));
    memset(&available, 0, sizeof(available));
    memset(packets, 0, sizeof(packets));
    memset(requests, 0, sizeof(requests));
    memset(mapped, 0, sizeof(mapped));
    memset(unmaps, 0, sizeof(unmaps));
    endpoint.device = device;
    hardware = (struct crystalhd_hw){ .adp = &adapter, .fetch_sem = 1,
        .rx_actq = &active, .rx_rdyq = &ready, .rx_freeq = &available,
        .rx_pkt_tag_seed = 0x70029070, .PauseThreshold = 12, .ResumeThreshold = 4,
        .PDRatio = 60, .FleaPowerState = FLEA_PS_ACTIVE,
        .pfnPostRxSideBuff = device == BC_PCI_DEVID_FLEA ?
            crystalhd_flea_hw_post_cap_buff : crystalhd_link_hw_post_cap_buff,
        .pfnNotifyFLLChange = notify_free, .pfnIssuePause = pause_capture,
        .pfnHWGetDoneSize = done_size, .pfnNotifyHardware = notify_hardware,
        .pfnStopRXDMAEngines = stop_dma };
    context = (struct crystalhd_cmd){ .adp = &adapter, .hw_ctx = &hardware,
        .state = BC_LINK_READY };
    for (unsigned i = 0; i < BC_RX_LIST_CNT; i++) {
        packets[i].desc_mem.phy_addr = 0x10000 + i * 4096;
        crystalhd_hw_free_rx_pkt(&hardware, &packets[i]);
    }
    maps = post_calls = stop_calls = irq_depth = irq_disables = irq_enables = 0;
    notify_calls = pause_calls = fail_post_call = 0;
    sem_attempts = hardware_notifications = map_attempts = translate_calls = 0;
    fetch_wait_calls = interrupt_after = 0;
    map_status = translate_status = post_status = queue_status = BC_STS_SUCCESS;
    wait_flush_status = BC_STS_ERROR;
    interrupt_lock = wait_signal = wait_suspend = wait_full_flush = false;
    wait_restart_fresh = wait_restart_mode422 = false;
    wait_restart_power = FLEA_PS_ACTIVE;
    wait_restart_start_status = wait_restart_add_status = BC_STS_ERROR;
    wait_restart_ready_status = wait_restart_complete_status = BC_STS_ERROR;
    stop_fault = false;
    checking_admission = false;
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
static struct crystalhd_dio_req *map_private(bool mode422);
static BC_STATUS submit(struct crystalhd_cmd *ctx,
                        struct crystalhd_dio_req *request);
static void dequeue_argument_cases(uint32_t device)
{
    struct crystalhd_rx_dequeue_result result;

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
            struct crystalhd_rx_dequeue_result result;
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
        struct crystalhd_rx_dequeue_result result;
        struct crystalhd_dio_req *request;

        reset(device);
        request = map_private(mode422);
        check(submit(&context, request) == BC_STS_SUCCESS,
              "prepare directly submitted mapping for raw dequeue");
        complete(0);
        memset(&result, 0xa5, sizeof(result));
        check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
              result.dio == request && result.flags == COMP_FLAG_DATA_VALID &&
              result.y_done_sz == 128 &&
              result.uv_done_sz == (mode422 ? 0U : 64U) &&
              memory_is_zero(&result.pib, sizeof(result.pib)) &&
              request->uinfo.comp_flags == result.flags && !unmaps[0],
              "raw dequeue returns one mapped 420 or 422 completion without retiring it");
        inventory_with_private(0, 0, 0, result.dio);
        check(submit(&context, result.dio) == BC_STS_SUCCESS &&
              maps == 1 && !unmaps[0],
              "raw dequeue result can return through mapped RX admission without remapping");
        inventory(1, 0, 0);
        complete(0);
        memset(&result, 0xa5, sizeof(result));
        check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
              result.dio == request && result.flags == COMP_FLAG_DATA_VALID &&
              maps == 1 && !unmaps[0],
              "resubmitted mapping completes and dequeues with the same identity");
        inventory_with_private(0, 0, 0, result.dio);
        check(crystalhd_unmap_dio(&adapter, result.dio) == BC_STS_SUCCESS,
              "raw dequeue caller retires its detached mapping exactly once");
        inventory(0, 0, 0);

        reset(device);
        check(add(0) == BC_STS_SUCCESS,
              "prepare legacy-submitted mapping for raw dequeue");
        complete(0);
        memset(&result, 0xa5, sizeof(result));
        check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
              result.dio == &requests[0] && result.flags == COMP_FLAG_DATA_VALID &&
              !unmaps[0],
              "raw dequeue accepts a legacy-submitted completion without remapping");
        inventory_with_private(0, 0, 0, result.dio);
        check(crystalhd_unmap_dio(&adapter, result.dio) == BC_STS_SUCCESS,
              "legacy-submitted raw result remains caller-owned until release");
        inventory(0, 0, 0);
    }
}
static void pib_dequeue_cases(uint32_t device)
{
    struct crystalhd_rx_dequeue_result result, expected_result;
    crystalhd_ioctl_data data;
    BC_DEC_OUT_BUFF expected_frame;

    reset(device);
    check(add(0) == BC_STS_SUCCESS, "prepare PIB completion for raw dequeue");
    complete(0);
    ready.packets[0]->flags = COMP_FLAG_DATA_VALID | COMP_FLAG_PIB_VALID;
    fill_ready_pib(ready.packets[0]);
    memset(&result, 0xa5, sizeof(result));
    memset(&expected_result, 0, sizeof(expected_result));
    expected_result.dio = &requests[0];
    expected_result.flags = COMP_FLAG_DATA_VALID | COMP_FLAG_PIB_VALID;
    expected_result.y_done_sz = 128;
    expected_result.uv_done_sz = 64;
    apply_expected_pib(&expected_result.pib, ready.packets[0]);
    check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
          !memcmp(&result, &expected_result, sizeof(result)) && !unmaps[0],
          "raw dequeue snapshots only valid completion metadata and keeps mapping ownership");
    inventory_with_private(0, 0, 0, result.dio);
    check(crystalhd_unmap_dio(&adapter, result.dio) == BC_STS_SUCCESS,
          "raw PIB completion is released exactly once by its caller");
    inventory(0, 0, 0);

    reset(device);
    check(add(0) == BC_STS_SUCCESS,
          "prepare completion without valid PIB for legacy dequeue");
    complete(0);
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
          "legacy dequeue without valid PIB preserves every unrelated output byte");
    inventory(0, 0, 0);

    reset(device);
    check(add(0) == BC_STS_SUCCESS, "prepare PIB completion for legacy dequeue");
    complete(0);
    ready.packets[0]->flags = COMP_FLAG_DATA_VALID | COMP_FLAG_PIB_VALID;
    fill_ready_pib(ready.packets[0]);
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
          "legacy dequeue changes only Flags, valid PIB fields and six output fields");
    inventory(0, 0, 0);
}
static void direct_format_dequeue_cases(uint32_t device)
{
    for (unsigned pib_valid = 0; pib_valid < 2; pib_valid++) {
        struct crystalhd_rx_dequeue_result result, expected;
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
        expected.dio = &requests[0];
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
        inventory_with_private(0, 0, 0, result.dio);
        check(crystalhd_unmap_dio(&adapter, result.dio) == BC_STS_SUCCESS,
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
    for (unsigned variant = 0; variant < 7; variant++) {
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
        check(add_data(&data) == expected, "legacy admission preserves layout validation status");
        if (variant < 5) {
            check(!map_attempts && !sem_attempts && !post_calls,
                  "invalid layout is rejected before mapping, locking or posting");
            inventory(0, 0, 0);
        } else {
            check(map_attempts == 1 && sem_attempts == 1 &&
                  requests[0].uinfo.xfr_buff == buffers[0] &&
                  requests[0].uinfo.xfr_len == 192 &&
                  requests[0].uinfo.uv_offset == (variant == 6 ? 0U : 64U) &&
                  requests[0].uinfo.b422mode == (variant == 6) && !unmaps[0],
                  "legacy admission preserves mapped 420 and 422 layouts");
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
                bool rejected = post && fault;
                reset(device);
                context.state = states[s];
                hardware.hw_pause_issued = paused;
                hardware.dma_fault = fault;
                check(add(0) == (rejected ? BC_STS_IO_ERROR : BC_STS_SUCCESS),
                      "only exact READY and unpaused admission attempts DMA, preserving fault policy");
                check(context.state == states[s] && hardware.hw_pause_issued == (bool)paused &&
                      hardware.dma_fault == (bool)fault && post_calls == (unsigned)post &&
                      map_attempts == 1 && sem_attempts == 1 && unmaps[0] == (unsigned)rejected,
                      "admission preserves state, balances locking and releases only rejected mappings");
                inventory(post && !fault, 0, !post);
                /* The DMA boundary now models a quiesced engine for cleanup. */
                hardware.dma_fault = false;
                drain();
            }
        }
    }
}
static struct crystalhd_dio_req *map_private(bool mode422)
{
    struct crystalhd_dio_req *request = NULL;
    check(crystalhd_map_dio(&adapter, buffers[0], 192, mode422 ? 0 : 64,
                           mode422, false, &request) == BC_STS_SUCCESS,
          "frontend prepares a fresh private mapped RX request");
    inventory_with_private(0, 0, 0, request);
    return request;
}
static BC_STATUS submit(struct crystalhd_cmd *ctx, struct crystalhd_dio_req *request)
{
    BC_STATUS status;
    checking_admission = true;
    status = crystalhd_rx_submit(ctx, request);
    checking_admission = false;
    return status;
}
static void mapped_admission_cases(uint32_t device)
{
    enum { IMMEDIATE, BUSY, DEFERRED, PAUSED, INTERRUPTED, NO_PACKET,
           BAD_DESCRIPTOR, POST_ERROR, ACTIVE_ERROR, BUSY_QUEUE_ERROR,
           FREE_QUEUE_ERROR, DMA_FAULT, NULL_CONTEXT, NULL_HARDWARE, NULL_REQUEST };
    for (unsigned variant = IMMEDIATE; variant <= NULL_REQUEST; variant++) {
        struct crystalhd_dio_req *request;
        struct crystalhd_rx_dma_pkt *pool;
        struct crystalhd_dio_user_info before;
        BC_STATUS expected = BC_STS_SUCCESS;
        uint32_t state;
        reset(device);
        request = map_private(false);
        memcpy(&before, &request->uinfo, sizeof(before));
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
                     variant == NULL_REQUEST ? NULL : request) == expected,
              "mapped admission normalizes accepted BUSY and preserves rejection status");
        if (variant == NO_PACKET) hardware.rx_pkt_pool_head = pool;
        check(maps == 1 && map_attempts == 1 && !unmaps[0] &&
              !memcmp(&request->uinfo, &before, sizeof(before)),
              "mapped admission neither maps, unmaps nor rewrites the borrowed request");
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
            inventory_with_private(0, 0, 0, request);
            check(crystalhd_unmap_dio(&adapter, request) == BC_STS_SUCCESS,
                  "caller releases the mapping after failed admission");
            inventory(0, 0, 0);
        }
        /* No accepted DMA remains faulted; cleanup models a quiesced engine. */
        hardware.dma_fault = false;
        drain();
    }
}
static void mapped_completion_cases(uint32_t device)
{
    for (unsigned mode422 = 0; mode422 < 2; mode422++) {
        crystalhd_ioctl_data data = {0};
        struct crystalhd_dio_req *request;
        reset(device);
        request = map_private(mode422);
        check(submit(&context, request) == BC_STS_SUCCESS && maps == 1 && !unmaps[0],
              "direct admission transfers the mapped registration to capture");
        inventory(1, 0, 0);
        complete(0);
        inventory(0, 1, 0);
        check(bc_cproc_fetch_frame(&context, &data) == BC_STS_SUCCESS &&
              data.udata.u.DecOutData.Flags == COMP_FLAG_DATA_VALID &&
              data.udata.u.DecOutData.OutPutBuffs.YuvBuff == (uint8_t *)buffers[0] &&
              data.udata.u.DecOutData.OutPutBuffs.YuvBuffSz == 192 &&
              data.udata.u.DecOutData.OutPutBuffs.UVbuffOffset == (mode422 ? 0U : 64U) &&
              data.udata.u.DecOutData.OutPutBuffs.b422Mode == mode422 &&
              data.udata.u.DecOutData.OutPutBuffs.YBuffDoneSz == 128 &&
              data.udata.u.DecOutData.OutPutBuffs.UVBuffDoneSz == (mode422 ? 0U : 64U) &&
              map_attempts == 1 && maps == 1 && unmaps[0] == 1,
              "legacy fetch retires directly submitted 420 and 422 layouts exactly once");
        inventory(0, 0, 0);
        drain();
    }
}
static void add_failures(uint32_t device)
{
    struct crystalhd_dio_req private_request = {0};
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
    check(crystalhd_hw_add_cap_buffer(&hardware, &private_request, false) == BC_STS_INSUFF_RES,
          "packet pool exhaustion leaves all existing registrations reachable");
    inventory(0, 0, BC_RX_LIST_CNT);
    drain();
}
static void retry_cases(uint32_t device)
{
    BC_STATUS statuses[] = { BC_STS_SUCCESS, BC_STS_BUSY, BC_STS_IO_ERROR };
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        reset(device);
        check(add(0) == BC_STS_SUCCESS, "queue registration for completion retry");
        post_status = statuses[i];
        check(crystalhd_rx_pkt_done(&hardware, 0, BC_STS_IO_ERROR) == statuses[i],
              "failed completion propagates repost result");
        check(!unmaps[0], "completion retries never unmap in IRQ context");
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
            struct crystalhd_rx_dequeue_result result;
            struct crystalhd_dio_req *request;
            uint32_t expected_state = BC_LINK_INIT;
            unsigned post_before;

            reset(device);
            request = map_private(mode422);
            check(submit(&context, request) == BC_STS_SUCCESS,
                  "prepare format completion for a concurrent full flush");
            complete(0);
            ready.packets[0]->flags = COMP_FLAG_FMT_CHANGE |
                COMP_FLAG_DATA_VALID | COMP_FLAG_PIB_VALID;
            fill_ready_pib(ready.packets[0]);
            context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
            check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
                  result.dio == request && !unmaps[0],
                  "format dequeue detaches the old mapped registration");
            inventory_with_private(0, 0, 0, result.dio);

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
            check(bc_cproc_fmt_change(&context, &result) ==
                      BC_STS_IO_USER_ABORT,
                  "late format completion is cancelled after a full flush");
            check(!result.dio && context.state == expected_state &&
                  unmaps[0] == 1 &&
                  post_calls == post_before && !mapped[0] &&
                  (!restart || (mapped[1] && !unmaps[1])),
                  "cancelled format completion cannot restore state, queue or DMA ownership");
            check(bc_cproc_fmt_change(&context, &result) == BC_STS_INV_ARG &&
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
        struct crystalhd_rx_dequeue_result result;
        struct crystalhd_dio_req *request;
        bool mode422 = variant & 1;
        unsigned post_before;

        reset(device);
        request = map_private(mode422);
        check(submit(&context, request) == BC_STS_SUCCESS,
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
              result.dio == request && result.capture_epoch == 0 &&
              hardware.rx_cancel_epoch == 1 && context.state == BC_LINK_INIT &&
              (device != BC_PCI_DEVID_FLEA ||
               (!pause_calls && hardware.hw_pause_issued)),
              "packet epoch survives a full flush after ready-pop and before dequeue resumes");
        inventory_with_private(0, 0, 0, result.dio);
        post_before = post_calls;
        check(bc_cproc_fmt_change(&context, &result) == BC_STS_IO_USER_ABORT &&
              !result.dio && unmaps[0] == 1 && !mapped[0] &&
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
            struct crystalhd_rx_dequeue_result result;
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
            struct crystalhd_rx_dequeue_result result;

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
            check(result.dio == &requests[0] &&
                  result.capture_epoch == 1 &&
                  result.capture_epoch == hardware.rx_cancel_epoch &&
                  result.flags == COMP_FLAG_FMT_CHANGE &&
                  context.state == BC_LINK_READY,
                  "post-restart dequeue returns only the current-epoch registration");
            check(pause_calls == 1 && !hardware.hw_pause_issued,
                  "a current-epoch packet resumes FLEA after a cross-epoch wait");
            inventory_with_private(0, 0, 0, result.dio);
            check(bc_cproc_fmt_change(&context, &result) == BC_STS_SUCCESS &&
                  !result.dio && !unmaps[0] &&
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
        struct crystalhd_rx_dequeue_result result;
        struct crystalhd_dio_req *request;
        uint64_t epoch;

        reset(device);
        request = map_private(mode422);
        check(submit(&context, request) == BC_STS_SUCCESS,
              "prepare format completion for discard control");
        complete(0);
        ready.packets[0]->flags = COMP_FLAG_FMT_CHANGE;
        context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
        check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS,
              "discard control dequeues one format registration");
        epoch = result.capture_epoch;
        inventory_with_private(0, 0, 0, result.dio);
        check(flush_capture(&context, true, 1) == BC_STS_SUCCESS &&
              hardware.rx_cancel_epoch == epoch,
              "discard preserves the capture epoch for retained registrations");
        check(bc_cproc_fmt_change(&context, &result) == BC_STS_SUCCESS &&
              !result.dio && context.state == BC_LINK_READY &&
              !unmaps[0],
              "format completion remains valid across non-destructive discard");
        inventory(1, 0, 0);
        drain();
    }
}
static void format_failed_stop_epoch_case(uint32_t device)
{
    struct crystalhd_rx_dequeue_result result;
    struct crystalhd_dio_req *request;
    unsigned post_before;

    reset(device);
    request = map_private(false);
    check(submit(&context, request) == BC_STS_SUCCESS,
          "prepare detached format completion for a failed full stop");
    complete(0);
    ready.packets[0]->flags = COMP_FLAG_FMT_CHANGE;
    context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
    check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
          result.capture_epoch == 0,
          "failed-stop case retains the admission epoch in the dequeue result");
    inventory_with_private(0, 0, 0, result.dio);
    stop_fault = true;
    check(flush_capture(&context, true, 0) == BC_STS_IO_ERROR &&
          hardware.dma_fault && hardware.rx_cancel_epoch == 1 &&
          context.state == BC_LINK_INIT,
          "destructive flush invalidates detached results even when DMA stop fails");
    post_before = post_calls;
    check(bc_cproc_fmt_change(&context, &result) == BC_STS_IO_USER_ABORT &&
          !result.dio && unmaps[0] == 1 && post_calls == post_before,
          "failed full stop cannot revive its detached format registration");
    inventory(0, 0, 0);
    stop_fault = false;
    hardware.dma_fault = false;
    drain();
}
static void format_fresh_epoch_cases(uint32_t device)
{
    for (unsigned mode422 = 0; mode422 < 2; mode422++) {
        struct crystalhd_rx_dequeue_result result;
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
              result.dio == &requests[1] && result.capture_epoch == 1 &&
              result.capture_epoch == hardware.rx_cancel_epoch,
              "fresh completion carries the nonzero current epoch");
        inventory_with_private(0, 0, 0, result.dio);
        check(bc_cproc_fmt_change(&context, &result) == BC_STS_SUCCESS &&
              !result.dio && !unmaps[1] && context.state == BC_LINK_READY,
              "current-epoch format completion requeues normally");
        inventory(1, 0, 0);
        drain();
    }
}
static void format_capture_gate_case(uint32_t device)
{
    struct crystalhd_rx_dequeue_result result;
    struct crystalhd_dio_req *request;
    unsigned post_before;

    reset(device);
    request = map_private(false);
    check(submit(&context, request) == BC_STS_SUCCESS,
          "prepare format completion for the capture-state gate");
    complete(0);
    ready.packets[0]->flags = COMP_FLAG_FMT_CHANGE;
    context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
    check(crystalhd_rx_dequeue(&context, &result) == BC_STS_SUCCESS &&
          result.capture_epoch == hardware.rx_cancel_epoch,
          "capture-state gate starts with an otherwise current result");
    inventory_with_private(0, 0, 0, result.dio);
    context.state &= ~BC_LINK_CAP_EN;
    post_before = post_calls;
    check(bc_cproc_fmt_change(&context, &result) == BC_STS_IO_USER_ABORT &&
          !result.dio && unmaps[0] == 1 && post_calls == post_before &&
          context.state == BC_LINK_INIT,
          "capture-disabled state rejects an equal-epoch format result");
    inventory(0, 0, 0);
    drain();
}
static void cancellation_cases(uint32_t device)
{
    crystalhd_ioctl_data data = {0};
    reset(device);
    for (unsigned i = 0; i < 3; i++) check(add(i) == BC_STS_SUCCESS, "prepare active/free/ready cancellation");
    complete(0); inventory(1, 1, 1);
    check(crystalhd_hw_stop_capture(&hardware, false) == BC_STS_SUCCESS,
          "discard moves active and ready registrations back to free");
    inventory(0, 0, 3);
    check(!unmaps[0] && !unmaps[1] && !unmaps[2], "discard retains all pinned registrations");
    check(crystalhd_hw_start_capture(&hardware) == BC_STS_SUCCESS, "discarded buffers can restart capture");
    inventory(2, 0, 1);
    data.udata.u.FlushRxCap.bDiscardOnly = 0;
    check(bc_cproc_flush_cap_buffs(&context, &data) == BC_STS_SUCCESS &&
          !(context.state & (BC_LINK_CAP_EN | BC_LINK_FMT_CHG)),
          "command cancellation clears capture/format state and drains ownership");
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
    /* The hardware boundary now represents a completed reset/quiescence. */
    stop_fault = false; hardware.dma_fault = false;
    drain();

    reset(device);
    hardware.rx_actq = hardware.rx_rdyq = hardware.rx_freeq = NULL;
    check(crystalhd_hw_stop_capture(&hardware, true) == BC_STS_SUCCESS && !stop_calls,
          "monitor context without RX queues needs no DMA stop");
    inventory(0, 0, 0);
}
static void flush_argument_and_gate_cases(uint32_t device)
{
    const uint32_t no_capture_states[] = {
        BC_LINK_INVALID, BC_LINK_INIT, BC_LINK_FMT_CHG,
        BC_LINK_SUSPEND, BC_LINK_PAUSED, BC_LINK_RESUME,
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
        stop_fault = false;
        hardware.dma_fault = false;
        drain();
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
        stop_fault = false;
        hardware.dma_fault = false;
        drain();
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
              context.state == BC_LINK_READY && hardware_notifications == 1 &&
              !post_calls && !start_post_observed,
              "faulted hardware start preserves its error after capture-state publication");
        inventory(0, 0, 2);
        hardware.dma_fault = false;
        drain();
    }
}
int main(void)
{
    uint32_t devices[] = { BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA };
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        dequeue_argument_cases(devices[i]);
        dequeue_gate_wait_cases(devices[i]);
        mapped_dequeue_cases(devices[i]);
        pib_dequeue_cases(devices[i]);
        direct_format_dequeue_cases(devices[i]);
        completion_case(devices[i]);
        admission_layout_cases(devices[i]);
        admission_state_cases(devices[i]);
        mapped_admission_cases(devices[i]);
        mapped_completion_cases(devices[i]);
        add_failures(devices[i]);
        retry_cases(devices[i]);
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
        flush_argument_and_gate_cases(devices[i]);
        discard_flush_cases(devices[i]);
        full_flush_cases(devices[i]);
        start_command_cases(devices[i]);
        invalid_command_arguments(devices[i]);
        invalid_capture_start_arguments(devices[i]);
    }
    printf("RX ownership: %u scenarios, %u checks, %u failures\n", groups, checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
