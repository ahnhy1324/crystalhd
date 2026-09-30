/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the production RX ready-queue wait with deterministic kernel
 * primitives. Queue storage and time are boundary stubs.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define KERN_ERR ""
#define printk(...) ((void)0)
#define dev_err(device, ...) ((void)(device))
#define dev_info(device, ...) ((void)(device))
#define BC_LINK_DIOQ_SIG UINT32_C(0x09223280)
#define BC_PCI_DEVID_LINK UINT32_C(0x1612)
#define BC_PCI_DEVID_FLEA UINT32_C(0x1615)
#define COMP_FLAG_FMT_CHANGE UINT32_C(0x01)
#define COMP_FLAG_PIB_VALID UINT32_C(0x02)

struct device { int unused; };
struct pci_dev { struct device dev; uint32_t device; };
struct crystalhd_adp { struct pci_dev *pdev; };
struct crystalhd_rx_dma_pkt { uint32_t pkt_tag, flags; };
struct crystalhd_dioq {
    uint32_t sig, count;
    int lock, event;
    struct crystalhd_rx_dma_pkt *packet;
};
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    struct crystalhd_dioq *rx_rdyq, *rx_freeq;
    int fetch_sem;
    uint32_t PICHeight, PICWidth, LastPicNo, LastTwoPicNo;
};

static struct pci_dev endpoint;
static struct crystalhd_adp adapter = { .pdev = &endpoint };
static struct crystalhd_hw hardware;
static struct crystalhd_dioq ready, freeq;
static struct crystalhd_rx_dma_pkt packet;
static struct device dummy_device;
static unsigned long jiffies;
static unsigned checks, failures, scenarios;
static unsigned wait_calls, down_calls, fetch_calls, add_calls;
static int wait_result;
static bool interrupt_lock, inject_packet_after_wait;

static void check(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", message);
    }
}

static struct device *chddev(void) { return &dummy_device; }

/* One fake tick represents one 250 ms wait slice. Advancing on each deadline
 * check also bounds the intentionally broken stale-result case.
 */
#define msecs_to_jiffies(milliseconds) \
    (((unsigned long)(milliseconds) + 249) / 250)
static bool fake_time_after_eq(unsigned long current, unsigned long deadline)
{
    bool expired = current >= deadline;

    jiffies++;
    return expired;
}
#define time_after_eq(current, deadline) \
    fake_time_after_eq((current), (deadline))

#define spin_lock_irqsave(lock_ptr, flags) do { \
    assert(!*(lock_ptr)); \
    *(lock_ptr) = 1; \
    (flags) = 0; \
} while (0)
#define spin_unlock_irqrestore(lock_ptr, flags) do { \
    assert(*(lock_ptr)); \
    *(lock_ptr) = 0; \
    (void)(flags); \
} while (0)

#define crystalhd_wait_on_event(event_ptr, condition, timeout, result, nosig) do { \
    assert((event_ptr) == &ready.event && !ready.lock); \
    (void)(condition); \
    (void)(timeout); \
    (void)(nosig); \
    wait_calls++; \
    if (inject_packet_after_wait) { \
        assert(!ready.count && !ready.packet); \
        ready.packet = &packet; \
        ready.count = 1; \
        inject_packet_after_wait = false; \
    } \
    (result) = wait_result; \
} while (0)

static int down_interruptible(int *sem)
{
    down_calls++;
    assert(sem == &hardware.fetch_sem && *sem == 1);
    if (interrupt_lock) {
        interrupt_lock = false;
        return -EINTR;
    }
    *sem = 0;
    return 0;
}

static void up(int *sem)
{
    assert(sem == &hardware.fetch_sem && *sem == 0);
    *sem = 1;
}

static void *crystalhd_dioq_fetch(struct crystalhd_dioq *queue)
{
    struct crystalhd_rx_dma_pkt *result;

    assert(queue == &ready && !queue->lock);
    fetch_calls++;
    result = queue->packet;
    if (result) {
        queue->packet = NULL;
        queue->count--;
    }
    return result;
}

static int crystalhd_dioq_add(struct crystalhd_dioq *queue, void *data,
                              bool wake, uint32_t tag)
{
    assert(queue == &freeq && data == &packet && !queue->packet);
    (void)wake;
    (void)tag;
    add_calls++;
    queue->packet = data;
    queue->count++;
    return 0;
}

static uint32_t link_GetRptDropParam(struct crystalhd_hw *hw,
                                     uint32_t height, uint32_t width,
                                     void *data)
{
    assert(hw == &hardware && data == &packet);
    (void)height;
    (void)width;
    return 1;
}

static uint32_t flea_GetRptDropParam(struct crystalhd_hw *hw, void *data)
{
    assert(hw == &hardware && data == &packet);
    return 1;
}

#include "rx-fetch-wait-function.h"

static void reset(uint32_t device)
{
    scenarios++;
    endpoint.device = device;
    ready = (struct crystalhd_dioq){ .sig = BC_LINK_DIOQ_SIG };
    freeq = (struct crystalhd_dioq){ .sig = BC_LINK_DIOQ_SIG };
    packet = (struct crystalhd_rx_dma_pkt){
        .pkt_tag = UINT32_C(0x70029070),
        .flags = COMP_FLAG_FMT_CHANGE,
    };
    hardware = (struct crystalhd_hw){
        .adp = &adapter,
        .rx_rdyq = &ready,
        .rx_freeq = &freeq,
        .fetch_sem = 1,
    };
    jiffies = checks ? 17 : 0;
    wait_calls = down_calls = fetch_calls = add_calls = 0;
    wait_result = -EBUSY;
    interrupt_lock = inject_packet_after_wait = false;
}

static void internal_semaphore_interrupt_case(uint32_t device)
{
    struct crystalhd_rx_dma_pkt *result;
    uint32_t signal = 0;

    reset(device);
    ready.packet = &packet;
    ready.count = 1;
    interrupt_lock = true;
    result = crystalhd_dioq_fetch_wait(&hardware, 1, &signal);
    check(!result && signal == 1,
          "an interrupted fetch semaphore is reported as a signal");
    check(ready.count == 1 && ready.packet == &packet && !fetch_calls &&
          !add_calls, "interrupted admission leaves the ready packet queued");
    check(hardware.fetch_sem == 1 && !ready.lock && down_calls == 1 &&
          !wait_calls, "interrupted admission leaves every lock unowned");

    signal = 0;
    result = crystalhd_dioq_fetch_wait(&hardware, 1, &signal);
    check(result == &packet && !signal && !ready.count && !ready.packet &&
          fetch_calls == 1, "the preserved ready packet remains fetchable once");
    check(hardware.fetch_sem == 1 && !ready.lock && down_calls == 2,
          "successful retry balances the fetch semaphore");
}

static void slice_timeout_packet_race_case(uint32_t device)
{
    struct crystalhd_rx_dma_pkt *result;
    uint32_t signal = 0;

    reset(device);
    inject_packet_after_wait = true;
    result = crystalhd_dioq_fetch_wait(&hardware, 1, &signal);
    check(result == &packet && !signal,
          "a packet arriving after a wait slice is fetched before the deadline");
    check(wait_calls == 1 && down_calls == 1 && fetch_calls == 1 &&
          !add_calls, "slice race performs exactly one wait and one fetch");
    check(!ready.count && !ready.packet && hardware.fetch_sem == 1 &&
          !ready.lock, "slice race returns with queue and semaphore balanced");
}

static void event_interrupt_case(uint32_t device)
{
    uint32_t signal = 0;

    reset(device);
    wait_result = -EINTR;
    check(!crystalhd_dioq_fetch_wait(&hardware, 1, &signal) && signal == 1,
          "an event-wait interrupt is reported as a signal");
    check(wait_calls == 1 && !down_calls && !fetch_calls &&
          hardware.fetch_sem == 1 && !ready.lock,
          "event interruption creates no queue or semaphore owner");
}

static void deadline_timeout_case(uint32_t device)
{
    uint32_t signal = 0;

    reset(device);
    check(!crystalhd_dioq_fetch_wait(&hardware, 1, &signal) && !signal,
          "a real deadline remains distinguishable from interruption");
    check(wait_calls == 4 && !down_calls && !fetch_calls &&
          hardware.fetch_sem == 1 && !ready.lock,
          "deadline polling leaves queue and semaphore balanced");
}

int main(void)
{
    const uint32_t devices[] = { BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA };

    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        internal_semaphore_interrupt_case(devices[i]);
        slice_timeout_packet_race_case(devices[i]);
        event_interrupt_case(devices[i]);
        deadline_timeout_case(devices[i]);
    }
    printf("RX fetch wait: %u scenarios, %u checks, %u failures\n",
           scenarios, checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
