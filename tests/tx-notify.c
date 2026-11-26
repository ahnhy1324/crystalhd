/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual notification/helper bodies; only hardware, clock and locking leaves
 * are modeled. This executes no device access and does not qualify native DMA. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef unsigned int BC_STATUS;
struct device { unsigned unused; };
struct pci_dev { struct device dev; };
struct crystalhd_adp { struct pci_dev *pdev; };
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    uint32_t TxBuffInfoAddr;
    BC_STATUS (*pfnDevDRAMRead)(struct crystalhd_hw *, uint32_t, uint32_t, uint32_t *);
    BC_STATUS (*pfnDevDRAMWrite)(struct crystalhd_hw *, uint32_t, uint32_t, uint32_t *);
    uint32_t (*pfnReadDevRegister)(struct crystalhd_adp *, uint32_t);
    void (*pfnWriteDevRegister)(struct crystalhd_adp *, uint32_t, uint32_t);
};

#include "crystalhd_tx_notify.h"
#include "tx-notify-types.h"
#include "tx-notify-store.h"

static unsigned checks, failures, lock_depth, lock_calls, unlock_calls;
static unsigned hardware_step;
static bool helper_active;
static u64 clock_value;
static void check(bool value, const char *why)
{
    checks++;
    if (!value) { failures++; fprintf(stderr, "FAIL: %s\n", why); }
}
static u64 tx_now(void)
{
    check(lock_depth == 1, "record timestamp is sampled under its own lock");
    if (helper_active)
        check(hardware_step == 4, "DONE is recorded only after the original scratch write");
    return ++clock_value;
}
static void tx_lock_irq(unsigned long *flags)
{
    check(lock_depth == 0, "record lock is acquired once without recursion");
    *flags = 0x183; lock_depth++; lock_calls++;
}
static void tx_unlock_irq(unsigned long flags)
{
    check(lock_depth == 1 && flags == 0x183, "record unlock restores its own flags");
    lock_depth--; unlock_calls++;
}
#include "tx-notify-core.h"

static void dev_dbg(struct device *dev, const char *format, ...)
{
    (void)dev; (void)format;
}

/* The mock register address is an identifier only. No hardware is accessed. */
#define INDICATE_TX_DONE_REG 0x24U
static struct crystalhd_hw *expected_hw;
static BC_STATUS read_status, write_status;
static uint32_t sequence_read, sequence_written, scratch_read, scratch_written;
static uint32_t expected_info_address;
static bool move_info_address;
static BC_STATUS dram_read(struct crystalhd_hw *hw, uint32_t addr,
                           uint32_t words, uint32_t *value)
{
    check(hardware_step++ == 0 && hw == expected_hw && words == 1 &&
          addr == expected_info_address + offsetof(TX_INPUT_BUFFER_INFO, SeqNum),
          "first action reads the original sequence address once");
    if (!read_status) *value = sequence_read;
    if (move_info_address) hw->TxBuffInfoAddr += 0x1000;
    return read_status;
}
static BC_STATUS dram_write(struct crystalhd_hw *hw, uint32_t addr,
                            uint32_t words, uint32_t *value)
{
    check(hardware_step++ == 1 && hw == expected_hw && words == 1 &&
          addr == expected_info_address + offsetof(TX_INPUT_BUFFER_INFO, SeqNum),
          "second action writes the same sequence address once");
    sequence_written = *value;
    return write_status;
}
static uint32_t register_read(struct crystalhd_adp *adp, uint32_t addr)
{
    check(hardware_step++ == 2 && adp == expected_hw->adp &&
          addr == INDICATE_TX_DONE_REG, "third action reads original scratch once");
    return scratch_read;
}
static void register_write(struct crystalhd_adp *adp, uint32_t addr, uint32_t value)
{
    check(hardware_step++ == 3 && adp == expected_hw->adp &&
          addr == INDICATE_TX_DONE_REG, "fourth action writes original scratch once");
    scratch_written = value;
}
#include "tx-notify-helper.h"

static void reset_store(void)
{
    check(!lock_depth && lock_calls == unlock_calls, "previous record released its lock");
    memset(&tx_store, 0, sizeof(tx_store));
    lock_calls = unlock_calls = 0; clock_value = 1000;
}
static void helper_case(BC_STATUS rd, BC_STATUS wr, uint32_t seq, uint32_t scratch,
                        bool move_address)
{
    struct pci_dev pci = {0};
    struct crystalhd_adp adp = { &pci };
    struct crystalhd_hw hw = { &adp, 0x401200, dram_read, dram_write,
                              register_read, register_write };
    uint32_t actual_seq = rd ? 0 : seq;
    reset_store(); expected_hw = &hw; hardware_step = 0;
    expected_info_address = hw.TxBuffInfoAddr; move_info_address = move_address;
    read_status = rd; write_status = wr; sequence_read = seq; scratch_read = scratch;
    sequence_written = scratch_written = 0x12345678;
    helper_active = true;
    crystalhd_flea_update_tx_done_to_fw(&hw);
    helper_active = false;
    check(hardware_step == 4, "observer adds no hardware call or error retry");
    check(sequence_written == actual_seq + 1U && scratch_written == scratch + 1U,
          "original ignored-status and modulo-32-bit increments remain intact");
#ifdef CRYSTALHD_TX_NOTIFY_RESEARCH
    {
        const struct chd_tx_notify_record *r = &tx_store.records[0];
        const uint32_t expected[] = {expected_info_address,
            expected_info_address + offsetof(TX_INPUT_BUFFER_INFO, SeqNum),
            rd, actual_seq, actual_seq + 1U, wr, scratch, scratch + 1U};
        check(tx_store.calls == 1 && tx_store.count == 1 && !tx_store.dropped &&
              !tx_store.invalid && !tx_store.saturated, "one bounded DONE row per original helper");
        check(r->event == CHD_TX_DONE && CHD_TX_DONE == 7 && r->count == 8 &&
              r->context == (uintptr_t)&hw && r->time_ns == 1001 &&
              !memcmp(r->value, expected, sizeof(expected)),
              "DONE row retains exact original statuses and before/attempted-write values");
    }
#else
    check(tx_store.calls == 0 && !lock_calls && !unlock_calls,
          "default helper performs no observer call or lock");
#endif
}
static void bounded_core(void)
{
    const uint32_t values[] = {0x12345678, 0xffffffff};
    struct crystalhd_hw *identity = (struct crystalhd_hw *)(uintptr_t)1;
    struct chd_tx_notify_record last;
    unsigned i;
    reset_store();
    for (i = 0; i < CHD_TX_NOTIFY_CAPACITY; i++)
        crystalhd_tx_notify(identity, CHD_TX_DONE, values, 2);
    check(tx_store.count == CHD_TX_NOTIFY_CAPACITY &&
          tx_store.calls == CHD_TX_NOTIFY_CAPACITY && !tx_store.dropped,
          "every fixed-capacity slot accepts exactly one copied record");
    last = tx_store.records[CHD_TX_NOTIFY_CAPACITY - 1];
    check(last.context == 1 && last.value[0] == values[0] && last.value[1] == values[1],
          "context is identity only and original values are copied without dereference");
    crystalhd_tx_notify(identity, CHD_TX_DONE, values, 2);
    check(tx_store.count == CHD_TX_NOTIFY_CAPACITY && tx_store.dropped == 1 &&
          tx_store.calls == CHD_TX_NOTIFY_CAPACITY + 1U &&
          !memcmp(&last, &tx_store.records[CHD_TX_NOTIFY_CAPACITY - 1], sizeof(last)),
          "overflow is counted without overwriting preserved evidence");
    tx_store.calls = UINT64_MAX; tx_store.dropped = UINT64_MAX;
    crystalhd_tx_notify(identity, CHD_TX_DONE, values, 2);
    check(tx_store.calls == UINT64_MAX && tx_store.dropped == UINT64_MAX &&
          tx_store.saturated, "counter exhaustion is explicit and never wraps");
    for (i = 0; i < 5; i++) {
        reset_store();
        crystalhd_tx_notify(identity, i == 3 ? 0 : i == 4 ? CHD_TX_DONE + 1 : CHD_TX_DONE,
                            i == 0 ? NULL : values,
                            i == 1 ? 0 : i == 2 ? CHD_TX_NOTIFY_VALUES + 1 : 2);
        check(tx_store.invalid && tx_store.calls == 1 && tx_store.dropped == 1 &&
              !tx_store.count, "invalid record is rejected before reading supplied values");
    }
#ifndef CRYSTALHD_TX_NOTIFY_RESEARCH
    {
        unsigned side_effect = 0;
        CHD_TX_NOTE((side_effect++, identity), side_effect++, side_effect++);
        check(side_effect == 0, "default macro does not evaluate any operand");
    }
#endif
    check(!lock_depth && lock_calls == unlock_calls, "all recorder exits unlock");
}
int main(void)
{
    helper_case(0, 0, 41, 7, false);
    helper_case(9, 0, 41, 7, false);
    helper_case(0, 10, 41, 7, false);
    helper_case(9, 10, 41, 7, false);
    helper_case(0, 0, UINT32_MAX, UINT32_MAX, false);
    helper_case(0, 10, 41, 7, true);
    bounded_core();
    printf("TX notify helper and bounded recorder: %u checks, %u failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
