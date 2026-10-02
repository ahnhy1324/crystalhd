/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Run the extracted production Flea RX post and BUSY ownership wrapper.
 * Queueing, locks and publication boundaries are deterministic stubs;
 * no device, DMA or MMIO is accessed and IRQ timing is not simulated.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#include "DriverFwShare.h"
#include "FleaDefs.h"
#include "rx-types.h"

#define KERN_ERR ""
#define printk(...) ((void)0)
#define dev_err(dev, ...) ((void)(dev))
#define dev_dbg(dev, ...) ((void)(dev))
#define READ_ONCE(value) (value)

_Static_assert(sizeof(PIC_DELIVERY_HOST_INFO) == 8 * sizeof(uint32_t),
               "picture-delivery record is eight DWORDs");
_Static_assert(offsetof(PIC_DELIVERY_HOST_INFO, ListIndex) == 0,
               "list index is word zero");
_Static_assert(offsetof(PIC_DELIVERY_HOST_INFO, HostDescMemLowAddr_Y) == 4,
               "Y low address is word one");
_Static_assert(offsetof(PIC_DELIVERY_HOST_INFO, HostDescMemHighAddr_Y) == 8,
               "Y high address is word two");
_Static_assert(offsetof(PIC_DELIVERY_HOST_INFO, HostDescMemLowAddr_UV) == 12,
               "UV low address is word three");
_Static_assert(offsetof(PIC_DELIVERY_HOST_INFO, HostDescMemHighAddr_UV) == 16,
               "UV high address is word four");
_Static_assert(offsetof(PIC_DELIVERY_HOST_INFO, RxSeqNumber) == 20,
               "sequence number is word five");
_Static_assert(offsetof(PIC_DELIVERY_HOST_INFO, ChannelID) == 24,
               "channel ID is word six");
_Static_assert(offsetof(PIC_DELIVERY_HOST_INFO, Reserved) == 28,
               "reserved word is excluded from publication");

struct device { int unused; };
struct pci_dev { struct device dev; };
struct crystalhd_adp { struct pci_dev *pdev; };
struct crystalhd_rx_buffer { int unused; };
struct crystalhd_dioq {
    void *packets[DMA_ENGINE_CNT + 1];
    uint32_t tags[DMA_ENGINE_CNT + 1];
    unsigned count;
};
struct crystalhd_rx_dma_pkt {
    struct crystalhd_rx_buffer *buffer;
    struct { uint64_t phy_addr; } desc_mem;
    uint64_t uv_phy_addr;
    uint32_t pkt_tag;
};
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    int rx_lock, lock;
    enum list_sts rx_list_sts[DMA_ENGINE_CNT];
    uint32_t rx_list_post_index, RxSeqNum, channelNum, PicQSts;
    uint32_t rx_pkt_tag_seed, FleaRxPicDelAddr, RxCaptureState;
    bool dma_fault;
    struct crystalhd_dioq *rx_actq, *rx_freeq;
    BC_STATUS (*pfnDevDRAMWrite)(struct crystalhd_hw *, uint32_t, uint32_t,
                               const uint32_t *);
    void (*pfnWriteDevRegister)(struct crystalhd_adp *, uint32_t, uint32_t);
    void (*pfnNotifyFLLChange)(struct crystalhd_hw *, bool);
};

enum event { DRAM, QUEUE, SYNC, MAILBOX, FREE_QUEUE, NOTIFY };
enum owner { CALLER, ACTIVE_QUEUE, FREE_QUEUE_OWNER };
static struct pci_dev endpoint;
static struct crystalhd_adp adapter = { .pdev = &endpoint };
static struct crystalhd_dioq active, free_queue;
static struct crystalhd_rx_buffer buffer;
static struct crystalhd_hw hardware;
static struct crystalhd_rx_dma_pkt packet, other_packet;
static enum owner packet_owner;
static unsigned checks, groups, failures, queue_calls, free_queue_calls;
static unsigned lock_calls, write_calls, sync_calls, mailbox_calls, notify_calls;
static unsigned event_count;
static enum event events[6];
static BC_STATUS queue_status, free_queue_status, write_status;
static unsigned partial_write_words;
static uint32_t published[8], expected[7];
static uint32_t initial_pic_status, initial_sequence, initial_list, initial_tag;
static uint32_t other_tag;
static uint32_t initial_list_states[DMA_ENGINE_CNT];
static bool irq_enabled, initial_irq_enabled, inject_dma_fault;
static bool inject_list_change;
static uint32_t injected_list;
static unsigned lock_depth;
static int *lock_stack[2];
static unsigned long saved_irq_flags[2];

static void check(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", message);
    }
}

static void record_event(enum event event)
{
    assert(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = event;
}

static void test_lock(int *lock, unsigned long *flags)
{
    assert(!*lock);
    assert(lock_depth < 2);
    assert((lock_depth == 0 && lock == &hardware.rx_lock) ||
           (lock_depth == 1 && lock == &hardware.lock));
    *flags = irq_enabled ? 1UL : 0UL;
    saved_irq_flags[lock_depth] = *flags;
    lock_stack[lock_depth++] = lock;
    irq_enabled = false;
    *lock = 1;
    lock_calls++;
    if (lock == &hardware.rx_lock && inject_list_change) {
        hardware.rx_list_post_index = injected_list;
        initial_list = injected_list;
        expected[0] = injected_list;
        inject_list_change = false;
    }
    if (lock == &hardware.lock && inject_dma_fault) {
        hardware.dma_fault = true;
        inject_dma_fault = false;
    }
}

static void test_unlock(int *lock, unsigned long flags)
{
    assert(*lock == 1);
    assert(lock_depth && lock_stack[lock_depth - 1] == lock);
    assert(flags == saved_irq_flags[lock_depth - 1]);
    assert(!irq_enabled);
    lock_depth--;
    *lock = 0;
    irq_enabled = flags != 0;
    assert(!lock_depth || !irq_enabled);
}

#define spin_lock_irqsave(lock, flags) test_lock((lock), &(flags))
#define spin_unlock_irqrestore(lock, flags) test_unlock((lock), (flags))

static BC_STATUS crystalhd_dioq_add(struct crystalhd_dioq *queue, void *data,
                                  bool irq, uint32_t tag)
{
    BC_STATUS status;

    check(data == &packet && !irq, "queueing retains the existing packet");
    check(packet_owner == CALLER, "queue admission cannot transfer ownership twice");
    check(packet.pkt_tag == initial_tag,
          "the packet tag remains unchanged until active-queue admission succeeds");
    if (queue == &active) {
        queue_calls++;
        record_event(QUEUE);
        check(hardware.rx_lock == 1 && hardware.lock == 1 && !irq_enabled,
              "active-queue admission holds rx_lock then the device lock");
        check(tag == hardware.rx_pkt_tag_seed + initial_list,
              "queue admission uses the tag for the admitted list");
        check(hardware.PicQSts == initial_pic_status &&
              hardware.rx_list_post_index == initial_list &&
              hardware.RxSeqNum == initial_sequence &&
              memcmp(initial_list_states, hardware.rx_list_sts,
                     sizeof(initial_list_states)) == 0,
              "queue admission precedes all list, picture and sequence changes");
        check(event_count == 2 && events[0] == DRAM && write_status == BC_STS_SUCCESS,
              "a successful metadata write precedes ownership admission");
        status = queue_status;
    } else {
        check(queue == &free_queue, "BUSY queues only on the existing free queue");
        free_queue_calls++;
        record_event(FREE_QUEUE);
        check(!hardware.rx_lock && !hardware.lock && !lock_depth &&
              irq_enabled == initial_irq_enabled,
              "the wrapper queues BUSY only after both locks and IRQ flags restore");
        check(tag == initial_tag, "BUSY preserves the caller's original packet tag");
        status = free_queue_status;
    }
    if (status == BC_STS_SUCCESS) {
        assert(queue->count < sizeof(queue->packets) / sizeof(queue->packets[0]));
        queue->packets[queue->count] = data;
        queue->tags[queue->count++] = tag;
        packet_owner = queue == &active ? ACTIVE_QUEUE : FREE_QUEUE_OWNER;
    }
    return status;
}

static BC_STATUS write_dram(struct crystalhd_hw *hw, uint32_t address,
                           uint32_t words, const uint32_t *data)
{
    write_calls++;
    record_event(DRAM);
    check(hw == &hardware && address == hardware.FleaRxPicDelAddr,
          "metadata is published at the existing delivery address");
    check(words == 7, "publication excludes the reserved DWORD");
    check(hw->lock == 1 && hw->rx_lock == 1 && !irq_enabled,
          "metadata publication holds both locks in rx-to-device order");
    check(hw->RxSeqNum == initial_sequence &&
          hw->rx_list_post_index == initial_list &&
          hw->PicQSts == initial_pic_status && packet.pkt_tag == initial_tag &&
          memcmp(initial_list_states, hw->rx_list_sts,
                 sizeof(initial_list_states)) == 0 && packet_owner == CALLER,
          "metadata write precedes all ownership, tag and admission changes");
    if (words == 7) {
        for (unsigned i = 0; i < 7; i++)
            check(data[i] == expected[i],
                  "each attempted DWORD matches the deterministic wire record");
        check(data[7] == 0, "the excluded reserved word remains zero-initialized");
        unsigned copied = write_status == BC_STS_SUCCESS ? 7 : partial_write_words;
        assert(copied <= 7);
        memcpy(published, data, copied * sizeof(uint32_t));
    }
    return write_status;
}

static void crystalhd_rx_buffer_sync_for_device(struct crystalhd_adp *adp,
                                               struct crystalhd_rx_buffer *rx)
{
    sync_calls++;
    record_event(SYNC);
    check(adp == &adapter && rx == &buffer,
          "the existing captured backing is synchronized");
    check(hardware.lock == 1 && hardware.rx_lock == 1 && !irq_enabled,
          "buffer sync remains inside the admission and device locks");
    check(event_count == 3 && events[0] == DRAM && events[1] == QUEUE &&
          packet_owner == ACTIVE_QUEUE &&
          packet.pkt_tag == hardware.rx_pkt_tag_seed + initial_list,
          "successful metadata and ownership admission precede synchronization");
    check(hardware.rx_list_post_index == (initial_list + 1) % DMA_ENGINE_CNT &&
          hardware.RxSeqNum == initial_sequence,
          "the list is armed but the sequence is not yet advanced at sync");
}

static void write_register(struct crystalhd_adp *adp, uint32_t reg,
                           uint32_t value)
{
    mailbox_calls++;
    record_event(MAILBOX);
    check(adp == &adapter && reg == RX_POST_MAILBOX &&
          value == hardware.channelNum,
          "the existing mailbox carries the admitted channel");
    check(hardware.lock == 1 && hardware.rx_lock == 1 && !irq_enabled,
          "mailbox publication remains inside both transaction locks");
    check(event_count == 4 && events[2] == SYNC,
          "buffer synchronization precedes the mailbox notification");
    check(hardware.RxSeqNum == initial_sequence && packet_owner == ACTIVE_QUEUE,
          "the sequence advances only after the owned packet's doorbell");
}

#include "fire-rxdma.h"
#include "rx-post.h"

static void notify_fll(struct crystalhd_hw *hw, bool cleanup)
{
    notify_calls++;
    record_event(NOTIFY);
    check(hw == &hardware && !cleanup, "the wrapper preserves its FLL notification");
    check(!hw->rx_lock && !hw->lock && !lock_depth &&
          irq_enabled == initial_irq_enabled,
          "FLL notification occurs after releasing both transaction locks");
}

static void prepare_attempt(void)
{
    event_count = queue_calls = free_queue_calls = lock_calls = 0;
    write_calls = sync_calls = mailbox_calls = notify_calls = 0;
    memset(events, 0, sizeof(events));
    queue_status = free_queue_status = write_status = BC_STS_SUCCESS;
    partial_write_words = 0;
    inject_dma_fault = inject_list_change = false;
}

static void reset(uint32_t list, uint32_t channel, uint64_t y, uint64_t uv,
                  uint32_t sequence)
{
    groups++;
    hardware = (struct crystalhd_hw){
        .adp = &adapter, .rx_list_post_index = list, .RxSeqNum = sequence,
        .channelNum = channel, .PicQSts = (UINT32_C(1) << channel) | 0x100U,
        .rx_pkt_tag_seed = 0x70029070U, .FleaRxPicDelAddr = 0x00d30400U,
        .RxCaptureState = 1, .rx_actq = &active, .rx_freeq = &free_queue,
        .pfnDevDRAMWrite = write_dram, .pfnWriteDevRegister = write_register,
        .pfnNotifyFLLChange = notify_fll,
    };
    packet = (struct crystalhd_rx_dma_pkt){
        .buffer = &buffer, .desc_mem = { .phy_addr = y },
        .uv_phy_addr = uv, .pkt_tag = 0xdeadbeefU,
    };
    initial_pic_status = hardware.PicQSts;
    initial_sequence = sequence;
    initial_list = list;
    initial_tag = packet.pkt_tag;
    active = (struct crystalhd_dioq){ .packets = { &other_packet },
        .tags = { hardware.rx_pkt_tag_seed + (list == 0 ? 1 : 0) }, .count = 1 };
    other_tag = active.tags[0];
    free_queue = (struct crystalhd_dioq){0};
    packet_owner = CALLER;
    if (list < DMA_ENGINE_CNT)
        hardware.rx_list_sts[1 - list] = rx_waiting_y_intr;
    memcpy(initial_list_states, hardware.rx_list_sts, sizeof(initial_list_states));
    for (unsigned i = 0; i < 8; i++)
        published[i] = 0xcafef00dU;
    expected[0] = list;
    expected[1] = (uint32_t)y;
    expected[2] = (uint32_t)(y >> 32);
    expected[3] = (uint32_t)uv;
    expected[4] = (uint32_t)(uv >> 32);
    expected[5] = sequence;
    expected[6] = channel;
    irq_enabled = initial_irq_enabled = true;
    lock_depth = 0;
    memset(lock_stack, 0, sizeof(lock_stack));
    memset(saved_irq_flags, 0, sizeof(saved_irq_flags));
    prepare_attempt();
}

static void check_uncommitted_state(void)
{
    check(!hardware.rx_lock && !hardware.lock && !lock_depth,
          "all acquired locks are released in reverse order");
    check(irq_enabled == initial_irq_enabled,
          "the original enabled or disabled IRQ state is restored");
    check(hardware.RxSeqNum == initial_sequence &&
          hardware.rx_list_post_index == initial_list,
          "rejection leaves sequence and list index unchanged");
    check(memcmp(initial_list_states, hardware.rx_list_sts,
                 sizeof(initial_list_states)) == 0,
          "rejection leaves both list states unchanged");
    check(hardware.PicQSts == initial_pic_status,
          "rejection leaves picture availability unchanged");
    check(packet.pkt_tag == initial_tag, "rejection leaves the packet tag unchanged");
    check(active.count == 1 && active.packets[0] == &other_packet &&
          active.tags[0] == other_tag,
          "rejection preserves the other active packet and its queue tag");
    check(!sync_calls && !mailbox_calls,
          "uncommitted metadata is neither synchronized nor posted to firmware");
}

static void check_no_metadata_write(void)
{
    for (unsigned i = 0; i < 8; i++)
        check(published[i] == 0xcafef00dU, "rejection does not write a metadata DWORD");
}

static void rejected(BC_STATUS expected_status, struct crystalhd_hw *hw,
                     struct crystalhd_rx_dma_pkt *rx, unsigned expected_locks)
{
    check(crystalhd_flea_hw_fire_rxdma(hw, rx) == expected_status,
          "precondition rejects with the existing status");
    check(!event_count && !queue_calls,
          "precondition rejection does not queue, sync or publish");
    check(lock_calls == expected_locks,
          "rejection retains the existing lock acquisition boundary");
    check_uncommitted_state();
    check(packet_owner == CALLER && !free_queue.count,
          "direct precondition rejection leaves ownership with the caller");
    check_no_metadata_write();
}

static void test_preconditions(void)
{
    reset(0, 3, UINT64_C(0x1234567887654321), 0, 0x01020304);
    rejected(BC_STS_INV_ARG, NULL, &packet, 0);
    reset(0, 3, 0, 0, 0);
    rejected(BC_STS_INV_ARG, &hardware, NULL, 0);
    reset(0, 3, 0, 0, 0);
    packet.buffer = NULL;
    rejected(BC_STS_INV_ARG, &hardware, &packet, 0);
    reset(DMA_ENGINE_CNT, 3, 0, 0, 0);
    rejected(BC_STS_INV_ARG, &hardware, &packet, 0);
    reset(0, 3, 0, 0, 0);
    hardware.dma_fault = true;
    rejected(BC_STS_IO_ERROR, &hardware, &packet, 0);
    reset(0, 3, 0, 0, 0);
    hardware.RxCaptureState = 0;
    rejected(BC_STS_BUSY, &hardware, &packet, 0);
    reset(0, 3, 0, 0, 0);
    hardware.RxCaptureState = 2;
    rejected(BC_STS_BUSY, &hardware, &packet, 0);
    reset(1, 3, 0, 0, 0);
    hardware.rx_list_sts[1] = rx_waiting_y_intr;
    initial_list_states[1] = rx_waiting_y_intr;
    rejected(BC_STS_BUSY, &hardware, &packet, 1);
    reset(0, 3, 0, 0, 0);
    hardware.PicQSts = initial_pic_status = 0x100U;
    rejected(BC_STS_BUSY, &hardware, &packet, 1);
    reset(0, 3, 0, 0, 0);
    inject_list_change = true;
    injected_list = DMA_ENGINE_CNT;
    rejected(BC_STS_INV_ARG, &hardware, &packet, 1);
    reset(0, 3, 0, 0, 0);
    inject_list_change = true;
    injected_list = 1;
    rejected(BC_STS_BUSY, &hardware, &packet, 1);
    reset(0, 3, 0, 0, 0);
    inject_dma_fault = true;
    rejected(BC_STS_IO_ERROR, &hardware, &packet, 2);
    reset(0, 3, 0, 0, 0);
    inject_dma_fault = true;
    irq_enabled = initial_irq_enabled = false;
    rejected(BC_STS_IO_ERROR, &hardware, &packet, 2);
}

static void test_queue_failure(void)
{
    const BC_STATUS errors[] = {BC_STS_BUSY, BC_STS_IO_ERROR, BC_STS_INSUFF_RES};

    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        reset(i % DMA_ENGINE_CNT, 3, UINT64_C(0xfedcba9876543210),
              UINT64_C(0x8765432112345678), 0xdead1234U);
        queue_status = errors[i];
        check(crystalhd_flea_hw_fire_rxdma(&hardware, &packet) == errors[i],
              "queue rejection preserves the queue's status");
        check(event_count == 2 && events[0] == DRAM && events[1] == QUEUE &&
              queue_calls == 1 && write_calls == 1 && lock_calls == 2,
              "queue failure after a successful write never syncs or rings the doorbell");
        check_uncommitted_state();
        check(packet_owner == CALLER && !free_queue.count,
              "failed active-queue admission leaves ownership with the caller");
        for (unsigned word = 0; word < 7; word++)
            check(published[word] == expected[word],
                  "successful metadata write does not itself publish the packet");
        check(published[7] == 0xcafef00dU,
              "queue failure still excludes the reserved word from the write");
    }
}

static void check_success(void)
{
    check(event_count >= 4 && events[0] == DRAM && events[1] == QUEUE &&
          events[2] == SYNC && events[3] == MAILBOX,
          "successful submission orders DRAM/queue/sync/mailbox within the transaction");
    check(queue_calls == 1 && write_calls == 1 && sync_calls == 1 &&
          mailbox_calls == 1 && lock_calls == 2 && !lock_depth &&
          !hardware.rx_lock && !hardware.lock && irq_enabled == initial_irq_enabled,
          "success restores both locks and the caller's original IRQ state");
    check(hardware.rx_list_sts[initial_list] ==
          (rx_waiting_y_intr | (packet.uv_phy_addr ? rx_waiting_uv_intr : 0)),
          "only present UV adds its completion wait bit");
    check(hardware.rx_list_sts[1 - initial_list] == initial_list_states[1 - initial_list],
          "posting does not change the other list's status");
    check(hardware.rx_list_post_index == (initial_list + 1) % DMA_ENGINE_CNT,
          "list selection retains ping-pong behavior");
    check(hardware.RxSeqNum == initial_sequence + UINT32_C(1),
          "the sequence advances once after publication");
    check(hardware.PicQSts ==
          (initial_pic_status & ~(UINT32_C(1) << hardware.channelNum)),
          "only the posted channel's availability is consumed");
    check(packet_owner == ACTIVE_QUEUE && active.count == 2 && !free_queue.count &&
          active.packets[0] == &other_packet &&
          active.tags[0] == other_tag &&
          active.packets[1] == &packet &&
          active.tags[1] == hardware.rx_pkt_tag_seed + initial_list &&
          packet.pkt_tag == active.tags[1],
          "only successful queue admission commits the tag and active ownership");
    check(published[7] == 0xcafef00dU,
          "seven-DWORD transfer does not publish the reserved word");
}

static void test_success(uint32_t list, uint32_t channel, uint64_t y,
                         uint64_t uv, uint32_t sequence, bool enabled)
{
    reset(list, channel, y, uv, sequence);
    hardware.rx_list_sts[1 - list] = initial_list_states[1 - list] = rx_uv_error;
    irq_enabled = initial_irq_enabled = enabled;
    check(crystalhd_flea_hw_fire_rxdma(&hardware, &packet) == BC_STS_SUCCESS,
          "valid metadata submission succeeds");
    check_success();
    check(event_count == 4 && !notify_calls && !free_queue_calls,
          "direct submission does not call wrapper notification or free queue");
}

static void test_write_failure(void)
{
    const BC_STATUS errors[] = {BC_STS_BUSY, BC_STS_ERROR, BC_STS_IO_ERROR,
                               BC_STS_INV_ARG, BC_STS_INSUFF_RES};

    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        for (unsigned partial = 0; partial < 2; partial++) {
            reset(i % DMA_ENGINE_CNT, 3, UINT64_C(0xfedcba9876543210),
                  partial ? UINT64_C(0x8765432112345678) : 0, UINT32_MAX);
            write_status = errors[i];
            partial_write_words = partial ? 3 : 0;
            irq_enabled = initial_irq_enabled = partial == 0;
            check(crystalhd_flea_hw_fire_rxdma(&hardware, &packet) == errors[i],
                  "failed DRAM write returns its exact transport status");
            check(event_count == 1 && events[0] == DRAM && write_calls == 1 &&
                  !queue_calls && lock_calls == 2,
                  "write failure cannot reach ownership admission or any later publication");
            check_uncommitted_state();
            check(packet_owner == CALLER && !free_queue.count,
                  "write failure leaves the direct caller owning the packet");
            for (unsigned word = 0; word < 8; word++)
                check(published[word] == (word < partial_write_words ?
                      expected[word] : 0xcafef00dU),
                      "even a partially changed record is never posted after write failure");
            prepare_attempt();
            check(crystalhd_flea_hw_fire_rxdma(&hardware, &packet) == BC_STS_SUCCESS,
                  "a failed write can retry with the same caller-owned packet");
            check_success();
        }
    }
}

static void test_queue_retry(void)
{
    reset(1, 2, UINT64_C(0x123456789abcdef0),
          UINT64_C(0x87654321fedcba98), UINT32_MAX);
    queue_status = BC_STS_INSUFF_RES;
    check(crystalhd_flea_hw_fire_rxdma(&hardware, &packet) == BC_STS_INSUFF_RES,
          "queue failure after metadata write is returned");
    check_uncommitted_state();
    check(packet_owner == CALLER && !free_queue.count,
          "a retry starts with exclusive caller ownership after queue failure");
    prepare_attempt();
    check(crystalhd_flea_hw_fire_rxdma(&hardware, &packet) == BC_STS_SUCCESS,
          "queue admission can retry without a sequence gap or old tag mutation");
    check_success();
}

static void test_wrapper(void)
{
    const BC_STATUS errors[] = {BC_STS_BUSY, BC_STS_ERROR, BC_STS_IO_ERROR,
                               BC_STS_INV_ARG, BC_STS_INSUFF_RES};

    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        reset(i % DMA_ENGINE_CNT, 3, UINT64_C(0x123456789abcdef0), 0, 123);
        write_status = errors[i];
        check(crystalhd_flea_hw_post_cap_buff(&hardware, &packet) == errors[i],
              "the real wrapper returns a failed write's status");
        check_uncommitted_state();
        check(notify_calls == 1 && events[event_count - 1] == NOTIFY,
              "the wrapper still notifies free-list length outside the transaction");
        if (errors[i] == BC_STS_BUSY) {
            check(packet_owner == FREE_QUEUE_OWNER && free_queue_calls == 1 &&
                  free_queue.count == 1 && free_queue.packets[0] == &packet &&
                  free_queue.tags[0] == initial_tag && event_count == 3 &&
                  events[0] == DRAM && events[1] == FREE_QUEUE,
                  "BUSY transfers the unchanged packet exactly once to the free queue");
        } else {
            check(packet_owner == CALLER && !free_queue_calls && !free_queue.count &&
                  event_count == 2 && events[0] == DRAM,
                  "non-BUSY transport errors leave the caller owning the packet");
        }
        check_no_metadata_write();
    }

    reset(0, 2, UINT64_C(0x123456789abcdef0), 0, UINT32_MAX);
    write_status = BC_STS_BUSY;
    free_queue_status = BC_STS_INSUFF_RES;
    check(crystalhd_flea_hw_post_cap_buff(&hardware, &packet) == BC_STS_INSUFF_RES,
          "a BUSY free-queue failure returns its hard error");
    check_uncommitted_state();
    check(packet_owner == CALLER && !free_queue.count && free_queue_calls == 1 &&
          !queue_calls && !notify_calls && event_count == 2 &&
          events[0] == DRAM && events[1] == FREE_QUEUE,
          "failed BUSY admission does not transfer ownership or notify a false free count");

    reset(1, 2, UINT64_C(0x123456789abcdef0), 0, UINT32_MAX);
    queue_status = BC_STS_BUSY;
    free_queue_status = BC_STS_INSUFF_RES;
    check(crystalhd_flea_hw_post_cap_buff(&hardware, &packet) == BC_STS_INSUFF_RES,
          "BUSY from active admission preserves a free-queue hard error");
    check_uncommitted_state();
    check(packet_owner == CALLER && !free_queue.count && free_queue_calls == 1 &&
          queue_calls == 1 && !notify_calls && event_count == 3 &&
          events[0] == DRAM && events[1] == QUEUE && events[2] == FREE_QUEUE,
          "failed fallback after a write does not publish or transfer the packet");

    for (unsigned busy = 0; busy < 2; busy++) {
        reset(1, 2, UINT64_C(0x123456789abcdef0),
              UINT64_C(0x87654321fedcba98), UINT32_MAX);
        queue_status = busy ? BC_STS_BUSY : BC_STS_INSUFF_RES;
        check(crystalhd_flea_hw_post_cap_buff(&hardware, &packet) == queue_status,
              "the wrapper returns active-queue failure after successful metadata write");
        check_uncommitted_state();
        check(packet_owner == (busy ? FREE_QUEUE_OWNER : CALLER) &&
              free_queue_calls == busy && free_queue.count == busy &&
              notify_calls == 1,
              "the wrapper preserves BUSY versus hard-error ownership after queue failure");
    }

    reset(0, 2, UINT64_C(0x123456789abcdef0),
          UINT64_C(0x87654321fedcba98), UINT32_MAX);
    irq_enabled = initial_irq_enabled = false;
    check(crystalhd_flea_hw_post_cap_buff(&hardware, &packet) == BC_STS_SUCCESS,
          "the real wrapper preserves a successful transaction");
    check_success();
    check(event_count == 5 && events[4] == NOTIFY && notify_calls == 1 &&
          !free_queue_calls,
          "successful wrapper notification is outside the transaction with IRQs still disabled");
}

int main(void)
{
    test_preconditions();
    test_write_failure();
    test_queue_failure();
    test_queue_retry();
    test_wrapper();
    for (uint32_t list = 0; list < DMA_ENGINE_CNT; list++) {
        for (uint32_t channel = 0; channel < 4; channel++) {
            test_success(list, channel, UINT64_C(0xfedcba9876543210), 0, 17, true);
            test_success(list, channel, UINT64_C(0x123456789abcdef0),
                         UINT64_C(0x87654321fedcba98), 0xdead1234U, false);
            test_success(list, channel, UINT64_C(0x00000001ffffffff),
                         UINT64_C(0x0000000100000000), UINT32_MAX, true);
        }
    }
    printf("Flea RX metadata: %u checks in %u scenarios, %u failures\n",
           checks, groups, failures);
    return failures ? 1 : 0;
}
