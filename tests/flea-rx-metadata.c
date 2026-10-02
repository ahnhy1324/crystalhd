/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Run the extracted production Flea RX post function. Queueing, locks and
 * publication boundaries are stubs; no device, DMA or MMIO is accessed.
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
struct crystalhd_dioq { int unused; };
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
    struct crystalhd_dioq *rx_actq;
    BC_STATUS (*pfnDevDRAMWrite)(struct crystalhd_hw *, uint32_t, uint32_t,
                               const uint32_t *);
    void (*pfnWriteDevRegister)(struct crystalhd_adp *, uint32_t, uint32_t);
};

enum event { QUEUE, DRAM, SYNC, MAILBOX };
static struct pci_dev endpoint;
static struct crystalhd_adp adapter = { .pdev = &endpoint };
static struct crystalhd_dioq active;
static struct crystalhd_rx_buffer buffer;
static struct crystalhd_hw hardware;
static struct crystalhd_rx_dma_pkt packet;
static unsigned checks, groups, failures, queue_calls, lock_calls;
static unsigned event_count;
static enum event events[4];
static BC_STATUS queue_status;
static uint32_t published[8], expected[7];
static uint32_t initial_pic_status, initial_sequence, initial_list;
static uint32_t initial_list_states[DMA_ENGINE_CNT];

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
    *flags = 0;
    *lock = 1;
    lock_calls++;
}

static void test_unlock(int *lock, unsigned long flags)
{
    (void)flags;
    assert(*lock == 1);
    *lock = 0;
}

#define spin_lock_irqsave(lock, flags) test_lock((lock), &(flags))
#define spin_unlock_irqrestore(lock, flags) test_unlock((lock), (flags))

static BC_STATUS crystalhd_dioq_add(struct crystalhd_dioq *queue, void *data,
                                  bool irq, uint32_t tag)
{
    queue_calls++;
    record_event(QUEUE);
    check(queue == &active && data == &packet && !irq,
          "the existing packet is queued on the active queue");
    check(hardware.rx_lock == 1 && !hardware.lock,
          "ownership is queued while holding only rx_lock");
    check(tag == hardware.rx_pkt_tag_seed + initial_list &&
          packet.pkt_tag == tag,
          "the tag retains the original list index");
    check(hardware.PicQSts ==
          (initial_pic_status & ~(UINT32_C(1) << hardware.channelNum)),
          "queueing consumes only this channel's picture bit");
    return queue_status;
}

static BC_STATUS write_dram(struct crystalhd_hw *hw, uint32_t address,
                           uint32_t words, const uint32_t *data)
{
    record_event(DRAM);
    check(hw == &hardware && address == hardware.FleaRxPicDelAddr,
          "metadata is published at the existing delivery address");
    check(words == 7, "publication excludes the reserved DWORD");
    check(hw->lock == 1 && !hw->rx_lock,
          "metadata publication holds only the device lock");
    check(hw->RxSeqNum == initial_sequence,
          "the sequence is not advanced before publication");
    check(hw->rx_list_post_index == (initial_list + 1) % DMA_ENGINE_CNT,
          "the next list is selected before publication");
    if (words == 7) {
        memcpy(published, data, 7 * sizeof(uint32_t));
        for (unsigned i = 0; i < 7; i++)
            check(published[i] == expected[i],
                  "each published DWORD matches the deterministic wire record");
    }
    return BC_STS_SUCCESS;
}

static void crystalhd_rx_buffer_sync_for_device(struct crystalhd_adp *adp,
                                               struct crystalhd_rx_buffer *rx)
{
    record_event(SYNC);
    check(adp == &adapter && rx == &buffer,
          "the existing captured backing is synchronized");
    check(hardware.lock == 1 && !hardware.rx_lock,
          "buffer sync remains inside the device lock");
    check(event_count == 3 && events[1] == DRAM,
          "metadata is published before buffer synchronization");
}

static void write_register(struct crystalhd_adp *adp, uint32_t reg,
                           uint32_t value)
{
    record_event(MAILBOX);
    check(adp == &adapter && reg == RX_POST_MAILBOX &&
          value == hardware.channelNum,
          "the existing mailbox carries the same nonzero channel");
    check(hardware.lock == 1 && !hardware.rx_lock,
          "mailbox publication remains inside the device lock");
    check(event_count == 4 && events[2] == SYNC,
          "buffer synchronization precedes the mailbox notification");
}

#include "fire-rxdma.h"

static void reset(uint32_t list, uint32_t channel, uint64_t y, uint64_t uv,
                  uint32_t sequence)
{
    groups++;
    hardware = (struct crystalhd_hw){
        .adp = &adapter, .rx_list_post_index = list, .RxSeqNum = sequence,
        .channelNum = channel, .PicQSts = (UINT32_C(1) << channel) | 0x100U,
        .rx_pkt_tag_seed = 0x70029070U, .FleaRxPicDelAddr = 0x00d30400U,
        .RxCaptureState = 1, .rx_actq = &active,
        .pfnDevDRAMWrite = write_dram, .pfnWriteDevRegister = write_register,
    };
    packet = (struct crystalhd_rx_dma_pkt){
        .buffer = &buffer, .desc_mem = { .phy_addr = y },
        .uv_phy_addr = uv, .pkt_tag = 0xdeadbeefU,
    };
    initial_pic_status = hardware.PicQSts;
    initial_sequence = sequence;
    initial_list = list;
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
    event_count = queue_calls = lock_calls = 0;
    memset(events, 0, sizeof(events));
    queue_status = BC_STS_SUCCESS;
}

static void check_idle_state(void)
{
    check(!hardware.rx_lock && !hardware.lock, "all acquired locks are released");
    check(hardware.RxSeqNum == initial_sequence &&
          hardware.rx_list_post_index == initial_list,
          "rejection leaves sequence and list index unchanged");
    check(memcmp(initial_list_states, hardware.rx_list_sts,
                 sizeof(initial_list_states)) == 0,
          "rejection leaves both list states unchanged");
    check(hardware.PicQSts == initial_pic_status,
          "rejection leaves or restores picture availability");
    for (unsigned i = 0; i < 8; i++)
        check(published[i] == 0xcafef00dU, "rejection never publishes a DWORD");
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
    check_idle_state();
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
        check(event_count == 1 && events[0] == QUEUE && queue_calls == 1,
              "queue failure never publishes, syncs or notifies");
        check_idle_state();
    }
}

static void test_success(uint32_t list, uint32_t channel, uint64_t y,
                         uint64_t uv, uint32_t sequence)
{
    reset(list, channel, y, uv, sequence);
    hardware.rx_list_sts[1 - list] = rx_uv_error;
    initial_list_states[1 - list] = rx_uv_error;
    check(crystalhd_flea_hw_fire_rxdma(&hardware, &packet) == BC_STS_SUCCESS,
          "valid metadata submission succeeds");
    check(event_count == 4 && events[0] == QUEUE && events[1] == DRAM &&
          events[2] == SYNC && events[3] == MAILBOX,
          "successful submission preserves queue/DRAM/sync/mailbox ordering");
    check(queue_calls == 1 && lock_calls == 2 &&
          !hardware.rx_lock && !hardware.lock,
          "success uses and releases exactly the existing two locks");
    check(hardware.rx_list_sts[list] ==
          (rx_waiting_y_intr | (uv ? rx_waiting_uv_intr : 0)),
          "only present UV adds its completion wait bit");
    check(hardware.rx_list_sts[1 - list] == rx_uv_error,
          "posting does not change the other list's status");
    check(hardware.rx_list_post_index == (list + 1) % DMA_ENGINE_CNT,
          "list selection retains ping-pong behavior");
    check(hardware.RxSeqNum == sequence + UINT32_C(1),
          "the sequence advances once after publication");
    check(hardware.PicQSts ==
          (initial_pic_status & ~(UINT32_C(1) << channel)),
          "only the posted channel's availability is consumed");
    check(published[7] == 0xcafef00dU,
          "seven-DWORD transfer does not publish the reserved word");
}

int main(void)
{
    test_preconditions();
    test_queue_failure();
    for (uint32_t list = 0; list < DMA_ENGINE_CNT; list++) {
        for (uint32_t channel = 0; channel < 4; channel++) {
            test_success(list, channel, UINT64_C(0xfedcba9876543210), 0, 17);
            test_success(list, channel, UINT64_C(0x123456789abcdef0),
                         UINT64_C(0x87654321fedcba98), 0xdead1234U);
            test_success(list, channel, UINT64_C(0x00000001ffffffff),
                         UINT64_C(0x0000000100000000), UINT32_MAX);
        }
    }
    printf("Flea RX metadata: %u checks in %u scenarios, %u failures\n",
           checks, groups, failures);
    return failures ? 1 : 0;
}
