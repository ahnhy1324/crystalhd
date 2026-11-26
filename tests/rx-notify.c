/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Verbatim RX observer backend, copied-value store and original PicQ handler.
 * Clock, locks and one register-read leaf are modeled. This is neither device
 * execution nor a replacement test for the existing ARM/SEAL receipt backend. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
#include "rx-types.h"
struct crystalhd_adp { unsigned identity; };
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    uint32_t PicQSts, RxCaptureState;
    uint32_t (*pfnReadDevRegister)(struct crystalhd_adp *, uint32_t);
};
/* Only fields read by the actual observer are modeled. Metadata/flag/status
 * declarations above are extracted unchanged from maintained source. */
struct crystalhd_rx_dma_pkt {
    uint64_t capture_epoch;
    uint32_t pkt_tag, flags, y_done_sz, uv_done_sz;
    struct { uint32_t picture_number, flags; } pib;
    struct crystalhd_rx_metadata metadata;
    struct crystalhd_rx_format_metadata format_metadata;
};
#include "crystalhd_rx_receipt.h"
#include "crystalhd_tx_notify.h"
#include "rx-store.h"
static unsigned checks, failures, lock_depth, lock_calls, unlock_calls;
static u64 clock_ns;
static void check(bool ok, const char *why)
{
    checks++;
    if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", why); }
}
static u64 tx_now(void)
{
    check(lock_depth == 1, "record timestamp is protected by recorder lock");
    return ++clock_ns;
}
static void tx_lock_irq(unsigned long *flags)
{
    check(lock_depth == 0, "recorder lock is not recursive");
    *flags = 0x183; lock_depth++; lock_calls++;
}
static void tx_unlock_irq(unsigned long flags)
{
    check(lock_depth == 1 && flags == 0x183, "recorder restores its flags");
    lock_depth--; unlock_calls++;
}
#include "rx-core.h"
#ifdef CRYSTALHD_RX_NOTIFY_RESEARCH
static u64 ktime_get_mono_fast_ns(void) { return ++clock_ns; }
#include "rx-backend.h"
#endif
static void reset_store(void)
{
    check(!lock_depth && lock_calls == unlock_calls, "previous calls released the lock");
    memset(&tx_store, 0, sizeof(tx_store));
    lock_calls = unlock_calls = 0; clock_ns = 0x1234567800000000ULL;
}
#ifdef CRYSTALHD_RX_NOTIFY_RESEARCH
static void record(unsigned index, struct crystalhd_hw *hw, unsigned event,
                   const uint32_t *values, unsigned count, const char *why)
{
    const struct chd_tx_notify_record *r = &tx_store.records[index];
    check(index < tx_store.count && r->context == (uintptr_t)hw &&
          r->event == event && r->count == count &&
          !memcmp(r->value, values, count * sizeof(*values)), why);
}
#define RECORD(i, hw, event, why, ...) do { \
    const uint32_t expected[] = { __VA_ARGS__ }; \
    record(i, hw, event, expected, sizeof(expected)/sizeof(*expected), why); \
} while (0)
static void backend_controls(void)
{
    struct crystalhd_hw hw = {0};
    struct crystalhd_rx_dma_pkt pkt = {0}, before;
    struct crystalhd_rx_receipt_key key = {0};
    uint64_t irq_time;
    uint32_t low = (uint32_t)(uintptr_t)&pkt;
    uint32_t high = (uint32_t)((uint64_t)(uintptr_t)&pkt >> 32);
    pkt.pkt_tag = 0x34343434; pkt.capture_epoch = 0x8765432100000002ULL;
    pkt.flags = COMP_FLAG_DATA_VALID; pkt.y_done_sz = 123; pkt.uv_done_sz = 45;
    before = pkt;
    reset_store();
    crystalhd_rx_receipt_reserve(&hw, &pkt, 1, 0xabcdef12, 3, &key);
    check(key.run_id == 0xabcdef1234343434ULL && key.index_plus_one == 2,
          "reserve binds original sequence/tag/list only into caller's stack key");
    check(!memcmp(&before, &pkt, sizeof(pkt)), "reserve does not mutate packet storage");
    RECORD(0, &hw, CHD_RX_RESERVE, "reserve records exact context, packet and epoch",
           low, high, pkt.pkt_tag, 1, 0xabcdef12, 3, 2, 0x87654321);
    pkt.pkt_tag++; /* Simulate recycle after the actual fire call returns. */
    crystalhd_rx_receipt_fire_return(&hw, &key);
    RECORD(1, &hw, CHD_RX_FIRE_RETURN, "fire return retains original key despite packet reuse",
           0x34343434, 0xabcdef12, 1);
    irq_time = crystalhd_rx_receipt_irq_entry(&hw);
    crystalhd_rx_receipt_irq(&hw, 1, irq_time, BC_STS_IO_ERROR);
    RECORD(2, &hw, CHD_RX_IRQ, "IRQ preserves status and full original entry timestamp",
           1, BC_STS_IO_ERROR, (uint32_t)irq_time, (uint32_t)(irq_time >> 32));
    crystalhd_rx_receipt_complete(&hw, &pkt, BC_STS_SUCCESS);
    RECORD(3, &hw, CHD_RX_COMPLETE, "success completion copies actual byte counts and flags",
           low, high, pkt.pkt_tag, 0, 123, 45, COMP_FLAG_DATA_VALID, 2, 0x87654321);
    crystalhd_rx_receipt_complete(&hw, &pkt, BC_STS_IO_ERROR);
    RECORD(4, &hw, CHD_RX_COMPLETE, "error completion never reports stale previous sizes",
           low, high, pkt.pkt_tag, BC_STS_IO_ERROR, 0, 0, 0, 2, 0x87654321);
    pkt.metadata.valid = true; pkt.metadata.firmware_timestamp = 0x1122334455667788ULL;
    pkt.metadata.picture_number = 67; pkt.metadata.picture_flags = 0x200;
    pkt.metadata.eos_trailer = true;
    crystalhd_rx_receipt_parse(&hw, &pkt, true);
    RECORD(5, &hw, CHD_RX_PARSE, "normal parse keeps exact 64-bit timestamp and EOS",
           low, high, pkt.pkt_tag, 1, COMP_FLAG_DATA_VALID, 1,
           0x55667788, 0x11223344, 67, 0x200, 1, 0);
    pkt.metadata.valid = false; pkt.flags = COMP_FLAG_PIB_VALID | COMP_FLAG_FMT_CHANGE;
    pkt.pib.picture_number = 68; pkt.pib.flags = 0x100;
    pkt.format_metadata.valid = true;
    pkt.format_metadata.firmware_timestamp = 0xaabbccdd12345678ULL;
    crystalhd_rx_receipt_parse(&hw, &pkt, false);
    RECORD(6, &hw, CHD_RX_PARSE, "false parse with valid format remains distinguishable",
           low, high, pkt.pkt_tag, 0, pkt.flags, 0, 0x12345678, 0xaabbccdd, 68, 0x100, 0, 1);
    pkt.format_metadata.valid = false; pkt.flags = COMP_FLAG_DATA_VALID;
    crystalhd_rx_receipt_parse(&hw, &pkt, false);
    RECORD(7, &hw, CHD_RX_PARSE, "parse failure does not expose stale invalid metadata",
           low, high, pkt.pkt_tag, 0, COMP_FLAG_DATA_VALID, 0, 0, 0, 0, 0, 0, 0);
    crystalhd_rx_receipt_disposition(&hw, &pkt, 0x3456);
    RECORD(8, &hw, CHD_RX_DISPOSITION, "disposition is preserved without interpretation",
           low, high, pkt.pkt_tag, 0x3456);
    before = pkt;
    crystalhd_rx_receipt_reserve(NULL, &pkt, 0, 0, 0, &key);
    crystalhd_rx_receipt_reserve(&hw, NULL, 0, 0, 0, &key);
    crystalhd_rx_receipt_reserve(&hw, &pkt, 0, 0, 0, NULL);
    crystalhd_rx_receipt_fire_return(NULL, &key);
    crystalhd_rx_receipt_fire_return(&hw, NULL);
    key.index_plus_one = 0; crystalhd_rx_receipt_fire_return(&hw, &key);
    check(!crystalhd_rx_receipt_irq_entry(NULL), "null IRQ context does not sample time");
    crystalhd_rx_receipt_irq(NULL, 0, 1, BC_STS_SUCCESS);
    crystalhd_rx_receipt_complete(NULL, &pkt, BC_STS_SUCCESS);
    crystalhd_rx_receipt_complete(&hw, NULL, BC_STS_SUCCESS);
    crystalhd_rx_receipt_parse(NULL, &pkt, true);
    crystalhd_rx_receipt_parse(&hw, NULL, true);
    crystalhd_rx_receipt_disposition(NULL, &pkt, 1);
    crystalhd_rx_receipt_disposition(&hw, NULL, 1);
    check(tx_store.count == 9 && !memcmp(&before, &pkt, sizeof(pkt)),
          "invalid hook inputs neither record nor mutate packet");
}
#else
static void backend_controls(void)
{
    struct crystalhd_hw hw = {0};
    struct crystalhd_rx_dma_pkt pkt = {0}, before = pkt;
    struct crystalhd_rx_receipt_key key = { 123, 456 }, before_key = key;
    unsigned side_effect = 0;
    reset_store();
    crystalhd_rx_receipt_cleanup(NULL);
    crystalhd_rx_receipt_descriptors(&pkt, 7);
    crystalhd_rx_receipt_reserve(&hw, &pkt, 1, 2, 3, &key);
    crystalhd_rx_receipt_fire_return(&hw, &key);
    check(crystalhd_rx_receipt_irq_entry(&hw) == 0, "default IRQ hook does not sample time");
    crystalhd_rx_receipt_irq(&hw, 1, 2, BC_STS_IO_ERROR);
    crystalhd_rx_receipt_complete(&hw, &pkt, BC_STS_SUCCESS);
    crystalhd_rx_receipt_parse(&hw, &pkt, true);
    crystalhd_rx_receipt_disposition(&hw, &pkt, 1);
    CHD_RX_NOTE((side_effect++, &hw), side_effect++, side_effect++);
    check(!side_effect && !tx_store.calls && !lock_calls &&
          !memcmp(&before, &pkt, sizeof(pkt)) && !memcmp(&before_key, &key, sizeof(key)),
          "default backend and macro leave caller state and operands unchanged");
}
#endif
/* Model address identifier only: the real handler still executes exactly its
 * one original register callback and assignments, without device access. */
#define RX_DMA_PIC_QSTS_MBOX 0x108U
static unsigned reads;
static uint32_t bitmap;
static struct crystalhd_adp *expected_adp;
static uint32_t register_read(struct crystalhd_adp *adp, uint32_t address)
{
    check(adp == expected_adp && address == RX_DMA_PIC_QSTS_MBOX,
          "PicQ reads the original mailbox address");
    reads++; return bitmap;
}
#include "rx-picq.h"
static void picq_controls(void)
{
    struct crystalhd_adp adp = {0};
    struct crystalhd_hw hw = { &adp, 0xffffffff, 0, register_read };
    unsigned i;
    expected_adp = &adp;
    for (i = 0; i < 2; i++) {
        reset_store(); reads = 0; bitmap = i ? 0 : 0x80000003;
        hw.RxCaptureState = i ? 1 : 7;
        crystalhd_flea_handle_PicQSts_intr(&hw);
        check(reads == 1 && hw.PicQSts == bitmap && hw.RxCaptureState == 1,
              "PicQ observer preserves one read and original state assignments");
#ifdef CRYSTALHD_RX_NOTIFY_RESEARCH
        RECORD(0, &hw, CHD_RX_PICQ, "PicQ record contains same-call value and pre-transition state",
               RX_DMA_PIC_QSTS_MBOX, bitmap, i ? 1 : 7);
#else
        check(!tx_store.calls, "default PicQ adds no observer");
#endif
    }
    expected_adp = NULL;
}
static void boundary_controls(void)
{
    const uint32_t value = 0x12345678;
    struct chd_tx_notify_record saved;
    reset_store();
    tx_store.count = CHD_TX_NOTIFY_CAPACITY - 1;
    crystalhd_tx_notify(NULL, CHD_TX_NOTIFY_LAST, &value, 1);
    saved = tx_store.records[CHD_TX_NOTIFY_CAPACITY - 1];
    check(tx_store.count == CHD_TX_NOTIFY_CAPACITY && saved.event == CHD_TX_NOTIFY_LAST &&
          saved.value[0] == value && !tx_store.invalid && !tx_store.dropped,
          "last declared event fits final bounded slot without context dereference");
    crystalhd_tx_notify(NULL, CHD_TX_NOTIFY_LAST, &value, 1);
    check(tx_store.dropped == 1 && tx_store.count == CHD_TX_NOTIFY_CAPACITY &&
          !memcmp(&saved, &tx_store.records[CHD_TX_NOTIFY_CAPACITY - 1], sizeof(saved)),
          "capacity exhaustion preserves evidence and reports the dropped record");
    reset_store();
    crystalhd_tx_notify(NULL, CHD_TX_NOTIFY_LAST + 1, &value, 1);
    check(tx_store.invalid && tx_store.dropped == 1 && !tx_store.count,
          "first undeclared event is not silently accepted");
    check(!lock_depth && lock_calls == unlock_calls, "all recorder exits release lock");
}
int main(void)
{
    backend_controls(); picq_controls(); boundary_controls();
    printf("RX notify hooks and bounded records: %u checks, %u failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
