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

#define KERN_ERR ""
#define printk(...) ((void)0)
#define dev_err(dev, ...) ((void)(dev))
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
    int lock, fetch_sem;
    bool hw_pause_issued, dma_fault;
    uint32_t rx_pkt_tag_seed, DrvTotalFrmCaptured;
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
static unsigned sem_attempts, hardware_notifications;
static unsigned fail_post_call;
static BC_STATUS map_status, translate_status, post_status, queue_status;
static bool interrupt_lock, wait_signal, stop_fault, notify_ok;
static struct crystalhd_dioq *fail_queue;

static void check(bool condition, const char *message)
{
    checks++;
    if (!condition) { failures++; fprintf(stderr, "FAIL: %s\n", message); }
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
static void inventory(unsigned active_count, unsigned ready_count, unsigned free_count)
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
        check(queued_request(&requests[i]) == (unsigned)mapped[i],
              "each live mapping has exactly one queued owner");
        check(unmaps[i] <= 1, "a registration is unmapped at most once");
    }
    check(active.count == active_count && ready.count == ready_count &&
          available.count == free_count, "active/ready/free ownership matches transition");
    check(!irq_depth && !hardware.lock && hardware.fetch_sem == 1,
          "transition releases IRQ, pool lock and capture semaphore");
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
static void *crystalhd_dioq_fetch_wait(struct crystalhd_hw *hw, uint32_t timeout,
                                     uint32_t *signal)
{
    assert(hw == &hardware && timeout == BC_PROC_OUTPUT_TIMEOUT / 1000);
    assert(hardware.fetch_sem == 1);
    *signal = wait_signal;
    return wait_signal ? NULL : crystalhd_dioq_fetch(hw->rx_rdyq);
}
static BC_STATUS crystalhd_map_dio(struct crystalhd_adp *adp, void *buffer,
        uint32_t size, uint32_t uv, bool mode422, bool tx, struct crystalhd_dio_req **result)
{
    size_t i;
    assert(adp == &adapter && !tx);
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
    if (interrupt_lock) { interrupt_lock = false; return -1; }
    *sem = 0;
    return 0;
}
static void down(int *sem) { assert(sem == &hardware.fetch_sem && *sem == 1); *sem = 0; }
static void up(int *sem) { assert(sem == &hardware.fetch_sem && !*sem); *sem = 1; }
static void disable_irq(int irq)
{
    assert(irq == endpoint.irq && !irq_depth && !hardware.fetch_sem);
    irq_depth++; irq_disables++;
}
static void enable_irq(int irq)
{
    assert(irq == endpoint.irq && irq_depth == 1);
    irq_depth--; irq_enables++;
}
static struct device *chddev(void) { return &endpoint.dev; }
static uint64_t rdtsc_ordered(void) { return 1000; }
static BC_STATUS crystalhd_xlat_sgl_to_dma_desc(struct crystalhd_dio_req *request,
        struct dma_desc_mem *memory, uint32_t *uv, struct device *dev, uint32_t destination)
{
    assert(mapped[request_index(request)] && memory && dev == &endpoint.dev && !destination);
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
    hardware_notifications++;
    return notify_ok;
}
static void stop_dma(struct crystalhd_hw *hw)
{
    assert(hw == &hardware && irq_depth == 1 && !hw->fetch_sem);
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

static void reset(uint32_t device)
{
    groups++;
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
    sem_attempts = hardware_notifications = 0;
    map_status = translate_status = post_status = queue_status = BC_STS_SUCCESS;
    interrupt_lock = wait_signal = stop_fault = false;
    notify_ok = true; fail_queue = NULL;
    inventory(0, 0, 0);
}
static BC_STATUS add(unsigned index)
{
    crystalhd_ioctl_data data = {0};
    assert(index < BC_RX_LIST_CNT);
    data.udata.u.RxBuffs.YuvBuff = (uint8_t *)buffers[index];
    data.udata.u.RxBuffs.YuvBuffSz = sizeof(buffers[index]);
    data.udata.u.RxBuffs.UVbuffOffset = 128;
    return bc_cproc_add_cap_buff(&context, &data);
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
    reset(device);
    check(add(0) == BC_STS_SUCCESS, "submit format-change registration");
    complete(0);
    ready.packets[0]->flags = COMP_FLAG_FMT_CHANGE | COMP_FLAG_PIB_VALID;
    ready.packets[0]->pib.width = 1280;
    ready.packets[0]->pib.height = 720;
    context.state = BC_LINK_INIT | BC_LINK_CAP_EN;
    if (failure == 1) interrupt_lock = true;
    if (failure == 2) translate_status = BC_STS_IO_ERROR;
    if (failure == 3) post_status = BC_STS_IO_ERROR;
    BC_STATUS status = bc_cproc_fetch_frame(&context, &data);
    check(status == (failure == 1 ? BC_STS_IO_USER_ABORT :
          failure ? BC_STS_IO_ERROR : BC_STS_SUCCESS), "format-change status matches the failing boundary");
    check(data.udata.u.DecOutData.Flags == (COMP_FLAG_FMT_CHANGE | COMP_FLAG_PIB_VALID) &&
          data.udata.u.DecOutData.PibInfo.ppb.width == 1280,
          "format-change notification preserves format metadata");
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
static void invalid_command_arguments(uint32_t device)
{
    BC_STATUS (*commands[])(struct crystalhd_cmd *, crystalhd_ioctl_data *) = {
        bc_cproc_start_capture, bc_cproc_flush_cap_buffs, bc_cproc_add_cap_buff };
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
static void start_command_cases(uint32_t device)
{
    crystalhd_ioctl_data data = {0};
    const BC_STATUS statuses[] = { BC_STS_SUCCESS, BC_STS_BUSY, BC_STS_IO_ERROR };
    for (unsigned variant = 0; variant < 4; variant++) {
        reset(device);
        context.state = BC_LINK_INIT;
        hardware.DrvTotalFrmCaptured = 23;
        data.udata.u.RxCap.PauseThsh = variant & 1 ? 17 : 0;
        data.udata.u.RxCap.ResumeThsh = variant & 2 ? 9 : 0;
        check(bc_cproc_start_capture(&context, &data) == BC_STS_SUCCESS,
              "start capture accepts independent default or explicit thresholds");
        check(hardware.PauseThreshold == (variant & 1 ? 17U : HW_PAUSE_THRESHOLD) &&
              hardware.ResumeThreshold == (variant & 2 ? 9U : HW_RESUME_THRESHOLD) &&
              hardware.DefaultPauseThreshold == hardware.PauseThreshold &&
              !hardware.DrvTotalFrmCaptured,
              "start stores thresholds, records the pause default and resets captured count");
        check(context.state == (BC_LINK_INIT | BC_LINK_CAP_EN) &&
              hardware_notifications == 1 && sem_attempts == 1 && !post_calls,
              "start enables capture but waits for format readiness before posting DMA");
        inventory(0, 0, 0);
    }
    reset(device);
    context.state = BC_LINK_INIT | BC_LINK_FMT_CHG;
    memset(&data, 0, sizeof(data));
    check(bc_cproc_start_capture(&context, &data) == BC_STS_SUCCESS &&
          context.state == BC_LINK_READY && hardware_notifications == 1 && !post_calls,
          "empty ready capture normalizes NO_DATA to success");
    inventory(0, 0, 0);

    reset(device);
    context.state = BC_LINK_INIT;
    notify_ok = false;
    check(bc_cproc_start_capture(&context, &data) == BC_STS_IO_ERROR &&
          context.state == BC_LINK_INIT && hardware_notifications == 1 && !post_calls,
          "failed start notification does not publish capture state or post DMA");
    inventory(0, 0, 0);

    reset(device);
    hardware.DrvTotalFrmCaptured = 23;
    hardware.DefaultPauseThreshold = 31;
    interrupt_lock = true;
    check(bc_cproc_start_capture(&context, &data) == BC_STS_IO_USER_ABORT &&
          hardware.PauseThreshold == 12 && hardware.ResumeThreshold == 4 &&
          hardware.DefaultPauseThreshold == 31 && hardware.DrvTotalFrmCaptured == 23 &&
          context.state == BC_LINK_READY && !hardware_notifications && !post_calls,
          "interrupted start leaves thresholds, counters and capture state unchanged");
    inventory(0, 0, 0);

    for (unsigned s = 0; s < sizeof(statuses) / sizeof(statuses[0]); s++) {
        reset(device);
        context.state = BC_LINK_INIT;
        check(add(0) == BC_STS_SUCCESS && add(1) == BC_STS_SUCCESS,
              "prepare queued buffers for actual start command");
        context.state |= BC_LINK_FMT_CHG;
        post_status = statuses[s];
        check(bc_cproc_start_capture(&context, &data) ==
              (statuses[s] == BC_STS_IO_ERROR ? BC_STS_IO_ERROR : BC_STS_SUCCESS),
              "start command preserves hard errors and queues BUSY buffers for retry");
        check(context.state == BC_LINK_READY && hardware_notifications == 1 &&
              post_calls == (statuses[s] == BC_STS_SUCCESS ? 2U : 1U),
              "ready start posts only the admitted DMA engines");
        inventory(statuses[s] == BC_STS_SUCCESS ? 2 : 0, 0,
                  statuses[s] == BC_STS_SUCCESS ? 0 : 2);
        drain();
    }
}
int main(void)
{
    uint32_t devices[] = { BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA };
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        completion_case(devices[i]);
        add_failures(devices[i]);
        retry_cases(devices[i]);
        for (unsigned failure = 0; failure < 4; failure++) format_case(devices[i], failure);
        cancellation_cases(devices[i]);
        start_command_cases(devices[i]);
        invalid_command_arguments(devices[i]);
    }
    printf("RX ownership: %u scenarios, %u checks, %u failures\n", groups, checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
