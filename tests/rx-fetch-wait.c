/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise production waiting and bounded ready-queue fetch with deterministic
 * kernel primitives. Queue storage, hardware parsers and time are boundaries.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KERN_ERR ""
#define printk(...) ((void)0)
#define dev_err(device, ...) ((void)(device))
#define dev_info(device, ...) ((void)(device))
#define BC_LINK_DIOQ_SIG UINT32_C(0x09223280)
#define BC_PCI_DEVID_LINK UINT32_C(0x1612)
#define BC_PCI_DEVID_FLEA UINT32_C(0x1615)
#define COMP_FLAG_FMT_CHANGE UINT32_C(0x01)
#define COMP_FLAG_PIB_VALID UINT32_C(0x02)
#define BC_STS_SUCCESS 0
#define min_t(type, left, right) \
    ((type)(left) < (type)(right) ? (type)(left) : (type)(right))

struct device { int unused; };
struct pci_dev { struct device dev; uint32_t device; };
struct crystalhd_adp { struct pci_dev *pdev; };
struct crystalhd_rx_dma_pkt {
    uint32_t pkt_tag, flags;
    struct crystalhd_rx_dma_pkt *next;
    uint32_t picture, parsed_flags;
    bool custom_picture;
};
struct crystalhd_dioq {
    uint32_t sig, count;
    int lock, event;
    struct crystalhd_rx_dma_pkt *packet;
};
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    struct crystalhd_dioq *rx_rdyq, *rx_freeq;
    struct crystalhd_rx_dma_pkt *rx_fallback_head;
    int fetch_sem;
    uint32_t PICHeight, PICWidth, LastPicNo, LastTwoPicNo;
};

static struct pci_dev endpoint;
static struct crystalhd_adp adapter = { .pdev = &endpoint };
static struct crystalhd_hw hardware;
static struct crystalhd_dioq ready, freeq;
static struct crystalhd_rx_dma_pkt packet;
static struct crystalhd_rx_dma_pkt candidates[16];
static struct device dummy_device;
static unsigned long jiffies;
static unsigned checks, failures, scenarios;
static unsigned wait_calls, down_calls, fetch_calls, add_calls, retain_calls;
static unsigned parser_calls;
static int wait_result, add_result;
static uint32_t reported_picture;
static bool interrupt_lock, inject_packet_after_wait;
static bool forbid_wait, replenish_discarded;
static bool steal_before_fetch;
static struct crystalhd_rx_dma_pkt *stolen_packet;

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
    assert(!forbid_wait); \
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

    assert(queue == &ready && !queue->lock && !hardware.fetch_sem);
    fetch_calls++;
    /* Fail promptly if a broken try-fetch endlessly consumes replenishment. */
    assert(!forbid_wait || fetch_calls <= 32);
    result = queue->packet;
    if (result) {
        queue->packet = result->next;
        result->next = NULL;
        queue->count--;
    }
    if (steal_before_fetch) {
        steal_before_fetch = false;
        stolen_packet = result;
        return NULL;
    }
    return result;
}

static int crystalhd_dioq_add(struct crystalhd_dioq *queue, void *data,
                              bool wake, uint32_t tag)
{
    struct crystalhd_rx_dma_pkt **tail;
    struct crystalhd_rx_dma_pkt *discarded = data;

    assert(queue == &freeq && discarded && !discarded->next &&
           !hardware.fetch_sem);
    (void)wake;
    assert(tag == discarded->pkt_tag);
    add_calls++;
    if (add_result)
        return add_result;
    /* Model an IRQ/repost supplying another candidate as each one is rejected. */
    if (replenish_discarded)
        queue = &ready;
    tail = &queue->packet;
    while (*tail)
        tail = &(*tail)->next;
    *tail = discarded;
    queue->count++;
    return 0;
}

static void crystalhd_hw_retain_rx_pkt(struct crystalhd_hw *hw,
                                       struct crystalhd_rx_dma_pkt *retained)
{
    assert(hw == &hardware && retained && !retained->next && !hw->fetch_sem);
    retain_calls++;
    retained->next = hw->rx_fallback_head;
    hw->rx_fallback_head = retained;
}

static uint32_t link_GetRptDropParam(struct crystalhd_hw *hw,
                                     uint32_t height, uint32_t width,
                                     void *data)
{
    struct crystalhd_rx_dma_pkt *candidate = data;

    assert(hw == &hardware && candidate && !hw->fetch_sem);
    (void)height;
    (void)width;
    parser_calls++;
    candidate->flags |= candidate->parsed_flags;
    return candidate->custom_picture ? candidate->picture : reported_picture;
}

static uint32_t flea_GetRptDropParam(struct crystalhd_hw *hw, void *data)
{
    struct crystalhd_rx_dma_pkt *candidate = data;

    assert(hw == &hardware && candidate && !hw->fetch_sem);
    parser_calls++;
    candidate->flags |= candidate->parsed_flags;
    return candidate->custom_picture ? candidate->picture : reported_picture;
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
    memset(candidates, 0, sizeof(candidates));
    hardware = (struct crystalhd_hw){
        .adp = &adapter,
        .rx_rdyq = &ready,
        .rx_freeq = &freeq,
        .fetch_sem = 1,
    };
    jiffies = checks ? 17 : 0;
    wait_calls = down_calls = fetch_calls = add_calls = retain_calls = 0;
    parser_calls = 0;
    wait_result = -EBUSY;
    add_result = BC_STS_SUCCESS;
    reported_picture = 1;
    interrupt_lock = inject_packet_after_wait = false;
    forbid_wait = replenish_discarded = false;
    steal_before_fetch = false;
    stolen_packet = NULL;
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

static void discard_owner_fallback_case(uint32_t device)
{
    for (unsigned fail_add = 0; fail_add < 2; fail_add++) {
        struct crystalhd_rx_dma_pkt *result;
        uint32_t signal = 0;

        reset(device);
        packet.flags = 0;
        ready.packet = &packet;
        ready.count = 1;
        reported_picture = 0;
        add_result = fail_add ? -ENOSPC : BC_STS_SUCCESS;
        result = crystalhd_dioq_fetch_wait(&hardware, 1, &signal);
        check(!result && !signal && fetch_calls == 1 && add_calls == 1,
              "discarded completion transfers ownership exactly once");
        check((freeq.packet == &packet) == !fail_add &&
              (hardware.rx_fallback_head == &packet) == fail_add &&
              retain_calls == fail_add,
              "discard keeps one free-queue or fallback owner after add failure");
        check(!ready.packet && !ready.count && hardware.fetch_sem == 1 &&
              !ready.lock,
              "discard fallback returns with ready queue and semaphore balanced");
    }
}

static struct crystalhd_rx_dma_pkt *queue_candidate(unsigned index,
                                                    uint32_t picture,
                                                    uint32_t flags)
{
    struct crystalhd_rx_dma_pkt *candidate = &candidates[index];
    struct crystalhd_rx_dma_pkt **tail = &ready.packet;

    assert(index < sizeof(candidates) / sizeof(candidates[0]));
    *candidate = (struct crystalhd_rx_dma_pkt){
        .pkt_tag = UINT32_C(0x70029080) + index,
        .picture = picture, .flags = flags, .custom_picture = true,
    };
    while (*tail)
        tail = &(*tail)->next;
    *tail = candidate;
    ready.count++;
    return candidate;
}

static void legacy_parser_history_cases(uint32_t device)
{
    for (unsigned rejected = 0; rejected < 3; rejected++) {
        struct crystalhd_rx_dma_pkt *first, *second;
        uint32_t signal = 0;

        reset(device);
        hardware.LastPicNo = 7;
        hardware.LastTwoPicNo = 6;
        first = queue_candidate(0, rejected == 0 ? 0 : rejected == 1 ? 7 : 6, 0);
        second = queue_candidate(1, 8, 0);
        check(crystalhd_dioq_fetch_wait(&hardware, 1, &signal) == second && !signal,
              "legacy filtering skips decode-error/current-repeat/previous-repeat candidates");
        check(fetch_calls == 2 && parser_calls == 2 && add_calls == 1 &&
              freeq.packet == first && freeq.count == 1 && !ready.count &&
              !wait_calls && !retain_calls,
              "legacy rejection preserves one free owner and returns the next candidate");
        check(hardware.LastPicNo == 8 &&
              hardware.LastTwoPicNo == (rejected == 2 ? 6U : 7U) &&
              hardware.fetch_sem == 1 && !ready.lock,
              "legacy parser preserves exact repeat-history update semantics");
    }
    for (unsigned flags = 1; flags <= 3; flags++) {
        struct crystalhd_rx_dma_pkt *candidate;
        uint32_t signal = 0;

        reset(device);
        hardware.LastPicNo = 7;
        hardware.LastTwoPicNo = 6;
        candidate = queue_candidate(0, 0, flags);
        check(crystalhd_dioq_fetch_wait(&hardware, 1, &signal) == candidate &&
              !signal && !parser_calls && !add_calls && !wait_calls &&
              hardware.LastPicNo == 7 && hardware.LastTwoPicNo == 6 &&
              hardware.fetch_sem == 1 && !ready.lock,
              "legacy PIB/format markers bypass parser and leave repeat history unchanged");
    }
    if (device == BC_PCI_DEVID_FLEA) {
        struct crystalhd_rx_dma_pkt *candidate;
        uint32_t signal = 0;

        reset(device);
        hardware.LastPicNo = 7;
        hardware.LastTwoPicNo = 6;
        candidate = queue_candidate(0, 0, 0);
        candidate->parsed_flags = COMP_FLAG_FMT_CHANGE | COMP_FLAG_PIB_VALID;
        check(crystalhd_dioq_fetch_wait(&hardware, 1, &signal) == candidate &&
              !signal && parser_calls == 1 && !add_calls && !wait_calls &&
              hardware.LastPicNo == 7 && hardware.LastTwoPicNo == 6 &&
              hardware.fetch_sem == 1 && !ready.lock,
              "legacy Flea parser-discovered format marker is not discarded as decode error");
    }
}

static struct crystalhd_rx_dma_pkt *try_fetch(void)
{
    struct crystalhd_rx_dma_pkt *result;
    unsigned before = down_calls;
    unsigned long time_before = jiffies;

    assert(hardware.fetch_sem == 1);
    hardware.fetch_sem = 0; /* The hardware caller already owns fetch_sem. */
    forbid_wait = true;
    result = crystalhd_dioq_try_fetch_locked(&hardware);
    check(!hardware.fetch_sem && !ready.lock && down_calls == before &&
          !wait_calls && jiffies == time_before,
          "try-fetch never waits for pictures, changes time, or acquires/releases caller semaphore");
    hardware.fetch_sem = 1;
    return result;
}

static void try_fetch_cases(uint32_t device)
{
    reset(device);
    check(!try_fetch() && !fetch_calls && !parser_calls && !add_calls,
          "empty try-fetch leaves every owner and repeat history untouched");

    for (unsigned invalid = 0; invalid < 2; invalid++) {
        reset(device);
        if (invalid)
            hardware.rx_rdyq = NULL;
        else
            ready.sig = 0;
        check(!try_fetch() && !fetch_calls && !parser_calls && !add_calls,
              "absent or invalid ready queue returns empty without a wait or parser call");
    }

    reset(device);
    queue_candidate(0, 8, 0);
    steal_before_fetch = true;
    check(!try_fetch() && fetch_calls == 1 && !parser_calls && !add_calls &&
          !ready.count && stolen_packet == &candidates[0],
          "snapshot/pop race returns empty without parsing or recycling another consumer owner");

    for (unsigned variant = 0; variant < 5; variant++) {
        struct crystalhd_rx_dma_pkt *candidate;

        if (variant == 4 && device != BC_PCI_DEVID_FLEA)
            continue;
        reset(device);
        hardware.LastPicNo = 7;
        hardware.LastTwoPicNo = 6;
        candidate = queue_candidate(0, variant ? 0 : 8, variant < 4 ? variant : 0);
        if (variant == 4)
            candidate->parsed_flags = COMP_FLAG_FMT_CHANGE | COMP_FLAG_PIB_VALID;
        check(try_fetch() == candidate && !ready.count && !ready.packet &&
              !add_calls && !retain_calls && fetch_calls == 1 &&
              parser_calls == (variant == 0 || variant == 4),
              "try-fetch accepts ordinary, PIB, format and parser-discovered format candidates");
        check(hardware.LastPicNo == (variant ? 7U : 8U) &&
              hardware.LastTwoPicNo == (variant ? 6U : 7U),
              "try-fetch changes repeat history only for ordinary accepted pictures");
    }

    for (unsigned rejected = 0; rejected < 3; rejected++) {
        for (unsigned fail_add = 0; fail_add < 2; fail_add++) {
            struct crystalhd_rx_dma_pkt *first, *second;

            reset(device);
            hardware.LastPicNo = 7;
            hardware.LastTwoPicNo = 6;
            first = queue_candidate(0, rejected == 0 ? 0 : rejected == 1 ? 7 : 6, 0);
            second = queue_candidate(1, 8, 0);
            add_result = fail_add ? -ENOSPC : BC_STS_SUCCESS;
            check(try_fetch() == second && fetch_calls == 2 && parser_calls == 2 &&
                  add_calls == 1 && retain_calls == fail_add && !ready.count,
                  "bounded filtering skips rejected candidate and returns next accepted owner");
            check((freeq.packet == first) == !fail_add &&
                  (hardware.rx_fallback_head == first) == fail_add &&
                  hardware.LastPicNo == 8 &&
                  hardware.LastTwoPicNo == (rejected == 2 ? 6U : 7U),
                  "try rejection preserves one recycled/fallback owner and exact repeat history");
        }
    }

    reset(device);
    check(BC_RX_LIST_CNT == 16, "driver RX budget retains the literal sixteen-candidate limit");
    for (unsigned i = 0; i < 16; i++)
        queue_candidate(i, 0, 0);
    replenish_discarded = true;
    check(!try_fetch() && fetch_calls == 16 && parser_calls == 16 && add_calls == 16 &&
          ready.count == 16 && !freeq.count && !retain_calls,
          "continually replenished decode-error queue stops after exactly one bounded pass");
    for (unsigned i = 0; i < 16; i++) {
        struct crystalhd_rx_dma_pkt *cursor = ready.packet;
        unsigned owners = 0;
        for (unsigned count = 0; cursor; cursor = cursor->next) {
            assert(count++ < 16);
            owners += cursor == &candidates[i];
        }
        check(owners == 1, "bounded replenishment leaves each rejected packet with one queue owner");
    }

    reset(device);
    queue_candidate(0, 0, 0);
    replenish_discarded = true;
    check(!try_fetch() && fetch_calls == 1 && parser_calls == 1 && add_calls == 1 &&
          ready.count == 1,
          "single-candidate snapshot is not renewed by concurrent replenishment");
}

int main(void)
{
    const uint32_t devices[] = { BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA };

    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        internal_semaphore_interrupt_case(devices[i]);
        slice_timeout_packet_race_case(devices[i]);
        event_interrupt_case(devices[i]);
        deadline_timeout_case(devices[i]);
        discard_owner_fallback_case(devices[i]);
        legacy_parser_history_cases(devices[i]);
        try_fetch_cases(devices[i]);
    }
    printf("RX fetch wait: %u scenarios, %u checks, %u failures\n",
           scenarios, checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
